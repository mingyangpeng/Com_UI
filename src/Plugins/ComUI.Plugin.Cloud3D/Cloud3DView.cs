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

    private sealed class CloudData
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
    private readonly TextBlock _info = new() { FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap };
    private readonly ComboBox _projCombo = new() { Classes = { "input" }, MinWidth = 96 };
    private readonly ComboBox _presetCombo = new() { Classes = { "input" }, MinWidth = 96 };
    private readonly TextBox _strideBox = new() { Classes = { "input" }, MinWidth = 56, Text = "1" };
    private readonly TextBlock _cloudInfo = new() { FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _pickInfo = new() { FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap, Text = "无（Ctrl+左键单击拾取）" };
    private readonly TextBlock _selInfo = new() { FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap, Text = "无（Ctrl+左键拖拽框选）" };
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
        var snap = new CloudSnapshotView(ents, _orientation, _dist, _target);
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
            // 内嵌模式（draw 预览卡片用）：不订阅总线（数据由上游直喂 SetCloud），隐藏信息栏
            _embed = true;
            _info.IsVisible = false;
        }

        _projCombo.Items.Add("透视投影");
        _projCombo.Items.Add("正交投影");
        _projCombo.SelectedIndex = 0;
        _projCombo.SelectionChanged += (_, _) => Draw();

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
                return true;
            },
            () => _stride.ToString());

        var resetBtn = new Button { Classes = { "ghost" }, Content = "重置视角", MinWidth = 76 };
        resetBtn.Click += (_, _) => ResetView();
        var clearBtn = new Button { Classes = { "ghost" }, Content = "清空选区", MinWidth = 76 };
        clearBtn.Click += (_, _) => { _pickedCloud = null; _pickedIndex = -1; ClearSelection(); Draw(); };

        _paramsContent = BuildParams(resetBtn, clearBtn);

        var toolbar = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Margin = new Thickness(10, 8, 8, 4),
        };
        if (!_embed && ctx is not null)
        {
            var snapBtn = new Button { Classes = { "ghost" }, Content = "📷 快照", Cursor = new Cursor(StandardCursorType.Hand), MinWidth = 76 };
            snapBtn.Click += (_, _) => Snapshot();
            ToolTip.SetTip(snapBtn, "把当前可见点云与视角冻结为只读快照标签页（用于对比；不随数据更新，不进会话）");
            toolbar.Children.Add(snapBtn);

            // 数据源切换：列出总线上有数据的 cloud/* 主题（cloud/removed 为控制主题不列出）
            ToolTip.SetTip(_sourceCombo, "切换工作台订阅的点云主题");
            _sourceCombo.DropDownOpened += (_, _) => RefreshSourceTopics();
            _sourceCombo.SelectionChanged += OnSourceSelected;
            RefreshSourceTopics();
            toolbar.Children.Add(new TextBlock { Text = "数据源", FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
            toolbar.Children.Add(_sourceCombo);
        }
        toolbar.Children.Add(_info);

        _overlay.Children.Add(_rubber);
        _overlay.Children.Add(_pickMark);
        _tripod.Children.Add(new Ellipse { Width = 72, Height = 72, Fill = new SolidColorBrush(Color.FromArgb(0x55, 0, 0, 0)) });
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
            if (e.Key != Key.Escape) return;
            _pickedCloud = null;
            _pickedIndex = -1;
            ClearSelection();
            Draw();
            e.Handled = true;
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        root.Children.Add(toolbar);
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

    private StackPanel BuildParams(Button resetBtn, Button clearBtn)
    {
        var panel = new StackPanel { Margin = new Thickness(14, 4, 10, 0), Spacing = 2 };
        panel.Children.Add(Section("视角控制"));
        panel.Children.Add(Row("投影", _projCombo));
        panel.Children.Add(Row("抽稀间隔", _strideBox));
        panel.Children.Add(Row("视角预设", _presetCombo));
        panel.Children.Add(Row("视角", resetBtn));
        panel.Children.Add(Section("选区"));
        panel.Children.Add(RowText(_selInfo));
        panel.Children.Add(Row("清空", clearBtn));
        panel.Children.Add(Section("点云"));
        panel.Children.Add(RowText(_cloudInfo));
        panel.Children.Add(RowText(_pickInfo));
        panel.Children.Add(RowText(new TextBlock
        {
            Text = "颜色/点大小/变换矩阵在左侧点云树详情区按实体设置",
            FontSize = 11.5, Foreground = Dim, TextWrapping = TextWrapping.Wrap,
        }));
        return panel;
    }

    private static Control Section(string title) => new Border
    {
        Background = new SolidColorBrush(Color.FromRgb(0x2F, 0x2F, 0x33)),
        CornerRadius = new CornerRadius(3),
        Margin = new Thickness(0, 8, 0, 3),
        Padding = new Thickness(8, 3),
        Child = new TextBlock { Text = title, FontWeight = FontWeight.SemiBold, FontSize = 12 },
    };

    private static Control Row(string label, Control editor)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("64,*"), Margin = new Thickness(0, 2) };
        grid.Children.Add(new TextBlock { Text = label, FontSize = 12.5, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
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

    private void HookCommit(TextBox box, Func<double, bool> apply, Func<string> currentText)
    {
        void Commit()
        {
            if (double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) && apply(v))
                return;
            box.Text = currentText();
            box.CaretIndex = box.Text?.Length ?? 0;
        }
        box.KeyDown += (_, e) => { if (e.Key == Key.Enter) { Commit(); e.Handled = true; } };
        box.LostFocus += (_, _) => Commit();
    }

    // ==================== 点云实体管理 ====================

    /// <summary>同 Id 重发=更新（重新分块上传），新 Id=新增并取景。</summary>
    public void SetCloud(int count, float[] points, byte[]? colorsRgb, string? source, string? id)
    {
        if (!_renderer.Ok || count <= 0 || points.Length < count * 3) return;
        string cloudId = string.IsNullOrWhiteSpace(id) ? "点云" : id.Trim();
        bool isNew = !_clouds.ContainsKey(cloudId);
        var data0 = isNew ? null : _clouds[cloudId];

        // 采样估算边界（步长 64）：仅用于立即取景；精确边界由分块上传逐点合并
        var min = new Vector3(float.MaxValue);
        var max = new Vector3(float.MinValue);
        for (int i = 0; i < count; i += 64)
        {
            min = Vector3.ComponentMin(min, new Vector3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]));
            max = Vector3.ComponentMax(max, new Vector3(points[i * 3], points[i * 3 + 1], points[i * 3 + 2]));
        }
        min = Vector3.ComponentMin(min, new Vector3(points[0], points[1], points[2]));
        max = Vector3.ComponentMax(max, new Vector3(points[0], points[1], points[2]));

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
        _clouds[cloudId] = data;
        _renderer.SetEntityStyle(cloudId, data.ColorMode, data.PointSize, data.SingleColor);
        _renderer.SetEntityTransform(cloudId, data.Xform);

        if (isNew)
        {
            _target = (min + max) / 2;
            _fitRadius = Math.Max(0.01f, (max - min).Length / 2);
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

        _renderer.BeginUpload(cloudId, count, min, max, data.Pts, data.Col);
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
                min = Vector3.ComponentMin(min, new Vector3(data.Pts[i * 3], data.Pts[i * 3 + 1], data.Pts[i * 3 + 2]));
                max = Vector3.ComponentMax(max, new Vector3(data.Pts[i * 3], data.Pts[i * 3 + 1], data.Pts[i * 3 + 2]));
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

    /// <summary>移除点云实体（锁定=拒绝并提示，返回 false——树删除/Del/总线移除指令/切源清理统一走此闸）。</summary>
    public bool RemoveCloud(string id)
    {
        if (!_clouds.TryGetValue(id, out var data)) return false;
        if (data.Locked)
        {
            _ctx?.SetStatus($"「{id}」已锁定，先解锁再删除");
            return false;
        }
        _clouds.Remove(id);
        _renderer.RemoveEntity(id);
        if (_pickedCloud == id) { _pickedCloud = null; _pickedIndex = -1; UpdatePickMark(); }
        UpdateCloudInfoText();
        Draw();
        CloudsChanged?.Invoke();
        return true;
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
        };
        _clouds[newId] = data;
        _renderer.BeginUpload(newId, data.Count, data.BoundsMin, data.BoundsMax, data.Pts, data.Col);
        _renderer.SetEntityStyle(newId, data.ColorMode, data.PointSize);
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

    private void UpdateCloudInfoText() =>
        _cloudInfo.Text = _clouds.Count == 0
            ? "无"
            : $"{_clouds.Count} 实体 · 共 {_clouds.Values.Sum(c => c.Count):N0} 点" +
              (_clouds.Values.Any(c => c.Loaded < c.Count) ? "（上传中）" : "");

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
        Draw();
    }

    // ==================== 交互 ====================

    private void OnWheel(object? sender, PointerWheelEventArgs e)
    {
        if (!HasCloud) return;
        MarkInteract();
        _dist = Math.Clamp(_dist * (e.Delta.Y > 0 ? 0.9 : 1 / 0.9), _fitRadius * 0.05, _fitRadius * 30);
        Draw();
        e.Handled = true;
    }

    private void OnPress(object? sender, PointerPressedEventArgs e)
    {
        var pt = e.GetCurrentPoint(_viewArea);
        Focus();   // 让 Esc 清选区生效
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
            var right = Vector3.Transform(Vector3.UnitX, _orientation);
            var qPitch = Quaternion.FromAxisAngle(right, MathHelper.DegreesToRadians((float)dyDeg));
            _orientation = Quaternion.Normalize(qPitch * qYaw * _orientation);
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
        if (_pickedIndex < 0 || _pickedCloud is null || !_clouds.TryGetValue(_pickedCloud, out var c))
        {
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

    // ==================== 渲染呈现 ====================

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
            stride, _frame);

        using var fb = _wb.Lock();
        nint dst = fb.Address;
        int rowBytes = _vpW * 4;
        // GL 回读自底向上、位图自顶向下：逐行倒序拷贝（否则显示垂直翻转）
        for (int y = 0; y < _vpH; y++)
            Marshal.Copy(_frame, (_vpH - 1 - y) * rowBytes, (nint)(dst + (long)y * fb.RowBytes), rowBytes);
        _image.InvalidateVisual();

        UpdatePickMark();
        UpdateTripod();

        string src = string.Join(" · ", _clouds.Values.Select(c => c.Source).Where(s => s is not null).Distinct());
        string strideNote = "";
        if (stride > 1)
        {
            string why = densityStride > baseStride && stride == Math.Min(densityStride, 1000) ? "（密度封顶）"
                : _interacting && stride > _stride ? "（交互）" : "";
            strideNote = $" · 抽稀×{stride:N0}{why}";
        }
        long total = _clouds.Values.Sum(c => (long)c.Count);
        long loaded = _clouds.Values.Sum(c => (long)c.Loaded);
        string uploadNote = loaded < total ? $" · 上传中 {loaded * 100 / Math.Max(1, total)}%" : "";
        _info.Text = $"左键旋转 · 滚轮缩放 · 右键平移 · Ctrl+左键 拾取/框选 · Esc 清选{strideNote}{uploadNote} · {_renderer.Backend}{(src.Length > 0 ? " · " + src : "")}";
    }

    public void Dispose()
    {
        _busSub?.Dispose();
        _busSub = null;
        _busSubRemove?.Dispose();
        _busSubRemove = null;
        _renderer.Dispose();
        _wb?.Dispose();
        _wb = null;
    }
}
