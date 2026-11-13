// BusinessAlg_Region.cpp —— 业务层示例算子实现。
// 原则：本文件不含任何几何/几何算法细节，全部调用基础模块。

#include "include/BusinessAlg_api.h"
#include "ShapelyAlg.h"   // 基础模块总入口（几何/点云/图像原子能力）

#include <cmath>
#include <vector>

int BusinessAlg_ComputeRegionRatio(const float* xyz, int count,
                                   double min_z, double max_z,
                                   double roi_x, double roi_y,
                                   double roi_w, double roi_h,
                                   double* out_defect_ratio,
                                   double* out_area)
{
    if (!xyz || count <= 0 || !out_defect_ratio || !out_area)
        return -1;
    if (roi_w <= 0 || roi_h <= 0)
        return -1;
    *out_defect_ratio = 0.0;
    *out_area = 0.0;

    // 1) SoA float[3N] → Stereo（基础模块的原生 3D 容器）
    std::vector<shapely::Point3d> pts;
    pts.reserve((size_t)count);
    for (int i = 0; i < count; ++i)
        pts.push_back(shapely::Point3d{ (double)xyz[i * 3],
                                        (double)xyz[i * 3 + 1],
                                        (double)xyz[i * 3 + 2] });
    const shapely::Stereo cloud = shapely::Stereo::createFromPoints(pts);

    // 2) 高度区间过滤（基础模块）
    const shapely::Stereo band = cloud.filterByZRange(min_z, max_z);

    // 3) 2.5D 投影 → 几何（基础模块）
    const shapely::Geom projected = band.projectTo2D();
    if (projected.isEmpty())
        return 2;

    // 4) 与 ROI 求交，算有效投影面积（基础模块：ROI 四角点 → 面）
    const std::vector<shapely::Point2d> roiCorners = {
        { roi_x,         roi_y },
        { roi_x + roi_w, roi_y },
        { roi_x + roi_w, roi_y + roi_h },
        { roi_x,         roi_y + roi_h },
    };
    const shapely::Geom roi = shapely::Geom::createPolygon(roiCorners);
    const double area = projected.intersection(roi).area();

    // 5) 业务语义：占比
    *out_area = area;
    *out_defect_ratio = area / (roi_w * roi_h);
    return 0;
}
