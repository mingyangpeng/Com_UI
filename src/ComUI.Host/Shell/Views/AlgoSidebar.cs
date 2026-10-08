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
/// 「算法配方」侧边栏（活动栏 ⚡）：上半 = 算法插件命令（单击弹参数框运行），下半 = 流水线算法配方（单击发布 pipeline/open）。
/// </summary>
public sealed class AlgoSidebar : UserControl
{
    private readonly Action<AlgoRecord, AlgoCommand> _onRun;
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
    private Button? _selectedRow;
    private TextBox? _renameBox;        // 行内改名进行中（期间放行 TextBox 自身的 Ctrl+C 文本复制）

    public AlgoSidebar(Action<AlgoRecord, AlgoCommand> onRun, IBus bus)
    {
        _onRun = onRun;
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
                DeleteSelected();
            }
        };
    }

    private static string NameOf(string path) =>
        Path.GetFileNameWithoutExtension(path).Replace(".pipeline", "");

    /// <summary>Del/Backspace：删除选中的算法文件（锁定状态拒绝——先解锁或复制副本）。</summary>
    private void DeleteSelected()
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
        try
        {
            var name = NameOf(_lastClickedPath);
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
        var dir = Path.Combine(AppPaths.RepoRoot, "config", "algorithms");
        var files = Directory.Exists(dir)
            ? Directory.EnumerateFiles(dir, "*.pipeline.json").ToList()
            : new List<string>();
        _pipelineSection.Children.Add(new TextBlock
        {
            Text = "流水线算法",
            FontSize = 11.5,
            Foreground = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x92)),
            Margin = new Thickness(6, 10, 0, 4),
        });
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
            if (path == _lastClickedPath)
            {
                _selectedRow = row;
                row.Background = SelectedBrush;
            }
            // 单击 = 延迟打开（等待可能的第二击）；双击 = 行内改名（统一用 Sdk.Ui 组件，BUG-059 教训）
            var dbl = new DoubleClickDetector();
            DispatcherTimer? pendingOpen = null;
            row.Click += (_, _) =>
            {
                // 单击同时选中该行（Ctrl+C 复制对象），高亮反馈
                if (!ReferenceEquals(_selectedRow, row))
                {
                    if (_selectedRow is not null) _selectedRow.Background = Brushes.Transparent;
                    _selectedRow = row;
                    row.Background = SelectedBrush;
                }
                _lastClickedPath = path;
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


    public void Refresh(IReadOnlyList<AlgoRecord> algos)
    {
        _root.Children.Clear();

        if (algos.Count == 0)
        {
            _root.Children.Add(new TextBlock
            {
                Text = "尚无已加载的算法插件\n\n算法 DLL + *.algo.json 放入 comdll/algo/ 后\n按 F5 重新加载",
                FontSize = 12,
                LineHeight = 19,
                Foreground = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7F)),
                Margin = new Thickness(8, 16, 8, 0),
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        foreach (var algo in algos)
        {
            var head = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                Spacing = 6,
                Margin = new Thickness(6, 10, 0, 4),
            };
            head.Children.Add(new TextBlock
            {
                Text = $"{algo.Name}  v{algo.Version}",
                FontSize = 11.5,
                Foreground = new SolidColorBrush(Color.FromRgb(0x8A, 0x8A, 0x92)),
            });
            if (algo.IsJsonDriven)
            {
                head.Children.Add(new Border
                {
                    Background = new SolidColorBrush(Color.FromRgb(0x2D, 0x3F, 0x2F)),
                    CornerRadius = new CornerRadius(3),
                    Padding = new Thickness(5, 0),
                    Child = new TextBlock
                    {
                        Text = "JSON",
                        FontSize = 10,
                        Foreground = new SolidColorBrush(Color.FromRgb(0x7B, 0xC8, 0x86)),
                    },
                });
            }
            _root.Children.Add(head);

            foreach (var cmd in algo.Commands)
            {
                var a = algo;
                var c = cmd;
                var row = new Button
                {
                    Classes = { "sideitem" },
                    Content = new StackPanel
                    {
                        Orientation = Orientation.Horizontal,
                        Spacing = 8,
                        Children =
                        {
                            new TextBlock
                            {
                                Text = "▶",
                                FontSize = 10,
                                Foreground = new SolidColorBrush(Color.FromRgb(0x4F, 0xC1, 0xFF)),
                                VerticalAlignment = VerticalAlignment.Center,
                            },
                            new TextBlock { Text = c.Title, VerticalAlignment = VerticalAlignment.Center },
                        },
                    },
                };
                ToolTip.SetTip(row, string.IsNullOrWhiteSpace(c.Id) ? c.Title : c.Id);
                row.Click += (_, _) => _onRun(a, c);
                _root.Children.Add(row);
            }
        }
        _root.Children.Add(_pipelineSection);
        RefreshPipelines();
    }
}
