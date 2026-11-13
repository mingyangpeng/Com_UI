// Point3DAlg_CircleFit.cpp —— 高度切片圆/孔位量测实现（纯函数，零 UI）。
// 切片聚类复用 shapely::CloudProcessor::segmentEuclideanCluster；
// 圆拟合为 Kåsa 代数法（中心化 3x3 法方程，确定性），残差双侧 MAD 修剪重拟合
// ——与 DomeFit 球拟合同构（球是 4 参数，圆是 3 参数），修剪内核共用 FitUtil。

#include "Point3DAlg_CircleFit.h"

#include "Point3DAlg_FitUtil.h"
#include "ShapelyAlg.h"   // CloudProcessor（聚类）

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using point3d_fit::MedianOf;

namespace {

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;

// 中心化 Kåsa 圆拟合（对 [begin,end) 的 xy 区间）。false = 点数 < 3 / 退化 / 非有限。
bool KasaCircle(const std::vector<double>& xs, const std::vector<double>& ys,
                size_t begin, size_t end, double& cx, double& cy, double& R)
{
    const size_t n = end - begin;
    if (n < 3)
        return false;
    double mx = 0, my = 0;
    for (size_t i = begin; i < end; ++i) {
        mx += xs[i];
        my += ys[i];
    }
    mx /= (double)n;
    my /= (double)n;
    // 中心化法方程 3x3：[Σu² Σuv Σu; Σuv Σv² Σv; Σu Σv n]·[2a,2b,k'] = [Σu·s, Σv·s, Σs]
    double S[3][3] = { { 0 } };
    double rhs[3] = { 0 };
    for (size_t i = begin; i < end; ++i) {
        const double u = xs[i] - mx, v = ys[i] - my;
        const double s = u * u + v * v;
        const double row[3] = { u, v, 1.0 };
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c)
                S[r][c] += row[r] * row[c];
            rhs[r] += row[r] * s;
        }
    }
    // 高斯消元（3x3，部分主元）
    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r)
            if (std::fabs(S[r][col]) > std::fabs(S[piv][col]))
                piv = r;
        if (std::fabs(S[piv][col]) < 1e-15)
            return false;
        if (piv != col) {
            for (int c = 0; c < 3; ++c) std::swap(S[col][c], S[piv][c]);
            std::swap(rhs[col], rhs[piv]);
        }
        for (int r = col + 1; r < 3; ++r) {
            const double f = S[r][col] / S[col][col];
            if (f == 0.0) continue;
            for (int c = col; c < 3; ++c) S[r][c] -= f * S[col][c];
            rhs[r] -= f * rhs[col];
        }
    }
    double sol[3] = { 0 };
    for (int r = 2; r >= 0; --r) {
        double t = rhs[r];
        for (int c = r + 1; c < 3; ++c) t -= S[r][c] * sol[c];
        sol[r] = t / S[r][r];
        if (!std::isfinite(sol[r]))
            return false;
    }
    const double ccx = 0.5 * sol[0], ccy = 0.5 * sol[1];
    const double r2 = sol[2] + ccx * ccx + ccy * ccy;
    if (!(r2 > 0.0))
        return false;
    cx = ccx + mx;
    cy = ccy + my;
    R = std::sqrt(r2);
    return std::isfinite(cx) && std::isfinite(cy) && std::isfinite(R);
}

} // namespace

Point3DAlg_CircleFitParams::Point3DAlg_CircleFitParams()
    : cluster_tolerance(0.2)
    , min_points(20)
    , radius_min(0.05)
    , radius_max(50.0)
    , mad_factor(3.0)
    , iterations(3)
    , rms_max(0.02)
{
}

Point3DAlg_CircleFitResult::Point3DAlg_CircleFitResult()
    : radius(0.0), rms(0.0), mean_z(0.0), point_count(0), valid(false)
{
    center[0] = 0.0;
    center[1] = 0.0;
}

