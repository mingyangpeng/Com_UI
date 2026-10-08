using Avalonia.Media;

namespace ComUI.Core;

public enum LogLevel { Debug, Info, Warn, Error }

public sealed class LogEntry
{
    public DateTime Time { get; }
    public LogLevel Level { get; }
    public string Message { get; }

    public LogEntry(DateTime time, LogLevel level, string message)
    {
        Time = time;
        Level = level;
        Message = message;
    }

    public string TimeString => Time.ToString("HH:mm:ss.fff");

    public string LevelTag => Level switch
    {
        LogLevel.Debug => "DBUG",
        LogLevel.Info => "INFO",
        LogLevel.Warn => "WARN",
        _ => "ERR ",
    };

    public Brush LevelBrush => Level switch
    {
        LogLevel.Debug => new SolidColorBrush(Color.FromRgb(0x9D, 0x9D, 0x9D)),
        LogLevel.Info => new SolidColorBrush(Color.FromRgb(0x56, 0x9C, 0xD6)),
        LogLevel.Warn => new SolidColorBrush(Color.FromRgb(0xE5, 0xC0, 0x7B)),
        _ => new SolidColorBrush(Color.FromRgb(0xF1, 0x4C, 0x4C)),
    };
}

/// <summary>宿主日志服务，插件通过 IPluginContext 写入。</summary>
public sealed class LogService
{
    private readonly object _gate = new();
    public event Action<LogEntry>? EntryAdded;

    public void Debug(string message) => Add(LogLevel.Debug, message);
    public void Info(string message) => Add(LogLevel.Info, message);
    public void Warn(string message) => Add(LogLevel.Warn, message);
    public void Error(string message) => Add(LogLevel.Error, message);

    private void Add(LogLevel level, string message)
    {
        lock (_gate)
        {
            EntryAdded?.Invoke(new LogEntry(DateTime.Now, level, message));
        }
    }
}
