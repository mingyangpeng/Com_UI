using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using ComUI.Sdk;
using ComUI.Sdk.Ui;

namespace ComUI.Plugin.NodeGraph;

/// <summary>
/// 「算子库」侧边栏（macOS 风格）：顶部搜索框 + 一级分组栏目 + 二级子模块 + 图标卡片网格。
/// 算子以「小图标 + 名称」卡片呈现（两列自动换行），替代旧版全宽逐行罗列——大幅缩短纵向长度；
/// 每个一级分组内按语义再分二级子模块（OperatorGlyphs 推导），方便快速查找。
/// 拖拽到流水线画布 = 在落点创建节点；单击 = 右侧属性面板显示说明+参数编辑+加入画布。
/// 栏目（一级分组）头支持按住拖动排序——架构与宿主「面板」列表同一套（PluginListSidebar）：
/// Content 结构稳定（Rebuild 只重填内部列表，绝不整体替换——旧实现连搜索框一起冲掉），
/// 容器级按下/移动/释放状态机（capture 在控件上，命中表驱动）。
/// 栏目头：按下-移动超 5px = 排序拖拽（落点横线指示），原地点击 = 折叠/展开；搜索过滤时禁用排序。
/// 栏目顺序持久化在 config/op_order.json（旧版纯数组格式自动兼容）。
/// 拖拽卡片 = 直接放置（用面板当前参数）。
/// </summary>
public sealed class NodeLibraryView : UserControl, IDisposable, IPanelParamsProvider
{

    /// <summary>属性面板「加入画布」按钮触发（参数 = 节点类型 Id；面板参数覆盖由宿主读取应用）。</summary>
    public event Action<string>? OperatorClicked;

    /// <summary>拖拽算子松手时触发（指针捕获式，进程内可靠），参数 = 节点类型 Id + 屏幕坐标。</summary>
    public event Action<string, PixelPoint>? OperatorPlaced;

    /// <summary>请求删除子流程算子（defId, 来源 pipeline.json 路径）。实际删文件+注销由插件处理。</summary>
    public event Action<string, string>? OperatorDeleteRequested;

    private readonly HashSet<string> _collapsedGroups = new();
    private string _filter = "";
    private string? _selectedDefId;                            // 属性面板当前显示的算子 Id（单击行切换）
    /// <summary>算子参数覆盖（面板里编辑的值）：defId → 参数名 → 文本。加入画布时由宿主应用到新节点。</summary>
    public IReadOnlyDictionary<string, Dictionary<string, string>> ParamOverrides => _paramOverrides;
    private readonly Dictionary<string, Dictionary<string, string>> _paramOverrides = new();
    private readonly List<string> _groupOrder = new();        // 栏目显示顺序（拖拽排序，持久化）
    private readonly Dictionary<string, Border> _headerIndex = new();   // 栏目名 → 头（视觉顺序，命中用）
    private readonly StackPanel _listPanel = new() { Spacing = 0 };     // 列表容器（Rebuild 只重填它）
    private Border? _selectedCard;                            // 当前选中卡片（选中态蓝色高亮）
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
    private readonly IBus? _bus;                              // 总线：点击算子时推送属性栏展示（PropsShow）

    // —— 容器级栏目排序状态机（与「面板」列表同一套） ——
    private string? _pressGroup;                              // 按下的栏目头（未达拖拽阈值）
    private Point _pressPos;
    private string? _dragGroup;                               // 排序拖拽中的栏目名

