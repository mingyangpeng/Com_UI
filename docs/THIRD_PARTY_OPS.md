# 第三方算子库插件开发规则

面向**第三方算法开发人员**：把自有算法 DLL 接入 ComUI 的「算子库」，成为算法流水线里可拖拽、可连线的节点。
通用插件开发说明见 [PLUGIN_DEV.md](PLUGIN_DEV.md)；本文只讲算子库交付物的完整规则。

> 参考实现：`comdll/algo/DemoStitching/`（DLL + `demo.node.json` + `demo.algo.json` 三件套）。

## 1. 交付物与目录

```
comdll/algo/<你的名称>/
├── YourAlgo.dll            # 算法程序集（net8.0，引用 ComUI.Sdk 或零依赖）
├── your.node.json          # ★ 节点算子声明（进算子库，必需）
├── your.algo.json          # 算法命令声明（进「算法配方」侧边栏，可选）
└── <依赖 DLL>              # 所有依赖必须自包含在本目录（宿主不做全局解析）
```

- 宿主启动与 **F5 重载**时自动递归扫描 `comdll/algo/**/*.node.json`，无需注册代码。
- 注册成功日志：`算子(JSON)已注册: <名称> <<id>> ← <文件名>`；失败会点名原因（缺字段/找不到程序集/类型/方法）。
- **Id 冲突规则**：`node.id` 与已有算子重复时**忽略并跳过**（日志可见），不会覆盖内置算子。

## 2. `*.node.json` 字段规范

```jsonc
{
  "node": {
    "id": "vendor.gamma",          // ★ 全局唯一；约定 <厂商/来源>.<名称>，避免与内置冲突
    "name": "伽马校正",             // ★ 算子库与节点头显示名
    "group": "2D 图像",             // 算子库分组栏目（新栏目自动出现在列表尾部，顺序可拖拽）
    "category": "处理",             // 节点头配色：源(绿)/处理(蓝)/输出(橙)
    "description": "…"              // 描述（悬停提示）
  },
  "runtime": "dotnet",              // ★ 当前仅支持 dotnet（native 请包一层 C# 壳）
  "assembly": "YourAlgo.dll",       // ★ 相对本 JSON 所在目录
  "type": "YourNs.Filters",         // ★ 类型全名（或短名）；方法可静态或实例（实例需无参构造）
  "method": "Gamma",                // ★ 入口方法（public/nonpublic 均可）

  "inputs": [                       // 输入端口：name 显示在节点上；bind=方法参数名（缺省=name）
    { "name": "输入", "bind": "input", "type": "image" }
  ],
  "params": [                       // 用户参数（节点卡片上可编辑）
    { "name": "gamma", "label": "Gamma", "type": "number", "default": 1.5, "min": 0.2, "max": 3 }
  ],
  "outputs": [                      // 输出端口：单输出可省略 from（直接用返回值）；
    { "name": "输出", "type": "image" }   // 多输出用 from 指定返回值属性路径（如 "Image"）
  ]
}
```

**端口类型**只有两种：`image`（图像链）/ `cloud`（点云链）——连线按类型匹配。
**参数类型**：`number / int / text / bool / enum（配 options）/ path`（宿主提供文件浏览）。

## 3. 方法签名绑定规则

执行器按**参数名**绑定（忽略大小写），优先级：

1. **输入端口**：`inputs` 的 `name`/`bind` 匹配的参数 ← 上游端口的产出对象（原引用传递，零拷贝）；
2. **框架注入**：`CancellationToken`（预留，当前注入 `None`）、`IProgress<string>`（输出进日志面板）；
3. **用户参数**：`params` 的 `name` 匹配的参数 ← 节点卡片当前值（自动转换 int/float/double/bool/enum 等）；
4. 其余参数：有默认值用默认值，否则报「无法绑定方法参数」。

**返回值**：

