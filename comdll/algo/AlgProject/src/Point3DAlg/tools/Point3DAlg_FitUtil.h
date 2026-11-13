#ifndef POINT3DALG_FITUTIL_H
#define POINT3DALG_FITUTIL_H

// Point3DAlg_FitUtil —— Point3DAlg 量测类算法共用的数值内核（header-only inline）。
// 目前承载：
//   MedianOf    —— 中位数（偶数取中间两值均值，与 numpy 语义一致）
//   FitPlaneZ   —— 最小二乘平面 z = a·x + b·y + c（3x3 法方程，中心化提数值稳定性）
// 使用方：Point3DAlg_DomeFit / Point3DAlg_RegionPlane。
// 纯 inline、无导出符号：各 TU 自带副本，不进 DLL 导出表（与项目"公共头不暴露
// Eigen/重型依赖"的约定一致——本头仅依赖 STL）。

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace point3d_fit {

// 中位数（会重排 v）
inline double MedianOf(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + (long)m, v.end());
    const double hi = v[m];
    if (v.size() % 2 == 1)
        return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + (long)m);
    return 0.5 * (lo + hi);
}

// 最小二乘平面：z = a·x + b·y + c（对 [begin,end) 区间的点）。
// 返回 false = 点数 < 3 或法方程退化（点共线/坐标全相同）。
inline bool FitPlaneZ(const std::vector<double>& px, const std::vector<double>& py,
                      const std::vector<double>& pz, size_t begin, size_t end,
                      double& a, double& b, double& c)
{
    const size_t n = end - begin;
    if (n < 3)
        return false;
    double sx = 0, sy = 0, sz = 0;
    for (size_t i = begin; i < end; ++i) {
        sx += px[i]; sy += py[i]; sz += pz[i];
    }
    const double mx = sx / (double)n, my = sy / (double)n, mz = sz / (double)n;
    // 中心化法方程 2x2：[Sxx Sxy; Sxy Syy]·[a,b] = [Sxz, Syz]
    double sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
    for (size_t i = begin; i < end; ++i) {
        const double dx = px[i] - mx, dy = py[i] - my, dz = pz[i] - mz;
        sxx += dx * dx; sxy += dx * dy; syy += dy * dy;
        sxz += dx * dz; syz += dy * dz;
    }
    const double det = sxx * syy - sxy * sxy;
    if (std::fabs(det) < 1e-15)
        return false;
    a = (sxz * syy - syz * sxy) / det;
    b = (syz * sxx - sxz * sxy) / det;
    c = mz - a * mx - b * my;
    return std::isfinite(a) && std::isfinite(b) && std::isfinite(c);
}

} // namespace point3d_fit

#endif // POINT3DALG_FITUTIL_H
