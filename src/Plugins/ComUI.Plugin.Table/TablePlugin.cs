using ComUI.Sdk;

namespace ComUI.Plugin.Table;

[Plugin("ui.table", "数据表格", "1.0.0",
    "DataGrid 表格显示示例：随机数据生成、自动刷新，展示结构化数据显示能力")]
public sealed class TablePlugin : IPlugin
{
    private readonly List<IDisposable> _views = new();

    public void Initialize(IPluginContext context)
    {
        context.Log("数据表格插件已初始化");
    }

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        new PanelDescriptor
        {
            Id = "ui.table.main",
            Title = "数据表格",
            Role = PanelRole.Document,
            AutoOpen = false,
            CreateView = () => { var v = new TableView(); _views.Add(v); return v; },
        },
    };

    public void Shutdown()
    {
        foreach (var v in _views) v.Dispose();
        _views.Clear();
    }
}
