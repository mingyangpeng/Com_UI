// Stereo.cpp —— Stereo 三维数据管理与度量实现（Pimpl，纯 double 数学）。
// 不依赖 PCL/GEOS：内部就是 vector<Point3d> + 纯数学（PCA/SAT/暴力最近邻）。
// 点数大时最近邻为 O(n·m)——业务层应先下采样或用 CloudProcessor 的空间树。

#include "Stereo.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <queue>

namespace shapely {

// ---------------- Pimpl：内部点集容器 ----------------

struct Stereo::Impl
{
    std::vector<Point3d> pts;
};

// ---------------- 生命周期 ----------------

Stereo::Stereo() : impl_(new Impl) {}
Stereo::~Stereo() { delete impl_; }

Stereo::Stereo(Stereo&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = new Impl;
}

Stereo& Stereo::operator=(Stereo&& other) noexcept
{
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = new Impl;
    }
    return *this;
}

Stereo Stereo::makeFromPoints(std::vector<Point3d> pts)
{
    Stereo s;
    s.impl_->pts = std::move(pts);
    return s;
}

// ---------------- 基础数据管理 ----------------

bool Stereo::isEmpty() const
{
    return !impl_ || impl_->pts.empty();
}

size_t Stereo::size() const
{
    return impl_ ? impl_->pts.size() : 0;
}

std::vector<Point3d> Stereo::getPoints() const
{
    return impl_ ? impl_->pts : std::vector<Point3d>();
}

void Stereo::getZBounds(double& min_z, double& max_z) const
{
    min_z = 0.0;
    max_z = 0.0;
    if (isEmpty())
        return;
    const std::vector<Point3d>& pts = impl_->pts;
    min_z = max_z = pts[0].z;
    for (const Point3d& p : pts) {
        if (p.z < min_z) min_z = p.z;
        if (p.z > max_z) max_z = p.z;
    }
}

Stereo Stereo::filterByZRange(double min_z, double max_z) const
{
    Stereo out;
    if (!impl_)
        return out;
    std::vector<Point3d> filtered;
    const std::vector<Point3d>& pts = impl_->pts;
    filtered.reserve(pts.size());
    for (const Point3d& p : pts)
        if (p.z >= min_z && p.z <= max_z)
            filtered.push_back(p);
    return makeFromPoints(std::move(filtered));
}

// ---------------- 包围盒与体积 ----------------

AABB3d Stereo::getAABB() const
{
    AABB3d bb;
    if (isEmpty())
        return bb;
    const std::vector<Point3d>& pts = impl_->pts;
    bb.min_x = bb.max_x = pts[0].x;
    bb.min_y = bb.max_y = pts[0].y;
    bb.min_z = bb.max_z = pts[0].z;
    for (const Point3d& p : pts) {
        if (p.x < bb.min_x) bb.min_x = p.x;
        if (p.x > bb.max_x) bb.max_x = p.x;
        if (p.y < bb.min_y) bb.min_y = p.y;
        if (p.y > bb.max_y) bb.max_y = p.y;
        if (p.z < bb.min_z) bb.min_z = p.z;
        if (p.z > bb.max_z) bb.max_z = p.z;
    }
    bb.valid = true;
    return bb;
}

