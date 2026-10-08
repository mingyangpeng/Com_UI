# 开发 Bug 记录

> 目的：同步记录开发阶段的代码错误，方便下次遇到类似症状时快速检索。
> 格式：编号 | 日期 | 症状（含原始报错） | 根因 | 修复 | 检索关键词。
> 新 bug 追加到对应分类末尾，编号递增。

## 目录

- [A. Avalonia / 框架 API 差异](#a-avalonia--框架-api-差异)
- [B. 线程与异步](#b-线程与异步)
- [C. 构建 / 工程](#c-构建--工程)
- [D. 逻辑 / 运行时](#d-逻辑--运行时)

---

## A. Avalonia / 框架 API 差异

### BUG-001 | 2026-10-01 | WPF→Avalonia 迁移：CreateView 返回类型
- **症状**：`CS0246 未能找到类型或命名空间名"FrameworkElement"`（插件与宿主多处）
- **根因**：Avalonia 没有 System.Windows.FrameworkElement，对应类型是 `Avalonia.Controls.Control`
- **修复**：Sdk 契约 `Control CreateView()`；所有插件 using 改 Avalonia
- **关键词**：FrameworkElement, Control, 迁移, using System.Windows

### BUG-002 | 2026-10-01 | Avalonia 布局/枚举命名空间
- **症状**：`CS0176 无法使用实例引用来访问成员 VerticalAlignment.Top`、`CS0103 Orientation`
- **根因**：`VerticalAlignment/HorizontalAlignment/Orientation/ScrollBarVisibility` 在 `Avalonia.Layout` 与 `Avalonia.Controls.Primitives`，缺 using 时编译器把右侧解析成同名属性
- **修复**：补 `using Avalonia.Layout;`；ScrollBarVisibility 用全限定 `Avalonia.Controls.Primitives.ScrollBarVisibility.Auto`
- **关键词**：CS0176, CS0103, VerticalAlignment, Orientation, ScrollBarVisibility, Avalonia.Layout

### BUG-003 | 2026-10-01 | StreamGeometry.BeginFigure 参数
- **症状**：`CS7036 未提供与 BeginFigure(Point, bool) 的所需参数 isFilled 对应的参数`
- **根因**：Avalonia 11.3 的 `StreamGeometryContext.BeginFigure(Point, bool isFilled)` 无默认值（master 分支有默认值，不能照抄新代码）
- **修复**：显式传参 `BeginFigure(pt, false)`、`LineTo(pt, true)`
- **关键词**：CS7036, StreamGeometry, BeginFigure, isFilled

### BUG-004 | 2026-10-01 | Avalonia Vector3D 没有叉乘
- **症状**：`CS1061 "Vector3D"未包含"Cross"`、`CS0117 "Vector3D"未包含"CrossProduct"`
- **根因**：`Avalonia.Vector3D`（注意命名空间是 Avalonia 不是 Avalonia.Media.Media3D）只有 Dot/Substract/Multiply 等静态方法，没有 Cross
- **修复**：自己写 `private static Vector3D Cross(a, b)`（注意 Substract 是这个拼写）
- **关键词**：Vector3D, CrossProduct, 叉乘, 3D 数学

### BUG-005 | 2026-10-01 | MenuItem 没有 IsCheckable
- **症状**：`AVLN2000: Unable to resolve suitable regular or attached property IsCheckable on type MenuItem`
- **根因**：那是 WPF 的 API；Avalonia 用 `ToggleType="CheckBox"` + `IsChecked`
- **修复**：XAML 改 `ToggleType="CheckBox"`
- **关键词**：AVLN2000, IsCheckable, ToggleType, MenuItem, CheckBox

### BUG-006 | 2026-10-01 | 附加属性 ToolTip 的 XAML 写法
- **症状**：`AVLN2000: Unable to resolve ... property ToolTip on type Button`
- **根因**：Avalonia 的 ToolTip 是附加属性，写法是 `ToolTip.Tip="..."`
- **修复**：XAML 用 `ToolTip.Tip`；代码用 `ToolTip.SetTip(ctrl, text)`；**对象初始化器里不能写附加属性**（CS0747/CS0117）
- **关键词**：AVLN2000, ToolTip.Tip, SetTip, 附加属性, CS0747

### BUG-007 | 2026-10-01 | MultiTrigger 条件写法（WPF 版时期）
- **症状**：`MC3074: 标记"Conditions"在 XML 命名空间中不存在`
- **根因**：WPF 里也必须写 `<MultiTrigger.Conditions>` 而不是 `<Conditions>`
- **修复**：`<MultiTrigger><MultiTrigger.Conditions><Condition .../></MultiTrigger.Conditions></MultiTrigger>`
- **关键词**：MC3074, MultiTrigger, Conditions, XAML

### BUG-008 | 2026-10-02 | Dock.Avalonia 11.3 工厂没有流式 API
- **症状**：`CS1061 "Factory"未包含"Tool"的定义`（照官方 master 示例写 `f.Tool(out var t, ...)`）
- **根因**：流式构建器（WithId/AppendTool/Add...）是 Dock 12.x 的 API；11.3 用 `Create*` 工厂方法
- **修复**：`f.CreateTool()/CreateDocumentDock()/CreateToolDock()/CreateProportionalDock()/CreateProportionalDockSplitter()/CreateRootDock()/CreateList<T>(...)`，属性逐个赋值
- **关键词**：Dock.Avalonia, Factory, 流式 API, CreateTool, 版本差异, master 示例

### BUG-009 | 2026-10-02 | Dock.Avalonia 11.3 的 Document/Tool 没有 Content 属性
- **症状**：`CS0117 "Tool"未包含"Content"的定义`
- **根因**：Dock 11.3 用 **`Context`** 属性承载内容（`IDockable.Context`），且 Create* 返回接口类型（IDocumentDock/IToolDock/IRootDock），不能赋给具体类变量
- **修复**：`doc.Context = 视图控件`；字段类型全用接口
- **关键词**：CS0117, CS0266, Context, IDockable, DocumentDock, 接口类型

### BUG-010 | 2026-10-02 | DockControl 没有 DocumentTemplate/ToolTemplate
- **症状**：`AVLN2000: Unable to resolve ... property DocumentTemplate on type DockControl`；随后工具窗/文档**内容全空白**（无报错）
- **根因**：Dock 11.3 的 DockControl 按可停靠对象（dockable）的**类型**在可视化树里匹配 DataTemplate，内容区域绑定的是 dockable 本身，需要为 Document/Tool 类型提供模板渲染其 Context
- **修复**：MainWindow 加
  ```xml
  xmlns:model="using:Dock.Model.Mvvm.Controls"
  <Window.DataTemplates>
      <DataTemplate DataType="model:Document"><ContentControl Content="{Binding Context}"/></DataTemplate>
      <DataTemplate DataType="model:Tool"><ContentControl Content="{Binding Context}"/></DataTemplate>
  </Window.DataTemplates>
  ```
- **关键词**：AVLN2000, DockControl, DataTemplate, DataType, Context, 内容空白, DataContext

### BUG-011 | 2026-10-02 | x:Name 不能标在 ColumnDefinition 上
- **症状**：代码后台引用 `LeftCol/BottomRow` 报 `CS0103 当前上下文中不存在名称`
- **根因**：Avalonia XAML 编译器只为控件生成 x:Name 字段，ColumnDefinition/RowDefinition 不是控件
- **修复**：通过父级 Grid 的 `ColumnDefinitions[i]/RowDefinitions[i]` 访问
- **关键词**：CS0103, x:Name, ColumnDefinition, RowDefinition, GridSplitter 布局

### BUG-012 | 2026-10-02 | NumericUpDown 是 decimal
- **症状**：`CS0266 无法将 double 隐式转换为 decimal?`
- **根因**：Avalonia NumericUpDown 的 Value/Minimum/Maximum/Increment 全是 decimal
- **修复**：`(decimal)doubleValue`、收集时 `(int)(Value ?? 0m)`
- **关键词**：CS0266, NumericUpDown, decimal

### BUG-013 | 2026-10-02 | Run / TextBlock.Inlines 命名空间
- **症状**：`CS0246 未能找到类型或命名空间名"Run"`（已有 using Avalonia.Media）
- **根因**：Run 在 `Avalonia.Controls.Documents`
- **修复**：`using Avalonia.Controls.Documents;`
- **关键词**：CS0246, Run, Inlines, Documents

### BUG-014 | 2026-10-02 | XAML 生成字段被私有字段遮蔽 ★高危
- **症状**：运行时 `NullReferenceException` 在 SetStatus/RefreshCount —— 布局显示正常但功能随机失效，且崩溃点远离真实原因
- **根因**：自己声明了 `private TextBlock _statusText` 想引用 XAML 的 `x:Name="StatusText"`，但**从未赋值**。Avalonia 生成的字段名就是 `StatusText`（与 x:Name 相同），`_statusText` 是另一个永远为 null 的字段
- **修复**：ctor 里 `InitializeComponent();` 之后 `_statusText = StatusText;` 逐个接线
- **关键词**：NPE, NullReferenceException, x:Name, InitializeComponent, 字段遮蔽, 生成字段, 说是就绪但不工作

### BUG-015 | 2026-10-02 | 进度回调线程崩溃（进程直接退出）
- **症状**：`InvalidOperationException: Call from invalid thread` at `TextBlock.set_Text`，**进程整个崩掉**（异常发生在线程池线程，不走 Dispatcher 异常处理）
- **根因**：`new Progress<string>()` 在后台线程创建时没有同步上下文，回调直接在线程池执行；宿主 SetStatus 直接改 UI 控件
- **修复**：宿主服务层（SetStatus 等）内部做 `Dispatcher.UIThread.CheckAccess()` / `Post`，**不依赖调用方在 UI 线程**
- **关键词**：Call from invalid thread, Progress, 线程, 崩溃, 进程退出, VerifyAccess

## B. 线程与异步

### BUG-016 | 2026-10-02 | 对话框 fire-and-forget + tcs 竞态 → 静默失败
- **症状**：点击"执行"后**什么都没发生**（无日志、无报错、无崩溃）
- **根因**：`_ = dlg.ShowDialog(owner); var ok = await tcs.Task;` 模式下，OK 回调/收值逻辑抛出的异常进入**未观察任务**，被完全吞掉
- **修复**：直接 `await dlg.ShowDialog(owner)`，值收集放在 OK 回调内 try/catch；App 订阅 `TaskScheduler.UnobservedTaskException` 写 error.log（注意该事件在 GC 后才触发，日志时间会滞后）
- **关键词**：静默失败, 无响应, 未观察异常, UnobservedTaskException, ShowDialog, TaskCompletionSource, 竞态

### BUG-017 | 2026-10-02 | DataBus 泛型 T 不匹配导致订阅方收不到数据
- **症状**：算法日志显示"已发布 Image → image/stitched"，但预览面板始终显示"暂无数据"，**无任何异常**
- **根因**：反射场景 `bus.Publish(topic, value)` 的编译期 T=object，`LatestType` 记成 object；订阅方 `Action<ImagePayload>` 在交付时 `is Action<T>` 匹配失败被静默跳过
- **修复**：Publish 记录**运行时类型** `payload.GetType()`；Deliver 匹配失败时回退 `Delegate.DynamicInvoke(payload)`
- **关键词**：泛型, object, 反射, 发布订阅, 收不到数据, DynamicInvoke, LatestType, 静默丢数据

### BUG-023 | 2026-10-02 | 同名类型无法互转（类型身份分裂）★高危
- **症状**：`ArgumentException: Object of type 'ComUI.Sdk.ImagePayload' cannot be converted to type 'ComUI.Sdk.ImagePayload'`（同名类型"自己转不了自己"）
- **根因**：两连击 —— ① 插件 csproj 把 `ComUI.Sdk.dll` 一起复制进了插件文件夹；② 宿主扫描插件文件夹时对**所有** DLL 调 `LoadFromAssemblyPath`，把 Sdk 拉进了插件独立 ALC。之后插件程序集解析 Sdk 引用时**优先绑定同 ALC 内已加载的副本**（根本不会走 Load() 覆写回退默认上下文）→ 插件里的 ImagePayload 和宿主的 ImagePayload 是两个不同程序集的同名类型
- **修复**：① csproj 复制排除 `ComUI.Sdk.*`（Sdk 随宿主分发）；② 扫描时先 `AssemblyName.GetAssemblyName(dll)` 读元数据，宿主默认上下文已有的程序集直接跳过
- **关键词**：cannot be converted to type, 类型身份, AssemblyLoadContext, LoadFromAssemblyPath, Sdk 副本, 双份类型, 插件隔离, ArgumentException 同名

## C. 构建 / 工程

### BUG-018 | 2026-10-01 | .NET 10 SDK 生成 .slnx 而非 .sln
- **症状**：`MSB1009: 项目文件不存在（开关:D:\pmy\Com_UI\ComUI.sln）`
- **根因**：.NET 10 SDK 的 `dotnet new sln` 默认生成新格式 `ComUI.slnx`
- **修复**：直接 `dotnet build ComUI.slnx`
- **关键词**：MSB1009, slnx, sln, dotnet new sln, .NET 10

### BUG-019 | 2026-10-01 | MSBuild 复制目标路径层级错误 + 残留毒目录 ★连环坑
- **症状 1**：构建成功但 comdll 为空，Copy 任务提示"两个文件大小及时间戳一致"（文件其实复制到了错误位置）
- **根因 1**：插件在 `src/Plugins/X/` 三层深度，`$(MSBuildThisFileDirectory)..\..\comdll` 只回到 `src/`，文件全进了 `src/comdll`
- **症状 2**：修好路径后，宿主启动时 FindUpward("comdll") 被**残留的 src/comdll** 截胡，日志显示扫描 `D:/pmy/Com_UI/src/comdll/ui`（0 个子文件夹）→ 插件 0 个
- **修复**：路径改 `..\..\..\comdll`；**删除 src/comdll 残留**。教训：用目录名做向上探测时，仓库内该名字的目录必须唯一；复制目标路径错了会留下毒目录持续作祟
- **关键词**：MSBuildThisFileDirectory, 复制目标, 路径层级, FindUpward, 残留目录, 插件 0 个, 扫描路径不对

### BUG-020 | 2026-10-02 | exe 被占用导致构建失败
- **症状**：`MSB3027 无法将 apphost.exe 复制到 ComUI.Host.exe，文件被另一进程使用`
- **根因**：上一次启动的 ComUI.Host.exe 还在运行
- **修复**：`taskkill //IM ComUI.Host.exe //F` 后再构建（git-bash 里双斜杠）
- **关键词**：MSB3027, MSB3021, 文件被占用, taskkill, 重新构建前先关进程

## D. 逻辑 / 运行时

### BUG-021 | 2026-10-02 | `out` 作为 C# 参数名
- **症状**：`CS0103/编译错误`（3D 渲染器局部函数参数名用了 out）
- **根因**：`out` 是 C# 关键字，不能作标识符
- **修复**：改名 `dst`
- **关键词**：out 关键字, 参数名, CS 错误

### BUG-022 | 2026-10-02 | IProgress<T>.Report 只有 1 个参数
- **症状**：`CS1501 "Report"方法没有采用 2 个参数的重载`
- **根因**：.NET 的 IProgress<T>.Report(T) 单参数；想要 (percent, message) 两参要自己包一层
- **修复**：局部函数 `void Report(int pct, string msg) => progress?.Report($"[{pct}%] {msg}");`
- **关键词**：CS1501, IProgress, Report, 进度

### BUG-024 | 2026-10-02 | 代码构建的 Grid 单元格全叠在第一列 → 文字重叠"看似乱码" ★易误判
- **症状**：总线监视面板文字互相覆盖成"乱码"（如 `Clou98981oad`），只看得到一列内容；无任何报错
- **根因**：代码动态往 `Grid` 里加 TextBlock 时**忘了设 `Grid.ColumnProperty`**，4 个单元格全排在 Column 0 上互相覆盖；XAML 写法会显式写 Grid.Column，代码构建时极易漏
- **修复**：`tb.SetValue(Grid.ColumnProperty, col);`（同理注意 Grid.Row）
- **关键词**：Grid.Column, ColumnProperty, 文字重叠, 乱码, 看似乱码, 代码构建控件, 叠加
- **变体（2026-10-02 节点卡片）**：代码构建的 Grid 多列布局里，第二个子元素忘设 Column 且列定义含 Auto——第一个子元素（长文本）会挤进 Auto 列把状态图标挤出可视区。规则：**代码构建 Grid 时给每个子元素显式设列**，一个都不例外。

### BUG-025 | 2026-10-02 | 命令重入：对话框打开期间再点命令 → 空参数执行 ★隐蔽
- **症状**：点"执行"后算法报 `缺少参数 points`（GetParam 找不到任何参数），但对话框明明填了值；参数收集代码看起来完全正确
- **根因**：`RunCommandAsync` 是 fire-and-forget（`_ = RunCommandAsync(...)`），防重入守卫 `_runCts` **在对话框关闭后才赋值**——对话框打开期间（await ShowDialog 让出线程）再次点击命令会并发进入第二份执行，两份各有自己的 values 字典，先关的那份对话框把值收进自己的字典，真正跑起来的执行拿到的却是空字典
- **修复**：方法入口用 `Interlocked.CompareExchange(ref _runGate, 1, 0)` 抢占执行门（对话框打开期间也占用），外层 finally 归零；`_runCts` 仅用于取消
- **关键词**：重入, fire-and-forget, 防重入, 空参数, 缺少参数, 对话框并发, Interlocked, 双击, ShowDialog 期间

### BUG-026 | 2026-10-02 | 重建布局时控件已有 visual parent → 布局线程崩溃 ★必然复发
- **症状**：F5 重载 / 视窗重排后随机崩溃：`InvalidOperationException: The control SidebarHost already has a visual parent ContentPresenter while trying to add it as a child`（error.log 可查）
- **根因**：重建 Dock 布局时**复用了**日志/属性等视图实例，旧布局还挂在 DockControl 上未卸下，新工具窗又添加同一控件 → 同一视觉节点两个父级
- **修复**：`BuildLayout` 开头先 `Dock.Layout = null` 卸下旧布局，并**新建** LogView/PropertiesView 实例（字段可变）、清空面板视图缓存；日志订阅 lambda 读字段所以自动指向新实例
- **关键词**：already has a visual parent, 双父级, 布局重建, 重载崩溃, Dock.Layout null, 视觉树, 控件复用
- **变体（实测崩溃 13:35）**：同样错误发生在 `PluginListSidebar` 上——BUG-026 修复时重建了 LogView/PropertiesView 但**漏了侧边栏视图**（readonly 字段复用）。教训：**凡是"重建布局时复用的控件实例"都要逐一排查**，最好统一在 BuildLayout 开头全部新建。

### BUG-027 | 2026-10-02 | .bat 文件含 UTF-8 中文 → cmd 按 GBK 解析成乱码且打碎命令行
- **症状**：双击 start.bat 报 `'冲彴' 不是内部或外部命令`、`'build' 不是内部或外部命令`（连 `dotnet build` 都被切碎），界面全是乱码
- **根因**：Write 工具写出的是 UTF-8 文件；中文 Windows 的 cmd.exe 用 ANSI 代码页（GBK/CP936）**逐行解析批处理**，UTF-8 中文注释的字节被错读成 GBK 乱码（"启动"→"冲彴"），多字节序列还会吞掉换行导致后续命令被截断
- **修复**：**bat 文件内容只用 ASCII**（注释和 echo 都用英文），任何代码页都不会出错；若必须中文则首行加 `chcp 65001 >nul`（仍有边角风险，不推荐）。.sh 文件无此问题（Linux 原生 UTF-8）
- **关键词**：bat 乱码, 冲彴, cmd 编码, GBK, CP936, chcp 65001, 批处理中文, 不是内部或外部命令

### BUG-028 | 2026-10-02 | Avalonia 代码设 Foreground=null 不等于"继承样式" → 图标变黑不可见
- **症状**：活动栏按钮非选中状态下图标完全看不见（深色底上近黑前景），选中态正常
- **根因**：切换高亮时写了 `btn.Foreground = active ? White : null`——Avalonia 里**本地值 null 会覆盖样式里的 Foreground 定义**并落到属性默认值（近黑），而不是回到样式的灰色；Background=null 无影响（背景默认即透明），Foreground 这种继承属性会拿 null 渲染
- **修复**：非选中态显式设置颜色（如 #868690），不要用 null"还原"；或把颜色全部放进样式用伪类（:checked/:pointer）切换，代码里只动 Classes
- **关键词**：Foreground null, 本地值覆盖样式, 图标不可见, 深色背景黑字, 默认前景色, 样式回退

### BUG-029 | 2026-10-02 | 画布平移处理器抢走内部控件的指针捕获 → Slider/ComboBox 失灵
- **症状**：节点编辑器里点击/拖动节点上的 Slider、ComboBox 完全无反应（参数改不了），节点拖动和画布平移正常
- **根因**：编辑器根控件（_editor，Border）的 PointerPressed 里写 `e.Source is Border` 判断是否平移——**Slider/ComboBox 的控件模板内部也是 Border**，点击它们时 e.Source 命中条件成立，根控件抢了 `Pointer.Capture`，内部控件再也收不到移动/释放事件
- **修复**：平移仅在 `ReferenceEquals(e.Source, 画布/编辑器背景)` 时触发；教训：**祖先级 PointerPressed + 模板内类型判断** 会误伤所有同类型模板控件，必须按"是否背景本身"判定，且注意 Capture 会截断后续事件流
- **关键词**：指针捕获, PointerCapture, e.Source, Slider 不响应, ComboBox 失灵, 模板 Border, 平移, 事件抢占, 祖先事件

### BUG-030 | 2026-10-02 | git-bash 管道执行 python 脚本按 GBK 解码 → 写入文件的中文变乱码 ★工具链
- **症状**：用 `python - <<'EOF'` 补丁修改含中文的 C# 文件后，插入的新代码里中文全变成"鏂板缓…"式乱码，且替换串里的 `\\n` 变成真实换行导致 `CS1010 常量中有换行符`
- **根因**：中文 Windows 的 git-bash 把 heredoc 脚本按本地代码页（GBK）喂给 python 的 stdin（python3 默认按 UTF-8 解析源码，此处由 stdin 编码决定），中文双编码错乱；替换目标匹配失败时 replace 静默不生效，写入时反而把乱码写进文件
- **修复**：**含中文的文件修改一律用 Edit/Write 工具**，python 补丁只做纯 ASCII 的定位替换；写入后必须 `grep` 复查中文是否完好
- **关键词**：python heredoc, git-bash, GBK, 中文乱码, CS1010, stdin 编码, 替换失败, 工具链

### BUG-031 | 2026-10-02 | Avalonia Pen 没有 IBrush 开头的 5 参构造 → 重载解析错位
- **症状**：`CS1503 参数 1 无法从 IBrush 转换为 uint`（想用 `new Pen(brush, 2.2, null, cap, cap)` 设置圆头笔帽）
- **根因**：Avalonia 11.3 的 Pen 构造是 `Pen(IBrush, double, IDashStyle, PenLineCap, PenLineJoin, double)`（**6 参**）和 `Pen(uint, ...)`；传 5 个参数时缺 miterLimit，重载解析落到 uint 版本报错。另外 **Shape（Path 等）没有 StrokeCap 属性**
- **修复**：`new Pen { Brush = ..., Thickness = ..., LineCap = ..., LineJoin = ... }` 对象初始化器；Shape 上只设 Stroke/StrokeThickness
- **关键词**：Pen 构造, CS1503, uint, PenLineCap, Round, Shape StrokeCap, 重载

### BUG-032 | 2026-10-02 | 节点编辑器三连：拖拽失效 / 多余直线 / 连线不显示
- **症状 1**：算子库算子拖不到流水线画布（OLE DoDragDrop 路径整体无效）
- **症状 2**：连接两算子时出现"一条曲线 + 一条多余直线"
- **症状 3**：连线松手后连线不显示，要拖动一下节点才出现
- **根因**：① OLE 拖拽（DragDrop.DoDragDrop）在此宿主环境不可靠 → 改为**指针捕获式放置**：按下即 Capture(item)，松手 `PointToScreen` 拿屏幕坐标，经 `PlaceNodeAtScreen` → `PointToClient` 反算世界坐标创建节点（全程进程内，确定性生效）；② 临时预览线与真实连线画在**同一个 Canvas**，清理时序有竞态 → 预览线拆到独立 `_tempLayer`，松手 `Children.Clear()` 整层清空；③ `OnPointerReleased` 里 `Connect` 之后**漏调 `RedrawEdges()`** → 补上并 `_edges.InvalidateVisual()`
- **修复要点**：PointToScreen 返回 PixelPoint、PointToClient 接收 PixelPoint（事件签名用 PixelPoint 而非 Point）；`_world.PointToClient` 会自动反算 RenderTransform 得到世界坐标，可直接用于落点
- **补充根因（实测确认）**："两条线"主因 = **从已连线的输出端口拉线时不拆旧线**：旧连线（曲线）+ 新拖的预览线（直线观感）同时可见。修复：输出端口拉线 = 拆线重连（`DisconnectOutput` 移除该输出上的所有连线后再进入预览拖拽），与输入端口语义对齐
- **补充根因（实测确认）**："两条线"主因 = **从已连线的输出端口拉线时不拆旧线**：旧连线（曲线）+ 新拖的预览线（直线观感）同时可见。修复：输出端口拉线 = 拆线重连（`DisconnectOutput` 移除该输出上的所有连线后再进入预览拖拽），与输入端口语义对齐
- **再补充（用户复测仍见两条白线）**：预览线改用 **StreamGeometry + CubicBezierTo**（与正式连线同图元语义），且**每次移动整层 Clear 后只放一条**——排除 PolyBezierSegment 语义疑点与任何残留；若仍复现需录屏定位
- **关键词**：拖拽失效, DoDragDrop, 指针捕获, PointToScreen, PointToClient, PixelPoint, 多余直线, 两条白线, 临时线残留, 连线不显示, RedrawEdges, 拖动才显示, 输出端口拆线, StreamGeometry

### BUG-033 | 2026-10-02 | 重建布局后属性面板内容为空 / 工具面板被隐藏后无法找回
- **症状**：① 视窗重排/F5 后右侧属性面板只剩表头没有内容；② 属性/日志工具窗被 Dock 标题栏的隐藏按钮收起后，没有任何入口能找回
- **根因**：① BuildLayout 重建时新建了 `_propsView` 实例但只在启动时填充过宿主信息，重建后未回填；② Dock.Avalonia 工具窗自带隐藏按钮（CanClose=false 只禁了关闭，没禁隐藏）
- **修复**：① BuildLayout 末尾统一回填 `SetHostRows` + 未选择占位；② 新增 `AppDockFactory.EnsureTool`（按 Id 查找工具，缺失则重建并激活），视图菜单加"显示属性面板/显示日志面板"；③ BuildLayout 整体 try/catch，异常时回退最小默认布局，杜绝整窗空白
- **关键词**：属性面板 空, 内容为空, 工具窗 隐藏, 找回, EnsureTool, 视窗重排, 重建布局, 白屏, 面板消失, 占位

### BUG-034 | 2026-10-02 | 点击工具窗"固定"按钮后面板内容消失 ★Dock 语义坑
- **症状**：多次点击右侧属性面板和标题栏的"固定"(📌)按钮后：① 整个右侧面板消失无入口找回；② 点日志的固定后标题栏还在但内容区全空
- **根因**：Dock.Avalonia 工具窗标题栏的"固定"= **Pin（钉住）**：点击后工具从 ToolDock 的 VisibleDockables 移入 RootDock.PinnedDockables（收起为边缘小图标条），停靠区内容随之清空；布局里没有配置可见的 PinnedDock，内容等于凭空消失。且钉住后 EnsureTool 按"面板内 VisibleDockables 找 Id"会找不到，重复创建还会产生同 Id 冲突
- **修复**：所有常驻工具 `CanPin = false`（标题栏固定按钮不再显示，共 5 处：导航/日志/属性/插件工具/EnsureTool 动态创建）；验证：辅助树中每工具只剩 1 个按钮（隐藏）。教训：**Dock 库的按钮语义要对文档确认，"固定"≠"保持可见"而是"钉到边缘"**；找不到时优先查 PinnedDockables
- **关键词**：固定按钮, Pin, CanPin, PinnedDockables, 钉住, 工具窗消失, 内容全没, Dock.Avalonia 语义

### BUG-035 | 2026-10-02 | 移除 Dock.Avalonia 手写外壳：重写过程中的连环坑 ★多处根因离症状很远
- **背景**：Dock 语义坑（BUG-033/034）+ 侧边栏交互始终不符合预期 → 整体移除 Dock.Avalonia，手写 Grid 确定性外壳（活动栏/侧边栏/标签页/属性/日志全宿主直管）
- **坑 1（启动即崩溃）**：TabControl 的 `SelectionChanged` 在 `InitializeComponent()` 期间就触发一次（选择模型初始化），此时 code-behind 的 `_manager` 还没赋值 → NRE 段错误。**修复**：事件处理器开头 `if (_manager is null) return;`。教训：**AXAML 里挂的事件处理器可能在构造函数完成前被调用，处理器不得依赖 ctor 后段初始化的字段**
- **坑 2（AVLN2000）**：StackPanel 没有 BorderBrush/BorderThickness 属性（它是 Decorator 不是 TemplatedControl），AXAML 编译报"Unable to resolve suitable regular or attached property"。要边框就用 Border 包一层
- **坑 3（主显示区被压扁）**：Grid `ColumnDefinitions="46,235,4,*,4,255"` 有 6 列，但元素只写到 column 2/3/4 —— 主区网格被放进 4px 的分隔列，255 列空着。**列定义和元素 Grid.Column 要一一对账**
- **坑 4（分隔条全废）**：App.axaml.cs 里残留全局 `GridSplitter.Width=1/Height=1` 样式（Dock 时代发丝线），把新外壳的 4px 分隔条压成 1px。删样式
- **坑 5（编译报错具有欺骗性）**：Roslyn 在声明阶段有错（如 CS0246 找不到类型）时**跳过方法体绑定**，只报两三个字段错误，方法体里十几处缺失成员全部隐藏。看到"声明错误"先补全类型，再重新编译看第二批
- **坑 6（测试侧记）**：Avalonia 无障碍树在复杂状态（节点编辑器 + 算子库侧边栏同屏）下可能把活动栏按钮整个丢出树外；元素点击（AXPress）不受影响，坐标点击对后台窗口无效。自动化验证优先用元素索引点击
- **关键词**：Dock 移除, 手写外壳, SelectionChanged, InitializeComponent, NRE, AVLN2000, StackPanel Border, 列错位, GridSplitter 样式残留, 声明错误隐藏方法体错误, 无障碍树丢按钮

### BUG-036 | 2026-10-02 | 活动栏 🧰 emoji 渲染成超大位图，溢出按钮遮挡侧边栏 ★字体渲染坑
- **症状**：活动栏第 4 个图标（算子库 🧰）比其他图标大数倍，红色彩色位图溢出 46px 宽的活动栏列，盖住侧边栏「3D 点云」分组行的左半截；其余 emoji（🧩⚡⚙🧾）正常
- **根因**：Windows 上 🧰（U+1F9F0）等部分 SMP emoji 由 Segoe UI Emoji 以**彩色位图字形**渲染，实际尺寸远超 FontSize=17；按钮内容不裁剪，直接溢出。不同 emoji 落不同回退字体，表现不一致
- **修复**：活动栏图标统一 `new Viewbox{ Width=18, Height=18, Child=TextBlock(icon) }` 归一化（超大的缩小、正常的微放大，视觉一致），按钮加 `ClipToBounds=true` 兜底；PluginListSidebar 行内图标同理归一化 14×14（防止同样的溢出压到标题）。代价：按钮的无障碍 Name 从 emoji 变成控件类型名（ToolTip 仍在）
- **关键词**：emoji 遮挡, 图标过大, Segoe UI Emoji 位图字形, Viewbox 归一化, ClipToBounds, 图标溢出, 算子库图标, 遮挡侧边栏

### BUG-037 | 2026-10-03 | 算子库分组头色带"超出左边" + Tool 面板从面板列表误开成标签页导致侧边栏变空 ★两个问题一个根
- **症状 1**：算子库侧边栏里 IO / 3D 点云 / 2D 图像 分组头色带比搜索框（8px 边距）和算子行（22px 缩进）都靠左，全宽贴到侧边栏左缘，视觉上"超出左边"
- **症状 2**：在「面板」列表里点"算子库"（Tool 面板）会把它开成中间标签页；之后点活动栏 🧰 想切侧边栏视图时，**同一个单实例视图已被标签页持有**，塞进侧边栏失败 → 侧边栏变空白
- **根因**：① GroupHeader/MakeRow 的 Border 无水平边距（全宽贴边），与其它内容的边距不一致；② 面板列表回调对所有面板一律 OpenPanel 开标签页，而活动栏的侧边栏视图与标签页**共享 GetPanelView 单实例缓存**——Avalonia 一个控件只能有一个视觉父级
- **修复**：① 分组头/行统一 `Margin(8,0,8,0) + CornerRadius(4)`（行内缩进 22→14，内容绝对位置不变）；② 面板列表点击走 `ShowPanel`：**Tool 面板 → 按PanelId 找活动栏条目切侧边栏（与点图标同语义），Document 面板 → 开标签页**。ActivityEntry record 加 PanelId 字段用于反查。设计定案：**Tool 面板的家是侧边栏视图，Document 面板的家是标签页，二者不可混**
- **验证**：像素测量分组头色带起点从 x=31（贴边）变为 x=37（边界+8px×缩放）✔；面板列表点算子库只切侧边栏不开标签页 ✔；活动栏 🧰 开合切换正常 ✔
- **关键词**：分组头贴边, 超出左边, 算子库, Tool 面板, 标签页, 单实例缓存, visual parent, 侧边栏空白, 面板列表, ShowPanel, ActivityEntry PanelId

### BUG-038 | 2026-10-04 | 插件原生依赖链四连坑：OpenTK 在插件里加载不起来 ★M2 第一个硬骨头
- **症状**：Image2D 插件（OpenTK 离屏渲染）面板显示"面板创建失败：Could not load OpenTK.Windowing.Desktop"；修完变"软件回退（无 GL 上下文）"；再修变成 BadImageFormat(0x8007000B)；最后 GL 起来了但 P/Invoke 找不到 'glfw'
- **根因 ①**：PluginManager 构造 PluginLoadContext 时 **mainDllPath 恒为 null** → AssemblyDependencyResolver 从未生效，原生库（glfw3.dll）永远解析不到。修复：按"文件夹内 *.deps.json 同名 .dll"推断主程序集传入
- **根因 ②**：插件输出不自包含——AssemblyDependencyResolver **只在插件文件夹内**解析依赖（不查 NuGet 缓存），宿主兜底只覆盖宿主已知程序集（Avalonia/Sdk 等）。修复：Image2D csproj `CopyLocalLockFileAssemblies=true`（OpenTK 全家进输出）+ Avalonia `ExcludeAssets="runtime"`（运行时用宿主版本，防类型身份分裂 + 省 40MB）+ CopyToComdll 改递归拷贝（`**\*.*` + `%(RecursiveDir)`，保留 runtimes/ 布局）
- **根因 ③**：原生库平铺到插件根目录时 `runtimes\**\native\*.*` 把 win-x86 的 glfw3.dll 覆盖了 win-x64 的 → BadImageFormat。修复：Include 只取 win-x64/linux-x64/linux-arm64。**校验手段：python 读 PE 头 machine 字段确认架构**（0x8664=x64）
- **根因 ④**：OpenTK 4.9 的 GLFW P/Invoke 名是 **"glfw"**，包内文件叫 **glfw3.dll**。修复：平铺 glfw3.dll 到插件根目录（OpenTK 自带解析器探测自身目录）+ PluginLoadContext.LoadUnmanagedDll 加 glfw→glfw3 别名 + loose 兜底（与托管 loose 对齐）
- **诊断手段**：GlImageRenderer 构造 catch 里 `Console.Error.WriteLine`（重定向进 host_run.log）；这是"静默吞异常降级"的教训——**降级路径必须留诊断痕迹，否则只能靠猜**
- **关键词**：OpenTK, 插件原生依赖, AssemblyDependencyResolver, mainDllPath, CopyLocalLockFileAssemblies, glfw, glfw3, BadImageFormat, x86 覆盖, runtimes 平铺, 自包含插件, ExcludeAssets runtime

### BUG-039 | 2026-10-04 | F5 重载后 UI 插件 0 个：程序集"宿主已有"误判 ★隐藏最深的旧账
- **症状**：点"重新加载插件"后所有 UI 插件消失，日志刷"跳过宿主已有程序集 ComUI.Plugin.NodeGraph / OpenTK.Windowing.Desktop"——**插件自身和它的依赖被当成了宿主程序集**
- **根因**：PluginManager.LoadAll 的跳过名单取自 `AppDomain.CurrentDomain.GetAssemblies()`——**整个进程**的程序集，包括 collectible ALC 里卸载不完全的旧插件/依赖程序集（OpenTK 有原生状态、静态注册表等残留根，卸载经常不彻底）。第一次加载后这些名字永久留在进程里 → 第二次扫描全部误判跳过
- **修复**：改查 `AssemblyLoadContext.Default.Assemblies`（真正的宿主上下文）。这样：宿主/框架/contracts（Default 里）仍被跳过（类型身份保护不变）；collectible ALC 残留不再阻塞重载（新 ALC 加载自己的新副本）
- **附带修复**：重载后 AutoOpen 的 Document 面板不再自动恢复 → 提取 `AutoOpenPanels()`，启动与 F5 共用
- **关键词**：F5, 重新加载插件, UI 插件 0 个, AppDomain.GetAssemblies, AssemblyLoadContext.Default, 宿主已有程序集 误判, collectible ALC 卸载不完全, AutoOpen 恢复

### BUG-040 | 2026-10-04 | 两个 GL 插件共存 → 原生 AV 秒杀进程 ★无任何托管栈可查
- **症状**：新增 Cloud3D 插件后点开其标签页，进程直接死亡；error.log 无新条目，.NET Runtime 事件只有"exception code c0000005"没有调用栈
- **根因**：UI 线程上每个 GL 渲染器各自持有一个上下文，**`MakeCurrent` 会抢走整个线程的"当前上下文"**。Image2D 的 Draw 每次都 MakeCurrent（正确），但 Cloud3D 的 `UploadCloud`（以及 Image2D 的 `UploadImage`）没抢——总线重放触发上传时，当前上下文是别人的：GL 调用全部无效（GL error 静默），**点云 VBO 实际是 0 字节**；之后 DrawArrays(100000) 在 attrib 数组指向空缓冲的情况下让驱动越界读 → c0000005
- **修复**：**每个 GL 入口（Upload/Draw/Render）第一行都 `_win?.MakeCurrent()`**——同线程多上下文的铁律：不能假设"上次是我的"
- **诊断手段**：GL 错误码轮询（CheckGl→stderr）+ .NET Runtime 事件日志；这类 AV 没有 managed 栈，只能靠缩小范围（哪些 GL 调用、什么顺序）+ 检查 GL 数据是否真的落地
- **关键词**：多 GL 上下文, MakeCurrent, 抢上下文, c0000005, AccessViolation, 驱动越界, VBO 落空, 无托管栈, .NET Runtime 1026

### BUG-041 | 2026-10-04 | 点云画出来是空的：MVP 矩阵约定（组合顺序 + 上传转置）★GL 经典坑
- **症状**：点云上传成功、DrawArrays 无 GL 错误，但视口全黑
- **诊断**：在 C# 按与 GLSL `uMVP * v` **完全相同的内存语义**手工求值 NDC（不能依赖 Vector4.Transform，OpenTK 的重载语义不同），四种组合一次跑全：`proj*view/false` → w=0 全裁；**`view*proj/true` → center(0,0,0.98) edge(0.29,-0.23,0.98) 正确**
- **根因**：OpenTK 4 的 Matrix4 是**行向量约定**（变换 = v·M），组合顺序应为 `view * proj`；上传给 GLSL 列向量语义时必须 `UniformMatrix4(loc, true, ref mvp)`（转置）
- **修复**：`var mvp = view * proj; GL.UniformMatrix4(_uMvp, true, ref mvp);`，GLSL 保持 `uMVP * vec4(aPos,1)`
- **教训**：GL 数学库矩阵约定（行/列向量、内存布局）三件事——乘法顺序、上传 transpose、着色器乘法方向——**必须成套**，错一个就是"渲染无错误但全黑"。NDC 手工求值是最快的定位手段
- **关键词**：MVP 矩阵约定, 行向量, 列向量, transpose, view*proj, proj*view, NDC 诊断, 渲染全黑, w=0, OpenTK Matrix4

### BUG-042 | 2026-10-04 | 相机在转、画面冻结：WriteableBitmap 纯像素更新不触发重呈现 ★用户报"所有鼠标操作无效"的真凶
- **症状**：拖拽/滚轮时相机状态在变（stderr 日志 yaw/pitch/dist 持续变化）、GL 零错误，**但视口画面纹丝不动**——用户视角就是"点云所有鼠标操作都无效"
- **根因**：Render 只做 WriteableBitmap 的 Lock/Copy/Unlock 像素更新，**没有任何 UI 元素失效 → Avalonia 合成器不知道内容变了 → 不重新呈现**。之前"点大小 3→6 画面变化 7.7 倍"的验证是假阳性：那次 TextBox 文本同时变了，相邻 UI 失效带动整帧重组合，顺带把位图新内容画了出来；纯渲染更新（拖拽/滚轮）永远冻结
- **修复**：Lock/Copy 之后调用 `_image.InvalidateVisual()` 强制 Image 重绘（Cloud3DView + Image2DView 两处——Image2D 的缩放/平移同样潜在冻结）
- **诊断手段**：**截图 A/B 像素 diff**——拖拽前后差异只有光标（169 px）= 显示冻结；修复后 36649 px。教训：**"日志证明内部状态在变"≠"用户看到变化"，视觉行为必须做像素级 A/B 验证**
- **关联**：BUG-040/041 修复后日志确认相机在转，正是"状态变了但画面没变"让人误判成输入无效
- **关键词**：WriteableBitmap 不刷新, 画面冻结, InvalidateVisual, 合成器, 拖拽无效, 滚轮无效, 相机在转画面不动, 像素 diff, 假阳性验证

### BUG-043 | 2026-10-04 | GLSL 着色器里写中文注释 → NVIDIA 编译器 internal corruption ★藏在字符串里
- **症状**：给点云顶点着色器加了中文行注释后，GL 初始化失败："着色器链接失败: Vertex info: error C0000: syntax error, unexpected $end, expecting :: at token <EOF> / error C9000: internal corruption, aborting"，面板显示"无 GL 上下文"
- **根因**：GLSL 源码是 C# 里的 UTF-8 字符串，NVIDIA 的 GLSL 编译器对**多字节非 ASCII 字符**（中文注释）解析崩溃
- **修复**：着色器源码内注释一律用英文。教训：**GLSL 字符串内容视为纯 ASCII 域，中文说明写在 C# 侧注释里**
- **关键词**：GLSL, 中文注释, internal corruption, C0000, C9000, NVIDIA, 着色器编译失败, 无 GL 上下文

---

## BUG-044 | 2026-10-04 | 轨道相机 pitch 过极点卡死 → 终版方案：四元数自由翻滚 ★yaw/pitch 方案的先天缺陷
- **症状演进**：v1 pitch 钳 ±89°（拖到极点不能再动）→ v2 翻转公式配错（阈值 89 + 常量 179，映射在 89.5° 是恒等映射）→ yaw 疯涨到 88790°、极点震荡"卡死" → v3 修正配对（90+180）能过极点但**水平拖拽方向在过极点后反转**，用户仍不满意
- **根因**：yaw/pitch 欧拉角分解**天生有极点**——过极点必须靠翻转/钳制打补丁，每种补丁都有体验缺陷（卡死/自旋/方向反转）
- **终版方案：四元数自由翻滚（tumbling）**——抛弃 yaw/pitch，相机姿态存 `_orientation` 四元数：水平拖拽绕世界 Y 轴增量旋转，垂直拖拽绕**相机本地 right 轴**增量旋转，`_orientation = Normalize(qPitch * qYaw * _orientation)`；up 向量由四元数导出、恒垂直于视线 → **LookAt 永不退化、上下可无限翻滚、无任何钳制/翻转分支**。视线 forward 序列实测两次穿越极点全程平滑。平移/重置视角同步改用四元数基向量
- **教训**：需要"上下一直转"的查看器，直接上四元数翻滚——欧拉角轨道相机 + 极点补丁是治标不治本
- **关键词**：轨道相机, 极点, 卡死, 四元数, 翻滚, tumbling, yaw 疯涨, 欧拉角缺陷, Quaternion, LookAt 退化

---

## 轨道相机（设计备忘）
- 交互：左键拖 = 旋转（yaw -= dx×0.35，pitch += dy×0.35 过极点翻转），滚轮 = 缩放（×0.9），右键拖 = 平移（沿相机 right/up 基向量）；PointerCaptureLost 兜底清状态。
- 裁剪盒：近截面/远截面 = 世界坐标 min/max 两角（各 xyz 三参数），顶点着色器 `any(lessThan/greaterThan)` 盒外剔除；投影 near/far 固定 0.001/1e6（无深度附着不怕精度损失）。属性面板参数按功能分区（显示/点云/裁剪盒），参考 CloudCompare。
- 裁剪盒：近截面/远截面从单一距离改为 **世界坐标 min/max 两角（各 xyz 三参数）**，顶点着色器 `any(lessThan/greaterThan)` 盒外剔除；投影 near/far 固定 0.001/1e6（无深度附着不怕精度损失）。属性面板参数按功能分区（显示/点云/裁剪盒），参考 CloudCompare。

---

## 维护约定

1. **谁写**：每次调试修复后，由当次会话随手追加（ symptoms 抄原始报错关键行）。
2. **怎么查**：先按报错代码（CS/AVLN/MSB）或关键词搜本文档；再按"症状现象"搜。
3. **去重**：同类问题在原条目上补充新症状/新变体，不新开条目。
4. **星级**：★ 标记"症状与根因相距很远、极难定位"的坑。

---

### BUG-045 | 2026-10-04 | 点大小参数无效：Windows/NVIDIA 驱动要求显式启用 GL_PROGRAM_POINT_SIZE
- **症状**：着色器里 `gl_PointSize` 赋值正确、uniform 也传到了，但点云渲染出来永远是 1px，"点大小"参数怎么改都没反应
- **根因**：Core Profile 下 `gl_PointSize` **不是自动生效**的——Windows NVIDIA 驱动要求显式 `GL.Enable(EnableCap.ProgramPointSize)`，否则忽略着色器写入的点大小
- **修复**：InitGl 里加 `GL.Enable(EnableCap.ProgramPointSize)`。教训：**GL 状态机没有"默认合理"，每个能力都要显式开启**
- **关键词**：gl_PointSize, ProgramPointSize, 点大小, 无效, NVIDIA, Core Profile

---

### BUG-046 | 2026-10-04 | UniformMatrix4 的 transpose 方向：false 才对（OpenTK 行主序 → GL 列主序）★"平的点云"总根因
- **症状演进**：点云画出来"被拍扁到一个平面"（用户连报 5 次）；期间 NDC 诊断还给出过"矩阵正确"的假阳性
- **根因**：OpenTK `Matrix4` 内存是**行主序**，`GL.UniformMatrix4(loc, transpose, ref m)` 把这 16 个 float 按**列主序**读走——`transpose:false` 恰好完成行→列转换；传 `true` 会二次转置得到错误矩阵。NDC 诊断假阳性是因为手写 Mul 函数把 M·v 算成了 Mᵀ·v，诊断工具本身错了
- **修复**：`GL.UniformMatrix4(_uMvp, false, ref mvp)`。教训：**矩阵约定问题要用"渲染一个已知顶点+读回像素"来终验，手推数学容易把诊断工具本身写错**
- **关键词**：UniformMatrix4, transpose, 行主序, 列主序, OpenTK, 点云拍扁, 假阳性

---

### BUG-047 | 2026-10-04 | GL 回读自底向上 × 位图自顶向下 → 3D/2D 显示整体垂直翻转 ★两个插件同一个坑
- **症状**：3D 点云世界 Y 轴在屏幕上是倒的（高度伪彩红色/高点在屏幕下方、蓝色/低点在上方）；2D 图像同样被镜像（测试图案垂直对称所以一直没暴露）。附带后果：视角预设的上/下会颠倒、CPU 拾取投影坐标全错、坐标轴指示器方向反
- **根因**：`glReadPixels` 行序**自底向上**（第一行 = GL 帧缓冲最底行），`WriteableBitmap` 行序**自顶向下**（第一行 = 屏幕最顶行），直拷 `Marshal.Copy` = 垂直镜像。2D 的纹理上传路径恰好双翻转抵消了一半，更具迷惑性；2D 软件回退路径是自顶向下的正确映射，GL 路径与它不一致暴露了本意
- **定位方法**：像素分析——高度伪彩下统计暖色（高点）与冷色（低点）像素的平均屏幕 y，暖色在下即翻转。**不依赖肉眼，对称测试图案看不出来**
- **修复**：呈现时逐行倒序拷贝：`Marshal.Copy(_frame, (_vpH-1-y)*rowBytes, dst + y*fb.RowBytes, rowBytes)`；2D 仅 GL 分支倒序（软件分支本就正序）。同时翻转交互符号保持手感：平移 `+up*dy`（拖下→云下）、默认视角 pitch -15°（俯视）。CPU 投影按"GL 列主序解释 OpenTK 行主序"展开分量计算（见 Cloud3DView.Project）
- **关键词**：glReadPixels, 行序, 垂直翻转, WriteableBitmap, Marshal.Copy, 像素分析, 高度伪彩, 2D 镜像

---

### BUG-048 | 2026-10-04 | 自定义 jet 色带端点：t=1 输出纯黑，立方体顶面隐入背景
- **症状**：立方体点云高度伪彩下顶面一片"消失"（与 #161618 背景同色）
- **根因**：旧 jet 公式 `1.5 - abs(4t - k)` 在 t=1 时三通道全 clamp 到 0 → 黑色；t=0.5 是黄色而非绿色，端点/中点都不符合惯例
- **修复**：改 5 段线性插值 蓝→青→绿→黄→红，t=0 纯蓝、t=1 纯红，端点语义明确（也方便用暖/冷分布做翻转诊断）
- **关键词**：jet, 伪彩, 色带端点, 顶面黑色, 高度着色

---

### BUG-049 | 2026-10-04 | 选区高亮不渲染：归一化 ubyte 掩码写 1 = 1/255 ★状态对、画面不变
- **症状**：Ctrl+框选后选区信息正确（"20,114 点 · 质心…"），但画面纹丝不动、高亮色一个像素都没有
- **根因**：选区 VBO 用 `VertexAttribPointer(2, 1, UnsignedByte, normalized:true)`——归一化把 byte 值除以 255；`_sel[i] = 1` 进着色器是 **0.004**，永远过不了 `vSel > 0.5` 阈值。掩码语义（0/1）与归一化属性（0/255）错位
- **修复**：`_sel[i] = 255`（归一化后 = 1.0）。教训：**normalized 属性的"1"是 255 不是 1；"状态正确但渲染无效"先查属性编码**
- **连带改进**：① 选区高亮色从琥珀 (1.0,0.72,0.08) 改洋红 (1.0,0.15,1.0)——琥珀与 jet 色带橙段 (255,204,0) 几乎同色，高度伪彩下选区不可辨；② 框选 0 点显示"框选 0 点（范围内无点）"，与未框选的"无（Ctrl+左键拖拽框选）"区分（否则无法判断功能是否执行）
- **关键词**：normalized, UnsignedByte, 掩码, 选区高亮, 1/255, vSel, 洋红, 状态对画面不变

---

## M4 一期备忘（2026-10-04）
- **选择联动上线并验证**：3D 拾取 → `sel/point3d`（SelectionPayload.Point3D，含索引/xyz）；3D 框选 → `sel/cloud-box`（CloudBox + Bounds3D 六元组）；2D Ctrl+框选 → `sel/roi2d`（Rect2D 图像像素坐标）。总线监视器确认四主题（cloud/merged、image/stitched、sel/point3d、sel/cloud-box、sel/roi2d）全部在线并带时间戳。2D 自有 ROI 青色常显（随缩放平移联动）、外部 ROI（Source≠自身）洋红虚线回显、Esc 清除。
- **2D 呈现行序修复的视觉确认**（BUG-047 收尾）：渐变图案（r=x 递增、g=y 递增）显示上暗下亮（86→197）、左暗右亮（133→152），方向与源图一致，2D 镜像确认已修。
- **测试工具坑（重要）**：SendInput 组合键拖拽必须**先移动到起点再按下**——`output/input.ps1` 的 ctrldrag 曾先按下再 Move-Steps，导致按下落在上一次操作的鼠标位置（曾落在活动栏上静默失败）；3D 框选当时通过纯属巧合（前一步光标恰在视口内）。任何"press→move→release"注入一律 move-first。

---

## M3.2 二期备忘（2026-10-04）
- **亿级分块上传 + 交互抽稀上线并实测（24M 点）**：① `GlCloudRenderer` 拆 `BeginUpload(count)`（预分配显存，_count=0）+ `UploadChunk(points, colors, start, n)`（BufferSubData 逐块填充、_count 前进即渐进绘制）；② `Cloud3DView.SetCloud` **不再克隆载荷数组**（总线载荷约定不可变，亿级省一半内存），边界用**步长 64 采样估算**（24M 约 37 万次迭代、瞬时完成，视图立即正确取景，jet 色域即刻可用），`DispatcherTimer` 30ms/拍搬 200 万点（24M≈12 拍约 1.2s，UI 无长阻塞）；③ **交互抽稀**：点数 > 500 万（定案阈值）时相机交互（旋转/平移/滚轮/预设）自动 stride=⌈N/500 万⌉，静止 0.2s（DispatcherTimer）补画全量；有效抽稀 = max(用户抽稀间隔, 交互自动)；信息栏显示"抽稀×N（交互）/ 上传中 x%"。**实测（24,024,006 点）**：渐进上传端到端完成态正确（小云 58,806 单块 + 24M 12 块都正常渲染）；拖拽中信息栏出现"抽稀×5（交互）"（像素对比 1510px 差异）、静止后恢复基线（0 差异）、视图平滑旋转（22 万像素变化）。**未捕获中间态**：24M 上传约 1.2s 完成，"上传中 x%"瞬态未截到（端到端完成态已验证）；**待做**：真 100M 数据实测、屏显密度封顶（每像素 1 点）、后台线程上传（需 GL 上下文跨线程调度，当前 UI 线程时间切片）、按块增量更新/剔除（等设备树多实体重构）。

---

## M4-2 联动演示备忘（2026-10-04）
- **"点云选点 ↔ 图像高亮"完整落地并亚像素验证**：① Sdk SelectionPayload 增 `Uv` 字段（约定 u=(x-minX)/sizeX、v=(z-minZ)/sizeZ）；② 新算子「关联高度图源」(source.heightmap，联动演示组)：同一高度场（双高斯峰+谷+细纹理）同源生成点云（x,z 平面+y=高度，256×256=65,536 点）与 jet 伪彩图像，点 (i,j) ↔ 像素 (i,j) 构造上一一对应；示例图改为 高度图→pub.image+pub.cloud 双发布；③ 3D 拾取发布 sel/point3d 时携带 Uv；④ 2D 订阅 sel/point3d，按 uv 画洋红十字标（随缩放平移重投影）。**验证**：拾取 #36456 (-0.276, 0.056, 0.171) → 预测 uv (0.408, 0.557) → 2D 十字标实测 uv (0.406, 0.555)，偏差 0.4/0.6px（亚像素）；预测屏幕位置 (949,502) vs 实测 (949,501)。
- **★采样边界低估坑**：SetCloud 的步长 64 采样边界用于 UV 映射会出错——点云按行序存储时（j 外 i 内），采样点只命中每行 4 个 i 值，X 范围被低估（实测 X[-1.5, 0.76] 而真实 [-1.5, 1.5]）→ uv 横向全偏。修复：分块上传时逐点合并精确边界（每块 200 万次迭代 ≈ 25ms 摊在 30ms 拍内），采样边界只用于即时取景；上传完成后 cloudInfo 显示精确边界。**教训：采样估算只能用于取景/色域这类容差场景，坐标映射必须精确遍历（可分摊到分块流水线里）**。
- **Avalonia 多输出算子**：NodeDef.Outputs 数组多项 + Execute 返回字典多项即可（"关联高度图源"同时输出点云+图像），示例图一线两分支。

---

## M5 设备树一期备忘（2026-10-04）
- **多实体点云管理上线并验证**：① CloudPayload 增 `Id`（空落"点云"；同 Id 重发=更新）；② GlCloudRenderer 多实体化——按 Id 的 Entity{VAO+三VBO+Count/Cap/Visible/Bounds}，BeginUpload(id,…)/UploadChunk(id,…)/SetSelection(id,…)/SetVisible/RemoveEntity/GetEntities；Draw 共享 uniforms 一次、逐实体 uMinY/uMaxY（各自高度色域）+DrawArrays，ReadPixels 一次；③ Cloud3DView 重构为 `Dictionary<string,CloudData>`（拾取/框选跨实体、按实体选区掩码与 sel/point3d Uv、发布带 [实体Id] 前缀；框选聚合"共 N 点（高度图 45,352 · 立方体 156,499）"并逐实体发 sel/cloud-box）；④ 新面板「点云树」（Tool 侧边栏 🌲：显隐复选框/删除按钮/点数/详情，订阅 CloudsChanged）；插件双面板（3D 视图 Document + 点云树 Tool，GetView 惰性共享实例）。示例图双分支（高度图→pub.image+pub.cloud，立方体→第二个 pub.cloud 同主题）。
- **实测（高度图 65,536 + 立方体 61,206→1,506,006）**：双云渲染 ✓（22 万彩色像素）、树列两实体+计数 ✓、显隐切换 ✓（7.4k↔30.1万）、删除 ✓、同 Id 更新+新 Id 新增 ✓（重跑后两实体恢复、高度图重发无重复）、跨实体拾取 ✓（"[立方体] #668665 (1.640,1.620,5.000)" z=5 恰为边长 10 前面）、跨实体框选聚合 ✓。
- **★多实体上传目标错乱坑**：单实体时代用"最近 BeginUpload 的实体"作上传目标是隐式状态——两个云交错上传时（同拍双发布），后实体的 BeginUpload 覆盖目标，先实体的分块数据全部写进后实体的 VBO（容量守卫还截断了它），先实体 Count 恒 0 永不显示、后实体内容混杂。修复：**UploadChunk 显式按 Id 定位实体，删除 _uploadTarget 隐式状态**。教训：多实体化时所有"当前目标"类隐式状态都要显式化为参数。
- **小修**：cloudInfo 的"（上传中）"在完成时不刷新 → UploadTick 完成分支补 UpdateCloudInfoText。窗口被用户最小化/关闭会中断测试（无崩溃记录，WER LocalDumps 已配置 HKCU 指向 output/dumps，下次原生崩溃有转储可查）。

---

## M5 二期备忘（2026-10-04）
- **每实体独立样式上线并验证**：① 渲染器 Entity 增 ColorMode/PointSize，Draw 的 uMode/uPointSize 移入逐实体循环（uMinY/uMaxY 本就逐实体——每朵云独立高度色域）；② 视图 CloudData 增样式字段（新实体继承属性面板当前全局值），SetCloudStyle(id,…) 公共 API + ApplyStyleToAll（全局控件=批量应用+新云默认值）；③ 点云树详情区构建选中实体的 颜色模式/点大小 控件（Enter/失焦提交，_syncing 防重建回声）。**实测**：选中立方体改"单色"→ jet 饱和像素 22 万→5.5 万（仅高度图保持伪彩）、蓝灰像素 16.5 万（立方体）——只影响该实体 ✓。
- **cloud/removed 总线语义**：CloudPayload.TopicRemoved="cloud/removed"（只需 Id）；查看器订阅→RemoveCloud；配套算子「移除点云」(pub.remove，联动演示组，参数=实体Id)。总线链路 5 行调用已验证的 RemoveCloud，E2E 拖放演示因用户占用屏幕未完成（算子已入库可手动拖入验证）。
- **★树行命中测试坑**：行名 TextBlock 挂 PointerPressed 收不到点击——Avalonia 里 **Background=null 的控件不参与命中测试**，须给容器 Border 设透明 Brush（`Colors.Transparent`）并把选中逻辑挂 Border（整行可点）。
- **工具链坑（重要）**：① Bash 工具的 heredoc（即使 <<'EOF' 引号形式）会把 `\` 折叠成 `\`——python 补丁里匹配 C# 源码的字面 `\n` 必须用 `chr(92)+'n'` 构造，直接写 `\n` 会变成真换行导致模式失配；② SendInput 中文输入用 KEYEVENTF_UNICODE（wScan=字符码，dwFlags=0x0004，up 加 0x0002）——input.ps1 的 type 模式；③ 测试期间用户会 F5 重载/收起侧边栏/切标签/开全屏应用——**每步操作前必须重查窗口 bounds 与标签位置**（本节验证因此重试多轮）。

---

### BUG-050 | 2026-10-05 | 节点卡片参数标签中文字被裁：Avalonia 中文字度量宽度偏小
- **症状**：节点卡片参数行（如"主题"）标签的末字右侧被裁掉（用户报"主题的字被遮挡"，截图显示文本框几乎贴住"题"字）
- **根因**：参数行 Grid 列 "Auto,*"，标签 TextBlock 的**中文字符度量宽度偏小**——"主题"实测/布局给 19px，渲染实际需 ~22px（每汉字 ≈ 字号 11px），末字右侧 ~3px 画出布局外被裁；文本框紧贴其右加剧观感
- **修复**：标签 TextBlock 加 `MinWidth = 44`（容纳 4 汉字，覆盖全部现有标签：主题/分辨率/起伏幅度/实体Id…）+ 右距 6px；副产品：所有参数行的编辑框左缘对齐，观感更整齐
- **教训**：**Avalonia 里 CJK 文本的度量宽度可能小于渲染宽度（字体回退度量差异），涉及 Auto 布局的中文字段要预留余量（MinWidth），否则末字被裁**
- **关键词**：中文度量, TextBlock 裁字, Auto 列, MinWidth, 节点卡片, 参数标签, 主题

---

### BUG-051 | 2026-10-05 | 端口圆点变半圆 + 参数文本框下缘裁字（用户截图报）
- **症状**：① 端口圆点（如"点云"输出）显示为半圆；② 参数文本框（如"cloud/merged"）文字下缘被裁
- **根因**：① 端口圆点用负边距外伸卡片边缘各 6px（设计=嵌在边上），任何祖先裁剪/缩放渲染都会裁掉外半；② 参数 TextBox MinHeight 24 无显式 Padding，主题默认内边距下内容高度 ≈13px < 11.5px 字号的行高 15px，下缘（g 的降部）被裁
- **修复**：① **端口圆点完整移入卡片内**（margin 归零，中心在卡内 15px 处），连线端点公式同步 `n.X+15` / `n.X+CardW-15`（顺带修正了原先公式与圆点中心 9-15px 的系统偏差）；② TextBox 显式 `Padding(6,1,6,1)`，内容高度 17px ≥ 行高
- **教训**：**贴边元素（负边距外伸）依赖"无人裁剪"的脆弱假设，能内移就内移**；小尺寸 TextBox 显式给 Padding，别依赖主题默认值
- **关键词**：端口圆点, 半圆, 负边距, 裁剪, TextBox Padding, 下缘裁字, 连线端点

---

### BUG-052 | 2026-10-05 | 滑杆圆钮下半被下一行滑杆盖住（用户截图+用户自诊）
- **症状**：节点卡片滑杆的圆形圆钮下半被下一个滑杆行盖住，看起来是半圆
- **根因**：参数行 Grid 高 30，滑杆控件 24 高，主题模板的圆钮（~12-14px）在滑杆内的垂直位置偏下，圆钮下缘越出行边界；StackPanel 后画的行（下一行滑杆控件的不透明模板背景）盖住越界部分
- **修复（按用户方案）**：① 滑杆 `Height = 18`（圆钮更小，完全含在控件内）；② 滑杆 `Background = Brushes.Transparent`（行透明化：圆钮即使探入下一行也完整显示）；**不加大行高**（用户明确拒绝 ParamH 34 方案）
- **验证**：像素成像——分辨率钮 y404-413、起伏幅度钮 y434-443，均完整圆形、下方无遮挡痕迹 ✓
- **教训**：**模板控件在紧凑容器里的溢出会被后续兄弟的不透明背景盖住；修法=控件限高+背景透明，而不是加大容器**
- **关键词**：滑杆, 圆钮, 半圆, 被盖住, Background 透明, Height 限高, 节点卡片

---

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

### BUG-053（未解）| 2026-10-05 | 点云树详情分段不渲染（列表下方全空）
- **症状**：树列表正常（行可见），列表下方 y230-810 全空——"点云属性"分段（标题+颜色模式/RGB/点大小/变换矩阵）完全不渲染。上一实例（手动单击选中后）矩阵区 y911-1010 曾正常显示。
- **已排查**：详情区包 Border(0x252528)+标题；Refresh 自动选中第一个（_deselected/Esc）；Grid "Auto,*,Auto" 行2=Auto。
- **怀疑**：① 自动选中未触发（构造时序/CloudsChanged 时序）；② 行2 内容测高 0（_detailHost 空？）；③ 需对比"手动选中后正常"与"自动选中不渲染"的差异——BuildStyleControls 是否执行。
- **关键词**：详情分段, 不渲染, 自动选中, 测高, Grid Auto

---

### BUG-053 修复 | 2026-10-05 | 点云树详情分段不渲染
- **根因**：① "详情分段+自动选中"补丁因 heredoc 截断实际未写入（脚本断言失败未保存，文件仍是旧版）；② RefreshDetail 先 Clear 再"同实体跳过重建"——上传完成的 CloudsChanged 一触发样式控件即消失。
- **修复**：重写 CloudTreePanel——① "点云属性"分段（Border 底色 0x252528+标题）常驻；② Refresh 自动选中第一个（_deselected 标志，Esc 置位/单击重选复位）；③ RefreshDetail 只在选中实体变化时重建控件（同实体仅刷新文本）；④ 紧凑间距。
- **验证**：像素+a11y 双确认——分段内容 y740-1000（标题/颜色模式/点大小/矩阵 4 行/应用变换按钮/信息文本），自动选中"高度图"生效。
- **教训**：python heredoc 长补丁必须验证写入（断言失败=未保存）；改完要核对文件实际状态。

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
