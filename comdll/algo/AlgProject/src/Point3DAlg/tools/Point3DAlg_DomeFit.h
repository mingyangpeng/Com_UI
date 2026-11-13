#ifndef POINT3DALG_DOMEFIT_H
#define POINT3DALG_DOMEFIT_H

// Point3DAlg_DomeFit —— 拱形球顶 bump 拟合与高度量测（纯函数，零 UI）。
//
// 场景：晶圆/MOI 工件上的微凸台（bump）顶部为拱形球冠，量测需求是
//   bump 高度 = 球顶 apex（球心沿基面法向的高度 + 球半径）− 基面高度。
//
// 设计要点（与 Point3DAlg_Regist 一致）：
//   * 纯函数：显式传入/返回，便于测试与 ComUI 复用；
//   * 全确定性：基面用迭代修剪最小二乘平面、球用代数法闭式解，
//     不用 RANSAC——同一输入永远同一输出，便于回归比对；
//   * 聚类复用 shapely::CloudProcessor::segmentEuclideanCluster（欧式聚类）；
//   * apex 比取最高点稳：单点噪声被球拟合平均掉（网格点云顶点可能
//     正好没有采样在几何最高处）。
//
// 算法链路（已在 moi_wafer.ply 上用等价 Python 脚本验证）：
//   1. 稳健基面：对全点云做 z = a·x + b·y + c 的迭代修剪最小二乘
//      （剔除高于中位数 + 3·MAD 的点，迭代 5 次），扣掉倾斜后取
//      残差中位数为零平面；
//   2. 高度分离：相对基面高度 > height_threshold 的点为 bump 候选；
//   3. 欧式聚类：候选点按 cluster_tolerance 连通成簇，点数 < min_points 的丢弃；
//   4. 每簇球拟合：代数法闭式解（Kåsa）+ 一次稳健修剪（剔除残差 >
//      3·MAD 的点重拟合），球心 c 与半径 R；
//   5. 高度 = (c + R·n)·n − 基面，n 为基面法向（|n|=1）；
//      顶点位置 = c − R·n（球冠顶点，朝基面上方一侧）。
//
// 注：基面为平面模型。晶圆整体弯曲（弓形）在 30mm 尺度上 < 2µm 高度
// 影响（实测高度与位置相关性 < 0.12），平面已够；需要时再扩展多项式基面。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <string>
#include <vector>

// ---------------- 拟合参数 ----------------
struct POINT3DALG_EXPORT Point3DAlg_DomeFitParams {
    double height_threshold;    // bump 判定高度阈值（相对基面，同点云单位；默认 0.02mm）
    double cluster_tolerance;   // 欧式聚类连通距离（默认 0.12mm ≈ 3~4 倍点间距）
    int    min_points;          // 簇最少点数（默认 8，滤掉孤立噪点）
    double base_mad_factor;     // 基面修剪因子：剔除 > 中位数 + factor·MAD（默认 3.0）
    int    base_iterations;     // 基面修剪迭代次数（默认 5）
    double sphere_mad_factor;   // 球拟合修剪因子（残差 > 中位数 + factor·MAD 剔除重拟合；默认 3.0）
    double max_radius;          // 球半径合理上限（默认 1.0mm；超出判为拟合失败）
    double min_radius;          // 球半径合理下限（默认 0.01mm；过小判为拟合失败）
    double max_fit_rms;         // 球拟合残差 RMS 上限（默认 0.005mm；正常 bump rms ≤ 6.7µm、
                                //   p99≈3.7µm，劣质拟合/非球冠簇 ≥ 9µm，超出判 invalid）

    Point3DAlg_DomeFitParams();
};

// ---------------- 单个 bump 拟合结果 ----------------
struct POINT3DALG_EXPORT Point3DAlg_DomeBump {
    double height;          // bump 高度 = apex − 基面（量测值）
    double apex[3];         // 球冠顶点坐标（c + R·n，n = 基面法向上方向）
    double center[3];       // 拟合球心
    double radius;          // 拟合球半径
    double centroid[3];     // 簇质心（bump 大致位置）
    double rms;             // 球拟合残差 RMS（质量指标）
    int    point_count;     // 簇内点数
    bool   valid;           // false = 该簇球拟合失败（半径越界/点数不足/退化）

    Point3DAlg_DomeBump();
};

// ---------------- 基面拟合结果 ----------------
struct POINT3DALG_EXPORT Point3DAlg_DomeBase {
    double normal[3];       // 基面法向（单位向量，朝 z 正方向一侧）
    double d;               // 平面方程 n·p + d = 0 的常数项
    double sigma;           // 基面内点残差 RMS（基面噪声水平）
    int    inlier_count;    // 参与最终拟合的点数

