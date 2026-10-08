using Avalonia;
using ComUI.Core;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Host.Views;

/// <summary>右侧属性视图（标题栏 + 键值行）。</summary>
public sealed class PropertiesView : UserControl
{
    private readonly StackPanel _panelProps = new();
    private readonly TextBlock _paramsHeader;
    private readonly StackPanel _paramsHost = new();
    private readonly Separator _paramsSep;
    private readonly StackPanel _hostProps = new();

    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));

    /// <summary>用户点击标题栏收起按钮（宿主折叠右侧属性列）。</summary>
    public event Action? CollapseRequested;

    public PropertiesView()
    {
        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(12, 10, 8, 0) };
        head.Children.Add(new TextBlock { Text = "属性", FontWeight = FontWeight.SemiBold, FontSize = 12, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
        var collapse = new Button
        {
            Content = "▷",
            FontSize = 11,
            Width = 22,
            Height = 20,
            Padding = new Thickness(0),
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Foreground = Dim,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(collapse, "收起属性面板（视图菜单或活动栏 ⚙ 可重新打开）");
        collapse.Click += (_, _) => CollapseRequested?.Invoke();
        Grid.SetColumn(collapse, 1);
        head.Children.Add(collapse);

        _paramsHeader = new TextBlock { Text = "参数", FontWeight = FontWeight.SemiBold, FontSize = 12, Foreground = Dim, Margin = new Thickness(14, 12, 0, 0), IsVisible = false };
        _paramsSep = new Separator { Margin = new Thickness(12, 10, 12, 10), IsVisible = false };

        var root = new ScrollViewer
        {
            VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
            Content = new StackPanel { Spacing = 4 },
        };
        var panel = (StackPanel)root.Content!;
        panel.Children.Add(head);
        panel.Children.Add(new TextBlock { Text = "面板", FontWeight = FontWeight.SemiBold, FontSize = 12, Foreground = Dim, Margin = new Thickness(14, 10, 0, 0) });
        panel.Children.Add(_panelProps);
        panel.Children.Add(_paramsHeader);
        panel.Children.Add(_paramsHost);
        panel.Children.Add(_paramsSep);
        panel.Children.Add(new TextBlock { Text = "宿主信息", FontWeight = FontWeight.SemiBold, FontSize = 12, Foreground = Dim, Margin = new Thickness(14, 0, 0, 0) });
        panel.Children.Add(_hostProps);
        Content = root;
    }

    public void SetPanelRows(IEnumerable<(string Key, string Value)> rows)
    {
        _panelProps.Children.Clear();
        foreach (var (key, value) in rows)
            _panelProps.Children.Add(MakeRow(key, value));
    }

    public void SetHostRows(IEnumerable<(string Key, string Value)> rows)
    {
        _hostProps.Children.Clear();
        foreach (var (key, value) in rows)
            _hostProps.Children.Add(MakeRow(key, value));
    }

    /// <summary>设置激活面板的参数编辑区（null = 无参数，整节隐藏）。控件由插件提供并持有。</summary>
    public void SetParams(Control? content)
    {
        _paramsHost.Children.Clear();
        bool has = content is not null;
        if (has) _paramsHost.Children.Add(content);
        _paramsHeader.IsVisible = has;
        _paramsHost.IsVisible = has;
        _paramsSep.IsVisible = has;
    }

    private static Control MakeRow(string key, string value)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("86,*"), Margin = new Thickness(14, 3, 10, 3) };
        grid.Children.Add(new TextBlock
        {
            Text = key,
            FontSize = 12,
            Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
        });
        var val = new TextBlock
        {
            Text = value,
            FontSize = 12,
            VerticalAlignment = VerticalAlignment.Center,
        };
        val.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(val);
        return grid;
    }
}
