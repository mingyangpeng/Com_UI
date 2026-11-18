#ifndef COMUI_NATIVE_ABI_H
#define COMUI_NATIVE_ABI_H
// ComUI 原生算子统一 C ABI（v1）——与 docs/THIRD_PARTY_OPS.md §5.5 一致。
//
// 【调用模型】每个算法节点对应一个由算法 DLL 导出的 C 函数：
//     int32_t Xxx(const AlgoCInput* in, AlgoCOutput* out);
//   宿主按 node.json 的 params/连线声明组装 AlgoCInput 调用该函数；
//   输出缓冲一律节点侧 malloc 分配，宿主用同 DLL 导出的
//     void algo_free(void* p)
//   释放（free 配对）。本头文件为纯结构声明，不含函数符号——结构体
//   无需 extern "C" 保护；入口函数的声明与导出宏在各 DLL 的适配 .cpp
//   内以 extern "C" + 自己的 <Target>_EXPORT 标注。
//
// 【返回码】0 = 成功；非 0 = 失败（宿主只按 rc != 0 判定，具体码随节点实现）：
//   1  输入缺失（输入未连线 / 路径未设置 / 输入数量不足）
//   2  算子执行失败（核心算子报错 / 输入数据无效）
//   3  参数不支持或内存不足
//   4  量测类业务失败（拟合失败 / 无有效结果 / 点数不足）
//   失败时 out->error 必非空；注意测量类节点成功（rc = 0）时也可能经
//   error 通道回传结果文本（manual_regist 先例，见 docs/HOST_VERIFY.md"待真机确认"）。
//
// 【编码】text 参数与 error 文本均为 UTF-8；错误消息为静态存储（宿主只读不释放）。
#include <stdint.h>

// 单个节点参数：按 kind 解释对应字段（三字段共用一块存储，非严格 union）。
// 宿主 marshal 不带参数名——适配层按 node.json params 声明顺序 + kind 取值，
// 同 kind 的多个参数以出现次序区分（FindParam 先例）。
typedef struct AlgoCParam {
    int32_t kind;          // 0=double 1=int32 2=text(UTF-8) 3=bool(i!=0)
    double  d;             // kind = 0 时的值
    int32_t i;             // kind = 1 时的值
    const char* s;         // kind = 2 时的 UTF-8 文本（宿主侧内存，节点只读）
} AlgoCParam;

// 输入图像视图：宿主恒为 BGRA 紧排（行步长 = width*4，无对齐填充）。
// 内存归宿主，节点只读不释放。
typedef struct AlgoCImage { int32_t width, height; const uint8_t* bgra; } AlgoCImage;

// 输入点云视图：xyz 为交错数组 [x0,y0,z0,x1,y1,z1,...]（count 个点，单位随算子约定）；
// rgb 可为 NULL（无颜色）。内存归宿主，节点只读不释放。
typedef struct AlgoCCloud { int32_t count; const float* xyz; const uint8_t* rgb; } AlgoCCloud;

// 节点输入包：三类输入的个数与数组（未连线的输入个数为 0、数组为 NULL）。
typedef struct AlgoCInput {
    int32_t paramCount;  const AlgoCParam* params;   // 节点参数（按 node.json 声明顺序）
    int32_t imageCount;  const AlgoCImage* images;   // 输入图像（连到 image 输入脚）
    int32_t cloudCount;  const AlgoCCloud* clouds;   // 输入点云（连到 cloud 输入脚）
} AlgoCInput;

// 输出图像：BGRA 紧排；bgra 由节点 malloc，宿主用 algo_free 释放。
typedef struct AlgoCImageOut { int32_t width, height; uint8_t* bgra; } AlgoCImageOut;

// 输出点云：xyz 为交错数组（count 个点）；xyz/rgb 由节点 malloc（rgb 可为 NULL），
// 宿主用 algo_free 释放。
typedef struct AlgoCCloudOut { int32_t count; float* xyz; uint8_t* rgb; } AlgoCCloudOut;

// 节点输出包：成功时由节点填充；images/clouds 数组本身与其内部缓冲均为 malloc
//（宿主用 algo_free 释放）；error 为 UTF-8 静态存储（宿主只读不释放）。
typedef struct AlgoCOutput {
    int32_t imageCount; AlgoCImageOut* images;
    int32_t cloudCount; AlgoCCloudOut* clouds;
    const char* error;   // 失败消息（静态存储，宿主只读不释放）
} AlgoCOutput;

#endif // COMUI_NATIVE_ABI_H