// PCA 有向包围盒：协方差矩阵特征分解（雅可比迭代）取三主轴，
// 点集投影到主轴坐标系求极值 → 中心/半长/旋转矩阵。
OBB3d Stereo::getOBB() const
{
    OBB3d obb;
    const std::vector<Point3d>& pts = impl_ ? impl_->pts : std::vector<Point3d>();
    if (pts.size() < 3)
        return obb;

    // 质心
    double cx = 0, cy = 0, cz = 0;
    for (const Point3d& p : pts) { cx += p.x; cy += p.y; cz += p.z; }
    const double inv = 1.0 / (double)pts.size();
    cx *= inv; cy *= inv; cz *= inv;

    // 协方差（对称 3x3）
    double cov[6] = { 0, 0, 0, 0, 0, 0 };   // xx, xy, xz, yy, yz, zz
    for (const Point3d& p : pts) {
        const double dx = p.x - cx, dy = p.y - cy, dz = p.z - cz;
        cov[0] += dx * dx; cov[1] += dx * dy; cov[2] += dx * dz;
        cov[3] += dy * dy; cov[4] += dy * dz;
        cov[5] += dz * dz;
    }
    for (double& v : cov) v *= inv;

    // 雅可比特征分解（对称矩阵 → 特征值 + 特征向量）
    double A[3][3] = {
        { cov[0], cov[1], cov[2] },
        { cov[1], cov[3], cov[4] },
        { cov[2], cov[4], cov[5] },
    };
    double V[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int iter = 0; iter < 64; ++iter) {
        // 找最大非对角元素
        int mi = 0, mj = 1;
        double off = std::fabs(A[0][1]);
        if (std::fabs(A[0][2]) > off) { off = std::fabs(A[0][2]); mi = 0; mj = 2; }
        if (std::fabs(A[1][2]) > off) { off = std::fabs(A[1][2]); mi = 1; mj = 2; }
        if (off < 1e-12)
            break;
        // 旋转消元（J=[c s; -s c]，A←JᵀAJ）：消元条件 tan2θ = 2b/(d−a)，
        // 分母必须是 A[mj][mj]−A[mi][mi]——写反会使旋转不减反增非对角元，
        // 迭代不收敛、主轴任意（2026-11-09 回归：旋转盒 OBB 体积失真实锤）
        const double theta = 0.5 * std::atan2(2.0 * A[mi][mj],
                                               A[mj][mj] - A[mi][mi]);
        const double c = std::cos(theta), s = std::sin(theta);
        for (int k = 0; k < 3; ++k) {
            const double aki = A[k][mi], akj = A[k][mj];
            A[k][mi] = c * aki - s * akj;
            A[k][mj] = s * aki + c * akj;
        }
        for (int k = 0; k < 3; ++k) {
            const double aik = A[mi][k], ajk = A[mj][k];
            A[mi][k] = c * aik - s * ajk;
            A[mj][k] = s * aik + c * ajk;
        }
        for (int k = 0; k < 3; ++k) {
            const double vki = V[k][mi], vkj = V[k][mj];
            V[k][mi] = c * vki - s * vkj;
            V[k][mj] = s * vki + c * vkj;
        }
    }

    // 特征值排序（大→小），特征向量即主轴
    int order[3] = { 0, 1, 2 };
    const double eig[3] = { A[0][0], A[1][1], A[2][2] };
    std::sort(order, order + 3, [&](int a, int b) { return eig[a] > eig[b]; });

    // 主轴（行向量）→ 旋转矩阵（行主序，行 = 主轴方向）
    double axes[3][3];
    for (int d = 0; d < 3; ++d)
        for (int k = 0; k < 3; ++k)
            axes[d][k] = V[k][order[d]];

    // 投影到主轴坐标系求极值
    double lo[3] = { 0, 0, 0 }, hi[3] = { 0, 0, 0 };
    for (size_t i = 0; i < pts.size(); ++i) {
        const double dx = pts[i].x - cx, dy = pts[i].y - cy, dz = pts[i].z - cz;
        for (int d = 0; d < 3; ++d) {
            const double proj = dx * axes[d][0] + dy * axes[d][1] + dz * axes[d][2];
            if (i == 0) { lo[d] = hi[d] = proj; }
            else {
                if (proj < lo[d]) lo[d] = proj;
                if (proj > hi[d]) hi[d] = proj;
            }
        }
    }

    // 中心 = 质心 + 主轴方向偏移
    obb.center = Point3d{
        cx + 0.5 * (lo[0] + hi[0]) * axes[0][0] +
        0.5 * (lo[1] + hi[1]) * axes[1][0] +
        0.5 * (lo[2] + hi[2]) * axes[2][0],
        cy + 0.5 * (lo[0] + hi[0]) * axes[0][1] +
        0.5 * (lo[1] + hi[1]) * axes[1][1] +
        0.5 * (lo[2] + hi[2]) * axes[2][1],
        cz + 0.5 * (lo[0] + hi[0]) * axes[0][2] +
        0.5 * (lo[1] + hi[1]) * axes[1][2] +
        0.5 * (lo[2] + hi[2]) * axes[2][2],
    };
    for (int d = 0; d < 3; ++d)
        obb.extents[d] = 0.5 * (hi[d] - lo[d]);
    for (int d = 0; d < 3; ++d)
        for (int k = 0; k < 3; ++k)
            obb.rotation[d * 3 + k] = axes[d][k];
    obb.valid = true;
    return obb;
}

double Stereo::getAABBVolume() const
{
    return getAABB().volume();
}

