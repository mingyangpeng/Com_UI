#ifndef SHAPELY_STEREO_H
#define SHAPELY_STEREO_H

// shapely::Stereo —— 三维空间基础数据管理与度量模块（Pimpl，公共头纯净）。
// 设计约束（见技术交底书）：
//   * 纯三维：只处理 X/Y/Z 坐标与空间度量，不做 2D 布尔运算；
//   * 无业务语义：只回答纯数学事实（距离/包围盒），不做业务判断；
//   * Pimpl：内部点云容器隐藏在 .cpp；
//   * RAII + 移动语义：禁拷贝；
//   * 2.5D 桥接：projectTo2D() 剥离 Z 生成 Geom。
// 泛化输入（鸭子类型模板，编译期提取 .x/.y/.z 转 double）：
//   cv::Point3f/Point3d、含 .x/.y/.z 的自定义结构体、
//   扁平数组 [x,y,z,...]、二维嵌套数组（内层取 [0][1][2]）。
//   PCL 点云输入走适配层 StereoPCLAdapter.hpp。

#include "Types.h"
#include "Geom.h"
#include "AlgExport.h"

#include <vector>

namespace shapely {

class SHAPELYALG_EXPORT Stereo
{
public:
    // ---------------- 生命周期（RAII，只移动） ----------------
    ~Stereo();
    Stereo(Stereo&& other) noexcept;
    Stereo& operator=(Stereo&& other) noexcept;
    Stereo(const Stereo&) = delete;
    Stereo& operator=(const Stereo&) = delete;

    // ---------------- 基础数据管理 ----------------

    /// 是否为空点集（未创建或 0 点）。所有度量对空对象安全：返回 0/-1/false。
    bool isEmpty() const;

    /// 当前持有的三维点数量。
    size_t size() const;

    /// 输出全部三维点（拷贝，单位与创建输入一致，本项目惯例 mm）。
    std::vector<Point3d> getPoints() const;

    /// 提取 Z 轴（高度）范围。
    /// [out] min_z  最小高度；空点集时置 0
    /// [out] max_z  最大高度；空点集时置 0
    void getZBounds(double& min_z, double& max_z) const;

    /// 按高度区间过滤，返回全新 Stereo 对象（原对象不变）。
    /// [in] min_z  下限（含）；[in] max_z 上限（含）。区间外的点全部剔除。
    Stereo filterByZRange(double min_z, double max_z) const;

    // ---------------- 包围盒与体积 ----------------

    /// 轴对齐包围盒（AABB）：三轴独立取 min/max。点数 >= 1 即有效。
    AABB3d getAABB() const;

    /// 有向包围盒（OBB）：PCA 主成分分析取三主轴，点集投影到主轴系求极值。
    /// 点数 < 3 时返回 invalid 的默认盒。第三轴存在 180° 等价解（对包围语义无影响）。
    OBB3d getOBB() const;

    /// AABB 体积（三边长乘积，mm³ 惯例）。空点集返回 0。
    double getAABBVolume() const;

    /// OBB 体积（8 × 三半长乘积）。通常显著小于 AABB 体积（更贴合真实物体）。
    double getOBBVolume() const;

    // ---------------- 点集间距离与位置（纯数学事实） ----------------

    /// 点集质心（各轴坐标的算术平均）。空点集返回 (0,0,0)。
    /// 用途：工件坐标系原点、旋转中心的自然选择。
    Point3d getCentroid() const;

    /// 两个点集间最近的欧氏距离（KD 树加速：对 other 建树 + 逐点查询，O(n·log m)）。
    /// 单位：与坐标同单位。任一方为空返回 -1。
    /// 对同一 other 反复查询时，请复用 KdTree3（建树一次）避免重复建树开销。
    double minDistanceTo(const Stereo& other) const;

    /// 寻找两点集中距离最近的一对点（KD 树加速，见 minDistanceTo）。
    /// [in]  other     另一个点集
    /// [out] pt_self  本点集中的最近点
    /// [out] pt_other other 中的最近点
    /// 任一方为空时输出 (0,0,0)。
    void findClosestPair(const Stereo& other,
                         Point3d& pt_self, Point3d& pt_other) const;

