#ifndef COMUI_NATIVE_ABI_H
#define COMUI_NATIVE_ABI_H
// ComUI 原生算子统一 C ABI（v1）——与 docs/THIRD_PARTY_OPS.md §5.5 一致。
// 本文件为纯 C 结构声明；入口函数的导出宏由各 DLL 的适配 .cpp 用自己的 <Target>_EXPORT 标注。
#include <stdint.h>

typedef struct AlgoCParam {
    int32_t kind;          // 0=double 1=int32 2=text(UTF-8) 3=bool(i!=0)
    double  d;
    int32_t i;
    const char* s;
} AlgoCParam;

typedef struct AlgoCImage { int32_t width, height; const uint8_t* bgra; } AlgoCImage;
typedef struct AlgoCCloud { int32_t count; const float* xyz; const uint8_t* rgb; } AlgoCCloud;

typedef struct AlgoCInput {
    int32_t paramCount;  const AlgoCParam* params;
    int32_t imageCount;  const AlgoCImage* images;
    int32_t cloudCount;  const AlgoCCloud* clouds;
} AlgoCInput;

typedef struct AlgoCImageOut { int32_t width, height; uint8_t* bgra; } AlgoCImageOut;
typedef struct AlgoCCloudOut { int32_t count; float* xyz; uint8_t* rgb; } AlgoCCloudOut;

typedef struct AlgoCOutput {
    int32_t imageCount; AlgoCImageOut* images;
    int32_t cloudCount; AlgoCCloudOut* clouds;
    const char* error;   // 失败消息（静态存储，宿主只读不释放）
} AlgoCOutput;

#endif // COMUI_NATIVE_ABI_H
