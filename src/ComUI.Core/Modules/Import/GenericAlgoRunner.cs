using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.Json;
using ComUI.Sdk;

namespace ComUI.Core;

/// <summary>
/// 通用算法执行器：读取 *.algo.json，把"算法 DLL + JSON"自动包装成宿主命令。
/// - runtime=dotnet：反射调用类型方法，按参数名绑定（总线输入 / 用户参数 / 进度 / 取消令牌）
/// - runtime=native：P/Invoke C 导出函数（基本类型参数）
/// 复杂流程（多步编排、自定义前后处理）仍可写 IAlgoPlugin 手写壳，两者并存。
/// </summary>
public static class GenericAlgoRunner
{
    public static AlgoCommand? Create(AlgoJsonDescriptor json, string folder,
                                      PluginLoadContext? alc, IBus bus, LogService log)
    {
        try
        {
            Validate(json);

            if (json.Runtime.Equals("native", StringComparison.OrdinalIgnoreCase))
                return CreateNative(json, folder, bus, log);

            return CreateDotNet(json, folder, alc, bus, log);
        }
        catch (Exception ex)
        {
            log.Error($"算法 JSON 加载失败 [{json.Plugin.Id}] ({folder}): {ex.Message}");
            return null;
        }
    }

    private static void Validate(AlgoJsonDescriptor json)
    {
        if (string.IsNullOrWhiteSpace(json.Plugin.Id)) throw new Exception("缺少 plugin.id");
        if (string.IsNullOrWhiteSpace(json.Plugin.Name)) throw new Exception("缺少 plugin.name");
        if (string.IsNullOrWhiteSpace(json.Assembly)) throw new Exception("缺少 assembly");
        if (json.Runtime.Equals("dotnet", StringComparison.OrdinalIgnoreCase))
        {
            if (string.IsNullOrWhiteSpace(json.Type)) throw new Exception("runtime=dotnet 需要 type");
            if (string.IsNullOrWhiteSpace(json.Method)) throw new Exception("runtime=dotnet 需要 method");
        }
    }

    // ==================== dotnet 运行时 ====================

    private static AlgoCommand CreateDotNet(AlgoJsonDescriptor json, string folder,
                                            PluginLoadContext? alc, IBus bus, LogService log)
    {
        var asmPath = Path.Combine(folder, json.Assembly);
        if (!File.Exists(asmPath))
            throw new Exception($"找不到程序集 {json.Assembly}");

        Assembly asm = alc != null
            ? alc.LoadFromAssemblyPath(asmPath)
            : Assembly.LoadFrom(asmPath);

        var type = asm.GetType(json.Type!, throwOnError: false)
                   ?? asm.GetTypes().FirstOrDefault(t => t.FullName == json.Type || t.Name == json.Type)
                   ?? throw new Exception($"找不到类型 {json.Type}");

        var method = type.GetMethod(json.Method!,
                        BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static | BindingFlags.Instance)
                    ?? throw new Exception($"找不到方法 {json.Type}.{json.Method}");

        return new AlgoCommand
        {
            Id = json.Plugin.Id + "." + (json.Method ?? "run"),
            Title = json.Plugin.Name,
            Params = json.Params.Count == 0 ? null : json.Params.Select(ToSdkParam).ToList(),
            Execute = ctx => InvokeDotNetAsync(json, method, type, folder, bus, log, ctx),
        };
    }

    private static ComUI.Sdk.AlgoParam ToSdkParam(AlgoJsonDescriptor.AlgoJsonParam p) => new()
    {
        Name = p.Name,
        Label = string.IsNullOrWhiteSpace(p.Label) ? p.Name : p.Label,
        Type = p.Type.Trim().ToLowerInvariant() switch
        {
            "number" => ComUI.Sdk.AlgoParamType.Number,
            "int" => ComUI.Sdk.AlgoParamType.Int,
            "text" or "string" => ComUI.Sdk.AlgoParamType.Text,
            "bool" or "boolean" => ComUI.Sdk.AlgoParamType.Boolean,
            "enum" => ComUI.Sdk.AlgoParamType.Enum,
            "path" => ComUI.Sdk.AlgoParamType.Path,
            _ => ComUI.Sdk.AlgoParamType.Text,
        },
        Default = ConvertDefault(p),
        Options = p.Options?.ToArray(),
        Min = p.Min,
        Max = p.Max,
    };

