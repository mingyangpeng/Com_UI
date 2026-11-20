using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Sdk.Ui;

/// <summary>
/// 垃圾桶矢量图标：emoji 🗑 是彩色位图字体，无法用 Foreground 着色、小字号下发虚，
/// 这里用 Path 绘制线性垃圾桶（上宽下窄的梯形桶身 + 两条竖纹，无盖），可着色、任意缩放，更形象清晰。
/// </summary>
public static class TrashGlyph
{
    // 基于 16×16 坐标系：梯形桶身（顶部开口，无盖）→ 两条竖纹
    private const string PathData =
        "M4.5,3.5 L5.5,14 L10.5,14 L11.5,3.5 " +
        "M7,6 L7.3,11.5 M9,6 L8.7,11.5";

    /// <summary>构建垃圾桶图标。size = 像素边长，brush = 线条颜色（null 用灰色）。</summary>
    public static Control Build(double size = 14, IBrush? brush = null) => new Avalonia.Controls.Shapes.Path
    {
        Data = StreamGeometry.Parse(PathData),
        Stroke = brush ?? DefaultBrush,
        StrokeThickness = 1.3,
        StrokeLineCap = PenLineCap.Round,
        Stretch = Stretch.Uniform,
        Width = size,
        Height = size,
        VerticalAlignment = VerticalAlignment.Center,
        HorizontalAlignment = HorizontalAlignment.Center,
    };

    /// <summary>白色（深色主题上清晰、与文字同色）。</summary>
    public static readonly IBrush DefaultBrush = new SolidColorBrush(Color.FromRgb(0xFF, 0xFF, 0xFF));

    /// <summary>图标 + 文字横向组合（供带文字的删除按钮用）。</summary>
    public static Control WithText(string text, double size = 14, IBrush? brush = null)
    {
        var panel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 5,
            VerticalAlignment = VerticalAlignment.Center,
        };
        panel.Children.Add(Build(size, brush));
        panel.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
        return panel;
    }
}
