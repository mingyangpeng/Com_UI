using System.Collections.Generic;

namespace ComUI.Sdk;

/// <summary>
/// 总线约定主题注册表（主题即插件之间的契约）：
/// 新增互联先在此登记（主题名/载荷/谁发谁订），再在各端引用常量——禁止散落字符串字面量（拼错即静默失联）。
/// 三类：数据（算法产出→显示，最新帧保留）、选择联动（面板对等广播，载荷带 Source 防回环）、工作流（事件通知）。
/// </summary>
public static class BusTopics
{
    // —— 数据主题（算法产出 → 显示面板；每主题保留最新帧，晚订阅立即拿到当前帧） ——

    /// <summary>合并/生成的点云（流水线 pub.cloud、算法 → 3D 显示）。载荷：CloudPayload。</summary>
    public const string CloudMerged = "cloud/merged";

    /// <summary>点云移除（pub.remove 算子 → 3D 显示移除实体）。载荷：string（实体 Id）。</summary>
    public const string CloudRemoved = "cloud/removed";

    /// <summary>拼接/生成的图像（流水线 pub.image、算法 → 2D 显示）。载荷：ImagePayload。</summary>
    public const string ImageStitched = "image/stitched";

    // —— 选择联动主题（面板对等广播；载荷 SelectionPayload 带 Source 字段防回环） ——

    /// <summary>3D 拾取单点（点云 → 图像高亮等联动）。载荷：SelectionPayload(Kind=Point3D，含 Uv)。</summary>
    public const string SelPoint3D = "sel/point3d";

    /// <summary>2D ROI 框选（图像 → 需要框选信息的一方）。载荷：SelectionPayload(Kind=Roi2D)。</summary>
    public const string SelRoi2D = "sel/roi2d";

    /// <summary>3D 框选区域。载荷：SelectionPayload(Kind=CloudBox，含 Bounds3D)。</summary>
    public const string SelCloudBox = "sel/cloud-box";

    // —— 工作流主题（算法侧边栏 ↔ 流水线插件；载荷 string 文件路径） ——

    /// <summary>打开算法（侧边栏单击 → 流水线插件开新页或跳转已打开页）。载荷：string（文件路径）。</summary>
    public const string PipelineOpen = "pipeline/open";

    /// <summary>算法文件已保存/列表变化（流水线、侧边栏改名 → 侧边栏刷新）。载荷：string（文件路径）。</summary>
    public const string AlgoSaved = "algo/saved";

    /// <summary>算法锁定状态切换（侧边栏 → 流水线插件同步已打开页只读态）。载荷：string（文件路径）。</summary>
    public const string AlgoLocked = "algo/locked";

    /// <summary>激活/聚焦一个窗口面板（载荷=面板 Id，如 "ui.cloud3d.view"）——数据树点击行名跳转对应工作台用。</summary>
    public const string PanelActivate = "ui/activate-panel";
}

/// <summary>数据总线：算法 ↔ 显示的桥梁。按主题发布/订阅，每个主题保留最新一帧。</summary>
/// <remarks>
/// - 晚订阅者立即可拿到该主题的当前最新帧（"先出数据后开面板"不会错过）。
/// - 总线对载荷类型零假设：推荐使用 comdll/common 中共享契约（算法 IO 接口）定义的类型，
///   本 Sdk 内置的 <see cref="ImagePayload"/> / <see cref="CloudPayload"/> 仅为占位与示例。
/// - 大帧建议实现 IDisposable 并由订阅方释放；当前实现管理托管数组，GC 自行回收。
/// </remarks>
public interface IBus
{
    /// <summary>
    /// 发布一帧到主题（线程安全；保留为最新帧并推送给订阅者）。
    /// 替换时若旧帧实现 IDisposable 且未被引用则由总线释放——
    /// 订阅回调内必须同步消费（拷贝/上传），回调返回后不得再引用旧帧。
    /// </summary>
    void Publish<T>(string topic, T payload);

    /// <summary>
    /// 订阅主题。<paramref name="uiThread"/> 为 true 时回调被封送到 UI 线程。
    /// 若主题已有保留帧，会立即回调一次。
    /// </summary>
    IDisposable Subscribe<T>(string topic, Action<T> handler, bool uiThread = false);

    /// <summary>取主题当前保留帧（无帧或类型不符返回 false）。</summary>
    bool TryGetLatest<T>(string topic, out T value);

    /// <summary>全部主题快照（主题/载荷类型/帧计数/最近发布时间）——总线监视面板用。</summary>
    IReadOnlyList<BusTopicInfo> GetTopics();

    /// <summary>
    /// 总线级发布钩子：每次 Publish 存储完成后触发（主题, 载荷）。
    /// 可能在任意线程回调——全局追踪类界面需自行封送 UI 线程。总线监视面板用。
    /// </summary>
    event Action<string, object?>? FramePublished;

