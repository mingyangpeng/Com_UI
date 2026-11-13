// Point3DAlg_BumpReport.cpp —— bump 全参数量测报表实现（纯函数，零 UI）。
// 高度走 Point3DAlg_DomeFit，直径走 Point3DAlg_FitCircle2D（半高切片），
// 共面性偏差与统计为本地实现；数值内核共用 Point3DAlg_FitUtil.h。

#include "Point3DAlg_BumpReport.h"

#include "Point3DAlg_DomeFit.h"
#include "Point3DAlg_CircleFit.h"
#include "Point3DAlg_FitUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <unordered_map>
#include <vector>

using CloudXYZ = pcl::PointCloud<pcl::PointXYZ>;

Point3DAlg_BumpReportParams::Point3DAlg_BumpReportParams()
    : height_threshold(0.02)
    , cluster_tolerance(0.25)
    , min_points(8)
    , depth_ratio(0.35)
    , half_band(0.02)
    , circle_mad_factor(3.0)
    , circle_rms_max(0.02)
    , height_min(0.0)
    , height_max(0.0)
    , diameter_min(0.0)
    , diameter_max(0.0)
    , nominal_pitch_x(0.0)
    , nominal_pitch_y(0.0)
    , nominal_origin_x(0.0)
    , nominal_origin_y(0.0)
    , pos_threshold(0.05)
{
}

Point3DAlg_BumpReportEntry::Point3DAlg_BumpReportEntry()
    : x(0.0), y(0.0), height(0.0), diameter(0.0), circle_rms(0.0),
      circle_points(0), coplanarity_dev(0.0), pos_dev(-1.0), matched_nom(0),
      ng_flags(0)
{
}

Point3DAlg_BumpReportStats::Point3DAlg_BumpReportStats()
    : count(0), ng_height(0), ng_diameter(0),
      h_mean(0.0), h_sigma(0.0), h_min(0.0), h_max(0.0),
      d_mean(0.0), d_sigma(0.0), cop_sigma(0.0),
      nominal_total(0), nominal_matched(0), nominal_unmatched(0),
      pos_mean(0.0), pos_sigma(0.0), pos_max(0.0), ng_pos(0)
{
}

