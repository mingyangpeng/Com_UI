using Avalonia.Controls;
using ComUI.Core;
using Avalonia.Controls.Documents;
using Avalonia.Controls.Templates;
using Avalonia.Media;

namespace ComUI.Host.Views;

/// <summary>底部日志视图（紧凑行距，样式 logList 定义于 App.axaml）。</summary>
public sealed class LogView : UserControl
{
    private readonly ListBox _list = new()
    {
        FontFamily = new FontFamily("Consolas, Menlo, monospace"),
        FontSize = 12,
    };

    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));

    public LogView()
    {
        _list.Classes.Add("logList");

        _list.ItemTemplate = new FuncDataTemplate<LogEntry>((e, _) =>
        {
            if (e is null) return new Control();
            var tb = new TextBlock { TextWrapping = TextWrapping.NoWrap };
            tb.Inlines.Add(new Run { Text = e.TimeString + "  ", Foreground = Dim });
            tb.Inlines.Add(new Run { Text = e.LevelTag + "  ", Foreground = e.LevelBrush });
            tb.Inlines.Add(new Run { Text = e.Message });
            return tb;
        });

        Content = _list;
    }

    public void Append(LogEntry e)
    {
        _list.Items.Add(e);
        if (_list.Items.Count > 1000)
            _list.Items.RemoveAt(0);
        _list.ScrollIntoView(_list.Items[^1]);
    }

    public void Clear() => _list.Items.Clear();
}