double Stereo::getOBBVolume() const
{
    return getOBB().volume();
}

// ---------------- 点集间距离（纯数学事实） ----------------

double Stereo::minDistanceTo(const Stereo& other) const
{
    if (isEmpty() || other.isEmpty())
        return -1.0;
    // KD 树加速：对 other 建树（O(m log m)）+ 逐点最近邻查询（O(n·log m)），
    // 替代暴力 O(n·m)——百万级扫描点云可用
    KdTree3 tree;
    if (!tree.build(other.impl_->pts))
        return -1.0;
    double best = std::numeric_limits<double>::max();
    for (const Point3d& p : impl_->pts) {
        int idx = -1;
        double d = 0;
        if (tree.nearest(p, idx, d) && d < best)
            best = d;
    }
    return best < std::numeric_limits<double>::max() ? best : -1.0;
}

void Stereo::findClosestPair(const Stereo& other,
                             Point3d& pt_self, Point3d& pt_other) const
{
    pt_self = Point3d();
    pt_other = Point3d();
    if (isEmpty() || other.isEmpty())
        return;
    const std::vector<Point3d>& A = impl_->pts;
    // KD 树加速：树建在 other 上，逐点查询最近的 B 点（输出索引即对方最近点）
    KdTree3 tree;
    if (!tree.build(other.impl_->pts))
        return;
    double best = std::numeric_limits<double>::max();
    int bestOther = -1;
    const Point3d* bestSelf = nullptr;
    for (const Point3d& a : A) {
        int idx = -1;
        double d = 0;
        if (tree.nearest(a, idx, d) && d < best) {
            best = d;
            bestOther = idx;
            bestSelf = &a;
        }
    }
    if (bestSelf && bestOther >= 0) {
        pt_self = *bestSelf;
        pt_other = other.impl_->pts[(size_t)bestOther];
    }
}

bool Stereo::hasCoincidentPointWith(const Stereo& other, double tolerance) const
{
    if (isEmpty() || other.isEmpty())
        return false;
    const std::vector<Point3d>& A = impl_->pts;
    const std::vector<Point3d>& B = other.impl_->pts;
    const double tol2 = tolerance * tolerance;
    for (const Point3d& a : A) {
        for (const Point3d& b : B) {
            const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            if (dx * dx + dy * dy + dz * dz <= tol2)
                return true;
        }
    }
    return false;
}

// ---------------- 2.5D 降维桥接 ----------------

Geom Stereo::projectTo2D() const
{
    if (isEmpty())
        return Geom();
    const std::vector<Point3d>& pts = impl_->pts;
    if (pts.size() < 3)
        return Geom();
    // XY 点集 → 凸包多边形（Geom::convexHull 输入线串即可得到凸包面）
    std::vector<Point2d> xy;
    xy.reserve(pts.size());
    for (const Point3d& p : pts)
        xy.push_back(Point2d{ p.x, p.y });
    Geom hull = Geom::createLine(xy).convexHull();
    return hull;
}

// ---------------- 坐标系变换 ----------------

Point3d Stereo::getCentroid() const
{
    Point3d c;
    if (isEmpty())
        return c;
    const std::vector<Point3d>& pts = impl_->pts;
    for (const Point3d& p : pts) {
        c.x += p.x;
        c.y += p.y;
        c.z += p.z;
    }
    const double inv = 1.0 / (double)pts.size();
    return Point3d{ c.x * inv, c.y * inv, c.z * inv };
}

Stereo Stereo::transform(const float matrix[16]) const
{
    Stereo out;
    if (isEmpty() || !matrix)
        return out;
    // p' = T·p（列向量左乘），行主序取元素
    std::vector<Point3d> pts;
    pts.reserve(impl_->pts.size());
    for (const Point3d& p : impl_->pts) {
        pts.push_back(Point3d{
            matrix[0] * p.x + matrix[1] * p.y + matrix[2] * p.z + matrix[3],
            matrix[4] * p.x + matrix[5] * p.y + matrix[6] * p.z + matrix[7],
            matrix[8] * p.x + matrix[9] * p.y + matrix[10] * p.z + matrix[11],
        });
    }
    return makeFromPoints(std::move(pts));
}

Stereo Stereo::translate(double dx, double dy, double dz) const
{
    Stereo out;
    if (isEmpty())
        return out;
    std::vector<Point3d> pts;
    pts.reserve(impl_->pts.size());
    for (const Point3d& p : impl_->pts)
        pts.push_back(Point3d{ p.x + dx, p.y + dy, p.z + dz });
    return makeFromPoints(std::move(pts));
}

