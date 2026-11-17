using System.IO;
using Avalonia.Controls;
using ComUI.Sdk;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 节点图插件（核心）：算法是独立步骤，每步一个节点，连线控制数据流向。
/// 文件菜单可"新建流水线页面"（多页面并存）；左侧算子库可拖拽创建节点；
/// 保存的算法进入算子库「算法」栏目，可拖拽/单击展开复用；
/// 参数内嵌在节点上运行时可改，改动后自动按拓扑序重跑；结果经数据总线发布到其他面板。
/// </summary>
[Plugin("ui.nodegraph", "算法流水线", "1.0.0",
    "ComfyUI 式节点编辑器：源/处理/输出节点、端口连线、运行时参数调节、自动重跑、保存算法复用、结果发布到总线")]
public sealed class NodeGraphPlugin : IPlugin
{
    private readonly List<IDisposable> _views = new();
    private readonly List<NodeEditorView> _pages = new();
    private NodeEditorView? _activeEditor;   // 当前可见（已挂可视树）的流水线页——算子库落点目标
    private NodeLibraryView? _library;
    private IPluginContext _ctx = null!;
    private int _pageCount;
    private string _algorithmsDir = "";
    private string _opOrderPath = "";       // 算子库栏目顺序持久化（config/op_order.json）
    private IDisposable? _busSub;
    private IDisposable? _lockSub;

    /// <summary>已保存的算法（文件名, 完整路径）。</summary>
    private readonly List<(string Name, string Path)> _algorithms = new();

    public void Initialize(IPluginContext context)
    {
        _ctx = context;
        context.Log("算法流水线插件已初始化");
        context.AddFileMenuEntry("新建流水线页面", NewPage);
        // 宿主「算法」侧边栏单击流水线算法 → 展开到最近页面
        _busSub = context.Bus.Subscribe<string>(BusTopics.PipelineOpen, OpenPipeline, uiThread: true);
        // 侧边栏锁定/解锁切换 → 同步已打开页面的只读状态
        _lockSub = context.Bus.Subscribe<string>(BusTopics.AlgoLocked, OnAlgoLockChanged, uiThread: true);

        // 会话恢复工厂：按算法文件路径重建流水线页（宿主启动恢复用）
        context.RegisterDocumentFactory("pipeline", path =>
        {
            var page = (NodeEditorView)OpenPage("流水线", openDynamic: false);
            page.LoadGraphFile(path);
            return page;
        });

        // 保存算法的目录：仓库根/config/algorithms
        var repo = Path.GetFullPath(Path.Combine(context.PluginFolder, "..", "..", ".."));
        _algorithmsDir = Path.Combine(repo, "config", "algorithms");
        _opOrderPath = Path.Combine(repo, "config", "op_order.json");
        try
        {
            Directory.CreateDirectory(_algorithmsDir);
            foreach (var f in Directory.EnumerateFiles(_algorithmsDir, "*.pipeline.json"))
                _algorithms.Add((FileNameToName(f), f));
        }
        catch (Exception ex)
        {
            context.LogError($"算法目录初始化失败: {ex.Message}");
        }

        // 注册 JSON 声明的算子（算法 DLL + *.node.json）
        try
        {
            var comdll = Path.GetFullPath(Path.Combine(context.PluginFolder, "..", ".."));
            foreach (var def in NodeJsonRegistry.LoadFrom(comdll, m => context.Log(m)))
                NodeDefs.Register(def);
        }
        catch (Exception ex)
        {
            context.LogError($"算子 JSON 注册失败: {ex.Message}");
        }

        // 注册「子流程」算子：已转为算子的流水线（params.json isOperator=true）动态注册为黑盒节点
        try
        {
            RegisterSubPipelineOperators(m => context.Log(m));
        }
        catch (Exception ex)
        {
            context.LogError($"子流程算子注册失败: {ex.Message}");
        }
    }

