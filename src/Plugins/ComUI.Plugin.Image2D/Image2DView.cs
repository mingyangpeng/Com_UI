using System;
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

namespace ComUI.Plugin.Image2D;

/// <summary>
/// 2D 静态大图查看器（M2.1）：整图 GL 纹理 + 视口回读呈现（WriteableBitmap）。
/// 交互：滚轮以光标为中心缩放、左键拖拽平移、适配窗口 / 1:1、光标处像素取色；
/// Ctrl+左键拖拽框选 ROI（发布 sel/roi2d），订阅外部 ROI 虚线回显，Esc 清除。
/// 视图状态以视口设备像素为单位；DIP ↔ 像素经 RenderScaling 换算。
/// </summary>
public sealed class Image2DView : UserControl, IDisposable
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    private const string SelfId = "ui.image2d.view";

    private readonly GlImageRenderer _renderer = new();
    private readonly IBus _bus;
    private readonly IPluginContext? _ctx;   // 快照开动态文档页用（测试/内嵌场景可空）
    private readonly Image _image = new() { Stretch = Stretch.None, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
    private readonly Border _imageArea;
    private readonly TextBlock _info = new() { FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _pixel = new() { FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };

    // ROI 叠加层：自有 ROI（青色实线）+ 外部 ROI（洋红虚线）+ 框选橡皮筋 + 三维点高亮（洋红十字标）
    private readonly Canvas _overlay = new() { IsHitTestVisible = false };
    private readonly Rectangle _roiRect = new() { IsVisible = false, StrokeThickness = 1.5, Stroke = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)) };
    private readonly Rectangle _roiExtRect = new() { IsVisible = false, StrokeThickness = 1.5, Stroke = new SolidColorBrush(Color.FromRgb(0xFF, 0x26, 0xFF)), StrokeDashArray = new AvaloniaList<double> { 4, 2 } };
    private readonly Rectangle _roiRubber = new() { IsVisible = false, StrokeThickness = 1, Stroke = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)), StrokeDashArray = new AvaloniaList<double> { 4, 2 } };
    private readonly Line _markH = new() { IsVisible = false, StrokeThickness = 1.5, Stroke = new SolidColorBrush(Color.FromRgb(0xFF, 0x26, 0xFF)) };
    private readonly Line _markV = new() { IsVisible = false, StrokeThickness = 1.5, Stroke = new SolidColorBrush(Color.FromRgb(0xFF, 0x26, 0xFF)) };

    private IDisposable? _busSub;
    private IDisposable? _busSubRoi;
    private IDisposable? _busSubPoint;
    private string _sourceTopic = BusTopics.ImageStitched;    // 当前订阅的数据源主题（可切换）
    private readonly ComboBox _sourceCombo = new() { Classes = { "input" }, MinWidth = 150 };
    private bool _refreshingSource;                           // 下拉重填期间的选中噪声抑制
    private WriteableBitmap? _wb;
    private byte[] _frame = Array.Empty<byte>();      // 回读缓冲（按视口复用）
    private byte[]? _src;                              // 源图 BGRA（像素取色用）
    private int _w, _h;
    private bool _hasImage;
    private bool _hidden;   // 数据树隐藏后的清屏态
    private string? _sourceName;

    // 视图状态（视口设备像素单位）
    private double _scale = 1, _ox = 0, _oy = 0;
    private int _vpW, _vpH;
    private double _scaling = 1;

    private Point? _dragLast;
    private Point? _roiLast;
    private double[]? _roi;        // 自有 ROI，图像像素 [x, y, w, h]
    private double[]? _extRoi;     // 外部 ROI（总线回显/其他面板）
    private double[]? _extUv;      // 外部三维点（归一化 uv，sel/point3d 高亮）

    public Image2DView(IBus bus, IPluginContext? ctx = null)
    {
        _bus = bus;
        _ctx = ctx;
        Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20));
        Focusable = true;

        var toolbar = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), Margin = new Thickness(8, 8, 8, 4) };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        if (ctx is not null)
        {
            var snapBtn = new Button { Classes = { "ghost" }, Content = "📷 快照", Cursor = new Cursor(StandardCursorType.Hand) };
            snapBtn.Click += (_, _) => Snapshot();
            buttons.Children.Add(snapBtn);
            ToolTip.SetTip(snapBtn, "把当前画面冻结为只读快照标签页（用于对比；不随数据更新，不进会话）");

            // 数据源切换：列出总线上有数据的 image/* 主题（sel/* 控制主题不列出）
            ToolTip.SetTip(_sourceCombo, "切换工作台订阅的图像主题");
            _sourceCombo.DropDownOpened += (_, _) => RefreshSourceTopics();
            _sourceCombo.SelectionChanged += OnSourceSelected;
            RefreshSourceTopics();
            buttons.Children.Add(_sourceCombo);
        }
        buttons.Children.Add(GhostBtn("＋", () => ZoomAt(_vpW / 2.0, _vpH / 2.0, 1.25), "以视图中心放大 1.25×（像素级检视）"));
        buttons.Children.Add(GhostBtn("－", () => ZoomAt(_vpW / 2.0, _vpH / 2.0, 0.8), "以视图中心缩小"));
        buttons.Children.Add(GhostBtn("适配窗口", Fit));
        buttons.Children.Add(GhostBtn("1:1", () => ZoomAt(_vpW / 2.0, _vpH / 2.0, 1.0 / _scale)));
        toolbar.Children.Add(buttons);
        toolbar.Children.Add(_info);
        Grid.SetColumn(_info, 2);

        _imageArea = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x16, 0x16, 0x18)),
            ClipToBounds = true,
            Child = new Grid { Children = { _image, _overlay } },
        };
        _overlay.Children.Add(_roiExtRect);
        _overlay.Children.Add(_roiRect);
        _overlay.Children.Add(_roiRubber);
        _overlay.Children.Add(_markH);
        _overlay.Children.Add(_markV);
        _imageArea.PointerWheelChanged += OnWheel;
        _imageArea.PointerPressed += OnPress;
        _imageArea.PointerMoved += OnMove;
        _imageArea.PointerReleased += OnRelease;
        _imageArea.PointerExited += (_, _) => _pixel.Text = "";
        KeyDown += (_, e) =>
        {
            if (e.Key != Key.Escape) return;
            _roi = null;
            _extRoi = null;
            _extUv = null;
            UpdateRoiOverlay();
            e.Handled = true;
        };
        _imageArea.SizeChanged += (_, e) =>
        {
            _scaling = (TopLevel.GetTopLevel(this)?.RenderScaling) ?? 1;
            _vpW = Math.Max(1, (int)(e.NewSize.Width * _scaling));
            _vpH = Math.Max(1, (int)(e.NewSize.Height * _scaling));
            _frame = new byte[_vpW * _vpH * 4];
            _wb = null;   // 尺寸变了重建
            if (_hasImage) Fit();
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto") };
        root.Children.Add(toolbar);
        _imageArea.SetValue(Grid.RowProperty, 1);
        root.Children.Add(_imageArea);
        var statusBar = new Border { Height = 24, Child = _pixel, Margin = new Thickness(8, 2) };
        statusBar.SetValue(Grid.RowProperty, 2);
        root.Children.Add(statusBar);
        Content = root;

        // 订阅总线约定主题（晚订阅自动拿最新帧）；面板单实例，随 Shutdown 退订。
        // 数据源主题可由工具栏下拉切换（SwitchSource），sel/* 控制订阅固定。
        _busSub = bus.Subscribe<ImagePayload>(_sourceTopic,
            p => { if (p.Width <= 0 || p.Height <= 0) ClearImage(p.Source); else SetImage(p.Width, p.Height, p.PixelsBgra, p.Source); }, uiThread: true);
        _busSubRoi = bus.Subscribe<SelectionPayload>(SelectionPayload.TopicRoi2D, p =>
        {
            if (p.Kind != SelectionKind.Roi2D || p.Source == SelfId) return;
            _extRoi = p.Rect2D;
            UpdateRoiOverlay();
        }, uiThread: true);
        _busSubPoint = bus.Subscribe<SelectionPayload>(SelectionPayload.TopicPoint3D, p =>
        {
            // 三维点高亮：按约定 u=(x-minX)/sizeX、v=(z-minZ)/sizeZ 映射到图像像素
            if (p.Kind != SelectionKind.Point3D || p.Uv is not { Length: 2 }) return;
            _extUv = p.Uv;
            UpdateRoiOverlay();
        }, uiThread: true);
    }

    // ==================== 数据源切换 ====================

    /// <summary>重填数据源下拉：总线上最新载荷为图像的主题，当前源必在列。</summary>
    private void RefreshSourceTopics()
    {
        _refreshingSource = true;
        try
        {
            _sourceCombo.Items.Clear();
            var topics = _bus.GetTopics()
                .Where(t => t.LatestType == nameof(ImagePayload))
                .Select(t => t.Topic)
                .Distinct()
                .ToList();
            if (!topics.Contains(_sourceTopic)) topics.Insert(0, _sourceTopic);
            if (!topics.Contains(BusTopics.ImageStitched)) topics.Insert(0, BusTopics.ImageStitched);
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

    /// <summary>切换数据源：退订旧主题、订阅新主题（总线保留帧自动送达，有数据立即显示；
    /// 新主题无数据则保留当前画面，等下一帧）。sel/roi2d、sel/point3d 控制订阅不受影响。</summary>
    private void SwitchSource(string topic)
    {
        if (topic == _sourceTopic) return;
        _sourceTopic = topic;
        _busSub?.Dispose();
        _busSub = _bus.Subscribe<ImagePayload>(topic,
            p => { if (p.Width <= 0 || p.Height <= 0) ClearImage(p.Source); else SetImage(p.Width, p.Height, p.PixelsBgra, p.Source); }, uiThread: true);
        _ctx?.SetStatus($"2D 工作台数据源 → {topic}");
    }

    // ==================== 图像设置 ====================

    /// <summary>快照创建完成（参数=快照视图）——插件据此登记跟踪（关闭后下次快照时回收 GL 资源）。</summary>
    public event Action<Control>? SnapshotCreated;

    /// <summary>冻结当前画面为只读快照标签页：BGRA 引用共享（发布帧约定不可变），
    /// restoreKey=null 不进会话；之后总线更新与快照无关。</summary>
    private void Snapshot()
    {
        if (_ctx is null) return;
        if (!_hasImage || _hidden || _src is null)
        {
            _ctx.SetStatus("无图像可快照");
            return;
        }
        string src = _sourceName is null ? "" : $" · {_sourceName}";
        var snap = new ImageSnapshotView(_w, _h, _src, _sourceName);
        _ctx.OpenDocument($"📷 快照 · 2D 图像 {DateTime.Now:HH:mm:ss}{src}", snap, null);
        SnapshotCreated?.Invoke(snap);
    }

    public void SetImage(int w, int h, byte[] bgra, string? source)
    {
        if (w <= 0 || h <= 0 || bgra.Length < w * h * 4) return;
        _hidden = false;
        _src = bgra; _w = w; _h = h; _hasImage = true;
        _sourceName = string.IsNullOrWhiteSpace(source) ? null : source;
        _renderer.UploadImage(w, h, bgra);
        Fit();
    }

    /// <summary>清空显示（数据树隐藏 2D 图像时以 0×0 载荷通知）：Render 无图早退会残留上一帧，故以 1×1 黑占位重绘。</summary>
    public void ClearImage(string? source)
    {
        _hidden = true;
        // Render 对无图早退（残留上一帧）——必须保持 _hasImage=true 用 1×1 黑占位真正重绘
        _src = null; _w = 1; _h = 1; _hasImage = true;
        _sourceName = string.IsNullOrWhiteSpace(source) ? null : source;
        _renderer.UploadImage(1, 1, new byte[] { 0, 0, 0, 255 });
        _scale = 1; _ox = 0; _oy = 0;
        Render();
    }

    // ==================== 视图变换 ====================

    private void Fit()
    {
        if (!_hasImage || _vpW <= 0 || _vpH <= 0) return;
        double s = Math.Min((_vpW - 24.0) / _w, (_vpH - 24.0) / _h);
        if (s > 1) s = 1;   // 静态大图只往下适配
        _scale = s;
        _ox = (_vpW - _w * s) / 2;
        _oy = (_vpH - _h * s) / 2;
        Render();
    }

    /// <summary>以光标（视口像素）为中心缩放，保持光标下的图像点不动。</summary>
    private void ZoomAt(double cx, double cy, double factor)
    {
        if (!_hasImage) return;
        double ns = Math.Clamp(_scale * factor, 0.0005, 64);
        if (Math.Abs(ns - _scale) < 1e-12) return;
        _ox = cx - (cx - _ox) * ns / _scale;
        _oy = cy - (cy - _oy) * ns / _scale;
        _scale = ns;
        Render();
    }

    // ==================== 指针交互 ====================

    private void OnWheel(object? sender, PointerWheelEventArgs e)
    {
        var pos = e.GetPosition(_imageArea);
        double factor = e.Delta.Y > 0 ? 1.2 : 1 / 1.2;
        ZoomAt(pos.X * _scaling, pos.Y * _scaling, factor);
        e.Handled = true;
    }

    private void OnPress(object? sender, PointerPressedEventArgs e)
    {
        var pt = e.GetCurrentPoint(_imageArea);
        Focus();
        if (pt.Properties.IsLeftButtonPressed && e.KeyModifiers.HasFlag(KeyModifiers.Control))
        {
            _roiLast = pt.Position;
            _dragLast = null;
        }
        else if (pt.Properties.IsLeftButtonPressed)
        {
            _dragLast = pt.Position;
            _imageArea.Cursor = new Cursor(StandardCursorType.SizeAll);
        }
        else return;
        e.Pointer.Capture(_imageArea);
        e.Handled = true;
    }

    private void OnMove(object? sender, PointerEventArgs e)
    {
        var pos = e.GetPosition(_imageArea);
        if (_dragLast is { } last)
        {
            _ox += (pos.X - last.X) * _scaling;
            _oy += (pos.Y - last.Y) * _scaling;
            _dragLast = pos;
            Render();
        }
        else if (_roiLast is { } r)
        {
            UpdateRubber(r, pos);
        }
        UpdatePixel(pos);
    }

    private void OnRelease(object? sender, PointerReleasedEventArgs e)
    {
        if (_roiLast is { } r)
        {
            var pos = e.GetPosition(_imageArea);
            _roiRubber.IsVisible = false;
            if (Math.Abs(pos.X - r.X) + Math.Abs(pos.Y - r.Y) >= 4) CommitRoi(r, pos);
            _roiLast = null;
        }
        _dragLast = null;
        _imageArea.Cursor = Cursor.Default;
        e.Pointer.Capture(null);
    }

    // ==================== ROI 框选与联动 ====================

    /// <summary>框选落定：换算图像像素坐标（裁剪到图内），常显叠加并发布 sel/roi2d。</summary>
    private void CommitRoi(Point a, Point b)
    {
        if (!_hasImage) return;
        double x0 = (Math.Min(a.X, b.X) * _scaling - _ox) / _scale;
        double y0 = (Math.Min(a.Y, b.Y) * _scaling - _oy) / _scale;
        double x1 = (Math.Max(a.X, b.X) * _scaling - _ox) / _scale;
        double y1 = (Math.Max(a.Y, b.Y) * _scaling - _oy) / _scale;
        double x = Math.Max(0, Math.Floor(x0)), y = Math.Max(0, Math.Floor(y0));
        double w = Math.Min(_w, Math.Ceiling(x1)) - x, h = Math.Min(_h, Math.Ceiling(y1)) - y;
        if (w < 1 || h < 1 || x >= _w || y >= _h) return;
        _roi = new[] { x, y, w, h };
        UpdateRoiOverlay();
        _bus.Publish(SelectionPayload.TopicRoi2D, new SelectionPayload(
            SelectionKind.Roi2D, SelfId, null, new double[] { x, y, w, h }, $"{_w}×{_h} 图"));
    }

    private void UpdateRoiOverlay()
    {
        PlaceRoi(_roiRect, _roi);
        PlaceRoi(_roiExtRect, _extRoi);
        PlacePoint();
    }

    /// <summary>三维点高亮十字标：uv → 图像像素 → 视口 DIP（随缩放平移联动）。</summary>
    private void PlacePoint()
    {
        if (_extUv is not { Length: 2 } || !_hasImage)
        {
            _markH.IsVisible = false;
            _markV.IsVisible = false;
            return;
        }
        double cx = (_ox + _extUv[0] * _w * _scale) / _scaling;
        double cy = (_oy + _extUv[1] * _h * _scale) / _scaling;
        if (cx < -20 || cy < -20 || cx > _vpW / _scaling + 20 || cy > _vpH / _scaling + 20)
        {
            _markH.IsVisible = false;
            _markV.IsVisible = false;
            return;
        }
        const double r = 9;
        _markH.IsVisible = true;
        _markH.StartPoint = new Point(cx - r, cy);
        _markH.EndPoint = new Point(cx + r, cy);
        _markV.IsVisible = true;
        _markV.StartPoint = new Point(cx, cy - r);
        _markV.EndPoint = new Point(cx, cy + r);
    }

    /// <summary>图像像素 ROI → 视口 DIP 位置（随缩放平移联动）。</summary>
    private void PlaceRoi(Rectangle rect, double[]? roi)
    {
        if (roi is not { Length: 4 } || !_hasImage || _scaling <= 0)
        {
            rect.IsVisible = false;
            return;
        }
        double left = (_ox + roi[0] * _scale) / _scaling;
        double top = (_oy + roi[1] * _scale) / _scaling;
        double w = roi[2] * _scale / _scaling;
        double h = roi[3] * _scale / _scaling;
        if (left > _vpW / _scaling || top > _vpH / _scaling || left + w < 0 || top + h < 0)
        {
            rect.IsVisible = false;
            return;
        }
        rect.IsVisible = true;
        Canvas.SetLeft(rect, left);
        Canvas.SetTop(rect, top);
        rect.Width = w;
        rect.Height = h;
    }

    private void UpdateRubber(Point a, Point b)
    {
        _roiRubber.IsVisible = true;
        _roiRubber.Width = Math.Abs(b.X - a.X);
        _roiRubber.Height = Math.Abs(b.Y - a.Y);
        Canvas.SetLeft(_roiRubber, Math.Min(a.X, b.X));
        Canvas.SetTop(_roiRubber, Math.Min(a.Y, b.Y));
    }

    // ==================== 像素取色 ====================

    private void UpdatePixel(Point dipPos)
    {
        if (!_hasImage || _src is null) return;
        double px = dipPos.X * _scaling, py = dipPos.Y * _scaling;
        int ix = (int)Math.Floor((px - _ox) / _scale);
        int iy = (int)Math.Floor((py - _oy) / _scale);
        if (ix < 0 || iy < 0 || ix >= _w || iy >= _h) { _pixel.Text = ""; return; }
        int i = (iy * _w + ix) * 4;
        byte b = _src[i], g = _src[i + 1], r = _src[i + 2], a = _src[i + 3];
        _pixel.Text = $"({ix}, {iy})  BGRA({b}, {g}, {r}, {a})  #{r:X2}{g:X2}{b:X2}";
    }

    // ==================== 渲染呈现 ====================

    private void Render()
    {
        if (!_hasImage || _vpW <= 0 || _vpH <= 0) return;
        EnsureWriteable();

        if (_renderer.GlAvailable)
            _renderer.Draw(_vpW, _vpH, _w, _h, _scale * _scaling, _ox * _scaling, _oy * _scaling, _frame);
        else
            SoftwareDraw();

        using var fb = _wb!.Lock();
        nint dst = fb.Address;
        int rowBytes = _vpW * 4;
        // GL 回读自底向上、位图自顶向下：GL 路径逐行倒序拷贝（否则显示垂直翻转）；软件路径本就自顶向下
        bool gl = _renderer.GlAvailable;
        for (int y = 0; y < _vpH; y++)
        {
            int srcRow = gl ? (_vpH - 1 - y) : y;
            Marshal.Copy(_frame, srcRow * rowBytes, (nint)(dst + (long)y * fb.RowBytes), rowBytes);
        }
        // 纯像素更新不触发合成器重呈现（缩放/平移时只有位图在变），必须强制 Image 重绘
        _image.InvalidateVisual();
        UpdateRoiOverlay();
        UpdateInfo();
    }

    /// <summary>软件回退（无 GL 上下文时）：最近邻采样。</summary>
    private void SoftwareDraw()
    {
        var dst = _frame;
        // 背景 #1E1E20 → BGRA
        for (int i = 0; i < dst.Length; i += 4)
        { dst[i] = 0x20; dst[i + 1] = 0x1E; dst[i + 2] = 0x1E; dst[i + 3] = 0xFF; }

        if (_src is null) return;
        double s = _scale * _scaling;
        double x0 = _ox, y0 = _oy;
        int syStart = Math.Max(0, (int)Math.Floor(y0));
        int syEnd = Math.Min(_vpH, (int)Math.Ceiling(y0 + _h * s));
        int sxStart = Math.Max(0, (int)Math.Floor(x0));
        int sxEnd = Math.Min(_vpW, (int)Math.Ceiling(x0 + _w * s));
        for (int sy = syStart; sy < syEnd; sy++)
        {
            int iy = (int)((sy - y0) / s);
            if (iy < 0 || iy >= _h) continue;
            int srcRow = iy * _w * 4;
            int dstRow = sy * _vpW * 4;
            for (int sx = sxStart; sx < sxEnd; sx++)
            {
                int ix = (int)((sx - x0) / s);
                if (ix < 0 || ix >= _w) continue;
                int si = srcRow + ix * 4, di = dstRow + sx * 4;
                dst[di] = _src[si]; dst[di + 1] = _src[si + 1];
                dst[di + 2] = _src[si + 2]; dst[di + 3] = _src[si + 3];
            }
        }
    }

    private void EnsureWriteable()
    {
        if (_wb is not null && _vpW == _wb.PixelSize.Width && _vpH == _wb.PixelSize.Height) return;
        _wb?.Dispose();
        var dpi = new Vector(96 * _scaling, 96 * _scaling);
        _wb = new WriteableBitmap(new PixelSize(_vpW, _vpH), dpi, PixelFormats.Bgra8888, AlphaFormat.Opaque);
        _image.Source = _wb;
    }

    private void UpdateInfo()
    {
        if (_hidden)
        {
            _info.Text = $"已隐藏 · {_sourceName}";
            return;
        }
        string src = _sourceName is null ? "" : $" · {_sourceName}";
        _info.Text = $"{_w}×{_h} ({_w * (long)_h / 1_000_000}MP) · 缩放 {_scale * 100:0.#}% · {_renderer.Backend}{src}";
    }

    private static Button GhostBtn(string text, Action onClick, string? tip = null)
    {
        var b = new Button { Classes = { "ghost" }, Content = text, Cursor = new Cursor(StandardCursorType.Hand) };
        b.Click += (_, _) => onClick();
        if (tip is not null) ToolTip.SetTip(b, tip);
        return b;
    }

    public void Dispose()
    {
        _busSub?.Dispose();
        _busSub = null;
        _busSubRoi?.Dispose();
        _busSubRoi = null;
        _busSubPoint?.Dispose();
        _busSubPoint = null;
        _renderer.Dispose();
        _wb?.Dispose();
        _wb = null;
    }
}
