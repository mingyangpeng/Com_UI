using Avalonia.Controls;
namespace ComUI.Sdk;

/// <summary>宿主提供给 UI 插件的服务。</summary>
public interface IPluginContext
{
    /// <summary>宿主版本，例如 "0.2.0"。</summary>
    string HostVersion { get; }

    /// <summary>本插件所在文件夹。</summary>
    string PluginFolder { get; }

    void Log(string message);

    void LogWarning(string message);

    void LogError(string message);

    /// <summary>更新底部状态栏文本。</summary>
    void SetStatus(string status);

    /// <summary>数据总线：订阅算法结果、发布选择事件等。</summary>
    IBus Bus { get; }

    /// <summary>
    /// 在宿主「文件」菜单中追加一个插件入口（如"新建流水线页面"）。
    /// 在 Initialize 中调用；宿主加载完成后统一渲染。
    /// </summary>
    void AddFileMenuEntry(string header, Action execute);

    /// <summary>
    /// 请求宿主打开一个动态文档页（新标签页）。
    /// 适用于插件运行期才确定数量的页面（如多个流水线页面）。
    /// </summary>

    /// <summary>
    /// 打开动态文档页并携带恢复键（会话持久化）。
    /// 恢复键格式 "<类型>:<键体>"——类型经 RegisterDocumentFactory 登记，
    /// 键体由该工厂解释（如 pipeline:D:\x\y.pipeline.json）。无恢复键的页面不进会话。
    /// </summary>
    void OpenDocument(string title, Control content, string? restoreKey);

    /// <summary>登记动态文档页恢复工厂：会话恢复时按类型以键体重建内容（Initialize 时注册一次）。</summary>
    void RegisterDocumentFactory(string docType, Func<string, Control> reopen);

    /// <summary>面板首次显示时触发（参数 = 面板 Id）。</summary>
    event Action<string>? PanelShown;

    /// <summary>面板被隐藏 / 关闭时触发。</summary>
    event Action<string>? PanelHidden;
}

/// <summary>宿主提供给算法插件的服务。</summary>
public interface IAlgoContext
{
    string HostVersion { get; }

    string PluginFolder { get; }

    void Log(string message);

    void LogWarning(string message);

    void LogError(string message);

    /// <summary>发布算法结果到总线（显示面板订阅即显示）。</summary>
    void Publish<T>(string topic, T payload);
}
