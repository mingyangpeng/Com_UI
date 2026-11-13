// Point3DAlg_CloudDiff.cpp —— 两片点云高度差对比实现（纯函数，零 UI）。
// 栅格化用 XY 网格哈希（2D 键打包 uint64，负坐标经 uint32 回绕仍一一对应）；
// 格内 z 取中位数（point3d_fit::MedianOf，与 DomeFit/RegionPlane 共用内核）。

#include "Point3DAlg_CloudDiff.h"

#include "Point3DAlg_FitUtil.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

using point3d_fit::MedianOf;

namespace {

// XY 格键（floor(cell) 打包；ix/iv 可为负，uint32 回绕后同值仍同键）
inline uint64_t CellKey2(int32_t ix, int32_t iy)
{
    return ((uint64_t)(uint32_t)ix << 32) | (uint32_t)iy;
}

// 栅格化：每格 z 中位数（点数不足的格丢弃）
void RasterizeZ(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud, double cell, int minPts,
                std::unordered_map<uint64_t, double>& outZ,
                std::unordered_map<uint64_t, std::pair<double, double>>* outCenter = nullptr)
{
    std::unordered_map<uint64_t, std::vector<double>> cells;
    cells.reserve(cloud->size() / 2 + 1);
    for (const auto& p : cloud->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        const int32_t ix = (int32_t)std::floor(p.x / cell);
        const int32_t iy = (int32_t)std::floor(p.y / cell);
        cells[CellKey2(ix, iy)].push_back((double)p.z);
    }
    for (const auto& kv : cells) {
        if ((int)kv.second.size() < minPts)
            continue;
        outZ[kv.first] = MedianOf(kv.second);
        if (outCenter) {
            const int32_t ix = (int32_t)((uint32_t)(kv.first >> 32));
            const int32_t iy = (int32_t)((uint32_t)(kv.first & 0xFFFFFFFFu));
            (*outCenter)[kv.first] = { ((double)ix + 0.5) * cell, ((double)iy + 0.5) * cell };
        }
    }
}

// B 格键整体平移 (gi,gj) 后与 A 的 mean|diff|（对齐得分，越小越好）
double AlignScore(const std::unordered_map<uint64_t, double>& zA,
                  const std::unordered_map<uint64_t, double>& zB, int gi, int gj)
{
    // 得分用 RMS（L2）而非 mean|diff|（L1）：台阶/边缘错位的误差集中在少量格上，
    // L1 会被稀释（0.175mm 错位仅惩罚 ~1% 格，实测把 dx 拉偏 0.125）；L2 对结构
    // 错位敏感。另：mean|diff| 不惩罚重叠减少，偏移大到重叠萎缩的候选会在平滑区
    // 得到虚假低分（配准经典陷阱）——匹配格 < 70% 参考格直接判不可用。
    const long long ref = (long long)std::min(zA.size(), zB.size());
    double sum2 = 0.0;
    long long n = 0;
    for (const auto& kv : zB) {
        const int32_t ix = (int32_t)(uint32_t)(kv.first >> 32);
        const int32_t iy = (int32_t)(uint32_t)(kv.first & 0xFFFFFFFFu);
        const auto it = zA.find(CellKey2(ix + gi, iy + gj));
        if (it == zA.end())
            continue;
        const double d = kv.second - it->second;
        sum2 += d * d;
        ++n;
    }
    if (n == 0 || n * 10 < ref * 7)
        return 1e300;
    return std::sqrt(sum2 / (double)n);
}

} // namespace

Point3DAlg_CloudDiffParams::Point3DAlg_CloudDiffParams()
    : cell_size(0.1)
    , min_points_per_cell(3)
    , diff_threshold(0.02)
    , align_search(0.0)
{
}

Point3DAlg_CloudDiffResult::Point3DAlg_CloudDiffResult()
    : cells(0), mean(0.0), sigma(0.0), max_abs(0.0), max_pos(0.0), max_neg(0.0),
      over_count(0), over_ratio(0.0), aligned(false), aligned_dx(0.0), aligned_dy(0.0)
{
}

