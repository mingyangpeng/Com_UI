using System.IO;
using System.Reflection;
using System.Text.Json;
using System.Text.Json.Serialization;
using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 算子 JSON 注册：扫描 comdll 下的 *.node.json，把「算法 DLL + JSON 声明」注册成算子库条目。
/// 声明内容：节点名称/分组/分类、输入输出端口（类型 image/cloud）、参数、入口方法。
/// 执行时按参数名绑定（输入端口按 name 注入、params 按 name 注入、CancellationToken/IProgress 自动注入），
/// 输出按属性路径从返回值取值（单输出可省略，直接用返回值）。
/// </summary>
public static class NodeJsonRegistry
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static IEnumerable<NodeDef> LoadFrom(string comdllRoot, Action<string> log)
    {
        var algoDir = Path.Combine(comdllRoot, "algo");
        if (!Directory.Exists(algoDir)) yield break;

        foreach (var file in Directory.EnumerateFiles(algoDir, "*.node.json", SearchOption.AllDirectories))
        {
            NodeJson? json = null;
            try
            {
                json = JsonSerializer.Deserialize<NodeJson>(File.ReadAllText(file), JsonOptions);
            }
            catch (Exception ex)
            {
                log($"算子 JSON 解析失败 {Path.GetFileName(file)}: {ex.Message}");
            }
            if (json?.Node is null) continue;

            var def = Build(json, Path.GetDirectoryName(file)!, log);
            if (def is not null)
            {
                log($"算子(JSON)已注册: {json.Node.Name} <{json.Node.Id}> ← {Path.GetFileName(file)}");
                yield return def;
            }
        }
    }

    private static NodeDef? Build(NodeJson json, string folder, Action<string> log)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(json.Node.Id) || string.IsNullOrWhiteSpace(json.Node.Name))
                throw new Exception("缺少 node.id / node.name");

            Assembly? asm = null;
            if (json.Runtime.Equals("dotnet", StringComparison.OrdinalIgnoreCase))
            {
                var asmPath = Path.Combine(folder, json.Assembly);
                if (!File.Exists(asmPath)) throw new Exception($"找不到程序集 {json.Assembly}");
                asm = Assembly.LoadFrom(asmPath); // 默认上下文：重复加载自动复用
            }
            else
            {
                throw new Exception("节点图算子暂仅支持 runtime=dotnet（native 请包一层 C# 壳）");
            }

            var type = asm!.GetType(json.Type ?? "", false)
                       ?? asm.GetTypes().FirstOrDefault(t => t.FullName == json.Type || t.Name == json.Type)
                       ?? throw new Exception($"找不到类型 {json.Type}");
            var method = type.GetMethod(json.Method ?? "",
                            BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance)
                        ?? throw new Exception($"找不到方法 {json.Type}.{json.Method}");

            return new NodeDef
            {
                Id = json.Node.Id,
                Name = json.Node.Name,
                Category = string.IsNullOrWhiteSpace(json.Node.Category) ? "处理" : json.Node.Category,
                Group = string.IsNullOrWhiteSpace(json.Node.Group) ? "通用" : json.Node.Group,
                Inputs = json.Inputs.Select(p => new PortDef { Name = p.Name, Type = ParseType(p.Type) }).ToArray(),
                Outputs = json.Outputs.Select(p => new PortDef { Name = p.Name, Type = ParseType(p.Type) }).ToArray(),
                Params = json.Params.Select(p => new AlgoParam
                {
                    Name = p.Name,
                    Label = string.IsNullOrWhiteSpace(p.Label) ? p.Name : p.Label,
                    Type = p.Type.Trim().ToLowerInvariant() switch
                    {
                        "number" => AlgoParamType.Number,
                        "int" => AlgoParamType.Int,
                        "text" or "string" => AlgoParamType.Text,
                        "bool" or "boolean" => AlgoParamType.Boolean,
                        "enum" => AlgoParamType.Enum,
                        "path" => AlgoParamType.Path,
                        _ => AlgoParamType.Text,
                    },
                    Default = ConvertDefault(p.Default, p.Type),
                    Options = p.Options?.ToArray(),
                    Min = p.Min,
                    Max = p.Max,
                }).ToArray(),
                Execute = ctx => Invoke(json, method, type, ctx),
            };
        }
        catch (Exception ex)
        {
            log($"算子 JSON 注册失败 [{json.Node.Id}] ({folder}): {ex.Message}");
            return null;
        }
    }

    private static PortType ParseType(string t) =>
        t.Trim().ToLowerInvariant() == "cloud" ? PortType.Cloud : PortType.Image;

    private static object? ConvertDefault(JsonElement el, string type)
    {
        if (el.ValueKind == JsonValueKind.Undefined || el.ValueKind == JsonValueKind.Null) return "";
        return type.Trim().ToLowerInvariant() switch
        {
            "number" => el.ValueKind == JsonValueKind.Number ? el.GetDouble() : double.TryParse(el.ToString(), out var d) ? d : 0,
            "int" => el.ValueKind == JsonValueKind.Number ? el.GetInt32() : int.TryParse(el.ToString(), out var i) ? i : 0,
            "bool" or "boolean" => el.ValueKind == JsonValueKind.True || el.ToString() == "true",
            _ => el.ToString(),
        };
    }

    private static Dictionary<string, object?> Invoke(NodeJson json, MethodInfo method, Type type, NodeRunContext ctx)
    {
        var pars = method.GetParameters();
        var args = new object?[pars.Length];

        foreach (var p in pars)
        {
            // 1) 输入端口（JSON inputs：name 或 bind 对应方法参数名）
            var input = json.Inputs.FirstOrDefault(i =>
                (i.Bind ?? i.Name).Equals(p.Name, StringComparison.OrdinalIgnoreCase) ||
                i.Name.Equals(p.Name, StringComparison.OrdinalIgnoreCase));
            if (input != null)
            {
                if (!ctx.Inputs.TryGetValue(input.Name, out var frame) || frame is null)
                    throw new Exception($"输入「{input.Name}」未连线或上游无产出");
                args[p.Position] = ConvertTo(frame, p.ParameterType, p.Name);
                continue;
            }

            // 2) 框架注入
            if (p.ParameterType == typeof(CancellationToken)) { args[p.Position] = CancellationToken.None; continue; }
            if (p.ParameterType == typeof(IProgress<string>))
            {
                args[p.Position] = new Progress<string>(m => ctx.Log(m));
                continue;
            }

            // 3) 用户参数
            if (json.Params.Any(x => x.Name.Equals(p.Name, StringComparison.OrdinalIgnoreCase)))
            {
                args[p.Position] = ConvertTo(
                    ctx.Params.TryGetValue(p.Name, out var pv) ? pv : null,
                    p.ParameterType, p.Name);
                continue;
            }

            if (p.HasDefaultValue) { args[p.Position] = p.DefaultValue; continue; }
            throw new Exception($"无法绑定方法参数「{p.Name}」");
        }

        object? receiver = method.IsStatic ? null : Activator.CreateInstance(type);
        object? result;
        try
        {
            result = method.Invoke(receiver, args);
        }
        catch (TargetInvocationException tie)
        {
            throw tie.InnerException ?? tie;
        }

        if (result is Task task)
        {
            task.GetAwaiter().GetResult();
            var rp = task.GetType().GetProperty("Result");
            result = rp is not null && rp.CanRead ? rp.GetValue(task) : null;
        }

        // 输出：按端口取值。单输出且端口无对应属性 → 直接用返回值
        var dict = new Dictionary<string, object?>();
        foreach (var port in json.Outputs)
        {
            if (json.Outputs.Count == 1 && string.IsNullOrWhiteSpace(port.From))
            {
                dict[port.Name] = result;
                continue;
            }
            var path = string.IsNullOrWhiteSpace(port.From) ? port.Name : port.From;
            dict[port.Name] = ResolvePath(result, path);
        }
        return dict;
    }

    private static object ConvertTo(object? value, Type target, string name)
    {
        var t = Nullable.GetUnderlyingType(target) ?? target;
        try
        {
            if (value is null) return t.IsValueType ? Activator.CreateInstance(t)! : null!;
            if (t.IsInstanceOfType(value)) return value;
            if (t.IsEnum) return Enum.Parse(t, value.ToString()!, true);
            if (t == typeof(int)) return Convert.ToInt32(value);
            if (t == typeof(long)) return Convert.ToInt64(value);
            if (t == typeof(float)) return Convert.ToSingle(value);
            if (t == typeof(double)) return Convert.ToDouble(value);
            if (t == typeof(bool)) return Convert.ToBoolean(value);
            return Convert.ChangeType(value, t);
        }
        catch (Exception ex)
        {
            throw new Exception($"参数「{name}」值无法转换为 {t.Name}: {ex.Message}");
        }
    }

    private static object? ResolvePath(object? root, string path)
    {
        var cur = root;
        foreach (var part in path.Split('.', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            if (cur is null) return null;
            var prop = cur.GetType().GetProperty(part, BindingFlags.Public | BindingFlags.Instance);
            if (prop is null) throw new Exception($"结果对象缺少属性 {part}");
            cur = prop.GetValue(cur);
        }
        return cur;
    }

    // ==================== JSON 模型 ====================

    public sealed class NodeJson
    {
        [JsonPropertyName("node")] public NodeMeta Node { get; set; } = new();
        [JsonPropertyName("runtime")] public string Runtime { get; set; } = "dotnet";
        [JsonPropertyName("assembly")] public string Assembly { get; set; } = "";
        [JsonPropertyName("type")] public string? Type { get; set; }
        [JsonPropertyName("method")] public string? Method { get; set; }
        [JsonPropertyName("inputs")] public List<JsonPort> Inputs { get; set; } = new();
        [JsonPropertyName("outputs")] public List<JsonOut> Outputs { get; set; } = new();
        [JsonPropertyName("params")] public List<JsonParam> Params { get; set; } = new();
    }

    public sealed class NodeMeta
    {
        [JsonPropertyName("id")] public string Id { get; set; } = "";
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("group")] public string? Group { get; set; }
        [JsonPropertyName("category")] public string? Category { get; set; }
        [JsonPropertyName("description")] public string? Description { get; set; }
    }

    public sealed class JsonPort
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        /// <summary>方法参数名（缺省 = name）。</summary>
        [JsonPropertyName("bind")] public string? Bind { get; set; }
        [JsonPropertyName("type")] public string Type { get; set; } = "image";
    }

    public sealed class JsonOut
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        /// <summary>返回值上的属性路径（缺省 = 端口名；单输出可省略）。</summary>
        [JsonPropertyName("from")] public string? From { get; set; }
        [JsonPropertyName("type")] public string Type { get; set; } = "image";
    }

    public sealed class JsonParam
    {
        [JsonPropertyName("name")] public string Name { get; set; } = "";
        [JsonPropertyName("label")] public string? Label { get; set; }
        [JsonPropertyName("type")] public string Type { get; set; } = "number";
        [JsonPropertyName("default")] public JsonElement Default { get; set; }
        [JsonPropertyName("options")] public List<string>? Options { get; set; }
        [JsonPropertyName("min")] public double? Min { get; set; }
        [JsonPropertyName("max")] public double? Max { get; set; }
    }
}
