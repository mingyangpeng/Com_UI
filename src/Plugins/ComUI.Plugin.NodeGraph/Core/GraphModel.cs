using System.Text.Json;
using Avalonia;
using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>端口数据类型。</summary>
public enum PortType
{
    Image,
    Cloud,
    /// <summary>控制端口（flow.for/while 的「计数」）：作用域标记边，不传数据——纯视觉区分（琥珀色）。</summary>
    Control,
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
    /// <summary>算子说明（node.json node.description / 内置算子手写）——算子库属性面板展示。</summary>
    public string? Description { get; init; }
    public PortDef[] Inputs { get; init; } = Array.Empty<PortDef>();
    public PortDef[] Outputs { get; init; } = Array.Empty<PortDef>();
    public ComUI.Sdk.AlgoParam[] Params { get; init; } = Array.Empty<ComUI.Sdk.AlgoParam>();
    public required Func<NodeRunContext, Dictionary<string, object?>> Execute { get; init; }
}

/// <summary>画布上的一个节点实例。</summary>
public sealed class NodeInstance
{
    /// <summary>可赋值：LoadFromJson 保留保存文件里的 Id——子流程参数键（节点 Id 锚定）跨加载稳定，改名不再失配。</summary>
    public string Id { get; init; } = Guid.NewGuid().ToString("N")[..8];
    public required string DefId { get; init; }
    /// <summary>用户自定义标题（双击改名）；null = 显示算子名。</summary>
    public string? Title { get; set; }
    public double X { get; set; }
    public double Y { get; set; }
    /// <summary>卡片显示宽度（ComfyUI 式角把手拉伸，仅宽度；高度恒为内容自适应。null = 默认 CardW）。</summary>
    public double? Width { get; set; }
    public Dictionary<string, object?> Params { get; } = new();
    public NodeStatus Status { get; set; } = NodeStatus.Idle;
    public string? Error { get; set; }

    /// <summary>
    /// 循环作用域矩形（仅 flow.for/flow.while 有效）：世界坐标下的框选范围。
    /// 框内节点（卡片左上角落入）= 循环体，每轮迭代执行；框外节点只执行一次。
    /// null = 未框选，回退到旧的"从计数出口连线推导作用域"方式。
    /// </summary>
    public ScopeRect? Scope { get; set; }
}

/// <summary>循环作用域矩形（可序列化，世界坐标）。</summary>
public sealed class ScopeRect
{
    public double X { get; set; }
    public double Y { get; set; }
    public double W { get; set; }
    public double H { get; set; }

    /// <summary>点是否落入矩形。</summary>
    public bool Contains(double x, double y) => x >= X && x <= X + W && y >= Y && y <= Y + H;
}

/// <summary>连线上的一个可拖拽锚点（世界坐标）。用于把线拆成多段贝塞尔，实现局部变形。</summary>
public sealed class AnchorPoint
{
    public double X { get; set; }
    public double Y { get; set; }
}

/// <summary>一条连线：从上游输出端口 → 下游输入端口。</summary>
public sealed class Connection
{
    public required string FromNode { get; init; }
    public required string FromPort { get; init; }
    public required string ToNode { get; init; }
    public required string ToPort { get; init; }

    /// <summary>
    /// 反馈边：该连线在数据流图中成环（目的节点沿出边可回到来源节点）。
    /// 拓扑排序忽略它；执行时其输入读「上一轮迭代的输出快照」（双缓冲），
    /// 从而实现"把下游结果回灌上游、逐轮迭代"的反馈回路。
    /// </summary>
    public bool Feedback { get; set; }

    /// <summary>源端侧贝塞尔控制点的垂直偏移（世界坐标；0=默认）。拖拽连线时可独立调整。</summary>
    public double Bend { get; set; }

    /// <summary>目标端侧贝塞尔控制点的垂直偏移（两个控制点独立，按住线的哪一段就那段跟手）。</summary>
    public double Bend2 { get; set; }

    /// <summary>曲线中间的可拖拽锚点（世界坐标，按顺序连接）。按住线某处插入锚点后，
    /// 线在该处拆成两段、该点可独立拖动——实现真正的局部变形，端点与其它段不动。</summary>
    public List<AnchorPoint> Anchors { get; set; } = new();

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

