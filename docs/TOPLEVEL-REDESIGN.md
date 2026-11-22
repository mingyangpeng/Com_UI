# ComUI 顶层重设计：数据结构先行（v1，2026-11-22）

> 与 ARCHITECTURE-DESIGN.md（B1-B6 增量重构）的关系：B 批是"把现有房间打扫干净"，本方案是"重新画户型图"。**B 批先做、本方案分阶段长出来**，两者不冲突——B 批的 Controller/接口边界正是本方案的接入点。

## 一、现状数据结构的四个结构性缺陷（代码实证）

### 缺陷 ①：CloudData 四合一（数据/展示/选择/元信息焊死）
```csharp
// 现状：15 个字段，四种职责挤在一个类
internal sealed class CloudData {
    // 数据：float[] Pts / byte[]? Col / float[]? Normals
    // 展示：int ColorMode / float PointSize / Vector3 SingleColor
    // 选择：byte[]? Sel / int SelCount
    // 元信息：string Id / string? Source / bool Locked / double[] Xform / BoundsMin/Max / Count / Loaded
}
```
后果：换配色模式=改数据类；Undo 要拷全部 15 字段；树面板/属性面板/渲染器/工具全耦合读它。

### 缺陷 ②：Undo 靠"拷贝快照 + 共享数组约定"——约定不强制
```csharp
// 现状：EntSnapshot 浅拷贝，Pts/Col 引用共享，正确性靠"载荷不可变"的口头约定
public sealed class EntSnapshot { float[] Pts; byte[]? Col; float[]? Normals; double[] Xform; …15 字段 }
public sealed class UndoEntry { 4 个 List：UndoSnapshots/UndoDeletes/RedoSnapshots/RedoDeletes }
```
谁违反约定就地雷（TransformTool 曾因快照顺序错返工两次）。

### 缺陷 ③：点云数据无 schema——逐点标量槽不存在
Pts=float[] xyz、Col=byte[] rgb，全是裸数组。CC_ALIGNMENT 归档项"标量场+任意色带（载荷契约无逐点标量槽）"就是这个缺陷。加一个"按强度着色"要动：载荷契约/算子封送/渲染着色/IO 四层。

### 缺陷 ④：两套点云结构并存
NodeGraph 的 CloudPayload 与 Cloud3D 的 CloudData 结构相似但独立维护（手抄契约）；算子输出→工具台实体的桥接靠字段对拷。IO（PLY/LAS/XYZ/CSV）四个 reader 各自直写裸数组。

## 二、顶层目标：三层模型 + 命令层

```
┌────────────────────────────────────────────────────┐
│ Host（壳：装载/Tab/会话）            —— 不动          │
├────────────────────────────────────────────────────┤
│ UI 插件层   Cloud3D │ Image2D │ NodeGraph │ …       │
│   Controller（B 批拆出的交互层）＝模型的消费者        │
├────────────────────────────────────────────────────┤
│ ★ Domain 层（新，纯数据+纯函数，零 UI 依赖，全量可单测）│
│   PointFrame / PointSchema   —— 不可变点云帧         │
│   EntityView / DisplayStyle  —— 可变展示实体         │
│   WorkbenchDocument + ICommand —— 文档+命令历史      │
│   IoBuilders（PLY/LAS/XYZ → 帧构建器）               │
├────────────────────────────────────────────────────┤
│ Sdk（契约+共享工具）  Core（宿主服务） —— 不动        │
└────────────────────────────────────────────────────┘
依赖规则：Domain 不依赖任何 UI；UI 插件依赖 Domain+Sdk；
算子输出经适配器产出 PointFrame（NodeGraph 与工具台共用同一模型）。
```

## 三、核心数据结构（C# 草案）

