using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Sdk.Ui;

/// <summary>
/// 图钉徽标（形状绘制——emoji 是彩色字体无法用 Foreground 着色）。
/// 造型参考 VS2022 固定图标：圆钉头 + 细针尖的经典图钉。
/// <b>按下 = 蓝色竖直钉</b>（#0A84FF，语义=钉住不动），
/// <b>拔起 = 灰色斜钉</b>（#9D9D9D，整体旋转 45°，语义=可移动）。
/// 放置规则与 LockGlyph 同族：行/栏目头右端对齐一列。
/// </summary>
public static class PinGlyph
{
    /// <summary>构建图钉图标（14×14 Canvas）。</summary>
    public static Control Build(bool pinned)
    {
        var brush = pinned ? PinnedBrush : UnpinnedBrush;

        // 钉头：圆角近圆（VS 风格的圆头）
        var head = new Border
        {
            Width = 7, Height = 7, CornerRadius = new CornerRadius(3.5),
            Background = brush,
        };
        // 针尖：细杆（下端略收）
        var needle = new Border
        {
            Width = 2.2, Height = 6.5, CornerRadius = new CornerRadius(1),
            Background = brush,
            HorizontalAlignment = HorizontalAlignment.Center,
        };

        var pin = new Canvas { Width = 9, Height = 13 };   // 头 7 + 针 6.5，针略入头 0.5
        pin.Children.Add(head);
        Canvas.SetLeft(head, 1);
        Canvas.SetTop(head, 0);
        pin.Children.Add(needle);
        Canvas.SetLeft(needle, (9 - 2.2) / 2);
        Canvas.SetTop(needle, 6.5);

        var canvas = new Canvas { Width = 14, Height = 14 };
        canvas.Children.Add(pin);
        if (pinned)
        {
            Canvas.SetLeft(pin, 2.5);   // 按下：竖直居中
            Canvas.SetTop(pin, 0.5);
        }
        else
        {
            // 拔起：绕钉身中点旋转 45°（VS unpin 同款斜放）
            pin.RenderTransformOrigin = new RelativePoint(4.5, 10, RelativeUnit.Absolute);
            pin.RenderTransform = new RotateTransform(45);
            Canvas.SetLeft(pin, 3);
            Canvas.SetTop(pin, 1);
        }
        return canvas;
    }

    public static readonly IBrush PinnedBrush = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF));
    public static readonly IBrush UnpinnedBrush = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));
}