bool Point3DAlg_CloudDiff(const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloudA,
                          const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloudB,
                          const Point3DAlg_CloudDiffParams& params,
                          Point3DAlg_CloudDiffResult& out,
                          std::vector<float>* out_diff_cloud)
{
    out = Point3DAlg_CloudDiffResult();
    if (out_diff_cloud)
        out_diff_cloud->clear();
    if (!cloudA || cloudA->empty() || !cloudB || cloudB->empty())
        return false;
    if (!(params.cell_size > 0.0))
        return false;

    const double cell = params.cell_size;
    std::unordered_map<uint64_t, double> zA, zB;
    std::unordered_map<uint64_t, std::pair<double, double>> center;
    RasterizeZ(cloudA, cell, params.min_points_per_cell, zA, &center);
    RasterizeZ(cloudB, cell, params.min_points_per_cell, zB, nullptr);
    if (zA.empty() || zB.empty())
        return false;

    // ---- XY 粗对齐（可选）：格域整数搜索(粗) + ±1 格细分(步 cell/4) ----
    // 得分 = mean|diff|（L1，鲁棒）；对齐量施加于 B（对齐后 B'+T ≈ A）。
    double tdx = 0.0, tdy = 0.0;
    if (params.align_search > 0.0) {
        const int G = (int)std::ceil(params.align_search / cell);
        int bestGi = 0, bestGj = 0;
        double bestScore = AlignScore(zA, zB, 0, 0);
        for (int gi = -G; gi <= G; ++gi)
            for (int gj = -G; gj <= G; ++gj) {
                if (gi == 0 && gj == 0)
                    continue;
                const double sc = AlignScore(zA, zB, gi, gj);
                if (sc < bestScore) {
                    bestScore = sc;
                    bestGi = gi;
                    bestGj = gj;
                }
            }
        // 细化：±2 格、步 cell/4（B 点级平移重栅格化，O(25n)）——
        // 真值平移可能落在两整格正中（残差 cell/2），±1 格（±cell/4）够不着
        const double sub = cell * 0.25;
        double bestDx = bestGi * cell, bestDy = bestGj * cell;
        for (int si = -2; si <= 2; ++si)
            for (int sj = -2; sj <= 2; ++sj) {
                if (si == 0 && sj == 0)
                    continue;
                std::unordered_map<uint64_t, double> zBsub;
                pcl::PointCloud<pcl::PointXYZ>::Ptr shifted(new pcl::PointCloud<pcl::PointXYZ>);
                shifted->reserve(cloudB->size());
                const double sx = bestDx + si * sub, sy = bestDy + sj * sub;
                for (const auto& p : cloudB->points)
                    shifted->push_back(pcl::PointXYZ((float)(p.x + sx), (float)(p.y + sy), p.z));
                RasterizeZ(shifted, cell, params.min_points_per_cell, zBsub, nullptr);
                if (zBsub.empty())
                    continue;
                const double sc = AlignScore(zA, zBsub, 0, 0);
                if (sc < bestScore) {
                    bestScore = sc;
                    bestDx = sx;
                    bestDy = sy;
                }
            }
        tdx = bestDx;
        tdy = bestDy;
        out.aligned = true;
        out.aligned_dx = tdx;
        out.aligned_dy = tdy;
    }

    // ---- 被测云施加对齐平移后重栅格化（无对齐时 tdx=tdy=0，栅格化结果不变）----
    if (out.aligned) {
        pcl::PointCloud<pcl::PointXYZ>::Ptr shifted(new pcl::PointCloud<pcl::PointXYZ>);
        shifted->reserve(cloudB->size());
        for (const auto& p : cloudB->points)
            shifted->push_back(pcl::PointXYZ((float)(p.x + tdx), (float)(p.y + tdy), p.z));
        zB.clear();
        RasterizeZ(shifted, cell, params.min_points_per_cell, zB, nullptr);
        if (zB.empty())
            return false;
    }

    // 差值：遍历 B 的格，取 A 同格（格中心以 A 侧为准，避免重复容器）
    std::vector<double> diffs;
    diffs.reserve(std::min(zA.size(), zB.size()));
    if (out_diff_cloud)
        out_diff_cloud->reserve(std::min(zA.size(), zB.size()) * 3);

    double sum = 0.0, sum2 = 0.0;
    double maxAbs = 0.0, maxPos = -1e300, maxNeg = 1e300;
    long long over = 0;
    const bool judge = params.diff_threshold > 0.0;
    for (const auto& kv : zB) {
        const auto itA = zA.find(kv.first);
        if (itA == zA.end())
            continue;
        const double d = kv.second - itA->second;
        diffs.push_back(d);
        sum += d;
        sum2 += d * d;
        const double ad = std::fabs(d);
        if (ad > maxAbs) maxAbs = ad;
        if (d > maxPos) maxPos = d;
        if (d < maxNeg) maxNeg = d;
        if (judge && ad > params.diff_threshold)
            ++over;
        if (out_diff_cloud) {
            const auto itC = center.find(kv.first);
            if (itC != center.end()) {
                out_diff_cloud->push_back((float)itC->second.first);
                out_diff_cloud->push_back((float)itC->second.second);
                out_diff_cloud->push_back((float)d);
            }
        }
    }
    const long long n = (long long)diffs.size();
    if (n == 0)
        return false;

    out.cells = n;
    out.mean = sum / (double)n;
    out.sigma = std::sqrt(std::max(0.0, sum2 / (double)n - out.mean * out.mean));
    out.max_abs = maxAbs;
    out.max_pos = maxPos;
    out.max_neg = maxNeg;
    out.over_count = over;
    out.over_ratio = (double)over / (double)n;
    return true;
}