    /// <summary>取主题当前保留帧的运行时对象（不校验类型）——总线监视详情用。</summary>
    bool TryGetLatestRaw(string topic, out object? payload);
}

/// <summary>主题快照信息。</summary>
public sealed record BusTopicInfo(string Topic, string? LatestType, long FrameCount, DateTime? LastPublish);

/// <summary>图像帧载荷。**契约 = OpenCV 模式**：布局与 cv::Mat(CV_8UC4) 完全一致——
/// 行主序、逐像素交错 B,G,R,A、**紧排列（step = Width×4，无行对齐填充）**。
/// cv::Mat 连续（isContinuous）时 data 可整块 memcpy；ROI/非连续 Mat 需逐行拷贝或先 clone()；
/// 3 通道 BGR Mat 先 cvtColor 到 BGRA。详见 docs/THIRD_PARTY_OPS.md。</summary>
public sealed class ImagePayload
{
    public int Width { get; init; }
    public int Height { get; init; }

    /// <summary>BGRA32 像素数据（= cv::Mat CV_8UC4 data），长度 = Width * Height * 4，紧排列。</summary>
    public byte[] PixelsBgra { get; init; } = Array.Empty<byte>();

    /// <summary>图像来源说明，如 "拼接结果"。</summary>
    public string? Source { get; init; }

    /// <summary>数据名（数据树「2D 图像」栏显示；null=未命名）。draw 导入/发布算子可设置。</summary>
    public string? Id { get; init; }
}

/// <summary>点云帧载荷。**契约 = PCL 模式**：数据为 PCL PointCloud&lt;PointXYZRGB&gt; 的 SoA 展开——
/// Points = [x0,y0,z0, x1,y1,z1, …]（float，长度 = Count×3）；
/// ColorsRgb = [r0,g0,b0, r1,g1,b1, …]（byte，长度 = Count×3，可选）。
/// PCL 侧逐点拷 x/y/z，颜色从 packed rgb（uint32：r&lt;&lt;16|g&lt;&lt;8|b）拆三字节。详见 docs/THIRD_PARTY_OPS.md。</summary>
public sealed class CloudPayload
{
    /// <summary>点云实体 Id（查看器按它做多实体管理：同 Id 重发=更新；空则落 "点云"）。</summary>
    public string? Id { get; init; }

    /// <summary>XYZ 坐标（SoA：x,y,z 连续 float），长度 = Count * 3。</summary>
    public float[] Points { get; init; } = Array.Empty<float>();

    /// <summary>逐点 RGB（SoA：r,g,b 连续 byte），长度 = Count * 3；为空时由查看器按高度伪彩上色。</summary>
    public byte[]? ColorsRgb { get; init; }

    public int Count { get; init; }

    public string? Source { get; init; }

    /// <summary>约定主题名：点云帧发布（查看器按 Id 增/改）。</summary>
    public const string TopicMerged = BusTopics.CloudMerged;

    /// <summary>约定主题名：点云移除（只需 Id，查看器收到后删除该实体）。</summary>
    public const string TopicRemoved = BusTopics.CloudRemoved;
}

/// <summary>面板联动选择事件载荷。</summary>
public enum SelectionKind
{
    /// <summary>三维点选择：Point3D 有效。</summary>
    Point3D,

    /// <summary>二维矩形 ROI：Rect2D 有效（图像像素坐标）。</summary>
    Roi2D,

    /// <summary>三维包围盒选择（如点云框选）：Bounds3D 有效。</summary>
    CloudBox,
}

/// <summary>面板间联动的选择载荷。</summary>
public sealed record SelectionPayload(
    SelectionKind Kind,
    string Source,                 // 发起面板 Id
    double[]? Point3D,             // Kind == Point3D: [x, y, z]
    double[]? Rect2D,              // Kind == Roi2D: [x, y, w, h]（图像像素坐标）
    string? Note = null,
    double[]? Bounds3D = null,     // Kind == CloudBox: [minX, minY, minZ, maxX, maxY, maxZ]
    double[]? Uv = null)           // Kind == Point3D 可选: [u, v] 点云包围盒内归一化位置
                                   //   约定 u=(x-minX)/sizeX、v=(z-minZ)/sizeZ；关联数据（如高度图）按
                                   //   u*图宽、v*图高 映射到图像像素，实现"点云选点↔图像高亮"
{
    /// <summary>约定主题名：三维点选择。</summary>
    public const string TopicPoint3D = BusTopics.SelPoint3D;

    /// <summary>约定主题名：图像 ROI 选择。</summary>
    public const string TopicRoi2D = BusTopics.SelRoi2D;

    /// <summary>约定主题名：三维包围盒选择（点云框选等）。</summary>
    public const string TopicCloudBox = BusTopics.SelCloudBox;
}
