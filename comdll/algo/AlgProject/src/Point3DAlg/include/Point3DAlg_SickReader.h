#ifndef POINT3DALG_SICKREADER_H
#define POINT3DALG_SICKREADER_H

// SICK Ranger3 扫数据读取类（C++）。
// 数据布局与换算链路同 Juxin6X3D stitchingDepthImg（ReadSickData/ReadFolderData/ReadScan3DMultiPart），
// 已按 Z:\ScanImage\3D 真实数据逐字节验证：
//   .dat 平面布局：[N × 5120B 高度图（2560 × uint16 小端，0 = 无效点）]
//                  [N × 20B  编码器记录（5 × int32：A, 0x40000000, B, C, 0）]
//     A 为扫描轴编码器值，逐行 ±scanStep，蛇形往返由方向翻转体现。
//   同名 .xml（icon_data_format，a/c axis range scale/offset）：**预留**——当前实现
//   未读取 xml，换算完全依赖 recipe/common json（需要兜底时再按 icon_data_format 补）。
//   Scan3DPart*.json "Coordinate 3D"：[[x1, x2, y, 0], ...] 每个扫描行（pass）一条，y 为行中心。
//
// 用法（见 Point3DAlg_test.cpp）：
//   1. Point3DAlg_LoadSickConfig(recipe, common, cfg)   —— 读两个 json 配置（提取不到值报错）
//   2. SickReader reader(cfg)                           —— 构造时接收 SICK 相关参数
//   3. reader.LoadRaw(folder, frame)                    —— 加载原始图像（uint16 原始值 + 编码器）
//   4. reader.GetDepth() / reader.GetCloud()            —— 矫正后深度图 / 深度图生成的点云
//   5. Point3DAlg_SaveCloud / Point3DAlg_SaveDepth      —— 单独保存点云 / 深度图

#include "AlgExport.h"
#include <cstdint>
#include <string>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- SICK 相关参数（构造 SickReader 时传入） ----------------
// Point3DAlg_LoadSickConfig 从两个 json 提取；也可手工填。
typedef struct Point3DAlg_SickConfig {
    double mm_pulse;    /* 编码器脉冲→mm（CommonParams mmPulse，默认 0.0002） */
    double x_scale;     /* 扫描向 mm/列（配方 scanpulses 原值）。
                         注意：GetCloud 的 x 步距不用它，用数据实测 scanStep×mmPulse——
                         配方镜头与数据不匹配（如 5x 配方读 10x 数据）时配方值会错。
                         本字段仅供日志/与实测值一致性校验，算法不消费。 */
    double y_scale;     /* 轮廓向 mm/px（配方 scaleXYZ[1]） */
    double z_scale;     /* 高度 mm/单位（配方 scaleXYZ[2]） */
    double z_offset;    /* 高度零偏 mm（配方 scaleXYZ[3]） */
    double comp_fit0;   /* 一阶补偿分母系数（compensate_All[lens][0]） */
    double comp_fit1;   /* 一阶补偿斜率（compensate_All[lens][1]） */
    double comp_poly[6];/* 高阶补偿系数（compensateValue_All[lens][0..5]，不足 6 个补 0） */
    int    comp_count;  /* comp_poly 有效系数个数 */
} Point3DAlg_SickConfig;

// ---------------- 单独的 json 配置读取接口 ----------------
// 读用户配方 + 通用参数两个 json，提取 SICK 相关参数。
// recipe_path / common_path：NULL 或空串 = 默认路径
//   （recipe 默认 Z:\Calibrate\3D\MtxJuxin60003D.dll.user.json，
//    common 默认 Z:\Juxin60003D_CommonParams.json）。
// 提取不到必需值（scanpulses/scaleXYZ/LensRatio/mmPulse/补偿组）→ 返回非 0 并写 err_msg：
//   1 = recipe 文件不存在；  2 = recipe 缺 scanpulses / scaleXYZ；
//   3 = recipe 缺 LensRatio；4 = common 文件不存在；
//   5 = common 缺 mmPulse；  6 = common 缺补偿组（LensRatio 无 "5x"/"10x" 对应组）。
// err_msg 为调用方提供的缓冲（可为 NULL），失败原因写入其中（UTF-8/ANSI，截断到 err_len）。
POINT3DALG_EXPORT int Point3DAlg_LoadSickConfig(const char* recipe_path, const char* common_path,
                                                Point3DAlg_SickConfig* cfg,
                                                char* err_msg, int err_len);

// ---------------- 单独的保存接口 ----------------
// 点云保存为 PLY（binary=1 二进制 little-endian，0 ASCII 文本）。返回 0 成功。
POINT3DALG_EXPORT int Point3DAlg_SaveCloud(const char* path,
                                           const float* xyz, int count,
                                           int binary /*0=ASCII 1=二进制*/);

