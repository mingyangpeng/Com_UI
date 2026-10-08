using System.Text.Json;
using Avalonia;
using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>端口数据类型。</summary>
public enum PortType
{
    Image,
    Cloud,
}

/// <summary>端口定义。</summary>
public sealed class PortDef
{
    public string Name { get; init; } = "";
    public PortType Type { get; init; }
}

/// <summary>节点运行状态。</summary>
public enum NodeStatus
{
    Idle,
    Running,
    Done,
    Error,
}

/// <summary>节点执行上下文：上游输入 + 自身参数 + 日志/发布。</summary>
public sealed class NodeRunContext
{
    public required IReadOnlyDictionary<string, object?> Inputs { get; init; }
    public required IReadOnlyDictionary<string, object?> Params { get; init; }
    public required Action<string> Log { get; init; }
    public required Action<string, object?> Publish { get; init; }
    public required string NodeId { get; init; }
    /// <summary>卡片内嵌预览（draw 算子用；载荷为 ImagePayload 或 CloudPayload；实现方负责封送 UI 线程）。</summary>
    public required Action<object> Preview { get; init; }

    public T Input<T>(string port) =>
        Inputs.TryGetValue(port, out var v) && v is T t ? t : throw new Exception($"输入「{port}」缺失或类型不符");

    public T Param<T>(string name, T fallback) =>
        Params.TryGetValue(name, out var v) && v is T t ? t : fallback;
}

/// <summary>
/// 节点类型定义：算法的一个独立步骤。
/// Execute 在后台线程调用，输入来自上游输出（按端口名），返回输出端口名 → 值。
/// </summary>
public sealed class NodeDef
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    /// <summary>分类：源 / 处理 / 输出（决定节点头部颜色）。</summary>
    public required string Category { get; init; }
    /// <summary>卡片带内嵌预览区（draw 类算子：输出直接显示在节点上）。</summary>
    public bool HasPreview { get; init; }

    /// <summary>算子库分组栏目（如 "2D 图像"、"3D 点云"、"IO"），决定它在算子库中的归属。</summary>
    public string Group { get; init; } = "通用";
    public PortDef[] Inputs { get; init; } = Array.Empty<PortDef>();
    public PortDef[] Outputs { get; init; } = Array.Empty<PortDef>();
    public ComUI.Sdk.AlgoParam[] Params { get; init; } = Array.Empty<ComUI.Sdk.AlgoParam>();
    public required Func<NodeRunContext, Dictionary<string, object?>> Execute { get; init; }
}

/// <summary>画布上的一个节点实例。</summary>
public sealed class NodeInstance
{
    public string Id { get; } = Guid.NewGuid().ToString("N")[..8];
    public required string DefId { get; init; }
    /// <summary>用户自定义标题（双击改名）；null = 显示算子名。</summary>
    public string? Title { get; set; }
    public double X { get; set; }
    public double Y { get; set; }
    public Dictionary<string, object?> Params { get; } = new();
    public NodeStatus Status { get; set; } = NodeStatus.Idle;
    public string? Error { get; set; }
}

/// <summary>一条连线：从上游输出端口 → 下游输入端口。</summary>
public sealed class Connection
{
    public required string FromNode { get; init; }
    public required string FromPort { get; init; }
    public required string ToNode { get; init; }
    public required string ToPort { get; init; }

