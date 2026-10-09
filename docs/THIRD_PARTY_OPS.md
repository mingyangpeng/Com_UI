# 第三方算子库插件开发规则（Node JSON）

面向**第三方算法厂商**：把自有算法 DLL 接入 ComUI 的「算子库」，成为算法流水线里可拖拽、可连线的节点。
通用插件开发说明见 [PLUGIN_DEV.md](PLUGIN_DEV.md)；本文只讲算子库交付物（DLL + `*.node.json`）的完整规则，两篇互链、互补。

> **本文所有行为均以宿主实现为准**（文档与实现逐条核对过）：
> - `src/Plugins/ComUI.Plugin.NodeGraph/Operators/NodeJsonRegistry.cs` —— JSON 声明解析、反射绑定、输出取值
> - `src/Plugins/ComUI.Plugin.NodeGraph/Core/GraphExecutor.cs` —— 拓扑排序与逐节点执行
> - `src/Plugins/ComUI.Plugin.NodeGraph/Operators/NodeDefs.cs` —— 算子表注册、内置节点（含发布节点）
> - `src/ComUI.Sdk/IBus.cs` —— 载荷类型与总线主题契约

---

## 1. 交付物与目录

```
comdll/algo/<你的名称>/
├── YourAlgo.dll            # 算法程序集（net8.0，引用 ComUI.Sdk 或零依赖）
├── your.node.json          # ★ 节点算子声明（进算子库，必需）
├── your.algo.json          # 算法命令声明（进「⚡算法配方」侧边栏，可选）
└── <依赖 DLL>              # 所有依赖必须自包含在本目录（宿主不做全局解析）
```

