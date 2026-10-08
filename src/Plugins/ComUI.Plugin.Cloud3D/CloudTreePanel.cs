using System;
using System.Globalization;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
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
    private static readonly Brush SectionBg = new SolidColorBrush(Color.FromRgb(0x25, 0x25, 0x28));

    private readonly Cloud3DView _view;
    private readonly IBus? _bus;
    private readonly StackPanel _imageList = new() { Margin = new Thickness(2, 1) };
    private readonly TextBlock _imageHeaderText = new()
    {
        Text = "2D 图像", FontWeight = FontWeight.SemiBold, FontSize = 12.5,
        VerticalAlignment = VerticalAlignment.Center,
    };
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
    private string? _imgRenaming;   // 正在行内改名的图像
    private string? _imgClipId;     // 图像剪贴板
    private readonly StackPanel _list = new() { Margin = new Thickness(4, 2) };
    private readonly StackPanel _detailHost = new() { Spacing = 1 };
    private readonly TextBlock _detailTitle = new()
    {
        Text = "点云属性", FontWeight = FontWeight.SemiBold, FontSize = 12,
        Margin = new Thickness(0, 0, 0, 2),
    };
    private readonly TextBlock _detail = new()
    {
        FontSize = 11.5, Foreground = Dim, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 3, 0, 0),
    };
    private string? _selected;
    private string? _renaming;      // 正在双击改名的实体
    private string? _clipId;
    private string? _styleBuiltFor;
    private bool _syncing;
    private bool _deselected;       // 用户 Esc 主动取消选中（暂停自动选中）

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
        var cloudHeaderText = new TextBlock
        {
            Text = "3D 点云", FontWeight = FontWeight.SemiBold, FontSize = 12.5,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var cloudHeader = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(8, 8, 8, 2) };
        cloudHeader.Children.Add(cloudHeaderText);
        _cloudEyeBtn.SetValue(Grid.ColumnProperty, 1);
        cloudHeader.Children.Add(_cloudEyeBtn);
        UpdateCloudEyeGlyph();
        var scroll = new ScrollViewer
        {
            Content = new StackPanel { Children = { BuildImageHeaderRow(), _imageList, cloudHeader, _list } },
        };

        // "点云属性/图像属性"分段：独立底色 + 标题，常驻可见（无选中时显示提示）
        var detailSection = new Border
        {
            Background = SectionBg,
            CornerRadius = new CornerRadius(4),
            Padding = new Thickness(8, 5, 8, 7),
            Margin = new Thickness(4, 3, 4, 4),
            Child = new StackPanel { Children = { _detailTitle, _detailHost } },
        };

        var root = new Grid { RowDefinitions = new RowDefinitions("*,Auto") };
        scroll.SetValue(Grid.RowProperty, 0);
        root.Children.Add(scroll);
        detailSection.SetValue(Grid.RowProperty, 1);
        root.Children.Add(detailSection);
        Content = root;

        view.CloudsChanged += Refresh;
        AttachedToVisualTree += (_, _) => AttachBus();
        DetachedFromVisualTree += (_, _) => { _imgSub?.Dispose(); _imgSub = null; };
        KeyDown += (_, e) =>
        {
            switch (e.Key)
            {
                case Key.C when e.KeyModifiers.HasFlag(KeyModifiers.Control):
                    if (_imgSelected is { }) _imgClipId = _imgSelected; else CopySel();
                    RefreshDetail(); e.Handled = true; break;
                case Key.V when e.KeyModifiers.HasFlag(KeyModifiers.Control):
                    if (_imgClipId is { } && _imgEntries.Any(x => x.Id == _imgClipId)) PasteImage(); else Paste();
                    e.Handled = true; break;
                case Key.Delete or Key.Back when _imgSelected is { }:
                    RemoveImage(_imgSelected); e.Handled = true; break;
                case Key.Delete or Key.Back when !string.IsNullOrEmpty(_selected):
                    RemoveSel(); e.Handled = true; break;
                case Key.Escape:
                    _selected = null; _imgSelected = null; _deselected = true; Refresh(); RebuildImageRows(); RefreshDetail(); e.Handled = true; break;
            }
        };
        Refresh();
        RebuildImageRows();
    }

    // ==================== 2D 图像栏 ====================

    /// <summary>「2D 图像」标题行：文字 + 总睁眼/闭眼开关（控制下方图像显示与否）。</summary>
    private Control BuildImageHeaderRow()
    {
        _eyeBtn.PointerPressed += (_, e) =>
        {
            ToggleMasterEye();
            e.Handled = true;
        };
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(8, 6, 8, 2) };
        grid.Children.Add(_imageHeaderText);
        _eyeBtn.SetValue(Grid.ColumnProperty, 1);
        grid.Children.Add(_eyeBtn);
        UpdateEyeGlyph();
        return grid;
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
        if (_imgEntries.Count == 0)
        {
            _imageList.Children.Add(new TextBlock
            {
                Text = "（无图像——运行流水线或从预览卡导入）",
                FontSize = 11.5, Foreground = Dim, Margin = new Thickness(14, 3, 0, 3),
            });
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

        bool selected = _imgSelected == entry.Id;
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
            _imgSelected = entry.Id;
            _selected = null;          // 图像/点云选中互斥
            Focus();
            Refresh();                 // 云列表去掉旧选中高亮
            RefreshDetail();
            // 点击行名 = 查看该数据：显示该图 + 跳转 2D 工作台（绿点仍是显隐开关）
            if (entry.Pixels is not null) SendImageToWorkbench(entry);
            _bus?.Publish(BusTopics.PanelActivate, "ui.image2d.view");
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
        if (_imgSelected == id) _imgSelected = null;
        RebuildImageRows();
        RefreshDetail();
    }

    // ==================== 列表 ====================

    private void Refresh()
    {
        _list.Children.Clear();
        var infos = _view.GetCloudInfos();
        // 无选中（且非用户主动取消）时自动选第一个，属性区开箱即见
        if (!_deselected && _selected is null && _imgSelected is null && infos.Count > 0)
            _selected = infos[0].Id;
        if (_selected is not null && infos.All(i => i.Id != _selected))
            _selected = infos.Count > 0 && !_deselected ? infos[0].Id : null;
        foreach (var info in infos)
            _list.Children.Add(BuildRow(info));
        if (_list.Children.Count == 0)
            _list.Children.Add(new TextBlock
            {
                Text = "（无点云——运行流水线或等待总线发布）",
                FontSize = 12, Foreground = Dim, Margin = new Thickness(4, 6),
            });
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

        bool selected = _selected == info.Id;
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
            _selected = info.Id;
            _imgSelected = null;   // 图像/点云选中互斥
            _deselected = false;
            Focus();   // 接管键盘（Ctrl+C/V/Del）
            Refresh();
            RebuildImageRows();
            _bus?.Publish(BusTopics.PanelActivate, "ui.cloud3d.view");   // 点击行名 = 跳转 3D 工作台
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
        if (_view.RemoveCloud(_selected)) _selected = null;   // 锁定=拒绝（视图已提示），保留选中
        Refresh();
    }

    // ==================== 属性分段 ====================

    private void RefreshDetail()
    {
        // 图像选中 → 图像属性
        var imgEntry = _imgEntries.FirstOrDefault(x => x.Id == _imgSelected);
        if (imgEntry is not null)
        {
            _styleBuiltFor = null;
            _detailHost.Children.Clear();
            _detailTitle.Text = "图像属性";
            var clip2 = _imgClipId is null ? "" : $"\n剪贴板：{_imgClipId}（Ctrl+V 粘贴副本）";
            _detail.Text = $"{imgEntry.Id}\n尺寸 {imgEntry.W}×{imgEntry.H} · 已发布 {imgEntry.Frames} 帧" +
                           (imgEntry.Source is null ? "" : $"\n来源 {imgEntry.Source}") +
                           (imgEntry.Pixels is null ? "\n（大图未保留像素）" : (imgEntry.Displayed ? "\n正在 2D 工作台显示" : "")) + clip2;
            _detailHost.Children.Add(_detail);
            return;
        }

        var info = _view.GetCloudInfos().FirstOrDefault(c => c.Id == _selected);
        if (info.Id is null || info.Id.Length == 0)
        {
            _styleBuiltFor = null;
            _detailHost.Children.Clear();
            _detailTitle.Text = "点云属性";
            _detail.Text = "单击列表中的点云/图像，在此显示并编辑其属性";
            _detailHost.Children.Add(_detail);
            return;
        }
        _detailTitle.Text = "点云属性";

        // 选中实体变化才重建控件（避免清空后不重建的消失 bug）；同一实体只刷新文本
        if (_styleBuiltFor != info.Id)
            BuildStyleControls(info);

        Vector3 size = info.BoundsMax - info.BoundsMin;
        var clip = _clipId is null ? "" : $"\n剪贴板：{_clipId}（Ctrl+V 粘贴）";
        _detail.Text = $"{info.Id} · {info.Count:N0} 点（已上传 {info.Loaded:N0}）{(info.Visible ? "" : " · 已隐藏")}" +
                       (info.SelCount > 0 ? $"\n选区 {info.SelCount:N0} 点" : "") +
                       $"\n尺寸 {size.X:F2} × {size.Y:F2} × {size.Z:F2}" +
                       (info.Source is null ? "" : $"\n来源 {info.Source}") + clip;
        if (!_detailHost.Children.Contains(_detail))
            _detailHost.Children.Add(_detail);
    }

    /// <summary>构建选中实体的独立样式控件（颜色模式/单色RGB/点大小/变换矩阵）。</summary>
    private void BuildStyleControls(Cloud3DView.CloudInfo info)
    {
        _detailHost.Children.Clear();

        var modeCombo = new ComboBox { Classes = { "input" }, MinWidth = 92, FontSize = 11.5, Height = 24 };
        foreach (var name in new[] { "逐点色", "高度伪彩", "单色" }) modeCombo.Items.Add(name);
        _syncing = true;
        modeCombo.SelectedIndex = Math.Clamp(info.ColorMode, 0, 2);
        _syncing = false;
        modeCombo.SelectionChanged += (_, _) =>
        {
            if (_syncing || _selected is null) return;
            _view.SetCloudStyle(_selected, modeCombo.SelectedIndex, null);
        };

        var sizeBox = new TextBox
        {
            Classes = { "input" }, MinWidth = 52, FontSize = 11.5, Height = 24,
            Text = info.PointSize.ToString("0"),
            Background = Brushes.Transparent, BorderThickness = new Thickness(0),
            Padding = new Thickness(2, 0, 2, 0),
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

        _detailHost.Children.Add(row1);
        _detailHost.Children.Add(row2);

        // 单色模式：显示 RGB 三通道（每实体单色）
        if (info.ColorMode == 2)
        {
            var rgbGrid = new Grid { ColumnDefinitions = new ColumnDefinitions("58,*,*,*"), Margin = new Thickness(0, 1) };
            rgbGrid.Children.Add(new TextBlock { Text = "RGB", FontSize = 11.5, Foreground = Dim, VerticalAlignment = VerticalAlignment.Center });
            byte[] cur = { (byte)Math.Clamp(info.SingleColor.X * 255, 0, 255), (byte)Math.Clamp(info.SingleColor.Y * 255, 0, 255), (byte)Math.Clamp(info.SingleColor.Z * 255, 0, 255) };
            var rgbBoxes = new TextBox[3];
            void CommitRgb()
            {
                if (_selected is null) return;
                var nv = new Vector3(
                    byte.TryParse(rgbBoxes[0].Text, out var vr) ? vr : (byte)Math.Round(info.SingleColor.X * 255),
                    byte.TryParse(rgbBoxes[1].Text, out var vg) ? vg : (byte)Math.Round(info.SingleColor.Y * 255),
                    byte.TryParse(rgbBoxes[2].Text, out var vb) ? vb : (byte)Math.Round(info.SingleColor.Z * 255));
                _view.SetCloudStyle(_selected, null, null, new Vector3(nv.X / 255f, nv.Y / 255f, nv.Z / 255f));
            }
            for (int k = 0; k < 3; k++)
            {
                var tb = new TextBox { FontSize = 10, MinWidth = 20, Height = 20, Padding = new Thickness(2, 0), Text = cur[k].ToString(), HorizontalContentAlignment = HorizontalAlignment.Center };
                tb.KeyDown += (_, e) => { if (e.Key == Key.Enter) { CommitRgb(); e.Handled = true; } };
                tb.LostFocus += (_, _) => CommitRgb();
                rgbBoxes[k] = tb;
                tb.SetValue(Grid.ColumnProperty, 1 + k);
                rgbGrid.Children.Add(tb);
            }
            _detailHost.Children.Add(rgbGrid);
        }

        // 变换矩阵 4x4（行主序，末列平移）
        var matHead = new TextBlock { Text = "变换矩阵", FontSize = 11.5, Foreground = Dim, Margin = new Thickness(0, 4, 0, 1) };
        _detailHost.Children.Add(matHead);
        var matBoxes = new TextBox[16];
        var matGrid = new Grid { RowDefinitions = new RowDefinitions("22,22,22,22") };
        for (int r = 0; r < 4; r++)
        {
            var rowG = new Grid { ColumnDefinitions = new ColumnDefinitions("*,4,*,4,*,4,*") };
            for (int c = 0; c < 4; c++)
            {
                var tb = new TextBox
                {
                    FontSize = 9.5, Height = 20, Padding = new Thickness(2, 0),
                    Text = info.Xform[r * 4 + c].ToString("0.###"),
                    HorizontalContentAlignment = HorizontalAlignment.Center,
                    Background = Brushes.Transparent, BorderThickness = new Thickness(0, 0, 0, 1),
                };
                matBoxes[r * 4 + c] = tb;
                tb.SetValue(Grid.ColumnProperty, c * 2);   // 列 0,2,4,6（奇数列为 4px 间隔）
            }
            rowG.Children.Add(matBoxes[r * 4]);
            rowG.Children.Add(matBoxes[r * 4 + 1]);
            rowG.Children.Add(matBoxes[r * 4 + 2]);
            rowG.Children.Add(matBoxes[r * 4 + 3]);
            rowG.SetValue(Grid.RowProperty, r);
            matGrid.Children.Add(rowG);
        }
        var applyBtn = new Button { Classes = { "ghost" }, Content = "应用变换", MinWidth = 60, MinHeight = 20, FontSize = 11, Margin = new Thickness(0, 2) };
        var resetBtn = new Button { Classes = { "ghost" }, Content = "重置", MinWidth = 44, MinHeight = 20, FontSize = 11, Margin = new Thickness(6, 2, 0, 0) };
        var btnRow = new StackPanel { Orientation = Orientation.Horizontal, Children = { applyBtn, resetBtn } };
        _detailHost.Children.Add(matGrid);
        _detailHost.Children.Add(btnRow);
        _detailHost.Children.Add(_detail);
        _styleBuiltFor = info.Id;

        void ApplyMatrix()
        {
            if (_selected is null) return;
            var x = new double[16];
            for (int k = 0; k < 16; k++)
            {
                if (!double.TryParse(matBoxes[k].Text, NumberStyles.Float, CultureInfo.InvariantCulture, out var v)) return;   // 任一非法整体放弃
                x[k] = v;
            }
            _view.SetCloudXform(_selected, x);
        }
        applyBtn.Click += (_, _) => ApplyMatrix();
        resetBtn.Click += (_, _) =>
        {
            double[] ident = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            _view.SetCloudXform(_selected ?? "", ident);
        };
    }
}
