using ComUI.Plugin.NodeGraph;
using ComUI.Sdk;
using Xunit;

namespace ComUI.Core.Tests;

/// <summary>
/// GraphExecutor 跳过语义（BUG：未连线节点曾抛异常中止整图，后面的预览节点永远不执行）：
/// 未连线输入的节点 = 跳过（不中止），下游级联跳过；已连线的分支照常执行。
/// </summary>
public class GraphExecutorTests
{
    private sealed class Probe
    {
        public int Runs;
        public Dictionary<string, object?> Execute(NodeRunContext ctx)
        {
            Interlocked.Increment(ref Runs);
            return new Dictionary<string, object?> { ["输出"] = new ImagePayload() };
        }
    }

    private static (NodeDef Def, Probe Probe) Def(string id, params PortDef[] inputs)
    {
        var probe = new Probe();
        var def = new NodeDef
        {
            Id = id,
            Name = id,
            Category = "处理",
            Inputs = inputs,
            Outputs = new[] { new PortDef { Name = "输出", Type = PortType.Image } },
            Execute = probe.Execute,
        };
        NodeDefs.Register(def);
        return (def, probe);
    }

    private static NodeInstance Node(string defId, double x = 0, double y = 0) => new() { DefId = defId, X = x, Y = y };

    private static async Task<(bool failed, string message)> Run(GraphDocument g)
    {
        try
        {
            GraphExecutor.busPublish = (_, _) => { };
            await GraphExecutor.RunAsync(g, (_, _, _) => { }, _ => { }, CancellationToken.None);
            return (false, "");
        }
        catch (Exception ex)
        {
            return (true, ex.Message);
        }
    }

    [Fact]
    public async Task 未连线的节点被跳过_整图不失败()
    {
        var (srcDef, src) = Def($"exec.src.{Guid.NewGuid():N}");
        var (orphanDef, orphan) = Def($"exec.orphan.{Guid.NewGuid():N}", new PortDef { Name = "输入", Type = PortType.Image });

        var g = new GraphDocument();
        g.AddNode(Node(srcDef.Id));
        g.AddNode(Node(orphanDef.Id));

        var (failed, msg) = await Run(g);

        Assert.False(failed, $"未连线节点不应中止整图: {msg}");
        Assert.Equal(1, src.Runs);        // 源节点正常执行
        Assert.Equal(0, orphan.Runs);     // 未连线节点被跳过
    }

    [Fact]
    public async Task 跳过节点的下游级联跳过()
    {
        var id = Guid.NewGuid().ToString("N");
        var (srcDef, src) = Def($"exec.src2.{id}");
        var (orphanDef, orphan) = Def($"exec.orphan2.{id}", new PortDef { Name = "输入", Type = PortType.Image });
        var (downDef, down) = Def($"exec.down.{id}", new PortDef { Name = "输入", Type = PortType.Image });

        var g = new GraphDocument();
        var srcNode = Node(srcDef.Id);
        var orphanNode = Node(orphanDef.Id);
        var downNode = Node(downDef.Id);
        g.AddNode(srcNode);
        g.AddNode(orphanNode);
        g.AddNode(downNode);
        g.Connect(orphanNode.Id, "输出", downNode.Id, "输入");   // 下游依赖被跳过的孤儿节点

        var (failed, msg) = await Run(g);

        Assert.False(failed, msg);
        Assert.Equal(1, src.Runs);
        Assert.Equal(0, orphan.Runs);
        Assert.Equal(0, down.Runs);       // 上游被跳过 → 级联跳过
    }

    [Fact]
    public async Task 已连线分支照常执行()
    {
        var id = Guid.NewGuid().ToString("N");
        var (srcDef, src) = Def($"exec.src3.{id}");
        var (orphanDef, orphan) = Def($"exec.orphan3.{id}", new PortDef { Name = "输入", Type = PortType.Image });
        var (okDef, ok) = Def($"exec.ok.{id}", new PortDef { Name = "输入", Type = PortType.Image });

        var g = new GraphDocument();
        var srcNode = Node(srcDef.Id);
        var orphanNode = Node(orphanDef.Id);
        var okNode = Node(okDef.Id);
        g.AddNode(srcNode);
        g.AddNode(orphanNode);
        g.AddNode(okNode);
        g.Connect(srcNode.Id, "输出", okNode.Id, "输入");     // 源 → ok 正常链路

        var (failed, msg) = await Run(g);

        Assert.False(failed, msg);
        Assert.Equal(1, src.Runs);
        Assert.Equal(0, orphan.Runs);
        Assert.Equal(1, ok.Runs);        // 与孤儿并行的正常分支照常执行
    }

    [Fact]
    public async Task 算法真异常仍中止整图()
    {
        var badDef = new NodeDef
        {
            Id = $"exec.bad.{Guid.NewGuid():N}",
            Name = "坏节点",
            Category = "处理",
            Execute = _ => throw new Exception("算法内部错误"),
        };
        NodeDefs.Register(badDef);

        var g = new GraphDocument();
        g.AddNode(Node(badDef.Id));

        var (failed, msg) = await Run(g);

        Assert.True(failed);
        Assert.Contains("算法内部错误", msg);
    }
}
