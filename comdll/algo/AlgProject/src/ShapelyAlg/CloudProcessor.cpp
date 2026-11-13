// CloudProcessor.cpp —— PCL 点云算子实现。
// 显式实例化白名单：pcl::PointXYZ / PointXYZI / PointXYZRGB（文末）。
// 注意（PCL 1.9 + MSVC 踩坑记录）：
//   * 算法重头文件（filters/segmentation/registration/...）只在本 .cpp include，
//     公共头零模板传染；
//   * LMEDS/MSAC 在部分 SAC 模型（球/圆柱）不支持，底层回退 RANSAC；
//   * Harris3D/ISS 需要 pcl::PointCloud<pcl::PointXYZ> 中转（只支持 XYZ 输入）；
//   * PCL 点型不能聚合初始化/互赋值（PointXYZRGB），用 makeQueryPoint 逐字段。

#include "CloudProcessor.h"
#include "FitUtil.h"
#include "Stereo.h"   // meshCropByOBB 复用 Stereo::pointInOBB（同库 cpp 交叉引用）

#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/statistical_outlier_removal.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/filters/crop_box.h>
#include <pcl/filters/random_sample.h>
#include <pcl/filters/filter.h>
#include <pcl/common/transforms.h>
#include <pcl/common/io.h>
#include <pcl/common/centroid.h>
#include <pcl/common/common.h>
#include <pcl/common/point_tests.h>
#include <pcl/features/normal_3d.h>
#include <pcl/features/fpfh.h>
#include <pcl/keypoints/harris_3d.h>
#include <pcl/keypoints/iss_3d.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/segmentation/extract_clusters.h>
#include <pcl/segmentation/region_growing.h>
#include <pcl/registration/icp.h>
#include <pcl/registration/icp_nl.h>
#include <pcl/registration/gicp.h>
#include <pcl/registration/ndt.h>
#include <pcl/registration/ia_ransac.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/octree/octree_search.h>
#include <pcl/octree/octree_pointcloud_changedetector.h>
#include <pcl/surface/convex_hull.h>
#include <pcl/surface/concave_hull.h>
#include <pcl/surface/mls.h>
#include <pcl/surface/poisson.h>
#include <pcl/surface/gp3.h>
#include <pcl/surface/marching_cubes_hoppe.h>
#include <pcl/surface/marching_cubes_rbf.h>
#include <pcl/surface/grid_projection.h>
#include <pcl/surface/organized_fast_mesh.h>

#include <pcl/sample_consensus/sac_model_cone.h>
#include <pcl/sample_consensus/ransac.h>
#include <pcl/sample_consensus/impl/sac_model_cone.hpp>
#include <pcl/sample_consensus/impl/ransac.hpp>

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <utility>


namespace shapely {

// ---------------- 内部工具 ----------------

namespace {

// shapely::FitMethod → pcl::SACMethods（LMEDS/MSAC 部分模型不支持，回退 RANSAC）
int toSacMethod(FitMethod m)
{
    switch (m) {
    case FitMethod::LMEDS: return pcl::SAC_LMEDS;
    case FitMethod::MSAC:  return pcl::SAC_MSAC;
    default:                return pcl::SAC_RANSAC;
    }
}

// Eigen 4x4 → 行主序 float[16]。
// 返回 false 表示矩阵含 NaN/Inf（PCL 的 LLS/GICP 等在退化几何上会出现
// hasConverged()==true 但变换矩阵为 NaN，必须拦截，否则污染下游）。
template <typename M>
bool matToArray(const M& m, float out[16])
{
    bool finite = true;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float v = m(r, c);
            out[r * 4 + c] = v;
            if (!std::isfinite(v))
                finite = false;
        }
    return finite;
}

// 查询点构造：逐字段赋值（PointXYZRGB 等带额外字段的点型不能聚合初始化，
// 显式实例化白名单要求所有代码路径对全部白名单点型可编译）。
// 值初始化 `{}` 保证 rgb/intensity 等未用字段为 0 而非未初始化内存。
template <typename PointT>
PointT makeQueryPoint(const float q[3])
{
    PointT p{};
    p.x = q[0];
    p.y = q[1];
    p.z = q[2];
    return p;
}

// PNPOLY 射线法（W. Randolph Franklin）：XY 平面点在多边形内判定（even-odd）。
// 要求 poly.size() >= 3；边界点归属为半开语义。
bool pointInPolygonXY(const std::vector<Point2d>& poly, double x, double y)
{
    bool inside = false;
    const size_t n = poly.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const double xi = poly[i].x, yi = poly[i].y;
        const double xj = poly[j].x, yj = poly[j].y;
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi) + xi))
            inside = !inside;
    }
    return inside;
}

// 构造只含坐标的点：其余字段显式清零。
// PCL 点型的默认构造不初始化 rgb/intensity（PointXYZRGB 的 rgb 会是未初始化内存），
// 因此用值初始化 `PointT p{}` 而不是 `PointT p`。
template <typename PointT>
PointT makePoint(float x, float y, float z)
{
    PointT p{};
    p.x = x;
    p.y = y;
    p.z = z;
    return p;
}

// 归一化并集：并查集（DBSCAN 聚类用）
class UnionFind
{
public:
    explicit UnionFind(size_t n) : parent_(n), rank_(n, 0)
    {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    int find(int x)
    {
        while (parent_[x] != x) {
            parent_[x] = parent_[parent_[x]];
            x = parent_[x];
        }
        return x;
    }

    void unite(int a, int b)
    {
        a = find(a);
        b = find(b);
        if (a == b)
            return;
        if (rank_[a] < rank_[b])
            std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b])
            ++rank_[a];
    }

private:
    std::vector<int> parent_;
    std::vector<int> rank_;
};

// 拟合入口的非有限点剔除 + 全局索引映射（映射后 out_clean 点 j = 原始 src 行 global[j]）。
// Debug 回归实锤：含 NaN/Inf 的点进 RANSAC 会触发 PCL KdTree 断言
//（kdtree_flann.hpp:136 nearestKSearch isValid，Release 静默 no solution）。
template <typename PointT>
bool fitInputClean(const typename CloudProcessor<PointT>::CloudConstPtr& src,
                   typename CloudProcessor<PointT>::CloudPtr& out_clean,
                   std::vector<int>& out_global)
{
    out_global.clear();
    if (!src || src->empty())
        return false;
    out_global.resize(src->size());
    size_t k = 0;
    for (size_t i = 0; i < src->size(); ++i) {
        const PointT& p = (*src)[i];
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z))
            out_global[k++] = (int)i;
    }
    out_global.resize(k);
    if (out_global.empty())
        return false;   // 全非有限才拒绝；1~2 点由调用方按各自语义处理
    out_clean = CloudProcessor<PointT>::extractByIndices(src, out_global);
    return out_clean && !out_clean->empty();
}

} // namespace

