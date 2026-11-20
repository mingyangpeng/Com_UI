#ifndef POINT3DALG_SICKAPI_H
#define POINT3DALG_SICKAPI_H

// Point3DAlg_SickApi.h —— SICK 读取深层 C 接口（1.1.0 新增）。
// 把 Point3DAlg_SickReader C++ 类已有能力（原始图/编码器/行中心 Y/强度图/
// 逐点强度/拼接质量自检/FOV 结构化框）暴露给 C 宿主；本层只做 C 边界
// （句柄生命周期 + 缓冲拷贝 + try/catch），不含算法。
//
// 说明：本头文件 include Point3DAlg_SickReader.h（含 C++ 类），**仅限 C++
// 宿主 include**；纯 C 宿主请按 Point3DAlg_SickConfig / Point3DAlg_StitchQualityResult
// 的 POD 布局自行映射（两结构均为平铺数值字段，无指针成员）。
//
// 错误码口径（本文件全部接口，与既有 ReadSickCloud 家族对齐，**不使用**
// 点云计算族的 -3=参数越界/-4=内存 全局约定，避免同 DLL 内歧义）：
//   0  成功
//   -1 参数/空指针错误（handle 或必填输出指针为 NULL；StitchQuality 的
//      threshold<=0 例外，按 -3 报）
//   -2 数据/结果缺失（未加载数据、无 .dat、无强度、无衔接带、结果为空）
//   -3 帧号越界（LoadRawInto）；StitchQuality 的 threshold<=0（参数越界）
//   -4 配置/数据错误（LoadSickConfig 失败、LoadRaw/GetDepth 抛异常、内存
//      分配失败——与 ReadSickCloud 的 -4 口径一致）
//   -99 未预期异常兜底（本层全捕获，不跨 DLL）
//
// 内存约定：全部 out_* 缓冲由本库 malloc，统一用 Point3DAlg_FreeBuffer
// 释放（Point3DAlg_api.h）；均为**独立副本**（非类内部缓冲），释放后句柄
// 仍可用。输出指针在出错时统一置 NULL/0。

#include "AlgExport.h"
#include "Point3DAlg_SickReader.h"   // Point3DAlg_SickConfig / StitchQualityResult