        /// <summary>已转为算子库算子（随 JSON 持久化；宿主启动/重载时注册为算子，保存同步写 params.json）。</summary>
        public bool IsOperator { get; set; }

        /// <summary>转为算子后归属的算子库栏目（保存时用户选择；空=默认「子流程」）。</summary>
        public string OperatorGroup { get; set; } = "";

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
    /// <summary>来源是否为流程控制节点（for/while）——其连线是「作用域标记」（控制边），
    /// 不传数据：与数据连线共存，不触发输入端一对一换源。</summary>
    public static bool IsFlowSource(string fromNode) =>
        NodeDefs.Map.TryGetValue(fromNode, out var d) && d.Id is "flow.for" or "flow.while";

    public void Connect(string fromNode, string fromPort, string toNode, string toPort)
    {
        bool flowEdge = IsFlowSource(fromNode);
        // 成环检测：从目的节点沿「数据出边」能否回到来源节点 → 这条新线是反馈边。
        // 反馈边不顶掉输入端口已有的种子线（种子+反馈共存：首轮用种子，之后用反馈）。
        bool feedback = !flowEdge && WouldCreateCycle(fromNode, toNode);
        if (!flowEdge && !feedback)
            DisconnectInput(toNode, toPort);   // 普通输入端一对一：换源；输出端允许一对多扇出
        // 流程控制边 / 反馈边都允许与已有连线共存
        Connections.Add(new Connection
        {
            FromNode = fromNode, FromPort = fromPort, ToNode = toNode, ToPort = toPort,
            Feedback = feedback,
        });
        Changed?.Invoke();
    }

    /// <summary>新增 from→to 的连线是否形成数据流环：从 to 沿出边（忽略 flow 控制边）能否走到 from。</summary>
    public bool WouldCreateCycle(string fromNode, string toNode)
    {
        if (fromNode == toNode) return true;
        var visited = new HashSet<string>();
        var queue = new Queue<string>();
        queue.Enqueue(toNode);
        while (queue.Count > 0)
        {
            var id = queue.Dequeue();
            if (!visited.Add(id)) continue;
            if (id == fromNode) return true;
            foreach (var c in Connections)
            {
                if (c.FromNode != id || IsFlowSource(c.FromNode)) continue;
                queue.Enqueue(c.ToNode);
            }
        }
        return false;
    }

    public void DisconnectInput(string toNode, string toPort)
    {
        Connections.RemoveAll(c => c.ToNode == toNode && c.ToPort == toPort);
        Changed?.Invoke();
    }

    public Connection? FindInputConnection(string toNode, string toPort) =>
        Connections.FirstOrDefault(c => c.ToNode == toNode && c.ToPort == toPort);

    /// <summary>端口的数据连线（跳过流程控制节点的作用域标记边和反馈边）。</summary>
    public Connection? FindDataConnection(string toNode, string toPort) =>
        Connections.FirstOrDefault(c => c.ToNode == toNode && c.ToPort == toPort && !IsFlowSource(c.FromNode) && !c.Feedback);

