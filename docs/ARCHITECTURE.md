# ComUI 架构与模块说明

> 面向后期开发的全局地图：分层结构、每个模块的职责、扩展点、互联方式。
> 交互规则与组件复用见 [CONVENTIONS.md](CONVENTIONS.md)；踩坑记录见 [BUGS.md](BUGS.md)。

## 总览：UI + Core + 插件（三层，契约横跨）

```
┌───────────────────────────────────────────────────────────────┐
│  UI 层（表现）  ComUI.Host（WinExe）                            │
│    主窗口/活动栏/侧边栏/分屏标签区/浮动窗/属性/日志/状态栏、       │
│    拖拽交互、会话保存恢复                                        │
├───────────────────────────────────────────────────────────────┤
│  Core 层（引擎） ComUI.Core（类库，UI 引用之）                   │
│    插件加载/卸载/隔离、数据总线、面板生命周期、关联联动、           │
│    分屏树模型、配置读取（host.json）                              │
├───────────────────────────────────────────────────────────────┤
│  插件层  comdll/ui/（业务面板）+ comdll/algo/（算法）            │
│    NodeGraph、Cloud3D、Image2D、Table、DemoPreview、DemoAlgo…   │
└───────────────────────────────────────────────────────────────┘
          ▲                    ▲                    ▲
          └──── ComUI.Sdk（契约层，三方共同依赖）────┘
               插件契约 + BusTopics 主题注册表 + Sdk.Ui 交互原语
               common/：算法 IO 接口 DLL（外部团队类型，预加载）
```

**依赖方向（单向，强制）**：UI → Core → Sdk ← 插件；插件之间**零直接引用**（互联只走总线主题 + 声明契约）；宿主编译期不知道任何具体插件（运行期反射加载）。

**分层判断规则**（新能力放哪里）：

| 判断 | 归属 |
|---|---|
| 窗口管理、布局、会话、插件加载、面板生命周期 | Core（模型/引擎）+ UI（渲染/交互） |
| 面板内容、业务逻辑（渲染查看、节点编辑、算法） | 插件 |
| 跨插件共享的类型契约、主题、交互原语 | Sdk |
| 外部算法团队的类型 | common/ |
| 拿不准时 | 先放插件（将来可移出），不放框架（移不出） |

## 物理结构

