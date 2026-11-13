#ifndef SHAPELY_FIT_UTIL_H
#define SHAPELY_FIT_UTIL_H

// FitUtil.h —— 拟合算法内部共享核心（**非对外接口**，业务层勿直接 include）。
// 2D 圆几何最小二乘（Kasa 代数初值 + Gauss-Newton 精化）：
//   ImageProcessor::fitCircle（2D 点集）与 CloudProcessor::fitCircle3D
//   （平面投影后的 2D 坐标）共用同一实现，避免两份拷贝漂移。
// 纯标准库，零第三方依赖；header-only inline。

#include "Types.h"

#include <cmath>
#include <vector>

namespace shapely {
namespace detail {

// 3x3 线性方程组（部分主元高斯消元）。奇异/非有限返回 false。
inline bool solve3x3(double m[3][3], const double rhs[3], double out[3])
{
    double a[3][4];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            a[r][c] = m[r][c];
        a[r][3] = rhs[r];
        if (!std::isfinite(a[r][3]))
            return false;
    }
    double scale = 0.0;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            scale = (std::max)(scale, std::fabs(a[r][c]));
    if (scale <= 0.0)
        return false;
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col]))
                piv = r;
        if (std::fabs(a[piv][col]) < 1e-12 * scale)
            return false;   // 奇异
        if (piv != col)
            for (int c = col; c < 4; ++c)
                std::swap(a[col][c], a[piv][c]);
        for (int r = col + 1; r < 3; ++r) {
            const double f = a[r][col] / a[col][col];
            for (int c = col; c < 4; ++c)
                a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 2; r >= 0; --r) {
        double s = a[r][3];
        for (int c = r + 1; c < 3; ++c)
            s -= a[r][c] * out[c];
        out[r] = s / a[r][r];
        if (!std::isfinite(out[r]))
            return false;
    }
    return true;
}

// 2D 圆几何最小二乘：Kasa 代数拟合（x²+y² = 2a·x + 2b·y + c 正规方程）初值
// + Gauss-Newton 精化（残差 = 点到圆周距离 − r）。
// [in] xy  扁平点序 [x0,y0, x1,y1, ...]（与坐标同单位）
// [out] cx/cy/r  圆心与半径；rms = 点到圆周残差均方根
// 返回：false = 点数 < 3 / 非有限 / 共线退化（半径远超点云跨度）。
inline bool fitCircle2D(const std::vector<double>& xy,
                        double& cx, double& cy, double& r, double& rms)
{
    const size_t n = xy.size() / 2;
    if (n < 3)
        return false;

    // ---- Kasa 正规方程 ----
    double Sxx = 0, Sxy = 0, Syy = 0, Sx = 0, Sy = 0, Sz = 0, Sxz = 0, Syz = 0;
    for (size_t i = 0; i < n; ++i) {
        const double x = xy[i * 2], y = xy[i * 2 + 1];
        if (!std::isfinite(x) || !std::isfinite(y))
            return false;
        const double z = x * x + y * y;
        Sxx += x * x; Sxy += x * y; Syy += y * y;
        Sx += x;      Sy += y;      Sz += z;
        Sxz += x * z; Syz += y * z;
    }
    double m[3][3] = { { Sxx, Sxy, Sx }, { Sxy, Syy, Sy }, { Sx, Sy, (double)n } };
    const double rhs[3] = { Sxz, Syz, Sz };
    double sol[3] = { 0, 0, 0 };
    if (!solve3x3(m, rhs, sol))
        return false;
    double a = 0.5 * sol[0], b = 0.5 * sol[1];
    double radius = std::sqrt((std::max)(0.0, sol[2] + a * a + b * b));

    // 点云跨度（共线/发散守卫）
    double span = 0.0;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j) {
            const double dx = xy[i * 2] - xy[j * 2];
            const double dy = xy[i * 2 + 1] - xy[j * 2 + 1];
            span = (std::max)(span, std::sqrt(dx * dx + dy * dy));
        }

    // ---- Gauss-Newton 精化（30 轮，J = [-dx/d, -dy/d, -1]） ----
    for (int it = 0; it < 30; ++it) {
        double J[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
        double g[3] = { 0, 0, 0 };
        for (size_t i = 0; i < n; ++i) {
            const double dx = xy[i * 2] - a, dy = xy[i * 2 + 1] - b;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-12)
                continue;
            const double res = d - radius;
            const double jx = -dx / d, jy = -dy / d, jr = -1.0;
            J[0][0] += jx * jx; J[0][1] += jx * jy; J[0][2] += jx * jr;
            J[1][1] += jy * jy; J[1][2] += jy * jr;
            J[2][2] += jr * jr;
            g[0] += jx * res; g[1] += jy * res; g[2] += jr * res;
        }
        J[1][0] = J[0][1];
        J[2][0] = J[0][2];
        J[2][1] = J[1][2];
        for (int k = 0; k < 3; ++k)
            J[k][k] *= 1.0001;   // 轻微阻尼改善部分弧段病态
        double step[3] = { 0, 0, 0 };
        if (!solve3x3(J, g, step))
            break;   // 退化 → 停在当前解，由末尾守卫判定
        a -= step[0];
        b -= step[1];
        radius -= step[2];
        if (!std::isfinite(a) || !std::isfinite(b) || !std::isfinite(radius))
            return false;
        if (std::fabs(step[0]) + std::fabs(step[1]) + std::fabs(step[2]) < 1e-12)
            break;
    }

    double ss = 0;
    for (size_t i = 0; i < n; ++i) {
        const double dx = xy[i * 2] - a, dy = xy[i * 2 + 1] - b;
        const double d = std::sqrt(dx * dx + dy * dy);
        ss += (d - radius) * (d - radius);
    }
    cx = a;
    cy = b;
    r = radius;
    rms = std::sqrt(ss / (double)n);
    if (!std::isfinite(r) || !std::isfinite(rms) || r > span * 1e6)
        return false;
    return true;
}

} // namespace detail
} // namespace shapely

#endif // SHAPELY_FIT_UTIL_H