```csharp
// ===== 1. Schema 化点帧：逐点属性=具名槽位（解锁标量场）=====
public sealed record PointSlot(string Name, PointSlotKind Kind);   // Xyz/ColorRgb/Normal/Scalar/…
public sealed class PointSchema { public IReadOnlyList<PointSlot> Slots { get; } }

// ===== 2. 不可变点帧：构造后内容不变，写操作=派生新帧（共享未变缓冲）=====
public sealed class PointFrame
{
    public int Count { get; }
    public PointSchema Schema { get; }
    public float[] Xyz { get; }                 // 恒在
    public byte[]? ColorRgb { get; }            // 槽位可选
    public float[]? Scalar { get; }             // ★ 新能力：逐点标量（强度/高度/置信度）
    public float[]? Normals { get; }
    public Bounds3 Bounds { get; }              // 构造时算好缓存，O(1) 读
}

// ===== 3. 可变展示实体：引用帧 + 自己的状态 =====
public sealed class EntityView
{
    public string Id { get; }
    public PointFrame Frame { get; }            // 换帧=一次引用赋值（工具输出/命令）
    public RigidTransform Transform { get; set; }
    public DisplayStyle Style { get; set; }     // ColorMode/PointSize/SingleColor/Visible/Locked
    public SelectionMask Selection { get; }     // 独立于帧（帧不可变，选择可变）
}

// ===== 4. 文档 + 命令：所有变更的唯一入口，命令即 Undo 单元 =====
public sealed class WorkbenchDocument
{
    public IReadOnlyList<EntityView> Entities { get; }
    public event Action Changed;                // 渲染器/树面板/统计面板订阅刷新
    public void Apply(ICommand cmd);            // Do + 入历史（撤销栈免费）
}
public interface ICommand
{
    string Description { get; }                 // 状态栏文案/Undo 菜单名
    void Do(WorkbenchDocument doc);
    void Undo(WorkbenchDocument doc);
}
// 具体命令（每个 ~20 行，替代现在 15 字段快照+4 个 List 的 UndoEntry）：
// AddEntityCmd / RemoveEntityCmd / TransformEntityCmd / ExtractSelectionCmd / MergeCmd / SetStyleCmd
```

## 四、四个缺陷→四个收益（对账表）

| 缺陷 | 新方案 | 直接收益 |
|---|---|---|
| ① 四合一 CloudData | PointFrame（数据）+ EntityView（展示）分离 | 树/面板/渲染器只认自己该读的层；CloudData 类退役 |
| ② 快照式 Undo | 命令模式，Undo=反向命令 | RemoveEntityCmd.Undo=重新插回同一引用——**不再拷数组、不再依赖共享约定**；每命令 ~20 行 vs 现在 15 字段快照 |
| ③ 无标量槽 | PointSchema 槽位 + PointFrame.Scalar | "按强度/高度着色"开箱即用，CC 归档项解锁；新属性零契约改动 |
| ④ 两套结构+四 IO | 唯一 PointFrame + IoBuilders | 算子输出=帧构建器调用；新格式=一个 reader 类；NodeGraph/Cloud3D 桥接代码删除 |

## 五、迁移路径（与 B 批衔接，全程可发布）

```
阶段 0（已完成论证）：B1-B6 增量重构 → View 拆出 4 Controller + ICloudWorkbench 接口
                     ★ 这一步就是本方案的接入点：Controller 已不直接摸数据
阶段 1（~2 晚）：Domain 层骨架落地：PointFrame/Schema/EntityView/Document/3 个基础命令
                + IO 构建器（XYZ 先行）——与现有 CloudData 并存，不动 UI
阶段 2（~2 晚）：导入路径切 Domain（reader→PointFrame→EntityView→渲染器桥接）
                工具逐个命令化（Extract/Transform/Merge 三个先行，CloudUndo 退役一半）
阶段 3（~2 晚）：CloudData 退役、NodeGraph CloudPayload→PointFrame 适配器、
                标量场着色（第一个新能力落地=方案价值的实证）
阶段 4（下一迭代）：NodeEditorView 拆解 + Image2D 同模型评估
每阶段验收：133+n 测试全绿 + 22 键冒烟 + 截图对比（UI 行为零变化）+ PM 独立复核
```

## 六、代价与不做的事

**代价（诚实）**：阶段 1-3 约 6 晚；双模型并存期的桥接代码（一次性，阶段 3 清零）；WBS 冻结窗口（重构期间不开新功能）。

**不做**：数据库/文件持久化格式重定义（会话恢复维持现状）；多线程域模型（渲染上传队列已是唯一并发点，帧不可变天然线程安全）；EntityView 引入响应式框架（手写 event 够用）。

**风险**：桥接期双结构字段对拷 bug（缓解：适配器单测覆盖 100% 字段映射）；命令粒度争议（缓解：先按现有 Undo 拦截点 1:1 建命令，不重新设计粒度）。

## 七、决策请求

