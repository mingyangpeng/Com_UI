# ComUI 新架构设计（重构蓝图 v2.1 终稿）

> 2026-11-22。双视角（首席架构师+高级算法工程师）终审定稿。v2.0→v2.1 七处修订：数学栈统一 OpenTK（修不存在类型 System.Numerics.Vector3d）、ICloudWorkbench 渲染面收窄（IRenderBridge）、单事件过渡策略（WorkbenchRefresh）、刷新责任映射表、Origin 渲染语义界定（数据边界 vs 渲染管线）、SpatialIndex API 热路径化（计数重载/超集语义/扩环精确 KNN）、标量槽语义收窄（外部标量 vs 派生量）。现状参考见 ARCHITECTURE.md；历史记录见 ARCHITECTURE-DESIGN.md/TOPLEVEL-REDESIGN.md。B1 已验收（PM 七项门），V3 执行中。

---

## 一、系统定位与设计目标

**定位**：单机工业检测桌面平台（2D 图像 + 3D 点云 + 83 算子流水线 + 表格），Avalonia 11 / .NET 8 插件化。

| # | 目标 | 可判定标准 |
|---|---|---|
| G1 | 点云数据模型统一 | IO→算子→工具台→渲染走同一个 PointFrame；桥接对拷代码 grep=0 |
| G2 | 变更可追溯 | 工作台实体变更全经 WorkbenchDocument.Apply(命令)；Undo=反向命令 |
| G3 | 交互与模型分离 | 4 个 Controller 只依赖 ICloudWorkbench（渲染面收窄为 IRenderBridge），fake 可单测 |
| G4 | 插件边界硬化 | 契约单份 Sdk + Domain 单份共享（ALC 清单**只增 Domain 一项**、逐字评审）；插件目录无宿主程序集副本 |
| G5 | 全程可发布 | 每批：测试全绿 + 22 键冒烟 + UI 截图零变化 |

**不变式**：45 键位表 / GlCloudRenderer 对外 SetXxx API / 配置 JSON 格式 / comdll 部署布局 / 已验收 UI 行为。
**范围自限**：Domain v1 = 点云域（帧+IO）。Image2D/Table 数据模型维持插件本地；工作台文档/命令不入 Domain（无第二消费者，晋升路径已留）。

## 二、总体分层架构

```
┌──────────────────────────────────────────────────────────────┐
│  ComUI.Host（壳）：启动/主窗/Tab/会话布局恢复/装载入口        │
├──────────────────────────────────────────────────────────────┤
│  UI 插件层                                                   │
│  Cloud3D: 组合根 View(实现 ICloudWorkbench)                  │
│           + Workbench(Document/ICommand/Commands)            │
│           + Controllers + Modes + Render                     │
│  NodeGraph: 图模型/编辑器/算子声明（产出 Sdk 载荷，无 Domain）│
│  Image2D / Table / DemoPreview（数据模型各自本地）           │
├──────────────────────────────────────────────────────────────┤
│  ★ ComUI.Domain（新）—— 平台共享的点云数据契约与机制         │
│  Points(PointFrame/Schema/SpatialIndex) / IO(FrameBuilder+   │
│  readers/writers)；仅 BCL + OpenTK.Mathematics（唯一数学栈） │
├──────────────────────────────────────────────────────────────┤
│  ComUI.Sdk（契约+共享工具单份）：IPlugin/IBus/NativeAlgo/     │
│  AppPaths/Ui 原语/CloudPayload 载荷契约                      │
├──────────────────────────────────────────────────────────────┤
│  ComUI.Core（宿主服务）：PluginManager/PluginLoadContext/    │
│  DataBus/LogService/HostConfig（AppPaths 垫片）              │
└──────────────────────────────────────────────────────────────┘
```

**依赖规则**：
1. Domain 仅依赖 BCL + OpenTK.Mathematics；**不依赖 Sdk/Core/UI**；**全工程唯一数学栈=OpenTK.Mathematics**（禁混 System.Numerics 几何类型）
2. 插件依赖 Domain + Sdk；禁插件互引；NodeGraph 不依赖 Domain（消费侧适配）
3. Host 依赖 Core+Sdk+Domain；Core 依赖 Sdk；宿主服务不认识具体插件
4. Sdk 不依赖任何上层
5. 共享单份：PluginLoadContext 共享清单 {Sdk} → {Sdk, Domain}（唯一清单变更、逐字评审）；两程序集在插件 csproj 均 ExcludeAssets=runtime

