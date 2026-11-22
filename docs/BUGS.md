# 开发 Bug 记录

> 目的：同步记录开发阶段的代码错误，方便下次遇到类似症状时快速检索。
> 格式：编号 | 日期 | 症状（含原始报错） | 根因 | 修复 | 检索关键词。
> 新 bug 追加到对应分类末尾，编号递增。

## 目录

- [BUGS-archive.md：2026-10 上旬条目归档（10-01~10-05，BUG-001~BUG-053 及同期备忘）](BUGS-archive.md)
- [维护约定](#维护约定)
- 2026-10-05 起的叙事段与 BUG-054~BUG-086 条目按时间顺序排列于本文件其余部分

---

> → 2026-10 上旬（10-01~10-05）分类条目区（A/B/C/D 分类 BUG-001~BUG-043、BUG-044、轨道相机设计备忘）已归档至 [BUGS-archive.md](BUGS-archive.md)。


---

## 维护约定

1. **谁写**：每次调试修复后，由当次会话随手追加（ symptoms 抄原始报错关键行）。
2. **怎么查**：先按报错代码（CS/AVLN/MSB）或关键词搜本文档；再按"症状现象"搜。
3. **去重**：同类问题在原条目上补充新症状/新变体，不新开条目。
4. **星级**：★ 标记"症状与根因相距很远、极难定位"的坑。

---

> → BUG-045~BUG-052 条目及 M4 一期 / M3.2 二期 / M4-2 / M5 设备树一期 / M5 二期备忘（2026-10-04）已归档至 [BUGS-archive.md](BUGS-archive.md)。

## BUG-052 终版：滑杆整体重写为自绘控件（用户要求重写）
- **多轮模板层修复（限高/透明背景/行高）均未彻底解决，按用户要求重写**：节点卡片数值滑杆（Int/Number with Min/Max）弃用 Avalonia `Slider` 模板控件，改为 **自绘紧凑滑杆 `BuildCompactSlider`**：Canvas 上自绘 [未填充轨道 Border + 填充 Border + 圆钮 Ellipse(12px)]，点击/拖拽改值（PointerCapture + 位置反算 frac），回调走原 ValueChanged 语义（Params + 标签刷新 + ScheduleAutoRun 防抖）。
- **结构保证**：圆钮是自身 20px 高 Canvas 内的 12px 椭圆——不外伸、无模板裁剪、无相邻行遮挡，三类历史成因（负边距外伸/模板裁剪/相邻行覆盖）全部不可能复现。
- **交互语义**：按下即跳值、拖拽连续改值、防抖自动重跑（与原 Slider 一致）；数值标签同步。
- **验证状态**：部署于 comdll（NodeGraph DLL 10:13），实例运行中；圆钮渲染/拖拽手测交用户确认（自动化验证被环境反复打断）。
- **教训**：**模板控件在紧凑自定义容器里反复出显示 bug 时，与其继续打补丁不如用基础图元自绘重写——几十行代码消灭一整类模板布局问题**。
- **关键词**：自绘滑杆, 重写, BuildCompactSlider, Canvas, 圆钮, 轨道, 拖拽

---

## 滑杆终版：自绘+可编辑数值框（2026-10-05，用户反馈两点落地）
- **① 滑杆右端数值支持手动输入**：数值 TextBlock 改为 TextBox（右对齐、FontSize 11、Enter/失焦提交、越界回落当前值）；拖动滑杆时文本实时同步；提交后 setSliderV 联动圆钮/填充位置 + Params + 防抖自动重跑。
- **② 滑杆与数值间距**：滑杆 Canvas 右距 10px（原紧贴）。
- **BuildCompactSlider 签名变更**：返回 `(Control Control, Action<double> SetValue)`——SetValue 供外部（数值框提交）联动滑杆视觉；changed 仅用户交互时回调。
- **实测**：数值框输入新值 → 防抖自动重跑（日志新参数执行）✓；用户自行拖拽滑杆至 512 → 自动重跑 ✓（同一机制双向验证）。

---

## 点云树交互重做（2026-10-05，用户需求：去按钮改键盘+小绿点）
- **落地并全链路验证**：① 行首小绿点=显隐开关（显示=实心绿 0x66BB6A，隐藏=空心灰圈+行名变暗），点击双向切换（3D 联动显隐）✓；② 去掉 复制/删除 按钮，改键盘：**Ctrl+C 复制选中 → Ctrl+V 粘贴生成副本 → Del 删除选中**（行单击=选中并接管键盘，Esc 取消选中）✓；③ **双击行名=行内改名**（TextBox 行内编辑，Enter/失焦提交、Esc 取消、同名/空名拒绝）✓；④ 详情区含剪贴板状态行 + 每实体独立样式。DuplicateCloud 改为返回新 Id（粘贴后自动选中新副本）。
- **自动化测试坑（复现多次）**：① SendInput 的 Ctrl+字母组合键对 Avalonia 不可靠（KeyModifiers 读不到→快捷键不触发；Ctrl+鼠标则正常）——组合键验证改用 **CUA pressKey("ctrl+c")**（可靠）；② 系统双击需两次点击 <500ms——两个独立 powershell 进程间隔超时，必须**同进程 dblclick 模式**（input.ps1 已加）；③ 中文 heredoc 会被 GBK 折叠——PS1 脚本一律 ASCII 或用 Write 工具。
- **关键词**：点云树, 小绿点, 显隐, Ctrl+C, Ctrl+V, Del, 双击改名, 行内编辑, 键盘快捷键

---

## 点云树详情区 + 变换矩阵（2026-10-05，用户需求：树下方控制选中点云属性）
- **布局拆分（CloudCompare 式）**：① 点云树详情区（选中实体属性）：颜色模式 / 单色RGB（模式=单色时显示，每实体）/ 点大小 / **变换矩阵 4×4**（行主序、末列平移；应用变换/重置按钮；Enter 逐格编辑后点应用）+ 信息；② 右侧属性面板改为**视角控制**：投影 / 抽稀间隔 / 视角预设 / 重置视角（全局颜色模式/点大小/单色RGB 从属性面板移除，改由树详情区按实体控制）。
- **变换管线**：着色器加 `uniform mat4 uModel`（gl_Position = uMVP * uModel * aPos）；每实体 Matrix4（OpenTK 行向量约定，UI 行主序需转置存入）；CPU 拾取/框选/拾取标记/发布坐标全部先过实体变换（渲染即所见）；同 Id 更新数据时保留已有变换。
- **坑**：① 4 列网格列号 `c*2`（奇数列为 4px 间隔）——首版 `c==3?3:c*2` 把第 4 格挤进间隔列；② a11y 不暴露无边框 TextBox（矩阵格/数值框），自动化只能像素定位，环境被用户并行操作时不可靠。
- **关键词**：变换矩阵, uModel, 每实体属性, 详情区, 视角控制, 行主序, 转置

---

> → BUG-053（未解 / 修复记录，2026-10-05）已归档至 [BUGS-archive.md](BUGS-archive.md)。

---

## 插件关联契约（2026-10-05，用户方向：框架+插件、插件间声明式关联）
- **Sdk**：`PanelDescriptor.AssociatedPanels`（string[]?）——声明本面板与哪些面板 Id 逻辑关联（跨插件可用）。
- **宿主联动**（MainWindow.SwitchSidebarForAssociation）：激活 Document 面板时——① 自动切入声明关联它的 Tool 侧边栏；② 当前侧边栏工具面板声明了关联但不含激活面板 → 切回面板列表；③ 未声明关联的面板（算子库等）不受影响。
- **首个接入**：点云树 `AssociatedPanels = ["ui.cloud3d.view"]`——切到 3D 标签自动显示点云树，切到 2D/流水线自动切回。
- **验证**：切 3D 标签 → 侧边栏自动切点云树（文字 719px + 绿点 145px）；切 2D 标签 → 自动切回面板列表（绿点 0）✓。
- **语义**：插件注册（GetPanels）+ 声明关联（AssociatedPanels）+ 宿主联动——插件零宿主代码即可获得"跟随文档面板"行为；Cloud3D↔CloudTree 的宿主引用模式保留在插件内部（GetView），关联契约负责布局联动。

---

## 代码梳理（2026-10-05，用户要求：框架+插件、插件关联、去无用调试/废弃代码）
- **扫描结果**：全项目 43 个源文件 8563 行——无 Console 输出、无 TODO/FIXME、无死方法（XAML 事件处理器全部在用）、无废弃 API（AppDomain 仅用于全局异常兜底属正常）。
- **清理内容**：① Cloud3DView 死字段 `_pointSize`（样式改每实体后遗留）；② Image2DView 未用 `using System.Threading.Tasks`；③ **config/layout.json 删除**（无任何代码引用，属废弃文件）；④ 一次性调试脚本清理（38→3 个可复用工具移至 tools/：input.ps1 SendInput 注入 / shot.ps1 截图 / restore2.ps1 窗口恢复）；⑤ 旧验证截图清理（196→39 张保留关键证据）。
- **架构全景（梳理后）**：8 个面板注册（Cloud3D 双面板 view+tree、DemoPreview 双面板、NodeGraph 双面板 main+library、Image2D、Table）；插件关联契约 4 处引用（Sdk 定义 + 宿主联动 + Cloud3D 声明）；IPanelParamsProvider 1 个实现（Cloud3DView）。
- **验证**：清理后构建零错误，切 3D 标签插件关联联动正常（绿点 145px）。

---

## 算法栏目迁移（2026-10-05，用户需求：保存的算法应在⚡算法侧边栏，不在算子库）
- **变更**：① 算子库（NodeLibraryView）删除"算法"栏目及相关事件/字段/方法（AlgorithmRow/AlgorithmClicked/AlgorithmPlaced/AddAlgorithmEntry/_algorithms）；② ⚡算法侧边栏（AlgoSidebar）新增"流水线算法"区——扫描 config/algorithms/*.pipeline.json，单击发布 `pipeline/open` 总线事件；③ NodeGraphPlugin 订阅 `pipeline/open` → 展开到最近流水线页面；保存算法时发布 `algo/saved` → AlgoSidebar 即时刷新。
- **验证**：⚡侧边栏显示"流水线算法"+"我的算法"（a11y 确认）；单击"我的算法" → 流水线执行（日志"流水线执行完成"）✓；算子库中"算法"字样 = 0 ✓。
- **总线主题**：`pipeline/open`（string 路径）+ `algo/saved`（string 路径）。

---

## 算法三 Bug 修复（2026-10-05，用户报：双击改名 / 单击开新页 / 加载恢复连线参数）
- **Bug1 双击改名**：① 侧边栏行双击 → 行内改名（重命名文件）；② 节点卡片头部双击 → 行内改名（NodeInstance.Title，空/原名=还原算子名，序列化持久化）。
- **Bug2 单击开新页**：单击侧边栏算法 → 新标签页完整加载。
- **Bug3 加载恢复**：节点+参数+连线全部恢复。
- **验证（E2E）**：双击节点头部 → 改名框出现 → 输入"我的高度图"回车 → 标题替换；保存 → JSON 含 `"Title"`；双击侧边栏行 → 改名框存活 → 输入回车 → 文件改名（我的算法→重命名算法，验证后还原）；单击 → 新标签页 + 自动执行"5 节点/3 连线" ✓。

### 修复过程中发现的 5 个隐藏缺陷（全部修复+验证）
1. **OpenDocumentRequested 从未接线**：PluginManager.UiContextCreated 事件存在、HostPluginContext.OpenDocument 也触发事件，但 MainWindow 从未订阅 → 插件请求开动态页全部静默丢失。修复：MainWindow 构造订阅 UiContextCreated → HostPluginContext.OpenDocumentRequested → OpenDynamicDocument 创建标签页（Tag="doc" 区分面板页）。
2. **LoadFromJson 连线全丢（BUG-054）**：节点实例化后 Id 是新生成的 Guid，连线却按保存的旧 Id 匹配 → 全部失配被丢弃（运行报"输入未连线"）。修复：idMap 重映射（与 PasteFromJson 同模式）。
3. **SetAlgorithmsDir 死 API（BUG-055）**：NodeEditorView.SetAlgorithmsDir 定义了但全项目零调用 → 保存算法时 Path.Combine("", ...) 抛"empty path"异常。修复：OpenPage 统一调用 view.SetAlgorithmsDir(_algorithmsDir)。
4. **指针捕获未释放（BUG-056）**：节点拖动 PointerPressed 里 Capture(this)，OnPointerReleased 不释放 → 后续按下（含双击第二击）不再路由到卡片头部。修复：release 时 e.Pointer.Capture(null)。
5. **单击开页与双击改名竞速（BUG-057）**：单击 pipeline/open 异步开标签页会抢焦点，把刚出现的改名框瞬间 LostFocus 提交掉（表现为双击改名"完全无效"）。修复：单击延迟 300ms 派发（DispatcherTimer），第二击到达即取消——标准单击/双击消歧模式。

### 测试基建（tools/）
- **fgdblclick.ps1 新增**：ALT 击键解锁前台锁 + SetForegroundWindow + 前台校验（失败则最小化/还原兜底）+ 双击。CUA/UIA 交互后窗口常失前台，裸 SendInput 会被吞——必须先激活并校验。
- **input.ps1 修复**：rightdrag/leftdrag 模式原先按下前不 AbsMove 到起点（点击落在旧鼠标位置）——补 AbsMove(x1,y1)（与 ctrldrag 一致）。
- **坑**：PowerShell 5.1 脚本含中文必须 UTF-8 **带 BOM**，否则 here-string 解析崩（Write 工具默认无 BOM，需转换）。
- **CUA 经验**：① AX 树元素上限 ~400，超出按优先级裁剪（活动栏/侧边栏最先消失）——先关多余标签页再查侧边栏；② 元素索引在每次观察后重编号，观察与点击必须同 cell；③ UIA Invoke 触发 Click 事件而非 PointerPressed——测 PointerPressed 逻辑必须 SendInput；④ 树内"图标文本在按钮之后"= 文本的父按钮在前面。

---

## 算法跳转去重 + 锁定保护（2026-10-05，用户需求：已打开则跳转；锁图标禁止修改）
- **需求 A（已打开跳转）**：单击已打开的算法不再重复建页，直接跳转到该标签页。
  - 实现：① NodeEditorView 新增 `LoadedPath`（加载的文件路径）；② NodeGraphPlugin.OpenPipeline 按 LoadedPath 查找已打开页 → 找到则 `_ctx.OpenDocument(name, existing)`；③ MainWindow.OpenDynamicDocument 按内容控件引用去重（ReferenceEquals）→ 命中即 SelectedItem 聚焦，不建新页。
  - 验证：日志"算法 我的算法 已打开，跳转到该页面"，标签页数量不变 ✓。
- **需求 B（锁定保护）**：算法行右侧锁图标 🔒/🔓，锁定 = 只读打开禁止修改；可解锁或复制副本后编辑。
  - **持久化**：PipelineFile 新增 `Locked` 字段（GraphDocument.Locked 镜像，Serialize 写/LoadFromJson 读）；侧边栏 ToggleLock 用 JsonNode 原地改写 JSON。
  - **只读编辑器**（NodeEditorView._readOnly + SetReadOnly）：参数编辑器 IsEnabled=false、节点头部不拖/不删/不改名、端口不连线/不拆线、拖放/添加节点/示例图/清空全部拦截（日志提示）；保存按钮变「📋 复制副本」。
  - **复制副本**：以「原名 副本」保存解锁副本（Locked=false）→ 发布 pipeline/open 新标签页打开可编辑。
  - **解锁联动**：锁切换发布 `algo/locked`（路径）→ NodeGraphPlugin.OnAlgoLockChanged 重读文件 Locked → 同步所有 LoadedPath 匹配的已打开页 SetReadOnly。
  - 验证：锁点击 → JSON Locked=true + 已打开页日志"只读模式"+ 7 个参数框全部禁用 ✓；复制副本 → 副本文件 Locked=false/5节点/3连线 + 新页打开执行 ✓；解锁 → Locked=false ✓。
- **BUG-058（锁点击冒泡）**：Avalonia 的 Button.Click 是**冒泡路由事件**——行内嵌套锁按钮的 Click 会冒泡到行按钮，点锁同时触发"打开算法"。修复：锁按钮 Click 里 `e.Handled = true`。验证：点锁后无"请求打开动态页面"日志 ✓。
- **总线新主题**：`algo/locked`（string 路径）。

---

## 3D 性能收尾：屏显密度封顶 + 后台线程上传（2026-10-05，M3.2 遗留两项全部完成）
- **密度封顶（每像素至多 1 点）**：Cloud3DView.Draw 里有效抽稀间隔 = max(用户间隔, 交互上限, 密度封顶)；密度封顶间隔 = ⌈可见点数/视口像素数⌉（可见点 > 视口像素才生效，上限 ×1000）。信息栏标注「抽稀×N（密度封顶）」。随窗口 resize 自动重算（resize 触发 Draw）。实测：601万+6.5万点 ÷ 视口76万px → **抽稀×8（密度封顶）** ✓；小点云（6.5万+6.1万）无标记 ✓。拾取循环仍用 EffStride（不含密度封顶，精度优先）。
- **后台线程上传（GL 上下文跨线程）**：GlCloudRenderer 构造时用 `GLFW.CreateWindow(1,1,"comui-3d-upload", null, 主窗口WindowPtr)` 创建**共享对象命名空间**的上传上下文 + 专职 worker 线程（BlockingCollection 任务队列）。
  - **零拷贝**：worker `fixed` 指针直读总线载荷数组（契约不可变），无 scratch 拷贝；无逐点色时 worker 专用灰度缓冲（200）复用。
  - **零竞态设计**：① worker 每块上传完成才推进 e.Count，Draw 只读 [0,Count)（=已完成区），与写入区零重叠；② 同 Id 重发时 UI 线程 BeginUpload 只建壳（VAO+attrib 指针，微秒级），BufferData 预分配作为 BeginJob 入队 worker（FIFO 先于后续块）——消除 UI 重分配与 worker 写入的竞态；③ RemoveEntity 延迟删除：入队 DeleteJob，worker 先完成该实体在途块再删缓冲；④ 每块 GL.Flush() 推给驱动；⑤ 最终精确边界经 UploadCompleted 事件（worker 线程）→ 视图 Dispatcher.Post 回填（UV 映射/拾取需要精确值）。
  - **失败回退**：共享上下文创建失败或 worker 异常 → BackgroundUpload=false → 视图自动走原 UI 线程分块路径（行为与旧版完全一致）。
  - **实测**：6,012,006 点 3 块上传全程 UI 可交互、渲染正常（像素验证 33,180 亮像素）；重启后小云（6.5万+6.1万）渲染 15,134 亮像素与此前一致 ✓。
- **GLFW/线程要点（记坑）**：① OpenTK 4.9.4 的 `NativeWindow.WindowPtr` 就是 `GLFW.Window*`（可直接传给 CreateWindow 的 share 参数）；② CreateWindow/DestroyWindow 必须在 GLFW 主线程（=glfwInit 所在线程=UI 线程），MakeContextCurrent 可在任意线程；③ WindowHint 是全局状态，宿主此后无其他窗口创建故不回收；④ GL bindings 主线程加载一次即可（同驱动同版本上下文函数指针一致，worker 不重复加载避免与 UI 线程的静态写竞态）。

---

## 文件操作统一（2026-10-05，用户需求：文件名双击改名全 UI 统一 Esc/Enter；锁挂名字右上角；文件复制/粘贴/保存）
- **BUG-059（Esc 误提交）**：三处行内改名框（算法侧边栏行/节点卡片/点云树行）中，前两处的 Esc 处理只移除了输入框——**控件移除后 LostFocus 仍触发 Commit**，Esc 实际变成"提交当前输入"。点云树原本有 `_renaming` 守卫是对的。统一修复：`done` 标志守卫（Enter/Esc 处理后忽略后续 LostFocus）。统一语义：**双击进入改名（全选）→ Enter 确认 / Esc 取消 / 失焦确认；空名或同名 = 还原**。实测：输入 zzz/hello → Esc → 原名保留 ✓。
- **锁徽标重做（用户截图指定样式+配色）**：锁图标从行右侧独立列改为**挂在文件名右上角**（紧贴名字、上对齐小徽标）。**emoji 🔒/🔓 是彩色字体无法用 Foreground 着色** → 改为形状绘制简笔锁（Canvas：圆角锁体 + 锁环 Border；解锁=锁环抬起右移）。**配色（用户定）**：解锁=绿色 #7BC886，锁定=灰色 #A0A0A8。像素验证：解锁 56 绿像素 / 锁定 62 灰像素、零互串 ✓。
- **文件基础操作**：① 侧边栏行单击=选中（高亮 #2F333B）+ 打开；**Ctrl+C 复制选中的算法文件、Ctrl+V 粘贴为「原名 副本」**（解锁可编辑、重名自动递增 -副本N，发布 algo/saved 刷新）——实测生成"456 副本" ✓；改名进行中放行 TextBox 自身的 Ctrl+C 文本复制（_renameBox 守卫）。② **Ctrl+S / 保存按钮统一入口 SaveOrCopyAsync**：已加载文件的页面直接写回原文件（按钮文字变「💾 保存」）、新页面弹另存对话框、锁定页=复制副本——实测改分辨率 256→300 → Ctrl+S → JSON 落盘 ✓。③ 画布背景按下即聚焦（_editor.Focusable）——否则焦点不在视图内时 Ctrl+S 不可达。④ AlgoSidebar 加 LogMessage 回调（宿主注入日志），复制/粘贴有日志反馈。
- **测试坑（复用）**：锁徽标挂到名字右上角后**紧贴名字右侧的点击会命中锁按钮而非行**——自动化/手动点击行要用名字正中偏左的位置；CUA 交互后首个 SendInput 仍需 fgclick/fgdblclick（前台校验）且可能被吞需重试。
- **行等宽/锁对齐（用户报"长度不一体丑"）**：锁挂在名字右上角后，名字长短不一 → 锁位置参差。要求锁对齐一列 → 统一钉在行右端。**坑（Avalonia 布局链）**：侧边栏整条链（ScrollViewer→StackPanel→StackPanel→Button）都不横向拉伸子项——`HorizontalScrollBarVisibility=Disabled` 和 Button `HorizontalContentAlignment=Stretch` 都无效，行始终贴合内容宽度。**确定性方案：行 Width 显式跟随 ScrollViewer.Viewport 宽度**（LayoutUpdated 同步，减 _root 横向 Margin 16；滚动条出现/消失自动跟随）。实测三行全部 219px 等宽、三锁全部 x=242 对齐 ✓，锁点击在行右端正常切换 ✓。

---

## 分屏阶段 1（2026-10-05，SplitNode 树 + 右键拆分 + 键盘兜底）
- **交付**：① Core 分屏树模型 `ComUI.Core/Split/SplitTree.cs`（SplitLayout 持根引用：Split 拆分/MovePanel 跨组移动/RemovePanel 回收/FindLeaf/Leaves；不变式=容器≥2 子退化、空叶回收、同面板只在单组）；**22 例单测**（分屏方位/同方向兄弟/回收/退化/查找/单组不变式）。② UI 层：标签区改 SplitHost 网格（SplitNode 树递归渲染 + 每格 TabControl + 格间 GridSplitter）；标签右键菜单（向左/右/上/下拆分、移到下一格、关闭）；**Ctrl+Alt+方向 = 朝该方向拆分当前激活标签**（键盘/自动化兜底）。实测：算法流水线向右拆分 → 双格渲染（像素验证左右都有内容）、关闭右格 ✕ → 空格回收回单格 ✓。
- **修复链（三个连环坑，全靠单测+日志围栏定位）**：
  1. **SplitLayout.ReplaceInParent 漏回填 Parent**——包裹容器插入后 Parent=null，后续空叶回收链静默失效（RemovePanel 时 `leaf.Parent is null` 直接 return）。修复=替换时 `new_.Parent = p`。教训：**树操作"替换子节点"必须同步回填父指针**。
  2. **BuildGroup 事件挂接顺序**——`SelectedIndex` 在 `SelectionChanged +=` 之前设置，事件全丢（_activeLeaf 永不更新、属性面板不联动）。修复=先挂事件再设选中。教训：**控件初始化时事件挂接必须在触发属性赋值之前**。
  3. **重建视觉父级冲突（BUG-037 同源）**——RebuildSplitView 直接拆格重挂 TabItem，旧 TabControl 的内容/头呈现器仍持有视图 → "already has a visual parent"。修复=重建前先把所有 TabItem 从旧组 Items 摘除。
  另：Grid 行列定义字符串拼装曾抛 FormatException（原因未深究）——改为 **GridLength 对象 API 直接构建**（确定性，绕开字符串解析）。
- **自动化经验**：Avalonia 右键菜单（ContextMenu）合成输入打不开（SendInput/CAA 均不行）——键盘兜底入口（Ctrl+Alt+方向）既是真功能也是自动化测试通道；元素树 400 上限时右键菜单项会被裁剪查不到。

---

## 分屏阶段 2（2026-10-05，标签拖拽：跨组移动/方向拆分/预览高亮/Esc 取消）
- **交付**：按下标签头移动 5px 进入拖拽 → 全区半透明预览高亮（中心=整格移入、边缘 28%=方向半格拆分）→ 松手落位；Esc 取消；拖出窗口由 Pointer.Capture(主窗) 兜底。实测：边缘拖=右拆出新格（2D 图像@新格）✓、中心拖=跨组移动（算法流水线移入右组）✓、拖回原组中心=无操作 ✓。
- **实现要点**：拖拽候选在标签头 PointerPressed 记录（阈值检测放 Window_PointMoved，避免影响单击选中/✕ 点击）；捕获到主窗（e.Pointer.Capture(this)）保证拖出窗口仍收到事件；命中检测用 TabControl.TranslatePoint→组矩形；_groups（Leaf,Control）在 RenderNode 时重建。
- **调试经验**：拖拽"无效"不一定是 bug——**释放点在组中心区（rx<0.72）且源=目标组时"无操作"是设计行为**；先看坐标算 rx/ry 落在哪个区域再下结论（本次三连"失败"里两次是测试坐标落在中心区）。

---

## 会话持久化（2026-10-05，退出保存布局 / 启动恢复 / 视图菜单默认布局）
- **交付**：① `config/session.json`（窗口几何/侧边栏/分屏树/动态页清单），窗口 Closing 保存、启动自动恢复；② **文档恢复契约**（Sdk）：`OpenDocument(title, content, restoreKey)` + `RegisterDocumentFactory(docType, 键体→内容)`——插件 Initialize 注册工厂（NodeGraph 注册 "pipeline"=按算法文件路径重建页），宿主会话恢复时按恢复键 "<类型>:<键体>" 重建内容，**插件零宿主耦合**；③ 分屏树序列化在 Core（SplitLayout.ToJson/LoadFromJson，25 例单测含回环）；④ 视图菜单「默认布局」= 删 session + 关全部标签 + 新树 + AutoOpen。
- **doc 键翻译**：保存时 doc:N→恢复键（无键的未命名页自动丢弃）、恢复时恢复键→新 doc:N（TranslateDocKeys 递归替换 Panels 数组）——树模型本身不感知 doc 语义。
- **恢复顺序**：构造期恢复窗口几何（避免显示后跳变）→ Start 里插件加载后恢复（动态页工厂重建 → 分屏树载入 → 面板标签补建（插件缺失剔除）→ Rebuild → 侧边栏）；AutoOpenPanels 先跑（重复打开去重聚焦）。
- **E2E**：开算法页+拆分 → WM_CLOSE 优雅关闭 → session.json（sidebar=算法·docs=[i]·几何）→ 重启 → "会话已恢复（含分屏布局）" + 节点卡片×2（主面板+恢复页）+ 恢复页自动执行 ✓。
- **坑**：① `taskkill /F` 强杀不触发 Closing——会话保存验证必须 WM_CLOSE（PostMessage 0x10）优雅关闭；② 接口方法加可选参数=新签名，旧 2 参声明须删（否则实现类 CS0535）；③ AX 树 400 上限时恢复验证靠日志（"会话已恢复"+节点卡片计数）而非元素查询。

---

## 分屏阶段 3（2026-10-06，浮动窗口）
- **交付**：① 标签右键「浮动窗口」+ **Ctrl+Alt+F**（浮动当前激活标签，键盘/自动化兜底）+ **拖出主窗边界松手 = 浮动**；② 浮动窗（Owner=主窗，随主窗最小化/关闭，无任务栏按钮）带「⇲ 固定到主窗」工具条按钮；③ 固定回主窗 = 拆出视图 → 关浮动窗（_redocking 抑制清理）→ 回最近聚焦组。E2E：Ctrl+Alt+F → 浮动窗出现（"结果预览 — ComUI"@482,132 focused，主窗仍在）→ 点固定 → 浮动窗关闭、标签回主窗条 ✓。
- **坑**：① Avalonia `Window.Owner` 是 protected——**设 Owner 的公开入口是 `win.Show(owner)` 重载**（对象初始化器赋值 CS1540）；② 浮动窗包裹视图（Border→DockPanel→[工具条,视图]），固定回主窗时需拆出内层视图再挂标签（外层包装丢弃）；③ 浮动面板不占分屏格（模型 RemovePanel），OpenPanel 时若面板在浮动窗 → `win.Activate()` 前置而非选中标签。

---

## 非托管载荷 + 上传路径重构（2026-10-06，领域 4：原生内存稳态）
- **交付**：① `BeginUpload` 增加 `points/colorsRgb` 参数——后台 worker 的 BeginJob **一次性完成**：NativeMemory.Alloc 原生缓冲 → Marshal.Copy 托管载荷 → GL.BufferData 直接从原生指针 → 释放判定（同 Id 更新时先释放上一帧原生缓冲）；② UI 线程零 GL 数据操作——UploadTick 后台路径退化为 `SetRevealed(id, loaded)` 揭示步进（渐进显示纯 UI 侧推进，无 GL 调用）；③ 同步回退路径（无后台时）保留原分块 BufferSubData。
- **内存布局**：稳态 = GL 显存缓冲（由原生暂存喂入）+ 托管载荷数组（拾取用）；原生暂存在 BufferData 后即无用但随 Entity 存活至下帧/删除——后续优化点：BufferSubData 完成后立即释放暂存（当前随 Entity 生命周期）。
- **已删**：UploadChunkAsync（worker 分块队列路径——被一次性 BufferData 取代）、RevealGl worker 分支、_bgGray。
- **验证**：构建 0 错误；25/25 单测；E2E 点云渲染正常（20555 亮像素）✓。拖拽 E2E 因前台锁 + 会话恢复布局混叠未能复测——拖拽代码与阶段 2 验证版完全一致（未改动）。
- **坑**：① 接口加可选参数=新签名（旧 2 参声明必须删除，否则实现类 CS0535）；② NativeMemory.Alloc 返回 void*→nint 需显式转换；③ 前台锁验证（fgclick）在系统焦点被抢占时连续失败——SendInput 点击仍会送达窗口（按光标位置路由），验证性点击可继续但需确认落点窗口。

---

## 架构评审修复（2026-10-06，用户"再次 review"后 7 项发现全修）
- **修①（原生泄漏）**：DeleteEntityGl 只删 GL 对象不释放 NativePts/NativeCol——删一个大云泄漏数百 MB。修复=DeleteEntityGl + GlCloudRenderer.Dispose 均补 NativeMemory.Free。
- **修②（M4 回归）**：BeginUploadGl 不再逐块累积精确边界、UploadCompleted 不再触发 → 视图/渲染器边界永久停留在采样预估值 → 大云 UV 映射横向全偏。修复=BeginUploadGl 拷贝原生时顺带一遍 O(n) min/max → 写回 e.Bounds + 触发 UploadCompleted（视图回填 data.Bounds，拾取 UV/ jet 色域恢复正确）。
- **修③（会话几何）**：最小化态关窗时 Position=(-32000,-32000) 会存进 session.json → 恢复后窗口在屏幕外。修复=TrackGoodGeo（PositionChanged/SizeChanged 记录最近非最小化几何）+ 保存时回退 + 恢复时 x/y/W/H 合理钳制。
- **修④（游离叶）**：RestoreSession 替换 _layout 后 _activeLeaf 仍指旧树叶——总线事件开面板会挂到游离叶不可见。修复=恢复后重置 _activeLeaf 到恢复树。
- **修⑤（进程存活）**：主窗关闭后浮动窗维持应用存活（默认 OnLastWindowClose）。修复=主窗 Closing 先关全部浮动窗再存会话（ Avalonia 11 已移除 ShutdownMode，显式关闭是正解）。
- **修⑥⑦**：删 RevealGl 死方法；浮动面板不进会话的行为在 USAGE.md 标注。
- **修复教训**：`var a = X, b = Y;` 在 using/await 上下文中可能触发 CS0819（拆成两行）；unsafe 指针操作所在方法必须逐个标 unsafe（类级 AllowUnsafeBlocks 不自动覆盖）。

---

## 总线替换释放机制（2026-10-06，领域 4 收尾：IDisposable 旧帧释放契约）
- **交付**：DataBus.Publish 替换最新帧时，若旧帧实现 IDisposable 且与新帧非同一实例 → 锁外释放（Dispose 可能较重；释放时新帧已生效，晚订阅不会拿到旧帧）。26 例单测新增"替换释放"验证（旧帧 A 释放、新帧 B 不释放、TryGetLatest 拿到新帧）。
- **消费契约（IBus.Publish 注释同步）**：订阅回调内必须同步消费完毕（拷贝/上传进显存），回调返回后不得再引用旧帧——Cloud3DView 现行路径已符合（BeginJob 一次性拷入原生暂存，托管数组仅 BeginJob 期间被引用）。
- **当前载荷为托管数组**：Dispose 无实际释放动作（GC 回收）；机制为未来原生载荷（算子直产原生）预留的正确回收通道。
- **踩坑**：xunit 嵌套类访问外层实例字段需显式传递引用（primary constructor 参数注入）。

---

## BUG-060 + draw 栏目交付（2026-10-06，会话恢复工厂时序 + 算子内嵌预览）
- **BUG-060（会话恢复动态页静默丢失）**：PluginManager 里 `instance.Initialize(ctx)` 先于 `UiContextCreated?.Invoke(ctx)`——插件在 Initialize 里 `RegisterDocumentFactory` 时事件已发完，宿主订阅（挂在 UiContextCreated 处理器里）永远晚一步 → `_docFactories` 恒为空 → 会话恢复的流水线动态页被静默跳过，用户看到的是自动打开的示例图，伪装成"加载了算法但跳过新节点"。修复=UiContextCreated 改在 Initialize **之前**触发（宿主先挂上事件再初始化插件）。**教训**：① "事件在订阅者就位前触发"是静默失效，必须用时序单测或启动日志自证；② 排查时先确认"看到的是哪个页面"（示例图 vs 恢复页：标签名/按钮文字「保存算法」vs「💾 保存」），别急着怀疑加载逻辑——本次 5 节点谜团其实是示例图，算法文件根本没被打开过；③ 强杀进程不触发 Closing，会话不落盘——手工构造 session.json 时注意 Split 字段是**嵌套 JSON 字符串**不是对象。
- **draw 栏目（输出预览）**：算子库新增「输出预览」分类（draw.2d/draw.3d，HasPreview=true）；执行时 `ctx.Preview(载荷)` → GraphExecutor.PreviewReady 事件 → NodeEditorView 预览槽（Dispatcher.UIThread.Post 路由）。draw.2d 卡片内嵌 Image；draw.3d 通过 **Sdk.Ui.PreviewSurfaces.CloudPreview 工厂**（Cloud3D 插件注册 MiniCloudPreview=Cloud3DView embed 模式：不订阅总线/隐藏信息栏）内嵌迷你 3D 视图，支持拖拽旋转；卡片带「→ 工作台」按钮：有名点云保持原 Id 发布（更新工作台同名实体），匿名点云兜底命名「预览点云」；导入后自动激活对应工作台标签。
- **插件互联实例**：draw.3d 是"插件零直接引用"的样板——NodeGraph 只认 Sdk.Ui.ICloudPreview 接口，Cloud3D 注册工厂，宿主不知道预览的存在。
- **验证**：构建 0 错误；27/27 单测；E2E=会话恢复→算法 i 页 7 节点自动执行→2D 卡片缩略图→3D 卡片拖拽旋转→两键导入工作台（日志「已导入 2D/3D 工作台」+ 总线计数 +1 + 标签自动切换）→ 算子库「输出预览」栏目可见 ✓。

---

## BUG-061（用户报"draw 算子无法拖拽到流水线"）：算子库落点路由到最后创建页而非可见页（2026-10-06）
- **症状**：从算子库拖 2D/3D 预览到画布，节点不出现；日志有 `未处理异常: ArgumentException — Control does not belong to a visual tree`。
- **根因**：`NodeGraphPlugin` 的库落点/单击都写死 `_pages.LastOrDefault()`（最后创建的页）而非**当前可见页**——多页面（会话恢复「算法 i」+ 自动打开示例图）出现后，目标页标签不活跃=视图不在可视树，`PlaceNodeAtScreen → _editor.PointToClient` 直接抛 ArgumentException（未处理异常钩子捕获，节点未添加）；即使不抛也会把节点加到用户看不见的页。
- **修复**：插件跟踪 `_activeEditor`（`view.AttachedToVisualTree += → _activeEditor = view`，标签选中=挂树=成为落点目标），拖放与单击都路由 `(_activeEditor ?? _pages.LastOrDefault())`；`PlaceNodeAtScreen/PlacePipelineAtScreen` 加 `GetVisualRoot() is null` 防御（打日志"请先切换到流水线页面"而非崩溃）。
- **教训**：① "Last/Current"类隐式目标在多实例出现后必然错位——插件侧的"活动对象"应由视图挂树事件驱动，不要用创建序推断；② 单页时代正确的代码在多页面特性上线后变 bug——新特性（会话恢复多页）要回头审查所有 `_pages` 消费者。
- **验证**：拖 2D/3D 预览到可见画布均落点正确（日志「添加节点」+卡片出现+无异常）；27/27 单测 ✓。

---

## BUG-062（用户报两项）：连线扇出 + 卡片 Del 删除（2026-10-06）
- **扇出（一个输出连多条输入线）**：GraphModel.Connect 只做了输入端替换（DisconnectInput），输出端无限制——扇出虽被 UI 的"已连端口拉出=拆线重连"挡住大半，但模型层不设防（粘贴/程序化 Connect/输入端发起的连线路径均可绕过）。修复=**Connect 同时 DisconnectOutput**（每个端口至多一条线，新连线替换旧连线，链式语义）；新增单测 `Connect_ReplacesExistingOnBothPorts` 锁定（28/28）。
- **Del 删除**：原删除=右键菜单唯一入口。补齐键盘路径：① 卡片 Focusable + 任意按下选中（`AddHandler(PointerPressedEvent, …, handledEventsToo:true)`——头部把 Pressed 标记 Handled 用于拖动/改名，普通冒泡收不到）；② 选中=边框变系统蓝 #0A84FF（只换色不变厚度，防布局抖动）；③ 卡片 Focus() 使 Del 可达（**参数框按下不抢焦点**：`e.Source is not TextBox` 才 Focus，避免打断输入）；④ UserControl KeyDown 里 Delete 且非只读 → DeleteNode；⑤ 画布空白按下=取消选中，DeleteNode 同步清选中。E2E：点卡片→蓝框→Del→卡片消失+连线清理+自动重跑 ✓。
- **教训**：① handledEventsToo 是"父容器要在子控件已处理 Pressed 后仍收到事件"的正解（普通冒泡被 Handled 拦截）；② 给卡片发键盘焦点时必须排除内部 TextBox，否则点参数框就丢焦点；③ "UI 挡住了"≠"模型安全"——不变式要落在模型层+单测，UI 路径只是第一道防线。

---

## BUG-063（用户报"连线应该连接到输入才对"）：draw 卡片连线端点错位 + 扇出误禁（2026-10-06）
- **错位**：draw 卡片加了名字行+预览区后端口行被推低，但连线端点仍按"端口紧跟头部"的行号公式推算 → 边画到名字行高度（用户截图实证）。**修复=端口锚点改用实际端口点控件的布局位置**：PortRow 登记 `_portDots[(nodeId,port,isInput)]`，In/OutPortCenter 优先 `dot.TranslatePoint(中心, _world)`（已布局才采信），行号公式只做兜底；`_world.LayoutUpdated` + `_edgeLayoutDirty` 在布局完成后补画一次。
- **★两段式踩坑**：① 第一次修复后边仍错位——`RedrawEdges` 在布局前运行，**未布局控件的 TranslatePoint 返回 (0,0) 而非 null**（端点塌缩到画布左上角成小绿点）；且脏标记只在"已布局但 Translate 失败"分支置位，**未布局分支漏置** → 布局完成后永远不补画。终版=只要走了回退路径（点已登记但未布局或 Translate 失败）都置脏。**教训：TranslatePoint 对未布局控件给 (0,0) 不给 null，必须配 IsArrangeValid 门禁；"布局后补画"的脏标记必须在所有回退路径置位**。
- **扇出纠偏（BUG-062 的限制撤销）**：用户澄清原始诉求是"一个输出应能连多个输入"（此前被"已连输出拉出=拆线重连"挡住做不到），我此前把"可连接多个输入"误读为要禁止扇出。终版语义=**输入端一对一（新线替换旧线=换源）；输出端一对多扇出（数据流分发）**——撤销 Connect 的 DisconnectOutput、撤销输出端拉起即拆线；输入端拉起拆线保留。单测改写 `Connect_InputReplaces_OutputFanOut`（30/30）。
- **教训：需求条款的正反解读在动手前应让用户确认一句（"是要禁止还是目前做不到？"）——语义反了修复方向全反**。

---

## BUG-064（用户报"名字缺默认名/改不了名/无法导入"）：模板控件 e.Source 误抢焦点（2026-10-06）
- **根因（三症状同源）**：卡片"按下即选中+聚焦"处理器用 `e.Source is not TextBox` 排除输入框——但 **Avalonia 模板控件（TextBox/Button）的 e.Source 是其模板内部元素（TextPresenter/Border 等）**，判断恒不成立 → 点名字框被 card.Focus() 抢走光标（打不了字）、点「→ 工作台」按钮被抢（点击失效）、滑杆同险。**修复=聚焦条件改为 `!e.Handled`**（TextBox/滑杆的按压均标记 Handled；按钮再叠 Source 判断兜底）——"无控件处理过的惰性区域"才把键盘焦点交给卡片。
- **教训：模板控件的 e.Source 是模板子元素，命中判断不能只看控件类型；要用 e.Handled（是否已被处理）作为"惰性区域"判据**。
- **draw 名字框默认名**：原来只有 Watermark"跟随输入"，用户要求显示默认名本身——建卡时即填 `ResolveExportName`（输入源节点名），连线增删后随 RefreshPreviewNames 刷新（输入中不打断），用户改过的（__varName）不动。
- **数据树 2D 图像栏补齐 3D 栏目交互**：行首绿点=当前显示在 2D 工作台（点击=重新发布该图显示，≤16MP 保留像素，更大只留元数据）；单击选中（与点云互斥）→ 详情区"图像属性"（名字/尺寸/帧数/来源）；双击改名（Sdk.Ui.InlineRename）；Ctrl+C/V 复制粘贴副本（像素深拷贝、-副本 递增）；Del 删除；Esc 取消选中。

---

## BUG-065（用户报"面板列表下栏目打不开对应的面板"）：关选中标签→SelectedContentHost 持有视图→重建中断→中间空白（2026-10-06）
- **症状链**：关标签 → `未处理异常: InvalidOperationException — The control Image2DView already has a visual parent ContentPresenter(PART_SelectedContentHost)` / `ArgumentOutOfRangeException` → RebuildSplitView 中断 → **中间区整片空白** → 之后点面板列表任何行都"打不开"（OpenPanel→RebuildSplitView 在同一脏状态上再抛）。error.log 堆栈实锤三连（13:17:46/13:18:02/13:18:04）。
- **根因**：Avalonia TabControl 的**选中项内容由自身的 PART_SelectedContentHost 呈现**（不在 ItemPresenter 里）。带选中状态 `Items.Remove` 时，TabControl 处理索引变化会把（仍挂在旧呈现器上的）视图再次挂进呈现器 → "already has a visual parent"；后续重建在同一失同步状态上反复抛。
- **修复（两段式摘除 + 双端兜底）**：① RebuildSplitView 先**整体清空各 TabControl 的 SelectedItem**（释放 SelectedContentHost 持有）再逐个 Items.Remove；② 单条 Remove 失败兜底 `Items.Clear()`（整组即将丢弃）；③ BuildGroup 挂载加 try/catch（单标签挂载失败只丢该标签+日志，不再拖垮整格）。
- **教训**：① **TabControl 的选中内容由独立呈现器持有——批量搬移 TabItem 前先清选中**，"复用控件先摘除再挂载"（BUG-037）只覆盖了 ItemPresenter 路径；② 未处理异常把 UI 重建打断后会留下"永久脏状态"，后续所有同类操作连环失败——重建循环必须逐条兜底；③ 用户报"打不开"时先查 error.log 的异常链（本次是关标签埋雷、开面板引爆，两个操作隔了 20 分钟）。
- **验证**：关选中中的「3D 点云」标签 → 无异常、布局完好、选中落相邻标签 ✓；面板列表点「2D 图像」（已开→聚焦右格）/「数据表格」（新开标签）✓；30/30 单测。

---

## BUG-066（用户报"拖拽后窗口名字不显示、图像不显示"）：选中内容释放时序竞态（2026-10-06）
- **症状**：把 2D 图像标签拖到右边拆分，右格**整个空白**——没有标签头、没有图像；error.log 连环 "already has a visual parent ContentPresenter(PART_SelectedContentHost)"（同 BUG-065 的异常面，但发生在拖拽路径）。
- **排查弯路**：① 先怀疑 BUG-065 修复没覆盖拖拽路径——加链路诊断（press/dragstart/release/drop/deselect/rebuild 全链路文件日志），结果前几轮自动化拖拽全因**坐标按错**（标签实际位置和截图估算差 100px+，按到空白带=无操作）+ **用户同时在操作界面**（布局被用户拖动过、进程被重启过）而"无效"，白白空转多轮；② 诊断输出用相对路径写到应用 CWD 找不到文件——**改绝对路径**；③ python 补丁里的 `
` 被 Write 工具写成真实换行，把 C# 字符串字面量劈成两行→10 个编译错误，**应用一直跑着旧版**（构建失败但 start 照样启动旧 exe——构建成功判定再次翻车）。
- **真根因（诊断链路实锤）**：deselect 传里 `SelectedItem=null + UpdateLayout` 后，选中内容的视觉父级**仍是 ContentPresenter**——释放走绑定+布局泵，**一次 UpdateLayout 泵不完**；多标签组场景下后续 deselect 的 UpdateLayout 顺带泵完了释放（侥幸），**单标签组（用户场景：右格只有 2D 图像一个标签）没有后续顶班**→重建挂载时内容仍被旧呈现器持有→抛异常→挂载中断→空格。
- **修复**：deselect 传的释放改为**有界循环**——`SelectedItem=null` 后循环（≤6 次）`tc.UpdateLayout()` 直到内容的 GetVisualParent()==null 才放行重建。
- **教训**：① **释放依赖布局泵的时序假设必须用循环泵到"确认脱离"为止**（一次 UpdateLayout 是赌运气）；② 自动化验证 UI 前先核对窗口/标签的**当前**几何（用户操作会让上一次截图全部失效），一次截图+一次点击内完成；③ 构建失败后 `start` 启动的是旧 exe——**构建失败必须中止后续启动**（本轮 10 个编译错误还启动了，白测半小时）。
- **验证**：拖 2D 图像入左格 → 标签头+图像正常显示、右格回收、零异常 ✓；30/30 单测。

---

## BUG-067（用户报"从面板界面点 3D 点云就报错"+"核实面板内的所有"）：ti.Parent 重建后为 null，父级依赖路径整体静默失效（2026-10-06）
- **根因**：TabItem 的 `ti.Parent`（逻辑父级）在分屏重建后**可能为 null**（诊断实锤：rebuild 快照里 parent= 空）——所有 `ti.Parent is TabControl` 判断恒假：① SelectTab 不切换选中（面板列表点行"没反应"）；② RebuildSplitView 的清选中/摘除整体跳过（选中内容继续挂在将被丢弃的呈现器上→挂载抛 already has a visual parent，即用户看到的报错）；③ 另一处 Items.Remove 同病。**BUG-065/066 的修复都建立在 Parent 可用之上，被这一层击穿**。
- **修复**：新增 `TabControlOf(tab)`——**优先经 _groups 分屏注册表**（leaf.PanelIds 含 key 且 tc.Items 含 tab）找活 TabControl，Parent 仅作兜底；SelectTab/清选中+摘除/Items.Remove 三处全部改走该辅助。清选中+释放循环逻辑不变（BUG-066 的有界泵保留）。
- **教训**：① **不要用 ti.Parent 定位 TabItem 所在的 TabControl——重建/摘除后逻辑父级不可靠，用自己维护的注册表（_groups）反查**；② 用户一句话（"我从面板界面里点击的"）胜过半小时拖拽复现——报错的操作路径要问清入口；③ "核实所有"= 面板列表 8 行逐行点击验证（4 Document 开/聚焦 + 3 Tool 切侧边栏 + 1 聚焦），而不是只测报告的那一行。
- **验证**：面板列表全部 8 行逐行点击——3D 点云/结果预览/2D 图像（聚焦对应标签）、数据表格（新开标签）、数据树/算子库/总线监视（侧边栏正确切换）、算法流水线（聚焦）——全程 error.log 零新异常 ✓；30/30 单测。

---

## 2D 图像显隐开关（用户报"数据树里不能关闭显示"，2026-10-06）
- **原状**：2D 图像行绿点只能"发送显示"（单向），没有 3D 点云那样的隐藏切换——用户点绿点想关显示没反应。
- **实现**：① 数据树绿点改真开关：显示中→隐藏 = 发布 **0×0 隐藏信号**（ImagePayload{Width=0,Height=0}），未显示→显示 = 重发像素；② Image2DView 订阅路由 0×0 → ClearImage（清屏+状态栏"已隐藏 · 来源"）；③ TrackImage 跳过 0×0 信号不进条目，且**显示状态镜像**（最近发布的条目=显示中，其余自动熄灭——流水线再发布时绿点自动转移）。
- **★坑（自己注释里写了还是踩）**：ClearImage 初版设 `_hasImage=false`——**Render 对无图早退、WriteableBitmap 残留上一帧**，清屏不生效（设计注释"以 1×1 黑占位重绘"写了，实现却把开关关了）。终版=`_w=1,_h=1,_hasImage=true` + 1×1 黑纹理，让 Render 真正重绘。**教训：清屏类操作必须让渲染管线真的画一帧"空"，不能只改状态标志**。
- **验证**：点绿点→工作台清屏（黑+"已隐藏 · 数据树（已隐藏）"）+点空心+名变暗；再点→图像恢复+点实心 ✓；30/30 单测。

---

## 总线监视升级为窗口面板 + 详细追踪（用户需求，2026-10-06）
- **需求**：总线监视应为窗口面板（进「面板」列表、从活动栏移除），每条数据详细追踪。
- **交付**：① PanelDescriptor Role Tool→Document（自动进面板列表、出活动栏——上一条"面板列表只列 Document"的过滤天然配合）；② **IBus 新增总线级基础设施**：`FramePublished` 钩子（每次 Publish 锁外触发，任意线程）+ `TryGetLatestRaw`（运行时最新帧）——BusNull 空实现同步补齐；③ BusMonitorView 重写：上半主题表（类型/帧数/频率/最后发布，行可选中），下半详情=概览 + **最新帧逐字段转储**（反射，数组显长度、字符串截断）+ **发布历史**（环形缓冲 ≤100 条，毫秒时间戳 + 摘要，最近在前）+「清空历史」。
- **坑**：接口加成员后所有 IBus 实现类都要补（BusNull CS0535）；反射 DumpProps 的 switch 顺序——string 在 Array 前（string 也是 IEnumerable 但不是 Array ✓ 无碍，Array 分支用 GetElementType）。
- **验证**：面板列表出现「总线监视」→ 点击开标签页 → 主题表 9 行 → 点 cloud/merged → 详情显示 CloudPayload 字段（Count/Id/Points/Source）+ 4 条历史 ✓；活动栏 📡 消失 ✓；30/30 单测。

---

## BUG-068（用户报"点面板只新建窗口不跳转"）：UI 线程死锁——内嵌迷你 3D 视图的 GL worker 与主视图 worker 互卡（2026-10-06）
- **症状**：应用启动后一切输入失效——点面板行/标签/任何按钮都无反应（用户感知为"不跳转"）；画面静止但无异常、CPU 不烧（0.08s/3s）、全部线程 Wait(UserRequest)。**日志面板定格在「[3D 预览] 预览点云」之后**——UI 线程卡死在 draw.3d 预览创建的瞬间。
- **根因**：draw.3d 预览的 MiniCloudPreview=Cloud3DView(embed)——**每个 Cloud3DView 各自创建后台 GL 上传线程 + 共享上下文**（M3.2 的 worker 机制）。主视图 worker 持共享上下文 current 时，内嵌视图在 UI 线程创建自己的窗口/上下文/worker——**跨线程 GL 上下文争用把 UI 线程永久挂起**（驱动层同步等待，无异常无超时）。
- **修复**：`GlCloudRenderer(allowBackgroundWorker)`——**内嵌模式禁用后台 worker**（预览点云小，UI 线程同步上传足够），主视图 worker 保留。顺带修掉字段初始化器+构造器双建 renderer 的泄漏。
- **排查教训**：① **"点击无反应"先判 UI 线程死活**（CPU 采样 + 线程 Wait 状态 + 日志是否定格），别急着查点击路由——本次绕了 SelectTab/TabControl/前台锁一大圈，其实输入根本没进消息循环；② 日志定格行=卡死现场（[3D 预览]后一帧不动）；③ 单实例 Mutex 会把我的 start 静默转发到旧实例——**测试前核对进程启动时间**；④ 用户在机器旁时其操作会混入证据（画面变化但我的日志无记录=用户点的）。
- **验证**：修复后标签点击/面板行点击全部恢复（[select-tab]/[sel-changed] 日志 + 画面切换）；面板行→开新标签并跳转、切走再点行→跳回 ✓；30/30 单测。

---

## BUG-069（用户报"点 3D 点云面板界面停在算法流水线"）：UI 线程在 3D 视图选择路径被 GL 上下文争用挂起（2026-10-06）
- **症状**：运行一段后，点面板行/标签全部"无反应"（用户看到 3D 点云标签存在但界面不切）；诊断实锤：`SelectedItem` 已赋值成功（日志 selNow=ui.cloud3d.view）但视觉不刷新，且之后一切输入死——**UI 线程在 SelectTab→SyncPropsFor→（3D 视图参数/挂载路径）被挂起**（CPU 静止、无异常）。新实例一切正常（标签/面板行跳转验证 ✓）——挂起是运行中 GL 争用累积触发。
- **根因链**：主 3D 视图的**后台上传 worker 终身持有共享 GL 上下文**（线程启动时 MakeContextCurrent 一次、线程退出才释放）——UI 线程后续的 GL 操作（视图选择时的挂载/参数构建/渲染）与驱动层跨共享上下文同步互卡 → 永久挂起（BUG-068 的另一变种：那次是创建窗口挂死，这次是选择路径挂死）。
- **修复**：**worker 改为每个任务单独持上下文、任务完成立即释放**（MakeContextCurrent(_uploadWin)→执行→MakeContextCurrent(0)）——把争用窗口从"线程终身"压到"单任务瞬间"；异常路径也补释放。配合 BUG-068（内嵌视图禁 worker），全进程 GL 争用面已最小化。
- **排查教训**：① "属性已变但视觉不动"=UI 线程在属性变更后的回调链里挂起（日志定格行=挂起点）；② **自动化输入会被环境吞**（前台恢复后首击、用户在机器旁抢焦点）——"点击无效"先看 CPU/日志判断输入是否真的到达，别急着改代码；③ GL 跨线程上下文"终身持有"是隐患——**持锁/持上下文的窗口越短越好**。
- **验证**：新实例标签点击切换 ✓（日志+画面）；30/30 单测。运行中挂起的复现依赖驱动时序，交用户真鼠标长时使用观察。

---

## BUG-070（用户报"关闭后首次打开不跳转，已打开再点可跳转"）：缓存视图未从旧呈现器释放→重开挂载失败（2026-10-06）
- **症状**：面板已打开时点行=正常跳转；**关闭标签后首次点行**=标签出现但不切换。
- **根因**：面板视图是缓存单例（_panelViews）。关闭标签→重建时，选中内容的释放靠"SelectedItem=null+布局泵"——**布局泵不可靠**（BUG-066 诊断实锤：泵 6 次内容仍挂 ContentPresenter）。没释放干净→旧 TabControl 被丢弃时**视图仍挂在其 SelectedContentHost 上**→重开同一面板复用该缓存视图→新格挂载失败→选择死→"首次打开不跳转"。已打开再点可跳=视图当前就挂在活动格上（无需迁移）。
- **修复**：deselect 布局泵后加**强制摘除兜底**——内容仍挂着时，找到该 TabControl 模板里的 PART_SelectedContentHost 呈现器直接 `Content=null`（整组即将丢弃、绑定不会再更新，无副作用）。
- **教训**：①"释放走布局泵"类时序机制必须有**可验证的强制兜底**（泵完检查+直接摘除）；②缓存复用的视图在"关闭→重开"路径上最易暴露残留父级——关闭路径的释放要按"最坏情况"设计。
- **验证**：关「3D 点云」标签→点面板行重开→标签出现+自动跳转+3D 视图完整显示（2 实体 126,742 点）✓；30/30 单测。

---

## "UI 又挂起"乌龙（2026-10-06 15:28，用户报"有bug 你操作"）
- **现象**：面板行/标签点击全部"无反应"，CPU 静止——看似又是 UI 线程挂起（BUG-069 复发）。
- **真相（dump 实锤）**：`dotnet-dump collect + clrstack -all` 显示 **UI 线程正常在 GetMessage 等输入、GL worker 正常等任务——应用完全健康**。用 **UIA Invoke 直接调用面板行按钮 → 跳转正常**；UIA Select 标签 → 切换正常。→ **是我的 SendInput 鼠标模拟静默失效**（前台校验通过、WindowFromPoint 正确，但 SendInput 被 UIPI/前台锁拒绝且脚本不查返回值照样打印 "click done"）。fgclick 已加 SendInput 返回值校验（失败打 WARN）。
- **教训**：① **"点击无反应"的三层判定顺序：UI 线程死活（dump/CPU）→ 输入是否送达（UIA 直调对照）→ 才轮到点击路由代码**——UIA Invoke 是绕过鼠标模拟的黄金对照手段；② **自动化工具自身会说谎**——SendInput/前台校验通过≠点击送达，关键验证要有第二通道（UIA）；③ dotnet-dump（dotnet tool install --global dotnet-dump）是挂起类问题的终极诊断，几分钟拿全线程栈。
- **应用侧结论**：面板行打开/跳转/关闭重开/标签切换全部正常（UIA+真鼠标双通道验证）。

---

## BUG-072（用户报"点 2D 图像标签开了但不切换"）：重建期间自动选中回写模型，覆盖新面板的 ActivePanelId（2026-10-06）
- **症状**：面板已打开时点行=跳转 ✓；**关闭后（或新开）点行=标签出现但停在旧标签**（用户实锤截图：2D 标签在条上、画面停在 3D 点云）。
- **诊断实锤**：`BuildGroup items=6 activeIdx=4`——2D 是第 6 个标签（索引 5）但 activeIdx=4。**根因**：RebuildSplitView 拆除旧 TabControl 时，逐个 Items.Remove 触发 TabControl **自动补选**（选中项被移走后自动选下一个）→ Group_SelectionChanged → `leaf.ActivePanelId = 自动补选的 key`——**把 OpenPanel 刚设置的 ActivePanelId（新面板）覆盖成了拆除噪声选中的旧标签** → BuildGroup 按被污染的 ActivePanelId 选中了错误标签 → SelectTab 虽把 SelectedItem 设为新标签，但初始选中已错+内容呈现不同步 → 视觉停在旧标签。
- **修复**：`_rebuilding` 守卫——RebuildSplitView 全程置位，**Group_SelectionChanged 在重建期间直接返回**（拆除自动选中是噪声，不得回写模型）。
- **教训**：① **"事件处理器回写模型"类代码必须考虑重建/拆除窗口期的噪声事件**——拆除引发的自动选中不是用户意图；② 诊断日志的 activeIdx 与预期索引对比（4 vs 5）一步定位污染源；③ 该 bug 与 BUG-070（关闭重开不跳转）叠加出现——070 修的是"视图残留父级"，072 修的是"ActivePanelId 被覆盖"，两者都表现为"开了不跳"。
- **验证**：关 2D 图像→点面板行重开→`activeIdx=5`（正确）→标签创建+自动跳转+2D 工作台完整显示 ✓；已打开再点=跳转 ✓；30/30 单测。

---

## BUG-073（用户报"点 3D 不跳、点 2D 却跳到 3D"）：重建后布局推迟——视觉落后模型一步（2026-10-06）
- **症状（用户精确序列）**：3D/2D 都关闭 → 点 3D 点云=标签创建但不跳转 → 点 2D 图像=**跳转到 3D 点云**。选择效果永远滞后一次点击。
- **根因**：RebuildSplitView 把新 TabControl 挂到树上后，**模板应用/生成器/选中内容呈现默认推迟到下一个布局帧**——正常几毫秒内完成，但若下一次输入先到（用户连续点击），推迟的布局被新的重建覆盖/打断——**视觉状态永远落后模型一步**（模型 ActivePanelId/SelectedItem 都是对的，画面停在上一帧）。BUG-072 修掉了模型被污染的问题后，这个"渲染滞后"才单独显形。
- **修复**：重建末尾强制 `SplitHost.UpdateLayout()`——同步完成模板应用+选中内容呈现，视觉立即跟上模型。
- **教训**：**"属性对了画面不动"的下一层排查方向=布局/渲染是否被推迟**（Avalonia 挂树≠立即呈现，模板应用在布局帧）；同步路径上需要立即可见的 UI 变更，改完树必须 UpdateLayout 收尾。
- **验证**：3D/2D 都关闭 → 点 3D=创建+立即跳转 ✓ → 点 2D=跳转到 2D（不再跳 3D）✓；30/30 单测。

---

## 面板/标签栏重构（用户"一直修不好，重构吧"，2026-10-06）
- **背景**：BUG-065~073 连环九个 bug 全部源于同一机制——**每次开/关标签都整树拆掉所有 TabControl 再重建**。拆装过程引入：内容呈现器持有视图（065/070）、自动选中噪声回写模型（072）、布局推迟视觉滞后（073）、生成器失同步（065）、ti.Parent 失效（067）、GL 上下文争用（068/069）。补丁逐个堵，堵不胜堵。
- **重构原则**：**日常操作（开/关/选中）走增量路径——只动目标标签组，绝不拆树**；只有结构变化（拆分/合并/拖拽/浮动/会话恢复）才走整树重建。
- **实现**：① `TryAddTabIncremental(leaf, tab)`——经 _groups 找到活 TabControl 直接 `Items.Add + SelectedItem=tab`（挂载中的组模板已应用，选中同步生效，无布局推迟）；失败回退重建兜底。② `CloseTab` 增量——组内还有标签时只摘该标签（`ReleaseTabContent` 清选中+释放内容后 `Items.Remove`，选相邻标签）；空叶回收才重建。③ `ReleaseTabContent(tc, tab)` 提取为共享方法（清选中→布局泵→强制摘除兜底），重建的 deselect pass 复用。④ OpenPanel/OpenDynamicDocument 均增量优先。
- **效果**：开/关标签不再触碰其他组——内容呈现器拆装、自动选中、布局推迟这一整类 bug 的触发面从"每次操作"降到"仅结构变化"。
- **验证**：3D/2D 都关闭→点 3D=创建+立即跳转 ✓→点 2D=直接跳 2D ✓→关 2D→点行=重开+立即跳转 ✓；30/30 单测。
- **教训**：**"每次全量重建"的 UI 架构在控件复用+模板化控件（TabControl）下是 bug 温床——增量更新 + 结构性重建分离才是正解**；连环 bug 同源时应及早重构而非继续打补丁。

---

## BUG-074（用户报"2D/3D 打不开、总线监视还有 bug"）：总线监视列定义笔误 "24%*"——每秒异常风暴拖垮 UI（2026-10-06）
- **症状**：面板里的 2D/3D 点行打不开；总线监视异常。error.log 实锤：`FormatException: The input string '24%' was not in a correct format. at ColumnDefinitions..ctor at BusMonitorView.Refresh()`——**每秒一条**（1s 定时器刷新）。
- **根因**：重写 BusMonitorView 时行 Grid 列定义写成 `"30*,22*,12*,12*,24%*"`（多了个 %）——GridLength 解析直接抛。总线监视标签开着时，**定时器每秒抛一次异常**，Dispatcher 反复处理失败操作，把整个 UI 的输入/打开操作拖垮（表现为"面板打不开"）。
- **修复**：`24%*` → `24*`。修复后 15 秒零新异常（风暴停止）；用户确认"修复了"。
- **附带改进**：总线监视打开时**播种历史**——遍历 GetTopics + TryGetLatestRaw 把每个主题的现有保留帧记为历史起点（时间=LastPublish），历史区开箱即见（此前显示"打开面板后开始捕获"空荡一片，易被误认为不工作）。
- **教训**：① **字符串式布局定义（ColumnDefinitions("...")）是运行时解析——笔误不在编译期暴露，只在运行时炸**，写完要过一遍每个 token；② **"UI 越用越坏/打不开"先查 error.log 有没有周期性异常风暴**（定时器类组件的刷新异常会持续放血）；③ 一个组件的异常能拖垮全局交互——Dispatcher 操作失败的处理要警惕。

---

## BUG-075（用户报"所有面板都不显示了"）：会话恢复时视图残留父级→重建抛异常→中间区全空白（2026-10-06）
- **症状**：启动后只剩活动栏+侧边栏，中间标签区整片空白；日志"会话已恢复"后紧跟 `未处理异常: InvalidOperationException — The specified element is already the child of another element`。
- **根因**：会话恢复的重建中，个别缓存视图没从旧呈现器上释放干净（时序类残留——BUG-070 同族的漏网场景），重挂载抛"already a child"→重建中断→SplitHost 空白。此前会话文件还带着 bug 时代的脏布局，放大了触发概率。
- **修复（自愈式重建）**：RebuildSplitView 加 catch——重建失败时**全量强制摘除所有标签内容**（ForceDetachContent：按视觉父级类型清 ContentPresenter.Content / Panel.Children）后按当前树重试一次；重试再失败只记日志不让异常逃逸。顺带：① 重写整个方法（缩进规范化——此前补丁链留下 17 行错位缩进）；② CloseTab 增量路径补"模型未给下一激活标签→兜底选末位"（避免无选中空内容组）；③ 删除脏 session.json 回默认布局。
- **教训**：① **重建类代码必须有自愈兜底**——一次挂载失败不该把整个中间区变成空白（用户感知=全坏）；② 会话文件会带着 bug 时代的脏状态存活，修完结构性 bug 后应重置会话验证干净路径；③ 缩进修复脚本要按大括号层级配对验证，不能只按空格数盲改。
- **验证**：默认布局+会话恢复两轮启动全部面板正常显示；构建 0 错误；30/30 单测。

---

## BUG-076（用户报"算子库里的卡片咋不能复制了"）：点卡片标题栏不给键盘焦点→Ctrl+C/V/Del 不可达（2026-10-06）
- **根因**：卡片头（标题栏=拖动手柄）的按压处理器为拖动标记 `e.Handled=true`——卡片级处理器的聚焦条件 `!e.Handled` 因此失败 → **点标题栏后卡片没拿到键盘焦点** → 编辑器的 KeyDown（Ctrl+C 复制/Ctrl+V 粘贴/Del 删除）全部够不到。点卡片身体（参数区）可以聚焦所以"有时能用"，点标题（最自然的点击位置）必坏。
- **修复**：头部自己的处理器在进入拖动分支时补 `SelectCard + card.Focus()`（经 _cards 反查——头部构建时 card 尚未注册，点击时已就绪）。
- **教训**：**"标记 Handled 的处理器要对自己区域的键盘可达性负责**"——上游标记 Handled 会让下游 handledEventsToo 处理器的 `!e.Handled` 守卫失效，焦点类副作用要在标记方补齐，不能只依赖兜底方。
- **验证**：构建 0 错误、30/30 单测；干净状态交用户真鼠标验证（点标题→Ctrl+C→Ctrl+V 出副本）。另：用户已用 Del 成功删除算法文件（i/deltest 均已删）——Del 功能实战通过。

---

## 移除「结果预览」演示面板（用户"不清楚结果预览是干啥"，2026-10-06）
- **背景**：结果预览=DemoPreview 插件的 M1 期演示视图（订阅 image/stitched 显示最新图）——产品语义定型后与 2D 图像工作台完全重复（且无缩放/取色/ROI 等任何工作台能力），留着只会让用户困惑。
- **移除**：① 插件删 ui.preview.image 面板注册（插件更名"总线监视" v1.1.0，唯一面板=总线监视窗口）；② Views.cs 删 PreviewView 类（2641 字符死代码）；③ 会话/自动打开对已删面板的引用由现有容错自动剔除（实测干净启动 ✓）。
- **教训**：演示期面板在产品语义定型后要主动清退——"每个面板都要能一句话说清自己的产品价值"，说不清的就是该删的。

---

## BUG-077（重构中实锤）：标签沿标签条拖放松手被误判"上边缘拆分"——左右排序手势无处落（2026-10-07）
- **症状**：把标签沿标签条拖到同组另一个位置松手，结果不是排序，而是被拆出一个新格（释放点在组矩形顶部 28% 带内 → zone="top"）。用户要的"已打开面板左右排序"因此没有自然手势。
- **根因**：拖拽落点命中检测 HitGroup 把**整个组矩形**（含标签条）按 28% 边缘带划分——标签条只有 ~30px 高，但"top 带"有 260px 高，沿标签条拖放必然落进 top 带。
- **修复**：HitGroup **优先判定标签条高度内 = "center"**（按插入位置处理：同组=ReorderPanel+增量摘插不整树重建；跨组=MovePanel 带插入下标），上边缘拆分只在标签条以下区域生效。拖拽预览同步改为插入位置竖线（边缘拆分仍用半格高亮）。配套：SplitLayout 新增 `ReorderPanel`（组内重排）与 `MovePanel(from,id,to,toIndex)`（按位移动），单测锁定语义（下标按"移出后"语义、越界钳制、原位返回 false）。
- **教训**：① **拖放 zone 判定要以"手势自然落点"优先**——排序手势的落点在标签条上，先判条再判带；② UI 手势改语义时，模型层（SplitLayout）先落 API + 单测，UI 只做翻译。

---

## BUG-078（用户报"一直在试错，直接重构"）：面板列表行拖拽释放收不到——ButtonBase 标 Handled，推倒重构去 Button 化（2026-10-07）
- **症状**：面板列表行拖拽排序松手后无任何反应（顺序不变、panel_order.json 不落盘）；此前按"加 handledEventsToo"补丁的版本同样无效。
- **根因**：行是 `Button`——ButtonBase 自带指针捕获 + 把 PointerReleased 标 Handled + Click 语义，与"按下→移动阈值→捕获→释放落位"的拖拽状态机天然纠缠（释放收不到 → 状态机永远停在已按下；Click 还需要抑制防止拖放误打开——BUG-064 同源家族）。另有两个放大器：① 慢速拖拽时目标行 ToolTip 弹出恰好挡住释放点；② 首次拖拽 _order 为空时相对位置算不出（需按视觉顺序播种）。
- **修复（重构）**：**行改普通 Border（非 Button）**，按下/移动/释放三个指针事件收在侧边栏容器一级（一套状态机管所有行，捕获在容器）；点击=按下与释放同行；拖拽开始时清空全部行 ToolTip、结束（无论落点是否命中）Refresh 重建行还原样式；落点用 2px 蓝色横线实时指示；落盘前按当前视觉顺序播种 _order。Button 的捕获/Handled/Click 分支全部消失。
- **教训**：① **要拖拽的行不要用 Button**——自定义行为控件与 ButtonBase 的内建指针语义互搏，普通 Border+容器级状态机更短更稳；② **"构建成功"必须看完整尾部 0 错误**——本轮探针构建静默失败后连续跑了三轮旧二进制（构建输出被 tail 截断没看见 15 个错误）；③ E2E 输入自动化：宿主应用会抢前台，SetForegroundWindow 靠 ALT 技巧+校验，抢不到就用 **HWND_TOPMOST + 真鼠标**（fgdrag.ps1 -topmost，SendInput 返回值校验需 Send 返回 uint 而非 void——void 让校验永远假报警）。

---

## BUG-079（用户报"窗口关闭后页面布局保持原状、也没有快照按键"）：会话树幽灵键把空格子钉死——不变量双向强制（2026-10-07）
- **症状**：① 关掉某格里最后一个真实标签，格子不回收，留一片空白（"布局保持原状"）；② 重启后旧布局原样恢复但快照标签没了，只剩空格子（用户预期里"没有快照按键"即此处——快照标签按设计不恢复，空格子却是 bug）。
- **根因**：快照等无恢复键的动态文档页以 `doc:N` 进了分屏树；保存时 TranslateDocKeys 只翻译有恢复键的页（幽灵键原样落盘）；恢复时 `if (pid.Contains(':')) continue;` 盲信"doc 键已建"直接跳过。幽灵键使 leaf.PanelIds 永不为空 → CloseTab 的空格回收永远不触发。同族隐患：恢复时插件缺失的清理直接戳 `PanelIds.Remove`，绕过回收链（不修正 ActivePanelId、不回收空格）。
- **修复（重构=确立不变量「分屏树只存可恢复的键」，两端强制，剔除一律走模型回收链）**：
  ① **保存端**（SaveSession）：序列化前把无恢复键的 `doc:*` 从模型 `_layout.RemovePanel` 剔除（空格回收/容器退化/激活面板修正是 SplitLayout 既有 tested 行为）；② **恢复端**（RestoreSession）：doc 键重建过（`_tabs` 里有）才保留，否则 RemovePanel；面板键建不出（插件缺失）同样 RemovePanel（替换原直戳 PanelIds 的写法）。旧脏会话文件重启即自愈。
- **验证**：污染会话（4 格含 doc:1/doc:2 幽灵键）重启→收敛为干净单格；拍快照后退出→会话 Split 零 doc: 键；拆出独立格→关唯一标签→格子塌缩回单格。构建 0 错误。
- **教训**：① **持久化引用必须与"可重建"对齐**——树里存了重建不出来的键，等于给回收逻辑埋了永久性障碍；② 剔除条目永远走模型自己的回收 API，不要直接戳底层集合（回收链/激活态修复都在那里）；③ 用户报"布局保持原状"这类话，先看会话文件里存了什么——证据在数据里，不在猜测里。

---

## BUG-080（用户报"欢迎界面关不了→关了之后其他窗口全空白"）：SelectedItem=null 被 TabControl 自动补选，强摘兜底把呈现器压成局部值——组存活时内容区永久空白（2026-10-07）
- **症状链**：欢迎页原本不可关（刻意设计的永久落地页）；改成可关后，关闭它 → 组内其他标签内容区全黑（标签在、内容没了）。UIA 断言+截图复现实锤。
- **根因（探针三级定位）**：`ReleaseTabContent` 先 `SelectedItem=null` 再布局泵——但 **Avalonia TabControl 清空选中会被立刻自动补选**（被关标签还在 Items 里，常把它自己选回来）→ 布局泵永远等不到内容脱离 → 触发 FORCED-DETACH：`PART_SelectedContentHost.Content = null` 是**局部值，永久压掉模板对 SelectedItem 的绑定** → 组存活的场景里，后续任何换选都不再挂内容 → 空白。**被释放标签在 0 号位时必现**（欢迎页永远 Insert(0)）；重建场景整组丢弃所以楔子无害——这正是它藏到今天的原因。同根因潜伏面：组内重排/关闭 0 号位选中标签。
- **修复（结构性：释放=把选中移走，不是清空）**：ReleaseTabContent 先把选中移到组内其他标签（呈现器自然换内容、被释放页随之脱离，无需强摘）；组内没有其他标签（该组即将随重建/回收丢弃）才保留原"清空+布局泵+强摘"路径（楔子随整组消失）。CloseTab 增量路径/组内重排/重建三处调用方全部受益。
- **验证**：菜单重开欢迎页→UIA 关闭→3D 内容完整渲染（两轮开合循环稳定）；构建 0 错误、34/34 单测。
- **教训**：① **"清空选中"在带自动补选的控件上不是幂等操作**——先证实清空是否真的清空，再设计依赖它的释放/摘除流程；② 强摘兜底改写模板绑定属性=局部值陷阱，只允许用在"即将丢弃的控件"上；③ 用户报"XX 关不了"背后可能连锁着一串结构问题——把行为改成用户预期后，要顺着新路径把下游全走一遍。

---

## BUG-081（用户报"2D图像窗口不显示"）：会话恢复替换模型后，AutoOpen 预建标签沦为孤儿——"已打开"判定只看登记表，聚焦分支静默无操作（2026-10-07）
- **症状**：关闭某面板标签后（或重启后），从面板列表/数据树再打开它毫无反应，整会话都打不开。UIA 列举+探针实锤：标签条只剩欢迎页，OpenPanel 命中 `inTabs=True` 走聚焦分支后无声返回。
- **根因**：Start 的时序是 `AutoOpenPanels()`（把 2D/3D/总线监视开进**默认树**）→ `RestoreSession()`（用**用户保存的树**整体替换模型并重建）。用户保存的树里没有这些面板（关闭过）→ AutoOpen 预建标签在重建中被拆下，但 **`_tabs` 登记表还留着它们=孤儿**。此后 OpenPanel 的"已打开"检查只查 `_tabs` → 命中聚焦分支 → `TabControlOf` 反查不到任何组 → 静默 no-op。
- **修复（结构性：'已打开'必须真可达）**：OpenPanel 三态判定——浮动中=前置激活；**在分屏树里（`_layout.FindLeaf` 非空）=聚焦**；只躺在登记表=孤儿，按新建处理（复用缓存视图、覆盖重建登记条目）。数据树跳转/面板列表/视图菜单全走 OpenPanel，一处修复全入口生效。
- **验证**：欢迎页-only 会话下，数据树行点击→2D 工作台打开且渲染 ✓；3D 同样唤回 ✓；关闭→重开闭环 ✓；用户并行实测"2D 快照+浮动"成功。
- **教训**：① **登记表 ≠ 可达**——"已存在"的判定必须落到真实结构（树/挂载），否则聚焦分支成为静默黑洞；② 启动时序里"先预建、后被会话替换"的模式，凡是替换模型的操作都要清算预建对象的登记（本例的孤儿、BUG-079 的幽灵键同属"模型替换后的遗留物"家族）。

---

## BUG-082（用户报"看日志 有bug"）：「＋ 添加节点」弹层重开触发"StackPanel 已有视觉父级"——Popup 重挂重套模板，复用旧子树被新 ContentPresenter 收养（2026-10-07）
- **症状**：error.log 在 15:22/15:35 反复出现 `InvalidOperationException: The control StackPanel already has a visual parent ScrollContentPresenter ... while trying to add it as a child of ContentPresenter (Host = ScrollViewer)`（含未观察 Task 异常变体）。用户建 7 节点流水线期间反复开关弹层时触发。
- **根因**：`BuildDefList()` 子树为 `Border→ScrollViewer→list(StackPanel)`，作为 `Popup.Child` 常驻复用。Popup 关闭时子树脱离可视树，**重开时重套模板**——ScrollViewer 的新 ScrollContentPresenter 要收养 `list`，而旧 presenter 的父子链没断 → 布局期抛"已有视觉父级"（栈：Grid.MeasureCell→ContentPresenter.ApplyTemplate→UpdateChild，Grid 正是 ScrollViewer 模板内层）。全项目唯一一处该模式（grep `new Popup` 仅此）。
- **修复**：弹层每次打开前重建子树（`if (!popup.IsOpen) popup.Child = BuildDefList();`）——列表小重建成本可忽略，旧子树整棵废弃无父子链残留。
- **验证**：三轮开关弹层，error.log 零新增（修复前每轮一条）；弹层内容完整。
- **教训**：**Popup/弹层的 Child 不要复用控件子树**——重开=重挂+重套模板，模板呈现器与旧父子链冲突是必然；要么每次重建，要么子树里不放"会被模板呈现器收养的 Content"（ScrollViewer.Content/ContentControl.Content）。ContextMenu 无此患是因为菜单项由 Menu 自己的呈现器管理且无 ScrollViewer 包裹复用列表。

---

## BUG-083（用户报"变得不可见了"，附截图）：运行状态按钮用 null 恢复样式——null 是局部值压掉样式，按钮整个不可见（2026-10-07）
- **症状**：运行状态反馈首版在空闲态把 `Background/Foreground` 置 null "恢复默认"——Avalonia 里 **null 是局部值**，优先级高于样式设定：ghost 样式的前景/背景全被压掉 → 文字无画刷、底色透明，按钮整个不可见。
- **修复**：恢复用 `ClearValue(Button.BackgroundProperty)` 清局部值回落样式；运行态只设绿底**不动 Foreground**（ghost 样式的近白前景在绿底上本就清晰，不去和样式打架）。
- **验证**：两态裁剪放大截图——运行中绿底"▶ 运行中…"+停止可用；完成后灰底+停止置灰禁用。
- **教训**：**Avalonia 恢复样式默认 = ClearValue，不是置 null**（null 局部值压样式是 WPF/Avalonia 通坑）；改模板化控件的视觉，能不动 Foreground 就不动——样式体系自己会配好对比度。

---

## BUG-084（用户报"打不开"）：`?.SelectedItem = tab` null 条件赋值是 C# 预览功能——net8.0 编译直接失败（2026-10-13）
- **症状**：应用打不开；`dotnet build` 报 `error CS8652: 功能"null 条件分配"当前为预览版且*不受支持*`，生成失败（start.bat 的 `if errorlevel 1` 分支静默退出）。
- **根因**：`MainWindow.Tabs.cs` SelectTab 里写了 `TabControlOf(tab)?.SelectedItem = tab;`——null 条件赋值（`?.` 左侧赋值）是 C# 13 预览特性，当前 LangVersion（net8.0 默认 C# 12）不允许。
- **修复**：改普通判空：`var tc = TabControlOf(tab); if (tc is not null) tc.SelectedItem = tab;`
- **教训**：**`?.` 只能用于读，不能用于赋值**（除非显式开 `<LangVersion>preview</LangVersion>`）；写完顺手编译验证，别让语法糖把整个宿主挡在门外。

---

## BUG-085（用户报"卡片里的 2D 预览 3D 预览不显示内容"）：未连线节点抛异常中止整图——排在后面的预览节点永远不执行（2026-10-13）
- **症状**：流水线里存在任一"输入未连线"的节点（如拖出来还没连线的发布点云/3D 预览），整图执行失败；已正确连线的 2D/3D 预览卡永远不显示内容。日志特征：`流水线执行失败: 节点「发布点云」执行失败: 输入「输入」未连线`，且其后再无任何节点执行日志。
- **根因**：`GraphExecutor` 对未连线输入 `throw new Exception("输入未连线")` → 整图中止。拓扑排序（Kahn）里未连线节点入度=0 排最前——一个忘连线的输出节点把包括预览卡在内的所有后续节点全部拖死。用户建图过程中"先拖节点再连线"是常态，每拖一个新输出节点预览就会失效一次，体感即"预览坏了"。
- **修复（语义重构：未连线=跳过，不是失败）**：输入端口未连线 → 该节点跳过（状态回灰、日志`[节点名] 跳过：输入「X」未连线`），下游依赖它的节点级联跳过（`跳过：上游未执行`）；已连线的其他分支照常执行。算法真异常（Execute 内部抛出）仍中止整图标红——错误语义与"未配置"语义分离。
- **验证**：新增 `GraphExecutorTests`（4 例）：未连线节点被跳过整图不失败、下游级联跳过、已连线并行分支照常执行、真异常仍中止——42/42 全通过。
- **教训**：① **"配置不完整"与"执行出错"是两类语义**——前者应跳过隔离（局部影响），后者才中止（全局影响）；混用的代价是任何半成品图都无法运行；② 拓扑序里"入度 0"既包括源节点也包括未连线节点，执行器必须区分这两者；③ 用户报"XX 不显示"时先查执行日志的节点执行顺序——证据在日志里。

---

## BUG-086（用户需求"卡片之间的连线支持点选、删除"）：连线 Path 命中区太细难点中——宽透明描边扩大命中 + Del 删除选中（2026-10-13）
- **症状**：连线（2.2px 贝塞尔 Path）无法被点选删除；用户只能删节点来断开连线，误伤其他连线。
- **根因**：原 RedrawEdges 画的连线 Path 没有指针事件处理，且 2.2px 线宽命中区太窄，鼠标几乎点不中（Avalonia Path 命中测试按描边几何算，细线在 125% DPI 下更难点中）。
- **修复**（NodeEditorView.cs）：① 每条连线 Path 绑定 PointerPressed → SelectEdge(c)，e.Handled=true 防止画布 EditorPressed 把刚建立的选中立即清掉；② _selectedEdge 字段记录选中连线，选中时在彩色线之下垫 5.5px 白色高亮线（原色叠上保持类型色可辨）；③ Del 键删除选中连线（与节点删除互斥：选中连线即取消节点选中），删除后 RedrawEdges + ScheduleAutoRun + 日志；④ 点画布空白取消选中（EditorPressed 补 SelectEdge(null)，内部去重零开销）；⑤ 悬停连线光标变 Hand 提示可点。
- **关键词**：连线, 点选, 删除, Path 命中区, SelectEdge, _selectedEdge, Del 键, 高亮
- **教训**：① 细线条图元点中率低的通用解法：命中区与视觉分离——宽透明 Path 垫底接管命中（本项目用彩色实线自身接命中，选中高亮垫底），窄实线只负责显示；② 面板里 Del 删除多目标（节点/连线）时用互斥选中态（_selectedNode / _selectedEdge）保证删除目标唯一；③ 连线 PointerPressed 里必须 e.Handled=true，否则画布 EditorPressed 的 SelectEdge(null) 会立即把刚建立的选中清掉。
