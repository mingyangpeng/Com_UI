using System;
using System.Runtime.InteropServices;
using System.Globalization;
using System.IO;
using Avalonia;
using Avalonia.Collections;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Interactivity;
using Avalonia.VisualTree;
using Path = Avalonia.Controls.Shapes.Path;
using IOPath = System.IO.Path;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using ComUI.Sdk;
using ComUI.Sdk.Ui;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 节点图编辑器（ComfyUI 式）：
/// 节点卡片可拖动、参数内嵌运行时可改；端口拖拽连线（贝塞尔曲线，按类型着色），
/// 从已连线的输入端口拖出=断开重连；画布支持平移与滚轮缩放；
/// 卡片右下角把手拖拽拉伸宽度（ComfyUI 式：高度恒为内容自适应、预览随宽放大、字号不变；持久化 Width）；
/// 修改参数/连线后自动按拓扑序重跑整图，结果经数据总线发布。
/// </summary>
public sealed class NodeEditorView : UserControl, IDisposable, IPanelParamsProvider
{
        // ===== 卡片布局常量（端口中心位置按此精确计算） =====
    private const double CardW = 210, HeaderH = 28, BodyPad = 8, RowH = 24, ParamH = 30, PortD = 12;
    private const double CloudPreviewRatio = 0.75;   // 3D 预览高/宽比（无图像纵横比可循，取 4:3 视口）
    private const string DragFormat = "NodeDefId";
    private const string PipelineFormat = "PipelinePath";

    private readonly GraphDocument _graph = new();
    private readonly IBus _bus;
    private readonly Action<string> _log;

    private readonly Canvas _world = new();
    private readonly Canvas _edges = new();
    private readonly Canvas _tempLayer = new();   // 连线预览（独立层，松手即清空）
    private readonly Canvas _scopeLayer = new();  // 循环作用域框选可视化（虚线框，最底层）
    private readonly Canvas _nodesLayer = new();
    private readonly ScaleTransform _scaleT = new(1, 1);
    private readonly TranslateTransform _transT = new();
    private readonly Border _editor;

    private readonly Dictionary<string, Border> _cards = new();
    private readonly Dictionary<string, Ellipse> _dots = new();
    private sealed class PreviewSlot
    {
        public Border Box = null!;
        public Image? Img;                       // 2D 缩略图
        public Sdk.Ui.ICloudPreview? Cloud;      // 3D 迷你视图（经 Sdk 预览契约创建）
        public ImagePayload? LastImage;
        public CloudPayload? LastCloud;
    }

    private readonly Dictionary<string, PreviewSlot> _previewSlots = new();
    private readonly Dictionary<string, Button> _previewExportButtons = new();
    private readonly Dictionary<string, TextBox> _previewNameBoxes = new();   // draw 卡片变量名框（默认名跟随输入）

    private enum DragMode { None, Pan, Node, Connect, Resize, ScopeSelect, EdgeBend, ScopeMove, ScopeResize }
    private Point _scopeStart;          // 框选起点（世界坐标）
    private Rectangle? _rubberBand;     // 框选橡皮筋（临时）

    // 连线形状拖动：在按下处插入/命中锚点，拖动该锚点实现局部变形（端点不变）
    private Connection? _bendEdge;
    private AnchorPoint? _dragAnchor;              // 正在拖的锚点
    private Point _dragAnchorPrev;                 // 锚点上次位置（增量基准）
    // 作用域框编辑（双击框）
    private ScopeRect? _editScope;                 // 正在编辑的作用域（指向循环节点的 Scope 实例）
    private string? _editScopeNodeId;              // 拥有该 Scope 的循环节点 Id
    private enum ScopeHandle { None, Move, L, R, T, B, TL, TR, BL, BR }
    private ScopeHandle _scopeHandle;
    private Point _scopeDragStart;                 // 拖动起点（世界坐标）
    private ScopeRect _scopeDragOriginal;          // 拖动开始时的矩形副本
    private DragMode _mode = DragMode.None;
    private double _resizeStartW;   // Resize 按压时的卡片宽（增量基于起点；ComfyUI 式仅宽度可拉）
    private Point _lastScreen;
    private string? _dragNode;
    private (string Node, string Port, PortType Type)? _pending;
    private Path? _tempEdge;
    private string? _selectedNode;   // 选中卡片（Del 删除目标）
    private Connection? _selectedEdge;   // 选中的连线（Del 删除目标；点画布/卡片取消）
    private readonly Dictionary<(string NodeId, string Port, bool Input), Ellipse> _portDots = new();   // 端口点实际控件（连线锚点以其布局位置为准）
    private bool _edgeLayoutDirty;   // 锚点在布局完成前用了公式兜底 → 布局完成后补一次重绘
    private string? _clipNode;       // 卡片剪贴板（Ctrl+C 的节点片段 JSON）
    private int _pasteSeq;           // 连续粘贴的级联偏移序号

    private bool _autoRun = false;   // 自动运行默认关（用户定案）：改参数不自动重跑，手动 ▶ 运行
    private bool _running;
    private string _algorithmsDir = "";

    /// <summary>本页加载的算法文件路径（null = 未加载文件的新页面）——用于"已打开则跳转"去重。</summary>
    public string? LoadedPath { get; private set; }

    /// <summary>只读模式（加载锁定的算法）：禁止改参数/连线/节点，保存按钮变「复制副本」。</summary>
    private bool _readOnly;
    private Button _saveBtn = null!;
    private Button _runBtn = null!;    // 运行状态视觉反馈：运行中变绿
    private Button _stopBtn = null!;   // 运行状态视觉反馈：空闲置灰禁用
    private Button _toOperatorBtn = null!;   // 转为算子：已是算子后按钮文案反馈
    /// <summary>卡片参数显示刷新回调：(节点Id, 参数名) → 设置卡片编辑器显示。右侧属性面板编辑时调用。</summary>
    private readonly Dictionary<(string NodeId, string Param), Action<string>> _paramRefreshers = new();

    private static readonly IBrush RunActiveBrush = new SolidColorBrush(Color.FromRgb(0x2E, 0x8B, 0x3E));
    private const long MinRunVisibleMs = 600;   // 运行态最短可见时长（瞬间完成的流水线也保持反馈）

    /// <summary>算法保存成功后触发（文件名, 完整路径）—— 算子库据此新增"算法"条目并即时注册子流程算子。</summary>
    public event Action<string, string>? AlgorithmSaved;

    /// <summary>设置算法保存目录（宿主 config/algorithms）。</summary>
    public void SetAlgorithmsDir(string dir) => _algorithmsDir = dir;

    /// <summary>获取算子库现有栏目列表（转算子时供用户选择归属栏目）。由插件注入。</summary>
    public Func<IReadOnlyList<string>>? GroupSource { get; set; }
    private readonly DispatcherTimer _debounce;
    private CancellationTokenSource? _runCts;