**职责一句话**：Sdk 管"是什么"；Domain 管"点云数据与机制"；Core 管"宿主怎么跑"；插件管"用户看到什么"；Host 管"组装"。

## 三、数据结构设计（双视角终稿）

### 3.0 设计原则
1. SoA + 槽位化（新属性零契约改动）
2. **float32 局部坐标 + double 锚点**——Origin 只在数据边界（IO/算子/显示），渲染管线维持局部坐标现状（见 3.3）
3. 帧不可变 ⇒ 派生数据记忆化（空间索引/法线）——不可变设计的算法红利
4. 零拷贝=构造契约（Wrap 系列）
5. 一切复杂度有账（字节/点数/上限）

### 3.1 Domain·PointSchema（快慢双径）
```csharp
public sealed record PointSlot(string Name, PointSlotKind Kind, int Components, Type ElementType);
public sealed class PointSchema
{
    public IReadOnlyList<PointSlot> Slots { get; }
    public static PointSchema Xyz { get; }           // 最小 schema
    public bool Has(string slot);                    // 合帧/适配的兼容判定
}
```
快路径=帧上已知槽直读属性（热路径零字典开销）；慢路径=`GetBuffer<T>(slot)` 未知槽（强度/分类/时间戳）随加随用。

### 3.2 Domain·PointFrame + SpatialIndex（记忆化派生数据）
```csharp
public sealed class PointFrame
{
    public long FrameId { get; }                      // 全局单调：渲染桥判同帧跳过重传、命令历史判引用
    public int Count { get; }                         // 上限 int.MaxValue（>2³¹ 明确不支持）
    public PointSchema Schema { get; }
    public OpenTK.Mathematics.Vector3d Origin { get; }// ★ 地理锚点（数据边界语义，见 3.3）
    public Bounds3 Bounds { get; }                    // Build 缓存；空帧=空盒约定(Min≥Max)

    public float[] Xyz { get; }                       // 12B/点 恒有
    public byte[]? ColorRgb { get; }                  // 3B/点
    public float[]? Normals { get; }                  // 12B/点
    public float[]? GetScalar(string name);           // ★ 多槽：intensity/confidence（外部标量）
    public T[]? GetBuffer<T>(string slot);            // 通用槽慢路径

    // —— 记忆化派生数据（不可变帧的安全红利）——
    public SpatialIndex GetSpatialIndex(double cell); // 每帧最多缓存 2 个分辨率（LRU）
    public float[]? GetNormals();                     // 单条目缓存（现 N 键半径为规范参数）
}

public sealed class SpatialIndex                      // 体素哈希网格
{
    // ★ 热路径 API（v2.1）：
    public int QueryRadiusCount(Vector3 center, float r);             // 纯计数（SOR 主用例，零分配）
    public void QueryRadius(Vector3 center, float r, List<int> into); // 复用缓冲；格网超集，
                                                                      // 调用方须精确距离后过滤
    public void QueryKnn(Vector3 center, int k, List<int> into);      // 扩环搜索=精确 kNN
    public int VoxelKey(Vector3 p);
}
```
**收益对账**：RandomSample/Outlier/Normals/Regist 四工具手搓网格+暴力邻域 → 统一一处；SOR 百万点计数从"每点一分配"变零分配。
**标量槽语义收窄（v2.1）**：只装**外部带来的**标量（强度/置信度）；几何派生量（高度）走着色器现算（现状已支持），不入槽不占内存。

### 3.3 渲染语义界定（v2.1 关键修正：Origin 不进渲染管线）
- **数据边界**：IO 导出/算子交换/面板坐标显示用 `Origin + local`（double 精度）
- **渲染管线**：维持局部坐标 float 现状——相机 target 以 `worldTarget − Origin` 折算局部系
- 理由：shader 内做 Origin+T·local 会让 float32 在 10⁶ 坐标上重新抖动（等于重写渲染器，违反冻结承诺）；相机相对渲染列为远期项，仅当亚毫米抖动实测出现才启动

