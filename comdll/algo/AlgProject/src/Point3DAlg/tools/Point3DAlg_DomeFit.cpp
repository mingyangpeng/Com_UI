// Point3DAlg_DomeFit.cpp —— 拱形球顶 bump 拟合实现（纯函数，零 UI）。
// 聚类复用 shapely::CloudProcessor::segmentEuclideanCluster；
// 基面（迭代修剪最小二乘平面）与球拟合（Kåsa 代数闭式解 + 修剪）为确定性自实现，
// 数值流程与 scripts/_wafer_fitstat.py 基准一致（已在 moi_wafer.ply 验证）。

#include "Point3DAlg_DomeFit.h"

#include "ShapelyAlg.h"   // CloudProcessor（聚类）

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;

// ---------------- 3x3 / 4x4 线性代数最小实现（避免引 Eigen 到公共头） ----------------

// 解 4x4 线性方程组 A·x = b（高斯消元 + 部分主元）。返回 false = 奇异。
bool Solve4x4(double A[4][4], double b[4], double x[4])
{
    // 增广矩阵消元
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        double best = std::fabs(A[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            const double v = std::fabs(A[r][col]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-12)
            return false;
        if (piv != col) {
            for (int c = 0; c < 4; ++c) std::swap(A[col][c], A[piv][c]);
            std::swap(b[col], b[piv]);
        }
        for (int r = col + 1; r < 4; ++r) {
            const double f = A[r][col] / A[col][col];
            if (f == 0.0) continue;
            for (int c = col; c < 4; ++c) A[r][c] -= f * A[col][c];
            b[r] -= f * b[col];
        }
    }
    // 回代
    for (int r = 3; r >= 0; --r) {
        double s = b[r];
        for (int c = r + 1; c < 4; ++c) s -= A[r][c] * x[c];
        x[r] = s / A[r][r];
        if (!std::isfinite(x[r]))
            return false;
    }
    return true;
}

// 最小二乘平面与中位数抽到 Point3DAlg_FitUtil.h（与 RegionPlane 共用）：
#include "Point3DAlg_FitUtil.h"
using point3d_fit::FitPlaneZ;
using point3d_fit::MedianOf;

// Kåsa 代数球拟合：x²+y²+z² = 2cx·x + 2cy·y + 2cz·z + k（k = R²−|c|²）。
// 返回 false = 点数 < 4 或退化。
bool FitSphereKasa(const std::vector<double>& px, const std::vector<double>& py,
                   const std::vector<double>& pz,
                   size_t begin, size_t end,
                   double& cx, double& cy, double& cz, double& R)
{
    // 中心化避免大坐标下的病态
    double mx = 0, my = 0, mz = 0;
    const size_t n = end - begin;
    if (n < 4)
        return false;
    for (size_t i = begin; i < end; ++i) {
        mx += px[i]; my += py[i]; mz += pz[i];
    }
    mx /= n; my /= n; mz /= n;
    // 法方程（关于中心化坐标）：[Σu² Σuv Σuw Σu; ... ]·[2cx',2cy',2cz',k'] = [Σu·s, ...]
    // s = u²+v²+w²；直接构建 4x4 对称矩阵
    double S[4][4] = { {0} };
    double rhs[4] = { 0 };
    for (size_t i = begin; i < end; ++i) {
        const double u = px[i] - mx, v = py[i] - my, w = pz[i] - mz;
        const double s = u * u + v * v + w * w;
        const double row[4] = { u, v, w, 1.0 };
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c)
                S[r][c] += row[r] * row[c];
            rhs[r] += row[r] * s;
        }
    }
    double sol[4];
    if (!Solve4x4(S, rhs, sol))
        return false;
    const double ccx = 0.5 * sol[0], ccy = 0.5 * sol[1], ccz = 0.5 * sol[2];
    const double k = sol[3];
    const double r2 = k + ccx * ccx + ccy * ccy + ccz * ccz;
    if (!(r2 > 0.0))
        return false;
    cx = ccx + mx; cy = ccy + my; cz = ccz + mz;
    R = std::sqrt(r2);
    return std::isfinite(cx) && std::isfinite(cy) && std::isfinite(cz) && std::isfinite(R);
}

} // namespace

