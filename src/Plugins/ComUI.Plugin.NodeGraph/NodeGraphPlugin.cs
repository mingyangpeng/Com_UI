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
            (_activeEditor ?? _pages.LastOrDefault())?.PlaceNodeAtScreen(defId, screen);
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

    /// <summary>算子库单击：添加到最近打开的流水线页面中心。</summary>
    private void AddToActivePage(string defId) =>
        (_activeEditor ?? _pages.LastOrDefault())?.AddNodeCentered(defId);

    public void Shutdown()
    {
        _busSub?.Dispose();
        _busSub = null;
        _lockSub?.Dispose();
        _lockSub = null;
        foreach (var v in _views) v.Dispose();
        _views.Clear();
        _pages.Clear();
    }
}
