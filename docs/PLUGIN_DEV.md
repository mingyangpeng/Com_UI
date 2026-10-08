# ComUI 插件开发说明

面向插件开发人员（UI 面板 / 算法 / 节点算子三类交付物）。第三方算法厂商接入算子库的专项规则见 [THIRD_PARTY_OPS.md](THIRD_PARTY_OPS.md)。契约层代码在 `src/ComUI.Sdk/`，
本文与其同步；架构全貌见 [ARCHITECTURE.md](ARCHITECTURE.md)，交互组件复用规则见 [CONVENTIONS.md](CONVENTIONS.md)。

## 0. 总览：三类交付物

| 交付物 | 位置 | 形态 | 适用 |
|---|---|---|---|
| **UI 插件** | `comdll/ui/<插件名>/` | C# 类库（引用 ComUI.Sdk） | 显示面板、工具侧边栏 |
| **算法插件** | `comdll/algo/<名称>/` | 算法 DLL + `*.algo.json` | 算法侧边栏可运行的命令 |
| **节点算子** | `comdll/algo/<名称>/` | 同一 DLL + `*.node.json` | 算法流水线里的节点 |

- 算法侧**无需写壳代码**：JSON 声明 + DLL 即接入（宿主通用执行器反射调用）；只有需要自定义生命周期才手写 `IAlgoPlugin`。
- 一个程序集可含多个插件（`[Plugin]` 标记多个类）。
- 宿主启动与 **F5** 都会扫描加载；插件可卸载重载（`Shutdown` 必须干净收尾）。

## 1. 快速开始：最小 UI 插件

**项目文件**（csproj，注意末尾的复制目标——编译产物自动进 `comdll/ui/`）：

```xml
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <RootNamespace>ComUI.Plugin.Demo</RootNamespace>
    <Version>1.0.0</Version>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Avalonia" Version="11.3.*" />
  </ItemGroup>
  <ItemGroup>
    <ProjectReference Include="..\..\ComUI.Sdk\ComUI.Sdk.csproj" />
  </ItemGroup>
  <Target Name="CopyToComdll" AfterTargets="Build">
    <ItemGroup>
      <_PluginOutputs Include="$(OutDir)*.*" Exclude="$(OutDir)ComUI.Sdk.*" />
    </ItemGroup>
    <Copy SourceFiles="@(_PluginOutputs)"
          DestinationFolder="$(MSBuildThisFileDirectory)..\..\..\comdll\ui\Demo\"
          SkipUnchangedFiles="true" />
  </Target>
</Project>
```

**插件代码**：

```csharp
using Avalonia.Controls;
using ComUI.Sdk;

[Plugin("comui.demo", "演示插件", "1.0.0", "最小示例：订阅总线图像并显示")]
public sealed class DemoPlugin : IPlugin
{
    private IPluginContext _ctx = null!;

    public void Initialize(IPluginContext context)
    {
        _ctx = context;
        context.Log("演示插件已初始化");
    }

    public IEnumerable<PanelDescriptor> GetPanels() => new[]
    {
        new PanelDescriptor
        {
            Id = "comui.demo.view",          // 约定格式：插件Id.面板名（全局唯一，会话持久化按它匹配）
            Title = "演示面板",
            Role = PanelRole.Document,       // Document=中间标签页（可分屏/浮动）；Tool=活动栏侧边栏
            AutoOpen = true,                 // 首次启动自动打开
            Icon = "🧪",
            CreateView = () => new DemoView(_ctx.Bus),
        },
    };

    public void Shutdown() { /* 停定时器/线程、退订总线、释放 GL */ }
}

public sealed class DemoView : UserControl
{
    public DemoView(IBus bus)
    {
        var img = new Image();
        Content = img;
        bus.Subscribe<ImagePayload>(BusTopics.ImageStitched, p =>
        {
            // 晚订阅自动拿到最新保留帧；uiThread=true 时回调已切到 UI 线程
        }, uiThread: true);
    }
}
```

放入 `src/Plugins/` 加入解决方案，构建后出现在「🧩 面板」列表。

## 2. UI 插件契约

### 2.1 生命周期

`Initialize(IPluginContext)` → `GetPanels()`（注册面板）→ 面板首次显示时 `CreateView()`（**懒创建，宿主缓存单实例**）→ `Shutdown()`（F5 重载/退出前）。

### 2.2 PanelDescriptor 字段

