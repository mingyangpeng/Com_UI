using System;
using System.IO;
using System.Text.Json;
using System.Text.Json.Nodes;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace ComUI.Sdk.Ui;

/// <summary>
/// 文件/实体锁定徽标（形状绘制简笔锁——emoji 是彩色字体无法用 Foreground 着色）：
/// <b>解锁 = 绿色开锁</b>（#7BC886），<b>锁定 = 灰色闭锁</b>（#A0A0A8）。
/// 放置规则：行布局等宽拉伸，动作（含锁）统一钉在行右端对齐一列。
/// </summary>
public static class LockGlyph
{
    /// <summary>构建锁图标（12×13 Canvas）。</summary>
    public static Control Build(bool locked)
    {
        var brush = locked ? LockedBrush : UnlockedBrush;
        var body = new Border
        {
            Width = 8, Height = 6, CornerRadius = new CornerRadius(1.5),
            Background = brush,
        };
        var shackle = new Border
        {
            Width = 6, Height = 6, CornerRadius = new CornerRadius(3, 3, 0, 0),
            BorderThickness = new Thickness(1.4, 1.4, 1.4, 0),
            BorderBrush = brush,
            Background = Brushes.Transparent,
        };
        var canvas = new Canvas { Width = 12, Height = 13 };
        canvas.Children.Add(body);
        canvas.Children.Add(shackle);
        Canvas.SetLeft(body, 2);
        Canvas.SetTop(body, 7);
        if (locked)
        {
            Canvas.SetLeft(shackle, 3);   // 闭：锁环正落于锁体上
            Canvas.SetTop(shackle, 2);
        }
        else
        {
            Canvas.SetLeft(shackle, 5);   // 开：锁环抬起右移
            Canvas.SetTop(shackle, 0);
        }
        return canvas;
    }

    public static readonly IBrush LockedBrush = new SolidColorBrush(Color.FromRgb(0xA0, 0xA0, 0xA8));
    public static readonly IBrush UnlockedBrush = new SolidColorBrush(Color.FromRgb(0x7B, 0xC8, 0x86));
}

/// <summary>文件锁定状态的持久化约定：算法等文件型 JSON 顶层 <c>"Locked": true/false</c> 字段（缺省 = 未锁定）。
/// 锁定语义 = 以只读打开（禁改内容），解锁或复制副本后可编辑。</summary>
public static class FileLock
{
    /// <summary>读取 Locked 字段（文件不存在/解析失败 = 未锁定）。</summary>
    public static bool IsLocked(string path)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            return doc.RootElement.TryGetProperty("Locked", out var el) && el.GetBoolean();
        }
        catch { return false; }
    }

    /// <summary>写入 Locked 字段（原地改写，其余内容不动）。</summary>
    public static void Set(string path, bool locked)
    {
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(path));
            if (node is null) return;
            node["Locked"] = locked;
            File.WriteAllText(path, node.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        }
        catch { }
    }

    /// <summary>切换锁定状态并返回切换后的值。</summary>
    public static bool Toggle(string path)
    {
        var now = !IsLocked(path);
        Set(path, now);
        return now;
    }
}
