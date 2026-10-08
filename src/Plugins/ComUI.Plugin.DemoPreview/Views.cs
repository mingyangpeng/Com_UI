using System.Runtime.InteropServices;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;
using ComUI.Sdk;

namespace ComUI.Plugin.DemoPreview;


/// <summary>
/// 总线监视（窗口面板）：上半=主题表（点击选中），下半=选中主题的详细追踪——
/// 最新帧内容逐字段 + 发布历史（环形缓冲，最多 100 条）。总线级 FramePublished 钩子驱动。
/// </summary>
public sealed class BusMonitorView : UserControl, IDisposable
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));
    private static readonly Brush Hint = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));
    private static readonly Brush SelBg = new SolidColorBrush(Color.FromRgb(0x2F, 0x2F, 0x33));
    private static readonly Brush Accent = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7));

    private sealed class Frame
    {
        public DateTime Time;
        public object? Payload;
    }

    private readonly IBus _bus;
    private readonly object _gate = new();
    private readonly Dictionary<string, List<Frame>> _history = new(StringComparer.OrdinalIgnoreCase);
    private readonly DateTime _openedAt = DateTime.Now;
    private string? _selected;

    private readonly StackPanel _rows = new() { Spacing = 1 };
    private readonly TextBlock _detailTitle = new() { FontWeight = FontWeight.SemiBold, FontSize = 12.5, Margin = new Thickness(0, 0, 0, 4) };
    private readonly StackPanel _detail = new() { Spacing = 2 };
    private readonly DispatcherTimer _timer;

    public BusMonitorView(IBus bus)
    {
        _bus = bus;
        Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20));

        // 工具条
        var title = new TextBlock
        {
            Text = "📡 总线监视", FontWeight = FontWeight.SemiBold, FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var clearBtn = new Button { Content = "清空历史", Classes = { "ghost" }, FontSize = 11 };
        clearBtn.Click += (_, _) => { lock (_gate) _history.Clear(); Refresh(); RefreshDetail(); };
        var toolbar = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(12, 8, 12, 4) };
        toolbar.Children.Add(title);
        clearBtn.SetValue(Grid.ColumnProperty, 1);
        toolbar.Children.Add(clearBtn);

        // 主题表
        var header = new Grid { ColumnDefinitions = new ColumnDefinitions("30*,22*,12*,12*,24*"), Margin = new Thickness(12, 2) };
        AddHeader(header, 0, "主题");
        AddHeader(header, 1, "最新帧类型");
        AddHeader(header, 2, "帧数");
        AddHeader(header, 3, "频率");
        AddHeader(header, 4, "最后发布");
        var scroll = new ScrollViewer
        {
            VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
            Content = _rows,
        };

        // 详情区
        var detailScroll = new ScrollViewer
        {
            VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
            Content = new StackPanel { Margin = new Thickness(12, 6, 12, 10), Children = { _detailTitle, _detail } },
        };

        var grid = new Grid { RowDefinitions = new RowDefinitions("Auto,Auto,2*,Auto,3*") };
        grid.Children.Add(toolbar);
        header.SetValue(Grid.RowProperty, 1);
        grid.Children.Add(header);
        scroll.SetValue(Grid.RowProperty, 2);
        grid.Children.Add(scroll);
        var sep = new Border { Height = 1, Background = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x38)), Margin = new Thickness(8, 4, 8, 0) };
        sep.SetValue(Grid.RowProperty, 3);
        grid.Children.Add(sep);
        detailScroll.SetValue(Grid.RowProperty, 4);
        grid.Children.Add(detailScroll);
        Content = grid;

        // 播种：把总线现有主题的保留帧记为历史起点（否则刚打开时历史区空荡——"详细追踪"开箱即见）
        foreach (var t in _bus.GetTopics())
            if (_bus.TryGetLatestRaw(t.Topic, out var seed) && t.LastPublish is { } lp)
            {
                if (!_history.TryGetValue(t.Topic, out var list))
                {
                    list = new List<Frame>();
                    _history[t.Topic] = list;
                }
                list.Add(new Frame { Time = lp, Payload = seed });
            }

        // 总线级钩子：任意线程回调 → 封送 UI 线程记录历史
        _bus.FramePublished += OnFramePublished;
        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) => { Refresh(); RefreshDetail(); };
        _timer.Start();
        Refresh();
        RefreshDetail();
    }

    private void OnFramePublished(string topic, object? payload)
    {
        Avalonia.Threading.Dispatcher.UIThread.Post(() =>
        {
            lock (_gate)
            {
                if (!_history.TryGetValue(topic, out var list))
                {
                    list = new List<Frame>();
                    _history[topic] = list;
                }
                list.Add(new Frame { Time = DateTime.Now, Payload = payload });
                if (list.Count > 100) list.RemoveAt(0);
            }
        });
    }

    private void Refresh()
    {
        _rows.Children.Clear();
        var topics = _bus.GetTopics().OrderBy(t => t.Topic).ToList();
        if (topics.Count == 0)
        {
            _rows.Children.Add(new TextBlock
            {
                Text = "（总线无数据 —— 运行一个算法后这里会显示主题）",
                Foreground = Hint, FontSize = 12, Margin = new Thickness(12, 4),
            });
            return;
        }
        foreach (var t in topics)
        {
            int tracked;
            lock (_gate) tracked = _history.TryGetValue(t.Topic, out var h) ? h.Count : 0;
            var elapsed = (DateTime.Now - _openedAt).TotalSeconds;
            var freq = elapsed > 1 ? $"{t.FrameCount / elapsed:0.#}/s" : "-";
            var row = new Grid { ColumnDefinitions = new ColumnDefinitions("30*,22*,12*,12*,24*"), Margin = new Thickness(12, 1) };
            AddCell(row, 0, t.Topic, bold: true);
            AddCell(row, 1, t.LatestType ?? "-");
            AddCell(row, 2, t.FrameCount.ToString());
            AddCell(row, 3, freq);
            AddCell(row, 4, t.LastPublish?.ToString("HH:mm:ss") ?? "-");
            var border = new Border
            {
                Child = row,
                Background = _selected == t.Topic ? SelBg : Brushes.Transparent,
                CornerRadius = new CornerRadius(3),
                Padding = new Thickness(2, 1),
                Cursor = new Cursor(StandardCursorType.Hand),
            };
            border.PointerPressed += (_, e) =>
            {
                _selected = t.Topic;
                Refresh();
                RefreshDetail();
                e.Handled = true;
            };
            _rows.Children.Add(border);
        }
    }

    private void RefreshDetail()
    {
        _detail.Children.Clear();
        if (_selected is not { } topic)
        {
            _detailTitle.Text = "点击上方主题行，查看详细追踪";
            _detail.Children.Add(new TextBlock { Text = "（选中后显示：最新帧内容逐字段 + 发布历史）", Foreground = Hint, FontSize = 12 });
            return;
        }
        _detailTitle.Text = topic;

        var info = _bus.GetTopics().FirstOrDefault(t => t.Topic == topic);
        List<Frame> hist;
        lock (_gate) hist = _history.TryGetValue(topic, out var h) ? h.ToList() : new List<Frame>();

        // 概览
        _detail.Children.Add(new TextBlock
        {
            Text = $"类型 {info?.LatestType ?? "-"} · 总帧数 {info?.FrameCount ?? 0} · 打开后捕获 {hist.Count} 帧" +
                   (info?.LastPublish is { } lp ? $" · 最后发布 {lp:HH:mm:ss}" : ""),
            Foreground = Dim, FontSize = 11.5, TextWrapping = TextWrapping.Wrap,
        });

        // 最新帧内容（逐字段）
        _detail.Children.Add(new TextBlock
        {
            Text = "最新帧内容", FontWeight = FontWeight.SemiBold, FontSize = 12, Margin = new Thickness(0, 8, 0, 2),
        });
        if (_bus.TryGetLatestRaw(topic, out var latest))
        {
            foreach (var line in DumpProps(latest))
                _detail.Children.Add(new TextBlock
                {
                    Text = line, FontSize = 11.5, FontFamily = new FontFamily("Consolas, Menlo, monospace"),
                    Foreground = Brushes.White, TextWrapping = TextWrapping.Wrap,
                });
        }
        else
        {
            _detail.Children.Add(new TextBlock { Text = "（无保留帧）", Foreground = Hint, FontSize = 11.5 });
        }

        // 发布历史（最近在前）
        _detail.Children.Add(new TextBlock
        {
            Text = $"发布历史（最近 {Math.Min(hist.Count, 50)} 条）", FontWeight = FontWeight.SemiBold, FontSize = 12, Margin = new Thickness(0, 8, 0, 2),
        });
        if (hist.Count == 0)
        {
            _detail.Children.Add(new TextBlock { Text = "（打开面板后开始捕获）", Foreground = Hint, FontSize = 11.5 });
            return;
        }
        foreach (var f in hist.AsEnumerable().Reverse().Take(50))
        {
            _detail.Children.Add(new TextBlock
            {
                Text = $"{f.Time:HH:mm:ss.fff}  {Summarize(f.Payload)}",
                FontSize = 11, FontFamily = new FontFamily("Consolas, Menlo, monospace"),
                Foreground = Dim, TextTrimming = TextTrimming.CharacterEllipsis,
            });
        }
    }

    /// <summary>载荷逐字段转储（数组只显示长度，字符串截断）。</summary>
    private static IEnumerable<string> DumpProps(object? o)
    {
        if (o is null) { yield return "（null 载荷）"; yield break; }
        yield return $"类型: {o.GetType().Name}";
        foreach (var prop in o.GetType().GetProperties().OrderBy(p => p.Name))
        {
            object? v;
            try { v = prop.GetValue(o); } catch { continue; }
            var text = v switch
            {
                null => "null",
                string str => str.Length > 80 ? str[..80] + "…" : str,
                Array a => $"{a.GetType().GetElementType()?.Name}[{a.Length}]",
                _ => v.ToString() ?? ""
            };
            yield return $"{prop.Name} = {text}";
        }
    }

    private static string Summarize(object? payload)
    {
        if (payload is null) return "null";
        var parts = new List<string> { payload.GetType().Name };
        foreach (var prop in payload.GetType().GetProperties().OrderBy(p => p.Name).Take(4))
        {
            object? v;
            try { v = prop.GetValue(payload); } catch { continue; }
            var text = v switch
            {
                null => "null",
                string str => str.Length > 24 ? str[..24] + "…" : str,
                Array a => $"[{a.Length}]",
                _ => v.ToString() ?? ""
            };
            parts.Add($"{prop.Name}={text}");
        }
        return string.Join(" ", parts);
    }

    private static void AddHeader(Grid g, int col, string text)
    {
        var tb = new TextBlock
        {
            Text = text,
            FontWeight = FontWeight.SemiBold,
            FontSize = 12,
            Foreground = Dim,
        };
        tb.SetValue(Grid.ColumnProperty, col);
        g.Children.Add(tb);
    }

    private static void AddCell(Grid g, int col, string text, bool bold = false)
    {
        var tb = new TextBlock
        {
            Text = text,
            FontSize = 12,
            FontWeight = bold ? FontWeight.Medium : FontWeight.Normal,
            FontFamily = new FontFamily("Consolas, Menlo, monospace"),
            TextTrimming = TextTrimming.CharacterEllipsis,
            Foreground = bold ? Brushes.White : Brushes.White,
        };
        tb.SetValue(Grid.ColumnProperty, col);
        g.Children.Add(tb);
    }

    public void Dispose()
    {
        _bus.FramePublished -= OnFramePublished;
        _timer.Stop();
    }
}