// 深度图保存：rows×cols 的 float 深度图（mm，0=无效）存为 16 位灰度 PNG
// （有效点线性映射 1..65535，0 = 无效点专用；zmin/zmax 为映射范围，传 0 则按数据自适应）。
POINT3DALG_EXPORT int Point3DAlg_SaveDepth(const char* path,
                                           const float* depth, int rows, int cols,
                                           double zmin, double zmax);

// 深度图可视版：同上映射到 1..255 的 8 位灰度 PNG（低=暗 高=亮），供人眼查看。
// 注意：依赖 OpenCV（POINT3DALG_NO_OPENCV 构建时返回 -9，主链路不受影响）。
POINT3DALG_EXPORT int Point3DAlg_SaveDepthView(const char* path,
                                               const float* depth, int rows, int cols,
                                               double zmin, double zmax);

// FOV 框标注：8 位 RGB 可视图上画随机彩色框 + FOV 序号（画在框左上角）。
// 注意：依赖 OpenCV（POINT3DALG_NO_OPENCV 构建时返回 -9，主链路不受影响）。
//   每个 FOV 的框颜色按 id 哈希随机（HSV 全彩），与灰度底图区分明显。
//   rects[i] = {x, y, w, h}（像素，列 x 起、行 y 起）；ids[i] = FOV 序号。
//   ids 为 NULL 时用数组下标 i 作为序号。返回 0 成功。
POINT3DALG_EXPORT int Point3DAlg_DrawFovBoxes(const char* path,
                                              const float* depth, int rows, int cols,
                                              double zmin, double zmax,
                                              const int* rects /*x,y,w,h ×n*/,
                                              const int* ids, int n);

#ifdef __cplusplus
} // extern "C"
#endif

// ---------------- 拼接质量自检结果 ----------------
// 多相机衔接带的格级 z 差统计（差值 = 相邻 rank 相机在物理同位置的 z 差）。
struct POINT3DALG_EXPORT Point3DAlg_StitchQualityResult {
    long long pairs;         // 配对格数（两相机均有有效 z）
    double dh_mean;          // 差值均值
    double dh_sigma;         // 差值标准差
    double dh_max_abs;       // 最大绝对差
    long long outlier;       // |差值| > 阈值的格数
    double outlier_ratio;    // outlier / pairs

    Point3DAlg_StitchQualityResult();
};

// ---------------- SICK 读取类 ----------------
// 构造时接收 SICK 相关参数（Point3DAlg_SickConfig）；
// LoadRaw 加载原始图像，GetDepth 得矫正后深度图，GetCloud 得深度图生成的点云。
// 任何一步失败（文件缺失/无有效点/内存不足）抛 std::runtime_error 并带人话描述。
class POINT3DALG_EXPORT Point3DAlg_SickReader
{
public:
    // cfg：SICK 相关参数（来自 Point3DAlg_LoadSickConfig 或手工填写）
    explicit Point3DAlg_SickReader(const Point3DAlg_SickConfig& cfg);

    // ---------------- 加载原始图像 ----------------
    // folders：扫描数据文件夹列表（每个相机一个，如 3D 与 3D-2；单相机传 1 个即可）。
    //   每个文件夹内 *.dat + Scan3DPart*.json（.xml 预留未读），各相机独立拼接后按
    //   Juxin 双相机交织合并（见 GetDepth 注释）。
    // frame：>=0 只读每个相机的第 frame 帧（按文件名顺序，从 0 计）；<0 读全部帧。
    // 加载后 Width()/Rows()/RowY() 立即可用；Raw()/Encoder() 懒构建
    // （首次调用才从 FOV 拼合，避免全量复制占内存）。
    void LoadRaw(const std::vector<std::string>& folders, int frame = -1);

    // 单相机便捷重载
    void LoadRaw(const std::string& folder, int frame = -1)
        { LoadRaw(std::vector<std::string>{ folder }, frame); }

    // 相机数量（LoadRaw 之后有效）
    int DeviceCount() const { return (int)devices_.size(); }

