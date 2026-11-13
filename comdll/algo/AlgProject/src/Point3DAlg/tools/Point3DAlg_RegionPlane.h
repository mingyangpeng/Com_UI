#ifndef POINT3DALG_REGIONPLANE_H
#define POINT3DALG_REGIONPLANE_H

// Point3DAlg_RegionPlane —— XY 矩形 ROI 平面量测（纯函数，零 UI）。
//
// 场景：对点云的指定矩形区域拟合平面，量测高度与平面度——AOI 的基础量测：
//   基面标定 / 台阶高度差（两个 ROI 各拟合一次相减）/ 工件平面检测。
//
// 算法：ROI 内点迭代修剪最小二乘（双侧剔除 |残差−中位数| > factor·MAD 的点，
// 每轮内点从 ROI 全点重建，允许误删点回归；与 DomeFitBase 同构，但双侧对称——
// 独立平面的噪声对称，且台阶/异物可能出现在任一侧）。
// 稳健边界：minority 污染 ≤20% 可靠剔除（≈30% 为 MAD 类方法公认极限；
// 实测 40% 双峰污染时首轮 MAD≈半峰间距，剔除失效）——ROI 应以主平面为主。
//
// 输出：
//   height —— 平面在 ROI 中心 (cx,cy) 处的 z 高度（mm；z 即扫描高度向）
//   pv     —— 平面度 PV = 内点残差 max − min（mm）
//   sigma  —— 内点残差标准差（mm）
//   a,b,c  —— 平面系数 z = a·x + b·y + c（调用方可做台阶差：两 ROI 的 height 相减）
//
// 注：ROI 内应只有一个主平面（跨台阶的 ROI 会折中拟合，无唯一解）；点数
// < 3 或退化（共线）返回 false。头文件不依赖 PCL 以外的重型组件。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

// ---------------- 拟合参数 ----------------
struct POINT3DALG_EXPORT Point3DAlg_RegionPlaneParams {
    double mad_factor;    // 修剪因子：剔除 |残差−中位数| > factor·MAD（默认 3.0）
    int    iterations;    // 迭代修剪轮数（默认 5）

    Point3DAlg_RegionPlaneParams();
};

// ---------------- 量测结果 ----------------
struct POINT3DALG_EXPORT Point3DAlg_RegionPlaneResult {
    double a, b, c;       // 平面 z = a·x + b·y + c
    double height;        // 平面在 ROI 中心处的 z 高度
    double pv;            // 平面度 PV（内点残差极差）
    double sigma;         // 内点残差标准差
    int    roi_count;     // ROI 内总点数
    int    fit_count;     // 参与最终拟合的内点数
    double resid_max;     // 内点最大正残差（PV 分量）
    double resid_min;     // 内点最大负残差

    Point3DAlg_RegionPlaneResult();
};

/// ROI 平面拟合量测。
/// [in] cloud       点云（mm；z 为高度向）
/// [in] roi_x/y/w/h XY 矩形（半开区间 [x, x+w) × [y, y+h)）
/// 返回 false = 点云空 / ROI 内点 < 3 / 平面退化（ROI 内点共线）。
POINT3DALG_EXPORT bool Point3DAlg_RegionPlaneFit(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
    double roi_x, double roi_y, double roi_w, double roi_h,
    const Point3DAlg_RegionPlaneParams& params,
    Point3DAlg_RegionPlaneResult& out);

#endif // POINT3DALG_REGIONPLANE_H