// ---------------- 工厂（非模板部分） ----------------

static std::vector<Point3d> boxCorners(double min_x, double min_y, double min_z,
                                       double max_x, double max_y, double max_z)
{
    return {
        { min_x, min_y, min_z }, { max_x, min_y, min_z },
        { max_x, max_y, min_z }, { min_x, max_y, min_z },
        { min_x, min_y, max_z }, { max_x, min_y, max_z },
        { max_x, max_y, max_z }, { min_x, max_y, max_z },
    };
}

Stereo Stereo::createFromBox(const AABB3d& box)
{
    if (!box.valid)
        return Stereo();
    return makeFromPoints(boxCorners(box.min_x, box.min_y, box.min_z,
                                      box.max_x, box.max_y, box.max_z));
}

Stereo Stereo::createFromBox(const OBB3d& box)
{
    if (!box.valid)
        return Stereo();
    // 8 角点 = center ± extents·axes（axes = rotation 的行）
    std::vector<Point3d> pts;
    pts.reserve(8);
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                const double s[3] = { (double)sx, (double)sy, (double)sz };
                double x = box.center.x, y = box.center.y, z = box.center.z;
                for (int d = 0; d < 3; ++d) {
                    const double off = s[d] * box.extents[d];
                    x += off * box.rotation[d * 3 + 0];
                    y += off * box.rotation[d * 3 + 1];
                    z += off * box.rotation[d * 3 + 2];
                }
                pts.push_back(Point3d{ x, y, z });
            }
    return makeFromPoints(std::move(pts));
}

Stereo Stereo::createFromBox(double min_x, double min_y, double min_z,
                             double max_x, double max_y, double max_z)
{
    return makeFromPoints(boxCorners(min_x, min_y, min_z, max_x, max_y, max_z));
}

// ---------------- OBB3d SAT（Types.h 声明的实现） ----------------

namespace {

// 15 条分离轴 SAT：A 三轴 + B 三轴 + 9 叉积；任一轴分离即不相交
bool obbSeparated(const OBB3d& a, const OBB3d& b)
{
    const double* RA = a.rotation;
    const double* RB = b.rotation;
    const double T[3] = { b.center.x - a.center.x,
                          b.center.y - a.center.y,
                          b.center.z - a.center.z };
    // 轴取 rotation 的行（行 = 主轴方向，与 getOBB 的填充约定一致）
    const double axesA[3][3] = {
        { RA[0], RA[1], RA[2] },
        { RA[3], RA[4], RA[5] },
        { RA[6], RA[7], RA[8] },
    };
    const double axesB[3][3] = {
        { RB[0], RB[1], RB[2] },
        { RB[3], RB[4], RB[5] },
        { RB[6], RB[7], RB[8] },
    };
    auto testAxis = [&](const double L[3]) -> bool {
        const double len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (len < 1e-12)
            return false;   // 退化轴跳过
        const double l[3] = { L[0] / len, L[1] / len, L[2] / len };
        const double t = std::fabs(T[0] * l[0] + T[1] * l[1] + T[2] * l[2]);
        double ra = 0, rb = 0;
        for (int d = 0; d < 3; ++d) {
            ra += a.extents[d] * std::fabs(axesA[d][0] * l[0] + axesA[d][1] * l[1] + axesA[d][2] * l[2]);
            rb += b.extents[d] * std::fabs(axesB[d][0] * l[0] + axesB[d][1] * l[1] + axesB[d][2] * l[2]);
        }
        return t > ra + rb;   // true = 该轴分离
    };
    for (int i = 0; i < 3; ++i)
        if (testAxis(axesA[i])) return true;
    for (int j = 0; j < 3; ++j)
        if (testAxis(axesB[j])) return true;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            const double cross[3] = {
                axesA[i][1] * axesB[j][2] - axesA[i][2] * axesB[j][1],
                axesA[i][2] * axesB[j][0] - axesA[i][0] * axesB[j][2],
                axesA[i][0] * axesB[j][1] - axesA[i][1] * axesB[j][0],
            };
            if (testAxis(cross)) return true;
        }
    return false;
}

} // namespace

bool OBB3d::intersects(const OBB3d& other) const
{
    if (!valid || !other.valid)
        return false;
    return !obbSeparated(*this, other);
}

