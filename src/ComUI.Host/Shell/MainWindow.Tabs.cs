using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.VisualTree;
using ComUI.Core.Split;
using ComUI.Host.Views;
using ComUI.Sdk;

namespace ComUI.Host.Shell;

/// <summary>
/// 主窗口的分屏标签区（partial，模型=Core/Split/SplitTree）：
/// 标签构建/开关、拆分与跨组移动、分屏树渲染、标签拖拽（拆分/移组/组内排序/浮动）。
/// </summary>
public partial class MainWindow
{
    // ===== 分屏标签区（SplitNode 树驱动，见 docs/ARCHITECTURE.md 已定案） =====
    private SplitLayout _layout = new();                                 // 分屏树（Core 模型；默认布局重置时换新）
    private readonly Dictionary<string, TabItem> _tabs = new();         // 唯一键 → 标签页（键=面板 Id / doc:N / welcome）
    private SplitLeaf? _activeLeaf;                                     // 最近聚焦的标签组（新面板落点）
    private int _docSeq;                                                // 动态文档页序号

    // —— 标签拖拽（按下标签头移动 5px 进入拖拽，预览放置区域，松手落位；Esc 取消） ——
    private (TabItem Tab, Point Start)? _dragPending;                   // 已按下未达拖拽阈值
    private TabItem? _dragTab;                                          // 拖拽中的标签
    private bool _dragging;
    private Canvas? _dragOverlay;
    private Border? _dragPreview;
    private Border? _dragInsertLine;                                    // 组内排序插入位置指示（竖线）
    private readonly List<(SplitLeaf Leaf, TabControl Control)> _groups = new();   // 渲染时重建

    private bool _rebuilding;   // 分屏重建中：Group_SelectionChanged 的自动选中是拆除噪声，不得回写模型
    private bool _reordering;   // 组内重排中：Remove+Insert 引发的选中噪声，同样不得回写模型

    /// <summary>确保欢迎页在（可关闭的普通标签：BuildTabItem 全套——✕/右键菜单/拖拽）。
    /// 没有则插入第一个标签组首位；根是容器也安全（取第一个叶子，不强转 Root）。</summary>
    private void EnsureWelcomeTab()
    {
        if (_tabs.ContainsKey("welcome")) return;
        var welcome = BuildTabItem("welcome", "欢迎", new WelcomeView());
        _tabs["welcome"] = welcome;
        var leaf = _layout.Leaves().First();
        leaf.PanelIds.Insert(0, "welcome");
        leaf.ActivePanelId = "welcome";
        RebuildSplitView();
    }

    /// <summary>打开（或聚焦）一个面板标签页（新页落入最近聚焦的标签组）。
    /// "已打开"必须真可达：浮动中=前置；在分屏树里=聚焦；只躺在 _tabs 里而不在任何格子
    /// （恢复会话替换模型后，AutoOpen 预建标签沦为孤儿）= 按新建处理覆盖重建，
    /// 否则聚焦分支静默无操作，面板整会话都打不开（BUG-081）。</summary>
    private void OpenPanel(PanelDescriptor panel)
    {
        var plugin = _manager.UiPlugins.FirstOrDefault(u => u.Panels.Contains(panel));
        if (plugin is null) return;

        if (_tabs.TryGetValue(panel.Id, out var existing))
        {
            if (_floating.TryGetValue(panel.Id, out var fw)) { fw.Activate(); return; }   // 面板在浮动窗：前置
            if (_layout.FindLeaf(panel.Id) is not null) { SelectTab(existing); return; }  // 在树中：聚焦
        }

        var view = GetPanelView(plugin, panel);
        var tab = BuildTabItem(panel.Id, panel.Title, view);
        _tabs[panel.Id] = tab;
        var leaf = _activeLeaf ?? _layout.Leaves().First();
        leaf.PanelIds.Add(panel.Id);
        leaf.ActivePanelId = panel.Id;
        if (TryAddTabIncremental(leaf, tab)) return;   // ★增量：不整树重建
        RebuildSplitView();
        SelectTab(tab);
    }

