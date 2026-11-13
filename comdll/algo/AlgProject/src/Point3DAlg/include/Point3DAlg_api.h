#ifndef POINT3DALG_API_H
#define POINT3DALG_API_H

#include "AlgExport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SICK Ranger3 扫描数据读取（一步式 C 接口）。
 * 类接口（推荐）见 include/Point3DAlg_SickReader.h，本接口内部同样走 SickReader。
 *
 * folder：扫描数据文件夹（*.dat + 同名 .xml + Scan3DPart*.json）。
 * frame：帧选择。<0 读取全部 .dat 文件；>=0 只读第 frame 帧（按文件名顺序，从 0 计）。
 * recipe_path / common_path：换算参数 json 路径。传 NULL 或空串 = 默认路径
 *   （recipe：Z:\Calibrate\3D\MtxJuxin60003D.dll.user.json，
 *    common：Z:\Juxin60003D_CommonParams.json）。提取不到必需值报错。
 * out_xyz：输出 SoA 点云缓冲（malloc，用 Point3DAlg_FreeBuffer 释放）。
 * out_count：输出点数。
 * 返回 0 成功；<0 错误码（-1 参数错；-2 无 .dat；-3 帧号越界；-4 配置/数据错误）。
 */
POINT3DALG_EXPORT int Point3DAlg_ReadSickCloud(const char* folder, int frame,
                                               const char* recipe_path, const char* common_path,
                                               float** out_xyz, int* out_count);

/* 释放 Point3DAlg_ReadSickCloud 的输出缓冲 */
POINT3DALG_EXPORT void Point3DAlg_FreeBuffer(void* p);

#ifdef __cplusplus
}
#endif

#endif /* POINT3DALG_API_H */
