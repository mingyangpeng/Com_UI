using ComUI.Sdk;

namespace ComUI.Plugin.DemoPreview;

/// <summary>
/// 总线监视插件：窗口面板——每条总线数据的详细追踪台（主题表+最新帧逐字段+发布历史）。
/// （原"结果预览"演示面板已移除：2D 图像工作台完全覆盖其功能。）
/// </summary>
[Plugin("ui.preview", "总线监视", "1.1.0",
    "总线监视窗口面板：主题表（类型/帧数/频率/最后发布）+选中主题详细追踪（最新帧逐字段+发布历史）")]
public sealed class DemoPreviewPlugin : IPlugin
{
    private readonly List<IDisposable> _views = new();
    private IPluginContext _ctx = null!;

    public void Initialize(IPluginContext context)
    {
        _ctx = context;
        context.Log("总线监视插件已初始化");
    }

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        new PanelDescriptor
        {
            Id = "ui.preview.bus",
            Title = "总线监视",
            Role = PanelRole.Document,   // 窗口面板：进「面板」列表、开成标签页；不再占活动栏
            Icon = "📡",
            CreateView = () => { var v = new BusMonitorView(_ctx.Bus); _views.Add(v); return v; },
        },
    };

    public void Shutdown()
    {
        foreach (var v in _views) v.Dispose();
        _views.Clear();
    }
}
