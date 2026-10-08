using Avalonia.Controls;
using Avalonia.VisualTree;
using ComUI.Sdk;

namespace ComUI.Plugin.Image2D;

/// <summary>
/// M2 里程碑插件：OpenGL 静态大图查看器（Image2D）。
/// 订阅总线约定主题 image/stitched 显示算法发布图像；内置 100MP 测试图生成验证大图链路。
/// </summary>
[Plugin("ui.image2d", "2D 图像", "0.1.0",
    "OpenGL 静态大图查看器：GPU 缩放平移、mipmap 抗摩尔纹、放大像素检视、光标取色；订阅 image/stitched 总线主题")]
public sealed class Image2DPlugin : IPlugin
{
    private readonly List<IDisposable> _views = new();
    private readonly List<IDisposable> _snapshots = new();   // 快照标签页（关闭后由下次快照回收，Shutdown 全清）
    private IPluginContext _ctx = null!;

    public void Initialize(IPluginContext context)
    {
        _ctx = context;
        context.Log("2D 图像插件已初始化（OpenGL 静态大图查看器）");
    }

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        new PanelDescriptor
        {
            Id = "ui.image2d.view",
            Title = "2D 图像",
            Role = PanelRole.Document,
            AutoOpen = true,
            Icon = "🖼",
            CreateView = () => { var v = new Image2DView(_ctx.Bus, _ctx); v.SnapshotCreated += TrackSnapshot; _views.Add(v); return v; },
        },
    };

    /// <summary>登记快照视图；顺手回收已关闭标签页（不在可视树=已关闭）的 GL 资源。</summary>
    private void TrackSnapshot(Control snap)
    {
        if (snap is not IDisposable d) return;
        for (int i = _snapshots.Count - 1; i >= 0; i--)
        {
            if (_snapshots[i] is Control c && c.GetVisualRoot() is not null) continue;
            _snapshots[i].Dispose();
            _snapshots.RemoveAt(i);
        }
        _snapshots.Add(d);
    }

    public void Shutdown()
    {
        foreach (var v in _views) v.Dispose();
        _views.Clear();
        foreach (var s in _snapshots) s.Dispose();
        _snapshots.Clear();
    }
}