- **三件套同目录**：`*.node.json` 与它引用的 DLL 放在一起；`assembly` 是**相对本 JSON 所在目录**的路径。
- **`algo.json` 可选**：同一 DLL 可同时配 `node.json`（流水线节点）与 `algo.json`（侧边栏命令），两者互不冲突、互不影响 —— `algo.json` 规则见 [PLUGIN_DEV.md §4](PLUGIN_DEV.md#4-算法插件json-声明推荐)。
- **依赖自包含**：非宿主的第三方 DLL、原生库一律平铺在本目录。**`ComUI.Sdk.dll` 不要拷进来** —— 它由宿主默认上下文提供，拷入会导致类型身份分裂（加载失败或转换异常）。
- **自动注册**：宿主启动与 **F5 重载**时递归扫描 `comdll/algo/**/*.node.json`，**无需写任何注册代码**。
  - 成功日志：`算子(JSON)已注册: <名称> <<id>> ← <文件名>`
  - 失败日志：`算子 JSON 注册失败 [<id>] (<目录>): <原因>` —— 原因会点名（缺字段 / 找不到程序集 / 找不到类型 / 找不到方法）。
- **Id 冲突 = 静默忽略**：`NodeDefs.Register` 遇到已存在的 Id 直接返回 `false`（Id 比较忽略大小写），**不会覆盖内置算子**，也不会报错 —— 所以 Id 必须自带厂商前缀（见 §8.3）。
- **参考实现**：`comdll/algo/DemoStitching/`（DLL + `demo.node.json` + `demo.algo.json`），源码在 `src/Plugins/DemoAlgo.Core/`。
- ⚠️ **热更新注意**：算子 DLL 由宿主用 `Assembly.LoadFrom` 载入**默认上下文**，运行期文件被占用（覆盖会失败），且**不随 F5 卸载** —— 改 DLL 请先退出宿主再覆盖，然后重启（仅改 `node.json` 可 F5 生效）。

---

## 2. `*.node.json` 字段规范

文件是 **JSONC**（支持 `//` 注释与尾逗号），属性名大小写不敏感。完整模板：

```jsonc
{
  "node": {
    "id": "vendor.gamma",           // ★ 全局唯一；约定 <厂商/来源>.<名称>，避免与内置冲突
    "name": "伽马校正",              // ★ 算子库与节点头显示名
    "group": "2D 图像",              //   算子库分组栏目（新栏目自动出现在列表尾部，栏目头可拖拽排序）
    "category": "处理",              //   节点头配色：源(绿)/处理(蓝)/输出(橙)，缺省 "处理"
    "description": "Gamma 校正：亮部/暗部非线性调整"   // 悬停提示
  },

  "runtime": "dotnet",               // ★ 当前仅支持 dotnet（native 请包一层 C# 壳，导出规则见 §5）
  "assembly": "YourAlgo.dll",        // ★ 相对本 JSON 所在目录
  "type": "YourNs.Filters",          // ★ 类型全名（也可写短名，按 FullName/Name 回退匹配）
  "method": "Gamma",                 // ★ 入口方法名（public/nonpublic、静态/实例均可）

  "inputs": [                        // 输入端口：name 显示在节点上；bind = 方法参数名（缺省 = name）
    { "name": "输入", "bind": "input", "type": "image" }
  ],

  "params": [                        // 用户参数：在节点卡片上可编辑
    { "name": "gamma", "label": "Gamma", "type": "number", "default": 1.5, "min": 0.2, "max": 3 },
    { "name": "mode",   "label": "模式", "type": "enum", "default": "ICP", "options": ["ICP", "NDT"] },
    { "name": "outDir", "label": "输出目录", "type": "path", "default": "" }
  ],

  "outputs": [                       // 输出端口：单输出可省略 from（直接用返回值）；
    { "name": "输出", "type": "image" }    // 多输出用 from 指定返回值属性路径（如 "Image"、"Result.Image"）
  ]
}
```

### 2.1 字段逐项说明

| 字段 | 必填 | 说明 |
|---|---|---|
| `node.id` | ★ | 全局唯一键。重复即**静默忽略**；冲突判断忽略大小写。建议 `acme.gamma` 这种 `<厂商>.<名称>` 形式 |
| `node.name` | ★ | 算子库条目名 / 节点头显示名。缺 `id` 或 `name` → 报「缺少 node.id / node.name」 |
| `node.group` | | 算子库栏目名，缺省 `通用`。新栏目自动出现在列表尾部，顺序存 `config/op_order.json` |
| `node.category` | | 仅决定节点头配色（`源`/`处理`/`输出`），缺省 `处理`。写其它值按字面显示 |
| `node.description` | | 悬停提示文本 |
| `runtime` | ★ | `dotnet`（C# 反射）或 `native`（C++ DLL，统一 C ABI 见 §5.5，宿主自动 marshal） |
| `assembly` | ★ | DLL 文件名，相对本 JSON 目录；找不到 → 报「找不到程序集 X」 |
| `type` | ★ | 类型全名。先按全名解析，失败再按 `FullName` 或 `Name` 回退匹配；找不到 → 报「找不到类型 X」 |
| `method` | ★ | 方法名。`Public|NonPublic|Static|Instance` 全涵盖；找不到 → 报「找不到方法 X.Y」。<br>⚠️ **不要写同名重载**：多匹配会抛 `AmbiguousMatchException`，整个算子注册失败 |
| `inputs[].name` | | 节点卡上显示的端口名，也是连线与取值用的键 |
| `inputs[].bind` | | 对应的**方法参数名**（缺省 = `name`）。与参数名比较**忽略大小写** |
| `inputs[].type` | | `image` 或 `cloud`。**非 `cloud` 一律按 `image` 处理** —— 连线按类型匹配 |
| `params[].name` | ★ | 参数键，与方法参数名匹配（忽略大小写） |
| `params[].label` | | 卡片显示名，缺省用 `name` |
| `params[].type` | | 见下表 |
| `params[].default` | | 缺省值，按 `type` 转换（number→double、int→int、bool→bool、其余→string） |
| `params[].min` / `max` | | 数值滑杆范围（可选） |
| `params[].options` | | `enum` 必配，字符串数组 |
| `outputs[].name` | | 输出端口名（下游连线用） |
| `outputs[].from` | | 返回值上的**属性路径**（点分，如 `"Image"`、`"Result.Image"`）；缺省 = 端口名。<br>**单输出且未写 `from` → 直接用返回值** |
| `outputs[].type` | | `image` / `cloud`，规则同输入 |

### 2.2 参数类型

| `type` 写法 | 映射 | 卡片控件 |
|---|---|---|
| `number` | 双精度浮点 | 数值框/滑杆（带 min/max） |
| `int` | 32 位整数 | 数值框/滑杆 |
| `text` / `string` | 字符串 | 文本框 |
| `bool` / `boolean` | 布尔 | 勾选框 |
| `enum` | 枚举字符串 | 下拉（取 `options`） |
| `path` | 路径字符串 | 文本框 + 文件浏览 |
| 其它 / 未识别 | **降级为文本** | 文本框 |

---

## 3. 方法签名绑定规则（最核心）

执行器**按参数名绑定**（忽略大小写），对方法的每个参数依次尝试四级来源，**先命中先用**：

| 优先级 | 来源 | 注入内容 |
|---|---|---|
| 1 | **输入端口** | `inputs` 的 `bind`（缺省 `name`）匹配 → 上游端口的产出对象，**原引用传递，零拷贝** |
| 2 | **框架注入** | `CancellationToken` → 注入 `CancellationToken.None`（预留位，见 §5.5）<br>`IProgress<string>` → 注入一个把消息转发到日志面板的 `Progress<string>` |
| 3 | **用户参数** | `params` 的 `name` 匹配 → 节点卡片当前值，自动转 `int/long/float/double/bool/enum`，其余走 `Convert.ChangeType` |
| 4 | **默认值** | 方法有默认值 → 用默认值 |

- 四级都没命中 → 抛「无法绑定方法参数「X」」，节点红点。
- 输入端口未连线 / 上游无产出 → 抛「输入「X」未连线或上游无产出」。
- 值转换失败 → 抛「参数「X」值无法转换为 T」。
- **输入参数请直接声明为 `ImagePayload` / `CloudPayload`**（声明成 `byte[]` 之类会转换失败）。

### 3.1 静态与实例方法

- 静态方法：直接调用；
- 实例方法：每次执行 `Activator.CreateInstance(type)` —— **类型必须有公开无参构造函数**，且是**每次执行新建一个实例**（不要依赖实例字段保存跨次状态）。

### 3.2 异步方法

返回 `Task` / `Task<T>` 时，执行器 `GetAwaiter().GetResult()` 同步等待并取 `Result`。
⚠️ 非泛型 `Task` 取不到结果（产出为 `null`）—— 要产出就返回 `Task<T>`。

### 3.3 返回值与输出端口

- **单输出 + 未写 `from`** → 返回值直接作为该端口的产出；
- **多输出**（或写了 `from`）→ 按 `from` 的点分路径从返回值上取 **public 实例属性**（如 `"Image"`、`"Result.Image"`）；路径上任一属性缺失 → 抛「结果对象缺少属性 X」。
- 没有输出端口也能注册（纯副作用节点，如写文件）。

### 3.4 参考签名（`DemoAlgo.Core`）

```csharp
// 单输出：返回值直接作为输出端口；参数名 input/gamma 分别对应 inputs.bind 与 params.name
public static ImagePayload Gamma(ImagePayload input, double gamma, CancellationToken ct);

// 多输出：返回对象的属性按 from 取
public static StitchResult Stitch(int points, double voxel, string mode,
                                  IProgress<string>? progress, CancellationToken ct);

public sealed class StitchResult
{
    public ImagePayload Image { get; init; }   // outputs: { "name":"图像", "from":"Image", "type":"image" }
    public CloudPayload Cloud { get; init; }   // outputs: { "name":"点云", "from":"Cloud", "type":"cloud" }
}
```

---

## 4. 数据契约（图像 = OpenCV 模式，点云 = PCL 模式）

节点间流动的载荷类型（`src/ComUI.Sdk/IBus.cs`，契约已在 Sdk 权威声明），引用 `ComUI.Sdk` 即可与内置算子互通。
**第三方算法的数据进出一律按 OpenCV / PCL 的模式对接，零转换歧义。**

### 4.1 图像：`ImagePayload` ≡ `cv::Mat(CV_8UC4)`

| ImagePayload | OpenCV 对应 |
|---|---|
| `Width` / `Height` | `cols` / `rows` |
| `PixelsBgra` | `data`（**BGRA** 四通道交错，行主序），长度 = `Width × Height × 4` |
| `Id` | 数据树显示名（null = 未命名） |

- **紧排列**：step = Width×4，**无行对齐填充**——`mat.isContinuous()` 时 `data` 指针整块 `memcpy` 即可；
  ROI / 非连续 Mat 逐行拷贝或先 `clone()`；
- 3 通道 BGR 结果先 `cvtColor(BGR2BGRA)`；C#（OpenCvSharp）侧 `mat.GetArray(out byte[])` 后直接填。

```cpp
// C++ 桥接示例：把处理结果交给宿主（桥接层包成 ImagePayload）
cv::Mat bgra;
cv::cvtColor(result, bgra, cv::COLOR_BGR2BGRA);
CV_Assert(bgra.isContinuous());          // 非连续先 bgra = bgra.clone();
// → 紧排 BGRA 字节 + cols + rows
```

### 4.2 点云：`CloudPayload` ≡ PCL `PointCloud<PointXYZRGB>` 的 SoA 展开

| CloudPayload | PCL 对应 |
|---|---|
| `Points`（x,y,z 连续 float，`Count × 3`） | `points[i].x / .y / .z` |
| `ColorsRgb`（r,g,b 逐点三字节，可选） | `points[i].rgb` 拆包（uint32：r<<16\|g<<8\|b） |
| `Count` | `points.size()` |

```cpp
// C++ 桥接示例：PCL → 载荷（SoA 展开）
const auto& pts = cloud.points;
payload.points.resize(pts.size() * 3);
payload.colors.resize(pts.size() * 3);
for (size_t i = 0; i < pts.size(); i++) {
    payload.points[i * 3 + 0] = pts[i].x;
    payload.points[i * 3 + 1] = pts[i].y;
    payload.points[i * 3 + 2] = pts[i].z;
    uint32_t c = pts[i].rgb;
    payload.colors[i * 3 + 0] = (c >> 16) & 0xFF;   // r
    payload.colors[i * 3 + 1] = (c >> 8)  & 0xFF;   // g
    payload.colors[i * 3 + 2] =  c        & 0xFF;   // b
}
```

无 `ColorsRgb` 时查看器按高度伪彩上色。

**实体语义**：`CloudPayload.Id` 是点云实体键 —— 同 Id 重发 = **更新**该实体，空 Id 落「点云」。

**红线：数组不可变。** 载荷一旦产出/发布，就被下游**零拷贝引用共享**（执行器传的是原引用）。因此：
- **禁止原地修改**上游传进来的 `PixelsBgra` / `Points` / `ColorsRgb`；
- 要改就先 `new byte[]` 复制一份再算（内置算子 `img.gray`/`img.blur` 就是这么做的）；
- 也不要缓存并复用自己已产出的数组后再次修改。

---

## 5. C++ 原生 DLL 构建规则（导出契约）

**native 已支持即插即用**：`runtime: "native"` 的 JSON + C++ DLL，宿主加载时按 JSON 自动生成调用壳
（按 §5.5 统一 C ABI marshal 参数/图像/点云并调 algo_free 释放）——**第三方零 C# 代码**。
本节前四条是 DLL 的导出与可见性铁律（决定宿主能否找到你的符号），§5.5 是调用契约。

### 5.1 一个宏：编译期定义 → 导出；使用方不定义 → 导入

```cpp
// imgio_api.h —— 与你的 C 接口头文件一起交付
#pragma once

#ifdef IMGIO_EXPORTS
#  define IMGIO_API __declspec(dllexport)   // 构建本 DLL 时定义 IMGIO_EXPORTS → 导出
#else
#  define IMGIO_API __declspec(dllimport)   // 使用方（宿主/壳）不定义 → 导入
#endif
// Linux 等价：#define IMGIO_API __attribute__((visibility("default")))
```

**CMake 侧（最易错）**：`IMGIO_EXPORTS` 必须是 **PRIVATE**：

```cmake
target_compile_definitions(YourAlgo PRIVATE IMGIO_EXPORTS)   # ✔ PRIVATE
# 绝不能写成 PUBLIC / INTERFACE —— 定义会传播给使用方，
# 使用方也把你的头文件展开成 dllexport → 链接时找不到导入符号。
```

### 5.2 可见性：hidden + 不自动导出 → 接口函数逐个带宏

```cmake
set_target_properties(YourAlgo PROPERTIES
    CXX_VISIBILITY_PRESET hidden          # 非导出符号对外不可见
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)       # 不自动导出全部符号
```

这意味着**不带 `IMGIO_API` 宏的符号一律不导出**——C 接口函数必须**逐个**标注：

```cpp
IMGIO_API int  your_algo_run(int width, int height, const unsigned char* bgra);
IMGIO_API void your_algo_free(void* handle);
```

### 5.3 `extern "C"` 必须（防 C++ 名字改编）

C++ 编译器会做名字改编（name mangling），宿主侧 `GetProcAddress` / `DllImport` 按原名找符号会失败。
头文件用标准包裹模式，C/C++ 双方都能包含：

```cpp
#ifdef __cplusplus
extern "C" {
#endif

IMGIO_API int  your_algo_run(int width, int height, const unsigned char* bgra);
IMGIO_API void your_algo_free(void* handle);

#ifdef __cplusplus
}
#endif
```

### 5.4 检查清单

- [ ] 头文件：宏 + `extern "C"` 包裹（如上模板）；
- [ ] CMake：`PRIVATE IMGIO_EXPORTS`、`CXX_VISIBILITY_PRESET hidden`、`WINDOWS_EXPORT_ALL_SYMBOLS OFF`；
- [ ] 每个 C 接口函数都带 `IMGIO_API`；
- [ ] 验证：Windows 下 `dumpbin /exports YourAlgo.dll`（或 `objdump -p`）应看到**未改编**的函数名。


### 5.5 统一 C ABI（调用契约，v1）

每个算子导出**一个统一签名入口**（JSON `method` 指名）+ 一个释放函数。头文件模板（可直接抄）：

```c
// comui_native_abi.h —— ComUI 原生算子统一 C ABI
#pragma once
#include <stdint.h>

#ifdef IMGIO_EXPORTS
#  define IMGIO_API __declspec(dllexport)     // 构建本 DLL 时定义；Linux 用 visibility("default")
#else
#  define IMGIO_API __declspec(dllimport)
#endif

typedef struct AlgoCParam  { int32_t kind; double d; int32_t i; const char* s; } AlgoCParam;
// kind: 0=double  1=int32  2=text(UTF-8)  3=bool(i!=0)

typedef struct AlgoCImage  { int32_t width, height; const uint8_t* bgra; } AlgoCImage;   // 紧排 BGRA
typedef struct AlgoCCloud  { int32_t count; const float* xyz; const uint8_t* rgb; } AlgoCCloud;  // SoA，rgb 可空

typedef struct AlgoCInput  { int32_t paramCount; const AlgoCParam* params;
                             int32_t imageCount; const AlgoCImage* images;
                             int32_t cloudCount; const AlgoCCloud* clouds; } AlgoCInput;

typedef struct AlgoCImageOut { int32_t width, height; uint8_t* bgra; } AlgoCImageOut;    // DLL 分配
typedef struct AlgoCCloudOut { int32_t count; float* xyz; uint8_t* rgb; } AlgoCCloudOut; // DLL 分配，rgb 可空

typedef struct AlgoCOutput { int32_t imageCount; AlgoCImageOut* images;
                             int32_t cloudCount; AlgoCCloudOut* clouds;
                             const char* error; } AlgoCOutput;   // error=静态存储，宿主只读

#ifdef __cplusplus
extern "C" {
#endif

// 算子入口（每个算子一个导出，JSON method 指名；返回 0=成功，非 0=失败码）
IMGIO_API int32_t your_algo(const AlgoCInput* in, AlgoCOutput* out);

// 释放约定（必须导出）：宿主拷贝完输出后逐指针调用（数组与每个数据缓冲；error 不释放）
IMGIO_API void algo_free(void* p);

#ifdef __cplusplus
}
#endif
```

**实现示例**（图像伽马，对应下方 node.json）：

```cpp
IMGIO_API int32_t native_gamma(const AlgoCInput* in, AlgoCOutput* out) {
    double gamma = 1.0;
    for (int i = 0; i < in->paramCount; i++)
        if (in->params[i].kind == 0) gamma = in->params[i].d;          // 按 JSON 声明顺序
    const AlgoCImage* img = &in->images[0];                            // 输入按端口顺序

    size_t n = (size_t)img->width * img->height * 4;
    uint8_t* px = (uint8_t*)malloc(n);                                  // DLL 分配输出
    for (size_t i = 0; i < n; i += 4) {
        px[i]   = (uint8_t)(255 * pow(img->bgra[i]   / 255.0, 1.0 / gamma));
        px[i+1] = (uint8_t)(255 * pow(img->bgra[i+1] / 255.0, 1.0 / gamma));
        px[i+2] = (uint8_t)(255 * pow(img->bgra[i+2] / 255.0, 1.0 / gamma));
        px[i+3] = 255;
    }
    out->images = (AlgoCImageOut*)malloc(sizeof(AlgoCImageOut));
    out->images[0] = { img->width, img->height, px };
    out->imageCount = 1;  out->cloudCount = 0;  out->error = nullptr;
    return 0;
}
IMGIO_API void algo_free(void* p) { free(p); }
```

**node.json（native）**：

```jsonc
{
  "node": { "id": "vendor.native_gamma", "name": "原生伽马", "group": "2D 图像", "category": "处理" },
  "runtime": "native",
  "assembly": "YourAlgo.dll",
  "method": "native_gamma",
  "inputs":  [ { "name": "输入", "type": "image" } ],
  "params":  [ { "name": "gamma", "label": "Gamma", "type": "number", "default": 2.0 } ],
  "outputs": [ { "name": "输出", "type": "image" } ]
}
```

**绑定规则**：参数按 JSON `params` 声明顺序传入（`kind` 区分类型）；输入按 `inputs` 端口顺序传入
（`type` 决定进 images 还是 clouds）；返回的图像/点云按 `outputs` 端口顺序依次分发
（image 端口取第 1/2/…个返回图像，cloud 端口同理）。`algo.json`（算法配方侧边栏）同样支持
`runtime: "native"`——输入按主题取总线最新帧，输出 `from` 字段填 `"image"`/`"cloud"`（缺省 image）。

---

## 6. 执行模型

1. **拓扑序执行**：Kahn 拓扑排序后逐节点执行。图中有环 → 抛「图中存在环路，无法执行：…」（整条流水线不启动）。
2. **后台线程**：整张图在 `Task.Run` 后台线程跑，方法内可长时间运算，**不要碰任何 UI 控件**。
3. **节点状态**：黄 = 执行中 / 绿 = 成功 / 红 = 失败（悬停看错误信息）。（瞬间完成的流水线，工具栏运行态也会保持约 0.6 秒，反馈不会一闪而过。）
4. **异常语义**：方法抛出 → 该节点红点（悬停显示你的 `ex.Message`）→ 外层再抛「节点「X」执行失败: <message>」→ **整条流水线中止**。所以请 `throw new Exception("人话描述")`，把参数值、尺寸、失败环节写进去。
5. **取消（如实说明）**：`CancellationToken` 参数是**预留位，当前注入的是 `CancellationToken.None`** —— 你方法里的 `ThrowIfCancellationRequested()` 不会真的被触发。真正生效的取消只在**节点与节点之间**检查（点「■ 停止」后后续节点不再启动）。长任务请自行控制时长或分块。
6. **日志**：`IProgress<string>.Report(msg)` 的内容进底部日志面板，前缀为 `[<算子名>]`；返回值前自行 `Console` 打印不会被收集。
7. **自动运行**：默认**关**（手动「▶ 运行」）。开启后改参数/连线会自动重跑。
8. **内嵌预览**：`draw.2d` / `draw.3d` 那种卡片内嵌预览**只对内置算子开放**，JSON 声明的算子没有 —— 想看结果就接发布节点（§7）。

---

## 7. 数据如何到达工作台（最易踩的关键点）

**JSON 算子的 outputs 只流向下游端口，不会自动上总线。** 无论返回值是什么，工作台都看不到，除非末端接了内置的**「发布图像」/「发布点云」**节点：

```
你的算子 ──输出──→ 发布图像(topic=image/stitched) ──→ 总线 ──→ 2D 工作台显示
你的算子 ──输出──→ 发布点云(topic=cloud/merged)  ──→ 总线 ──→ 3D 工作台显示
```

内置发布节点（`NodeDefs`，栏目 `IO`）：

| Id | 名称 | 输入 | 参数 | 默认主题 |
|---|---|---|---|---|
| `pub.image` | 发布图像 | image | `topic`（文本） | `image/stitched` |
| `pub.cloud` | 发布点云 | cloud | `topic`（文本） | `cloud/merged` |

- 改 `topic` 参数即可**多主题分流**；2D/3D 工作台工具栏的「**数据源**」下拉会列出总线上已有数据、类型匹配的主题（约定主题恒在列），切换即可看不同分支的结果 —— 主题清单见 [PLUGIN_DEV.md §3](PLUGIN_DEV.md#3-数据总线插件间唯一数据通道)。
- 只想快速看一眼而不占用总线：可接内置 `draw.2d` / `draw.3d` 预览卡片（栏目「输出预览」），卡片上的「→ 2D 工作台 / → 3D 工作台」一键导入。

---

## 8. 接入验证清单（七步）

1. **放置**：`comdll/algo/<名称>/` 下放 `YourAlgo.dll` + `your.node.json`，依赖全部同目录，目标框架 net8.0，不拷 `ComUI.Sdk.dll`。
2. **加载**：宿主内按 **F5**（或重启），底部日志出现 `算子(JSON)已注册: <名称> <<id>> ← <文件>`。
   没有出现 = 注册失败，日志会点名原因，对号入座：

   | 日志原因 | 排查 |
   |---|---|
   | `缺少 node.id / node.name` | JSON 缺必填字段或为空 |
   | `算子 JSON 解析失败 <文件>: …` | JSON 语法错（注释/尾逗号是允许的，其它不行） |
   | `找不到程序集 X` | `assembly` 拼错，或 DLL 不在 JSON 同目录 |
   | `找不到类型 X` | `type` 命名空间/类名不对 |
   | `找不到方法 X.Y` | 方法名不对，或**同名重载**导致歧义 |
   | `找不到原生库 / 找不到导出…` | 检查 assembly 路径与导出名（§5 导出规则：宏/extern C/dumpbin 验证） |
   | **无任何日志** | 文件不在 `comdll/algo/` 下、后缀不是 `*.node.json`，或 **Id 与已有算子重复被静默忽略** |
3. **出现**：左侧「🧰 算子库」按 `group` 栏目分组出现你的算子（可搜索、栏目头可拖拽排序）。
4. **建节点**：拖到画布（或单击加到中心），节点卡片上参数按 `params` 生成、端口按 `inputs/outputs` 生成。
5. **连线**：从上游输出端口拉线到你的输入端口（**类型必须匹配**：image↔image、cloud↔cloud）。
6. **运行**：「▶ 运行」→ 黄（执行中）→ 绿（成功）；红 = 悬停看错误信息（就是你抛的 message）。
7. **看结果**：末端接 `pub.image` / `pub.cloud` → 2D/3D 工作台显示；或接 `draw.2d` / `draw.3d` 看卡片内预览。

---

## 9. 规则红线（七条）

1. **纯计算**：算子 DLL 禁止引用/创建任何 UI 控件（Avalonia 等）—— 显示一律经总线由工作台负责。
2. **自包含**：目标框架 **net8.0**；所有第三方依赖平铺在本目录（宿主只在本目录解析）；**`ComUI.Sdk.dll` 由宿主提供，不要拷入**。
3. **Id 命名空间**：`node.id` 用 `<厂商>.<名称>` 前缀 —— 冲突时是**静默忽略**，不会覆盖内置算子，也不会报错，排查成本极高。
4. **数组不可变**：产出/发布的载荷数组不得原地修改（下游零拷贝共享，见 §4）。
5. **异常带信息**：`throw new Exception("人话描述")` —— 节点红点悬停直接显示它，也是流水线中止时唯一的线索。
6. **无生命周期钩子**：算子没有 `Initialize`/`Shutdown`（不是 `IPlugin`）；实例方法每次执行新建实例。资源在方法内自申请自释放。
7. **native = 统一 C ABI**：`runtime: "native"` 走 §5.5 契约（单一入口签名 + algo_free 释放约定），宿主自动 marshal；只有需要复杂生命周期/自定义宿主服务时才写 C# 壳（IAlgoPlugin）。
