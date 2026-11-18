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

    // ==================== 流程控制（for/while/switch） ====================

    [Fact]
    public async Task FlowFor_Loop_RerunsDownstream_PerIteration()
    {
        // flow.for(0..3) → 探针：整图按迭代序列跑 3 次，下游探针每次都真实执行
        var (defC, probeC) = Def("flowtest.consumer");
        var g = new GraphDocument();
        var loop = Node("flow.for");
        loop.Params["起始"] = 0;
        loop.Params["终止"] = 3;
        loop.Params["步长"] = 1;
        var consumer = Node(defC.Id);
        g.AddNode(loop);
        g.AddNode(consumer);
        g.Connect(loop.Id, "计数", consumer.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(3, probeC.Runs);   // 3 次迭代 = 下游执行 3 次
    }

    [Fact]
    public async Task FlowSwitch_RoutesToSelectedPortOnly()
    {
        // switch(分支=1)：出口0 的下游跳过（上游无产出），出口1 的下游执行
        // 探针产出 ImagePayload → 用图像型分支；消费者必须带输入端口（跳过语义按连线判定）
        var (defS, src) = Def("flowtest.src");
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (def0, c0) = Def("flowtest.out0", inPort);
        var (def1, c1) = Def("flowtest.out1", inPort);
        var g = new GraphDocument();
        var srcNode = Node(defS.Id);
        var sw = Node("flow.switch_image");
        sw.Params["分支"] = 1;
        var out0 = Node(def0.Id);
        var out1 = Node(def1.Id);
        g.AddNode(srcNode);
        g.AddNode(sw);
        g.AddNode(out0);
        g.AddNode(out1);
        g.Connect(srcNode.Id, "输出", sw.Id, "输入");
        g.Connect(sw.Id, "出口0", out0.Id, "输入");
        g.Connect(sw.Id, "出口1", out1.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(0, c0.Runs);    // 未选中分支 → 整段跳过
        Assert.Equal(1, c1.Runs);    // 选中分支 → 执行
    }

    [Fact]
    public async Task FlowLoop_StepZero_DoesNotHang()
    {
        // 步长=0 防死循环：迭代上限兜底（执行可完成）
        var (defZero, probeZero) = Def("flowtest.zero");
        var g = new GraphDocument();
        var loop = Node("flow.for");
        loop.Params["起始"] = 0;
        loop.Params["终止"] = 10;
        loop.Params["步长"] = 0;
        var consumer = Node(defZero.Id);
        g.AddNode(loop);
        g.AddNode(consumer);
        g.Connect(loop.Id, "计数", consumer.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(10, probeZero.Runs);   // 步长=0 按 1 处理 → 0..9 共 10 次（不死循环）
    }

    [Fact]
    public async Task Presets_BindInputNodeOutput_ForSubPipeline()
    {
        // 预置输出（子流程算子的输入绑定）：预置源节点输出 → 下游收到预置值而非空产出
        CloudPayload? seen = null;
        var (defSrc2, _) = Def("flowtest.src2");
        var def = new NodeDef
        {
            Id = "flowtest.observer",
            Name = "observer", Category = "处理",
            Inputs = new[] { new PortDef { Name = "输入", Type = PortType.Cloud } },
            Outputs = new[] { new PortDef { Name = "输出", Type = PortType.Cloud } },
            Execute = ctx => { seen = ctx.Inputs.TryGetValue("输入", out var v) ? v as CloudPayload : null; return new Dictionary<string, object?>(); },
        };
        NodeDefs.Register(def);

        var g = new GraphDocument();
        var src = Node("flowtest.src2");
        var observer = Node("flowtest.observer");
        g.AddNode(src);
        g.AddNode(observer);
        g.Connect(src.Id, "输出", observer.Id, "输入");

        var payload = new CloudPayload { Count = 7 };
        var presets = new Dictionary<(string, string), object?> { [(src.Id, "输出")] = payload };
        try
        {
            GraphExecutor.busPublish = (_, _) => { };
            await GraphExecutor.RunAsync(g, (_, _, _) => { }, _ => { }, CancellationToken.None, presets);
        }
        catch { }
        Assert.Same(payload, seen);
    }

    internal const int MaxIterationsForTest = 10000;

    [Fact]
    public async Task FlowWhile_StepOvershoot_StopsBounded()
    {
        // while 步进越过终止值（0→100 步长 3 永远命不中 100）→ 越过即停，不跑满上限的垃圾迭代
        var (defConsumer, probeConsumer) = Def("flowtest.whilecons");
        var g = new GraphDocument();
        var loop = Node("flow.while");
        loop.Params["起始"] = 0;
        loop.Params["终止"] = 100;
        loop.Params["步长"] = 3;
        var consumer = Node(defConsumer.Id);
        g.AddNode(loop);
        g.AddNode(consumer);
        g.Connect(loop.Id, "计数", consumer.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(34, probeConsumer.Runs);   // 0,3,…,99 → 34 次即停
    }

    [Fact]
    public async Task FlowFor_MultipleLoops_EachUsesOwnSequence()
    {
        // 两个循环节点：各自按自身参数算序列（短者钳位在末值），迭代总数取最大
        var (defA, probeA) = Def("flowtest.multiA");
        var (defB, probeB) = Def("flowtest.multiB");
        var g = new GraphDocument();
        var loopA = Node("flow.for");
        loopA.Params["起始"] = 0;
        loopA.Params["终止"] = 2;
        loopA.Params["步长"] = 1;
        var loopB = Node("flow.for");
        loopB.Params["起始"] = 0;
        loopB.Params["终止"] = 5;
        loopB.Params["步长"] = 1;
        var ca = Node(defA.Id);
        var cb = Node(defB.Id);
        g.AddNode(loopA);
        g.AddNode(loopB);
        g.AddNode(ca);
        g.AddNode(cb);
        g.Connect(loopA.Id, "计数", ca.Id, "输入");
        g.Connect(loopB.Id, "计数", cb.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(5, probeB.Runs);   // 总迭代 = 最长序列
        Assert.Equal(5, probeA.Runs);   // 短序列节点同样每次迭代执行（其计数输出钳位在末值）
    }

    [Fact]
    public async Task FlowFor_Scope_SourceRunsOnce_BodyRunsPerIteration()
    {
        // 作用域 = 循环节点下游闭包：源（作用域外）只执行一次；循环体（作用域内）每次迭代执行。
        // 作用域标记 = 从「计数」画到体首节点的控制连线，与数据连线共存（控制边不传数据）。
        var (defSrc, srcProbe) = Def("flowtest.scopesrc");
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (defBody, bodyProbe) = Def("flowtest.scopebody", inPort);
        var g = new GraphDocument();
        var src = Node(defSrc.Id);
        var loop = Node("flow.for");
        loop.Params["起始"] = 0;
        loop.Params["终止"] = 3;
        loop.Params["步长"] = 1;
        var body = Node(defBody.Id);
        g.AddNode(src);
        g.AddNode(loop);
        g.AddNode(body);
        g.Connect(src.Id, "输出", body.Id, "输入");          // 数据连线
        g.Connect(loop.Id, "计数", body.Id, "输入");          // 作用域标记（控制边，共存）

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(1, srcProbe.Runs);      // 作用域外：只执行一次
        Assert.Equal(3, bodyProbe.Runs);     // 作用域内：每次迭代执行
    }

    // ==================== 参数模板 {计数} / {循环名.计数}（迭代期替换，宿主执行器） ====================

    /// <summary>带参数快照的探针：记录每轮迭代收到的参数值（模板替换结果）。</summary>
    private sealed class ParamProbe
    {
        public int Runs;
        public readonly List<string> Seen = new();
        public Dictionary<string, object?> Execute(NodeRunContext ctx)
        {
            Interlocked.Increment(ref Runs);
            lock (Seen) { Seen.Add((ctx.Params.TryGetValue("folder", out var v) ? v?.ToString() : null) ?? ""); }
            return new Dictionary<string, object?> { ["输出"] = new ImagePayload() };
        }
    }

    private static (NodeDef Def, ParamProbe Probe) PDef(string id, params PortDef[] inputs)
    {
        var probe = new ParamProbe();
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

    private static NodeInstance LoopNode(double start, double end, double step = 1, string? title = null)
    {
        var n = new NodeInstance { DefId = "flow.for" };
        n.Params["起始"] = (int)start;
        n.Params["终止"] = (int)end;
        n.Params["步长"] = (int)step;
        if (title is not null) n.Title = title;
        return n;
    }

    [Fact]
    public async Task 循环模板_裸计数_单循环_替换为当前值_原文保留()
    {
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (def, probe) = PDef($"tpl.bare.{Guid.NewGuid():N}", inPort);
        var g = new GraphDocument();
        var loop = LoopNode(0, 3);
        var body = Node(def.Id);
        body.Params["folder"] = @"Z:\data\case_{计数}";
        g.AddNode(loop);
        g.AddNode(body);
        g.Connect(loop.Id, "计数", body.Id, "输入");   // 作用域标记

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(new[] { @"Z:\data\case_0", @"Z:\data\case_1", @"Z:\data\case_2" }, probe.Seen);
        Assert.Equal(@"Z:\data\case_{计数}", body.Params["folder"]);   // 节点原文不动（卡片持续显示公式）
    }

    [Fact]
    public async Task 循环模板_显式循环名_双循环各自取值()
    {
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (def, probe) = PDef($"tpl.named.{Guid.NewGuid():N}", inPort);
        var g = new GraphDocument();
        var loopA = LoopNode(0, 2, title: "外层");
        var loopB = LoopNode(10, 13, title: "内层");
        var body = Node(def.Id);
        body.Params["folder"] = "{外层.计数}/{内层.计数}";
        g.AddNode(loopA);
        g.AddNode(loopB);
        g.AddNode(body);
        g.Connect(loopA.Id, "计数", body.Id, "输入");
        g.Connect(loopB.Id, "计数", body.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        Assert.Equal(new[] { "0/10", "1/11", "1/12" }, probe.Seen);   // A 序列 0,1（钳位末值）× B 序列 10,11,12
    }

    [Fact]
    public async Task 循环模板_多循环裸计数歧义或未知名_保留原文()
    {
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (def, probe) = PDef($"tpl.amb.{Guid.NewGuid():N}", inPort);
        var g = new GraphDocument();
        var loopA = LoopNode(0, 2, title: "A");
        var loopB = LoopNode(10, 12, title: "B");
        var body = Node(def.Id);
        body.Params["folder"] = "{计数}|{不存在.计数}";
        g.AddNode(loopA);
        g.AddNode(loopB);
        g.AddNode(body);
        g.Connect(loopA.Id, "计数", body.Id, "输入");
        g.Connect(loopB.Id, "计数", body.Id, "输入");

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        // 裸 {计数} 多循环歧义 + 未知名 → 原文保留（每次迭代同值）
        Assert.All(probe.Seen, s => Assert.Equal("{计数}|{不存在.计数}", s));
    }

    [Fact]
    public async Task 子流程入口绑定_预置输入端口_节点用预置值执行()
    {
        // 子流程算子入口：入度 0 且有输入端口的节点，父图数据以 (节点Id, 输入端口名) 预置。
        // 此前执行器只认输出键预置 + missingInput 先跳过 → 入口节点永远被跳过、下游全灰（真 bug）。
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (defEntry, entryProbe) = PDef($"sub.entry.{Guid.NewGuid():N}", inPort);
        var g = new GraphDocument();
        var entry = Node(defEntry.Id);
        g.AddNode(entry);

        var payload = new ImagePayload();
        var presets = new Dictionary<(string, string), object?> { [(entry.Id, "输入")] = payload };
        GraphExecutor.busPublish = (_, _) => { };
        await GraphExecutor.RunAsync(g, (_, _, _) => { }, _ => { }, CancellationToken.None, presets);

        Assert.Equal(1, entryProbe.Runs);   // 修复前：missingInput 先跳过 → 0
    }

    [Fact]
    public async Task 框选作用域_混合模式_无矩形循环仍按连线闭包并入()
    {
        // 循环 A 有框选矩形（框住 bodyA）；循环 B 无矩形、连线到 bodyB。
        // 修复前：scope 只算矩形 → B 的下游不在作用域内，只执行一次（真 bug）。
        var (defA, probeA) = PDef($"mix.a.{Guid.NewGuid():N}");
        var inPort = new PortDef { Name = "输入", Type = PortType.Image };
        var (defB, probeB) = PDef($"mix.b.{Guid.NewGuid():N}", inPort);
        var g = new GraphDocument();
        var loopA = LoopNode(0, 2);
        loopA.Scope = new ScopeRect { X = -10, Y = -10, W = 50, H = 50 };   // 覆盖 bodyA(0,0)
        var bodyA = Node(defA.Id);                                          // (0,0) 在框内
        var loopB = LoopNode(0, 3);
        var bodyB = Node(defB.Id, 500, 500);                                // 远离框
        g.AddNode(loopA);
        g.AddNode(bodyA);
        g.AddNode(loopB);
        g.AddNode(bodyB);
        g.Connect(loopB.Id, "计数", bodyB.Id, "输入");   // B 的作用域标记（闭包）

        var (failed, msg) = await Run(g);
        Assert.False(failed, msg);
        int total = Math.Max(2, 3);
        Assert.Equal(total, probeA.Runs);   // 框内：每次迭代执行（A 序列 2 次，总迭代取最大 3，钳位语义）
        Assert.Equal(3, probeB.Runs);       // 修复前：不在 scope → 只跑 1 次
    }
}
