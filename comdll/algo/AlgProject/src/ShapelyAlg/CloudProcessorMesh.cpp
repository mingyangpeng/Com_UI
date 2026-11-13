#include "CloudProcessor.h"

#include <algorithm>
#include <cmath>
#include <limits>

// CloudProcessorMesh.cpp —— 点-网格距离家族（基准实测唯一热点）专用编译单元。
// 单独 TU 编译的原因：/openmp:llvm 对含 Eigen/PCL 重型模板的 CloudProcessor.cpp
// 会产生访问冲突（MSVC OpenMP 与对齐栈对象共存的已知问题）——本 TU 只含纯 POD
// 数学，OpenMP 并行安全。OpenMP 决策依据见 dispatch/SHAPELY_ROADMAP.md 方向 D。

namespace {

using shapely::Point3d;

// 点到三角面最近点（Ericson, Real-Time Collision Detection 5.1.5，精确）
Point3d closestPointOnTriangle(const Point3d& a, const Point3d& b, const Point3d& c,
                               const Point3d& p)
{
    const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
    const double acx = c.x - a.x, acy = c.y - a.y, acz = c.z - a.z;
    const double apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
    const double d1 = abx * apx + aby * apy + abz * apz;
    const double d2 = acx * apx + acy * apy + acz * apz;
    if (d1 <= 0 && d2 <= 0)
        return a;
    const double bpx = p.x - b.x, bpy = p.y - b.y, bpz = p.z - b.z;
    const double d3 = abx * bpx + aby * bpy + abz * bpz;
    const double d4 = acx * bpx + acy * bpy + acz * bpz;
    if (d3 >= 0 && d4 <= d3)
        return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double v = d1 / (d1 - d3);
        return Point3d{ a.x + v * abx, a.y + v * aby, a.z + v * abz };
    }
    const double cpx = p.x - c.x, cpy = p.y - c.y, cpz = p.z - c.z;
    const double d5 = abx * cpx + aby * cpy + abz * cpz;
    const double d6 = acx * cpx + acy * cpy + acz * cpz;
    if (d6 >= 0 && d5 <= d6)
        return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double w = d2 / (d2 - d6);
        return Point3d{ a.x + w * acx, a.y + w * acy, a.z + w * acz };
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return Point3d{ b.x + w * (c.x - b.x), b.y + w * (c.y - b.y), b.z + w * (c.z - b.z) };
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    return Point3d{ a.x + abx * v + acx * w, a.y + aby * v + acy * w,
                    a.z + abz * v + acz * w };
}

// Möller–Trumbore：从 p 沿 dir 发射射线是否穿越三角面（t > 0，含边界）。
// 射线方向取固定斜向（避开轴对齐棱线——轴对齐射线命中共享棱会被两三角形双重计数）
bool rayCrossesTriangle(const Point3d& p, const Point3d& a, const Point3d& b, const Point3d& c,
                        const double dirx, const double diry, const double dirz)
{
    const double e1x = b.x - a.x, e1y = b.y - a.y, e1z = b.z - a.z;
    const double e2x = c.x - a.x, e2y = c.y - a.y, e2z = c.z - a.z;
    const double hx = diry * e2z - dirz * e2y;
    const double hy = dirz * e2x - dirx * e2z;
    const double hz = dirx * e2y - diry * e2x;
    const double det = e1x * hx + e1y * hy + e1z * hz;
    if (std::fabs(det) < 1e-12)
        return false;
    const double f = 1.0 / det;
    const double sx = p.x - a.x, sy = p.y - a.y, sz = p.z - a.z;
    const double u = f * (sx * hx + sy * hy + sz * hz);
    if (u < 0.0 || u > 1.0)
        return false;
    // q = s × e1
    const double qx = sy * e1z - sz * e1y;
    const double qy = sz * e1x - sx * e1z;
    const double qz = sx * e1y - sy * e1x;
    const double v = f * (dirx * qx + diry * qy + dirz * qz);
    if (v < 0.0 || u + v > 1.0)
        return false;
    const double t = f * (e2x * qx + e2y * qy + e2z * qz);
    return t > 1e-12;
}

} // namespace

