using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using ComUI.Core;
using ComUI.Core.Split;

namespace ComUI.Host.Shell;

/// <summary>
/// 主窗口的会话持久化（partial，config/session.json）：
/// 窗口几何、分屏树（doc 键 ↔ 恢复键翻译）、动态页清单、侧边栏；退出保存、启动恢复、视图菜单「默认布局」。
/// </summary>
public partial class MainWindow
{
    private (int X, int Y, int W, int H, bool Maximized) _lastGoodGeo = (0, 0, 1600, 900, false);
    private readonly Dictionary<string, Func<string, Control>> _docFactories = new();   // 文档类型 → 恢复工厂
    private readonly Dictionary<string, string> _docRestoreKeys = new();   // doc:N → 恢复键

    private string SessionPath => Path.Combine(AppPaths.ConfigDir, "session.json");

    /// <summary>恢复会话里的窗口几何（构造期调用，避免显示后跳变）。</summary>
    private void RestoreWindowGeometry()
    {
        try
        {
            if (!File.Exists(SessionPath)) return;
            var data = System.Text.Json.JsonSerializer.Deserialize<SessionData>(File.ReadAllText(SessionPath), SessionOpts);
            if (data?.Window is not { } w || w.W <= 0 || w.H <= 0) return;
            // 垃圾几何钳制（旧版本曾把最小化态 -32000 存进会话）
            int x = w.X is < -100 or > 5000 ? 0 : w.X;
            int y = w.Y is < -100 or > 5000 ? 0 : w.Y;

            Width = Math.Clamp(w.W, 400, 3840);
            Height = Math.Clamp(w.H, 300, 2160);
            WindowStartupLocation = WindowStartupLocation.Manual;
            Position = new PixelPoint(x, y);
            if (w.Maximized) WindowState = WindowState.Maximized;
        }
        catch { }
    }

    /// <summary>记录最近一次非最小化的窗口几何（最小化时 Position=-32000，不能存）。</summary>
    private void TrackGoodGeo()
    {
        if (WindowState == WindowState.Minimized) return;
        _lastGoodGeo = ((int)Position.X, (int)Position.Y, (int)Bounds.Width, (int)Bounds.Height, WindowState == WindowState.Maximized);
    }

    /// <summary>退出时保存会话：窗口几何 / 分屏树（doc 键翻译为恢复键）/ 动态页清单 / 侧边栏。
    /// 不变量「分屏树只存可恢复的键」：无恢复键的动态页（快照等，重启无法重建）先从模型剔除，
    /// 否则幽灵键会把空格子一直钉在布局里（症状：关闭窗口后布局保持原状、格子里什么都没有）。</summary>
    private void SaveSession()
    {
        try
        {
            foreach (var leaf in _layout.Leaves().ToList())
                foreach (var pid in leaf.PanelIds.ToList())
                    if (pid.StartsWith("doc:") && !_docRestoreKeys.ContainsKey(pid))
                        _layout.RemovePanel(leaf, pid);   // 走回收链：空格回收/容器退化/激活面板修正
            var splitNode = System.Text.Json.Nodes.JsonNode.Parse(_layout.ToJson());
            TranslateDocKeys(splitNode, _docRestoreKeys);   // doc:N → 恢复键（无键的未命名页恢复时自动丢弃）
            // 最小化时 Position 是 (-32000,-32000)——不存垃圾几何，回退上次有效值
            int px = Position.X;
            int py = Position.Y;
            if (WindowState == WindowState.Minimized || px < -100 || py < -100)
            {
                px = _lastGoodGeo.X;
                py = _lastGoodGeo.Y;
            }
            var data = new SessionData
            {
                Window = new SessionWindow
                {
                    X = px, Y = py,
                    W = (int)(_lastGoodGeo.W > 0 ? _lastGoodGeo.W : Width),
                    H = (int)(_lastGoodGeo.H > 0 ? _lastGoodGeo.H : Height),
                    Maximized = WindowState == WindowState.Maximized || _lastGoodGeo.Maximized,
                },
                Sidebar = _activeSidebar?.Title,
                Split = splitNode?.ToJsonString(),
                Docs = _tabs.Where(kv => TabKey(kv.Value).StartsWith("doc:"))
                            .Where(kv => _docRestoreKeys.ContainsKey(kv.Key))
                            .Select(kv => new SessionDoc
                            {
                                Key = _docRestoreKeys[kv.Key],
                                Title = _tabTitles.TryGetValue(kv.Key, out var t) ? t : "",
                            })
                            .ToList(),
                // 浮动窗：退出时还浮着的面板（含 doc 页与窗口面板）——几何一并存，
                // 重启后原位浮出（Closing 先存后关，此刻登记表还完整）。
                // doc:N 键与分屏树同样翻译成恢复键（doc:N 跨重启不稳定）
                Floating = _floating.Where(kv => kv.Key != "welcome")
                                    .Select(kv => new SessionFloat
                                    {
                                        Key = _docRestoreKeys.TryGetValue(kv.Key, out var rk) ? rk : kv.Key,
                                        X = kv.Value.Position.X,
                                        Y = kv.Value.Position.Y,
                                        W = (int)kv.Value.Width,
                                        H = (int)kv.Value.Height,
                                    })
                                    .ToList(),
            };
            Directory.CreateDirectory(AppPaths.ConfigDir);
            File.WriteAllText(SessionPath, System.Text.Json.JsonSerializer.Serialize(data, SessionOpts));
            _log.Info("会话已保存");
        }
        catch (Exception ex)
        {
            _log.Warn($"会话保存失败: {ex.Message}");
        }
    }

