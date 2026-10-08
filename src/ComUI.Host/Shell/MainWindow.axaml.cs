using System.Diagnostics;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using ComUI.Core;
using ComUI.Core.Split;
using ComUI.Sdk;
using ComUI.Host.Views;

namespace ComUI.Host.Shell;

/// <summary>
/// 主窗口（手写确定性布局，无第三方停靠库）：
/// 活动栏图标 = 面板开关（点击展开/再点收起）；中间标签页；右侧属性；底部日志。
/// 所有面板状态由宿主直接管理，无 Dock 语义/延迟渲染/序列化等间接层。
/// 本文件=外壳骨架；标签分屏区/拖拽见 MainWindow.Tabs.cs，浮动窗见 MainWindow.Floating.cs，
/// 会话持久化见 MainWindow.Session.cs。
/// </summary>
public partial class MainWindow : Window
{
    public static MainWindow? Instance { get; private set; }

    private readonly LogService _log = new();
    private readonly DataBus _bus = new();
    private readonly PluginManager _manager;

    private readonly LogView _logView = new();
    private readonly PropertiesView _propsView = new();
    private PluginListSidebar _pluginList = null!;
    private AlgoSidebar _algoList = null!;

    private readonly Dictionary<string, Control> _panelViews = new();   // 面板视图缓存
    private readonly StackPanel _activityBar;
    private readonly Dictionary<string, Button> _activityButtons = new(); // 图标 → 按钮
    private readonly List<ActivityEntry> _activityEntries = new();

    private ActivityEntry? _activeSidebar;     // 当前侧边栏视图（null = 收起）
    private bool _propsCollapsed;
    private bool _logCollapsed;
    private CancellationTokenSource? _runCts;
    private bool _autoOpen = true;

    private readonly Dictionary<string, string> _tabTitles = new();        // 全部标签 key → 标题（Tabs/Floating/Session 共用）

    private TextBlock _statusText = null!;
    private TextBlock _busyText = null!;
    private TextBlock _pluginCountText = null!;
    private Button _cancelRunBtn = null!;