    // ---------------- 基于原始图矫正后的拼接深度图 ----------------
    // 单相机：拼接图布局（与 Juxin MxtStitchingDepthImg 语义一致）：
    //   行 = 轮廓轴（2560 × pass 数，含视野重叠裁剪），列 = 扫描轴（编码器行程/scanStep）；
    //   每个原始行旋转 90° 成为拼接图一列，列号 = (A-aMin)/scanStep；
    //   蛇形 down 的 pass 行内镜像；pass 之间按行中心 Y（运动表）排布换行。
    //   z_mm = raw × z_scale + z_offset - diff(col)（激光倾斜补偿）。
    // 多相机（双相机交织，与 Juxin MxtStitchingDepthImg v2 一致）：
    //   每个相机先各自拼接成单机图 → 按各自起始编码器脉冲对齐（左补 0 到公共起点）→
    //   按起始脉冲排序 → 列交织合并（第 k 列依次取排序后第 k%N 台相机的第 k/N 列）→
    //   左右全零裁剪。交织后 x 步距 = 实测 scanStep × mmPulse / 相机数（见 GetCloud）。
    // 返回 StitchRows()×StitchCols() 的行主序缓冲（内部持有，随对象存活）。
    // LoadRaw 之后调用；重复调用返回同一缓冲。
    const float* GetDepth();

    // ---------------- 基于拼接深度图生成的点云 ----------------
    // x = col × (实测 scanStep × mmPulse) / DeviceCount()（交织后步距；不用配方 scanpulses，
    //   原因见 x_scale 字段注释），y = (StitchRows()/2 - row) × y_scale，
    // z = 深度图值（0 的点跳过）。
    // 返回 SoA 点云（xyz 连续 float，×3），点数存 *out_count（可为 NULL）。
    // 依赖 GetDepth()；重复调用返回同一缓冲。
    const float* GetCloud(int* out_count = nullptr);

    // ---------------- 原始数据访问（LoadRaw 之后有效） ----------------
    // 相机 0 的原始数据懒构建：LoadRaw 不再复制全量（此前相机 0 数据在
    // fovs[].raw 与 raw_ 各存一份，双相机 ~360MB 数据内存翻倍）——
    // 首次调用 Raw()/Encoder() 时才从 FOV 拼合（review P2）。
    const uint16_t*      Raw() const;                              // 原始高度值
    const int32_t*       Encoder() const;                          // 每行扫描轴编码器值
    int                  Width() const { return width_; }          // 每行轮廓像素数（2560）
    long long            Rows() const { return rows_; }            // 原始总行数
    const std::vector<double>& RowY() const { return rowY_; }     // 每个扫描行（pass）的中心 Y

    // ---------------- 强度（GetDepth 之后有效；新格式 .dat 才有） ----------------
    // 强度图与拼接深度图同几何（行主序 uint8，0 = 无效；无强度格式返回空 vector）
    bool                        HasIntensity() const { return hasIntensity_; }
    const std::vector<uint8_t>& GetIntensity() const { return intensity_; }
    // 点云（非零 z 点）逐点强度（uint8/点，与点顺序一致；GetCloud 之后有效）
    const std::vector<uint8_t>& GetCloudIntensity() const { return cloudIntensity_; }

    // ---------------- 拼接图尺寸（GetDepth 之后有效） ----------------
    long long            StitchRows() const { return stitchRows_; }  // 拼接深度图行数
    long long            StitchCols() const { return stitchCols_; }  // 拼接深度图列数

    // ---------------- FOV 框信息（LoadRaw 之后有效） ----------------
    // FOV 在最终拼接图（含多相机交织 + 左右裁剪）中的覆盖矩形，用于可视标注。
    // rects[i*4..i*4+3] = {x, y, w, h}（像素：列 x、行 y、宽 w、高 h）；
    // ids[i] = FOV 全局序号（相机 d 的 FOV j 编号 = d*1000+j，标注为 "d:j"）。
    // 返回框数量。GetDepth 之后调用（需要裁剪后的最终列偏移）。
    int FovBoxes(std::vector<int>& rects, std::vector<int>& ids) const;

    // ---------------- 拼接质量自检（GetDepth 之后有效） ----------------
    // 多相机衔接带：相邻 rank 相机的物理同位置列在交织图中相邻——逐行配对两格
    // 求 z 差。正常标定下差 ≈ 两相机噪声合成；差值大 = 相机标定漂移/丢帧/同步异常。
    // 单相机（n<2）无衔接带，返回 false。
    bool StitchQuality(double diff_threshold,
                       Point3DAlg_StitchQualityResult& out) const;

private:
    Point3DAlg_SickReader(const Point3DAlg_SickReader&);            // 禁拷贝（缓冲大）
    Point3DAlg_SickReader& operator=(const Point3DAlg_SickReader&);