    private static object? ConvertDefault(AlgoJsonDescriptor.AlgoJsonParam p)
    {
        if (p.Default.ValueKind == JsonValueKind.Undefined || p.Default.ValueKind == JsonValueKind.Null)
            return p.Type.Trim().ToLowerInvariant() switch
            {
                "number" => 0.0,
                "int" => 0,
                "bool" or "boolean" => false,
                _ => "",
            };
        return p.Type.Trim().ToLowerInvariant() switch
        {
            "number" => p.Default.GetDouble(),
            "int" => p.Default.GetInt32(),
            "bool" or "boolean" => p.Default.GetBoolean(),
            _ => p.Default.ToString(),
        };
    }

    private static async Task InvokeDotNetAsync(AlgoJsonDescriptor json, MethodInfo method, Type type,
        string folder, IBus bus, LogService log, IAlgoRunContext ctx)
    {
        var pars = method.GetParameters();
        var args = new object?[pars.Length];

        foreach (var p in pars)
        {
            // 1) 总线输入
            var input = json.Inputs.FirstOrDefault(i => i.Name.Equals(p.Name, StringComparison.OrdinalIgnoreCase));
            if (input != null)
            {
                // TryGetLatest<T> 的 T 在运行时才确定，用反射调用
                var getter = typeof(IBus).GetMethod(nameof(IBus.TryGetLatest))!.MakeGenericMethod(p.ParameterType);
                var getterArgs = new object?[] { input.Topic, null };
                var ok = (bool)(getter.Invoke(bus, getterArgs) ?? false);
                if (!ok || getterArgs[1] is null)
                    throw new Exception($"输入「{p.Name}」无数据：主题 {input.Topic} 还没有 {p.ParameterType.Name} 帧");
                args[p.Position] = getterArgs[1];
                continue;
            }

            // 2) 框架注入：取消令牌 / 进度
            if (p.ParameterType == typeof(CancellationToken)) { args[p.Position] = ctx.Cancel; continue; }
            if (p.ParameterType == typeof(IProgress<string>))
            {
                args[p.Position] = new Progress<string>(s => ctx.Report(-1, s));
                continue;
            }

            // 3) 用户参数
            var value = TryGetParamValue(json, ctx, p.Name);
            if (value != null)
            {
                args[p.Position] = ConvertTo(value, p.ParameterType, p.Name);
                continue;
            }

            // 4) 有默认值 → 缺省
            if (p.HasDefaultValue) { args[p.Position] = p.DefaultValue; continue; }

            throw new Exception($"无法绑定方法参数「{p.Name}」（未在 params/inputs 中声明且无默认值）");
        }

        object? receiver = method.IsStatic ? null : Activator.CreateInstance(type);
        object? result;
        try
        {
            result = method.Invoke(receiver, args);
        }
        catch (TargetInvocationException tie)
        {
            // 透传真实异常，避免只看到 "Exception has been thrown by the target of an invocation"
            throw tie.InnerException ?? tie;
        }

        if (result is Task task)
        {
            await task;
            var resultProp = task.GetType().GetProperty("Result");
            result = resultProp != null && resultProp.CanRead ? resultProp.GetValue(task) : null;
        }

        foreach (var output in json.Outputs)
        {
            var value = ResolvePath(result, output.From);
            if (value is null)
            {
                log.Warn($"[{json.Plugin.Id}] 输出 {output.From} 为空，跳过发布");
                continue;
            }
            bus.Publish(output.Topic, value);
            log.Info($"[{json.Plugin.Id}] 已发布 {output.From} → {output.Topic}");
        }
    }

