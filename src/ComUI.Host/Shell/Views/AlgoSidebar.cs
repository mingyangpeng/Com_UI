using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using ComUI.Core;
using ComUI.Sdk;
using ComUI.Sdk.Ui;

namespace ComUI.Host.Views;

/// <summary>
/// 「算法配方」侧边栏（活动栏 ⚡）：流水线算法配方（单击发布 pipeline/open）。
/// </summary>
public sealed class AlgoSidebar : UserControl
{
    private readonly IBus _bus;
    private readonly StackPanel _root = new() { Margin = new Thickness(8, 10), Spacing = 2 };
    private readonly StackPanel _pipelineSection = new() { Margin = new Thickness(0, 6, 0, 0) };

    /// <summary>宿主日志回调（复制/粘贴等操作反馈）。</summary>
    public Action<string>? LogMessage { get; set; }

    private static readonly IBrush SelectedBrush = new SolidColorBrush(Color.FromRgb(0x2F, 0x33, 0x3B));

    private readonly List<Button> _pipelineRows = new();
    private double _lastViewportW = 219;   // 侧边栏内容宽（视口宽 - _root 横向 Margin 16）

    private string? _copiedPath;        // Ctrl+C 复制的算法文件路径
    private string? _lastClickedPath;   // 最近单击选中的算法行（复制/粘贴的操作对象）
    private TextBox? _renameBox;        // 行内改名进行中（期间放行 TextBox 自身的 Ctrl+C 文本复制）

    private readonly HashSet<string> _selectedPaths = new();      // 多选集合（全选/批量删除）
    private readonly Dictionary<string, Button> _rowByPath = new(); // 路径→行按钮（高亮刷新用）
    private bool _ctrlDown;   // Ctrl 按住状态（Ctrl+单击 = 多选切换）

    public AlgoSidebar(IBus bus)
    {
        _bus = bus;
        Background = new SolidColorBrush(Color.FromRgb(0x23, 0x23, 0x26));
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            // Disabled = 内容宽度约束到视口宽度：行按钮才能横向拉伸满宽（否则无限宽约束下全部贴合文字，长短不一）
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            Content = _root,
        };
        // 保存算法后即时刷新
        _bus.Subscribe<string>(BusTopics.AlgoSaved, _ => RefreshPipelines(), uiThread: true);

        // 行等宽：跟随视口宽度（布局链不拉伸子项，显式设宽保证所有行等宽、锁对齐一列）
        if (Content is ScrollViewer sv)
            sv.LayoutUpdated += (_, _) =>
            {
                var v = sv.Viewport.Width;
                if (Math.Abs(v - _lastViewportW) < 0.5) return;   // 无变化跳过，避免每次布局都重设
                _lastViewportW = Math.Max(60, v - 16);
                foreach (var r in _pipelineRows) r.Width = _lastViewportW;
            };