// =====================================================================
// 参数 / 结构默认值
// =====================================================================

Point3DAlg_DomeFitParams::Point3DAlg_DomeFitParams()
    : height_threshold(0.02)
    , cluster_tolerance(0.12)
    , min_points(8)
    , base_mad_factor(3.0)
    , base_iterations(5)
    , sphere_mad_factor(3.0)
    , max_radius(1.0)
    , min_radius(0.01)
    , max_fit_rms(0.005)
{
}

Point3DAlg_DomeBump::Point3DAlg_DomeBump()
    : height(0.0), radius(0.0), rms(0.0), point_count(0), valid(false)
{
    std::memset(apex, 0, sizeof(apex));
    std::memset(center, 0, sizeof(center));
    std::memset(centroid, 0, sizeof(centroid));
}

Point3DAlg_DomeBase::Point3DAlg_DomeBase()
    : d(0.0), sigma(0.0), inlier_count(0)
{
    normal[0] = 0.0; normal[1] = 0.0; normal[2] = 1.0;
}

double Point3DAlg_DomeBase::HeightOf(double px, double py, double pz) const
{
    // 平面 n·p + d = 0；高度 = n·p + d（n 朝上为正）
    return normal[0] * px + normal[1] * py + normal[2] * pz + d;
}

Point3DAlg_DomeStats::Point3DAlg_DomeStats()
    : count(0), mean(0.0), stddev(0.0), min(0.0), max(0.0), p5(0.0), p50(0.0), p95(0.0),
      coplanarity(0.0), fit_sigma(0.0), fit_p(0.0), fit_q(0.0)
{
}

// =====================================================================
// 基面拟合
// =====================================================================

