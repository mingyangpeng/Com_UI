#ifndef POINT3DALG_PROFILE_H
#define POINT3DALG_PROFILE_H

// Point3DAlg_Profile —— 轮廓/截面量测（纯函数，零 UI）。
//
// 场景：沿一条 XY 线段从点云提取 z 剖面——台阶剖面分析、焊缝/胶线轮廓、
// 边缘落差、平面度剖面。
//
// 算法：
//   1. 取到线段垂直距离 ≤ band 且投影落在线段范围内的点（无限直线不取）；
//   2. 投影到弧长 s ∈ [0, L]，按 bin_size 分格，每格 z 取中位数（抗单点噪声）；
//   3. 统计：z 范围及位置、相邻有效格最大 z 跳变（台阶/落差指标 max_step）。
//
// 输出剖面 (s, z) 按 s 升序；max_step 对噪声敏感（≈√2×点云噪声），台阶判定
// 应设工程阈值（如 > 0.1mm）而非直接取 max_step。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <vector>

// ---------------- 参数 ----------------
struct POINT3DALG_EXPORT Point3DAlg_ProfileParams {
    double band;               // 带半宽 mm：点到线段垂直距离 ≤ band 才参与（默认 0.2）
    double bin_size;           // 弧长分格 mm（默认 0.1；过小会导致空格/单点格）
    int    min_points_per_bin; // 格最少点数（默认 1；0 视为 1。调大会在数据网格与
                               // bin 网格次拍频的格上丢格——bin 边界浮点抖动
                               // 使格内点数随机 ±1，默认 1 保证剖面连续）

    Point3DAlg_ProfileParams();
};

// ---------------- 剖面统计 ----------------
struct POINT3DALG_EXPORT Point3DAlg_ProfileResult {
    long long points;      // 剖面格数（输出点数）
    long long used_points; // 带内参与点数（band 收窄则减少——带外剔除的验证口径）
    double length;         // 线段长度 mm
    double z_min, z_max;   // 剖面 z 范围
    double s_at_min;       // z_min 位置（弧长 mm）
    double s_at_max;       // z_max 位置
    double max_step;       // 相邻有效格最大 z 跳变（台阶/落差指标）
    double s_at_step;      // 跳变位置（两格中点弧长）

    Point3DAlg_ProfileResult();
};

/// 轮廓剖面提取。
/// [in]  cloud        点云（mm，z 为高度向）
/// [in]  x0,y0,x1,y1  线段两端点（XY）
/// [out] out          统计
/// [out] out_profile  可为 nullptr；否则追加剖面点列：每格 2 个 float
///                    （s=弧长 mm，z=格内 z 中位数），按 s 升序
/// 返回 false = 云空 / 线段长度 0 / 参数非法 / 带内无点。
POINT3DALG_EXPORT bool Point3DAlg_Profile(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
    double x0, double y0, double x1, double y1,
    const Point3DAlg_ProfileParams& params,
    Point3DAlg_ProfileResult& out,
    std::vector<float>* out_profile = nullptr);

#endif // POINT3DALG_PROFILE_H
