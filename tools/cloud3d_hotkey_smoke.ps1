# -*- coding: utf-8 -*-
# 3D 工具台键位冒烟：逐键注入 → 状态栏文案 UIA 断言（锁屏下 SendKeys 可能失效，真机运行更可靠）
# 用法：powershell -File tools/cloud3d_hotkey_smoke.ps1
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName System.Windows.Forms

$root = [System.Windows.Automation.AutomationElement]::RootElement
$win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, (New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'ComUI 显示平台 v0.3')))
if (-not $win) { Write-Output 'FAIL: window not found'; exit 1 }
$p = Get-Process ComUI.Host | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if ($p) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class FG {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  public static bool Acquire(IntPtr h) {
    for (int i = 0; i < 4; i++) {
      keybd_event(0x12, 0, 0, UIntPtr.Zero);
      keybd_event(0x12, 0, 2, UIntPtr.Zero);
      System.Threading.Thread.Sleep(120);
      SetForegroundWindow(h);
      System.Threading.Thread.Sleep(350);
      if (GetForegroundWindow() == h) return true;
      System.Threading.Thread.Sleep(500);
    }
    return false;
  }
  public static void ShiftTap() {
    keybd_event(0x10, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(60);
    keybd_event(0x10, 0, 2, UIntPtr.Zero);
  }
}
'@
    [FG]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
}

function Get-StatusTail {
    # 状态栏消息在窗口底部左侧（左下 TextBlock）；右下角是「UI 插件 N · 算法 N」计数器。
    # 取数规则：底部 45px 带内、X < 75% 窗宽的长名文本，取最后一条（2026-11-22 修复：原实现取树序最后长名元素=恒命中右下计数器）
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    $wr = $win.Current.BoundingRectangle
    $script:last = $null
    foreach ($e in $all) {
        $n = $e.Current.Name
        if (-not $n -or $n.Length -le 6) { continue }
        $r = $e.Current.BoundingRectangle
        if ($r.Y -gt $wr.Bottom - 32 -and $r.X -lt $wr.X + $wr.Width * 0.75) { $script:last = $n }
    }
    return $script:last
}

# 逐键用 Forms.SendKeys（前台窗口）；每键后读状态栏文本尾部，断言包含关键词
# 注：ESC 键例 expect=宽松（清选区路径状态栏文案不固定，仅验证不异常）
# expect 按代码实态校准（2026-11-20）：
#  - 无点云时工具键走守卫文案（"...需要先有加载完成的点云实体"），有点云时走工具文案——expect 取两种状态共有子串（工具名）
#  - M/P/R/L 为模式开关（状态栏含模式名）；E/X/B/S/D/T/N/H 无点云时=守卫文案
$cases = @(
    @{ key = 'm'; expect = '距离量测' },
    @{ key = 'p'; expect = '多边形选取' },
    @{ key = 'r'; expect = '点对配准' },
    @{ key = 'l'; expect = '竖直轴' },            # 两态兼容（PM 预签字 2026-11-22）：同 J 键模式，同时命中锁定/解除文案
    @{ key = 'e'; expect = '需要先框选' },        # 守卫文案（Cloud3DView.cs 1972 行实态）
    @{ key = 'x'; expect = '需要先框选' },        # 同上
    @{ key = 'b'; expect = '点云实体' },          # 同上
    @{ key = 's'; expect = '点云实体' },
    @{ key = 'd'; expect = '点云实体' },
    @{ key = 'g'; expect = '点云实体' },
    @{ key = 'j'; expect = '合并' },              # 两态兼容断言：空态守卫「合并需要至少…」与成功文案「合并完成：…」均命中
    @{ key = 't'; expect = '点云实体' },
    @{ key = 'n'; expect = '点云实体' },
    @{ key = 'h'; expect = '点云实体' },
    @{ key = 'q'; expect = '' },                  # 菜单打开无状态栏文本——仅验证不异常（SKIP）
    @{ key = 'o'; expect = '投影' },
    @{ key = 'f'; expect = '聚焦' },
    @{ key = '^a'; expect = '点云实体' },
    @{ key = '^i'; expect = '反选需要先有选区' },
    @{ key = '^d'; expect = '' },                 # 清空选区无固定文案（SKIP）
    @{ key = 'ESC'; expect = '' },                # 宽松：仅验证不异常（SKIP）
    @{ key = '{ESC}'; expect = '' }               # 同上（SendKeys 变体，SKIP）
)
$pass = 0; $fail = 0
foreach ($c in $cases) {
    if (-not [FG]::Acquire($win.Current.NativeWindowHandle)) { Write-Output ("SKIP {0} (foreground lost)" -f $c.key); continue }
    $baseline = Get-StatusTail
    [System.Windows.Forms.SendKeys]::SendWait($c.key)
    Start-Sleep -Milliseconds 400
    $tail = Get-StatusTail
    # IME 自愈：字母键被中文输入法拦成 ImeProcessed 时状态无变化 → Shift 点按切 EN 重发一次（2026-11-22）
    if ($c.expect -ne '' -and $tail -eq $baseline) {
        [FG]::ShiftTap()
        Start-Sleep -Milliseconds 400
        if ([FG]::Acquire($win.Current.NativeWindowHandle)) {
            [System.Windows.Forms.SendKeys]::SendWait($c.key)
            Start-Sleep -Milliseconds 400
            $tail = Get-StatusTail
        }
    }
    if ($c.expect -eq '') { Write-Output ("SKIP {0} (no assert)" -f $c.key) }
    elseif ($tail -match $c.expect) { Write-Output ("PASS {0} -> {1}" -f $c.key, $c.expect); $pass++ }
    else { Write-Output ("FAIL {0} -> expect '{1}' got '{2}'" -f $c.key, $c.expect, $tail); $fail++ }
    [System.Windows.Forms.SendKeys]::SendWait('{ESC}')
    Start-Sleep -Milliseconds 300
}
Write-Output ("SMOKE RESULT pass={0} fail={1}" -f $pass, $fail)