```
src/
├── ComUI.Core/                    # ★ 引擎层（UI 引用；插件不引用）
│   ├── Core/AppPaths.cs           #   目录解析（exe 向上找仓库根；comdll/common/ui/algo、config、output）
│   ├── Modules/
│   │   ├── Logging/LogService.cs  #   日志：LogEntry + EntryAdded 事件（UI 端订阅显示）
│   │   ├── Bus/DataBus.cs         #   数据总线：IBus 实现，按主题发布/订阅，每主题保留最新帧，晚订阅立即拿到当前帧
│   │   ├── Plugins/               #   插件模式
│   │   │   ├── PluginLoadContext.cs  # 每插件一个可卸载 ALC；宿主已有程序集回退默认上下文
│   │   │   ├── PluginManager.cs      # 加载编排：common 预加载 → ui/ 扫描 → algo/ 扫描；UiContextCreated 事件；
│   │   │   │                         # HostPluginContext（AddFileMenuEntry/OpenDocument(恢复键)/RegisterDocumentFactory/
│   │   │   │                         # Bus/Panel 事件）、HostAlgoContext；官方插件校验（requiredPlugins）
│   │   │   ├── HostConfig.cs         # config/host.json 读取（JSONC：pluginDirectory/autoOpenPlugins/requiredPlugins）
│   │   │   └── HostAlgoRunContext.cs # 一次算法执行的参数/进度/取消
│   │   └── Import/                #   声明式导入（算法 DLL + JSON，零 UI 代码接入）
│   │       ├── AlgoJsonDescriptor.cs  # *.algo.json 模型
│   │       └── GenericAlgoRunner.cs   # 反射调用：参数名绑定输入/参数/进度/取消；输出属性路径 → 发布主题
│   └── Split/SplitTree.cs         #   分屏树模型（SplitLayout：Split/MovePanel/RemovePanel/FindLeaf +
│                                  #   ToJson/LoadFromJson 会话序列化）——纯模型可单测
│
├── ComUI.Host/                    # ★ UI 层（WinExe，引用 Core）
│   ├── Program.cs                 #   入口：单实例 Mutex、AppBuilder
│   ├── App.axaml(.cs)             #   全局异常围栏（三层钩子→日志面板+error.log）、共享样式
│   └── Shell/                     #   外壳（手写确定性布局，无第三方停靠库）
│       ├── MainWindow.axaml(.cs)     # Grid 布局：活动栏/侧边栏/分屏标签区(SplitHost)/属性/日志/状态栏（外壳骨架）
│       ├── MainWindow.Tabs.cs        # partial：分屏树渲染/标签开关拆分/标签拖拽（排序+拆分+浮动）/会话外标签操作
│       ├── MainWindow.Floating.cs    # partial：浮动窗（浮动/固定回主窗/关闭清理）
│       ├── MainWindow.Session.cs     # partial：会话保存恢复（session.json）/默认布局/恢复键翻译
│       └── Views/                    # PluginListSidebar/AlgoSidebar(算法文件管理)/LogView/
│                                     # PropertiesView/WelcomeView/ParamDialog
│
├── ComUI.Sdk/                     # ★ 契约层（改接口 = 全体插件重编译，谨慎）
│   ├── IPlugin.cs                 #   UI 插件：Initialize/GetPanels(多面板)/Shutdown
│   │                              #   PanelDescriptor{Id,Title,CreateView,Role(Document/Tool),DefaultDock,AutoOpen,
│   │                              #   AllowMultipleInstances,Icon,AssociatedPanels}
│   │                              #   外壳约定：Document → 分屏标签格；Tool → 活动栏侧边栏视图
│   │                              #   IPanelParamsProvider：面板可选实现，向属性面板「参数」节提供控件
│   ├── IPluginContext.cs          #   宿主服务：Log/SetStatus/Bus/AddFileMenuEntry/OpenDocument(恢复键)/
│   │                              #   RegisterDocumentFactory(会话恢复工厂)/PanelShown|Hidden
│   ├── IAlgoPlugin.cs             #   算法手写壳：IAlgoContext/AlgoCommand/AlgoParam/IAlgoRunContext
│   ├── IBus.cs                    #   总线契约 + BusTopics 主题注册表 + 载荷类型
│   └── Ui/                        #   共享交互原语（宿主与插件共用，禁止界面里重写）：
│                                  #     InlineRename（行内改名+双击检测器）、LockGlyph+FileLock（锁约定）
│
└── Plugins/                       # ★ 插件层（详见下节）
```

> 拆分已完成（2026-10-05）：ComUI.Core 类库承载引擎（命名空间 `ComUI.Core`），ComUI.Host 只留 Shell（引用 Core）；`HostVersion` 单一事实来源在 Core（App 常量引用）；跨程序集的 RaisePanelShown/Hidden 已改 public。Core 单测：tests/ComUI.Core.Tests（DataBus/HostConfig/GraphModel/SplitTree/SplitJson，25 例，`dotnet test`）。

## 插件互联（四条通道，按耦合从低到高）