| 字段 | 说明 |
|---|---|
| `Id` | 全局唯一，约定 `插件Id.面板名`；会话布局按它恢复 |
| `Title` / `Icon` | 标签标题 / 活动栏图标（任意字符/emoji） |
| `Role` | `Document`（标签页，支持分屏/拖拽排序/浮动）或 `Tool`（活动栏侧边栏视图） |
| `CreateView` | 视图工厂（懒创建；同一实例只挂一处） |
| `AutoOpen` | 首次启动自动打开 |
| `AssociatedPanels` | 声明式关联：激活关联的 Document 面板时宿主自动切到本 Tool 侧边栏 |
| `AllowMultipleInstances` | 预留：允许开第二实例（A/B 对比用） |
| `DefaultDock`/`DefaultSize` | Tool 面板初始位（手写外壳下停靠区固定左列） |

面板可选实现 `IPanelParamsProvider.CreateParamsContent()`：返回控件放进右侧属性面板「参数」节（直接调参，不走弹窗）。

### 2.3 IPluginContext 服务

| 成员 | 用途 |
|---|---|
| `Bus` | 数据总线（见 §3） |
| `Log/LogWarning/LogError` | 写底部日志面板 |
| `SetStatus` | 写状态栏 |
| `OpenDocument(title, content, restoreKey)` | 开动态文档标签页（restoreKey=null 不进会话，如快照） |
| `RegisterDocumentFactory(docType, reopen)` | 登记恢复工厂：会话重启按 `类型:键体` 重建页面 |
| `AddFileMenuEntry` | 往宿主「文件」菜单加入口 |
| `PanelShown/PanelHidden` | 面板首次显示/隐藏通知 |

## 3. 数据总线（插件间唯一数据通道）

**发布 → 保留最新帧 → 订阅即显示**。晚订阅自动收到保留帧；`uiThread: true` 回调切到 UI 线程。
主题名**必须用 `BusTopics` 常量**（禁止散落字面量）：

| 常量 | 主题 | 载荷 | 语义 |
|---|---|---|---|
| `ImageStitched` | `image/stitched` | `ImagePayload` | 2D 图像帧（BGRA32） |
| `CloudMerged` | `cloud/merged` | `CloudPayload` | 点云帧（按 Id 增/改实体） |
| `CloudRemoved` | `cloud/removed` | `CloudPayload` | 按 Id 删除点云实体 |
| `SelPoint3D` | `sel/point3d` | `SelectionPayload` | 三维点选择（含归一化 uv 供图像高亮） |
| `SelRoi2D` | `sel/roi2d` | `SelectionPayload` | 2D 矩形 ROI（图像像素坐标） |
| `SelCloudBox` | `sel/cloud-box` | `SelectionPayload` | 点云框选（包围盒） |
| `PipelineOpen` | `pipeline/open` | `string` | 请求打开某流水线算法（载荷=文件路径） |
| `AlgoSaved` | `algo/saved` | `string` | 算法配方已保存（侧边栏即时刷新） |
| `AlgoLocked` | `algo/locked` | `string` | 算法锁定状态切换 |
| `PanelActivate` | `ui/activate-panel` | `string` | 请求打开/聚焦某窗口面板（载荷=面板 Id） |

**载荷约定**：`ImagePayload{Width,Height,PixelsBgra,Source,Id}`；`CloudPayload{Id,Points(XYZ 三元组),ColorsRgb?,Count,Source}`。
**数组不可变约定**：发布后的载荷数组视为只读（下游零拷贝引用共享，如快照/多实体）。

## 4. 算法插件（JSON 声明，推荐）

交付物 = 算法 DLL + `*.algo.json`，放 `comdll/algo/<名称>/`。模板（见 `comdll/algo/DemoStitching/demo.algo.json`）：

```jsonc
{
  "plugin": { "id": "algo.demo", "name": "演示拼接", "version": "1.0.0", "description": "…" },
  "runtime": "dotnet",              // dotnet = C# 反射；native = C 导出函数
  "assembly": "DemoAlgo.Core.dll",
  "type": "DemoAlgo.Core.FakeStitcher",
  "method": "Stitch",
  "params": [                        // 宿主自动生成参数对话框
    { "name": "points", "label": "点数", "type": "int", "default": 200000, "min": 10000, "max": 2000000 },
    { "name": "mode",   "label": "模式", "type": "enum", "default": "ICP", "options": ["ICP", "NDT"] }
  ],
  "inputs": [],                      // 从总线取最新帧（name 对应方法参数名）
  "outputs": [ { "name": "图像", "topic": "image/stitched" } ]   // 返回值属性 → 发布到主题
}
```

