using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 「算子库」侧边栏：顶部搜索框 + 按分组栏目折叠的算子列表（全宽行、统一对齐）。
/// 拖拽到流水线画布 = 在落点创建节点；单击 = 添加到当前流水线页面中心。
/// 栏目（分组）头支持按住拖动排序——架构与宿主「面板」列表同一套（PluginListSidebar）：
/// Content 结构稳定（Rebuild 只重填内部列表，绝不整体替换——旧实现连搜索框一起冲掉），
/// 容器级按下/移动/释放状态机（capture 在控件上，命中表驱动）。
/// 栏目头：按下-移动超 5px = 排序拖拽（落点横线指示），原地点击 = 折叠/展开；搜索过滤时禁用排序。
/// 栏目顺序持久化在 config/op_order.json（未登记的新栏目按注册顺序续后）。
/// </summary>
public sealed class NodeLibraryView : UserControl, IDisposable
{

    /// <summary>单击（非拖拽）某算子时触发，参数 = 节点类型 Id。</summary>
    public event Action<string>? OperatorClicked;

    /// <summary>拖拽算子松手时触发（指针捕获式，进程内可靠），参数 = 节点类型 Id + 屏幕坐标。</summary>
    public event Action<string, PixelPoint>? OperatorPlaced;

    private readonly HashSet<string> _collapsedGroups = new();
    private string _filter = "";
    private readonly List<string> _groupOrder = new();        // 栏目显示顺序（拖拽排序，持久化）
    private readonly Dictionary<string, Border> _headerIndex = new();   // 栏目名 → 头（视觉顺序，命中用）
    private readonly StackPanel _listPanel = new() { Spacing = 0 };     // 列表容器（Rebuild 只重填它）
    private readonly Border _dropLine = new()                 // 栏目落点指示（目标头上/下缘蓝色横线）
    {
        Background = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
        Height = 2,
        CornerRadius = new CornerRadius(1),
        HorizontalAlignment = HorizontalAlignment.Left,
        VerticalAlignment = VerticalAlignment.Top,
        IsHitTestVisible = false,
        IsVisible = false,
    };
    private readonly string? _orderPath;                      // 栏目顺序持久化文件（null=不持久化）

