using System.Globalization;
using ComUI.Sdk;

namespace ComUI.Core;

/// <summary>一次算法执行的运行上下文（宿主实现）。</summary>
public sealed class HostAlgoRunContext : IAlgoRunContext
{
    private readonly LogService _log;
    private readonly Action<string>? _setStatus;

    public HostAlgoRunContext(string title, Dictionary<string, object?> parameters,
        LogService log, Action<string>? setStatus)
    {
        Title = title;
        Params = parameters;
        _log = log;
        _setStatus = setStatus;
    }

    public string Title { get; }
    public Dictionary<string, object?> Params { get; }

    /// <summary>宿主持有的取消源，外部"取消"按钮调用 Cancel()。</summary>
    public CancellationTokenSource CancelSource { get; } = new();

    public CancellationToken Cancel => CancelSource.Token;

    public void Report(int percent, string? message = null)
    {
        if (!string.IsNullOrEmpty(message))
            _log.Info($"[{Title}] {message}{(percent >= 0 ? $" ({percent}%)" : "")}");
        var pct = percent >= 0 ? $" {percent}%" : "";
        var msg = string.IsNullOrEmpty(message) ? "" : $" {message}";
        _setStatus?.Invoke($"▶ {Title}{pct}{msg}");
    }

    public T GetParam<T>(string name)
    {
        if (!Params.TryGetValue(name, out var raw))
            throw new Exception($"缺少参数 {name}");
        return ConvertValue<T>(raw, name);
    }

    private static T ConvertValue<T>(object? raw, string name)
    {
        var target = typeof(T);
        var t = Nullable.GetUnderlyingType(target) ?? target;
        try
        {
            if (raw is null) return default!;
            if (t.IsInstanceOfType(raw)) return (T)raw;
            if (t.IsEnum) return (T)Enum.Parse(t, raw.ToString()!, true);
            if (t == typeof(int)) return (T)Convert.ChangeType(Convert.ToInt32(raw), t, CultureInfo.InvariantCulture);
            if (t == typeof(double)) return (T)Convert.ChangeType(Convert.ToDouble(raw), t, CultureInfo.InvariantCulture);
            if (t == typeof(bool)) return (T)Convert.ChangeType(Convert.ToBoolean(raw), t, CultureInfo.InvariantCulture);
            return (T)Convert.ChangeType(raw, t, CultureInfo.InvariantCulture);
        }
        catch (Exception ex)
        {
            throw new Exception($"参数「{name}」值 {raw} 无法转换为 {t.Name}: {ex.Message}");
        }
    }
}