    /// <summary>插件请求打开的动态文档页（无 PanelDescriptor，Tag 用 "doc" 区分）。
    /// 同一内容控件重复请求 = 聚焦已有标签页（算法已打开时单击跳转，不再重复建页）。</summary>
    private void OpenDynamicDocument(string title, Control content, string? restoreKey = null)
    {
        // 同一内容控件重复请求 = 聚焦已有标签页（算法已打开时单击跳转，不再重复建页）
        var pair = _tabs.FirstOrDefault(kv => ReferenceEquals(kv.Value.Content, content));
        if (pair.Value is not null) { SelectTab(pair.Value); return; }

        var key = $"doc:{++_docSeq}";
        var tab = BuildTabItem(key, title, content);
        _tabs[key] = tab;
        _tabTitles[key] = title;
        if (restoreKey is not null) _docRestoreKeys[key] = restoreKey;
        var leaf = _activeLeaf ?? _layout.Leaves().First();
        leaf.PanelIds.Add(key);
        leaf.ActivePanelId = key;
        if (TryAddTabIncremental(leaf, tab)) return;   // ★增量：不整树重建
        RebuildSplitView();
        SelectTab(tab);
    }

    /// <summary>选中标签页 → 右侧属性同步显示该面板信息。
    /// 注意：XAML InitializeComponent 期间 SelectionChanged 就会触发一次，此时 _manager 尚未创建，必须守卫。</summary>
    private void Group_SelectionChanged(object? sender, SelectionChangedEventArgs e)
    {
        if (_manager is null) return;
        if (_rebuilding || _reordering) return;   // 重建/重排期间拆除旧组的自动选中是噪声——不得覆盖模型 ActivePanelId（BUG-072：新开面板不跳转）
        if (sender is TabControl tc && tc.SelectedItem is TabItem sel)
        {
            var key = TabKey(sel);
            if (_layout.FindLeaf(key) is { } leaf) { leaf.ActivePanelId = key; _activeLeaf = leaf; }
            SyncPropsFor(sel);
        }
    }

    /// <summary>按标签页同步属性面板与关联联动（欢迎/动态页清空参数区）。</summary>
    private void SyncPropsFor(TabItem tab)
    {
        if (tab.Tag is PanelTag tag)
        {
            var plugin = _manager.UiPlugins.FirstOrDefault(u => u.Panels.Any(p => p.Id == tag.PanelId));
            var panel = plugin?.Panels.FirstOrDefault(p => p.Id == tag.PanelId);
            if (plugin is not null && panel is not null)
            {
                UpdatePropsFor(panel, plugin);
                if (panel.Role == PanelRole.Document)
                    SwitchSidebarForAssociation(tag.PanelId);
                return;
            }
        }
        _propsView.SetParams(null);   // 欢迎/动态文档页：清空参数区
    }

    private static string TabKey(TabItem t) => t.Tag switch
    {
        PanelTag tag => tag.PanelId,
        string s => s,
        _ => "",
    };

    /// <summary>关闭所有面板标签页（欢迎页保留）。</summary>
    private void CloseAllTabs()
    {
        foreach (var key in _tabs.Where(kv => kv.Value.Tag is PanelTag).Select(kv => kv.Key).ToList())
            CloseTab(_tabs[key]);
    }

    // ==================== 分屏标签区（树驱动） ====================

