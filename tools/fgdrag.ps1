param(
    [int]$x1 = 0, [int]$y1 = 0,
    [int]$x2 = 0, [int]$y2 = 0,
    [int]$hwnd = 0,
    [int]$steps = 20,
    [int]$wheelN = 0,   # >0 = 滚轮模式：在 (x1,y1) 连滚 $wheelN 格，每格增量 $x2（120=前滚/放大）。注：注入已可用，但对 Avalonia 的最终送达受前台抢占影响——E2E 缩放优先用「＋/－」按钮
    [switch]$topmost   # 置顶拖拽：不抢前台（前台锁常失败/打扰用户），临时 HWND_TOPMOST 让鼠标命中目标窗口
)

# 前台激活 + 左键拖拽：SetForegroundWindow 受前台锁限制时用 ALT 键技巧解锁，
# 激活后校验前台句柄再拖拽（裸 SendInput 会被吞到别的窗口——曾把拖拽发进无关应用）。
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class MouseSim {
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTU { [FieldOffset(0)] public uint type; [FieldOffset(8)] public MOUSEINPUT mi; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUTU[] inputs, int size);
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int nIndex);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public const uint MOVE = 0x0001, ABSOLUTE = 0x8000, LEFTDOWN = 0x0002, LEFTUP = 0x0004,
                      WHEELFLAG = 0x0800, KEYDOWN = 0x0000, KEYUP = 0x0002;
    public static INPUTU Wheel(int delta) { INPUTU i = new INPUTU(); i.type = 0; i.mi.dwFlags = WHEELFLAG; i.mi.mouseData = (uint)delta; return i; }

    public static INPUTU AbsMove(int x, int y) {
        int w = GetSystemMetrics(0), h = GetSystemMetrics(1);
        INPUTU i = new INPUTU(); i.type = 0;
        i.mi.dx = (int)(x * 65535.0 / (w - 1)); i.mi.dy = (int)(y * 65535.0 / (h - 1));
        i.mi.dwFlags = MOVE | ABSOLUTE;
        return i;
    }
    public static INPUTU Btn(uint flags) { INPUTU i = new INPUTU(); i.type = 0; i.mi.dwFlags = flags; return i; }
    public static INPUTU Key(ushort vk, uint flags) {
        INPUTU i = new INPUTU(); i.type = 1; i.ki.wVk = vk; i.ki.dwFlags = flags; return i;
    }
    public static uint Send(params INPUTU[] inputs) { return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUTU))); }
}
"@

$target = [IntPtr]$hwnd
if ($topmost) {
    # 置顶模式：鼠标按下落在光标下的窗口（首次按下顺带激活），不依赖前台锁
    [MouseSim]::SetWindowPos($target, [IntPtr](-1), 0, 0, 0, 0, 0x0003) | Out-Null   # HWND_TOPMOST, SWP_NOSIZE|SWP_NOMOVE
    Start-Sleep -Milliseconds 200
} else {
    # ALT 击键满足前台锁条件，再激活
    [MouseSim]::Send([MouseSim]::Key(0x12, [MouseSim]::KEYDOWN))
    Start-Sleep -Milliseconds 30
    [MouseSim]::Send([MouseSim]::Key(0x12, [MouseSim]::KEYUP))
    Start-Sleep -Milliseconds 50
    [MouseSim]::SetForegroundWindow($target) | Out-Null
    Start-Sleep -Milliseconds 150
    $fg = [MouseSim]::GetForegroundWindow()
    if ($fg -ne $target) {
        # 兜底：最小化再还原强制前台
        [MouseSim]::ShowWindow($target, 6) | Out-Null   # SW_MINIMIZE
        Start-Sleep -Milliseconds 120
        [MouseSim]::ShowWindow($target, 9) | Out-Null   # SW_RESTORE
        Start-Sleep -Milliseconds 250
        $fg = [MouseSim]::GetForegroundWindow()
    }
    Write-Output "foreground: 0x$($fg.ToString('X')) target: 0x$($target.ToString('X')) match: $($fg -eq $target)"
    if ($fg -ne $target) { Write-Output "ABORT: not foreground"; exit 1 }
}

if ($wheelN -gt 0) {
    # 滚轮模式：先轻点一下聚焦目标窗口（滚轮路由跟焦点走），再连滚 N 格
    [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1)) | Out-Null
    Start-Sleep -Milliseconds 120
    [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN)) | Out-Null
    Start-Sleep -Milliseconds 40
    [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP)) | Out-Null
    Start-Sleep -Milliseconds 150
    $rets = @()
    for ($k = 1; $k -le $wheelN; $k++) {
        $rets += [MouseSim]::Send([MouseSim]::Wheel($x2))
        Start-Sleep -Milliseconds 60
    }
    Write-Output ("sendinput returns: " + (($rets | Select-Object -Unique) -join ','))
    if ($topmost) { [MouseSim]::SetWindowPos($target, [IntPtr](-2), 0, 0, 0, 0, 0x0003) | Out-Null }
    Write-Output "wheel x$wheelN done at ($x1,$y1) delta=$x2"
    exit 0
}

# 拖拽（前台已校验）：按下 → 分步移动 → 释放，校验 SendInput 返回值
$m = [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
Start-Sleep -Milliseconds 120
$d = [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN))
Start-Sleep -Milliseconds 80
for ($k = 1; $k -le $steps; $k++) {
    $mx = [int]($x1 + ($x2 - $x1) * $k / $steps)
    $my = [int]($y1 + ($y2 - $y1) * $k / $steps)
    [MouseSim]::Send([MouseSim]::AbsMove($mx, $my)) | Out-Null
    Start-Sleep -Milliseconds 20
}
Start-Sleep -Milliseconds 80
$u = [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
if ($topmost) {
    [MouseSim]::SetWindowPos($target, [IntPtr](-2), 0, 0, 0, 0, 0x0003) | Out-Null   # HWND_NOTOPMOST 还原
}
if (($d -band 1) -ne 1 -or ($u -band 1) -ne 1) {
    Write-Output "WARN: SendInput rejected (down=$d up=$u) — 拖拽可能未送达，建议重试"
} else {
    Write-Output "drag done ($x1,$y1)->($x2,$y2)"
}
