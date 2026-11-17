using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>按拓扑序执行整张节点图（后台线程调用；检测环路；逐节点回报状态）。
/// 支持 flow.for / flow.while 循环节点：检测到时整图按迭代序列循环执行，
/// 每次迭代循环节点的输出 = 当前计数（下游节点每次迭代都真实执行，发布/预览逐次触发）。</summary>
public static class GraphExecutor
{
    /// <summary>循环迭代上限（防 步长=0/方向反 导致死循环）。</summary>
    private const int MaxIterations = 10000;

    /// <summary>预置输出：(节点Id, 输出端口) → 值——单 pass 前注入（子流程算子把父级输入绑进子图用）。</summary>
    public static async Task RunAsync(
        GraphDocument graph,
        Action<NodeInstance, NodeStatus, string?> setStatus,
        Action<string> log,
        CancellationToken ct,
        IReadOnlyDictionary<(string Node, string Port), object?>? presets = null,
        IDictionary<(string Node, string Port), object?>? collectOutputs = null)
    {
        await Task.Run(() =>
        {
            var (order, byId) = TopoSort(graph);
            var outputs = new Dictionary<(string Node, string Port), object?>();
            if (presets is not null)
                foreach (var kv in presets) outputs[kv.Key] = kv.Value;   // 预置注入（子流程输入绑定）

            // ===== 循环驱动：图中有 flow.for / flow.while → 作用域 = 其下游闭包 =====
            // 作用域内节点每次迭代都执行；作用域外节点整个运行期只执行一次（先行）。
            // 作用域标记 = 从循环节点「计数」出口画到体首节点的连线（控制边，不传数据）。
            var loopNodes = graph.Nodes.Where(n => n.DefId is "flow.for" or "flow.while").ToList();
            if (loopNodes.Count > 0)
            {
                // 作用域闭包：从循环节点沿出边可达的全部节点（含循环节点自身）
                var scope = new HashSet<string>(loopNodes.Select(n => n.Id));
                var queue = new Queue<string>(loopNodes.Select(n => n.Id));
                while (queue.Count > 0)
                {
                    var id = queue.Dequeue();
                    foreach (var c in graph.Connections.Where(c => c.FromNode == id))
                    {
                        if (byId.ContainsKey(c.ToNode) && scope.Add(c.ToNode))
                            queue.Enqueue(c.ToNode);
                    }
                }

                var outScope = order.Where(id => !scope.Contains(id)).ToList();
                var inScope = order.Where(id => scope.Contains(id)).ToList();
                var driver = loopNodes[0];
                log($"[循环] 作用域 {inScope.Count} 个节点（作用域外 {outScope.Count} 个只执行一次）");

                // 先行：作用域外节点按拓扑序执行一次（输出保留，供作用域内每轮迭代消费）
                RunPass(graph, outScope, byId, outputs, setStatus, log, ct, null);

                // 多个循环节点：各自按自身参数算序列，迭代次数取最大者（短序列钳位在末值）
                var nodeSeqs = loopNodes.ToDictionary(n => n.Id, IterationSequence);
                var total = nodeSeqs.Values.Max(s => s.Count);
                for (int it = 0; it < total; it++)
                {
                    ct.ThrowIfCancellationRequested();
                    var passPresets = new Dictionary<(string Node, string Port), object?>();
                    var counters = new Dictionary<string, int>();
                    foreach (var n in loopNodes)
                    {
                        var s = nodeSeqs[n.Id];
                        var val = s[Math.Min(it, s.Count - 1)];
                        passPresets[(n.Id, "计数")] = val;
                        counters[n.Id] = val;   // 模板 {循环名.计数} / {计数} 的替换源
                    }
                    log($"[循环] 第 {it + 1}/{total} 次迭代：计数 = {passPresets.First().Value}");
                    RunPass(graph, inScope, byId, outputs, setStatus, log, ct, passPresets, counters);
                }

                if (collectOutputs is not null)
                    foreach (var kv in outputs) collectOutputs[kv.Key] = kv.Value;
                return;
            }

            RunPass(graph, order, byId, outputs, setStatus, log, ct, presets);

            if (collectOutputs is not null)
                foreach (var kv in outputs) collectOutputs[kv.Key] = kv.Value;
        }, ct);
    }

    /// <summary>循环迭代序列：for = 起始…终止（步长）；while = 起始步进到终止（≠ 终止即继续）。</summary>
    private static List<int> IterationSequence(NodeInstance node)
    {
        int start = node.Params.TryGetValue("起始", out var s) && s is int si ? si : 0;
        int end = node.Params.TryGetValue("终止", out var e) && e is int ei ? ei : 10;
        int step = node.Params.TryGetValue("步长", out var st) && st is int sti ? sti : 1;
        if (step == 0) step = 1;

        var seq = new List<int>();
        int i = start;
        if (node.DefId == "flow.while")
        {
            // ≠ 终止即继续，但步进方向越过终止值也应停（0→100 步长 3 永远命不中 100，
            // 否则会跑满迭代上限的垃圾次数）——计数越过 end 即停
            while (i != end && seq.Count < MaxIterations &&
                   !(step > 0 && i > end) && !(step < 0 && i < end))
            {
                seq.Add(i);
                i += step;
            }
        }
        else
        {
            while ((step > 0 ? i < end : i > end) && seq.Count < MaxIterations) { seq.Add(i); i += step; }
            if (seq.Count == 0) seq.Add(start);   // 空区间至少跑一次（起点即终点）
        }
        return seq;
    }

