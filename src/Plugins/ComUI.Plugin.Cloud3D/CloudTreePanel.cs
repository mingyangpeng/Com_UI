using System;
using System.Globalization;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Controls.Shapes;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using OpenTK.Mathematics;

using ComUI.Sdk;
using ComUI.Sdk.Ui;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 数据树（原点云树）：「2D 图像」栏（订阅 image/stitched，按 Id 命名，无名归入"未命名图像"）
/// + 「3D 点云」栏（行首小绿点=显隐开关，单击行=选中自动选第一个；Ctrl+C 复制 / Ctrl+V 粘贴 /
/// Del 删除 / 双击行名改名）；下方"点云属性"分段常驻显示选中实体的
/// 颜色模式/单色RGB/点大小/变换矩阵（CloudCompare 式）。
/// </summary>
public sealed class CloudTreePanel : UserControl
{
    private static readonly Brush Dim = new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D));
    private static readonly Brush DotOn = new SolidColorBrush(Color.FromRgb(0x66, 0xBB, 0x6A));
    private static readonly Brush DotOff = new SolidColorBrush(Color.FromRgb(0x2A, 0x2A, 0x2E));
    private static readonly Brush DotOffStroke = new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));
    private static readonly Brush SelBg = new SolidColorBrush(Color.FromRgb(0x2F, 0x2F, 0x33));

    private readonly Cloud3DView _view;
    private readonly IBus? _bus;
    private readonly StackPanel _imageList = new() { Margin = new Thickness(2, 1) };
    private readonly Border _eyeBtn = new()
    {
        Width = 26, Height = 20, Background = Brushes.Transparent, CornerRadius = new CornerRadius(3),
        Cursor = new Cursor(StandardCursorType.Hand), VerticalAlignment = VerticalAlignment.Center,
    };
    private string? _lastShownId;   // 最后显示过的图像（总开关重新睁开时恢复）
    private readonly Border _cloudEyeBtn = new()
    {
        Width = 26, Height = 20, Background = Brushes.Transparent, CornerRadius = new CornerRadius(3),
        Cursor = new Cursor(StandardCursorType.Hand), VerticalAlignment = VerticalAlignment.Center,
    };
    private List<string> _lastVisibleClouds = new();   // 总开关闭眼前显示中的点云集
    private IDisposable? _imgSub;

    // 顶部并列 Tab：0=2D 图像，1=3D 点云（两栏内容叠放，切换只切 IsVisible）
    private int _activeTab;
    private Border _tabImage = null!;
    private Border _tabCloud = null!;
    private TextBlock _tabImageText = null!;
    private TextBlock _tabCloudText = null!;
    private Border _imageContent = null!;
    private Border _cloudContent = null!;

    /// <summary>2D 图像条目：≤16MP 保留像素（可再发送工作台/复制），更大只留元数据。</summary>
    private sealed class ImageEntry
    {
        public string Id = "";
        public int W, H, Frames;
        public string? Source;
        public byte[]? Pixels;
        public bool Displayed;   // 当前正显示在 2D 工作台
        public bool Locked;      // 锁定：不可删除/改名（锁徽标切换，与点云/算法配方同款）
    }

    private readonly List<ImageEntry> _imgEntries = new();
    private string? _imgSelected;   // 选中的图像条目
    private readonly HashSet<string> _imgMulti = new();   // 图像栏多选集合（Ctrl+单击/全选）
    private string? _imgRenaming;   // 正在行内改名的图像
    private string? _imgClipId;     // 图像剪贴板
    private readonly StackPanel _list = new() { Margin = new Thickness(4, 2) };
    private string? _selected;
    private readonly HashSet<string> _cloudMulti = new();   // 点云栏多选集合（Ctrl+单击/全选）
    private string? _renaming;      // 正在双击改名的实体
    private string? _clipId;
    private bool _deselected;       // 用户 Esc 主动取消选中（暂停自动选中）
    private bool _ctrlDown;         // Ctrl 按住状态（Ctrl+单击 = 多选切换）
    private int _xformMode = 2;     // 空间变换输入方式：0=欧拉角+平移, 1=四元数+平移, 2=4x4 变换矩阵

    public CloudTreePanel(Cloud3DView view, IBus? bus = null)
    {
        _view = view;
        _bus = bus;
        Background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x20));
        Focusable = true;

        _cloudEyeBtn.PointerPressed += (_, e) =>
        {
            ToggleCloudEye();
            e.Handled = true;
        };
        _eyeBtn.PointerPressed += (_, e) =>
        {
            ToggleMasterEye();
            e.Handled = true;
        };
        UpdateCloudEyeGlyph();
        UpdateEyeGlyph();
        // 顶部并列 Tab（2D 图像 / 3D 点云），每个 Tab 自带总开关眼按钮；内容区两栏叠放，切换只切 IsVisible
        _tabImageText = new TextBlock
        {
            Text = "2D 图像", FontWeight = FontWeight.SemiBold, FontSize = 12.5,
            VerticalAlignment = VerticalAlignment.Center,
        };
        _tabCloudText = new TextBlock
        {
            Text = "3D 点云", FontWeight = FontWeight.SemiBold, FontSize = 12.5,
            VerticalAlignment = VerticalAlignment.Center,
        };
        _tabImage = BuildTab(_tabImageText, _eyeBtn, 0);
        _tabCloud = BuildTab(_tabCloudText, _cloudEyeBtn, 1);
        var tabStrip = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,*"),
            Margin = new Thickness(6, 6, 6, 2),
        };
        _tabImage.SetValue(Grid.ColumnProperty, 0);
        _tabCloud.SetValue(Grid.ColumnProperty, 1);
        tabStrip.Children.Add(_tabImage);
        tabStrip.Children.Add(_tabCloud);

        // 工具条：全选 / 加锁 / 解锁 / 删除（作用于当前 Tab 栏）
        var toolStrip = BuildToolStrip();
        var topBlock = new StackPanel { Children = { tabStrip, toolStrip } };

        // 两栏内容叠放在同一 Grid 格：列表字段（_imageList/_list）保持原引用，Refresh 逻辑不变
        _imageContent = new Border
        {
            Child = new ScrollViewer { Content = _imageList, HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled },
        };
        _cloudContent = new Border
        {
            Child = new ScrollViewer
            {
                Content = new StackPanel { Margin = new Thickness(4, 4, 4, 2), Children = { _list } },
                HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
            },
        };
        var contentSwitch = new Grid();
        contentSwitch.Children.Add(_imageContent);
        contentSwitch.Children.Add(_cloudContent);

        // 属性区已移至宿主右侧属性面板（经 PropsShow 总线推送），数据树只保留 Tab + 工具条 + 列表
        var root = new Grid { RowDefinitions = new RowDefinitions("Auto,*") };
        topBlock.SetValue(Grid.RowProperty, 0);
        root.Children.Add(topBlock);
        contentSwitch.SetValue(Grid.RowProperty, 1);
        root.Children.Add(contentSwitch);
        Content = root;

        view.CloudsChanged += Refresh;
        AttachedToVisualTree += (_, _) => AttachBus();
        DetachedFromVisualTree += (_, _) => { _imgSub?.Dispose(); _imgSub = null; };
        KeyDown += (_, e) =>
        {
            if (e.Key is Key.LeftCtrl or Key.RightCtrl) _ctrlDown = true;
            switch (e.Key)
            {
                case Key.A when e.KeyModifiers.HasFlag(KeyModifiers.Control):
                    SelectAllCurrent(); e.Handled = true; break;
                case Key.C when e.KeyModifiers.HasFlag(KeyModifiers.Control):
                    if (_activeTab == 0 && _imgSelected is { }) _imgClipId = _imgSelected; else CopySel();
                    RefreshDetail(); e.Handled = true; break;
                case Key.V when e.KeyModifiers.HasFlag(KeyModifiers.Control):
                    if (_activeTab == 0 && _imgClipId is { } && _imgEntries.Any(x => x.Id == _imgClipId)) PasteImage(); else Paste();
                    e.Handled = true; break;
                case Key.Delete or Key.Back:
                    _ = DeleteCurrentAsync(); e.Handled = true; break;
                case Key.Escape:
                    _selected = null; _imgSelected = null; _imgMulti.Clear(); _cloudMulti.Clear();
                    _deselected = true; Refresh(); RebuildImageRows(); RefreshDetail(); e.Handled = true; break;
            }
        };
        KeyUp += (_, e) =>
        {
            if (e.Key is Key.LeftCtrl or Key.RightCtrl) _ctrlDown = false;
        };
        Refresh();
        RebuildImageRows();
        SwitchTab(0);   // 默认显示 2D 图像 Tab，初始化高亮/可见性/属性标题
    }

    // ==================== 2D 图像栏 ====================

    /// <summary>顶部并列 Tab：文字 + 总开关眼按钮，点击切换 2D/3D 内容栏。</summary>
    private Border BuildTab(TextBlock text, Border eyeBtn, int index)
    {
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        grid.Children.Add(text);
        eyeBtn.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(eyeBtn);
        var tab = new Border
        {
            Child = grid,
            Padding = new Thickness(8, 5),
            Margin = new Thickness(0, 0, 4, 0),
            CornerRadius = new CornerRadius(4),
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        // 点 Tab 切换（眼按钮自己 Handled=true，点它不会冒泡触发切换）
        tab.PointerPressed += (_, e) =>
        {
            SwitchTab(index);
            e.Handled = true;
        };
        return tab;
    }

    /// <summary>切换 2D/3D Tab：只切内容可见性 + Tab 高亮 + 属性区标题，列表字段原样不动。</summary>
    private void SwitchTab(int index)
    {
        _activeTab = index;
        bool image = index == 0;
        _imageContent.IsVisible = image;
        _cloudContent.IsVisible = !image;
        _tabImage.Background = image ? SelBg : Brushes.Transparent;
        _tabCloud.Background = image ? Brushes.Transparent : SelBg;
        _tabImageText.Foreground = image ? Brushes.White : Dim;
        _tabCloudText.Foreground = image ? Dim : Brushes.White;
        RefreshDetail();
    }

    // ==================== 多选 / 工具条 ====================

    /// <summary>工具条：全选 / 加锁 / 解锁 / 删除，作用于当前 Tab 栏。</summary>
    private Control BuildToolStrip()
    {
        var strip = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 2,
            Margin = new Thickness(8, 2, 8, 2),
        };
        strip.Children.Add(MakeToolBtn("☑", "全选（Ctrl+A）", () => SelectAllCurrent()));
        strip.Children.Add(MakeToolBtn("🔒", "加锁选中项（禁止删除/改名）", () => SetLockCurrent(true)));
        strip.Children.Add(MakeToolBtn("🔓", "解锁选中项", () => SetLockCurrent(false)));
        strip.Children.Add(MakeToolBtn(TrashGlyph.Build(), "删除选中项（需确认）", () => _ = DeleteCurrentAsync()));
        return strip;
    }

    private static Button MakeToolBtn(object glyph, string tip, Action onClick)
    {
        var btn = new Button
        {
            Content = glyph is Control c ? c : new TextBlock { Text = glyph.ToString(), FontSize = 12.5, VerticalAlignment = VerticalAlignment.Center },
            Classes = { "ghost" },
            Width = 26, Height = 22,
            Padding = new Thickness(0),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(btn, tip);
        btn.Click += (_, _) => onClick();
        return btn;
    }

    /// <summary>当前栏目标 Id 集合：图像=_imgEntries；点云=_view 的云信息。</summary>
    private IEnumerable<string> CurrentIds()
        => _activeTab == 0
            ? _imgEntries.Select(e => e.Id).ToList()
            : _view.GetCloudInfos().Select(i => i.Id).ToList();

    /// <summary>当前栏多选集合（图像/点云各自独立）。</summary>
    private HashSet<string> CurrentMulti() => _activeTab == 0 ? _imgMulti : _cloudMulti;

    /// <summary>当前生效的选择集：多选非空用多选，否则回退单选目标（主选中）。</summary>
    private List<string> EffectiveSelection()
    {
        var multi = CurrentMulti();
        if (multi.Count > 0) return multi.Where(id => CurrentIds().Contains(id)).ToList();
        if (_activeTab == 0) return _imgSelected is { } id0 ? new List<string> { id0 } : new List<string>();
        return _selected is { } id1 ? new List<string> { id1 } : new List<string>();
    }

    private void SelectAllCurrent()
    {
        var multi = CurrentMulti();
        multi.Clear();
        foreach (var id in CurrentIds()) multi.Add(id);
        RefreshSelectionUI();
        _view.TreeStatus($"已全选 {multi.Count} 项");
    }

    /// <summary>刷新两栏行高亮（选中项=SelBg）。</summary>
    private void RefreshSelectionUI()
    {
        Refresh();          // 点云行按 _selected + _cloudMulti 重建
        RebuildImageRows(); // 图像行按 _imgSelected + _imgMulti 重建
    }

    private void SetLockCurrent(bool locked)
    {
        var ids = EffectiveSelection();
        if (ids.Count == 0) { _view.TreeStatus("先选中要加锁/解锁的条目"); return; }
        int changed = 0;
        if (_activeTab == 0)
        {
            foreach (var id in ids)
            {
                var e = _imgEntries.FirstOrDefault(x => x.Id == id);
                if (e is not null && e.Locked != locked) { e.Locked = locked; changed++; }
            }
            RebuildImageRows();
        }
        else
        {
            foreach (var id in ids)
            {
                var info = _view.GetCloudInfos().FirstOrDefault(c => c.Id == id);
                if (info.Id is null || info.Locked == locked) continue;
                _view.SetCloudLocked(id, locked); changed++;
            }
            Refresh();
        }
        RefreshDetail();
        _view.TreeStatus($"已{(locked ? "加锁" : "解锁")} {changed} 项");
    }

    private async Task DeleteCurrentAsync()
    {
        var ids = EffectiveSelection();
        if (ids.Count == 0) { _view.TreeStatus("先选中要删除的条目"); return; }

        // 过滤锁定项（锁定=不可删除），仅对可删项确认
        var removable = _activeTab == 0
            ? ids.Where(id => _imgEntries.FirstOrDefault(x => x.Id == id) is { Locked: false }).ToList()
            : ids.Where(id => _view.GetCloudInfos().FirstOrDefault(c => c.Id == id) is { Id: not null, Locked: false }).ToList();
        int lockedCount = ids.Count - removable.Count;
        if (removable.Count == 0)
        {
            _view.TreeStatus($"选中的 {lockedCount} 项均已锁定，先解锁再删除");
            return;
        }
        var owner = TopLevel.GetTopLevel(this) as Window;
        string extra = lockedCount > 0 ? $"\n（另有 {lockedCount} 项已锁定，将跳过）" : "";
        string msg = removable.Count == 1
            ? $"确定删除「{removable[0]}」？此操作不可撤销。{extra}"
            : $"确定删除选中的 {removable.Count} 项？此操作不可撤销。{extra}";
        if (!await ConfirmDialog.ShowAsync(owner, "删除确认", msg, "删除", danger: true)) return;

        if (_activeTab == 0)
        {
            foreach (var id in removable) RemoveImage(id);
        }
        else
        {
            foreach (var id in removable) _view.RemoveCloud(id);
        }
        CurrentMulti().Clear();
        _selected = null; _imgSelected = null;
        RefreshSelectionUI();
        RefreshDetail();
        _view.TreeStatus($"已删除 {removable.Count} 项");
    }

    /// <summary>总开关：有显示中的图像 → 全部隐藏（闭眼）；全部隐藏 → 恢复显示最后看过的图像（睁眼）。</summary>
    private void ToggleMasterEye()
    {
        if (_imgEntries.Any(x => x.Displayed))
        {
            var showing = _imgEntries.First(x => x.Displayed);
            _lastShownId = showing.Id;
            if (_bus is not null)
                _bus.Publish(BusTopics.ImageStitched, new ImagePayload
                {
                    Id = showing.Id, Width = 0, Height = 0,
                    PixelsBgra = Array.Empty<byte>(), Source = "数据树（已隐藏）",
                });
            foreach (var x in _imgEntries) x.Displayed = false;
        }
        else
        {
            var target = _imgEntries.FirstOrDefault(x => x.Id == _lastShownId && x.Pixels is not null)
                ?? _imgEntries.FirstOrDefault(x => x.Pixels is not null);
            if (target is not null) SendImageToWorkbench(target);
        }
        RebuildImageRows();
        UpdateEyeGlyph();
    }

    /// <summary>眼图标：睁眼=绿（眼眶+瞳孔）；闭眼=灰（一条线）。形状绘制（emoji 是彩色字体无法着色——BUG-036 同源）。</summary>
    private static Canvas BuildEyeGlyph(bool open)
    {
        var brush = open ? new SolidColorBrush(Color.FromRgb(0x66, 0xBB, 0x6A))
                         : new SolidColorBrush(Color.FromRgb(0x77, 0x77, 0x7C));
        var canvas = new Canvas { Width = 16, Height = 10, VerticalAlignment = VerticalAlignment.Center };
        if (open)
        {
            var eye = new Ellipse { Width = 14, Height = 9, Stroke = brush, StrokeThickness = 1.5 };
            Canvas.SetLeft(eye, 1); Canvas.SetTop(eye, 0.5);
            var pupil = new Ellipse { Width = 4, Height = 4, Fill = brush };
            Canvas.SetLeft(pupil, 6); Canvas.SetTop(pupil, 3);
            canvas.Children.Add(eye);
            canvas.Children.Add(pupil);
        }
        else
        {
            var lid = new Line { StartPoint = new Point(1, 5), EndPoint = new Point(15, 5), Stroke = brush, StrokeThickness = 1.8 };
            canvas.Children.Add(lid);
        }
        return canvas;
    }

    private void UpdateEyeGlyph()
    {
        bool open = _imgEntries.Any(x => x.Displayed);
        _eyeBtn.Child = BuildEyeGlyph(open);
        ToolTip.SetTip(_eyeBtn, open ? "图像显示中，点击全部隐藏" : "图像已隐藏，点击恢复显示");
    }

    private void UpdateCloudEyeGlyph()
    {
        bool open = _view.GetCloudInfos().Any(i => i.Visible);
        _cloudEyeBtn.Child = BuildEyeGlyph(open);
        ToolTip.SetTip(_cloudEyeBtn, open ? "点云显示中，点击全部隐藏" : "点云已隐藏，点击恢复显示");
    }

    /// <summary>3D 点云总开关：有显示中的 → 全部隐藏（记住显示集）；全部隐藏 → 恢复记住的显示集。</summary>
    private void ToggleCloudEye()
    {
        var infos = _view.GetCloudInfos();
        if (infos.Count == 0) return;
        if (infos.Any(i => i.Visible))
        {
            _lastVisibleClouds = infos.Where(i => i.Visible).Select(i => i.Id).ToList();
            foreach (var i in infos) _view.SetCloudVisible(i.Id, false);
        }
        else
        {
            var restore = _lastVisibleClouds.Count > 0 ? _lastVisibleClouds : infos.Select(i => i.Id).ToList();
            foreach (var id in restore) _view.SetCloudVisible(id, true);
        }
        Refresh();
        UpdateCloudEyeGlyph();
    }

    private void AttachBus()
    {
        if (_bus is null || _imgSub is not null) return;
        _imgSub = _bus.Subscribe<ImagePayload>(BusTopics.ImageStitched, OnImagePublished, uiThread: true);
        if (_bus.TryGetLatest<ImagePayload>(BusTopics.ImageStitched, out var latest))
            TrackImage(latest);
    }

    private void OnImagePublished(ImagePayload img) => TrackImage(img);

    private void TrackImage(ImagePayload img)
    {
        if (img.Width <= 0 || img.Height <= 0) return;   // 隐藏信号（0×0）不进条目
        var key = string.IsNullOrEmpty(img.Id) ? "未命名图像" : img.Id!;
        var entry = _imgEntries.FirstOrDefault(x => x.Id == key);
        if (entry is null)
        {
            entry = new ImageEntry { Id = key };
            _imgEntries.Add(entry);
            if (_imgSelected is null && _selected is null) _imgSelected = entry.Id;   // 开箱即见
        }
        entry.W = img.Width;
        entry.H = img.Height;
        entry.Frames++;
        entry.Source = img.Source;
        entry.Pixels = img.Width * (long)img.Height <= 16_000_000 ? img.PixelsBgra : null;   // 大图只留元数据
        // 显示状态镜像：工作台当前显示的就是最近发布的这张
        foreach (var x in _imgEntries) x.Displayed = x == entry;
        RebuildImageRows();
        if (_imgSelected == entry.Id) RefreshDetail();
    }

    private void RebuildImageRows()
    {
        _imageList.Children.Clear();
        UpdateEyeGlyph();
        // 清理已不存在的图像多选 Id
        var imgIds = _imgEntries.Select(e => e.Id).ToHashSet();
        _imgMulti.RemoveWhere(id => !imgIds.Contains(id));
        if (_imgEntries.Count == 0)
        {
            return;
        }
        foreach (var entry in _imgEntries)
            _imageList.Children.Add(BuildImageRow(entry));
    }

    private Control BuildImageRow(ImageEntry entry)
    {
        // 绿点 = 当前显示在 2D 工作台；点击 = 发送该图像到工作台显示
        var dot = new Ellipse
        {
            Width = 10, Height = 10,
            VerticalAlignment = VerticalAlignment.Center,
            Fill = entry.Displayed ? DotOn : DotOff,
            Stroke = entry.Displayed ? null : DotOffStroke,
            StrokeThickness = entry.Displayed ? 0 : 1.5,
        };
        var dotBtn = new Border
        {
            Width = 24, Height = 22, Background = Brushes.Transparent,
            Child = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Children = { dot },
            },
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(dotBtn, entry.Pixels is null
            ? "大图未保留像素，无法再发送"
            : entry.Displayed ? "显示中，点击隐藏" : "点击显示（发送到 2D 工作台）");
        dotBtn.PointerPressed += (_, e) =>
        {
            if (entry.Displayed) HideImage(entry); else SendImageToWorkbench(entry);
            e.Handled = true;
        };

        Control nameCell;
        if (_imgRenaming == entry.Id)
        {
            nameCell = InlineRename.Begin(entry.Id,
                newId =>
                {
                    _imgRenaming = null;
                    var oldId = entry.Id;
                    newId = newId.Trim();
                    if (newId.Length > 0 && newId != oldId && _imgEntries.All(x => x.Id != newId))
                    {
                        entry.Id = newId;
                        if (_imgSelected == oldId) _imgSelected = newId;
                        if (_imgClipId == oldId) _imgClipId = newId;
                        if (_imgMulti.Remove(oldId)) _imgMulti.Add(newId);
                    }
                    RebuildImageRows(); RefreshDetail();
                },
                () => { _imgRenaming = null; RebuildImageRows(); });
            ((TextBox)nameCell).BorderThickness = new Thickness(1);
        }
        else
        {
            nameCell = new TextBlock
            {
                Text = entry.Id, FontSize = 12.5,
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = entry.Displayed ? Brushes.White : Dim,
                TextTrimming = TextTrimming.CharacterEllipsis,
            };
        }
        var size = new TextBlock
        {
            Text = $"{entry.W}×{entry.H}", FontSize = 11, Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(6, 0, 0, 0),
        };

        // 锁徽标（Sdk.Ui.LockGlyph，与点云/算法配方同款）：绿开锁=未锁，灰闭锁=锁定（不可删除/改名）
        var lockBtn = new Button
        {
            Content = LockGlyph.Build(entry.Locked),
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Padding = new Thickness(6, 0, 2, 0),
            VerticalAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(lockBtn, entry.Locked ? "已锁定（不可删除/改名），点击解锁" : "未锁定，点击锁定");
        lockBtn.Click += (_, _) => { entry.Locked = !entry.Locked; RebuildImageRows(); };

        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto,Auto") };
        grid.Children.Add(dotBtn);
        nameCell.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(nameCell);
        size.SetValue(Grid.ColumnProperty, 2);
        grid.Children.Add(size);
        lockBtn.SetValue(Grid.ColumnProperty, 3);
        grid.Children.Add(lockBtn);

        bool selected = _imgSelected == entry.Id || _imgMulti.Contains(entry.Id);
        var row = new Border
        {
            Height = 26,
            Child = grid,
            Margin = new Thickness(4, 0, 4, 0),
            Padding = new Thickness(2, 1),
            CornerRadius = new CornerRadius(3),
            Background = selected ? SelBg : Brushes.Transparent,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        row.PointerPressed += (_, e) =>
        {
            if (e.ClickCount >= 2)
            {
                if (entry.Locked) { _view.TreeStatus($"「{entry.Id}」已锁定，先解锁再改名"); e.Handled = true; return; }
                _imgRenaming = entry.Id;   // 双击行名 → 行内改名
                RebuildImageRows();
                e.Handled = true;
                return;
            }
            if (_ctrlDown)
            {
                // Ctrl+单击：切换多选（主选中保持在最后单击项）
                if (!_imgMulti.Add(entry.Id)) _imgMulti.Remove(entry.Id);
                _imgSelected = entry.Id;
                _selected = null;          // 图像/点云选中互斥
                Focus();
                RebuildImageRows(); RefreshDetail();
                e.Handled = true;
                return;
            }
            _imgSelected = entry.Id;
            _imgMulti.Clear();             // 普通单击取消多选
            _selected = null;          // 图像/点云选中互斥
            Focus();
            Refresh();                 // 云列表去掉旧选中高亮
            // 先切标签（会重置属性面板为面板模式），再推云属性，避免属性被覆盖
            if (entry.Pixels is not null) SendImageToWorkbench(entry);
            _bus?.Publish(BusTopics.PanelActivate, "ui.image2d.view");
            RefreshDetail();
            e.Handled = true;
        };
        return row;
    }

    /// <summary>把保留像素的图像重新发布到 2D 工作台（绿点=当前显示）。</summary>
    private void SendImageToWorkbench(ImageEntry entry)
    {
        if (_bus is null || entry.Pixels is null) return;
        _bus.Publish(BusTopics.ImageStitched, new ImagePayload
        {
            Id = entry.Id, Width = entry.W, Height = entry.H,
            PixelsBgra = entry.Pixels, Source = entry.Source ?? "数据树",
        });
        foreach (var x in _imgEntries) x.Displayed = x == entry;
        _lastShownId = entry.Id;
        RebuildImageRows();
    }

    /// <summary>隐藏：发布 0×0 隐藏信号，2D 工作台清屏。</summary>
    private void HideImage(ImageEntry entry)
    {
        if (_bus is null) return;
        _bus.Publish(BusTopics.ImageStitched, new ImagePayload
        {
            Id = entry.Id, Width = 0, Height = 0,
            PixelsBgra = Array.Empty<byte>(), Source = "数据树（已隐藏）",
        });
        entry.Displayed = false;
        RebuildImageRows();
        if (_imgSelected == entry.Id) RefreshDetail();
    }

    private void PasteImage()
    {
        var src = _imgEntries.FirstOrDefault(x => x.Id == _imgClipId);
        if (src is null) return;
        var newId = src.Id + "-副本";
        for (int i = 2; _imgEntries.Any(x => x.Id == newId); i++) newId = $"{src.Id}-副本{i}";
        _imgEntries.Add(new ImageEntry
        {
            Id = newId, W = src.W, H = src.H, Frames = src.Frames,
            Source = src.Source, Pixels = src.Pixels, Displayed = false,
        });
        _imgSelected = newId;
        RebuildImageRows();
        RefreshDetail();
    }

    private void RemoveImage(string id)
    {
        var entry = _imgEntries.FirstOrDefault(x => x.Id == id);
        if (entry is null) return;
        if (entry.Locked) { _view.TreeStatus($"「{id}」已锁定，先解锁再删除"); return; }
        _imgEntries.Remove(entry);
        _imgMulti.Remove(id);
        if (_imgSelected == id) _imgSelected = null;
        RebuildImageRows();
        RefreshDetail();
    }

    // ==================== 列表 ====================

    private void Refresh()
    {
        _list.Children.Clear();
        var infos = _view.GetCloudInfos();
        // 清理已不存在的点云多选 Id（数据刷新/删除后）
        var cloudIds = infos.Select(i => i.Id).ToHashSet();
        _cloudMulti.RemoveWhere(id => !cloudIds.Contains(id));
        // 无选中（且非用户主动取消）时自动选第一个，属性区开箱即见
        if (!_deselected && _selected is null && _imgSelected is null && infos.Count > 0)
            _selected = infos[0].Id;
        if (_selected is not null && infos.All(i => i.Id != _selected))
            _selected = infos.Count > 0 && !_deselected ? infos[0].Id : null;
        foreach (var info in infos)
            _list.Children.Add(BuildRow(info));
        UpdateCloudEyeGlyph();
        RefreshDetail();
    }

    private Control BuildRow(Cloud3DView.CloudInfo info)
    {
        // 显隐小绿点：显示=实心绿；隐藏=空心灰圈
        var dot = new Ellipse
        {
            Width = 10, Height = 10,
            VerticalAlignment = VerticalAlignment.Center,
            Fill = info.Visible ? DotOn : DotOff,
            Stroke = info.Visible ? null : DotOffStroke,
            StrokeThickness = info.Visible ? 0 : 1.5,
        };
        var dotBtn = new Border
        {
            Width = 24, Height = 22,
            Background = Brushes.Transparent,   // 透明 Brush 可命中
            Child = new StackPanel
            {
                Orientation = Orientation.Horizontal,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
                Children = { dot },
            },
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(dotBtn, info.Visible ? "显示中，点击隐藏" : "已隐藏，点击显示");
        dotBtn.PointerPressed += (_, e) =>
        {
            _view.SetCloudVisible(info.Id, !info.Visible);
            e.Handled = true;
        };

        Control nameCell;
        if (_renaming == info.Id)
        {
            // 双击改名：Sdk.Ui.InlineRename 统一组件（Enter/失焦提交，Esc 取消）
            nameCell = InlineRename.Begin(info.Id,
                newId =>
                {
                    _renaming = null;
                    var oldId = info.Id;
                    if (newId.Length > 0 && newId != oldId && _view.RenameCloud(oldId, newId))
                    {
                        if (_selected == oldId) _selected = newId;
                        if (_clipId == oldId) _clipId = newId;
                        if (_cloudMulti.Remove(oldId)) _cloudMulti.Add(newId);
                    }
                    Refresh();
                },
                () => { _renaming = null; Refresh(); });
            ((TextBox)nameCell).BorderThickness = new Thickness(1);
        }
        else
        {
            nameCell = new TextBlock
            {
                Text = info.Id,
                FontSize = 12.5,
                VerticalAlignment = VerticalAlignment.Center,
                Foreground = info.Visible ? Brushes.White : Dim,
            };
        }
        var count = new TextBlock
        {
            Text = info.Loaded < info.Count ? $"{info.Loaded:N0}/{info.Count:N0}" : $"{info.Count:N0}",
            FontSize = 11, Foreground = Dim,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(6, 0, 0, 0),
        };

        // 锁徽标（Sdk.Ui.LockGlyph，与算法配方同款）：绿开锁=未锁，灰闭锁=锁定（不可删除/改名、切数据源保留）
        var lockBtn = new Button
        {
            Content = LockGlyph.Build(info.Locked),
            Background = Brushes.Transparent,
            BorderThickness = new Thickness(0),
            Padding = new Thickness(6, 0, 2, 0),
            VerticalAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        ToolTip.SetTip(lockBtn, info.Locked ? "已锁定（不可删除/改名，切数据源保留），点击解锁" : "未锁定，点击锁定");
        lockBtn.Click += (_, _) => _view.SetCloudLocked(info.Id, !info.Locked);

        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*,Auto,Auto") };
        grid.Children.Add(dotBtn);
        nameCell.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(nameCell);
        count.SetValue(Grid.ColumnProperty, 2);
        grid.Children.Add(count);
        lockBtn.SetValue(Grid.ColumnProperty, 3);
        grid.Children.Add(lockBtn);

        bool selected = _selected == info.Id || _cloudMulti.Contains(info.Id);
        var row = new Border
        {
            Child = grid,
            Padding = new Thickness(2, 1),
            CornerRadius = new CornerRadius(3),
            Background = selected ? SelBg : Brushes.Transparent,   // 透明 Brush 可命中
            Cursor = new Cursor(StandardCursorType.Hand),
        };
        row.PointerPressed += (_, e) =>
        {
            if (e.ClickCount >= 2 && !string.IsNullOrEmpty(_selected))
            {
                _renaming = info.Id;   // 双击行名 → 行内改名
                Refresh();
                e.Handled = true;
                return;
            }
            if (_ctrlDown)
            {
                // Ctrl+单击：切换多选（主选中保持在最后单击项）
                if (!_cloudMulti.Add(info.Id)) _cloudMulti.Remove(info.Id);
                _selected = info.Id;
                _imgSelected = null;   // 图像/点云选中互斥
                Focus();
                Refresh();
                e.Handled = true;
                return;
            }
            _selected = info.Id;
            _cloudMulti.Clear();         // 普通单击取消多选
            _view.SetWorkingEntity(info.Id);   // M7：树选中 → 3D 工作实体跟随
            _imgSelected = null;   // 图像/点云选中互斥
            _deselected = false;
            Focus();   // 接管键盘（Ctrl+C/V/Del）
            // 先切标签（会重置属性面板为面板模式），再 Refresh 推云属性，避免属性被覆盖
            _bus?.Publish(BusTopics.PanelActivate, "ui.cloud3d.view");   // 点击行名 = 跳转 3D 工作台
            Refresh();
            RebuildImageRows();
            e.Handled = true;
        };
        return row;
    }

    private void CopySel()
    {
        if (string.IsNullOrEmpty(_selected)) return;
        _clipId = _selected;
        RefreshDetail();
    }

    private void Paste()
    {
        if (string.IsNullOrEmpty(_clipId)) return;
        var newId = _view.DuplicateCloud(_clipId);
        if (newId is not null) _selected = newId;
        Refresh();
    }

    private void RemoveSel()
    {
        if (string.IsNullOrEmpty(_selected)) return;
        var id = _selected;
        if (_view.RemoveCloud(id)) { _selected = null; _cloudMulti.Remove(id); }   // 锁定=拒绝（视图已提示），保留选中
        Refresh();
    }

    // ==================== 属性分段 ====================

    // ==================== 右侧属性面板推送（PropsShow 通道，与节点属性同源） ====================

    /// <summary>把当前选中的点云/图像属性推到宿主右侧属性面板。每次新建控件（Avalonia 控件不可重复挂载）。</summary>
    private void RefreshDetail()
    {
        if (_bus is null) return;

        // 图像选中 → 图像属性
        var imgEntry = _imgEntries.FirstOrDefault(x => x.Id == _imgSelected);
        if (imgEntry is not null)
        {
            var rows = new List<(string, string)>
            {
                ("尺寸", $"{imgEntry.W}×{imgEntry.H}"),
                ("已发布", $"{imgEntry.Frames} 帧"),
                ("显示状态", imgEntry.Displayed ? "正在 2D 工作台显示" : (imgEntry.Pixels is null ? "大图未保留像素" : "就绪")),
            };
            if (imgEntry.Source is not null) rows.Add(("来源", imgEntry.Source));
            if (_imgClipId is not null) rows.Add(("剪贴板", _imgClipId));
            _bus.Publish(BusTopics.PropsShow, new PropsRequest
            {
                SectionTitle = "图像",
                Title = imgEntry.Id,
                Description = "运行流水线或从预览卡导入图像；单击条目可发送到 2D 工作台显示。",
                Rows = rows,
                ParamsContent = null,
            });
            return;
        }

        var info = _view.GetCloudInfos().FirstOrDefault(c => c.Id == _selected);
        if (info.Id is null || info.Id.Length == 0)
        {
            _bus.Publish(BusTopics.PropsShow, new PropsRequest
            {
                SectionTitle = "点云",
                Title = "未选中点云",
                Description = "单击数据树中的点云或图像条目，在此查看并编辑属性",
                Rows = Array.Empty<(string, string)>(),
                ParamsContent = null,
            });
            return;
        }

        Vector3 size = info.BoundsMax - info.BoundsMin;
        var infoRows = new List<(string, string)>
        {
            ("点数", $"{info.Count:N0}（已上传 {info.Loaded:N0}）"),
            ("尺寸", $"{size.X:F2} × {size.Y:F2} × {size.Z:F2}"),
            ("可见性", info.Visible ? "显示" : "已隐藏"),
        };
        if (info.SelCount > 0) infoRows.Add(("选区", $"{info.SelCount:N0} 点"));
        if (info.Source is not null) infoRows.Add(("来源", info.Source));
        if (_clipId is not null) infoRows.Add(("剪贴板", _clipId));

        _bus.Publish(BusTopics.PropsShow, new PropsRequest
        {
            SectionTitle = "点云",
            Title = info.Id,
            Rows = infoRows,
            ParamsContent = BuildStyleControls(info),
        });
    }

    /// <summary>构建选中点云的样式控件（每次新建），分"显示样式 / 空间变换"两个可折叠分组。</summary>
    private Control BuildStyleControls(Cloud3DView.CloudInfo info)
    {
        var root = new StackPanel { Spacing = 0, Margin = new Thickness(-12, 0, -12, 0) };

        // ============ 分组一：显示样式（颜色模式 / 点大小 / 单色 RGB） ============
        var styleGroup = new StackPanel { Spacing = 6 };
        var modeCombo = new ComboBox { Classes = { "input" }, MinWidth = 92, FontSize = 11.5, Height = 24 };
        foreach (var name in new[] { "逐点色", "高度伪彩", "单色" }) modeCombo.Items.Add(name);
        modeCombo.SelectedIndex = Math.Clamp(info.ColorMode, 0, 2);
        modeCombo.SelectionChanged += (_, _) =>
        {
            if (_selected is null) return;
            _view.SetCloudStyle(_selected, modeCombo.SelectedIndex, null);
        };

        var sizeBox = new TextBox
        {
            Classes = { "input" }, MinWidth = 52, FontSize = 11.5, Height = 24,
            Text = info.PointSize.ToString("0"),
            Background = Brushes.Transparent, BorderThickness = new Thickness(0),
            Padding = new Thickness(2, 0, 2, 0),
            // 与左侧 TextBlock（VerticalAlignment=Center）严格垂直居中对齐。
            // TextBox.input 样式把 Padding 改成 9,5，本地又覆盖为 2,0,2,0，
            // 默认内容对齐非居中 → 数字相对标签偏上，必须显式居中。
            // 右侧用枚举名 VerticalAlignment.Center，避免与实例属性同名触发 CS0176。
            VerticalAlignment = VerticalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
        };
        void CommitSize()
        {
            if (_selected is null) return;
            if (double.TryParse(sizeBox.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) && v >= 1 && v <= 100)
                _view.SetCloudStyle(_selected, null, (float)v);
            else sizeBox.Text = _view.GetCloudInfos().FirstOrDefault(c => c.Id == _selected).PointSize.ToString("0");
        }
        sizeBox.KeyDown += (_, e) => { if (e.Key == Key.Enter) { CommitSize(); e.Handled = true; } };
        sizeBox.LostFocus += (_, _) => CommitSize();

        var row1 = new Grid { ColumnDefinitions = new ColumnDefinitions("58,*"), Margin = new Thickness(0, 1) };
        row1.Children.Add(new TextBlock { Text = "颜色模式", FontSize = 11.5, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
        modeCombo.SetValue(Grid.ColumnProperty, 1);
        row1.Children.Add(modeCombo);
        var row2 = new Grid { ColumnDefinitions = new ColumnDefinitions("58,*"), Margin = new Thickness(0, 1) };
        row2.Children.Add(new TextBlock { Text = "点大小", FontSize = 11.5, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
        sizeBox.SetValue(Grid.ColumnProperty, 1);
        row2.Children.Add(sizeBox);
        styleGroup.Children.Add(row1);
        styleGroup.Children.Add(row2);

        // ============ 颜色选择：色板点选 + HEX 手动输入（始终可用，选色即切到"单色"模式） ============
        // 常用色板（灰阶 + 彩色），点击任一色块即以该色作为单色渲染，并同步预览与 HEX 输入框。
        (byte R, byte G, byte B)[] palette =
        {
            (255,255,255),(199,199,204),(142,142,147),(80,80,85),(30,30,32),(128,128,133),(190,190,195),(240,240,242),
            (231, 76, 60),(255,149, 0),(255,204, 0),(76,217,100),(0,201,167),(90,200,250),( 0,122,255),(148, 88,214),
            (175, 82,222),(255, 45,149),(255,142,170),(152,118, 84),(155,185, 85),(0,150,199),(40,180, 99),(253,180, 50),
        };
        byte[] cur =
        {
            (byte)Math.Clamp(info.SingleColor.X * 255, 0, 255),
            (byte)Math.Clamp(info.SingleColor.Y * 255, 0, 255),
            (byte)Math.Clamp(info.SingleColor.Z * 255, 0, 255),
        };
        string HexOf(byte r, byte g, byte b) => $"{r:X2}{g:X2}{b:X2}";

        var colorBlock = new StackPanel { Spacing = 6, Margin = new Thickness(0, 8, 0, 0) };

        // —— 当前颜色预览 + HEX 输入（6 位十六进制，默认 FFFFFF）——
        var preview = new Border
        {
            Width = 26, Height = 22, CornerRadius = new CornerRadius(5),
            Background = new SolidColorBrush(Color.FromRgb(cur[0], cur[1], cur[2])),
            BorderBrush = new SolidColorBrush(Color.FromRgb(0x55, 0x55, 0x5C)), BorderThickness = new Thickness(1),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var hexBox = new TextBox
        {
            FontSize = 11, Height = 22, Text = HexOf(cur[0], cur[1], cur[2]),
            Padding = new Thickness(8, 0, 2, 0),
            HorizontalContentAlignment = HorizontalAlignment.Left, VerticalContentAlignment = VerticalAlignment.Center,
            Background = new SolidColorBrush(Color.FromRgb(0x3A, 0x3A, 0x40)),
            Foreground = new SolidColorBrush(Color.FromRgb(0xE6, 0xE6, 0xEA)),
            BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(5),
            CaretBrush = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
        };
        var hexRow = new Grid { ColumnDefinitions = new ColumnDefinitions("30,*"), Margin = new Thickness(0, 1) };
        hexRow.Children.Add(preview);
        hexBox.SetValue(Grid.ColumnProperty, 1);
        hexRow.Children.Add(hexBox);
        colorBlock.Children.Add(hexRow);

        // —— 色板网格（固定列宽，与属性窗口宽度无关）——
        const int cols = 8;
        var paletteGrid = new Grid
        {
            Margin = new Thickness(0, 2, 0, 0),
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        for (int i = 0; i < cols; i++) paletteGrid.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Auto));
        int rows = (palette.Length + cols - 1) / cols;
        for (int i = 0; i < rows; i++) paletteGrid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));

        var swatches = new List<Border>();
        void Highlight(byte r, byte g, byte b)
        {
            foreach (var s in swatches)
                s.BorderThickness = s.Tag is (byte sr, byte sg, byte sb) && sr == r && sg == g && sb == b
                    ? new Thickness(2) : new Thickness(0);
        }
        void ApplyColor(byte r, byte g, byte b)
        {
            if (_selected is null) return;
            // 同时切到"单色"模式，保证点选的颜色立即生效可见。
            _view.SetCloudStyle(_selected, 2, null, new Vector3(r / 255f, g / 255f, b / 255f));
            preview.Background = new SolidColorBrush(Color.FromRgb(r, g, b));
            hexBox.Text = HexOf(r, g, b);
            Highlight(r, g, b);
        }
        for (int i = 0; i < palette.Length; i++)
        {
            var (r, g, b) = palette[i];
            var sw = new Border
            {
                Width = 22, Height = 20, CornerRadius = new CornerRadius(4),
                Background = new SolidColorBrush(Color.FromRgb(r, g, b)),
                BorderBrush = Brushes.White, BorderThickness = new Thickness(0),
                Margin = new Thickness(2), Cursor = new Cursor(StandardCursorType.Hand),
                Tag = (r, g, b), HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center,
            };
            sw.PointerPressed += (_, _) => ApplyColor(r, g, b);
            sw.SetValue(Grid.ColumnProperty, i % cols);
            sw.SetValue(Grid.RowProperty, i / cols);
            swatches.Add(sw);
            paletteGrid.Children.Add(sw);
        }
        // 高亮色板中与当前单色一致的色块。
        Highlight(cur[0], cur[1], cur[2]);
        colorBlock.Children.Add(paletteGrid);

        void CommitHex()
        {
            string s = (hexBox.Text ?? "").Trim().TrimStart('#');
            if (s.Length == 6
                && byte.TryParse(s.Substring(0, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var r)
                && byte.TryParse(s.Substring(2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var g)
                && byte.TryParse(s.Substring(4, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var b))
            {
                cur[0] = r; cur[1] = g; cur[2] = b;
                ApplyColor(r, g, b);
            }
            else hexBox.Text = HexOf(cur[0], cur[1], cur[2]);   // 非法输入回退到当前颜色
        }
        hexBox.KeyDown += (_, e) => { if (e.Key == Key.Enter) { CommitHex(); e.Handled = true; } };
        hexBox.LostFocus += (_, _) => CommitHex();

        styleGroup.Children.Add(colorBlock);
        root.Children.Add(new CollapsibleSection("显示样式", styleGroup, expanded: true));

        // ============ 分组二：空间变换（三种输入：欧拉角+平移 / 四元数+平移 / 4x4 矩阵） ============
        // 统一约定：4x4 为列向量齐次矩阵（行主序存放，左上 3x3 旋转，末列 x3/x7/x11 平移），
        // 与 Eigen::Matrix4d 一致；欧拉角用 ZYX 内旋（Yaw-Pitch-Roll，度），四元数 (x,y,z,w) 归一化。
        var xformGroup = new StackPanel { Spacing = 4 };

        var xformModeCombo = new ComboBox { Classes = { "input" }, MinWidth = 120, FontSize = 11, Height = 24 };
        foreach (var name in new[] { "欧拉角 + 平移", "四元数(x,y,z,w) + 平移", "变换矩阵 4×4" }) xformModeCombo.Items.Add(name);
        xformModeCombo.SelectedIndex = Math.Clamp(_xformMode, 0, 2);

        var inputHost = new Panel();   // 承载当前模式的输入控件（切模式时整体替换）
        // 苹果风格按钮：蓝色主按钮 + 灰色次按钮，圆角无描边
        var applyBtn = new Button
        {
            Content = "应用变换", FontSize = 11, Height = 26, MinWidth = 76,
            Background = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
            Foreground = Brushes.White, BorderThickness = new Thickness(0),
            CornerRadius = new CornerRadius(6), Padding = new Thickness(12, 0),
            HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand), Margin = new Thickness(0, 6, 0, 0),
        };
        var resetBtn = new Button
        {
            Content = "重置", FontSize = 11, Height = 26, MinWidth = 52,
            Background = new SolidColorBrush(Color.FromRgb(0x3A, 0x3A, 0x40)),
            Foreground = new SolidColorBrush(Color.FromRgb(0xE6, 0xE6, 0xEA)),
            BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(6),
            Padding = new Thickness(12, 0), HorizontalContentAlignment = HorizontalAlignment.Center,
            VerticalContentAlignment = VerticalAlignment.Center,
            Cursor = new Cursor(StandardCursorType.Hand), Margin = new Thickness(8, 6, 0, 0),
        };
        var btnRow = new StackPanel { Orientation = Orientation.Horizontal, Children = { applyBtn, resetBtn } };
        var modeRow = new Grid { ColumnDefinitions = new ColumnDefinitions("58,*"), Margin = new Thickness(0, 1) };
        modeRow.Children.Add(new TextBlock { Text = "输入方式", FontSize = 11.5, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
        xformModeCombo.SetValue(Grid.ColumnProperty, 1);
        modeRow.Children.Add(xformModeCombo);

        xformGroup.Children.Add(modeRow);
        xformGroup.Children.Add(inputHost);
        xformGroup.Children.Add(btnRow);
        root.Children.Add(new CollapsibleSection("空间变换", xformGroup, expanded: true));

        // 三个模式的输入控件 + 各自的"读取为 16 元素矩阵"函数
        var matBoxes = new TextBox[16];
        var eulerBoxes = new TextBox[6];   // rx, ry, rz, tx, ty, tz
        var quatBoxes = new TextBox[7];    // x, y, z, w, tx, ty, tz

        // 苹果风格数字输入：圆角浅灰填充、无横线、无边框，聚焦时轻微高亮
        TextBox NumBox(string text, double fontSize, Thickness padding)
        {
            var box = new TextBox
            {
                FontSize = fontSize, Height = 24, Padding = padding, Text = text,
                HorizontalContentAlignment = HorizontalAlignment.Center,
                VerticalContentAlignment = VerticalAlignment.Center,
                Background = new SolidColorBrush(Color.FromRgb(0x3A, 0x3A, 0x40)),
                BorderThickness = new Thickness(0), CornerRadius = new CornerRadius(5),
                Foreground = new SolidColorBrush(Color.FromRgb(0xE6, 0xE6, 0xEA)),
                CaretBrush = new SolidColorBrush(Color.FromRgb(0x0A, 0x84, 0xFF)),
            };
            box.GotFocus += (_, _) => box.Background = new SolidColorBrush(Color.FromRgb(0x4A, 0x4A, 0x52));
            box.LostFocus += (_, _) => box.Background = new SolidColorBrush(Color.FromRgb(0x3A, 0x3A, 0x40));
            return box;
        }
        Grid LabeledRow(string label, TextBox box, double labelW = 52)
        {
            var g = new Grid { ColumnDefinitions = new ColumnDefinitions($"{labelW},*"), Margin = new Thickness(0, 1) };
            g.Children.Add(new TextBlock { Text = label, FontSize = 10, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
            box.SetValue(Grid.ColumnProperty, 1);
            g.Children.Add(box);
            return g;
        }

        // —— 模式 2：4x4 矩阵（苹果风格：等宽圆角格、无横线）——
        var matGrid = new Grid { RowDefinitions = new RowDefinitions("24,24,24,24"), ColumnDefinitions = new ColumnDefinitions("*,*,*,*") };
        for (int r = 0; r < 4; r++)
        {
            for (int c = 0; c < 4; c++)
            {
                var tb = NumBox(info.Xform[r * 4 + c].ToString("0.###"), 10, new Thickness(2, 0));
                matBoxes[r * 4 + c] = tb;
                tb.SetValue(Grid.RowProperty, r);
                tb.SetValue(Grid.ColumnProperty, c);
                tb.Margin = new Thickness(2);
                matGrid.Children.Add(tb);
            }
        }

        // —— 模式 0：欧拉角 + 平移（从当前矩阵反解初值）——
        var eulerGrid = new StackPanel { Spacing = 1 };
        {
            var (rx, ry, rz) = MatrixToEulerDeg(info.Xform);
            string[] el = { "绕X(°)", "绕Y(°)", "绕Z(°)", "平移X", "平移Y", "平移Z" };
            double[] ev = { rx, ry, rz, info.Xform[3], info.Xform[7], info.Xform[11] };
            for (int k = 0; k < 6; k++)
            {
                var tb = NumBox(ev[k].ToString("0.###"), 10, new Thickness(3, 0));
                eulerBoxes[k] = tb;
                eulerGrid.Children.Add(LabeledRow(el[k], tb, 44));
            }
        }

        // —— 模式 1：四元数 + 平移（从当前矩阵反解初值）——
        var quatGrid = new StackPanel { Spacing = 1 };
        {
            var (qx, qy, qz, qw) = MatrixToQuat(info.Xform);
            string[] ql = { "q x", "q y", "q z", "q w", "平移X", "平移Y", "平移Z" };
            double[] qv = { qx, qy, qz, qw, info.Xform[3], info.Xform[7], info.Xform[11] };
            for (int k = 0; k < 7; k++)
            {
                var tb = NumBox(qv[k].ToString("0.####"), 10, new Thickness(3, 0));
                quatBoxes[k] = tb;
                quatGrid.Children.Add(LabeledRow(ql[k], tb, 44));
            }
        }

        void ShowMode(int mode)
        {
            _xformMode = mode;
            inputHost.Children.Clear();
            inputHost.Children.Add(mode switch { 0 => eulerGrid, 1 => quatGrid, _ => matGrid });
        }
        xformModeCombo.SelectionChanged += (_, _) => ShowMode(xformModeCombo.SelectedIndex);
        ShowMode(xformModeCombo.SelectedIndex);   // 修正：原误用 modeCombo（显示样式），应按变换方式

        bool TryParse(TextBox box, out double v) =>
            double.TryParse(box.Text, NumberStyles.Float, CultureInfo.InvariantCulture, out v);

        // 应用后立即重建属性面板：三种输入都回读当前点云真实 Xform，保证矩阵实时显示生效后的变换
        void ApplyAndRefresh(double[] xf)
        {
            if (_selected is null) return;
            _view.SetCloudXform(_selected, xf);
            RefreshDetail();
        }

        void ApplyMatrix()
        {
            if (_selected is null) return;
            int mode = xformModeCombo.SelectedIndex;
            if (mode == 2)
            {
                var x = new double[16];
                for (int k = 0; k < 16; k++)
                    if (!TryParse(matBoxes[k], out x[k])) return;   // 任一非法整体放弃
                ApplyAndRefresh(x);
                return;
            }
            // 欧拉角/四元数模式：先读 6/7 个标量（失败则整体放弃）
            var vals = new double[mode == 0 ? 6 : 7];
            var boxes = mode == 0 ? eulerBoxes : quatBoxes;
            for (int k = 0; k < vals.Length; k++)
                if (!TryParse(boxes[k], out vals[k])) return;
            double[] xf = mode == 0
                ? EulerToMatrix(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5])
                : QuatToMatrix(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5], vals[6]);
            ApplyAndRefresh(xf);
        }
        applyBtn.Click += (_, _) => ApplyMatrix();
        resetBtn.Click += (_, _) =>
        {
            double[] ident = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            ApplyAndRefresh(ident);
        };

        return root;
    }

    /// <summary>欧拉角(绕X/Y/Z, 度, ZYX 内旋) + 平移 → 4x4 列向量齐次矩阵(行主序 16 元素)。</summary>
    private static double[] EulerToMatrix(double rx, double ry, double rz, double tx, double ty, double tz)
    {
        double cr = Math.Cos(rx * Math.PI / 180), sr = Math.Sin(rx * Math.PI / 180);
        double cp = Math.Cos(ry * Math.PI / 180), sp = Math.Sin(ry * Math.PI / 180);
        double cy = Math.Cos(rz * Math.PI / 180), sy = Math.Sin(rz * Math.PI / 180);
        // R = Rz(rz) * Ry(ry) * Rx(rx)  （先绕 X，再 Y，最后 Z）
        double r00 = cy * cp, r01 = cy * sp * sr - sy * cr, r02 = cy * sp * cr + sy * sr;
        double r10 = sy * cp, r11 = sy * sp * sr + cy * cr, r12 = sy * sp * cr - cy * sr;
        double r20 = -sp,    r21 = cp * sr,                r22 = cp * cr;
        return new[]
        {
            r00, r01, r02, tx,
            r10, r11, r12, ty,
            r20, r21, r22, tz,
            0, 0, 0, 1,
        };
    }

    /// <summary>四元数 (x,y,z,w) 归一化 + 平移 → 4x4 列向量齐次矩阵(行主序 16 元素)。</summary>
    private static double[] QuatToMatrix(double qx, double qy, double qz, double qw, double tx, double ty, double tz)
    {
        double n = Math.Sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (n < 1e-12) { qx = 0; qy = 0; qz = 0; qw = 1; }
        else { qx /= n; qy /= n; qz /= n; qw /= n; }
        double r00 = 1 - 2 * (qy * qy + qz * qz), r01 = 2 * (qx * qy - qz * qw),   r02 = 2 * (qx * qz + qy * qw);
        double r10 = 2 * (qx * qy + qz * qw),     r11 = 1 - 2 * (qx * qx + qz * qz), r12 = 2 * (qy * qz - qx * qw);
        double r20 = 2 * (qx * qz - qy * qw),     r21 = 2 * (qy * qz + qx * qw),   r22 = 1 - 2 * (qx * qx + qy * qy);
        return new[]
        {
            r00, r01, r02, tx,
            r10, r11, r12, ty,
            r20, r21, r22, tz,
            0, 0, 0, 1,
        };
    }

    /// <summary>从 4x4 旋转矩阵(行主序)反解欧拉角(绕X/Y/Z, 度, ZYX 内旋)；万向锁时 rz=0。</summary>
    private static (double rx, double ry, double rz) MatrixToEulerDeg(double[] m)
    {
        // R = Rz(rz)Ry(ry)Rx(rx)：m[8]=r20=-sin(ry)
        double ry = Math.Asin(Math.Clamp(-m[8], -1, 1));
        double rx, rz;
        if (Math.Abs(Math.Cos(ry)) > 1e-6)
        {
            rx = Math.Atan2(m[9], m[10]);   // r21, r22
            rz = Math.Atan2(m[4], m[0]);    // r10, r00
        }
        else
        {
            rx = Math.Atan2(-m[1], m[5]);   // 万向锁：用 r01/r11
            rz = 0;
        }
        return (rx * 180 / Math.PI, ry * 180 / Math.PI, rz * 180 / Math.PI);
    }

    /// <summary>从 4x4 旋转矩阵(行主序)反解四元数 (x,y,z,w)，标准 trace 分支法。</summary>
    private static (double x, double y, double z, double w) MatrixToQuat(double[] m)
    {
        double tr = m[0] + m[5] + m[10];
        double x, y, z, w;
        if (tr > 0)
        {
            double s = Math.Sqrt(tr + 1.0) * 2;
            w = 0.25 * s;
            x = (m[6] - m[9]) / s;   // r12 - r21
            y = (m[8] - m[2]) / s;   // r20 - r02
            z = (m[1] - m[4]) / s;   // r01 - r10
        }
        else if (m[0] > m[5] && m[0] > m[10])
        {
            double s = Math.Sqrt(1.0 + m[0] - m[5] - m[10]) * 2;
            w = (m[6] - m[9]) / s; x = 0.25 * s;
            y = (m[1] + m[4]) / s; z = (m[2] + m[8]) / s;
        }
        else if (m[5] > m[10])
        {
            double s = Math.Sqrt(1.0 + m[5] - m[0] - m[10]) * 2;
            w = (m[8] - m[2]) / s; x = (m[1] + m[4]) / s;
            y = 0.25 * s;          z = (m[6] + m[9]) / s;
        }
        else
        {
            double s = Math.Sqrt(1.0 + m[10] - m[0] - m[5]) * 2;
            w = (m[1] - m[4]) / s; x = (m[2] + m[8]) / s;
            y = (m[6] + m[9]) / s; z = 0.25 * s;
        }
        return (x, y, z, w);
    }
}
