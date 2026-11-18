using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 算子栏目选择对话框（转为算子时使用）：可选现有栏目或输入新栏目名。
/// 返回选中的栏目名；取消返回 null。
/// </summary>
public static class GroupPickerDialog
{
    /// <param name="owner">宿主窗口</param>
    /// <param name="groups">现有栏目名列表</param>
    /// <param name="current">当前默认栏目</param>
    public static async Task<string?> ShowAsync(
        Window? owner, System.Collections.Generic.IEnumerable<string> groups, string current = "子流程")
    {
        // 去重、排序，确保默认项在列表里
        var options = groups.Where(g => !string.IsNullOrWhiteSpace(g))
                            .Distinct(StringComparer.OrdinalIgnoreCase)
                            .OrderBy(g => g, StringComparer.OrdinalIgnoreCase)
                            .ToList();
        if (!options.Any(g => g.Equals(current, StringComparison.OrdinalIgnoreCase)))
            options.Insert(0, current);

        var combo = new ComboBox
        {
            ItemsSource = options,
            SelectedItem = options.First(g => g.Equals(current, StringComparison.OrdinalIgnoreCase)),
            IsEditable = true,          // 允许输入新栏目名
            Width = 280,
            MinHeight = 30,
            FontSize = 12.5,
        };
        combo.Classes.Add("input");

        var ok = new Button { Content = "确定", MinWidth = 82 };
        ok.Classes.Add("primary");
        var cancel = new Button { Content = "取消", MinWidth = 82 };
        cancel.Classes.Add("ghost");

        var root = new StackPanel { Margin = new Thickness(24, 20, 24, 20) };
        root.Children.Add(new TextBlock
        {
            Text = "选择算子栏目", FontSize = 14.5, FontWeight = FontWeight.SemiBold,
        });
        root.Children.Add(new TextBlock
        {
            Text = "", Height = 1,
            Background = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0xFF, 0xFF)),
            Margin = new Thickness(0, 12, 0, 12),
        });
        root.Children.Add(new TextBlock
        {
            Text = "这个算子将显示在算子库的哪个栏目下（可直接输入新栏目名）",
            FontSize = 12, Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
            TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 8),
        });
        root.Children.Add(combo);
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
            Title = "选择算子栏目",
            SizeToContent = SizeToContent.WidthAndHeight,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = root,
        };
        dlg.Classes.Add("dialog");

        var tcs = new TaskCompletionSource<string?>();
        void Finish(string? result) { tcs.TrySetResult(result); dlg.Close(); }
        // ComboBox IsEditable 时 Text 是用户输入/选中的文本
        string CurrentText() => (combo.Text ?? combo.SelectedItem?.ToString() ?? "").Trim();
        ok.Click += (_, _) => Finish(CurrentText() is { Length: > 0 } s ? s : null);
        cancel.Click += (_, _) => Finish(null);
        dlg.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape) Finish(null);
            else if (e.Key == Key.Enter) Finish(CurrentText() is { Length: > 0 } s ? s : null);
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
