using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using OpenTK.Mathematics;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 快照视图（对比用，只读）：冻结某一时刻的可见实体（点/色数组引用共享——总线载荷约定不可变）
/// 与工作台当时的相机姿态；仅保留查看导航——左键旋转、滚轮缩放、右键平移、透视/正交、重置视角、XYZ 指示器。
/// 无拾取/框选/抽稀参数/选区发布，不订阅总线——后续数据更新与快照无关。
/// 上传走同步分块路径（禁后台 GL worker：多 worker 共享上下文曾互卡死锁）。
/// 以动态文档标签打开（restoreKey=null，不进会话，重启丢弃）。
/// </summary>
public sealed class CloudSnapshotView : UserControl, IDisposable
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    /// <summary>快照实体：数据数组引用共享（不克隆），Count 已按工作台当前揭示进度截取。</summary>
    public sealed record SnapEntity(string Id, float[] Pts, byte[]? Col, int Count,
        Vector3 BoundsMin, Vector3 BoundsMax, int ColorMode, float PointSize, Vector3 SingleColor, double[] Xform);

    private const int UploadChunkPoints = 2_000_000;   // 分块上传每块点数（与工作台一致）

    private readonly GlCloudRenderer _renderer;
    private readonly Image _image = new() { Stretch = Stretch.None, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
    private readonly Border _viewArea;
    private readonly TextBlock _info = new() { FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center };
    private readonly ComboBox _projCombo = new() { Classes = { "input" }, MinWidth = 96 };

    // 右下角 XYZ 坐标轴指示器（与工作台同款）
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

    private sealed class SnapData
    {
        public required string Id;
        public required float[] Pts;
        public byte[]? Col;
        public int Count;
        public int Loaded;
        public Vector3 BoundsMin, BoundsMax;
        public double[] Xform = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    }

    private readonly List<SnapData> _clouds = new();
    private WriteableBitmap? _wb;
    private byte[] _frame = Array.Empty<byte>();
    private double _scaling = 1;
    private int _vpW, _vpH;

    // 相机（继承工作台当时姿态）
    private Quaternion _orientation;
    private double _dist;
    private Vector3 _target;
    private float _fitRadius = 5;

    private Point? _orbitLast;
    private Point? _panLast;
    private Avalonia.Threading.DispatcherTimer? _uploadTimer;

    private bool HasCloud => _clouds.Count > 0;

    public CloudSnapshotView(IReadOnlyList<SnapEntity> entities, Quaternion orientation, double dist, Vector3 target)
    {
        // 同步上传路径（embed 同款）：多后台 GL worker 共享上下文曾互卡死锁
        _renderer = new GlCloudRenderer(allowBackgroundWorker: false);
        _orientation = orientation;
        _dist = dist;
        _target = target;

        Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20));
        Focusable = true;

        foreach (var e in entities)
        {
            if (e.Count <= 0 || e.Pts.Length < e.Count * 3) continue;
            var data = new SnapData
            {
                Id = e.Id,
                Pts = e.Pts,
                Col = e.Col is not null && e.Col.Length >= e.Count * 3 ? e.Col : null,
                Count = e.Count,
                BoundsMin = e.BoundsMin,
                BoundsMax = e.BoundsMax,
                Xform = (double[])e.Xform.Clone(),
            };
            _clouds.Add(data);
            _renderer.SetEntityStyle(e.Id, e.ColorMode, e.PointSize, e.SingleColor);
            _renderer.SetEntityTransform(e.Id, data.Xform);
            _renderer.BeginUpload(e.Id, data.Count, data.BoundsMin, data.BoundsMax, data.Pts, data.Col);
            float radius = (data.BoundsMax - data.BoundsMin).Length / 2;
            if (radius > _fitRadius) _fitRadius = radius;
        }
        _fitRadius = Math.Max(0.01f, _fitRadius);

        var resetBtn = new Button { Classes = { "ghost" }, Content = "重置视角", MinWidth = 76 };
        resetBtn.Click += (_, _) => ResetView();
        _projCombo.Items.Add("透视投影");
        _projCombo.Items.Add("正交投影");
        _projCombo.SelectedIndex = 0;
        _projCombo.SelectionChanged += (_, _) => Draw();

        var toolbar = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Margin = new Thickness(10, 8, 8, 4),
            Children = { resetBtn, _projCombo, _info },
        };

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
            Child = new Grid { Children = { _image, _tripod } },
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

        if (_clouds.Count > 0)
        {
            _uploadTimer = new Avalonia.Threading.DispatcherTimer { Interval = TimeSpan.FromMilliseconds(30) };
            _uploadTimer.Tick += UploadTick;
            _uploadTimer.Start();
        }
    }

    // ==================== 分块上传（同步路径，UI 线程逐块） ====================

    private void UploadTick(object? sender, EventArgs e)
    {
        var data = _clouds.FirstOrDefault(c => c.Loaded < c.Count);
        if (data is null || !_renderer.Ok) { _uploadTimer?.Stop(); return; }
        int n = Math.Min(UploadChunkPoints, data.Count - data.Loaded);
        // 同步路径逐点合并精确边界（初始为采样估计；UV/取景需要精确值——与工作台同规则）
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
        Draw();
    }

    // ==================== 相机（只读导航） ====================

    /// <summary>先绕 X 俯仰、再绕 Y 偏航得到的相机姿态（与工作台 ResetView 同一构造方式）。</summary>
    private static Quaternion ViewQuat(double yawDeg, double pitchDeg) => Quaternion.Normalize(
        Quaternion.FromAxisAngle(Vector3.UnitY, MathHelper.DegreesToRadians((float)yawDeg)) *
        Quaternion.FromAxisAngle(Vector3.UnitX, MathHelper.DegreesToRadians((float)pitchDeg)));

    private void ResetView()
    {
        _orientation = ViewQuat(30, -15);
        _dist = _fitRadius * 2.5f;
        Draw();
    }

    private void OnWheel(object? sender, PointerWheelEventArgs e)
    {
        if (!HasCloud) return;
        _dist = Math.Clamp(_dist * (e.Delta.Y > 0 ? 0.9 : 1 / 0.9), _fitRadius * 0.05, _fitRadius * 30);
        Draw();
        e.Handled = true;
    }

    private void OnPress(object? sender, PointerPressedEventArgs e)
    {
        var pt = e.GetCurrentPoint(_viewArea);
        Focus();
        if (pt.Properties.IsLeftButtonPressed) { _orbitLast = pt.Position; _panLast = null; }
        else if (pt.Properties.IsRightButtonPressed) { _panLast = pt.Position; _orbitLast = null; }
        else return;
        e.Pointer.Capture(_viewArea);
        e.Handled = true;
    }

    private void OnMove(object? sender, PointerEventArgs e)
    {
        var pos = e.GetPosition(_viewArea);
        var pt = e.GetCurrentPoint(_viewArea);
        if (_orbitLast is { } o && pt.Properties.IsLeftButtonPressed)
        {
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
            double dx = (pos.X - p.X) * _scaling;
            double dy = (pos.Y - p.Y) * _scaling;
            var right = Vector3.Transform(Vector3.UnitX, _orientation);
            var up = Vector3.Transform(Vector3.UnitY, _orientation);
            double s = _dist * 0.0016;
            // grab 语义（相机反向移动）：拖右 → 云右；拖下 → 云下
            _target += (-right * (float)dx + up * (float)dy) * (float)s;
            _panLast = pos;
            Draw();
        }
    }

    private void OnRelease(object? sender, PointerReleasedEventArgs e)
    {
        _orbitLast = null;
        _panLast = null;
        e.Pointer.Capture(null);
        e.Handled = true;
    }

    // ==================== XYZ 指示器 ====================

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
        int stride = pixels > 0 && visible > pixels ? (int)Math.Ceiling(visible / (double)pixels) : 1;
        stride = Math.Min(stride, 1000);
        _renderer.Draw(_vpW, _vpH, _orientation, (float)(_dist * _scaling), _target, isOrtho,
            stride, _frame);

        using var fb = _wb.Lock();
        nint dst = fb.Address;
        int rowBytes = _vpW * 4;
        // GL 回读自底向上、位图自顶向下：逐行倒序拷贝（否则显示垂直翻转）
        for (int y = 0; y < _vpH; y++)
            Marshal.Copy(_frame, (_vpH - 1 - y) * rowBytes, (nint)(dst + (long)y * fb.RowBytes), rowBytes);
        _image.InvalidateVisual();

        UpdateTripod();

        long total = _clouds.Sum(c => (long)c.Count);
        long loaded = _clouds.Sum(c => (long)c.Loaded);
        string uploadNote = loaded < total ? $" · 上传中 {loaded * 100 / Math.Max(1, total)}%" : "";
        string strideNote = stride > 1 ? $" · 抽稀×{stride:N0}（密度封顶）" : "";
        string src = string.Join(" · ", _clouds.Select(c => c.Id).Distinct());
        _info.Text = $"快照 {_clouds.Count} 实体 · {total:N0} 点 · 左键旋转 · 滚轮缩放 · 右键平移{strideNote}{uploadNote} · {_renderer.Backend} · {src}";
    }

    public void Dispose()
    {
        _uploadTimer?.Stop();
        _renderer.Dispose();
        _wb?.Dispose();
        _wb = null;
    }
}
