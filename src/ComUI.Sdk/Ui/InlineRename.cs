using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace ComUI.Sdk.Ui;

/// <summary>
/// 行内改名统一组件与交互规则（全 UI 唯一实现，禁止各处重写——BUG-059 教训）：
/// 双击进入（文本全选）→ <b>Enter / 失焦 = 确认</b>，<b>Esc = 取消</b>（各最多回调一次）；
/// 空文本或与原名相同 = 调用方还原显示。确认/取消后输入框由调用方移出可视树。
/// </summary>
public static class InlineRename
{
    /// <summary>双击判定阈值（毫秒）。单击动作若会异步抢焦点（如打开标签页），须延迟 300ms 派发，
    /// 否则第二击触发的改名框会被焦点变化瞬间提交掉。</summary>
    public const double DoubleClickMs = 450;

    /// <summary>单击动作延迟派发时长（毫秒），等待可能到来的第二击。</summary>
    public const int SingleClickDelayMs = 300;

    /// <summary>创建标准改名输入框（样式与行为全 UI 统一）。
    /// commit 收到去除首尾空白后的文本（可能为空，调用方自行判断还原）；cancel 表示放弃修改。</summary>
    public static TextBox Begin(string initialText, Action<string> commit, Action cancel)
    {
        var done = false;   // Enter/Esc 处理后控件移出可视树仍会触发一次 LostFocus，必须挡住（否则 Esc 变成误提交）
        void Once(Action f)
        {
            if (done) return;
            done = true;
            f();
        }

        var box = new TextBox
        {
            Text = initialText,
            FontSize = 11.5,
            MinWidth = 120,
            MinHeight = 20,
            Padding = new Thickness(4, 0, 4, 0),
            Background = new SolidColorBrush(Color.FromRgb(0x25, 0x25, 0x28)),
        };
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                e.Handled = true;
                Once(() => commit(box.Text?.Trim() ?? ""));
            }
            else if (e.Key == Key.Escape)
            {
                e.Handled = true;
                Once(cancel);
            }
        };
        box.LostFocus += (_, _) => Once(() => commit(box.Text?.Trim() ?? ""));
        box.AttachedToVisualTree += (_, _) => { box.Focus(); box.SelectAll(); };
        return box;
    }
}

/// <summary>双击判定（时间戳法，兼容 UIA/自动化注入的 Click 事件——它们不产生系统级双击标志）。
/// 每个可双击目标（每个节点卡片/每一行）持有独立实例，避免跨目标误判。</summary>
public sealed class DoubleClickDetector
{
    private DateTime _last = DateTime.MinValue;

    /// <summary>本次事件距上次是否构成双击；是则复位（连续三击视为两组双击）。</summary>
    public bool IsDouble()
    {
        var now = DateTime.Now;
        var dbl = (now - _last).TotalMilliseconds < InlineRename.DoubleClickMs;
        _last = dbl ? DateTime.MinValue : now;
        return dbl;
    }
}
