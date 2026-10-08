using Avalonia.Controls;
using Avalonia.VisualTree;
using ComUI.Sdk;

namespace ComUI.Plugin.Cloud3D;

/// <summary>
/// 3D 点云插件：查看器（Document）+ 点云树（Tool 侧边栏）。
/// 订阅总线约定主题 cloud/merged；按 Id 多实体管理（M5 设备树一期）。
/// </summary>
[Plugin("ui.cloud3d", "3D 点云", "0.2.0",
    "OpenGL 点云查看器：多实体点云（按 Id 管理）、轨道相机（左键旋转/滚轮缩放/右键平移）、Ctrl+拾取/框选联动、" +
    "分块渐进上传、交互抽稀、视角预设、XYZ 指示器；附数据树侧边栏（2D 图像/3D 点云，显隐/删除/详情）")]
public sealed class Cloud3DPlugin : IPlugin
{
    private readonly List<IDisposable> _views = new();
    private readonly List<IDisposable> _snapshots = new();   // 快照标签页（关闭后由下次快照回收，Shutdown 全清）
    private IPluginContext _ctx = null!;
    private Cloud3DView? _view;
    private IBus Bus => _ctx.Bus;

    public void Initialize(IPluginContext context)
    {
        // draw.3d 预览控件工厂（算子流水线的 3D 预览卡片内嵌用）
        Sdk.Ui.PreviewSurfaces.CloudPreview = payload => new MiniCloudPreview(payload);

        _ctx = context;
        context.Log("3D 点云插件已初始化（OpenGL 多实体点云查看器 + 数据树）");
    }

    private Cloud3DView GetView() => _view ??= CreateView();

    private Cloud3DView CreateView()
    {
        var v = new Cloud3DView(_ctx.Bus, ctx: _ctx);
        v.SnapshotCreated += TrackSnapshot;
        _views.Add(v);
        return v;
    }

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

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        new PanelDescriptor
        {
            Id = "ui.cloud3d.view",
            Title = "3D 点云",
            Role = PanelRole.Document,
            AutoOpen = true,
            Icon = "☁",
            CreateView = GetView,
        },
        new PanelDescriptor
        {
            Id = "ui.cloud3d.tree",
            Title = "数据树",
            Role = PanelRole.Tool,
            AutoOpen = false,
            Icon = "🌲",
            // 声明式关联：激活 3D 视图标签时宿主自动切入本侧边栏
            AssociatedPanels = new[] { "ui.cloud3d.view" },
            CreateView = () => new CloudTreePanel(GetView(), Bus),
        },
    };

    public void Shutdown()
    {
        foreach (var v in _views) v.Dispose();
        _views.Clear();
        foreach (var s in _snapshots) s.Dispose();
        _snapshots.Clear();
        _view = null;
    }
}
