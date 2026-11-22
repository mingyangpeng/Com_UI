# ComUI 架构优化设计（修订版 v3，2026-11-22）

> 对 v2 的三处修订：①插件引 Core → 改为 AppPaths 下沉 Sdk（避免独立加载上下文拷贝出第二份 Core.dll 的类型身份分裂风险）；②3767→700 目标不实 → 修订为分两批拆到 ≤1500；③渲染器内整降级顺带项。措辞全部去黑话。

## 一、一句话方案

代码骨架（插件化分层）是对的，不动。要做的是：**把 3767 行的巨型文件按职责拆成 4 个分工明确的小文件，再堵 3 个已知工程漏洞（路径手写×3 / 部署拷贝×5 / 配置非原子）。每批拆完 133 个测试必须全绿 + UI 截图对比一致。**

## 二、现状（实测）

| 模块 | 行数 | 健康度 |
|---|---|---|
| Host 壳 / Core / Sdk / 小插件 | ~7.5千 | 健康，不动 |
| **Cloud3D 插件** | 8670 | ⚠ Cloud3DView.cs **3767 行 ≈100 方法**装了全部职责 |
| **NodeGraph 插件** | 7001 | ⚠ NodeEditorView.cs 2709 行（下一迭代同法治） |
| tests | 133 个 | 健康 |

## 三、改完后的 Cloud3D 插件（目标形态）

```
Cloud3DView.cs            ~1500 行：拼装+事件接线+量测/配准两模式（模式状态机独立成批后 ≈900）
KeyboardController.cs     快捷键分发（45 键位表冻结）
ToolbarController.cs      工具条构建/拖拽排序/右键菜单/tool_groups.json
ParamsPanelController.cs  右侧面板：切片/色带/实体统计/量测清单/拾取历史
ToolRunner.cs             17 工具执行+守卫文案+参数对话框编排
GlUploadQueue.cs          【顺带】渲染器上传 worker 线程隔离
（不动）GlCloudRenderer.cs 对外 API 不变；CloudXxx 纯函数层原样
```

**控制器与 View 的边界**：新建接口 `ICloudWorkbench`（点云集合/渲染器/SetStatus/重绘/实体操作），View 实现它，四个 Controller 只依赖该接口——控制器之间互不认识，View 是唯一持全量状态的单点。控制器可对 fake 接口写单测。

## 四、施工批次（每批一晚量，独立可交付可回退）

### B1 路径统一 + 测试分组（半天，零行为变化）
- AppPaths.cs 从 Core **移到 Sdk**（Sdk 本就承载共享工具：FileLock/CollapsibleSection 同类）；Core 留同名转发避免宿主 8 文件大扫
- Cloud3DView.cs L412/L425、GlCloudRenderer.cs L115 手工推算 → `AppPaths.RepoRoot`（结构性根治 5 级数错类 bug）
- tests/ComUI.Core.Tests 内部加 Core/Cloud3D/NodeGraph 三个文件夹归置（**不改工程名**——改名零收益纯扰动）
- 验收：`grep BaseDirectory src/Plugins` 仅剩插件自身目录语义；133 全绿；启动冒烟

### B2+B3+B4 巨型文件三刀（每晚一刀）
| 刀 | 迁出内容 | View 剩余 |
|---|---|---|
| B2 | ToolbarController（工具条全套 ≈400 行） | ~3300 |
| B3 | ParamsPanelController（面板 ≈500 行） | ~2800 |
| B4 | ToolRunner（工具执行+守卫+对话框 ≈900 行） | ~1900 |
| B5（可选） | MeasureMode/RegistMode 两模式状态机 | ~1300 |

每刀验收：grep 锚点在位 + 133 全绿 + 新 Controller 行为测试 2-3 例 + 22 键冒烟 + 截图 UI 逐像素一致。**期间 Cloud3DView.cs 冻结新功能进入**（防 WBS 并行冲突重演）。

### B6 部署收敛 + 配置原子写（半天）
- Directory.Build.targets 单一定义 CopyToComdll（UI 插件五个，DemoAlgo.Core 独立不动）；各 csproj 只留一行部署名
- Core 新增 CloudConfigStore：temp+rename 原子写，cloud3d_view/tool_groups 迁入；既有 5 个容错测试语义不变
- 验收：部署目录与现状逐字节一致；断电场景单测（写中途残留 temp 清理）

### 渲染器内整（顺带，不单列）
GlUploadQueue 从 GlCloudRenderer 析出（744 行内上传 worker 是唯一并发点），对外 SetXxx API 冻结。

### 下一迭代
NodeGraph NodeEditorView 2709 行同法（Drag/Wiring/Menu Controller）。

## 五、冻结清单（本方案零触碰）

45 键位表 / GlCloudRenderer 对外 API / 配置 JSON 字段格式 / comdll 部署布局 / 已验收 UI 行为与手感 / 键位帮助浮层内容。

## 六、明确不做（YAGNI 否决，含理由）

DI 容器（构造注入够用）/ MVVM 重写（锁屏验收链绑定代码后置，收益不抵风险）/ Host 拆解（已 partial 化无痛点）/ 跨插件 GL 抽象库（两栈差异大）/ 微服务（单机桌面工具）。

## 七、风险与回滚

| 风险 | 缓解 |
|---|---|
| 拆分引入行为回归（记忆漂移链前科） | Edit 逐块+每块 grep 落盘；禁脚本全量写回；每批全量测试 |
| AppPaths 下沉后部署缺 Sdk 新版本 | Sdk.dll 随宿主加载同现状；构建后宿主加载日志验证 |
| 真机 UIA 验证受用户在线干扰 | 22 键冒烟已带前台自愈；截图挂 pending 清单统一回填 |
| 阶段中断留半拆 | 每刀独立可交付，批内不跨文件留悬空引用 |

## 八、对 PM 已批 V2 的增量说明

PM V2 规格为"插件引 ComUI.Core + tests 改名 ComUI.Tests"。本修订改为 **AppPaths 下沉 Sdk + tests 只加文件夹不改名**（理由见第三节首行与 B1），执行时按本修订同步 PM 复核口径。
