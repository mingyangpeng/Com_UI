using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// macOS 风格模态确认框（数据树批量删除二次确认用）。返回 true=确认，false=取消/Esc/关闭。
/// 样式类 danger/primary/ghost 与宿主全局样式同源。
/// </summary>
public static class ConfirmDialog
{
    public static async Task<bool> ShowAsync(
        Window? owner, string title, string message,
        string confirmText = "确认", bool danger = false)
    {
        var msg = new TextBlock
        {
            Text = message,
            FontSize = 12.5,
            Foreground = new SolidColorBrush(Color.FromRgb(0xC8, 0xC8, 0xCE)),
            TextWrapping = TextWrapping.Wrap,
            MaxWidth = 340,
        };

        var ok = new Button { Content = confirmText, MinWidth = 82 };
        ok.Classes.Add(danger ? "danger" : "primary");
        var cancel = new Button { Content = "取消", MinWidth = 82 };
        cancel.Classes.Add("ghost");

        var root = new StackPanel { Margin = new Thickness(24, 20, 24, 20) };
        root.Children.Add(new TextBlock
        {
            Text = title, FontSize = 14.5, FontWeight = FontWeight.SemiBold,
        });
        root.Children.Add(new TextBlock
        {
            Text = "", Height = 1,
            Background = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0xFF, 0xFF)),
            Margin = new Thickness(0, 12, 0, 12),
        });
        root.Children.Add(msg);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 10,
            Margin = new Thickness(0, 20, 0, 0),
        };
        buttons.Children.Add(cancel);
        buttons.Children.Add(ok);
        root.Children.Add(buttons);

        var dlg = new Window
        {
            Title = title,
            SizeToContent = SizeToContent.WidthAndHeight,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = root,
        };
        dlg.Classes.Add("dialog");

        var tcs = new TaskCompletionSource<bool>();
        void Finish(bool result) { tcs.TrySetResult(result); dlg.Close(); }
        ok.Click += (_, _) => Finish(true);
        cancel.Click += (_, _) => Finish(false);
        dlg.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape) Finish(false);
            else if (e.Key == Key.Enter) Finish(true);
        };

        try
        {
            if (owner is not null) _ = dlg.ShowDialog(owner);
            else dlg.Show();
            return await tcs.Task;
        }
        finally
        {
            dlg.Close();
        }
    }
}
