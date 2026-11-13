#ifndef POINT3DALG_CIRCLEFIT_H
#define POINT3DALG_CIRCLEFIT_H

// Point3DAlg_CircleFit —— 高度切片圆/孔位量测（纯函数，零 UI）。
//
// 场景：定位孔/螺孔/圆柱 boss 在点云中是**竖直圆壁**——取高度带 [z_low, z_high]
// 切片（壁点保留、基面/顶面点被切掉），切片点聚类后每簇做 XY 平面圆拟合，
// 得到孔位圆心/孔径与拟合质量。
//
// 算法：
//   1. 切片：z ∈ [z_low, z_high] 的点（含端点）；
//   2. 欧式聚类（复用 shapely::CloudProcessor::segmentEuclideanCluster）；
//   3. 每簇 Kåsa 代数圆拟合（x²+y² = 2a·x + 2b·y + k，中心化 3x3 法方程），
//      残差 |dist−R| 双侧 MAD 修剪后重拟合（与 DomeFit 球拟合同构）；
//   4. 半径越界 / 残差超限 / 非有限 → valid=false（保留在结果里，调用方过滤）。
//
// 注：
//   * 圆壁点应尽量覆盖整圆——部分弧（<180°）时半径误差被放大（Kåsa 对短弧
//     病态），rms_max 守卫可拦但圆心可能仍偏；孔壁采样不足时调大高度带；
//   * z 带为绝对高度：调用方可用 DomeFit 的 base.heightOf 或 RegionPlane 的
//     height 先标定基面再换算。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <string>
#include <vector>

// ---------------- 参数 ----------------
struct POINT3DALG_EXPORT Point3DAlg_CircleFitParams {
    double cluster_tolerance;   // 切片点聚类连通距离 mm（默认 0.2）
    int    min_points;          // 每簇最少点数（默认 20；Kåsa 3x3 至少 3，留余量滤噪）
    double radius_min;          // 半径合理下限 mm（默认 0.05）
    double radius_max;          // 半径合理上限 mm（默认 50）
    double mad_factor;          // 圆拟合残差修剪因子（默认 3.0）
    int    iterations;          // 修剪重拟合轮数（默认 3）
    double rms_max;             // 拟合残差 RMS 上限 mm（默认 0.02；超出判非圆）

    Point3DAlg_CircleFitParams();
};

// ---------------- 单个圆/孔位结果 ----------------
struct POINT3DALG_EXPORT Point3DAlg_CircleFitResult {
    double center[2];       // 圆心 XY（mm）
    double radius;          // 半径 mm
    double rms;             // 圆拟合残差 RMS mm（质量指标）
    double mean_z;          // 簇内点平均 z（孔位 z 参考）
    int    point_count;     // 簇内点数
    bool   valid;           // false = 半径越界/残差超限/拟合失败

    Point3DAlg_CircleFitResult();
};

/// 单簇圆拟合核心（XY 平面内；xy 为 x,y 交替 SoA，count 点）。
/// 独立暴露：调用方已有轮廓点/边缘点时直接用，不必走切片。
/// valid=false = 点数<3/退化/半径越界/rms 超限（out 仍带已算数据供诊断）。
POINT3DALG_EXPORT bool Point3DAlg_FitCircle2D(const double* xy, int count,
                                              double mad_factor, int iterations,
                                              double radius_min, double radius_max,
                                              double rms_max,
                                              Point3DAlg_CircleFitResult& out);

/// 高度切片圆/孔位检测：z ∈ [z_low, z_high] 切片 → 聚类 → 逐簇圆拟合。
/// [out] circles 按簇点数降序；valid=false 的簇同样保留（含失败原因数据）。
/// 返回：检出簇总数（含无效）；-1 参数非法；0 无切片点/无簇。
POINT3DALG_EXPORT int Point3DAlg_DetectCircles(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
    double z_low, double z_high,
    const Point3DAlg_CircleFitParams& params,
    std::vector<Point3DAlg_CircleFitResult>& circles);

#endif // POINT3DALG_CIRCLEFIT_H