| 通道 | 机制 | 实例 |
|---|---|---|
| ① 数据流 | 算法 → 约定数据主题 → 显示面板（最新帧保留，晚订阅立即拿到） | pub.cloud → `cloud/merged` → 3D 显示 |
| ② 选择联动 | 面板对等广播 sel/* 主题，载荷带 Source 防回环 | 3D 拾取 → `sel/point3d` → 2D 高亮（亚像素验证） |
| ③ 工作流 | 事件通知（轻量载荷） | `pipeline/open` 开页、`algo/saved` 刷新、`algo/locked` 同步只读 |
| ④ 布局关联 | PanelDescriptor.AssociatedPanels 声明，宿主仲裁 | 切 3D 标签自动显示点云树 |

插件 → 宿主方向：IPluginContext 服务（Log/SetStatus/OpenDocument/RegisterDocumentFactory/AddFileMenuEntry）。
**预留**（无场景不建）：请求-响应 = 请求主题 + 关联 ID + 回复主题；强类型服务 = Sdk 服务接口。

**主题即契约**：统一登记在 `ComUI.Sdk.BusTopics`（禁止散落字符串字面量）——数据 `image/stitched`、`cloud/merged`、`cloud/removed`；选择联动 `sel/point3d`、`sel/roi2d`、`sel/cloud-box`；工作流 `pipeline/open`、`algo/saved`、`algo/locked`。载荷/发布方/订阅方见 BusTopics 注释。

## 插件

```
src/Plugins/
├── ComUI.Plugin.NodeGraph/        # ★ 官方插件（非框架）：算法流水线（ComfyUI 式节点编辑器）
│   ├── NodeGraphPlugin.cs         #   面板注册/文件菜单"新建流水线页面"/算子JSON注册/pipeline-open 订阅/
│   │                              #   文档恢复工厂（"pipeline" 类型，会话恢复按路径重建页）
│   ├── Core/                      #   GraphModel（图文档/节点/连线/序列化与粘贴）、GraphExecutor（拓扑+后台执行）
│   ├── Operators/                 #   NodeDefs（内置算子注册表）、NodeJsonRegistry（*.node.json → 算子）
│   └── UI/                        #   NodeEditorView（画布）、NodeLibraryView（算子库）、NamePrompt
├── ComUI.Plugin.Table/            # 示例：DataGrid
├── ComUI.Plugin.DemoPreview/      # 示例：总线订阅显示（一插件两面板）
├── ComUI.Plugin.Image2D/          # ★ M2：OpenGL 静态大图查看器（离屏 GL + FBO 回读 + 软件回退）
├── ComUI.Plugin.Cloud3D/          # ★ M3/M5：OpenGL 3D 点云（多实体/点云树/后台上传线程/密度封顶/原生缓冲）
└── DemoAlgo.Core/                 # 示例：算法 DLL + demo.algo.json + demo.node.json
```

**新增算子的三种方式**（按推荐排序）：① JSON 声明（`comdll/algo/<名>/` 放 DLL + *.node.json 或 *.algo.json，零 C#）；② 手写壳（IAlgoPlugin/IPlugin）；③ 内置算子（NodeDefs.All）。

## 关键机制备忘

- **插件隔离**：每插件文件夹一个 collectible ALC；F5 = 全部 Shutdown → 卸载 → 重扫（卸载后 WeakReference 验证回收完整性，未回收 → WARN 点名——典型原因：总线/事件未退订）；单插件失败标红不影响其他。
- **插件原生依赖**：AssemblyDependencyResolver 只在插件文件夹内解析——带非宿主依赖（如 OpenTK）的插件必须自包含（CopyLocalLockFileAssemblies + 原生库平铺，详见 BUGS.md BUG-038）。
- **类型身份**：common/ 与 ComUI.Sdk 由宿主默认上下文加载；扫描时跳过宿主已有程序集（防类型分裂）。
- **数据流**：算法 → Publish(topic, 数据) → 总线保留最新帧 → 面板 Subscribe 即显示。
- **面板生命周期**：注册（GetPanels）→ 懒创建 → 单实例缓存 → 关闭=隐藏；同面板多实例需声明 AllowMultipleInstances；面板可浮动为独立窗口（不占分屏格，浮动态+几何随会话保存恢复）。
- **执行门**：算法命令 Interlocked 互斥，取消经 CancellationToken。
- **GL 呈现行序（BUG-047）**：glReadPixels 自底向上 × WriteableBitmap 自顶向下——GL 路径逐行倒序拷贝。
- **外壳布局**：手写 Grid（活动栏 46/侧边栏 235/标签区 */属性 255/日志 200）；Tool 侧边栏固定左列（定案：不做左右停靠）。
- **分屏标签区**：SplitNode 树（Core 模型）递归渲染为嵌套 Grid + 格间 GridSplitter；每格一个 TabControl；标签拖拽（**标签条上=按落点插入位置**（同组=左右排序、跨组=按位插入，插入线指示）、标签条以下边缘 28%=方向拆分、中心=移入组末尾、拖出主窗=浮动；同组排序走模型 ReorderPanel+增量摘插，不整树重建）；Ctrl+Alt+方向=拆分、Ctrl+Alt+F=浮动。
- **工作台数据源切换（2026-10-07）**：2D/3D 工作台工具栏「数据源」下拉——列出总线 GetTopics() 中最新载荷类型匹配的主题（CloudPayload/ImagePayload），切换=退订旧数据主题+订阅新主题（3D 先清空当前实体；控制订阅 cloud/removed 与 sel/* 固定不切换）；约定默认主题（cloud/merged、image/stitched）恒在列。发布侧（发布点云/发布图像的主题参数）与订阅侧（数据源）均可配，多主题分流成为完整工作流。
- **算子库栏目排序（2026-10-07，按「面板」列表架构重构）**：Content 结构稳定（Rebuild 只重填内部列表——旧实现 `Content=BuildContent()` 连搜索框一起冲掉的潜伏 bug 一并消灭）；容器级按下/移动/释放状态机（capture 在控件上，命中表驱动——栏目头与算子行手势互不干扰）；栏目头移动>5px=排序拖拽（落点横线）、原地点击=折叠切换；搜索过滤时禁用排序。渲染按 `config/op_order.json` 的栏目名顺序（未登记栏目按注册顺序续后，JSON 算子/新栏目自动兼容）。
- **快照对比（2026-10-07）**：2D/3D 工作台「📷 快照」→ 插件侧只读视图（ImageSnapshotView / CloudSnapshotView）经 `ctx.OpenDocument(title, view, null)` 开动态文档标签；数据=冻结引用共享（总线载荷约定不可变，零拷贝；3D 未传完实体按已揭示点数截取）+ 相机姿态继承；只保留查看导航（3D 上传走同步分块路径——多后台 GL worker 共享上下文曾死锁）；restoreKey=null 不进会话；插件 TrackSnapshot 登记，新快照时回收已关闭（不在可视树）快照的 GL 资源，Shutdown 全清。
- **点云上传**：后台线程 BeginJob 一次性完成（NativeMemory 原生暂存 → Marshal.Copy → GL.BufferData 从原生指针），UI 线程零 GL 数据操作；UI 侧 SetRevealed 揭示步进做渐进显示；同步回退路径（无后台）保留分块 BufferSubData。

## 已定案已实施（领域 1-4 全部落地）

1. **分屏（自由拖拽 VS Code 式）**：SplitNode 树（Core）+ 三阶段——①树+右键拆分（左/右/上/下/移到下一格）②拖拽（**标签条落点=按位插入/组内排序**、标签条以下跨组/方向拆分+预览高亮+Esc 取消）③拖出主窗=浮动。**同面板两格默认禁止**（对比面板声明 AllowMultipleInstances）。
2. **会话持久化**：退出写 `config/session.json`（窗口几何/分屏树/侧边栏/动态页清单/浮动窗清单含几何），启动自动恢复（动态页经注册工厂按恢复键重建；浮动窗按会话原位浮出，doc 键与分屏树同样经恢复键翻译）；**视图菜单「默认布局」** = 清 session 回初始态。恢复后 _activeLeaf 重置到恢复树（防游离叶）；几何保存跳过最小化态（-32000 防御）+ 恢复钳制。
3. **面板浮动**：标签右键「浮动窗口」/ Ctrl+Alt+F / 拖出主窗边界松手 = 独立窗口（`win.Show(owner)` 随主窗最小化/关闭）；「⇲ 固定到主窗」拖回。**浮动态+几何进会话**（Closing 先存后关浮动窗；恢复时树里没有的键按 Floating 清单浮出）。「面板」列表栏目顺序持久化在 `config/panel_order.json`（行拖拽排序，按插件粒度）。
4. **非托管载荷**：后台 BeginJob 一次性 NativeMemory 原生暂存 → GL.BufferData；同 Id 更新先释放上一帧原生缓冲；DeleteEntityGl/Dispose 释放（防泄漏）；BeginJob 顺带 O(n) 精确边界 → UploadCompleted 回传（UV 映射/jet 色域正确性）。

**剩余优化**（真实 1 亿点数据接入时按需）：算子直产原生（消托管中转 2.4GB 峰值）、拾取改读原生（消托管稳态数组）、总线 IDisposable 替换释放（当前载荷为托管，机制未启用）。

## 官方组件（不可卸载，产品基线——用户定案）

- **NodeGraph（算法流水线）**：官方插件。框架代码零类型引用（保持架构纯净、可整体替换），但属产品基线——`config/host.json` 的 `requiredPlugins` 声明，启动/F5 重载后校验，缺失 → 日志 ERROR + 状态栏警示（框架照常启动，其余插件不受影响）。F5 热更新照常（不可卸载 ≠ 不可升级）。
- **算法配方侧边栏（算法文件管理，2026-10-07 由「算法」改名——产品语义：算法=保存的配方）**：宿主内置视图（编译进程序集 = 天然不可卸载）。直接扫 config/algorithms、发工作流主题；与流水线插件只经总线交互（零类型引用）。
- host.json（JSONC）由 Core 实际读取：`pluginDirectory`（插件目录覆盖）、`autoOpenPlugins`（自动开面板开关）、`requiredPlugins`（官方插件清单）。

## 预览面板契约（Sdk.Ui.PreviewSurfaces）

插件 UI 可在算子/节点卡片内嵌 3D 点云预览，而不依赖 Cloud3D 插件：

- **Sdk.Ui**：`ICloudPreview`（Control + SetCloud(CloudPayload) + IDisposable）、`PreviewSurfaces.CloudPreview` 工厂槽位（默认 null）。
- **Cloud3D 插件**：Initialize 时注册 `PreviewSurfaces.CloudPreview = payload => new MiniCloudPreview(payload)`（Cloud3DView embed 模式：不订阅总线、隐藏信息栏）。
- **NodeGraph 插件**：draw.3d 算子卡片经工厂创建内嵌预览；draw.2d 直接用 Image。执行时 `ctx.Preview(载荷)` → `GraphExecutor.PreviewReady` 事件 → 预览槽按载荷类型分发。
- 卸载 Cloud3D 后 draw.3d 卡片自动退化为占位（工厂为 null），不崩溃。

**框架时序不变式（BUG-060）**：`PluginManager` 必须先 `UiContextCreated?.Invoke(ctx)` 再 `instance.Initialize(ctx)`——宿主的事件订阅挂在 UiContextCreated 里，Initialize 期间触发的注册事件（如 RegisterDocumentFactory）必须有订阅者在场。

## 分发前清单（给第三方前必须，当前内部用可暂缓）

- 插件/宿主版本契约：插件声明 MinHostVersion，加载时校验（Sdk 接口演进后旧插件二进制兼容性）。
- 插件开发指南：从零写一个插件的 step-by-step（含 CopyToComdll/原生依赖/总线接入）。
- 配置文件版本迁移：session/pipeline/host.json 加 version 字段（当前解析失败静默回默认）。

## 触发条件预留（到条件再建，勿提前）

- **GL 上下文池**：第 3 个 GL 插件出现时（当前 Image2D + Cloud3D 主 + Cloud3D 上传共 3 个，自开自管）。
- **总线大帧内存预算**：多主题大帧并存超预算时丢最旧帧。
- **多屏不同 DPI**：GL 回读坐标换算仅单屏验证过，用户报告多屏异常时处理。
- **算子直产原生载荷**：1 亿点级数据接入时（当前峰值含托管中转 ≈2.4GB）。
- **拾取改读原生缓冲**：消托管稳态数组（当前拾取读托管 Pts）。

## 运行时容错与质量（已落地）

- **异常围栏**：App 三层钩子（Dispatcher/AppDomain/TaskScheduler）——UI 线程异常 `e.Handled=true` 吞掉防崩溃，同步送日志面板（App.UiLog）+ output/error.log。
- **ALC 卸载验证**：F5 重载卸载后 WeakReference 检查回收完整性，未回收 → 日志 WARN 点名（典型原因：插件总线/事件未退订）。实测全部回收 ✓。
- **Core 单测**：tests/ComUI.Core.Tests（`dotnet test`）25 例——DataBus（最新帧/晚订阅/退订/隔离）、HostConfig（JSONC/缺省/容错）、GraphModel（序列化加载回环/连线重映射/粘贴/锁定/改名持久化）、SplitTree（拆分/移动/回收/退化/单组不变式）、SplitJson（会话序列化回环）；曾当场抓出 JsonToObject 数字参数 int→double 真 bug 与 ReplaceInParent 漏回填 Parent 真 bug。

## 开发约定

1. **跨平台**：只允许 Avalonia/.NET API，禁止 WPF/Win32 专属调用（Ubuntu 同源运行）。
2. **线程**：所有 UI 触碰必须经 `Dispatcher.UIThread`；宿主服务层内部自行封送，不信任调用方。
3. **新面板**：插件 `GetPanels()` 注册，懒创建单实例；交互组件先查 [CONVENTIONS.md](CONVENTIONS.md) 复用清单。
4. **大帧**：图像/点云走总线不做多余拷贝；亿级点云渲染优化见 M2/M3 里程碑与 BUGS.md。
5. **错误**：可预见的失败带上下文抛出（解开 TargetInvocationException）；宿主统一记录到日志面板。
6. **每修一个 bug**：按格式追加到 [BUGS.md](BUGS.md)，同类合并。