bool Point3DAlg_BumpReport(const CloudXYZ::ConstPtr& cloud,
                           const Point3DAlg_BumpReportParams& params,
                           std::vector<Point3DAlg_BumpReportEntry>& entries,
                           Point3DAlg_BumpReportStats* stats)
{
    entries.clear();
    if (stats)
        *stats = Point3DAlg_BumpReportStats();
    if (!cloud || cloud->empty())
        return false;

    // ---- 1. DomeFit：高度/球顶/球半径 ----
    Point3DAlg_DomeFitParams dp;
    dp.height_threshold = params.height_threshold;
    dp.cluster_tolerance = params.cluster_tolerance;
    dp.min_points = params.min_points;
    std::vector<Point3DAlg_DomeBump> bumps;
    if (!Point3DAlg_DomeFit(cloud, dp, bumps, nullptr))
        return false;

    // ---- 2. z-bin 装箱（bin = half_band），逐 bump 半高带取点 ----
    const double hb = params.half_band > 0.0 ? params.half_band : 0.02;
    std::unordered_map<int32_t, std::vector<const pcl::PointXYZ*>> zbins;
    zbins.reserve(cloud->size() / 4 + 1);
    for (const auto& p : cloud->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        zbins[(int32_t)std::floor(p.z / hb)].push_back(&p);
    }

    // ---- 3. 共面性最佳拟合面（对全部有效 bump 的高度）----
    // 与 DomeStatsOf.fit_sigma 同一平面模型；dev 逐 bump 输出
    double fp = 0.0, fq = 0.0, fr = 0.0;
    {
        std::vector<double> bx, by, bh;
        bx.reserve(bumps.size());
        by.reserve(bumps.size());
        bh.reserve(bumps.size());
        for (const auto& b : bumps)
            if (b.valid) {
                bx.push_back(b.apex[0]);
                by.push_back(b.apex[1]);
                bh.push_back(b.height);
            }
        if (bx.size() >= 4) {
            std::vector<size_t> idx(bx.size());
            for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
            for (int it = 0; it < 3; ++it) {
                std::vector<double> qx(idx.size()), qy(idx.size()), qz(idx.size());
                for (size_t k = 0; k < idx.size(); ++k) {
                    qx[k] = bx[idx[k]]; qy[k] = by[idx[k]]; qz[k] = bh[idx[k]];
                }
                if (!point3d_fit::FitPlaneZ(qx, qy, qz, 0, idx.size(), fp, fq, fr))
                    break;
                std::vector<double> rAll(bx.size());
                for (size_t i = 0; i < bx.size(); ++i)
                    rAll[i] = bh[i] - (fp * bx[i] + fq * by[i] + fr);
                const double med = point3d_fit::MedianOf(rAll);
                std::vector<double> ad(bx.size());
                for (size_t i = 0; i < bx.size(); ++i) ad[i] = std::fabs(rAll[i] - med);
                const double mad = point3d_fit::MedianOf(ad);
                const double hi = med + 3.0 * mad + 1e-12, lo = med - 3.0 * mad - 1e-12;
                std::vector<size_t> keep;
                keep.reserve(idx.size());
                for (size_t i = 0; i < bx.size(); ++i)
                    if (rAll[i] >= lo && rAll[i] <= hi) keep.push_back(i);
                if (keep.size() < 4) break;
                const bool conv = keep.size() == idx.size();
                idx = keep;
                if (conv) break;
            }
        }
    }

    // ---- 4. 逐 bump：半高切片圆拟合 + coplanarity + NG 位 ----
    entries.reserve(bumps.size());
    for (const auto& b : bumps) {
        if (!b.valid)
            continue;
        Point3DAlg_BumpReportEntry e;
        e.x = b.apex[0];
        e.y = b.apex[1];
        e.height = b.height;
        e.coplanarity_dev = b.height - (fp * b.apex[0] + fq * b.apex[1] + fr);

        // 截面：距球顶 depth_ratio×height 处的球冠截面圆半径（解析）
        const double ratio = (params.depth_ratio > 0.0 && params.depth_ratio < 1.0)
                                 ? params.depth_ratio : 0.35;
        const double zHalf = b.apex[2] - b.height * ratio;
        double rHalf = 0.0;
        if (b.radius > 0.0 && b.height > 0.0) {
            const double dz = b.radius - b.height * ratio;
            const double d2 = b.radius * b.radius - dz * dz;
            if (d2 > 0.0)
                rHalf = std::sqrt(d2);
        }
        // 直径主口径：解析值 2×rHalf（球参数已知，零拟合噪声——半高带是环带，
        // 0.04 网格下环带内仅 ~10 个稀疏格点，圆拟合实测偏差可达 9%）。
        // CircleFit 降为截面校验（rms/点数）。
        if (rHalf > 0.0)
            e.diameter = 2.0 * rHalf;
        if (rHalf > 0.0) {
            const double rSearch = rHalf * 1.3 + 0.05;
            const long long zLo = (long long)std::floor((zHalf - hb) / hb);
            const long long zHi = (long long)std::floor((zHalf + hb) / hb);
            std::vector<double> xy;
            for (long long zb = zLo; zb <= zHi; ++zb) {
                const auto it = zbins.find((int32_t)zb);
                if (it == zbins.end())
                    continue;
                for (const pcl::PointXYZ* p : it->second) {
                    const double ddx = p->x - e.x, ddy = p->y - e.y;
                    if (ddx * ddx + ddy * ddy <= rSearch * rSearch) {
                        xy.push_back(p->x);
                        xy.push_back(p->y);
                    }
                }
            }
            if ((int)xy.size() / 2 >= 3) {
                Point3DAlg_CircleFitResult c;
                // 截面校验：圆拟合残差应小（点确实落在一个圆上）；截面圆半径
                // ≤ 球半径（×1.2 容差）。diameter 不取拟合值（见上）。
                if (Point3DAlg_FitCircle2D(xy.data(), (int)xy.size() / 2,
                                           params.circle_mad_factor, 3,
                                           0.005, b.radius * 1.2 + 0.01,
                                           params.circle_rms_max, c) &&
                    c.valid) {
                    e.circle_rms = c.rms;
                    e.circle_points = c.point_count;
                } else {
                    e.ng_flags |= 4;   // 截面校验失败
                }
            } else {
                e.ng_flags |= 4;       // 截面点不足
            }
        } else {
            e.ng_flags |= 4;           // 半径无效，无法定截面
        }

        // ---- 位置度（名义网格，pitch>0 启用）----
        if (params.nominal_pitch_x > 0.0 && params.nominal_pitch_y > 0.0) {
            const double gx = (e.x - params.nominal_origin_x) / params.nominal_pitch_x;
            const double gy = (e.y - params.nominal_origin_y) / params.nominal_pitch_y;
            const double ix = std::round(gx), iy = std::round(gy);
            const bool inGrid = ix >= 0.0 && iy >= 0.0 &&
                                ix * params.nominal_pitch_x <= 1e6 &&
                                iy * params.nominal_pitch_y <= 1e6;
            if (inGrid) {
                const double nx = params.nominal_origin_x + ix * params.nominal_pitch_x;
                const double ny = params.nominal_origin_y + iy * params.nominal_pitch_y;
                const double ddx = e.x - nx, ddy = e.y - ny;
                e.pos_dev = std::sqrt(ddx * ddx + ddy * ddy);
                e.matched_nom = 1;
                if (e.pos_dev > params.pos_threshold)
                    e.ng_flags |= 8;   // bit3 位置度越界
            } else {
                e.matched_nom = 0;
                e.ng_flags |= 8;       // 不在名义网格邻域（多出/错位）
            }
        }

        // NG 位
        if (params.height_min > 0.0 && e.height < params.height_min)
            e.ng_flags |= 1;
        if (params.height_max > 0.0 && e.height > params.height_max)
            e.ng_flags |= 1;
        if (params.diameter_min > 0.0 && e.diameter > 0.0 && e.diameter < params.diameter_min)
            e.ng_flags |= 2;
        if (params.diameter_max > 0.0 && e.diameter > params.diameter_max)
            e.ng_flags |= 2;

        entries.push_back(e);
    }

    // apex (x,y) 升序（帧间稳定对应）
    std::sort(entries.begin(), entries.end(),
              [](const Point3DAlg_BumpReportEntry& a, const Point3DAlg_BumpReportEntry& b) {
                  if (a.x != b.x) return a.x < b.x;
                  return a.y < b.y;
              });

    // ---- 5. 汇总 ----
    if (stats) {
        stats->count = (int)entries.size();
        std::vector<double> hs, ds, cops;
        for (const auto& e : entries) {
            if (e.ng_flags & 1) ++stats->ng_height;
            if (e.ng_flags & 2) ++stats->ng_diameter;
            hs.push_back(e.height);
            cops.push_back(e.coplanarity_dev);
            if (e.diameter > 0.0) ds.push_back(e.diameter);
        }
        auto stat = [](std::vector<double>& v, double& mean, double& sigma) {
            if (v.empty()) { mean = sigma = 0.0; return; }
            double s = 0.0;
            for (double x : v) s += x;
            mean = s / (double)v.size();
            double s2 = 0.0;
            for (double x : v) s2 += (x - mean) * (x - mean);
            sigma = v.size() > 1 ? std::sqrt(s2 / (double)(v.size() - 1)) : 0.0;
        };
        stat(hs, stats->h_mean, stats->h_sigma);
        stat(ds, stats->d_mean, stats->d_sigma);
        double copMean = 0.0;
        stat(cops, copMean, stats->cop_sigma);   // dev 相对拟合面，均值≈0
        // 位置度统计（启用名义网格时）：
        // 名义点范围 = 已匹配 bump 的名义索引范围（不外推）——
        // nominal_unmatched = 范围内无 bump 对应的名义点数（内部漏检）；
        // 阵列外圈的整行/整列缺失超出检出范围，不计入（需整板行列表才可判，暂不做）。
        if (params.nominal_pitch_x > 0.0 && params.nominal_pitch_y > 0.0) {
            long long ixMin = 0, ixMax = -1, iyMin = 0, iyMax = -1;
            bool anyNom = false;
            for (const auto& e : entries)
                if (e.matched_nom == 1) {
                    const long long ix = (long long)std::llround(
                        (e.x - params.nominal_origin_x) / params.nominal_pitch_x);
                    const long long iy = (long long)std::llround(
                        (e.y - params.nominal_origin_y) / params.nominal_pitch_y);
                    if (!anyNom) {
                        ixMin = ixMax = ix; iyMin = iyMax = iy; anyNom = true;
                    } else {
                        ixMin = std::min(ixMin, ix); ixMax = std::max(ixMax, ix);
                        iyMin = std::min(iyMin, iy); iyMax = std::max(iyMax, iy);
                    }
                }
            if (anyNom)
                stats->nominal_total = (int)((ixMax - ixMin + 1) * (iyMax - iyMin + 1));
            stats->nominal_matched = (int)std::count_if(
                entries.begin(), entries.end(),
                [](const Point3DAlg_BumpReportEntry& e) { return e.matched_nom == 1; });
            stats->nominal_unmatched = stats->nominal_total - stats->nominal_matched;
            std::vector<double> posv;
            for (const auto& e : entries)
                if (e.matched_nom == 1) posv.push_back(e.pos_dev);
            stat(posv, stats->pos_mean, stats->pos_sigma);
            stats->pos_max = posv.empty() ? 0.0 : *std::max_element(posv.begin(), posv.end());
            stats->ng_pos = (int)std::count_if(
                entries.begin(), entries.end(),
                [](const Point3DAlg_BumpReportEntry& e) { return (e.ng_flags & 8) != 0; });
        }
        if (!hs.empty()) {
            stats->h_min = *std::min_element(hs.begin(), hs.end());
            stats->h_max = *std::max_element(hs.begin(), hs.end());
        }
        // cops 的均值未归零的修正：dev 相对拟合面本身均值≈0，无需额外处理
    }
    return !entries.empty();
}
