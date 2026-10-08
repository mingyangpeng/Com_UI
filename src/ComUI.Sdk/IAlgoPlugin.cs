namespace ComUI.Sdk;

/// <summary>算法插件接口（手写壳）。一般场景推荐用 JSON 描述 + 宿主通用执行器，无需写代码。</summary>
public interface IAlgoPlugin
{
    /// <summary>插件被加载时调用一次。</summary>
    void Initialize(IAlgoContext context);

    /// <summary>注册用户可触发的算法命令（宿主显示在「算法」侧边栏，按插件分组）。</summary>
    IEnumerable<AlgoCommand> GetCommands();

    /// <summary>插件被卸载 / 重载前调用。</summary>
    void Shutdown();
}

/// <summary>一条用户可触发的算法命令。</summary>
public sealed class AlgoCommand
{
    /// <summary>命令唯一 Id，如 "stitch.run"。</summary>
    public string Id { get; init; } = "";

    /// <summary>显示名称，如 "执行拼接"。</summary>
    public string Title { get; init; } = "";

    /// <summary>参数声明（非空时宿主自动生成参数对话框）。可为 null 表示无参。</summary>
    public IReadOnlyList<AlgoParam>? Params { get; init; }

    /// <summary>执行体（后台线程调用，可长时间运行，注意响应 Cancel）。</summary>
    public Func<IAlgoRunContext, Task> Execute { get; init; } = _ => Task.CompletedTask;
}

/// <summary>算法参数类型。</summary>
public enum AlgoParamType
{
    /// <summary>浮点数。</summary>
    Number,

    /// <summary>整数。</summary>
    Int,

    /// <summary>文本。</summary>
    Text,

    /// <summary>布尔。</summary>
    Boolean,

    /// <summary>枚举（取值为 <see cref="AlgoParam.Options"/> 之一）。</summary>
    Enum,

    /// <summary>文件路径（宿主提供浏览按钮）。</summary>
    Path,
}

/// <summary>算法参数声明。</summary>
public sealed class AlgoParam
{
    public string Name { get; init; } = "";
    public string Label { get; init; } = "";
    public AlgoParamType Type { get; init; } = AlgoParamType.Number;
    public object? Default { get; init; }
    public string[]? Options { get; init; }
    public double? Min { get; init; }
    public double? Max { get; init; }
}

/// <summary>一次算法执行上下文：传参、进度、取消。</summary>
public interface IAlgoRunContext
{
    /// <summary>用户点击"取消"后触发。</summary>
    CancellationToken Cancel { get; }

    /// <summary>报告进度（0-100）与消息（显示在状态栏 / 日志）。</summary>
    void Report(int percent, string? message = null);

    /// <summary>读取用户填写的参数值。</summary>
    T GetParam<T>(string name);
}
