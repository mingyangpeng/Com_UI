using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Collections;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using ComUI.Sdk;
using OpenTK.Mathematics;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 点云查看器（M5 多实体）：四元数翻滚相机（左键旋转/滚轮缩放/右键平移/Ctrl+左键拾取与框选）。
/// 按 Id 管理多个点云实体（同 Id 重发=更新），分块渐进上传、交互抽稀、XYZ 指示器；
/// 点云树侧边栏通过 CloudsChanged/GetCloudInfos/SetCloudVisible/RemoveCloud 驱动。
/// </summary>
public sealed class Cloud3DView : UserControl, IDisposable, IPanelParamsProvider
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    private const string SelfId = "ui.cloud3d.view";

    internal sealed class CloudData
    {
        public required string Id;
        public required float[] Pts;
        public byte[]? Col;
        public byte[] Sel = Array.Empty<byte>();
        public int Count;
        public int Loaded;
        public int SelCount;
        public Vector3 BoundsMin, BoundsMax;
        public string? Source;
        public int ColorMode = 1;
        public float PointSize = 3f;
        public Vector3 SingleColor = new(0.7f, 0.75f, 0.8f);
        public double[] Xform = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };   // 行主序 4x4（末列平移）
        public bool Locked;   // 锁定：不可删除/改名，切数据源保留（数据树锁徽标切换）
        public float[]? Normals;   // 逐点法线（SoA，渲染器持有显存副本；同 Id 重发后自动重传）
        public float? ColorMin, ColorMax;   // 伪彩色带域覆盖（null=包围盒；重发保留）
    }

    /// <summary>点云树展示用快照。</summary>
    public readonly record struct CloudInfo(string Id, int Count, int Loaded, bool Visible, int SelCount,
        Vector3 BoundsMin, Vector3 BoundsMax, string? Source, int ColorMode, float PointSize,
        Vector3 SingleColor, double[] Xform, bool Locked);

    private readonly GlCloudRenderer _renderer;
    private readonly IBus _bus;
    private readonly IPluginContext? _ctx;   // 快照开动态文档页用（embed/测试场景可空）
    private readonly Image _image = new() { Stretch = Stretch.None, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
    private readonly Border _viewArea;
    private readonly ComboBox _projCombo = new() { Classes = { "input" }, MinWidth = 96 };
    private readonly ComboBox _presetCombo = new() { Classes = { "input" }, MinWidth = 96 };
    private readonly TextBox _strideBox = new() { Classes = { "input" }, MinWidth = 56, Text = "1" };
    /// <summary>切片带（轴可选）：上/下限 TextBox 空文本=关闭切片。</summary>
    private readonly TextBox _clipLoBox = new() { Classes = { "input" }, MinWidth = 56 };
    private readonly TextBox _clipHiBox = new() { Classes = { "input" }, MinWidth = 56 };
    private readonly TextBox _rangeLoBox = new() { Classes = { "input" }, MinWidth = 56 };
    private readonly TextBox _rangeHiBox = new() { Classes = { "input" }, MinWidth = 56 };
    private readonly Avalonia.Controls.Slider _lightSlider = new()
    {
        Minimum = 0.2, Maximum = 1.0, Value = 1.0, TickFrequency = 0.05, MinWidth = 96,
    };
    private readonly ComboBox _clipAxisCombo = new() { Classes = { "input" }, MinWidth = 56 };   // 0=X 1=Y(高度) 2=Z
    /// <summary>远裁剪（截断）距离：0=自动（dist*100），正数=固定 far，放大时可避免远处点云被截掉。</summary>
    private float _farClip;
    private readonly TextBox _farClipBox = new() { Classes = { "input" }, MinWidth = 56, Text = "0" };
    private readonly TextBlock _cloudInfo = new() { FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap, FontFamily = new Avalonia.Media.FontFamily("Cascadia Mono, Consolas, monospace") };
    private readonly TextBlock _pickInfo = new() { FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap, Text = "无（Ctrl+左键单击拾取）", FontFamily = new Avalonia.Media.FontFamily("Cascadia Mono, Consolas, monospace") };
    private readonly TextBlock _selInfo = new() { FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap, Text = "无（Ctrl+左键拖拽框选）", FontFamily = new Avalonia.Media.FontFamily("Cascadia Mono, Consolas, monospace") };
    private readonly StackPanel _paramsContent;
    private Vector3 _defaultSingle = new(0.7f, 0.75f, 0.8f);   // 新点云单色默认（历史约定色）

    // 覆盖层：框选橡皮筋 + 拾取标记（不参与命中测试）
    private readonly Canvas _overlay = new() { IsHitTestVisible = false };
    private readonly Rectangle _rubber = new()
    {
        IsVisible = false, StrokeThickness = 1,
        Stroke = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)),
        StrokeDashArray = new AvaloniaList<double> { 4, 2 },
    };
    private readonly Ellipse _pickMark = new()
    {
        IsVisible = false, Width = 11, Height = 11, StrokeThickness = 1.5,
        Stroke = new SolidColorBrush(Color.FromRgb(0xFF, 0x26, 0xFF)),
    };

    // 右下角 XYZ 坐标轴指示器
    private readonly Canvas _tripod = new()
    {
        Width = 72, Height = 72, IsHitTestVisible = false,
        HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Bottom,
        Margin = new Thickness(0, 0, 10, 10),
    };
    private readonly Line _axX = AxisLine(0xE5, 0x39, 0x35);
    private readonly Line _axY = AxisLine(0x43, 0xA0, 0x47);
    private readonly Line _axZ = AxisLine(0x42, 0xA5, 0xF5);
    private readonly TextBlock _axXl = AxisLabel("X", 0xE5, 0x39, 0x35);
    private readonly TextBlock _axYl = AxisLabel("Y", 0x43, 0xA0, 0x47);
    private readonly TextBlock _axZl = AxisLabel("Z", 0x42, 0xA5, 0xF5);

    private IDisposable? _busSub;
    private IDisposable? _busSubRemove;
    private string _sourceTopic = CloudPayload.TopicMerged;   // 当前订阅的数据源主题（可切换）
    private readonly ComboBox _sourceCombo = new() { Classes = { "input" }, MinWidth = 150 };
    private bool _refreshingSource;                           // 下拉重填期间的选中噪声抑制
    private WriteableBitmap? _wb;
    private byte[] _frame = Array.Empty<byte>();
    private bool _hasColors;
    private double _scaling = 1;
    private int _vpW, _vpH;

    private Quaternion _orientation = Quaternion.Identity;
    private double _dist = 10;
    private Vector3 _target = Vector3.Zero;
    private float _fitRadius = 5;
    // 沿视线的推进量（穿行）：>0 相机持续朝 fwd 前进、可越过中心到背面；_dist 只作为初始适配基准不再随滚轮变。
    private double _travel;

    // 多实体点云（数组来自总线载荷，约定不可变，不克隆）
    private const long InteractCap = 5_000_000;       // 交互时渲染点数上限（定案约 500 万）
    private const int UploadChunkPoints = 2_000_000;  // 分块上传每块点数
    private readonly Dictionary<string, CloudData> _clouds = new();
    private string? _pickedCloud;
    private int _pickedIndex = -1;
    private int _stride = 1;                          // 用户抽稀间隔
    private bool _interacting;                        // 相机交互中（交互抽稀生效）
    private Avalonia.Threading.DispatcherTimer? _uploadTimer;
    private Avalonia.Threading.DispatcherTimer? _idleTimer;

    private Point? _orbitLast;
    private Point? _panLast;
    private Point? _pickStart;

    // ==================== 工具（CloudCompare 风格：提取/剔除/距离量测/算子调用） ====================
    private readonly CloudToolOps _tools = new();
    private bool _measureMode;                        // 距离量测模式：连续两次拾取 = 一条量测
    private (string Cloud, int Index)? _measureFirst; // 量测第一点（待第二点）
    private Button? _measureBtn;                      // 量测模式按钮（激活高亮）
    private bool _polyMode;                           // 多边形选取模式（左键加点/右键或 Enter 闭合）
    private readonly List<Point> _polyPts = new();    // 当前多边形顶点（视图 DIP 坐标）
    private Button? _polyBtn;                         // 多边形模式按钮（激活高亮）
    private sealed class Measurement
    {
        /// <summary>折线顶点（≥2）：两点=普通距离量测；右键/Enter 结束后新拾取开始下一条独立折线。</summary>
        public readonly List<(string Cloud, int Index)> Points = new();
    }
    private readonly List<Measurement> _measurements = new();
    private Measurement? _activeMeasure;              // 折线续拾取中的活动链（右键/Enter 结束）
    private readonly Canvas _measureLayer = new() { IsHitTestVisible = false };   // 量测线+距离标签（跟随相机重投影）
    private readonly TextBlock _measureInfo = new() { FontSize = 11.5, Foreground = Dim, TextWrapping = TextWrapping.Wrap, FontFamily = new Avalonia.Media.FontFamily("Cascadia Mono, Consolas, monospace") };
    private readonly TextBlock _pickHistoryInfo = new() { FontSize = 11.5, Foreground = Dim, TextWrapping = TextWrapping.Wrap, Text = "无（拾取命中自动记录）" };
    private (double x, double y, double z, double w, double h)? _lastSelBounds;   // 最近框选的世界包围盒（ROI 类参数预填）
    private int _toolSeq;                             // 工具产出实体序号
    private static readonly Brush MeasureBrush = new SolidColorBrush(Color.FromRgb(0xFF, 0xD6, 0x0A));
    private static readonly Brush PolyBrush = new SolidColorBrush(Color.FromRgb(0xBF, 0x5A, 0xF2));
    private static readonly Brush RegistBrush = new SolidColorBrush(Color.FromRgb(0xFF, 0x26, 0xFF));
    private static readonly Brush ActiveTintYellow = new SolidColorBrush(Color.FromArgb(0x2E, 0xFF, 0xD6, 0x0A));   // 量测激活（systemYellow tint）
    private static readonly Brush ActiveTintPurple = new SolidColorBrush(Color.FromArgb(0x2E, 0xBF, 0x5A, 0xF2));   // 多边形激活（systemPurple tint）
    private static readonly Brush ActiveTintMagenta = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0x26, 0xFF));  // 配准激活（magenta tint）
    private static readonly Brush ActiveTintBlue = new SolidColorBrush(Color.FromArgb(0x2E, 0x0A, 0x84, 0xFF));     // 锁俯仰激活（systemBlue tint）
    private readonly TextBlock _emptyState = new()
    {
        Text = "Ctrl+左键框选 · 拖拽导入点云 · 按住把手拖动工具组",
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
        HorizontalAlignment = HorizontalAlignment.Center,
        VerticalAlignment = VerticalAlignment.Center,
        IsHitTestVisible = false,
        IsVisible = false,
    };
    private Border? _helpOverlay;                     // 快捷键帮助浮层（? 键开关）
    private Button? _toolOpsBtn;                      // 算子菜单按钮（Q 键定位用）
    private readonly CloudUndo _undo = new();         // Undo/Redo 栈（通用撤销：删除/新建/变换/合并）
    private string? _settingsPath;                    // config/cloud3d_view.json（视图设置持久化）
    private readonly List<(double X, double Y, double Z)> _pickHistory = new();
    private const int PickHistoryMax = 50;
    private static readonly System.Collections.Generic.Dictionary<string, CloudAction> IconAction = new()
    {
        ["extract"] = CloudAction.ExtractInside,
        ["crop"] = CloudAction.ExtractOutside,
        ["boxcrop"] = CloudAction.BoxCrop,
        ["polygon"] = CloudAction.PolyToggle,
        ["measure"] = CloudAction.MeasureToggle,
        ["clear"] = CloudAction.SelectClear,
        ["copy"] = CloudAction.CopyMeasurements,
        ["regist"] = CloudAction.RegistToggle,
        ["random"] = CloudAction.RandomSample,
        ["outlier"] = CloudAction.OutlierFilter,
        ["clip"] = CloudAction.PlaneSplit,
        ["merge"] = CloudAction.Merge,
        ["transform"] = CloudAction.Transform,
        ["normal"] = CloudAction.Normals,
        ["heightmap"] = CloudAction.HeightMap,
        ["import"] = CloudAction.Import,
        ["export"] = CloudAction.Export,
        ["operator"] = CloudAction.OperatorMenu,
        ["focus"] = CloudAction.FocusEntity,
        ["lockpitch"] = CloudAction.LockPitchToggle,
    };
    private const double PointSizeStep = 0.5;
    private readonly Polyline _polyPreview = new()    // 多边形预览线（overlay 层，随加点更新）
    {
        IsVisible = false, StrokeThickness = 1.5, Stroke = PolyBrush,
    };

    private bool _lockPitch;                          // 锁定竖直轴旋转：orbit 只绕世界竖直轴水平转（俯视巡检）
    private Button? _lockPitchBtn;

    // 工具条分组：组可拖动重排（对齐算子库栏目头拖拽架构），tool_groups.json 持久化
    private readonly List<string> _groupOrder = new()
        { "视图", "选取", "量测", "配准", "处理", "文件", "算子" };
    private string? _orderPath;
    private string? _dragGroup;                       // 拖拽中的组名
    private int _dragStartIndex = -1;                 // 拖拽起始组索引
    private readonly System.Collections.Generic.HashSet<string> _hiddenGroups = new();   // 右键菜单隐藏的组

    // 点对配准：两片点云交替拾取对应点对（≥3 对）→ Horn 刚体变换粗配准 → 衔接 ICP 算子精配
    private bool _registMode;
    private Button? _registBtn;                        // 配准模式按钮（激活高亮）
    private (string Cloud, int Index)? _registPending; // 已拾取的移动侧点（待对应参考点）
    private string? _registMovingCloud;                // 移动实体（第一对的拾取侧锁定）
    private readonly List<((string Cloud, int Index) Moving, (string Cloud, int Index) Ref)> _registPairs = new();
    private readonly Canvas _registLayer = new() { IsHitTestVisible = false };   // 配对线（洋红）

    /// <summary>点云集合变化（新增/上传完成/删除/显隐）——点云树订阅刷新。</summary>
    public event Action? CloudsChanged;

    // ==================== 数据源切换 ====================

    /// <summary>重填数据源下拉：总线上最新载荷为点云的主题（排除 cloud/removed 控制主题），当前源必在列。</summary>
    private void RefreshSourceTopics()
    {
        _refreshingSource = true;
        try
        {
            _sourceCombo.Items.Clear();
            var topics = _bus.GetTopics()
                .Where(t => t.LatestType == nameof(CloudPayload) && t.Topic != CloudPayload.TopicRemoved)
                .Select(t => t.Topic)
                .Distinct()
                .ToList();
            if (!topics.Contains(_sourceTopic)) topics.Insert(0, _sourceTopic);
            if (!topics.Contains(CloudPayload.TopicMerged)) topics.Insert(0, CloudPayload.TopicMerged);
            foreach (var t in topics) _sourceCombo.Items.Add(t);
            _sourceCombo.SelectedItem = _sourceTopic;
        }
        finally { _refreshingSource = false; }
    }

    private void OnSourceSelected(object? sender, SelectionChangedEventArgs e)
    {
        if (_refreshingSource) return;
        if (_sourceCombo.SelectedItem is not string topic || topic == _sourceTopic) return;
        SwitchSource(topic);
    }

    /// <summary>切换数据源：退订旧主题 → 清空未锁定实体（锁定的保留——用户拍板）→ 订阅新主题
    /// （总线保留帧自动送达，有数据立即显示）。cloud/removed 控制订阅不受影响。</summary>
    private void SwitchSource(string topic)
    {
        if (topic == _sourceTopic) return;
        _sourceTopic = topic;
        _busSub?.Dispose();
        int kept = _clouds.Values.Count(c => c.Locked);
        foreach (var id in _clouds.Values.Where(c => !c.Locked).Select(c => c.Id).ToList())
            RemoveCloud(id);
        _busSub = _bus.Subscribe<CloudPayload>(topic,
            p => SetCloud(p.Count, p.Points, p.ColorsRgb, p.Source, p.Id), uiThread: true);
        _ctx?.SetStatus(kept > 0 ? $"3D 工作台数据源 → {topic}（已锁定保留 {kept} 实体）" : $"3D 工作台数据源 → {topic}");
        Draw();
    }

    /// <summary>快照创建完成（参数=快照视图）——插件据此登记跟踪（关闭后下次快照时回收 GL 资源）。</summary>
    public event Action<Control>? SnapshotCreated;

    /// <summary>冻结当前可见实体与相机姿态为只读快照标签页：点/色数组引用共享（载荷约定不可变），
    /// 上传未完成的实体按已揭示点数截取（所见即所得）；restoreKey=null 不进会话。</summary>
    private void Snapshot()
    {
        if (_ctx is null) return;
        if (!HasCloud)
        {
            _ctx.SetStatus("无点云可快照");
            return;
        }
        var ents = _clouds.Values
            .Where(c => _renderer.IsVisible(c.Id) && c.Loaded > 0)
            .Select(c => new CloudSnapshotView.SnapEntity(
                c.Id, c.Pts, c.Col, Math.Min(c.Loaded, c.Count),
                c.BoundsMin, c.BoundsMax, c.ColorMode, c.PointSize, c.SingleColor, (double[])c.Xform.Clone()))
            .ToList();
        if (ents.Count == 0)
        {
            _ctx.SetStatus("无可见点云可快照");
            return;
        }
        var snap = new CloudSnapshotView(ents, _orientation, _dist, _target, _travel);
        _ctx.OpenDocument($"📷 快照 · 3D 点云 {DateTime.Now:HH:mm:ss}", snap, null);
        SnapshotCreated?.Invoke(snap);
    }

    private bool HasCloud => _clouds.Count > 0;

    private readonly bool _embed;

    public Cloud3DView(IBus bus, bool embed = false, IPluginContext? ctx = null)
    {
        _bus = bus;
        _ctx = ctx;
        _renderer = new GlCloudRenderer(allowBackgroundWorker: !embed);   // 内嵌预览禁后台 worker（多 worker GL 上下文互卡死锁）
        _renderer.UploadCompleted += OnUploadCompleted;   // 后台 worker 线程触发，回调内部转 UI 线程
        Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20));
        Focusable = true;
        if (embed)
        {
            // 内嵌模式（draw 预览卡片用）：不订阅总线（数据由上游直喂 SetCloud）
            _embed = true;
        }

        _projCombo.Items.Add("透视投影");
        _projCombo.Items.Add("正交投影");
        _projCombo.SelectedIndex = 0;
        _projCombo.SelectionChanged += (_, _) =>
        {
            if (_settingsPath is not null) SaveViewSettings();   // 投影直改（含组合框）同步持久化
            Draw();
        };

        foreach (var name in new[] { "等轴", "前", "后", "左", "右", "上", "下" })
            _presetCombo.Items.Add(name);
        _presetCombo.SelectedIndex = 0;
        _presetCombo.SelectionChanged += (_, _) => ApplyPreset(_presetCombo.SelectedIndex);

        HookCommit(_strideBox,
            v =>
            {
                int n = (int)Math.Round(v);
                if (n < 1 || n > 1000) return false;
                _stride = n;
                Draw();
                SaveViewSettings();
                return true;
            },
            () => _stride.ToString());

        HookCommit(_farClipBox,
            v =>
            {
                if (v < 0 || v > 1e9) return false;
                _farClip = (float)v;
                SaveViewSettings();
                Draw();
                return true;
            },
            () => _farClip.ToString("0.##"));

        var resetBtn = new Button { Classes = { "ghost" }, Content = "重置视角", MinWidth = 76 };
        resetBtn.Click += (_, _) => ResetView();
        var clearBtn = new Button { Classes = { "ghost" }, Content = "清空选区", MinWidth = 76 };
        clearBtn.Click += (_, _) => { _pickedCloud = null; _pickedIndex = -1; ClearSelection(); Draw(); };
        var clearRangeBtn = new Button { Classes = { "ghost" }, Content = "清除色带域", MinWidth = 76 };
        clearRangeBtn.Click += (_, _) =>
        {
            _rangeLoBox.Text = "";
            _rangeHiBox.Text = "";
            ApplyColorRange();
        };
        HookCommit(_rangeLoBox, v => { ApplyColorRange(); return true; }, () => _rangeLoBox.Text ?? "");
        HookCommit(_rangeHiBox, v => { ApplyColorRange(); return true; }, () => _rangeHiBox.Text ?? "");
        var clearClipBtn = new Button { Classes = { "ghost" }, Content = "清除切片", MinWidth = 76 };
        clearClipBtn.Click += (_, _) =>
        {
            _clipLoBox.Text = "";
            _clipHiBox.Text = "";
            _renderer.SetZClip(ClipAxis, null, null);
            Draw();
        };
        _clipAxisCombo.Items.Add("X");
        _clipAxisCombo.Items.Add("Y (高度)");
        _clipAxisCombo.Items.Add("Z");
        _clipAxisCombo.SelectedIndex = 1;   // 默认高度轴
        _lightSlider.ValueChanged += (_, e) => { _renderer.SetLightStrength((float)e.NewValue); Draw(); };
        _clipAxisCombo.SelectionChanged += (_, _) => ApplyZClip();   // 轴切换即按新轴重应用切片带
        HookCommit(_clipLoBox, v => { ApplyZClip(); return true; }, () => _clipLoBox.Text ?? "");
        HookCommit(_clipHiBox, v => { ApplyZClip(); return true; }, () => _clipHiBox.Text ?? "");

        // 数据源下拉的初始化移到属性面板构建处（数据源已迁入右侧属性面板）
        _paramsContent = BuildParams(resetBtn, clearBtn, clearRangeBtn, clearClipBtn, !_embed && ctx is not null);

        // 工具栏（Apple：无填充、底边 hairline 分隔、图标统一底座）
        // 工具条分组持久化路径（仓库根/config/tool_groups.json）
        if (ctx is not null)
        {
            try
            {
                var repo = System.IO.Path.GetFullPath(System.IO.Path.Combine(AppContext.BaseDirectory, "..", "..", ".."));
                _orderPath = System.IO.Path.Combine(repo, "config", "tool_groups.json");
                LoadToolOrder();
            }
            catch { }
        }

        // 视图设置持久化路径 + 加载（抽稀/截断/切片/色带域/投影/锁定轴）
        if (ctx is not null && _settingsPath is null)
        {
            try
            {
                var repo = System.IO.Path.GetFullPath(System.IO.Path.Combine(AppContext.BaseDirectory, "..", "..", ".."));
                _settingsPath = System.IO.Path.Combine(repo, "config", "cloud3d_view.json");
                var vs = CloudViewSettings.Load(_settingsPath);
                _stride = vs.Stride;
                _farClip = (float)vs.FarClip;
                _farClipBox.Text = vs.FarClip.ToString("F1", CultureInfo.InvariantCulture);
                _strideBox.Text = vs.Stride.ToString(CultureInfo.InvariantCulture);
                _clipAxisCombo.SelectedIndex = vs.ClipAxis;
                if (vs.ClipLo is not null && vs.ClipHi is not null)
                {
                    _clipLoBox.Text = vs.ClipLo.Value.ToString("F3", CultureInfo.InvariantCulture);
                    _clipHiBox.Text = vs.ClipHi.Value.ToString("F3", CultureInfo.InvariantCulture);
                    _renderer.SetZClip(vs.ClipAxis, (float)vs.ClipLo, (float)vs.ClipHi);
                }
                if (vs.ColorMin is not null && vs.ColorMax is not null)
                {
                    _rangeLoBox.Text = vs.ColorMin.Value.ToString("F3", CultureInfo.InvariantCulture);
                    _rangeHiBox.Text = vs.ColorMax.Value.ToString("F3", CultureInfo.InvariantCulture);
                }
                _lockPitch = vs.LockPitch;
                _projCombo.SelectedIndex = vs.Projection;
            }
            catch { }
        }

        var toolbar = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Margin = new Thickness(8, 6, 8, 0),
        };
        if (!_embed && ctx is not null)
        {
            // 顶部只放图标：快照按钮用矢量相机图标，去掉文字；数据源已迁入属性面板
            var snapBtn = new Button
            {
                Classes = { "ghost" },
                Content = BuildCameraIcon(),
                Cursor = new Cursor(StandardCursorType.Hand),
                Width = 24, Height = 22,
                Padding = new Thickness(0),
                HorizontalContentAlignment = HorizontalAlignment.Center,
                VerticalContentAlignment = Avalonia.Layout.VerticalAlignment.Center,
            };
            snapBtn.Click += (_, _) => Snapshot();
            ToolTip.SetTip(snapBtn, "快照：把当前可见点云与视角冻结为只读标签页（用于对比；不随数据更新，不进会话）");
            toolbar.Children.Add(snapBtn);

            BuildToolbar(toolbar);   // 按功能分组渲染 + 组拖动排序（tool_groups.json 持久化）
            AttachToolbarDragHandlers(toolbar);
        }

        _overlay.Children.Add(_rubber);
        _overlay.Children.Add(_pickMark);
        _overlay.Children.Add(_measureLayer);
        _overlay.Children.Add(_registLayer);
        _overlay.Children.Add(_polyPreview);
        _overlay.Children.Add(_emptyState);
        _tripod.Children.Add(new Ellipse
        {
            Width = 72, Height = 72,
            Stroke = new SolidColorBrush(Color.FromArgb(0x28, 0xFF, 0xFF, 0xFF)),
            StrokeThickness = 1,
        });
        _tripod.Children.Add(_axZ);
        _tripod.Children.Add(_axY);
        _tripod.Children.Add(_axX);
        _tripod.Children.Add(_axZl);
        _tripod.Children.Add(_axYl);
        _tripod.Children.Add(_axXl);

        _viewArea = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x16, 0x16, 0x18)),
            ClipToBounds = true,
            Child = new Grid { Children = { _image, _overlay, _tripod } },
        };
        _viewArea.PointerWheelChanged += OnWheel;
        _viewArea.PointerPressed += OnPress;
        _viewArea.PointerMoved += OnMove;
        _viewArea.PointerReleased += OnRelease;
        _viewArea.PointerCaptureLost += (_, _) => { _orbitLast = null; _panLast = null; };
        _viewArea.SizeChanged += (_, e) =>
        {
            _scaling = (TopLevel.GetTopLevel(this)?.RenderScaling) ?? 1;
            _vpW = Math.Max(1, (int)(e.NewSize.Width * _scaling));
            _vpH = Math.Max(1, (int)(e.NewSize.Height * _scaling));
            _frame = new byte[_vpW * _vpH * 4];
            _wb = null;
            Draw();
        };
        KeyDown += (_, e) =>
        {
            // 文本框聚焦：全键交还编辑器（工具快捷键/撤销链不误触输入）
            if (e.Source is TextBox) return;

            var action = CloudHotkeys.Resolve(e.Key, e.KeyModifiers, _measureMode, _polyMode, _registMode);
            if (action == CloudAction.None) return;
            e.Handled = true;
            switch (action)
            {
                case CloudAction.ModeConfirm:
                    if (_polyMode) ApplyPolygonSelection();
                    else if (_registMode) ApplyRegistPairs();
                    else if (_measureMode && _activeMeasure is not null) { _activeMeasure = null; _ctx?.SetStatus("折线已结束——继续拾取开始新折线"); }
                    return;
                case CloudAction.ModeBack:
                    if (_registMode) UndoRegistStep();
                    else UndoMeasureStep();
                    return;
                case CloudAction.ModeCancel:
                    if (_polyMode) { EndPolyMode(); _ctx?.SetStatus("多边形选取已取消"); }
                    else if (_registMode) { EndRegistMode(); _ctx?.SetStatus("点对配准已退出"); }
                    else if (_measureMode) { _measureFirst = null; _ctx?.SetStatus("已取消挂起的起点（继续拾取重新开始）"); }
                    else
                    {
                        _pickedCloud = null; _pickedIndex = -1; ClearSelection();
                        if (_pickHistory.Count > 0) { _pickHistory.Clear(); UpdatePickHistoryInfo(); _ctx?.SetStatus("已清空拾取历史"); }
                    }
                    Draw();
                    return;
                case CloudAction.MeasureToggle: ToggleMeasure(); return;
                case CloudAction.PolyToggle: TogglePoly(); return;
                case CloudAction.RegistToggle: ToggleRegist(); return;
                case CloudAction.LockPitchToggle: ToggleLockPitch(); return;
                case CloudAction.ExtractInside: ExtractSelected(keepInside: true); return;
                case CloudAction.ExtractOutside: ExtractSelected(keepInside: false); return;
                case CloudAction.BoxCrop: BoxCropTool(); return;
                case CloudAction.SelectAll: SelectAllPoints(); return;
                case CloudAction.SelectInvert: InvertSelection(); return;
                case CloudAction.SelectClear: _pickedCloud = null; _pickedIndex = -1; ClearSelection(); Draw(); return;
                case CloudAction.FocusEntity: FocusSelectedTool(); return;
                case CloudAction.ResetView: ResetView(); return;
                case CloudAction.Preset1: ApplyPreset(0); return;
                case CloudAction.Preset2: ApplyPreset(1); return;
                case CloudAction.Preset3: ApplyPreset(2); return;
                case CloudAction.Preset4: ApplyPreset(3); return;
                case CloudAction.Preset5: ApplyPreset(4); return;
                case CloudAction.Preset6: ApplyPreset(5); return;
                case CloudAction.Preset7: ApplyPreset(6); return;
                case CloudAction.ProjToggle: ToggleProj(); return;
                case CloudAction.NudgeLeft: NudgeCamera(-5, 0); return;
                case CloudAction.NudgeRight: NudgeCamera(5, 0); return;
                case CloudAction.NudgeUp: NudgeCamera(0, 5); return;
                case CloudAction.NudgeDown: NudgeCamera(0, -5); return;
                case CloudAction.PanLeft: PanBy(-0.05, 0); return;
                case CloudAction.PanRight: PanBy(0.05, 0); return;
                case CloudAction.PanUp: PanBy(0, 0.05); return;
                case CloudAction.PanDown: PanBy(0, -0.05); return;
                case CloudAction.PointSizeUp: AdjustPointSize(+0.5); return;
                case CloudAction.PointSizeDown: AdjustPointSize(-0.5); return;
                case CloudAction.RandomSample: RandomSampleTool(); return;
                case CloudAction.OutlierFilter: OutlierTool(); return;
                case CloudAction.PlaneSplit: RansacTool(); return;
                case CloudAction.Merge: MergeTool(); return;
                case CloudAction.Transform: TransformTool(); return;
                case CloudAction.Normals: NormalsTool(); return;
                case CloudAction.HeightMap: HeightMapTool(); return;
                case CloudAction.Import: ImportTool(); return;
                case CloudAction.Export: ExportTool(); return;
                case CloudAction.OperatorMenu: OpenOperatorMenu(); return;
                case CloudAction.CopyMeasurements: CopyMeasurements(); return;
                case CloudAction.Undo: PerformUndo(); return;
                case CloudAction.Redo: PerformRedo(); return;
                case CloudAction.Help: ToggleHelp(); return;
            }
        };

        // 工具栏底边 hairline（跨全宽）
        var toolbarHost = new Grid { RowDefinitions = new RowDefinitions("*,Auto") };
        toolbarHost.Children.Add(toolbar);
        toolbarHost.Children.Add(new Border
        {
            VerticalAlignment = VerticalAlignment.Bottom, Height = 1,
            Background = new SolidColorBrush(Color.FromArgb(0x22, 0xFF, 0xFF, 0xFF)),
        });

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        root.Children.Add(toolbarHost);
        _viewArea.SetValue(Grid.RowProperty, 1);
        root.Children.Add(_viewArea);
        Content = root;

        if (!_renderer.Ok)
        {
            _viewArea.Child = new TextBlock
            {
                Text = "无 GL 上下文",
                Foreground = new SolidColorBrush(Color.FromRgb(0xF1, 0x4C, 0x4C)),
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
        }

        if (!_embed)
        {
            _busSub = bus.Subscribe<CloudPayload>(CloudPayload.TopicMerged,
                p => SetCloud(p.Count, p.Points, p.ColorsRgb, p.Source, p.Id), uiThread: true);
            _busSubRemove = bus.Subscribe<CloudPayload>(CloudPayload.TopicRemoved,
                p => { if (!string.IsNullOrWhiteSpace(p.Id)) RemoveCloud(p.Id); }, uiThread: true);
        }
    }

    // ==================== 属性面板参数区 ====================

    public Control? CreateParamsContent() => _paramsContent;

    private StackPanel BuildParams(Button resetBtn, Button clearBtn, Button clearRangeBtn, Button clearClipBtn, bool showDataSource)
    {
        var panel = new StackPanel { Margin = new Thickness(14, 4, 10, 0), Spacing = 2 };
        // 数据源切换（从顶部工具栏迁来）：列出总线上有数据的 cloud/* 主题（cloud/removed 为控制主题不列出）
        if (showDataSource)
        {
            ToolTip.SetTip(_sourceCombo, "切换工作台订阅的点云主题（切换时未锁定的点云会被清空，锁定的保留）");
            _sourceCombo.DropDownOpened += (_, _) => RefreshSourceTopics();
            _sourceCombo.SelectionChanged += OnSourceSelected;
            RefreshSourceTopics();
            panel.Children.Add(Section("数据源"));
            panel.Children.Add(Row("主题", _sourceCombo));
        }
        panel.Children.Add(Section("视角控制"));
        panel.Children.Add(Row("投影", _projCombo));
        panel.Children.Add(Row("抽稀间隔", _strideBox));
        panel.Children.Add(Row("截断距离", _farClipBox));
        panel.Children.Add(Row("视角预设", _presetCombo));
        panel.Children.Add(Row("视角", resetBtn));
        panel.Children.Add(Section("高度切片"));
        panel.Children.Add(RowText(new TextBlock
        {
            Text = "按数据 Y（高度）坐标过滤显示，GPU 侧过滤实时生效；两端都填才启用",
            FontSize = 11, Foreground = Dim, TextWrapping = TextWrapping.Wrap,
        }));
        panel.Children.Add(Row("切片轴", _clipAxisCombo));
        panel.Children.Add(Row("切片下限", _clipLoBox));
        panel.Children.Add(Row("切片上限", _clipHiBox));
        panel.Children.Add(Row("清除", clearClipBtn));
        panel.Children.Add(Section("选区"));
        panel.Children.Add(RowText(_selInfo));
        panel.Children.Add(Row("清空", clearBtn));
        panel.Children.Add(Section("量测"));
        panel.Children.Add(RowText(_measureInfo));
        var exportCsvBtn = new Button { Classes = { "ghost" }, Content = "导出 CSV", MinWidth = 76 };
        exportCsvBtn.Click += (_, _) => ExportMeasurementsCsv();
        panel.Children.Add(Row("导出", exportCsvBtn));
        panel.Children.Add(Section("拾取历史"));
        panel.Children.Add(RowText(_pickHistoryInfo));
        var copyHistBtn = new Button { Classes = { "ghost" }, Content = "复制历史", MinWidth = 76 };
        copyHistBtn.Click += (_, _) =>
        {
            if (_pickHistory.Count == 0) { _ctx?.SetStatus("拾取历史为空"); return; }
            var lines = _pickHistory.Select(p => FormattableString.Invariant($"{p.X:F4},{p.Y:F4},{p.Z:F4}"));
            var top = TopLevel.GetTopLevel(this);
            _ = top?.Clipboard?.SetTextAsync(string.Join(Environment.NewLine, lines));
            _ctx?.SetStatus($"拾取历史已复制（{_pickHistory.Count} 点，x,y,z 每行一坐标）");
        };
        panel.Children.Add(Row("复制", copyHistBtn));
        var exportHistBtn = new Button { Classes = { "ghost" }, Content = "导出 CSV", MinWidth = 76 };
        exportHistBtn.Click += (_, _) => ExportPickHistoryCsv();
        panel.Children.Add(Row("导出", exportHistBtn));
        panel.Children.Add(Section("点云"));
        panel.Children.Add(RowText(_cloudInfo));
        panel.Children.Add(RowText(_pickInfo));
        panel.Children.Add(Row("光照强度", _lightSlider));
        panel.Children.Add(RowText(new TextBlock
        {
            Text = "伪彩色带域（作用于当前工作实体；留空=包围盒域）",
            FontSize = 11, Foreground = Dim, TextWrapping = TextWrapping.Wrap,
        }));
        panel.Children.Add(Row("色带下限", _rangeLoBox));
        panel.Children.Add(Row("色带上限", _rangeHiBox));
        panel.Children.Add(Row("清除域", clearRangeBtn));
        panel.Children.Add(RowText(new TextBlock
        {
            Text = "颜色/点大小/变换矩阵在左侧点云树详情区按实体设置",
            FontSize = 11.5, Foreground = Dim, TextWrapping = TextWrapping.Wrap,
        }));
        return panel;
    }

    /// <summary>纯矢量相机图标（快照按钮）：机身圆角矩形 + 顶部取景凸起 + 镜头圆。
    /// 用 Viewbox 统一缩放（镜头圆也等比缩小），size 即最终像素边长。</summary>
    private static Control BuildCameraIcon(double size = 13)
    {
        // 基于 16×16 本地坐标系绘制，再由 Viewbox 缩放到 size
        const string body =
            "M2.2,5.2 L13.8,5.2 A1.6,1.6 0 0 1 15.4,6.8 L15.4,13.2 A1.6,1.6 0 0 1 13.8,14.8 L2.2,14.8 A1.6,1.6 0 0 1 0.6,13.2 L0.6,6.8 A1.6,1.6 0 0 1 2.2,5.2 Z";
        const string top = "M5.4,5.2 L6.2,3.2 L9.8,3.2 L10.6,5.2";
        var brush = new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xEA));
        var canvas = new Panel { Width = 16, Height = 16 };
        canvas.Children.Add(new Avalonia.Controls.Shapes.Path
        {
            Data = StreamGeometry.Parse(body + " " + top),
            Stroke = brush,
            StrokeThickness = 1.3,
            StrokeLineCap = PenLineCap.Round,
            Stretch = Stretch.Uniform,
            Width = 16, Height = 16,
        });
        canvas.Children.Add(new Ellipse
        {
            Width = 5.2, Height = 5.2,
            Stroke = brush, StrokeThickness = 1.3,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 1.4, 0, 0),
        });
        return new Viewbox { Child = canvas, Width = size, Height = size, Stretch = Stretch.Uniform };
    }

    private static Control Section(string title) => new TextBlock
    {
        Text = title,
        FontWeight = FontWeight.SemiBold,
        FontSize = 12,
        Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xEC)),
        Margin = new Thickness(0, 10, 0, 4),
    };

    private static Control Row(string label, Control editor)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("64,*"), Margin = new Thickness(0, 2) };
        grid.Children.Add(new TextBlock { Text = label, FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
        editor.SetValue(Grid.ColumnProperty, 1);
        editor.Margin = new Thickness(6, 2, 0, 2);
        grid.Children.Add(editor);
        return grid;
    }

    private static Control RowText(TextBlock info) => new Border { Margin = new Thickness(2, 2, 0, 2), Child = info };

    private static Line AxisLine(byte r, byte g, byte b) => new()
    {
        Stroke = new SolidColorBrush(Color.FromRgb(r, g, b)),
        StrokeThickness = 1.6,
        StartPoint = new Point(36, 36),
        EndPoint = new Point(36, 36),
    };

    private static TextBlock AxisLabel(string text, byte r, byte g, byte b) => new()
    {
        Text = text, FontSize = 10, FontWeight = FontWeight.SemiBold,
        Foreground = new SolidColorBrush(Color.FromRgb(r, g, b)),
    };

    private void HookCommit(TextBox box, Func<double, bool> apply, Func<string> currentText, Action? afterApply = null)
    {
        void Commit()
        {
            if (double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) && apply(v))
            {
                afterApply?.Invoke();
                return;
            }
            box.Text = currentText();
            box.CaretIndex = box.Text?.Length ?? 0;
        }
        box.KeyDown += (_, e) => { if (e.Key == Key.Enter) { Commit(); e.Handled = true; } };
        box.LostFocus += (_, _) => Commit();
    }

    // ==================== 点云实体管理 ====================

    /// <summary>矢量三分量全部有限（NaN/Infinity 毒化防护）。</summary>
    private static bool IsFinite(Vector3 v) =>
        float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z);

    /// <summary>同 Id 重发=更新（重新分块上传），新 Id=新增并取景。
    /// keepCamera=true 跳过新实体取景（提取/剔除等对比场景不打断当前视角）。</summary>
    public void SetCloud(int count, float[] points, byte[]? colorsRgb, string? source, string? id, bool keepCamera = false)
    {
        if (!_renderer.Ok || count <= 0 || points.Length < count * 3) return;
        string cloudId = string.IsNullOrWhiteSpace(id) ? "点云" : id.Trim();
        bool isNew = !_clouds.ContainsKey(cloudId);
        var data0 = isNew ? null : _clouds[cloudId];

        // 同 Id 重发更小点云：清空拾取/选区引用（否则拾取标记按旧索引越界崩溃）
        if (_pickedCloud == cloudId && _pickedIndex >= count)
        {
            _pickedCloud = null;
            _pickedIndex = -1;
        }

        // 采样估算边界（步长 64）：仅用于立即取景；精确边界由分块上传逐点合并
        // NaN/Infinity 毒化防护：非有限分量跳过（一个坏点会让 Bounds 变 NaN → 相机/裁剪全 NaN 整个视图空白）
        var min = new Vector3(float.MaxValue);
        var max = new Vector3(float.MinValue);
        bool anyFinite = false;
        for (int i = 0; i < count; i += 64)
        {
            var p = new Vector3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]);
            if (!IsFinite(p)) continue;
            min = Vector3.ComponentMin(min, p);
            max = Vector3.ComponentMax(max, p);
            anyFinite = true;
        }
        if (!anyFinite) return;   // 全部无效点：不入库（避免 NaN 边界毒化相机）
        min = Vector3.ComponentMin(min, new Vector3(points[0], points[1], points[2]));

        if (isNew)
        {
            _hasColors = colorsRgb is not null && colorsRgb.Length >= count * 3;
        }
        var data = new CloudData
        {
            Id = cloudId,
            Pts = points,
            Col = colorsRgb is not null && colorsRgb.Length >= count * 3 ? colorsRgb : null,
            Sel = new byte[count],
            Count = count,
            Loaded = 0,
            SelCount = 0,
            BoundsMin = min,
            BoundsMax = max,
            Source = string.IsNullOrWhiteSpace(source) ? null : source,
            ColorMode = isNew ? (_hasColors ? 0 : 1) : (data0?.ColorMode ?? 1),
            PointSize = isNew ? 3f : (data0?.PointSize ?? 3f),
            SingleColor = isNew ? _defaultSingle : (data0?.SingleColor ?? _defaultSingle),
        };
        data.Xform = data0?.Xform ?? data.Xform;   // 更新：保留实体已有变换；新建=单位阵
        data.Locked = data0?.Locked ?? false;      // 更新：保留锁标志（同 Id 重发不清锁）
        data.SingleColor = data0?.SingleColor ?? data.SingleColor;   // 更新：保留单色设置（重发不丢样式）
        data.ColorMin = data0?.ColorMin; data.ColorMax = data0?.ColorMax;   // 更新：保留色带域覆盖
        _clouds[cloudId] = data;
        // 法线随上传管线重分配（杜绝旧 Cap 与新 normals 的越界竞态）；更新路径沿用旧法线
        var normals = data0?.Normals ?? data.Normals;
        if (normals is not null && normals.Length >= count * 3) data.Normals = normals;
        else data.Normals = null;
        // 实体壳先创建（BeginUpload），样式/变换后设——顺序反了首帧发布会丢样式（实体尚不存在）
        _renderer.BeginUpload(cloudId, count, min, max, data.Pts, data.Col, data.Normals);
        _renderer.SetEntityStyle(cloudId, data.ColorMode, data.PointSize, data.SingleColor);
        _renderer.SetEntityTransform(cloudId, data.Xform);
        _renderer.SetEntityLit(cloudId, data.Normals is not null);   // 法线保留则恢复光照
        if (data.ColorMin is not null && data.ColorMax is not null)
            _renderer.SetEntityColorRange(cloudId, data.ColorMin, data.ColorMax);   // 重发恢复色带域

        if (isNew && !keepCamera)
        {
            _target = (min + max) / 2;
            _fitRadius = Math.Max(0.01f, (max - min).Length / 2);
            if (!float.IsFinite(_fitRadius)) _fitRadius = 10f;   // NaN 兜底（坏点毒化边界时整视图永久空白）
            ResetView();
        }

        if (_uploadTimer is null)
        {
            _uploadTimer = new Avalonia.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
            _uploadTimer.Tick += UploadTick;
        }
        if (_idleTimer is null)
        {
            _idleTimer = new Avalonia.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
            _idleTimer.Tick += (_, _) => { _interacting = false; _idleTimer?.Stop(); Draw(); };
        }

        _uploadTimer.Stop();
        _uploadTimer.Start();
        UpdateCloudInfoText();
        CloudsChanged?.Invoke();
    }

    /// <summary>分块上传驱动：后台模式每拍仅入队一块（GL 上传在 worker 共享上下文执行，UI 零阻塞）；
    /// 同步回退路径每拍在 UI 线程传一块并逐点合并精确边界（采样预估只用于取景，UV 映射/伪彩色域需要精确值）。</summary>
    private void UploadTick(object? sender, EventArgs e)
    {
        var data = _clouds.Values.FirstOrDefault(c => c.Loaded < c.Count);
        if (data is null || !_renderer.Ok) { _uploadTimer?.Stop(); return; }
        int n = Math.Min(UploadChunkPoints, data.Count - data.Loaded);
        if (_renderer.BackgroundUpload)
        {
            data.Loaded += n;
            _renderer.SetRevealed(data.Id, data.Loaded);   // 数据已一次性进显存，此处仅揭示进度
        }
        else
        {
            var min = data.BoundsMin;
            var max = data.BoundsMax;
            for (int i = data.Loaded; i < data.Loaded + n; i++)
            {
                var p = new Vector3(data.Pts[i * 3], data.Pts[i * 3 + 1], data.Pts[i * 3 + 2]);
                if (!IsFinite(p)) continue;   // NaN 毒化防护：坏点不进边界（否则 min/max 全 NaN 渲染异常）
                min = Vector3.ComponentMin(min, p);
                max = Vector3.ComponentMax(max, p);
            }
            data.BoundsMin = min;
            data.BoundsMax = max;
            _renderer.UploadChunk(data.Id, data.Pts, data.Col, data.Loaded, n, min, max);
            data.Loaded += n;
        }
        if (data.Loaded >= data.Count)
        {
            UpdateCloudInfoText();   // 完成时刷新"（上传中）"标记
            CloudsChanged?.Invoke();
        }
        Draw();
    }

    /// <summary>后台 worker 上传完成回调：把实体最终精确边界回填到视图账本（UV 映射/拾取需要精确值）。</summary>
    private void OnUploadCompleted(string id, Vector3 min, Vector3 max)
    {
        Avalonia.Threading.Dispatcher.UIThread.Post(() =>
        {
            if (!_clouds.TryGetValue(id, out var d)) return;
            d.BoundsMin = min;
            d.BoundsMax = max;
            if (d.Loaded >= d.Count) { UpdateCloudInfoText(); CloudsChanged?.Invoke(); }
        });
    }

    /// <summary>移除点云实体（锁定=拒绝并提示，返回 false——树删除/Del/总线移除指令/切源清理统一走此闸）。
    /// 删除操作压入 Undo 栈（Ctrl+Z 可恢复）。</summary>
    public bool RemoveCloud(string id)
    {
        if (!_clouds.TryGetValue(id, out var data)) return false;
        if (data.Locked)
        {
            _ctx?.SetStatus($"「{id}」已锁定，先解锁再删除");
            return false;
        }
        PushRemoveUndo(data);
        _clouds.Remove(id);
        _renderer.RemoveEntity(id);
        if (_workingEntity == id) _workingEntity = null;
        if (_pickedCloud == id) { _pickedCloud = null; _pickedIndex = -1; UpdatePickMark(); }
        // 量测折线/配对引用了被删实体：整条移除，层重建 + 清单刷新
        if (_measurements.Any(m => m.Points.Any(p => p.Cloud == id)))
        {
            _measurements.RemoveAll(m => m.Points.Any(p => p.Cloud == id));
            _activeMeasure = null;
            RebuildMeasureLayer();
            UpdateMeasureInfo();
        }
        if (_registMode && _registPairs.Any(p => p.Moving.Cloud == id || p.Ref.Cloud == id))
        {
            _registPairs.Clear();
            _registPending = null;
            RebuildRegistLayer();
            _ctx?.SetStatus("配准点对引用了被删除的实体——已清空，请重新拾取");
        }
        UpdateCloudInfoText();
        Draw();
        CloudsChanged?.Invoke();
        return true;
    }

    /// <summary>删除核心（无 Undo 压栈——Undo/Redo 应用路径专用）。</summary>
    private void RemoveCloudCore(string id)
    {
        if (!_clouds.Remove(id)) return;
        _renderer.RemoveEntity(id);
        if (_pickedCloud == id) { _pickedCloud = null; _pickedIndex = -1; UpdatePickMark(); }
        if (_registMode && _registPairs.Any(p => p.Moving.Cloud == id || p.Ref.Cloud == id))
        {
            _registPairs.Clear();
            _registPending = null;
            RebuildRegistLayer();
        }
        if (_workingEntity == id) _workingEntity = null;
        UpdateCloudInfoText();
    }

    /// <summary>锁定/解锁点云实体（锁定=不可删除/改名、切数据源保留；数据树锁徽标切换）。</summary>
    public void SetCloudLocked(string id, bool locked)
    {
        if (!_clouds.TryGetValue(id, out var c)) return;
        c.Locked = locked;
        CloudsChanged?.Invoke();
    }

    /// <summary>数据树提示通道（树自身无状态栏，借工作台上下文反馈）。</summary>
    public void TreeStatus(string msg) => _ctx?.SetStatus(msg);

    public void SetCloudVisible(string id, bool visible)
    {
        _renderer.SetVisible(id, visible);
        Draw();
        CloudsChanged?.Invoke();
    }

    /// <summary>重命名点云实体（同名/空名/锁定拒绝）。成功后拾取引用同步换 Id。</summary>
    public bool RenameCloud(string oldId, string newId)
    {
        newId = newId?.Trim() ?? "";
        if (oldId == newId || string.IsNullOrEmpty(newId) || _clouds.ContainsKey(newId)) return false;
        if (!_clouds.TryGetValue(oldId, out var old)) return false;
        if (old.Locked) { _ctx?.SetStatus($"「{oldId}」已锁定，先解锁再改名"); return false; }
        _clouds.Remove(oldId);
        old.Id = newId;
        _clouds[newId] = old;
        _renderer.RenameEntity(oldId, newId);
        if (_pickedCloud == oldId) _pickedCloud = newId;
        if (_workingEntity == oldId) _workingEntity = newId;
        foreach (var m in _measurements)   // 量测/配对引用同步改名
            for (int i = 0; i < m.Points.Count; i++)
                if (m.Points[i].Cloud == oldId) m.Points[i] = (newId, m.Points[i].Index);
        for (int i = 0; i < _registPairs.Count; i++)
        {
            var (mc, mi) = _registPairs[i].Moving;
            var (rc, ri) = _registPairs[i].Ref;
            if (mc == oldId) mc = newId;
            if (rc == oldId) rc = newId;
            _registPairs[i] = ((mc, mi), (rc, ri));
        }
        UpdateCloudInfoText();
        Draw();
        CloudsChanged?.Invoke();
        return true;
    }

    public IReadOnlyList<CloudInfo> GetCloudInfos() => _clouds.Values
        .Select(c => new CloudInfo(c.Id, c.Count, c.Loaded, _renderer.IsVisible(c.Id), c.SelCount,
            c.BoundsMin, c.BoundsMax, c.Source, c.ColorMode, c.PointSize, c.SingleColor, (double[])c.Xform.Clone(), c.Locked))
        .ToList();

    /// <summary>复制点云实体（CloudCompare 式克隆：数据与样式一致，新 Id 后缀 -副本）。</summary>
    /// <summary>复制点云实体，返回新实体 Id（源不存在返回 null）。</summary>
    public string? DuplicateCloud(string id)
    {
        if (!_clouds.TryGetValue(id, out var src)) return null;
        string newId = UniqueCopyId(id);
        var data = new CloudData
        {
            Id = newId,
            Pts = (float[])src.Pts.Clone(),
            Col = src.Col is null ? null : (byte[])src.Col.Clone(),
            Sel = new byte[src.Count],
            Count = src.Count,
            Loaded = 0,
            SelCount = 0,
            BoundsMin = src.BoundsMin,
            BoundsMax = src.BoundsMax,
            Source = src.Source,
            ColorMode = src.ColorMode,
            PointSize = src.PointSize,
            SingleColor = src.SingleColor,                // 复制单色（此前丢失：副本落到默认色）
        };
        data.Xform = (double[])src.Xform.Clone();         // 复制变换矩阵（此前丢失：副本落回单位阵）
        _clouds[newId] = data;
        _renderer.BeginUpload(newId, data.Count, data.BoundsMin, data.BoundsMax, data.Pts, data.Col);
        _renderer.SetEntityStyle(newId, data.ColorMode, data.PointSize, data.SingleColor);
        _renderer.SetEntityTransform(newId, data.Xform);
        _uploadTimer.Stop();
        _uploadTimer.Start();
        UpdateCloudInfoText();
        CloudsChanged?.Invoke();
        return newId;
    }

    private string UniqueCopyId(string id)
    {
        string first = id + "-副本";
        if (!_clouds.ContainsKey(first)) return first;
        for (int k = 2; k < 1000; k++)
        {
            string s = first + k;
            if (!_clouds.ContainsKey(s)) return s;
        }
        return id + "-副本-" + Guid.NewGuid().ToString("N").Substring(0, 6);
    }

    /// <summary>设置单个实体的独立样式（点云树"每点云自己的属性"）。</summary>
    public void SetCloudStyle(string id, int? colorMode, float? pointSize, Vector3? singleColor = null)
    {
        if (!_clouds.TryGetValue(id, out var c)) return;
        if (colorMode is { } m) c.ColorMode = m;
        if (pointSize is { } ps) c.PointSize = ps;
        if (singleColor is { } sc) c.SingleColor = sc;
        _renderer.SetEntityStyle(id, c.ColorMode, c.PointSize, c.SingleColor);
        Draw();
    }

    /// <summary>设置实体变换矩阵（x = UI 行主序 16 元素，末列平移）。</summary>
    public void SetCloudXform(string id, double[] x)
    {
        if (!_clouds.TryGetValue(id, out var c) || x.Length < 16) return;
        c.Xform = (double[])x.Clone();
        _renderer.SetEntityTransform(id, c.Xform);
        Draw();
        CloudsChanged?.Invoke();
    }

    private void UpdateCloudInfoText()
    {
        if (_clouds.Count == 0) { _cloudInfo.Text = "无"; return; }
        long total = _clouds.Values.Sum(c => c.Count);
        var text = $"{_clouds.Count} 实体 · 共 {total:N0} 点" +
                   (_clouds.Values.Any(c => c.Loaded < c.Count) ? "（上传中）" : "");

        // 世界包围盒三轴尺寸（跨实体合并）+ 密度（点数/包围盒体积）
        Vector3 wmin = new(float.MaxValue), wmax = new(float.MinValue);
        foreach (var c in _clouds.Values)
        {
            for (int i = 0; i < 8; i++)
            {
                var lp = new Vector3((i & 1) == 0 ? c.BoundsMin.X : c.BoundsMax.X,
                                     (i & 2) == 0 ? c.BoundsMin.Y : c.BoundsMax.Y,
                                     (i & 4) == 0 ? c.BoundsMin.Z : c.BoundsMax.Z);
                var w = new Vector3(
                    (float)(c.Xform[0] * lp.X + c.Xform[1] * lp.Y + c.Xform[2] * lp.Z + c.Xform[3]),
                    (float)(c.Xform[4] * lp.X + c.Xform[5] * lp.Y + c.Xform[6] * lp.Z + c.Xform[7]),
                    (float)(c.Xform[8] * lp.X + c.Xform[9] * lp.Y + c.Xform[10] * lp.Z + c.Xform[11]));
                wmin = Vector3.ComponentMin(wmin, w);
                wmax = Vector3.ComponentMax(wmax, w);
            }
        }
        var size = wmax - wmin;
        if (size.X > 0 && size.Y > 0 && size.Z > 0)
        {
            double vol = (double)size.X * size.Y * size.Z;
            var dens = vol > 0 ? total / vol : 0;
            var densText = dens >= 0.01 ? dens.ToString("N1", CultureInfo.InvariantCulture) : "—";
            text += $"  包围盒 {size.X:F1}×{size.Y:F1}×{size.Z:F1} · 密度 {densText} 点/unit³";
        }
        long sel = _clouds.Values.Sum(c => (long)c.SelCount);
        if (sel > 0) text += $"  选中 {sel:N0} 点";
        _cloudInfo.Text = text;
    }

    /// <summary>有效抽稀间隔 = 用户设置 与 交互上限（可见点>500 万时交互自动抽稀）取大者。</summary>
    private int EffStride
    {
        get
        {
            int s = _stride;
            long visible = _renderer.VisiblePoints;
            if (_interacting && visible > InteractCap)
                s = Math.Max(s, (int)Math.Ceiling(visible / (double)InteractCap));
            return Math.Clamp(s, 1, 1000);
        }
    }

    /// <summary>标记相机交互开始：进入交互抽稀，静止 0.2s 后由空闲计时器补画全量。</summary>
    private void MarkInteract()
    {
        if (_renderer.VisiblePoints <= InteractCap) return;
        _interacting = true;
        _idleTimer?.Stop();
        _idleTimer?.Start();
    }

    /// <summary>视角预设：等轴/前/后/左/右/上/下（只改朝向，不动距离与目标点）。</summary>
    private void ApplyPreset(int idx)
    {
        if (!HasCloud) return;
        MarkInteract();
        _orientation = idx switch
        {
            1 => ViewQuat(0, 0),        // 前：相机在 +Z
            2 => ViewQuat(180, 0),      // 后
            3 => ViewQuat(-90, 0),      // 左：相机在 -X
            4 => ViewQuat(90, 0),       // 右：相机在 +X
            5 => ViewQuat(0, -90),      // 上：俯视
            6 => ViewQuat(0, 90),       // 下：仰视
            _ => ViewQuat(45, -35.26f), // 等轴
        };
        Draw();
    }

    /// <summary>先绕 X 俯仰、再绕 Y 偏航得到的相机姿态（与 ResetView 同一构造方式）。</summary>
    private static Quaternion ViewQuat(double yawDeg, double pitchDeg) => Quaternion.Normalize(
        Quaternion.FromAxisAngle(Vector3.UnitY, MathHelper.DegreesToRadians((float)yawDeg)) *
        Quaternion.FromAxisAngle(Vector3.UnitX, MathHelper.DegreesToRadians((float)pitchDeg)));

    private void ResetView()
    {
        _orientation = ViewQuat(30, -15);
        _dist = _fitRadius * 2.5f;
        _travel = 0;   // 回到全景起点（未穿行）
        Draw();
    }

    // ==================== 交互 ====================

    private void OnWheel(object? sender, PointerWheelEventArgs e)
    {
        if (!HasCloud) return;
        if (_polyMode) { e.Handled = true; return; }   // 多边形选取锁定视角（屏幕顶点随缩放失效）
        if (e.KeyModifiers.HasFlag(KeyModifiers.Control))
        {
            // Ctrl+滚轮：全部可见实体点大小 ±0.5（快捷键 +/- 的鼠标替代；穿行语义不动）
            MarkInteract();
            AdjustPointSize(e.Delta.Y > 0 ? +0.5 : -0.5);
            e.Handled = true;
            return;
        }
        MarkInteract();
        // 滚轮 = 沿视线穿行。上滚前进（可越过中心、穿过前面点云继续靠近背面点），下滚后退。
        // 步长取场景半径的比例，让不同尺度的点云手感一致；下界 -半径*1.5 允许略微后退到全景之外。
        // 不设上界：可一路穿过整个点云。_dist 固定为初始适配基准，点大小由真实相机距离决定，
        // 因此越靠近的点在窗口中越大。
        double step = Math.Max(_fitRadius * 0.12, 1e-4);
        _travel += e.Delta.Y > 0 ? step : -step;
        _travel = Math.Max(_travel, -_fitRadius * 1.5);
        Draw();
        e.Handled = true;
    }

    private void OnPress(object? sender, PointerPressedEventArgs e)
    {
        var pt = e.GetCurrentPoint(_viewArea);
        Focus();   // 让 Esc 清选区生效
        if (_polyMode)
        {
            // 多边形选取模式：左键加点、右键闭合——绘制期视角完全锁定（旋转/平移/缩放均不生效，
            // 顶点是屏幕坐标，相机一动投影就失效），Esc 取消、Enter 闭合
            if (pt.Properties.IsLeftButtonPressed) PolyAddPoint(pt.Position);
            else if (pt.Properties.IsRightButtonPressed) ApplyPolygonSelection();
            e.Handled = true;
            return;
        }
        if (_measureMode && pt.Properties.IsRightButtonPressed)
        {
            if (_activeMeasure is not null) { _activeMeasure = null; _ctx?.SetStatus("折线已结束——继续拾取开始新折线"); }
            e.Handled = true;
            return;
        }
        if (_registMode)
        {
            // 配准模式：普通左键单击=拾取（免 Ctrl，配准要点很多）；右键=应用 ≥3 对
            if (pt.Properties.IsRightButtonPressed) { ApplyRegistPairs(); e.Handled = true; return; }
            if (pt.Properties.IsLeftButtonPressed) { PickAt(pt.Position); e.Handled = true; return; }
            return;
        }
        if (pt.Properties.IsLeftButtonPressed && e.KeyModifiers.HasFlag(KeyModifiers.Control))
        {
            _pickStart = pt.Position;
            _orbitLast = null; _panLast = null;
        }
        else if (pt.Properties.IsLeftButtonPressed) { _orbitLast = pt.Position; _panLast = null; }
        else if (pt.Properties.IsRightButtonPressed) { _panLast = pt.Position; _orbitLast = null; }
        e.Pointer.Capture(_viewArea);
        e.Handled = true;
    }

    private void OnMove(object? sender, PointerEventArgs e)
    {
        var pos = e.GetPosition(_viewArea);
        var pt = e.GetCurrentPoint(_viewArea);
        if (_orbitLast is { } o && pt.Properties.IsLeftButtonPressed)
        {
            MarkInteract();
            double dxDeg = -(pos.X - o.X) * 0.35;
            double dyDeg = -(pos.Y - o.Y) * 0.35;   // 拖下 → 从下方看（与拖拽方向一致的 grab 手感）
            var qYaw = Quaternion.FromAxisAngle(Vector3.UnitY, MathHelper.DegreesToRadians((float)dxDeg));
            if (_lockPitch)
            {
                _orientation = Quaternion.Normalize(qYaw * _orientation);   // 锁定竖直轴：只水平旋转
            }
            else
            {
                var right = Vector3.Transform(Vector3.UnitX, _orientation);
                var qPitch = Quaternion.FromAxisAngle(right, MathHelper.DegreesToRadians((float)dyDeg));
                _orientation = Quaternion.Normalize(qPitch * qYaw * _orientation);
            }
            _orbitLast = pos;
            Draw();
        }
        else if (_panLast is { } p && pt.Properties.IsRightButtonPressed)
        {
            MarkInteract();
            double dx = (pos.X - p.X) * _scaling;
            double dy = (pos.Y - p.Y) * _scaling;
            // 相机自身 right/up：含 roll 也始终与屏幕对齐，视线过极点时不退化
            var right = Vector3.Transform(Vector3.UnitX, _orientation);
            var up = Vector3.Transform(Vector3.UnitY, _orientation);
            double s = _dist * 0.0016;
            // grab 语义（相机反向移动）：拖右 → 云右；拖下 → 云下
            _target += (-right * (float)dx + up * (float)dy) * (float)s;
            _panLast = pos;
            Draw();
        }
        else if (_pickStart is { } s && pt.Properties.IsLeftButtonPressed)
        {
            UpdateRubber(s, pos);
        }
    }

    private void OnRelease(object? sender, PointerReleasedEventArgs e)
    {
        if (_pickStart is { } s)
        {
            var pos = e.GetPosition(_viewArea);
            _rubber.IsVisible = false;
            if (Math.Abs(pos.X - s.X) + Math.Abs(pos.Y - s.Y) < 4) PickAt(s);
            else BoxSelect(s, pos);
            _pickStart = null;
        }
        _orbitLast = null; _panLast = null;
        e.Pointer.Capture(null);
        e.Handled = true;
    }

    // ==================== 拾取 / 框选 ====================

    /// <summary>与顶点着色器完全一致的投影（先过实体变换矩阵再过 MVP；OpenTK 行主序上传后
    /// GL 按列主序解释，按分量展开避免约定歧义）。返回归一化屏幕坐标（y 向下）与 NDC 深度；
    /// 相机后方时 x 为 NaN。</summary>
    private static (double sx, double sy, float z) Project(float[] pts, double[] xf, Matrix4 m, int i)
    {
        float x = pts[i * 3], y = pts[i * 3 + 1], z = pts[i * 3 + 2];
        float mx = (float)(xf[0] * x + xf[1] * y + xf[2] * z + xf[3]);
        float my = (float)(xf[4] * x + xf[5] * y + xf[6] * z + xf[7]);
        float mz = (float)(xf[8] * x + xf[9] * y + xf[10] * z + xf[11]);
        float cx = m.M11 * mx + m.M21 * my + m.M31 * mz + m.M41;
        float cy = m.M12 * mx + m.M22 * my + m.M32 * mz + m.M42;
        float cz = m.M13 * mx + m.M23 * my + m.M33 * mz + m.M43;
        float cw = m.M14 * mx + m.M24 * my + m.M34 * mz + m.M44;
        if (cw <= 1e-6f) return (double.NaN, double.NaN, 0);
        return ((cx / cw + 1) * 0.5, (0.5 - cy / cw * 0.5), cz / cw);
    }

    private void PickAt(Point p)
    {
        if (!HasCloud || _vpW <= 0) return;
        double mx = p.X * _scaling, my = p.Y * _scaling;
        double r2 = 144 * _scaling * _scaling;   // 12 DIP 拾取半径
        var m = _renderer.LastMvp;
        int stride = EffStride;
        int best = -1;
        string bestCloud = "";
        float bestZ = float.MaxValue;
        foreach (var c in _clouds.Values)
        {
            if (!_renderer.IsVisible(c.Id)) continue;
            for (int i = 0; i < c.Loaded; i += stride)
            {
                var (sx, sy, z) = Project(c.Pts, c.Xform, m, i);
                if (double.IsNaN(sx)) continue;
                double dx = sx * _vpW - mx, dy = sy * _vpH - my;
                if (dx * dx + dy * dy <= r2 && z < bestZ) { bestZ = z; best = i; bestCloud = c.Id; }
            }
        }
        _pickedCloud = best >= 0 ? bestCloud : null;
        _pickedIndex = best;
        if (best >= 0)
        {
            var c = _clouds[bestCloud];
            float px = c.Pts[best * 3], py = c.Pts[best * 3 + 1], pz = c.Pts[best * 3 + 2];
            // 变换后世界坐标（渲染即所见）
            float wx = (float)(c.Xform[0] * px + c.Xform[1] * py + c.Xform[2] * pz + c.Xform[3]);
            float wy = (float)(c.Xform[4] * px + c.Xform[5] * py + c.Xform[6] * pz + c.Xform[7]);
            float wz = (float)(c.Xform[8] * px + c.Xform[9] * py + c.Xform[10] * pz + c.Xform[11]);
            _pickInfo.Text = $"[{bestCloud}] #{best}  ({wx:F3}, {wy:F3}, {wz:F3})";

            // 归一化 uv（X→u, Z→v，Y 为高度）：关联数据（高度图等）据此映射到图像像素
            double[]? uv = null;
            double sxSpan = c.BoundsMax.X - c.BoundsMin.X, szSpan = c.BoundsMax.Z - c.BoundsMin.Z;
            if (sxSpan > 1e-9 && szSpan > 1e-9)
                uv = new[] { (px - c.BoundsMin.X) / sxSpan, (pz - c.BoundsMin.Z) / szSpan };
            _bus.Publish(SelectionPayload.TopicPoint3D, new SelectionPayload(
                SelectionKind.Point3D, SelfId,
                new double[] { wx, wy, wz },
                null, $"[{bestCloud}] #{best}", null, uv));
        }
        else
        {
            _pickInfo.Text = "无（Ctrl+左键单击拾取）";
        }
        if (best >= 0 && _measureMode) MeasureCapture(bestCloud, best);   // 量测模式：拾取点进入折线流水
        if (best >= 0 && _registMode) RegistCapture(bestCloud, best);     // 配准模式：拾取点进入配对流水
        if (best >= 0)
        {
            var wc = WorldOf(bestCloud, best);
            if (wc is { } w) AppendPickHistory(w.X, w.Y, w.Z);
        }
        Draw();
    }

    private void BoxSelect(Point a, Point b)
    {
        if (!HasCloud || _vpW <= 0) return;
        double x0 = Math.Min(a.X, b.X) * _scaling, x1 = Math.Max(a.X, b.X) * _scaling;
        double y0 = Math.Min(a.Y, b.Y) * _scaling, y1 = Math.Max(a.Y, b.Y) * _scaling;
        var m = _renderer.LastMvp;
        int stride = EffStride;
        int total = 0;
        var aggMin = new Vector3(float.MaxValue);
        var aggMax = new Vector3(float.MinValue);
        var aggC = Vector3.Zero;
        var parts = new List<string>();
        foreach (var c in _clouds.Values)
        {
            if (!_renderer.IsVisible(c.Id)) { Array.Clear(c.Sel); continue; }
            var min = new Vector3(float.MaxValue);
            var max = new Vector3(float.MinValue);
            var ctr = Vector3.Zero;
            int n = 0;
            for (int i = 0; i < c.Loaded; i += stride)
            {
                var (sx, sy, z) = Project(c.Pts, c.Xform, m, i);
                if (!double.IsNaN(sx) && sx * _vpW >= x0 && sx * _vpW <= x1 && sy * _vpH >= y0 && sy * _vpH <= y1)
                {
                    c.Sel[i] = 255;   // 归一化 ubyte 属性：255 才映射到 1.0
                    n++;
                    var v = new Vector3(
                        (float)(c.Xform[0] * c.Pts[i * 3] + c.Xform[1] * c.Pts[i * 3 + 1] + c.Xform[2] * c.Pts[i * 3 + 2] + c.Xform[3]),
                        (float)(c.Xform[4] * c.Pts[i * 3] + c.Xform[5] * c.Pts[i * 3 + 1] + c.Xform[6] * c.Pts[i * 3 + 2] + c.Xform[7]),
                        (float)(c.Xform[8] * c.Pts[i * 3] + c.Xform[9] * c.Pts[i * 3 + 1] + c.Xform[10] * c.Pts[i * 3 + 2] + c.Xform[11]));
                    min = Vector3.ComponentMin(min, v);
                    max = Vector3.ComponentMax(max, v);
                    ctr += v;
                }
                else c.Sel[i] = 0;
            }
            c.SelCount = n;
            _renderer.SetSelection(c.Id, c.Sel);
            if (n > 0)
            {
                total += n;
                aggMin = Vector3.ComponentMin(aggMin, min);
                aggMax = Vector3.ComponentMax(aggMax, max);
                aggC += ctr;
                parts.Add($"{c.Id} {n:N0}");
            }
            if (n > 0)
            {
                ctr /= n;
                _bus.Publish(SelectionPayload.TopicCloudBox, new SelectionPayload(
                    SelectionKind.CloudBox, SelfId, null, null,
                    $"[{c.Id}] {n:N0} 点",
                    new double[] { min.X, min.Y, min.Z, max.X, max.Y, max.Z }));
            }
        }
        if (total > 0)
        {
            aggC /= total;
            _lastSelBounds = (aggMin.X, aggMin.Y, aggMin.Z, aggMax.X - aggMin.X, aggMax.Y - aggMin.Y);   // ROI 类工具参数预填源
            _selInfo.Text = $"共 {total:N0} 点（{string.Join(" · ", parts)}）\n" +
                            $"质心 ({aggC.X:F3}, {aggC.Y:F3}, {aggC.Z:F3}) 范围 X[{aggMin.X:F3}, {aggMax.X:F3}] Y[{aggMin.Y:F3}, {aggMax.Y:F3}] Z[{aggMin.Z:F3}, {aggMax.Z:F3}]";
        }
        else _selInfo.Text = "框选 0 点（范围内无点）";
        Draw();
    }

    private void ClearSelection()
    {
        foreach (var c in _clouds.Values)
        {
            if (c.SelCount == 0) continue;
            Array.Clear(c.Sel);
            c.SelCount = 0;
            _renderer.SetSelection(c.Id, c.Sel);
        }
        _selInfo.Text = "无（Ctrl+左键拖拽框选）";
    }

    private void UpdateRubber(Point a, Point b)
    {
        _rubber.IsVisible = true;
        _rubber.Width = Math.Abs(b.X - a.X);
        _rubber.Height = Math.Abs(b.Y - a.Y);
        Canvas.SetLeft(_rubber, Math.Min(a.X, b.X));
        Canvas.SetTop(_rubber, Math.Min(a.Y, b.Y));
    }

    private void UpdatePickMark()
    {
        if (_pickedIndex < 0 || _pickedCloud is null || !_clouds.TryGetValue(_pickedCloud, out var c) ||
            _pickedIndex >= c.Count)   // 同 Id 重发更小点云后旧索引越界——清引用防崩溃
        {
            _pickedCloud = null;
            _pickedIndex = -1;
            _pickMark.IsVisible = false;
            return;
        }
        var (sx, sy, z) = Project(c.Pts, c.Xform, _renderer.LastMvp, _pickedIndex);
        if (double.IsNaN(sx) || sx < 0 || sx > 1 || sy < 0 || sy > 1)
        {
            _pickMark.IsVisible = false;
            return;
        }
        _pickMark.IsVisible = true;
        Canvas.SetLeft(_pickMark, sx * _vpW / _scaling - 5.5);
        Canvas.SetTop(_pickMark, sy * _vpH / _scaling - 5.5);
    }

    private void UpdateTripod()
    {
        var camRight = Vector3.Transform(Vector3.UnitX, _orientation);
        var camUp = Vector3.Transform(Vector3.UnitY, _orientation);
        UpdateAxis(_axX, _axXl, Vector3.UnitX, camRight, camUp);
        UpdateAxis(_axY, _axYl, Vector3.UnitY, camRight, camUp);
        UpdateAxis(_axZ, _axZl, Vector3.UnitZ, camRight, camUp);
    }

    private static void UpdateAxis(Line line, TextBlock label, Vector3 axis, Vector3 camRight, Vector3 camUp)
    {
        double dx = Vector3.Dot(axis, camRight);
        double dy = -Vector3.Dot(axis, camUp);   // 屏幕 y 向下
        line.StartPoint = new Point(36, 36);
        line.EndPoint = new Point(36 + dx * 25, 36 + dy * 25);
        Canvas.SetLeft(label, 36 + dx * 33 - 4);
        Canvas.SetTop(label, 36 + dy * 33 - 7);
    }

    // ==================== 工具：提取/剔除/量测/算子 ====================

    private void ToggleMeasure()
    {
        if (!_measureMode)
        {
            if (_polyMode) EndPolyMode();       // 模式互斥：开量测先收多边形
            if (_registMode) EndRegistMode();   // 开量测先收配准
        }
        _measureMode = !_measureMode;
        if (_measureBtn is not null)
        {
            if (_measureMode)
                _measureBtn.Background = ActiveTintYellow;
            else
                _measureBtn.ClearValue(Button.BackgroundProperty);   // 置 null 会压样式（BUG-083 同族）
        }
        _measureFirst = null;
        UpdateModeCursor();
        _ctx?.SetStatus(_measureMode
            ? "距离量测：Ctrl+左键拾取第一点（再拾取第二点成线）"
            : "距离量测已关闭");
    }

    private void TogglePoly()
    {
        if (_polyMode) { EndPolyMode(); return; }
        if (_measureMode) ToggleMeasure();
        if (_registMode) ToggleRegist();
        _polyMode = true;
        _polyPts.Clear();
        _polyPreview.Points.Clear();
        _polyPreview.IsVisible = false;
        if (_polyBtn is not null) _polyBtn.Background = ActiveTintPurple;
        UpdateModeCursor();
        _ctx?.SetStatus("多边形选取：左键加点（≥3 个），右键/Enter 闭合应用选区；Esc 取消");
    }

    private void EndPolyMode()
    {
        _polyMode = false;
        _polyPts.Clear();
        _polyPreview.IsVisible = false;
        _polyPreview.Points.Clear();
        if (_polyBtn is not null) _polyBtn.ClearValue(Button.BackgroundProperty);
        UpdateModeCursor();
        Draw();
    }

    private void PolyAddPoint(Point p)
    {
        _polyPts.Add(p);
        _polyPreview.IsVisible = true;
        _polyPreview.Points.Add(p);
        Draw();
    }

    /// <summary>多边形闭合：对可见实体逐点屏幕投影 → 射线法判定 → Sel 标记（与框选同一后处理：
    /// 高亮渲染 + 聚合信息 + CloudBox 广播 + _lastSelBounds 记录），随后 ⧉/✂ 即可提取/剔除。</summary>
    private void ApplyPolygonSelection()
    {
        if (_polyPts.Count < 3)
        {
            _ctx?.SetStatus("多边形至少 3 个顶点（继续左键加点或 Esc 取消）");
            return;
        }
        var poly = _polyPts.Select(p => (p.X, p.Y)).ToList();
        var m = _renderer.LastMvp;
        int stride = EffStride;
        int total = 0;
        var aggMin = new Vector3(float.MaxValue);
        var aggMax = new Vector3(float.MinValue);
        var aggC = Vector3.Zero;
        var parts = new List<string>();
        foreach (var c in _clouds.Values)
        {
            if (!_renderer.IsVisible(c.Id)) { Array.Clear(c.Sel); continue; }
            var min = new Vector3(float.MaxValue);
            var max = new Vector3(float.MinValue);
            var ctr = Vector3.Zero;
            int n = 0;
            for (int i = 0; i < c.Loaded; i += stride)
            {
                var (sx, sy, _) = Project(c.Pts, c.Xform, m, i);
                bool hit = !double.IsNaN(sx) &&
                           CloudToolOps.PointInPolygon(poly, sx * _vpW / _scaling, sy * _vpH / _scaling);
                if (hit)
                {
                    c.Sel[i] = 255;
                    n++;
                    var v = new Vector3(
                        (float)(c.Xform[0] * c.Pts[i * 3] + c.Xform[1] * c.Pts[i * 3 + 1] + c.Xform[2] * c.Pts[i * 3 + 2] + c.Xform[3]),
                        (float)(c.Xform[4] * c.Pts[i * 3] + c.Xform[5] * c.Pts[i * 3 + 1] + c.Xform[6] * c.Pts[i * 3 + 2] + c.Xform[7]),
                        (float)(c.Xform[8] * c.Pts[i * 3] + c.Xform[9] * c.Pts[i * 3 + 1] + c.Xform[10] * c.Pts[i * 3 + 2] + c.Xform[11]));
                    min = Vector3.ComponentMin(min, v);
                    max = Vector3.ComponentMax(max, v);
                    ctr += v;
                }
                else c.Sel[i] = 0;
            }
            c.SelCount = n;
            _renderer.SetSelection(c.Id, c.Sel);
            if (n > 0)
            {
                total += n;
                aggMin = Vector3.ComponentMin(aggMin, min);
                aggMax = Vector3.ComponentMax(aggMax, max);
                aggC += ctr;
                parts.Add($"{c.Id} {n:N0}");
                ctr /= n;
                _bus.Publish(SelectionPayload.TopicCloudBox, new SelectionPayload(
                    SelectionKind.CloudBox, SelfId, null, null,
                    $"[{c.Id}] 多边形 {n:N0} 点",
                    new double[] { min.X, min.Y, min.Z, max.X, max.Y, max.Z }));
            }
        }
        if (total > 0)
        {
            aggC /= total;
            _lastSelBounds = (aggMin.X, aggMin.Y, aggMin.Z, aggMax.X - aggMin.X, aggMax.Y - aggMin.Y);
            _selInfo.Text = $"多边形选区 共 {total:N0} 点（{string.Join(" · ", parts)}）——⧉ 提取 / ✂ 剔除";
        }
        else _selInfo.Text = "多边形选区 0 点";
        EndPolyMode();
        _ctx?.SetStatus($"多边形选区应用：{total:N0} 点（⧉ 提取 / ✂ 剔除）");
        Draw();
    }

    // ==================== 工具条分组与拖拽排序 ====================

    /// <summary>组定义：名称 → 按钮描述（图标/悬浮提示/点击）。toggle 按钮的字段赋值在工厂内（重建后引用刷新）。</summary>
    private System.Collections.Generic.Dictionary<string, List<(string Icon, string Tip, Action Click)>> GroupFactories() => new()
    {
        ["视图"] = new()
        {
            ("focus", "聚焦选中实体：把视角对准当前实体（有选区者优先）——多实体场景逐个查看", FocusSelectedTool),
            ("lockpitch", "锁定竖直轴旋转：开关——只绕世界竖直轴水平旋转（俯视巡检，防止翻滚）", ToggleLockPitch),
        },
        ["选取"] = new()
        {
            ("extract", "提取框内：把框选选中的点集生成为新点云实体（按当前抽稀间隔采样）", () => ExtractSelected(keepInside: true)),
            ("crop", "剔除框内：把框选未选中的点集（框外）生成为新点云实体（原实体保留）", () => ExtractSelected(keepInside: false)),
            ("boxcrop", "盒裁剪：轴对齐包围盒（实体局部坐标，预填当前包围盒）保留盒内/盒外点为新实体", BoxCropTool),
            ("polygon", "多边形选取：左键依次加点、右键/Enter 闭合——选区内点高亮，之后 提取 / 剔除；Esc 取消当前多边形", TogglePoly),
        },
        ["量测"] = new()
        {
            ("measure", "距离量测：开关模式——开启后 Ctrl+左键连续拾取：两点=距离，继续拾取=折线累段；右键/Enter 结束当前折线；清除按钮删全部", ToggleMeasure),
            ("copy", "复制量测清单：把全部量测（折线点数/总长/逐段/首末差）以文本复制到剪贴板（报表粘贴用）", CopyMeasurements),
            ("clear", "清除全部量测线", () => { _measurements.Clear(); _activeMeasure = null; _measureFirst = null; RebuildMeasureLayer(); UpdateMeasureInfo(); Draw(); }),
        },
        ["配准"] = new()
        {
            ("regist", "点对配准：在两片点云上交替拾取对应点对（≥3 对）→ 刚体粗配准（RMS 进状态栏）→ 可再跑 ⚙「手动配准(ICP)」精配；左键拾取、右键/Enter 应用、Esc 退出", ToggleRegist),
            ("match", "质心对齐：把移动实体平移到基准实体质心重合（粗配准第一步；配对点拾取/ICP 前用）", MatchCentersTool),
        },
        ["处理"] = new()
        {
            ("random", "随机下采样：按比例均匀随机保留点（新实体；大数据先抽稀再做去噪/对比）", RandomSampleTool),
            ("outlier", "离群点滤波：半径邻域计数不足的点剔除（新实体=内点；悬浮提示含调参建议）", OutlierTool),
            ("clip", "平面分割：RANSAC 提取最大平面——平面点集/残差点集各成新实体（悬浮提示含调参建议）", RansacTool),
            ("merge", "合并实体：把全部可见已加载实体按世界坐标合并为一个新实体", MergeTool),
            ("transform", "变换工具：平移/三轴旋转/指定轴缩放/镜像（叠加到实体当前变换，数据树可看 4×4）", TransformTool),
            ("normal", "法线估计+光照：半径邻域 PCA 逐点法线 → Lambert 光照显示（立体感；再点关闭光照）", NormalsTool),
            ("heightmap", "高度图：点云按 XY 网格聚合高度 → jet 色带图发布到 2D 工作台联动显示", HeightMapTool),
            ("voxel", "体素抽稀：确定性每体素取首点（本地处理；与算子库质心版语义不同）", VoxelSampleTool),
        },
        ["文件"] = new()
        {
            ("import", "导入点云：PLY（ascii/binary）或 XYZ/CSV 文本 → 新实体", ImportTool),
            ("export", "导出点云：当前实体保存为 PLY（含 RGB）或 XYZ/CSV 文件", ExportTool),
        },
        ["算子"] = new()
        {
            ("operator", "算子工具：直接调用算子库 Point3D 算子（体素抽稀/平面量测/圆孔量测/剖面量测/点云对比…）作用于当前实体", () => { }),
        },
    };

    /// <summary>按组序渲染工具条：快照固定首位 + 各组 Border（左缘拖拽把手 grip）+ 组间 hairline 分隔。
    /// 算子组的按钮用下拉菜单（BuildToolMenu）。</summary>
    private void BuildToolbar(StackPanel toolbar)
    {
        // 保留快照按钮（固定首位不参与分组拖动）；清掉旧组 Border 与分隔线
        var keep = toolbar.Children.Where(c => c is Button).ToList();
        toolbar.Children.Clear();
        foreach (var k in keep) toolbar.Children.Add(k);
        if (keep.Count > 0) toolbar.Children.Add(BuildToolSeparator());
        var factories = GroupFactories();
        var groups = _groupOrder.Where(factories.ContainsKey).Where(g => !_hiddenGroups.Contains(g)).ToList();

        for (int gi = 0; gi < groups.Count; gi++)
        {
            var name = groups[gi];
            if (gi > 0) toolbar.Children.Add(BuildToolSeparator());

            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
            var panel = new Border
            {
                Child = row,
                CornerRadius = new CornerRadius(6),
                Padding = new Thickness(2, 1),
                Tag = name,
            };
            panel.ContextMenu = BuildGroupContextMenu(name);

            // 拖拽把手（组头竖点 ⋮⋮）：拖动排序入口；ToolTip 带组名
            var grip = new Border
            {
                Width = 7,
                Margin = new Thickness(0, 2, 2, 2),
                Background = new SolidColorBrush(Color.FromArgb(0x30, 0xFF, 0xFF, 0xFF)),
                CornerRadius = new CornerRadius(2),
                Cursor = new Cursor(StandardCursorType.SizeAll),
            };
            ToolTip.SetTip(grip, $"「{name}」组——按住拖动可整组排序");
            grip.PointerPressed += (_, e) =>
            {
                if (!e.GetCurrentPoint(grip).Properties.IsLeftButtonPressed) return;
                _dragGroup = name;
                _dragStartIndex = _groupOrder.IndexOf(name);
                e.Pointer.Capture(toolbar);
                e.Handled = true;
            };
            row.Children.Add(grip);

            foreach (var (icon, tip, click) in factories[name])
            {
                if (icon == "operator")
                {
                    var b = new Button
                    {
                        Classes = { "toolBtn" },
                        Content = ToolIcons.Make("operator", 14),
                        Width = 28, Height = 26, Padding = new Thickness(0),
                        HorizontalContentAlignment = HorizontalAlignment.Center,
                        VerticalContentAlignment = VerticalAlignment.Center,
                        Opacity = 0.82,
                        Cursor = new Cursor(StandardCursorType.Hand),
                    };
                    b.PointerEntered += (_, _) => b.Opacity = 1.0;
                    b.PointerExited += (_, _) => b.Opacity = 0.82;
                    ToolTip.SetTip(b, tip);
                    b.Click += (_, _) => OpenOperatorMenu();
                    _toolOpsBtn = b;
                    row.Children.Add(b);
                    continue;
                }

                // ToolTip 追加快捷键标注（从 CloudHotkeys.Bindings 反查键位文本）
                var btnTip = tip;
                if (IconAction.TryGetValue(icon, out var act))
                {
                    var kb = Array.Find(CloudHotkeys.Bindings, b => b.Action == act);
                    if (kb.Keys is not null && tip.IndexOf('(') < 0) btnTip = $"{tip}（{kb.Keys}）";
                }
                var btn = BuildToolButton(icon, btnTip, click);
                // toggle 按钮重建后按当前模式恢复激活态底色
                var isActive = (btn == _measureBtn && _measureMode) ||
                               (btn == _polyBtn && _polyMode) ||
                               (btn == _registBtn && _registMode) ||
                               (btn == _lockPitchBtn && _lockPitch);
                // 字段刷新（工厂重建后引用必须指向新控件）
                if (icon == "measure") { _measureBtn = btn; isActive = _measureMode; }
                else if (icon == "polygon") { _polyBtn = btn; isActive = _polyMode; }
                else if (icon == "regist") { _registBtn = btn; isActive = _registMode; }
                else if (icon == "lockpitch") { _lockPitchBtn = btn; isActive = _lockPitch; }
                if (isActive)
                {
                    var tint = icon switch
                    {
                        "measure" => ActiveTintYellow,
                        "polygon" => ActiveTintPurple,
                        "regist" => ActiveTintMagenta,
                        "lockpitch" => ActiveTintBlue,
                        _ => (Brush?)null,
                    };
                    if (tint is not null) btn.Background = tint;
                }
                row.Children.Add(btn);
            }
            toolbar.Children.Add(panel);
        }
        toolbarSelf = toolbar;
    }

    // ===== 组拖拽状态机（容器级：grip 启动 → 移动>5px 拖组 → 实时重排预览 → 松手持久化） =====

    private void AttachToolbarDragHandlers(StackPanel toolbar)
    {
        toolbar.PointerMoved += (_, e) =>
        {
            if (_dragGroup is null) return;
            var pos = e.GetPosition(toolbar);
            // 实时重排预览：按指针 x 找落点组索引，把拖组 Border 挪过去
            var borders = toolbar.Children.OfType<Border>().Where(b => b.Tag is string).ToList();
            int from = borders.FindIndex(b => (string)b.Tag! == _dragGroup);
            if (from < 0) return;
            int to = from;
            for (int i = 0; i < borders.Count; i++)
            {
                if (i == from) continue;
                var c = borders[i].Bounds.Center.X;
                if (pos.X < c) { to = i; break; }
                to = i + (pos.X > c ? 1 : 0);
            }
            to = Math.Clamp(to, 0, borders.Count - 1);
            if (to != from)
            {
                var fromCtrl = borders[from];
                toolbar.Children.Remove(fromCtrl);
                // Remove 后重查目标 Border 的实际索引（分隔线穿插 filtered 与 Children 索引不一致）
                int idxTo = toolbar.Children.IndexOf(borders[to]);
                toolbar.Children.Insert(idxTo, fromCtrl);
            }
            e.Handled = true;
        };

        toolbar.PointerReleased += (_, e) =>
        {
            if (_dragGroup is null) return;
            // 视觉顺序 → 组序持久化
            var visual = toolbar.Children.OfType<Border>()
                          .Where(b => b.Tag is string).Select(b => (string)b.Tag!).ToList();
            _groupOrder.Clear();
            _groupOrder.AddRange(visual);
            SaveToolOrder();
            _dragGroup = null;
            _dragStartIndex = -1;
            _ctx?.SetStatus($"工具条顺序已保存（{_groupOrder.Count} 组）——「{string.Join(" · ", _groupOrder)}」");
            e.Handled = true;
        };

        toolbar.PointerCaptureLost += (_, _) => { _dragGroup = null; };
    }

    private void LoadToolOrder()
    {
        if (_orderPath is null || !File.Exists(_orderPath)) return;
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(_orderPath));
            if (doc.RootElement.TryGetProperty("Order", out var arr) && arr.ValueKind == System.Text.Json.JsonValueKind.Array)
            {
                var saved = arr.EnumerateArray().Select(x => x.GetString() ?? "").Where(x => x.Length > 0).ToList();
                // 已保存序在前（交集保持），未登记组按默认序续后
                var merged = saved.Where(_groupOrder.Contains).ToList();
                foreach (var g in _groupOrder)
                    if (!merged.Contains(g)) merged.Add(g);
                _groupOrder.Clear();
                _groupOrder.AddRange(merged);
            }
            if (doc.RootElement.TryGetProperty("Hidden", out var hid) && hid.ValueKind == System.Text.Json.JsonValueKind.Array)
                foreach (var h in hid.EnumerateArray())
                    if (h.GetString() is { Length: > 0 } hg) _hiddenGroups.Add(hg);
        }
        catch { }
    }

    private void SaveToolOrder()
    {
        if (_orderPath is null) return;
        try
        {
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(_orderPath)!);
            var json = System.Text.Json.JsonSerializer.Serialize(
                new System.Collections.Generic.Dictionary<string, object?>
                {
                    ["Order"] = _groupOrder,
                    ["Hidden"] = _hiddenGroups.ToList(),
                },
                new System.Text.Json.JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
            File.WriteAllText(_orderPath, json);
        }
        catch (Exception ex) { _ctx?.SetStatus($"工具条顺序保存失败：{ex.Message}"); }
    }

    /// <summary>组 Border 右键菜单：隐藏本组 / 重置默认顺序（显示全部+清 Hidden）。</summary>
    private ContextMenu BuildGroupContextMenu(string name)
    {
        var menu = new ContextMenu();
        var hide = new MenuItem { Header = $"隐藏「{name}」" };
        hide.Click += (_, _) =>
        {
            _hiddenGroups.Add(name);
            SaveToolOrder();
            BuildToolbar(toolbarSelf!);
            _ctx?.SetStatus($"已隐藏「{name}」组——右键其他组 → 重置默认顺序 可恢复");
        };
        menu.Items.Add(hide);
        var moveUp = new MenuItem { Header = "上移", IsEnabled = _groupOrder.IndexOf(name) > 0 };
        moveUp.Click += (_, _) =>
        {
            int i = _groupOrder.IndexOf(name);
            if (i <= 0) return;
            (_groupOrder[i - 1], _groupOrder[i]) = (_groupOrder[i], _groupOrder[i - 1]);
            SaveToolOrder();
            BuildToolbar(toolbarSelf!);
        };
        var moveDown = new MenuItem { Header = "下移", IsEnabled = _groupOrder.IndexOf(name) < _groupOrder.Count - 1 };
        moveDown.Click += (_, _) =>
        {
            int i = _groupOrder.IndexOf(name);
            if (i < 0 || i >= _groupOrder.Count - 1) return;
            (_groupOrder[i + 1], _groupOrder[i]) = (_groupOrder[i], _groupOrder[i + 1]);
            SaveToolOrder();
            BuildToolbar(toolbarSelf!);
        };
        menu.Items.Add(moveUp);
        menu.Items.Add(moveDown);
        var reset = new MenuItem { Header = "重置默认顺序并显示全部" };
        reset.Click += (_, _) =>
        {
            _hiddenGroups.Clear();
            _groupOrder.Clear();
            _groupOrder.AddRange(new[] { "视图", "选取", "量测", "配准", "处理", "文件", "算子" });
            SaveToolOrder();
            BuildToolbar(toolbarSelf!);
            _ctx?.SetStatus("工具条已重置为默认分组顺序（全部显示）");
        };
        menu.Items.Add(reset);
        return menu;
    }

    private StackPanel? toolbarSelf;   // 工具条容器引用（组右键菜单重建用）

    /// <summary>工具按钮（Apple 底座）：矢量线条图标 + 透明底座 + hover 高亮（伪类样式在 ctor 注册）。</summary>
    private Button BuildToolButton(string iconKey, string tip, Action click)
    {
        var b = new Button
        {
            Classes = { "ghost" },
            Content = ToolIcons.Make(iconKey, 14),
            Width = 28, Height = 26, Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        b.Opacity = 0.82;   // Apple 手感：默认微暗、hover 变亮（图标独立 Opacity，不与激活态底色冲突）
        b.PointerEntered += (_, _) => b.Opacity = 1.0;
        b.PointerExited += (_, _) => b.Opacity = 0.82;
        ToolTip.SetTip(b, tip);
        b.Click += (_, _) => click();
        return b;
    }

    private static Border BuildToolSeparator() => new()
    {
        Width = 1, Height = 16, Margin = new Thickness(5, 0),
        Background = new SolidColorBrush(Color.FromArgb(0x30, 0xFF, 0xFF, 0xFF)),
        VerticalAlignment = VerticalAlignment.Center,
    };

    /// <summary>按当前框选选区生成新点云实体：keepInside=true 取选中点（提取），false 取未选中点（剔除框内=保留框外）。
    /// 按 EffStride 采样语义（与框选一致）；每个来源实体各出一个新实体（Xform/样式随源复制，渲染即所见）。</summary>
    private void ExtractSelected(bool keepInside)
    {
        var sources = _clouds.Values.Where(c => c.SelCount > 0 && c.Loaded > 0).ToList();
        if (sources.Count == 0)
        {
            _ctx?.SetStatus("提取/剔除需要先框选：Ctrl+左键拖拽框选后重试");
            return;
        }

        var created = new List<string>();
        foreach (var c in sources)
        {
            int stride = EffStride;
            var keep = new List<int>();
            for (int i = 0; i < c.Loaded; i += stride)
                if ((c.Sel[i] == 255) == keepInside) keep.Add(i);
            if (keep.Count == 0) continue;

            var pts = new float[keep.Count * 3];
            for (int k = 0; k < keep.Count; k++)
            {
                pts[k * 3] = c.Pts[keep[k] * 3];
                pts[k * 3 + 1] = c.Pts[keep[k] * 3 + 1];
                pts[k * 3 + 2] = c.Pts[keep[k] * 3 + 2];
            }
            byte[]? col = null;
            if (c.Col is not null)
            {
                col = new byte[keep.Count * 3];
                for (int k = 0; k < keep.Count; k++)
                {
                    col[k * 3] = c.Col[keep[k] * 3];
                    col[k * 3 + 1] = c.Col[keep[k] * 3 + 1];
                    col[k * 3 + 2] = c.Col[keep[k] * 3 + 2];
                }
            }

            var id = $"{c.Id}·{(keepInside ? "提取" : "剔除")}#{_toolSeq++}";
            SetCloud(keep.Count, pts, col, "工具", id, keepCamera: true);   // 对比场景：不重置视角
            if (_clouds.TryGetValue(id, out var nu))
            {
                nu.Xform = (double[])c.Xform.Clone();          // 渲染即所见：子集沿用源实体变换
                nu.ColorMode = c.ColorMode;
                nu.PointSize = c.PointSize;
                nu.SingleColor = c.SingleColor;
                _renderer.SetEntityTransform(id, nu.Xform);
                _renderer.SetEntityStyle(id, nu.ColorMode, nu.PointSize, nu.SingleColor);
            }
            created.Add($"{id}({keep.Count:N0})");
        }

        ClearSelection();
        _ctx?.SetStatus(created.Count > 0
            ? $"{(keepInside ? "提取框内" : "剔除框内")}完成：{string.Join("、", created)}"
            : "选区内无可保留的点");
        Draw();
    }

    /// <summary>拾取成功后进入量测流水（_measureMode 时）：第一点挂起；第二点成折线；后续拾取续段（右键/Enter 结束当前折线）。</summary>
    private void MeasureCapture(string cloudId, int index)
    {
        if (_measureFirst is not { } first) { _measureFirst = (cloudId, index); return; }
        if (_activeMeasure is null)
        {
            var m = new Measurement();
            m.Points.Add(first);
            m.Points.Add((cloudId, index));
            _measurements.Add(m);
            _activeMeasure = m;
        }
        else
        {
            _activeMeasure.Points.Add((cloudId, index));   // 折线续段：新点接在链尾
        }
        _measureFirst = null;
        RebuildMeasureLayer();
        UpdateMeasureInfo();
        var m0 = _activeMeasure;
        var lastW = WorldOf(cloudId, index);
        var prevW = WorldOf(m0.Points[^2].Cloud, m0.Points[^2].Index);
        if (lastW is { } b && prevW is { } a)
            _ctx?.SetStatus($"量测 {MeasurementsCount}：本段 {Vector3.Distance(a, b):F3}｜折线总长 {PolyTotal(m0):F3}（{m0.Points.Count} 点，右键/Enter 结束）");
        Draw();
    }

    private int MeasurementsCount => _measurements.Count;

    /// <summary>量测模式 Backspace：优先撤挂起起点 → 撤活动折线末段（<2 点整条移除）→ 撤上一条完整折线。</summary>
    private void UndoMeasureStep()
    {
        if (_measureFirst is not null)
        {
            _measureFirst = null;
            _ctx?.SetStatus("已撤销挂起的起点——重新拾取");
            return;
        }
        if (_activeMeasure is not null)
        {
            if (_activeMeasure.Points.Count > 2)
            {
                _activeMeasure.Points.RemoveAt(_activeMeasure.Points.Count - 1);
                RebuildMeasureLayer();
                UpdateMeasureInfo();
                _ctx?.SetStatus($"已撤回折线末段（剩 {_activeMeasure.Points.Count} 点，总长 {PolyTotal(_activeMeasure):F3}）");
            }
            else
            {
                _measurements.Remove(_activeMeasure);
                _activeMeasure = null;
                RebuildMeasureLayer();
                UpdateMeasureInfo();
                _ctx?.SetStatus("已撤销整条量测——继续拾取开始新量测");
            }
            Draw();
            return;
        }
        if (_measurements.Count > 0)
        {
            _measurements.RemoveAt(_measurements.Count - 1);
            RebuildMeasureLayer();
            UpdateMeasureInfo();
            Draw();
            _ctx?.SetStatus($"已撤销上一条量测（剩余 {_measurements.Count} 条）");
        }
    }

    /// <summary>折线总长（失效点段跳过）。</summary>
    private double PolyTotal(Measurement m)
    {
        double sum = 0;
        for (int i = 1; i < m.Points.Count; i++)
        {
            var (pa, pb) = (WorldOf(m.Points[i - 1].Cloud, m.Points[i - 1].Index), WorldOf(m.Points[i].Cloud, m.Points[i].Index));
            if (pa is { } a && pb is { } b) sum += Vector3.Distance(a, b);
        }
        return sum;
    }

    /// <summary>点（局部坐标）经实体变换后的世界坐标；实体不存在/索引越界返回 null。</summary>
    private Vector3? WorldOf(string cloudId, int index)
    {
        if (!_clouds.TryGetValue(cloudId, out var c) || index < 0 || index >= c.Count) return null;
        float x = c.Pts[index * 3], y = c.Pts[index * 3 + 1], z = c.Pts[index * 3 + 2];
        return new Vector3(
            (float)(c.Xform[0] * x + c.Xform[1] * y + c.Xform[2] * z + c.Xform[3]),
            (float)(c.Xform[4] * x + c.Xform[5] * y + c.Xform[6] * z + c.Xform[7]),
            (float)(c.Xform[8] * x + c.Xform[9] * y + c.Xform[10] * z + c.Xform[11]));
    }

    /// <summary>量测清单变化后重建覆盖层控件（每折线 = 各段线 + 末点总长标签；位置在 Draw→UpdateMeasurements 每帧重投影）。</summary>
    private void RebuildMeasureLayer()
    {
        _measureLayer.Children.Clear();
        foreach (var m in _measurements)
        {
            for (int s = 0; s < m.Points.Count - 1; s++)
                _measureLayer.Children.Add(new Line { Stroke = MeasureBrush, StrokeThickness = 1.5, IsVisible = false });
            var label = new TextBlock
            {
                FontSize = 11, Foreground = MeasureBrush, IsVisible = false,
                Background = new SolidColorBrush(Color.FromArgb(0x88, 0x20, 0x20, 0x22)),
                Padding = new Thickness(3, 1),
            };
            _measureLayer.Children.Add(label);
        }
    }

    /// <summary>相机每帧重投影量测折线（Draw 调用）：端点世界坐标（WorldOf 已过实体变换）→ MVP（与渲染同管线）。</summary>
    private void UpdateMeasurements()
    {
        var mvp = _renderer.LastMvp;
        double ProjX(Vector3 w)
        {
            float cw = mvp.M14 * w.X + mvp.M24 * w.Y + mvp.M34 * w.Z + mvp.M44;
            if (cw <= 1e-6f) return double.NaN;
            float cc = mvp.M11 * w.X + mvp.M21 * w.Y + mvp.M31 * w.Z + mvp.M41;
            return (cc / cw + 1) * 0.5;
        }
        double ProjY(Vector3 w)
        {
            float cw = mvp.M14 * w.X + mvp.M24 * w.Y + mvp.M34 * w.Z + mvp.M44;
            if (cw <= 1e-6f) return double.NaN;
            float cc = mvp.M12 * w.X + mvp.M22 * w.Y + mvp.M32 * w.Z + mvp.M42;
            return 0.5 - cc / cw * 0.5;
        }
        bool ProjVisible(Vector3 w, out double px, out double py)
        {
            px = ProjX(w); py = ProjY(w);
            return !double.IsNaN(px) && !double.IsNaN(py) && px >= 0 && px <= 1 && py >= 0 && py <= 1;
        }

        int k = 0;
        foreach (var m in _measurements)
        {
            var world = m.Points.Select(pt => WorldOf(pt.Cloud, pt.Index)).ToList();
            for (int s = 0; s < m.Points.Count - 1; s++, k++)
            {
                var line = (Line)_measureLayer.Children[k];
                if (world[s] is not { } a || world[s + 1] is not { } b ||
                    !ProjVisible(a, out var ax, out var ay) || !ProjVisible(b, out var bx, out var by))
                { line.IsVisible = false; continue; }
                line.IsVisible = true;
                line.StartPoint = new Point(ax * _vpW / _scaling, ay * _vpH / _scaling);
                line.EndPoint = new Point(bx * _vpW / _scaling, by * _vpH / _scaling);
            }
            // 末点标签：折线总长
            var label = (TextBlock)_measureLayer.Children[k]; k++;
            if (world[^1] is not { } last || !ProjVisible(last, out var lx, out var ly) || m.Points.Count < 2)
            { label.IsVisible = false; continue; }
            label.IsVisible = true;
            label.Text = m.Points.Count == 2
                ? PolyTotal(m).ToString("F3")
                : $"{m.Points.Count}点 {PolyTotal(m):F3}";
            Canvas.SetLeft(label, lx * _vpW / _scaling + 6);
            Canvas.SetTop(label, ly * _vpH / _scaling - 8);
        }
    }

    private void UpdateMeasureInfo()
    {
        if (_measurements.Count == 0) { _measureInfo.Text = "无（📏 开启后 Ctrl+左键拾取，连续拾取=折线；右键/Enter 结束）"; return; }
        var parts = new List<string>();
        for (int i = 0; i < _measurements.Count; i++)
        {
            var m = _measurements[i];
            var p0 = WorldOf(m.Points[0].Cloud, m.Points[0].Index);
            var pn = WorldOf(m.Points[^1].Cloud, m.Points[^1].Index);
            parts.Add(p0 is { } a && pn is { } b
                ? $"#{i + 1} {m.Points.Count}点 总长 {PolyTotal(m):F3}  Δ首末({b.X - a.X:F2}, {b.Y - a.Y:F2}, {b.Z - a.Z:F2})"
                : $"#{i + 1}（点已失效）");
        }
        _measureInfo.Text = string.Join('\n', parts);
    }

    /// <summary>盒裁剪工具：轴对齐包围盒（实体局部坐标）保留盒内/盒外点为新实体。纯本地数学（不走 DLL）。</summary>
    private async void BoxCropTool()
    {
        var src = _clouds.Values.FirstOrDefault(c => c.SelCount > 0 && c.Loaded > 0)
                  ?? _clouds.Values.FirstOrDefault(c => _renderer.IsVisible(c.Id) && c.Loaded > 0)
                  ?? _clouds.Values.FirstOrDefault(c => c.Loaded > 0);
        if (src is null) { _ctx?.SetStatus("盒裁剪需要先有加载完成的点云实体"); return; }

        var b = src.BoundsMin;
        var bx = src.BoundsMax;
        var boxOp = new CloudToolOps.ToolOp
        {
            Id = "tool.boxcrop",
            Name = $"盒裁剪 · {src.Id}",
            Description = "轴对齐盒裁剪（实体局部坐标；已预填当前包围盒）：保留盒内或盒外点为新实体",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "min_x", Label = "最小 X", Type = "number", Default = b.X.ToString("F3", CultureInfo.InvariantCulture) },
                new CloudToolOps.ToolParam { Name = "min_y", Label = "最小 Y", Type = "number", Default = b.Y.ToString("F3", CultureInfo.InvariantCulture) },
                new CloudToolOps.ToolParam { Name = "min_z", Label = "最小 Z", Type = "number", Default = b.Z.ToString("F3", CultureInfo.InvariantCulture) },
                new CloudToolOps.ToolParam { Name = "max_x", Label = "最大 X", Type = "number", Default = bx.X.ToString("F3", CultureInfo.InvariantCulture) },
                new CloudToolOps.ToolParam { Name = "max_y", Label = "最大 Y", Type = "number", Default = bx.Y.ToString("F3", CultureInfo.InvariantCulture) },
                new CloudToolOps.ToolParam { Name = "max_z", Label = "最大 Z", Type = "number", Default = bx.Z.ToString("F3", CultureInfo.InvariantCulture) },
                new CloudToolOps.ToolParam { Name = "keep_outside", Label = "保留盒外", Type = "bool", Default = "false", Description = "勾选 = 保留盒外点（剔除盒内）；不勾 = 保留盒内点" },
            },
        };

        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, boxOp); }
        catch { return; }
        if (res is null) return;

        double Get(string name) => Convert.ToDouble(res.Values[Array.FindIndex(boxOp.Params, p => p.Name == name)], CultureInfo.InvariantCulture);
        bool keepInside = Convert.ToBoolean(res.Values[^1]) == false;   // keep_outside 取反
        var keep = CloudToolOps.SelectBoxInterior(src.Pts, src.Loaded,
            Get("min_x"), Get("min_y"), Get("min_z"), Get("max_x"), Get("max_y"), Get("max_z"),
            keepInside);
        if (keep.Count == 0) { _ctx?.SetStatus("盒内/盒外无可保留的点（检查盒范围）"); return; }

        var pts = new float[keep.Count * 3];
        for (int k = 0; k < keep.Count; k++)
        {
            pts[k * 3] = src.Pts[keep[k] * 3];
            pts[k * 3 + 1] = src.Pts[keep[k] * 3 + 1];
            pts[k * 3 + 2] = src.Pts[keep[k] * 3 + 2];
        }
        byte[]? col = null;
        if (src.Col is not null)
        {
            col = new byte[keep.Count * 3];
            for (int k = 0; k < keep.Count; k++)
            {
                col[k * 3] = src.Col[keep[k] * 3];
                col[k * 3 + 1] = src.Col[keep[k] * 3 + 1];
                col[k * 3 + 2] = src.Col[keep[k] * 3 + 2];
            }
        }

        var id = $"{src.Id}·盒裁#{_toolSeq++}";
        SetCloud(keep.Count, pts, col, "工具", id, keepCamera: true);
        if (_clouds.TryGetValue(id, out var nu))
        {
            nu.Xform = (double[])src.Xform.Clone();
            nu.ColorMode = src.ColorMode;
            nu.PointSize = src.PointSize;
            nu.SingleColor = src.SingleColor;
            _renderer.SetEntityTransform(id, nu.Xform);
            _renderer.SetEntityStyle(id, nu.ColorMode, nu.PointSize, nu.SingleColor);
        }
        _ctx?.SetStatus($"盒裁剪完成：{id}({keep.Count:N0} 点，{(keepInside ? "盒内" : "盒外")})");
        Draw();
    }

    /// <summary>算子工具菜单：每次打开从算子库目录重扫（alg 侧更新即时可见）。</summary>
    private ContextMenu BuildToolMenu()
    {
        var dir = ResolveAlgDir();
        var ops = CloudToolOps.LoadFromDir(dir, m => _ctx?.SetStatus(m));
        var menu = new ContextMenu();
        if (ops.Count == 0)
        {
            menu.Items.Add(new MenuItem { Header = "（算子库无可用 Point3D 算子）", IsEnabled = false });
            return menu;
        }
        foreach (var op in ops)
        {
            var item = new MenuItem
            {
                Header = op.Name,
                Tag = op,
            };
            if (!string.IsNullOrWhiteSpace(op.Description)) ToolTip.SetTip(item, op.Description);
            item.Click += (_, _) => RunToolOp(op);
            menu.Items.Add(item);
        }
        return menu;
    }

    /// <summary>算子库目录：comdll/algo/AlgProject（插件目录 comdll/ui/Cloud3D → 仓库根 → comdll）。</summary>
    private static string ResolveAlgDir()
    {
        var pluginDir = AppContext.BaseDirectory;                       // …/comdll/ui/Cloud3D/
        var root = System.IO.Path.GetFullPath(System.IO.Path.Combine(pluginDir, "..", "..", ".."));
        return System.IO.Path.Combine(root, "comdll", "algo", "AlgProject");
    }

    // ==================== 工具：随机采样 / 去噪 / 合并 / 导入 / 导出 ====================

    /// <summary>工具产出实体的通用落库：子集/处理结果沿用源实体的变换与样式，视角不重置。</summary>
    private void AddToolEntity(string id, CloudPayload p, CloudData? styleFrom)
    {
        SetCloud(p.Count, p.Points, p.ColorsRgb, p.Source ?? "工具", id, keepCamera: true);
        var created = _clouds.TryGetValue(id, out var cd) ? cd : null;
        if (created is not null) PushAddUndo(created);   // 新建类操作入 Undo 栈（Ctrl+Z 删除该实体）
        if (styleFrom is not null && _clouds.TryGetValue(id, out var nu))
        {
            nu.Xform = (double[])styleFrom.Xform.Clone();
            nu.ColorMode = styleFrom.ColorMode;
            nu.PointSize = styleFrom.PointSize;
            nu.SingleColor = styleFrom.SingleColor;
            _renderer.SetEntityTransform(id, nu.Xform);
            _renderer.SetEntityStyle(id, nu.ColorMode, nu.PointSize, nu.SingleColor);
        }
    }

    /// <summary>变换工具：平移/绕 Z 旋转/缩放/镜像叠加到实体当前变换（原点=实体局部原点；可多次叠加）。</summary>
    private async void TransformTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("变换工具需要先有加载完成的点云实体"); return; }
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.xform", Name = $"变换 · {src.Id}",
            Description = "把平移/绕 Z 旋转/统一缩放/镜像叠加到实体当前变换（多次调用可累乘；数据树详情区可看 4×4 矩阵）",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "translate_x", Label = "平移 X", Type = "number", Default = "0" },
                new CloudToolOps.ToolParam { Name = "translate_y", Label = "平移 Y", Type = "number", Default = "0" },
                new CloudToolOps.ToolParam { Name = "translate_z", Label = "平移 Z", Type = "number", Default = "0" },
                new CloudToolOps.ToolParam { Name = "rotate_x", Label = "绕X旋转(度)", Type = "number", Default = "0", Min = -360, Max = 360 },
                new CloudToolOps.ToolParam { Name = "rotate_y", Label = "绕Y旋转(度)", Type = "number", Default = "0", Min = -360, Max = 360 },
                new CloudToolOps.ToolParam { Name = "rotate_z", Label = "绕Z旋转(度)", Type = "number", Default = "0", Min = -360, Max = 360 },
                new CloudToolOps.ToolParam { Name = "scale_x", Label = "缩放 X", Type = "number", Default = "1", Min = 0.001, Max = 10000 },
                new CloudToolOps.ToolParam { Name = "scale_y", Label = "缩放 Y", Type = "number", Default = "1", Min = 0.001, Max = 10000 },
                new CloudToolOps.ToolParam { Name = "scale_z", Label = "缩放 Z", Type = "number", Default = "1", Min = 0.001, Max = 10000 },
                new CloudToolOps.ToolParam { Name = "mirror_x", Label = "镜像 X", Type = "bool", Default = "false" },
                new CloudToolOps.ToolParam { Name = "mirror_y", Label = "镜像 Y", Type = "bool", Default = "false" },
                new CloudToolOps.ToolParam { Name = "mirror_z", Label = "镜像 Z", Type = "bool", Default = "false" },
                new CloudToolOps.ToolParam { Name = "reset_first", Label = "先重置变换", Type = "bool", Default = "false", Description = "勾选 = 忽略实体当前变换，从单位阵开始叠加（恢复原始坐标）" },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;

        double v(int i) => Convert.ToDouble(res!.Values[i], CultureInfo.InvariantCulture);
        bool bv(int i) => Convert.ToBoolean(res!.Values[i]);
        // 全默认检测按索引直接判定（0-2=平移 0、3-5=旋转 0、6-8=缩放 1）
        bool changed = false;
        for (int i = 0; i < 9 && !changed; i++)
        {
            double expect = i < 3 ? 0 : i < 6 ? 0 : 1;
            if (Math.Abs(v(i) - expect) > 1e-9) changed = true;
        }
        if (!changed && !bv(9) && !bv(10) && !bv(11) && !bv(12))
        {
            _ctx?.SetStatus("变换未改动（全默认值）");
            return;
        }
        try
        {
            var xformBefore = (double[])src.Xform.Clone();
            var newXform = CloudToolOps.ComposeTransform(src.Xform,
                v(0), v(1), v(2), v(3), v(4), v(5), v(6), v(7), v(8), bv(9), bv(10), bv(11), bv(12));
            src.Xform = newXform;
            _renderer.SetEntityTransform(src.Id, src.Xform);
            // 变换入 Undo 栈（before=变换前快照 / after=变换后快照 → Ctrl+Z 恢复姿态）
            var ue = new CloudUndo.UndoEntry { Description = $"变换「{src.Id}」" };
            var beforeSnap = CaptureSnapshot(src);
            beforeSnap.Xform = xformBefore;
            ue.UndoSnapshots.Add(beforeSnap);
            ue.RedoSnapshots.Add(CaptureSnapshot(src));
            _undo.Push(ue);
            Draw();
            CloudsChanged?.Invoke();
            _ctx?.SetStatus($"变换已应用：{src.Id}（平移 ({v(0):F3}, {v(1):F3}, {v(2):F3}) · 旋转 ({v(3):F1}, {v(4):F1}, {v(5):F1})° · 缩放 ({v(6):F3}, {v(7):F3}, {v(8):F3}){(bv(9) ? " · 镜像X" : "")}{(bv(10) ? " · 镜像Y" : "")}{(bv(11) ? " · 镜像Z" : "")}{(bv(12) ? " · 已重置" : "")}）");
        }
        catch (Exception ex) { _ctx?.SetStatus($"变换失败：{ex.Message}"); }
    }

    /// <summary>平面分割工具：RANSAC 提取最大平面——平面点集/残差各成新实体（基面提取、去背景常用）。</summary>
    private async void RansacTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("此工具需要先有加载完成的点云实体"); return; }
        if (src.Loaded > 20_000_000)
        {
            _ctx?.SetStatus($"平面分割迭代成本 O(次数×点数)——当前 {src.Loaded:N0} 点太大，请先 🎲 抽稀（建议 ≤2000 万）");
            return;
        }
        var diag = (src.BoundsMax - src.BoundsMin).Length;
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.ransac", Name = $"平面分割 · {src.Id}",
            Description = "RANSAC 提取最大平面：平面点集/残差点集各成新实体（基面提取/去背景）。距离阈值=点到平面的判内距离；迭代越多越稳",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "dist", Label = "距离阈值", Type = "number", Default = Math.Max(0.01, diag * 0.002).ToString("F4", CultureInfo.InvariantCulture), Min = 1e-6 },
                new CloudToolOps.ToolParam { Name = "iterations", Label = "迭代次数", Type = "int", Default = "200", Min = 1, Max = 5000 },
                new CloudToolOps.ToolParam { Name = "rounds", Label = "分割轮数", Type = "int", Default = "1", Min = 1, Max = 20, Description = "逐轮在残差上继续提取最大平面（多基面场景）——每轮平面各成一个实体" },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;
        double dist = Convert.ToDouble(res.Values[0], CultureInfo.InvariantCulture);
        int iters = Convert.ToInt32(res.Values[1], CultureInfo.InvariantCulture);
        int rounds = Convert.ToInt32(res.Values[2], CultureInfo.InvariantCulture);
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            _ctx?.SetStatus($"平面分割执行中…（{src.Loaded:N0} 点 × {iters} 迭代 × {rounds} 轮）");
            var created = new List<string>();
            int inlierTotal = 0;
            var restOut = payload;
            await Task.Run(() =>
            {
                var cur = payload;
                for (int r = 0; r < rounds; r++)
                {
                    if (cur.Count < 50) break;   // 剩余太少不再分割
                    var (pl, rs, il) = CloudPointIO.RansacPlaneSplit(cur, dist, iters);
                    inlierTotal += il;
                    var idP = $"平面{r + 1}#{_toolSeq++}";
                    var plLocal = pl;
                    created.Add($"{idP}({plLocal.Count:N0})");
                    Avalonia.Threading.Dispatcher.UIThread.Post(() => AddToolEntity(idP, plLocal, src));
                    cur = rs;
                }
                restOut = cur;
            });
            restOut = restOut!;
            var idR = $"残差#{_toolSeq++}";
            AddToolEntity(idR, restOut, src);
            _ctx?.SetStatus($"平面分割完成：{created.Count} 个平面 {string.Join("、", created)}｜残差 {restOut.Count:N0} 点（{idR}）｜内点合计占比 {100.0 * inlierTotal / src.Loaded:F1}%");
            Draw();
        }
        catch (Exception ex) { _ctx?.SetStatus($"平面分割失败：{ex.Message}"); }
    }

    /// <summary>量测清单复制到剪贴板：折线点数/总长/逐段/首末差，报表可直接粘贴。</summary>
    private async void CopyMeasurements()
    {
        if (_measurements.Count == 0) { _ctx?.SetStatus("无量测可复制"); return; }
        var lines = new List<string> { $"# ComUI 3D 量测清单 {DateTime.Now:yyyy-MM-dd HH:mm:ss}" };
        for (int i = 0; i < _measurements.Count; i++)
        {
            var m = _measurements[i];
            var p0 = WorldOf(m.Points[0].Cloud, m.Points[0].Index);
            var pn = WorldOf(m.Points[^1].Cloud, m.Points[^1].Index);
            lines.Add($"#{i + 1} {m.Points.Count}点 总长 {PolyTotal(m):F3}");
            for (int s = 1; s < m.Points.Count; s++)
            {
                var (pa, pb) = (WorldOf(m.Points[s - 1].Cloud, m.Points[s - 1].Index), WorldOf(m.Points[s].Cloud, m.Points[s].Index));
                if (pa is { } a && pb is { } b)
                    lines.Add($"  段{s}: ({a.X:F3},{a.Y:F3},{a.Z:F3}) -> ({b.X:F3},{b.Y:F3},{b.Z:F3})  |Δ|={Vector3.Distance(a, b):F3}");
            }
            if (p0 is { } st && pn is { } en)
                lines.Add($"  首末: Δ({en.X - st.X:F3}, {en.Y - st.Y:F3}, {en.Z - st.Z:F3})");
        }
        var text = string.Join('\n', lines);
        var top = TopLevel.GetTopLevel(this);
        if (top?.Clipboard is null) { _ctx?.SetStatus("剪贴板不可用"); return; }
        try
        {
            await top.Clipboard.SetTextAsync(text);
            _ctx?.SetStatus($"量测清单已复制（{_measurements.Count} 条）——可直接粘贴到报表");
        }
        catch (Exception ex) { _ctx?.SetStatus($"复制失败：{ex.Message}"); }
    }

    /// <summary>视图设置持久化（变更即存）：抽稀/截断/切片/色带域/投影/锁定轴。</summary>
    private void SaveViewSettings()
    {
        if (_settingsPath is null) return;
        CloudViewSettings.Save(_settingsPath, new CloudViewSettings
        {
            Stride = _stride,
            FarClip = _farClip,
            ClipAxis = ClipAxis,
            ClipLo = _clipLoBox.Text is { Length: > 0 } lo && double.TryParse(lo, NumberStyles.Float, CultureInfo.InvariantCulture, out var clo) ? clo : null,
            ClipHi = _clipHiBox.Text is { Length: > 0 } hi && double.TryParse(hi, NumberStyles.Float, CultureInfo.InvariantCulture, out var chi) ? chi : null,
            ColorMin = _rangeLoBox.Text is { Length: > 0 } rlo && double.TryParse(rlo, NumberStyles.Float, CultureInfo.InvariantCulture, out var cmi) ? cmi : null,
            ColorMax = _rangeHiBox.Text is { Length: > 0 } rhi && double.TryParse(rhi, NumberStyles.Float, CultureInfo.InvariantCulture, out var cma) ? cma : null,
            Projection = _projCombo.SelectedIndex,
            LockPitch = _lockPitch,
        });
    }

    /// <summary>量测清单 CSV 导出（CloudMeasurements.ToCsv 纯函数）。</summary>
    private async void ExportMeasurementsCsv()
    {
        if (_measurements.Count == 0) { _ctx?.SetStatus("无量测可导出"); return; }
        var top = TopLevel.GetTopLevel(this);
        if (top is null) return;
        var file = await top.StorageProvider.SaveFilePickerAsync(new Avalonia.Platform.Storage.FilePickerSaveOptions
        {
            Title = "导出量测清单 CSV",
            SuggestedFileName = $"量测清单_{DateTime.Now:yyyyMMdd_HHmmss}",
            DefaultExtension = "csv",
            FileTypeChoices = new[] { new Avalonia.Platform.Storage.FilePickerFileType("CSV 文本") { Patterns = new[] { "*.csv" } } },
        });
        if (file is null) return;
        var path = file.Path.LocalPath;
        try
        {
            var polylines = new List<double[]>();
            foreach (var m in _measurements)
            {
                var pl = new List<double>();
                foreach (var pt in m.Points)
                    if (WorldOf(pt.Cloud, pt.Index) is { } w) { pl.Add(w.X); pl.Add(w.Y); pl.Add(w.Z); }
                polylines.Add(pl.ToArray());
            }
            var csv = await Task.Run(() => CloudMeasurements.ToCsv(polylines.ToArray()));
            await File.WriteAllTextAsync(path, csv);
            _ctx?.SetStatus($"量测清单 CSV 已导出：{System.IO.Path.GetFileName(path)}");
        }
        catch (Exception ex) { _ctx?.SetStatus($"导出失败：{ex.Message}"); }
    }

    /// <summary>拾取历史追加（纯函数截断+去重）。</summary>
    private void AppendPickHistory(double x, double y, double z)
    {
        _pickHistory.Add((x, y, z));
        var trimmed = CloudMeasurements.TrimHistory(_pickHistory, PickHistoryMax);
        if (trimmed.Length != _pickHistory.Count)
        {
            _pickHistory.Clear();
            foreach (var p in trimmed) _pickHistory.Add(p);
        }
        UpdatePickHistoryInfo();
    }

    private void UpdatePickHistoryInfo()
    {
        if (_pickHistory.Count == 0) { _pickHistoryInfo.Text = "无（拾取命中自动记录）"; return; }
        var parts = new List<string>();
        int show = Math.Min(3, _pickHistory.Count);
        for (int i = _pickHistory.Count - show; i < _pickHistory.Count; i++)
        {
            var p = _pickHistory[i];
            parts.Add($"({p.X:F2}, {p.Y:F2}, {p.Z:F2})");
        }
        _pickHistoryInfo.Text = $"共 {_pickHistory.Count} 点：" + string.Join("  ", parts) + (_pickHistory.Count > 3 ? " …" : "");
    }

    /// <summary>拾取历史 CSV 导出（CloudMeasurements.HistoryToCsv 纯函数）。</summary>
    private async void ExportPickHistoryCsv()
    {
        if (_pickHistory.Count == 0) { _ctx?.SetStatus("拾取历史为空"); return; }
        var top = TopLevel.GetTopLevel(this);
        if (top is null) return;
        var file = await top.StorageProvider.SaveFilePickerAsync(new Avalonia.Platform.Storage.FilePickerSaveOptions
        {
            Title = "导出拾取历史 CSV",
            SuggestedFileName = $"拾取历史_{DateTime.Now:yyyyMMdd_HHmmss}",
            DefaultExtension = "csv",
            FileTypeChoices = new[] { new Avalonia.Platform.Storage.FilePickerFileType("CSV 文本") { Patterns = new[] { "*.csv" } } },
        });
        if (file is null) return;
        var path = file.Path.LocalPath;
        try
        {
            var csv = await Task.Run(() => CloudMeasurements.HistoryToCsv(_pickHistory));
            await File.WriteAllTextAsync(path, csv);
            _ctx?.SetStatus($"拾取历史 CSV 已导出：{System.IO.Path.GetFileName(path)}（{_pickHistory.Count} 点）");
        }
        catch (Exception ex) { _ctx?.SetStatus($"导出失败：{ex.Message}"); }
    }

    /// <summary>伪彩色带域提交：作用于工作实体（选区优先）；任一端留空=回退包围盒域。</summary>
    private void ApplyColorRange()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("色带域需要先有加载完成的点云实体"); return; }
        bool loOk = double.TryParse(_rangeLoBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var lo);
        bool hiOk = double.TryParse(_rangeHiBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var hi);
        if (loOk && hiOk)
        {
            src.ColorMin = (float)Math.Min(lo, hi);
            src.ColorMax = (float)Math.Max(lo, hi);
            _renderer.SetEntityColorRange(src.Id, src.ColorMin, src.ColorMax);
            _ctx?.SetStatus($"色带域已设：{src.Id} [{src.ColorMin:F3}, {src.ColorMax:F3}]（高度伪彩对比度拉满）");
        }
        else
        {
            src.ColorMin = null; src.ColorMax = null;
            _renderer.SetEntityColorRange(src.Id, null, null);
            if (loOk || hiOk) _ctx?.SetStatus("色带域需要上下限都填（或清除）——已回退包围盒域");
        }
        SaveViewSettings();
        Draw();
    }

    /// <summary>切片上/下限提交：两端都有有效数值才启用（单端不启用），否则关闭切片。</summary>
    private void ApplyZClip()
    {
        bool loOk = double.TryParse(_clipLoBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var lo);
        bool hiOk = double.TryParse(_clipHiBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var hi);
        if (loOk && hiOk)
        {
            _renderer.SetZClip(ClipAxis, (float)Math.Min(lo, hi), (float)Math.Max(lo, hi));   // 用户填反自动纠正
            _ctx?.SetStatus($"切片启用：[{Math.Min(lo, hi):F3}, {Math.Max(lo, hi):F3}]（{ClipAxisName} 坐标）");
        }
        else
        {
            _renderer.SetZClip(ClipAxis, null, null);
            if (loOk || hiOk) _ctx?.SetStatus("切片需要上下限都填（或清除）");
        }
        SaveViewSettings();
        Draw();
    }

    private int ClipAxis => Math.Max(0, _clipAxisCombo.SelectedIndex);
    private string ClipAxisName => ClipAxis switch { 0 => "X", 1 => "Y", _ => "Z" };

    /// <summary>高度图工具：点云按 XY 网格聚合高度统计 → jet 着色 ImagePayload → 总线 image/stitched → 2D 工作台联动显示。</summary>
    /// <summary>法线估计+光照：半径邻域 PCA → 逐点法线上传显存 → 实体光照开关（toggle：已有法线时仅切光照）。</summary>
    private async void NormalsTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("法线估计需要先有加载完成的点云实体"); return; }
        if (src.Loaded > 10_000_000)
        {
            _ctx?.SetStatus($"法线估计邻域查询成本高——当前 {src.Loaded:N0} 点太大，请先 🎲 抽稀（建议 ≤1000 万）");
            return;
        }
        // 已有法线：先给关闭光照的出口（toggle）
        if (src.Normals is not null)
        {
            bool lit = _renderer.TryGetNormals(src.Id, out _) && true;
            // toggle：无法读回实体 Lit——用计数翻转：直接切关闭
            _renderer.SetEntityLit(src.Id, false);
            Draw();
            _ctx?.SetStatus($"光照已关闭（法线保留，{src.Loaded:N0} 点）——再点 🧮 重新计算或改半径");
            // 注意：连续点击会再次重算——第二次点击起视为重算入口
        }
        var diag = (src.BoundsMax - src.BoundsMin).Length;
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.normals", Name = $"法线估计 · {src.Id}",
            Description = "半径邻域 PCA 逐点法线（朝向统一朝 +Y）→ Lambert 光照显示（立体感）。半径默认=包围盒对角 0.5% 自适应；大数据先抽稀",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "radius", Label = "邻域半径", Type = "number", Default = Math.Max(0.01, diag * 0.005).ToString("F4", CultureInfo.InvariantCulture), Min = 1e-6 },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;
        double radius = Convert.ToDouble(res.Values[0], CultureInfo.InvariantCulture);
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            _ctx?.SetStatus($"法线估计执行中…（{src.Loaded:N0} 点）");
            var normals = await Task.Run(() => CloudNormals.Compute(payload.Points, payload.Count, radius));
            src.Normals = normals;
            _renderer.SetNormals(src.Id, normals);
            _renderer.SetEntityLit(src.Id, true);
            Draw();
            _ctx?.SetStatus($"法线估计完成：{src.Loaded:N0} 点法线已上传，光照模式开启（🧮 再点可关闭/重算）");
        }
        catch (Exception ex) { _ctx?.SetStatus($"法线估计失败：{ex.Message}"); }
    }

    private async void HeightMapTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("高度图需要先有加载完成的点云实体"); return; }
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.heightmap", Name = $"高度图 · {src.Id}",
            Description = "点云按 XY 网格聚合高度（顶面 max）→ jet 色带高度图 → 发布到 2D 工作台联动显示。分辨率越大越精细",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "grid_w", Label = "横向网格数", Type = "int", Default = "512", Min = 16, Max = 4096 },
                new CloudToolOps.ToolParam { Name = "export_csv", Label = "导出高度CSV", Type = "bool", Default = "false", Description = "同时把网格高度导出为 CSV（每行 row,col,x,y,z，供第三方算法）" },
                new CloudToolOps.ToolParam { Name = "export_png", Label = "导出高度PNG", Type = "bool", Default = "false", Description = "同时把高度图保存为 PNG 文件" },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;
        int gridW = Convert.ToInt32(res.Values[0], CultureInfo.InvariantCulture);
        bool exportCsv = Convert.ToBoolean(res.Values[1]);
        bool exportPng = Convert.ToBoolean(res.Values[2]);
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            _ctx?.SetStatus($"高度图生成中…（{src.Loaded:N0} 点）");
            var meta = await Task.Run(() =>
            {
                // 网格高度按实体 XY 纵横比自动配 H（保证格子近正方）
                double spanx = Math.Max(src.BoundsMax.X - src.BoundsMin.X, 1e-9);
                double spany = Math.Max(src.BoundsMax.Y - src.BoundsMin.Y, 1e-9);
                int gridH = Math.Max(16, Math.Min(4096, (int)Math.Round(gridW * spany / spanx)));
                var (heights, mask, w, h, minx, miny, spanx2, spany2, zmin, zmax) = CloudRaster.RasterizeHeight(payload, gridW, gridH, "max");
                _rasterMeta = (heights, mask, w, h, minx, miny, spanx2, spany2);
                return CloudRaster.ToImage(heights, mask, w, h, zmin, zmax, $"{src.Id}·高度图");
            });
            _bus.Publish(BusTopics.ImageStitched, meta);   // 2D 工作台订阅 image/stitched → 自动显示
            _ctx?.SetStatus($"高度图已生成并发布：{meta.Width}×{meta.Height}（切换到 2D 工作台查看）");

            if (exportPng)
            {
                var topPng = TopLevel.GetTopLevel(this);
                if (topPng is not null)
                {
                    var pngFile = await topPng.StorageProvider.SaveFilePickerAsync(new Avalonia.Platform.Storage.FilePickerSaveOptions
                    {
                        Title = "导出高度图 PNG",
                        SuggestedFileName = $"{src.Id}·高度图",
                        DefaultExtension = "png",
                        FileTypeChoices = new[] { new Avalonia.Platform.Storage.FilePickerFileType("PNG 图像") { Patterns = new[] { "*.png" } } },
                    });
                    if (pngFile is not null)
                    {
                        var pngPath = pngFile.Path.LocalPath;
                        await Task.Run(() =>
                        {
                            using var bmp = new Avalonia.Media.Imaging.WriteableBitmap(
                                new Avalonia.PixelSize(meta.Width, meta.Height), new Avalonia.Vector(96, 96),
                                Avalonia.Platform.PixelFormats.Bgra8888, Avalonia.Platform.AlphaFormat.Opaque);
                            using var fb = bmp.Lock();
                            System.Runtime.InteropServices.Marshal.Copy(meta.PixelsBgra, 0, fb.Address, meta.PixelsBgra.Length);
                            using var fs2 = System.IO.File.Create(pngPath);
                            bmp.Save(fs2);
                        });
                        _ctx?.SetStatus($"高度图 PNG 已导出：{System.IO.Path.GetFileName(pngPath)}");
                    }
                }
            }

            if (exportCsv)
            {
                var top = TopLevel.GetTopLevel(this);
                if (top is not null)
                {
                    var file = await top.StorageProvider.SaveFilePickerAsync(new Avalonia.Platform.Storage.FilePickerSaveOptions
                    {
                        Title = "导出高度网格 CSV（row,col,x,y,z）",
                        SuggestedFileName = $"{src.Id}·高度网格",
                        DefaultExtension = "csv",
                    });
                    if (file is not null)
                    {
                        var (heights, mask, w, h, minx, miny, spanx, spany) = _rasterMeta!.Value;
                        var csvPath = file.Path.LocalPath;
                        await Task.Run(() =>
                        {
                            using var wtr = new System.IO.StreamWriter(csvPath);
                            wtr.WriteLine("row,col,x,y,z");
                            for (int gy = 0; gy < h; gy++)
                                for (int gx = 0; gx < w; gx++)
                                {
                                    int idx = gy * w + gx;
                                    if (!mask[idx]) continue;
                                    double cx = minx + (gx + 0.5) * spanx / w;
                                    double cy = miny + (gy + 0.5) * spany / h;
                                    wtr.WriteLine(FormattableString.Invariant($"{gy},{gx},{cx:F4},{cy:F4},{heights[idx]:F4}"));
                                }
                        });
                        _ctx?.SetStatus($"高度网格 CSV 已导出：{System.IO.Path.GetFileName(csvPath)}");
                    }
                }
            }
        }
        catch (Exception ex)
        {
            _ctx?.SetStatus($"高度图生成失败：{ex.Message}");
        }
    }

    private (float[] Heights, bool[] Mask, int W, int H, double MinX, double MinY, double SpanX, double SpanY)? _rasterMeta;   // 最近一次栅格化元数据（CSV 导出用）

    private void ToggleLockPitch()
    {
        _lockPitch = !_lockPitch;
        if (_lockPitchBtn is not null)
        {
            if (_lockPitch)
                _lockPitchBtn.Background = new SolidColorBrush(Color.FromRgb(0x2A, 0x3A, 0x5A));
            else
                _lockPitchBtn.ClearValue(Button.BackgroundProperty);
        }
        _ctx?.SetStatus(_lockPitch ? "已锁定竖直轴：左键拖拽只水平旋转（俯视巡检）" : "竖直轴锁定已解除（自由翻滚）");
    SaveViewSettings();
    }

    /// <summary>质心对齐（CloudCompare Match centers）：移动实体平移到基准实体质心重合（世界坐标差直接加到平移分量）。</summary>
    private async void MatchCentersTool()
    {
        var visible = _clouds.Values.Where(c => _renderer.IsVisible(c.Id) && c.Loaded > 0).ToList();
        if (visible.Count < 2)
        {
            _ctx?.SetStatus("质心对齐需要两个可见的已加载实体（基准/被移动）");
            return;
        }
        var sel = _clouds.Values.FirstOrDefault(c => c.SelCount > 0 && c.Loaded > 0);
        var choices = visible.Select(c => c.Id).ToList();
        var movDefault = sel?.Id ?? choices[1];
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.matchcenters", Name = "质心对齐",
            Description = "把移动实体的质心平移到基准实体质心重合（只平移不旋转——粗配准第一步；之后可用 🔗 点对配准/⚙ ICP 精化）",
            DllPath = "", Entry = "",
            Params = Array.Empty<CloudToolOps.ToolParam>(),
            CloudInputs = 2,
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op, cloudChoices: choices, preselect: (movDefault, choices.FirstOrDefault(c => c != movDefault))); }
        catch { return; }
        if (res is null) return;
        if (!_clouds.TryGetValue(res.CloudA!, out var mov) || !_clouds.TryGetValue(res.CloudB!, out var rf))
        {
            _ctx?.SetStatus("质心对齐：基准/被移动实体无效");
            return;
        }

        Vector3 LocalCenter(CloudData c)
        {
            double sx = 0, sy = 0, sz = 0;
            for (int i = 0; i < c.Loaded; i++)
            {
                sx += c.Pts[i * 3]; sy += c.Pts[i * 3 + 1]; sz += c.Pts[i * 3 + 2];
            }
            return new Vector3((float)(sx / c.Loaded), (float)(sy / c.Loaded), (float)(sz / c.Loaded));
        }
        Vector3 ToWorld(CloudData c, Vector3 lp) => new(
            (float)(c.Xform[0] * lp.X + c.Xform[1] * lp.Y + c.Xform[2] * lp.Z + c.Xform[3]),
            (float)(c.Xform[4] * lp.X + c.Xform[5] * lp.Y + c.Xform[6] * lp.Z + c.Xform[7]),
            (float)(c.Xform[8] * lp.X + c.Xform[9] * lp.Y + c.Xform[10] * lp.Z + c.Xform[11]));

        var movCenter = ToWorld(mov, LocalCenter(mov));
        var refCenter = ToWorld(rf, LocalCenter(rf));
        var d = refCenter - movCenter;
        mov.Xform[3] += d.X;
        mov.Xform[7] += d.Y;
        mov.Xform[11] += d.Z;
        _renderer.SetEntityTransform(mov.Id, mov.Xform);
        Draw();
        CloudsChanged?.Invoke();
        _ctx?.SetStatus($"质心对齐完成：「{mov.Id}」平移 ({d.X:F3}, {d.Y:F3}, {d.Z:F3}) 与「{rf.Id}」质心重合");
    }

    // ==================== 快捷键动作：选区/投影/步进/点大小/算子菜单/帮助 ====================

    /// <summary>全选工作实体（按抽稀间隔采样语义，与框选一致）。</summary>
    private void SelectAllPoints()
    {
        var src = PickWorkingEntity();
        if (src is null || src.Loaded <= 0) { _ctx?.SetStatus("全选需要先有加载完成的点云实体"); return; }
        var sel = CloudSelection.SelectAll(src.Count);   // 长度与 SetCloud 分配语义一致（Count），渐进上传期间也稳定
        int n = 0;
        for (int i = 0; i < src.Loaded; i++) if (sel[i] == 255) n++;
        src.Sel = sel;
        src.SelCount = n;
        _renderer.SetSelection(src.Id, sel);
        _selInfo.Text = $"全选 {n:N0} 点（{src.Id}）——提取 / 剔除 / 反选可用";
        Draw();
        _ctx?.SetStatus($"已全选「{src.Id}」{n:N0} 点");
    }

    /// <summary>反选工作实体选区。</summary>
    private void InvertSelection()
    {
        var src = PickWorkingEntity();
        if (src is null || src.SelCount <= 0 || src.Loaded <= 0) { _ctx?.SetStatus("反选需要先有选区（框选/全选后）"); return; }
        var sel = CloudSelection.Invert(src.Sel, src.Loaded, out var n);
        src.Sel = sel;
        src.SelCount = n;
        _renderer.SetSelection(src.Id, sel);
        _selInfo.Text = $"反选后 {n:N0} 点（{src.Id}）——提取 / 剔除可用";
        Draw();
        _ctx?.SetStatus($"已反选：{n:N0} 点");
    }

    /// <summary>配准模式 Backspace：撤挂起点 → 撤最后一对。</summary>
    private void UndoRegistStep()
    {
        if (_registPending is not null) { _registPending = null; _ctx?.SetStatus("已撤销挂起的移动点——重新拾取"); return; }
        if (_registPairs.Count > 0)
        {
            _registPairs.RemoveAt(_registPairs.Count - 1);
            RebuildRegistLayer();
            _ctx?.SetStatus($"已撤销最后一对（剩余 {_registPairs.Count} 对）");
            Draw();
        }
    }

    /// <summary>投影切换（透视 ↔ 正交）。</summary>
    private void ToggleProj()
    {
        _projCombo.SelectedIndex = _projCombo.SelectedIndex == 1 ? 0 : 1;
        SaveViewSettings();
        _ctx?.SetStatus(_projCombo.SelectedIndex == 1 ? "正交投影" : "透视投影");
    }

    /// <summary>方向键相机步进（旋转 ±5°，与拖拽同公式；锁定竖直轴时 pitch 忽略）。</summary>
    private void NudgeCamera(double yawDeg, double pitchDeg)
    {
        if (_lockPitch) pitchDeg = 0;
        _orientation = CloudCamera.NudgeOrientation(_orientation, yawDeg, pitchDeg);
        Draw();
    }

    /// <summary>Shift+方向键平移步进（相机 right/up 向 0.05×半径）。</summary>
    private void PanBy(double dx, double dy)
    {
        MarkInteract();
        var right = Vector3.Transform(Vector3.UnitX, _orientation);
        var up = Vector3.Transform(Vector3.UnitY, _orientation);
        double s = _fitRadius * 0.05;
        _target += (-right * (float)(dx * s) + up * (float)(dy * s));
        Draw();
    }

    /// <summary>点大小步进（全部可见实体 ±step，clamp 1..10；渲染器与账本同步）。</summary>
    private void AdjustPointSize(double step)
    {
        int touched = 0;
        foreach (var c in _clouds.Values)
        {
            if (!_renderer.IsVisible(c.Id) || c.Loaded <= 0) continue;
            c.PointSize = Math.Clamp(c.PointSize + (float)step, 1f, 10f);
            _renderer.SetEntityPointSize(c.Id, c.PointSize);
            touched++;
        }
        if (touched == 0) { _ctx?.SetStatus("无可见实体可调点大小"); return; }
        Draw();
        SaveViewSettings();
        _ctx?.SetStatus($"点大小 {(step > 0 ? "+" : "")}{step:F1}（{touched} 个可见实体）");
    }

    /// <summary>算子工具菜单（Q 键 / ⚙ 按钮共用）。</summary>
    private void OpenOperatorMenu()
    {
        if (_toolOpsBtn is null) return;
        var menu = BuildToolMenu();
        menu.Open(_toolOpsBtn);
    }

    /// <summary>快捷键帮助浮层开关（? 键）。</summary>
    private void ToggleHelp()
    {
        if (_helpOverlay is not null && _overlay.Children.Contains(_helpOverlay))
        {
            _overlay.Children.Remove(_helpOverlay);
            Draw();
            return;
        }
        var tb = new TextBlock
        {
            FontSize = 11.5,
            Foreground = new SolidColorBrush(Color.FromRgb(0xD8, 0xD8, 0xDC)),
            FontFamily = new Avalonia.Media.FontFamily("Cascadia Mono, Consolas, monospace"),
        };
        var sb = new System.Text.StringBuilder();
        sb.AppendLine("快捷键（焦点在画布时生效；文本框输入不受影响）");
        sb.AppendLine();
        foreach (var (_, keys, desc) in CloudHotkeys.Bindings)
            sb.AppendLine($"  {keys,-14} {desc}");
        tb.Text = sb.ToString();
        _helpOverlay = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(0xE8, 0x1A, 0x1A, 0x1C)),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(18, 14),
            MaxWidth = 560,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Child = tb,
        };
        _overlay.Children.Add(_helpOverlay);
        Draw();
    }

    /// <summary>体素抽稀工具：确定性每体素取首点（本地处理，与算子库质心版语义互补）。</summary>
    private async void VoxelSampleTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("此工具需要先有加载完成的点云实体"); return; }
        var diag = (src.BoundsMax - src.BoundsMin).Length;
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.voxel", Name = $"体素抽稀 · {src.Id}",
            Description = "确定性体素下采样：每体素取首点（同输入同输出）。体素尺寸默认=包围盒对角/256；与 ⚙「体素下采样」算子（取质心）语义不同",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "voxel", Label = "体素尺寸", Type = "number", Default = Math.Max(0.001, diag / 256).ToString("F4", CultureInfo.InvariantCulture), Min = 1e-6 },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;
        double voxel = Convert.ToDouble(res.Values[0], CultureInfo.InvariantCulture);
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            _ctx?.SetStatus($"体素抽稀执行中…（{src.Loaded:N0} 点）");
            var (kept, count) = await Task.Run(() => CloudPointIO.VoxelDownsample(payload, voxel));
            var id = $"体素#{_toolSeq++}";
            AddToolEntity(id, kept, src);
            _ctx?.SetStatus($"体素抽稀完成：{src.Loaded:N0} → {count:N0} 点（{id}）");
            Draw();
        }
        catch (Exception ex) { _ctx?.SetStatus($"体素抽稀失败：{ex.Message}"); }
    }

    // ==================== Undo/Redo（CloudCompare 惯例：Ctrl+Z / Ctrl+Y） ====================

    /// <summary>CloudData → 纯数据快照（浅拷贝：数组引用共享，Xform 克隆）。</summary>
    private static CloudUndo.EntSnapshot CaptureSnapshot(CloudData c) => new()
    {
        Id = c.Id,
        Pts = c.Pts,
        Col = c.Col,
        Normals = c.Normals,
        Xform = (double[])c.Xform.Clone(),
        BoundsMin = c.BoundsMin,
        BoundsMax = c.BoundsMax,
        Count = c.Count,
        Loaded = c.Loaded,
        SelCount = c.SelCount,
        Sel = c.Sel,
        ColorMode = c.ColorMode,
        PointSize = c.PointSize,
        SingleColor = c.SingleColor,
        Locked = c.Locked,
        Source = c.Source,
    };

    /// <summary>删除类操作压栈（删除前快照 → undo 重建）。</summary>
    private void PushRemoveUndo(CloudData data)
    {
        var e = new CloudUndo.UndoEntry { Description = $"删除「{data.Id}」" };
        e.UndoSnapshots.Add(CaptureSnapshot(data));
        e.RedoDeletes.Add(data.Id);
        _undo.Push(e);
    }

    /// <summary>新建类操作压栈（新实体 → undo 删除该实体）。</summary>
    private void PushAddUndo(CloudData created)
    {
        var e = new CloudUndo.UndoEntry { Description = $"新建「{created.Id}」" };
        e.RedoSnapshots.Add(CaptureSnapshot(created));
        e.UndoDeletes.Add(created.Id);
        _undo.Push(e);
    }

    /// <summary>按快照恢复/重建实体（样式/变换/法线/锁定全量还原；同 Id 覆盖，缺失重建）。</summary>
    private void RestoreSnapshot(CloudUndo.EntSnapshot snap)
    {
        if (_clouds.TryGetValue(snap.Id, out var cur))
        {
            _clouds[snap.Id] = new CloudData
            {
                Id = snap.Id, Pts = snap.Pts, Col = snap.Col, Normals = snap.Normals,
                Xform = (double[])snap.Xform.Clone(), BoundsMin = snap.BoundsMin, BoundsMax = snap.BoundsMax,
                Count = snap.Count, Loaded = snap.Loaded, SelCount = snap.SelCount, Sel = snap.Sel,
                ColorMode = snap.ColorMode, PointSize = snap.PointSize, SingleColor = snap.SingleColor,
                Locked = snap.Locked, Source = snap.Source,
            };
            var d = _clouds[snap.Id];
            _renderer.BeginUpload(d.Id, d.Loaded, d.BoundsMin, d.BoundsMax, d.Pts, d.Col, d.Normals);
            _renderer.SetEntityStyle(d.Id, d.ColorMode, d.PointSize, d.SingleColor);
            _renderer.SetEntityTransform(d.Id, d.Xform);
            _renderer.SetEntityLit(d.Id, d.Normals is not null);
            if (d.SelCount > 0) _renderer.SetSelection(d.Id, d.Sel);
        }
        else
        {
            SetCloud(snap.Count, snap.Pts, snap.Col, snap.Source, snap.Id, keepCamera: true);
            if (_clouds.TryGetValue(snap.Id, out var d2))
            {
                d2.Xform = (double[])snap.Xform.Clone();
                d2.ColorMode = snap.ColorMode;
                d2.PointSize = snap.PointSize;
                d2.SingleColor = snap.SingleColor;
                d2.Locked = snap.Locked;
                if (snap.Normals is not null) { d2.Normals = snap.Normals; _renderer.SetNormals(snap.Id, snap.Normals); _renderer.SetEntityLit(snap.Id, true); }
                _renderer.SetEntityTransform(snap.Id, d2.Xform);
                _renderer.SetEntityStyle(snap.Id, d2.ColorMode, d2.PointSize, d2.SingleColor);
            }
        }
    }

    /// <summary>应用 undo/redo entry：先删 RedoDeletes/UndoDeletes 实体，再恢复快照。</summary>
    private void ApplyEntry(CloudUndo.UndoEntry e, List<CloudUndo.EntSnapshot> snaps, List<string> deletes)
    {
        foreach (var id in deletes)
            if (_clouds.ContainsKey(id)) RemoveCloudCore(id);
        foreach (var snap in snaps) RestoreSnapshot(snap);
        UpdateCloudInfoText();
        UpdateMeasureInfo();
        CloudsChanged?.Invoke();
        Draw();
    }

    private void PerformUndo()
    {
        var e = _undo.PopUndo();
        if (e is null) { _ctx?.SetStatus("没有可撤销的操作"); return; }
        ApplyEntry(e, e.UndoSnapshots, e.UndoDeletes);
        _ctx?.SetStatus($"已撤销：{e.Description}（Ctrl+Y 重做）");
    }

    private void PerformRedo()
    {
        var e = _undo.PopRedo();
        if (e is null) { _ctx?.SetStatus("没有可重做的操作"); return; }
        ApplyEntry(e, e.RedoSnapshots, e.RedoDeletes);
        _ctx?.SetStatus($"已重做：{e.Description}");
    }

    /// <summary>聚焦选中实体：视角对准该实体（target=中心、距离=半径×2.5；保持当前朝向）。</summary>
    private void FocusSelectedTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("聚焦需要先有加载完成的点云实体"); return; }
        // 实体世界包围盒（8 角过变换）
        var min = new Vector3(float.MaxValue);
        var max = new Vector3(float.MinValue);
        for (int i = 0; i < 8; i++)
        {
            var lp = new Vector3((i & 1) == 0 ? src.BoundsMin.X : src.BoundsMax.X,
                                 (i & 2) == 0 ? src.BoundsMin.Y : src.BoundsMax.Y,
                                 (i & 4) == 0 ? src.BoundsMin.Z : src.BoundsMax.Z);
            var w = new Vector3(
                (float)(src.Xform[0] * lp.X + src.Xform[1] * lp.Y + src.Xform[2] * lp.Z + src.Xform[3]),
                (float)(src.Xform[4] * lp.X + src.Xform[5] * lp.Y + src.Xform[6] * lp.Z + src.Xform[7]),
                (float)(src.Xform[8] * lp.X + src.Xform[9] * lp.Y + src.Xform[10] * lp.Z + src.Xform[11]));
            min = Vector3.ComponentMin(min, w);
            max = Vector3.ComponentMax(max, w);
        }
        _target = (min + max) / 2;
        _fitRadius = Math.Max(0.01f, (max - min).Length / 2);
        _dist = _fitRadius * 2.5f;
        _travel = 0;
        if (!float.IsFinite(_fitRadius)) _fitRadius = 10f;
        Draw();
        _ctx?.SetStatus($"已聚焦「{src.Id}」（{src.Loaded:N0} 点）");
    }

    private async void RandomSampleTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("此工具需要先有加载完成的点云实体"); return; }
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.random", Name = $"随机下采样 · {src.Id}",
            Description = "均匀随机保留一部分点为新实体（本地处理，不调 DLL）。建议值：预览 0.1、去噪前 0.3",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "ratio", Label = "保留比例(0~1)", Type = "number", Default = "0.3", Min = 0.001, Max = 1 },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;
        double ratio = Convert.ToDouble(res.Values[0], CultureInfo.InvariantCulture);
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            var (outs, _) = await Task.Run(() =>
            {
                var sampled = CloudPointIO.RandomSample(payload, ratio);
                return (new[] { sampled }, (string?)null);
            });
            var id = $"随机采样#{_toolSeq++}";
            AddToolEntity(id, outs[0], src);
            _ctx?.SetStatus($"随机下采样完成：{src.Loaded:N0} → {outs[0].Count:N0} 点（{id}）");
            Draw();
        }
        catch (Exception ex) { _ctx?.SetStatus($"随机下采样失败：{ex.Message}"); }
    }

    private async void OutlierTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("此工具需要先有加载完成的点云实体"); return; }
        if (src.Loaded > 20_000_000)
        {
            _ctx?.SetStatus($"离群点滤波邻域查询成本高——当前 {src.Loaded:N0} 点太大，请先 🎲 抽稀（建议 ≤2000 万）");
            return;
        }
        // 默认半径 = 包围盒对角线的 0.5%（自适应尺度）
        var diag = (src.BoundsMax - src.BoundsMin).Length;
        double defRadius = Math.Max(1e-4, diag * 0.005);
        var op = new CloudToolOps.ToolOp
        {
            Id = "tool.sor", Name = $"离群点滤波 · {src.Id}",
            Description = "半径邻域计数滤波（SOR 的格网加速等价实现）：半径内邻居 < 阈值的孤立点剔除，输出内点为新实体。半径默认=包围盒对角 0.5%；大数据先抽稀再滤波",
            DllPath = "", Entry = "",
            Params = new[]
            {
                new CloudToolOps.ToolParam { Name = "radius", Label = "邻域半径", Type = "number", Default = defRadius.ToString("F4", CultureInfo.InvariantCulture), Min = 1e-6 },
                new CloudToolOps.ToolParam { Name = "min_neighbors", Label = "最少邻居数", Type = "int", Default = "5", Min = 1, Max = 500 },
            },
        };
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op); }
        catch { return; }
        if (res is null) return;
        double radius = Convert.ToDouble(res.Values[0], CultureInfo.InvariantCulture);
        int minN = Convert.ToInt32(res.Values[1], CultureInfo.InvariantCulture);
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            _ctx?.SetStatus($"离群点滤波执行中…（{src.Loaded:N0} 点）");
            var (outs, _) = await Task.Run(() =>
            {
                var (kept, removed) = CloudPointIO.StatisticOutlierRemove(payload, radius, minN);
                return (new[] { kept }, removed);
            });
            var id = $"去噪#{_toolSeq++}";
            AddToolEntity(id, outs[0], src);
            _ctx?.SetStatus($"离群点滤波完成：剔除 {((long)src.Loaded - outs[0].Count):N0} 个离群点，保留 {outs[0].Count:N0} 点（{id}）");
            Draw();
        }
        catch (Exception ex) { _ctx?.SetStatus($"离群点滤波失败：{ex.Message}"); }
    }

    private void MergeTool()
    {
        var sources = _clouds.Values.Where(c => _renderer.IsVisible(c.Id) && c.Loaded > 0)
                                    .OrderBy(c => c.Id, StringComparer.OrdinalIgnoreCase)
                                    .ToList();
        if (sources.Count < 2)
        {
            _ctx?.SetStatus("合并需要至少两个可见的已加载实体（隐藏不需要的实体即可参与选择）");
            return;
        }
        try
        {
            // 世界坐标合并：各实体点先过自身变换（渲染即所见）
            var parts = new List<CloudPayload>();
            foreach (var c in sources)
            {
                var pts = new float[c.Loaded * 3];
                for (int i = 0; i < c.Loaded; i++)
                {
                    pts[i * 3] = (float)(c.Xform[0] * c.Pts[i * 3] + c.Xform[1] * c.Pts[i * 3 + 1] + c.Xform[2] * c.Pts[i * 3 + 2] + c.Xform[3]);
                    pts[i * 3 + 1] = (float)(c.Xform[4] * c.Pts[i * 3] + c.Xform[5] * c.Pts[i * 3 + 1] + c.Xform[6] * c.Pts[i * 3 + 2] + c.Xform[7]);
                    pts[i * 3 + 2] = (float)(c.Xform[8] * c.Pts[i * 3] + c.Xform[9] * c.Pts[i * 3 + 1] + c.Xform[10] * c.Pts[i * 3 + 2] + c.Xform[11]);
                }
                parts.Add(new CloudPayload { Id = c.Id, Count = c.Loaded, Points = pts, ColorsRgb = c.Col });
            }
            var merged = CloudPointIO.Merge(parts);
            var id = $"合并#{_toolSeq++}";
            SetCloud(merged.Count, merged.Points, merged.ColorsRgb, "工具", id);   // 合并结果取景（Xform=单位阵）
            _ctx?.SetStatus($"合并完成：{string.Join(" + ", sources.Select(s => $"{s.Id}({s.Loaded:N0})"))} → {id}（{merged.Count:N0} 点）");
            Draw();
        }
        catch (Exception ex) { _ctx?.SetStatus($"合并失败：{ex.Message}"); }
    }

    private async void ImportTool()
    {
        var top = TopLevel.GetTopLevel(this);
        if (top is null) return;
        var files = await top.StorageProvider.OpenFilePickerAsync(new Avalonia.Platform.Storage.FilePickerOpenOptions
        {
            Title = "导入点云（PLY / XYZ / CSV）",
            AllowMultiple = false,
            FileTypeFilter = new[]
            {
                new Avalonia.Platform.Storage.FilePickerFileType("点云文件") { Patterns = new[] { "*.ply", "*.las", "*.xyz", "*.csv", "*.txt", "*.pts" } },
            },
        });
        var file = files.FirstOrDefault();
        if (file is null) return;
        var path = file.Path.LocalPath;
        _ctx?.SetStatus($"导入中…{System.IO.Path.GetFileName(path)}");
        try
        {
            var payload = await Task.Run(() => CloudPointIO.LoadFrom(path));
            if (payload.Count <= 0) throw new Exception("文件不含有效点");
            var id = string.IsNullOrWhiteSpace(payload.Id) ? $"导入#{_toolSeq++}" : payload.Id;
            SetCloud(payload.Count, payload.Points, payload.ColorsRgb, "导入", id);   // 新数据源取景
            _ctx?.SetStatus($"导入完成：{System.IO.Path.GetFileName(path)} → {id}（{payload.Count:N0} 点" +
                            (payload.ColorsRgb is null ? "，无颜色" : "，含 RGB") + "）");
        }
        catch (Exception ex)
        {
            _ctx?.SetStatus($"导入失败：{ex.Message}");
        }
    }

    private async void ExportTool()
    {
        var src = PickWorkingEntity();
        if (src is null) { _ctx?.SetStatus("此工具需要先有加载完成的点云实体"); return; }
        var top = TopLevel.GetTopLevel(this);
        if (top is null) return;
        var file = await top.StorageProvider.SaveFilePickerAsync(new Avalonia.Platform.Storage.FilePickerSaveOptions
        {
            Title = "导出点云（PLY 含 RGB / XYZ 文本）",
            SuggestedFileName = src.Id,
            DefaultExtension = "ply",
            FileTypeChoices = new[]
            {
                new Avalonia.Platform.Storage.FilePickerFileType("PLY（ascii，含颜色与法线）") { Patterns = new[] { "*.ply" } },
                new Avalonia.Platform.Storage.FilePickerFileType("LAS 1.2") { Patterns = new[] { "*.las" } },
                new Avalonia.Platform.Storage.FilePickerFileType("XYZ 文本") { Patterns = new[] { "*.xyz" } },
            },
        });
        if (file is null) return;
        var path = file.Path.LocalPath;
        _ctx?.SetStatus($"导出中…{System.IO.Path.GetFileName(path)}");
        try
        {
            var payload = new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source };
            float[]? nrm = _renderer.TryGetNormals(src.Id, out var n) && n is not null && n.Length >= payload.Count * 3 ? n : null;
            await Task.Run(() => CloudPointIO.SaveTo(path, payload, nrm));
            _ctx?.SetStatus($"导出完成：{src.Id}({src.Loaded:N0} 点{(nrm is not null ? "，含法线" : "")}) → {path}");
        }
        catch (Exception ex)
        {
            _ctx?.SetStatus($"导出失败：{ex.Message}");
        }
    }

    private string? _workingEntity;                   // 树面板显式指定的工作实体（M7 联动）

    /// <summary>树面板点选实体 → 工作实体跟随（工具条 F/E/S 等作用于该实体）。</summary>
    public void SetWorkingEntity(string? id)
    {
        if (id is not null && _clouds.ContainsKey(id)) _workingEntity = id;
        else _workingEntity = null;
    }

    /// <summary>工作实体：树显式指定 → 有选区者优先 → 第一个可见已加载 → 任意已加载。</summary>
    private CloudData? PickWorkingEntity()
    {
        if (_workingEntity is not null && _clouds.TryGetValue(_workingEntity, out var we) && we.Loaded > 0)
            return we;
        return _clouds.Values.FirstOrDefault(c => c.SelCount > 0 && c.Loaded > 0)
        ?? _clouds.Values.FirstOrDefault(c => _renderer.IsVisible(c.Id) && c.Loaded > 0)
        ?? _clouds.Values.FirstOrDefault(c => c.Loaded > 0);
    }

    /// <summary>点对配准模式：交替拾取两片点云的对应点（第一对锁定移动实体），≥3 对后右键/Enter 应用刚体变换。</summary>
    private void ToggleRegist()
    {
        if (_registMode) { EndRegistMode(); return; }
        if (_measureMode) ToggleMeasure();
        if (_polyMode) EndPolyMode();
        _registMode = true;
        _registPending = null;
        _registMovingCloud = null;
        _registPairs.Clear();
        RebuildRegistLayer();
        if (_registBtn is not null) _registBtn.Background = ActiveTintMagenta;
        UpdateModeCursor();
        _ctx?.SetStatus("点对配准：Ctrl+左键在移动点云拾取第一点，再在参考点云拾取对应点……（≥3 对后右键/Enter 应用）");
    }

    private void EndRegistMode()
    {
        _registMode = false;
        _registPending = null;
        _registMovingCloud = null;
        _registPairs.Clear();
        RebuildRegistLayer();
        if (_registBtn is not null) _registBtn.ClearValue(Button.BackgroundProperty);
        UpdateModeCursor();
        Draw();
    }

    /// <summary>配准拾取流水：pending 空 = 拾取移动侧并锁定实体；否则必须在另一片点云拾取对应参考点成对。</summary>
    private void RegistCapture(string cloudId, int index)
    {
        if (_registPending is not { } pending)
        {
            _registMovingCloud ??= cloudId;
            if (cloudId != _registMovingCloud)
            {
                _ctx?.SetStatus($"配准约定：第一对拾取的「{_registMovingCloud}」为移动实体——请在它上面拾取移动点");
                return;
            }
            _registPending = (cloudId, index);
            return;
        }
        if (cloudId == _registMovingCloud)
        {
            _ctx?.SetStatus("请在另一片点云（参考云）上拾取对应点——同一片内的点无法配准");
            return;
        }
        // 侧别锁定：首对确定 移动云（Pending 侧）与参考云（成对侧）——此后所有对的两侧必须同云，防跨对混乱
        var refCloud = _registPairs.Count > 0 ? _registPairs[0].Ref.Cloud : null;
        if (refCloud is not null && cloudId != refCloud)
        {
            _ctx?.SetStatus($"参考云锁定为「{refCloud}」——请在同一参考云上拾取对应点（多实体逐个配准）");
            return;
        }
        _registPairs.Add((pending, (cloudId, index)));
        _registPending = null;
        RebuildRegistLayer();
        var pm = WorldOf(pending.Cloud, pending.Index);
        var pr = WorldOf(cloudId, index);
        if (pm is { } a && pr is { } b)
            _ctx?.SetStatus($"配对 {_registPairs.Count}：|Δ|={Vector3.Distance(a, b):F3}（≥3 对后右键/Enter 应用粗配准）");
        Draw();
    }

    /// <summary>应用点对配准：世界坐标收集 → Horn 刚体解 → 移动实体 Xform 复合（M×旧）→ 状态栏报 RMS。</summary>
    private void ApplyRegistPairs()
    {
        if (_registPairs.Count < 3)
        {
            _ctx?.SetStatus($"点对配准需要 ≥3 对对应点（当前 {_registPairs.Count}）——继续交替拾取，或 Esc 退出");
            return;
        }
        try
        {
            var mv = new float[_registPairs.Count * 3];
            var rf = new float[_registPairs.Count * 3];
            for (int i = 0; i < _registPairs.Count; i++)
            {
                var pm = WorldOf(_registPairs[i].Moving.Cloud, _registPairs[i].Moving.Index);
                var pr = WorldOf(_registPairs[i].Ref.Cloud, _registPairs[i].Ref.Index);
                if (pm is not { } a || pr is not { } b) throw new Exception("配对点已失效（实体被删或点数变化）");
                mv[i * 3] = a.X; mv[i * 3 + 1] = a.Y; mv[i * 3 + 2] = a.Z;
                rf[i * 3] = b.X; rf[i * 3 + 1] = b.Y; rf[i * 3 + 2] = b.Z;
            }
            var (m, rms) = CloudRegist.ComputeRigid(mv, rf);
            if (!_clouds.TryGetValue(_registMovingCloud!, out var moving))
                throw new Exception("移动实体不存在");
            var nx = new double[16];
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                {
                    double s = 0;
                    for (int k = 0; k < 4; k++) s += m[i * 4 + k] * moving.Xform[k * 4 + j];
                    nx[i * 4 + j] = s;
                }
            moving.Xform = nx;
            _renderer.SetEntityTransform(moving.Id, moving.Xform);
            int pairs = _registPairs.Count;
            string movingName = moving.Id;
            EndRegistMode();
            Draw();
            CloudsChanged?.Invoke();
            _ctx?.SetStatus($"点对粗配准完成：{pairs} 对 → RMS {rms:F4}（「{movingName}」已移动）——可再跑 ⚙「手动配准(ICP)」精配");
        }
        catch (Exception ex)
        {
            _ctx?.SetStatus($"点对配准失败：{ex.Message}");
        }
    }

    private void RebuildRegistLayer()
    {
        _registLayer.Children.Clear();
        foreach (var _ in _registPairs)
            _registLayer.Children.Add(new Line { Stroke = RegistBrush, StrokeThickness = 1.5, IsVisible = false });
    }

    /// <summary>配对线每帧重投影（Draw 调用，洋红：移动点 → 参考点）。</summary>
    private void UpdateRegistLines()
    {
        var mvp = _renderer.LastMvp;
        double ProjX(Vector3 w)
        {
            float cw = mvp.M14 * w.X + mvp.M24 * w.Y + mvp.M34 * w.Z + mvp.M44;
            if (cw <= 1e-6f) return double.NaN;
            float cc = mvp.M11 * w.X + mvp.M21 * w.Y + mvp.M31 * w.Z + mvp.M41;
            return (cc / cw + 1) * 0.5;
        }
        double ProjY(Vector3 w)
        {
            float cw = mvp.M14 * w.X + mvp.M24 * w.Y + mvp.M34 * w.Z + mvp.M44;
            if (cw <= 1e-6f) return double.NaN;
            float cc = mvp.M12 * w.X + mvp.M22 * w.Y + mvp.M32 * w.Z + mvp.M42;
            return 0.5 - cc / cw * 0.5;
        }
        for (int i = 0; i < _registPairs.Count; i++)
        {
            var line = (Line)_registLayer.Children[i];
            var pm = WorldOf(_registPairs[i].Moving.Cloud, _registPairs[i].Moving.Index);
            var pr = WorldOf(_registPairs[i].Ref.Cloud, _registPairs[i].Ref.Index);
            if (pm is not { } a || pr is not { } b)
            { line.IsVisible = false; continue; }
            float cw0 = mvp.M14 * a.X + mvp.M24 * a.Y + mvp.M34 * a.Z + mvp.M44;
            float cw1 = mvp.M14 * b.X + mvp.M24 * b.Y + mvp.M34 * b.Z + mvp.M44;
            if (cw0 <= 1e-6f || cw1 <= 1e-6f) { line.IsVisible = false; continue; }
            double ax = (mvp.M11 * a.X + mvp.M21 * a.Y + mvp.M31 * a.Z + mvp.M41) / cw0, ay = (mvp.M12 * a.X + mvp.M22 * a.Y + mvp.M32 * a.Z + mvp.M42) / cw0;
            double bx = (mvp.M11 * b.X + mvp.M21 * b.Y + mvp.M31 * b.Z + mvp.M41) / cw1, by = (mvp.M12 * b.X + mvp.M22 * b.Y + mvp.M32 * b.Z + mvp.M42) / cw1;
            ax = (ax + 1) * 0.5; ay = 0.5 - ay * 0.5; bx = (bx + 1) * 0.5; by = 0.5 - by * 0.5;
            if (ax < 0 || ax > 1 || ay < 0 || ay > 1 || bx < 0 || bx > 1 || by < 0 || by > 1)
            { line.IsVisible = false; continue; }
            line.IsVisible = true;
            line.StartPoint = new Point(ax * _vpW / _scaling, ay * _vpH / _scaling);
            line.EndPoint = new Point(bx * _vpW / _scaling, by * _vpH / _scaling);
        }
    }

    /// <summary>执行工具算子：单输入 = 有选区者优先（重发全量点），否则第一个可见实体；
    /// 双输入（对比/配准）= 对话框选基准/被测（预选：有选区者与其余首个可见实体）。
    /// 对话框取消/执行失败仅提示；输出点云逐个成新实体，成功消息进状态栏。</summary>
    private async void RunToolOp(CloudToolOps.ToolOp op)
    {
        var sel = _clouds.Values.FirstOrDefault(c => c.SelCount > 0 && c.Loaded > 0);
        var visible = _clouds.Values.Where(c => _renderer.IsVisible(c.Id) && c.Loaded > 0).ToList();
        var src = sel ?? visible.FirstOrDefault() ?? _clouds.Values.FirstOrDefault(c => c.Loaded > 0);
        if (src is null)
        {
            _ctx?.SetStatus($"「{op.Name}」需要先有加载完成的点云实体");
            return;
        }

        // 双实体候选（加载完成的可见实体，选区优先排序）
        List<string>? choices = op.CloudInputs == 2
            ? visible.Select(c => c.Id).ToList()
            : null;
        if (choices is not null && choices.Count < 2)
        {
            _ctx?.SetStatus($"「{op.Name}」需要两个可见的已加载点云实体（基准/被测）——当前只有 {choices.Count} 个");
            return;
        }
        if (choices is not null && sel is not null)
        {
            choices.Remove(src.Id);
            choices.Insert(0, src.Id);
        }

        var prefill = BuildRoiPrefill(op, src);
        var win = TopLevel.GetTopLevel(this) as Window;
        ToolParamResult? res;
        try { res = await ToolParamDialog.ShowAsync(win, op, prefill, choices, (sel?.Id, null)); }
        catch { return; }
        if (res is null) return;

        var inputs = new List<CloudPayload>();
        if (op.CloudInputs == 2 && choices is not null)
        {
            foreach (var cid in new[] { res.CloudA, res.CloudB })
            {
                if (cid is null || !_clouds.TryGetValue(cid, out var c) || c.Loaded <= 0)
                {
                    _ctx?.SetStatus($"「{op.Name}」基准/被测实体无效");
                    return;
                }
                inputs.Add(new CloudPayload { Id = c.Id, Count = c.Loaded, Points = c.Pts, ColorsRgb = c.Col, Source = c.Source });
            }
        }
        else
        {
            inputs.Add(new CloudPayload { Id = src.Id, Count = src.Loaded, Points = src.Pts, ColorsRgb = src.Col, Source = src.Source });
        }

        _ctx?.SetStatus($"「{op.Name}」执行中…（{string.Join(" vs ", inputs.Select(i => i.Id))}）");
        try
        {
            var (outs, msg) = await Task.Run(() => _tools.Invoke(op, inputs, res.Values));
            var created = new List<string>();
            foreach (var oc in outs)
            {
                if (oc.Count <= 0 || oc.Points.Length < oc.Count * 3) continue;
                var id = $"{op.Name}#{_toolSeq++}";
                SetCloud(oc.Count, oc.Points, oc.ColorsRgb, "工具", id);
                created.Add($"{id}({oc.Count:N0})");
            }
            _ctx?.SetStatus($"「{op.Name}」完成：{created.Count} 个输出" + (created.Count > 0 ? $" {string.Join("、", created)}" : "")
                            + (msg is null ? "" : $" | {msg}"));
        }
        catch (Exception ex)
        {
            _ctx?.SetStatus($"「{op.Name}」失败：{ex.Message}");
        }
    }

    /// <summary>工具参数预填：①ROI 类（x/y/w/h、roi_*）= 最近框选的世界包围盒；②剖面类（x0/y0/x1/y1）=
    /// 最后一条量测线两端点的世界坐标。无可用来源返回 null（对话框用声明默认值）。</summary>
    private Dictionary<string, string>? BuildRoiPrefill(CloudToolOps.ToolOp op, CloudData src)
    {
        var map = new Dictionary<string, string>();
        if (_lastSelBounds is { } r)
        {
            void Fill(string n, string? v)
            {
                if (v is not null && op.Params.Any(p => p.Name == n)) map[n] = v;
            }
            Fill("x", r.x.ToString("F3", CultureInfo.InvariantCulture));
            Fill("roi_x", r.x.ToString("F3", CultureInfo.InvariantCulture));
            Fill("y", r.y.ToString("F3", CultureInfo.InvariantCulture));
            Fill("roi_y", r.y.ToString("F3", CultureInfo.InvariantCulture));
            Fill("w", r.w.ToString("F3", CultureInfo.InvariantCulture));
            Fill("roi_w", r.w.ToString("F3", CultureInfo.InvariantCulture));
            Fill("width", r.w.ToString("F3", CultureInfo.InvariantCulture));
            Fill("h", r.h.ToString("F3", CultureInfo.InvariantCulture));
            Fill("roi_h", r.h.ToString("F3", CultureInfo.InvariantCulture));
            Fill("height", r.h.ToString("F3", CultureInfo.InvariantCulture));
            Fill("z_low", r.z.ToString("F3", CultureInfo.InvariantCulture));
            Fill("z_high", (r.z + 0.1).ToString("F3", CultureInfo.InvariantCulture));
        }
        if (_measurements.Count > 0 && op.Params.Any(p => p.Name is "x0" or "x1"))
        {
            var last = _measurements[^1];
            var pa = WorldOf(last.Points[0].Cloud, last.Points[0].Index);
            var pb = WorldOf(last.Points[^1].Cloud, last.Points[^1].Index);
            if (pa is { } a && pb is { } b)
            {
                map["x0"] = a.X.ToString("F3", CultureInfo.InvariantCulture);
                map["y0"] = a.Y.ToString("F3", CultureInfo.InvariantCulture);
                map["x1"] = b.X.ToString("F3", CultureInfo.InvariantCulture);
                map["y1"] = b.Y.ToString("F3", CultureInfo.InvariantCulture);
            }
        }
        return map.Count > 0 ? map : null;
    }

    // ==================== 渲染呈现 ====================

    /// <summary>模式光标统一（任一拾取/量测/配准模式开 = Cross；退出恢复 Default）。</summary>
    private void UpdateModeCursor()
    {
        _viewArea.Cursor = (_measureMode || _polyMode || _registMode)
            ? new Cursor(StandardCursorType.Cross)
            : new Cursor(StandardCursorType.Arrow);
    }

    private void Draw()
    {
        if (!HasCloud || !_renderer.Ok || _vpW <= 0 || _vpH <= 0) return;
        if (_wb is null || _vpW != _wb.PixelSize.Width || _vpH != _wb.PixelSize.Height)
        {
            _wb?.Dispose();
            _wb = new WriteableBitmap(new PixelSize(_vpW, _vpH), new Vector(96 * _scaling, 96 * _scaling),
                PixelFormats.Bgra8888, AlphaFormat.Opaque);
            _image.Source = _wb;
        }

        bool isOrtho = _projCombo.SelectedIndex == 1;
        // 屏显密度封顶：可见点数超过视口像素数（每像素至多 1 点）时按比例加大抽稀间隔
        long pixels = (long)_vpW * _vpH;
        long visible = _renderer.VisiblePoints;
        int densityStride = pixels > 0 && visible > pixels ? (int)Math.Ceiling(visible / (double)pixels) : 1;
        int baseStride = EffStride;
        int stride = Math.Max(baseStride, Math.Min(densityStride, 1000));
        _renderer.Draw(_vpW, _vpH, _orientation, (float)(_dist * _scaling), _target, isOrtho,
            stride, _frame, _farClip, (float)(_fitRadius * _scaling), (float)(_travel * _scaling));

        using var fb = _wb.Lock();
        nint dst = fb.Address;
        int rowBytes = _vpW * 4;
        // GL 回读自底向上、位图自顶向下：逐行倒序拷贝（否则显示垂直翻转）
        for (int y = 0; y < _vpH; y++)
            Marshal.Copy(_frame, (_vpH - 1 - y) * rowBytes, (nint)(dst + (long)y * fb.RowBytes), rowBytes);
        _image.InvalidateVisual();

        UpdatePickMark();
        UpdateTripod();
        UpdateMeasurements();
        UpdateRegistLines();
        UpdateModeCursor();
        UpdateEmptyState();

    }

    /// <summary>空态提示（无实体且 GL 正常时居中次级引导；有云即隐藏）。</summary>
    private void UpdateEmptyState()
    {
        _emptyState.IsVisible = !HasCloud && _renderer.Ok;
    }

    public void Dispose()
    {
        _busSub?.Dispose();
        _busSub = null;
        _busSubRemove?.Dispose();
        _busSubRemove = null;
        _tools.Dispose();   // 算子工具 invoker（原生库句柄）
        _renderer.Dispose();
        _wb?.Dispose();
        _wb = null;
    }
}