    Point3DAlg_DomeBase();
    // 平面上方高度：p 相对基面的有符号高度（沿法向）
    double HeightOf(double px, double py, double pz) const;
};

// =====================================================================
// 主入口（纯函数）
// =====================================================================

/// 拟合基面（不分离 bump，单独暴露便于诊断）。
/// 返回 false = 点数 < 10 或平面退化（法向无法确定）。
POINT3DALG_EXPORT bool Point3DAlg_DomeFitBase(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
    const Point3DAlg_DomeFitParams& params,
    Point3DAlg_DomeBase& out_base);

/// 拱形球顶 bump 拟合主入口：基面 → 高度分离 → 聚类 → 每簇球拟合。
/// [in]  cloud   输入点云（建议已去掉 NaN；网格点云或散点均可）
/// [out] bumps   每个 bump 的拟合结果（含失败簇，valid=false）
/// [out] out_base 基面结果（可为 nullptr = 不需要）
/// 返回：false = 输入非法 / 基面失败 / 无任何 bump 簇。
/// 注：结果按簇点数降序排列（大 bump 在前）。
POINT3DALG_EXPORT bool Point3DAlg_DomeFit(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloud,
    const Point3DAlg_DomeFitParams& params,
    std::vector<Point3DAlg_DomeBump>& bumps,
    Point3DAlg_DomeBase* out_base = nullptr);

/// 高度统计（CPK 用）：均值/标准差/分位数 + 共面性量测。
/// 共面性两条口径（用哪个看工艺定义）：
///   coplanarity —— 极差法 = max − min（对单个离群 bump 敏感， JEDEC 常用）
///   fit_sigma   —— 剥离整体倾斜/翘曲后的散差：对 (x, y, height) 拟合最佳
///                  平面 h = p·x + q·y + r，取内点残差 RMS（MAD 修剪离群 bump）
struct POINT3DALG_EXPORT Point3DAlg_DomeStats {
    int    count;           // 有效 bump 数
    double mean;            // 高度均值
    double stddev;          // 高度标准差
    double min, max;        // 高度范围
    double p5, p50, p95;    // 高度分位数
    double coplanarity;     // 共面性（极差法）= max − min
    double fit_sigma;       // 最佳拟合面残差 RMS（剥离倾斜/翘曲的共面性散差）
    double fit_p, fit_q;    // 最佳拟合面倾斜系数 h = fit_p·x + fit_q·y + r（诊断晶圆翘曲向）

    Point3DAlg_DomeStats();
};

POINT3DALG_EXPORT Point3DAlg_DomeStats Point3DAlg_DomeStatsOf(
    const std::vector<Point3DAlg_DomeBump>& bumps);

// ---------------- bump 对比（金标准 NG 判定，纯函数） ----------------

struct POINT3DALG_EXPORT Point3DAlg_BumpCompareParams {
    double match_radius;    // XY 匹配半径 mm（默认 0.3；≈ bump 间距一半）
    double ng_threshold;    // 高度偏差 NG 阈值 mm（默认 0.02）

    Point3DAlg_BumpCompareParams();
};

struct POINT3DALG_EXPORT Point3DAlg_BumpCompareResult {
    int    matched;         // 一对一配对数
    int    unmatched_ref;   // 基准中无对应被测的 bump 数（漏测）
    int    unmatched_test;  // 被测中无对应基准的 bump 数（多出/异物）
    int    ng_count;        // |高度差| > ng_threshold 的配对数
    double ng_ratio;        // ng_count / matched
    double dh_mean;         // 高度差均值（h_test − h_ref，正 = 被测高）
    double dh_rms;          // 高度差 RMS
    double dh_max_abs;      // 最大绝对高度差
    std::vector<double> diff_xy;   // 配对 bump 的 apex XY（x,y 交替，按差值绝对值降序）
    std::vector<double> diff_h;    // 对应高度差 mm
    std::vector<int>    diff_ng;   // 对应 NG 标记（1/0）

    Point3DAlg_BumpCompareResult();
};

/// 基准/被测 bump 集一对一匹配 + 高度差 NG 判定。
/// 只取 valid bump；XY 最近贪心一对一（同 ref 冲突取距离更小者）。
/// 返回 false = 两组全空。
POINT3DALG_EXPORT bool Point3DAlg_BumpCompare(
    const std::vector<Point3DAlg_DomeBump>& ref,
    const std::vector<Point3DAlg_DomeBump>& test,
    const Point3DAlg_BumpCompareParams& params,
    Point3DAlg_BumpCompareResult& out);

#endif // POINT3DALG_DOMEFIT_H
