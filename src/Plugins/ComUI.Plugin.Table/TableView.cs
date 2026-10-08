using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Data;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using ComUI.Sdk;

namespace ComUI.Plugin.Table;

/// <summary>数据表格面板：工具栏 + DataGrid（Avalonia 跨平台表格控件）。</summary>
public sealed class TableView : UserControl, IDisposable
{
    private readonly IPluginContext? _ctx;
    private readonly Random _random = new();
    private readonly DataGrid _grid = new();
    private readonly DispatcherTimer _timer;
    private bool _autoRefresh;

    private static readonly string[] StatusValues = { "正常", "正常", "正常", "正常", "偏高", "偏低", "异常" };

    public TableView(IPluginContext? ctx = null)
    {
        _ctx = ctx;

        var toolbar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 0 };

        var genBtn = new Button { Content = "⟳ 生成数据 (200 行)", Margin = new Thickness(0, 0, 8, 0) };
        genBtn.Click += (_, _) => Generate(200);
        toolbar.Children.Add(genBtn);

        var autoBtn = MakeGhostButton("▶ 自动刷新：关");
        autoBtn.Click += (_, _) =>
        {
            _autoRefresh = !_autoRefresh;
            autoBtn.Content = _autoRefresh ? "⏸ 自动刷新：开" : "▶ 自动刷新：关";
            if (_autoRefresh) _timer.Start();
            else _timer.Stop();
            _ctx?.SetStatus($"数据表格：自动刷新已{(_autoRefresh ? "开启" : "关闭")}");
        };
        toolbar.Children.Add(autoBtn);

        var clearBtn = MakeGhostButton("清空");
        clearBtn.Click += (_, _) => { _grid.ItemsSource = null; _ctx?.SetStatus("数据表格：已清空"); };
        toolbar.Children.Add(clearBtn);

        toolbar.Children.Add(new TextBlock
        {
            Text = "表格类显示示例：结构化数据、列排序、行选择",
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C)),
            FontSize = 11,
            Margin = new Thickness(14, 0, 0, 0),
        });

        var toolbarBorder = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x25, 0x25, 0x26)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x3E, 0x3E, 0x42)),
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(10, 7, 10, 7),
            Child = toolbar,
        };

        ConfigureGrid();

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        toolbarBorder.SetValue(Grid.RowProperty, 0);
        root.Children.Add(toolbarBorder);
        _grid.SetValue(Grid.RowProperty, 1);
        root.Children.Add(_grid);
        Content = root;

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(800) };
        _timer.Tick += (_, _) => Generate(200, quiet: true);

        Generate(200);
    }

    private void ConfigureGrid()
    {
        _grid.AutoGenerateColumns = false;
        _grid.IsReadOnly = true;
        _grid.GridLinesVisibility = DataGridGridLinesVisibility.Horizontal;
        _grid.HeadersVisibility = DataGridHeadersVisibility.Column;
        _grid.SelectionMode = DataGridSelectionMode.Single;
        _grid.FontSize = 12;

        _grid.Columns.Add(new DataGridTextColumn { Header = "序号", Binding = new Binding("Id"), Width = new DataGridLength(70) });
        _grid.Columns.Add(new DataGridTextColumn { Header = "时间", Binding = new Binding("Time"), Width = new DataGridLength(120) });
        _grid.Columns.Add(new DataGridTextColumn { Header = "通道 A", Binding = new Binding("ChA"), Width = new DataGridLength(1, DataGridLengthUnitType.Star) });
        _grid.Columns.Add(new DataGridTextColumn { Header = "通道 B", Binding = new Binding("ChB"), Width = new DataGridLength(1, DataGridLengthUnitType.Star) });
        _grid.Columns.Add(new DataGridTextColumn { Header = "状态", Binding = new Binding("Status"), Width = new DataGridLength(90) });
    }

    private void Generate(int count, bool quiet = false)
    {
        var now = DateTime.Now;
        var rows = new List<SignalRow>(count);
        for (int i = 0; i < count; i++)
        {
            rows.Add(new SignalRow
            {
                Id = i + 1,
                Time = now.AddMilliseconds(-(count - i) * 50).ToString("HH:mm:ss.fff", CultureInfo.InvariantCulture),
                ChA = Math.Round(_random.NextDouble() * 100 - 20, 2),
                ChB = Math.Round(_random.NextDouble() * 60, 2),
                Status = StatusValues[_random.Next(StatusValues.Length)],
            });
        }
        _grid.ItemsSource = rows;
        if (!quiet)
            _ctx?.SetStatus($"数据表格：已生成 {count} 行数据");
    }

    private static Button MakeGhostButton(string text)
    {
        var btn = new Button { Content = text, Margin = new Thickness(0, 0, 8, 0) };
        btn.Classes.Add("ghost");
        return btn;
    }

    public void Dispose() => _timer.Stop();
}

/// <summary>表格一行数据。</summary>
public sealed class SignalRow
{
    public int Id { get; init; }
    public string Time { get; init; } = "";
    public double ChA { get; init; }
    public double ChB { get; init; }
    public string Status { get; init; } = "";
}