- 同步返回或 `Task`/`Task<T>`（自动等待并取 `Result`）；
- **单输出**且 outputs 未写 `from` → 返回值即端口产出；
- **多输出** → 按 `from` 属性路径从返回值取（支持 `"Result.Image"` 点分路径）。

**参考签名**（`DemoAlgo.Core`）：

```csharp
// 单输出：返回值直接作为输出端口
public static ImagePayload Gamma(ImagePayload input, double gamma, CancellationToken ct);

// 多输出：返回对象的属性按 from 取
public static StitchResult Stitch(int points, double voxel, string mode);
public sealed class StitchResult
{
    public ImagePayload Image { get; init; }
    public CloudPayload Cloud { get; init; }
}
```

## 4. 数据类型契约（ComUI.Sdk）

节点间流动的推荐载荷（引用 `ComUI.Sdk`，与内置算子互通）：

| 类型 | 字段 | 约定 |
|---|---|---|
| `ImagePayload` | `Width, Height, PixelsBgra, Source?, Id?` | **BGRA32** 字节序，长度 = W×H×4 |
| `CloudPayload` | `Id?, Points, ColorsRgb?, Count, Source?` | `Points` = XYZ 三元组 float（长度 = Count×3）；`ColorsRgb` 逐点 RGB（缺省由查看器按高度伪彩） |

**红线**：载荷数组**发布/产出后视为不可变**（下游零拷贝引用共享）；`Id` 是点云实体键（同 Id 重发=更新，空 Id 落"点云"）。

## 5. 执行模型

- 流水线按**拓扑序**执行，每个节点在**后台线程**调用——方法内可长时间运算，**不要碰 UI 控件**；
- 异常：抛出即节点红点 + 错误信息（悬停可见）+ 流水线中止，请抛带人话 message 的异常；
- 日志：`IProgress<string>.Report(...)` 或返回前自行记录（进日志面板，前缀 `[算子名]`）；
- 取消：`CancellationToken` 参数为预留位（当前注入 `None`），长任务请自行控制时长或分块；
- 自动运行开启时，改参数/连线会自动重跑（默认关，手动「▶ 运行」）。

## 6. 数据如何到达工作台（重要）

**JSON 算子的 outputs 只流向下游端口，不会自动上总线。** 要在 2D/3D 工作台显示结果，
流水线末端必须接内置的**「发布图像」/「发布点云」**节点（主题参数默认 `image/stitched` / `cloud/merged`，
工作台即订阅它们；多主题分流见 PLUGIN_DEV.md §总线）。

```
你的算子 ──输出──→ 发布图像(主题 image/stitched) ──→ 总线 ──→ 2D 工作台显示
```

## 7. 接入验证清单

1. DLL + node.json 放入 `comdll/algo/<名称>/`（依赖同目录）；
2. 宿主内按 **F5**（或重启），日志出现 `算子(JSON)已注册: …`（失败看点名原因）；
3. 左侧「🧰 算子库」出现你的算子（按 `group` 栏目分组，可搜索）；
4. 拖到画布 → 连线 → 设参数 → 「▶ 运行」；
5. 末端接发布节点 → 工作台显示结果；节点状态点：黄=执行中、绿=成功、红=失败（悬停看错误）。

## 8. 规则红线（汇总）

1. **纯计算**：算子 DLL 禁止 UI 代码/控件引用（显示一律经总线由工作台负责）；
2. **自包含**：依赖全部放本目录；目标框架 net8.0；
3. **Id 命名空间**：`node.id` 用 `<厂商>.<名称>` 前缀防冲突（冲突=静默忽略）；
4. **数组不可变**：产出/发布的载荷数组不得原地修改；
5. **异常带信息**：`throw new Exception("人话描述")`——节点红点悬停直接显示；
6. **无副作用收尾**：算子无生命周期钩子（无 Initialize/Shutdown），资源在方法内自申请自释放；
7. **native 暂不支持**：C 导出函数请包一层 C# 静态方法壳再声明。
