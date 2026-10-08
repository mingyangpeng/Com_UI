using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Plugin.NodeGraph;

/// <summary>输入名称的小对话框（macOS 风格，用于保存算法命名）。</summary>
public static class NamePrompt
{
    public static async Task<string?> ShowAsync(Window owner, string title, string? subtitle, string initial)
    {
        var tb = new TextBox { Text = initial, MinWidth = 280 };
        tb.Classes.Add("input");
        tb.SelectAll();

        var ok = new Button { Content = "保存" };
        ok.Classes.Add("primary");
        var cancel = new Button { Content = "取消" };
        cancel.Classes.Add("ghost");

        var root = new StackPanel { Margin = new Thickness(24, 20, 24, 20) };
        root.Children.Add(new TextBlock { Text = title, FontSize = 14.5, FontWeight = FontWeight.SemiBold });
        if (!string.IsNullOrWhiteSpace(subtitle))
            root.Children.Add(new TextBlock
            {
                Text = subtitle,
                FontSize = 12,
                Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 4, 0, 0),
            });
        root.Children.Add(new Border
        {
            Height = 1,
            Background = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0xFF, 0xFF)),
            Margin = new Thickness(0, 14, 0, 14),
        });
        root.Children.Add(tb);
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 10,
            Margin = new Thickness(0, 18, 0, 0),
        };
        buttons.Children.Add(cancel);
        buttons.Children.Add(ok);
        root.Children.Add(buttons);

        var dlg = new Window
        {
            Title = title,
            Width = 440,
            SizeToContent = SizeToContent.Height,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = root,
        };
        dlg.Classes.Add("dialog");

        var tcs = new TaskCompletionSource<string?>();
        void Confirm() { tcs.TrySetResult(string.IsNullOrWhiteSpace(tb.Text) ? null : tb.Text.Trim()); dlg.Close(); }
        ok.Click += (_, _) => Confirm();
        cancel.Click += (_, _) => { tcs.TrySetResult(null); dlg.Close(); };
        dlg.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape) { tcs.TrySetResult(null); dlg.Close(); }
            else if (e.Key == Key.Enter) Confirm();
        };
        dlg.Opened += (_, _) => tb.Focus();

        _ = dlg.ShowDialog(owner);
        var result = await tcs.Task;
        dlg.Close();
        return result;
    }
}
