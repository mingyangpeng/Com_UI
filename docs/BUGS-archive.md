# 开发 Bug 记录（归档：2026-10-01 ~ 2026-10-05）

> 本文件承接自 docs/BUGS.md（2026-11-22 拆分归档），收录 2026-10-01 ~ 2026-10-05 期间的开发条目区：
> BUG-001 ~ BUG-053（含 BUG-053 未解 / 修复记录），以及同期备忘（轨道相机设计备忘、M4 一期 / M3.2 二期 / M4-2 / M5 设备树一期 / M5 二期备忘）。
> 「维护约定」与 BUG-054 起的条目、叙事段仍保留在 [BUGS.md](BUGS.md)。

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