#ifdef __cplusplus
extern "C" {
#endif

// ---------------- 句柄生命周期 ----------------

// 不透明句柄：内部持有 Point3DAlg_SickConfig + Point3DAlg_SickReader。
typedef struct Point3DAlg_SickReaderImpl* Point3DAlg_SickReaderH;

// 创建读取器句柄。
// cfg：SICK 参数（Point3DAlg_LoadSickConfig 填充或手工填写）；NULL → 返回 NULL。
// cfg 数值合法性（y_scale/z_scale>0）延后到 LoadRawInto 校验。
// 失败/异常返回 NULL（无错误码出口）。用 FreeSickReader 释放。
POINT3DALG_EXPORT Point3DAlg_SickReaderH Point3DAlg_CreateSickReader(const Point3DAlg_SickConfig* cfg);

// 加载扫描数据到句柄（可重复调用覆盖上一次加载）。
// folder：扫描数据文件夹。支持 ';' 分隔的多相机路径列表（如
//   "Z:/ScanImage/3D;Z:/ScanImage/3D-2"，顺序 = 相机序）；StitchQuality 需要
//   >=2 台相机，单相机传 1 个路径。
// frame：<0 读全部帧；>=0 只读每个相机的第 frame 帧（按文件名顺序，从 0 计）。
// recipe_path / common_path：**非 NULL 且非空串**时先用 LoadSickConfig 重读
//   配置并替换句柄内 cfg（失败 → -4）；**NULL 或空串 = 沿用句柄当前 cfg**
//   （注意：与 ReadSickCloud 的"NULL=默认路径"约定不同——句柄有状态，
//   需要默认路径时先 LoadSickConfig(NULL,NULL,...) 再 CreateSickReader）。
// 返回 0 成功；-1 参数错（handle/folder NULL、folder 空串、路径列表为空）；
//   -2 任一文件夹无 .dat；-3 frame 越界（按各文件夹 .dat 数逐一校验，任一
//   越界即 -3）；-4 配置读取失败 / LoadRaw 抛异常 / 内存不足。
POINT3DALG_EXPORT int Point3DAlg_LoadRawInto(Point3DAlg_SickReaderH handle,
                                             const char* folder, int frame,
                                             const char* recipe_path, const char* common_path);

// 释放句柄（NULL 安全）。
// use-after-free 口径：释放后句柄立即失效，后续任何接口传同一句柄属**未定义
// 行为**（本层不做悬挂句柄检测/引用计数——句柄按值生命周期管理，宿主须
// 自行保证先 Free 后停用；重复 Free 同一非空句柄同样未定义）。
POINT3DALG_EXPORT void Point3DAlg_FreeSickReader(Point3DAlg_SickReaderH handle);

// ---------------- 信息查询 ----------------

// 读取器尺寸与状态。LoadRaw 之后即可取 width/rows/device_count/has_intensity；
// stitch_rows/stitch_cols 需 GetDepth 之后（未生成时输出 0，本接口不触发拼接）。
// 各 out_* 允许单独为 NULL（只取关心的字段）；handle NULL → -1；未加载数据
// （从未成功 LoadRawInto）→ -2。
// 输出：out_width=每行轮廓像素数（2560）；out_rows=原始总行数（相机 0）；
//   out_stitch_rows/out_stitch_cols=拼接深度图行/列数；out_device_count=相机数；
//   out_has_intensity=1 当前 .dat 含强度通道 / 0 无。
POINT3DALG_EXPORT int Point3DAlg_GetSickInfo(Point3DAlg_SickReaderH handle,
                                             int* out_width, long long* out_rows,
                                             long long* out_stitch_rows, long long* out_stitch_cols,
                                             int* out_device_count, int* out_has_intensity);

// 每个扫描行（pass）的中心 Y（运动表，mm；对应 C++ RowY()）。LoadRaw 之后有效。
// out_row_y：double 数组（库 malloc，FreeBuffer 释放）；out_count=元素个数。
// 错误码：-1 handle/out_row_y/out_count NULL；-2 未加载或运动表为空（0 行）；
//   -4 内存不足。
POINT3DALG_EXPORT int Point3DAlg_GetSickRowY(Point3DAlg_SickReaderH handle,
                                             double** out_row_y, int* out_count);

// 原始高度图（相机 0）与逐行编码器值。LoadRaw 之后有效（懒构建：首次调用
// 从 FOV 拼合，可能有一次性内存/时间开销）。
// 布局：out_raw = out_h × out_w 的 uint16 行主序（行数=Rows()，列数=Width()，
//   0 = 无效点）；out_encoder = out_h 个 int32（每行扫描轴编码器值）。
// out_raw/out_encoder 允许各自为 NULL（只取其一）；out_w/out_h 必填。
// 输出为库 malloc 副本（非类内部缓冲），FreeBuffer 释放。
// 错误码：-1 handle/out_w/out_h NULL；-2 未加载或原始数据为空；-4 内存不足。
POINT3DALG_EXPORT int Point3DAlg_GetSickRaw(Point3DAlg_SickReaderH handle,
                                            unsigned short** out_raw, int* out_w, long long* out_h,
                                            int** out_encoder);

// 强度图与逐点强度（新格式 .dat 才有强度通道）。
// 前置：内部触发 GetDepth（强度图随拼接图生成）与 GetCloud（确定点数）。
// 布局：out_img = out_h × out_w 的 uint8 行主序（行=StitchRows，列=StitchCols，
//   与拼接深度图同几何，0=无效）；out_cloud_int = out_point_count 个 uint8，
//   与 GetCloud 点一一对应（按 GetCloud 生成顺序=拼接图行主序、跳过 0 深度格，
//   从强度图同格取值——类成员 GetCloudIntensity 当前未填充，本层按上述映射
//   等效产出，类实现填充后语义不变）。
// out_cloud_int/out_point_count 允许为 NULL（只取强度图）；out_img/out_w/out_h 必填。
// 错误码：-1 handle/out_img/out_w/out_h NULL；-2 未加载 / 无强度通道 / 强度图为空；
//   -4 拼接/取点云抛异常、内存不足。
POINT3DALG_EXPORT int Point3DAlg_GetSickIntensity(Point3DAlg_SickReaderH handle,
                                                  unsigned char** out_img, int* out_w, long long* out_h,
                                                  unsigned char** out_cloud_int, int* out_point_count);

// 拼接质量自检（多相机衔接带格级 z 差统计；diff_threshold 为超差判定阈值 mm）。
// 需 >=2 台相机且已生成拼接深度图（内部触发 GetDepth）。
// out 填充已导出的 Point3DAlg_StitchQualityResult（pairs/dh_mean/dh_sigma/
//   dh_max_abs/outlier/outlier_ratio，字段语义见 Point3DAlg_SickReader.h）。
// 错误码：-1 handle/out NULL；-2 单相机（无衔接带）/无有效配对格；
//   -3 diff_threshold<=0（参数越界）；-4 GetDepth 抛异常。
POINT3DALG_EXPORT int Point3DAlg_StitchQuality(Point3DAlg_SickReaderH handle,
                                               double diff_threshold,
                                               Point3DAlg_StitchQualityResult* out);

// FOV 框结构化数据（既有 DrawFovBoxes 只画图拿不到矩形坐标，本接口补齐）。
// 前置：内部触发 GetDepth（FOV 框随拼接记录，为最终拼接图坐标）。
// 布局：out_rects = out_count×4 个 int，每框连续 {x,y,w,h}（像素：列 x、行 y、
//   宽 w、高 h）；out_ids = out_count 个 int，id 透传类输出（单相机 = FOV 序号
//   j；多相机 = 相机 d 的 FOV j 编号 d*1000+j）。
// out_rects/out_ids 为两块独立库 malloc 缓冲，各自用 FreeBuffer 释放。
// 错误码：-1 handle/out_rects/out_ids/out_count 任一 NULL；-2 未加载 / 无 FOV 框；
//   -4 GetDepth 抛异常、内存不足。
POINT3DALG_EXPORT int Point3DAlg_GetFovBoxes(Point3DAlg_SickReaderH handle,
                                             int** out_rects, int** out_ids, int* out_count);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // POINT3DALG_SICKAPI_H