    private static readonly Brush Hairline = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0xFF, 0xFF));
    private static readonly Brush DimText = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    public NodeEditorView(IBus bus, Action<string> log)
    {
        _bus = bus;
        _log = log;
        GraphExecutor.busPublish = (t, v) => _bus.Publish(t, v);
        GraphExecutor.PreviewReady += OnNodePreview;   // draw 卡片内嵌预览（按 nodeId 过滤本页卡片）

        _world.Children.Add(_scopeLayer);   // 最底层：循环作用域虚线框（双击可编辑）
        _scopeLayer.IsHitTestVisible = true;
        _world.Children.Add(_edges);
        _edges.IsHitTestVisible = true;   // 连线需要可点击选中（节点层在其后添加，仍优先命中节点）
        _world.Children.Add(_tempLayer);
        _tempLayer.IsHitTestVisible = false;
        _world.Children.Add(_nodesLayer);
        _world.RenderTransform = new TransformGroup { Children = { _scaleT, _transT } };

        _editor = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x14, 0x14, 0x19)),
            Child = _world,
            ClipToBounds = true,
            Focusable = true,   // 点画布即聚焦：Ctrl+S 等快捷键可达本视图
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        root.Children.Add(BuildToolbar());
        _editor.SetValue(Grid.RowProperty, 1);
        root.Children.Add(_editor);
        Content = root;

        // Ctrl+S = 保存（已加载文件直接写回；新页面弹另存对话框；锁定页复制副本）
        KeyDown += (_, e) =>
        {
            if (e.Key == Key.S && e.KeyModifiers.HasFlag(KeyModifiers.Control))
            {
                e.Handled = true;
                _ = SaveOrCopyAsync();
            }
            else if (e.Key == Key.Delete && !e.Handled && !_readOnly && _editScope is not null)
            {
                // 作用域编辑态 Del = 清除该循环节点的作用域框（回退连线闭包作用域）——优先于删节点
                e.Handled = true;
                var owner = _editScopeNodeId is not null ? _graph.FindNode(_editScopeNodeId) : null;
                if (owner is not null) owner.Scope = null;
                _editScope = null;
                _editScopeNodeId = null;
                RedrawScopes();
                SaveScopeConfig();
                _log("已清除循环作用域——作用域回退为「计数」连线闭包");
            }
            else if (e.Key == Key.Back && !e.Handled && !_readOnly && _selectedEdge is { } resetEdge)
            {
                // Backspace = 重置选中连线的形状（清锚点+Bend，恢复直线），不删除连线本身
                e.Handled = true;
                resetEdge.Anchors.Clear();
                resetEdge.Bend = resetEdge.Bend2 = 0;
                RedrawEdges();
                SaveScopeConfig();
                _log("已重置连线形状为直线（锚点已清除）；Del 才是删除连线");
            }
            else if (e.Key == Key.Delete && !e.Handled && !_readOnly && _selectedEdge is { } edge)
            {
                e.Handled = true;
                var fromNode = _graph.FindNode(edge.FromNode);
                var toNode = _graph.FindNode(edge.ToNode);
                _graph.Connections.RemoveAll(c => ReferenceEquals(c, edge));
                _selectedEdge = null;
                RedrawEdges();
                ScheduleAutoRun();
                if (fromNode is not null && toNode is not null)
                    _log($"删除连线「{NodeDefs.Map[fromNode.DefId].Name} → {NodeDefs.Map[toNode.DefId].Name}」");
            }
            else if (e.Key == Key.Delete && !e.Handled && !_readOnly && _selectedNode is { } sel)
            {
                e.Handled = true;
                SelectCard(null);
                DeleteNode(sel);
            }
            else if (e.Key == Key.C && e.KeyModifiers.HasFlag(KeyModifiers.Control) && !e.Handled
                     && _selectedNode is { } cp && e.Source is not TextBox)
            {
                var src = _graph.FindNode(cp);
                if (src is null) return;
                e.Handled = true;
                _clipNode = _graph.CopyNodeToJson(cp);
                _pasteSeq = 0;
                _log($"已复制节点「{NodeDefs.Map[src.DefId].Name}」——Ctrl+V 粘贴副本");
            }
            else if (e.Key == Key.V && e.KeyModifiers.HasFlag(KeyModifiers.Control) && !e.Handled
                     && _clipNode is not null && !_readOnly && e.Source is not TextBox)
            {
                e.Handled = true;
                _pasteSeq++;
                if (_graph.PasteNode(_clipNode, 24 * _pasteSeq, 24 * _pasteSeq) is not { } pasted) return;
                BuildCard(pasted, NodeDefs.Map[pasted.DefId]);
                SelectCard(pasted.Id);
                ScheduleAutoRun();
                _log($"已粘贴节点「{NodeDefs.Map[pasted.DefId].Name}」");
            }
        };

        // 画布平移 / 缩放
        _editor.PointerPressed += EditorPressed;
        _editor.PointerWheelChanged += EditorWheel;
        PointerMoved += OnPointerMoved;
        PointerReleased += OnPointerReleased;

        // 端口锚点依赖布局：卡片尺寸变化（如预览区出现）/标签激活后的首次布局都要补画连线
        _world.LayoutUpdated += (_, _) =>
        {
            if (!_edgeLayoutDirty) return;
            _edgeLayoutDirty = false;
            RedrawEdges();
        };

        _debounce = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(500) };
        _debounce.Tick += (_, _) => { _debounce.Stop(); if (_autoRun) RunGraph(); };

        // 画布接收算子库拖拽
        _editor.AddHandler(DragDrop.DragOverEvent, (_, e) =>
        {
            e.DragEffects = e.Data.Contains(DragFormat) || e.Data.Contains(PipelineFormat)
                ? DragDropEffects.Copy : DragDropEffects.None;
            e.Handled = true;
        });
        _editor.AddHandler(DragDrop.DropEvent, (_, e) =>
        {
            if (_readOnly) { _log("算法已锁定（只读），解锁或复制副本后才能修改"); return; }
            var p = e.GetPosition(_world);
            if (e.Data.Contains(DragFormat))
            {
                AddNodeAtCenter((string)e.Data.Get(DragFormat)!, p.X, p.Y);
                e.Handled = true;
            }
            else if (e.Data.Contains(PipelineFormat) && e.Data.Get(PipelineFormat) is string path && File.Exists(path))
            {
                PasteGraphFile(path, p);   // 拖入已保存的算法 = 展开整段编排
                e.Handled = true;
            }
        });
    }

    public void Dispose()
    {
        // 视图销毁时若仍在全屏，恢复窗口并解绑 Esc 监听，避免残留
        if (_fullscreenWindow is { } win)
        {
            if (_fullscreenKeyHandler is not null) win.KeyDown -= _fullscreenKeyHandler;
            if (win.WindowState == WindowState.FullScreen) win.WindowState = _fullscreenPrevState;
            _fullscreenWindow = null;
            _fullscreenKeyHandler = null;
        }
    }

    // ==================== 工具栏 ====================

    // 工具栏图标（纯图标按钮，名称在 ToolTip）
    private const string IconRun = "▶";       // 运行
    private const string IconRunning = "⏸";   // 运行中（点击/状态指示）
    private const string IconStop = "■";       // 停止
    private const string IconAuto = "⚡";      // 自动运行
    private const string IconSave = "💾";      // 保存算法
    private const string IconCopy = "📋";      // 复制副本（只读）
    private const string IconClear = "🧹";     // 清空画布（扫帚，比垃圾桶更贴切"清扫"）
    private const string IconExport = "⬇";     // 导出 C++
    private const string IconToOp = "⬆";       // 转为算子
    private const string IconIsOp = "✓";       // 已是算子
    private const string IconFullscreen = "⛶"; // 全屏（Esc 退出）
    private const string IconFit = "⊡";        // 视野内（全部节点落入视口）

    private Button? _autoBtn;
    private Window? _fullscreenWindow;         // 当前由本视图切入全屏的窗口
    private WindowState _fullscreenPrevState = WindowState.Normal;   // 进入全屏前的窗口状态
    private EventHandler<KeyEventArgs>? _fullscreenKeyHandler;   // Esc 退出全屏的窗口级监听

    private Control BuildToolbar()
    {
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };

        // ＋ 添加节点 / 示例图 已移除（用户定案）：建节点走算子库拖拽/单击；示例不再预置
        var runBtn = ToolIcon(IconRun, "运行流水线");
        runBtn.Click += (_, _) => RunGraph();
        _runBtn = runBtn;
        bar.Children.Add(runBtn);

        var stopBtn = ToolIcon(IconStop, "停止运行");
        stopBtn.Click += (_, _) =>
        {
            if (!_running || _runCts is null) { _log("当前没有正在执行的流水线"); return; }
            _runCts.Cancel();   // RunGraph 捕获取消并记日志"流水线已取消"
        };
        _stopBtn = stopBtn;
        bar.Children.Add(stopBtn);
        SetRunState(false);   // 初始：停止钮置灰禁用

        var autoBtn = ToolIcon(IconAuto, "自动运行：关");
        _autoBtn = autoBtn;
        UpdateAutoVisual(autoBtn);
        autoBtn.Click += (_, _) =>
        {
            _autoRun = !_autoRun;
            UpdateAutoVisual(autoBtn);
            _log($"自动运行已{(_autoRun ? "开启" : "关闭")}");
        };
        bar.Children.Add(autoBtn);

        var saveBtn = ToolIcon(IconSave, "保存算法");
        saveBtn.Click += (_, _) => _ = SaveOrCopyAsync();
        _saveBtn = saveBtn;
        bar.Children.Add(saveBtn);

        var clearBtn = ToolIcon(IconClear, "清空画布");
        clearBtn.Click += async (_, _) =>
        {
            if (_readOnly) { _log("算法已锁定（只读），解锁或复制副本后才能修改"); return; }
            if (_graph.Nodes.Count == 0 && _graph.Connections.Count == 0) { _log("画布已是空的"); return; }
            var owner = TopLevel.GetTopLevel(this) as Window;
            bool confirmed = await ConfirmDialog.ShowAsync(
                owner, "清空画布",
                $"将移除当前页面全部 {_graph.Nodes.Count} 个算子和 {_graph.Connections.Count} 条连线，且无法撤销。确定继续？",
                "清空", danger: true);
            if (confirmed) ClearAll();
        };
        bar.Children.Add(clearBtn);

        var exportCppBtn = ToolIcon(IconExport, "导出 C++ 工程");
        exportCppBtn.Click += (_, _) => _ = ExportCppAsync();
        bar.Children.Add(exportCppBtn);

        var toOpBtn = ToolIcon(IconToOp, "转为算子");
        toOpBtn.Click += (_, _) => _ = ToOperatorAsync();
        _toOperatorBtn = toOpBtn;
        bar.Children.Add(toOpBtn);

        // —— 视图操作组 ——
        var fitBtn = ToolIcon(IconFit, "视野内：所有算子卡片落入视口");
        fitBtn.Click += (_, _) => FitAllNodesInView();
        bar.Children.Add(fitBtn);

        var fullBtn = ToolIcon(IconFullscreen, "全屏（Esc 退出）");
        fullBtn.Click += (_, _) => ToggleFullscreen();
        bar.Children.Add(fullBtn);

        bar.Children.Add(new TextBlock
        {
            Text = "算子库：单击选中，再单击加入画布 · 拖端口连线 · 右击空白处拖框=设循环作用域（框内含循环算子）· Del/右击节点删除 · 滚轮缩放",
            VerticalAlignment = VerticalAlignment.Center,
            FontSize = 11,
            Foreground = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C)),
            Margin = new Thickness(6, 0, 0, 0),
        });

        return new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x23, 0x23, 0x26)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x31, 0x31, 0x35)),
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(12, 8, 12, 8),
            Child = bar,
        };
    }

    private static Button Ghost(string text)
    {
        var b = new Button { Content = text };
        b.Classes.Add("ghost");
        return b;
    }

    /// <summary>
    /// macOS 工具栏纯图标按钮：固定小方块 + 统一图标尺寸，完整名称放 ToolTip。
    /// 图标用 Viewbox 归一化到 15×15，避免 emoji 在不同字体下大小不一。
    /// </summary>
    private static Button ToolIcon(string icon, string tip)
    {
        var b = new Button
        {
            Width = 30, Height = 26, Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Content = new Viewbox
            {
                Width = 15, Height = 15, Stretch = Stretch.Uniform,
                Child = new TextBlock
                {
                    Text = icon, FontSize = 13,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                },
            },
        };
        b.Classes.Add("ghost");
        ToolTip.SetTip(b, tip);
        return b;
    }

    /// <summary>更新自动运行按钮：开=亮色前景，关=灰色前景；ToolTip 显示状态。</summary>
    private void UpdateAutoVisual(Button btn)
    {
        btn.Opacity = _autoRun ? 1.0 : 0.45;
        ToolTip.SetTip(btn, _autoRun ? "自动运行：开（点击关闭）" : "自动运行：关（点击开启）");
    }

    /// <summary>替换图标按钮的图标字符（保留 Viewbox 结构）并同步 ToolTip。</summary>
    private static void SetIcon(Button btn, string icon, string? tip = null)
    {
        if (btn.Content is Viewbox vb && vb.Child is TextBlock tb)
            tb.Text = icon;
        if (tip is not null) ToolTip.SetTip(btn, tip);
    }

    /// <summary>视野内：调整缩放和平移，使所有算子卡片落入当前视口（留 8% 边距）。</summary>
    private void FitAllNodesInView()
    {
        if (_graph.Nodes.Count == 0) { _log("画布为空，无需调整视野"); return; }
        double minX = double.MaxValue, minY = double.MaxValue;
        double maxX = double.MinValue, maxY = double.MinValue;
        foreach (var n in _graph.Nodes)
        {
            // 卡片实际布局高度优先（内容自适应），未布局时用估算高度
            double w = _cards.TryGetValue(n.Id, out var card) && card.Bounds.Width > 0 ? card.Bounds.Width : (n.Width ?? AutoCardWidth(NodeDefs.Map[n.DefId]));
            double h = _cards.TryGetValue(n.Id, out var c2) && c2.Bounds.Height > 0 ? c2.Bounds.Height : 160;
            minX = Math.Min(minX, n.X);
            minY = Math.Min(minY, n.Y);
            maxX = Math.Max(maxX, n.X + w);
            maxY = Math.Max(maxY, n.Y + h);
        }
        var vp = _editor.Bounds;
        if (vp.Width <= 0 || vp.Height <= 0) { _log("视口尚未就绪，请稍后再试"); return; }

        double contentW = Math.Max(1, maxX - minX), contentH = Math.Max(1, maxY - minY);
        double scale = Math.Min(vp.Width / contentW, vp.Height / contentH) * 0.92;
        scale = Math.Clamp(scale, 0.35, 2.5);
        _scaleT.ScaleX = _scaleT.ScaleY = scale;
        // 让内容中心对齐视口中心
        double cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
        _transT.X = vp.Width / 2 - cx * scale;
        _transT.Y = vp.Height / 2 - cy * scale;
        _log($"视野已调整：{_graph.Nodes.Count} 个节点全部落入视口（缩放 {scale:F2}×）");
    }

    /// <summary>切换_fullscreen：本视图所在窗口进入全屏；Esc 退出并恢复原窗口状态。</summary>
    private void ToggleFullscreen()
    {
        var win = TopLevel.GetTopLevel(this) as Window;
        if (win is null) { _log("无法获取顶层窗口，全屏不可用"); return; }

        // 已在全屏 → 退出
        if (_fullscreenWindow is not null && ReferenceEquals(_fullscreenWindow, win) &&
            win.WindowState == WindowState.FullScreen)
        {
            ExitFullscreen();
            return;
        }

        // 进入全屏：记录窗口与原状态，挂窗口级 Esc 监听（画布内任意焦点都能响应）
        // 若本视图之前挂在别的窗口上（页面重开等），先清理旧监听
        if (_fullscreenWindow is not null && _fullscreenKeyHandler is not null)
            _fullscreenWindow.KeyDown -= _fullscreenKeyHandler;
        _fullscreenWindow = win;
        _fullscreenPrevState = win.WindowState;
        _fullscreenKeyHandler = (_, e) =>
        {
            if (e.Key == Key.Escape)
            {
                e.Handled = true;
                ExitFullscreen();
            }
        };
        win.KeyDown += _fullscreenKeyHandler;
        win.WindowState = WindowState.FullScreen;
        _log("已进入全屏（Esc 退出）");
    }

    private void ExitFullscreen()
    {
        if (_fullscreenWindow is not { } win) return;
        if (_fullscreenKeyHandler is not null) win.KeyDown -= _fullscreenKeyHandler;
        _fullscreenKeyHandler = null;
        if (win.WindowState == WindowState.FullScreen)
            win.WindowState = _fullscreenPrevState;
        _fullscreenWindow = null;
        _log("已退出全屏");
    }

    // ==================== 坐标 / 视图 ====================

    private Point ViewportCenter()
    {
        var c = _editor.Bounds.Center;
        return new Point((c.X - _transT.X) / _scaleT.ScaleX, (c.Y - _transT.Y) / _scaleT.ScaleY);
    }

    private void EditorPressed(object? sender, PointerPressedEventArgs e)
    {
        // 连线等子元素已 Handled 的事件不处理，避免冒泡到这里把刚选中的连线又清空
        if (e.Handled) return;
        SelectCard(null);
        SelectEdge(null);   // 点画布空白同时取消连线选中（内部去重，无选中时零开销）
        if (_editScope is not null)   // 退出作用域编辑态
        {
            _editScope = null;
            _editScopeNodeId = null;
            RedrawScopes();
        }
        var props = e.GetCurrentPoint(_editor).Properties;
        // 仅空白画布（world/_editor 自身）触发平移/框选；节点、滑块等内部 Border 不得抢捕获
        bool onBackground = ReferenceEquals(e.Source, _world) || ReferenceEquals(e.Source, _editor) ||
                            ReferenceEquals(e.Source, _edges) || ReferenceEquals(e.Source, _scopeLayer);
        if (props.IsRightButtonPressed && onBackground && !_readOnly)
        {
            // 右击空白拖动 = 框选循环作用域（松开时赋给框内最近的循环节点）
            _mode = DragMode.ScopeSelect;
            _scopeStart = e.GetPosition(_world);
            e.Pointer.Capture(this);
            e.Handled = true;
            _editor.Focus();
        }
        else if (props.IsMiddleButtonPressed || (props.IsLeftButtonPressed && onBackground))
        {
            _mode = DragMode.Pan;
            _lastScreen = e.GetPosition(this);
            e.Pointer.Capture(this);
            e.Handled = true;
            if (onBackground) _editor.Focus();
        }
    }

    private void EditorWheel(object? sender, PointerWheelEventArgs e)
    {
        double old = _scaleT.ScaleX;
        double factor = e.Delta.Y > 0 ? 1.15 : 1 / 1.15;
        double @new = Math.Clamp(old * factor, 0.35, 2.5);
        if (Math.Abs(@new - old) < 0.001) return;

        var mouse = e.GetPosition(_editor);
        double wx = (mouse.X - _transT.X) / old, wy = (mouse.Y - _transT.Y) / old;
        _scaleT.ScaleX = _scaleT.ScaleY = @new;
        _transT.X = mouse.X - wx * @new;
        _transT.Y = mouse.Y - wy * @new;
        e.Handled = true;
    }

    private void OnPointerMoved(object? sender, PointerEventArgs e)
    {
        switch (_mode)
        {
            case DragMode.Pan:
            {
                var p = e.GetPosition(this);
                _transT.X += p.X - _lastScreen.X;
                _transT.Y += p.Y - _lastScreen.Y;
                _lastScreen = p;
                break;
            }
            case DragMode.Node when _dragNode is not null:
            {
                var p = e.GetPosition(this);
                double dx = (p.X - _lastScreen.X) / _scaleT.ScaleX;
                double dy = (p.Y - _lastScreen.Y) / _scaleT.ScaleY;
                _lastScreen = p;
                var n = _graph.FindNode(_dragNode)!;
                n.X += dx; n.Y += dy;
                if (_cards.TryGetValue(n.Id, out var card))
                {
                    Canvas.SetLeft(card, n.X);
                    Canvas.SetTop(card, n.Y);
                }
                RedrawEdges();
                break;
            }
            case DragMode.Resize when _dragNode is not null:
            {
                var p = e.GetPosition(this);
                // ComfyUI 式拉伸：仅宽度（行横向跟随变宽、预览随宽放大、字号不变）；
                // 高度恒为内容自适应，不存在独立拉高 → 永无底部空白。增量始终基于按压起点
                double w = Math.Clamp(_resizeStartW + (p.X - _lastScreen.X) / _scaleT.ScaleX, CardW, 900);
                if (_graph.FindNode(_dragNode) is { } rn) rn.Width = w;
                if (_cards.TryGetValue(_dragNode, out var rcard)) rcard.Width = w;
                RedrawEdges();   // 输出端口随宽度右移，连线即时跟随
                break;   // 不更新 _lastScreen：增量始终基于按压起点
            }
            case DragMode.Connect:
            {
                var w = e.GetPosition(_world);
                UpdateTempEdge(w);
                break;
            }
            case DragMode.EdgeBend when _bendEdge is not null:
            {
                if (_dragAnchor is not null)
                {
                    // 拖锚点：整个位移直接给该锚点，只有它所在的两段跟手，端点与其它锚点不动
                    var p = e.GetPosition(_world);
                    _dragAnchor.X += p.X - _dragAnchorPrev.X;
                    _dragAnchor.Y += p.Y - _dragAnchorPrev.Y;
                    _dragAnchorPrev = p;
                }
                RedrawEdges();
                break;
            }
            case DragMode.ScopeMove when _editScope is not null:
            {
                var p = e.GetPosition(_world);
                double dx = p.X - _scopeDragStart.X, dy = p.Y - _scopeDragStart.Y;
                _editScope.X = _scopeDragOriginal.X + dx;
                _editScope.Y = _scopeDragOriginal.Y + dy;
                RedrawScopes();
                break;
            }
            case DragMode.ScopeResize when _editScope is not null:
            {
                var p = e.GetPosition(_world);
                double dx = p.X - _scopeDragStart.X, dy = p.Y - _scopeDragStart.Y;
                var o = _scopeDragOriginal;
                double x = o.X, y = o.Y, w = o.W, h = o.H;
                const double min = 60;
                switch (_scopeHandle)
                {
                    case ScopeHandle.L: x = Math.Min(o.X + dx, o.X + o.W - min); w = o.X + o.W - x; break;
                    case ScopeHandle.R: w = Math.Max(min, o.W + dx); break;
                    case ScopeHandle.T: y = Math.Min(o.Y + dy, o.Y + o.H - min); h = o.Y + o.H - y; break;
                    case ScopeHandle.B: h = Math.Max(min, o.H + dy); break;
                    case ScopeHandle.TL: x = Math.Min(o.X + dx, o.X + o.W - min); w = o.X + o.W - x; y = Math.Min(o.Y + dy, o.Y + o.H - min); h = o.Y + o.H - y; break;
                    case ScopeHandle.TR: w = Math.Max(min, o.W + dx); y = Math.Min(o.Y + dy, o.Y + o.H - min); h = o.Y + o.H - y; break;
                    case ScopeHandle.BL: x = Math.Min(o.X + dx, o.X + o.W - min); w = o.X + o.W - x; h = Math.Max(min, o.H + dy); break;
                    case ScopeHandle.BR: w = Math.Max(min, o.W + dx); h = Math.Max(min, o.H + dy); break;
                }
                _editScope.X = x; _editScope.Y = y; _editScope.W = w; _editScope.H = h;
                RedrawScopes();
                break;
            }
            case DragMode.ScopeSelect:
            {
                var w = e.GetPosition(_world);
                double x = Math.Min(_scopeStart.X, w.X), y = Math.Min(_scopeStart.Y, w.Y);
                double width = Math.Abs(w.X - _scopeStart.X), height = Math.Abs(w.Y - _scopeStart.Y);
                _tempLayer.Children.Clear();
                _rubberBand = new Rectangle
                {
                    Stroke = new SolidColorBrush(Color.FromRgb(0x7F, 0x77, 0xDD)),
                    StrokeThickness = 1.5,
                    StrokeDashArray = new AvaloniaList<double> { 5, 3 },
                    Fill = new SolidColorBrush(Color.FromArgb(0x18, 0x7F, 0x77, 0xDD)),
                    IsHitTestVisible = false,
                };
                Canvas.SetLeft(_rubberBand, x);
                Canvas.SetTop(_rubberBand, y);
                _rubberBand.Width = width;
                _rubberBand.Height = height;
                _tempLayer.Children.Add(_rubberBand);
                break;
            }
        }
    }

    private void OnPointerReleased(object? sender, PointerEventArgs e)
    {
        if (_mode == DragMode.ScopeSelect)
        {
            var w = e.GetPosition(_world);
            double x = Math.Min(_scopeStart.X, w.X), y = Math.Min(_scopeStart.Y, w.Y);
            double width = Math.Abs(w.X - _scopeStart.X), height = Math.Abs(w.Y - _scopeStart.Y);
            _tempLayer.Children.Clear();
            _rubberBand = null;
            if (width > 20 && height > 20)
                ApplyScopeRect(new ScopeRect { X = x, Y = y, W = width, H = height });
        }
        else if (_mode == DragMode.Connect && _pending is not null)
        {
            var w = e.GetPosition(_world);
            // 只找输入口：拖动起点恒为连线输出侧（点输入口拖动时也已换算成其上游输出）。
            // 允许同节点自连接（模糊输出→模糊输入 = 迭代自反馈，成环检测会自动标 Feedback）。
            var hit = HitPort(w, inputsOnly: true);
            // 类型相配才可落；例外：Control 起线（flow.for/while 的「计数」作用域标记）可落任意输入口
            if (hit is { } target && (target.port.Type == _pending.Value.Type || _pending.Value.Type == PortType.Control))
            {
                bool selfLoop = target.node.Id == _pending.Value.Node;
                _graph.Connect(_pending.Value.Node, _pending.Value.Port, target.node.Id, target.port.Name);
                if (selfLoop)
                    _log("已建立自反馈：本节点输出回灌自己的输入（迭代处理，逐轮累积）");
                RedrawEdges();          // 立即显示新连线（否则要等下一次节点拖动才重绘）
                ScheduleAutoRun();
            }
            RemoveTempEdge();
        }
        else if (_mode == DragMode.EdgeBend)
        {
            _bendEdge = null;
            _dragAnchor = null;
            if (!_readOnly) SaveScopeConfig();   // 锚点随 pipeline.json 持久化
        }
        else if (_mode is DragMode.ScopeMove or DragMode.ScopeResize)
        {
            if (!_readOnly) SaveScopeConfig();   // Scope 随 pipeline.json 持久化
            RedrawScopes();   // 退出编辑态重画（无手柄）
        }
            _mode = DragMode.None;
            _dragNode = null;
            _pending = null;
            // 拖动时指针被捕获到根元素；不释放会让后续按下（含双击改名）不再路由到卡片头部
            e.Pointer.Capture(null);
        }

    // ==================== 节点卡片 ====================

    /// <summary>
    /// 按算子参数数量/标签内容推算合理的卡片初始宽度：
    /// 参数越多 → 卡片越宽（参数行的编辑器更舒展）；无/少参数 → 收紧不浪费。
    /// 有输出预览（draw 类）保证足够宽展示图像。仅在算子首次放置（node.Width 未拉伸）时生效，
    /// 用户手动拉伸过的宽度优先保留。
    /// </summary>
    private static double AutoCardWidth(NodeDef def)
    {
        // 数量维度：参数每多一个给编辑器+标签多一点余量（封顶 8 个，避免超宽）
        double byCount = 176 + Math.Min(def.Params.Length, 8) * 14;   // 176 .. 288

        // 内容维度：最长标签（参数名/端口名）按 11px 字估算，保证不被裁、编辑器至少 96px
        double maxLabel = 44;
        foreach (var p in def.Params)
            maxLabel = Math.Max(maxLabel, EstimateLabelPx(p.Label) + 12);
        foreach (var port in def.Inputs)
            maxLabel = Math.Max(maxLabel, EstimateLabelPx(port.Name) + 12);
        foreach (var port in def.Outputs)
            maxLabel = Math.Max(maxLabel, EstimateLabelPx(port.Name) + 12);
        double byContent = 20 + maxLabel + 96;   // 左右 padding + 标签列 + 编辑器列

        double w = Math.Max(byCount, byContent);
        if (def.HasPreview) w = Math.Max(w, 250);   // 预览算子留足图像宽度
        return Math.Clamp(w, 176, 320);
    }

    /// <summary>11px 字体的文本估算宽度：CJK 全角≈11px，ASCII≈6px。</summary>
    private static double EstimateLabelPx(string? text)
    {
        if (string.IsNullOrEmpty(text)) return 0;
        double px = 0;
        foreach (var c in text) px += c > 0x2E80 ? 11.0 : 6.0;
        return px;
    }

    private NodeInstance AddNode(string defId, double x, double y)
    {
        var def = NodeDefs.Map[defId];
        var node = new NodeInstance
        {
            DefId = def.Id, X = Math.Max(8, x), Y = Math.Max(8, y),
            Width = AutoCardWidth(def),   // 按参数数量自动定宽（用户后续可拉伸覆盖）
        };
        foreach (var p in def.Params)
            node.Params[p.Name] = p.Default;
        _graph.AddNode(node);
        BuildCard(node, def);
        RedrawEdges();
        ScheduleAutoRun();
        _log($"添加节点「{def.Name}」");
        return node;
    }

    /// <summary>以落点为中心添加节点（按该算子的实际宽度居中，避免固定 CardW 造成偏移）。</summary>
    private NodeInstance? AddNodeAtCenter(string defId, double centerX, double centerY)
    {
        if (!NodeDefs.Map.TryGetValue(defId, out var def)) return null;
        return AddNode(defId, centerX - AutoCardWidth(def) / 2, centerY - 30);
    }

    /// <summary>draw 卡片内嵌预览：按 nodeId 路由到本页卡片（执行线程触发，封送 UI）。</summary>
    private void OnNodePreview(string nodeId, object payload)
    {
        Dispatcher.UIThread.Post(() =>
        {
            if (!_previewSlots.TryGetValue(nodeId, out var slot)) return;   // 非本页卡片
            switch (payload)
            {
                case ImagePayload img:
                    slot.LastImage = img;
                    if (slot.Img is { } image)
                    {
                        var wb = new WriteableBitmap(new PixelSize(img.Width, img.Height), new Vector(96, 96), PixelFormats.Bgra8888, AlphaFormat.Opaque);
                        using (var fb = wb.Lock())
                            Marshal.Copy(img.PixelsBgra, 0, fb.Address, Math.Min(img.PixelsBgra.Length, img.Width * img.Height * 4));
                        image.Source = wb;
                        image.IsVisible = true;
                        slot.Box.IsVisible = true;
                    }
                    if (_previewExportButtons.TryGetValue(nodeId, out var btn2d)) btn2d.IsEnabled = true;
                    break;

                case CloudPayload cloud:
                    slot.LastCloud = cloud;
                    if (slot.Cloud is null && Sdk.Ui.PreviewSurfaces.CloudPreview is { } factory)
                    {
                        slot.Cloud = factory(new CloudPayload { Id = "预览", Points = cloud.Points, ColorsRgb = cloud.ColorsRgb, Count = cloud.Count });
                        slot.Box.Child = slot.Cloud.Control;
                        slot.Box.IsVisible = true;
                    }
                    slot.Cloud?.SetCloud(cloud);
                    if (_previewExportButtons.TryGetValue(nodeId, out var btn3d)) btn3d.IsEnabled = true;
                    break;
            }
        });
    }

    /// <summary>draw 卡片「→ 工作台」：把最近预览的输出发布到工作台主题（快速导入）。</summary>
    private void ExportPreview(string nodeId, bool isCloud)
    {
        if (!_previewSlots.TryGetValue(nodeId, out var slot)) return;
        var name = ResolveExportName(nodeId);
        if (isCloud)
        {
            if (slot.LastCloud is not { } cloud) return;
            var exported = new CloudPayload { Id = name, Points = cloud.Points, ColorsRgb = cloud.ColorsRgb, Count = cloud.Count, Source = "流水线预览" };
            _bus.Publish(BusTopics.CloudMerged, exported);
            _log($"已导入 3D 工作台：{name}（{exported.Count:N0} 点）");
        }
        else
        {
            if (slot.LastImage is not { } img) return;
            var exported = new ImagePayload { Id = name, Width = img.Width, Height = img.Height, PixelsBgra = img.PixelsBgra, Source = img.Source };
            _bus.Publish(BusTopics.ImageStitched, exported);
            _log($"已导入 2D 工作台：{name}（{img.Width}×{img.Height}）");
        }
    }

    /// <summary>导入数据名：卡片变量名 &gt; 输入连线源节点名 &gt; 卡片标题。</summary>
    private string ResolveExportName(string nodeId)
    {
        var node = _graph.FindNode(nodeId);
        if (node is not null
            && node.Params.TryGetValue("__varName", out var v)
            && v is string s && !string.IsNullOrWhiteSpace(s))
            return s.Trim();
        var conn = node is null ? null : _graph.FindInputConnection(nodeId, "输入");
        if (conn is not null)
        {
            var src = _graph.FindNode(conn.FromNode);
            if (src is not null)
                return src.Title ?? NodeDefs.Map[src.DefId].Name;
        }
        if (node is null) return "预览";
        return node.Title ?? NodeDefs.Map[node.DefId].Name;
    }

    private void BuildCard(NodeInstance node, NodeDef def)
    {
        var dot = new Ellipse { Width = 8, Height = 8, Fill = IdleBrush, VerticalAlignment = VerticalAlignment.Center };
        _dots[node.Id] = dot;
        _cards[node.Id] = BuildCardVisual(node, def, dot);
        _nodesLayer.Children.Add(_cards[node.Id]);
        Canvas.SetLeft(_cards[node.Id], node.X);
        Canvas.SetTop(_cards[node.Id], node.Y);
    }

    private Border BuildCardVisual(NodeInstance node, NodeDef def, Ellipse dot)
    {
        var headerBrush = def.Category switch
        {
            "源" => new SolidColorBrush(Color.FromRgb(0x2F, 0x6B, 0x3A)),
            "输出" => new SolidColorBrush(Color.FromRgb(0x8A, 0x5A, 0x1F)),
            _ => new SolidColorBrush(Color.FromRgb(0x15, 0x5A, 0x8A)),
        };

        var header = new Border
        {
            Background = headerBrush,
            Height = HeaderH,
            CornerRadius = new CornerRadius(8, 8, 0, 0),
            Padding = new Thickness(10, 0, 8, 0),
            Cursor = new Cursor(StandardCursorType.SizeAll),
        };
        var headerGrid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        var titleBlock = new TextBlock
        {
            Text = node.Title ?? def.Name,
            FontSize = 12,
            FontWeight = FontWeight.SemiBold,
            Foreground = Brushes.White,
            VerticalAlignment = VerticalAlignment.Center,
            TextTrimming = TextTrimming.CharacterEllipsis,
            Margin = new Thickness(0, 0, 6, 0),
        };
        headerGrid.Children.Add(titleBlock);
        dot.HorizontalAlignment = HorizontalAlignment.Right;
        dot.SetValue(Grid.ColumnProperty, 1);
        headerGrid.Children.Add(dot);
        header.Child = headerGrid;

        // 头部拖动节点 / 右键删除 / 双击改名
        var dbl = new DoubleClickDetector();
        header.PointerPressed += (_, e) =>
        {
            if (_readOnly)
            {
                _log("算法已锁定（只读），解锁或复制副本后才能拖动/编辑");
                return;   // 锁定算法只读：不拖动/不删除/不改名
            }
            var props = e.GetCurrentPoint(header).Properties;
            if (props.IsRightButtonPressed)
            {
                DeleteNode(node.Id);
                e.Handled = true;
                return;
            }
            if (!props.IsLeftButtonPressed) return;
            // 450ms 内两次左键 = 行内改名（Sdk.Ui.DoubleClickDetector，每卡独立实例避免跨卡误判）
            if (dbl.IsDouble())
            {
                BeginNodeRename(node, headerGrid, titleBlock);
                e.Handled = true;
                return;
            }
            _mode = DragMode.Node;
            _dragNode = node.Id;
            _lastScreen = e.GetPosition(this);
            e.Pointer.Capture(this);
            e.Handled = true;
            RaiseCard(node.Id);
            // 头部按压=拖动会标记 Handled，卡片级处理器的聚焦条件（!e.Handled）因此失败——
            // 键盘焦点必须在这里补上，否则点标题栏后 Ctrl+C/V/Del 全部不可达（复制失效的根因）
            SelectCard(node.Id);
            if (_cards.TryGetValue(node.Id, out var selfCard)) selfCard.Focus();
        };

        var body = new StackPanel { Margin = new Thickness(BodyPad, BodyPad, BodyPad, BodyPad) };

        if (def.HasPreview)
        {
            // 变量名（导入工作台时数据树里的数据名）：空 = 跟随输入源节点名
            var nameBox = new TextBox
            {
                Watermark = "跟随输入", FontSize = 11, Height = 26,
                Padding = new Thickness(6, 1, 6, 1), HorizontalContentAlignment = HorizontalAlignment.Left,
            };
            if (node.Params.TryGetValue("__varName", out var vn) && vn is string vnS && vnS.Length > 0)
                nameBox.Text = vnS;
            else
                nameBox.Text = ResolveExportName(node.Id);   // 默认名直接显示（跟随输入源），不只给灰水印
            if (_readOnly) nameBox.IsEnabled = false;   // 锁定算法只读
            void CommitName()
            {
                var t = nameBox.Text?.Trim() ?? "";
                if (t.Length == 0) node.Params.Remove("__varName");
                else node.Params["__varName"] = t;
            }
            nameBox.KeyDown += (_, ke) => { if (ke.Key == Key.Enter) { CommitName(); nameBox.Focus(); ke.Handled = true; } };
            nameBox.LostFocus += (_, _) => CommitName();
            var nameLabel = new TextBlock
            {
                Text = "名字", FontSize = 11,
                Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
                VerticalAlignment = VerticalAlignment.Center,
                MinWidth = 44, Margin = new Thickness(0, 0, 6, 0),   // CJK 度量偏小（BUG-050）
            };
            Grid.SetColumn(nameLabel, 0);
            Grid.SetColumn(nameBox, 1);
            var nameRow = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), Margin = new Thickness(0, 0, 0, 4) };
            nameRow.Children.Add(nameLabel);
            nameRow.Children.Add(nameBox);

            // 内嵌输出预览（draw 卡片）：2D=缩略图；3D=迷你交互视图（Sdk 预览契约）
            var previewBorder = new Border
            {
                Background = new SolidColorBrush(Color.FromRgb(0x14, 0x14, 0x19)),
                CornerRadius = new CornerRadius(3),
                IsVisible = false,
            };
            var slot = new PreviewSlot { Box = previewBorder };
            if (def.Id == "draw.3d" && Sdk.Ui.PreviewSurfaces.CloudPreview is { } factory)
            {
                // 3D 迷你视图占位：首帧数据到达时创建（避免无输入时空转）
                slot.Cloud = null;
                previewBorder.Child = new TextBlock
                {
                    Text = "等待点云输入…", FontSize = 11, Foreground = Brushes.DimGray,
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center,
                };
                // 3D 预览无图像纵横比可循：高 = 宽 × CloudPreviewRatio——宽随卡片拉伸时预览区
                // 对角放大（与 2D 图像"高=宽/纵横比"行为对齐）；GL 视口随控件 SizeChanged 自适应
                previewBorder.SizeChanged += (_, e) =>
                {
                    if (e.NewSize.Width > 0)
                        previewBorder.Height = e.NewSize.Width * CloudPreviewRatio;
                };
            }
            else
            {
                // 2D 预览不设固定高：Stretch=Uniform 在定宽约束下自动"宽=卡片宽、高=宽/纵横比"
                // ——拉伸卡片宽度即放大预览（ComfyUI 式），卡片高度随内容自适应
                var previewImage = new Image { Stretch = Stretch.Uniform, IsVisible = false };
                previewBorder.Child = previewImage;
                slot.Img = previewImage;
            }

            // 「→ 工作台」快速导入按钮（收到输出后可用）
            var exportBtn = new Button
            {
                Content = def.Id == "draw.3d" ? "→ 3D 工作台" : "→ 2D 工作台",
                Classes = { "ghost" },
                FontSize = 11,
                IsEnabled = false,
                HorizontalAlignment = HorizontalAlignment.Stretch,
                Margin = new Thickness(0, 4, 0, 0),
            };
            var exportKey = node.Id;
            var exportIsCloud = def.Id == "draw.3d";
            exportBtn.Click += (_, _) => ExportPreview(exportKey, exportIsCloud);
            body.Children.Add(nameRow);
            body.Children.Add(previewBorder);
            body.Children.Add(exportBtn);
            _previewSlots[node.Id] = slot;
            _previewExportButtons[node.Id] = exportBtn;
            _previewNameBoxes[node.Id] = nameBox;
        }

        foreach (var port in def.Inputs)
        {
            body.Children.Add(PortRow(node, port, isInput: true));
        }
        foreach (var port in def.Outputs)
        {
            body.Children.Add(PortRow(node, port, isInput: false));
        }

        foreach (var p in def.Params)
        {
            var editor = BuildParamEditor(node, p);
            var prow = new Grid
            {
                Height = ParamH,
                ColumnDefinitions = new ColumnDefinitions("Auto,*"),
                Children =
                {
                    new TextBlock
                    {
                        Text = p.Label, FontSize = 11,
                        Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
                        VerticalAlignment = VerticalAlignment.Center,
                        // 中文字符度量宽度偏小（"主题"实测 19px、渲染需 22px）会裁掉末字右侧，
                        // 保留 4 汉字宽度 + 6px 间距，编辑框也随列对齐
                        MinWidth = 44,
                        Margin = new Thickness(0, 0, 6, 0),
                    },
                    editor,
                },
            };
            if (!string.IsNullOrWhiteSpace(p.Description))
                ToolTip.SetTip(prow, p.Description);   // 卡片空间紧：说明走悬浮提示
            body.Children.Add(prow);
            var ed = (Control)editor;
            ed.SetValue(Grid.ColumnProperty, 1);
            ed.Margin = new Thickness(8, 4, 0, 4);
            if (_readOnly) ed.IsEnabled = false;   // 锁定算法只读：参数不可改
        }

        var card = new Border
        {
            Width = node.Width ?? AutoCardWidth(def),   // 角把手拉伸；未拉伸时按参数数量自动定宽（高度恒为内容自适应）
            Background = new SolidColorBrush(Color.FromRgb(0x26, 0x26, 0x29)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x3F, 0x3F, 0x44)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(6),
            ClipToBounds = false,
            Focusable = true,   // 选中后获得键盘焦点（Del 删除可达）
        };
        var content = new StackPanel { Children = { header, body } };
        var overlay = new Grid();
        overlay.Children.Add(content);

        // 右下角拉伸手柄（ComfyUI 式）：拖拽改卡片宽度，高度随内容自适应
        var grip = new Border
        {
            Width = 14, Height = 14,
            Background = Brushes.Transparent,   // 透明可命中
            Cursor = new Cursor(StandardCursorType.SizeWestEast),
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Bottom,
            Child = new TextBlock
            {
                Text = "◢", FontSize = 9,
                Foreground = new SolidColorBrush(Color.FromRgb(0x6E, 0x6E, 0x76)),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            },
        };
        ToolTip.SetTip(grip, "拖动调整卡片宽度（预览随宽放大，高度自适应）");
        grip.PointerPressed += (_, e) =>
        {
            if (_readOnly) return;   // 锁定算法只读：不可拉伸
            if (!e.GetCurrentPoint(grip).Properties.IsLeftButtonPressed) return;
            _mode = DragMode.Resize;
            _dragNode = node.Id;
            _resizeStartW = card.Width;
            _lastScreen = e.GetPosition(this);
            e.Pointer.Capture(this);
            e.Handled = true;
            SelectCard(node.Id);
        };
        overlay.Children.Add(grip);
        card.Child = overlay;
        // 任意位置按下=选中（handledEventsToo：头部把 Pressed 标记 Handled 用于拖动/改名，这里也要收到）
        card.AddHandler(PointerPressedEvent, (_, e) =>
        {
            SelectCard(node.Id);
            // 只在"无控件处理过该按压"时才把键盘焦点交给卡片（Del 删除可达）。
            // 不能只判 e.Source 类型：TextBox/Button 是模板控件，e.Source 是其模板内元素，
            // 误抢焦点会打断输入框光标和按钮点击（导入失效的根因）。
            if (!e.Handled && e.Source is not TextBox && e.Source is not Button) card.Focus();
        }, RoutingStrategies.Bubble, true);
        return card;
    }

    /// <summary>双击节点头部 → 行内改名（Sdk.Ui.InlineRename 统一组件）；空或等于算子名 = 还原为算子名。</summary>
    private void BeginNodeRename(NodeInstance node, Grid headerGrid, TextBlock titleBlock)
    {
        var def = NodeDefs.Map[node.DefId];
        TextBox box = null!;
        void Restore()
        {
            var idx = headerGrid.Children.IndexOf(box);
            if (idx >= 0) headerGrid.Children.RemoveAt(idx);
            if (!headerGrid.Children.Contains(titleBlock))
            {
                headerGrid.Children.Insert(0, titleBlock);
                titleBlock.SetValue(Grid.ColumnProperty, 0);
            }
        }
        box = InlineRename.Begin(node.Title ?? def.Name,
            t =>
            {
                node.Title = string.IsNullOrEmpty(t) || t == def.Name ? null : t;
                titleBlock.Text = node.Title ?? def.Name;
                Restore();
            },
            Restore);   // Esc = 取消，还原原名
        box.VerticalAlignment = VerticalAlignment.Center;
        headerGrid.Children.Remove(titleBlock);
        headerGrid.Children.Insert(0, box);
        box.SetValue(Grid.ColumnProperty, 0);
    }

    /// <summary>自绘紧凑滑杆：轨道+填充+圆钮全自绘。模板 Slider 的圆钮在紧凑行里会被
    /// 相邻行盖掉下半（多次修复无效），自绘后布局完全可控。
    /// 返回 (控件, 外部设值)；changed 仅在用户交互（点击/拖拽）时回调。</summary>

    private Control PortRow(NodeInstance node, PortDef port, bool isInput)
    {
        var ellipse = new Ellipse
        {
            Width = PortD,
            Height = PortD,
            Fill = PortBrush(port.Type),
            Stroke = new SolidColorBrush(Color.FromRgb(0x14, 0x14, 0x19)),
            StrokeThickness = 2,
            VerticalAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        _portDots[(node.Id, port.Name, isInput)] = ellipse;   // 连线锚点=实际布局位置（draw 卡片预览区会推低端口行）

        ellipse.PointerPressed += (_, e) =>
        {
            if (_readOnly) return;   // 锁定算法只读：不允许连线/拆线
            if (!e.GetCurrentPoint(ellipse).Properties.IsLeftButtonPressed) return;
            e.Handled = true;
            var w = e.GetPosition(_world);

            if (isInput)
            {
                // 已连线 → 断开并从上游输出重新拖出
                var conn = _graph.FindInputConnection(node.Id, port.Name);
                if (conn is not null)
                {
                    _pending = (conn.FromNode, conn.FromPort, port.Type);
                    _graph.DisconnectInput(node.Id, port.Name);
                    RedrawEdges();
                }
                else
                {
                    _pending = (node.Id, port.Name, port.Type);
                }
            }
            else
            {
                // 输出端口：直接拖出新线（一对多扇出；输入端一对一，接到已占用输入=换源）
                _pending = (node.Id, port.Name, port.Type);
            }

            if (_pending is not null)
            {
                _mode = DragMode.Connect;
                e.Pointer.Capture(this);
                StartTempEdge(e.GetPosition(_world));
            }
        };

        var text = new TextBlock
        {
            Text = port.Name,
            FontSize = 11.5,
            VerticalAlignment = VerticalAlignment.Center,
        };

        var row = new Grid { Height = RowH };
        if (isInput)
        {
            // 圆点完整置于卡片内（外伸部分会被裁成半圆），连线端点公式同步内收
            ellipse.HorizontalAlignment = HorizontalAlignment.Left;
            ellipse.Margin = new Thickness(0);
            text.Margin = new Thickness(16, 0, 0, 0);
            text.HorizontalAlignment = HorizontalAlignment.Left;
            row.Children.Add(ellipse);
            row.Children.Add(text);
        }
        else
        {
            ellipse.HorizontalAlignment = HorizontalAlignment.Right;
            ellipse.Margin = new Thickness(0);
            text.Margin = new Thickness(0, 0, 16, 0);
            text.HorizontalAlignment = HorizontalAlignment.Right;
            row.Children.Add(text);
            row.Children.Add(ellipse);
        }
        return row;
    }

    private Control BuildParamEditor(NodeInstance node, ComUI.Sdk.AlgoParam p, bool registerRefresher = true)
    {
        object? current = node.Params.TryGetValue(p.Name, out var v) ? v : p.Default;

        // 编辑值写回节点参数 + 刷新卡片显示（右侧属性面板与卡片双向同步的桥）
        void Write(object? val)
        {
            node.Params[p.Name] = val;
            ScheduleAutoRun();
            if (registerRefresher && _paramRefreshers.TryGetValue((node.Id, p.Name), out var refresh))
                refresh(val?.ToString() ?? "");
        }

        Control editor;
        switch (p.Type)
        {
            case ComUI.Sdk.AlgoParamType.Enum when p.Options is { Length: > 0 } opts:
            {
                var combo = new ComboBox
                {
                    ItemsSource = opts,
                    SelectedItem = current?.ToString(),
                    MinHeight = 26,
                    FontSize = 11.5,
                };
                combo.SelectionChanged += (_, _) => Write(combo.SelectedItem?.ToString() ?? "");
                editor = combo;
                break;
            }
            case ComUI.Sdk.AlgoParamType.Boolean:
            {
                var cb = new CheckBox
                {
                    IsChecked = current is bool b && b,
                    HorizontalAlignment = HorizontalAlignment.Left,
                    MinHeight = 22,
                };
                cb.IsCheckedChanged += (_, _) => Write(cb.IsChecked == true);
                editor = cb;
                break;
            }
            case ComUI.Sdk.AlgoParamType.Int:
            {
                int curV = Convert.ToInt32(current ?? 0);
                var min = p.Min is { } imin ? (int)Math.Round(imin) : int.MinValue;
                var max = p.Max is { } imax2 ? (int)Math.Round(imax2) : int.MaxValue;
                var box = MakeNumBox(FormatNum(curV));
                void Commit()
                {
                    if (double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var nv))
                    {
                        nv = Math.Clamp(Math.Round(nv), min, max);
                        if (Math.Abs(nv - curV) > 1e-9) { curV = (int)nv; node.Params[p.Name] = curV; box.Text = FormatNum(curV); ScheduleAutoRun(); }
                        else box.Text = FormatNum(curV);
                    }
                    else box.Text = FormatNum(curV);
                }
                box.KeyDown += (_, e) => { if (e.Key == Key.Enter) { Commit(); e.Handled = true; } };
                box.LostFocus += (_, _) => Commit();
                editor = box;
                break;
            }
            case ComUI.Sdk.AlgoParamType.Number:
            {
                double curV = Convert.ToDouble(current ?? 0.0);
                double min = p.Min ?? double.MinValue, max = p.Max ?? double.MaxValue;
                var box = MakeNumBox(FormatNum(curV));
                void Commit()
                {
                    if (double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var nv))
                    {
                        nv = Math.Clamp(nv, min, max);
                        if (Math.Abs(nv - curV) > 1e-9) { curV = nv; node.Params[p.Name] = nv; box.Text = FormatNum(curV); ScheduleAutoRun(); }
                        else box.Text = FormatNum(curV);
                    }
                    else box.Text = FormatNum(curV);
                }
                box.KeyDown += (_, e) => { if (e.Key == Key.Enter) { Commit(); e.Handled = true; } };
                box.LostFocus += (_, _) => Commit();
                editor = box;
                break;
            }
            default:
            {
                var tb = new TextBox { Text = current?.ToString() ?? "", MinHeight = 26, FontSize = 11.5, Padding = new Thickness(6, 2, 6, 2) };
                tb.TextChanged += (_, _) => Write(tb.Text);
                editor = tb;
                break;
            }
        }

        // 卡片显示刷新回调：右侧属性面板编辑时同步卡片数值（面板编辑为权威，卡片为显示）
        if (registerRefresher)
        {
            _paramRefreshers[(node.Id, p.Name)] = display =>
            {
                switch (editor)
                {
                    case TextBox tb: tb.Text = display; break;
                    case ComboBox cb when cb.Items.Cast<object>().Any(o => o?.ToString() == display):
                        cb.SelectedItem = display; break;
                    case CheckBox cb2: cb2.IsChecked = display == "True" || display == "true"; break;
                }
            };
        }
        return editor;
    }

    /// <summary>参数输入框（统一输入模式：无滑杆；带浅底色的可编辑域样式）。</summary>
    private static TextBox MakeNumBox(string text)
    {
        return new TextBox
        {
            Text = text,
            FontSize = 11.5,
            MinHeight = 26,
            Padding = new Thickness(6, 2, 6, 2),
            HorizontalContentAlignment = HorizontalAlignment.Right,
        };
    }


    private static string FormatNum(double v) => Math.Abs(v - Math.Round(v)) < 0.005
        ? Math.Round(v).ToString("F0")
        : v.ToString("F2");

    /// <summary>把卡片提到最上层，避免节点重叠时内容被压住。</summary>
    private void RaiseCard(string nodeId)
    {
        if (!_cards.TryGetValue(nodeId, out var card)) return;
        _nodesLayer.Children.Remove(card);
        _nodesLayer.Children.Add(card);
    }

    /// <summary>在当前视口中心添加一个节点（算子库单击时调用）。返回新节点实例（算子库参数覆盖应用用）。</summary>
    public NodeInstance? AddNodeCentered(string defId)
    {
        var c = ViewportCenter();
        return AddNodeAtCenter(defId, c.X, c.Y);
    }

    /// <summary>「⬆ 转为算子」：把当前流水线标记为算子并保存——宿主启动/重载时注册进算子库
    /// 「子流程」栏目（输入端口=图内需外部数据的节点端口，输出端口=末端节点输出），同步写参数配置文件。</summary>
    private async Task ToOperatorAsync()
    {
        if (_readOnly) { _log("算法已锁定（只读），解锁或复制副本后才能转为算子"); return; }
        if (_graph.Nodes.Count == 0) { _log("当前页面是空的，没有可转为算子的内容"); return; }

        // 选择归属栏目（可输入新栏目名）——用户取消则不转
        var groups = GroupSource?.Invoke() ?? new[] { "子流程" };
        var group = await GroupPickerDialog.ShowAsync(
            TopLevel.GetTopLevel(this) as Window, groups,
            string.IsNullOrWhiteSpace(_graph.OperatorGroup) ? "子流程" : _graph.OperatorGroup);
        if (string.IsNullOrWhiteSpace(group)) { _log("已取消转为算子"); return; }

        _graph.IsOperator = true;
        _graph.OperatorGroup = group!;
        await SaveOrCopyAsync();
        SetIcon(_toOperatorBtn, IconIsOp, $"已是算子（{group}）");
        _log($"已转为算子「{group}」栏目（即时出现在算子库）");
    }

    /// <summary>算子拖拽放置：按屏幕坐标落点创建节点（仅当落点在画布内）。返回新节点实例。</summary>
    public NodeInstance? PlaceNodeAtScreen(string defId, PixelPoint screen)
    {
        if (_editor.GetVisualRoot() is null) { _log("请先切换到流水线页面再放置节点"); return null; }
        var client = _editor.PointToClient(screen);
        if (!_editor.Bounds.Contains(client))
        {
            _log("请在流水线画布区域内放置节点");
            return null;
        }
        var world = _world.PointToClient(screen);
        return AddNodeAtCenter(defId, world.X, world.Y);
    }

    /// <summary>已保存算法拖拽放置：在落点展开整段编排。</summary>
    public void PlacePipelineAtScreen(string path, PixelPoint screen)
    {
        if (_editor.GetVisualRoot() is null) { _log("请先切换到流水线页面再展开算法"); return; }
        var client = _editor.PointToClient(screen);
        if (!_editor.Bounds.Contains(client))
        {
            _log("请在流水线画布区域内展开算法");
            return;
        }
        var world = _world.PointToClient(screen);
        _graph.PasteFromJson(File.ReadAllText(path), world);
        RebuildAllCards();
        RedrawEdges();
        ScheduleAutoRun();
        _log($"已展开算法: {IOPath.GetFileNameWithoutExtension(path)}");
    }

    private void SelectCard(string? nodeId)
    {
        if (_selectedNode == nodeId) return;
        if (_selectedNode is { } prev && _cards.TryGetValue(prev, out var prevCard))
            prevCard.BorderBrush = new SolidColorBrush(Color.FromRgb(0x3F, 0x3F, 0x44));
        _selectedNode = nodeId;
        if (nodeId is { } id && _cards.TryGetValue(id, out var card))
            card.BorderBrush = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF));
        RefreshNodeProps();   // 兼容旧的 IPanelParamsProvider 路径（宿主持有本控件实例时原地刷新）
        PushNodeToProps();    // 统一通道：与算子库共用 PropsShow 总线，确保属性栏一定显示当前节点
    }

    /// <summary>
    /// 画布节点选中 → 经总线把节点信息/参数控件推给属性栏（与左侧算子库同一条 PropsShow 通道）。
    /// 之前画布走 IPanelParamsProvider.CreateParamsContent 旧路径，会被算子库的对象模式 SetParams 挤掉，
    /// 导致"点了画布节点右边没反应"。统一走总线后两条路径不再打架。
    /// 参数控件每次新建（Avalonia 控件不能重复挂载），删除按钮回写节点删除。
    /// </summary>
    private void PushNodeToProps()
    {
        if (_selectedNode is null || _graph.FindNode(_selectedNode) is not { } node)
        {
            // 取消选中：清空属性栏，避免残留上一个节点的信息
            _bus.Publish(BusTopics.PropsShow, new PropsRequest
            {
                SectionTitle = "算子",
                Title = "未选中节点",
                Description = "单击画布算子卡片查看说明并调节参数",
                Rows = Array.Empty<(string, string)>(),
                ParamsContent = null,
            });
            return;
        }
        var def = NodeDefs.Map.TryGetValue(node.DefId, out var d) ? d : null;
        if (def is null) return;

        // 每次构建全新参数控件（不复用字段实例，避免已被挂载的控件二次 Add 抛异常）
        var paramsRoot = new StackPanel { Spacing = 6 };
        var editorHost = new StackPanel { Spacing = 8 };
        foreach (var p in def.Params)
        {
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(0, 3) };
            var labelHost = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            labelHost.Children.Add(new TextBlock
            {
                Text = p.Label, FontSize = 11, Foreground = DimText,
                TextWrapping = TextWrapping.Wrap,
            });
            if (!string.IsNullOrWhiteSpace(p.Description))
            {
                labelHost.Children.Add(new TextBlock
                {
                    Text = p.Description, FontSize = 10, Foreground = DimText,
                    TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 1, 0, 0),
                    Opacity = 0.75,
                });
                ToolTip.SetTip(row, p.Description);
            }
            row.Children.Add(labelHost);
            var editor = BuildParamEditor(node, p, registerRefresher: false);
            editor.SetValue(Grid.ColumnProperty, 1);
            row.Children.Add(editor);
            editorHost.Children.Add(row);
        }
        if (def.Params.Length == 0)
            editorHost.Children.Add(new TextBlock { Text = "本算子无参数", FontSize = 11, Foreground = DimText });
        paramsRoot.Children.Add(editorHost);

        if (!_readOnly)
        {
            var delBtn = new Button
            {
                Content = "🗑 删除节点（Del）", Classes = { "ghost" },
                FontSize = 11.5, HorizontalAlignment = HorizontalAlignment.Stretch,
            };
            delBtn.Click += (_, _) => DeleteNode(node.Id);
            paramsRoot.Children.Add(delBtn);
        }

        var vis = OperatorGlyphs.For(def);
        var rows = new List<(string, string)>
        {
            ("类型 Id", def.Id),
            ("分组", def.Group),
            ("子模块", vis.Sub),
            ("状态", node.Status.ToString()),
            ("输入端口", def.Inputs.Length == 0 ? "无" : string.Join("、", def.Inputs.Select(p => p.Name))),
            ("输出端口", def.Outputs.Length == 0 ? "无" : string.Join("、", def.Outputs.Select(p => p.Name))),
            ("参数个数", def.Params.Length.ToString()),
        };
        _bus.Publish(BusTopics.PropsShow, new PropsRequest
        {
            SectionTitle = "算子",
            Title = $"{vis.Icon}  {node.Title ?? def.Name}",
            Description = string.IsNullOrEmpty(def.Description) ? "（该算子未提供详细说明）" : def.Description,
            Rows = rows,
            ParamsContent = paramsRoot,
        });
    }

    // ==================== 右侧属性面板（IPanelParamsProvider） ====================

    private TextBlock? _nodePropsTitle;
    private TextBlock? _nodePropsDesc;
    private TextBlock? _nodePropsStatus;
    private StackPanel? _nodePropsEditorHost;
    private Button? _nodePropsDeleteBtn;

    /// <summary>右侧属性面板内容（自更新控件实例——选中卡片原地刷新，宿主零改动）：
    /// 选中卡片时显示名称/说明/状态/参数编辑器（编辑直接写节点参数并同步卡片显示）。</summary>
    public Control? CreateParamsContent()
    {
        var root = new StackPanel { Spacing = 6 };
        _nodePropsTitle = new TextBlock { FontSize = 12, FontWeight = FontWeight.SemiBold };
        _nodePropsDesc = new TextBlock { FontSize = 11, Foreground = DimText, TextWrapping = TextWrapping.Wrap };
        _nodePropsStatus = new TextBlock { FontSize = 11, TextWrapping = TextWrapping.Wrap, IsVisible = false };
        _nodePropsEditorHost = new StackPanel { Spacing = 6 };
        _nodePropsDeleteBtn = new Button
        {
            Content = "🗑 删除节点（Del）",
            Classes = { "ghost" },
            FontSize = 11,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        _nodePropsDeleteBtn.Click += (_, _) =>
        {
            if (_selectedNode is { } sel && !_readOnly) DeleteNode(sel);
        };
        root.Children.Add(_nodePropsTitle);
        root.Children.Add(_nodePropsDesc);
        root.Children.Add(_nodePropsStatus);
        root.Children.Add(_nodePropsEditorHost);
        root.Children.Add(_nodePropsDeleteBtn);
        RefreshNodeProps();
        return root;
    }

    /// <summary>选中卡片变化：刷新右侧面板（名称/说明/状态/参数编辑器——编辑直写节点参数并同步卡片）。</summary>
    private void RefreshNodeProps()
    {
        if (_nodePropsTitle is null || _nodePropsDesc is null || _nodePropsEditorHost is null ||
            _nodePropsDeleteBtn is null || _nodePropsStatus is null)
            return;
        _nodePropsEditorHost.Children.Clear();

        if (_selectedNode is null || !_graph.Nodes.Any(n => n.Id == _selectedNode))
        {
            _nodePropsTitle.Text = "未选中算子卡片";
            _nodePropsDesc.Text = "单击画布卡片查看说明并调节参数";
            _nodePropsStatus.IsVisible = false;
            _nodePropsDeleteBtn.IsVisible = false;
            return;
        }

        var node = _graph.FindNode(_selectedNode)!;
        var def = NodeDefs.Map[node.DefId];
        _nodePropsTitle.Text = node.Title ?? def.Name;
        _nodePropsDesc.Text = string.IsNullOrEmpty(def.Description) ? def.Id : def.Description;
        _nodePropsDeleteBtn.IsVisible = !_readOnly;

        // 状态行（错误红 / 运行黄 / 正常隐藏）
        if (node.Status == NodeStatus.Error && !string.IsNullOrEmpty(node.Error))
        {
            _nodePropsStatus.Text = "⚠ " + node.Error;
            _nodePropsStatus.Foreground = new SolidColorBrush(Color.FromRgb(0xF1, 0x4C, 0x4C));
            _nodePropsStatus.IsVisible = true;
        }
        else _nodePropsStatus.IsVisible = false;

        foreach (var p in def.Params)
        {
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("76,*") };
            var labelHost = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            labelHost.Children.Add(new TextBlock
            {
                Text = p.Label,
                FontSize = 11,
                Foreground = DimText,
                TextWrapping = TextWrapping.Wrap,
            });
            if (!string.IsNullOrWhiteSpace(p.Description))
            {
                labelHost.Children.Add(new TextBlock
                {
                    Text = p.Description, FontSize = 10, Foreground = DimText,
                    TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 1, 0, 0),
                    Opacity = 0.75,
                });
                ToolTip.SetTip(row, p.Description);
            }
            row.Children.Add(labelHost);
            // 面板编辑器不注册卡片刷新回调（面板是权威），写节点参数后反向刷新卡片显示
            var editor = BuildParamEditor(node, p, registerRefresher: false);
            editor.SetValue(Grid.ColumnProperty, 1);
            row.Children.Add(editor);
            _nodePropsEditorHost.Children.Add(row);
        }
        if (def.Params.Length == 0)
            _nodePropsEditorHost.Children.Add(new TextBlock { Text = "本算子无参数", FontSize = 11, Foreground = DimText });
    }

    /// <summary>选中/取消选中连线（高亮 + Del 删除目标切换；与节点选中互斥）。</summary>
    private void SelectEdge(Connection? edge)
    {
        if (ReferenceEquals(_selectedEdge, edge)) return;
        _selectedEdge = edge;
        if (edge is not null)
        {
            SelectCard(null);   // 选中连线 = 取消节点选中（互斥，Del 目标唯一）
            _log("已选中连线（Del 删除）");
        }
        RedrawEdges();   // 高亮垫底线随重绘恢复
    }

    private void DeleteNode(string nodeId)
    {
        var node = _graph.FindNode(nodeId);
        if (node is null) return;
        if (_selectedNode == nodeId) _selectedNode = null;
        _log($"删除节点「{NodeDefs.Map[node.DefId].Name}」");
        _graph.RemoveNode(nodeId);
        foreach (var k in _portDots.Keys.Where(k => k.NodeId == nodeId).ToList()) _portDots.Remove(k);
        foreach (var k in _paramRefreshers.Keys.Where(k => k.NodeId == nodeId).ToList()) _paramRefreshers.Remove(k);
        if (_cards.Remove(nodeId, out var card)) _nodesLayer.Children.Remove(card);
        if (_dots.Remove(nodeId, out var dot)) _nodesLayer.Children.Remove(dot);
        RedrawEdges();
        RedrawScopes();   // 删除循环节点 → 去掉对应作用域框
        ScheduleAutoRun();
    }

    private void ClearAll()
    {
        _graph.Connections.Clear();
        foreach (var n in _graph.Nodes.ToList())
            _graph.RemoveNode(n.Id);
        _nodesLayer.Children.Clear();
        _cards.Clear();
        _dots.Clear();
        _portDots.Clear();
        RedrawEdges();
        RedrawScopes();
    }

    // ==================== 连线 ====================

    private Point OutPortCenter(NodeInstance n, string port)
    {
        if (_portDots.TryGetValue((n.Id, port, false), out var odot))
        {
            if (odot.IsArrangeValid)
            {
                var oc = odot.TranslatePoint(new Point(PortD / 2.0, PortD / 2.0), _world);
                if (oc is { } occ) return occ;   // 实际布局位置优先（draw 卡片预览区会改变端口行位置）
            }
            _edgeLayoutDirty = true;   // 已登记但未布局/未挂树：布局完成后 LayoutUpdated 补画
        }
        var def = NodeDefs.Map[n.DefId];
        int idx = def.Outputs.ToList().FindIndex(p => p.Name == port);
        int inCount = def.Inputs.Length;
        return new Point(n.X + (n.Width ?? CardW) - 15, n.Y + HeaderH + BodyPad + (inCount + idx + 0.5) * RowH);
    }

    private Point InPortCenter(NodeInstance n, string port)
    {
        if (_portDots.TryGetValue((n.Id, port, true), out var idot))
        {
            if (idot.IsArrangeValid)
            {
                var ic = idot.TranslatePoint(new Point(PortD / 2.0, PortD / 2.0), _world);
                if (ic is { } icc) return icc;
            }
            _edgeLayoutDirty = true;
        }
        var def = NodeDefs.Map[n.DefId];
        int idx = def.Inputs.ToList().FindIndex(p => p.Name == port);
        return new Point(n.X + 15, n.Y + HeaderH + BodyPad + (idx + 0.5) * RowH);
    }

    private void RedrawEdges()
    {
        // 整层重建：清掉所有连线相关子项（Path 可视线/halo/命中层 + Ellipse 锚点圆点），
        // 只保留临时预览线（不在本层）。此前只删 Path 导致锚点圆点残留越积越多。
        for (int i = _edges.Children.Count - 1; i >= 0; i--)
            if (!ReferenceEquals(_edges.Children[i], _tempEdge))
                _edges.Children.RemoveAt(i);

        foreach (var c in _graph.Connections)
        {
            var from = _graph.FindNode(c.FromNode);
            var to = _graph.FindNode(c.ToNode);
            if (from is null || to is null) continue;
            var p0 = OutPortCenter(from, c.FromPort);
            var p1 = InPortCenter(to, c.ToPort);
            var type = NodeDefs.Map[from.DefId].Outputs.First(o => o.Name == c.FromPort).Type;
            bool selected = ReferenceEquals(c, _selectedEdge);

            // 选中高亮：白色宽线垫底（先画，位于可视线下方）
            if (selected)
            {
                var halo = EdgePath(p0, p1, new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xEC)), 7, c.Bend, c.Bend2, c.Anchors);
                halo.IsHitTestVisible = false;
                _edges.Children.Add(halo);
            }

            // 可视线：反馈边=琥珀色虚线（回灌上一轮结果）；普通边=类型色；选中时略加粗
            var path = EdgePath(p0, p1, c.Feedback
                ? new SolidColorBrush(Color.FromRgb(0xEF, 0x9F, 0x27))
                : PortBrush(type), selected ? 3 : 2.2, c.Bend, c.Bend2, c.Anchors);
            if (c.Feedback) path.StrokeDashArray = new AvaloniaList<double> { 6, 4 };
            path.IsHitTestVisible = false;
            _edges.Children.Add(path);

            // 选中时画出锚点手柄（纯显示；点击由下方命中层接管，按位置判断是否是已有锚点）
            if (selected)
                foreach (var an in c.Anchors)
                {
                    var dot = new Ellipse
                    {
                        Width = 9, Height = 9,
                        Fill = new SolidColorBrush(Color.FromRgb(0xEF, 0x9F, 0x27)),
                        Stroke = new SolidColorBrush(Color.FromRgb(0x23, 0x23, 0x26)),
                        StrokeThickness = 1.5,
                        IsHitTestVisible = false,
                    };
                    Canvas.SetLeft(dot, an.X - 4.5);
                    Canvas.SetTop(dot, an.Y - 4.5);
                    _edges.Children.Add(dot);
                }

            // 透明粗命中层：14px 扩大点击区，盖在最上。
            // 第一次点击=选中点亮；已点亮时再次点击=直接拖拽改变曲线走向（端点不变）。
            var hit = EdgePath(p0, p1, Brushes.Transparent, 14, c.Bend, c.Bend2, c.Anchors);
            hit.Tag = c;
            hit.Cursor = new Cursor(selected ? StandardCursorType.SizeAll : StandardCursorType.Hand);
            ToolTip.SetTip(hit, selected
                ? "再次单击拖锚点（局部变形）· Backspace 重置为直线 · Del 删除 · 点空白取消"
                : "单击选中连线；选中后再次单击可在按下处插入锚点并拖动");
            hit.PointerPressed += (_, e) =>
            {
                if (!e.GetCurrentPoint(hit).Properties.IsLeftButtonPressed) return;
                e.Handled = true;
                _editor.Focus();   // 确保键盘焦点在本控件树内，Del 才能删除选中连线
                bool wasSelected = ReferenceEquals(_selectedEdge, c);
                SelectEdge(c);     // 已选中时内部去重直接 return，不会重绘打断本次拖拽
                if (wasSelected)
                {
                    // 已点亮 → 再次点击即可拖拽（端点不变）。
                    // 局部变形：命中已有锚点就拖它；否则在按下处的曲线上插入一个新锚点再拖。
                    _bendEdge = c;
                    var press = e.GetPosition(_world);
                    const double hitR = 14;   // 锚点命中半径（世界坐标）
                    const int maxAnchors = 3; // 每条连线锚点上限，防止点满屏小圆点
                    var existing = c.Anchors.FirstOrDefault(a =>
                        Math.Abs(a.X - press.X) < hitR && Math.Abs(a.Y - press.Y) < hitR);
                    if (existing is null)
                    {
                        if (c.Anchors.Count >= maxAnchors)
                        {
                            // 已达上限：不插入，改拖离按下处最近的锚点
                            existing = c.Anchors.OrderBy(a => Distance(new Point(a.X, a.Y), press)).First();
                            _log($"锚点已达上限（{maxAnchors} 个），改为拖动最近锚点；Backspace 可清空重设");
                        }
                        else
                        {
                            existing = new AnchorPoint();
                            // 插在曲线上离按下点最近的位置（按锚点顺序）
                            var at = CurvePointAt(p0, p1, c.Bend, c.Bend2, c.Anchors, press);
                            existing.X = at.X; existing.Y = at.Y;
                            int ins = 0;
                            while (ins < c.Anchors.Count && c.Anchors[ins].X < existing.X) ins++;
                            c.Anchors.Insert(ins, existing);
                            if (c.Anchors.Count == 1) { c.Bend = 0; c.Bend2 = 0; }   // 清掉旧单段模式残留偏移，锚点几何自足
                        }
                    }
                    _dragAnchor = existing;
                    _dragAnchorPrev = press;
                    _mode = DragMode.EdgeBend;
                    e.Pointer.Capture(this);
                    RedrawEdges();
                    _log("锚点拖拽中：只有按住的这一段跟手，端点不变；Del 删除连线，双击空白处取消选中");
                }
            };
            _edges.Children.Add(hit);
        }
        _edges.InvalidateVisual();
        RefreshPreviewNames();   // 连接变化 → 未自定义的变量名跟随新输入源
    }

    /// <summary>draw 卡片变量名框刷新：未自定义（Params 无 __varName）时显示当前解析结果（输入源节点名）。</summary>
    private void RefreshPreviewNames()
    {
        foreach (var kv in _previewNameBoxes)
        {
            if (kv.Value.IsFocused) continue;   // 正在输入不打断
            var node = _graph.FindNode(kv.Key);
            var custom = node is not null
                && node.Params.TryGetValue("__varName", out var v)
                && v is string vs && vs.Length > 0 ? vs : null;
            var want = custom ?? ResolveExportName(kv.Key);
            if (kv.Value.Text != want) kv.Value.Text = want;
        }
    }

    /// <summary>贝塞尔两控制点的水平外伸距离（随两端水平距离自适应；回连时加大避开卡片）。</summary>
    private static double EdgeDx(Point p0, Point p1)
    {
        double dx = Math.Clamp(Math.Abs(p1.X - p0.X) * 0.55, 30, 160);
        if (p1.X < p0.X) dx = Math.Clamp(dx + 90, 120, 260);
        return dx;
    }

    /// <summary>求曲线上离鼠标最近的世界坐标点（与 EdgeFigure 同构：无锚点单段 / 有锚点多段）。
    /// 插入新锚点时用它定位锚点初始位置，保证插下去的瞬间线不变形。</summary>
    private static Point CurvePointAt(Point p0, Point p1, double bend, double bend2,
        IReadOnlyList<AnchorPoint>? anchors, Point mouse)
    {
        Point best = p0;
        double bestD = double.MaxValue;
        void Consider(double x, double y)
        {
            double d = (x - mouse.X) * (x - mouse.X) + (y - mouse.Y) * (y - mouse.Y);
            if (d < bestD) { bestD = d; best = new Point(x, y); }
        }
        void SampleBezier(Point a, Point c1, Point c2, Point b, int steps)
        {
            for (int i = 0; i <= steps; i++)
            {
                double t = (double)i / steps, u = 1 - t;
                double x = u * u * u * a.X + 3 * u * u * t * c1.X + 3 * u * t * t * c2.X + t * t * t * b.X;
                double y = u * u * u * a.Y + 3 * u * u * t * c1.Y + 3 * u * t * t * c2.Y + t * t * t * b.Y;
                Consider(x, y);
            }
        }

        if (anchors is null || anchors.Count == 0)
        {
            double dx = EdgeDx(p0, p1);
            SampleBezier(p0, new Point(p0.X + dx, p0.Y + bend),
                new Point(p1.X - dx, p1.Y + bend2), p1, 80);
        }
        else
        {
            // 与 EdgeFigure 锚点模式同构：水平切线多段采样（bend/bend2 在锚点模式下不参与）
            var pts = new List<Point> { p0 };
            foreach (var an in anchors) pts.Add(new Point(an.X, an.Y));
            pts.Add(p1);
            for (int i = 0; i < pts.Count - 1; i++)
            {
                var A = pts[i];
                var B = pts[i + 1];
                double dx = EdgeDx(A, B);
                SampleBezier(A, new Point(A.X + dx, A.Y), new Point(B.X - dx, B.Y), B, 40);
            }
        }
        return best;
    }

    /// <summary>构造连线几何：无锚点=单段贝塞尔（bend/bend2）；有锚点=水平切线多段贝塞尔
    /// 平滑穿过 p0→锚点…→p1（锚点局部可拖，端点不变）。返回 PathFigure 供可视层/命中层共用。</summary>
    private static PathFigure EdgeFigure(Point p0, Point p1, double bend = 0, double bend2 = 0,
        IReadOnlyList<AnchorPoint>? anchors = null)
    {
        var fig = new PathFigure { StartPoint = p0, IsClosed = false };
        var seg = new PolyBezierSegment();

        if (anchors is null || anchors.Count == 0)
        {
            double dx = EdgeDx(p0, p1);
            seg.Points.Add(new Point(p0.X + dx, p0.Y + bend));
            seg.Points.Add(new Point(p1.X - dx, p1.Y + bend2));
            seg.Points.Add(p1);
        }
        else
        {
            // 水平切线模式（节点编辑器标准走线）：每段控制点沿水平方向外伸。
            // 不用 Catmull-Rom——锚点稀少时切线估计 (pAfter-pPrev)/6 跨度过大导致过冲突起。
            // 水平切线：曲线平缓、端口水平出入，锚点两侧同为水平切线 → 平滑无鼓包。
            var pts = new List<Point> { p0 };
            foreach (var a in anchors) pts.Add(new Point(a.X, a.Y));
            pts.Add(p1);
            for (int i = 0; i < pts.Count - 1; i++)
            {
                var A = pts[i];
                var B = pts[i + 1];
                double dx = EdgeDx(A, B);
                seg.Points.Add(new Point(A.X + dx, A.Y));   // 出控制点（水平外伸）
                seg.Points.Add(new Point(B.X - dx, B.Y));   // 入控制点（水平外伸）
                seg.Points.Add(B);
            }
        }
        fig.Segments.Add(seg);
        return fig;
    }

    private static Path EdgePath(Point p0, Point p1, IBrush brush, double thickness = 2.2,
        double bend = 0, double bend2 = 0, IReadOnlyList<AnchorPoint>? anchors = null)
    {
        return new Path
        {
            Stroke = brush,
            StrokeThickness = thickness,
            Data = new PathGeometry { Figures = { EdgeFigure(p0, p1, bend, bend2, anchors) } },
        };
    }

    private (NodeInstance node, PortDef port)? HitPort(Point world, bool inputsOnly = false)
    {
        (NodeInstance, PortDef)? best = null;
        double bestDist = 16;
        foreach (var n in _graph.Nodes)
        {
            var def = NodeDefs.Map[n.DefId];
            for (int i = 0; i < def.Inputs.Length; i++)
            {
                double d = Distance(world, InPortCenter(n, def.Inputs[i].Name));
                if (d < bestDist) { bestDist = d; best = (n, def.Inputs[i]); }
            }
            if (inputsOnly) continue;   // 目标必须是输入口（避免自连接时误命中近旁的输出口）
            for (int i = 0; i < def.Outputs.Length; i++)
            {
                double d = Distance(world, OutPortCenter(n, def.Outputs[i].Name));
                if (d < bestDist) { bestDist = d; best = (n, def.Outputs[i]); }
            }
        }
        return best;
    }

    private static double Distance(Point a, Point b)
    {
        double dx = a.X - b.X, dy = a.Y - b.Y;
        return Math.Sqrt(dx * dx + dy * dy);
    }

    private void StartTempEdge(Point world) => UpdateTempEdge(world);

    /// <summary>重建预览线：整层清空后只放一条贝塞尔曲线（杜绝任何残留/重复）。</summary>
    private void UpdateTempEdge(Point world)
    {
        if (_pending is null) return;
        _tempLayer.Children.Clear();
        var p0 = OutPosOf(_pending.Value);
        double dx = Math.Clamp(Math.Abs(world.X - p0.X) * 0.55, 30, 160);
        if (world.X < p0.X) dx = Math.Clamp(dx + 90, 120, 260);

        var geo = new StreamGeometry();
        using (var g = geo.Open())
        {
            g.BeginFigure(p0, false);
            g.CubicBezierTo(new Point(p0.X + dx, p0.Y), new Point(world.X - dx, world.Y), world);
            g.EndFigure(false);
        }
        _tempEdge = new Path { Stroke = Brushes.White, StrokeThickness = 2.2, Data = geo };
        _tempLayer.Children.Add(_tempEdge);
    }

    private Point OutPosOf((string Node, string Port, PortType Type) pending)
    {
        var n = _graph.FindNode(pending.Node)!;
        var def = NodeDefs.Map[n.DefId];
        // 从输入端口拆线拖出时，起点是上游的输出端口
        return def.Outputs.Any(o => o.Name == pending.Port)
            ? OutPortCenter(n, pending.Port)
            : InPortCenter(n, pending.Port);
    }

    private void RemoveTempEdge()
    {
        _tempLayer.Children.Clear();   // 清空整个预览层，杜绝残留直线
        if (_tempEdge is null) return;
        _edges.Children.Remove(_tempEdge);
        _tempEdge = null;
    }

    // ==================== 循环作用域框选 ====================

    /// <summary>右击框选完成：把矩形赋给框内循环节点，并重绘作用域可视化。</summary>
    private void ApplyScopeRect(ScopeRect rect)
    {
        // 框内找循环算子——判定规则与执行器一致：卡片左上角落入矩形（GraphExecutor r.Contains(n.X, n.Y)）
        var loopsInBox = _graph.Nodes.Where(n =>
            n.DefId is "flow.for" or "flow.while" &&
            rect.Contains(n.X, n.Y)).ToList();

        if (loopsInBox.Count == 0)
        {
            _log("框内没有循环算子（For/While）——请把循环算子也框进来");
            return;
        }

        // 每个框内循环节点都应用该矩形（多循环的并集执行器会处理）
        foreach (var n in loopsInBox) n.Scope = rect;
        RedrawScopes();
        int count = _graph.Nodes.Count(n => rect.Contains(n.X, n.Y));
        _log($"已设置循环作用域：{count} 个节点在框内（每轮迭代执行），框外节点只执行一次；右键框选可重设");
        if (!_readOnly) SaveScopeConfig();
    }

    /// <summary>重绘所有循环节点的作用域虚线框（世界坐标）。</summary>
    private void RedrawScopes()
    {
        _scopeLayer.Children.Clear();
        foreach (var n in _graph.Nodes)
        {
            if (n.Scope is not { } r) continue;
            bool editing = ReferenceEquals(r, _editScope);
            // 视觉框：纯显示，不参与命中——框内空白点击应穿透到画布（平移/取消选中），只有边框线可点
            var box = new Rectangle
            {
                Stroke = new SolidColorBrush(editing
                    ? Color.FromRgb(0xEF, 0x9F, 0x27) : Color.FromRgb(0x7F, 0x77, 0xDD)),
                StrokeThickness = editing ? 2 : 1.5,
                StrokeDashArray = new AvaloniaList<double> { 6, 4 },
                Fill = new SolidColorBrush(Color.FromArgb(0x0E, 0x7F, 0x77, 0xDD)),
                IsHitTestVisible = false,
            };
            Canvas.SetLeft(box, r.X);
            Canvas.SetTop(box, r.Y);
            box.Width = r.W;
            box.Height = r.H;
            _scopeLayer.Children.Add(box);

            // 四条边框命中条带（透明、10px 宽）：点击边框线=选中并可整体拖动
            AddScopeBorderHits(r, n, editing);

            if (editing) AddScopeHandles(r, n.Id);
        }
    }

    /// <summary>四条边框命中条带：只有点在边框线上才能选中/拖动作用域框。
    /// 框内空白不设命中，点击会穿透到画布（平移画布 / 取消选中），避免误触。</summary>
    private void AddScopeBorderHits(ScopeRect r, NodeInstance owner, bool editing)
    {
        const double band = 10;   // 命中带宽（世界坐标），沿边框线中心 ±5
        var bands = new (double X, double Y, double W, double H)[]
        {
            (r.X - band / 2, r.Y - band / 2, r.W + band, band),              // 上
            (r.X - band / 2, r.Y + r.H - band / 2, r.W + band, band),        // 下
            (r.X - band / 2, r.Y + band / 2, band, Math.Max(0, r.H - band)), // 左
            (r.X + r.W - band / 2, r.Y + band / 2, band, Math.Max(0, r.H - band)), // 右
        };
        foreach (var (bx, by, bw, bh) in bands)
        {
            if (bw <= 0 || bh <= 0) continue;
            var b = new Border
            {
                Background = Brushes.Transparent,   // 透明但可命中
                Cursor = new Cursor(editing ? StandardCursorType.SizeAll : StandardCursorType.Hand),
            };
            Canvas.SetLeft(b, bx);
            Canvas.SetTop(b, by);
            b.Width = bw;
            b.Height = bh;
            b.PointerPressed += (_, e) =>
            {
                if (!e.GetCurrentPoint(b).Properties.IsLeftButtonPressed) return;
                e.Handled = true;   // 阻止冒泡到画布（否则会立刻取消选中）
                _editScope = r;
                _editScopeNodeId = owner.Id;
                _scopeHandle = ScopeHandle.Move;
                _scopeDragStart = e.GetPosition(_world);
                _scopeDragOriginal = new ScopeRect { X = r.X, Y = r.Y, W = r.W, H = r.H };
                _mode = DragMode.ScopeMove;
                e.Pointer.Capture(this);
                RedrawScopes();   // 重画显示手柄 + 琥珀色选中态
                if (!editing) _log("已选中作用域框：拖边框整体移动、拖四角/四边缩放、Del 清除；点框内空白或画布空白退出");
            };
            _scopeLayer.Children.Add(b);
        }
    }

    /// <summary>编辑态作用域框的 8 个手柄（4角4边），拖动缩放矩形。</summary>
    private void AddScopeHandles(ScopeRect r, string nodeId)
    {
        var handles = new (double X, double Y, ScopeHandle H, StandardCursorType Cursor)[]
        {
            (r.X, r.Y, ScopeHandle.TL, StandardCursorType.TopLeftCorner),
            (r.X + r.W, r.Y, ScopeHandle.TR, StandardCursorType.TopRightCorner),
            (r.X, r.Y + r.H, ScopeHandle.BL, StandardCursorType.BottomLeftCorner),
            (r.X + r.W, r.Y + r.H, ScopeHandle.BR, StandardCursorType.BottomRightCorner),
            (r.X + r.W / 2, r.Y, ScopeHandle.T, StandardCursorType.SizeNorthSouth),
            (r.X + r.W / 2, r.Y + r.H, ScopeHandle.B, StandardCursorType.SizeNorthSouth),
            (r.X, r.Y + r.H / 2, ScopeHandle.L, StandardCursorType.SizeWestEast),
            (r.X + r.W, r.Y + r.H / 2, ScopeHandle.R, StandardCursorType.SizeWestEast),
        };
        foreach (var (hx, hy, h, cursor) in handles)
        {
            var handle = new Rectangle
            {
                Width = 10, Height = 10,
                Fill = new SolidColorBrush(Color.FromRgb(0xEF, 0x9F, 0x27)),
                Stroke = new SolidColorBrush(Color.FromRgb(0x23, 0x23, 0x26)),
                StrokeThickness = 1,
                Cursor = new Cursor(cursor),
            };
            Canvas.SetLeft(handle, hx - 5);
            Canvas.SetTop(handle, hy - 5);
            handle.PointerPressed += (_, e) =>
            {
                if (!e.GetCurrentPoint(handle).Properties.IsLeftButtonPressed) return;
                e.Handled = true;
                _editScope = r;
                _editScopeNodeId = nodeId;
                _scopeHandle = h;
                _scopeDragStart = e.GetPosition(_world);
                _scopeDragOriginal = new ScopeRect { X = r.X, Y = r.Y, W = r.W, H = r.H };
                _mode = DragMode.ScopeResize;
                e.Pointer.Capture(this);
            };
            _scopeLayer.Children.Add(handle);
        }
    }

    /// <summary>框选后自动持久化作用域（若已加载算法文件，直接写回；新页等用户保存）。</summary>
    private void SaveScopeConfig()
    {
        if (LoadedPath is null || !File.Exists(LoadedPath) || _readOnly) return;
        try
        {
            var name = IOPath.GetFileNameWithoutExtension(LoadedPath).Replace(".pipeline", "");
            File.WriteAllText(LoadedPath, _graph.Serialize(name));
        }
        catch { /* 作用域内存态已生效，写回失败不影响本次运行 */ }
    }

    private static IBrush PortBrush(PortType t) => t switch
    {
        PortType.Image => new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)),   // 图像=蓝
        PortType.Control => new SolidColorBrush(Color.FromRgb(0xE0, 0xA4, 0x51)), // 控制（循环计数/作用域标记）=琥珀
        _ => new SolidColorBrush(Color.FromRgb(0x81, 0xC7, 0x84)),                // 点云=绿
    };

    private static readonly IBrush IdleBrush = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));

    // ==================== 运行 ====================

    /// <summary>运行状态视觉反馈：运行中「⏸」图标+绿底、「■ 停止」可用；
    /// 空闲用 ClearValue 清局部值回落 ghost 样式（置 null 是局部值会压掉样式——按钮会整个不可见）、停止置灰禁用。</summary>
    private void SetRunState(bool running)
    {
        if (running)
        {
            _runBtn.Background = RunActiveBrush;
            SetIcon(_runBtn, IconRunning, "运行中（停止可中断）");
        }
        else
        {
            _runBtn.ClearValue(Button.BackgroundProperty);
            SetIcon(_runBtn, IconRun, "运行流水线");
        }
        _stopBtn.IsEnabled = running;
    }

    private void ScheduleAutoRun()
    {
        if (!_autoRun) return;
        _debounce.Stop();
        _debounce.Start();
    }

    private async void RunGraph()
    {
        if (_running) { _log("上一轮还在执行，已忽略本次触发"); return; }
        _running = true;
        SetRunState(true);
        _runCts = new CancellationTokenSource();

        foreach (var n in _graph.Nodes) SetStatus(n, NodeStatus.Idle, null);

        var started = Environment.TickCount64;
        try
        {
            await GraphExecutor.RunAsync(_graph, SetStatus, m => _log(m), _runCts.Token);
            _log($"流水线执行完成（{_graph.Nodes.Count} 节点 / {_graph.Connections.Count} 连线）");
        }
        catch (OperationCanceledException) { _log("流水线已取消"); }
        catch (Exception ex) { _log($"流水线执行失败: {ex.Message}"); }
        finally
        {
            // 运行态最短可见 600ms：瞬间完成的流水线绿态一闪而过，反馈等于没有
            // （只是视觉保持，结果早已发布；期间 _running=true，重复触发按"上一轮还在执行"忽略）
            var hold = MinRunVisibleMs - (Environment.TickCount64 - started);
            if (hold > 0) await Task.Delay((int)hold);
            _running = false;
            SetRunState(false);
        }
    }

    private void SetStatus(NodeInstance node, NodeStatus status, string? error)
    {
        node.Status = status;
        node.Error = error;
        Dispatcher.UIThread.Post(() =>
        {
            if (!_dots.TryGetValue(node.Id, out var dot)) return;
            dot.Fill = status switch
            {
                NodeStatus.Running => new SolidColorBrush(Color.FromRgb(0xE5, 0xC0, 0x7B)),
                NodeStatus.Done => new SolidColorBrush(Color.FromRgb(0x89, 0xD1, 0x85)),
                NodeStatus.Error => new SolidColorBrush(Color.FromRgb(0xF1, 0x4C, 0x4C)),
                _ => IdleBrush,
            };
            ToolTip.SetTip(dot, status switch
            {
                NodeStatus.Error => $"错误: {error}",
                NodeStatus.Done => "执行成功",
                NodeStatus.Running => "执行中…",
                _ => "待执行",
            });
        });
    }

    // ==================== 算法保存 / 展开 ====================

    private async Task SaveAlgorithmAsync()
    {
        if (_graph.Nodes.Count == 0) { _log("当前页面是空的，没有可保存的内容"); return; }

        var name = await NamePrompt.ShowAsync(TopLevel.GetTopLevel(this) as Window ?? new Window(),
            "保存算法", "算法将保存到左侧「算法」侧边栏，单击可展开复用。", "我的算法");
        if (string.IsNullOrWhiteSpace(name)) return;

        try
        {
            Directory.CreateDirectory(_algorithmsDir);
            var safe = string.Join("_", name.Split(IOPath.GetInvalidFileNameChars())).Trim();
            var path = IOPath.Combine(_algorithmsDir, safe + ".pipeline.json");
            await File.WriteAllTextAsync(path, _graph.Serialize(name));
            WriteParamsConfig(path);
            AlgorithmSaved?.Invoke(name, path);
            _log($"算法已保存: {name} → {path}" + (_graph.IsOperator ? "（已转为算子，含参数配置文件）" : ""));
        }
        catch (Exception ex)
        {
            _log($"保存失败: {ex.Message}");
        }
    }


    /// <summary>保存同步写参数配置文件（<算子>.params.json）：各节点参数快照 + 是否已转为算子。
    /// 键锚定节点 Id（<节点Id>.<参数名>，与子流程注册端同键——子图节点改名后参数覆盖不再失配）。
    /// 宿主启动/重载据此把已转算子的流水线注册进算子库「子流程」栏目。</summary>
    private void WriteParamsConfig(string pipelinePath)
    {
        var name = IOPath.GetFileNameWithoutExtension(pipelinePath).Replace(".pipeline", "");
        var cfgPath = IOPath.Combine(IOPath.GetDirectoryName(pipelinePath)!, name + ".params.json");
        var flat = new Dictionary<string, object?>();
        foreach (var n in _graph.Nodes)
            foreach (var kv in n.Params)
                flat[$"{n.Id}.{kv.Key}"] = kv.Value;
        var cfg = new Dictionary<string, object?>
        {
            ["isOperator"] = _graph.IsOperator,
            ["group"] = _graph.OperatorGroup,
            ["params"] = flat,
        };
        File.WriteAllText(cfgPath, System.Text.Json.JsonSerializer.Serialize(cfg,
            new System.Text.Json.JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
    }

    /// <summary>保存按钮 / Ctrl+S 的统一入口：锁定页 → 复制副本；已加载文件的页面 → 直接写回原文件；
    /// 新页面 → 弹另存对话框。</summary>
    private async Task SaveOrCopyAsync()
    {
        if (_readOnly) { await SaveCopyAsync(); return; }
        if (LoadedPath is not null && File.Exists(LoadedPath)) { SaveToLoadedPath(); return; }
        await SaveAlgorithmAsync();
    }

    /// <summary>直接写回当前加载的算法文件（Ctrl+S / 保存按钮）。</summary>
    private void SaveToLoadedPath()
    {
        try
        {
            var name = IOPath.GetFileNameWithoutExtension(LoadedPath!).Replace(".pipeline", "");
            File.WriteAllText(LoadedPath!, _graph.Serialize(name));
            WriteParamsConfig(LoadedPath!);
            _log($"算法已保存: {name} → {LoadedPath}" + (_graph.IsOperator ? "（含参数配置文件）" : ""));
            // 已加载文件（如 show1）转算子走这里——必须触发保存事件，否则算子库不会即时注册/刷新（BUG）。
            // 非算子仅普通保存不触发，避免重复污染算法列表。
            if (_graph.IsOperator)
                AlgorithmSaved?.Invoke(name, LoadedPath!);
        }
        catch (Exception ex)
        {
            _log($"保存失败: {ex.Message}");
        }
    }

    /// <summary>锁定算法的「复制副本」：以「原名 副本」保存一份解锁副本并发布 pipeline/open 打开可编辑页。</summary>
    private async Task SaveCopyAsync()
    {
        if (_graph.Nodes.Count == 0) { _log("当前页面是空的，没有可保存的内容"); return; }

        var baseName = LoadedPath is null
            ? "我的算法"
            : IOPath.GetFileNameWithoutExtension(LoadedPath).Replace(".pipeline", "");
        var name = baseName + " 副本";

        try
        {
            Directory.CreateDirectory(_algorithmsDir);
            var safe = string.Join("_", name.Split(IOPath.GetInvalidFileNameChars())).Trim();
            var path = IOPath.Combine(_algorithmsDir, safe + ".pipeline.json");
            var locked = _graph.Locked;
            _graph.Locked = false;   // 副本解锁
            await File.WriteAllTextAsync(path, _graph.Serialize(name));
            _graph.Locked = locked;
            WriteParamsConfig(path);
            AlgorithmSaved?.Invoke(name, path);
            _bus.Publish(BusTopics.PipelineOpen, path);   // 新标签页打开可编辑副本
            _log($"已复制副本: {name} → {path}（解锁可编辑）");
        }
        catch (Exception ex)
        {
            _log($"复制副本失败: {ex.Message}");
        }
    }

    /// <summary>「⬇ 导出 C++」：把当前图导出为完整可编译的 C++ 控制台工程
    /// （main.cpp + 算子真实源码副本 + CMakeLists；native 算子直接编译进可执行文件，
    /// 无源码的节点生成 TODO 桩）。选文件夹落盘。</summary>
    private async Task ExportCppAsync()
    {
        if (_graph.Nodes.Count == 0) { _log("当前页面是空的，没有可导出的内容"); return; }
        var storage = TopLevel.GetTopLevel(this)?.StorageProvider;
        if (storage is null) { _log("无法打开文件夹选择对话框"); return; }
        var folder = await storage.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "选择导出 C++ 工程的文件夹",
            AllowMultiple = false,
        });
        if (folder.Count == 0) return;
        var dir = folder[0].TryGetLocalPath();
        if (string.IsNullOrEmpty(dir)) { _log("所选文件夹不是本地路径"); return; }

        var name = LoadedPath is not null
            ? IOPath.GetFileNameWithoutExtension(LoadedPath).Replace(".pipeline", "")
            : "pipeline";
        try
        {
            var err = CppExporter.Export(_graph, dir, name);
            if (err is not null) { _log($"导出失败：{err}"); return; }
            _log($"已导出 C++ 工程 → {dir}（main.cpp + 算子源码 + CMakeLists.txt，CMake 直接构建）");
        }
        catch (Exception ex)
        {
            _log($"导出失败: {ex.Message}");
        }
    }

    /// <summary>从算法文件完整加载（新页面打开场景）：替换当前图，保留连线与参数。</summary>
    public void LoadGraphFile(string path)    {
        _graph.LoadFromJson(File.ReadAllText(path));
        LoadedPath = path;
        SetReadOnly(_graph.Locked);   // 锁定的算法以只读打开
        if (!_readOnly && _saveBtn is not null) SetIcon(_saveBtn, IconSave, "保存（写回原文件）");   // 文件语义：直接写回
        RebuildAllCards();
        RedrawEdges();
        RedrawScopes();   // 恢复持久化的循环作用域虚线框
        ScheduleAutoRun();
        _log($"已加载算法: {IOPath.GetFileNameWithoutExtension(path)}{(_graph.Locked ? "（已锁定·只读）" : "")}");
    }

    /// <summary>切换只读模式（锁定/解锁联动）：重建卡片以启用/禁用参数编辑，保存按钮切换形态。</summary>
    public void SetReadOnly(bool readOnly)
    {
        if (_readOnly == readOnly) return;
        _readOnly = readOnly;
        _graph.Locked = readOnly;
        if (_saveBtn is not null)
            SetIcon(_saveBtn, _readOnly ? IconCopy : IconSave, _readOnly ? "复制副本（算法已锁定）" : "保存算法");
        // 卡片已存在时重建（参数编辑器启用状态随 _readOnly 变化）
        if (_nodesLayer.Children.Count > 0)
        {
            RebuildAllCards();
            RedrawEdges();
        }
        _log(_readOnly ? "算法已锁定：只读模式（解锁或复制副本后可修改）" : "算法已解锁：可编辑");
    }

    /// <summary>从算法文件展开整段编排到当前画布（算子库拖拽/单击）。</summary>
    public void PasteGraphFile(string path, Point? center = null)
    {
        var c = center ?? ViewportCenter();
        _graph.PasteFromJson(File.ReadAllText(path), c);
        RebuildAllCards();
        RedrawEdges();
        ScheduleAutoRun();
        _log($"已展开算法: {IOPath.GetFileNameWithoutExtension(path)}");
    }

    /// <summary>重建全部节点卡片（结构批量变化后调用）。</summary>
    private void RebuildAllCards()
    {
        _nodesLayer.Children.Clear();
        _cards.Clear();
        _dots.Clear();
        foreach (var n in _graph.Nodes)
        {
            BuildCard(n, NodeDefs.Map[n.DefId]);
            Canvas.SetLeft(_cards[n.Id], n.X);
            Canvas.SetTop(_cards[n.Id], n.Y);
        }
    }

}
