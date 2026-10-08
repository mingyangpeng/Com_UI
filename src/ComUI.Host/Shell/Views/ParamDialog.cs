using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using ComUI.Core;
using ComUI.Sdk;

namespace ComUI.Host.Views;

/// <summary>
/// 算法参数对话框（macOS 风格模态）：按 AlgoParam 声明自动生成编辑行，
/// 点「确定」时统一校验并写入 values（键 = 参数名）。校验失败不关闭。
/// </summary>
public static class ParamDialog
{
    public static Task<bool> ShowAsync(Window owner, string title, string? subtitle,
        IReadOnlyList<AlgoParam> ps, Dictionary<string, object?> values, Action<string>? onError = null)
    {
        var tcs = new TaskCompletionSource<bool>();
        var editors = new List<(AlgoParam P, Control Editor)>();

        var body = new StackPanel { Margin = new Thickness(24, 20), Spacing = 14 };

        body.Children.Add(new TextBlock { Text = title, FontSize = 15, FontWeight = FontWeight.SemiBold });
        if (!string.IsNullOrWhiteSpace(subtitle))
        {
            body.Children.Add(new TextBlock
            {
                Text = subtitle,
                FontSize = 12,
                TextWrapping = TextWrapping.Wrap,
                Foreground = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
            });
        }

        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("110,*"), RowDefinitions = MakeRows(ps.Count), Margin = new Thickness(0, 4, 0, 0) };
        for (int i = 0; i < ps.Count; i++)
        {
            var p = ps[i];
            var label = new TextBlock
            {
                Text = string.IsNullOrWhiteSpace(p.Label) ? p.Name : p.Label,
                FontSize = 12.5,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(0, 0, 10, 0),
            };
            Grid.SetRow(label, i);
            grid.Children.Add(label);

            var editor = MakeEditor(p);
            Grid.SetRow(editor, i);
            Grid.SetColumn(editor, 1);
            grid.Children.Add(editor);
            editors.Add((p, editor));
        }
        body.Children.Add(grid);

