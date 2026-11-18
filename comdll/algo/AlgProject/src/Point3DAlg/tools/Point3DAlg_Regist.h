#ifndef POINT3DALG_REGIST_H
#define POINT3DALG_REGIST_H

// Point3DAlg_Regist —— 点云配准算法层（纯函数，零 UI）。
//
// 设计要点：
//   * 纯函数：所有操作显式传入/返回状态，便于测试、回放、undo，也天然兼容
//     ComUI 算子"每次执行新建实例"的约束；
//   * 变换统一 4x4 行主序 float[16]，与 CloudProcessor::transApplyMatrix
//     和 ComUI 节点参数格式一致，零转换；
//   * 底层算子走 ShapelyAlg::CloudProcessor，不重复实现；
//   * 本文件不 include 任何 visualization 头（UI 在 RegistTool.cpp）。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <string>
#include <vector>

// ---------------- 配准策略（键盘 1..7 直接映射） ----------------
enum Point3DAlg_RegistStrategy {
    POINT3DALG_REGIST_ICP_P2P    = 1,   // ICP 点到点：通用，最稳
    POINT3DALG_REGIST_ICP_P2PL   = 2,   // ICP 点到面：平面多时快且准，需法线
    POINT3DALG_REGIST_GICP       = 3,   // GICP：平面/曲面混合，精度最高
    POINT3DALG_REGIST_FPFH_RANSAC= 4,   // FPFH + RANSAC：无需初值，慢
    POINT3DALG_REGIST_NDT        = 5,   // NDT：大初始偏差稳健
    POINT3DALG_REGIST_PCA        = 6,   // PCA 主轴粗配准：极快，给 ICP 找初值
    POINT3DALG_REGIST_MULTISCALE = 7,   // 多尺度 ICP（Open3D multiscale_icp）：voxel 金字塔
                                        //   粗→细逐级配准，大初值偏差下比单级 ICP 稳
};

// 策略名（日志/标题栏显示）
POINT3DALG_EXPORT const char* Point3DAlg_RegistStrategyName(int strategy);

// ---------------- 状态（不可变：每次操作返回新状态） ----------------
struct POINT3DALG_EXPORT Point3DAlg_RegistSession {
    float  transform[16];       // 当前累积变换（源 → 目标），行主序
    int    strategy;            // 当前策略
    int    iteration;           // 已执行的配准次数
    double best_fitness;        // 历史最优 fitness（0~1，越大越好；1.0 = 完美对应）
    double best_rmse;           // 对应最优 fitness 的局内点 RMSE（mm）

    Point3DAlg_RegistSession();  // 单位阵 / 策略1 / fitness=-1
};

// 单次操作结果（全部纯函数的返回类型，跨 DLL 使用）
struct POINT3DALG_EXPORT Point3DAlg_RegistStep {
    Point3DAlg_RegistSession next;   // 新状态
    double fitness;                  // 本次 fitness（0~1，越大越好；<0 表示无效）
    double inlier_rmse;              // 局内点均方根误差（单位同点云，通常 mm）
    bool   improved;                 // 是否刷新了历史最优
    std::string message;             // 人话描述
};

// 配准参数
struct POINT3DALG_EXPORT Point3DAlg_RegistParams {
    double max_correspondence_dist;  // 对应点最大距离（同点云单位），<=0 时按点云间距自适应
    int    max_iteration;            // 单次配准最大迭代数（默认 60）
    double transform_epsilon;        // 收敛判据：变换增量阈值（默认 1e-8）
    double fitness_epsilon;          // 收敛判据：fitness 增量阈值（默认 1e-6）

    Point3DAlg_RegistParams();
};

// =====================================================================
// 操作（全部纯函数：不改入参，返回新状态）
// =====================================================================

/// 手动平移（在**目标坐标系**下微调，增量左乘）。
/// [in] dx/dy/dz  平移量（与点云同单位，通常 mm）
POINT3DALG_EXPORT Point3DAlg_RegistStep Point3DAlg_RegistTranslate(
    const Point3DAlg_RegistSession& s, double dx, double dy, double dz);

/// 手动旋转（在目标坐标系下，绕世界轴）。
/// [in] axis     0=X 1=Y 2=Z
/// [in] angle_deg 旋转角（度，右手系）
POINT3DALG_EXPORT Point3DAlg_RegistStep Point3DAlg_RegistRotate(
    const Point3DAlg_RegistSession& s, int axis, double angle_deg);

/// 手动旋转（绕**指定中心点**，手工配准里最常用：绕工件中心转）。
/// [in] pivot 旋转中心 [x,y,z]
POINT3DALG_EXPORT Point3DAlg_RegistStep Point3DAlg_RegistRotateAround(
    const Point3DAlg_RegistSession& s, const double pivot[3],
    int axis, double angle_deg);

/// 重置为单位阵。
POINT3DALG_EXPORT Point3DAlg_RegistStep Point3DAlg_RegistReset(
    const Point3DAlg_RegistSession& s);

/// 切换策略（不改变换）。
POINT3DALG_EXPORT Point3DAlg_RegistStep Point3DAlg_RegistSetStrategy(
    const Point3DAlg_RegistSession& s, int strategy);

/// 执行一次配准（以 s.transform 为初值）。
/// [in] source  源点云（待变换）
/// [in] target  目标点云（参考）
/// 返回 next.transform 为配准后的新变换。
POINT3DALG_EXPORT Point3DAlg_RegistStep Point3DAlg_RegistRun(
    const Point3DAlg_RegistSession& s,
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& source,
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& target,
    const Point3DAlg_RegistParams& params);

// =====================================================================
// 工具函数
// =====================================================================

/// 应用变换到点云（返回新点云，原云不变）。
POINT3DALG_EXPORT pcl::PointCloud<pcl::PointXYZ>::Ptr Point3DAlg_RegistApplyTransform(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud, const float transform[16]);

/// 单位阵判定（误差 < eps）。
POINT3DALG_EXPORT bool Point3DAlg_RegistIsIdentity(const float transform[16], double eps = 1e-6);

/// 点云平均最近邻间距（用于自适应阈值；稠密点云比采样估算更稳）。
POINT3DALG_EXPORT double Point3DAlg_RegistEstimateSpacing(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud);

// ---------------- 变换文件读写 ----------------

/// 保存为纯文本 4x4（每行 4 个数，可直接用文本编辑器改）。
POINT3DALG_EXPORT bool Point3DAlg_RegistSaveTransform(const char* path, const float transform[16]);

/// 保存为 ComUI 节点参数格式（一行 16 个逗号分隔值）。
/// 用途：从工具导出后直接粘进 node.json 的 init_transform 参数。
POINT3DALG_EXPORT bool Point3DAlg_RegistSaveTransformFlat(const char* path, const float transform[16]);

/// 从文本载入（支持 4x4 矩阵格式、单行 16 值逗号/空格分隔、含 "init_transform" 的整行）。
POINT3DALG_EXPORT bool Point3DAlg_RegistLoadTransform(const char* path, float transform[16]);

/// 从内存字符串解析 16 值（ComUI init_transform 参数用）：
/// 所有非数字字符视为分隔符，兼容 4x4 矩阵、单行 16 值逗号/空格分隔；解析结果非 16 值时返回 false。
POINT3DALG_EXPORT bool Point3DAlg_RegistParseTransformText(const char* text, float transform[16]);

/// 格式化为一行 16 值逗号分隔串（供 ComUI 参数使用）。
POINT3DALG_EXPORT std::string Point3DAlg_RegistFormatFlat(const float transform[16]);

#endif // POINT3DALG_REGIST_H