    // —— 容器级栏目排序状态机（与「面板」列表同一套） ——
    private string? _pressGroup;                              // 按下的栏目头（未达拖拽阈值）
    private Point _pressPos;
    private string? _dragGroup;                               // 排序拖拽中的栏目名

    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));
    private static readonly Brush Hint = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));
    private static readonly Brush Hairline = new SolidColorBrush(Color.FromArgb(0x26, 0xFF, 0xFF, 0xFF));
    private static readonly Brush GroupBg = new SolidColorBrush(Color.FromRgb(0x2A, 0x2A, 0x2E));
    private static readonly Brush SelectBlue = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF));

    public NodeLibraryView(string? orderPath = null)
    {
        _orderPath = orderPath;
        LoadOrder();

        var search = new TextBox
        {
            Watermark = "搜索算子…",
            Margin = new Thickness(8, 8, 8, 4),
            MinHeight = 28,
            FontSize = 12,
        };
        search.TextChanged += (_, _) => { _filter = search.Text ?? ""; Rebuild(); };

        var scroll = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = _listPanel,
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        root.Children.Add(search);
        scroll.SetValue(Grid.RowProperty, 1);
        root.Children.Add(scroll);
        Content = new Grid
        {
            Children = { root, _dropLine },   // 结构稳定：Rebuild 只重填 _listPanel
        };
        RebuildList();

        // 容器级指针状态机（栏目头排序）——capture 在本控件，命中表驱动
        PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
            _pressPos = e.GetPosition(this);
            _pressGroup = _filter.Length == 0 ? HitHeader(_pressPos)?.Key : null;   // 搜索过滤时禁用排序
        };
        PointerMoved += (_, e) =>
        {
            if (_pressGroup is { } pg && _dragGroup is null)
            {
                var p = e.GetPosition(this);
                if (Math.Abs(p.X - _pressPos.X) <= 5 && Math.Abs(p.Y - _pressPos.Y) <= 5) return;
                _dragGroup = pg;
                e.Pointer.Capture(this);   // 拖出行范围仍收移动/释放
            }
            if (_dragGroup is not null) UpdateDropLine(e.GetPosition(this), _dragGroup);
        };
        PointerReleased += (_, e) =>
        {
            var wasDragging = _dragGroup is not null;
            var dragGroup = _dragGroup;
            var pressGroup = _pressGroup;
            _pressGroup = null;
            _dragGroup = null;
            _dropLine.IsVisible = false;

            var pos = e.GetPosition(this);
            if (!wasDragging)
            {
                // 原地点击栏目头 = 折叠/展开
                if (pressGroup is not null && HitHeader(pos)?.Key == pressGroup)
                {
                    if (_collapsedGroups.Contains(pressGroup)) _collapsedGroups.Remove(pressGroup);
                    else _collapsedGroups.Add(pressGroup);
                    Rebuild();
                }
                return;
            }

            e.Pointer.Capture(null);
            // 栏目排序落位：相对顺序播种（未登记栏目补全）→ 移到目标前/后 → 持久化
            if (dragGroup is null || HitHeader(pos, dragGroup) is not { } hit ||
                hit.Value.TranslatePoint(new Point(0, 0), this) is not { } t)
                return;   // 落点没命中栏目头 = 取消，顺序不变

            var after = pos.Y > t.Y + hit.Value.Bounds.Height / 2;
            foreach (var g in NodeDefs.All.GroupBy(d => d.Group).Select(g => g.Key))
                if (!_groupOrder.Contains(g)) _groupOrder.Add(g);
            _groupOrder.Remove(dragGroup);
            var idx = _groupOrder.IndexOf(hit.Key);
            _groupOrder.Insert(idx < 0 ? _groupOrder.Count : idx + (after ? 1 : 0), dragGroup);
            SaveOrder();
            Rebuild();
        };
    }

    public void Dispose() { }

    // ==================== 栏目顺序（拖拽排序 + 持久化） ====================

    private void LoadOrder()
    {
        if (_orderPath is null || !File.Exists(_orderPath)) return;
        try
        {
            foreach (var name in System.Text.Json.JsonSerializer.Deserialize<string[]>(File.ReadAllText(_orderPath)) ?? Array.Empty<string>())
                if (!string.IsNullOrWhiteSpace(name)) _groupOrder.Add(name);
        }
        catch { }   // 顺序文件损坏=回退注册顺序
    }

    private void SaveOrder()
    {
        if (_orderPath is null) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_orderPath)!);
            File.WriteAllText(_orderPath, System.Text.Json.JsonSerializer.Serialize(_groupOrder));
        }
        catch { }   // 保存失败只影响下次启动的顺序
    }

    /// <summary>指针下的栏目头（exclude=拖拽来源栏目）。</summary>
    private KeyValuePair<string, Border>? HitHeader(Point pos, string? exclude = null)
    {
        foreach (var kv in _headerIndex)
        {
            if (exclude is not null && kv.Key == exclude) continue;
            if (kv.Value.TranslatePoint(new Point(0, 0), this) is not { } t) continue;
            if (pos.X < t.X || pos.X > t.X + kv.Value.Bounds.Width || pos.Y < t.Y || pos.Y > t.Y + kv.Value.Bounds.Height) continue;
            return kv;
        }
        return null;
    }

    /// <summary>落点指示线：贴目标栏目头上/下缘横贯行宽（上/下半决定插其前/后）。</summary>
    private void UpdateDropLine(Point pos, string dragGroup)
    {
        if (HitHeader(pos, dragGroup) is not { } hit || hit.Value.TranslatePoint(new Point(0, 0), this) is not { } t)
        {
            _dropLine.IsVisible = false;
            return;
        }
        var after = pos.Y > t.Y + hit.Value.Bounds.Height / 2;
        _dropLine.Margin = new Thickness(t.X, after ? t.Y + hit.Value.Bounds.Height - 1 : t.Y - 1, 0, 0);
        _dropLine.Width = hit.Value.Bounds.Width;
        _dropLine.IsVisible = true;
    }

    // ==================== 列表构建 ====================

    /// <summary>只重填内部列表（Content 结构稳定——搜索框/落点指示线永不脱离可视树）。</summary>
    private void Rebuild() => RebuildList();

    private void RebuildList()
    {
        _listPanel.Children.Clear();
        _headerIndex.Clear();

        // ===== 算子分组：栏目按拖拽顺序（未登记的新栏目按注册顺序续后），过滤搜索词 =====
        var groups = NodeDefs.All.GroupBy(d => d.Group).ToList();
        var ordered = _groupOrder.Where(n => groups.Any(g => g.Key == n))
                                 .Select(n => groups.First(g => g.Key == n))
                                 .Concat(groups.Where(g => !_groupOrder.Contains(g.Key)));

        foreach (var group in ordered)
        {
            var defs = group.Where(MatchFilter).ToList();
            if (defs.Count == 0) continue;

            bool expanded = !_collapsedGroups.Contains(group.Key);
            var header = GroupHeader(group.Key, defs.Count, expanded);
            _headerIndex[group.Key] = header;
            _listPanel.Children.Add(header);

            if (expanded)
                foreach (var def in defs)
                    _listPanel.Children.Add(OperatorRow(def));
        }

        if (_filter.Length > 0 && _listPanel.Children.Count == 0)
            _listPanel.Children.Add(new TextBlock
            {
                Text = "没有匹配的算子",
                FontSize = 11.5,
                Foreground = Hint,
                Margin = new Thickness(12, 10, 0, 0),
            });
    }

    private bool MatchFilter(NodeDef def) =>
        _filter.Length == 0 ||
        def.Name.Contains(_filter, StringComparison.OrdinalIgnoreCase) ||
        def.Group.Contains(_filter, StringComparison.OrdinalIgnoreCase);

    private bool MatchFilter(string name) =>
        _filter.Length == 0 || name.Contains(_filter, StringComparison.OrdinalIgnoreCase);

    /// <summary>栏目头（无自带指针处理器——排序状态机收在容器级）。</summary>
    private Border GroupHeader(string name, int count, bool expanded)
    {
        var arrow = new TextBlock
        {
            Text = expanded ? "▾" : "▸",
            FontSize = 10,
            Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 6, 0),
        };
        var title = new TextBlock
        {
            Text = name,
            FontSize = 12,
            FontWeight = FontWeight.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var countText = new TextBlock
        {
            Text = count.ToString(),
            FontSize = 11,
            Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(6, 0, 0, 0),
        };

        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,Auto,*,Auto") };
        grid.Children.Add(arrow);
        title.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(title);
        countText.SetValue(Grid.ColumnProperty, 2);
        grid.Children.Add(countText);

        var header = new Border
        {
            Height = 30,
            Background = GroupBg,
            BorderBrush = Hairline,
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(10, 0, 8, 0),
            // 与搜索框同宽内收：全宽贴边时色带比其它内容更靠左，视觉上像溢出到活动栏
            Margin = new Thickness(8, 0, 8, 0),
            CornerRadius = new CornerRadius(4),
            Cursor = new Cursor(StandardCursorType.Hand),
            Child = grid,
        };
        ToolTip.SetTip(header, "按住拖动可排序栏目；点击折叠/展开");
        return header;
    }

    // ==================== 算子条目 ====================

    private Control OperatorRow(NodeDef def)
    {
        var defId = def.Id;
        var item = MakeRow(CategoryColor(def.Category), def.Name, def.Id);

        Point? pressPoint = null;
        bool moving = false;

        item.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(item).Properties.IsLeftButtonPressed) return;
            pressPoint = e.GetPosition(item);
            moving = false;
            e.Pointer.Capture(item);
            item.Background = SelectBlue;
        };

        item.PointerMoved += (_, e) =>
        {
            if (pressPoint is null) return;
            if (!e.GetCurrentPoint(item).Properties.IsLeftButtonPressed) return;
            var p = e.GetPosition(item);
            if (Math.Abs(p.X - pressPoint.Value.X) > 5 || Math.Abs(p.Y - pressPoint.Value.Y) > 5)
                moving = true;
        };

        item.PointerReleased += (_, e) =>
        {
            bool wasMoving = moving;
            pressPoint = null;
            moving = false;
            e.Pointer.Capture(null);
            ResetItem(item);

            if (wasMoving)
            {
                var screen = item.PointToScreen(e.GetPosition(item));
                OperatorPlaced?.Invoke(defId, screen);
            }
            else
            {
                OperatorClicked?.Invoke(defId);
            }
        };

        item.PointerExited += (_, _) => { if (pressPoint is null) ResetItem(item); };
        ToolTip.SetTip(item, def.Id);
        return item;
    }

    private static Border MakeRow(Color dotColor, string text, string tooltip)
    {
        var row = new Border
        {
            Height = 28,
            Background = Brushes.Transparent,
            Margin = new Thickness(8, 0, 8, 0),
            CornerRadius = new CornerRadius(4),
            Cursor = new Cursor(StandardCursorType.Hand),
            Child = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 8,
                VerticalAlignment = VerticalAlignment.Center,
                Margin = new Thickness(14, 0, 0, 0),
                Children =
                {
                    new Border
                    {
                        Width = 9,
                        Height = 9,
                        CornerRadius = new CornerRadius(2),
                        Background = new SolidColorBrush(dotColor),
                        VerticalAlignment = VerticalAlignment.Center,
                    },
                    new TextBlock
                    {
                        Text = text,
                        FontSize = 12.5,
                        VerticalAlignment = VerticalAlignment.Center,
                        TextTrimming = TextTrimming.CharacterEllipsis,
                    },
                },
            },
        };
        ToolTip.SetTip(row, tooltip);
        return row;
    }

    private static void ResetItem(Border item) =>
        item.Background = Brushes.Transparent;

    private static Color CategoryColor(string category) => category switch
    {
        "源" => Color.FromRgb(0x2F, 0x6B, 0x3A),
        "输出" => Color.FromRgb(0x8A, 0x5A, 0x1F),
        _ => Color.FromRgb(0x15, 0x5A, 0x8A),
    };
}