    private static object? TryGetParamValue(AlgoJsonDescriptor json, IAlgoRunContext ctx, string name)
    {
        if (!json.Params.Any(p => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase)))
            return null;
        var mi = typeof(IAlgoRunContext).GetMethod("GetParam")!.MakeGenericMethod(typeof(object));
        return mi.Invoke(ctx, new object[] { name });
    }

    private static object ConvertTo(object value, Type target, string name)
    {
        try
        {
            var t = Nullable.GetUnderlyingType(target) ?? target;
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
            throw new Exception($"参数「{name}」值 {value} 无法转换为 {target.Name}: {ex.Message}");
        }
    }

    private static object? ResolvePath(object? root, string path)
    {
        var cur = root;
        foreach (var part in path.Split('.', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries))
        {
            if (cur is null) return null;
            var prop = cur.GetType().GetProperty(part, BindingFlags.Public | BindingFlags.Instance);
            if (prop is null) throw new Exception($"结果对象缺少属性 {part}（路径 {path}）");
            cur = prop.GetValue(cur);
        }
        return cur;
    }

    // ==================== native 运行时（C++ DLL + 统一 C ABI，宿主按 JSON 自动 marshal） ====================

    private static AlgoCommand CreateNative(AlgoJsonDescriptor json, string folder, IBus bus, LogService log)
    {
        var dllPath = Path.Combine(folder, json.Assembly);
        if (!File.Exists(dllPath))
            throw new Exception($"找不到原生库 {json.Assembly}");
        if (string.IsNullOrWhiteSpace(json.Method))
            throw new Exception("runtime=native 需要 method");

        // 加载时按 JSON 自动生成调用壳（统一 C ABI：参数/图像/点云 marshal + algo_free 释放约定）
        var invoker = new ComUI.Sdk.Native.NativeAlgoInvoker(dllPath, json.Method);

        return new AlgoCommand
        {
            Id = json.Plugin.Id + ".run",
            Title = json.Plugin.Name,
            Params = json.Params.Count == 0 ? null : json.Params.Select(ToSdkParam).ToList(),
            Execute = ctx => Task.Run(() => InvokeNative(json, invoker, bus, log, ctx)),
        };
    }

    /// <summary>native 执行：参数按声明顺序取值；总线输入按主题取最新帧（图像/点云）；
    /// 输出按声明顺序发布（from 指定 image/cloud，缺省 image——第 N 个同 kind 输出取第 N 个返回值）。</summary>
    private static void InvokeNative(AlgoJsonDescriptor json, ComUI.Sdk.Native.NativeAlgoInvoker invoker,
        IBus bus, LogService log, IAlgoRunContext ctx)
    {
        // 参数（按 JSON 声明顺序；缺省回落 default）
        var values = new List<object?>();
        foreach (var p in json.Params)
        {
            var v = TryGetParamValue(json, ctx, p.Name);
            values.Add(v ?? ConvertDefault(p));
        }

        // 总线输入：按主题取最新帧（先按图像试，再按点云）
        var images = new List<ImagePayload>();
        var clouds = new List<CloudPayload>();
        foreach (var input in json.Inputs)
        {
            if (bus.TryGetLatest<ImagePayload>(input.Topic, out var img) && img is not null) { images.Add(img); continue; }
            if (bus.TryGetLatest<CloudPayload>(input.Topic, out var c) && c is not null) { clouds.Add(c); continue; }
            throw new Exception($"输入「{input.Name}」无数据：主题 {input.Topic} 还没有图像/点云帧");
        }

        var (outImages, outClouds) = invoker.Run(values, images, clouds);

        // 输出发布：from = "image" / "cloud"（缺省 image）；同 kind 按声明顺序依次取
        int imgIdx = 0, cloudIdx = 0;
        foreach (var output in json.Outputs)
        {
            var kind = (output.From ?? "image").Trim().ToLowerInvariant();
            object? value;
            if (kind == "cloud")
            {
                if (cloudIdx >= outClouds.Count) { log.Warn($"[{json.Plugin.Id}] 输出 {output.From} 无对应点云，跳过发布"); continue; }
                value = outClouds[cloudIdx++];
            }
            else
            {
                if (imgIdx >= outImages.Count) { log.Warn($"[{json.Plugin.Id}] 输出 {output.From} 无对应图像，跳过发布"); continue; }
                value = outImages[imgIdx++];
            }
            bus.Publish(output.Topic, value);
            log.Info($"[{json.Plugin.Id}] 已发布 {output.From} → {output.Topic}");
        }
    }
}
