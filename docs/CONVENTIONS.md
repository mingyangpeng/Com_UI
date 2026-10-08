# UI 交互约定与复用清单

> 面向开发者：新增任何文件/实体类界面时，**先来这里找组件和规则**，不要重新实现、也不要重新向用户提问。
> 背景：加锁、双击改名、行对齐等能力曾被重复实现 2~3 遍（BUG-059 Esc 误提交即重复实现引入），本页将它们收敛为共享组件 + 统一规则。

## 组件清单（ComUI.Sdk.Ui —— 宿主与插件共用）

Sdk 是宿主与所有插件共享的唯一公共程序集（默认 ALC 加载，类型身份一致），且已引用 Avalonia，因此交互原语放这里。

| 组件 | 用途 | 取代了哪些重复实现 |
|---|---|---|
| `InlineRename.Begin(text, commit, cancel)` | 行内改名框：统一样式、焦点全选、Enter/失焦=确认、Esc=取消（done 守卫防 LostFocus 竞态） | 侧边栏行 / 节点卡片 / 点云树三处各自的 TextBox 实现 |
| `DoubleClickDetector`（每目标一实例） | 450ms 时间戳双击判定（UIA/自动化注入的 Click 不带系统双击标志，必须时间戳法） | 侧边栏行 / 节点头部两处时间戳逻辑 |
| `InlineRename.DoubleClickMs = 450`、`SingleClickDelayMs = 300` | 双击阈值与单击延迟常量 | 散落的魔法数字 |
| `LockGlyph.Build(locked)` | 形状绘制的锁徽标（emoji 是彩色字体无法着色）：解锁=绿 #7BC886 开锁、锁定=灰 #A0A0A8 闭锁 | AlgoSidebar 内的绘制函数 |
| `FileLock.IsLocked / Set / Toggle` | 文件锁定持久化：JSON 顶层 `"Locked": true/false` 字段（缺省=未锁定） | AlgoSidebar 与 NodeGraphPlugin 两处解析 |

## 交互规则（新界面默认遵循）

### 命名（一切文件/实体名）
- **双击进入行内改名**（文本全选）；**Enter / 失焦 = 确认，Esc = 取消**；空名或与原名相同 = 还原。
- 改名框必须用 `InlineRename.Begin`，不得手写 TextBox（done 守卫是踩过坑的：控件移出可视树后 LostFocus 仍会触发一次，没有守卫 Esc 会变成误提交）。
- 双击检测用 `DoubleClickDetector`，**每个可双击目标一个实例**（每行/每卡片），避免跨目标误判。

### 行布局（侧边栏文件列表类）
- **行等宽拉伸**、名字左对齐（过长省略号）、动作元素（锁/按钮）**统一钉在行右端对齐一列**。
- 布局链坑：ScrollViewer→StackPanel→Button 整条链不会横向拉伸子项（`HorizontalScrollBarVisibility=Disabled`、`HorizontalContentAlignment=Stretch` 均无效）。**确定性做法：行 Width 显式跟随 ScrollViewer.Viewport 宽度**（LayoutUpdated 同步，减容器横向 Margin）——参考 AlgoSidebar。
- **单击 = 选中**（高亮反馈，作为 Ctrl+C 的对象）**+ 主动作**；**双击 = 改名**。
- 单击的主动作若会异步抢焦点（如打开标签页），必须**延迟 `SingleClickDelayMs`(300ms) 派发**，第二击到达即取消——否则双击触发的改名框会被焦点变化瞬间提交掉。

### 锁定（文件级保护）
- 锁徽标点击切换；**绿=解锁（可编辑），灰=锁定（只读打开：禁改内容）**。
- 锁定持久化走 `FileLock`（JSON `Locked` 字段），不要另发明存储。
- 锁定页的"保存" = 复制副本（解锁、命名「原名 副本」）；解锁或对副本编辑后可正常保存。
- 锁状态变化发布总线事件（约定 `algo/locked`，载荷=路径），已打开的视图实时同步只读状态。
- 嵌套按钮注意：Avalonia `Button.Click` 是**冒泡路由事件**——行内嵌套按钮（锁）的 Click 会传到行按钮，处理完必须 `e.Handled = true`。

### 文件基础操作（文件类界面标配）
- **Ctrl+C** 复制选中文件引用；**Ctrl+V** 粘贴为「原名 副本」（解锁可编辑，重名自动递增 -副本N）。
- **Ctrl+S** 直接写回原文件（页面有 LoadedPath 时）；新文件弹另存对话框；锁定页 = 复制副本。
- 复制/粘贴等操作要有日志反馈（宿主注入 LogMessage 回调）。

### 分屏 / 浮动 / 会话（标签区交互）

- **拆分**：标签右键菜单（向左/右/上/下拆分、移到下一格）或 **Ctrl+Alt+方向**（键盘等价，自动化测试通道）；同方向拆分插入兄弟、异方向包裹新容器。
- **拖拽**：按下标签头移动 5px 进入；悬停格**中心**=移入该组、**边缘 28%**=朝该方向拆分（预览高亮指示落点）；拖出主窗边界松手=浮动；Esc 取消。
- **浮动**：Ctrl+Alt+F 或右键「浮动窗口」；浮动窗 `Show(owner)`（Owner 属性是 protected，勿直接赋值）；「⇲ 固定到主窗」拆出内层视图回标签区（外层包装丢弃）；浮动面板不占分屏格、不进会话。
- **同面板两格禁止**：分屏树不变式（同面板只在单组）；对比场景声明 AllowMultipleInstances。
- **会话**：退出自动保存（窗口几何/分屏树/侧边栏/带恢复键的动态页）；插件动态页要进会话必须 OpenDocument 传恢复键 + Initialize 时 RegisterDocumentFactory；几何保存跳过最小化态（-32000 防御）。
- **树操作**（Core.SplitTree）：结构变更用 SplitLayout 实例方法（持根引用自动更新）；**替换子节点必须回填 Parent 指针**（漏了回收链静默失效）。

### 自动化测试兼容（给开发和测试脚本）
- UIA/CAA 的元素点击触发的是 Click 事件，**不触发 PointerPressed**——测 PointerPressed 逻辑（如节点头部）必须 SendInput。
- CUA/UIA 交互后首个 SendInput 可能被吞：用 tools/fgclick.ps1 / fgdblclick.ps1（ALT 解前台锁 + 前台校验），必要时重试一次。
- AX 元素树约 400 上限，超出按优先级裁剪（活动栏/侧边栏最先消失）——关多余标签页后再查。

## 落点现状（谁在用）

| 界面 | 改名 | 双击 | 锁 | 复制/粘贴 | 保存 |
|---|---|---|---|---|---|
| ⚡算法侧边栏行（Host） | ✓ InlineRename | ✓ Detector | ✓ LockGlyph+FileLock | ✓ Ctrl+C/V | 页面内 Ctrl+S；**Del 删除（仅解锁）** |
| 流水线节点卡片（NodeGraph） | ✓ InlineRename | ✓ Detector | —（算子无锁） | — | — |
| 点云树行（Cloud3D） | ✓ InlineRename | —（选中即编辑名） | —（实体无锁） | ✓ Ctrl+C/V/Del | — |

标签区（分屏/拖拽/浮动/会话）由宿主 MainWindow 统一提供，插件面板无需任何额外代码即获得这些能力。

新增文件/实体类界面（例如未来的结果文件树、导出列表）：按上表取用组件，缺的能力先扩展 Sdk.Ui 再用，不要在界面里重写。