    /// <summary>构建标签页（标题/✕/右键分屏菜单）。</summary>
    private TabItem BuildTabItem(string key, string title, Control content)
    {
        var header = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        header.Children.Add(new TextBlock { Text = title, VerticalAlignment = VerticalAlignment.Center });
        var close = new Button { Content = "✕", Classes = { "tabclose" } };
        var tab = new TabItem { Tag = key, Header = header, Content = content };
        _tabTitles[key] = title;
        close.Click += (_, _) => CloseTab(tab);
        header.Children.Add(close);
        // 拖拽候选：按下记录起点（移动超 5px 才进入拖拽，不影响单击选中与 ✕ 点击）
        header.PointerPressed += (_, e) =>
        {
            if (e.GetCurrentPoint(header).Properties.IsLeftButtonPressed)
                _dragPending = (tab, e.GetPosition(SplitHost));
        };
        tab.ContextMenu = BuildTabMenu(tab);
        return tab;
    }

    /// <summary>标签右键菜单：拆分（拖拽的键盘/自动化兜底）+ 跨组移动 + 浮动 + 关闭。</summary>
    private ContextMenu BuildTabMenu(TabItem tab)
    {
        var menu = new ContextMenu();
        void Add(string header, Action act)
        {
            var mi = new MenuItem { Header = header };
            mi.Click += (_, _) => act();
            menu.Items.Add(mi);
        }
        Add("向左拆分", () => SplitTab(tab, SplitSide.Left));
        Add("向右拆分", () => SplitTab(tab, SplitSide.Right));
        Add("向上拆分", () => SplitTab(tab, SplitSide.Top));
        Add("向下拆分", () => SplitTab(tab, SplitSide.Bottom));
        menu.Items.Add(new Separator());
        Add("移到下一格", () => MoveTabNext(tab));
        Add("浮动窗口", () => FloatTab(tab));
        menu.Items.Add(new Separator());
        Add("关闭标签", () => CloseTab(tab));
        return menu;
    }

    /// <summary>把标签拆分到指定方位的新格子（同面板两格禁止：实例随标签移动，源组不保留）。</summary>
    private void SplitTab(TabItem tab, SplitSide side)
    {
        var key = TabKey(tab);
        var leaf = _layout.FindLeaf(key);
        if (leaf is null) return;
        var newLeaf = _layout.Split(leaf, side);
        _layout.MovePanel(leaf, key, newLeaf);
        RebuildSplitView();
        SelectTab(tab);
    }

    /// <summary>把标签移到下一个标签组（循环）。</summary>
    private void MoveTabNext(TabItem tab)
    {
        var key = TabKey(tab);
        var leaf = _layout.FindLeaf(key);
        if (leaf is null) return;
        var leaves = _layout.Leaves().ToList();
        if (leaves.Count < 2) return;
        var next = leaves[(leaves.IndexOf(leaf) + 1) % leaves.Count];
        _layout.MovePanel(leaf, key, next);
        RebuildSplitView();
        SelectTab(tab);
    }

    /// <summary>关闭标签：模型回收空格/退化单子容器，选组内相邻标签。</summary>
    private void CloseTab(TabItem tab)
    {
        var key = TabKey(tab);
        if (_floating.TryGetValue(key, out var floatWin)) { _redocking = false; _floating.Remove(key); floatWin.Close(); return; }   // 浮动窗关闭触发清理
        if (!_tabs.Remove(key)) return;
        var leaf = _layout.FindLeaf(key);
        if (leaf is not null) _layout.RemovePanel(leaf, key);

        // ★增量：组内还有其他标签 → 只摘这一个（不整树重建）；空叶回收才走重建
        var tc = TabControlOf(tab);
        if (tc is not null && leaf is not null && leaf.PanelIds.Count > 0 && tc.Items.Contains(tab))
        {
            ReleaseTabContent(tc, tab);
            try { tc.Items.Remove(tab); }
            catch { try { tc.Items.Clear(); } catch { } }
            if (leaf.ActivePanelId is { } nextKey && _tabs.TryGetValue(nextKey, out var next))
            {
                tc.SelectedItem = next;
                SyncPropsFor(next);
            }
            else if (tc.Items.Count > 0)
            {
                tc.SelectedIndex = Math.Max(0, tc.Items.Count - 1);   // 模型未给下一激活：兜底选末位，避免无选中空内容
            }
            return;
        }
        RebuildSplitView();
        if (leaf?.ActivePanelId is { } nk && _tabs.TryGetValue(nk, out var nt))
            SyncPropsFor(nt);
    }