// ---------------- filter ----------------

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterVoxelGrid(const CloudConstPtr& src, double leaf_size,
                                        int min_points_per_voxel)
{
    if (!src || leaf_size <= 0)
        return nullptr;
    pcl::VoxelGrid<PointT> f;
    f.setInputCloud(src);
    f.setLeafSize((float)leaf_size, (float)leaf_size, (float)leaf_size);
    if (min_points_per_voxel > 0)
        f.setMinimumPointsNumberPerVoxel((unsigned int)min_points_per_voxel);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterVoxelGridAndTrace(const CloudConstPtr& src, double leaf_size,
                                                std::vector<std::vector<int>>& out_trace)
{
    out_trace.clear();
    if (!src || leaf_size <= 0)
        return nullptr;

    // VoxelGrid 的质心索引表：保存叶子布局后，输入点所属体素 → 输出点索引
    pcl::VoxelGrid<PointT> f;
    f.setInputCloud(src);
    f.setLeafSize((float)leaf_size, (float)leaf_size, (float)leaf_size);
    f.setSaveLeafLayout(true);
    CloudPtr down(new pcl::PointCloud<PointT>);
    f.filter(*down);
    if (down->empty())
        return down;

    // 每个输入点按所属体素的质心索引归组（无质心的体素返回 -1，丢弃）
    out_trace.assign(down->size(), std::vector<int>());
    for (size_t i = 0; i < src->size(); ++i) {
        const PointT& p = (*src)[i];
        if (!pcl::isFinite(p))
            continue;
        const Eigen::Vector3i ijk = f.getGridCoordinates(p.x, p.y, p.z);
        const int centroid = f.getCentroidIndexAt(ijk);
        if (centroid >= 0 && (size_t)centroid < out_trace.size())
            out_trace[(size_t)centroid].push_back((int)i);
    }
    return down;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterRandomSample(const CloudConstPtr& src, int sample_size,
                                           unsigned int seed)
{
    if (!src || sample_size <= 0)
        return nullptr;
    pcl::RandomSample<PointT> f;
    f.setInputCloud(src);
    f.setSample((unsigned int)sample_size);
    f.setSeed(seed);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterUniformSample(const CloudConstPtr& src, int every_k)
{
    if (!src || every_k <= 0)
        return nullptr;
    // Open3D uniform_down_sample 语义：从索引 0 起，每 every_k 个点取 1 个
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(src->size() / (size_t)every_k + 1);
    for (size_t i = 0; i < src->size(); i += (size_t)every_k)
        out->push_back((*src)[i]);
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = src->is_dense;
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterFarthestPointSample(const CloudConstPtr& src, int num_samples,
                                                  int start_index)
{
    if (!src || src->empty() || num_samples <= 0)
        return nullptr;
    const size_t n = src->size();
    const size_t k = std::min((size_t)num_samples, n);
    std::vector<int> selected;
    selected.reserve(k);

    int first = (start_index >= 0 && (size_t)start_index < n) ? start_index : 0;
    selected.push_back(first);

    // min_dist[i] = 点 i 到已选集合的最小平方距离
    std::vector<float> min_dist(n, std::numeric_limits<float>::max());
    auto update = [&](int idx) {
        const PointT& p = (*src)[(size_t)idx];
        for (size_t i = 0; i < n; ++i) {
            const float dx = (*src)[i].x - p.x;
            const float dy = (*src)[i].y - p.y;
            const float dz = (*src)[i].z - p.z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < min_dist[i])
                min_dist[i] = d;
        }
    };
    update(first);

    while (selected.size() < k) {
        int best = -1;
        float best_d = -1.0f;
        for (size_t i = 0; i < n; ++i) {
            if (min_dist[i] > best_d) {
                best_d = min_dist[i];
                best = (int)i;
            }
        }
        if (best < 0)
            break;
        selected.push_back(best);
        min_dist[(size_t)best] = -1.0f;   // 已选点不再参选
        update(best);
    }

    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(selected.size());
    for (int idx : selected)
        out->push_back((*src)[(size_t)idx]);
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = src->is_dense;
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterRemoveNonFinite(const CloudConstPtr& src)
{
    if (!src)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    std::vector<int> mapping;
    pcl::removeNaNFromPointCloud(*src, *out, mapping);
    // removeNaN 只查 NaN；这里再兜底 Inf（PCL 的 pcl_isfinite 判断覆盖两者，
    // 但旧版 removeNaN 实现只比较自身，故显式过滤一遍）
    CloudPtr clean(new pcl::PointCloud<PointT>);
    clean->reserve(out->size());
    for (const PointT& p : out->points)
        if (pcl::isFinite(p))
            clean->push_back(p);
    clean->width = (uint32_t)clean->size();
    clean->height = 1;
    clean->is_dense = true;
    return clean;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterRemoveDuplicates(const CloudConstPtr& src, double tolerance)
{
    if (!src)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(src->size());
    if (src->empty())
        return out;

    if (tolerance <= 0) {
        // 精确去重：按坐标三元组哈希，O(n)
        // NaN 点跳过：NaN 参与 floor/哈希/比较都会产生垃圾行为，
        // 且 NaN != NaN 恒真会把 NaN 点全部当作"不重复"保留
        struct Key {
            float x, y, z;
            bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
        };
        struct KeyHash {
            size_t operator()(const Key& k) const
            {
                size_t h = std::hash<float>()(k.x);
                h ^= std::hash<float>()(k.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
                h ^= std::hash<float>()(k.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
                return h;
            }
        };
        std::unordered_map<Key, int, KeyHash> seen;
        seen.reserve(src->size());
        for (const PointT& p : src->points) {
            const Key key{ p.x, p.y, p.z };
            if (seen.find(key) == seen.end()) {
                seen[key] = 0;
                out->push_back(p);
            }
        }
    } else {
        // 容差去重：体素打包 + 邻域已有点距离复核（避免体素边界漏判）
        const float tol2 = (float)(tolerance * tolerance);
        std::unordered_map<int64_t, std::vector<int>> grid;
        auto cellOf = [&](float v) { return (int64_t)std::floor(v / (float)tolerance); };
        for (const PointT& p : src->points) {
            // NaN/Inf 点跳过：floor(NaN) 转整型是未定义行为，且 NaN 比较恒 false
            // 会让非有限点全部被当作"不重复"保留
            if (!pcl::isFinite(p))
                continue;
            const int64_t ix = cellOf(p.x), iy = cellOf(p.y), iz = cellOf(p.z);
            bool dup = false;
            for (int dx = -1; dx <= 1 && !dup; ++dx)
                for (int dy = -1; dy <= 1 && !dup; ++dy)
                    for (int dz = -1; dz <= 1 && !dup; ++dz) {
                        const int64_t key = ((ix + dx) * 73856093LL) ^
                                            ((iy + dy) * 19349663LL) ^
                                            ((iz + dz) * 83492791LL);
                        auto it = grid.find(key);
                        if (it == grid.end())
                            continue;
                        for (int oi : it->second) {
                            const PointT& q = (*out)[(size_t)oi];
                            const float ddx = p.x - q.x, ddy = p.y - q.y, ddz = p.z - q.z;
                            if (ddx * ddx + ddy * ddy + ddz * ddz <= tol2) {
                                dup = true;
                                break;
                            }
                        }
                    }
            if (dup)
                continue;
            grid[((ix * 73856093LL) ^ (iy * 19349663LL) ^ (iz * 83492791LL))].push_back((int)out->size());
            out->push_back(p);
        }
    }
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = src->is_dense;
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterPassThrough(const CloudConstPtr& src, const std::string& field_name,
                                          double limit_min, double limit_max)
{
    if (!src)
        return nullptr;
    pcl::PassThrough<PointT> f;
    f.setInputCloud(src);
    f.setFilterFieldName(field_name);
    f.setFilterLimits((float)limit_min, (float)limit_max);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterStatisticalOutlier(const CloudConstPtr& src, int mean_k, double std_dev_mul)
{
    if (!src)
        return nullptr;
    pcl::StatisticalOutlierRemoval<PointT> f;
    f.setInputCloud(src);
    f.setMeanK(mean_k);
    f.setStddevMulThresh(std_dev_mul);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterRadiusOutlier(const CloudConstPtr& src, double radius, int min_neighbors)
{
    if (!src)
        return nullptr;
    pcl::RadiusOutlierRemoval<PointT> f;
    f.setInputCloud(src);
    f.setRadiusSearch(radius);
    f.setMinNeighborsInRadius(min_neighbors);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::filterConditionalXYZ(const CloudConstPtr& src,
                                             double x_min, double x_max,
                                             double y_min, double y_max,
                                             double z_min, double z_max)
{
    if (!src)
        return nullptr;
    // 条件：各轴范围（GT/LT 严格比较，即开区间）
    pcl::ConditionAnd<PointT>::Ptr cond(new pcl::ConditionAnd<PointT>);
    auto addRange = [&cond](const std::string& field, double lo, double hi) {
        pcl::FieldComparison<PointT>::Ptr c(new pcl::FieldComparison<PointT>(
            field, pcl::ComparisonOps::GT, (float)lo));
        pcl::FieldComparison<PointT>::Ptr c2(new pcl::FieldComparison<PointT>(
            field, pcl::ComparisonOps::LT, (float)hi));
        cond->addComparison(c);
        cond->addComparison(c2);
    };
    addRange("x", x_min, x_max);
    addRange("y", y_min, y_max);
    addRange("z", z_min, z_max);
    pcl::ConditionalRemoval<PointT> f;
    f.setInputCloud(src);
    f.setCondition(cond);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

// ---------------- extract ----------------

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::extractByIndices(const CloudConstPtr& src, const std::vector<int>& indices)
{
    if (!src || indices.empty())
        return nullptr;
    pcl::PointIndices::Ptr sel(new pcl::PointIndices);
    sel->indices = indices;
    pcl::ExtractIndices<PointT> f;
    f.setInputCloud(src);
    f.setIndices(sel);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::extractByIndicesInverted(const CloudConstPtr& src, const std::vector<int>& indices)
{
    if (!src)
        return nullptr;
    pcl::PointIndices::Ptr sel(new pcl::PointIndices);
    sel->indices = indices;
    pcl::ExtractIndices<PointT> f;
    f.setInputCloud(src);
    f.setIndices(sel);
    f.setNegative(true);
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::extractCropBox(const CloudConstPtr& src, const float min_corner[3],
                                       const float max_corner[3])
{
    if (!src)
        return nullptr;
    pcl::CropBox<PointT> f;
    f.setInputCloud(src);
    f.setMin(Eigen::Vector4f(std::min(min_corner[0], max_corner[0]),
                             std::min(min_corner[1], max_corner[1]),
                             std::min(min_corner[2], max_corner[2]), 1.0f));
    f.setMax(Eigen::Vector4f(std::max(min_corner[0], max_corner[0]),
                             std::max(min_corner[1], max_corner[1]),
                             std::max(min_corner[2], max_corner[2]), 1.0f));
    CloudPtr out(new pcl::PointCloud<PointT>);
    f.filter(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::extractCropOBB(const CloudConstPtr& src, const OBB3d& box, bool invert)
{
    if (!src || !box.valid)
        return nullptr;
    // 点变换到盒局部系：p_local = R · (p - center)，R 的行 = 主轴 → 直接点乘
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(src->size());
    for (const PointT& p : src->points) {
        const double dx = (double)p.x - box.center.x;
        const double dy = (double)p.y - box.center.y;
        const double dz = (double)p.z - box.center.z;
        bool inside = true;
        for (int d = 0; d < 3 && inside; ++d) {
            const double proj = dx * box.rotation[d * 3 + 0] +
                                dy * box.rotation[d * 3 + 1] +
                                dz * box.rotation[d * 3 + 2];
            if (std::fabs(proj) > box.extents[d])
                inside = false;
        }
        if (inside != invert)
            out->push_back(p);
    }
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = src->is_dense;
    return out;
}

// 2D 多边形 ROI 裁剪：射线法逐点判定（自包含，无 GEOS 依赖）
template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::extractByPolygonROI(const CloudConstPtr& src,
                                            const std::vector<Point2d>& polygon,
                                            bool invert)
{
    if (!src || polygon.size() < 3)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(src->size());
    for (const PointT& p : src->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y))
            continue;   // 非有限点自动剔除
        if (pointInPolygonXY(polygon, (double)p.x, (double)p.y) != invert)
            out->push_back(p);   // 整点拷贝（保留 rgb/intensity）
    }
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = src->is_dense;
    return out;
}

// ---------------- trans ----------------

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transApplyMatrix(const CloudConstPtr& src, const float matrix_4x4[16])
{
    if (!src)
        return nullptr;
    Eigen::Matrix4f m;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            m(r, c) = matrix_4x4[r * 4 + c];
    CloudPtr out(new pcl::PointCloud<PointT>);
    pcl::transformPointCloud(*src, *out, m);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transProject(const CloudConstPtr& src, const float plane_coeffs[4])
{
    // 投影：p' = p - (n·p + d)/|n|² · n
    if (!src)
        return nullptr;
    const double a = plane_coeffs[0], b = plane_coeffs[1], c = plane_coeffs[2], d = plane_coeffs[3];
    const double n2 = a * a + b * b + c * c;
    if (n2 <= 0)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->resize(src->size());
    for (size_t i = 0; i < src->size(); ++i) {
        const PointT& p = (*src)[i];
        const double t = (a * p.x + b * p.y + c * p.z + d) / n2;
        PointT& q = (*out)[i];
        q = p;   // 保留 rgb/intensity 等其余字段（PointXYZRGB 的 rgb 否则是未初始化值）
        q.x = (float)((double)p.x - t * a);
        q.y = (float)((double)p.y - t * b);
        q.z = (float)((double)p.z - t * c);
    }
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transTranslate(const CloudConstPtr& src, double dx, double dy, double dz)
{
    if (!src)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->resize(src->size());
    for (size_t i = 0; i < src->size(); ++i) {
        (*out)[i] = (*src)[i];
        (*out)[i].x += (float)dx;
        (*out)[i].y += (float)dy;
        (*out)[i].z += (float)dz;
    }
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transRotate(const CloudConstPtr& src, double axis_x, double axis_y,
                                    double axis_z, double angle)
{
    if (!src)
        return nullptr;
    const double len = std::sqrt(axis_x * axis_x + axis_y * axis_y + axis_z * axis_z);
    if (len < 1e-12)
        return nullptr;
    Eigen::Affine3f t = Eigen::Affine3f::Identity();
    t.rotate(Eigen::AngleAxisf((float)angle,
                               Eigen::Vector3f((float)(axis_x / len),
                                               (float)(axis_y / len),
                                               (float)(axis_z / len))));
    CloudPtr out(new pcl::PointCloud<PointT>);
    pcl::transformPointCloud(*src, *out, t);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transRotateAround(const CloudConstPtr& src, const float pivot[3],
                                          double axis_x, double axis_y, double axis_z, double angle)
{
    if (!src)
        return nullptr;
    const double len = std::sqrt(axis_x * axis_x + axis_y * axis_y + axis_z * axis_z);
    if (len < 1e-12)
        return nullptr;
    // T = Translate(+pivot) · Rotate · Translate(-pivot)
    Eigen::Affine3f t = Eigen::Affine3f::Identity();
    t.translate(Eigen::Vector3f(pivot[0], pivot[1], pivot[2]));
    t.rotate(Eigen::AngleAxisf((float)angle,
                               Eigen::Vector3f((float)(axis_x / len),
                                               (float)(axis_y / len),
                                               (float)(axis_z / len))));
    t.translate(Eigen::Vector3f(-pivot[0], -pivot[1], -pivot[2]));
    CloudPtr out(new pcl::PointCloud<PointT>);
    pcl::transformPointCloud(*src, *out, t);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transScale(const CloudConstPtr& src, const double scale_xyz[3])
{
    if (!src)
        return nullptr;
    Eigen::Affine3f t = Eigen::Affine3f::Identity();
    t.scale(Eigen::Vector3f((float)scale_xyz[0], (float)scale_xyz[1], (float)scale_xyz[2]));
    CloudPtr out(new pcl::PointCloud<PointT>);
    pcl::transformPointCloud(*src, *out, t);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::transScaleAround(const CloudConstPtr& src, const double scale[3],
                                         const float center[3])
{
    // p' = c + s·(p - c)，等价于先平移到原点、缩放、再平移回中心
    if (!src)
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->resize(src->size());
    for (size_t i = 0; i < src->size(); ++i) {
        const PointT& p = (*src)[i];
        PointT& q = (*out)[i];
        q = p;
        q.x = (float)(center[0] + scale[0] * ((double)p.x - center[0]));
        q.y = (float)(center[1] + scale[1] * ((double)p.y - center[1]));
        q.z = (float)(center[2] + scale[2] * ((double)p.z - center[2]));
    }
    return out;
}

// ---------------- feature ----------------

template <typename PointT>
pcl::PointCloud<pcl::Normal>::Ptr
CloudProcessor<PointT>::featureNormalEstimation(const CloudConstPtr& src, int k_search)
{
    if (!src || src->empty() || k_search < 3)
        return nullptr;
    pcl::NormalEstimation<PointT, pcl::Normal> ne;
    ne.setInputCloud(src);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    ne.setSearchMethod(tree);
    ne.setKSearch(k_search);
    pcl::PointCloud<pcl::Normal>::Ptr out(new pcl::PointCloud<pcl::Normal>);
    ne.compute(*out);
    return out;
}

template <typename PointT>
pcl::PointCloud<pcl::Normal>::Ptr
CloudProcessor<PointT>::featureNormalEstimationRadius(const CloudConstPtr& src, double radius,
                                                     int max_neighbors)
{
    if (!src || src->empty() || radius <= 0)
        return nullptr;
    // Open3D KDTreeSearchParamHybrid(radius, max_nn)：radius 内取邻居，最多 max_nn 个。
    // PCL 的 NormalEstimation 半径模式无上限参数，这里用 KdTree 手工实现上限语义；
    // 半径内不足 3 点无法拟合平面 → 法线置 NaN。
    const unsigned int max_nn = (unsigned int)std::max(3, max_neighbors);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    tree->setInputCloud(src);
    pcl::PointCloud<pcl::Normal>::Ptr out(new pcl::PointCloud<pcl::Normal>);
    out->resize(src->size());
    out->is_dense = true;

    std::vector<int> nn_indices;
    std::vector<float> nn_dists;
    for (size_t i = 0; i < src->size(); ++i) {
        pcl::Normal& n = (*out)[i];
        const int found = tree->radiusSearch((*src)[i], radius, nn_indices, nn_dists, max_nn);
        if (found < 3) {
            n.normal_x = n.normal_y = n.normal_z = std::numeric_limits<float>::quiet_NaN();
            n.curvature = std::numeric_limits<float>::quiet_NaN();
            out->is_dense = false;
            continue;
        }
        Eigen::Vector4f plane;
        float curvature = 0.0f;
        if (!pcl::computePointNormal(*src, nn_indices, plane, curvature)) {
            n.normal_x = n.normal_y = n.normal_z = std::numeric_limits<float>::quiet_NaN();
            n.curvature = std::numeric_limits<float>::quiet_NaN();
            out->is_dense = false;
            continue;
        }
        n.normal_x = plane[0];
        n.normal_y = plane[1];
        n.normal_z = plane[2];
        n.curvature = curvature;
    }
    return out;
}

template <typename PointT>
pcl::PointCloud<pcl::Normal>::Ptr
CloudProcessor<PointT>::featureOrientNormalsToDirection(
    const pcl::PointCloud<pcl::Normal>::ConstPtr& normals, const float reference_direction[3])
{
    if (!normals || normals->empty())
        return nullptr;
    const double len = std::sqrt((double)reference_direction[0] * reference_direction[0] +
                                 (double)reference_direction[1] * reference_direction[1] +
                                 (double)reference_direction[2] * reference_direction[2]);
    if (len < 1e-12)
        return nullptr;
    const float dx = (float)(reference_direction[0] / len);
    const float dy = (float)(reference_direction[1] / len);
    const float dz = (float)(reference_direction[2] / len);

    pcl::PointCloud<pcl::Normal>::Ptr out(new pcl::PointCloud<pcl::Normal>(*normals));
    for (pcl::Normal& n : out->points) {
        const float dot = n.normal_x * dx + n.normal_y * dy + n.normal_z * dz;
        if (dot < 0.0f) {
            n.normal_x = -n.normal_x;
            n.normal_y = -n.normal_y;
            n.normal_z = -n.normal_z;
        }
    }
    return out;
}

template <typename PointT>
pcl::PointCloud<pcl::Normal>::Ptr
CloudProcessor<PointT>::featureOrientNormalsTowardsPoint(
    const CloudConstPtr& src, const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
    const float viewpoint[3])
{
    if (!src || !normals || src->empty() || normals->size() != src->size())
        return nullptr;
    pcl::PointCloud<pcl::Normal>::Ptr out(new pcl::PointCloud<pcl::Normal>(*normals));
    for (size_t i = 0; i < out->size(); ++i) {
        pcl::Normal& n = (*out)[i];
        // 视点方向：viewpoint - point（flipNormalTowardsViewpoint 的向量）
        const float vx = viewpoint[0] - (*src)[i].x;
        const float vy = viewpoint[1] - (*src)[i].y;
        const float vz = viewpoint[2] - (*src)[i].z;
        const float dot = n.normal_x * vx + n.normal_y * vy + n.normal_z * vz;
        if (dot < 0.0f) {
            n.normal_x = -n.normal_x;
            n.normal_y = -n.normal_y;
            n.normal_z = -n.normal_z;
        }
    }
    return out;
}

template <typename PointT>
pcl::PointCloud<pcl::Normal>::Ptr
CloudProcessor<PointT>::featureOrientNormalsConsistentTangentPlane(
    const CloudConstPtr& src, const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
    int k_neighbors)
{
    if (!src || !normals || src->empty() || normals->size() != src->size() || k_neighbors < 2)
        return nullptr;
    const int n = (int)src->size();
    const int k = std::max(2, std::min(n, k_neighbors));

    // 1) 构建 KNN 邻接图（无向，边权 = 两点距离）
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    tree->setInputCloud(src);
    std::vector<std::vector<int>> adj((size_t)n);
    std::vector<int> nn_idx;
    std::vector<float> nn_dist;
    for (int i = 0; i < n; ++i) {
        tree->nearestKSearch((*src)[(size_t)i], k, nn_idx, nn_dist);
        for (int j : nn_idx)
            if (j != i)
                adj[(size_t)i].push_back(j);
    }

    // 2) Prim 最小生成树（优先队列版，O(E log V)；n 大时不能用 O(n²) 线性扫描）
    std::vector<char> visited((size_t)n, 0);
    std::vector<int> parent((size_t)n, -1);
    std::vector<float> best((size_t)n, std::numeric_limits<float>::max());
    using Edge = std::pair<float, int>;   // (到已选集合的最小平方距离, 点索引)
    std::priority_queue<Edge, std::vector<Edge>, std::greater<Edge>> pq;
    best[0] = 0.0f;
    pq.push({ 0.0f, 0 });

    std::vector<int> order;
    order.reserve((size_t)n);
    while (!pq.empty()) {
        const int u = pq.top().second;
        const float w = pq.top().first;
        pq.pop();
        if (visited[(size_t)u] || w > best[(size_t)u])
            continue;   // 过期队列项
        visited[(size_t)u] = 1;
        order.push_back(u);
        for (int v : adj[(size_t)u]) {
            if (visited[(size_t)v])
                continue;
            const float dx = (*src)[(size_t)u].x - (*src)[(size_t)v].x;
            const float dy = (*src)[(size_t)u].y - (*src)[(size_t)v].y;
            const float dz = (*src)[(size_t)u].z - (*src)[(size_t)v].z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best[(size_t)v]) {
                best[(size_t)v] = d;
                parent[(size_t)v] = u;
                pq.push({ d, v });
            }
        }
    }

    // 3) 沿 MST 自根向外传播（order 已是 Prim 的出队顺序 = 拓扑序）：
    //    子法线与父法线夹角 > 90° 则翻转
    pcl::PointCloud<pcl::Normal>::Ptr out(new pcl::PointCloud<pcl::Normal>(*normals));
    for (int v : order) {
        const int p = parent[(size_t)v];
        if (p < 0)
            continue;   // 根或不连通分量起点，保持原朝向
        const pcl::Normal& pn = (*out)[(size_t)p];
        pcl::Normal& pv = (*out)[(size_t)v];
        const float dot = pn.normal_x * pv.normal_x +
                          pn.normal_y * pv.normal_y +
                          pn.normal_z * pv.normal_z;
        if (dot < 0.0f) {
            pv.normal_x = -pv.normal_x;
            pv.normal_y = -pv.normal_y;
            pv.normal_z = -pv.normal_z;
        }
    }
    return out;
}

template <typename PointT>
pcl::PointCloud<pcl::FPFHSignature33>::Ptr
CloudProcessor<PointT>::featureFPFH(const CloudConstPtr& src,
                                    const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                    double radius_search)
{
    if (!src || !normals || src->empty() || radius_search <= 0 ||
        normals->size() != src->size())
        return nullptr;
    // 退化输入（点数 < 3 或法线全无效）PCL 会返回全零直方图且 is_dense=true，
    // 这种"看似合法"的描述子会让配准静默产生错误对应，故在接口层拦截。
    if (src->size() < 3)
        return nullptr;
    bool has_valid_normal = false;
    for (const pcl::Normal& n : normals->points)
        if (pcl_isfinite(n.normal_x) && pcl_isfinite(n.normal_y) && pcl_isfinite(n.normal_z) &&
            (n.normal_x != 0.0f || n.normal_y != 0.0f || n.normal_z != 0.0f)) {
            has_valid_normal = true;
            break;
        }
    if (!has_valid_normal)
        return nullptr;
    pcl::FPFHEstimation<PointT, pcl::Normal, pcl::FPFHSignature33> fpfh;
    fpfh.setInputCloud(src);
    fpfh.setInputNormals(normals);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    fpfh.setSearchMethod(tree);
    fpfh.setRadiusSearch(radius_search);
    pcl::PointCloud<pcl::FPFHSignature33>::Ptr out(new pcl::PointCloud<pcl::FPFHSignature33>);
    fpfh.compute(*out);
    return out;
}

template <typename PointT>
bool CloudProcessor<PointT>::featureCovariances(const CloudConstPtr& src, int k_search,
                                                std::vector<std::array<float, 9>>& out_covariances)
{
    out_covariances.clear();
    if (!src || src->empty() || k_search < 3)
        return false;
    const int k = std::min((int)src->size(), k_search);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    tree->setInputCloud(src);
    out_covariances.resize(src->size());

    std::vector<int> nn_indices;
    std::vector<float> nn_dists;
    for (size_t i = 0; i < src->size(); ++i) {
        std::array<float, 9>& cov = out_covariances[i];
        cov.fill(0.0f);
        // 含 NaN/Inf 的点不参与协方差（该点协方差保持 0）
        if (!pcl::isFinite((*src)[i])) {
            nn_indices.clear();
            continue;
        }
        tree->nearestKSearch((*src)[i], k, nn_indices, nn_dists);
        if (nn_indices.size() < 3)
            continue;
        Eigen::Matrix3f m;
        Eigen::Vector4f centroid;
        pcl::computeMeanAndCovarianceMatrix(*src, nn_indices, m, centroid);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                cov[(size_t)(r * 3 + c)] = m(r, c);
    }
    return true;
}

template <typename PointT>
bool CloudProcessor<PointT>::featureMahalanobisDistance(const CloudConstPtr& src,
                                                        std::vector<double>& out_distances)
{
    out_distances.clear();
    if (!src || src->size() < 4)
        return false;
    // 任一 NaN/Inf 坐标都会污染 mean/cov，使全部距离变成 NaN——先检查
    for (const PointT& p : src->points)
        if (!pcl::isFinite(p))
            return false;
    const int n = (int)src->size();
    Eigen::MatrixXd data(n, 3);
    for (int i = 0; i < n; ++i) {
        data(i, 0) = (*src)[(size_t)i].x;
        data(i, 1) = (*src)[(size_t)i].y;
        data(i, 2) = (*src)[(size_t)i].z;
    }
    const Eigen::Vector3d mean = data.colwise().mean();
    const Eigen::MatrixXd centered = data.rowwise() - mean.transpose();
    Eigen::Matrix3d cov = (centered.adjoint() * centered) / (double)std::max(1, n - 1);
    // 加微小正则，避免共面点云协方差奇异
    cov += Eigen::Matrix3d::Identity() * 1e-9;

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    if (es.info() != Eigen::Success)
        return false;
    const Eigen::Vector3d inv_sqrt = es.eigenvalues().cwiseSqrt().cwiseInverse();

    out_distances.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        const Eigen::Vector3d d = data.row(i) - mean.transpose();
        out_distances[(size_t)i] = (es.eigenvectors().transpose() * d).cwiseProduct(inv_sqrt).norm();
    }
    return true;
}

template <typename PointT>
std::vector<float> CloudProcessor<PointT>::featureNearestNeighborDistance(const CloudConstPtr& src, int k)
{
    std::vector<float> out;
    if (!src || src->empty())
        return out;
    // k < 1 视为 1（最近邻）；含自身需 k+1 个邻居，但不超过点数
    const int kk = std::max(1, std::min((int)src->size(), std::max(1, k) + 1));
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    tree->setInputCloud(src);
    out.resize(src->size());
    std::vector<int> nn_indices;
    std::vector<float> nn_dists;
    for (size_t i = 0; i < src->size(); ++i) {
        tree->nearestKSearch((*src)[i], kk, nn_indices, nn_dists);
        // nn_dists 为平方距离；取最后一个（第 kk 个邻居 = 第 k 近邻，跳过自身）→ sqrt
        out[i] = std::sqrt(std::max(0.0f, nn_dists[nn_dists.size() - 1]));
    }
    return out;
}

// ---------------- keypoint ----------------

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::keypointHarris3D(const CloudConstPtr& src, double radius, double threshold)
{
    // Harris3D 只支持 PointXYZ 输入（内部用强度），先转 XYZ
    if (!src || src->empty())
        return nullptr;
    pcl::PointCloud<pcl::PointXYZ>::Ptr xyz(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::copyPointCloud(*src, *xyz);
    pcl::HarrisKeypoint3D<pcl::PointXYZ, pcl::PointXYZI> harris;
    harris.setInputCloud(xyz);
    harris.setRadius((float)radius);
    harris.setThreshold((float)threshold);
    harris.setNonMaxSupression(true);
    pcl::PointCloud<pcl::PointXYZI>::Ptr kp(new pcl::PointCloud<pcl::PointXYZI>);
    harris.compute(*kp);
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->resize(kp->size());
    for (size_t i = 0; i < kp->size(); ++i)
        (*out)[i] = makePoint<PointT>((*kp)[i].x, (*kp)[i].y, (*kp)[i].z);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::keypointISS3D(const CloudConstPtr& src, double salient_radius, double non_max_radius)
{
    if (!src || src->empty())
        return nullptr;
    pcl::PointCloud<pcl::PointXYZ>::Ptr xyz(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::copyPointCloud(*src, *xyz);
    typename pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
    pcl::ISSKeypoint3D<pcl::PointXYZ, pcl::PointXYZ> iss;
    iss.setInputCloud(xyz);
    iss.setSearchMethod(tree);
    iss.setSalientRadius((float)salient_radius);
    iss.setNonMaxRadius((float)non_max_radius);
    pcl::PointCloud<pcl::PointXYZ> kp;
    iss.compute(kp);
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->resize(kp.size());
    for (size_t i = 0; i < kp.size(); ++i)
        (*out)[i] = makePoint<PointT>(kp[i].x, kp[i].y, kp[i].z);
    return out;
}

// ---------------- fit ----------------

template <typename PointT>
bool CloudProcessor<PointT>::fitPlane(const CloudConstPtr& src, FitMethod method,
                                      double distance_threshold,
                                      std::vector<float>& out_coeffs, std::vector<int>& out_inliers)
{
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return false;
    pcl::SACSegmentation<PointT> seg;
    seg.setInputCloud(clean);
    seg.setModelType(pcl::SACMODEL_PLANE);
    seg.setMethodType(toSacMethod(method));
    seg.setDistanceThreshold(distance_threshold);
    seg.setOptimizeCoefficients(true);
    pcl::PointIndices inliers;
    pcl::ModelCoefficients coeffs;
    seg.segment(inliers, coeffs);
    if (inliers.indices.empty())
        return false;
    out_coeffs = coeffs.values;
    out_inliers.clear();
    out_inliers.reserve(inliers.indices.size());
    for (int i : inliers.indices)
        out_inliers.push_back(global[(size_t)i]);   // 映射回原始 src 行号（inlier 索引契约）
    return true;
}

template <typename PointT>
bool CloudProcessor<PointT>::fitSphere(const CloudConstPtr& src, FitMethod method,
                                       double distance_threshold,
                                       std::vector<float>& out_center, double& out_radius,
                                       std::vector<int>& out_inliers)
{
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return false;
    pcl::SACSegmentation<PointT> seg;
    seg.setInputCloud(clean);
    seg.setModelType(pcl::SACMODEL_SPHERE);
    // PCL 1.9：球模型仅支持 RANSAC/LMEDS；MSAC 回退 RANSAC
    seg.setMethodType(method == FitMethod::MSAC ? (int)pcl::SAC_RANSAC : toSacMethod(method));
    seg.setDistanceThreshold(distance_threshold);
    seg.setOptimizeCoefficients(true);
    pcl::PointIndices inliers;
    pcl::ModelCoefficients coeffs;
    seg.segment(inliers, coeffs);
    if (inliers.indices.empty() || coeffs.values.size() < 4)
        return false;
    out_center.assign(coeffs.values.begin(), coeffs.values.begin() + 3);
    out_radius = coeffs.values[3];
    out_inliers.clear();
    out_inliers.reserve(inliers.indices.size());
    for (int i : inliers.indices)
        out_inliers.push_back(global[(size_t)i]);   // 映射回原始 src 行号
    return true;
}

template <typename PointT>
bool CloudProcessor<PointT>::fitCylinder(const CloudConstPtr& src, FitMethod method,
                                         double distance_threshold,
                                         std::vector<float>& out_axis, std::vector<float>& out_point,
                                         double& out_radius, std::vector<int>& out_inliers)
{
    if (!src || src->empty())
        return false;
    // PCL 1.9 的 SampleConsensusModelCylinder 强制要求法线（computeModelCoefficients 里
    // `if (!normals_) return false`），必须走 SACSegmentationFromNormals，否则永远 0 局内点。
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return false;
    pcl::PointCloud<pcl::Normal>::Ptr normals = featureNormalEstimation(clean, 20);
    if (!normals || normals->size() != clean->size())
        return false;
    pcl::SACSegmentationFromNormals<PointT, pcl::Normal> seg;
    seg.setInputCloud(clean);
    seg.setInputNormals(normals);
    seg.setModelType(pcl::SACMODEL_CYLINDER);
    // PCL 1.9：圆柱模型仅支持 RANSAC
    seg.setMethodType(pcl::SAC_RANSAC);
    (void)method;
    seg.setNormalDistanceWeight(0.1);
    seg.setDistanceThreshold(distance_threshold);
    seg.setOptimizeCoefficients(true);
    // 注意：不要设置 setSamplesMaxDist——它把第二采样点限制在第一点的邻域内，
    // 而圆柱面上近邻两点的法线近似平行，computeModelCoefficients 的
    // denominator = a*c - b*b ≈ 0 会退化，导致所有采样失败（0 局内点）。
    // 圆柱模型每次只需 2 个点，全云随机采样反而更有效。
    seg.setMaxIterations(2000);
    pcl::PointIndices inliers;
    pcl::ModelCoefficients coeffs;
    seg.segment(inliers, coeffs);
    // coeffs = [point_on_axis(x,y,z), axis_direction(x,y,z), radius]
    if (inliers.indices.empty() || coeffs.values.size() < 7)
        return false;
    out_point.assign(coeffs.values.begin(), coeffs.values.begin() + 3);
    out_axis.assign(coeffs.values.begin() + 3, coeffs.values.begin() + 6);
    out_radius = coeffs.values[6];
    out_inliers.clear();
    out_inliers.reserve(inliers.indices.size());
    for (int i : inliers.indices)
        out_inliers.push_back(global[(size_t)i]);   // 映射回原始 src 行号
    return true;
}

// 3D 空间圆拟合：RANSAC 主平面 → 投影 → 2D 圆拟合（共享核）→ 回投 3D
template <typename PointT>
bool CloudProcessor<PointT>::fitCircle3D(const CloudConstPtr& src, FitMethod method,
                                         double distance_threshold, Point3d& out_center,
                                         float out_normal[3], double& out_radius,
                                         std::vector<int>& out_inliers, double& out_rms)
{
    out_center = Point3d();
    out_inliers.clear();
    out_radius = 0.0;
    out_rms = 0.0;
    if (out_normal) {
        out_normal[0] = out_normal[1] = out_normal[2] = 0.f;
    }
    if (!src || src->size() < 3)
        return false;
    // 拟合入口守卫：剔除非有限点（NaN 进 RANSAC 在 Debug 触发 PCL 断言），
    // 内点索引经 global 映射回原始行号
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return false;

    // 1) RANSAC 主平面（|n|=1 契约）
    std::vector<float> coeffs;
    std::vector<int> planeInliers;
    if (!fitPlane(clean, method, distance_threshold, coeffs, planeInliers) ||
        planeInliers.size() < 3)
        return false;
    const Eigen::Vector3f n(coeffs[0], coeffs[1], coeffs[2]);
    const double d0 = (double)coeffs[3];

    // 2) 平面内正交基：u ⊥ n（aux 取与 n 夹角小的坐标轴叉积），v = n × u
    const Eigen::Vector3f aux = (std::fabs(n.x()) < 0.9f) ? Eigen::Vector3f(1.f, 0.f, 0.f)
                                                          : Eigen::Vector3f(0.f, 1.f, 0.f);
    const Eigen::Vector3f u = n.cross(aux).normalized();
    const Eigen::Vector3f v = n.cross(u).normalized();

    // 3) 平面局内点投影（q = p − (n·p+d)·n）→ 2D 坐标（u/v 投影）
    std::vector<double> xy;
    xy.reserve((size_t)planeInliers.size() * 2);
    for (int idx : planeInliers) {
        const PointT& p = (*clean)[(size_t)idx];
        const double dist = (double)n.x() * p.x + (double)n.y() * p.y +
                            (double)n.z() * p.z + d0;
        const double qx = (double)p.x - dist * (double)n.x();
        const double qy = (double)p.y - dist * (double)n.y();
        const double qz = (double)p.z - dist * (double)n.z();
        xy.push_back((double)u.x() * qx + (double)u.y() * qy + (double)u.z() * qz);
        xy.push_back((double)v.x() * qx + (double)v.y() * qy + (double)v.z() * qz);
    }

    // 4) 2D 圆几何最小二乘（FitUtil 共享核）
    double cu = 0, cv = 0, r = 0, rms = 0;
    if (!detail::fitCircle2D(xy, cu, cv, r, rms))
        return false;

    // 5) 回投：center = cu·u + cv·v + (−d0)·n（圆心在平面上：n·center + d0 = 0）
    const Eigen::Vector3f center = cu * u + cv * v + (float)(-d0) * n;
    out_center = Point3d{ (double)center.x(), (double)center.y(), (double)center.z() };
    if (out_normal) {
        out_normal[0] = n.x();
        out_normal[1] = n.y();
        out_normal[2] = n.z();
    }
    out_radius = r;

    // 6) 圆周内点（平面局内点中，平面内距圆周 ≤ 同一阈值者）
    double ss = 0;
    for (size_t k = 0; k < planeInliers.size(); ++k) {
        const double du = xy[k * 2] - cu, dv = xy[k * 2 + 1] - cv;
        const double res = std::fabs(std::sqrt(du * du + dv * dv) - r);
        if (res <= distance_threshold) {
            out_inliers.push_back(planeInliers[k]);
            ss += res * res;
        }
    }
    if (out_inliers.size() < 3)
        return false;   // 圆退化（如平面点不成环）
    out_rms = std::sqrt(ss / (double)out_inliers.size());
    return true;
}

// 圆度：fitCircle3D 后按法向重建平面基，圆周局内点径向残差峰峰值
template <typename PointT>
bool CloudProcessor<PointT>::measureCircleRoundness(const CloudConstPtr& src, FitMethod method,
                                                    double distance_threshold,
                                                    double& out_roundness, Point3d& out_center,
                                                    float out_normal[3], double& out_radius)
{
    out_roundness = 0.0;
    std::vector<int> inliers;
    double rms = 0;
    if (!fitCircle3D(src, method, distance_threshold, out_center, out_normal, out_radius,
                     inliers, rms))
        return false;
    if (inliers.size() < 2)
        return false;
    const Eigen::Vector3f n(out_normal[0], out_normal[1], out_normal[2]);
    const Eigen::Vector3f aux = (std::fabs(n.x()) < 0.9f) ? Eigen::Vector3f(1.f, 0.f, 0.f)
                                                          : Eigen::Vector3f(0.f, 1.f, 0.f);
    const Eigen::Vector3f u = n.cross(aux).normalized();
    const Eigen::Vector3f v = n.cross(u).normalized();
    const Eigen::Vector3f c((float)out_center.x, (float)out_center.y, (float)out_center.z);
    double mn = 1e300, mx = -1e300;
    for (int idx : inliers) {
        const PointT& p = (*src)[(size_t)idx];
        const Eigen::Vector3f rel(p.x - c.x(), p.y - c.y(), p.z - c.z());
        const double du = (double)rel.dot(u), dv = (double)rel.dot(v);
        const double res = std::fabs(std::sqrt(du * du + dv * dv) - out_radius);
        mn = (std::min)(mn, res);
        mx = (std::max)(mx, res);
    }
    out_roundness = mx - mn;
    return true;
}

// 圆柱度：fitCylinder 后局内点到轴距离与半径之差的峰峰值
template <typename PointT>
bool CloudProcessor<PointT>::measureCylinderForm(const CloudConstPtr& src, FitMethod method,
                                                 double distance_threshold, double& out_form,
                                                 std::vector<float>& out_axis,
                                                 std::vector<float>& out_point,
                                                 double& out_radius)
{
    out_form = 0.0;
    std::vector<int> inliers;
    if (!fitCylinder(src, method, distance_threshold, out_axis, out_point, out_radius, inliers))
        return false;
    if (inliers.size() < 2)
        return false;
    const Eigen::Vector3f p0(out_point[0], out_point[1], out_point[2]);
    Eigen::Vector3f ax(out_axis[0], out_axis[1], out_axis[2]);
    if (ax.norm() < 1e-12)
        return false;
    ax.normalize();
    double mn = 1e300, mx = -1e300;
    for (int idx : inliers) {
        const PointT& p = (*src)[(size_t)idx];
        const Eigen::Vector3f rel(p.x - p0.x(), p.y - p0.y(), p.z - p0.z());
        const double h = (double)rel.dot(ax);
        const double radial = std::sqrt((std::max)(0.0, (double)rel.squaredNorm() - h * h));
        const double res = std::fabs(radial - out_radius);
        mn = (std::min)(mn, res);
        mx = (std::max)(mx, res);
    }
    out_form = mx - mn;
    return true;
}

// 圆锥拟合：直接构造 SampleConsensusModelCone + RANSAC
//（绕开 PCL 1.9 缺陷：SACSegmentationFromNormals 的 CONE 分支用自身默认值 0/0
//  覆盖模型开角约束，isModelValid 退化为 opening==0，任何真实圆锥都 0 局内点）
template <typename PointT>
bool CloudProcessor<PointT>::fitCone(const CloudConstPtr& src, FitMethod method,
                                     double distance_threshold, std::vector<float>& out_apex,
                                     std::vector<float>& out_axis, double& out_angle,
                                     std::vector<int>& out_inliers)
{
    out_apex.clear();
    out_axis.clear();
    out_angle = 0.0;
    out_inliers.clear();
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return false;
    // 圆锥模型强制要求法线（轴由三点法线差叉积导出、顶点由三切平面交解析求得）；
    // KNN 法线无定向不影响——切平面 (p, n) 与 n 的符号无关。
    pcl::PointCloud<pcl::Normal>::Ptr normals = featureNormalEstimation(clean, 20);
    if (!normals || normals->size() != clean->size())
        return false;

    std::vector<int> full((size_t)clean->size());
    for (size_t i = 0; i < full.size(); ++i)
        full[i] = (int)i;
    pcl::SampleConsensusModelCone<PointT, pcl::Normal>::Ptr model(
        new pcl::SampleConsensusModelCone<PointT, pcl::Normal>(clean, full, /*random=*/true));
    model->setInputNormals(normals);
    (void)method;   // PCL 1.9 圆锥模型仅 RANSAC
    pcl::RandomSampleConsensus<PointT> ransac(model, distance_threshold);
    ransac.setMaxIterations(10000);
    if (!ransac.computeModel())
        return false;
    std::vector<int> ransacInliers;
    ransac.getInliers(ransacInliers);
    if (ransacInliers.size() < 3)
        return false;
    Eigen::VectorXf raw;
    ransac.getModelCoefficients(raw);
    if (raw.size() < 7)
        return false;
    // 局内点 LM 精化（圆锥版返回 void，失败时 refined 保持原值），再重算内点口径
    Eigen::VectorXf refined = raw;
    model->optimizeModelCoefficients(ransacInliers, raw, refined);
    if (refined.size() >= 7)
        raw = refined;
    std::vector<int> cleanInliers;
    model->selectWithinDistance(raw, distance_threshold, cleanInliers);
    if (cleanInliers.size() < 3)
        return false;
    out_inliers.reserve(cleanInliers.size());
    for (int i : cleanInliers)
        out_inliers.push_back(global[(size_t)i]);   // 映射回原始 src 行号
    out_apex.assign(raw.data(), raw.data() + 3);
    out_axis.assign(raw.data() + 3, raw.data() + 6);
    out_angle = raw[6];
    return true;
}

// ---------------- segment ----------------

template <typename PointT>
bool CloudProcessor<PointT>::segmentPlaneRANSAC(const CloudConstPtr& src, double distance_threshold,
                                               CloudPtr& out_inliers, CloudPtr& out_outliers,
                                               std::vector<float>& out_coeffs)
{
    std::vector<int> inlierIdx;
    if (!fitPlane(src, FitMethod::RANSAC, distance_threshold, out_coeffs, inlierIdx))
        return false;
    out_inliers = extractByIndices(src, inlierIdx);
    // 补集（空时输出空点云而非 nullptr，调用方免判空）
    std::vector<int> outIdx;
    if (inlierIdx.size() < src->size()) {
        std::set<int> inset(inlierIdx.begin(), inlierIdx.end());
        for (int i = 0; i < (int)src->size(); ++i)
            if (!inset.count(i))
                outIdx.push_back(i);
    }
    if (outIdx.empty()) {
        out_outliers.reset(new pcl::PointCloud<PointT>);
    } else {
        out_outliers = extractByIndices(src, outIdx);
    }
    return true;
}

template <typename PointT>
std::vector<typename CloudProcessor<PointT>::CloudPtr>
CloudProcessor<PointT>::segmentEuclideanCluster(const CloudConstPtr& src, double cluster_tolerance,
                                                int min_size, int max_size)
{
    std::vector<CloudPtr> out;
    if (!src || src->empty())
        return out;
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return out;
    pcl::EuclideanClusterExtraction<PointT> ec;
    ec.setInputCloud(clean);
    ec.setClusterTolerance(cluster_tolerance);
    ec.setMinClusterSize(min_size);
    ec.setMaxClusterSize(max_size);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    ec.setSearchMethod(tree);
    std::vector<pcl::PointIndices> clusters;
    ec.extract(clusters);
    // 簇索引映射回原始 src 行号（clean 点 j = 原始行 global[j]）
    for (pcl::PointIndices& c : clusters) {
        for (int& i : c.indices)
            i = global[(size_t)i];
    }
    out.reserve(clusters.size());
    for (const pcl::PointIndices& c : clusters)
        out.push_back(extractByIndices(src, c.indices));
    // 按点数降序
    std::sort(out.begin(), out.end(),
              [](const CloudPtr& a, const CloudPtr& b) { return a->size() > b->size(); });
    return out;
}

template <typename PointT>
int CloudProcessor<PointT>::segmentDBSCAN(const CloudConstPtr& src, double eps, int min_points,
                                         std::vector<int>& out_labels)
{
    out_labels.clear();
    if (!src || src->empty() || eps <= 0 || min_points < 1)
        return 0;
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return 0;

    const int n = (int)clean->size();
    out_labels.assign((size_t)n, -1);
    // 标签按 clean 对齐，函数尾部映射回原始 src 行号（见 return 前）

    // 1) 半径搜索缓存每个点的邻居，并标记核心点（含自身的邻居数 >= min_points）
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    tree->setInputCloud(clean);
    std::vector<std::vector<int>> neighbors((size_t)n);
    std::vector<std::vector<float>> neighbor_dists((size_t)n);
    std::vector<char> is_core((size_t)n, 0);
    for (int i = 0; i < n; ++i) {
        tree->radiusSearch((*clean)[(size_t)i], eps, neighbors[(size_t)i], neighbor_dists[(size_t)i]);
        if ((int)neighbors[(size_t)i].size() >= min_points)
            is_core[(size_t)i] = 1;
    }

    // 2) 核心点之间按邻域连通性并查集合并（PCL 1.9 无 DBSCAN，这里手工实现）
    UnionFind uf((size_t)n);
    for (int i = 0; i < n; ++i) {
        if (!is_core[(size_t)i])
            continue;
        for (int j : neighbors[(size_t)i])
            if (is_core[(size_t)j])
                uf.unite(i, j);
    }

    // 3) 边界点吸附到最近的核心簇；无核心邻居则为噪声（-1）
    std::map<int, std::vector<int>> clusters;   // root → 点索引
    for (int i = 0; i < n; ++i) {
        if (is_core[(size_t)i]) {
            clusters[uf.find(i)].push_back(i);
            continue;
        }
        int best_root = -1;
        float best_d = std::numeric_limits<float>::max();
        const std::vector<int>& nn = neighbors[(size_t)i];
        const std::vector<float>& nd = neighbor_dists[(size_t)i];
        for (size_t k = 0; k < nn.size(); ++k) {
            const int j = nn[k];
            if (is_core[(size_t)j] && nd[k] < best_d) {
                best_d = nd[k];
                best_root = uf.find(j);
            }
        }
        if (best_root >= 0)
            clusters[best_root].push_back(i);
    }

    // 4) 按簇大小降序编号（大簇 = 0）
    std::vector<std::pair<size_t, std::vector<int>>> ordered(clusters.begin(), clusters.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const std::pair<size_t, std::vector<int>>& a,
                 const std::pair<size_t, std::vector<int>>& b) {
                  return a.second.size() > b.second.size();
              });
    for (size_t ci = 0; ci < ordered.size(); ++ci)
        for (int idx : ordered[ci].second)
            out_labels[(size_t)idx] = (int)ci;
    return (int)ordered.size();
}

template <typename PointT>
std::vector<std::vector<int>>
CloudProcessor<PointT>::segmentRegionGrowing(const CloudConstPtr& src,
                                             const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                             int k_neighbors, double smoothness_deg,
                                             double curvature_thresh,
                                             int min_size, int max_size)
{
    std::vector<std::vector<int>> out;
    if (!src || src->empty() || !normals || normals->size() != src->size())
        return out;
    if (k_neighbors < 3 || smoothness_deg <= 0)
        return out;

    pcl::RegionGrowing<PointT, pcl::Normal> rg;
    rg.setMinClusterSize(std::max(1, min_size));
    rg.setMaxClusterSize(std::max(min_size, max_size));
    rg.setSmoothModeFlag(true);
    rg.setCurvatureTestFlag(true);
    rg.setSmoothnessThreshold((float)(smoothness_deg * M_PI / 180.0));
    rg.setCurvatureThreshold((float)curvature_thresh);
    rg.setNumberOfNeighbours((unsigned int)k_neighbors);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    rg.setSearchMethod(tree);
    rg.setInputCloud(src);
    // PCL 1.9 的 setInputNormals 只收非 const 指针，这里拷贝一份（法线数据量小）
    typename pcl::PointCloud<pcl::Normal>::Ptr normals_copy(
        new pcl::PointCloud<pcl::Normal>(*normals));
    rg.setInputNormals(normals_copy);

    std::vector<pcl::PointIndices> clusters;
    rg.extract(clusters);
    out.reserve(clusters.size());
    for (const pcl::PointIndices& c : clusters)
        out.push_back(c.indices);
    std::sort(out.begin(), out.end(),
              [](const std::vector<int>& a, const std::vector<int>& b) {
                  return a.size() > b.size();
              });
    return out;
}

// 多平面迭代分割：RANSAC → 剔局内点 → 重复（active 索引表映射回原始 src）
template <typename PointT>
int CloudProcessor<PointT>::segmentPlanesMulti(const CloudConstPtr& src,
                                               double distance_threshold, int max_planes,
                                               int min_inliers,
                                               std::vector<std::vector<float>>& out_coeffs,
                                               std::vector<std::vector<int>>& out_inliers)
{
    out_coeffs.clear();
    out_inliers.clear();
    if (!src || src->size() < 3 || max_planes < 1 || min_inliers < 3)
        return 0;
    std::vector<int> active((size_t)src->size());
    for (size_t i = 0; i < active.size(); ++i)
        active[i] = (int)i;
    int found = 0;
    while (found < max_planes && (int)active.size() >= min_inliers) {
        CloudPtr remaining = extractByIndices(src, active);
        std::vector<float> coeffs;
        std::vector<int> local;
        if (!fitPlane(remaining, FitMethod::RANSAC, distance_threshold, coeffs, local) ||
            (int)local.size() < min_inliers)
            break;
        // 局内点局部索引 → 原始 src 全局索引（fitPlane 返回升序，global 亦升序）
        std::vector<int> global;
        global.reserve(local.size());
        for (int li : local)
            global.push_back(active[(size_t)li]);
        out_coeffs.push_back(coeffs);
        out_inliers.push_back(global);
        ++found;
        // 剔除本轮局内点
        std::set<int> removed(global.begin(), global.end());
        std::vector<int> next;
        next.reserve(active.size() - global.size());
        for (int idx : active)
            if (removed.find(idx) == removed.end())
                next.push_back(idx);
        active.swap(next);
    }
    return found;
}

// ---------------- regist ----------------

template <typename PointT>
bool CloudProcessor<PointT>::registICP_PointToPoint(const CloudConstPtr& source, const CloudConstPtr& target,
                                                    double max_dist, float out_transform[16],
                                                    const float* init_transform)
{
    if (!source || !target)
        return false;
    pcl::IterativeClosestPoint<PointT, PointT> icp;
    icp.setInputSource(source);
    icp.setInputTarget(target);
    icp.setMaxCorrespondenceDistance((float)max_dist);
    pcl::PointCloud<PointT> aligned;
    if (init_transform) {
        Eigen::Matrix4f init;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                init(r, c) = init_transform[r * 4 + c];
        icp.align(aligned, init);
    } else {
        icp.align(aligned);
    }
    if (!icp.hasConverged())
        return false;
    return matToArray(icp.getFinalTransformation(), out_transform);
}

template <typename PointT>
bool CloudProcessor<PointT>::registICP_PointToPlane(const CloudConstPtr& source, const CloudConstPtr& target,
                                                    const pcl::PointCloud<pcl::Normal>::Ptr& target_normals,
                                                    double max_dist, float out_transform[16],
                                                    const float* init_transform)
{
    if (!source || !target || !target_normals)
        return false;
    if (target_normals->size() != target->size())
        return false;
    // 点到面 ICP 需要 target 带法线（Normal 拼到 XYZ 字段）。
    // 退化邻域会让 computePointNormal 输出 NaN，NaN 法线会污染整个法方程，
    // 最终 hasConverged() 仍为 true 但变换矩阵全是 NaN —— 必须在此剔除。
    pcl::PointCloud<pcl::PointNormal>::Ptr targetN(new pcl::PointCloud<pcl::PointNormal>);
    pcl::copyPointCloud(*target, *targetN);
    size_t valid = 0;
    for (size_t i = 0; i < targetN->size(); ++i) {
        const pcl::Normal& n = (*target_normals)[i];
        if (pcl_isfinite(n.normal_x) && pcl_isfinite(n.normal_y) && pcl_isfinite(n.normal_z)) {
            (*targetN)[i].normal_x = n.normal_x;
            (*targetN)[i].normal_y = n.normal_y;
            (*targetN)[i].normal_z = n.normal_z;
            (*targetN)[i].curvature = n.curvature;
            ++valid;
        } else {
            (*targetN)[i].normal_x = (*targetN)[i].normal_y = (*targetN)[i].normal_z = 0.0f;
            (*targetN)[i].curvature = 0.0f;
        }
    }
    if (valid < 3)
        return false;
    // 薄几何预判：点到面 LLS 对完美平面等退化目标奇异，PCL 迭代内部会产出 NaN
    // 并以 NaN 点调用 nearestKSearch → Debug 触发 PCL KdTree 断言（Release 由下方
    // matToArray 拦截后回退点到点，但断言先发生）。预判直接走点到点回退。
    {
        double mnz[3] = { 1e300, 1e300, 1e300 }, mxz[3] = { -1e300, -1e300, -1e300 };
        bool any = false;
        for (const pcl::PointNormal& p : targetN->points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                continue;
            const double c[3] = { p.x, p.y, p.z };
            for (int k = 0; k < 3; ++k) {
                mnz[k] = (std::min)(mnz[k], c[k]);
                mxz[k] = (std::max)(mxz[k], c[k]);
            }
            any = true;
        }
        if (any) {
            const double ex = mxz[0] - mnz[0], ey = mxz[1] - mnz[1], ez = mxz[2] - mnz[2];
            const double largest = (std::max)({ex, ey, ez, 1e-12});
            const double thinnest = (std::min)({ex, ey, ez});
            if ((int)targetN->size() < 6 || thinnest <= 1e-6 * largest)
                return registICP_PointToPoint(source, target, max_dist, out_transform,
                                              init_transform);
        }
    }
    // source 侧同样用 PointNormal：ICPWithNormals 会探测 source 是否带 normal 字段，
    // 带则逐点旋转法线。未初始化内存会被当成有效法线，必须显式清零。
    pcl::PointCloud<pcl::PointNormal>::Ptr sourceN(new pcl::PointCloud<pcl::PointNormal>);
    pcl::copyPointCloud(*source, *sourceN);
    for (pcl::PointNormal& p : sourceN->points) {
        p.normal_x = 0.0f;
        p.normal_y = 0.0f;
        p.normal_z = 0.0f;
        p.curvature = 0.0f;
    }
    pcl::IterativeClosestPointWithNormals<pcl::PointNormal, pcl::PointNormal> icp;
    icp.setInputSource(sourceN);
    icp.setInputTarget(targetN);
    icp.setMaxCorrespondenceDistance((float)max_dist);
    pcl::PointCloud<pcl::PointNormal> aligned;
    icp.align(aligned);

    // PCL 1.9 的 PointToPlaneLLS 在退化几何（如纯平面）上法方程奇异，
    // hasConverged() 仍为 true 但变换矩阵为 NaN。此时回退到点到点 ICP。
    Eigen::Matrix4f T;
    bool valid_transform = icp.hasConverged() && matToArray(icp.getFinalTransformation(), out_transform);
    if (valid_transform)
        return true;
    pcl::IterativeClosestPoint<PointT, PointT> fallback;
    fallback.setInputSource(source);
    fallback.setInputTarget(target);
    fallback.setMaxCorrespondenceDistance((float)max_dist);
    pcl::PointCloud<PointT> aligned_pp;
    fallback.align(aligned_pp);
    if (!fallback.hasConverged())
        return false;
    return matToArray(fallback.getFinalTransformation(), out_transform);
}

template <typename PointT>
bool CloudProcessor<PointT>::registNDT(const CloudConstPtr& source, const CloudConstPtr& target,
                                       double voxel_size, float out_transform[16])
{
    if (!source || !target)
        return false;
    pcl::NormalDistributionsTransform<PointT, PointT> ndt;
    ndt.setInputSource(source);
    ndt.setInputTarget(target);
    ndt.setResolution((float)voxel_size);
    pcl::PointCloud<PointT> aligned;
    ndt.align(aligned);
    if (!ndt.hasConverged())
        return false;
    return matToArray(ndt.getFinalTransformation(), out_transform);
}

template <typename PointT>
bool CloudProcessor<PointT>::registPCA(const CloudConstPtr& source, const CloudConstPtr& target,
                                       float out_transform[16])
{
    // PCA 粗配准：两片点云的主轴对齐（质心平移 + 主轴旋转）
    if (!source || !target || source->empty() || target->empty())
        return false;
    auto pcaAxes = [](const CloudConstPtr& c, Eigen::Vector4f& centroid) {
        pcl::compute3DCentroid(*c, centroid);
        Eigen::MatrixXf d(c->size(), 3);
        for (size_t i = 0; i < c->size(); ++i) {
            d((Eigen::Index)i, 0) = (*c)[i].x - centroid[0];
            d((Eigen::Index)i, 1) = (*c)[i].y - centroid[1];
            d((Eigen::Index)i, 2) = (*c)[i].z - centroid[2];
        }
        Eigen::Matrix3f cov = (d.adjoint() * d) / (float)std::max<size_t>(1, c->size() - 1);
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> es(cov);
        // 特征值升序 → 主轴降序
        Eigen::Matrix3f axes = es.eigenvectors();
        Eigen::Vector3f order = es.eigenvalues();
        for (int i = 0; i < 2; ++i)
            for (int j = i + 1; j < 3; ++j)
                if (order[j] > order[i]) {
                    std::swap(order[i], order[j]);
                    axes.col(i).swap(axes.col(j));
                }
        return axes;
    };
    Eigen::Vector4f cs, ct;
    const Eigen::Matrix3f As = pcaAxes(source, cs);
    const Eigen::Matrix3f At = pcaAxes(target, ct);
    // R = At · As⁻¹（把 source 主轴转到 target 主轴）
    const Eigen::Matrix3f R = At * As.transpose();
    const Eigen::Vector3f t = ct.head<3>() - R * cs.head<3>();
    Eigen::Matrix4f T = Eigen::Matrix4f::Identity();
    T.block<3, 3>(0, 0) = R;
    T.block<3, 1>(0, 3) = t;
    return matToArray(T, out_transform);
}

template <typename PointT>
bool CloudProcessor<PointT>::registGICP(const CloudConstPtr& source, const CloudConstPtr& target,
                                        double max_dist, float out_transform[16])
{
    if (!source || !target || source->empty() || target->empty())
        return false;
    // GICP 内部按协方差做各向异性加权，对平面/圆柱件比点到点 ICP 更稳。
    // PCL 1.9 的 GICP 只预编译了 PointXYZRGBA，其他点型走头文件实现（本库白名单均可编译）。
    pcl::GeneralizedIterativeClosestPoint<PointT, PointT> gicp;
    gicp.setInputSource(source);
    gicp.setInputTarget(target);
    gicp.setMaxCorrespondenceDistance((float)max_dist);
    pcl::PointCloud<PointT> aligned;
    gicp.align(aligned);
    if (!gicp.hasConverged())
        return false;
    return matToArray(gicp.getFinalTransformation(), out_transform);
}

template <typename PointT>
bool CloudProcessor<PointT>::registFeatureMatching(
    const CloudConstPtr& source, const CloudConstPtr& target,
    const pcl::PointCloud<pcl::FPFHSignature33>::ConstPtr& source_features,
    const pcl::PointCloud<pcl::FPFHSignature33>::ConstPtr& target_features,
    double min_sample_distance, double max_correspondence_distance, float out_transform[16])
{
    if (!source || !target || !source_features || !target_features ||
        source->empty() || target->empty())
        return false;
    if (source_features->size() != source->size() || target_features->size() != target->size())
        return false;

    // FPFH 对找不到邻居的点输出全 NaN 直方图（PCL fpfh.hpp:250），而 SAC-IA 在
    // NaN 特征上会段错误（实测）。入口校验：任一特征含 NaN/Inf 直接拒绝。
    auto hasNanFeature = [](const pcl::PointCloud<pcl::FPFHSignature33>& f) {
        for (const pcl::FPFHSignature33& s : f.points)
            for (int d = 0; d < 33; ++d)
                if (!std::isfinite(s.histogram[d]))
                    return true;
        return false;
    };
    if (hasNanFeature(*source_features) || hasNanFeature(*target_features))
        return false;

    // SAC-IA：在 FPFH 描述子空间随机采样 3 对对应点 → SVD 估变换 → 打分取最优
    pcl::SampleConsensusInitialAlignment<PointT, PointT, pcl::FPFHSignature33> sac_ia;
    sac_ia.setInputSource(source);
    sac_ia.setSourceFeatures(source_features);
    sac_ia.setInputTarget(target);
    sac_ia.setTargetFeatures(target_features);
    sac_ia.setMinSampleDistance((float)min_sample_distance);
    sac_ia.setMaxCorrespondenceDistance((float)max_correspondence_distance);
    sac_ia.setMaximumIterations(500);
    sac_ia.setNumberOfSamples(3);
    sac_ia.setCorrespondenceRandomness(5);
    pcl::PointCloud<PointT> aligned;
    sac_ia.align(aligned);
    if (!sac_ia.hasConverged())
        return false;
    return matToArray(sac_ia.getFinalTransformation(), out_transform);
}

template <typename PointT>
bool CloudProcessor<PointT>::registEvaluate(const CloudConstPtr& source, const CloudConstPtr& target,
                                            const float transform[16], double max_dist,
                                            double& out_fitness, double& out_inlier_rmse)
{
    out_fitness = 0.0;
    out_inlier_rmse = 0.0;
    if (!source || !target || source->empty() || target->empty())
        return false;

    // 变换 source 后逐点查 target 最近邻；距离 ≤ max_dist 记为内点
    Eigen::Matrix4f T;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            T(r, c) = transform[r * 4 + c];
    pcl::PointCloud<PointT> transformed;
    pcl::transformPointCloud(*source, transformed, T);

    pcl::KdTreeFLANN<PointT> tree;
    tree.setInputCloud(target);
    std::vector<int> nn_idx(1);
    std::vector<float> nn_dist(1);
    size_t inliers = 0;
    double sq_sum = 0.0;
    const float max2 = (float)(max_dist * max_dist);
    for (const PointT& p : transformed.points) {
        if (tree.nearestKSearch(p, 1, nn_idx, nn_dist) <= 0)
            continue;
        if (nn_dist[0] <= max2) {
            ++inliers;
            sq_sum += nn_dist[0];
        }
    }
    out_fitness = (double)inliers / (double)source->size();
    out_inlier_rmse = inliers > 0 ? std::sqrt(sq_sum / (double)inliers) : 0.0;
    return true;
}

template <typename PointT>
bool CloudProcessor<PointT>::registInformationMatrix(const CloudConstPtr& source, const CloudConstPtr& target,
                                                     const float transform[16], double max_dist,
                                                     double out_information[36])
{
    if (!source || !target || source->empty() || target->empty())
        return false;
    std::memset(out_information, 0, sizeof(double) * 36);

    // 对应点对（变换后 source → target 最近邻），内点才参与协方差统计
    Eigen::Matrix4f T;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            T(r, c) = transform[r * 4 + c];
    pcl::PointCloud<PointT> transformed;
    pcl::transformPointCloud(*source, transformed, T);

    pcl::KdTreeFLANN<PointT> tree;
    tree.setInputCloud(target);
    std::vector<int> nn_idx(1);
    std::vector<float> nn_dist(1);
    const float max2 = (float)(max_dist * max_dist);

    std::vector<Eigen::Vector3d> src_pts, tgt_pts;
    for (size_t i = 0; i < transformed.size(); ++i) {
        if (tree.nearestKSearch(transformed[i], 1, nn_idx, nn_dist) <= 0)
            continue;
        if (nn_dist[0] > max2)
            continue;
        const PointT& s = transformed[i];
        const PointT& t = (*target)[(size_t)nn_idx[0]];
        src_pts.emplace_back(s.x, s.y, s.z);
        tgt_pts.emplace_back(t.x, t.y, t.z);
    }
    if (src_pts.size() < 6)
        return false;

    // 6x6 信息矩阵（对齐 Open3D GetInformationMatrixFromPointClouds）：
    // 每个对应点 p 贡献 3 个 rank-1 项 G_r·G_rᵀ，其中
    //   G_r1 = ( 0,  z, -y, 1, 0, 0)
    //   G_r2 = (-z,  0,  x, 0, 1, 0)
    //   G_r3 = ( y, -x,  0, 0, 0, 1)
    // 即 GTG = Σ_i [ ([p]×)ᵀ[p]×, ([p]×)ᵀ ; [p]×, I ]，顺序 [旋转, 平移]。
    Eigen::Matrix<double, 6, 6> M = Eigen::Matrix<double, 6, 6>::Zero();
    for (size_t i = 0; i < src_pts.size(); ++i) {
        // Open3D 用 target 点坐标构造 G_r
        const Eigen::Vector3d& p = tgt_pts[i];
        Eigen::Matrix<double, 3, 6> G = Eigen::Matrix<double, 3, 6>::Zero();
        G(0, 1) =  p.z(); G(0, 2) = -p.y(); G(0, 3) = 1.0;
        G(1, 0) = -p.z(); G(1, 2) =  p.x(); G(1, 4) = 1.0;
        G(2, 0) =  p.y(); G(2, 1) = -p.x(); G(2, 5) = 1.0;
        M += G.transpose() * G;
    }
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 6; ++c)
            out_information[r * 6 + c] = M(r, c);
    return true;
}

// ---------------- tree ----------------

template <typename PointT>
void CloudProcessor<PointT>::treeKdTreeKNN(const CloudConstPtr& src, const float query_point[3],
                                           int k, std::vector<int>& out_indices,
                                           std::vector<float>& out_distances)
{
    out_indices.clear();
    out_distances.clear();
    if (!src || src->empty())
        return;
    pcl::KdTreeFLANN<PointT> tree;
    tree.setInputCloud(src);
    std::vector<float> dist;
    tree.nearestKSearch(makeQueryPoint<PointT>(query_point), k, out_indices, dist);
    out_distances = std::move(dist);
}

template <typename PointT>
void CloudProcessor<PointT>::treeKdTreeRadiusSearch(const CloudConstPtr& src, const float query_point[3],
                                                    double radius, std::vector<int>& out_indices,
                                                    std::vector<float>& out_distances)
{
    out_indices.clear();
    out_distances.clear();
    if (!src || src->empty())
        return;
    pcl::KdTreeFLANN<PointT> tree;
    tree.setInputCloud(src);
    tree.radiusSearch(makeQueryPoint<PointT>(query_point), (float)radius, out_indices, out_distances);
}

template <typename PointT>
void CloudProcessor<PointT>::treeOctreeVoxelSearch(const CloudConstPtr& src, const float query_point[3],
                                                   std::vector<int>& out_indices,
                                                   double resolution)
{
    out_indices.clear();
    if (!src || src->empty())
        return;
    if (resolution <= 0)
        resolution = 1.0;
    pcl::octree::OctreePointCloudSearch<PointT> oct((float)resolution);
    oct.setInputCloud(src);
    oct.addPointsFromInputCloud();
    std::vector<int> vidx;
    if (oct.voxelSearch(makeQueryPoint<PointT>(query_point), vidx))
        out_indices = std::move(vidx);
}

template <typename PointT>
void CloudProcessor<PointT>::treeOctreeChangeDetection(const CloudConstPtr& src1, const CloudConstPtr& src2,
                                                       double min_resolution,
                                                       std::vector<int>& out_new_indices)
{
    out_new_indices.clear();
    // src1 为空是合法输入（全部点都是"新增"），但分辨率非法会让 octree 内部
    // 死循环挂死（实测 res=0 卡死），必须先钳制
    if (!src1 || !src2 || src2->empty() || min_resolution <= 0)
        return;
    pcl::octree::OctreePointCloudChangeDetector<PointT> oct((float)min_resolution);
    oct.setInputCloud(src1);
    oct.addPointsFromInputCloud();
    oct.switchBuffers();
    oct.setInputCloud(src2);
    oct.addPointsFromInputCloud();
    std::vector<int> newIdx;
    oct.getPointIndicesFromNewVoxels(newIdx);
    out_new_indices = std::move(newIdx);
}

// ---------------- surface ----------------

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::surfaceConvexHull(const CloudConstPtr& src)
{
    if (!src || src->empty())
        return nullptr;
    pcl::ConvexHull<PointT> hull;
    hull.setInputCloud(src);
    CloudPtr out(new pcl::PointCloud<PointT>);
    hull.reconstruct(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::surfaceConcaveHull(const CloudConstPtr& src, double alpha)
{
    if (!src || src->empty() || alpha <= 0)
        return nullptr;
    pcl::ConcaveHull<PointT> hull;
    hull.setInputCloud(src);
    hull.setAlpha(alpha);
    CloudPtr out(new pcl::PointCloud<PointT>);
    hull.reconstruct(*out);
    return out;
}

template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::surfaceSmoothMLS(const CloudConstPtr& src, double radius,
                                         int polynomial_order, double voxel_size,
                                         pcl::PointCloud<pcl::Normal>::Ptr* out_normals)
{
    if (!src || src->empty() || radius <= 0)
        return nullptr;
    // 输出统一用 PointNormal：MLS 计算的法线会写入该点型字段（PCL 1.9 无 getNormals）
    pcl::MovingLeastSquares<PointT, pcl::PointNormal> mls;
    mls.setInputCloud(src);
    typename pcl::search::KdTree<PointT>::Ptr tree(new pcl::search::KdTree<PointT>);
    mls.setSearchMethod(tree);
    mls.setSearchRadius(radius);
    mls.setPolynomialOrder(std::max(1, polynomial_order));
    if (voxel_size > 0) {
        // Open3D smooth_mls 只做投影平滑；需要补点时用体素膨胀上采样
        mls.setUpsamplingMethod(pcl::MovingLeastSquares<PointT, pcl::PointNormal>::VOXEL_GRID_DILATION);
        mls.setDilationVoxelSize((float)voxel_size);
        mls.setDilationIterations(1);
    }
    // 注意：PCL 1.9 的上采样路径（performUpsampling）会无条件写入内部 normals_，
    // setComputeNormals(false) 时该指针为空 → 空指针崩溃。必须始终开启法线计算，
    // 只是调用方未索取时丢弃结果。
    mls.setComputeNormals(true);
    pcl::PointCloud<pcl::PointNormal> mls_out;
    mls.process(mls_out);

    if (out_normals) {
        pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
        normals->reserve(mls_out.size());
        for (const pcl::PointNormal& p : mls_out.points) {
            pcl::Normal n;
            n.normal_x = p.normal_x;
            n.normal_y = p.normal_y;
            n.normal_z = p.normal_z;
            n.curvature = p.curvature;
            normals->push_back(n);
        }
        *out_normals = normals;
    }

    // MLS 输出点型固定为 PointNormal（PCL 1.9 无 getNormals 接口），
    // 转回 PointT 时无法恢复 rgb/intensity（MLS 内部不搬运这些字段）——
    // 对带属性点云意味着属性丢失，已在文档中说明。
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(mls_out.size());
    for (const pcl::PointNormal& p : mls_out.points)
        out->push_back(makePoint<PointT>(p.x, p.y, p.z));
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = mls_out.is_dense;
    return out;
}

namespace {

// PCL PolygonMesh → 原生 Mesh3d（顶点 + 三角面）
// PCL 的重建器可能输出四边形（GridProjection 全是四边形）或多边形（Poisson），
// 这里统一做扇形三角化：以 v[0] 为扇心拆成 (v0,vi,vi+1)。
template <typename PointT>
Mesh3d meshFromPolygonMesh(const pcl::PolygonMesh& mesh)
{
    Mesh3d out;
    pcl::PointCloud<PointT> vertices;
    pcl::fromPCLPointCloud2(mesh.cloud, vertices);
    out.vertices.reserve(vertices.size());
    for (const PointT& v : vertices.points)
        out.vertices.push_back(Point3d{ (double)v.x, (double)v.y, (double)v.z });
    out.triangles.reserve(mesh.polygons.size() * 2);
    const int nv = (int)out.vertices.size();
    for (const pcl::Vertices& poly : mesh.polygons) {
        if (poly.vertices.size() < 3)
            continue;
        // 校验索引，避免下游越界
        bool ok = true;
        for (uint32_t idx : poly.vertices)
            if ((int)idx < 0 || (int)idx >= nv) {
                ok = false;
                break;
            }
        if (!ok)
            continue;
        for (size_t k = 1; k + 1 < poly.vertices.size(); ++k)
            out.triangles.push_back({ (int)poly.vertices[0],
                                      (int)poly.vertices[k],
                                      (int)poly.vertices[k + 1] });
    }
    out.valid = !out.vertices.empty() && !out.triangles.empty();
    return out;
}

// 点型 → 带法线点型（表面重建输入统一转 PointNormal）
// 统一做法：合并坐标与法线，并剔除非法点。
// 背景：featureNormalEstimationRadius 对邻域不足的点输出 NaN 法线；而 PCL 的
// Poisson/GP3/MarchingCubes 遇到 NaN 法线不会报错（仍返回"成功"），但结果不可用
// ——必须在入口统一拦截，否则是静默错误。
template <typename PointT>
pcl::PointCloud<pcl::PointNormal>::Ptr makePointNormalCloud(
    const pcl::PointCloud<PointT>& src,
    const pcl::PointCloud<pcl::Normal>& normals,
    size_t* valid_count = nullptr)
{
    pcl::PointCloud<pcl::PointNormal>::Ptr out(new pcl::PointCloud<pcl::PointNormal>);
    out->reserve(src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        const PointT& p = src[i];
        const pcl::Normal& n = normals[i];
        if (!pcl::isFinite(p) ||
            !pcl_isfinite(n.normal_x) || !pcl_isfinite(n.normal_y) || !pcl_isfinite(n.normal_z))
            continue;
        // 零长度法线同样无法提供方向信息
        if (n.normal_x == 0.0f && n.normal_y == 0.0f && n.normal_z == 0.0f)
            continue;
        pcl::PointNormal pn;
        pn.x = p.x;
        pn.y = p.y;
        pn.z = p.z;
        pn.normal_x = n.normal_x;
        pn.normal_y = n.normal_y;
        pn.normal_z = n.normal_z;
        pn.curvature = n.curvature;
        out->push_back(pn);
    }
    out->width = (uint32_t)out->size();
    out->height = 1;
    out->is_dense = true;
    if (valid_count)
        *valid_count = out->size();
    return out;
}

// 重建输入门槛：有效点（有限坐标 + 有限非零法线）数量达标且占比过半。
// 占比过半的要求用于拦住"上游法线估计整体失效"的场景（如半径设得过小）。
inline bool checkReconstructionInput(size_t total, size_t valid_count, size_t min_points)
{
    return total > 0 && valid_count >= min_points && valid_count * 2 >= total;
}

} // namespace

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfacePoisson(const CloudConstPtr& src,
                                              const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                              int depth, int iso_divide,
                                              double samples_per_node)
{
    Mesh3d result;
    if (!src || src->empty() || !normals || normals->size() != src->size())
        return result;
    // Poisson 需要 PointNormal（坐标 + 法线合并）
    size_t valid_count = 0;
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_n =
        makePointNormalCloud(*src, *normals, &valid_count);
    if (!checkReconstructionInput(src->size(), valid_count, 3))
        return result;
    pcl::Poisson<pcl::PointNormal> poisson;
    poisson.setDepth(std::max(2, depth));
    poisson.setIsoDivide(std::max(1, std::min(iso_divide, depth)));
    poisson.setSamplesPerNode((float)samples_per_node);
    poisson.setInputCloud(cloud_n);
    pcl::PolygonMesh mesh;
    poisson.reconstruct(mesh);
    return meshFromPolygonMesh<pcl::PointNormal>(mesh);
}

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceGreedyProjection(const CloudConstPtr& src,
                                                       const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                                       double search_radius, double mu,
                                                       int max_neighbors)
{
    Mesh3d result;
    if (!src || src->empty() || !normals || normals->size() != src->size() || search_radius <= 0)
        return result;
    size_t valid_count = 0;
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_n =
        makePointNormalCloud(*src, *normals, &valid_count);
    if (!checkReconstructionInput(src->size(), valid_count, 3))
        return result;
    pcl::GreedyProjectionTriangulation<pcl::PointNormal> gp3;
    gp3.setInputCloud(cloud_n);
    gp3.setSearchRadius(search_radius);
    gp3.setMu(mu);
    gp3.setMaximumNearestNeighbors(max_neighbors);
    gp3.setMinimumAngle(M_PI / 18.0);       // 10°
    gp3.setMaximumAngle(2.0 * M_PI / 3.0);  // 120°
    gp3.setMaximumSurfaceAngle(M_PI / 4.0); // 45°
    gp3.setNormalConsistency(false);
    pcl::PolygonMesh mesh;
    gp3.reconstruct(mesh);
    return meshFromPolygonMesh<pcl::PointNormal>(mesh);
}

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceMarchingCubes(const CloudConstPtr& src,
                                                    const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                                    int resolution)
{
    Mesh3d result;
    if (!src || src->empty() || !normals || normals->size() != src->size() || resolution < 8)
        return result;
    size_t valid_count = 0;
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_n =
        makePointNormalCloud(*src, *normals, &valid_count);
    if (!checkReconstructionInput(src->size(), valid_count, 3))
        return result;
    pcl::MarchingCubesHoppe<pcl::PointNormal> mc;
    mc.setInputCloud(cloud_n);
    mc.setGridResolution(resolution, resolution, resolution);
    mc.setIsoLevel(0.0f);
    pcl::PolygonMesh mesh;
    mc.reconstruct(mesh);
    return meshFromPolygonMesh<pcl::PointNormal>(mesh);
}

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceMarchingCubesRBF(const CloudConstPtr& src,
                                                       const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                                       int resolution, double off_surface_epsilon)
{
    Mesh3d result;
    if (!src || src->empty() || !normals || normals->size() != src->size() || resolution < 8)
        return result;
    size_t valid_count = 0;
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_n =
        makePointNormalCloud(*src, *normals, &valid_count);
    if (!checkReconstructionInput(src->size(), valid_count, 3))
        return result;
    pcl::MarchingCubesRBF<pcl::PointNormal> mc;
    mc.setInputCloud(cloud_n);
    mc.setGridResolution(resolution, resolution, resolution);
    mc.setIsoLevel(0.0f);
    mc.setOffSurfaceDisplacement((float)off_surface_epsilon);
    pcl::PolygonMesh mesh;
    mc.reconstruct(mesh);
    return meshFromPolygonMesh<pcl::PointNormal>(mesh);
}

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceGridProjection(const CloudConstPtr& src,
                                                     const pcl::PointCloud<pcl::Normal>::ConstPtr& normals,
                                                     double resolution, int padding_size)
{
    Mesh3d result;
    if (!src || src->empty() || !normals || normals->size() != src->size() || resolution <= 0)
        return result;
    size_t valid_count = 0;
    pcl::PointCloud<pcl::PointNormal>::Ptr cloud_n =
        makePointNormalCloud(*src, *normals, &valid_count);
    if (!checkReconstructionInput(src->size(), valid_count, 3))
        return result;
    pcl::GridProjection<pcl::PointNormal> gp;
    gp.setInputCloud(cloud_n);
    gp.setResolution(resolution);
    gp.setPaddingSize(std::max(0, padding_size));
    // PCL 1.9 的 GridProjection::reconstruct(PolygonMesh&) 有缺陷（常返回空网格），
    // 但 reconstruct(points, polygons) 重载正常，故走后者再自行组装。
    pcl::PointCloud<pcl::PointNormal> gp_points;
    std::vector<pcl::Vertices> gp_polygons;
    gp.reconstruct(gp_points, gp_polygons);

    // PCL 缺陷补偿：GridProjection::getBoundingBox 在点云最大边长 > 1 时会把输入
    // 除以该边长（scaleInputDataPoint），但输出顶点**没有乘回来**，导致结果被整体
    // 缩小。这里复算同一缩放因子并还原。
    double scale = 1.0;
    {
        pcl::PointNormal mn, mx;
        pcl::getMinMax3D(*cloud_n, mn, mx);
        const double sx = (double)mx.x - mn.x;
        const double sy = (double)mx.y - mn.y;
        const double sz = (double)mx.z - mn.z;
        const double max_extent = std::max({ sx, sy, sz });
        if (max_extent > 1.0)
            scale = max_extent;
        // 注意：PCL 归一化时以缩放前的 min/max 为基准，缩放后点的绝对位置也变了，
        // GridProjection 不保存该平移信息，故只能还原"尺度"，绝对位置会带缩放偏差。
        // 对常见用法（相对形状/体积）足够；需要绝对坐标时请先把点云缩放到单位盒内。
    }

    result.vertices.reserve(gp_points.size());
    for (const pcl::PointNormal& p : gp_points.points)
        result.vertices.push_back(Point3d{ (double)p.x * scale,
                                           (double)p.y * scale,
                                           (double)p.z * scale });
    const int nv = (int)result.vertices.size();
    for (const pcl::Vertices& poly : gp_polygons) {
        if (poly.vertices.size() < 3)
            continue;
        bool ok = true;
        for (uint32_t idx : poly.vertices)
            if ((int)idx < 0 || (int)idx >= nv) {
                ok = false;
                break;
            }
        if (!ok)
            continue;
        // GridProjection 输出四边形 → 扇形三角化
        for (size_t k = 1; k + 1 < poly.vertices.size(); ++k)
            result.triangles.push_back({ (int)poly.vertices[0],
                                         (int)poly.vertices[k],
                                         (int)poly.vertices[k + 1] });
    }
    result.valid = !result.vertices.empty() && !result.triangles.empty();
    return result;
}

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceOrganizedFastMesh(const CloudConstPtr& src,
                                                        int triangle_pixel_size, bool adaptive_cut)
{
    Mesh3d result;
    // OrganizedFastMesh 只对有序点云（height > 1）有意义
    if (!src || src->empty() || src->height <= 1 || triangle_pixel_size < 1)
        return result;

    // PCL 1.9 只为 PointXYZ / PointXYZRGB / PointXYZRGBA 预编译了 OrganizedFastMesh；
    // PointXYZI 无实例，先转成 PointXYZ 再三角化（顶点坐标不变，只是丢失强度）。
    pcl::PointCloud<pcl::PointXYZ>::Ptr xyz(new pcl::PointCloud<pcl::PointXYZ>);
    pcl::copyPointCloud(*src, *xyz);
    xyz->width = src->width;
    xyz->height = src->height;
    xyz->is_dense = src->is_dense;

    pcl::OrganizedFastMesh<pcl::PointXYZ> ofm;
    ofm.setInputCloud(xyz);
    ofm.setTrianglePixelSize(triangle_pixel_size);
    ofm.setTriangulationType(adaptive_cut
                                 ? pcl::OrganizedFastMesh<pcl::PointXYZ>::TRIANGLE_ADAPTIVE_CUT
                                 : pcl::OrganizedFastMesh<pcl::PointXYZ>::TRIANGLE_RIGHT_CUT);
    pcl::PolygonMesh mesh;
    ofm.reconstruct(mesh);
    return meshFromPolygonMesh<pcl::PointXYZ>(mesh);
}

template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceMeshLaplacianSmooth(const Mesh3d& mesh,
                                                          int iterations, double lambda)
{
    if (!mesh.valid || mesh.vertices.empty() || mesh.triangles.empty() ||
        iterations <= 0 || lambda <= 0.0)
        return Mesh3d();

    // 构建顶点邻接表（三角面三条边 → 无向图）
    std::vector<std::set<int>> adj(mesh.vertices.size());
    for (const std::array<int, 3>& tri : mesh.triangles)
        for (int e = 0; e < 3; ++e) {
            const int a = tri[(size_t)e];
            const int b = tri[(size_t)((e + 1) % 3)];
            if (a < 0 || b < 0 || (size_t)a >= mesh.vertices.size() ||
                (size_t)b >= mesh.vertices.size())
                return Mesh3d();
            adj[(size_t)a].insert(b);
            adj[(size_t)b].insert(a);
        }

    // Umbrella 算子：p' = p + λ · (mean(neighbors) - p)
    Mesh3d out = mesh;
    const double step = std::min(1.0, lambda);
    for (int it = 0; it < iterations; ++it) {
        std::vector<Point3d> next = out.vertices;
        for (size_t i = 0; i < out.vertices.size(); ++i) {
            if (adj[i].empty())
                continue;   // 孤立顶点不动
            double sx = 0.0, sy = 0.0, sz = 0.0;
            for (int j : adj[i]) {
                sx += out.vertices[(size_t)j].x;
                sy += out.vertices[(size_t)j].y;
                sz += out.vertices[(size_t)j].z;
            }
            const double inv = 1.0 / (double)adj[i].size();
            const Point3d& p = out.vertices[i];
            next[i].x = p.x + step * (sx * inv - p.x);
            next[i].y = p.y + step * (sy * inv - p.y);
            next[i].z = p.z + step * (sz * inv - p.z);
        }
        out.vertices.swap(next);
    }
    return out;
}

// 网格顶点聚类抽稀：体素合并顶点 → 质心 + 面重映射（快速粗糙，不保流形）
template <typename PointT>
Mesh3d CloudProcessor<PointT>::surfaceMeshClusterDecimate(const Mesh3d& mesh, double cell)
{
    Mesh3d out;
    if (!mesh.valid || mesh.vertices.empty() || mesh.triangles.empty() || cell <= 0)
        return out;

    // 1) 顶点 → 体素簇（floor(v/cell) 为键）
    std::map<std::array<long long, 3>, int> clusterOfKey;
    std::vector<int> vertToCluster((size_t)mesh.vertices.size(), -1);
    std::vector<double> sumX, sumY, sumZ;
    std::vector<int> cnt;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Point3d& v = mesh.vertices[i];
        std::array<long long, 3> key{
            (long long)std::floor(v.x / cell),
            (long long)std::floor(v.y / cell),
            (long long)std::floor(v.z / cell),
        };
        auto it = clusterOfKey.find(key);
        int cid;
        if (it == clusterOfKey.end()) {
            cid = (int)cnt.size();
            clusterOfKey.emplace(key, cid);
            sumX.push_back(0.0);
            sumY.push_back(0.0);
            sumZ.push_back(0.0);
            cnt.push_back(0);
        } else {
            cid = it->second;
        }
        vertToCluster[i] = cid;
        sumX[(size_t)cid] += v.x;
        sumY[(size_t)cid] += v.y;
        sumZ[(size_t)cid] += v.z;
        ++cnt[(size_t)cid];
    }

    // 2) 簇 → 新顶点（质心）
    out.vertices.resize((size_t)cnt.size());
    for (size_t c = 0; c < cnt.size(); ++c)
        out.vertices[c] = Point3d{ sumX[c] / (double)cnt[c],
                                   sumY[c] / (double)cnt[c],
                                   sumZ[c] / (double)cnt[c] };

    // 3) 面重映射：剔除退化（簇重合）与重复面（按排序索引三元组去重）
    std::set<std::array<int, 3>> seen;
    for (const std::array<int, 3>& t : mesh.triangles) {
        const int a = vertToCluster[(size_t)t[0]];
        const int b = vertToCluster[(size_t)t[1]];
        const int c = vertToCluster[(size_t)t[2]];
        if (a == b || b == c || a == c)
            continue;
        std::array<int, 3> key{ a, b, c };
        std::array<int, 3> sorted = key;
        std::sort(sorted.begin(), sorted.end());
        if (!seen.insert(sorted).second)
            continue;
        out.triangles.push_back(key);   // 保留首次出现的原始绕向
    }
    out.valid = !out.triangles.empty();
    return out;
}

// ---------------- 网格度量：面积 / 有向体积 / 点-网格距离 ----------------

namespace {


// 面法向：叉积单位化，退化面返回 (0,0,0)
void triNormalDir(const Point3d& a, const Point3d& b, const Point3d& c, double out_n[3])
{
    const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
    const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
    const double nx = uy * vz - uz * vy;
    const double ny = uz * vx - ux * vz;
    const double nz = ux * vy - uy * vx;
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12) {
        out_n[0] = out_n[1] = out_n[2] = 0.0;
        return;
    }
    out_n[0] = nx / len;
    out_n[1] = ny / len;
    out_n[2] = nz / len;
}

} // namespace

template <typename PointT>
double CloudProcessor<PointT>::measureMeshArea(const Mesh3d& mesh)
{
    double area = 0;
    if (!mesh.valid || mesh.triangles.empty())
        return area;
    const int nv = (int)mesh.vertices.size();
    for (const std::array<int, 3>& t : mesh.triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
            continue;
        const Point3d& a = mesh.vertices[(size_t)t[0]];
        const Point3d& b = mesh.vertices[(size_t)t[1]];
        const Point3d& c = mesh.vertices[(size_t)t[2]];
        const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
        const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
        const double cx = uy * vz - uz * vy;
        const double cy = uz * vx - ux * vz;
        const double cz = ux * vy - uy * vx;
        area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }
    return area;
}

template <typename PointT>
double CloudProcessor<PointT>::measureMeshVolume(const Mesh3d& mesh)
{
    double volume = 0;
    if (!mesh.valid || mesh.triangles.empty())
        return volume;
    const int nv = (int)mesh.vertices.size();
    for (const std::array<int, 3>& t : mesh.triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
            continue;
        const Point3d& a = mesh.vertices[(size_t)t[0]];
        const Point3d& b = mesh.vertices[(size_t)t[1]];
        const Point3d& c = mesh.vertices[(size_t)t[2]];
        // 有向四面体体积（散度定理；闭合 + 绕向朝外 = 真实体积）
        volume += (a.x * (b.y * c.z - b.z * c.y) + a.y * (b.z * c.x - b.x * c.z) +
                   a.z * (b.x * c.y - b.y * c.x)) /
                  6.0;
    }
    return volume;
}



// 带符号点-网格距离：精确最近距离 × 射线奇偶符号（仅闭合网格有物理意义）

// 网格每面单位法线（退化/越界面 = (0,0,0)）
template <typename PointT>
std::vector<float> CloudProcessor<PointT>::meshFaceNormals(const Mesh3d& mesh)
{
    std::vector<float> out;
    if (!mesh.valid || mesh.triangles.empty())
        return out;
    const int nv = (int)mesh.vertices.size();
    out.assign(mesh.triangles.size() * 3, 0.0f);
    for (size_t f = 0; f < mesh.triangles.size(); ++f) {
        const std::array<int, 3>& t = mesh.triangles[f];
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
            continue;   // 越界面保持 (0,0,0)
        double n[3];
        triNormalDir(mesh.vertices[(size_t)t[0]], mesh.vertices[(size_t)t[1]],
                     mesh.vertices[(size_t)t[2]], n);
        out[f * 3] = (float)n[0];
        out[f * 3 + 1] = (float)n[1];
        out[f * 3 + 2] = (float)n[2];
    }
    return out;
}

// 网格每顶点法线（相邻面未归一叉积累加 = 面积加权，再单位化）
template <typename PointT>
std::vector<float> CloudProcessor<PointT>::meshVertexNormals(const Mesh3d& mesh)
{
    std::vector<float> out;
    if (!mesh.valid || mesh.vertices.empty())
        return out;
    const int nv = (int)mesh.vertices.size();
    std::vector<double> acc((size_t)nv * 3, 0.0);
    for (const std::array<int, 3>& t : mesh.triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
            continue;
        const Point3d& a = mesh.vertices[(size_t)t[0]];
        const Point3d& b = mesh.vertices[(size_t)t[1]];
        const Point3d& c = mesh.vertices[(size_t)t[2]];
        const double nx = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        const double ny = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
        const double nz = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        for (int k : t) {
            acc[(size_t)k * 3] += nx;
            acc[(size_t)k * 3 + 1] += ny;
            acc[(size_t)k * 3 + 2] += nz;
        }
    }
    out.resize((size_t)nv * 3, 0.0f);
    for (int v = 0; v < nv; ++v) {
        const double len = std::sqrt(acc[(size_t)v * 3] * acc[(size_t)v * 3] +
                                     acc[(size_t)v * 3 + 1] * acc[(size_t)v * 3 + 1] +
                                     acc[(size_t)v * 3 + 2] * acc[(size_t)v * 3 + 2]);
        if (len > 1e-12) {
            out[(size_t)v * 3] = (float)(acc[(size_t)v * 3] / len);
            out[(size_t)v * 3 + 1] = (float)(acc[(size_t)v * 3 + 1] / len);
            out[(size_t)v * 3 + 2] = (float)(acc[(size_t)v * 3 + 2] / len);
        }
    }
    return out;
}

// 按 OBB 裁剪网格：三顶点全在盒内的面保留，顶点重编号
template <typename PointT>
Mesh3d CloudProcessor<PointT>::meshCropByOBB(const Mesh3d& mesh, const OBB3d& box)
{
    Mesh3d out;
    if (!mesh.valid || !box.valid)
        return out;
    const int nv = (int)mesh.vertices.size();
    std::vector<char> inFlag((size_t)nv, 0);
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
        inFlag[i] = Stereo::pointInOBB(mesh.vertices[i], box) ? 1 : 0;
    std::vector<int> remap((size_t)nv, -1);
    for (const std::array<int, 3>& t : mesh.triangles) {
        if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
            continue;
        if (!inFlag[(size_t)t[0]] || !inFlag[(size_t)t[1]] || !inFlag[(size_t)t[2]])
            continue;
        for (int k : t) {
            if (remap[(size_t)k] < 0) {
                remap[(size_t)k] = (int)out.vertices.size();
                out.vertices.push_back(mesh.vertices[(size_t)k]);
            }
        }
        out.triangles.push_back({ remap[(size_t)t[0]], remap[(size_t)t[1]], remap[(size_t)t[2]] });
    }
    out.valid = !out.triangles.empty();
    return out;
}

// ---------------- measure ----------------

template <typename PointT>
bool CloudProcessor<PointT>::measureMinMax(const CloudConstPtr& src, Point3d& out_min, Point3d& out_max)
{
    if (!src || src->empty())
        return false;
    // PCL 的 getMinMax3D 在 is_dense==true 时跳过有限性检查（NaN 点会污染结果）。
    // 外部数据常把含 NaN 点云错标为 dense，因此这里不做信任 is_dense，
    // 始终扫描验证；发现非有限点时忽略它们、只统计有效点。
    bool all_finite = true;
    for (const PointT& p : src->points) {
        if (!pcl::isFinite(p)) {
            all_finite = false;
            break;
        }
    }
    if (all_finite) {
        PointT mn, mx;
        pcl::getMinMax3D(*src, mn, mx);
        out_min = Point3d{ (double)mn.x, (double)mn.y, (double)mn.z };
        out_max = Point3d{ (double)mx.x, (double)mx.y, (double)mx.z };
        return true;
    }
    // 含非有限点：只统计有限点（与 remove_non_finite 后的语义一致）
    bool first = true;
    double lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
    for (const PointT& p : src->points) {
        if (!pcl::isFinite(p))
            continue;
        const double v[3] = { p.x, p.y, p.z };
        if (first) {
            for (int k = 0; k < 3; ++k) { lo[k] = hi[k] = v[k]; }
            first = false;
        } else {
            for (int k = 0; k < 3; ++k) {
                if (v[k] < lo[k]) lo[k] = v[k];
                if (v[k] > hi[k]) hi[k] = v[k];
            }
        }
    }
    if (first)
        return false;   // 全部点都非有限
    out_min = Point3d{ lo[0], lo[1], lo[2] };
    out_max = Point3d{ hi[0], hi[1], hi[2] };
    return true;
}

template <typename PointT>
AABB3d CloudProcessor<PointT>::measureAABB(const CloudConstPtr& src)
{
    AABB3d box;
    Point3d mn, mx;
    if (!measureMinMax(src, mn, mx))
        return box;
    box.min_x = mn.x; box.min_y = mn.y; box.min_z = mn.z;
    box.max_x = mx.x; box.max_y = mx.y; box.max_z = mx.z;
    box.valid = true;
    return box;
}

template <typename PointT>
OBB3d CloudProcessor<PointT>::measureOBB(const CloudConstPtr& src)
{
    OBB3d obb;
    if (!src || src->size() < 3)
        return obb;

    // PCA 主轴（与 Stereo::getOBB 同语义：extents = 半长，rotation 行 = 主轴方向）
    Eigen::Vector4f centroid;
    pcl::compute3DCentroid(*src, centroid);
    Eigen::Matrix3f cov;
    pcl::computeCovarianceMatrixNormalized(*src, centroid, cov);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3f> es(cov);
    if (es.info() != Eigen::Success)
        return obb;

    // 特征值升序 → 主轴降序（行 = 主轴）
    std::array<int, 3> order = { 0, 1, 2 };
    const Eigen::Vector3f evals = es.eigenvalues();
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return evals[a] > evals[b]; });

    Eigen::Matrix3f axes;
    for (int d = 0; d < 3; ++d)
        axes.row(d) = es.eigenvectors().col(order[(size_t)d]).transpose();

    // 投影到主轴系求极值
    Eigen::Vector3f lo = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    Eigen::Vector3f hi = Eigen::Vector3f::Constant(-std::numeric_limits<float>::max());
    for (const PointT& p : src->points) {
        const Eigen::Vector3f rel(p.x - centroid[0], p.y - centroid[1], p.z - centroid[2]);
        const Eigen::Vector3f local = axes * rel;
        lo = lo.cwiseMin(local);
        hi = hi.cwiseMax(local);
    }
    const Eigen::Vector3f mid = 0.5f * (lo + hi);
    const Eigen::Vector3f center = centroid.head<3>() + axes.transpose() * mid;

    obb.center = Point3d{ (double)center[0], (double)center[1], (double)center[2] };
    for (int d = 0; d < 3; ++d) {
        obb.extents[d] = 0.5 * (double)(hi[d] - lo[d]);
        for (int k = 0; k < 3; ++k)
            obb.rotation[d * 3 + k] = (double)axes(d, k);
    }
    obb.valid = true;
    return obb;
}

template <typename PointT>
bool CloudProcessor<PointT>::measureCentroid(const CloudConstPtr& src, Point3d& out_centroid)
{
    if (!src || src->empty())
        return false;
    Eigen::Vector4f c;
    pcl::compute3DCentroid(*src, c);
    out_centroid = Point3d{ (double)c[0], (double)c[1], (double)c[2] };
    return true;
}

template <typename PointT>
bool CloudProcessor<PointT>::measureMeanAndCovariance(const CloudConstPtr& src, Point3d& out_mean,
                                                      double out_cov[9])
{
    if (!src || src->size() < 2)
        return false;
    Eigen::Matrix3d cov;
    Eigen::Vector4d mean;
    pcl::computeMeanAndCovarianceMatrix(*src, cov, mean);
    out_mean = Point3d{ mean[0], mean[1], mean[2] };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out_cov[r * 3 + c] = cov(r, c);
    return true;
}

template <typename PointT>
std::vector<float> CloudProcessor<PointT>::measurePointCloudDistance(const CloudConstPtr& src,
                                                                       const CloudConstPtr& target)
{
    std::vector<float> out;
    if (!src || !target || src->empty() || target->empty())
        return out;
    pcl::KdTreeFLANN<PointT> tree;
    tree.setInputCloud(target);
    out.resize(src->size());
    std::vector<int> nn_idx(1);
    std::vector<float> nn_dist(1);
    for (size_t i = 0; i < src->size(); ++i) {
        tree.nearestKSearch((*src)[i], 1, nn_idx, nn_dist);
        out[i] = std::sqrt(std::max(0.0f, nn_dist[0]));
    }
    return out;
}

// 平面度（GD&T 简化口径）：主平面局内点残差峰峰值
template <typename PointT>
bool CloudProcessor<PointT>::measureFlatness(const CloudConstPtr& src, double distance_threshold,
                                             double& out_flatness, std::vector<float>& out_coeffs)
{
    out_flatness = 0.0;
    out_coeffs.clear();
    if (!src || src->size() < 3)
        return false;
    CloudPtr clean;
    std::vector<int> global;
    if (!fitInputClean<PointT>(src, clean, global))
        return false;
    std::vector<int> inliers;
    if (!fitPlane(clean, FitMethod::RANSAC, distance_threshold, out_coeffs, inliers) ||
        inliers.size() < 2)
        return false;
    double mn = 1e30, mx = -1e30;
    for (int idx : inliers) {
        const PointT& p = (*clean)[(size_t)idx];
        const double res = (double)out_coeffs[0] * p.x + (double)out_coeffs[1] * p.y +
                           (double)out_coeffs[2] * p.z + (double)out_coeffs[3];
        mn = (std::min)(mn, res);
        mx = (std::max)(mx, res);
    }
    out_flatness = mx - mn;
    return true;
}

// 位置度：理论孔位 vs 实测圆心，逐理论位就近贪心配对
template <typename PointT>
int CloudProcessor<PointT>::measurePositionDeviation(const std::vector<Point2d>& theoretical,
                                                     const std::vector<Point2d>& measured,
                                                     double match_radius,
                                                     std::vector<PositionDeviation>& out_pairs)
{
    out_pairs.clear();
    if (match_radius < 0)
        return -1;
    std::vector<char> used(measured.size(), 0);
    int matched = 0;
    for (const Point2d& th : theoretical) {
        PositionDeviation item;
        item.theoretical = (int)out_pairs.size();
        item.measured = -1;
        double best = match_radius;
        int bestJ = -1;
        for (size_t j = 0; j < measured.size(); ++j) {
            if (used[j])
                continue;
            const double dx = measured[j].x - th.x, dy = measured[j].y - th.y;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d <= best) {
                best = d;
                bestJ = (int)j;
            }
        }
        if (bestJ >= 0) {
            used[(size_t)bestJ] = 1;
            item.measured = bestJ;
            item.dx = measured[(size_t)bestJ].x - th.x;
            item.dy = measured[(size_t)bestJ].y - th.y;
            item.dist = best;
            ++matched;
        }
        out_pairs.push_back(item);
    }
    return matched;
}

// 平面有符号残差数组（n 不归一内部除 |n|）
template <typename PointT>
bool CloudProcessor<PointT>::planeResiduals(const CloudConstPtr& src, const float plane[4],
                                            std::vector<float>& out_residuals)
{
    out_residuals.clear();
    if (!src || src->empty())
        return false;
    const double l = std::sqrt((double)plane[0] * plane[0] + (double)plane[1] * plane[1] +
                               (double)plane[2] * plane[2]);
    if (l < 1e-12)
        return false;
    const double a = plane[0] / l, b = plane[1] / l, c = plane[2] / l, d = plane[3] / l;
    out_residuals.resize(src->size());
    for (size_t i = 0; i < src->size(); ++i) {
        const PointT& p = (*src)[i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            out_residuals[i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        out_residuals[i] = (float)(a * p.x + b * p.y + c * p.z + d);
    }
    return true;
}

// 两平面夹角（度，锐角口径）
template <typename PointT>
double CloudProcessor<PointT>::measurePlaneAngle(const float plane1[4], const float plane2[4])
{
    const double n1[3] = { plane1[0], plane1[1], plane1[2] };
    const double n2[3] = { plane2[0], plane2[1], plane2[2] };
    const double l1 = std::sqrt(n1[0] * n1[0] + n1[1] * n1[1] + n1[2] * n1[2]);
    const double l2 = std::sqrt(n2[0] * n2[0] + n2[1] * n2[1] + n2[2] * n2[2]);
    if (l1 < 1e-12 || l2 < 1e-12)
        return -1.0;
    const double dot = (n1[0] * n2[0] + n1[1] * n2[1] + n1[2] * n2[2]) / (l1 * l2);
    if (!std::isfinite(dot))
        return -1.0;
    const double c = (std::min)(1.0, std::fabs(dot));
    return std::acos(c) * 180.0 / 3.14159265358979323846;
}

// 两平面交线：方向 = n1×n2，点 = 三方程（两面 + 过原点垂直 dir）联立解
template <typename PointT>
bool CloudProcessor<PointT>::measurePlaneIntersection(const float plane1[4],
                                                      const float plane2[4],
                                                      float out_point[3], float out_dir[3])
{
    if (!out_point || !out_dir)
        return false;
    for (int k = 0; k < 3; ++k) {
        out_point[k] = 0.f;
        out_dir[k] = 0.f;
    }
    const double l1 = std::sqrt((double)plane1[0] * plane1[0] + (double)plane1[1] * plane1[1] +
                                (double)plane1[2] * plane1[2]);
    const double l2 = std::sqrt((double)plane2[0] * plane2[0] + (double)plane2[1] * plane2[1] +
                                (double)plane2[2] * plane2[2]);
    if (l1 < 1e-12 || l2 < 1e-12)
        return false;
    const double n1[3] = { plane1[0] / l1, plane1[1] / l1, plane1[2] / l1 };
    const double n2[3] = { plane2[0] / l2, plane2[1] / l2, plane2[2] / l2 };
    const double d1 = (double)plane1[3] / l1;
    const double d2 = (double)plane2[3] / l2;
    // dir = n1 × n2（近平行 → 无交线）
    double dir[3] = { n1[1] * n2[2] - n1[2] * n2[1],
                      n1[2] * n2[0] - n1[0] * n2[2],
                      n1[0] * n2[1] - n1[1] * n2[0] };
    const double dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (dl < 1e-9)
        return false;
    for (double& v : dir)
        v /= dl;
    // 交线上最靠近原点的点：n1·x = -d1, n2·x = -d2, dir·x = 0
    double A[3][3] = { { n1[0], n1[1], n1[2] }, { n2[0], n2[1], n2[2] },
                       { dir[0], dir[1], dir[2] } };
    const double rhs[3] = { -d1, -d2, 0.0 };
    double sol[3] = { 0, 0, 0 };
    if (!detail::solve3x3(A, rhs, sol))
        return false;
    for (int k = 0; k < 3; ++k) {
        out_point[k] = (float)sol[k];
        out_dir[k] = (float)dir[k];
    }
    return true;
}

// ---------------- heightmap：3D → 2D 桥接 ----------------

template <typename PointT>
HeightMap CloudProcessor<PointT>::cloudToHeightMap(const CloudConstPtr& src,
                                                   double cell, int mode)
{
    HeightMap hm;
    if (!src || src->empty() || cell <= 0 || mode < 0 || mode > 3)
        return hm;

    // 有效点 XY 范围（不信任 is_dense，非有限点剔除）
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    for (const PointT& p : src->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        if (!any) {
            x0 = x1 = p.x;
            y0 = y1 = p.y;
            any = true;
        } else {
            if (p.x < x0) x0 = p.x;
            if (p.x > x1) x1 = p.x;
            if (p.y < y0) y0 = p.y;
            if (p.y > y1) y1 = p.y;
        }
    }
    if (!any)
        return hm;

    const int width = (int)std::floor((x1 - x0) / cell) + 1;
    const int height = (int)std::floor((y1 - y0) / cell) + 1;
    constexpr double kMaxCells = 20000000.0;   // 2000 万（80MB float + 20MB mask）
    if (width <= 0 || height <= 0 || (double)width * (double)height > kMaxCells)
        return hm;   // cell 过小/范围异常 → invalid（防撑爆内存）

    hm.width = width;
    hm.height = height;
    hm.x0 = x0;
    hm.y0 = y0;
    hm.cell = cell;
    const size_t n = (size_t)width * (size_t)height;
    hm.cells.assign(n, std::numeric_limits<float>::quiet_NaN());
    hm.mask.assign(n, 0);
    std::vector<double> acc(n, 0.0);   // mean 模式累加器
    std::vector<int> cnt(n, 0);

    for (const PointT& p : src->points) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            continue;
        const int col = (int)std::floor(((double)p.x - x0) / cell);
        const int row = (int)std::floor(((double)p.y - y0) / cell);
        if (col < 0 || row < 0 || col >= width || row >= height)
            continue;   // 浮点边界保护（理论上不会发生）
        const size_t idx = (size_t)row * (size_t)width + (size_t)col;
        if (mode == 0) {          // max：取最高
            if (!hm.mask[idx] || (float)p.z > hm.cells[idx])
                hm.cells[idx] = (float)p.z;
        } else if (mode == 1) {   // min：取最低
            if (!hm.mask[idx] || (float)p.z < hm.cells[idx])
                hm.cells[idx] = (float)p.z;
        } else {                  // mean：累加后除
            acc[idx] += (double)p.z;
            ++cnt[idx];
        }
        hm.mask[idx] = 1;
    }
    if (mode == 2) {
        for (size_t i = 0; i < n; ++i)
            if (cnt[i] > 0)
                hm.cells[i] = (float)(acc[i] / (double)cnt[i]);
    } else if (mode == 3) {   // 点计数密度图：cnt 已在栅格化阶段累计
        for (size_t i = 0; i < n; ++i)
            if (cnt[i] > 0)
                hm.cells[i] = (float)cnt[i];
    }

    hm.valid = true;
    return hm;
}

// 厚度图：上/下表面沿 Z 配对求差（栅格范围取两云包围盒并集）
template <typename PointT>
HeightMap CloudProcessor<PointT>::thicknessMap(const CloudConstPtr& top,
                                               const CloudConstPtr& bottom, double cell)
{
    HeightMap hm;
    if (!top || !bottom || top->empty() || bottom->empty() || cell <= 0)
        return hm;

    // 两云有效点 XY 包围盒并集（不信任 is_dense）
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    const auto grow = [&any, &x0, &y0, &x1, &y1](const CloudConstPtr& c) {
        for (const PointT& p : c->points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                continue;
            if (!any) {
                x0 = x1 = p.x;
                y0 = y1 = p.y;
                any = true;
            } else {
                if (p.x < x0) x0 = p.x;
                if (p.x > x1) x1 = p.x;
                if (p.y < y0) y0 = p.y;
                if (p.y > y1) y1 = p.y;
            }
        }
    };
    grow(top);
    grow(bottom);
    if (!any)
        return hm;

    const int width = (int)std::floor((x1 - x0) / cell) + 1;
    const int height = (int)std::floor((y1 - y0) / cell) + 1;
    constexpr double kMaxCells = 20000000.0;
    if (width <= 0 || height <= 0 || (double)width * (double)height > kMaxCells)
        return hm;

    hm.width = width;
    hm.height = height;
    hm.x0 = x0;
    hm.y0 = y0;
    hm.cell = cell;
    const size_t n = (size_t)width * (size_t)height;
    hm.cells.assign(n, std::numeric_limits<float>::quiet_NaN());
    hm.mask.assign(n, 0);

    // 上表面每格最高 / 下表面每格最低（两遍独立栅格化）
    const auto rasterize = [&](const CloudConstPtr& c, bool takeMax, std::vector<float>& dst) {
        for (const PointT& p : c->points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
                continue;
            const int col = (int)std::floor(((double)p.x - x0) / cell);
            const int row = (int)std::floor(((double)p.y - y0) / cell);
            if (col < 0 || row < 0 || col >= width || row >= height)
                continue;
            const size_t idx = (size_t)row * (size_t)width + (size_t)col;
            const bool empty = std::isnan(dst[idx]);
            if (empty || (takeMax ? (float)p.z > dst[idx] : (float)p.z < dst[idx]))
                dst[idx] = (float)p.z;
        }
    };
    std::vector<float> zTop(n, std::numeric_limits<float>::quiet_NaN());
    std::vector<float> zBot(n, std::numeric_limits<float>::quiet_NaN());
    rasterize(top, true, zTop);
    rasterize(bottom, false, zBot);

    // 同格双有效 → 厚度 = zTop − zBottom
    for (size_t i = 0; i < n; ++i) {
        if (!std::isnan(zTop[i]) && !std::isnan(zBot[i])) {
            hm.cells[i] = zTop[i] - zBot[i];
            hm.mask[i] = 1;
        }
    }
    hm.valid = true;
    return hm;
}

// 高度图反向点云化：有效格 → 格心点
template <typename PointT>
typename CloudProcessor<PointT>::CloudPtr
CloudProcessor<PointT>::heightMapToCloud(const HeightMap& hm)
{
    if (!hm.valid || hm.width <= 0 || hm.height <= 0 ||
        hm.cells.size() != (size_t)hm.width * (size_t)hm.height ||
        hm.mask.size() != hm.cells.size())
        return nullptr;
    CloudPtr out(new pcl::PointCloud<PointT>);
    out->reserve(hm.mask.size());
    for (int row = 0; row < hm.height; ++row) {
        for (int col = 0; col < hm.width; ++col) {
            const size_t idx = (size_t)row * (size_t)hm.width + (size_t)col;
            if (!hm.mask[idx])
                continue;
            PointT p{};   // 值初始化（rgb/intensity 清零）
            p.x = (float)(hm.x0 + ((double)col + 0.5) * hm.cell);
            p.y = (float)(hm.y0 + ((double)row + 0.5) * hm.cell);
            p.z = hm.cells[idx];
            out->push_back(p);
        }
    }
    out->width = (uint32_t)out->size();
    out->height = 1;
    return out;
}

// =====================================================================
// 显式实例化白名单（业务层直接链接，无模板传染）
// =====================================================================

template class CloudProcessor<pcl::PointXYZ>;
template class CloudProcessor<pcl::PointXYZI>;
template class CloudProcessor<pcl::PointXYZRGB>;

} // namespace shapely