bool Point3DAlg_DomeFitBase(const CloudXYZ::ConstPtr& cloud,
                            const Point3DAlg_DomeFitParams& params,
                            Point3DAlg_DomeBase& out_base)
{
    if (!cloud || cloud->size() < 10)
        return false;

    const size_t n = cloud->size();
    std::vector<double> px(n), py(n), pz(n);
    for (size_t i = 0; i < n; ++i) {
        px[i] = cloud->points[i].x;
        py[i] = cloud->points[i].y;
        pz[i] = cloud->points[i].z;
    }

    // 迭代修剪（与 Python 基准 _wafer_fitstat.py 逐行等价）：
    //   每轮对内点拟合平面，但残差统计（med/MAD）与候选重建都在**全部有限点**上做，
    //   即内点集每轮从全点重算（允许之前误删的点回归）；
    //   结束后用最后一轮平面 + 最终内点的中位数归零（不做额外重拟合）。
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    double a = 0, b = 0, c = 0;
    for (int it = 0; it < std::max(1, params.base_iterations); ++it) {
        // 索引重排到紧凑数组后拟合
        std::vector<double> qx(idx.size()), qy(idx.size()), qz(idx.size());
        for (size_t k = 0; k < idx.size(); ++k) {
            qx[k] = px[idx[k]]; qy[k] = py[idx[k]]; qz[k] = pz[idx[k]];
        }
        if (!FitPlaneZ(qx, qy, qz, 0, idx.size(), a, b, c))
            return false;

        // 残差（沿 z）：全部有限点
        std::vector<double> rAll(n);
        for (size_t i = 0; i < n; ++i)
            rAll[i] = pz[i] - (a * px[i] + b * py[i] + c);
        const double med = MedianOf(rAll);
        std::vector<double> absdev(n);
        for (size_t i = 0; i < n; ++i)
            absdev[i] = std::fabs(rAll[i] - med);
        const double mad = MedianOf(absdev);

        // 修剪：只剔除上方（bump 在上）；每轮从全点重建内点集
        const double cut = med + params.base_mad_factor * mad + 1e-12;
        std::vector<size_t> keep;
        keep.reserve(idx.size());
        for (size_t i = 0; i < n; ++i)
            if (rAll[i] <= cut)
                keep.push_back(i);
        if (keep.size() < 10)
            break;   // 修剪过度：用上一轮结果
        const bool converged = keep.size() == idx.size();
        idx = keep;
        if (converged)
            break;
    }

    // 最终基面 = 最后一轮拟合的 z = a·x + b·y + c（不再重拟合，与基准一致）；
    // 内点中位归零：吸收 LSQ 平面相对内点主体的整体偏移
    {
        std::vector<double> rIn(idx.size());
        for (size_t k = 0; k < idx.size(); ++k)
            rIn[k] = pz[idx[k]] - (a * px[idx[k]] + b * py[idx[k]] + c);
        c += MedianOf(rIn);
    }

    // 法向（朝 z+ 一侧）：n = (−a, −b, 1)/‖·‖；d 使 n·p + d = 0 ⇔ z − a·x − b·y − c = 0
    const double norm = std::sqrt(a * a + b * b + 1.0);
    out_base.normal[0] = -a / norm;
    out_base.normal[1] = -b / norm;
    out_base.normal[2] = 1.0 / norm;
    out_base.d = -c / norm;

    // 基面噪声 σ（最终内点残差 RMS，沿法向）
    double s2 = 0.0;
    for (size_t k = 0; k < idx.size(); ++k) {
        const double h = out_base.HeightOf(px[idx[k]], py[idx[k]], pz[idx[k]]);
        s2 += h * h;
    }
    out_base.sigma = std::sqrt(s2 / (double)idx.size());
    out_base.inlier_count = (int)idx.size();
    return true;
}

// =====================================================================
// 主入口
// =====================================================================