        var footer = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 10,
            Margin = new Thickness(0, 6, 0, 0),
        };
        var cancel = new Button { Classes = { "ghost" }, Content = "取消", MinWidth = 82 };
        var ok = new Button { Classes = { "primary" }, Content = "确定", MinWidth = 82 };
        footer.Children.Add(cancel);
        footer.Children.Add(ok);
        body.Children.Add(footer);

        var dlg = new Window
        {
            Classes = { "dialog" },
            Title = title,
            SizeToContent = SizeToContent.WidthAndHeight,
            Width = 470,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            MinWidth = 430,
            Content = body,
        };

        void Finish(bool confirmed)
        {
            tcs.TrySetResult(confirmed);
            dlg.Close();
        }

        bool TryCollect()
        {
            values.Clear();
            foreach (var (p, editor) in editors)
            {
                try
                {
                    values[p.Name] = ReadValue(p, editor);
                }
                catch (Exception ex)
                {
                    onError?.Invoke(ex.Message);
                    return false;
                }
            }
            return true;
        }

        ok.Click += (_, _) => { if (TryCollect()) Finish(true); };
        cancel.Click += (_, _) => Finish(false);
        dlg.Closed += (_, _) => tcs.TrySetResult(false);   // 标题栏 ✕ 关闭 = 取消

        _ = dlg.ShowDialog(owner);
        return tcs.Task;
    }

    // ==================== 编辑行 ====================

    private static RowDefinitions MakeRows(int n)
    {
        var defs = new RowDefinitions();
        for (int i = 0; i < n; i++) defs.Add(new RowDefinition(GridLength.Auto));
        return defs;
    }

    private static Control MakeEditor(AlgoParam p)
    {
        switch (p.Type)
        {
            case AlgoParamType.Boolean:
                return new CheckBox { IsChecked = p.Default as bool? ?? false, HorizontalAlignment = HorizontalAlignment.Left };

            case AlgoParamType.Enum:
                var combo = new ComboBox { Classes = { "input" }, MinWidth = 210 };
                foreach (var o in p.Options ?? Array.Empty<string>())
                    combo.Items.Add(o);
                var def = p.Default?.ToString();
                if (def is not null)
                    combo.SelectedIndex = (p.Options ?? Array.Empty<string>()).ToList().IndexOf(def);
                return combo;

            case AlgoParamType.Path:
                var pathBox = new TextBox { Classes = { "input" }, Text = p.Default?.ToString() ?? "", MinWidth = 250 };
                var browse = new Button
                {
                    Classes = { "ghost" },
                    Content = "…",
                    MinWidth = 36,
                    VerticalAlignment = VerticalAlignment.Center,
                };
                browse.Click += async (_, _) =>
                {
                    var top = TopLevel.GetTopLevel(browse);
                    if (top is null) return;
                    var files = await top.StorageProvider.OpenFilePickerAsync(new Avalonia.Platform.Storage.FilePickerOpenOptions
                    {
                        AllowMultiple = false,
                        Title = "选择文件",
                    });
                    if (files.Count > 0)
                        pathBox.Text = files[0].Path.LocalPath;
                };
                var wrap = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
                wrap.Children.Add(pathBox);
                wrap.Children.Add(browse);
                return wrap;

            default:   // Number / Int / Text
                var isNum = p.Type is AlgoParamType.Number or AlgoParamType.Int;
                var box = new TextBox
                {
                    Classes = { "input" },
                    Text = p.Default?.ToString() ?? "",
                    Watermark = isNum ? (p.Type == AlgoParamType.Int ? "整数" : "数值") : "",
                    MinWidth = 210,
                };
                if (isNum) ToolTip.SetTip(box, RangeTip(p));
                return box;
        }
    }

    private static string RangeTip(AlgoParam p)
    {
        if (p.Min is { } lo && p.Max is { } hi) return $"范围 [{lo}, {hi}]";
        if (p.Min is { } lo2) return $"最小 {lo2}";
        if (p.Max is { } hi2) return $"最大 {hi2}";
        return "";
    }

    private static object? ReadValue(AlgoParam p, Control editor)
    {
        switch (p.Type)
        {
            case AlgoParamType.Boolean:
                return ((CheckBox)editor).IsChecked == true;

            case AlgoParamType.Enum:
                return ((ComboBox)editor).SelectedItem?.ToString() ?? "";

            case AlgoParamType.Path:
                var wrap = (StackPanel)editor;
                var box = (TextBox)wrap.Children[0];
                return box.Text ?? "";

            case AlgoParamType.Int:
            {
                var text = ((TextBox)editor).Text?.Trim() ?? "";
                if (!int.TryParse(text, out var iv))
                    throw new Exception($"参数「{(string.IsNullOrWhiteSpace(p.Label) ? p.Name : p.Label)}」需要整数，当前为 “{text}”");
                if ((p.Min is { } lo && iv < lo) || (p.Max is { } hi && iv > hi))
                    throw new Exception($"参数「{p.Label}」超出范围 [{p.Min}, {p.Max}]");
                return iv;
            }

            case AlgoParamType.Number:
            {
                var text = ((TextBox)editor).Text?.Trim() ?? "";
                if (!double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var dv)
                    && !double.TryParse(text, out dv))
                    throw new Exception($"参数「{(string.IsNullOrWhiteSpace(p.Label) ? p.Name : p.Label)}」需要数值，当前为 “{text}”");
                if ((p.Min is { } lo2 && dv < lo2) || (p.Max is { } hi2 && dv > hi2))
                    throw new Exception($"参数「{p.Label}」超出范围 [{p.Min}, {p.Max}]");
                return dv;
            }

            default:   // Text
                return ((TextBox)editor).Text ?? "";
        }
    }
}