    /// <summary>启动恢复会话：动态页（工厂重建）→ 分屏树（恢复键翻译回 doc:N）→ 面板标签 → 侧边栏。
    /// 任何失败回退默认布局（AutoOpenPanels 已在 Start 里跑过，重复打开会去重聚焦）。</summary>
    private void RestoreSession()
    {
        if (!File.Exists(SessionPath)) return;
        try
        {
            var data = System.Text.Json.JsonSerializer.Deserialize<SessionData>(File.ReadAllText(SessionPath), SessionOpts);
            if (data is null) return;

            // 1) 动态页：按恢复键重建内容与标签（不进模型——模型在分屏 JSON 里）
            var docKeyMap = new Dictionary<string, string>();   // 恢复键 → 新 doc:N
            foreach (var d in data.Docs ?? new List<SessionDoc>())
            {
                var colon = d.Key.IndexOf(':');
                if (colon <= 0 || !_docFactories.TryGetValue(d.Key[..colon], out var factory)) continue;
                var key = $"doc:{++_docSeq}";
                var content = factory(d.Key[(colon + 1)..]);
                var tab = BuildTabItem(key, d.Title, content);
                _tabs[key] = tab;
                _tabTitles[key] = d.Title;
                _docRestoreKeys[key] = d.Key;
                docKeyMap[d.Key] = key;
            }

            // 2) 分屏树：恢复键 → doc:N 翻译后载入
            if (data.Split is not null)
            {
                var node = System.Text.Json.Nodes.JsonNode.Parse(data.Split);
                TranslateDocKeys(node, docKeyMap);
                _layout = new SplitLayout();
                _layout.LoadFromJson(node?.ToJsonString() ?? "{}");
                // 3) 树里的面板 Id 建标签页；建不出来的（插件缺失 / 无恢复键的动态页=快照等幽灵键）
                //    一律经 RemovePanel 剔除——回收空格、退化单子容器、修正激活面板，
                //    旧版本直接戳 PanelIds 或跳过，会留下"关不掉的空白格"（恢复端同样强制不变量）
                foreach (var leaf in _layout.Leaves().ToList())
                    foreach (var pid in leaf.PanelIds.ToList())
                    {
                        if (pid == "welcome") { EnsureWelcomeTab(); continue; }
                        bool built = pid.Contains(':')
                            ? _tabs.ContainsKey(pid)                       // doc 键：步骤 1 按恢复键重建过才存在
                            : CreatePanelTabUi(pid) is not null;           // 面板键：插件在场才可建
                        if (!built) _layout.RemovePanel(leaf, pid);
                    }
            }
            RebuildSplitView();
            // 上会话关闭了欢迎页（树里没有）→ 尊重关闭态：摘掉 Start 时预建的欢迎标签，
            // 否则它游离在 _tabs 里，「视图→欢迎页」会因已存在而无法重建
            if (_layout.FindLeaf("welcome") is null) _tabs.Remove("welcome");
            // 激活叶重置到恢复树（旧树的引用已失效，否则总线事件开面板会挂到游离叶）
            _activeLeaf = _layout.FindLeaf(_layout.Leaves().SelectMany(l => l.PanelIds).LastOrDefault(_tabs.ContainsKey))
                         ?? _layout.Leaves().FirstOrDefault();

            // 4) 浮动窗：会话登记的浮动面板原位浮出（doc 页已在步骤 1 重建；面板键此处建页）
            foreach (var f in data.Floating ?? new List<SessionFloat>())
            {
                try
                {
                    var fkey = docKeyMap.TryGetValue(f.Key, out var dk) ? dk : f.Key;   // 恢复键 → doc:N（面板键原样）
                    if (fkey == "welcome" || f.W <= 0 || f.H <= 0) continue;
                    if (_layout.FindLeaf(fkey) is not null) continue;   // 既在树又登记浮动=脏数据，以树为准
                    var tab = _tabs.TryGetValue(fkey, out var ft) ? ft : CreatePanelTabUi(fkey);
                    if (tab is null) continue;   // 插件缺失：丢弃该浮窗
                    // 垃圾几何钳制（与主窗同规则；Owner 最小化时 Position 可能为 -32000）
                    bool badGeo = f.X is < -100 or > 5000 || f.Y is < -100 or > 5000;
                    FloatTab(tab, badGeo ? null : new PixelPoint(f.X, f.Y), f.W, f.H);
                }
                catch (Exception ex)
                {
                    _log.Warn($"浮动窗恢复失败 {f.Key}: {ex.Message}");
                }
            }

            // 5) 侧边栏（与当前已激活项相同则不动——再激活会被当作"再次点击"收起）
            if (data.Sidebar is { } sb && _activeSidebar?.Title != sb)
            {
                sb = sb == "算法" ? "算法配方" : sb;   // 旧会话兼容：算法侧边栏已改名
                var entry = _activityEntries.FirstOrDefault(en => en.Title == sb);
                if (entry is not null) OnActivityClick(entry);
            }
            _log.Info("会话已恢复（含分屏布局）");
        }
        catch (Exception ex)
        {
            _log.Warn($"会话恢复失败（使用默认布局）: {ex.Message}");
        }
    }

