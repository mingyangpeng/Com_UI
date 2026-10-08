using System;
using ComUI.Core;
using System.IO;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using ComUI.Host.Shell;
namespace ComUI.Host;

public partial class App : Application
{
    public const string HostVersion = ComUI.Core.HostConfig.HostVersion;   // 单一事实来源在 Core

    /// <summary>UI 日志服务（MainWindow 构造时注入）：全局异常除写 error.log 外同步送日志面板。</summary>
    public static LogService? UiLog { get; set; }

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        Dispatcher.UIThread.UnhandledException += (_, e) =>
        {
            WriteErrorLog(e.Exception);
            e.Handled = true;
        };
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex)
                WriteErrorLog(ex);
        };
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            WriteErrorLog(e.Exception);
            e.SetObserved();
        };

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = new MainWindow();
        }

        base.OnFrameworkInitializationCompleted();
    }

    private static void WriteErrorLog(Exception ex)
    {
        try { UiLog?.Error($"未处理异常: {ex.GetType().Name} — {ex.Message}（详见 output/error.log）"); }
        catch { /* 日志面板失败不影响兜底 */ }
        try
        {
            var path = AppPaths.ErrorLogFile;
            File.AppendAllText(path, $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {ex}\r\n\r\n");
        }
        catch { /* 日志失败时忽略 */ }
    }
}