    // macOS 深色配色：柔和层级灰 + 系统蓝强调 + 细分隔线
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x98, 0x98, 0xA0));
    private static readonly Brush Hint = new SolidColorBrush(Color.FromRgb(0x7C, 0x7C, 0x84));
    private static readonly Brush Hairline = new SolidColorBrush(Color.FromArgb(0x1A, 0xFF, 0xFF, 0xFF));
    private static readonly Brush GroupBg = new SolidColorBrush(Color.FromRgb(0x28, 0x28, 0x2C));
    private static readonly Brush SelectBlue = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF));
    private static readonly Brush CardBg = new SolidColorBrush(Color.FromRgb(0x2C, 0x2C, 0x31));
    private static readonly Brush CardHover = new SolidColorBrush(Color.FromRgb(0x3A, 0x3A, 0x41));
    private static readonly Brush CardSelected = new SolidColorBrush(Color.FromArgb(0x38, 0x0A, 0x84, 0xFF));
    private static readonly Brush SubText = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x92));

    public NodeLibraryView(string? orderPath = null, IBus? bus = null)
    {
        _orderPath = orderPath;
        _bus = bus;
        LoadOrder();

        // macOS 风格搜索框：胶囊圆角、放大镜图标、内嵌清除按钮
        var searchBox = new TextBox
        {
            Watermark = "搜索算子",
            Classes = { "searchbox" },   // App.axaml 里覆盖焦点蓝框（保留输入功能）
            FontSize = 12,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Padding = new Thickness(0),
            VerticalContentAlignment = VerticalAlignment.Center,
            MinHeight = 26,
        };
        searchBox.TextChanged += (_, _) => { _filter = searchBox.Text ?? ""; Rebuild(); };
        var clearBtn = new Button
        {
            Content = "✕",
            FontSize = 10,
            Foreground = Dim,
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Width = 20, Height = 20,
            Padding = new Thickness(0),
            Cursor = new Cursor(StandardCursorType.Arrow),
            IsVisible = false,
        };
        clearBtn.Click += (_, _) => searchBox.Text = "";
        searchBox.TextChanged += (_, _) => clearBtn.IsVisible = !string.IsNullOrEmpty(searchBox.Text);
        var searchIcon = new TextBlock { Text = "⌕", FontSize = 16, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 6, 0) };
        var searchGrid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto") };
        searchGrid.Children.Add(searchIcon);
        searchBox.SetValue(Grid.ColumnProperty, 1);
        searchGrid.Children.Add(searchBox);
        clearBtn.SetValue(Grid.ColumnProperty, 2);
        searchGrid.Children.Add(clearBtn);
        var search = new Border
        {
            Child = searchGrid,
            Background = CardBg,
            BorderBrush = Hairline,
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(9),
            Padding = new Thickness(9, 3, 5, 3),
            Margin = new Thickness(10, 10, 4, 6),
            Height = 32,
        };

        // 折叠/展开全部栏目按钮（macOS 小图标按钮，状态随当前折叠情况切换）
        var foldBtn = new Button
        {
            Width = 32, Height = 32, Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 10, 10, 6),
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        foldBtn.Classes.Add("ghost");
        void UpdateFoldVisual()
        {
            var groups = NodeDefs.All.Select(d => d.Group).Distinct().ToList();
            bool anyExpanded = groups.Any(g => !_collapsedGroups.Contains(g));
            foldBtn.Content = new TextBlock
            {
                Text = anyExpanded ? "⊟" : "⊞",   // ⊟=收起全部, ⊞=展开全部
                FontSize = 15,
                Foreground = Dim,
                VerticalAlignment = VerticalAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Center,
            };
            ToolTip.SetTip(foldBtn, anyExpanded ? "折叠所有栏目" : "展开所有栏目");
        }
        foldBtn.Click += (_, _) =>
        {
            var groups = NodeDefs.All.Select(d => d.Group).Distinct().ToList();
            bool anyExpanded = groups.Any(g => !_collapsedGroups.Contains(g));
            if (anyExpanded)
                foreach (var g in groups) _collapsedGroups.Add(g);
            else
                _collapsedGroups.Clear();
            UpdateFoldVisual();
            Rebuild();
        };
        UpdateFoldVisual();

        var topRow = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        topRow.Children.Add(search);
        foldBtn.SetValue(Grid.ColumnProperty, 1);
        topRow.Children.Add(foldBtn);

        var scroll = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = _listPanel,
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        root.Children.Add(topRow);
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

    /// <summary>op_order.json 兼容两种格式：旧版纯数组 <c>["栏目A","栏目B"]</c>；新版对象 <c>{"Order":[...]}</c>。</summary>
    private void LoadOrder()
    {
        if (_orderPath is null || !File.Exists(_orderPath)) return;
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(File.ReadAllText(_orderPath));
            var root = doc.RootElement;
            if (root.ValueKind == System.Text.Json.JsonValueKind.Array)
            {
                foreach (var el in root.EnumerateArray())
                    if (el.ValueKind == System.Text.Json.JsonValueKind.String && !string.IsNullOrWhiteSpace(el.GetString()))
                        _groupOrder.Add(el.GetString()!);
                return;   // 旧版纯数组：只有顺序
            }
            if (root.ValueKind == System.Text.Json.JsonValueKind.Object)
            {
                if (root.TryGetProperty("Order", out var order) && order.ValueKind == System.Text.Json.JsonValueKind.Array)
                    foreach (var el in order.EnumerateArray())
                        if (el.ValueKind == System.Text.Json.JsonValueKind.String && !string.IsNullOrWhiteSpace(el.GetString()))
                            _groupOrder.Add(el.GetString()!);
            }
        }
        catch { }   // 顺序文件损坏=回退注册顺序
    }

    private void SaveOrder()
    {
        if (_orderPath is null) return;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_orderPath)!);
            var obj = new System.Text.Json.Nodes.JsonObject
            {
                ["Order"] = new System.Text.Json.Nodes.JsonArray(_groupOrder.Select(n => (System.Text.Json.Nodes.JsonNode?)n).ToArray()),
            };
            File.WriteAllText(_orderPath, obj.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
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
    /// <summary>重建算子库列表（外部算子注册变化后调用，如转算子即时刷新）。</summary>
    public void Rebuild() => RebuildList();

    private void RebuildList()
    {
        _listPanel.Children.Clear();
        _headerIndex.Clear();
        _selectedCard = null;

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

            if (!expanded) continue;

            // 二级子模块：按 OperatorGlyphs 推导的 Sub 分组，注册顺序续排
            var subGroups = defs.Select(d => (Def: d, Vis: OperatorGlyphs.For(d)))
                                .GroupBy(x => x.Vis.Sub).ToList();
            foreach (var sub in subGroups)
            {
                // 搜索结果里若该组只有 1 项且子模块标题冗余，仅 1 个二级分组时省略标题
                if (subGroups.Count > 1)
                    _listPanel.Children.Add(SubHeader(sub.Key, sub.Count()));

                var grid = new WrapPanel
                {
                    Orientation = Orientation.Horizontal,
                    Margin = new Thickness(8, subGroups.Count > 1 ? 2 : 5, 8, 8),
                };
                foreach (var x in sub)
                    grid.Children.Add(OperatorCard(x.Def, x.Vis));
                _listPanel.Children.Add(grid);
            }
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
        var (groupIcon, groupAccent) = OperatorGlyphs.ForGroup(name);
        var icon = new Border
        {
            Width = 18, Height = 18,
            CornerRadius = new CornerRadius(5),
            Background = new SolidColorBrush(Color.FromArgb(0x30, groupAccent.R, groupAccent.G, groupAccent.B)),
            Child = new Viewbox { Width = 12, Height = 12, Child = new TextBlock { Text = groupIcon, FontSize = 11 } },
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 7, 0),
        };
        var arrow = new TextBlock
        {
            Text = expanded ? "▾" : "▸",
            FontSize = 10,
            Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 5, 0),
        };
        var title = new TextBlock
        {
            Text = name,
            FontSize = 12,
            FontWeight = FontWeight.SemiBold,
            Foreground = new SolidColorBrush(Color.FromRgb(0xE8, 0xE8, 0xEC)),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var countText = new TextBlock
        {
            Text = count.ToString(),
            FontSize = 10.5,
            Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(7, 0, 0, 0),
        };
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,Auto,Auto,*") };
        grid.Children.Add(icon);
        arrow.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(arrow);
        title.SetValue(Grid.ColumnProperty, 2);
        grid.Children.Add(title);
        countText.SetValue(Grid.ColumnProperty, 3);
        grid.Children.Add(countText);

        var header = new Border
        {
            Height = 30,
            Background = GroupBg,
            BorderBrush = Hairline,
            BorderThickness = new Thickness(0, 0, 0, 1),
            Padding = new Thickness(9, 0, 7, 0),
            Margin = new Thickness(8, 4, 8, 0),
            CornerRadius = new CornerRadius(7),
            Cursor = new Cursor(StandardCursorType.Hand),
            Child = grid,
        };
        ToolTip.SetTip(header, "按住拖动可排序栏目；点击折叠/展开");
        return header;
    }

    /// <summary>二级子模块标题：细体灰字 + 左侧短线，轻量不抢分组头视觉。</summary>
    private Border SubHeader(string name, int count)
    {
        var bar = new Border
        {
            Width = 2, Height = 11,
            Background = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
            CornerRadius = new CornerRadius(1),
            Margin = new Thickness(0, 0, 6, 0),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var label = new TextBlock
        {
            Text = name,
            FontSize = 11,
            FontWeight = FontWeight.SemiBold,
            Foreground = SubText,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var num = new TextBlock
        {
            Text = count.ToString(),
            FontSize = 10,
            Foreground = Hint,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(6, 0, 0, 0),
        };
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,Auto,Auto,*") };
        grid.Children.Add(bar);
        label.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(label);
        num.SetValue(Grid.ColumnProperty, 2);
        grid.Children.Add(num);
        return new Border
        {
            Child = grid,
            Height = 18,
            Margin = new Thickness(13, 6, 13, 0),
            Padding = new Thickness(2, 0, 0, 0),
        };
    }

    // ==================== 算子条目 ====================

    /// <summary>算子属性面板（行下展开）：说明 + 参数编辑器 + 「加入画布」。
    /// 参数编辑值存 _paramOverrides——加入画布时由宿主应用到新节点（覆盖算子默认值）。</summary>
    // ==================== 右侧属性面板（IPanelParamsProvider） ====================

    private StackPanel? _paramsEditorHost;
    private Button? _paramsAddBtn;
    private Button? _paramsDeleteBtn;

    /// <summary>右侧属性面板的算子内容（自更新控件实例——行点击原地刷新，宿主零改动）。
    /// 未选中时显示提示；选中后显示说明 + 参数编辑器 + 「加入画布」。</summary>
    public Control? CreateParamsContent()
    {
        var root = CreateParamsContentOnce();
        RefreshParamsContent();
        return root;
    }

    /// <summary>
    /// 单击算子卡片：刷新自绘参数控件，并通过总线把算子的说明/信息行/参数控件推给宿主属性栏。
    /// Tool 侧边栏不进标签页，无法走 CreateParamsContent 自动挂载——这是主动推送通道。
    /// </summary>
    private void PushOperatorToProps(NodeDef def)
    {
        // 确保参数控件已创建（宿主 Launch 时不会为 Tool 面板调用 CreateParamsContent）
        var content = CreateParamsContentOnce();
        RefreshParamsContent();

        if (_bus is null) return;
        var vis = OperatorGlyphs.For(def);
        var rows = new List<(string, string)>
        {
            ("类型 Id", def.Id),
            ("分组", def.Group),
            ("子模块", vis.Sub),
            ("类别", def.Category),
            ("输入端口", def.Inputs.Length == 0 ? "无" : string.Join("、", def.Inputs.Select(p => p.Name))),
            ("输出端口", def.Outputs.Length == 0 ? "无" : string.Join("、", def.Outputs.Select(p => p.Name))),
            ("参数个数", def.Params.Length.ToString()),
        };
        _bus.Publish(BusTopics.PropsShow, new PropsRequest
        {
            SectionTitle = "算子",
            Title = $"{vis.Icon}  {def.Name}",
            Description = string.IsNullOrEmpty(def.Description) ? "（该算子未提供详细说明）" : def.Description,
            Rows = rows,
            ParamsContent = content,
        });
    }

    /// <summary>
    /// 每次构建全新的参数控件根。Avalonia 控件只能有一个视觉父级——
    /// 宿主 SetParams 每次 Clear+Add，若复用同一实例会触发 "already has a visual parent"，
    /// 异常被总线吞掉后属性栏整体不更新。故改为每次点击新建（旧实例随 Clear 被丢弃）。
    /// </summary>
    private Control CreateParamsContentOnce() => BuildParamsRoot();

    private Control BuildParamsRoot()
    {
        // 标题/说明已由宿主对象卡（PropsRequest.Title/Description）显示，此处不重复——只放编辑器 + 操作。
        var root = new StackPanel { Spacing = 6 };
        _paramsEditorHost = new StackPanel { Spacing = 6 };
        _paramsAddBtn = new Button
        {
            Content = "➕ 加入画布",
            Classes = { "primary" },
            FontSize = 11.5,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        _paramsAddBtn.Click += (_, _) => { if (_selectedDefId is not null) OperatorClicked?.Invoke(_selectedDefId); };
        root.Children.Add(_paramsEditorHost);
        root.Children.Add(_paramsAddBtn);

        // 删除子流程算子：仅内置以外（有来源 pipeline 文件）的算子可见。删除前弹窗二次确认。
        _paramsDeleteBtn = new Button
        {
            Content = TrashGlyph.WithText("删除此算子", 13),
            Classes = { "danger" },
            FontSize = 11.5,
            HorizontalAlignment = HorizontalAlignment.Stretch,
            IsVisible = false,
        };
        _paramsDeleteBtn.Click += async (_, _) => await DeleteSelectedOperatorAsync();
        root.Children.Add(_paramsDeleteBtn);
        return root;
    }

    /// <summary>删除当前选中的子流程算子：二次确认 → 触发事件由插件删文件+注销+刷新。</summary>
    private async Task DeleteSelectedOperatorAsync()
    {
        if (_selectedDefId is null || !NodeDefs.Map.TryGetValue(_selectedDefId, out var def) ||
            def.SourcePipelinePath is not { } path)
            return;   // 内置算子没有来源文件，不可删
        var owner = TopLevel.GetTopLevel(this) as Window;
        bool ok = await ConfirmDialog.ShowAsync(owner, "确认删除算子",
            $"确定要删除算子「{def.Name}」吗？\n\n这会同时删除它对应的算法流水线文件，使用该算子的流水线将无法运行。\n此操作不可恢复。",
            "删除算子", danger: true);
        if (!ok) return;
        OperatorDeleteRequested?.Invoke(def.Id, path);
    }

    /// <summary>行点击：把选中算子的说明/参数编辑器刷新进右侧属性面板（编辑值写 _paramOverrides）。</summary>
    private void RefreshParamsContent()
    {
        if (_paramsEditorHost is null || _paramsAddBtn is null || _paramsDeleteBtn is null)
            return;
        _paramsEditorHost.Children.Clear();
        if (_selectedDefId is null || !NodeDefs.Map.TryGetValue(_selectedDefId, out var def))
        {
            _paramsAddBtn.IsVisible = false;
            _paramsDeleteBtn.IsVisible = false;
            return;
        }
        _paramsAddBtn.IsVisible = true;
        // 仅子流程算子（有来源文件）可删除；内置算子隐藏删除按钮
        _paramsDeleteBtn.IsVisible = def.SourcePipelinePath is not null;

        var overrides = _paramOverrides.TryGetValue(def.Id, out var ov) ? ov : new Dictionary<string, string>();
        if (!_paramOverrides.ContainsKey(def.Id)) _paramOverrides[def.Id] = overrides;
        const double fieldW = 92;   // 输入框固定窄宽（macOS 表单：标签占左、控件右对齐、不撑满）
        foreach (var p in def.Params)
        {
            var row = new Grid
            {
                ColumnDefinitions = new ColumnDefinitions("*,Auto"),
                Margin = new Thickness(0, 3),
            };
            var labelHost = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
            labelHost.Children.Add(new TextBlock
            {
                Text = p.Label,
                FontSize = 11,
                Foreground = Dim,
                TextWrapping = TextWrapping.Wrap,
            });
            if (!string.IsNullOrWhiteSpace(p.Description))
            {
                labelHost.Children.Add(new TextBlock
                {
                    Text = p.Description, FontSize = 10, Foreground = Dim,
                    TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 1, 10, 0),
                    Opacity = 0.75,
                });
                ToolTip.SetTip(row, p.Description);
            }
            row.Children.Add(labelHost);
            Control editor;
            var current = overrides.TryGetValue(p.Name, out var ovText) && ovText is not null ? ovText : p.Default?.ToString() ?? "";
            switch (p.Type)
            {
                case ComUI.Sdk.AlgoParamType.Boolean:
                    var cb = new CheckBox { IsChecked = current == "True" || current == "true", HorizontalAlignment = HorizontalAlignment.Right };
                    cb.IsCheckedChanged += (_, _) => overrides[p.Name] = cb.IsChecked == true ? "true" : "false";
                    editor = cb;
                    break;
                case ComUI.Sdk.AlgoParamType.Enum when p.Options is { } opts:
                    var combo = new ComboBox { Width = fieldW, MinHeight = 24, FontSize = 11, HorizontalAlignment = HorizontalAlignment.Right };
                    foreach (var o in opts) combo.Items.Add(o);
                    combo.SelectedIndex = Math.Max(0, opts.ToList().IndexOf(current));
                    combo.SelectionChanged += (_, _) => overrides[p.Name] = combo.SelectedItem?.ToString() ?? "";
                    editor = combo;
                    break;
                default:
                    var tb = new TextBox
                    {
                        Text = current, Width = fieldW, MinHeight = 24, FontSize = 11,
                        Padding = new Thickness(6, 3, 6, 3),
                        HorizontalAlignment = HorizontalAlignment.Right,
                        CornerRadius = new CornerRadius(5),
                    };
                    tb.TextChanged += (_, _) => overrides[p.Name] = tb.Text;
                    editor = tb;
                    break;
            }
            editor.SetValue(Grid.ColumnProperty, 1);
            row.Children.Add(editor);
            _paramsEditorHost.Children.Add(row);
        }
        if (def.Params.Length == 0)
            _paramsEditorHost.Children.Add(new TextBlock { Text = "本算子无参数", FontSize = 11, Foreground = Dim });
    }

    private Control OperatorCard(NodeDef def, OpVisual vis)
    {
        var defId = def.Id;
        var item = MakeCard(vis, def.Name);

        Point? pressPoint = null;
        bool moving = false;

        item.PointerPressed += (_, e) =>
        {
            if (!e.GetCurrentPoint(item).Properties.IsLeftButtonPressed) return;
            pressPoint = e.GetPosition(item);
            moving = false;
            e.Pointer.Capture(item);
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

            if (wasMoving)
            {
                ApplyCardVisual(item, false, false);
                var screen = item.PointToScreen(e.GetPosition(item));
                OperatorPlaced?.Invoke(defId, screen);
            }
            else
            {
                // 单击行为：第一次 = 选中（属性面板显示说明/参数）；再次单击已选中算子 = 添加到当前显示的流水线页。
                bool alreadySelected = _selectedDefId == defId;
                if (_selectedCard is not null && _selectedCard != item) ApplyCardVisual(_selectedCard, false, false);
                _selectedCard = item;
                ApplyCardVisual(item, true, false);
                _selectedDefId = defId;
                PushOperatorToProps(def);

                if (alreadySelected)
                {
                    // 复用「加入画布」回调：它把算子加到当前显示的流水线页（_activeEditor），并应用面板参数覆盖。
                    OperatorClicked?.Invoke(defId);
                    FlashCard(item);   // 短暂脉冲反馈，表明已添加
                }
            }
        };

        item.PointerEntered += (_, _) => { if (pressPoint is null && item != _selectedCard) ApplyCardVisual(item, false, true); };
        item.PointerExited += (_, _) => { if (pressPoint is null && item != _selectedCard) ApplyCardVisual(item, false, false); };
        ToolTip.SetTip(item, def.Id);
        return item;
    }

    /// <summary>
    /// 算子卡片：竖向图标视图（macOS Launchpad 风格）——图标在上、完整名称在下（两行内）。
    /// 紧凑 54×56，图标 18×18、名称 FontSize=9 允许换行完整显示，算子 Id 在 ToolTip。
    /// </summary>
    private Border MakeCard(OpVisual vis, string text)
    {
        var iconBox = new Border
        {
            Width = 18, Height = 18,
            CornerRadius = new CornerRadius(5),
            Background = new SolidColorBrush(Color.FromArgb(0x2E, vis.Accent.R, vis.Accent.G, vis.Accent.B)),
            Child = new Viewbox { Width = 12, Height = 12, Stretch = Stretch.Uniform, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
                                  Child = new TextBlock { Text = vis.Icon, FontSize = 12 } },
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        var label = new TextBlock
        {
            Text = text,
            FontSize = 9,
            Foreground = new SolidColorBrush(Color.FromRgb(0xDE, 0xDE, 0xE3)),
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Top,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
            MaxLines = 2,
            LineHeight = 11,
            TextTrimming = TextTrimming.CharacterEllipsis,
        };
        var content = new StackPanel
        {
            Orientation = Orientation.Vertical,
            Spacing = 3,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Children = { iconBox, label },
        };

        return new Border
        {
            Width = 54,
            Height = 56,
            Background = CardBg,
            BorderBrush = Hairline,
            BorderThickness = new Thickness(1),
            Padding = new Thickness(2, 4, 2, 3),
            Margin = new Thickness(1.5),
            CornerRadius = new CornerRadius(8),
            Cursor = new Cursor(StandardCursorType.Hand),
            Child = content,
        };
    }

    /// <summary>统一切换卡片三态：选中（蓝底白字）/ hover（浅灰）/ 常态。</summary>
    private static void ApplyCardVisual(Border card, bool selected, bool hover)
    {
        card.Background = selected ? CardSelected : hover ? CardHover : CardBg;
        card.BorderBrush = selected ? SelectBlue : Hairline;
    }

    /// <summary>再次点击已选中算子并加入画布后，短暂把边框闪成绿色作反馈，再恢复选中态。</summary>
    private void FlashCard(Border card)
    {
        var green = new SolidColorBrush(Color.FromRgb(0x30, 0xD1, 0x76));
        card.BorderBrush = green;
        card.BorderThickness = new Thickness(2);
        var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(320) };
        EventHandler? tick = null;
        tick = (_, _) =>
        {
            timer.Stop();
            timer.Tick -= tick;
            card.BorderThickness = new Thickness(1);
            // 仅当仍是当前选中卡片才恢复选中态视觉，避免与其它卡片状态错乱
            ApplyCardVisual(card, ReferenceEquals(card, _selectedCard), false);
        };
        timer.Tick += tick;
        timer.Start();
    }
}