    private static string FileNameToName(string path)
    {
        var name = Path.GetFileNameWithoutExtension(path);
        return name.EndsWith(".pipeline", StringComparison.OrdinalIgnoreCase)
            ? name[..^".pipeline".Length]
            : name;
    }

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        new PanelDescriptor
        {
            Id = "ui.nodegraph.main",
            Title = "算法流水线",
            Role = PanelRole.Document,
            AutoOpen = true,
            // 注册面板走宿主标签页，不再重复开动态页
            CreateView = () => OpenPage("算法流水线", openDynamic: false),
        },
        new PanelDescriptor
        {
            Id = "ui.nodegraph.library",
            Title = "算子库",
            Role = PanelRole.Tool,
            DefaultDock = ToolDock.Left,
            Icon = "🧰",
            CreateView = CreateLibrary,
        },
    };

    private Control CreateLibrary()
    {
        var view = new NodeLibraryView(_opOrderPath);
        _library = view;
        view.OperatorClicked += AddToActivePage;
        view.OperatorPlaced += (defId, screen) =>
        {
            var node = (_activeEditor ?? _pages.LastOrDefault())?.PlaceNodeAtScreen(defId, screen);
            if (node is not null) ApplyParamOverrides(node, defId);   // 拖拽放置同样应用面板参数覆盖
        };
        _views.Add(view);
        return view;
    }

    /// <summary>文件菜单 → 新建空白流水线页面。</summary>
    private void NewPage()
    {
        _pageCount++;
        OpenPage($"流水线 {_pageCount}", openDynamic: true);
    }

    /// <summary>宿主「算法」侧边栏单击流水线算法 → 已打开则跳转到该页面，否则新开页面完整加载（连线+参数）。</summary>
    private void OpenPipeline(string path)
    {
        var name = FileNameToName(path);
        // 已打开（按文件路径去重）→ 聚焦已有标签页（宿主按内容去重，不重复建页）
        var existing = _pages.OfType<NodeEditorView>().FirstOrDefault(v => v.LoadedPath == path);
        if (existing is not null)
        {
            _ctx.OpenDocument(name, existing, null);   // 跳转已有页：恢复键已在其首次打开时登记
            _ctx.Log($"算法 {name} 已打开，跳转到该页面");
            return;
        }
        var page = OpenPage(name, openDynamic: true, restoreKey: $"pipeline:{path}") as NodeEditorView;
        page?.LoadGraphFile(path);
        _ctx.Log($"打开算法 {name}（新页面）");
    }

    /// <summary>侧边栏切换锁定状态 → 同步已打开页面的只读模式。</summary>
    private void OnAlgoLockChanged(string path)
    {
        var locked = ReadLocked(path);
        if (locked is null) return;
        foreach (var v in _pages.OfType<NodeEditorView>().Where(v => v.LoadedPath == path))
            v.SetReadOnly(locked.Value);
    }

    /// <summary>读取算法文件的 Locked 字段（Sdk.Ui.FileLock 统一约定；无字段 = 未锁定）。</summary>
    private static bool? ReadLocked(string path)
    {
        if (!File.Exists(path)) return null;
        return ComUI.Sdk.Ui.FileLock.IsLocked(path);
    }

    private Control OpenPage(string title, bool openDynamic, string? restoreKey = null)
    {
        var view = new NodeEditorView(_ctx.Bus, m => _ctx.Log(m));
        view.SetAlgorithmsDir(_algorithmsDir);   // 保存算法目录（动态页与主面板一致）
        view.AlgorithmSaved += (name, path) =>
        {
            _algorithms.Add((name, path));
            _ctx.Bus.Publish(BusTopics.AlgoSaved, path);   // 宿主「算法」侧边栏即时刷新
        };
        _pages.Add(view);
        _views.Add(view);
        view.AttachedToVisualTree += (_, _) => _activeEditor = view;   // 标签选中=挂树=成为落点目标
        if (openDynamic)
            _ctx.OpenDocument(title, view, restoreKey);
        return view;
    }

    /// <summary>算子库单击：添加到最近打开的流水线页面中心（应用算子面板里编辑的参数覆盖）。</summary>
    private void AddToActivePage(string defId)
    {
        var page = (_activeEditor ?? _pages.LastOrDefault());
        if (page is null) return;
        var node = page.AddNodeCentered(defId);
        if (node is not null) ApplyParamOverrides(node, defId);
    }

    /// <summary>把算子面板编辑的参数覆盖应用到新节点（数值/布尔按类型解析，其余存文本）。</summary>
    private void ApplyParamOverrides(NodeInstance node, string defId)
    {
        if (_library is null) return;
        if (!_library.ParamOverrides.TryGetValue(defId, out var ov)) return;
        var def = NodeDefs.Map[node.DefId];
        foreach (var p in def.Params)
        {
            if (!ov.TryGetValue(p.Name, out var raw) || raw is null) continue;
            node.Params[p.Name] = p.Type switch
            {
                ComUI.Sdk.AlgoParamType.Number => double.TryParse(raw, out var dv) ? dv : p.Default,
                ComUI.Sdk.AlgoParamType.Int => int.TryParse(raw, out var iv) ? iv : p.Default,
                ComUI.Sdk.AlgoParamType.Boolean => raw == "true" || raw == "True",
                _ => raw,
            };
        }
    }

    /// <summary>
    /// 子流程算子注册：「⬆ 转为算子」标记的流水线（params.json isOperator=true）→ 动态注册为
    /// 算子库「子流程」栏目的黑盒节点。端口自动推导：图内入度 0 且有输入端口的节点 = 输入端口
    /// （父图喂数据），出度 0 且有输出端口的节点 = 输出端口。参数扁平化为 "节点名.参数名"
    /// （与 params.json 同键），执行时映射回子图节点并整图运行（复用 GraphExecutor）。
    /// </summary>
    private void RegisterSubPipelineOperators(Action<string> log)
    {
        foreach (var (_, pipelinePath) in _algorithms)
        {
            try
            {
                var paramsPath = Path.Combine(Path.GetDirectoryName(pipelinePath)!,
                    Path.GetFileNameWithoutExtension(pipelinePath) + ".params.json");
                if (!File.Exists(paramsPath)) continue;

                using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(paramsPath));
                if (!doc.RootElement.TryGetProperty("isOperator", out var isOp) || !isOp.GetBoolean()) continue;

                var graph = new GraphDocument();
                graph.LoadFromJson(File.ReadAllText(pipelinePath));
                var defId = "sub." + Path.GetFileNameWithoutExtension(pipelinePath).Replace(' ', '_');

                // 输入端口：入度 0 且有输入端口的节点（需要外部数据）
                var inDeg = graph.Nodes.ToDictionary(n => n.Id, _ => 0);
                foreach (var c in graph.Connections)
                    if (graph.Nodes.Any(n => n.Id == c.ToNode)) inDeg[c.ToNode]++;

                var inputs = new List<PortDef>();
                var outputs = new List<PortDef>();
                foreach (var n in graph.Nodes)
                {
                    var def = NodeDefs.Map[n.DefId];
                    var label = n.Title ?? def.Name;
                    if (inDeg[n.Id] == 0)
                        foreach (var p in def.Inputs)
                            inputs.Add(new PortDef { Name = $"{label}.{p.Name}", Type = p.Type });
                    if (!graph.Connections.Any(c => c.FromNode == n.Id))
                        foreach (var p in def.Outputs)
                            outputs.Add(new PortDef { Name = $"{label}.{p.Name}", Type = p.Type });
                }

                // 参数扁平化（节点名.参数名）——与 params.json 同键，面板/保存同步共用
                var flatParams = new List<ComUI.Sdk.AlgoParam>();
                foreach (var n in graph.Nodes)
                {
                    var def = NodeDefs.Map[n.DefId];
                    var label = n.Title ?? def.Name;
                    foreach (var p in def.Params)
                        flatParams.Add(new ComUI.Sdk.AlgoParam
                        {
                            Name = $"{label}.{p.Name}", Label = $"{label}·{p.Label ?? p.Name}",
                            Type = p.Type, Default = n.Params.TryGetValue(p.Name, out var v) ? v : p.Default,
                            Min = p.Min, Max = p.Max, Options = p.Options,
                        });
                }

                var pipelinePathCopy = pipelinePath;
                var desc = $"子流程：{graph.Nodes.Count} 节点 / {graph.Connections.Count} 连线（双击编辑可改）";
                var subDef = new NodeDef
                {
                    Id = defId,
                    Name = Path.GetFileNameWithoutExtension(pipelinePath),
                    Category = "处理",
                    Group = "子流程",
                    Description = desc,
                    Inputs = inputs.ToArray(),
                    Outputs = outputs.ToArray(),
                    Params = flatParams.ToArray(),
                    Execute = ctx => RunSubPipeline(pipelinePathCopy, ctx),
                };
                if (NodeDefs.Register(subDef))
                    log($"子流程算子已注册: {subDef.Name} <{defId}>（{inputs.Count} 入 / {outputs.Count} 出）");
            }
            catch (Exception ex)
            {
                // 单个流水线损坏不阻断其它子流程注册
                log($"子流程算子注册失败 {Path.GetFileName(pipelinePath)}: {ex.Message}");
            }
        }
    }

    /// <summary>子流程嵌套深度（子流程套子流程允许；自引用/循环引用在此拦截——
    /// 无护栏会无限递归栈溢出直接杀进程，不可恢复）。</summary>
    private static int _subPipelineDepth;

    /// <summary>子流程算子执行：加载子图 → 应用扁平化参数 → 预置输入端口 → 整图运行 → 收集输出端口值。</summary>
    private static Dictionary<string, object?> RunSubPipeline(string pipelinePath, NodeRunContext ctx)
    {
        if (++_subPipelineDepth > 8)
        {
            _subPipelineDepth--;
            throw new Exception("子流程嵌套过深（疑似自引用循环）——子流程算子不能再引用包含它自己的流水线");
        }
        try
        {
            return RunSubPipelineInner(pipelinePath, ctx);
        }
        finally
        {
            _subPipelineDepth--;
        }
    }

    private static Dictionary<string, object?> RunSubPipelineInner(string pipelinePath, NodeRunContext ctx)
    {
        var sub = new GraphDocument();
        sub.LoadFromJson(File.ReadAllText(pipelinePath));

        // 扁平化参数（节点名.参数名）映射回子图节点
        foreach (var n in sub.Nodes)
        {
            var def = NodeDefs.Map[n.DefId];
            var label = n.Title ?? def.Name;
            foreach (var p in def.Params)
            {
                var key = $"{label}.{p.Name}";
                if (ctx.Params.TryGetValue(key, out var v))
                    n.Params[p.Name] = v;
            }
        }

        // 输入端口绑定：端口名 = 节点名.端口名 → 预置到对应子图节点的输出
        var presets = new Dictionary<(string, string), object?>();
        var inDeg = sub.Nodes.ToDictionary(n => n.Id, _ => 0);
        foreach (var c in sub.Connections)
            if (sub.Nodes.Any(n => n.Id == c.ToNode)) inDeg[c.ToNode]++;

        foreach (var n in sub.Nodes)
        {
            if (inDeg[n.Id] != 0) continue;
            var def = NodeDefs.Map[n.DefId];
            var label = n.Title ?? def.Name;
            foreach (var p in def.Inputs)
            {
                var key = $"{label}.{p.Name}";
                if (ctx.Inputs.TryGetValue(key, out var v) && v is not null)
                    presets[(n.Id, p.Name)] = v;
            }
        }

        // 同步整图运行（Execute 已在后台线程，无 UI 同步上下文）+ 收集子图全部输出
        var collected = new Dictionary<(string, string), object?>();
        GraphExecutor.RunAsync(sub, (_, _, _) => { }, m => { }, CancellationToken.None, presets, collected)
                     .GetAwaiter().GetResult();

        // 收集输出端口：出度 0 节点的输出
        var result = new Dictionary<string, object?>();
        foreach (var n in sub.Nodes)
        {
            if (sub.Connections.Any(c => c.FromNode == n.Id)) continue;
            var def = NodeDefs.Map[n.DefId];
            var label = n.Title ?? def.Name;
            foreach (var p in def.Outputs)
            {
                var key = $"{label}.{p.Name}";
                if (collected.TryGetValue((n.Id, p.Name), out var v))
                    result[key] = v;
            }
        }
        return result;
    }

    public void Shutdown()
    {
        _busSub?.Dispose();
        _busSub = null;
        _lockSub?.Dispose();
        _lockSub = null;
        foreach (var v in _views) v.Dispose();
        _views.Clear();
        _pages.Clear();
        _library = null;
        _activeEditor = null;
        _algorithms.Clear();
    }
}
