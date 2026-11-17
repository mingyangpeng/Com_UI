using System;
using System.Runtime.InteropServices;
using System.Globalization;
using System.IO;
using Avalonia;
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

    private enum DragMode { None, Pan, Node, Connect, Resize }
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

    /// <summary>算法保存成功后触发（文件名, 完整路径）—— 算子库据此新增"算法"条目。</summary>
    public event Action<string, string>? AlgorithmSaved;

    /// <summary>设置算法保存目录（宿主 config/algorithms）。</summary>
    public void SetAlgorithmsDir(string dir) => _algorithmsDir = dir;
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

        _world.Children.Add(_edges);
        _edges.IsHitTestVisible = false;
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
                AddNode((string)e.Data.Get(DragFormat)!, p.X - CardW / 2, p.Y - 30);
                e.Handled = true;
            }
            else if (e.Data.Contains(PipelineFormat) && e.Data.Get(PipelineFormat) is string path && File.Exists(path))
            {
                PasteGraphFile(path, p);   // 拖入已保存的算法 = 展开整段编排
                e.Handled = true;
            }
        });
    }

    public void Dispose() { }

    // ==================== 工具栏 ====================

    private Control BuildToolbar()
    {
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };

        // ＋ 添加节点 / 示例图 已移除（用户定案）：建节点走算子库拖拽/单击；示例不再预置
        var runBtn = Ghost("▶ 运行");
        runBtn.Click += (_, _) => RunGraph();
        _runBtn = runBtn;
        bar.Children.Add(runBtn);

        var stopBtn = Ghost("■ 停止");
        stopBtn.Click += (_, _) =>
        {
            if (!_running || _runCts is null) { _log("当前没有正在执行的流水线"); return; }
            _runCts.Cancel();   // RunGraph 捕获取消并记日志"流水线已取消"
        };
        _stopBtn = stopBtn;
        bar.Children.Add(stopBtn);
        SetRunState(false);   // 初始：运行钮灰、停止钮置灰禁用

        var autoBtn = Ghost(_autoRun ? "自动运行：开" : "自动运行：关");
        autoBtn.Click += (_, _) =>
        {
            _autoRun = !_autoRun;
            autoBtn.Content = _autoRun ? "自动运行：开" : "自动运行：关";
            _log($"自动运行已{(_autoRun ? "开启" : "关闭")}");
        };
        bar.Children.Add(autoBtn);

        var saveBtn = Ghost("💾 保存算法");
        saveBtn.Click += (_, _) => _ = SaveOrCopyAsync();
        _saveBtn = saveBtn;
        bar.Children.Add(saveBtn);

        var clearBtn = Ghost("清空");
        clearBtn.Click += (_, _) =>
        {
            if (_readOnly) { _log("算法已锁定（只读），解锁或复制副本后才能修改"); return; }
            ClearAll();
        };
        bar.Children.Add(clearBtn);

        var exportCppBtn = Ghost("⬇ 导出 C++");
        exportCppBtn.Click += (_, _) => _ = ExportCppAsync();
        bar.Children.Add(exportCppBtn);

        var toOpBtn = Ghost("⬆ 转为算子");
        toOpBtn.Click += (_, _) => _ = ToOperatorAsync();
        _toOperatorBtn = toOpBtn;
        bar.Children.Add(toOpBtn);

        bar.Children.Add(new TextBlock
        {
            Text = "从算子库拖入建节点 · 单击算子行=属性面板（参数编辑后加入画布） · 拖动端口连线 · Del/右键删除 · 空白处拖动平移 · 滚轮缩放",
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

    // ==================== 坐标 / 视图 ====================

    private Point ViewportCenter()
    {
        var c = _editor.Bounds.Center;
        return new Point((c.X - _transT.X) / _scaleT.ScaleX, (c.Y - _transT.Y) / _scaleT.ScaleY);
    }

    private void EditorPressed(object? sender, PointerPressedEventArgs e)
    {
        SelectCard(null);
        SelectEdge(null);   // 点画布空白同时取消连线选中（内部去重，无选中时零开销）
        var props = e.GetCurrentPoint(_editor).Properties;
        // 仅空白画布（world/_editor 自身）触发平移；节点、滑块等内部 Border 不得抢捕获
        bool onBackground = ReferenceEquals(e.Source, _world) || ReferenceEquals(e.Source, _editor) || ReferenceEquals(e.Source, _edges);
        if (props.IsMiddleButtonPressed || (props.IsLeftButtonPressed && onBackground))
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
        }
    }

    private void OnPointerReleased(object? sender, PointerEventArgs e)
    {
        if (_mode == DragMode.Connect && _pending is not null)
        {
            var w = e.GetPosition(_world);
            var hit = HitPort(w);
            if (hit is { } target && target.port.Type == _pending.Value.Type &&
                target.node.Id != _pending.Value.Node)
            {
                _graph.Connect(_pending.Value.Node, _pending.Value.Port, target.node.Id, target.port.Name);
                RedrawEdges();          // 立即显示新连线（否则要等下一次节点拖动才重绘）
                ScheduleAutoRun();
            }
            RemoveTempEdge();
        }
            _mode = DragMode.None;
            _dragNode = null;
            _pending = null;
            // 拖动时指针被捕获到根元素；不释放会让后续按下（含双击改名）不再路由到卡片头部
            e.Pointer.Capture(null);
        }

    // ==================== 节点卡片 ====================

    private NodeInstance AddNode(string defId, double x, double y)
    {
        var def = NodeDefs.Map[defId];
        var node = new NodeInstance { DefId = def.Id, X = Math.Max(8, x), Y = Math.Max(8, y) };
        foreach (var p in def.Params)
            node.Params[p.Name] = p.Default;
        _graph.AddNode(node);
        BuildCard(node, def);
        RedrawEdges();
        ScheduleAutoRun();
        _log($"添加节点「{def.Name}」");
        return node;
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
            body.Children.Add(new Grid
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
            });
            var ed = (Control)editor;
            ed.SetValue(Grid.ColumnProperty, 1);
            ed.Margin = new Thickness(8, 4, 0, 4);
            if (_readOnly) ed.IsEnabled = false;   // 锁定算法只读：参数不可改
        }

        var card = new Border
        {
            Width = node.Width ?? CardW,   // 角把手拉伸（默认 CardW；高度恒为内容自适应）
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
        return AddNode(defId, c.X - CardW / 2, c.Y - 30);
    }

    /// <summary>「⬆ 转为算子」：把当前流水线标记为算子并保存——宿主启动/重载时注册进算子库
    /// 「子流程」栏目（输入端口=图内需外部数据的节点端口，输出端口=末端节点输出），同步写参数配置文件。</summary>
    private async Task ToOperatorAsync()
    {
        if (_readOnly) { _log("算法已锁定（只读），解锁或复制副本后才能转为算子"); return; }
        if (_graph.Nodes.Count == 0) { _log("当前页面是空的，没有可转为算子的内容"); return; }
        _graph.IsOperator = true;
        await SaveOrCopyAsync();
        _toOperatorBtn.Content = "⬆ 已是算子";
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
        return AddNode(defId, world.X - CardW / 2, world.Y - 30);
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
        RefreshNodeProps();   // 右侧属性面板：显示选中卡片的说明与参数（宿主持有本控件实例，原地刷新）
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
            row.Children.Add(new TextBlock
            {
                Text = p.Label,
                FontSize = 11,
                Foreground = DimText,
                VerticalAlignment = VerticalAlignment.Center,
            });
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
        for (int i = _edges.Children.Count - 1; i >= 0; i--)
            if (_edges.Children[i] is Path p && !ReferenceEquals(p, _tempEdge))
                _edges.Children.RemoveAt(i);

        foreach (var c in _graph.Connections)
        {
            var from = _graph.FindNode(c.FromNode);
            var to = _graph.FindNode(c.ToNode);
            if (from is null || to is null) continue;
            var p0 = OutPortCenter(from, c.FromPort);
            var p1 = InPortCenter(to, c.ToPort);
            var type = NodeDefs.Map[from.DefId].Outputs.First(o => o.Name == c.FromPort).Type;
            var path = EdgePath(p0, p1, PortBrush(type));
            // 命中与选中共用：宽透明描边扩大点击区，窄实线显示颜色（选中之上的高亮在 HitEdge 重绘）
            path.Tag = c;
            path.StrokeThickness = 2.2;
            path.Cursor = new Cursor(StandardCursorType.Hand);
            path.PointerPressed += (_, e) =>
            {
                if (!e.GetCurrentPoint(path).Properties.IsLeftButtonPressed) return;
                e.Handled = true;
                SelectEdge(c);
            };
            _edges.Children.Add(path);
            if (ReferenceEquals(c, _selectedEdge))
            {
                // 选中高亮：白色宽线垫底 + 原色线叠上（保持类型色可辨）
                var halo = EdgePath(p0, p1, new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xEC)));
                halo.StrokeThickness = 5.5;
                _edges.Children.Add(halo);
            }
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

    private static Path EdgePath(Point p0, Point p1, IBrush brush, double thickness = 2.2)
    {
        // 控制点距离随水平距离自适应；目标在源左侧（回连）时加大弯曲避开卡片
        double dx = Math.Clamp(Math.Abs(p1.X - p0.X) * 0.55, 30, 160);
        if (p1.X < p0.X) dx = Math.Clamp(dx + 90, 120, 260);
        return new Path
        {
            Stroke = brush,
            StrokeThickness = thickness,
            Data = new PathGeometry
            {
                Figures =
                {
                    new PathFigure
                    {
                        StartPoint = p0,
                        IsClosed = false,
                        Segments = { new PolyBezierSegment { Points = { new Point(p0.X + dx, p0.Y), new Point(p1.X - dx, p1.Y), p1 } } },
                    }
                },
            },
        };
    }

    private (NodeInstance node, PortDef port)? HitPort(Point world)
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

    private static IBrush PortBrush(PortType t) => t == PortType.Image
        ? new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7))
        : new SolidColorBrush(Color.FromRgb(0x81, 0xC7, 0x84));

    private static readonly IBrush IdleBrush = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));

    // ==================== 运行 ====================

    /// <summary>运行状态视觉反馈：运行中「▶ 运行中…」绿底（文字沿用样式前景，近白在绿底上清晰）、「■ 停止」可用；
    /// 空闲用 ClearValue 清局部值回落 ghost 样式（置 null 是局部值会压掉样式——按钮会整个不可见）、停止置灰禁用。</summary>
    private void SetRunState(bool running)
    {
        if (running)
        {
            _runBtn.Background = RunActiveBrush;
            _runBtn.Content = "▶ 运行中…";
        }
        else
        {
            _runBtn.ClearValue(Button.BackgroundProperty);
            _runBtn.Content = "▶ 运行";
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
    /// 宿主启动/重载据此把已转算子的流水线注册进算子库「子流程」栏目。</summary>
    private void WriteParamsConfig(string pipelinePath)
    {
        var name = IOPath.GetFileNameWithoutExtension(pipelinePath).Replace(".pipeline", "");
        var cfgPath = IOPath.Combine(IOPath.GetDirectoryName(pipelinePath)!, name + ".params.json");
        var flat = new Dictionary<string, object?>();
        foreach (var n in _graph.Nodes)
        {
            var label = n.Title ?? NodeDefs.Map[n.DefId].Name;
            foreach (var kv in n.Params)
                flat[$"{label}.{kv.Key}"] = kv.Value;
        }
        var cfg = new Dictionary<string, object?> { ["isOperator"] = _graph.IsOperator, ["params"] = flat };
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
        if (!_readOnly && _saveBtn is not null) _saveBtn.Content = "💾 保存";   // 文件语义：直接写回
        RebuildAllCards();
        RedrawEdges();
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
            _saveBtn.Content = _readOnly ? "📋 复制副本" : "💾 保存算法";
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