    // FOV 数据结构：一个 .dat 文件 = 一个 FOV（一次连续采集的视野）。
    // LoadRaw 阶段 1 只读编码器块，即可确定全部拼接信息：
    //   换行（pass 序号）、行偏移（运动表）、
    //   上下裁剪 ROI（与上一行的视野重叠）、左右列范围（编码器跨度）。
    // 阶段 2 再按 ROI 多线程读取 .dat 图像块。
    // 蛇形 down 方向不落盘：拼接列号 = (A-aMin)/scanStep 已把方向统一消化。
    struct Fov {
        std::string    path;        // .dat 路径
        long long      lines;       // 行数（文件大小 / 5140）
        int32_t        aFirst;      // 首行编码器值（原始序）
        int32_t        aLast;       // 末行编码器值（原始序，尾部 0 已外推）
        int            pass;        // 扫描行序号（换行 = pass+1）
        long long      rowOff;      // 在单机拼接图中的行偏移
        int            cropTop;     // 上裁剪行数（与上行视野重叠的上半部分）
        int            cropBot;     // 下裁剪行数（重叠的下半部分，保留 [cropTop, lines-cropBot)）
        long long      colFirst;    // 首行在拼接图中的列号
        long long      colLast;     // 末行在拼接图中的列号
        std::vector<int32_t>  encRow;  // 每行编码器值（阶段 1 读取，阶段 2 保留）
        std::vector<uint16_t> raw;  // 高度图像块（阶段 2 多线程填充，行 × 2560）
        bool   hasRef = false;      // 文件含强度通道（新格式：每行 = 高度 2W + 强度 W + 编码器 20）
        std::vector<uint8_t>  refRow;  // 强度图像块（阶段 2 填充，行 × 2560，0 = 无效）
    };

    // 单相机数据（LoadRaw 逐相机加载，独立拼接）
    struct Device {
        std::string                folder;
        std::vector<Fov>           fovs;        // FOV 列表（按文件名顺序 = 扫描顺序）
        std::vector<double>        rowY;        // 每个 pass 的中心 Y（运动表）
        int                        scanStep;    // 编码器步距（众数）
        int32_t                    aMin;        // 编码器最小值
        int32_t                    aMax;        // 编码器最大值（拼接列 0 对应，Juxin 方向：A 大 → 列小）
        int                        nPass;       // pass 数
        std::vector<long long>     rowOff;      // 每个 pass 在单机拼接图中的行偏移
        long long                  stitchRows;  // 单机拼接图行数
        long long                  stitchCols;  // 单机拼接图列数
        std::vector<float>         depth;       // 单机拼接深度图（mm，懒生成）
        std::vector<uint8_t>       depthRef;    // 单机拼接强度图（0 = 无效；无强度格式为空）
    };

    // 阶段 1：只读编码器块 + json 运动表，建 FOV 结构（换行/裁剪/ROI 全部确定）
    void ScanDevice(Device& dev, int frame);

    // 按共用运动表确定每个 FOV 的行偏移与上下裁剪 ROI（视野重叠）
    void AssignFovRows(Device& dev);

    // 阶段 2：多线程按 FOV 读取 .dat 图像块（填充 Fov::raw）
    void LoadFovImages(Device& dev);

    // 单相机拼接深度图（Device::depth 懒生成，基于 FOV 结构）
    void BuildDeviceDepth(Device& dev);

    // 多相机交织合并（单机图 → 交织图，Juxin v2 逻辑）+ 左右全零裁剪
    void InterleaveMerge();

    // 相机 0 原始数据懒拼合（Raw()/Encoder() 首次调用时执行；const 方法用 mutable）
    void EnsureRaw0() const;

    Point3DAlg_SickConfig cfg_;          // SICK 相关参数（构造时传入）

    // 相机 0 的原始数据（懒拼合缓存，Raw()/Encoder() 访问时才填充）
    mutable std::vector<uint16_t> raw_;  // rows_ × width_（相机 0 全部 FOV 拼接）
    mutable std::vector<int32_t>  enc_;  // rows_
    std::vector<double>   rowY_;         // 每个 pass 的中心 Y
    int                   width_;        // 2560
    long long             rows_;

    // 多相机
    std::vector<Device>   devices_;      // 每个相机独立加载/拼接

    // 最终交织拼接图（GetDepth 产物）
    long long             stitchRows_;   // 交织图行数
    long long             stitchCols_;   // 交织图列数

    // FOV 框（InterleaveMerge 时记录，最终拼接图坐标）
    std::vector<int>      fovBoxes_;     // n × {x, y, w, h}
    std::vector<int>      fovBoxIds_;    // n × FOV 序号（相机 d 的 FOV j = d*1000+j）

    // 派生产物（懒生成，内部持有）
    std::vector<float>    depth_;        // stitchRows_ × stitchCols_（mm）
    std::vector<uint8_t>  intensity_;    // 同尺寸强度图（0 = 无效；无强度格式为空）
    std::vector<float>    cloud_;        // 点数 × 3（mm）
    std::vector<uint8_t>  cloudIntensity_;  // 点云逐点强度（非零 z 点，uint8/点）
    int                   cloudCount_;
    bool                  hasIntensity_ = false;   // 当前 .dat 格式含强度通道
};

#endif // POINT3DALG_SICKREADER_H
