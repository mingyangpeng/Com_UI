using System;
using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using ComUI.Sdk;

namespace ComUI.Plugin.Image2D;

/// <summary>
/// 2D 快照视图（对比用，只读）：冻结某一时刻的图像帧（BGRA 引用共享，发布帧约定不可变），
/// 仅保留查看导航——适配窗口/1:1、滚轮以光标为中心缩放、左键拖拽平移、光标取色。
/// 不订阅总线、不发布任何事件、无 ROI/选区——总线后续更新与本视图无关。
/// 以动态文档标签打开（restoreKey=null，不进会话，重启丢弃）。
/// </summary>
public sealed class ImageSnapshotView : UserControl, IDisposable
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    private readonly GlImageRenderer _renderer = new();
    private readonly Image _image = new() { Stretch = Stretch.None, HorizontalAlignment = HorizontalAlignment.Left, VerticalAlignment = VerticalAlignment.Top };
    private readonly Border _imageArea;
    private readonly TextBlock _info = new() { FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center };
    private readonly TextBlock _pixel = new() { FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Right };

    private readonly byte[] _src;      // 源图 BGRA（冻结引用，不克隆）
    private readonly int _w, _h;
    private readonly string? _source;
    private WriteableBitmap? _wb;
    private byte[] _frame = Array.Empty<byte>();   // 回读缓冲（按视口复用）

    // 视图状态（视口设备像素单位）
    private double _scale = 1, _ox = 0, _oy = 0;
    private int _vpW, _vpH;
    private double _scaling = 1;
    private Point? _dragLast;

    public ImageSnapshotView(int w, int h, byte[] bgra, string? source)
    {
        if (w <= 0 || h <= 0 || bgra.Length < w * h * 4)
            throw new ArgumentException($"快照图像尺寸非法 {w}×{h}");
        _src = bgra;
        _w = w;
        _h = h;
        _source = string.IsNullOrWhiteSpace(source) ? null : source;

        _renderer.UploadImage(w, h, bgra);

        Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20));
        var toolbar = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto"), Margin = new Thickness(8, 8, 8, 4) };
        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
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
            Child = _image,
        };
        _imageArea.PointerWheelChanged += OnWheel;
        _imageArea.PointerPressed += OnPress;
        _imageArea.PointerMoved += OnMove;
        _imageArea.PointerReleased += OnRelease;
        _imageArea.PointerExited += (_, _) => _pixel.Text = "";
        _imageArea.SizeChanged += (_, e) =>
        {
            _scaling = (TopLevel.GetTopLevel(this)?.RenderScaling) ?? 1;
            _vpW = Math.Max(1, (int)(e.NewSize.Width * _scaling));
            _vpH = Math.Max(1, (int)(e.NewSize.Height * _scaling));
            _frame = new byte[_vpW * _vpH * 4];
            _wb = null;   // 尺寸变了重建
            Fit();
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*,Auto") };
        root.Children.Add(toolbar);
        _imageArea.SetValue(Grid.RowProperty, 1);
        root.Children.Add(_imageArea);
        var statusBar = new Border { Height = 24, Child = _pixel, Margin = new Thickness(8, 2) };
        statusBar.SetValue(Grid.RowProperty, 2);
        root.Children.Add(statusBar);
        Content = root;
    }

    // ==================== 视图变换 ====================

    private void Fit()
    {
        if (_vpW <= 0 || _vpH <= 0) return;
        double s = Math.Min((_vpW - 24.0) / _w, (_vpH - 24.0) / _h);
        if (s > 1) s = 1;   // 只往下适配（与工作台一致）
        _scale = s;
        _ox = (_vpW - _w * s) / 2;
        _oy = (_vpH - _h * s) / 2;
        Render();
    }

    /// <summary>以光标（视口像素）为中心缩放，保持光标下的图像点不动。</summary>
    private void ZoomAt(double cx, double cy, double factor)
    {
        double ns = Math.Clamp(_scale * factor, 0.0005, 64);
        if (Math.Abs(ns - _scale) < 1e-12) return;
        _ox = cx - (cx - _ox) * ns / _scale;
        _oy = cy - (cy - _oy) * ns / _scale;
        _scale = ns;
        Render();
    }

    // ==================== 指针交互（只读：缩放/平移） ====================

    private void OnWheel(object? sender, PointerWheelEventArgs e)
    {
        var pos = e.GetPosition(_imageArea);
        double factor = e.Delta.Y > 0 ? 1.2 : 1 / 1.2;
        ZoomAt(pos.X * _scaling, pos.Y * _scaling, factor);
        e.Handled = true;
    }

    private void OnPress(object? sender, PointerPressedEventArgs e)
    {
        if (!e.GetCurrentPoint(_imageArea).Properties.IsLeftButtonPressed) return;
        _dragLast = e.GetCurrentPoint(_imageArea).Position;
        _imageArea.Cursor = new Cursor(StandardCursorType.SizeAll);
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
        UpdatePixel(pos);
    }

    private void OnRelease(object? sender, PointerReleasedEventArgs e)
    {
        _dragLast = null;
        _imageArea.Cursor = Cursor.Default;
        e.Pointer.Capture(null);
    }

    // ==================== 像素取色（只读检视） ====================

    private void UpdatePixel(Point dipPos)
    {
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
        if (_vpW <= 0 || _vpH <= 0) return;
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
        UpdateInfo();
    }

    /// <summary>软件回退（无 GL 上下文时）：最近邻采样。</summary>
    private void SoftwareDraw()
    {
        var dst = _frame;
        for (int i = 0; i < dst.Length; i += 4)
        { dst[i] = 0x20; dst[i + 1] = 0x1E; dst[i + 2] = 0x1E; dst[i + 3] = 0xFF; }

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
        string src = _source is null ? "" : $" · {_source}";
        _info.Text = $"快照 {_w}×{_h} ({_w * (long)_h / 1_000_000}MP) · 缩放 {_scale * 100:0.#}% · {_renderer.Backend}{src}";
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
        _renderer.Dispose();
        _wb?.Dispose();
        _wb = null;
    }
}
