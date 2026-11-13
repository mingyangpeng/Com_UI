// Point3DAlg_Profile.cpp —— 轮廓/截面量测实现（纯函数，零 UI）。
// 格内 z 中位数与差值统计共用 point3d_fit::MedianOf（FitUtil 内核）。

#include "Point3DAlg_Profile.h"

#include "Point3DAlg_FitUtil.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

using point3d_fit::MedianOf;

Point3DAlg_ProfileParams::Point3DAlg_ProfileParams()
    : band(0.2)
    , bin_size(0.1)
    , min_points_per_bin(1)
{
}

Point3DAlg_ProfileResult::Point3DAlg_ProfileResult()
    : points(0), used_points(0), length(0.0), z_min(0.0), z_max(0.0),
      s_at_min(0.0), s_at_max(0.0), max_step(0.0), s_at_step(0.0)
{
}

bool Point3DAlg_Profile(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
                        double x0, double y0, double x1, double y1,
                        const Point3DAlg_ProfileParams& params,
                        Point3DAlg_ProfileResult& out,
                        std::vector<float>* out_profile)
{
    out = Point3DAlg_ProfileResult();
    if (out_profile)
        out_profile->clear();
    if (!cloud || cloud->empty() || !params.band || !(params.band > 0.0) ||
        !(params.bin_size > 0.0))
        return false;

    const double dx = x1 - x0, dy = y1 - y0;
    const double len2 = dx * dx + dy * dy;
    if (!(len2 > 1e-12))
        return false;                       // 零长度线段
    const double len = std::sqrt(len2);
    const double ux = dx / len, uy = dy / len;   // 单位切向

    // ---- 1. 带内点 → 弧长分格（格内收集 z）----
    const double invBin = 1.0 / params.bin_size;
    std::unordered_map<long long, std::vector<double>> bins;
    long long binLo = 1LL << 30, binHi = -(1LL << 30);
    for (const auto& p : cloud->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        const double px = p.x - x0, py = p.y - y0;
        const double s = px * ux + py * uy;         // 投影弧长
        if (s < 0.0 || s > len)
            continue;                               // 只取线段范围（不外延）
        const double perp = std::fabs(px * uy - py * ux);   // 叉积 = 垂距
        if (perp > params.band)
            continue;
        const long long bi = (long long)std::floor(s * invBin);
        bins[bi].push_back(p.z);
        if (bi < binLo) binLo = bi;
        if (bi > binHi) binHi = bi;
    }
    if (bins.empty())
        return false;
    long long used = 0;
    for (const auto& kv : bins)
        used += (long long)kv.second.size();

    // ---- 2. 格内中位数 → 剖面序列（按 s 升序）----
    std::vector<std::pair<double, double>> prof;   // (s_center, z_median)
    prof.reserve((size_t)(binHi - binLo + 1));
    for (long long bi = binLo; bi <= binHi; ++bi) {
        const auto it = bins.find(bi);
        if (it == bins.end() || (int)it->second.size() < std::max(1, params.min_points_per_bin))
            continue;
        prof.emplace_back(((double)bi + 0.5) * params.bin_size, MedianOf(it->second));
    }
    if (prof.empty())
        return false;

    // ---- 3. 统计 ----
    out.points = (long long)prof.size();
    out.used_points = used;
    out.length = len;
    double zmin = 1e300, zmax = -1e300, smn = 0, smx = 0;
    double maxStep = 0.0, sStep = 0.0;
    for (size_t i = 0; i < prof.size(); ++i) {
        if (prof[i].second < zmin) { zmin = prof[i].second; smn = prof[i].first; }
        if (prof[i].second > zmax) { zmax = prof[i].second; smx = prof[i].first; }
        if (i > 0) {
            const double step = std::fabs(prof[i].second - prof[i - 1].second);
            if (step > maxStep) {
                maxStep = step;
                sStep = 0.5 * (prof[i].first + prof[i - 1].first);
            }
        }
    }
    out.z_min = zmin;
    out.z_max = zmax;
    out.s_at_min = smn;
    out.s_at_max = smx;
    out.max_step = maxStep;
    out.s_at_step = sStep;

    if (out_profile) {
        out_profile->reserve(prof.size() * 2);
        for (const auto& pr : prof) {
            out_profile->push_back((float)pr.first);
            out_profile->push_back((float)pr.second);
        }
    }
    return true;
}
