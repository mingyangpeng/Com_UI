using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Host.Shell;

/// <summary>
/// 主窗口的浮动窗管理（partial）：
/// 标签浮动为独立窗口（Owner=主窗，随主窗最小化/关闭）、固定回主窗、关闭清理登记。
/// 入口：标签右键菜单「浮动窗口」/ Ctrl+Alt+F / 拖出主窗边界松手。
/// </summary>
public partial class MainWindow
{
    private readonly Dictionary<string, Window> _floating = new();   // key → 浮动窗口
    private bool _redocking;                                         // 浮动窗固定回主窗期间抑制 Closed 清理

    /// <summary>把标签浮动为独立窗口（Owner=主窗；可「⇲ 固定」回主窗或拖回）。
    /// 标签在分屏树里 → 先从模型/格子摘除再浮出；不在树里（会话恢复的浮动页）→ 直接建窗。</summary>
    private void FloatTab(TabItem tab, PixelPoint? position = null, int? width = null, int? height = null)
    {
        var key = TabKey(tab);
        if (_floating.ContainsKey(key)) return;
        if (tab.Content is not Control content) return;

        var leaf = _layout.FindLeaf(key);
        if (leaf is not null)
        {
            _layout.RemovePanel(leaf, key);   // 模型回收（浮动面板不占格子）
            TabControlOf(tab)?.Items.Remove(tab);
            RebuildSplitView();
        }

        var title = _tabTitles.TryGetValue(key, out var t) ? t : key;
        var dockBtn = new Button { Content = "⇲ 固定到主窗", Classes = { "ghost" } };
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(8, 6) };
        bar.Children.Add(dockBtn);
        var top = new DockPanel { LastChildFill = true };
        DockPanel.SetDock(bar, Dock.Top);
        top.Children.Add(bar);
        top.Children.Add(content);

        var win = new Window
        {
            Title = title + " — ComUI",
            Content = new Border { Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20)), Child = top },
            Width = Math.Clamp(width ?? 940, 240, 3840),
            Height = Math.Clamp(height ?? 640, 180, 2160),
            WindowStartupLocation = WindowStartupLocation.Manual,
            Position = position ?? new PixelPoint((int)(Position.X + Bounds.Width / 2 - 470), (int)(Position.Y + 140)),
        };
        _floating[key] = win;
        win.Closed += (_, _) => OnFloatClosed(key);
        dockBtn.Click += (_, _) => DockBack(key);
        win.Show(this);   // Owner=主窗：随主窗最小化/关闭（Owner 属性是 protected，Show(owner) 是公开入口）
        _log.Info($"面板已浮动: {title}");
    }

    /// <summary>浮动窗口关闭（用户✕或主窗退出）：清理登记；固定回主窗时由 _redocking 抑制。</summary>
    private void OnFloatClosed(string key)
    {
        _floating.Remove(key);
        if (_redocking) return;
        _tabs.Remove(key);
        _docRestoreKeys.Remove(key);
        _tabTitles.Remove(key);
        _log.Info($"浮动窗口已关闭: {key}");
    }

    /// <summary>浮动窗固定回主窗：拆出视图 → 关窗（抑制清理）→ 回到最近聚焦组。</summary>
    private void DockBack(string key)
    {
        if (!_floating.Remove(key, out var win)) return;
        _redocking = true;
        var content = win.Content;
        win.Close();

        // 拆出原视图（浮动窗包裹：Border→DockPanel→[工具条, 视图]）
        Control view = (Control)content!;
        if (view is Border b && b.Child is DockPanel dp && dp.Children.Count > 1)
        {
            var inner = dp.Children[^1];
            dp.Children.Remove(inner);
            view = inner;
        }
        var tab = BuildTabItem(key, _tabTitles.TryGetValue(key, out var t) ? t : key, view);
        _tabs[key] = tab;
        var leaf = _activeLeaf ?? _layout.Leaves().First();
        leaf.PanelIds.Add(key);
        leaf.ActivePanelId = key;
        RebuildSplitView();
        SelectTab(tab);
    }
}