bool OBB3d::contains(const OBB3d& other) const
{
    if (!valid || !other.valid)
        return false;
    // OBB 包含 OBB：把 other 的 8 个角点变换到 this 的主轴坐标系，
    // 检查是否全部落在 [-extents, extents] 内。
    const double T[3] = { other.center.x - center.x,
                          other.center.y - center.y,
                          other.center.z - center.z };
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                const double s[3] = { (double)sx, (double)sy, (double)sz };
                double p[3] = { T[0], T[1], T[2] };
                for (int d = 0; d < 3; ++d) {
                    const double off = s[d] * other.extents[d];
                    p[0] += off * other.rotation[d * 3 + 0];
                    p[1] += off * other.rotation[d * 3 + 1];
                    p[2] += off * other.rotation[d * 3 + 2];
                }
                // p（this 原点坐标系）投影到 this 主轴
                for (int d = 0; d < 3; ++d) {
                    const double proj = p[0] * rotation[d * 3 + 0] +
                                        p[1] * rotation[d * 3 + 1] +
                                        p[2] * rotation[d * 3 + 2];
                    if (proj < -extents[d] - 1e-12 || proj > extents[d] + 1e-12)
                        return false;
                }
            }
    return true;
}

// =====================================================================
// 点-几何距离与判定（静态工具）
// =====================================================================

double Stereo::distanceToPlane(const Point3d& p, const double plane[4])
{
    const double a = plane[0], b = plane[1], c = plane[2], d = plane[3];
    const double n = std::sqrt(a * a + b * b + c * c);
    if (n < 1e-12)
        return 0.0;   // 非法法向：按头文件契约返回 0
    return (a * p.x + b * p.y + c * p.z + d) / n;
}

double Stereo::distanceToBox(const Point3d& p, const AABB3d& box)
{
    if (!box.valid)
        return -1.0;
    // 三轴独立夹到盒区间，剩余偏移即最近距离向量（盒内全为 0）
    const double dx = std::max({ box.min_x - p.x, p.x - box.max_x, 0.0 });
    const double dy = std::max({ box.min_y - p.y, p.y - box.max_y, 0.0 });
    const double dz = std::max({ box.min_z - p.z, p.z - box.max_z, 0.0 });
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Stereo::pointInAABB(const Point3d& p, const AABB3d& box)
{
    if (!box.valid)
        return false;
    return p.x >= box.min_x && p.x <= box.max_x &&
           p.y >= box.min_y && p.y <= box.max_y &&
           p.z >= box.min_z && p.z <= box.max_z;
}

bool Stereo::pointInOBB(const Point3d& p, const OBB3d& box)
{
    if (!box.valid)
        return false;
    // p_local[d] = rotation[d] · (p − center)，逐轴 |p_local[d]| <= extents[d]（含边界）
    const double rel[3] = { p.x - box.center.x, p.y - box.center.y, p.z - box.center.z };
    for (int d = 0; d < 3; ++d) {
        const double proj = rel[0] * box.rotation[d * 3 + 0] +
                            rel[1] * box.rotation[d * 3 + 1] +
                            rel[2] * box.rotation[d * 3 + 2];
        if (std::fabs(proj) > box.extents[d])
            return false;
    }
    return true;
}

double Stereo::meanNearestNeighborDistance() const
{
    if (!impl_ || impl_->pts.size() < 2)
        return -1.0;
    KdTree3 tree;
    if (!tree.build(impl_->pts))
        return -1.0;
    std::vector<int> idx;
    std::vector<double> dists;
    double sum = 0;
    for (const Point3d& p : impl_->pts) {
        tree.knn(p, 2, idx, dists);   // [0] = 自身（距离 0），[1] = 最近邻
        if (dists.size() < 2)
            return -1.0;
        sum += dists[1];
    }
    return sum / (double)impl_->pts.size();
}

// ---------------- 4x4 变换矩阵工具 ----------------

bool Stereo::invertTransform(const float matrix[16], float out_inverse[16])
{
    // 高斯-约当消元 + 部分主元（[A | I] → [I | A⁻¹]）
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            a[r][c] = (double)matrix[r * 4 + c];
        if (!std::isfinite(a[r][0]) || !std::isfinite(a[r][1]) || !std::isfinite(a[r][2]) ||
            !std::isfinite(a[r][3]))
            return false;
        for (int c = 4; c < 8; ++c)
            a[r][c] = (c == 4 + r) ? 1.0 : 0.0;   // 右侧拼单位阵
    }
    double scale = 0.0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            scale = (std::max)(scale, std::fabs(a[r][c]));
    if (scale <= 0.0)
        return false;
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col]))
                piv = r;
        if (std::fabs(a[piv][col]) < 1e-12 * scale)
            return false;   // 奇异
        if (piv != col)
            for (int c = col; c < 8; ++c)
                std::swap(a[col][c], a[piv][c]);
        const double inv = 1.0 / a[col][col];
        for (int c = col; c < 8; ++c)
            a[col][c] *= inv;
        for (int r = 0; r < 4; ++r) {
            if (r == col)
                continue;
            const double f = a[r][col];
            if (f == 0.0)
                continue;
            for (int c = col; c < 8; ++c)
                a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            if (!std::isfinite(a[r][4 + c]))
                return false;
            out_inverse[r * 4 + c] = (float)a[r][4 + c];
        }
    return true;
}

