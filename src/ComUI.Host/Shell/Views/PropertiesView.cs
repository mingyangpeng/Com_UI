using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using ComUI.Sdk;
using ComUI.Sdk.Ui;

namespace ComUI.Host.Views;

/// <summary>
/// 右侧属性视图（macOS 系统设置风格）：分组卡片 + 键值行，留白充足、层级清晰。
/// 两种模式：
/// - 对象模式（算子库点击项经 BusTopics.PropsShow 推送）：算子详情卡 + 参数卡，隐藏面板/宿主技术信息；
/// - 面板模式（标签页切换）：面板卡 + 宿主卡。
/// 参数卡共用，切换模式只显隐上下区块，避免控件重复挂载。
/// </summary>
public sealed class PropertiesView : UserControl
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x98, 0x98, 0xA0));
    private static readonly Brush Header = new SolidColorBrush(Color.FromRgb(0xF5, 0xF5, 0xF7));
    private static readonly Brush Hairline = new SolidColorBrush(Color.FromArgb(0x18, 0xFF, 0xFF, 0xFF));

    // —— 对象模式：算子详情 ——
    private readonly StackPanel _objectSection = new() { Spacing = 0, IsVisible = false };
    private CollapsibleSection _objectCard = null!;   // 标题随 SectionTitle 变化（算子/点云/图像）
    private readonly TextBlock _objTitle = new()
    {
        FontSize = 15, FontWeight = FontWeight.SemiBold, Foreground = Header,
        TextWrapping = TextWrapping.Wrap,
    };
    private readonly TextBlock _objDesc = new()
    {
        FontSize = 11.5, Foreground = Dim, TextWrapping = TextWrapping.Wrap,
    };
    private readonly StackPanel _objRows = new() { Spacing = 0 };

    // —— 共用参数卡 ——
    private readonly StackPanel _paramSection = new() { Spacing = 0, IsVisible = false };
    private readonly StackPanel _paramHost = new() { Spacing = 6 };

    // —— 面板模式：面板 / 宿主 ——
    private readonly StackPanel _hostSection = new() { Spacing = 0 };
    private readonly StackPanel _panelProps = new() { Spacing = 0 };
    private readonly StackPanel _hostProps = new() { Spacing = 0 };

    /// <summary>用户点击标题栏收起按钮（宿主折叠右侧属性列）。</summary>
    public event Action? CollapseRequested;

    public PropertiesView()
    {
        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(16, 14, 12, 4) };
        head.Children.Add(new TextBlock { Text = "属性", FontWeight = FontWeight.SemiBold, FontSize = 15, Foreground = Header, VerticalAlignment = VerticalAlignment.Center });
        var collapse = new Button
        {
            Content = "▷",
            FontSize = 11,
            Width = 22, Height = 20, Padding = new Thickness(0),
            Background = Brushes.Transparent, BorderThickness = new Thickness(0),
            Foreground = Dim, Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(collapse, "收起属性面板（视图菜单或活动栏 ⚙ 可重新打开）");
        collapse.Click += (_, _) => CollapseRequested?.Invoke();
        Grid.SetColumn(collapse, 1);
        head.Children.Add(collapse);

        // 对象详情卡（标题 + 说明 + 分隔 + 信息行）——可折叠
        var objInner = new StackPanel { Spacing = 8 };
        objInner.Children.Add(_objTitle);
        objInner.Children.Add(_objDesc);
        objInner.Children.Add(new Border { Height = 1, Background = Hairline });
        objInner.Children.Add(_objRows);
        _objectCard = new CollapsibleSection("算子", objInner, expanded: true);
        _objectSection.Children.Add(_objectCard);

        // 参数卡（插件自绘控件）——可折叠
        var paramInner = new StackPanel { Spacing = 6 };
        paramInner.Children.Add(_paramHost);
        var paramCard = new CollapsibleSection("参数", paramInner, expanded: true);
        _paramSection.Children.Add(paramCard);

        // 面板 / 宿主卡——可折叠
        _hostSection.Children.Add(new CollapsibleSection("面板", _panelProps, expanded: true));
        _hostSection.Children.Add(new CollapsibleSection("宿主", _hostProps, expanded: false));

        var body = new StackPanel { Spacing = 2 };
        body.Children.Add(head);
        body.Children.Add(_objectSection);
        body.Children.Add(_paramSection);
        body.Children.Add(_hostSection);
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled,
            Content = body,
        };
    }

    public void SetPanelRows(IEnumerable<(string Key, string Value)> rows)
    {
        _panelProps.Children.Clear();
        foreach (var (key, value) in rows)
            _panelProps.Children.Add(MakeRow(key, value));
        // 标签页切换 → 回到面板模式
        _objectSection.IsVisible = false;
        _hostSection.IsVisible = true;
    }

    public void SetHostRows(IEnumerable<(string Key, string Value)> rows)
    {
        _hostProps.Children.Clear();
        foreach (var (key, value) in rows)
            _hostProps.Children.Add(MakeRow(key, value));
    }

    /// <summary>设置参数编辑区（null = 隐藏整卡）。控件由插件提供并持有。</summary>
    public void SetParams(Control? content)
    {
        _paramHost.Children.Clear();
        bool has = content is not null;
        if (has) _paramHost.Children.Add(content);
        _paramSection.IsVisible = has;
    }

    /// <summary>显示 Tool 侧边栏（算子库）推送的选中对象详情；参数控件挂到共用参数卡。</summary>
    public void ShowObject(PropsRequest req)
    {
        _objectCard.Title = req.SectionTitle;
        _objTitle.Text = req.Title;
        _objDesc.Text = req.Description ?? "";
        _objDesc.IsVisible = !string.IsNullOrWhiteSpace(req.Description);

        _objRows.Children.Clear();
        for (int i = 0; i < req.Rows.Count; i++)
        {
            _objRows.Children.Add(MakeRow(req.Rows[i].Key, req.Rows[i].Value));
            if (i < req.Rows.Count - 1)
                _objRows.Children.Add(new Border { Height = 1, Background = Hairline, Margin = new Thickness(0, 3) });
        }

        _objectSection.IsVisible = true;
        _hostSection.IsVisible = false;   // 对象模式下收起面板/宿主技术信息

        // 参数控件单独保护：即使插件传来的控件挂载失败（如已被其它视觉树持有），
        // 也不能拖累上面的算子信息显示——这是属性栏最核心的内容。
        try { SetParams(req.ParamsContent); }
        catch (Exception ex)
        {
            _paramHost.Children.Clear();
            _paramSection.IsVisible = false;
            try { System.IO.File.AppendAllText(ComUI.Core.AppPaths.ErrorLogFile,
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] 属性栏参数挂载失败: {ex.Message}\r\n"); } catch { }
        }
    }

    /// <summary>清空对象详情（切换标签页时由 SetPanelRows 触发显隐切换）。</summary>
    public void ClearObject() => _objectSection.IsVisible = false;

    private static Control MakeRow(string key, string value)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("86,*"), Margin = new Thickness(0, 5) };
        grid.Children.Add(new TextBlock
        {
            Text = key, FontSize = 11.5, Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
        });
        var val = new TextBlock
        {
            Text = value, FontSize = 11.5, Foreground = Header,
            VerticalAlignment = VerticalAlignment.Center, TextWrapping = TextWrapping.Wrap,
        };
        Grid.SetColumn(val, 1);
        grid.Children.Add(val);
        return grid;
    }
}