    public MainWindow()
    {
        InitializeComponent();
        Instance = this;
        App.UiLog = _log;   // 全局异常围栏同步送日志面板

        _manager = new PluginManager(_log, _bus, s => SetStatus(s));
        // 插件请求打开动态文档页（OpenDocument）→ 创建标签页
        _manager.UiContextCreated += ctx =>
        {
            if (ctx is HostPluginContext hostCtx)
            {
                hostCtx.OpenDocumentRequested += (title, content, restoreKey) =>
                    Dispatcher.UIThread.Post(() => OpenDynamicDocument(title, content, restoreKey));
                hostCtx.DocumentFactoryRegistered += (type, factory) => _docFactories[type] = factory;
            }
        };

        _activityBar = this.FindControl<StackPanel>("ActivityBar")!;
        _logHost = this.FindControl<ContentControl>("LogHost")!;
        _propsHost = this.FindControl<ContentControl>("PropsHost")!;
        _statusText = this.FindControl<TextBlock>("StatusText")!;
        _busyText = this.FindControl<TextBlock>("BusyText")!;
        _pluginCountText = this.FindControl<TextBlock>("PluginCountText")!;
        _cancelRunBtn = this.FindControl<Button>("CancelRunBtn")!;

        _logHost.Content = _logView;
        _propsHost.Content = _propsView;

        Closing += (_, _) =>
        {
            // 先存会话（含浮动窗几何——浮动面板关闭时会从登记表摘除，关完就丢信息了），再关浮动窗
            SaveSession();
            foreach (var w in _floating.Values.ToList()) w.Close();
        };
        RestoreWindowGeometry();   // 会话窗口几何（分屏/页面在 Start 里恢复——需等插件加载）

        _pluginList = new PluginListSidebar((_, panel) => ShowPanel(panel));
        _algoList = new AlgoSidebar((algo, cmd) => _ = RunCommandAsync(algo, cmd), _bus);
        _algoList.LogMessage = m => _log.Info(m);   // 侧边栏文件操作（复制/粘贴）日志

        _propsView.CollapseRequested += () => SetPropsCollapsed(true);

        _log.EntryAdded += e => Dispatcher.UIThread.Post(() => _logView.Append(e));

        Opened += (_, _) => Start();
        PointerMoved += Window_PointMoved;
        PointerReleased += Window_PointReleased;
        PositionChanged += (_, e) => TrackGoodGeo();
        SizeChanged += (_, e) => TrackGoodGeo();
        KeyDown += (_, e) =>
        {
            if (e.Key == Key.F5) _ = ReloadPluginsAsync();
            else if (_dragging && e.Key == Key.Escape) { EndDrag(); }   // Esc 取消拖拽
            else if (e.KeyModifiers == (KeyModifiers.Control | KeyModifiers.Alt))
            {
                // Ctrl+Alt+方向 = 朝该方向拆分当前激活标签（键盘/自动化兜底，鼠标用标签右键菜单）
                SplitSide? side = e.Key switch
                {
                    Key.Left => SplitSide.Left,
                    Key.Right => SplitSide.Right,
                    Key.Up => SplitSide.Top,
                    Key.Down => SplitSide.Bottom,
                    _ => (SplitSide?)null,
                };
                if (side is { } sd && _activeLeaf?.ActivePanelId is { } key && _tabs.TryGetValue(key, out var tab))
                {
                    e.Handled = true;
                    SplitTab(tab, sd);
                }
                else if (e.Key == Key.F && _activeLeaf?.ActivePanelId is { } fkey && _tabs.TryGetValue(fkey, out var ftab))
                {
                    // Ctrl+Alt+F = 浮动当前激活标签（键盘兜底，鼠标用标签右键菜单）
                    e.Handled = true;
                    FloatTab(ftab);
                }
            }
        };
    }

    private ContentControl _logHost;
    private ContentControl _propsHost;

    // ==================== 启动 ====================

    private void Start()
    {
        // 数据树等插件视图请求激活窗口面板（载荷=面板 Id）→ 打开/聚焦对应标签页
        _bus.Subscribe<string>(BusTopics.PanelActivate, id =>
        {
            var panel = _manager.UiPlugins.SelectMany(u => u.Panels).FirstOrDefault(p => p.Id == id);
            if (panel is not null) OpenPanel(panel);
        }, uiThread: true);
        ApplyHostConfig();
        try
        {
            _manager.LoadAll();
        }
        catch (Exception ex)
        {
            _log.Error($"插件加载过程异常: {ex.Message}");
        }
        ReportMissingRequired();
        EnsureWelcomeTab();
        BuildActivityBar();
        RefreshSidebars();
        RefreshCount();
        FillHostProps();
        AutoOpenPanels();
        RestoreSession();   // 会话恢复（分屏树/动态页/侧边栏）；无会话文件时不动
    }

    /// <summary>读取 config/host.json：插件目录 / 自动打开面板 / 官方插件（不可卸载）清单。</summary>
    private void ApplyHostConfig()
    {
        var cfg = HostConfig.Load(AppPaths.ConfigDir, m => _log.Warn(m));
        AppPaths.ComDllRoot = Path.IsPathRooted(cfg.PluginDirectory)
            ? cfg.PluginDirectory
            : Path.Combine(AppPaths.RepoRoot, cfg.PluginDirectory);
        _autoOpen = cfg.AutoOpenPlugins;
        _manager.RequiredPlugins = cfg.RequiredPlugins;
    }

    /// <summary>官方插件（不可卸载）缺失 → 显著报错（框架照常启动，其余插件不受影响）。</summary>
    private void ReportMissingRequired()
    {
        foreach (var id in _manager.MissingRequiredPlugins)
        {
            _log.Error($"官方插件缺失：{id}（不可卸载组件，产品基线不完整，请恢复 comdll 目录后按 F5 重载）");
            SetStatus($"⚠ 官方插件缺失：{id}");
        }
    }

