using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>按拓扑序执行整张节点图（后台线程调用；检测环路；逐节点回报状态）。</summary>
public static class GraphExecutor
{
    public static async Task RunAsync(
        GraphDocument graph,
        Action<NodeInstance, NodeStatus, string?> setStatus,
        Action<string> log,
        CancellationToken ct)
    {
        await Task.Run(() =>
        {
            // ===== 拓扑排序（Kahn）=====
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

            // ===== 依序执行 =====
            var outputs = new Dictionary<(string Node, string Port), object?>();

            foreach (var id in order)
            {
                ct.ThrowIfCancellationRequested();
                var node = byId[id];
                var def = NodeDefs.Map[node.DefId];

                setStatus(node, NodeStatus.Running, null);

                try
                {
                    // 收集输入
                    var inputs = new Dictionary<string, object?>();
                    foreach (var port in def.Inputs)
                    {
                        var conn = graph.FindInputConnection(node.Id, port.Name);
                        if (conn is null)
                            throw new Exception($"输入「{port.Name}」未连线");
                        if (!outputs.TryGetValue((conn.FromNode, conn.FromPort), out var value) || value is null)
                            throw new Exception($"输入「{port.Name}」的上游没有产出");
                        inputs[port.Name] = value;
                    }

                    var ctx = new NodeRunContext
                    {
                        Inputs = inputs,
                        Params = node.Params,
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
        }, ct);
    }

    /// <summary>总线由插件在创建视图时注入（宿主 IBus 的包装）。</summary>
    public static Action<string, object?> busPublish { get; set; } = (_, _) =>
        throw new InvalidOperationException("总线未注入");

    /// <summary>卡片预览分发（多页面各自订阅，按 nodeId 过滤）。</summary>
    public static event Action<string, object>? PreviewReady;   // 载荷：ImagePayload（2D）/ CloudPayload（3D）
}