    /// 两点集间是否存在坐标重合（欧氏距离 <= tolerance）的点对。
    /// [in] tolerance  判重容差，与坐标同单位；建议传物理精度（如 1e-3）
    bool hasCoincidentPointWith(const Stereo& other, double tolerance) const;

    /// 全部点到其最近邻的平均距离（KD 树实现，O(n·log n)）。
    /// 用途：点云密度自适应选 radius/k 参数的常用依据。
    /// 返回：-1 = 空点集 / 点数 < 2。
    double meanNearestNeighborDistance() const;

    // ---------------- 点-几何距离与判定（静态工具） ----------------

    /// 点到平面的有符号距离：平面 ax+by+cz+d=0（plane = [a,b,c,d]，
    /// 法向不需单位化，内部除以 |n|；符号约定 = 法向朝一侧为正）。
    /// |n| = 0 返回 0。
    static double distanceToPlane(const Point3d& p, const double plane[4]);

    /// 点到 AABB 的最短距离：盒内（含边界）= 0；盒外 = 到最近面/棱/角的距离。
    /// box.invalid 返回 -1。
    static double distanceToBox(const Point3d& p, const AABB3d& box);

    /// 点是否在 AABB 内（含边界）。box.invalid 返回 false。
    static bool pointInAABB(const Point3d& p, const AABB3d& box);

    /// 点是否在 OBB 内（含边界）：p 变换到盒主轴系后逐轴 |p_local[d]| <= extents[d]
    ///（p_local[d] = rotation[d] · (p − center)，见 Types.h 的 OBB3d 约定）。
    static bool pointInOBB(const Point3d& p, const OBB3d& box);

    // ---------------- 2.5D 降维桥接 ----------------

    /// 剥离 Z 值，XY 点集 → Geom 凸包多边形（自动闭合）。
    /// 点数 < 3 时返回空 Geom（无法构成面）。返回对象可继续做全部 Geom 面运算。
    Geom projectTo2D() const;

    // ---------------- 坐标系变换 ----------------

    /// 应用 4x4 刚体变换（行主序，p' = T·p），返回新点集。
    /// [in] matrix  16 个元素按行主序：[r00 r01 r02 tx | r10 r11 r12 ty | r20 r21 r22 tz | 0001]
    /// 典型用途：扫描坐标系 → 工件坐标系（配准结果 T 的应用端）。
    Stereo transform(const float matrix[16]) const;

    /// 平移点云，返回新点集（原对象不变）。
    Stereo translate(double dx, double dy, double dz) const;

    // ---------------- 4x4 变换矩阵工具（静态，配准链常用） ----------------

    /// 4x4 行主序矩阵求逆（高斯消元 + 部分主元；奇异/非有限返回 false）。
    /// 用途：配准结果 T 的逆向应用（工件系 → 扫描系）。
    static bool invertTransform(const float matrix[16], float out_inverse[16]);

    /// 两变换串联：out = B · A（先应用 A 再应用 B；行主序，p' = M·p）。
    /// 典型用途：粗对位 T1 与精配准 T2 合成为单次变换。
    static void composeTransform(const float B[16], const float A[16], float out[16]);

    // =====================================================================
    // 静态工厂（模板，Header-Only）
    // =====================================================================

    /// 从 3D 点集创建：元素只需含 .x/.y/.z 成员（鸭子类型）。
    ///   cv::Point3f/Point3d、含三成员的自定义结构体均直接可用。
    /// 坐标统一 static_cast 为 double 存储。
    template <typename P> static Stereo createFromPoints(const std::vector<P>& pts);

    /// 从一维扁平数组创建：[x1,y1,z1, x2,y2,z2, ...] 三元组顺序解析。
    /// 末尾不足 3 个的元素被忽略。
    template <typename T> static Stereo createFromFlatArray(const std::vector<T>& flat);

    /// 从二维嵌套数组创建：内层取索引 0/1/2 作为 X/Y/Z，不足 3 个的行跳过。
    template <typename T> static Stereo createFromNestedArray(const std::vector<std::vector<T>>& nested);

    /// 从轴对齐包围盒创建：盒的 8 个角点作为点集。box.valid=false 返回空。
    static Stereo createFromBox(const AABB3d& box);

