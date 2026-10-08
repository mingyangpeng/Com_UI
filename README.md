# ComUI 显示平台

基于 **Avalonia** 的插件化显示框架（Windows / Linux Ubuntu 通用）。
宿主是固定外壳（侧边栏 / 分屏文档区 / 属性 / 日志），显示能力全部来自插件；
算法通过 **DLL + JSON 描述** 接入，无需写任何 UI 代码。

## 目录结构

```
Com_UI/
├── ComUI.slnx
├── start.bat / start.sh
├── docs/BUGS.md               # 开发 Bug 记录（持续维护）
├── comdll/                    # ★ 插件与数据目录
│   ├── common/                # 共享契约：算法 IO 接口 DLL（宿主最先加载）
│   ├── ui/<插件名>/            # UI 插件（实现 IPlugin，可注册多个面板）
│   └── algo/<插件名>/          # 算法插件（算法 DLL + *.algo.json）
├── config/
│   ├── host.json
│   └── layout.json            # 分屏布局（自动保存/恢复）
└── src/
    ├── ComUI.Sdk/             # 插件契约：IPlugin / IAlgoPlugin / IBus / PanelDescriptor
    ├── ComUI.Host/            # 宿主：Dock 布局 + 插件加载 + 通用算法执行器 + 数据总线
    └── Plugins/
        ├── ComUI.Plugin.Table/       # 示例：DataGrid 表格
        ├── ComUI.Plugin.DemoPreview/ # 示例：一个插件两个面板 + 总线订阅
        ├── ComUI.Plugin.NodeGraph/   # ★ 核心：算法流水线（ComfyUI 式节点编辑器）
        └── DemoAlgo.Core/            # 示例：算法 DLL + demo.algo.json 模板
```

## 编译与运行

```bash
# Windows：双击 start.bat，或
dotnet build ComUI.slnx -c Debug
src\ComUI.Host\bin\Debug\net8.0\ComUI.Host.exe

# Ubuntu
./start.sh
```

插件编译后自动复制到 comdll 对应目录。把新插件文件夹放进 comdll 后按 **F5** 热加载。

## UI 插件开发（引用 ComUI.Sdk）

```csharp
[Plugin("myid.mine", "我的插件", "1.0.0", "描述")]
public sealed class MyPlugin : IPlugin
{
    public void Initialize(IPluginContext ctx) { /* 存 ctx，可订阅 ctx.Bus */ }

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        // Document = 中间分屏区；Tool = 停靠/悬浮工具窗（DefaultDock 选 Left/Right/Bottom/Top/Floating）
        new PanelDescriptor { Id = "myid.main", Title = "我的面板",
            Role = PanelRole.Document, AutoOpen = true, CreateView = () => new MyView() },
        new PanelDescriptor { Id = "myid.side", Title = "侧边视图",
            Role = PanelRole.Tool, DefaultDock = ToolDock.Left, Icon = "🔧",
            CreateView = () => new MySideView() },
    };

    public void Shutdown() { /* 停定时器/线程 */ }
}
```

- 面板**懒创建 + 单实例**：`CreateView` 首次显示时才调用；声明 `AllowMultipleInstances=true` 可开副本对比。
- 数据来自 `ctx.Bus.Subscribe<T>(topic, handler)`；结果发布用 `ctx.Bus.Publish(topic, payload)`。

## 算法插件开发（DLL + JSON，零 UI 代码）

交付物 = 算法 DLL + 一份 `*.algo.json`（模板见 `src/Plugins/DemoAlgo.Core/demo.algo.json`）：

```json
{
  "plugin": { "id": "algo.x", "name": "算法名", "version": "1.0.0", "description": "..." },
  "runtime": "dotnet",
  "assembly": "YourAlgo.dll",
  "type": "Namespace.ClassName",
  "method": "Run",
  "params": [ { "name": "voxel", "label": "体素(mm)", "type": "number", "default": 0.05 } ],
  "inputs": [ { "name": "cloud", "source": "topic", "topic": "cloud/merged" } ],
  "outputs": [ { "from": "Result.Cloud", "topic": "cloud/merged" } ],
  "cancel": { "mode": "token" }
}
```

- 方法签名按参数名绑定：inputs/params 按 name 注入；`CancellationToken` 注入取消；`IProgress<string>` 注入进度。
- 复杂流程（多步编排）可写 `IAlgoPlugin` 手写壳，与 JSON 方式并存。
- 共享的图像/点云类型放 `comdll/common/`（如算法 IO 接口 DLL），宿主预加载保证类型身份一致。

## 算法流水线（节点图，核心插件）

「算法流水线」面板是一个 ComfyUI 式节点编辑器：**算法 = 独立步骤 = 节点**。

- **节点类型**：源（测试图像源/点云源）→ 处理（灰度化/反相/亮度对比度/模糊）→ 输出（发布图像/发布点云到总线）。
- **连线**：从输出端口拖到输入端口即连线（贝塞尔曲线、按类型着色）；拖动已连线的输入端口 = 断开重连；同一输入只允许一条线。
- **运行时参数**：参数控件内嵌在节点卡片上（滑杆/下拉/文本），改动后自动按拓扑序重跑（可关自动运行，手动 ▶ 运行），结果即时发布到总线、预览面板即时更新。
- **交互**：节点头部拖动移动、右键删除节点、空白处拖动平移、滚轮缩放、节点状态点（灰待执行/黄执行中/绿完成/红错误）。
- **执行**：拓扑排序（检测环路），后台线程顺序执行，输入缺失/类型不符/环路上报明确错误。

扩展新节点：在 `NodeDefs.All` 里加一个 `NodeDef`（声明输入/输出端口、参数、Execute 函数）即可出现在"＋添加节点"列表中。

## 数据总线与联动

- 算法 → `Publish(topic, 数据)` → 显示面板 `Subscribe` 自动显示；每主题保留最新帧。
- 面板联动走约定主题：`sel/point3d`（点云选点）、`sel/roi2d`（图像框 ROI），载荷见 `SelectionPayload`。

## 加载机制

- 每插件文件夹一个可卸载 ALC；F5 重载 = 全部 Shutdown → 卸载 → 重扫。
- 宿主默认上下文已有的程序集（Sdk / common）自动跳过，保证类型身份一致。
- 布局（分屏/工具窗位置）自动持久化到 config/layout.json。

## 路线图

- M1 ✅ 骨架：契约 v2 / 双目录加载 / Dock 分屏 / 数据总线 / JSON 算法执行器
- M1.5 ✅ 核心插件：算法流水线（节点图编辑器，ComfyUI 式）
- M2 2D：OpenGL 静态大图查看器（缩放平移 / 像素取色 / 叠加层 / ROI）
- M3 3D：OpenTK 点云与 Mesh（PCL 风格 API，亿级分块 + 交互抽稀）
- M4 联动：点选 / ROI 串通图像与点云双屏