void Stereo::composeTransform(const float B[16], const float A[16], float out[16])
{
    // out = B · A（行主序；先应用 A 再应用 B）
    double tmp[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += (double)B[r * 4 + k] * (double)A[k * 4 + c];
            tmp[r * 4 + c] = s;
        }
    for (int i = 0; i < 16; ++i)
        out[i] = (float)tmp[i];
}

// =====================================================================
// KdTree3：三维点集 KD 树（中位数切分，平衡树；纯数学无依赖）
// =====================================================================

struct KdTree3::Impl
{
    struct Node
    {
        int point = -1;    // build 输入的行号
        int left = -1;
        int right = -1;
        int axis = 0;      // 切分轴 0/1/2
        double split = 0;  // 切分坐标（= 本节点点的 axis 坐标）
    };

    std::vector<Point3d> pts;
    std::vector<Node> nodes;
    int root = -1;

    static double coord(const Point3d& p, int axis)
    {
        return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
    }

    // 递归建树：对 idx[lo,hi) 取 axis 中位数为根（nth_element，保证 O(n log n) 平均）
    int buildRange(std::vector<int>& idx, int lo, int hi, int depth)
    {
        if (lo >= hi)
            return -1;
        const int axis = depth % 3;
        const int mid = lo + (hi - lo) / 2;
        std::nth_element(idx.begin() + lo, idx.begin() + mid, idx.begin() + hi,
                         [&](int x, int y) {
                             return coord(pts[(size_t)x], axis) < coord(pts[(size_t)y], axis);
                         });
        Node n;
        n.axis = axis;
        n.split = coord(pts[(size_t)idx[(size_t)mid]], axis);
        n.point = idx[(size_t)mid];
        nodes.push_back(n);
        const int me = (int)nodes.size() - 1;   // 用索引访问（递归可能重分配）
        nodes[(size_t)me].left = buildRange(idx, lo, mid, depth + 1);
        nodes[(size_t)me].right = buildRange(idx, mid + 1, hi, depth + 1);
        return me;
    }

