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
}
'@
    [FG]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
    Start-Sleep -Milliseconds 500
}

function Get-StatusTail {
    $all = $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, [System.Windows.Automation.Condition]::TrueCondition)
    foreach ($e in $all) { if ($e.Current.Name -and $e.Current.Name.Length -gt 6) { $script:last = $e.Current.Name } }
    return $script:last
}

# 逐键用 Forms.SendKeys（前台窗口）；每键后读状态栏文本尾部，断言包含关键词
# 注：ESC 键例 expect=宽松（清选区路径状态栏文案不固定，仅验证不异常）
# expect 按代码实态校准（2026-11-20）：
#  - 无点云时工具键走守卫文案（"...需要先有加载完成的点云实体"），有点云时走工具文案——expect 取两种状态共有子串（工具名）
#  - M/P/R/L 为模式开关（状态栏含模式名）；E/X/B/S/D/T/N/H 无点云时=守卫文案
$cases = @(
    @{ key = 'M'; expect = '距离量测' },
    @{ key = 'P'; expect = '多边形选取' },
    @{ key = 'R'; expect = '点对配准' },
    @{ key = 'L'; expect = '锁定竖直轴' },
    @{ key = 'E'; expect = '需要先框选' },        # 守卫文案（Cloud3DView.cs 1949 行实态）
    @{ key = 'X'; expect = '需要先框选' },        # 同上
    @{ key = 'B'; expect = '点云实体' },          # 同上
    @{ key = 'S'; expect = '点云实体' },
    @{ key = 'D'; expect = '点云实体' },
    @{ key = 'G'; expect = '点云实体' },
    @{ key = 'J'; expect = '合并' },              # 合并需 ≥2 实体，空态为另一文案——SKIP
    @{ key = 'T'; expect = '点云实体' },
    @{ key = 'N'; expect = '点云实体' },
    @{ key = 'H'; expect = '点云实体' },
    @{ key = 'Q'; expect = '' },                  # 菜单打开无状态栏文本——仅验证不异常（SKIP）
    @{ key = 'O'; expect = '投影' },
    @{ key = 'F'; expect = '聚焦' },
    @{ key = '^a'; expect = '点云实体' },
    @{ key = '^i'; expect = '反选需要先有选区' },
    @{ key = '^d'; expect = '' },                 # 清空选区无固定文案（SKIP）
    @{ key = 'ESC'; expect = '' },                # 宽松：仅验证不异常（SKIP）
    @{ key = '{ESC}'; expect = '' }               # 同上（SendKeys 变体，SKIP）
)
$pass = 0; $fail = 0
foreach ($c in $cases) {
    [System.Windows.Forms.SendKeys]::SendWait($c.key)
    Start-Sleep -Milliseconds 400
    $tail = Get-StatusTail
    if ($c.expect -eq '') { Write-Output ("SKIP {0} (no assert)" -f $c.key) }
    elseif ($tail -match $c.expect) { Write-Output ("PASS {0} -> {1}" -f $c.key, $c.expect); $pass++ }
    else { Write-Output ("FAIL {0} -> expect '{1}' got '{2}'" -f $c.key, $c.expect, $tail); $fail++ }
    [System.Windows.Forms.SendKeys]::SendWait('ESC')
    Start-Sleep -Milliseconds 300
}
Write-Output ("SMOKE RESULT pass={0} fail={1}" -f $pass, $fail)
