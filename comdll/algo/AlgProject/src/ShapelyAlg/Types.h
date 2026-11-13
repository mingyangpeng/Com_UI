#ifndef SHAPELY_TYPES_H
#define SHAPELY_TYPES_H

// shapely 库内部原生轻量数据结构（公共头文件，零第三方依赖）。
// 所有对外输出统一使用这些结构与 double 精度，不绑定 GEOS/OpenCV/PCL/Eigen。

#include <cstddef>
#include <type_traits>
#include <vector>

#include "AlgExport.h"

namespace shapely {

// 二维点（double，单位由使用方约定，本项目惯例 mm）。
// 也作为 Geom 顶点输出的载体。
struct Point2d {
    double x = 0.0;
    double y = 0.0;
};

// 三维点（double）。Stereo 点集的原生载体；鸭子类型工厂也输出到此结构。
struct Point3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;   // 高度方向（本项目惯例）
};

// 二维轴对齐包围盒（AABB）。
struct BoundingBox2d {
    double min_x = 0.0, min_y = 0.0;   // 左下角
    double max_x = 0.0, max_y = 0.0;   // 右上角
    bool valid = false;                // false = 空盒（Geom::bounds 对空对象返回）

    double width()  const { return max_x - min_x; }   // X 向边长
    double height() const { return max_y - min_y; }   // Y 向边长
};

// 三维轴对齐包围盒（AABB，三轴与世界坐标系平行）。
struct AABB3d {
    double min_x = 0.0, min_y = 0.0, min_z = 0.0;   // 包围盒最小角点
    double max_x = 0.0, max_y = 0.0, max_z = 0.0;   // 包围盒最大角点
    bool valid = false;                             // false = 空盒

    /// 体积 = 三边长乘积（单位立方）。无效盒返回 0 或负数（未校验）。
    double volume() const { return (max_x - min_x) * (max_y - min_y) * (max_z - min_z); }

    /// 是否与 o 空间重叠（三轴投影区间均重叠；边界接触算相交）。
    /// 任一方 invalid 返回 false。
    bool intersects(const AABB3d& o) const
    {
        return valid && o.valid &&
               min_x <= o.max_x && o.min_x <= max_x &&
               min_y <= o.max_y && o.min_y <= max_y &&
               min_z <= o.max_z && o.min_z <= max_z;
    }

    /// 是否完全包含 o（o 全部落在本盒内部，边界接触算包含）。
    bool contains(const AABB3d& o) const
    {
        return valid && o.valid &&
               min_x <= o.min_x && max_x >= o.max_x &&
               min_y <= o.min_y && max_y >= o.max_y &&
               min_z <= o.min_z && max_z >= o.max_z;
    }
};

// 三维有向包围盒（OBB，PCA 主轴对齐，比 AABB 更贴合真实物体）。
struct SHAPELYALG_EXPORT OBB3d {
    Point3d center;                    // 盒中心（世界坐标）
    double extents[3] = { 0, 0, 0 };   // 三根主轴方向的半长（半边长）
    double rotation[9] = {             // 3x3 行主序旋转矩阵（第 d 行 = 第 d 主轴方向）
        1, 0, 0,
        0, 1, 0,
        0, 0, 1,
    };
    bool valid = false;

    /// 体积 = 8 × 三半长乘积（恒 ≤ 同点集的 AABB 体积）。
    double volume() const { return 8.0 * extents[0] * extents[1] * extents[2]; }

    /// 是否与 o 空间重叠。SAT 分离轴判定：A 三轴 + B 三轴 + 9 叉积轴，共 15 轴，
    /// 任一轴投影分离即不相交。O(n) 常数较大，适合少量盒间的精确判定。
    bool intersects(const OBB3d& o) const;

    /// 是否完全包含 o：o 的 8 个角点全部变换到本盒主轴系内判定。
    bool contains(const OBB3d& o) const;
};

// ---- 结构契约静态断言 ----------------------------------------------------
// 这些结构是与宿主/PCL 交换数据的布局契约（按值传参、memcpy/PCL 点云互转）。
// 防误加虚函数（引入 vtable 破坏标准布局）、非平凡构造/析构或无意识扩字段
// 导致布局漂移——编译期即失败，无需运行时测试。sizeof 上限为实测基准值
// （MSVC x64 / C++17）：扩字段属故意修改契约，须显式上调对应断言。
static_assert(std::is_trivially_copyable<Point2d>::value &&
              std::is_standard_layout<Point2d>::value,
              "Point2d must stay trivially copyable / standard layout");
static_assert(std::is_trivially_copyable<Point3d>::value &&
              std::is_standard_layout<Point3d>::value,
              "Point3d must stay trivially copyable / standard layout");
static_assert(std::is_trivially_copyable<BoundingBox2d>::value &&
              std::is_standard_layout<BoundingBox2d>::value,
              "BoundingBox2d must stay trivially copyable / standard layout");
static_assert(std::is_trivially_copyable<AABB3d>::value &&
              std::is_standard_layout<AABB3d>::value,
              "AABB3d must stay trivially copyable / standard layout");
static_assert(std::is_trivially_copyable<OBB3d>::value &&
              std::is_standard_layout<OBB3d>::value,
              "OBB3d must stay trivially copyable / standard layout");

static_assert(sizeof(Point2d) == sizeof(double) * 2, "Point2d layout drift");
static_assert(sizeof(Point3d) == sizeof(double) * 3, "Point3d layout drift");
static_assert(sizeof(BoundingBox2d) <= 40, "BoundingBox2d size limit (40B, 4 double + bool)");
static_assert(sizeof(AABB3d) <= 56, "AABB3d size limit (56B, 6 double + bool)");
static_assert(sizeof(OBB3d) < 256, "OBB3d size limit (128B now; large growth signals contract change)");

} // namespace shapely

#endif // SHAPELY_TYPES_H
