# 3D 点云工作台 — CloudCompare 对照终稿

> 对照 CloudCompare 日常使用集的补差状态归档（2026-11-20 收官）。后续清单自收官起冻结为归档项，不再开任务；下一迭代的范围裁定留待用户指令。

## 一、快捷键终版（45 键位）

**优先级链**（高→低）：文本框聚焦（全键交还编辑器，无例外）→ IME → 模式内 Enter/Back/Esc → 模式开关 → 全局映射。解析纯函数：`CloudHotkeys.Resolve`（45 动作），帮助浮层按 `?` 开关。

| 键 | 动作 | 说明 |
|---|---|---|
| M | 量测模式开关 | 折线累段连续拾取 |
| P | 多边形选取开关 | 左键加点/右键闭合 |
| R | 点对配准开关 | 两云交替拾取对应点 |
| L | 锁定竖直轴开关 | orbit 只水平旋转 |
| Esc | 模式取消 / 清选区 | 全态（模式开=退模式） |
| Enter | 模式确认 | 结束折线/应用配对/闭合多边形 |
| Backspace | 模式撤销 | 撤上一段/上一对/挂起点 |
| E / X | 提取框内 / 剔除框内 | 按当前选区生成新实体 |
| B | 盒裁剪 | 轴对齐包围盒 |
| Ctrl+A / Ctrl+I / Ctrl+D | 全选 / 反选 / 清选区 | 当前工作实体 |
| F | 聚焦当前实体 | 保持朝向 |
| Shift+F | 重置全景 | |
| 1–7 | 视角预设 | 等轴/前/后/左/右/上/下 |
| O | 透视 ↔ 正交 | |
| 方向键 | 相机旋转步进 ±5° | 锁定竖直轴时 pitch 忽略 |
| Shift+方向键 | 平移步进 | 0.05×半径 |
| + / − | 点大小 ±0.5 | 含小键盘 Add/Subtract、Shift+OemPlus |
| Ctrl+滚轮 | 点大小 ±0.5 | 鼠标替代 |
| S / D / G / J / T / N / H | 随机采样 / 去噪 / 平面分割 / 合并 / 变换 / 法线 / 高度图 | 工具直呼 |
| Ctrl+O / Ctrl+E | 导入 / 导出 | |
| Q | 算子工具菜单 | 83 算子库 Point3D 直调 |
| Ctrl+Shift+C | 复制量测清单 | |
| ? | 快捷键帮助浮层 | |

**TextBox 守卫**：焦点在文本框时全键交还编辑器（3D 视图无宿主保存语义——Ctrl+S 例外在 N1 落地时删除，见 N1 验收记录）。**IME**：ImeProcessed 键不进入解析。

## 二、配置 JSON schema 与容错

### config/tool_groups.json
```json
{ "Order": ["视图", "选取", ...], "Hidden": [] }
```
- Order：组显示顺序（未登记组按默认序续后）
- Hidden：隐藏组（右键菜单隐藏；无字段=全显，向后兼容）
- 重置默认顺序：右键 → 重置（恢复 7 组默认序+全部显示）

### config/cloud3d_view.json
```json
{ "Stride": 4, "FarClip": 123.5, "ClipAxis": 2, "ClipLo": 1.0, "ClipHi": 5.0,
  "ColorMin": -1.5, "ColorMax": 9.5, "Projection": 1, "LockPitch": true }
```
容错规则：坏 JSON/缺字段 → 回默认；Stride 钳 1–10000；ClipAxis 钳 0–2；Projection 钳 0–1；ClipLo>ClipHi 加载时自动互换。变更即存（HookCommit afterApply / SelectionChanged / ApplyZClip / ApplyColorRange / AdjustPointSize / ToggleProj / ToggleLockPitch 尾部）。

## 三、能力清单终版（对照 CC 补差状态）

