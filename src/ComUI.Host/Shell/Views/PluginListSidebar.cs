using System.Collections.Generic;
using System.IO;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using ComUI.Core;
using ComUI.Sdk;

namespace ComUI.Host.Views;

/// <summary>
/// 「面板」侧边栏：按插件分组列出已注册面板，单击打开（或聚焦）一个标签页，按住行拖动可排序栏目。
/// 行=普通 Border（非 Button），拖拽/点击状态机收在本容器一级：
/// ButtonBase 的指针捕获/Handled/Click 语义与拖拽天然纠缠（释放被标 Handled 收不到、
/// Click 需要抑制、模板命中陷阱——BUG-064 同源），去 Button 化后这些分支全部消失。
/// </summary>
public sealed class PluginListSidebar : UserControl
{
    private static string OrderPath => Path.Combine(AppPaths.ConfigDir, "panel_order.json");

    private sealed record RowEntry(string PluginId, Border Row, UiPluginRecord Plugin, PanelDescriptor Panel);

    private static readonly IBrush RowTextBrush = new SolidColorBrush(Color.FromRgb(0xCF, 0xCF, 0xD4));
    private static readonly IBrush RowHoverBrush = new SolidColorBrush(Color.FromRgb(0x2E, 0x2E, 0x33));

    private readonly Action<UiPluginRecord, PanelDescriptor> _onOpen;
    private readonly StackPanel _root = new() { Margin = new Thickness(8, 10), Spacing = 2 };
    private readonly List<string> _order = new();          // 插件栏目显示顺序（拖拽排序，持久化）
    private readonly List<RowEntry> _rows = new();         // 行注册表（视觉顺序；命中/点击/拖拽共用）
    private readonly Border _dropLine = new()              // 拖拽落点指示（目标行上/下缘的蓝色横线）
    {
        Background = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
        Height = 2,
        CornerRadius = new CornerRadius(1),
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Top,
        IsHitTestVisible = false,
        IsVisible = false,
    };
    private IReadOnlyList<UiPluginRecord> _lastPlugins = Array.Empty<UiPluginRecord>();

    // —— 容器级拖拽状态机：按下记录 → 移动超 5px 进入拖拽（捕获在本容器）→ 释放落位/取消 ——
    private RowEntry? _pressEntry;                         // 按下的行（未达拖拽阈值）
    private Point _pressPos;
    private string? _dragPlugin;                           // 拖拽中的插件 Id