        // 文件基础操作：Ctrl+C 复制选中的算法，Ctrl+V 粘贴为解锁副本（改名进行中时放行 TextBox 文本操作）
        KeyDown += (_, e) =>
        {
            if (e.Key is Key.LeftCtrl or Key.RightCtrl) _ctrlDown = true;
            if (_renameBox is not null) return;
            if (e.KeyModifiers.HasFlag(KeyModifiers.Control) && e.Key == Key.C)
            {
                e.Handled = true;
                if (_lastClickedPath is null || !File.Exists(_lastClickedPath)) { LogMessage?.Invoke("先单击选中一个算法再 Ctrl+C 复制"); return; }
                _copiedPath = _lastClickedPath;
                LogMessage?.Invoke($"已复制「{NameOf(_copiedPath)}」，Ctrl+V 粘贴为副本");
            }
            else if (e.KeyModifiers.HasFlag(KeyModifiers.Control) && e.Key == Key.V)
            {
                e.Handled = true;
                PasteAsCopy();
            }
            else if (e.Key is Key.Delete or Key.Back)
            {
                e.Handled = true;
                _ = DeleteSelectedAsync();
            }
        };
        KeyUp += (_, e) =>
        {
            if (e.Key is Key.LeftCtrl or Key.RightCtrl) _ctrlDown = false;
        };
    }

    private static string NameOf(string path) =>
        Path.GetFileNameWithoutExtension(path).Replace(".pipeline", "");

    /// <summary>Del/Backspace：删除选中的算法文件（锁定状态拒绝——先解锁或复制副本）。删除前弹窗二次确认。</summary>
    private async Task DeleteSelectedAsync()
    {
        if (_lastClickedPath is null || !File.Exists(_lastClickedPath))
        {
            LogMessage?.Invoke("先单击选中一个算法再 Del 删除");
            return;
        }
        if (FileLock.IsLocked(_lastClickedPath))
        {
            LogMessage?.Invoke($"「{NameOf(_lastClickedPath)}」已锁定，解锁后才能删除（或复制副本后删副本）");
            return;
        }
        var name = NameOf(_lastClickedPath);
        var owner = TopLevel.GetTopLevel(this) as Window;
        bool ok = await ConfirmDialog.ShowAsync(owner, "确认删除算法",
            $"确定要删除算法「{name}」吗？\n\n此操作不可恢复（文件将从磁盘永久删除）。", "删除", danger: true);
        if (!ok) { LogMessage?.Invoke($"已取消删除「{name}」"); return; }
        try
        {
            File.Delete(_lastClickedPath);
            _lastClickedPath = null;
            _copiedPath = null;
            RefreshPipelines();
            LogMessage?.Invoke($"已删除算法「{name}」");
        }
        catch (Exception ex)
        {
            LogMessage?.Invoke($"删除失败: {ex.Message}");
        }
    }

    /// <summary>批量删除：先筛出可删项，弹窗列出名称二次确认后再删（锁定项跳过）。</summary>
    private async Task DeleteManyAsync()
    {
        if (_selectedPaths.Count == 0)
        {
            LogMessage?.Invoke("先选择算法再删除：单击选中、Ctrl+单击多选，或点 ☑ 全选");
            return;
        }
        var deletable = _selectedPaths.Where(p => File.Exists(p) && !FileLock.IsLocked(p)).ToList();
        int skipped = _selectedPaths.Count(p => FileLock.IsLocked(p));
        if (deletable.Count == 0)
        {
            LogMessage?.Invoke(skipped > 0
                ? $"选中的 {skipped} 个算法已全部锁定，无法删除（先解锁）"
                : "选中的算法文件已不存在");
            return;
        }
        var names = deletable.Select(NameOf).ToList();
        var nameText = names.Count <= 6
            ? string.Join("、", names)
            : string.Join("、", names.Take(6)) + $" 等 {names.Count} 个";
        var owner = TopLevel.GetTopLevel(this) as Window;
        bool ok = await ConfirmDialog.ShowAsync(owner, "确认批量删除算法",
            $"确定要删除 {names.Count} 个算法吗？\n\n{nameText}\n\n此操作不可恢复（文件将从磁盘永久删除）。"
            + (skipped > 0 ? $"\n\n另有 {skipped} 个已锁定将被跳过。" : ""),
            $"删除 {names.Count} 个", danger: true);
        if (!ok) { LogMessage?.Invoke("已取消批量删除"); return; }
        int deleted = 0;
        foreach (var p in deletable)
        {
            try { File.Delete(p); deleted++; }
            catch (Exception ex) { LogMessage?.Invoke($"删除「{NameOf(p)}」失败: {ex.Message}"); }
        }
        _selectedPaths.Clear();
        _lastClickedPath = null;
        _copiedPath = null;
        RefreshPipelines();
        LogMessage?.Invoke(skipped > 0
            ? $"已删除 {deleted} 个算法；{skipped} 个已锁定被跳过（解锁后再删）"
            : $"已删除 {deleted} 个算法");
    }

    /// <summary>Ctrl+V：把复制的算法文件粘贴为「原名 副本」（解锁可编辑，重名自动递增）。</summary>
    private void PasteAsCopy()
    {
        if (_copiedPath is null || !File.Exists(_copiedPath))
        {
            LogMessage?.Invoke("剪贴板没有算法（先单击选中一个算法再 Ctrl+C）");
            return;
        }
        try
        {
            var dir = Path.GetDirectoryName(_copiedPath)!;
            var baseName = NameOf(_copiedPath);
            var name = baseName + " 副本";
            for (int k = 2; File.Exists(Path.Combine(dir, name + ".pipeline.json")); k++)
                name = baseName + " 副本" + k;
            var newPath = Path.Combine(dir, name + ".pipeline.json");
            File.Copy(_copiedPath, newPath);
            try
            {
                var node = System.Text.Json.Nodes.JsonNode.Parse(File.ReadAllText(newPath));
                if (node is not null)
                {
                    node["Locked"] = false;   // 副本 = 可编辑草稿
                    File.WriteAllText(newPath, node.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
                }
            }
            catch { }
            _bus.Publish(BusTopics.AlgoSaved, newPath);
            LogMessage?.Invoke($"已粘贴为副本「{name}」（解锁可编辑）");
        }
        catch (Exception ex)
        {
            LogMessage?.Invoke($"粘贴失败: {ex.Message}");
        }
    }

    /// <summary>扫描 config/algorithms/ 刷新流水线算法列表。</summary>
    public void RefreshPipelines()
    {
        _pipelineSection.Children.Clear();
        _pipelineRows.Clear();
        _rowByPath.Clear();
        var dir = Path.Combine(AppPaths.RepoRoot, "config", "algorithms");
        var files = Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, "*.pipeline.json").ToList()
            : new List<string>();
        _selectedPaths.RemoveWhere(p => !files.Contains(p));   // 清掉已不存在的选择
        _pipelineSection.Children.Add(BuildPipelineHeader(files));
        if (files.Count == 0)
        {
            _pipelineSection.Children.Add(new TextBlock
            {
                Text = "（在流水线页面点「保存算法」）",
                FontSize = 11, Foreground = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7F)),
                Margin = new Thickness(6, 2, 6, 0),
            });
            return;
        }
        foreach (var f in files)
        {
            var name = Path.GetFileNameWithoutExtension(f).Replace(".pipeline", "");
            var path = f;
            var locked = FileLock.IsLocked(path);
            // 锁徽标（Sdk.Ui.LockGlyph）：行右端对齐一列，解锁=绿色开锁，锁定=灰色闭锁
            var lockBtn = new Button
            {
                Content = LockGlyph.Build(locked),
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Padding = new Thickness(6, 0, 2, 0),
                VerticalAlignment = VerticalAlignment.Center,
                Cursor = new Cursor(StandardCursorType.Hand),
            };
            lockBtn.Click += (_, e) =>
            {
                e.Handled = true;   // Button.Click 是冒泡路由事件：阻止传到行按钮（避免同时触发"打开算法"）
                FileLock.Toggle(path);
                _bus.Publish(BusTopics.AlgoLocked, path);   // 已打开页面同步只读状态
                RefreshPipelines();
            };
            var row = new Button
            {
                Classes = { "sideitem" },
                // 覆盖样式的 Left：内容横向撑满按钮，锁才真正钉在行右端（否则内容贴合文字宽度，行长短不一）
                HorizontalContentAlignment = HorizontalAlignment.Stretch,
                // 行拉伸满宽：名字左对齐、锁统一钉在行右端——所有行等宽、锁对齐一列（名字长短不齐也不参差）
                Content = new Grid
                {
                    ColumnDefinitions = new ColumnDefinitions("*,Auto"),
                    Children =
                    {
                        new StackPanel
                        {
                            Orientation = Orientation.Horizontal,
                            Spacing = 3,
                            Children =
                            {
                                new TextBlock { Text = "📄", FontSize = 11, VerticalAlignment = VerticalAlignment.Center },
                                new TextBlock { Text = name, VerticalAlignment = VerticalAlignment.Center,
                                                TextTrimming = TextTrimming.CharacterEllipsis },
                            },
                        },
                        lockBtn,
                    },
                },
            };
            lockBtn.SetValue(Grid.ColumnProperty, 1);
            if (_selectedPaths.Contains(path))
                row.Background = SelectedBrush;
            _rowByPath[path] = row;
            // 单击 = 选中（Ctrl+单击 = 多选切换）；双击 = 行内改名（统一用 Sdk.Ui 组件，BUG-059 教训）
            var dbl = new DoubleClickDetector();
            DispatcherTimer? pendingOpen = null;
            row.Click += (_, _) =>
            {
                // Ctrl+单击 = 多选切换；普通单击 = 只选中该行（Ctrl+C 复制对象）
                if (_ctrlDown)
                {
                    if (!_selectedPaths.Add(path)) _selectedPaths.Remove(path);
                }
                else
                {
                    _selectedPaths.Clear();
                    _selectedPaths.Add(path);
                }
                _lastClickedPath = path;
                UpdateRowHighlights();
                pendingOpen?.Stop();
                pendingOpen = null;
                if (dbl.IsDouble())
                {
                    BeginRename(row, path, name);
                }
                else
                {
                    // 单击动作延迟派发（InlineRename.SingleClickDelayMs）：等第二击，否则开页抢焦点会把改名框瞬间提交掉
                    var path_ = path;
                    var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(InlineRename.SingleClickDelayMs) };
                    timer.Tick += (_, _) => { timer.Stop(); _bus.Publish(BusTopics.PipelineOpen, path_); };
                    pendingOpen = timer;
                    timer.Start();
                }
            };
            row.Width = _lastViewportW;
            _pipelineRows.Add(row);
            _pipelineSection.Children.Add(row);
        }
        if (_pipelineRows.Count > 0)
            _pipelineRows[0].InvalidateMeasure();
    }

    /// <summary>「流水线算法」标题行：左侧标题，右侧 5 个小图标按钮（新建页面 / 全选 / 全锁 / 解锁 / 删除）。</summary>
    private Control BuildPipelineHeader(List<string> files)
    {
        Control MakeToolBtn(object content, string tip, Action click)
        {
            var btn = new Button
            {
                Content = content,
                Background = Brushes.Transparent,
                BorderThickness = new Thickness(0),
                Padding = new Thickness(4, 1),
                MinWidth = 22,
                MinHeight = 20,
                FontSize = 12,
                Foreground = new SolidColorBrush(Color.FromRgb(0x9A, 0x9A, 0xA2)),
                VerticalAlignment = VerticalAlignment.Center,
                Cursor = new Cursor(StandardCursorType.Hand),
            };
            ToolTip.SetTip(btn, tip);
            btn.Click += (_, e) => { e.Handled = true; click(); };
            return btn;
        }

        var strip = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 1, VerticalAlignment = VerticalAlignment.Center };

        // 新建流水线页面：经总线请求流水线插件开一个空白页（宿主不直接引用插件）
        strip.Children.Add(MakeToolBtn("➕", "新建空白流水线页面", () =>
            _bus.Publish<object?>(BusTopics.PipelineNew, null)));

        // 全选：全部算法加入多选集合（Ctrl+单击行可逐个增减；再点一次取消全选）
        strip.Children.Add(MakeToolBtn("☑", "全选（Ctrl+单击行可增减）", () =>
        {
            if (_selectedPaths.Count == files.Count && files.Count > 0)
            {
                _selectedPaths.Clear();
                LogMessage?.Invoke("已取消全选");
            }
            else
            {
                foreach (var f in files) _selectedPaths.Add(f);
                LogMessage?.Invoke($"已全选 {files.Count} 个算法（删除键批量删、Ctrl+C 复制最后一个点选的）");
            }
            UpdateRowHighlights();
        }));

        // 全锁 / 解锁：对全部算法统一写 Locked 字段（不依赖选择，操作对象明确）
        strip.Children.Add(MakeToolBtn(LockGlyph.Build(true), "全部锁定", () =>
        {
            foreach (var f in files) FileLock.Set(f, true);
            foreach (var f in files) _bus.Publish(BusTopics.AlgoLocked, f);   // 已打开页面同步只读状态
            RefreshPipelines();
            LogMessage?.Invoke($"已锁定全部 {files.Count} 个算法（只读保护）");
        }));
        strip.Children.Add(MakeToolBtn(LockGlyph.Build(false), "全部解锁", () =>
        {
            foreach (var f in files) FileLock.Set(f, false);
            foreach (var f in files) _bus.Publish(BusTopics.AlgoLocked, f);
            RefreshPipelines();
            LogMessage?.Invoke($"已解锁全部 {files.Count} 个算法");
        }));

        // 删除：批量删除选中的（锁定的跳过并提示），删除前弹窗二次确认
        strip.Children.Add(MakeToolBtn(TrashGlyph.Build(), "删除选中的算法（锁定的跳过）", () => _ = DeleteManyAsync()));

        var title = new TextBlock
        {
            Text = "流水线算法",
            FontSize = 11.5,
            Foreground = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x92)),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var header = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto"),
            Margin = new Thickness(6, 10, 0, 4),
            Children = { title, strip },
        };
        title.SetValue(Grid.ColumnProperty, 0);
        strip.SetValue(Grid.ColumnProperty, 1);   // 必须显式设列：否则两者都落在第 0 列重叠成一团
        return header;
    }

    /// <summary>按多选集合刷新所有行的选中高亮。</summary>
    private void UpdateRowHighlights()
    {
        foreach (var (p, row) in _rowByPath)
            row.Background = _selectedPaths.Contains(p) ? SelectedBrush : Brushes.Transparent;
    }

    /// <summary>双击行内改名（Sdk.Ui.InlineRename 统一组件）：重命名文件 + 刷新列表 + 通知流水线插件。</summary>
    private void BeginRename(Button row, string path, string oldName)
    {
        var box = InlineRename.Begin(oldName,
            newName =>
            {
                _renameBox = null;
                if (newName.Length > 0 && newName != oldName)
                {
                    try
                    {
                        var safe = string.Join("_", newName.Split(Path.GetInvalidFileNameChars())).Trim();
                        var newPath = Path.Combine(Path.GetDirectoryName(path) ?? ".", safe + ".pipeline.json");
                        if (!File.Exists(newPath))
                        {
                            File.Move(path, newPath);
                            _bus.Publish(BusTopics.AlgoSaved, newPath);   // 触发刷新
                            return;
                        }
                    }
                    catch { }
                }
                RefreshPipelines();   // 空名/同名/失败 → 恢复
            },
            () => { _renameBox = null; RefreshPipelines(); });   // Esc = 取消，还原原名
        row.Content = box;
        _renameBox = box;
    }


    public void Refresh()
    {
        _root.Children.Add(_pipelineSection);
        RefreshPipelines();
    }
}
