using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Sdk.Ui;

/// <summary>
/// 苹果系统设置风格的可折叠分组卡片：标题行（▾/▸ + 名称）点击展开/收起内容。
/// 供右侧属性面板各分组、插件参数内容分组共用（SDK 层，宿主与插件均可引用）。
/// </summary>
public sealed class CollapsibleSection : Border
{
    private readonly TextBlock _chevron;
    private readonly TextBlock _headText;
    private readonly Border _contentHost;
    private bool _expanded;

    /// <summary>展开/收起状态。赋值只在状态真正变化时切换。</summary>
    public bool Expanded
    {
        get => _expanded;
        set { if (_expanded != value) Toggle(); }
    }

    /// <summary>标题文字（如"算子"/"点云"）。</summary>
    public string Title
    {
        get => _headText.Text ?? "";
        set => _headText.Text = value;
    }

    public CollapsibleSection(string title, Control content, bool expanded = true)
    {
        _expanded = expanded;
        var subtle = new SolidColorBrush(Color.FromRgb(0x8E, 0x8E, 0x93));
        var headColor = new SolidColorBrush(Color.FromRgb(0xE6, 0xE6, 0xEA));

        _chevron = new TextBlock
        {
            Text = expanded ? "▾" : "▸", FontSize = 10, Foreground = subtle,
            Width = 14, VerticalAlignment = VerticalAlignment.Center,
        };
        _headText = new TextBlock
        {
            Text = title, FontSize = 11.5, FontWeight = FontWeight.SemiBold,
            Foreground = headColor, VerticalAlignment = VerticalAlignment.Center,
        };
        var head = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("Auto,*"),
            Margin = new Thickness(12, 9),
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        head.Children.Add(_chevron);
        _headText.SetValue(Grid.ColumnProperty, 1);
        head.Children.Add(_headText);
        head.PointerPressed += (_, e) => { Toggle(); e.Handled = true; };

        _contentHost = new Border
        {
            Child = content,
            Padding = new Thickness(12, 0, 12, 12),
            IsVisible = expanded,
        };

        var inner = new StackPanel { Spacing = 0 };
        inner.Children.Add(head);
        inner.Children.Add(_contentHost);

        Background = new SolidColorBrush(Color.FromRgb(0x2C, 0x2C, 0x31));
        BorderBrush = new SolidColorBrush(Color.FromRgb(0x3A, 0x3A, 0x40));
        BorderThickness = new Thickness(1);
        CornerRadius = new CornerRadius(10);
        Margin = new Thickness(12, 0, 12, 8);
        ClipToBounds = true;
        Child = inner;
    }

    private void Toggle()
    {
        _expanded = !_expanded;
        _contentHost.IsVisible = _expanded;
        _chevron.Text = _expanded ? "▾" : "▸";
    }
}