    public PluginListSidebar(Action<UiPluginRecord, PanelDescriptor> onOpen)
    {
        _onOpen = onOpen;
        try
        {
            if (File.Exists(OrderPath))
                foreach (var id in System.Text.Json.JsonSerializer.Deserialize<string[]>(File.ReadAllText(OrderPath)) ?? Array.Empty<string>())
                    if (!string.IsNullOrWhiteSpace(id)) _order.Add(id);
        }
        catch { }   // 顺序文件损坏=回退注册顺序
        Background = new SolidColorBrush(Color.FromRgb(0x23, 0x23, 0x26));
        Content = new Grid
        {
            Children =
            {
                new ScrollViewer
                {
                    VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
                    Content = _root,
                },
                _dropLine,
            },
        };

        PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            _pressPos = e.GetPosition(this);
            _pressEntry = HitRow(_pressPos);
        };
        PointerMoved += (_, e) =>
        {
            if (_pressEntry is { } pe && _dragPlugin is null)
            {
                var p = e.GetPosition(this);
                if (Math.Abs(p.X - _pressPos.X) <= 5 && Math.Abs(p.Y - _pressPos.Y) <= 5) return;
                _dragPlugin = pe.PluginId;
                pe.Row.Opacity = 0.55;
                foreach (var en in _rows) ToolTip.SetTip(en.Row, null);   // 拖拽中不弹工具提示（慢速拖拽会恰好挡住释放点）
                e.Pointer.Capture(this);   // 拖出行范围仍收移动/释放
            }
            if (_dragPlugin is not null) UpdateDropLine(e.GetPosition(this), _dragPlugin);
        };
        PointerReleased += (_, e) =>
        {
            // 不过滤释放键位：非左键的按下不会置 _pressEntry/_dragPlugin，这里自然无副作用
            var wasDragging = _dragPlugin is not null;
            var dragPlugin = _dragPlugin;
            var pressEntry = _pressEntry;
            _pressEntry = null;
            _dragPlugin = null;
            _dropLine.IsVisible = false;

            var pos = e.GetPosition(this);
            if (!wasDragging)
            {
                // 纯点击：按下与释放落在同一行 → 打开面板
                if (pressEntry is { } pe && ReferenceEquals(HitRow(pos), pe))
                    _onOpen(pe.Plugin, pe.Panel);
                return;
            }

            e.Pointer.Capture(null);
            if (dragPlugin is null || HitRow(pos, dragPlugin) is not { } target ||
                target.Row.TranslatePoint(new Point(0, 0), this) is not { } t)
            {
                Refresh(_lastPlugins);   // 无落点：取消，行重建还原透明度与工具提示
                return;
            }
            var after = pos.Y > t.Y + target.Row.Bounds.Height / 2;   // 上半=插到其前，下半=其后

            // 相对顺序播种：顺序表不全时按当前视觉顺序补全（首次拖拽/新插件），
            // 否则"首拖到某行上/下半"算不出正确相对位置
            foreach (var en in _rows)
                if (!_order.Contains(en.PluginId)) _order.Add(en.PluginId);

            _order.Remove(dragPlugin);
            var idx = _order.IndexOf(target.PluginId);
            _order.Insert(idx < 0 ? _order.Count : idx + (after ? 1 : 0), dragPlugin);
            SaveOrder();
            Refresh(_lastPlugins);
        };
    }

    public void Refresh(IReadOnlyList<UiPluginRecord> plugins)
    {
        _root.Children.Clear();
        _rows.Clear();
        _lastPlugins = plugins;

        if (plugins.Count == 0)
        {
            _root.Children.Add(EmptyHint("尚无已加载的 UI 插件\n\n将插件文件夹放入 comdll/ui/ 后\n点击菜单「文件 → 重新加载插件」(F5)"));
            return;
        }

        // 拖拽排序的栏目顺序优先（未知插件按注册顺序排在其后；OrderBy 稳定）
        foreach (var plugin in plugins.OrderBy(p => { var i = _order.IndexOf(p.Meta.Id); return i < 0 ? int.MaxValue : i; }))
        {
            // 面板列表只列窗口面板（Document=开标签页的）；侧边栏工具（算子库/数据树/总线监视等）
            // 属于活动栏图标，不属于 UI 窗口，不在此列出
            var docPanels = plugin.Panels.Where(pn => pn.Role == PanelRole.Document).ToList();
            if (docPanels.Count == 0) continue;

            _root.Children.Add(new TextBlock
            {
                Text = $"{plugin.Name}  v{plugin.Version}",
                FontSize = 11.5,
                Foreground = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x92)),
                Margin = new Thickness(6, 10, 0, 4),
            });

            foreach (var panel in docPanels)
            {
                var row = BuildRow(plugin, panel);
                _rows.Add(new RowEntry(plugin.Meta.Id, row, plugin, panel));
                _root.Children.Add(row);
            }
        }
    }

    private Border BuildRow(UiPluginRecord plugin, PanelDescriptor panel)
    {
        var row = new Border
        {
            Padding = new Thickness(10, 6),
            CornerRadius = new CornerRadius(5),
            Background = Brushes.Transparent,
            Cursor = new Cursor(StandardCursorType.Hand),
            Child = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
                Children =
                {
                    // emoji 归一化到 14×14，防止位图 emoji 字形（🧰 等）放大溢出压到标题
                    new Viewbox { Width = 14, Height = 14, Child = new TextBlock { Text = string.IsNullOrWhiteSpace(panel.Icon) ? "▫" : panel.Icon!, FontSize = 13 } },
                    new TextBlock { Text = panel.Title, VerticalAlignment = VerticalAlignment.Center, FontSize = 12.5, Foreground = RowTextBrush },
                },
            },
        };
        ToolTip.SetTip(row, $"打开「{panel.Title}」（{panel.Role}）—— 按住拖动可排序栏目");
        row.PointerEntered += (_, _) => { if (_dragPlugin is null) row.Background = RowHoverBrush; };
        row.PointerExited += (_, _) => row.Background = Brushes.Transparent;
        return row;
    }

    /// <summary>指针下的行（excludePluginId=跳过拖拽来源插件自己的行）。</summary>
    private RowEntry? HitRow(Point pos, string? excludePluginId = null)
    {
        foreach (var en in _rows)
        {
            if (excludePluginId is not null && en.PluginId == excludePluginId) continue;
            if (en.Row.TranslatePoint(new Point(0, 0), this) is not { } t) continue;
            if (pos.X < t.X || pos.X > t.X + en.Row.Bounds.Width || pos.Y < t.Y || pos.Y > t.Y + en.Row.Bounds.Height) continue;
            return en;
        }
        return null;
    }

    /// <summary>落点指示线：贴目标行上/下缘横贯行宽。</summary>
    private void UpdateDropLine(Point pos, string dragPlugin)
    {
        if (HitRow(pos, dragPlugin) is not { } target || target.Row.TranslatePoint(new Point(0, 0), this) is not { } t)
        {
            _dropLine.IsVisible = false;
            return;
        }
        var after = pos.Y > t.Y + target.Row.Bounds.Height / 2;
        _dropLine.Margin = new Thickness(t.X, after ? t.Y + target.Row.Bounds.Height - 1 : t.Y - 1, 0, 0);
        _dropLine.Width = target.Row.Bounds.Width;
        _dropLine.IsVisible = true;
    }

    private void SaveOrder()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(OrderPath)!);
            File.WriteAllText(OrderPath, System.Text.Json.JsonSerializer.Serialize(_order));
        }
        catch { }   // 保存失败只影响下次启动的顺序，不影响本次
    }

    private static Control EmptyHint(string text) => new TextBlock
    {
        Text = text,
        FontSize = 12,
        LineHeight = 19,
        Foreground = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7F)),
        Margin = new Thickness(8, 16, 8, 0),
        TextWrapping = TextWrapping.Wrap,
    };
}