    /// <summary>分屏叶子的活动 TabControl（经 _groups 注册表反查）。</summary>
    private TabControl? GroupControlOf(SplitLeaf leaf)
    {
        foreach (var (l, tc) in _groups)
            if (ReferenceEquals(l, leaf)) return tc;
        return null;
    }

    /// <summary>
    /// 增量挂载：目标组已存在时直接加标签并选中——不整树重建。
    /// 重建（拆/装所有 TabControl）是 BUG-065~073 的温床（内容呈现器持有/自动选中噪声/布局推迟/生成器失同步），
    /// 日常开标签走本路径：挂载中的组模板已应用，选中同步生效。失败返回 false 走重建兜底。
    /// </summary>
    private bool TryAddTabIncremental(SplitLeaf leaf, TabItem tab)
    {
        var tc = GroupControlOf(leaf);
        if (tc is null) return false;   // 组还没建（首次）→ 重建
        try { tc.Items.Add(tab); }
        catch (Exception ex)
        {
            _log.Error($"标签增量挂载失败 {TabKey(tab)}: {ex.Message}");
            return false;
        }
        tc.SelectedItem = tab;
        SyncPropsFor(tab);
        return true;
    }

    /// <summary>释放标签内容与呈现器的持有（关闭/重排/重建前）。
    /// 关键：清空选中会被 TabControl 立刻自动补选（常把本标签又选回去——它还在 Items 里），
    /// 内容随之合法重挂 → 布局泵永远脱不开 → 强摘把呈现器 Content 写成局部值、
    /// 永久压掉模板绑定（组存活时=内容区从此空白，被释放标签在 0 号位时必现）。
    /// 因此"释放"=把选中移到组内其他标签，呈现器自然换内容、本页随之脱离；
    /// 组内没有其他标签（该组即将随重建/回收丢弃）才清空+强摘，楔子随整组消失。</summary>
    private void ReleaseTabContent(TabControl tc, TabItem tab)
    {
        if (!ReferenceEquals(tc.SelectedItem, tab)) return;   // 只有选中项的内容挂在 SelectedContentHost 上
        var other = tc.Items.OfType<TabItem>().FirstOrDefault(t => !ReferenceEquals(t, tab));
        if (other is not null)
            tc.SelectedItem = other;
        else
            tc.SelectedItem = null;
        // 释放走绑定+布局泵——一次 UpdateLayout 未必泵完，有界循环直到真正脱离
        for (int i = 0; i < 6; i++)
        {
            if ((tab.Content as Avalonia.Visual)?.GetVisualParent() is null) break;
            tc.UpdateLayout();
        }
        // 布局泵没释放成功（驱动/时序不可靠）→ 直接从呈现器上摘内容（仅剩单标签=组即将丢弃，无害）
        if ((tab.Content as Avalonia.Visual)?.GetVisualParent() is not null)
        {
            var host = tc.GetVisualDescendants().OfType<Avalonia.Controls.Presenters.ContentPresenter>()
                .FirstOrDefault(h => h.Name == "PART_SelectedContentHost");
            try { if (host is not null) host.Content = null; } catch { }
        }
    }

    /// <summary>标签所在的活动 TabControl：优先分屏注册表（重建后 ti.Parent 可能为 null——BUG-067）。</summary>
    private TabControl? TabControlOf(TabItem tab)
    {
        var key = TabKey(tab);
        foreach (var (leaf, tc) in _groups)
            if (leaf.PanelIds.Contains(key) && tc.Items.Contains(tab))
                return tc;
        return tab.Parent as TabControl;
    }