    static double dist2(const Point3d& a, const Point3d& b)
    {
        const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    void nearestRec(int node, const Point3d& q, int& bestIdx, double& bestDist2) const
    {
        if (node < 0)
            return;
        const Node& n = nodes[(size_t)node];
        const double d2 = dist2(pts[(size_t)n.point], q);
        if (d2 < bestDist2) {
            bestDist2 = d2;
            bestIdx = n.point;
        }
        const double diff = coord(q, n.axis) - n.split;
        const int nearChild = diff < 0 ? n.left : n.right;
        const int farChild = diff < 0 ? n.right : n.left;
        nearestRec(nearChild, q, bestIdx, bestDist2);
        if (diff * diff < bestDist2)   // 剪枝：split 平面距查询点可能更近才搜远侧
            nearestRec(farChild, q, bestIdx, bestDist2);
    }

    void radiusRec(int node, const Point3d& q, double r,
                   std::vector<std::pair<double, int>>& out) const
    {
        if (node < 0)
            return;
        const Node& n = nodes[(size_t)node];
        const double d2 = dist2(pts[(size_t)n.point], q);
        if (d2 <= r * r)
            out.emplace_back(std::sqrt(d2), n.point);
        const double diff = coord(q, n.axis) - n.split;
        if (diff < 0) {
            radiusRec(n.left, q, r, out);
            if (std::fabs(diff) <= r)
                radiusRec(n.right, q, r, out);
        } else {
            radiusRec(n.right, q, r, out);
            if (std::fabs(diff) <= r)
                radiusRec(n.left, q, r, out);
        }
    }

    // K 近邻：有界大顶堆（dist2, 行号），堆满后按 split 平面剪枝
    void knnRec(int node, const Point3d& q, int k,
                std::priority_queue<std::pair<double, int>>& heap) const
    {
        if (node < 0)
            return;
        const Node& n = nodes[(size_t)node];
        const double d2 = dist2(pts[(size_t)n.point], q);
        if ((int)heap.size() < k) {
            heap.emplace(d2, n.point);
        } else if (d2 < heap.top().first) {
            heap.pop();
            heap.emplace(d2, n.point);
        }
        const double diff = coord(q, n.axis) - n.split;
        const int nearChild = diff < 0 ? n.left : n.right;
        const int farChild = diff < 0 ? n.right : n.left;
        knnRec(nearChild, q, k, heap);
        const double worst = (int)heap.size() < k
                                 ? std::numeric_limits<double>::max()
                                 : heap.top().first;
        if (diff * diff < worst)
            knnRec(farChild, q, k, heap);
    }
};

KdTree3::KdTree3() : impl_(new Impl) {}
KdTree3::~KdTree3() { delete impl_; }

KdTree3::KdTree3(KdTree3&& other) noexcept
    : impl_(other.impl_)
{
    other.impl_ = new Impl;
}

KdTree3& KdTree3::operator=(KdTree3&& other) noexcept
{
    if (this != &other) {
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = new Impl;
    }
    return *this;
}

bool KdTree3::build(const std::vector<Point3d>& pts)
{
    impl_->pts = pts;
    impl_->nodes.clear();
    impl_->root = -1;
    if (pts.empty())
        return false;
    impl_->nodes.reserve(pts.size());
    std::vector<int> idx((size_t)pts.size());
    for (size_t i = 0; i < idx.size(); ++i)
        idx[i] = (int)i;
    impl_->root = impl_->buildRange(idx, 0, (int)idx.size(), 0);
    return true;
}

bool KdTree3::nearest(const Point3d& q, int& out_index, double& out_dist) const
{
    out_index = -1;
    out_dist = -1.0;
    if (impl_->root < 0)
        return false;
    double bestDist2 = std::numeric_limits<double>::max();
    int bestIdx = -1;
    impl_->nearestRec(impl_->root, q, bestIdx, bestDist2);
    if (bestIdx < 0)
        return false;
    out_index = bestIdx;
    out_dist = std::sqrt(bestDist2);
    return true;
}

void KdTree3::radiusSearch(const Point3d& q, double radius,
                           std::vector<int>& out_indices, std::vector<double>& out_dists) const
{
    out_indices.clear();
    out_dists.clear();
    if (impl_->root < 0 || radius < 0)
        return;
    std::vector<std::pair<double, int>> hits;
    impl_->radiusRec(impl_->root, q, radius, hits);
    std::sort(hits.begin(), hits.end());   // (dist, index) 升序
    out_indices.reserve(hits.size());
    out_dists.reserve(hits.size());
    for (const auto& h : hits) {
        out_indices.push_back(h.second);
        out_dists.push_back(h.first);
    }
}

void KdTree3::knn(const Point3d& q, int k,
                  std::vector<int>& out_indices, std::vector<double>& out_dists) const
{
    out_indices.clear();
    out_dists.clear();
    if (impl_->root < 0 || k <= 0)
        return;
    std::priority_queue<std::pair<double, int>> heap;
    impl_->knnRec(impl_->root, q, k, heap);
    std::vector<std::pair<double, int>> hits;
    hits.reserve(heap.size());
    while (!heap.empty()) {
        hits.emplace_back(std::sqrt(heap.top().first), heap.top().second);
        heap.pop();
    }
    std::sort(hits.begin(), hits.end());   // (dist, index) 升序
    out_indices.reserve(hits.size());
    out_dists.reserve(hits.size());
    for (const auto& h : hits) {
        out_indices.push_back(h.second);
        out_dists.push_back(h.first);
    }
}

bool KdTree3::empty() const
{
    return !impl_ || impl_->root < 0;
}

size_t KdTree3::size() const
{
    return (impl_ && impl_->root >= 0) ? impl_->pts.size() : 0;
}

} // namespace shapely
