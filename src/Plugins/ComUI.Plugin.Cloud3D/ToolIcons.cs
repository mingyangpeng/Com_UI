using Avalonia;
using Avalonia.Collections;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 工具栏矢量图标工厂（SF Symbols 风格：16×16 网格单色线条、1.3 圆帽描边、系统灰白）。
/// 全部由基础几何（Path/Polyline/Ellipse/Rect）组合，与快照相机图标同一绘制体系。
/// </summary>
public static class ToolIcons
{
    private static readonly Brush Ink = new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xEA));

    /// <summary>按 key 生成图标控件（Viewbox 统一缩放到 size）。</summary>
    public static Control Make(string key, double size = 14)
    {
        var canvas = new Panel { Width = 16, Height = 16 };
        foreach (var child in Shapes(key, Ink))
            canvas.Children.Add(child);
        return new Viewbox { Child = canvas, Width = size, Height = size, Stretch = Stretch.Uniform };
    }

    private static Shape Line(double x1, double y1, double x2, double y2, double thickness = 1.3, Brush? brush = null) => new Line
    {
        StartPoint = new Point(x1, y1),
        EndPoint = new Point(x2, y2),
        Stroke = brush ?? Ink,
        StrokeThickness = thickness,
        StrokeLineCap = PenLineCap.Round,
    };

    private static Shape Dot(double cx, double cy, double r, bool fill = true) => new Ellipse
    {
        Width = r * 2, Height = r * 2,
        Stroke = Ink, StrokeThickness = 1.3,
        Fill = fill ? Ink : null,
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Top,
        Margin = new Thickness(cx - r, cy - r, 0, 0),
    };

    private static Shape Rect(double x, double y, double w, double h, double thickness = 1.3, double radius = 1.5, bool dash = false)
    {
        var s = new Rectangle
        {
            Width = w, Height = h,
            Stroke = Ink, StrokeThickness = thickness,
            RadiusX = radius, RadiusY = radius,
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(x, y, 0, 0),
        };
        if (dash) s.StrokeDashArray = new AvaloniaList<double> { 3, 2 };
        return s;
    }

    private static Shape Poly(double[] pts, double thickness = 1.3, bool close = false) => new Polyline
    {
        Points = ToPoints(pts, close),
        Stroke = Ink,
        StrokeThickness = thickness,
        StrokeLineCap = PenLineCap.Round,
    };

    private static AvaloniaList<Point> ToPoints(double[] flat, bool close)
    {
        var list = new AvaloniaList<Point>();
        for (int i = 0; i + 1 < flat.Length; i += 2)
            list.Add(new Point(flat[i], flat[i + 1]));
        if (close && flat.Length >= 2) list.Add(new Point(flat[0], flat[1]));
        return list;
    }

    private static System.Collections.Generic.IEnumerable<Control> Shapes(string key, Brush ink)
    {
        switch (key)
        {
            case "extract":   // 两方框重叠（取子集）
                yield return Rect(1, 1, 9, 9, dash: true);
                yield return Rect(6, 6, 9, 9);
                break;

            case "copy":      // 两同框偏移（复制清单）
                yield return Rect(1, 5, 9, 9);
                yield return Rect(6, 1, 9, 9);
                break;

            case "crop":      // 虚线框 + 内部实块（剔除反义：保留块）
                yield return Rect(1, 1, 14, 14, dash: true);
                yield return Rect(5, 5, 6, 6);
                break;

            case "boxcrop":   // 立方体线框（等轴三面）
                yield return Poly(new[] { 2.0, 5.0, 8.0, 1.0, 14.0, 4.0, 8.0, 10.0, 2.0, 5.0 }, close: true);
                yield return Poly(new[] { 8.0, 10.0, 8.0, 14.0, 2.0, 9.0 });
                yield return Poly(new[] { 8.0, 10.0, 14.0, 4.0 });
                break;

            case "polygon":   // 五边形（多边形选取）
                yield return Poly(new[] { 8.0, 1.5, 14.5, 6.5, 12.0, 14.0, 4.0, 14.0, 1.5, 6.5 }, close: true);
                break;

            case "measure":   // 两点连线（量测）
                yield return Dot(3, 13, 1.8, fill: false);
                yield return Dot(13, 3, 1.8, fill: false);
                yield return Line(4.2, 11.8, 11.8, 4.2);
                break;

            case "clear":     // 圆 + 斜线（清除）
                yield return Dot(8, 8, 6, fill: false);
                yield return Line(3.7, 12.3, 12.3, 3.7);
                break;

            case "operator":  // 齿轮（圆 + 辐条）
                yield return Dot(8, 8, 4.2, fill: false);
                yield return Line(8, 1, 8, 3.4);
                yield return Line(8, 12.6, 8, 15);
                yield return Line(1, 8, 3.4, 8);
                yield return Line(12.6, 8, 15, 8);
                yield return Line(3.05, 3.05, 4.75, 4.75);
                yield return Line(11.25, 11.25, 12.95, 12.95);
                yield return Line(3.05, 12.95, 4.75, 11.25);
                yield return Line(11.25, 4.75, 12.95, 3.05);
                break;

            case "random":    // 方框 + 三点（随机采样）
                yield return Rect(1, 1, 14, 14);
                yield return Dot(5, 5, 1.2);
                yield return Dot(11, 8, 1.2);
                yield return Dot(5, 11, 1.2);
                break;

            case "outlier":   // 中心点 + 外散点（离群）
                yield return Dot(6.5, 9.5, 1.6);
                yield return Dot(13, 3.5, 1.2, fill: false);
                yield return Line(7.8, 8.2, 12, 4.3);
                yield return Line(12.8, 8.5, 14, 10);
                break;

            case "merge":     // 两圆相切 + 加号（合并）
                yield return Dot(5, 8, 4, fill: false);
                yield return Dot(12, 8, 4, fill: false);
                break;

            case "transform": // 四角框角（变换）
                yield return Poly(new[] { 1.0, 5.0, 1.0, 1.0, 5.0, 1.0 });
                yield return Poly(new[] { 11.0, 1.0, 15.0, 1.0, 15.0, 5.0 });
                yield return Poly(new[] { 15.0, 11.0, 15.0, 15.0, 11.0, 15.0 });
                yield return Poly(new[] { 5.0, 15.0, 1.0, 15.0, 1.0, 11.0 });
                yield return Line(6, 8, 10, 8);
                yield return Line(10, 8, 8.4, 6.4);
                yield return Line(10, 8, 8.4, 9.6);
                yield return Line(6, 8, 10, 8);
                yield return Line(6, 8, 7.6, 6.4);
                yield return Line(6, 8, 7.6, 9.6);
                break;

            case "import":    // 托盘 + 下箭头（导入）
                yield return Poly(new[] { 2.0, 10.0, 2.0, 14.0, 14.0, 14.0, 14.0, 10.0 });
                yield return Line(8, 1.5, 8, 9.5);
                yield return Line(8, 9.5, 5.5, 7);
                yield return Line(8, 9.5, 10.5, 7);
                break;

            case "export":    // 托盘 + 上箭头（导出）
                yield return Poly(new[] { 2.0, 10.0, 2.0, 14.0, 14.0, 14.0, 14.0, 10.0 });
                yield return Line(8, 9.5, 8, 1.5);
                yield return Line(8, 1.5, 5.5, 4);
                yield return Line(8, 1.5, 10.5, 4);
                break;

            case "normal":    // 基点 + 法线箭头
                yield return Dot(8, 12.5, 2, fill: false);
                yield return Line(8, 10.5, 8, 3);
                yield return Line(8, 3, 6, 5);
                yield return Line(8, 3, 10, 5);
                break;

            case "heightmap": // 山形（高度图）
                yield return Poly(new[] { 1.0, 14.0, 5.5, 5.0, 8.5, 10.0, 11.0, 6.5, 15.0, 14.0 });
                break;

            case "regist":    // 两圆 + 连线（点对配准）
                yield return Dot(3.5, 12, 2.2, fill: false);
                yield return Dot(12.5, 4, 2.2, fill: false);
                yield return Line(5.3, 10.5, 10.7, 5.5);
                break;

            case "match":     // 同心圆 + 箭头（质心对齐）
                yield return Dot(8, 8, 5.5, fill: false);
                yield return Dot(8, 8, 1.6);
                yield return Line(12, 4, 14.5, 1.5);
                break;

            case "focus":     // 同心圆 + 十字（聚焦）
                yield return Dot(8, 8, 4.5, fill: false);
                yield return Line(8, 1, 8, 3.5);
                yield return Line(8, 12.5, 8, 15);
                yield return Line(1, 8, 3.5, 8);
                yield return Line(12.5, 8, 15, 8);
                break;

            case "lockpitch": // 圆 + 水平线（锁定绕竖直轴）
                yield return Dot(8, 8, 6, fill: false);
                yield return Line(2, 8, 14, 8);
                yield return Dot(8, 8, 1.4);
                break;

            case "clip":      // 三层横线（切片）
                yield return Line(2, 5, 14, 5);
                yield return Line(3.5, 8, 12.5, 8);
                yield return Line(5, 11, 11, 11);
                break;

            case "heightmapcsv": // 山形 + 下载角标（高度图 CSV）
                yield return Poly(new[] { 1.0, 11.0, 4.5, 4.0, 7.0, 8.0, 9.5, 5.0, 12.5, 11.0 });
                yield return Line(13, 9, 13, 14.5);
                yield return Line(13, 14.5, 11, 12.5);
                yield return Line(13, 14.5, 15, 12.5);
                break;
        }
    }
}