| 选项 | 内容 | 建议 |
|---|---|---|
| A | B 批先落地（B1-B6，~4 晚），阶段 1-3 接续 | ★ 推荐：B 批的接口边界=本方案接入点，串行风险最小 |
| B | 直接 Domain-first（跳过 B2-B4 拆刀，Controller 直接按新模型写） | 快但双线改动叠加，回归定位难 |
| C | 只做 B 批，Domain 层 indefinitely 缓 | 保守，标量场等新能力继续缺位 |

## 八、Domain 层详细设计（v2 增补，2026-11-22，全部锚点已 grep 实证）

### 8.1 命令清单（与现有 Undo 拦截点 1:1，不重新设计粒度）

现有拦截点全景（Cloud3DView.cs，共 3 处）：
| 现有拦截点 | 行号 | 对应命令 | Undo 语义 |
|---|---|---|---|
| PushRemoveUndo（树删除/Del/总线移除/切源） | L942 | RemoveEntityCmd | 重新插回同一 EntityView 引用 |
| PushAddUndo（AddToolEntity 工具新建） | L2366 | AddEntityCmd | 按 Id 移除 |
| TransformTool before/after 快照 | L2434-2438 | TransformEntityCmd | 恢复 before 变换矩阵 |

派生命令（现有功能免费升级为可撤销）：ExtractSelectionCmd / MergeCmd / SetStyleCmd（样式与合并在现版不入 Undo——命令化后可选开放，默认保持现行为）。
禁止项：不引入"宏命令/事务组合"（现状无此需求，YAGNI）。

### 8.2 类型化总线（轻量版：主题注册表 + 单测锁类型，零破坏）

现状：`Publish<T>(string topic, T payload)` 已泛型，但 7 个主题为字符串约定（cloud/merged、cloud/removed、image/stitched、algo/locked、algo/saved、pipeline/new、pipeline/open），`FramePublished` 是 `Action<string, object?>`。
设计：`BusTopics` 静态注册表——每个主题一个常量 + XML 文档声明载荷类型 + 一个单测断言"主题→载荷类型映射无重复无漂移"。不引入强类型 Topic<T> 包装（改 IBus 接口会动全部插件，收益不足）。

### 8.3 IO 构建器
```csharp
public sealed class FrameBuilder {
    public static FrameBuilder Create(int count, PointSchema schema);
    public void SetXyz(ReadOnlySpan<float> xyz);   // 其余槽位按需
    public PointFrame Build();                     // Build 内算 Bounds + 冻结不可变
}
```
- reader：XyzReader 先行（最简），Ply/Las 逐个迁（每个 reader = 解析→FrameBuilder，不再直写 CloudData）
- writer：从 PointFrame 导出；LAS 的 Scale/Offset 走参数对象（现 CloudLas 的 0.001/min 语义保持）
- 验收锚点：既有 CloudLas/CloudPointIO 测试全数保留语义

### 8.4 渲染器对接（桥接点唯一）
GlCloudRenderer.BeginUpload(id, loaded, bounds, pts, col, nrm) 吃裸数组——**帧不可变让"载荷不可变约定"升级为类型保证**（上传 worker 持有的数组不可能被并发改写）。桥接位置唯一：SetCloud/命令 Apply 后的 Changed 事件处理器调 BeginUpload；SetSelection/SetEntityStyle 签名不变。

### 8.5 SelectionMask
独立可变类（byte[] mask + SelCount + 版本号），挂 EntityView。全选/反选/框选 = 对 mask 的纯函数（现有 CloudSelection.SelectAll/Invert 平移）。版本号驱动统计面板与渲染器选中色刷新，替代现在散落的 5 个手动 UpdateStatsText 调用点。

### 8.6 明确不做（防范围膨胀）
Document 磁盘持久化（.ccdoc 会话文件）——会话恢复维持布局级现状；多线程域模型（帧不可变已天然线程安全）；响应式框架。

### 8.7 阶段门矩阵
| 阶段 | 入口条件 | 出口条件 |
|---|---|---|
| 1 骨架 | B 批完成（Controller+ICloudWorkbench） | PointFrame/Document/3 命令 + 单测 ≥10 个全绿；现有 UI 零改动 |
| 2 导入切换 | 阶段 1 过 | XYZ 导入走 FrameBuilder；导入截图与现版逐像素一致 |
| 3 退役 | 阶段 2 过 | CloudData 无引用（grep=0）；标量场着色上屏截图；133+n 全绿 |