    /// <summary>打开所有声明 AutoOpen 的 Document 面板（启动与 F5 重载后共用；欢迎页保持选中）。</summary>
    private void AutoOpenPanels()
    {
        if (!_autoOpen) return;
        foreach (var plugin in _manager.UiPlugins)
            foreach (var panel in plugin.Panels.Where(p => p.Role == PanelRole.Document && p.AutoOpen))
                OpenPanel(panel);
        SelectTab("welcome");
    }

    private void FillHostProps()
    {
        _propsView.SetHostRows(new List<(string Key, string Value)>
        {
            ("宿主版本", App.HostVersion),
            (".NET", Environment.Version.ToString()),
            ("操作系统", Environment.OSVersion.VersionString),
            ("插件目录", AppPaths.ComDllRoot),
        });
    }

    // ==================== 活动栏 ====================

    /// <summary>活动栏条目：视图型（切换侧边栏内容）或开关型（显隐属性/日志）。
    /// PanelId：视图型条目对应的插件面板 Id（Tool 面板从面板列表点击时用它找回条目）。</summary>
    private sealed record ActivityEntry(string Icon, string Title, Func<Control>? View, Action? Toggle, string? PanelId = null);

    private void BuildActivityBar()
    {
        _activityBar.Children.Clear();
        _activityButtons.Clear();
        _activityEntries.Clear();

        _activityEntries.Add(new ActivityEntry("🧩", "面板", () => _pluginList, null));
        _activityEntries.Add(new ActivityEntry("⚡", "算法配方", () => _algoList, null));

        // 插件注册的左侧工具面板 = 算子库等（同级侧边栏视图）。
        // 注意：Tool 面板不再按 DefaultDock 过滤——手写外壳没有停靠区，
        // 任何 Tool 面板都必须从活动栏可达，否则注册了也无处显示。
        foreach (var plugin in _manager.UiPlugins)
        {
            foreach (var panel in plugin.Panels)
            {
                if (panel.Role != PanelRole.Tool) continue;
                var pl = plugin;
                var pn = panel;
                _activityEntries.Add(new ActivityEntry(
                    string.IsNullOrWhiteSpace(pn.Icon) ? "▤" : pn.Icon!,
                    pn.Title,
                    () => GetPanelView(pl, pn),
                    null,
                    pn.Id));
            }
        }

        _activityEntries.Add(new ActivityEntry("⚙", "属性", null, () => SetPropsCollapsed(!_propsCollapsed)));
        _activityEntries.Add(new ActivityEntry("🧾", "日志", null, () => SetLogCollapsed(!_logCollapsed)));

        foreach (var entry in _activityEntries)
        {
            var btn = new Button
            {
                Classes = { "activity" },
                // emoji 图标用 Viewbox 归一化：Segoe UI Emoji 会把部分字形（如 🧰）
                // 渲染成远超 FontSize 的彩色位图，溢出按钮遮挡侧边栏 —— 统一缩放进 18×18
                Content = new Viewbox
                {
                    Width = 18,
                    Height = 18,
                    Child = new TextBlock { Text = entry.Icon, FontSize = 17 },
                },
                ClipToBounds = true,
            };
            ToolTip.SetTip(btn, entry.Title);
            var captured = entry;
            btn.Click += (_, _) => OnActivityClick(captured);
            _activityButtons[entry.Title] = btn;
            _activityBar.Children.Add(btn);
        }

        // 默认：显示面板侧边栏
        OnActivityClick(_activityEntries[0]);
    }

    private void OnActivityClick(ActivityEntry entry)
    {
        if (entry.View is null)
        {
            entry.Toggle?.Invoke();   // 开关型：属性/日志
            return;
        }

        // 视图型：再次点击已激活项 = 收起侧边栏列
        if (_activeSidebar is { } current && current.Title == entry.Title)
        {
            SetSidebarCollapsed(!_sidebarCollapsed);
            return;
        }
        _activeSidebar = entry;
        SidebarContent.Content = entry.View();
        SetSidebarCollapsed(false);
    }

