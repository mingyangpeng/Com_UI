param(
    [int]$x1 = 0, [int]$y1 = 0,
    [int]$hwnd = 0
)

# 前台激活 + 双击：SetForegroundWindow 受前台锁限制时用 ALT 键技巧解锁，
# 激活后校验前台句柄再发双击（CUA/UIA 交互后窗口常失前台，裸 SendInput 会被吞）。
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
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
    public const uint MOVE = 0x0001, ABSOLUTE = 0x8000, LEFTDOWN = 0x0002, LEFTUP = 0x0004,
                      KEYDOWN = 0x0000, KEYUP = 0x0002;

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
    public static void Send(params INPUTU[] inputs) { SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUTU))); }
}
"@

$target = [IntPtr]$hwnd
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

# 同进程两次快速点击 = 系统级双击
[MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
Start-Sleep -Milliseconds 90
[MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN)); Start-Sleep -Milliseconds 40; [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
Start-Sleep -Milliseconds 200
[MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN)); Start-Sleep -Milliseconds 40; [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
Start-Sleep -Milliseconds 60
Write-Output "dblclick done at ($x1,$y1)"