### 3.4 Cloud3D·工作台类型 + ICloudWorkbench（G3 判定物，渲染面收窄）
```csharp
public sealed class EntityView
{
    public string Id { get; }                     // 文档内唯一（Do 前置校验）
    public PointFrame Frame { get; }              // 换帧=一次引用赋值
    public double[] Transform { get; set; }       // 4x4 视图级（可逆）；工具输出=烘焙进新帧（不可逆）——严格分离
    public DisplayStyle Style { get; set; }
    public SelectionMask Selection { get; }       // null=全未选；换帧清空（显式设置除外）；变换/样式保留
    public string Source { get; }
}
public sealed class SelectionMask
{
    public int Version { get; }                   // 驱动面板/渲染刷新（替代 5 处手动调用）
    public IEnumerable<int> SelectedIndices();    // O(n) 枚举选中索引
}

public sealed class WorkbenchDocument
{
    public IReadOnlyList<EntityView> Entities { get; }   // 顺序=树序
    public event Action<DocumentChange>? Changed;        // ★ 粒度化
    public void Apply(ICommand cmd);                     // Do+前置校验+入历史
    public void Undo(); public void Redo();
}
public readonly record struct DocumentChange(ChangeKind Kind, string EntityId);
// Kind: Added/Removed/FrameReplaced/Transformed/StyleChanged/SelectionChanged/Renamed

public interface ICommand
{
    string Description { get; }                   // 状态栏/Undo 菜单文案
    void Do(WorkbenchDocument doc);               // 前置校验失败抛 InvalidOperationException
    void Undo(WorkbenchDocument doc);
}

// ★ 渲染面收窄（v2.1）：Controller 永不接触 GlCloudRenderer 具体类 → fake 单测可做
public interface ICloudWorkbench
{
    WorkbenchDocument Document { get; }
    IRenderBridge Render { get; }                 // 渲染操作子集
    void SetStatus(string message);
    void RequestDraw();
    EntityView? WorkingEntity { get; }            // 树联动优先
    event Action? WorkbenchRefresh;               // ★ 唯一刷新事件：阶段 3 前内部由 CloudsChanged
}                                                 //   供能，之后切 Document.Changed——Controller 零改动
public interface IRenderBridge
{
    void UploadFrame(EntityView e);                  // FrameReplaced/Added
    void RemoveVisual(string id);                    // Removed
    void ApplyTransform(string id, double[] m);      // Transformed
    void ApplyStyle(string id, DisplayStyle s);      // StyleChanged
    void ApplySelection(string id, SelectionMask m); // SelectionChanged
}
```

### 3.5 命令清单（与 3 拦截点 1:1）
| 拦截点 | 命令 | Undo 语义 |
|---|---|---|
| PushRemoveUndo L942 | RemoveEntityCmd | 重新插回同一 EntityView 引用 |
| PushAddUndo L2366 | AddEntityCmd | 按 Id 移除 |
| Transform 快照 L2434-2438 | TransformEntityCmd | 恢复 before 矩阵 |

派生（默认现行为）：ExtractSelectionCmd / MergeCmd / SetStyleCmd。**禁止**：宏命令/粒度重设计。

### 3.6 不变式与资源账
1. 帧冻结后任何路径不可变数组；Wrap 路径"源缓冲不再变更"由构造契约锁定
2. 文档变更只经 Apply；Id 唯一；命令 Do 前置校验失败抛类型化异常
3. 帧共享不拷贝：内存 O(变更数) 而非 O(点数)
4. 资源账：≈31B/点 → 1M≈31MB、10M≈310MB（桌面实用上限 ~50M/帧）；空间索引 ~O(n)、每帧 ≤2 个分辨率 LRU；空帧合法（空盒约定），算法必须显式处理 Count=0

### 3.7 合帧策略（对齐 CC 惯例）
Xyz 恒并入；可选槽取交集；ColorRgb 缺失源填白；Scalar 缺失省略。填白策略属产品决策，一行可改。