    /// <summary>端口的反馈连线（上一轮输出回灌，若是首轮则无快照可读取）。</summary>
    public Connection? FindFeedbackConnection(string toNode, string toPort) =>
        Connections.FirstOrDefault(c => c.ToNode == toNode && c.ToPort == toPort && c.Feedback);

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
        public bool IsOperator { get; set; }
        public string OperatorGroup { get; set; } = "";
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
        public double? Width { get; set; }
        public ScopeRect? Scope { get; set; }
        public Dictionary<string, JsonElement>? Params { get; set; }
    }

    private sealed class PipelineConn
    {
        public string From { get; set; } = "";
        public string FromPort { get; set; } = "";
        public string To { get; set; } = "";
        public string ToPort { get; set; } = "";
        public bool Feedback { get; set; }
        public double Bend { get; set; }
        public double Bend2 { get; set; }
        public List<AnchorPoint>? Anchors { get; set; }
    }

    /// <summary>整张图序列化为算法 JSON（保存到算子库"算法"栏目）。</summary>
    public string Serialize(string name)
    {
        var doc = new PipelineFile
        {
            Name = name,
            Locked = Locked,
            IsOperator = IsOperator,
            OperatorGroup = OperatorGroup,
            Nodes = Nodes.Select(n => new PipelineNode
            {
                Id = n.Id, Def = n.DefId, Title = n.Title, X = n.X, Y = n.Y, Width = n.Width, Scope = n.Scope,
                Params = new Dictionary<string, JsonElement>(
                    n.Params.Select(kv => new KeyValuePair<string, JsonElement>(kv.Key, JsonValueOf(kv.Value)))),
            }).ToList(),
            Connections = Connections.Select(c => new PipelineConn
            {
                From = c.FromNode, FromPort = c.FromPort, To = c.ToNode, ToPort = c.ToPort,
                Feedback = c.Feedback, Bend = c.Bend, Bend2 = c.Bend2,
                Anchors = c.Anchors.Count > 0 ? c.Anchors.Select(a => new AnchorPoint { X = a.X, Y = a.Y }).ToList() : null,
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
        IsOperator = doc.IsOperator;
        OperatorGroup = doc.OperatorGroup ?? "";

        // 节点 Id 保留保存文件里的原值（子流程参数键锚定节点 Id，跨加载/改名稳定）；
        // 文件里 Id 为空或重复（手改损坏）→ 发新 Id 兜底，防 byId 字典构建炸键冲突
        var idMap = new Dictionary<string, NodeInstance>();
        var usedIds = new HashSet<string>();
        foreach (var n in doc.Nodes)
        {
            if (!NodeDefs.Map.ContainsKey(n.Def)) continue;
            var id = !string.IsNullOrWhiteSpace(n.Id) && usedIds.Add(n.Id)
                ? n.Id
                : Guid.NewGuid().ToString("N")[..8];
            var inst = new NodeInstance { Id = id, DefId = n.Def, X = n.X, Y = n.Y, Title = n.Title, Width = n.Width, Scope = n.Scope };
            if (n.Params is not null)
                foreach (var kv in n.Params)
                    inst.Params[kv.Key] = JsonToObject(kv.Value);
            idMap[n.Id] = inst;
            Nodes.Add(inst);
        }
        foreach (var c in doc.Connections)
        {
            if (idMap.TryGetValue(c.From, out var from) && idMap.TryGetValue(c.To, out var to))
                Connections.Add(new Connection
                {
                    FromNode = from.Id, FromPort = c.FromPort, ToNode = to.Id, ToPort = c.ToPort,
                    Feedback = c.Feedback,
                    Bend = c.Bend,
                    Bend2 = c.Bend2,
                    Anchors = c.Anchors is { Count: > 0 }
                        ? c.Anchors.Select(a => new AnchorPoint { X = a.X, Y = a.Y }).ToList()
                        : new List<AnchorPoint>(),
                });
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
                    Id = n.Id, Def = n.DefId, Title = n.Title, X = n.X, Y = n.Y, Width = n.Width,
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
        var inst = new NodeInstance { DefId = n.Def, X = n.X + dx, Y = n.Y + dy, Title = n.Title, Width = n.Width };
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
            var inst = new NodeInstance { DefId = n.Def, X = n.X + offX, Y = n.Y + offY, Title = n.Title, Width = n.Width };
            if (n.Params is not null)
                foreach (var kv in n.Params)
                    inst.Params[kv.Key] = JsonToObject(kv.Value);
            Nodes.Add(inst);
        }
        foreach (var c in doc.Connections)
        {
            if (idMap.TryGetValue(c.From, out var from) && idMap.TryGetValue(c.To, out var to))
                Connections.Add(new Connection
                {
                    FromNode = from, FromPort = c.FromPort, ToNode = to, ToPort = c.ToPort,
                    Feedback = c.Feedback,
                    Bend = c.Bend,
                    Bend2 = c.Bend2,
                    Anchors = c.Anchors is { Count: > 0 }
                        ? c.Anchors.Select(a => new AnchorPoint { X = a.X, Y = a.Y }).ToList()
                        : new List<AnchorPoint>(),
                });
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