bool Point3DAlg_DomeFit(const CloudXYZ::ConstPtr& cloud,
                        const Point3DAlg_DomeFitParams& params,
                        std::vector<Point3DAlg_DomeBump>& bumps,
                        Point3DAlg_DomeBase* out_base)
{
    bumps.clear();
    if (!cloud || cloud->size() < 10)
        return false;

    // ---- 1. 稳健基面 ----
    Point3DAlg_DomeBase base;
    if (!Point3DAlg_DomeFitBase(cloud, params, base))
        return false;
    if (out_base)
        *out_base = base;

    // ---- 2. 高度分离：相对基面高度 > 阈值的点 ----
    const size_t n = cloud->size();
    std::vector<int> hiIdx;
    hiIdx.reserve(n / 8);
    for (size_t i = 0; i < n; ++i) {
        const float x = cloud->points[i].x, y = cloud->points[i].y, z = cloud->points[i].z;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            continue;
        if (base.HeightOf(x, y, z) > params.height_threshold)
            hiIdx.push_back((int)i);
    }
    if (hiIdx.empty())
        return false;

    // ---- 3. 欧式聚类（复用 ShapelyAlg） ----
    CloudXYZ::Ptr hiCloud(new CloudXYZ);
    hiCloud->reserve(hiIdx.size());
    for (int i : hiIdx)
        hiCloud->push_back(cloud->points[i]);
    hiCloud->width = (uint32_t)hiCloud->size();
    hiCloud->height = 1;
    hiCloud->is_dense = true;

    std::vector<CloudXYZ::Ptr> clusters =
        shapely::CloudProcessor<pcl::PointXYZ>::segmentEuclideanCluster(
            hiCloud, params.cluster_tolerance, params.min_points, (int)n);

    // 按点数降序（大 bump 在前）
    std::sort(clusters.begin(), clusters.end(),
              [](const CloudXYZ::Ptr& A, const CloudXYZ::Ptr& B) {
                  return A->size() > B->size();
              });

    // ---- 4. 每簇球拟合 + 高度 ----
    bumps.reserve(clusters.size());
    for (const CloudXYZ::Ptr& cl : clusters) {
        Point3DAlg_DomeBump bump;
        bump.point_count = (int)cl->size();

        // 质心 + 簇内最高点沿法向高度（守卫用）
        double mx = 0, my = 0, mz = 0, peakH = -1e30;
        for (const auto& p : cl->points) {
            mx += p.x; my += p.y; mz += p.z;
            const double h = base.HeightOf(p.x, p.y, p.z);
            if (h > peakH) peakH = h;
        }
        const double cn = (double)cl->size();
        bump.centroid[0] = mx / cn; bump.centroid[1] = my / cn; bump.centroid[2] = mz / cn;

        // 坐标拷贝（球拟合按索引区间工作）
        const size_t m = cl->size();
        std::vector<double> qx(m), qy(m), qz(m);
        for (size_t i = 0; i < m; ++i) {
            qx[i] = cl->points[i].x; qy[i] = cl->points[i].y; qz[i] = cl->points[i].z;
        }

        double cx = 0, cy = 0, cz = 0, R = 0;
        bool ok = FitSphereKasa(qx, qy, qz, 0, m, cx, cy, cz, R);

        // 残差修剪重拟合一次（剔除球面远点——簇边缘可能扫到基面沟）
        if (ok) {
            std::vector<double> resid(m);
            for (size_t i = 0; i < m; ++i) {
                const double dx = qx[i] - cx, dy = qy[i] - cy, dz = qz[i] - cz;
                resid[i] = std::sqrt(dx * dx + dy * dy + dz * dz) - R;
            }
            const double med = MedianOf(resid);
            std::vector<double> absdev(m);
            for (size_t i = 0; i < m; ++i) absdev[i] = std::fabs(resid[i] - med);
            const double mad = MedianOf(absdev);
            const double cut = med + params.sphere_mad_factor * mad + 1e-12;
            std::vector<size_t> keep;
            keep.reserve(m);
            for (size_t i = 0; i < m; ++i)
                if (resid[i] <= cut) keep.push_back(i);
            if (keep.size() >= 4 && keep.size() < m) {
                std::vector<double> kx(keep.size()), ky(keep.size()), kz(keep.size());
                for (size_t k = 0; k < keep.size(); ++k) {
                    kx[k] = qx[keep[k]]; ky[k] = qy[keep[k]]; kz[k] = qz[keep[k]];
                }
                double cx2 = 0, cy2 = 0, cz2 = 0, R2 = 0;
                if (FitSphereKasa(kx, ky, kz, 0, keep.size(), cx2, cy2, cz2, R2)) {
                    cx = cx2; cy = cy2; cz = cz2; R = R2;   // 修剪后更稳
                    // 残差也按修剪后算
                    for (size_t k = 0; k < keep.size(); ++k) {
                        const double dx = kx[k] - cx, dy = ky[k] - cy, dz = kz[k] - cz;
                        resid[k] = std::sqrt(dx * dx + dy * dy + dz * dz) - R;
                    }
                    double s2 = 0.0;
                    for (size_t k = 0; k < keep.size(); ++k) s2 += resid[k] * resid[k];
                    bump.rms = std::sqrt(s2 / (double)keep.size());
                }
            } else {
                double s2 = 0.0;
                for (size_t i = 0; i < m; ++i) s2 += resid[i] * resid[i];
                bump.rms = std::sqrt(s2 / (double)m);
            }
        }

        // 半径合理性与有限性校验（NaN 拦截——项目铁律）
        if (!ok || !std::isfinite(R) || R < params.min_radius || R > params.max_radius) {
            bump.valid = false;
            bumps.push_back(bump);
            continue;
        }

        // apex = c + R·n（n 朝上）；高度 = apex 相对基面
        bump.center[0] = cx; bump.center[1] = cy; bump.center[2] = cz;
        bump.radius = R;
        bump.apex[0] = cx + R * base.normal[0];
        bump.apex[1] = cy + R * base.normal[1];
        bump.apex[2] = cz + R * base.normal[2];
        bump.height = base.HeightOf(bump.apex[0], bump.apex[1], bump.apex[2]);
        if (!std::isfinite(bump.height) || !std::isfinite(bump.apex[2])) {
            bump.valid = false;
            bumps.push_back(bump);
            continue;
        }
        // rms 若未在修剪分支算过（修剪未触发），在此兜底
        if (bump.rms <= 0.0) {
            double s2 = 0.0;
            for (size_t i = 0; i < m; ++i) {
                const double dx = qx[i] - cx, dy = qy[i] - cy, dz = qz[i] - cz;
                const double dr = std::sqrt(dx * dx + dy * dy + dz * dz) - R;
                s2 += dr * dr;
            }
            bump.rms = std::sqrt(s2 / (double)m);
        }

        // ---- 几何合理性守卫（拦截非球冠形貌的簇，如缺陷/毛刺/桥连）----
        // 1) 顶点悬空上限：apex 沿法向高度 − 簇内最高采样点 ≤ cluster_tolerance。
        //    正常球冠 apex 比最高采样点高 ≤ 50µm（实测 p99=3.7µm）；非球冠簇（缺陷
        //    桥连等）Kåsa 外推出的 apex 可悬空数百 µm——区分度 15 倍，零误杀。
        //    （注意不要用 h ≤ 2R 作守卫：拟合 R 本身有 ±30% 波动，h/R 拟合值
        //    越 2 很常见，实测会误杀 20+ 个正常 bump。）
        // 2) 拟合残差上限：rms > max_fit_rms 判非球冠/劣质拟合
        //    （正常 bump rms ≤ 6.7µm、p99=3.7µm；劣质拟合 ≥ 9µm）。
        bool guarded = bump.height - peakH <= params.cluster_tolerance &&
                       bump.rms <= params.max_fit_rms;
        if (!guarded) {
            bump.valid = false;
            bumps.push_back(bump);
            continue;
        }
        bump.valid = true;
        bumps.push_back(bump);
    }

    // 有效 bump 至少 1 个才算成功
    size_t nValid = 0;
    for (const auto& b : bumps)
        if (b.valid) ++nValid;
    return nValid > 0;
}