## 四、子系统设计

| 子系统 | 设计 |
|---|---|
| 插件生命周期 | 内核不动；部署核验入冒烟；ALC 清单变更须 PM 评审 |
| 类型化总线 | BusTopics 注册表（7 主题+载荷类型声明）+ 单测锁映射（阴性验证过审模式） |
| 命令与撤销 | Apply 单入口；CloudUndo 快照栈 D3 退役 |
| IO 管线 | readers→FrameBuilder→帧（Xyz 先行，Ply/Las/Csv 逐个迁）；writer 从帧导出（LAS 语义保持） |
| 渲染对接 | **刷新责任映射表**（v2.1）：FrameReplaced→UploadFrame / Removed→RemoveVisual / Transformed→ApplyTransform / StyleChanged→ApplyStyle / SelectionChanged→ApplySelection / Added→UploadFrame+树刷新——桥接歧义消除 |
| 算子执行 | 83 算子零改动；Cloud3D 侧 PayloadAdapter（载荷→FrameBuilder→帧），字段映射单测 100% |
| 配置与会话 | CloudConfigStore 原子写；坏 JSON 回默认语义不变；不做文档磁盘持久化 |
| 错误与诊断 | Domain 抛类型化异常（InvalidPointFile 等）零日志依赖；边界打点→LogService |

## 五、线程模型（三域）
| 域 | 职责 | 边界 |
|---|---|---|
| UI 线程 | 输入/Controller/命令 Apply/绘制调度 | 一切文档变更在此 |
| GL 上传线程 | BeginUpload 队列消费 | 只读帧数组（不可变保证） |
| 管线线程 | NodeGraph 防抖自动运行 | 结果经总线 uiThread:true 回 UI 再 Apply |

## 六、目标目录结构
```
src/ComUI.Domain/          Points/  IO/
src/Plugins/ComUI.Plugin.Cloud3D/
    Workbench/             Document/ICommand/Commands(6)/EntityView/SelectionMask/PayloadAdapter
    Cloud3DView.cs         组合根 ~1300 行（实现 ICloudWorkbench）
    Controllers/           Keyboard/Toolbar/ParamsPanel/ToolRunner（消费接口）
    Modes/ Render/         Measure/Regist + GlUploadQueue
    （保留）CloudXxx 纯函数/ToolIcons/ToolParamDialog/CloudTreePanel
tests/ComUI.Core.Tests/    Core/ Cloud3D/ NodeGraph/ Domain/
```

## 七、施工路线图
| 批次 | 内容 | 状态 | 验收门 |
|---|---|---|---|
| B1 地基 | AppPaths 下沉+垫片/3 hack 清零/测试归置 | ✅ PM 七项门通过 | — |
| V3=B2 | CloudToolbarController（≈400 行） | 执行中 | W1 验收点 grep+133 全绿+行为测试+View ≤3467 |
| V4 | CloudConfigStore 原子写 | 待派 | 断电场景单测+格式零改 |
| B3/B4 | ParamsPanel 刀 / ToolRunner 刀+ICloudWorkbench 定型 | 待派 | fake 单测+17 工具守卫 grep |
| B5 可选 | MeasureMode/RegistMode | 视情 | 模式行为测试 |
| D1 | Domain 骨架（帧+SpatialIndex+IO）+ ALC 清单增 Domain（PM 评审） | 入口=B 批完成 | 域单测 ≥10（含索引阴性验证）+UI 零改动+部署核验 |
| D2 | 导入切换（XYZ 先行） | — | 双截图逐像素 diff（硬验收）+导入性能基线实测校准预算 |
| D3 | CloudData 退役+外部标量着色首个落地 | — | grep CloudData=0+标量场截图+133+n 全绿 |
| 下一迭代 | NodeEditorView 2709 行同法 | — | 同 V3 模式 |

**附加纪律**：D1-D3 冻结 Cloud3D 新功能（压缩双模型窗口）；测试金字塔 Domain(纯·穷举)/Workbench(fake)/Controllers(fake)/UI(冒烟+截图) 四层。

