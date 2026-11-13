#ifndef POINT3DALG_CLOUDDIFF_H
#define POINT3DALG_CLOUDDIFF_H

// Point3DAlg_CloudDiff —— 两片点云的高度差对比（金标准复检，纯函数，零 UI）。
//
// 场景：标准片（基准）与被测片在同一工位扫描后对比——被测相对基准哪里高了/
// 低了多少、超差点比例多少（NG 判定）。XY 假设已对齐（同工位同夹具）。
//
// 算法：
//   1. 两片各自 XY 栅格化（cell = cell_size），每格取 z 中位数（抗单点噪声）；
//   2. 遍历两片共有的格，diff = zB − zA（正 = 被测高于基准）；
//   3. 统计 mean/σ/极值；diff_threshold > 0 时统计超差格数（|diff| > 阈值）。
//
// 注：格内点数 < min_points_per_cell 的格不参与（边缘/稀疏区）；
// 共同格数为 0（完全不重叠）返回 false。

#include "AlgExport.h"

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>

#include <vector>

// ---------------- 参数 ----------------
struct POINT3DALG_EXPORT Point3DAlg_CloudDiffParams {
    double cell_size;           // 栅格边长 mm（默认 0.1；≈3 倍点距可平滑单点噪声）
    int    min_points_per_cell; // 格最少点数（默认 3；不足不参与）
    double diff_threshold;      // 超差判定阈值 mm（默认 0.02；<=0 不判超差只统计）
    double align_search;        // XY 粗对齐搜索半径 mm（默认 0 = 不对齐）。
                                // 两片云装夹偏差会虚高边缘格 diff；>0 时先在
                                // ±search 内做格域搜索(RMS 得分+重叠 70% 门槛) +
                                // ±2 格细化（步 cell/4）。
                                // 精度受"格中位不变平台"限制 ≈ ±cell/2：平移小于
                                // cell 时格内 z 中位不变，得分简并——需要更准就把
                                // cell_size 调小。含 z 常差的对比需形貌有台阶/凸台
                                // 等特征锚定（纯梯度/斜面上 dz 会被平移吸收）

    Point3DAlg_CloudDiffParams();
};

// ---------------- 结果 ----------------
struct POINT3DALG_EXPORT Point3DAlg_CloudDiffResult {
    long long cells;        // 参与 diff 的共同格数
    double mean;            // 差值均值（zB − zA，正 = 被测高）
    double sigma;           // 差值标准差
    double max_abs;         // 最大绝对差
    double max_pos;         // 最大正差
    double max_neg;         // 最大负差（≤0）
    long long over_count;   // 超差格数（|diff| > threshold）
    double over_ratio;      // 超差占比
    bool   aligned;         // 是否执行了 XY 粗对齐
    double aligned_dx;      // 已施加于被测云 B 的 X 平移 mm（对齐后 B'+T ≈ A）
    double aligned_dy;      // 已施加于被测云 B 的 Y 平移 mm

    Point3DAlg_CloudDiffResult();
};

/// 高度差对比。
/// [in]  cloudA         基准点云
/// [in]  cloudB         被测点云
/// [out] out            统计
/// [out] out_diff_cloud 可为 nullptr；否则追加点云（每格一点：x=格中心 x、
///                      y=格中心 y、z=diff），供宿主伪彩查看 NG 分布
/// 返回 false = 云空 / 共同格数为 0。
POINT3DALG_EXPORT bool Point3DAlg_CloudDiff(
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloudA,
    const pcl::PointCloud<pcl::PointXYZ>::ConstPtr& cloudB,
    const Point3DAlg_CloudDiffParams& params,
    Point3DAlg_CloudDiffResult& out,
    std::vector<float>* out_diff_cloud = nullptr);

#endif // POINT3DALG_CLOUDDIFF_H
