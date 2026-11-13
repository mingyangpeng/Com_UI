// Point3DAlg_RegionPlane.cpp —— XY 矩形 ROI 平面量测实现（纯函数，零 UI）。
// 迭代修剪最小二乘与 DomeFitBase 同构（双侧对称版）；平面内核用共享的
// Point3DAlg_FitUtil.h（FitPlaneZ / MedianOf，与 DomeFit 共用同一份实现）。

#include "Point3DAlg_RegionPlane.h"

#include "Point3DAlg_FitUtil.h"

#include <cmath>
#include <vector>

using point3d_fit::FitPlaneZ;
using point3d_fit::MedianOf;

Point3DAlg_RegionPlaneParams::Point3DAlg_RegionPlaneParams()
    : mad_factor(3.0)
    , iterations(5)
{
}

Point3DAlg_RegionPlaneResult::Point3DAlg_RegionPlaneResult()
    : a(0.0), b(0.0), c(0.0), height(0.0), pv(0.0), sigma(0.0),
      roi_count(0), fit_count(0), resid_max(0.0), resid_min(0.0)
{
}

bool Point3DAlg_RegionPlaneFit(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
                               double roi_x, double roi_y, double roi_w, double roi_h,
                               const Point3DAlg_RegionPlaneParams& params,
                               Point3DAlg_RegionPlaneResult& out)
{
    out = Point3DAlg_RegionPlaneResult();
    if (!cloud || cloud->empty())
        return false;
    if (!(roi_w > 0.0) || !(roi_h > 0.0))
        return false;

    // ---- 收集 ROI 内点（半开区间），同时剔除非有限点 ----
    const double x1 = roi_x + roi_w, y1 = roi_y + roi_h;
    std::vector<double> px, py, pz;
    px.reserve(cloud->size() / 4);
    py.reserve(px.capacity());
    pz.reserve(px.capacity());
    for (const auto& p : cloud->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        if (p.x >= roi_x && p.x < x1 && p.y >= roi_y && p.y < y1) {
            px.push_back(p.x);
            py.push_back(p.y);
            pz.push_back(p.z);
        }
    }
    out.roi_count = (int)px.size();
    if (px.size() < 3)
        return false;

    // ---- 迭代修剪最小二乘（双侧对称剔除）----
    // 内点集每轮从 ROI 全点重建；循环结构与 DomeFitBase 一致：
    // 每轮对内点拟合 → 全点残差统计 → 重建内点。
    std::vector<size_t> idx(px.size());
    for (size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    double a = 0, b = 0, c = 0;
    const int iters = params.iterations > 0 ? params.iterations : 1;
    for (int it = 0; it < iters; ++it) {
        if (!FitPlaneZ(px, py, pz, 0, idx.size(), a, b, c))
            return false;
        // 残差：ROI 全点（允许误删点回归）
        std::vector<double> rAll(px.size());
        for (size_t i = 0; i < px.size(); ++i)
            rAll[i] = pz[i] - (a * px[i] + b * py[i] + c);
        const double med = MedianOf(rAll);
        std::vector<double> absdev(px.size());
        for (size_t i = 0; i < px.size(); ++i)
            absdev[i] = std::fabs(rAll[i] - med);
        const double mad = MedianOf(absdev);
        const double hi = med + params.mad_factor * mad + 1e-12;
        const double lo = med - params.mad_factor * mad - 1e-12;

        std::vector<size_t> keep;
        keep.reserve(idx.size());
        for (size_t i = 0; i < px.size(); ++i)
            if (rAll[i] >= lo && rAll[i] <= hi)
                keep.push_back(i);
        if (keep.size() < 3)
            break;   // 修剪过度：沿用上一轮内点
        const bool converged = keep.size() == idx.size();
        idx = keep;
        if (converged)
            break;
    }
    if (!FitPlaneZ(px, py, pz, 0, idx.size(), a, b, c))
        return false;

    // ---- 量测统计（最终内点相对最终平面）----
    out.a = a;
    out.b = b;
    out.c = c;
    const double cx = roi_x + roi_w * 0.5, cy = roi_y + roi_h * 0.5;
    out.height = a * cx + b * cy + c;

    std::vector<double> rIn(idx.size());
    double s2 = 0.0, rmax = -1e300, rmin = 1e300;
    for (size_t k = 0; k < idx.size(); ++k) {
        const double r = pz[idx[k]] - (a * px[idx[k]] + b * py[idx[k]] + c);
        rIn[k] = r;
        s2 += r * r;
        if (r > rmax) rmax = r;
        if (r < rmin) rmin = r;
    }
    out.resid_max = rmax;
    out.resid_min = rmin;
    out.pv = rmax - rmin;
    const double mean = s2 / (double)rIn.size();   // 一轮内已中位修剪，均值≈0，直接用平方均值
    out.sigma = std::sqrt(mean);
    out.fit_count = (int)idx.size();
    return true;
}
