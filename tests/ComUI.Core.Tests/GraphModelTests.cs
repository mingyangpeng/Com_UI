using Avalonia;
using ComUI.Plugin.NodeGraph;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>GraphModel 序列化/加载/粘贴核心逻辑（历史 bug 集中区：BUG-054 连线丢失、Title/Locked 持久化）。</summary>
public class GraphModelTests
{
    private static NodeDef Def(string id) => new()
    {
        Id = id,
        Name = "测试算子 " + id,
        Category = "源",
        Execute = _ => new Dictionary<string, object?>(),
    };

    private static NodeInstance Node(string defId, double x, double y)
    {
        var n = new NodeInstance { DefId = defId, X = x, Y = y };
        n.Params["p1"] = 42;
        return n;
    }

    static GraphModelTests()
    {
        NodeDefs.Register(Def("test.a"));
        NodeDefs.Register(Def("test.b"));
    }

    [Fact]
    public void Serialize_Load_Roundtrip_RestoresEverything()
    {
        var g = new GraphDocument();
        var n1 = Node("test.a", 10, 20);
        var n2 = Node("test.b", 100, 200);
        n1.Title = "自定义名";
        g.AddNode(n1);
        g.AddNode(n2);
        g.Connect(n1.Id, "out", n2.Id, "in");
        g.Locked = true;

        var json = g.Serialize("我的算法");
        var g2 = new GraphDocument();
        g2.LoadFromJson(json);

        Assert.Equal(2, g2.Nodes.Count);
        Assert.Equal("我的算法", System.Text.Json.JsonDocument.Parse(json).RootElement.GetProperty("Name").GetString());
        Assert.True(g2.Locked);                                        // 锁定持久化
        Assert.Equal(42, g2.Nodes.First(n => n.DefId == "test.a").Params["p1"]);   // 参数持久化
        Assert.Equal("自定义名", g2.Nodes.First(n => n.DefId == "test.a").Title);   // 节点改名持久化
        // 连线恢复（节点 Id 重新生成，按拓扑重映射——BUG-054 的修复点）
        Assert.Single(g2.Connections);
        var conn = g2.Connections[0];
        var from = g2.Nodes.First(n => n.Id == conn.FromNode);
        var to = g2.Nodes.First(n => n.Id == conn.ToNode);
        Assert.Equal("test.a", from.DefId);
        Assert.Equal("test.b", to.DefId);
        Assert.Equal("out", conn.FromPort);
        Assert.Equal("in", conn.ToPort);
    }

    [Fact]
    public void CopyPasteNode_PreservesParamsTitleAndVarName()
    {
        var g = new GraphDocument();
        var n = Node("draw.3d", 10, 20);   // PasteNode 校验算子已注册，用注册过的 draw.3d
        n.Title = "我的预览";
        n.Params["__varName"] = "立方体";
        g.AddNode(n);

        var json = g.CopyNodeToJson(n.Id);
        var pasted = g.PasteNode(json, 24, 24);

        Assert.NotNull(pasted);
        Assert.NotEqual(n.Id, pasted!.Id);
        Assert.Equal("draw.3d", pasted.DefId);
        Assert.Equal("我的预览", pasted.Title);
        Assert.Equal("立方体", pasted.Params["__varName"]);
        Assert.Equal(34, pasted.X);      // 原位 + 级联偏移
        Assert.Equal(2, g.Nodes.Count);
    }

    [Fact]
    public void VarName_SurvivesSaveLoad()
    {
        var g = new GraphDocument();
        var n = Node("test.a", 0, 0);
        n.Params["__varName"] = "我的变量";   // 非声明参数键：随 Params 全量持久化
        g.AddNode(n);

        var g2 = new GraphDocument();
        g2.LoadFromJson(g.Serialize("t"));

        Assert.Equal("我的变量", g2.Nodes[0].Params["__varName"]);
    }