    /// <summary>分屏 JSON 里 doc 键翻译（保存：doc:N→恢复键；恢复：恢复键→doc:N）。</summary>
    private static void TranslateDocKeys(System.Text.Json.Nodes.JsonNode? node, Dictionary<string, string> map)
    {
        if (node is not System.Text.Json.Nodes.JsonObject obj) return;
        if (obj["Panels"] is System.Text.Json.Nodes.JsonArray panels)
            for (int i = 0; i < panels.Count; i++)
                if (panels[i] is System.Text.Json.Nodes.JsonValue v && v.TryGetValue<string>(out var id) && map.TryGetValue(id, out var nk))
                    panels[i] = System.Text.Json.Nodes.JsonValue.Create(nk);
        if (obj["Children"] is System.Text.Json.Nodes.JsonArray children)
            foreach (var c in children) TranslateDocKeys(c, map);
    }

    /// <summary>只为面板建标签页（不动模型——会话恢复用；插件缺失返回 null）。</summary>
    private TabItem? CreatePanelTabUi(string panelId)
    {
        if (_tabs.ContainsKey(panelId)) return _tabs[panelId];
        var plugin = _manager.UiPlugins.FirstOrDefault(u => u.Panels.Any(p => p.Id == panelId));
        var panel = plugin?.Panels.FirstOrDefault(p => p.Id == panelId);
        if (plugin is null || panel is null) return null;
        var view = GetPanelView(plugin, panel);
        var tab = BuildTabItem(panel.Id, panel.Title, view);
        _tabs[panelId] = tab;
        return tab;
    }

    /// <summary>视图菜单「默认布局」：清会话文件 → 关全部标签 → 新树 + 欢迎页 + AutoOpen 面板。</summary>
    private void ResetLayout_Click(object? sender, RoutedEventArgs e)
    {
        try { File.Delete(SessionPath); } catch { }
        foreach (var key in _tabs.Keys.Where(k => k != "welcome").ToList())
            CloseTab(_tabs[key]);
        _tabs.Remove("welcome");
        _docRestoreKeys.Clear();
        _tabTitles.Clear();
        _layout = new SplitLayout();
        EnsureWelcomeTab();
        AutoOpenPanels();
        SelectTab("welcome");
        _log.Info("已恢复默认布局");
    }

    private static readonly System.Text.Json.JsonSerializerOptions SessionOpts = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
    };

    private sealed class SessionData
    {
        public SessionWindow? Window { get; set; }
        public string? Sidebar { get; set; }
        public string? Split { get; set; }
        public List<SessionDoc>? Docs { get; set; }
        public List<SessionFloat>? Floating { get; set; }
    }

    private sealed class SessionWindow
    {
        public int X { get; set; }
        public int Y { get; set; }
        public int W { get; set; }
        public int H { get; set; }
        public bool Maximized { get; set; }
    }

    private sealed class SessionDoc
    {
        public string Key { get; set; } = "";
        public string Title { get; set; } = "";
    }

    private sealed class SessionFloat
    {
        public string Key { get; set; } = "";
        public int X { get; set; }
        public int Y { get; set; }
        public int W { get; set; }
        public int H { get; set; }
    }
}