    /// 从有向包围盒创建：center ± extents 沿三主轴展开为 8 角点。
    static Stereo createFromBox(const OBB3d& box);

    /// 从边界值创建：六个边界值直接构成 AABB 再取 8 角点。
    static Stereo createFromBox(double min_x, double min_y, double min_z,
                                double max_x, double max_y, double max_z);

private:
    friend class StereoPCLAdapter;
    struct Impl;                     // Pimpl（定义在 .cpp）
    Stereo();                        // 空对象
    static Stereo makeFromPoints(std::vector<Point3d> pts);   // 内部构建（.cpp）

    // Pimpl：内部点云容器（实际类型在 .cpp）
    Impl* impl_ = nullptr;
};

// =====================================================================
// KdTree3：三维点集 KD 树空间索引（纯数学无依赖，Pimpl 只移动）
// 用途：对同一目标点集反复做最近邻/半径搜索（minDistanceTo 单次调用的加速版）。
// =====================================================================

class SHAPELYALG_EXPORT KdTree3
{
public:
    KdTree3();
    ~KdTree3();
    KdTree3(KdTree3&& other) noexcept;
    KdTree3& operator=(KdTree3&& other) noexcept;
    KdTree3(const KdTree3&) = delete;
    KdTree3& operator=(const KdTree3&) = delete;

    /// 用点集建树（内部拷贝坐标；平均 O(n log n)，中位数切分保证平衡）。
    /// 返回 false = 点数为 0（空树：查询一律失败/空列表）。
    bool build(const std::vector<Point3d>& pts);

    /// 最近邻查询：out_index = build 输入的行号，out_dist = 欧氏距离。
    /// 空树返回 false（输出置 -1）。
    bool nearest(const Point3d& q, int& out_index, double& out_dist) const;

    /// 半径搜索：半径内全部点（含边界 dist == radius），按距离升序（同距按索引升序）。
    /// 空树输出空列表。
    void radiusSearch(const Point3d& q, double radius,
                      std::vector<int>& out_indices, std::vector<double>& out_dists) const;

    /// K 近邻：按距离升序输出至多 k 个（同距按索引升序；k > 点数 = 全部点）。
    /// 有界堆 + 剪枝递归，O(log n) 平均。空树 / k <= 0 输出空列表。
    void knn(const Point3d& q, int k,
             std::vector<int>& out_indices, std::vector<double>& out_dists) const;

    /// 是否为空树（未 build / 建自空点集）。
    bool empty() const;

    /// 树内点数（= build 输入点数；空树 0）。
    size_t size() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// =====================================================================
// 模板工厂实现（Header-Only）
// =====================================================================

namespace detail {

template <typename P>
inline std::vector<Point3d> normalize3D(const std::vector<P>& pts)
{
    std::vector<Point3d> out;
    out.reserve(pts.size());
    for (const P& p : pts)
        out.push_back(Point3d{ (double)p.x, (double)p.y, (double)p.z });
    return out;
}

} // namespace detail

template <typename P>
inline Stereo Stereo::createFromPoints(const std::vector<P>& pts)
{
    return makeFromPoints(detail::normalize3D(pts));
}

template <typename T>
inline Stereo Stereo::createFromFlatArray(const std::vector<T>& flat)
{
    std::vector<Point3d> pts;
    const size_t n = flat.size() / 3;
    pts.reserve(n);
    for (size_t i = 0; i < n; ++i)
        pts.push_back(Point3d{ (double)flat[i * 3], (double)flat[i * 3 + 1], (double)flat[i * 3 + 2] });
    return makeFromPoints(std::move(pts));
}

template <typename T>
inline Stereo Stereo::createFromNestedArray(const std::vector<std::vector<T>>& nested)
{
    std::vector<Point3d> pts;
    pts.reserve(nested.size());
    for (const std::vector<T>& p : nested)
        if (p.size() >= 3)
            pts.push_back(Point3d{ (double)p[0], (double)p[1], (double)p[2] });
    return makeFromPoints(std::move(pts));
}

} // namespace shapely

#endif // SHAPELY_STEREO_H
