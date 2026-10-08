using Avalonia;
using ComUI.Core;
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;

namespace ComUI.Host;

internal static class Program
{
    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    private static Mutex? _singleInstanceMutex;

    [STAThread]
    public static void Main(string[] args)
    {
        // 单实例：二次启动时把已有窗口调到前台，避免"两个一模一样的窗口
        // 操作了一个、看的却是另一个"的混乱（调试期频繁重启尤其容易踩）
        _singleInstanceMutex = new Mutex(true, "ComUI.Host.SingleInstance", out var isNew);
        if (!isNew)
        {
            ActivateExistingWindow();
            return;
        }

        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);   // 主窗 Closing 时显式关浮动窗 → 无窗口 → 应用退出
        _singleInstanceMutex.ReleaseMutex();
    }

    private static void ActivateExistingWindow()
    {
        try
        {
            var me = Environment.ProcessId;
            foreach (var p in Process.GetProcessesByName("ComUI.Host"))
            {
                if (p.Id == me) continue;
                if (p.MainWindowHandle == IntPtr.Zero) continue;
                ShowWindow(p.MainWindowHandle, 9);   // SW_RESTORE（最小化时先还原）
                SetForegroundWindow(p.MainWindowHandle);
                break;
            }
        }
        catch { /* 激活失败就静默退出 */ }
    }

    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>()
        .UsePlatformDetect()
        .LogToTrace();
}