bool Point3DAlg_FitCircle2D(const double* xy, int count,
                            double mad_factor, int iterations,
                            double radius_min, double radius_max, double rms_max,
                            Point3DAlg_CircleFitResult& out)
{
    out = Point3DAlg_CircleFitResult();
    if (!xy || count < 3)
        return false;
    std::vector<double> xs((size_t)count), ys((size_t)count);
    for (int i = 0; i < count; ++i) {
        xs[(size_t)i] = xy[(size_t)i * 2];
        ys[(size_t)i] = xy[(size_t)i * 2 + 1];
    }
    std::vector<size_t> idx((size_t)count);
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;

    double cx = 0, cy = 0, R = 0;
    for (int it = 0; it < (iterations > 0 ? iterations : 1); ++it) {
        if (!KasaCircle(xs, ys, 0, idx.size(), cx, cy, R))
            return false;
        // 残差双侧 MAD 修剪（离群点/弧上杂物）
        std::vector<double> resid(idx.size());
        for (size_t k = 0; k < idx.size(); ++k) {
            const double dx = xs[idx[k]] - cx, dy = ys[idx[k]] - cy;
            resid[k] = std::sqrt(dx * dx + dy * dy) - R;
        }
        const double med = MedianOf(resid);
        std::vector<double> ad(idx.size());
        for (size_t k = 0; k < idx.size(); ++k) ad[k] = std::fabs(resid[k] - med);
        const double mad = MedianOf(ad);
        const double hi = med + mad_factor * mad + 1e-12;
        const double lo = med - mad_factor * mad - 1e-12;
        std::vector<size_t> keep;
        keep.reserve(idx.size());
        for (size_t k = 0; k < idx.size(); ++k)
            if (resid[k] >= lo && resid[k] <= hi)
                keep.push_back(idx[k]);
        if (keep.size() < 3)
            break;
        const bool converged = keep.size() == idx.size();
        idx = keep;
        if (converged)
            break;
    }
    // 最终拟合（与 DomeFitBase 一致：循环内最后一次拟合后内点可能有更新，重拟合一次）
    if (!KasaCircle(xs, ys, 0, idx.size(), cx, cy, R))
        return false;

    if (!(R >= radius_min && R <= radius_max))
        return false;

    double s2 = 0.0;
    for (size_t k = 0; k < idx.size(); ++k) {
        const double dx = xs[idx[k]] - cx, dy = ys[idx[k]] - cy;
        const double dr = std::sqrt(dx * dx + dy * dy) - R;
        s2 += dr * dr;
    }
    out.center[0] = cx;
    out.center[1] = cy;
    out.radius = R;
    out.rms = std::sqrt(s2 / (double)idx.size());
    out.point_count = (int)idx.size();
    out.valid = std::isfinite(out.rms) && out.rms <= rms_max;
    return out.valid;
}

int Point3DAlg_DetectCircles(const CloudXYZ::ConstPtr& cloud,
                             double z_low, double z_high,
                             const Point3DAlg_CircleFitParams& params,
                             std::vector<Point3DAlg_CircleFitResult>& circles)
{
    circles.clear();
    if (!cloud || cloud->empty() || !(z_low <= z_high) || !(params.cluster_tolerance > 0.0))
        return -1;

    // ---- 1. 高度切片 ----
    CloudXYZ::Ptr slice(new CloudXYZ);
    slice->reserve(cloud->size() / 8);
    for (const auto& p : cloud->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        if (p.z >= z_low && p.z <= z_high)
            slice->push_back(p);
    }
    if (slice->empty())
        return 0;
    slice->width = (uint32_t)slice->size();
    slice->height = 1;
    slice->is_dense = true;

    // ---- 2. 聚类分离各孔壁 ----
    std::vector<CloudXYZ::Ptr> clusters =
        shapely::CloudProcessor<pcl::PointXYZ>::segmentEuclideanCluster(
            slice, params.cluster_tolerance, params.min_points, (int)slice->size());
    std::sort(clusters.begin(), clusters.end(),
              [](const CloudXYZ::Ptr& A, const CloudXYZ::Ptr& B) {
                  return A->size() > B->size();
              });

    // ---- 3. 逐簇圆拟合 ----
    circles.reserve(clusters.size());
    for (const CloudXYZ::Ptr& cl : clusters) {
        Point3DAlg_CircleFitResult r;
        r.point_count = (int)cl->size();
        double mz = 0.0;
        std::vector<double> xy;
        xy.reserve(cl->size() * 2);
        for (const auto& p : cl->points) {
            xy.push_back(p.x);
            xy.push_back(p.y);
            mz += p.z;
        }
        r.mean_z = mz / (double)cl->size();

        const bool ok = Point3DAlg_FitCircle2D(xy.data(), (int)cl->size(),
                                               params.mad_factor, params.iterations,
                                               params.radius_min, params.radius_max,
                                               params.rms_max, r);
        // FitCircle2D 内部重置 out（含 point_count/mean_z）——簇级元数据回填
        r.point_count = (int)cl->size();
        r.mean_z = mz / (double)cl->size();
        if (!ok)
            r.valid = false;
        circles.push_back(r);
    }
    return (int)circles.size();
}