    [Fact]
    public void Connect_InputReplaces_OutputFanOut()
    {
        // 输入端一对一（新线替换旧线）；输出端允许一对多扇出（数据流分发）
        var g = new GraphDocument();
        var src = Node("test.a", 0, 0);
        var mid = Node("test.b", 100, 0);
        var dst = Node("test.c", 200, 0);
        g.AddNode(src); g.AddNode(mid); g.AddNode(dst);
        g.Connect(src.Id, "out", mid.Id, "in");
        g.Connect(src.Id, "out", dst.Id, "in");     // 同输出再连 → 扇出为两条
        Assert.Equal(2, g.Connections.Count);

        var other = Node("test.d", 300, 0);
        g.AddNode(other);
        g.Connect(other.Id, "out", dst.Id, "in");   // dst 输入被占用 → 换源（src→dst 消失）
        Assert.Equal(2, g.Connections.Count);
        Assert.Single(g.Connections, c => c.ToNode == dst.Id);
        Assert.Equal(other.Id, g.Connections.First(c => c.ToNode == dst.Id).FromNode);
    }

    [Fact]
    public void LoadFromJson_DropsConnectionsReferencingMissingNodes()
    {
        var g = new GraphDocument();
        g.AddNode(Node("test.a", 0, 0));
        var json = """
            { "Name": "x", "Nodes": [
                { "Id": "aaa", "Def": "test.a", "X": 0, "Y": 0 },
                { "Id": "bbb", "Def": "test.b", "X": 50, "Y": 0 } ],
              "Connections": [
                { "From": "aaa", "FromPort": "out", "To": "bbb", "ToPort": "in" },
                { "From": "aaa", "FromPort": "out", "To": "ghost", "ToPort": "in" } ] }
            """;
        g.LoadFromJson(json);
        Assert.Equal(2, g.Nodes.Count);
        Assert.Single(g.Connections);   // 指向不存在节点的连线被丢弃
    }

    [Fact]
    public void LoadFromJson_SkipsUnknownDefs()
    {
        var g = new GraphDocument();
        g.LoadFromJson("""
            { "Name": "x", "Nodes": [
                { "Id": "a", "Def": "test.a", "X": 0, "Y": 0 },
                { "Id": "b", "Def": "no.such.def", "X": 0, "Y": 0 } ],
              "Connections": [] }
            """);
        Assert.Single(g.Nodes);   // 未注册算子的节点被跳过
    }

    [Fact]
    public void PasteFromJson_RemapsIds_And_TranslatesToDropCenter()
    {
        var src = new GraphDocument();
        src.AddNode(Node("test.a", 0, 0));
        src.AddNode(Node("test.b", 100, 0));
        var json = src.Serialize("粘贴源");

        var dst = new GraphDocument();
        dst.PasteFromJson(json, new Point(500, 500));

        Assert.Equal(2, dst.Nodes.Count);
        // Id 全部重映射（与源不同）
        Assert.DoesNotContain(src.Nodes[0].Id, dst.Nodes.Select(n => n.Id));
        // 包围盒中心对齐落点：x∈[0,100] 中心 50 → 整体平移 +450（0→450、100→550）
        Assert.Contains(dst.Nodes, n => Math.Abs(n.X - 450) < 1);
        Assert.Contains(dst.Nodes, n => Math.Abs(n.X - 550) < 1);
    }

    [Fact]
    public void Connect_ReplacesExistingInputConnection()
    {
        var g = new GraphDocument();
        var a = Node("test.a", 0, 0);
        var b = Node("test.b", 0, 0);
        var c = Node("test.a", 0, 50);
        g.AddNode(a); g.AddNode(b); g.AddNode(c);
        g.Connect(a.Id, "out", b.Id, "in");
        g.Connect(c.Id, "out", b.Id, "in");   // 同输入端口第二条连线替换第一条
        Assert.Single(g.Connections, k => k.ToNode == b.Id);
        Assert.Equal(c.Id, g.Connections.First(k => k.ToNode == b.Id).FromNode);
    }

    [Fact]
    public void RemoveNode_RemovesItsConnections()
    {
        var g = new GraphDocument();
        var a = Node("test.a", 0, 0);
        var b = Node("test.b", 0, 0);
        g.AddNode(a); g.AddNode(b);
        g.Connect(a.Id, "out", b.Id, "in");
        g.RemoveNode(a.Id);
        Assert.Empty(g.Connections);
    }
}