参数类型：`number / int / text / bool / enum / path`。执行在后台线程，宿主提供取消/进度。
需要自定义生命周期/命令注册时才手写 `IAlgoPlugin`（`GetCommands` 返回 `AlgoCommand` 列表，显示在「算法配方」侧边栏）。

## 5. 节点算子（流水线节点）

同一 DLL 配 `*.node.json` 即注册进「算子库」（`NodeJsonRegistry` 启动/F5 自动扫描）：

```jsonc
{
  "node": { "id": "algo.gamma", "name": "伽马校正", "group": "2D 图像", "category": "处理" },
  "runtime": "dotnet", "assembly": "DemoAlgo.Core.dll", "type": "DemoAlgo.Core.Filters", "method": "Gamma",
  "inputs": [ { "name": "输入", "bind": "input", "type": "image" } ],
  "params": [ …同 algo.json… ],
  "outputs": [ … ]
}
```

`group` = 算子库分组栏目（栏目头可拖拽排序，顺序存 `config/op_order.json`）；`category` 决定节点头配色（源/处理/输出）。

## 6. 共享交互组件（ComUI.Sdk.Ui，先查表再自造）

| 组件 | 用途 |
|---|---|
| `InlineRename` | 行内改名（Enter 提交/Esc 取消/失焦提交） |
| `LockGlyph` | 锁徽标（绿开锁=未锁、灰闭锁=锁定，钉行右端） |
| `FileLock` | 文件型 JSON 的 `Locked` 字段读写（锁定=只读打开） |
| `PreviewSurfaces` | draw 预览卡片工厂（`CloudPreview` 等，卡片内嵌 3D 旋转+一键导入工作台） |
| `DoubleClickDetector` | 双击检测（与单击/拖拽消歧） |

约定细则（行布局、锁对齐、改名放行规则等）见 [CONVENTIONS.md](CONVENTIONS.md)。

## 7. 面板联动速查

- **声明式关联**：`AssociatedPanels = new[]{"ui.cloud3d.view"}` —— 激活该 Document 时宿主自动切到你的 Tool 侧边栏。
- **跳转面板**：发布 `BusTopics.PanelActivate`（载荷=面板 Id）→ 宿主打开/聚焦对应标签（数据树行点击即此机制）。
- **选择联动**：工作台圈选发布 `sel/*`；消费方按 `Source` 过滤（自己发的可忽略）。
- **动态文档页**：`OpenDocument` + `RegisterDocumentFactory` 组合支持运行期页面 + 会话恢复（参考算法流水线插件的 `pipeline:<路径>` 恢复键）。

## 8. 调试与验证

- **F5 热重载**：菜单「文件 → 重新加载插件」。`Shutdown` 里必须停定时器/线程、退订总线、释放 GL，否则 ALC 不回收（日志会 WARN 点名）。
- **日志**：底部日志面板（`Log/LogWarning/LogError`）；未处理异常进 `output/error.log`。
- **单测**：模型层放 `tests/ComUI.Core.Tests`（纯模型可测，UI 层用 E2E）。
- **E2E**：`tools/`（fgdrag/fgclick/shot 等，真实输入+截图+UIA 断言）；交互功能优先做成可点击按钮再自动化。

## 9. 常见坑（历史教训精选，详见 BUGS.md）

1. **视图单实例**：`CreateView` 产物被宿主缓存，同一实例只能挂一处（混开=视觉父级冲突）。
2. **GL 上下文**：每视图一个离屏渲染器；**禁多个后台 GL worker**（共享上下文互卡死锁——快照/预览走同步上传路径）。
3. **列表刷新**：Rebuild 只重填内部容器，**绝不 `Content = 新结构`**（会把搜索框/覆盖层一起孤立）。
4. **要拖拽的行别用 Button**：ButtonBase 的捕获/Handled/Click 与拖拽状态机互搏——用 Border + 容器级按下/移动/释放。
5. **Avalonia 恢复样式默认**：`ClearValue`，不是置 null（null 是局部值会压掉样式）。
6. **发布数组不可变**：下游引用共享零拷贝，发布后别原地改。