    private void SelectTab(TabItem tab)
    {
        TabControlOf(tab)?.SelectedItem = tab;
        SyncPropsFor(tab);
    }

    private void SelectTab(string key)
    {
        if (_tabs.TryGetValue(key, out var tab)) SelectTab(tab);
    }

    /// <summary>按分屏树重建标签区（网格 + 每格标签组 + 格间 GridSplitter）。</summary>
    private void RebuildSplitView()
    {
        _rebuilding = true;
        try
        {
            // 先把所有标签页从旧组摘除——TabControl 模板的内容/头呈现器持有视图，
            // 直接重建格子会"已有视觉父级"冲突（BUG-037 同源教训：复用控件先摘除再挂载）。
            // 两段式：先清各格选中（选中项的内容由 PART_SelectedContentHost 持有——BUG-065），
            // 再摘除全部容器；经 _groups 的活 TabControl 操作，不依赖 ti.Parent（BUG-067）。
            foreach (var (_, tc) in _groups)
                if (tc.SelectedItem is TabItem sel)
                    ReleaseTabContent(tc, sel);   // 清选中+释放内容（布局泵+强制摘除兜底）
            foreach (var (_, tc) in _groups)
            {
                foreach (var item in tc.Items.OfType<TabItem>().ToList())
                {
                    try { tc.Items.Remove(item); }
                    catch { try { tc.Items.Clear(); } catch { } }   // 生成器失同步兜底：整组即将丢弃
                }
            }
            _groups.Clear();
            SplitHost.Children.Clear();
            SplitHost.Children.Add(RenderNode(_layout.Root));
            // 重建后强制同步布局：模板应用/选中内容呈现默认推迟到下一帧，
            // 会被后续输入打断成"视觉落后模型一步"（BUG-073）
            SplitHost.UpdateLayout();
        }
        catch (Exception ex)
        {
            // 自愈：个别视图没释放干净导致挂载抛"already a child"（恢复/驱动时序类）——
            _log.Error($"分屏重建失败（自愈重试）: {ex.Message}");
            try
            {
                foreach (var ti in _tabs.Values)
                    ForceDetachContent(ti);
                _groups.Clear();
                SplitHost.Children.Clear();
                SplitHost.Children.Add(RenderNode(_layout.Root));
                SplitHost.UpdateLayout();
            }
            catch (Exception ex2)
            {
                _log.Error($"分屏重建自愈仍失败: {ex2.Message}");
            }
        }
        finally { _rebuilding = false; }
    }

    /// <summary>把标签内容从当前视觉父级上强制摘除（自愈路径：重建失败后全量清理）。</summary>
    private static void ForceDetachContent(TabItem ti)
    {
        if (ti.Content is not Avalonia.Visual v) return;
        switch (v.Parent)
        {
            case Avalonia.Controls.Presenters.ContentPresenter cp:
                try { cp.Content = null; } catch { }
                break;
            case Panel panel:
                try { panel.Children.Remove((Control)v); } catch { }
                break;
        }
    }

    private Control RenderNode(SplitNode node)
    {
        if (node is SplitLeaf leaf) return BuildGroup(leaf);
        var c = (SplitContainer)node;
        var grid = new Grid();
        // 直接用 GridLength 对象构建（字符串解析路径曾抛 FormatException，绕开）
        for (int i = 0; i < c.Children.Count; i++)
        {
            if (i > 0)
            {
                var gap = new ColumnDefinition(new GridLength(4, GridUnitType.Pixel));
                var rowGap = new RowDefinition(new GridLength(4, GridUnitType.Pixel));
                if (c.Vertical) grid.RowDefinitions.Add(rowGap); else grid.ColumnDefinitions.Add(gap);
            }
            var star = new ColumnDefinition(new GridLength(c.Stars[i], GridUnitType.Star));
            var rowStar = new RowDefinition(new GridLength(c.Stars[i], GridUnitType.Star));
            if (c.Vertical) grid.RowDefinitions.Add(rowStar); else grid.ColumnDefinitions.Add(star);
        }

        for (int i = 0; i < c.Children.Count; i++)
        {
            var child = RenderNode(c.Children[i]);
            if (i > 0)
            {
                var splitter = new GridSplitter { Background = new SolidColorBrush(Color.FromRgb(0x2A, 0x2A, 0x2E)) };
                if (c.Vertical) { splitter.Height = 4; Grid.SetRow(splitter, i * 2 - 1); }
                else { splitter.Width = 4; Grid.SetColumn(splitter, i * 2 - 1); }
                grid.Children.Add(splitter);
            }
            grid.Children.Add(child);
            if (c.Vertical) Grid.SetRow(child, i * 2);
            else Grid.SetColumn(child, i * 2);
        }
        return grid;
    }