    /// <summary>参数模板正则：{...} 占位（不跨嵌套）。</summary>
    private static readonly System.Text.RegularExpressions.Regex TplRegex =
        new(@"\{([^{}]+)\}", System.Text.RegularExpressions.RegexOptions.Compiled);

    /// <summary>迭代期参数模板替换：{计数}（唯一循环时）或 {循环名.计数}（循环节点标题/算子名锚定）→ 当前计数值。
    /// 只动含 "{" 的字符串参数；无任何替换发生返回 null（调用方沿用原字典，行为零差异）。
    /// 未知占位符保留原文（不吞用户路径里合法的花括号内容）。</summary>
    private static Dictionary<string, object?>? ApplyTemplates(
        NodeInstance node, Dictionary<string, NodeInstance> byId, IReadOnlyDictionary<string, int> counters, Action<string> log)
    {
        // 循环节点 Id → 显示名（Title 优先）→ 当前值；同名循环后者覆盖（罕见；歧义场景用显式不同名）
        var byName = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var kv in counters)
        {
            if (!byId.TryGetValue(kv.Key, out var ln)) continue;
            byName[ln.Title ?? NodeDefs.Map[ln.DefId].Name] = kv.Value;
        }

        Dictionary<string, object?>? result = null;
        foreach (var kv in node.Params)
        {
            if (kv.Value is not string s || !s.Contains('{')) continue;
            var r = TplRegex.Replace(s, m =>
            {
                var expr = m.Groups[1].Value.Trim();
                if (expr == "计数")
                {
                    if (counters.Count == 1) return counters.Values.First().ToString();
                    log($"[{node.Title ?? NodeDefs.Map[node.DefId].Name}] 模板 {{计数}} 有多个循环时存在歧义，已保留原文（请写 {{循环名.计数}}）");
                    return m.Value;
                }
                if (expr.EndsWith(".计数") && byName.TryGetValue(expr[..^3], out var v))
                    return v.ToString();
                return m.Value;   // 非模板内容（或循环名未命中）原样保留
            });
            if (r != s)
            {
                result ??= new Dictionary<string, object?>(node.Params);
                result[kv.Key] = r;
            }
        }
        return result;
    }

    /// <summary>拓扑排序（Kahn）：返回 (执行序, 节点表)；含环抛异常。</summary>
    private static (List<string> Order, Dictionary<string, NodeInstance> ById) TopoSort(GraphDocument graph)
    {
        var byId = graph.Nodes.ToDictionary(n => n.Id);
        var inDegree = graph.Nodes.ToDictionary(n => n.Id, _ => 0);
        var adjacency = graph.Nodes.ToDictionary(n => n.Id, _ => new List<string>());

        foreach (var c in graph.Connections)
        {
            if (!byId.ContainsKey(c.FromNode) || !byId.ContainsKey(c.ToNode)) continue;
            inDegree[c.ToNode]++;
            adjacency[c.FromNode].Add(c.ToNode);
        }

        var queue = new Queue<string>(inDegree.Where(kv => kv.Value == 0).Select(kv => kv.Key));
        var order = new List<string>();
        while (queue.Count > 0)
        {
            var id = queue.Dequeue();
            order.Add(id);
            foreach (var next in adjacency[id])
                if (--inDegree[next] == 0) queue.Enqueue(next);
        }

        if (order.Count != graph.Nodes.Count)
        {
            var loopIds = graph.Nodes.Where(n => !order.Contains(n.Id)).Select(n => NodeDefs.Map[n.DefId].Name);
            throw new Exception("图中存在环路，无法执行：" + string.Join("、", loopIds));
        }
        return (order, byId);
    }

    /// <summary>单次拓扑序执行（presets = 预置输出：循环计数注入 / 子流程输入绑定；counters = 循环计数器（节点Id→值），供参数模板 {循环名.计数} 迭代期替换）。</summary>
    private static void RunPass(
        GraphDocument graph,
        List<string> ids,
        Dictionary<string, NodeInstance> byId,
        Dictionary<(string Node, string Port), object?> outputs,
        Action<NodeInstance, NodeStatus, string?> setStatus,
        Action<string> log,
        CancellationToken ct,
        IReadOnlyDictionary<(string Node, string Port), object?>? presets,
        IReadOnlyDictionary<string, int>? counters = null)
    {
        // ===== 依序执行（outputs 跨 pass 共享：作用域外节点的输出供每轮迭代消费） =====
        // 未连线/上游被跳过的节点 = 跳过（灰态+日志），不中止整图——
        // 否则一个忘连线的输出节点会把后面的预览卡全部拖死（用户报"2D/3D 预览不显示"的根因）。
        // 真正的执行错误（算法抛异常）仍然中止整图并标红。
        var skipped = new HashSet<string>();
        if (presets is not null)
            foreach (var kv in presets) outputs[kv.Key] = kv.Value;   // 预置注入（循环计数/子流程输入）

        foreach (var id in ids)
        {
            ct.ThrowIfCancellationRequested();
            var node = byId[id];
            var def = NodeDefs.Map[node.DefId];

            // 循环/分支等 flow 控制节点：输出由预置（循环）或自身路由给出，不调用户 Execute
            if (def.Id is "flow.for" or "flow.while")
            {
                foreach (var port in def.Outputs)
                {
                    if (presets is not null && presets.TryGetValue((node.Id, port.Name), out var pv))
                        outputs[(node.Id, port.Name)] = pv;
                }
                setStatus(node, NodeStatus.Done, null);
                continue;
            }

                // 输入端口未连线，或上游节点被跳过（无产出）→ 本节点跳过，下游级联跳过
                var missingInput = def.Inputs.FirstOrDefault(p => graph.FindInputConnection(node.Id, p.Name) is null);
                if (missingInput is not null)
                {
                    skipped.Add(id);
                    setStatus(node, NodeStatus.Idle, null);
                    log($"[{def.Name}] 跳过：输入「{missingInput.Name}」未连线");
                    continue;
                }
                var skippedUpstream = def.Inputs
                    .Select(p => graph.FindInputConnection(node.Id, p.Name)!)
                    .Any(c => skipped.Contains(c.FromNode));
                if (skippedUpstream)
                {
                    skipped.Add(id);
                    setStatus(node, NodeStatus.Idle, null);
                    log($"[{def.Name}] 跳过：上游未执行");
                    continue;
                }

                setStatus(node, NodeStatus.Running, null);

                // 预置输出绑定（子流程输入节点）：该节点输出由外部供给 → 跳过自身执行，保留预置值
                if (presets is not null && def.Outputs.Any(p => presets.ContainsKey((node.Id, p.Name))))
                {
                    foreach (var port in def.Outputs)
                        if (presets.TryGetValue((node.Id, port.Name), out var pv))
                            outputs[(node.Id, port.Name)] = pv;
                    setStatus(node, NodeStatus.Done, null);
                    continue;
                }

                try
                {
                    // 收集输入；上游无产出（分支未选中）→ 本节点跳过（灰态），下游级联——
                    // switch 分支路由的核心语义：未选中的出口值为 null，挂在其上的子链整段跳过
                    var inputs = new Dictionary<string, object?>();
                    var noOutput = false;
                    foreach (var port in def.Inputs)
                    {
                        // 该端口上的连线：数据连线（非 flow 来源）优先；flow 连线 = 作用域标记（不传数据）
                        var conns = graph.Connections.Where(c => c.ToNode == node.Id && c.ToPort == port.Name).ToList();
                        var dataConn = conns.FirstOrDefault(c => !GraphDocument.IsFlowSource(c.FromNode));
                        if (dataConn is null)
                        {
                            if (conns.Count == 0) { noOutput = true; break; }   // 端口未连线
                            continue;   // 仅作用域标记边 → 控制端口，跳过不取数据
                        }
                        if (!outputs.TryGetValue((dataConn.FromNode, dataConn.FromPort), out var value) || value is null)
                        {
                            noOutput = true;
                            break;
                        }
                        inputs[port.Name] = value;
                    }
                    if (noOutput)
                    {
                        skipped.Add(id);
                        setStatus(node, NodeStatus.Idle, null);
                        log($"[{def.Name}] 跳过：上游分支无产出");
                        continue;
                    }

                    var ctx = new NodeRunContext
                    {
                        Inputs = inputs,
                        // 参数模板：作用域内字符串参数可写 {计数} / {循环名.计数}，迭代期替换为当前计数值。
                        // 替换进副本（node.Params 原文保留——卡片持续显示公式，重跑不残留上次值）
                        Params = counters is not null ? ApplyTemplates(node, byId, counters, log) ?? node.Params : node.Params,
                        Log = m => log($"[{def.Name}] {m}"),
                        Publish = (topic, payload) => busPublish(topic, payload),
                        NodeId = node.Id,
                        Preview = payload => PreviewReady?.Invoke(node.Id, payload),
                    };

                    var result = def.Execute(ctx);
                    foreach (var port in def.Outputs)
                        outputs[(node.Id, port.Name)] = result.TryGetValue(port.Name, out var v) ? v : null;

                    setStatus(node, NodeStatus.Done, null);
                }
                catch (Exception ex)
                {
                    setStatus(node, NodeStatus.Error, ex.Message);
                    throw new Exception($"节点「{def.Name}」执行失败: {ex.Message}");
                }
            }
        }

    /// <summary>总线由插件在创建视图时注入（宿主 IBus 的包装）。</summary>
    public static Action<string, object?> busPublish { get; set; } = (_, _) =>
        throw new InvalidOperationException("总线未注入");

    /// <summary>卡片预览分发（多页面各自订阅，按 nodeId 过滤）。</summary>
    public static event Action<string, object>? PreviewReady;   // 载荷：ImagePayload（2D）/ CloudPayload（3D）
}
