using System.Collections.Generic;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Plugin.Cloud3D;

/// <summary>对话框结果：参数值（按声明顺序）+ 双实体算子的基准/被测实体 Id（单输入算子为 null）。</summary>
public sealed class ToolParamResult
{
    public required List<object?> Values { get; init; }
    public string? CloudA { get; init; }
    public string? CloudB { get; init; }
}

/// <summary>
/// 工具算子参数对话框（通用）：按 CloudToolOps.ToolOp 的参数声明自动构建表单
/// （number/int=数值框带范围钳制、path/text=文本框、bool=勾选），行级悬浮提示=参数说明。
/// CloudInputs==2 时顶部出现基准/被测实体下拉（cloudChoices 提供候选）。
/// 取消返回 null。
/// </summary>
public static class ToolParamDialog
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    /// <param name="owner">宿主窗口</param>
    /// <param name="op">工具算子（标题/说明/参数声明）</param>
    /// <param name="prefill">可选预填：参数名 → 文本（如框选 ROI 边界自动填 x/y/w/h 类参数）</param>
    /// <param name="cloudChoices">双实体算子的候选实体 Id 列表（null/单输入 = 不出现实体选择）</param>
    /// <param name="preselect">实体下拉预选（基准, 被测）</param>
    public static async Task<ToolParamResult?> ShowAsync(Window? owner, CloudToolOps.ToolOp op,
        IReadOnlyDictionary<string, string>? prefill = null,
        IReadOnlyList<string>? cloudChoices = null,
        (string? A, string? B)? preselect = null)
    {
        var editors = new List<(CloudToolOps.ToolParam P, Control Editor, Func<object?> Read)>();
        var comboA = new ComboBox { MinWidth = 180, MinHeight = 26, FontSize = 12 };
        var comboB = new ComboBox { MinWidth = 180, MinHeight = 26, FontSize = 12 };
        bool dual = op.CloudInputs == 2 && cloudChoices is { Count: >= 2 };

        var root = new StackPanel { Margin = new Thickness(24, 18, 24, 18), MaxWidth = 520 };
        root.Children.Add(new TextBlock
        {
            Text = op.Name, FontSize = 14.5, FontWeight = FontWeight.SemiBold,
        });
        if (!string.IsNullOrWhiteSpace(op.Description))
            root.Children.Add(new TextBlock
            {
                Text = op.Description, FontSize = 11.5, Foreground = Dim,
                TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 6, 0, 0),
            });
        root.Children.Add(new Border
        {
            Height = 1, Background = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0xFF, 0xFF)),
            Margin = new Thickness(0, 10, 0, 12),
        });

        if (op.Params.Length == 0)
            root.Children.Add(new TextBlock { Text = "本工具无参数", FontSize = 12, Foreground = Dim });

        // 双实体算子：基准/被测选择（顶部）
        if (dual)
        {
            foreach (var c in cloudChoices!) { comboA.Items.Add(c); comboB.Items.Add(c); }
            comboA.SelectedItem = preselect?.A is { } a && cloudChoices.Contains(a) ? a : cloudChoices[0];
            comboB.SelectedItem = preselect?.B is { } b && cloudChoices.Contains(b) && b != comboA.SelectedItem
                ? b
                : cloudChoices.FirstOrDefault(c => c != comboA.SelectedItem) ?? cloudChoices[1];
            root.Children.Add(MakeEntityRow("基准点云", comboA));
            root.Children.Add(MakeEntityRow("被测点云", comboB));
        }

        foreach (var p in op.Params)
        {
            var initial = prefill is not null && prefill.TryGetValue(p.Name, out var pf) && !string.IsNullOrWhiteSpace(pf)
                ? pf
                : p.Default ?? "";
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("130,*"), Margin = new Thickness(0, 6) };
            var labelHost = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            labelHost.Children.Add(new TextBlock { Text = string.IsNullOrEmpty(p.Label) ? p.Name : p.Label, FontSize = 12, Foreground = Dim, TextWrapping = TextWrapping.Wrap });
            if (!string.IsNullOrWhiteSpace(p.Description))
                ToolTip.SetTip(row, p.Description);   // 行级悬浮=参数说明（属性说明契约）
            row.Children.Add(labelHost);

            Control editor;
            Func<object?> read;
            if (p.Type == "bool")
            {
                var cb = new CheckBox { IsChecked = initial is "true" or "True" or "1", HorizontalAlignment = HorizontalAlignment.Left, MinHeight = 24 };
                read = () => cb.IsChecked == true;
                editor = cb;
            }
            else
            {
                var tb = new TextBox
                {
                    Text = initial,
                    MinHeight = 26, FontSize = 12,
                    Padding = new Thickness(6, 3, 6, 3),
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                };
                if (p.Type == "int" || p.Type == "number")
                {
                    tb.TextAlignment = TextAlignment.Right;
                    double? mn = p.Min, mx = p.Max;
                    read = () =>
                    {
                        if (!double.TryParse(tb.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v))
                            v = double.TryParse(p.Default, NumberStyles.Float, CultureInfo.InvariantCulture, out var dv) ? dv : 0;
                        if (mn is { } lo) v = Math.Max(lo, v);
                        if (mx is { } hi) v = Math.Min(hi, v);
                        return p.Type == "int" ? (object)(int)Math.Round(v) : v;
                    };
                }
                else read = () => tb.Text ?? "";
                editor = tb;
            }
            editor.SetValue(Grid.ColumnProperty, 1);
            editor.Margin = new Thickness(10, 0, 0, 0);
            row.Children.Add(editor);
            editors.Add((p, editor, read));
            root.Children.Add(row);
        }

        var ok = new Button
        {
            Content = "执行",
            MinWidth = 82,
            MinHeight = 28,
            CornerRadius = new CornerRadius(6),
            Background = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
            Foreground = Brushes.White,
        };
        var cancel = new Button { Content = "取消", MinWidth = 82 };
        cancel.Classes.Add("ghost");
        var buttons = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            HorizontalAlignment = HorizontalAlignment.Right,
            Spacing = 10, Margin = new Thickness(0, 16, 0, 0),
        };
        buttons.Children.Add(cancel);
        buttons.Children.Add(ok);
        root.Children.Add(buttons);

        var dlg = new Window
        {
            Title = $"工具 · {op.Name}",
            SizeToContent = SizeToContent.WidthAndHeight,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = root,
        };
        dlg.Classes.Add("dialog");

        var tcs = new TaskCompletionSource<ToolParamResult?>();
        void Finish(ToolParamResult? result) { tcs.TrySetResult(result); dlg.Close(); }
        void Confirm()
        {
            var values = editors.Select(e => e.Read()).ToList();
            string? ca = null, cb = null;
            if (dual)
            {
                ca = comboA.SelectedItem?.ToString();
                cb = comboB.SelectedItem?.ToString();
                if (string.IsNullOrEmpty(ca) || string.IsNullOrEmpty(cb) || ca == cb) return;   // 基准/被测必选且不同
            }
            Finish(new ToolParamResult { Values = values, CloudA = ca, CloudB = cb });
        }
        ok.Click += (_, _) => Confirm();
        cancel.Click += (_, _) => Finish(null);
        dlg.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape) Finish(null);
            else if (e.Key == Key.Enter && e.Source is not TextBox) Confirm();
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

    private static Grid MakeEntityRow(string label, ComboBox combo)
    {
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("130,*"), Margin = new Thickness(0, 6) };
        g.Children.Add(new TextBlock { Text = label, FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
        combo.SetValue(Grid.ColumnProperty, 1);
        combo.Margin = new Thickness(10, 0, 0, 0);
        g.Children.Add(combo);
        return g;
    }
}