    // ==================== 面板显隐（列 / 行折叠） ====================

    private bool _sidebarCollapsed;

    private void SetSidebarCollapsed(bool collapsed)
    {
        _sidebarCollapsed = collapsed;
        var cols = MainGrid.ColumnDefinitions;
        cols[1].Width = new GridLength(collapsed ? 0 : 235);
        SidebarContent.IsVisible = !collapsed;
        SidebarSplitter.IsVisible = !collapsed;
        UpdateActivityHighlight();
    }

    private void SetPropsCollapsed(bool collapsed)
    {
        _propsCollapsed = collapsed;
        var cols = MainGrid.ColumnDefinitions;
        cols[5].Width = new GridLength(collapsed ? 0 : 255);
        PropsHost.IsVisible = !collapsed;
        PropsSplitter.IsVisible = !collapsed;
        UpdateActivityHighlight();
    }

    private void SetLogCollapsed(bool collapsed)
    {
        _logCollapsed = collapsed;
        var rows = CenterGrid.RowDefinitions;
        rows[1].Height = new GridLength(collapsed ? 0 : 4);
        rows[2].Height = new GridLength(collapsed ? 0 : 200);
        LogHost.IsVisible = !collapsed;
        LogSplitter.IsVisible = !collapsed;
        UpdateActivityHighlight();
    }

