#ifndef POINT3DALG_BUMPREPORT_H
#define POINT3DALG_BUMPREPORT_H

// Point3DAlg_BumpReport —— bump 全参数量测报表（高度 + 直径 + 共面性偏差 + NG 位）。
//
// 场景：产线对 bump 阵列的一次性全参数检测——每个 bump 输出：
//   高度（拱形球顶拟合，DomeFit）、半高处直径（切片圆拟合，CircleFit）、
//   相对最佳拟合面的共面性偏差、以及按上下限的 NG 位标记。
//
// 算法链路：
//   1. DomeFit 拟合全部 bump（高度/球心/半径/基面）；
//   2. 逐 bump 半高切片圆拟合：半高处球冠截面是圆，半径 r_half 由球参数解析
//      （sqrt(R² − (R−h/2)²)），取 apex 半高带内、XY 距球心 ≤ r_half×1.3 的点
//      做 FitCircle2D → 直径 = 2×拟合半径；圆心偏离 apex 可作质量旁证；
//   3. 共面性偏差：对全部 bump 的 (x, y, height) 拟合最佳平面，dev = height −
//      平面在该处高度（剔除晶圆整体倾斜/翘曲后的工艺散差，逐 bump 输出）；
//   4. NG 位（bit 掩码）：bit0 高度越界、bit1 直径越界、bit2 直径拟合失败。
//
// 性能：z-bin 哈希一次装箱，逐 bump 只扫其半高带覆盖的 z 格——480K 点
// 2000 bump 量级无 O(n²)。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <string>
#include <vector>

// ---------------- 参数 ----------------
struct POINT3DALG_EXPORT Point3DAlg_BumpReportParams {
    // DomeFit 透传（见 Point3DAlg_DomeFit.h）
    double height_threshold;    // bump 判定高度阈值 mm（默认 0.02）
    double cluster_tolerance;   // 聚类连通距离 mm（默认 0.25；须 > 冠直径）
    int    min_points;          // 每簇最少点数（默认 8）
    // 截面切片
    double depth_ratio;         // 截面深度（距球顶比例，默认 0.35）。
                                // 纯球冠的 z 范围是 [顶, 冠底]——"半高"可能低于冠底
                                // 而无截面；距顶比例恒在冠内
    double half_band;           // 切片带半宽 mm（默认 0.02；过小点太少、过大偏离截面）
    // 圆拟合
    double circle_mad_factor;   // 残差修剪因子（默认 3.0）
    double circle_rms_max;      // 圆拟合 RMS 上限 mm（默认 0.02，超限 diameter<=0）
    // NG 上下限（<=0 = 不判该位）
    double height_min;          // 高度下限 mm（默认 0）
    double height_max;          // 高度上限 mm（默认 0）
    double diameter_min;        // 直径下限 mm（默认 0）
    double diameter_max;        // 直径上限 mm（默认 0）
    // 位置度（名义网格）：pitch > 0 启用；nominal 点 = origin + i·pitch（规则网格，
    // 平移-only 不含旋转——小旋转 <0.5° 误差可忽略，大旋转需先配准）
    double nominal_pitch_x;     // 名义网格 X 间距 mm（默认 0 = 不算位置度）
    double nominal_pitch_y;     // 名义网格 Y 间距 mm
    double nominal_origin_x;    // 名义网格原点 X mm（首个名义位置）
    double nominal_origin_y;    // 名义网格原点 Y mm
    double pos_threshold;       // 位置度 NG 阈值 mm（默认 0.05）

    Point3DAlg_BumpReportParams();
};

// ---------------- 单 bump 报表 ----------------
struct POINT3DALG_EXPORT Point3DAlg_BumpReportEntry {
    double x, y;             // 球顶 XY
    double height;           // 高度（相对基面法向）
    double diameter;         // 距顶 depth_ratio 处截面直径 mm（球参数解析值 2×rHalf）
    double circle_rms;       // 圆拟合残差 RMS（diameter<=0 时无意义）
    int    circle_points;    // 圆拟合参与点数
    double coplanarity_dev;  // 相对最佳拟合面的高度偏差 mm
    double pos_dev;          // 位置度 mm = |apex XY − 最近名义点|（未启用/无名义点 = -1）
    int    matched_nom;      // 1 = 匹配到名义网格点；0 = 该 bump 不在名义网格邻域
    int    ng_flags;         // bit0 高度越界 bit1 直径越界 bit2 截面校验失败
                             // bit3 位置度越界 / 未匹配名义点/点不足

    Point3DAlg_BumpReportEntry();
};

// ---------------- 汇总统计 ----------------
struct POINT3DALG_EXPORT Point3DAlg_BumpReportStats {
    int    count;            // bump 总数
    int    ng_height;        // 高度 NG 数
    int    ng_diameter;      // 直径 NG 数（含拟合失败）
    double h_mean, h_sigma;  // 高度统计
    double h_min, h_max;
    double d_mean, d_sigma;  // 直径统计（仅有效拟合）
    double cop_sigma;        // 共面性偏差 σ（剥离拟合面后）
    int    nominal_total;    // 名义网格点总数（pitch 启用时；= 已匹配 bump 覆盖的名义索引范围）
    int    nominal_matched;  // 匹配到名义点的 bump 数
    int    nominal_unmatched;// 范围内无对应 bump 的名义点数（内部漏检；阵列外圈缺失不计）
    double pos_mean;         // 位置度均值
    double pos_sigma;        // 位置度标准差
    double pos_max;          // 最大位置度
    int    ng_pos;           // 位置度 NG 数

    Point3DAlg_BumpReportStats();
};

/// bump 全参数量测报表。
/// [out] entries  按 apex (x, y) 升序（帧间稳定对应）
/// [out] stats    汇总（可为 nullptr）
/// 返回 false = 云空 / 未检出 bump（与 DomeFit 失败语义一致）。
POINT3DALG_EXPORT bool Point3DAlg_BumpReport(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
    const Point3DAlg_BumpReportParams& params,
    std::vector<Point3DAlg_BumpReportEntry>& entries,
    Point3DAlg_BumpReportStats* stats = nullptr);

#endif // POINT3DALG_BUMPREPORT_H