## 八、治理
基线 133 语义锁定；Domain/Workbench 逐类单测（帧不可变/命令语义/适配器 100%/索引语义阴性验证）；性能预算 D2 实测校准（1M 导入<2s 草案值）；风险登记：双模型桥接→100% 映射单测+双截图 diff；拆刀回归→Edit 逐块+grep 落盘+禁脚本全量写回；真机干扰→前台自愈+pending 统一回填；范围膨胀→§1 范围自限+§3.3 禁止项。pending（终验前清零）：W1 组菜单截图；o/f/^i/l 满绿重跑。

## 九、附录：现状测绘
Host 3072/Core 1575/Sdk 1171/Cloud3D 8670（View 3767≈100 方法）/NodeGraph 7001（Editor 2709）/测试 133；CloudData 四合一 17 字段；Undo=15 字段快照+双 List×4、拦截点 L942/L2366/L2434；总线 7 字符串主题；约束：锁屏输入失效、SendKeys 前台+IME 自愈、GLSL ASCII。

## 十、alg 子仓库边界与同步管线（用户裁定：alg 维持子仓库形态）

**仓库边界**：alg（算法工程）是本仓库的 **submodule 子仓库**——独立版本、独立会话开发、由用户本人执行子仓库拉取/推送（执行侧遵守全局禁令：仓库操作由用户手动完成）。本仓库**只含部署产物，不含 alg 源码**。

**两条跨仓库通道（冻结，不加第三条）**：

| 通道 | 方向 | 内容 |
|---|---|---|
| ① node.json 声明 | alg → 本仓库 comdll/algo/ | 算子声明（id/参数/说明）+ Release DLL（本机 cmake --build --config Release 重建后拷贝） |
| ② 载荷契约 | 运行时 | NativeAlgo P/Invoke ↔ Sdk CloudPayload——Domain 与算子互不感知内部实现 |

**同步纪律**（已有机制承接）：
- alg 会话触碰 comdll/algo 下 node.json 后必跑 `python tools/check_node_json.py`（R1-R4 守卫；demo gamma 说明曾被冲掉 4 次）
- 参数说明"算子属性说明"交付契约：`NodeJsonRegistry_ParamDescriptions` 测试锁定（≥80 算子全量说明覆盖）
- 新算子上架流程：拉齐子模块 → 同步 node.json → 本机重建 Release DLL → 全量测试（近期提交已记录）

**架构含义**：G1"数据模型统一"的算子侧边界在 **PayloadAdapter（消费侧）**——alg 与 Domain 永不直接依赖，跨仓库耦合面 = Sdk 契约 + node.json 声明，两者均已冻结。alg 侧算子演进（拉齐后数量增长）对本仓库透明，只受契约测试约束。

## 十一、候补优化登记（触发条件驱动，非待办——触发才做）

| 候补项 | 触发条件 | 预估改动 |
|---|---|---|
| 分块帧（chunked frame） | 出现 >50M 点/帧的真实场景（当前实用上限） | Frame 拆 chunk + 渲染分块上传，中 |
| 相机相对渲染 | 局部系下实测亚毫米抖动（大坐标场景） | 渲染器相机矩阵改世界系，中 |
| 多视口共享文档 | 需要"多 3D 视图同步对比同一云" | ICloudWorkbench 多实例化（Document.Changed 多订阅已天然支持），小 |
| 跨插件命令路由 | 算子需要直接改工作台实体（替换选中实体类需求） | 总线→命令总线桥，中 |
| Image2D 域模型 | 2D 侧出现与点云域同型的撤销/文档需求 | EntityView/Document 上提 Domain（晋升路径已留），小 |
| 工作台文档持久化 | 用户实际因崩溃丢失工作状态 | Document 序列化 + 自恢复，中 |
| PointFrame 硬不可变 | 纪律级保证出现实际违反事故 | 数组改 ImmutableArray/ReadOnly 包装（性能代价需评估），小 |
| i18n 资源化 | 产品出海需求 | 全部中文字符串资源化，大 |

**已明确不做**（维持既有否决记录）：宏命令/事务、MVVM 重写、DI 容器、微服务、八叉树 LOD（触发=用户实测 >20M 点卡顿才复议）。