    /// <summary>构建一个标签组（TabControl，承载该组全部标签页）。</summary>
    private Control BuildGroup(SplitLeaf leaf)
    {
        var tc = new TabControl { Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20)) };
        foreach (var key in leaf.PanelIds)
        {
            if (!_tabs.TryGetValue(key, out var ti)) continue;
            try { tc.Items.Add(ti); }
            catch (Exception ex)
            {
                _log.Error($"标签挂载失败 {key}: {ex.Message}");
            }
        }
        var activeIdx = leaf.ActivePanelId is null ? -1 : leaf.PanelIds.IndexOf(leaf.ActivePanelId);
        // 先挂事件再设选中项（顺序反了事件全丢——选中态恢复时 _activeLeaf/属性面板不更新）
        tc.SelectionChanged += Group_SelectionChanged;
        tc.SelectedIndex = activeIdx >= 0 ? activeIdx : Math.Max(0, tc.Items.Count - 1);
        _groups.Add((leaf, tc));
        return tc;
    }

    // ==================== 标签拖拽 ====================

    private void Window_PointMoved(object? sender, PointerEventArgs e)
    {
        if (_dragPending is { } pending)
        {
            var pos = e.GetPosition(SplitHost);
            var dx = pos.X - pending.Start.X;
            var dy = pos.Y - pending.Start.Y;
            if (dx * dx + dy * dy > 25)   // 5px 阈值进入拖拽
            {
                _dragTab = pending.Tab;
                _dragging = true;
                _dragPending = null;
                StartDragOverlay();
                e.Pointer.Capture(this);   // 拖出窗口也能收到移动/释放
            }
        }
        if (_dragging) UpdateDragOverlay(e.GetPosition(SplitHost));
    }

    private void Window_PointReleased(object? sender, PointerEventArgs e)
    {
        if (_dragging)
        {
            var tab = _dragTab!;
            var winPos = e.GetPosition(this);
            var outside = winPos.X < 0 || winPos.Y < 0 || winPos.X > Bounds.Width || winPos.Y > Bounds.Height;
            var dropPos = e.GetPosition(SplitHost);
            EndDrag();
            e.Pointer.Capture(null);
            if (outside) FloatTab(tab);   // 拖出主窗边界松手 = 浮动
            else DropTab(tab, dropPos);
        }
        _dragPending = null;
    }

    private void StartDragOverlay()
    {
        _dragOverlay = new Canvas { IsHitTestVisible = false, Background = Brushes.Transparent };
        _dragPreview = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(0x30, 0x4F, 0xC3, 0xF7)),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)),
            BorderThickness = new Thickness(2),
            CornerRadius = new CornerRadius(4),
            IsHitTestVisible = false,
        };
        _dragInsertLine = new Border
        {
            Background = new SolidColorBrush(Color.FromRgb(0x4F, 0xC3, 0xF7)),
            Width = 3,
            CornerRadius = new CornerRadius(1.5),
            IsHitTestVisible = false,
            IsVisible = false,
        };
        _dragOverlay.Children.Add(_dragPreview);
        _dragOverlay.Children.Add(_dragInsertLine);
        SplitHost.Children.Add(_dragOverlay);
    }

    /// <summary>命中检测：指针落在哪个标签组 + 区域。
    /// 标签条高度内 = "center"（按插入位置处理：同组排序/跨组插位）——沿标签条拖放是排序手势，
    /// 不能算"上边缘拆分"；边缘 28% 带（标签条以下）= 方向拆分；其余中心 = 移入该组末尾。</summary>
    private (SplitLeaf Leaf, Rect Rect, string Zone)? HitGroup(Point pos)
    {
        foreach (var (leaf, tc) in _groups)
        {
            var origin = tc.TranslatePoint(new Point(0, 0), SplitHost);
            if (origin is null) continue;
            var rect = new Rect(origin.Value, tc.Bounds.Size);
            if (!rect.Contains(pos)) continue;
            if (pos.Y - rect.Y <= TabStripHeight(tc) + 2) return (leaf, rect, "center");
            var rx = (pos.X - rect.X) / rect.Width;
            var ry = (pos.Y - rect.Y) / rect.Height;
            var zone = rx < 0.28 ? "left" : rx > 0.72 ? "right"
                     : ry < 0.28 ? "top" : ry > 0.72 ? "bottom" : "center";
            return (leaf, rect, zone);
        }
        return null;
    }

    /// <summary>拖拽预览：边缘=半格高亮（方向拆分）；中心=标签条上竖直插入线（移入/组内排序的落点）。</summary>
    private void UpdateDragOverlay(Point pos)
    {
        if (_dragPreview is null || _dragInsertLine is null) return;
        var hit = HitGroup(pos);
        if (hit is null || hit.Value.Zone == "center" && GroupControlOf(hit.Value.Leaf) is null)
        {
            _dragPreview.IsVisible = false;
            _dragInsertLine.IsVisible = false;
            return;
        }
        var (leaf, rect, zone) = hit.Value;
        if (zone == "center")
        {
            // 中心 = 插入线（落在哪个标签头区间就插到哪）
            var tc = GroupControlOf(leaf)!;
            var idx = ComputeInsertionIndex(tc, pos, _dragTab);
            var x = InsertIndicatorX(tc, idx, _dragTab, rect.X);
            var stripHeight = TabStripHeight(tc);
            _dragPreview.IsVisible = false;
            _dragInsertLine.IsVisible = true;
            Canvas.SetLeft(_dragInsertLine, x);
            Canvas.SetTop(_dragInsertLine, rect.Y);
            _dragInsertLine.Height = stripHeight;
            return;
        }

        _dragInsertLine.IsVisible = false;
        _dragPreview.IsVisible = true;
        Canvas.SetLeft(_dragPreview, rect.X);
        Canvas.SetTop(_dragPreview, rect.Y);
        _dragPreview.Width = rect.Width;
        _dragPreview.Height = rect.Height;
        switch (zone)
        {
            case "left": _dragPreview.Width = rect.Width * 0.5; break;
            case "right": _dragPreview.Width = rect.Width * 0.5; Canvas.SetLeft(_dragPreview, rect.X + rect.Width * 0.5); break;
            case "top": _dragPreview.Height = rect.Height * 0.5; break;
            case "bottom": _dragPreview.Height = rect.Height * 0.5; Canvas.SetTop(_dragPreview, rect.Y + rect.Height * 0.5); break;
        }
    }

    /// <summary>插入下标：按"不含拖拽标签"的头中心点统计指针左侧数量（删除后语义，模型/UI 两边一致）。</summary>
    private int ComputeInsertionIndex(TabControl tc, Point pos, TabItem? exclude)
    {
        int idx = 0;
        foreach (var item in tc.Items.OfType<TabItem>())
        {
            if (exclude is not null && ReferenceEquals(item, exclude)) continue;
            if (item.TranslatePoint(new Point(0, 0), SplitHost) is not { } t) continue;
            if (pos.X >= t.X + item.Bounds.Width / 2) idx++;
        }
        return idx;
    }

    /// <summary>插入指示线 X：第 idx 个（不含拖拽标签）标签头左缘；越界=末位标签右缘。</summary>
    private double InsertIndicatorX(TabControl tc, int idx, TabItem? exclude, double fallbackX)
    {
        int i = 0;
        TabItem? last = null;
        foreach (var item in tc.Items.OfType<TabItem>())
        {
            if (exclude is not null && ReferenceEquals(item, exclude)) continue;
            if (item.TranslatePoint(new Point(0, 0), SplitHost) is not { } t) continue;
            if (i == idx) return t.X;
            last = item;
            i++;
        }
        if (last is not null && last.TranslatePoint(new Point(0, 0), SplitHost) is { } lt)
            return lt.X + last.Bounds.Width - 1;
        return fallbackX;
    }

    private static double TabStripHeight(TabControl tc)
    {
        foreach (var item in tc.Items.OfType<TabItem>())
            if (item.Bounds.Height > 0) return item.Bounds.Height;
        return 30;
    }

    /// <summary>松手落位：中心=按插入位置移入该组（同组=组内排序）；边缘=朝该方向拆分出新格并移入。</summary>
    private void DropTab(TabItem tab, Point pos)
    {
        var hit = HitGroup(pos);
        if (hit is null) return;
        var (leaf, _, zone) = hit.Value;
        var key = TabKey(tab);
        var src = _layout.FindLeaf(key);
        if (src is null) return;
        if (zone == "center")
        {
            var tc = GroupControlOf(leaf);
            if (tc is null) return;
            var idx = ComputeInsertionIndex(tc, pos, tab);
            if (ReferenceEquals(src, leaf))
            {
                // 同组中心 = 组内左右排序：模型重排 + 增量摘插（不整树重建——重建是 BUG-065 系温床）
                if (_layout.ReorderPanel(leaf, key, idx))
                    ReorderTabIncremental(tab, tc, idx);
                return;
            }
            _layout.MovePanel(src, key, leaf, idx);   // 跨组：按指针位置插入（不再一律追加到末尾）
        }
        else
        {
            var side = zone switch
            {
                "left" => SplitSide.Left,
                "right" => SplitSide.Right,
                "top" => SplitSide.Top,
                _ => SplitSide.Bottom,
            };
            var newLeaf = _layout.Split(leaf, side);
            _layout.MovePanel(src, key, newLeaf);
        }
        RebuildSplitView();
        SelectTab(tab);
    }

    /// <summary>组内重排的增量落位：清选中→摘下→插回→重新选中（期间 SelectionChanged 噪声由 _reordering 抑制）。</summary>
    private void ReorderTabIncremental(TabItem tab, TabControl tc, int idx)
    {
        _reordering = true;
        try
        {
            ReleaseTabContent(tc, tab);   // 是选中项时先释放内容持有（BUG-065 同源）
            tc.Items.Remove(tab);
            tc.Items.Insert(Math.Clamp(idx, 0, tc.Items.Count), tab);
        }
        catch (Exception ex)
        {
            _log.Error($"标签组内重排失败 {TabKey(tab)}: {ex.Message}");
            RebuildSplitView();   // 生成器失同步兜底：走整树重建
            SelectTab(tab);
            return;
        }
        finally { _reordering = false; }
        tc.SelectedItem = tab;
        SyncPropsFor(tab);
    }

    private void EndDrag()
    {
        _dragging = false;
        _dragTab = null;
        if (_dragOverlay is not null)
        {
            _dragOverlay.Children.Clear();
            if (_dragOverlay.Parent is Panel p) p.Children.Remove(_dragOverlay);
        }
        _dragOverlay = null;
        _dragPreview = null;
        _dragInsertLine = null;
    }
}
