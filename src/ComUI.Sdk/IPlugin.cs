using Avalonia;
using Avalonia.Controls;

namespace ComUI.Sdk;

/// <summary>
/// UI 插件必须实现的接口。宿主在 comdll/ui 的各插件文件夹中发现带有
/// <see cref="PluginAttribute"/> 的实现后：Initialize → GetPanels（注册面板）→
/// 面板首次显示时 CreateView → Shutdown（卸载/重载前）。
/// </summary>
public interface IPlugin
{
    /// <summary>插件被加载时调用一次。保存 context 以便写日志 / 订阅数据总线。</summary>
    void Initialize(IPluginContext context);

    /// <summary>注册该插件提供的所有面板（一个插件可注册多个）。</summary>
    IEnumerable<PanelDescriptor> GetPanels();

    /// <summary>插件被卸载 / 重载前调用：停止定时器与线程、释放资源。</summary>
    void Shutdown();
}

/// <summary>面板角色：中间文档区（可分屏/浮动）或停靠工具窗。</summary>
public enum PanelRole
{
    /// <summary>中间文档区，支持拖拽分屏、成组、拖出悬浮。</summary>
    Document,

    /// <summary>停靠工具窗，按 <see cref="PanelDescriptor.DefaultDock"/> 初始停靠，可拖动改位。</summary>
    Tool,
}

/// <summary>工具窗初始停靠位（用户之后可自由拖成固定/悬浮）。</summary>
public enum ToolDock
{
    Left,
    Right,
    Bottom,
    Top,

    /// <summary>初始为悬浮窗口。</summary>
    Floating,
}

/// <summary>面板描述：插件用它向宿主注册一个显示面板。</summary>
public sealed class PanelDescriptor
{    /// <summary>全局唯一 Id，约定格式 "插件Id.面板名"，布局持久化按它匹配。</summary>
    public string Id { get; init; } = "";

    /// <summary>标签 / 窗口标题。</summary>
    public string Title { get; init; } = "";

    /// <summary>创建面板视图（懒创建：面板第一次显示时才调用，实例由宿主缓存）。</summary>
    public Func<Control> CreateView { get; init; } = () => new Control();

    public PanelRole Role { get; init; } = PanelRole.Document;

    /// <summary>Role == Tool 时生效：初始停靠位。</summary>
    public ToolDock DefaultDock { get; init; } = ToolDock.Bottom;

    /// <summary>Role == Tool 时生效：初始尺寸（像素）。</summary>
    public Size? DefaultSize { get; init; }

    /// <summary>首次启动（尚无布局文件）时是否自动打开。</summary>
    public bool AutoOpen { get; init; }

    /// <summary>是否允许"新建副本"开第二个实例（A/B 对比用）。默认单实例。</summary>
    public bool AllowMultipleInstances { get; init; }

    /// <summary>活动栏图标（侧边栏视图用），任意字符 / emoji。</summary>
    public string? Icon { get; init; }

    /// <summary>声明式插件关联：本面板与这些面板 Id 逻辑关联（跨插件可用）。
    /// 宿主据此联动——激活关联的 Document 面板时，本 Tool 面板自动切入侧边栏；
    /// 反之激活无关面板时自动切走。示例：点云树关联 "ui.cloud3d.view"。</summary>
    public string[]? AssociatedPanels { get; init; }
}

/// <summary>
/// 面板视图可选实现：向右侧属性面板的「参数」节提供可编辑控件
/// （数值/下拉等，直接调参，不走滑杆）。宿主在面板激活时取一次并放入属性面板；
/// 控件实例由插件持有（面板单实例，属性区随激活切换）。
/// </summary>
public interface IPanelParamsProvider
{
    /// <summary>参数编辑控件；null 表示该面板无参数。</summary>
    Control? CreateParamsContent();
}