namespace shapely {

// 点-网格精确最近距离：基准实测热点（20k×12.8 万面 = 29.3s）→ OpenMP 按 point 并行
//（out[i] 写独立；OpenMP 2.0 要求 signed 循环变量）
template <typename PointT>
std::vector<float> CloudProcessor<PointT>::measurePointToMeshDistance(const CloudConstPtr& src,
                                                                      const Mesh3d& mesh)
{
    std::vector<float> out;
    if (!src || src->empty() || !mesh.valid || mesh.triangles.empty())
        return out;
    const int nv = (int)mesh.vertices.size();
    out.resize(src->size());
    const int np = (int)src->size();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < np; ++i) {
        const PointT& p = (*src)[(size_t)i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            out[(size_t)i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        const Point3d pt{ (double)p.x, (double)p.y, (double)p.z };
        double best = std::numeric_limits<double>::max();
        for (const std::array<int, 3>& t : mesh.triangles) {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
                continue;
            const Point3d q = closestPointOnTriangle(mesh.vertices[(size_t)t[0]],
                                                     mesh.vertices[(size_t)t[1]],
                                                     mesh.vertices[(size_t)t[2]], pt);
            const double dx = q.x - pt.x, dy = q.y - pt.y, dz = q.z - pt.z;
            const double d = dx * dx + dy * dy + dz * dz;
            if (d < best)
                best = d;
        }
        out[(size_t)i] = best < std::numeric_limits<double>::max()
                             ? (float)std::sqrt(best)
                             : std::numeric_limits<float>::quiet_NaN();
    }
    return out;
}

// 带符号版本：精确最近距离 × 射线奇偶符号（仅闭合网格有物理意义）
template <typename PointT>
std::vector<float> CloudProcessor<PointT>::measurePointToMeshSignedDistance(
    const CloudConstPtr& src, const Mesh3d& mesh)
{
    std::vector<float> out;
    if (!src || src->empty() || !mesh.valid || mesh.triangles.empty())
        return out;
    const int nv = (int)mesh.vertices.size();
    out.resize(src->size());
    // 固定斜向射线（单位化 (1, 0.3, 0.2)）——轴对齐射线在轴对齐网格上会命中
    // 共享棱导致双重计数，斜向属于测度零
    const double dirx = 0.9128709291752769, diry = 0.2738612787525831,
                 dirz = 0.18257418583505536;
    const int np = (int)src->size();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
    for (int i = 0; i < np; ++i) {
        const PointT& p = (*src)[(size_t)i];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            out[(size_t)i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        const Point3d pt{ (double)p.x, (double)p.y, (double)p.z };
        double best = std::numeric_limits<double>::max();
        int parity = 0;
        for (const std::array<int, 3>& t : mesh.triangles) {
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[0] >= nv || t[1] >= nv || t[2] >= nv)
                continue;
            const Point3d q = closestPointOnTriangle(mesh.vertices[(size_t)t[0]],
                                                     mesh.vertices[(size_t)t[1]],
                                                     mesh.vertices[(size_t)t[2]], pt);
            const double dx = q.x - pt.x, dy = q.y - pt.y, dz = q.z - pt.z;
            const double d = dx * dx + dy * dy + dz * dz;
            if (d < best)
                best = d;
            if (rayCrossesTriangle(pt, mesh.vertices[(size_t)t[0]],
                                   mesh.vertices[(size_t)t[1]],
                                   mesh.vertices[(size_t)t[2]], dirx, diry, dirz))
                ++parity;
        }
        if (best >= std::numeric_limits<double>::max()) {
            out[(size_t)i] = std::numeric_limits<float>::quiet_NaN();
            continue;
        }
        const double dist = std::sqrt(best);
        out[i] = (float)((parity % 2 == 1) ? -dist : dist);
    }
    return out;
}

// 白名单点型 × 两函数显式实例化（定义在本 TU，CloudProcessor.cpp 的类级
// 实例化不覆盖这两个成员——须在此补齐）
template std::vector<float> CloudProcessor<pcl::PointXYZ>::measurePointToMeshDistance(
    const CloudProcessor<pcl::PointXYZ>::CloudConstPtr&, const Mesh3d&);
template std::vector<float> CloudProcessor<pcl::PointXYZ>::measurePointToMeshSignedDistance(
    const CloudProcessor<pcl::PointXYZ>::CloudConstPtr&, const Mesh3d&);
template std::vector<float> CloudProcessor<pcl::PointXYZI>::measurePointToMeshDistance(
    const CloudProcessor<pcl::PointXYZI>::CloudConstPtr&, const Mesh3d&);
template std::vector<float> CloudProcessor<pcl::PointXYZI>::measurePointToMeshSignedDistance(
    const CloudProcessor<pcl::PointXYZI>::CloudConstPtr&, const Mesh3d&);
template std::vector<float> CloudProcessor<pcl::PointXYZRGB>::measurePointToMeshDistance(
    const CloudProcessor<pcl::PointXYZRGB>::CloudConstPtr&, const Mesh3d&);
template std::vector<float> CloudProcessor<pcl::PointXYZRGB>::measurePointToMeshSignedDistance(
    const CloudProcessor<pcl::PointXYZRGB>::CloudConstPtr&, const Mesh3d&);

} // namespace shapely
