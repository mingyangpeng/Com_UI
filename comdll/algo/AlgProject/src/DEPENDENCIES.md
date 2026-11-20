# 依赖库版本说明

## 项目概述
本项目包含五个动态库：
- **CommonAlg** - 通用基础模块（JSON / 文件 / 日志）
- **ShapelyAlg** - 二/三维几何基础库（GEOS / OpenCV / PCL）
- **Image2DAlg** - 2D 图像处理算法库（读取/通用处理/测量，上层封装 ShapelyAlg::ImageProcessor）
- **Point3DAlg** - 3D 点云处理算法库（SICK 读取 / 点云）
- **BusinessAlg** - 业务层模块（流程编排、业务语义）

## 核心依赖库

### OpenCV
| 项目 | 版本 | 用途 | 目录 |
|------|------|------|------|
| OpenCV | 4.4.0 | 2D 图像处理、特征提取、滤波、边缘检测 | `thrid/opencv-4.4.0/` |

**模块依赖：**
- core, imgproc, imgcodecs, features2d, highgui

### GEOS
| 项目 | 版本 | 用途 | 目录 |
|------|------|------|------|
| GEOS | 3.15.0 | ShapelyAlg 二维几何运算（布尔运算、面积、求交等） | `thrid/geos-3.15.0/` |

### PCL (Point Cloud Library)
| 项目 | 版本 | 用途 | 目录 |
|------|------|------|------|
| PCL | 1.9.1 | 3D 点云处理、配准、滤波、特征、分割 | `thrid/PCL-1.9.1/` |

**模块依赖：**
- common, io, filters, features, registration, segmentation, kdtree, search, visualization

**传递依赖：**
- Boost 1.68 (system, filesystem, thread, date_time, iostreams, serialization)
- Eigen 3.3+
- FLANN 1.9+
- VTK 8.2+ (可选，仅 visualization 模块)
- Qhull 2015.2+

**Boost 兼容性说明：** ShapelyAlg 通过 `src/ShapelyAlg/BoostCompat.h` 垫片支持
Boost 1.68 + 新版 MSVC STL（iostreams `fpos::seekpos` 兼容），版本以该头文件注释为准。

## 编译要求

| 项目 | 要求 |
|------|------|
| C++ 标准 | C++17 |
| CMake | 3.16+ |
| 编译器 | MSVC 2019+ / GCC 8+ / Clang 7+ |

## 输出产物

构建后生成 (在 `build/output/<Config>/`)：
```
CommonAlg.dll           # 通用基础模块 (JSON/文件/日志)
ShapelyAlg.dll          # 几何基础库 (GEOS/OpenCV/PCL)
Image2DAlg.dll          # 2D 算子 (Windows 动态库)
Point3DAlg.dll          # 3D 算子（SICK 读取 + 体素下采样）
BusinessAlg.dll         # 业务层算子（流程编排）
*.node.json             # ComUI 节点声明
```

## 部署说明 (ComUI 插件格式)

交付目录结构 `comdll/algo/<名称>/`：
```
<名称>/
├── CommonAlg.dll
├── ShapelyAlg.dll
├── Image2DAlg.dll
├── Point3DAlg.dll
├── BusinessAlg.dll
├── geos.dll / geos_c.dll        # GEOS 运行时 (ShapelyAlg 依赖)
├── opencv_world440.dll          # OpenCV 运行时 (Windows)
├── pcl_*_release.dll            # PCL 运行时（*_release 共 20 个，含依赖闭包 pcl_io_ply）
├── OpenNI2.dll (+ OpenNI2.jni.dll)  # pcl_io 传递依赖，缺失整链加载失败
├── vcomp140.dll                 # MSVC OpenMP 运行时（ShapelyAlg 点-网格距离并行，VC++ Redist 自带）
├── *.node.json                  # 算子声明 (ComUI 节点注册)
└── src/                         # 算子源码随包交付 (ComUI 编译期参考)
```

> Boost 1.68 为静态链接（`.lib`，无运行时 DLL，`dumpbin /dependents` 实测各 pcl DLL 均不依赖 boost DLL）。
> vcomp140.dll 来自 VC++ Redistributable（目标机装 Redist 即有；缺失时 ShapelyAlg.dll 加载失败），
> 来源 = ShapelyAlg 的 `/openmp:llvm`（点-网格距离家族并行，CMake 见 src/ShapelyAlg/CMakeLists.txt 注释）。

## 版本兼容性

| 组件 | 最低版本 | 推荐版本 | 当前使用 |
|------|----------|----------|----------|
| OpenCV | 4.0.0 | 4.4.0 | 4.4.0 |
| GEOS | 3.9 | 3.15.0 | 3.15.0 |
| PCL | 1.8.0 | 1.9.1 | 1.9.1 |
| Boost | 1.65 | 1.68+ | 1.68 |
| Eigen | 3.3 | 3.3+ | 3.3+ |

## 许可证

- OpenCV: BSD 3-Clause
- PCL: BSD 3-Clause
- Boost: BSL-1.0
- Eigen: MPL2

本项目代码遵循相同许可证或项目指定许可证。