| CC 功能 | 本平台对应 | 状态 |
|---|---|---|
| 点拾取/Ruler | 点拾取+折线量测+清单复制（CSV/剪贴板） | ✓ |
| Segment（多边形） | 多边形选取+提取/剔除 | ✓ |
| Crop（Box） | 盒裁剪（轴对齐，预填包围盒） | ✓ |
| RANSAC Plane | 平面分割（多轮迭代） | ✓ |
| SOR filter | 离群点滤波（格网加速半径计数） | ✓ |
| Random/Uniform subsample | 随机采样（确定性）+ 体素抽稀（本地+算子） | ✓ |
| Apply transformation | 变换工具（T·Rz·Ry·Rx·S 组合）+ 数据树 4×4 | ✓ |
| Match centers | 质心对齐（双下拉） | ✓ |
| Point-pair registration | 点对配准（Horn 刚体） | ✓ |
| Fine ICP | 算子库「手动配准(ICP)」 | ✓ |
| Cloud-cloud distance | 算子库「点云高度差对比」 | ✓ |
| Rasterize | 高度图（jet+CSV+PNG→2D 联动） | ✓ |
| Cross-section | 三轴切片（GPU 过滤） | ✓ |
| Color levels | 色带域覆盖（属性面板） | ✓ |
| Compute normals + shading | 法线 PCA + Lambert 光照（强度滑块） | ✓ |
| PLY/XYZ I/O | PLY（ascii+binary±法线）/XYZ/CSV | ✓ |
| **LAS 1.2 I/O** | format 0/2（scale 0.001+offset 自动） | ✓ |
| Entity tree ops | 数据树（锁/复制/改名/显隐/样式/聚焦联动） | ✓ |
| Viewpoints | 预设 7 视角+快捷键 1-7 | ✓ |
| Full-screen | 宿主级 | skip |

## 四、后续清单（归档，不再开任务）

| 项 | 不做理由 |
|---|---|
| 网格渲染/mesh | 渲染引擎级（面片管线+索引缓冲） |
| 实体包围盒线框 | 需线渲染通道 |
| LAS 外格式（E57/PCD/PTS） | LAS 已覆盖行业主流交付 |
| 标量场+任意色带 | 载荷契约无逐点标量槽 |
| EDL/环境光遮蔽/八叉树 LOD | shader 与内存架构大改 |
| 通用 Undo/Redo | 跨实体快照栈，工程量大 |
| 旋转剖面/十字剖面 | 三轴切片已覆盖 |
| 视图设置导出/导入分享 | JSON 手拷即达成，UI 导入导出属过度设计 |
| C2C 本地化（免算子直算） | 算子库「点云对比」已覆盖 |
| 2D ROI 反向圈选回 3D | 需 Image2D 侧跨插件协议改动（SelectionPayload 反向通道） |
| 橡皮筋视觉统一（虚线→实线+填充） | PM 裁定接受现状，选区视觉语言不混 |

## 五、已知约束

| 约束 | 说明 |
|---|---|
| 锁屏 GL 上下文 | GLFW 窗口创建在锁屏会话失败——UI/工具条正常，GL 渲染需真机解锁 |
| SendKeys 锁屏失效 | 键位冒烟脚本（tools/cloud3d_hotkey_smoke.ps1）需真机执行回填 PASS/FAIL |
| GLSL ASCII 强制 | shader 源常量 CheckAscii（英文注释），中文注释会导致构建失败 |
| heredoc 禁用 | 追加 C#/PS 含 `\n` 字面量内容一律 Write 工具整文件重写（heredoc 转义破坏[CS1010 实测]） |
| alg 同步覆盖 demo.node.json | alg 会话目录同步会覆盖 DemoStitching（本地资产）——已 4 次冲掉 gamma 说明。根治：`python tools/check_node_json.py` 守卫（R1/R2 说明全非空、R3 gamma 哨兵、R4 双份一致；exit 0/1）。**alg 会话触碰 node.json 后必跑**；日常回归在 `dotnet test` 后追加 |
| PrintWindow 截图 | tools/shot_window.ps1（锁屏可用，UIA 断言配合=视觉验收链） |