    /// <summary>活动栏图标高亮同步：视图型 = 侧边栏展开且激活；开关型 = 面板可见。</summary>
    private void UpdateActivityHighlight()
    {
        foreach (var entry in _activityEntries)
        {
            if (!_activityButtons.TryGetValue(entry.Title, out var btn)) continue;
            bool active = entry.View is not null
                ? !_sidebarCollapsed && ReferenceEquals(_activeSidebar, entry)
                : entry.Title switch
                {
                    "属性" => !_propsCollapsed,
                    "日志" => !_logCollapsed,
                    _ => false,
                };
            // 深色活动栏上 Foreground 置 null 会落到近黑默认前景（图标不可见），必须显式给色
            btn.Background = active ? new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)) : null;
            btn.Foreground = active ? Brushes.White : new SolidColorBrush(Color.FromRgb(0x86, 0x86, 0x90));
        }
    }

    // ==================== 面板 / 标签页 ====================

    private Control GetPanelView(UiPluginRecord plugin, PanelDescriptor panel)
    {
        if (_panelViews.TryGetValue(panel.Id, out var cached)) return cached;
        Control view;
        try
        {
            view = panel.CreateView();
        }
        catch (Exception ex)
        {
            _log.Error($"创建面板失败 {panel.Title}: {ex.Message}");
            view = new TextBlock
            {
                Text = $"面板创建失败：{ex.Message}",
                Foreground = new SolidColorBrush(Color.FromRgb(0xF1, 0x4C, 0x4C)),
                Margin = new Thickness(20),
            };
        }
        _panelViews[panel.Id] = view;
        (plugin.PluginContext as HostPluginContext)?.RaisePanelShown(panel.Id);
        return view;
    }

    /// <summary>面板列表点击入口：Tool 面板 → 切换侧边栏视图（与活动栏图标同语义）；
    /// Document 面板 → 打开标签页。同一个视图实例只能挂一处，混开会导致 visual parent 冲突。</summary>
    private void ShowPanel(PanelDescriptor panel)
    {
        if (panel.Role == PanelRole.Tool)
        {
            var entry = _activityEntries.FirstOrDefault(a => a.PanelId == panel.Id);
            if (entry is not null)
            {
                OnActivityClick(entry);
                return;
            }
        }
        OpenPanel(panel);
    }

    private void UpdatePropsFor(PanelDescriptor panel, UiPluginRecord plugin)
    {
        _propsView.SetPanelRows(new List<(string, string)>
        {
            ("面板 Id", panel.Id),
            ("标题", panel.Title),
            ("角色", panel.Role.ToString()),
            ("所属插件", $"{plugin.Name} <{plugin.Meta.Id}>"),
            ("插件版本", plugin.Version),
            ("加载时间", plugin.LoadTime.ToString("HH:mm:ss")),
        });
        // 面板实现了 IPanelParamsProvider → 属性面板显示其可编辑参数区
        _propsView.SetParams(_panelViews.TryGetValue(panel.Id, out var view)
            ? (view as IPanelParamsProvider)?.CreateParamsContent()
            : null);
    }

    private async Task RunCommandAsync(AlgoRecord algo, AlgoCommand cmd)
    {
        if (_runCts is not null)
        {
            SetStatus("已有算法在运行，请等待或取消。");
            return;
        }

        var values = new Dictionary<string, object?>();
        if (cmd.Params is { Count: > 0 })
        {
            var confirmed = await Views.ParamDialog.ShowAsync(this, cmd.Title, algo.Description, cmd.Params, values, m => _log.Error(m));
            if (!confirmed) return;
        }

        var ctx = new HostAlgoRunContext(cmd.Title, values, _log, s => SetStatus(s));
        _runCts = ctx.CancelSource;
        _busyText.Text = $"▶ {cmd.Title} 运行中…";
        _busyText.IsVisible = true;
        _cancelRunBtn.IsVisible = true;
        _log.Info($"开始执行: {cmd.Title}");

        try
        {
            await Task.Run(() => cmd.Execute(ctx), ctx.Cancel);
            _log.Info($"执行完成: {cmd.Title}");
            SetStatus($"{cmd.Title} 完成");
        }
        catch (OperationCanceledException)
        {
            _log.Warn($"已取消: {cmd.Title}");
            SetStatus($"{cmd.Title} 已取消");
        }
        catch (Exception ex)
        {
            _log.Error($"执行失败 {cmd.Title}: {ex}");
            SetStatus($"{cmd.Title} 失败");
        }
        finally
        {
            _runCts = null;
            _busyText.IsVisible = false;
            _cancelRunBtn.IsVisible = false;
        }
    }

    // ==================== 刷新 ====================

    private void RefreshSidebars()
    {
        _pluginList.Refresh(_manager.UiPlugins);
        _algoList.Refresh(_manager.AlgoPlugins);
    }

    private void RefreshCount()
        => _pluginCountText.Text = $"UI 插件 {_manager.UiPlugins.Count} · 算法 {_manager.AlgoPlugins.Count}";

    private void SetStatus(string status)
    {
        if (Dispatcher.UIThread.CheckAccess())
            StatusText.Text = status;
        else
            Dispatcher.UIThread.Post(() => StatusText.Text = status);
    }

    /// <summary>声明式插件关联联动：激活文档面板时自动切到声明关联它的工具面板；
    /// 当前侧边栏工具面板声明了关联但不含激活面板时切回面板列表（未声明关联的面板不受影响）。</summary>
    private void SwitchSidebarForAssociation(string activePanelId)
    {
        PanelDescriptor? Desc(string? pid) =>
            pid is null ? null : _manager.UiPlugins.SelectMany(u => u.Panels).FirstOrDefault(p => p.Id == pid);

        var current = Desc(_activeSidebar?.PanelId);
        if (current is { Role: PanelRole.Tool } && current.AssociatedPanels is { } curAssoc &&
            !curAssoc.Contains(activePanelId))
        {
            OnActivityClick(_activityEntries[0]);
            return;
        }
        var assocEntry = _activityEntries.FirstOrDefault(en =>
        {
            var pd = Desc(en.PanelId);
            return pd is { Role: PanelRole.Tool } && pd.AssociatedPanels?.Contains(activePanelId) == true;
        });
        if (assocEntry is not null && _activeSidebar?.PanelId != assocEntry.PanelId)
            OnActivityClick(assocEntry);
    }

    // ==================== 菜单 ====================

    private void TogglePanelMenu_Click(object? sender, RoutedEventArgs e) => SetSidebarCollapsed(!_sidebarCollapsed);
    private void TogglePropsMenu_Click(object? sender, RoutedEventArgs e) => SetPropsCollapsed(!_propsCollapsed);
    private void ToggleLogMenu_Click(object? sender, RoutedEventArgs e) => SetLogCollapsed(!_logCollapsed);

    private async void ReloadPlugins_Click(object? sender, RoutedEventArgs e) => await ReloadPluginsAsync();

    private async Task ReloadPluginsAsync()
    {
        SetStatus("正在重新加载插件…");
        CloseAllTabs();
        _panelViews.Clear();
        ApplyHostConfig();
        await Task.Run(_manager.ReloadAll);
        BuildActivityBar();
        RefreshSidebars();
        RefreshCount();
        AutoOpenPanels();
        ReportMissingRequired();
        SetStatus("插件重载完成");
    }

    private void OpenAll_Click(object? sender, RoutedEventArgs e)
    {
        foreach (var plugin in _manager.UiPlugins)
            foreach (var panel in plugin.Panels.Where(p => p.Role == PanelRole.Document))
                OpenPanel(panel);
    }

    private void CloseAllTabs_Click(object? sender, RoutedEventArgs e) => CloseAllTabs();

    /// <summary>视图菜单「欢迎页」：欢迎页可关闭，此处随时找回（没有则重建并聚焦）。</summary>
    private void Welcome_Click(object? sender, RoutedEventArgs e)
    {
        EnsureWelcomeTab();
        SelectTab("welcome");
    }

    private void OpenPluginDir_Click(object? sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(AppPaths.ComDllRoot);
            if (OperatingSystem.IsWindows())
                Process.Start(new ProcessStartInfo("explorer.exe", $"\"{AppPaths.ComDllRoot}\""));
            else if (OperatingSystem.IsLinux())
                Process.Start(new ProcessStartInfo("xdg-open", AppPaths.ComDllRoot) { UseShellExecute = true });
        }
        catch (Exception ex)
        {
            _log.Error($"打开插件目录失败: {ex.Message}");
        }
    }

    private void CancelRun_Click(object? sender, RoutedEventArgs e)
        => _runCts?.Cancel();

    private void Exit_Click(object? sender, RoutedEventArgs e) => Close();

    private void About_Click(object? sender, RoutedEventArgs e)
    {
        var text = new TextBlock
        {
            Text = $"ComUI 显示平台 v{App.HostVersion}\n\n" +
                   $"运行时: .NET {Environment.Version}\n" +
                   $"操作系统: {Environment.OSVersion.VersionString}\n" +
                   $"插件根目录: {AppPaths.ComDllRoot}\n\n" +
                   "UI 插件: comdll/ui/<插件>/ （实现 IPlugin，多面板注册）\n" +
                   "算法插件: comdll/algo/<插件>/ （算法 DLL + *.algo.json）\n" +
                   "共享契约: comdll/common/\n\n" +
                   "基于 Avalonia，支持 Windows / Linux。",
            TextWrapping = TextWrapping.Wrap,
            FontSize = 13,
        };

        var ok = new Button { Content = "确定", HorizontalAlignment = HorizontalAlignment.Right, MinWidth = 80 };
        var layout = new StackPanel { Margin = new Thickness(20), Spacing = 16 };
        layout.Children.Add(text);
        layout.Children.Add(ok);

        var about = new Window
        {
            Title = "关于 ComUI",
            Width = 520,
            Height = 360,
            CanResize = false,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = layout,
        };
        ok.Click += (_, _) => about.Close();
        about.ShowDialog(this);
    }
}

/// <summary>标签页 Tag：面板 Id + 标题。</summary>
public sealed record PanelTag(string PanelId, string Title);
