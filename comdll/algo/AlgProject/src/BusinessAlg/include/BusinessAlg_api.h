#ifndef BUSINESSALG_API_H
#define BUSINESSALG_API_H

#include "AlgExport.h"

#ifdef __cplusplus
extern "C" {
#endif

// 业务层公共 C 接口。
//
// 定位：本模块只做"业务流程编排"——原子能力（几何运算 / 点云算子 / 图像算子）
//       全部来自基础模块：
//         ShapelyAlg  —— 二/三维几何、图像、点云基础算子
//         Point3DAlg  —— SICK 扫描读取与点云生成
//         Image2DAlg  —— 图像读取
//       业务层负责：参数校验、流程串联、业务语义判定、结果组装。
//
// 新增算子步骤：
//   1. 在 BusinessAlg_comui.cpp 增加 Point3DAlg_ComuiXxx / BusinessAlg_ComuiXxx 入口；
//   2. 在此加声明（若需 C 级复用）；
//   3. 添加对应 *.node.json 声明。
//
// 现有算子见 BusinessAlg_comui.cpp 中的 <=> 分隔区。

// ---------------- 示例算子：区域缺陷率统计 ----------------
// 流程：点云 → 高度过滤(Stereo) → 2.5D 投影(Geom) → 区域面积/占比
// 说明：这是骨架示例，用于验证跨模块组合与链接；实际业务算子请按上述步骤追加。
//
// 参数：xyz/count  点云交错数组 [x0,y0,z0,...] 与点数（z 为高度）
//       min_z / max_z 高度区间；roi_x/y/w/h 感兴趣区域矩形
// 输出：defect_ratio（ROI 内有效点投影面积 / ROI 面积）、area（有效投影面积，mm²）
// 返回：0 成功；-1 空指针/参数非法（count<=0、roi_w/h<=0）；2 ROI 内无有效投影
BUSINESSALG_EXPORT int BusinessAlg_ComputeRegionRatio(const float* xyz, int count,
                                                      double min_z, double max_z,
                                                      double roi_x, double roi_y,
                                                      double roi_w, double roi_h,
                                                      double* out_defect_ratio,
                                                      double* out_area);

#ifdef __cplusplus
}
#endif

#endif // BUSINESSALG_API_H
