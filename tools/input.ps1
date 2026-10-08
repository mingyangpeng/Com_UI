param(
    [string]$mode = "rightdrag",   # rightdrag | leftdrag | ctrldrag | ctrlclick | click | key
    [int]$x1 = 0, [int]$y1 = 0, [int]$x2 = 0, [int]$y2 = 0,
    [string]$keys = "",            # key 模式：如 Escape
    [int]$steps = 20,
    [int]$hwnd = 12454366
)

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
    public const uint MOVE = 0x0001, ABSOLUTE = 0x8000, LEFTDOWN = 0x0002, LEFTUP = 0x0004,
                      RIGHTDOWN = 0x0008, RIGHTUP = 0x0010, KEYDOWN = 0x0000, KEYUP = 0x0002;
    public const uint WHEELFLAG = 0x0800;
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
    public static INPUTU UniKey(int ch, bool up) {
        INPUTU i = new INPUTU(); i.type = 1; i.ki.wScan = (ushort)ch; i.ki.dwFlags = 0x0004u | (up ? 0x0002u : 0); return i;
    }
    public static void Send(params INPUTU[] inputs) { SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUTU))); }
}
"@

[MouseSim]::SetForegroundWindow([IntPtr]$hwnd) | Out-Null
Write-Output "MARKER-V2 wheel-exists=$([MouseSim].GetMethod('Wheel') -ne $null)"
Start-Sleep -Milliseconds 250

function Move-Steps([int]$ax1, [int]$ay1, [int]$ax2, [int]$ay2) {
    [MouseSim]::Send([MouseSim]::AbsMove($ax1, $ay1))
    Start-Sleep -Milliseconds 120
    for ($k = 1; $k -le $steps; $k++) {
        $mx = [int]($ax1 + ($ax2 - $ax1) * $k / $steps)
        $my = [int]($ay1 + ($ay2 - $ay1) * $k / $steps)
        [MouseSim]::Send([MouseSim]::AbsMove($mx, $my))
        Start-Sleep -Milliseconds 20
    }
}

switch ($mode) {
    "rightdrag" {
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 120
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::RIGHTDOWN))
        Start-Sleep -Milliseconds 80
        Move-Steps $x1 $y1 $x2 $y2
        Start-Sleep -Milliseconds 80
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::RIGHTUP))
    }
    "leftdrag" {
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 120
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN))
        Start-Sleep -Milliseconds 80
        Move-Steps $x1 $y1 $x2 $y2
        Start-Sleep -Milliseconds 80
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
    }
    "ctrldrag" {
        [MouseSim]::Send([MouseSim]::Key(0x11, [MouseSim]::KEYDOWN))
        Start-Sleep -Milliseconds 80
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 100
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN))
        Start-Sleep -Milliseconds 80
        Move-Steps $x1 $y1 $x2 $y2
        Start-Sleep -Milliseconds 80
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
        Start-Sleep -Milliseconds 60
        [MouseSim]::Send([MouseSim]::Key(0x11, [MouseSim]::KEYUP))
    }
    "ctrlclick" {
        [MouseSim]::Send([MouseSim]::Key(0x11, [MouseSim]::KEYDOWN))
        Start-Sleep -Milliseconds 80
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 120
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN))
        Start-Sleep -Milliseconds 60
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
        Start-Sleep -Milliseconds 60
        [MouseSim]::Send([MouseSim]::Key(0x11, [MouseSim]::KEYUP))
    }
    "wheel" {
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 120
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN))
        Start-Sleep -Milliseconds 40
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
        Start-Sleep -Milliseconds 150
        for ($k = 1; $k -le $steps; $k++) {
            $r = [MouseSim]::Send([MouseSim]::Wheel($x2))
            Write-Output "wheel[$k] sendinput=$r"
            Start-Sleep -Milliseconds 60
        }
    }
    "click" {
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 100
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN))
        Start-Sleep -Milliseconds 50
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
    }
    "type" {
        foreach ($ch in $keys.ToCharArray()) {
            [MouseSim]::Send([MouseSim]::UniKey([int]$ch, $false))
            Start-Sleep -Milliseconds 25
            [MouseSim]::Send([MouseSim]::UniKey([int]$ch, $true))
            Start-Sleep -Milliseconds 35
        }
    }
    "chord" {
        # chord 模式：keys=修饰键vk,主键vk（如 "17,65" = Ctrl+A）
        $parts = $keys.Split(',')
        [MouseSim]::Send([MouseSim]::Key([uint16]$parts[0], [MouseSim]::KEYDOWN))
        Start-Sleep -Milliseconds 30
        [MouseSim]::Send([MouseSim]::Key([uint16]$parts[1], [MouseSim]::KEYDOWN))
        Start-Sleep -Milliseconds 30
        [MouseSim]::Send([MouseSim]::Key([uint16]$parts[1], [MouseSim]::KEYUP))
        [MouseSim]::Send([MouseSim]::Key([uint16]$parts[0], [MouseSim]::KEYUP))
        Start-Sleep -Milliseconds 45
    }
    "dblclick" {
        # 同进程两次快速点击 = 系统级双击（间隔 200ms < 500ms 双击时限）
        [MouseSim]::Send([MouseSim]::AbsMove($x1, $y1))
        Start-Sleep -Milliseconds 90
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN)); Start-Sleep -Milliseconds 40; [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
        Start-Sleep -Milliseconds 200
        [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTDOWN)); Start-Sleep -Milliseconds 40; [MouseSim]::Send([MouseSim]::Btn([MouseSim]::LEFTUP))
        Start-Sleep -Milliseconds 60
    }
    "key" {
        # 单个虚拟键码（十进制）：Escape=27
        [MouseSim]::Send([MouseSim]::Key([uint16]$keys, [MouseSim]::KEYDOWN))
        Start-Sleep -Milliseconds 50
        [MouseSim]::Send([MouseSim]::Key([uint16]$keys, [MouseSim]::KEYUP))
    }
}
Write-Output "$mode done ($x1,$y1)->($x2,$y2) keys=$keys"