    public (string, string) OutKey => (FromNode, FromPort);
    public (string, string) InKey => (ToNode, ToPort);
}

    /// <summary>节点图文档：节点 + 连线。</summary>
    public sealed class GraphDocument
    {
        public List<NodeInstance> Nodes { get; } = new();
        public List<Connection> Connections { get; } = new();

        /// <summary>算法文件锁定标志（随 JSON 持久化；锁定 = 只读打开，禁止修改）。</summary>
        public bool Locked { get; set; }

        public event Action? Changed;

    public NodeInstance? FindNode(string id) => Nodes.FirstOrDefault(n => n.Id == id);

    public void AddNode(NodeInstance node)
    {
        Nodes.Add(node);
        Changed?.Invoke();
    }

    public void RemoveNode(string nodeId)
    {
        var node = FindNode(nodeId);
        if (node is null) return;
        Nodes.Remove(node);
        Connections.RemoveAll(c => c.FromNode == nodeId || c.ToNode == nodeId);
        Changed?.Invoke();
    }

    /// <summary>连接一个输入端口（同输入端口只允许一条连线，旧连线自动被替换）。</summary>
    public void Connect(string fromNode, string fromPort, string toNode, string toPort)
    {
        DisconnectInput(toNode, toPort);   // 输入端一对一：接到已占用输入=换源；输出端允许一对多扇出
        Connections.Add(new Connection { FromNode = fromNode, FromPort = fromPort, ToNode = toNode, ToPort = toPort });
        Changed?.Invoke();
    }

    public void DisconnectInput(string toNode, string toPort)
    {
        Connections.RemoveAll(c => c.ToNode == toNode && c.ToPort == toPort);
        Changed?.Invoke();
    }

    public Connection? FindInputConnection(string toNode, string toPort) =>
        Connections.FirstOrDefault(c => c.ToNode == toNode && c.ToPort == toPort);

    /// <summary>断开某输出端口上的所有连线（从已连线输出端口拉线 = 拆线重连）。</summary>
    public void DisconnectOutput(string fromNode, string fromPort)
    {
        Connections.RemoveAll(c => c.FromNode == fromNode && c.FromPort == fromPort);
        Changed?.Invoke();
    }

    // ==================== 算法保存 / 加载 / 粘贴 ====================

    private sealed class PipelineFile
    {
        public string Name { get; set; } = "";
        public bool Locked { get; set; }
        public List<PipelineNode> Nodes { get; set; } = new();
        public List<PipelineConn> Connections { get; set; } = new();
    }

    private sealed class PipelineNode
    {
        public string Id { get; set; } = "";
        public string Def { get; set; } = "";
        public string? Title { get; set; }
        public double X { get; set; }
        public double Y { get; set; }
        public Dictionary<string, JsonElement>? Params { get; set; }
    }

    private sealed class PipelineConn
    {
        public string From { get; set; } = "";
        public string FromPort { get; set; } = "";
        public string To { get; set; } = "";
        public string ToPort { get; set; } = "";
    }

    /// <summary>整张图序列化为算法 JSON（保存到算子库"算法"栏目）。</summary>
    public string Serialize(string name)
    {
        var doc = new PipelineFile
        {
            Name = name,
            Locked = Locked,
            Nodes = Nodes.Select(n => new PipelineNode
            {
                Id = n.Id, Def = n.DefId, Title = n.Title, X = n.X, Y = n.Y,
                Params = new Dictionary<string, JsonElement>(
                    n.Params.Select(kv => new KeyValuePair<string, JsonElement>(kv.Key, JsonValueOf(kv.Value)))),
            }).ToList(),
            Connections = Connections.Select(c => new PipelineConn
            {
                From = c.FromNode, FromPort = c.FromPort, To = c.ToNode, ToPort = c.ToPort,
            }).ToList(),
        };
        return JsonSerializer.Serialize(doc, new JsonSerializerOptions { WriteIndented = true });
    }

    private static JsonElement JsonValueOf(object? v) => v switch
    {
        null => JsonSerializer.SerializeToElement(""),
        bool b => JsonSerializer.SerializeToElement(b),
        int i => JsonSerializer.SerializeToElement(i),
        double d => JsonSerializer.SerializeToElement(d),
        string s => JsonSerializer.SerializeToElement(s),
        _ => JsonSerializer.SerializeToElement(v.ToString() ?? ""),
    };

    /// <summary>用算法 JSON 整体替换当前图内容（加载场景）。</summary>
    public void LoadFromJson(string json)
    {
        var doc = JsonSerializer.Deserialize<PipelineFile>(json, new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
        }) ?? throw new Exception("算法文件为空或格式错误");

        Nodes.Clear();
        Connections.Clear();
        Locked = doc.Locked;

        // 节点实例化后 Id 是新生成的，连线按「保存 Id → 新实例」重映射，否则全部失配被丢弃
        var idMap = new Dictionary<string, NodeInstance>();
        foreach (var n in doc.Nodes)
        {
            if (!NodeDefs.Map.ContainsKey(n.Def)) continue;
            var inst = new NodeInstance { DefId = n.Def, X = n.X, Y = n.Y, Title = n.Title };
            if (n.Params is not null)
                foreach (var kv in n.Params)
                    inst.Params[kv.Key] = JsonToObject(kv.Value);
            idMap[n.Id] = inst;
            Nodes.Add(inst);
        }
        foreach (var c in doc.Connections)
        {
            if (idMap.TryGetValue(c.From, out var from) && idMap.TryGetValue(c.To, out var to))
                Connections.Add(new Connection { FromNode = from.Id, FromPort = c.FromPort, ToNode = to.Id, ToPort = c.ToPort });
        }
        Changed?.Invoke();
    }

    /// <summary>复制单节点为片段 JSON（卡片 Ctrl+C）：参数/标题/变量名随行。</summary>
    public string CopyNodeToJson(string nodeId)
    {
        var n = FindNode(nodeId);
        if (n is null) return "";
        var doc = new PipelineFile
        {
            Nodes = new List<PipelineNode>
            {
                new PipelineNode
                {
                    Id = n.Id, Def = n.DefId, Title = n.Title, X = n.X, Y = n.Y,
                    Params = new Dictionary<string, JsonElement>(
                        n.Params.Select(kv => new KeyValuePair<string, JsonElement>(kv.Key, JsonValueOf(kv.Value)))),
                },
            },
        };
        return JsonSerializer.Serialize(doc);
    }

    /// <summary>粘贴单节点片段：新 Id、内容照搬（参数/标题/变量名），位置 = 原位 + 偏移；返回新实例。</summary>
    public NodeInstance? PasteNode(string json, double dx, double dy)
    {
        var doc = JsonSerializer.Deserialize<PipelineFile>(json, new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
        });
        var n = doc?.Nodes.FirstOrDefault();
        if (n is null || !NodeDefs.Map.ContainsKey(n.Def)) return null;
        var inst = new NodeInstance { DefId = n.Def, X = n.X + dx, Y = n.Y + dy, Title = n.Title };
        if (n.Params is not null)
            foreach (var kv in n.Params)
                inst.Params[kv.Key] = JsonToObject(kv.Value);
        Nodes.Add(inst);
        Changed?.Invoke();
        return inst;
    }

    /// <summary>把算法 JSON 以"新实例"方式合并进当前图（拖拽算法进画布）：重映射 Id、按落点对齐包围盒。</summary>
    public void PasteFromJson(string json, Point dropCenter)
    {
        var doc = JsonSerializer.Deserialize<PipelineFile>(json, new JsonSerializerOptions
        {
            PropertyNameCaseInsensitive = true,
        }) ?? throw new Exception("算法文件为空或格式错误");
        if (doc.Nodes.Count == 0) return;

        // 包围盒中心 → 平移量
        double minX = doc.Nodes.Min(n => n.X), maxX = doc.Nodes.Max(n => n.X);
        double minY = doc.Nodes.Min(n => n.Y), maxY = doc.Nodes.Max(n => n.Y);
        double offX = dropCenter.X - (minX + maxX) / 2;
        double offY = dropCenter.Y - (minY + maxY) / 2;

        var idMap = new Dictionary<string, string>();
        foreach (var n in doc.Nodes)
        {
            if (!NodeDefs.Map.ContainsKey(n.Def)) continue; // 算子未注册则跳过
            var newId = Guid.NewGuid().ToString("N")[..8];
            idMap[n.Id] = newId;
            var inst = new NodeInstance { DefId = n.Def, X = n.X + offX, Y = n.Y + offY, Title = n.Title };
            if (n.Params is not null)
                foreach (var kv in n.Params)
                    inst.Params[kv.Key] = JsonToObject(kv.Value);
            Nodes.Add(inst);
        }
        foreach (var c in doc.Connections)
        {
            if (idMap.TryGetValue(c.From, out var from) && idMap.TryGetValue(c.To, out var to))
                Connections.Add(new Connection { FromNode = from, FromPort = c.FromPort, ToNode = to, ToPort = c.ToPort });
        }
        Changed?.Invoke();
    }

    private static object? JsonToObject(JsonElement el) => el.ValueKind switch
    {
        JsonValueKind.True => true,
        JsonValueKind.False => false,
        // 必须装箱 (object)：int?double 三元的公共类型是 double——数字参数存取后全变 double，
        // 执行期 Param<int> 匹配失败回落默认值（保存的整数参数重载后不生效，单测发现）
        JsonValueKind.Number => el.TryGetInt32(out var i) ? (object)i : el.GetDouble(),
        JsonValueKind.String => el.GetString(),
        _ => null,
    };
}