// =====================================================================
// bump 对比（金标准 NG 判定）
// =====================================================================

Point3DAlg_BumpCompareParams::Point3DAlg_BumpCompareParams()
    : match_radius(0.3)
    , ng_threshold(0.02)
{
}

Point3DAlg_BumpCompareResult::Point3DAlg_BumpCompareResult()
    : matched(0), unmatched_ref(0), unmatched_test(0), ng_count(0),
      ng_ratio(0.0), dh_mean(0.0), dh_rms(0.0), dh_max_abs(0.0)
{
}

bool Point3DAlg_BumpCompare(const std::vector<Point3DAlg_DomeBump>& ref,
                            const std::vector<Point3DAlg_DomeBump>& test,
                            const Point3DAlg_BumpCompareParams& params,
                            Point3DAlg_BumpCompareResult& out)
{
    out = Point3DAlg_BumpCompareResult();
    const double R2 = params.match_radius * params.match_radius;

    // valid 子集
    std::vector<const Point3DAlg_DomeBump*> rv, tv;
    for (const auto& b : ref)  if (b.valid) rv.push_back(&b);
    for (const auto& b : test) if (b.valid) tv.push_back(&b);
    if (rv.empty() && tv.empty())
        return false;

    // ref 建 XY 网格哈希（cell = match_radius），test 逐个找 9 邻格最近 ref
    struct Cand { double d2; int ti, ri; };
    std::vector<Cand> cands;
    if (!rv.empty() && !tv.empty()) {
        std::unordered_map<uint64_t, std::vector<int>> grid;
        auto key2 = [](int32_t ix, int32_t iy) {
            return ((uint64_t)(uint32_t)ix << 32) | (uint32_t)iy;
        };
        const double inv = 1.0 / std::max(params.match_radius, 1e-9);
        for (int ri = 0; ri < (int)rv.size(); ++ri) {
            const int32_t ix = (int32_t)std::floor(rv[(size_t)ri]->apex[0] * inv);
            const int32_t iy = (int32_t)std::floor(rv[(size_t)ri]->apex[1] * inv);
            grid[key2(ix, iy)].push_back(ri);
        }
        for (int ti = 0; ti < (int)tv.size(); ++ti) {
            const int32_t cx = (int32_t)std::floor(tv[(size_t)ti]->apex[0] * inv);
            const int32_t cy = (int32_t)std::floor(tv[(size_t)ti]->apex[1] * inv);
            int bestRi = -1;
            double bestD2 = R2;
            for (int du = -1; du <= 1; ++du)
                for (int dv = -1; dv <= 1; ++dv) {
                    const auto it = grid.find(key2(cx + du, cy + dv));
                    if (it == grid.end()) continue;
                    for (int ri : it->second) {
                        const double ddx = tv[(size_t)ti]->apex[0] - rv[(size_t)ri]->apex[0];
                        const double ddy = tv[(size_t)ti]->apex[1] - rv[(size_t)ri]->apex[1];
                        const double d2 = ddx * ddx + ddy * ddy;
                        if (d2 <= bestD2) { bestD2 = d2; bestRi = ri; }
                    }
                }
            if (bestRi >= 0)
                cands.push_back({ bestD2, ti, bestRi });
        }
        // 贪心一对一：按距离升序，冲突取更近者
        std::sort(cands.begin(), cands.end(),
                  [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        std::vector<char> usedT(tv.size(), 0), usedR(rv.size(), 0);
        for (const Cand& c : cands) {
            if (usedT[(size_t)c.ti] || usedR[(size_t)c.ri])
                continue;
            usedT[(size_t)c.ti] = 1;
            usedR[(size_t)c.ri] = 1;
            const double dh = tv[(size_t)c.ti]->height - rv[(size_t)c.ri]->height;
            out.diff_xy.push_back(rv[(size_t)c.ri]->apex[0]);
            out.diff_xy.push_back(rv[(size_t)c.ri]->apex[1]);
            out.diff_h.push_back(dh);
            out.diff_ng.push_back(std::fabs(dh) > params.ng_threshold ? 1 : 0);
        }
    }

    out.matched = (int)out.diff_h.size();
    out.unmatched_ref = (int)rv.size() - out.matched;
    out.unmatched_test = (int)tv.size() - out.matched;
    out.ng_count = (int)std::count(out.diff_ng.begin(), out.diff_ng.end(), 1);
    out.ng_ratio = out.matched > 0 ? (double)out.ng_count / (double)out.matched : 0.0;

    double s = 0.0, s2 = 0.0, mx = 0.0;
    for (double d : out.diff_h) {
        s += d;
        s2 += d * d;
        const double ad = std::fabs(d);
        if (ad > mx) mx = ad;
    }
    if (out.matched > 0) {
        out.dh_mean = s / (double)out.matched;
        out.dh_rms = std::sqrt(s2 / (double)out.matched);
        out.dh_max_abs = mx;
    }
    return true;
}

// =====================================================================
// 统计
// =====================================================================

Point3DAlg_DomeStats Point3DAlg_DomeStatsOf(const std::vector<Point3DAlg_DomeBump>& bumps)
{
    Point3DAlg_DomeStats st;
    std::vector<double> hs;
    hs.reserve(bumps.size());
    for (const auto& b : bumps)
        if (b.valid)
            hs.push_back(b.height);
    st.count = (int)hs.size();
    if (hs.empty())
        return st;
    double s = 0.0;
    for (double h : hs) s += h;
    st.mean = s / (double)hs.size();
    double s2 = 0.0;
    for (double h : hs) s2 += (h - st.mean) * (h - st.mean);
    st.stddev = hs.size() > 1 ? std::sqrt(s2 / (double)(hs.size() - 1)) : 0.0;
    st.min = *std::min_element(hs.begin(), hs.end());
    st.max = *std::max_element(hs.begin(), hs.end());
    st.coplanarity = st.max - st.min;   // 极差法共面性
    std::sort(hs.begin(), hs.end());
    auto pct = [&](double p) {
        const double pos = p / 100.0 * (double)(hs.size() - 1);
        const size_t lo = (size_t)pos;
        const size_t hi = std::min(lo + 1, hs.size() - 1);
        const double f = pos - (double)lo;
        return hs[lo] * (1.0 - f) + hs[hi] * f;
    };
    st.p5 = pct(5.0);
    st.p50 = pct(50.0);
    st.p95 = pct(95.0);

    // ---- 最佳拟合面残差 σ（剥离晶圆整体倾斜/翘曲后的共面性散差）----
    // 对 (x, y, height) 拟合平面，MAD 修剪离群 bump（与量测噪声不同源：
    // 个别坏拟合 bump 不应污染工艺共面性），残差 RMS。
    if (hs.size() >= 4) {
        std::vector<double> bx, by, bh;
        bx.reserve(hs.size());
        by.reserve(hs.size());
        bh.reserve(hs.size());
        for (const auto& b : bumps) {
            if (!b.valid) continue;
            bx.push_back(b.apex[0]);
            by.push_back(b.apex[1]);
            bh.push_back(b.height);
        }
        std::vector<size_t> idx(bx.size());
        for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
        double p = 0, q = 0, r0 = 0;
        for (int it = 0; it < 3; ++it) {
            std::vector<double> qx(idx.size()), qy(idx.size()), qz(idx.size());
            for (size_t k = 0; k < idx.size(); ++k) {
                qx[k] = bx[idx[k]]; qy[k] = by[idx[k]]; qz[k] = bh[idx[k]];
            }
            if (!point3d_fit::FitPlaneZ(qx, qy, qz, 0, idx.size(), p, q, r0))
                break;
            // 全量残差 → MAD 修剪（双侧对称：离群 bump 可高可低）
            std::vector<double> rAll(bx.size());
            for (size_t i = 0; i < bx.size(); ++i)
                rAll[i] = bh[i] - (p * bx[i] + q * by[i] + r0);
            const double med = point3d_fit::MedianOf(rAll);
            std::vector<double> ad(bx.size());
            for (size_t i = 0; i < bx.size(); ++i)
                ad[i] = std::fabs(rAll[i] - med);
            const double mad = point3d_fit::MedianOf(ad);
            const double hi = med + 3.0 * mad + 1e-12;
            const double lo = med - 3.0 * mad - 1e-12;
            std::vector<size_t> keep;
            keep.reserve(idx.size());
            for (size_t i = 0; i < bx.size(); ++i)
                if (rAll[i] >= lo && rAll[i] <= hi)
                    keep.push_back(i);
            if (keep.size() < 4)
                break;
            const bool converged = keep.size() == idx.size();
            idx = keep;
            if (converged)
                break;
        }
        st.fit_p = p;
        st.fit_q = q;
        double s3 = 0.0;
        for (size_t k = 0; k < idx.size(); ++k) {
            const double rr = bh[idx[k]] - (p * bx[idx[k]] + q * by[idx[k]] + r0);
            s3 += rr * rr;
        }
        st.fit_sigma = std::sqrt(s3 / (double)idx.size());
    }
    return st;
}
