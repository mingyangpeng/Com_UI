#ifndef ALG_EXPORT_H
#define ALG_EXPORT_H

// 统一导出宏（各模块共用，源码树静态文件；不再用 GenerateExportHeader 生成）。
//
// 机制：编译本 DLL 时 CMake 定义 <MODULE>_BUILD_DLL → 属性展开为 dllexport；
//       外部使用时不定义 → 展开为 dllimport。
//
// 用法（各模块公共头）：
//   #include "AlgExport.h"
//   SHAPELYALG_EXPORT class Geom { ... };
//   SHAPELYALG_EXPORT int SomeFunc(void);
//
// 注意：开关宏名必须是 <MODULE>_BUILD_DLL，不能直接用 -DSHAPELYALG_EXPORT——
//   那样宏会被定义成 1，`SHAPELYALG_EXPORT class Geom` 会展开成 `1 class Geom`。
//
// 新增模块：复制一组下面的分支，把模块名替换掉即可。

// ---------------- CommonAlg ----------------
#ifdef _WIN32
#  ifdef COMMONALG_BUILD_DLL
#    define COMMONALG_EXPORT __declspec(dllexport)
#  else
#    define COMMONALG_EXPORT __declspec(dllimport)
#  endif
#else
#  define COMMONALG_EXPORT __attribute__((visibility("default")))
#endif

// ---------------- ShapelyAlg ----------------
#if defined(_WIN32)
#  ifdef SHAPELYALG_BUILD_DLL
#    define SHAPELYALG_EXPORT __declspec(dllexport)
#  else
#    define SHAPELYALG_EXPORT __declspec(dllimport)
#  endif
#else
#  define SHAPELYALG_EXPORT __attribute__((visibility("default")))
#endif

// ---------------- Image2DAlg ----------------
#if defined(_WIN32)
#  ifdef IMAGE2DALG_BUILD_DLL
#    define IMAGE2DALG_EXPORT __declspec(dllexport)
#  else
#    define IMAGE2DALG_EXPORT __declspec(dllimport)
#  endif
#else
#  define IMAGE2DALG_EXPORT __attribute__((visibility("default")))
#endif

// ---------------- Point3DAlg ----------------
#if defined(_WIN32)
#  ifdef POINT3DALG_BUILD_DLL
#    define POINT3DALG_EXPORT __declspec(dllexport)
#  else
#    define POINT3DALG_EXPORT __declspec(dllimport)
#  endif
#else
#  define POINT3DALG_EXPORT __attribute__((visibility("default")))
#endif

// ---------------- BusinessAlg ----------------
#if defined(_WIN32)
#  ifdef BUSINESSALG_BUILD_DLL
#    define BUSINESSALG_EXPORT __declspec(dllexport)
#  else
#    define BUSINESSALG_EXPORT __declspec(dllimport)
#  endif
#else
#  define BUSINESSALG_EXPORT __attribute__((visibility("default")))
#endif

#endif // ALG_EXPORT_H
