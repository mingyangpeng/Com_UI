cmake_minimum_required(VERSION 3.16)
project(ShapelyAlg LANGUAGES CXX)

# 单文件夹布局：
#   ShapelyAlg.h        对外总入口（业务层只 include 它）
#   *.h/*.cpp           模块头与实现（业务层不直接引用）
#   ShapelyAlg_test.cpp 单元测试入口
#   ShapelyAlg_demo.cpp 第三方调用实例（独立 exe）

file(GLOB SOURCES "*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*_test\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*_demo\.cpp$")
file(GLOB HEADERS "*.h")

# 无 PCL 构建（工单 002）：PCL 依赖 TU 从 GLOB 摘出，按 PCL_FOUND 条件编入
# （同 Point3DAlg 摘 Point3DAlg_CloudOps.cpp 的先例）：
#   CloudProcessor.cpp / CloudIO.cpp  顶部直接 include <pcl/*>；
#   CloudProcessorMesh.cpp            仅为 Mesh3d include CloudProcessor.h（间接引 PCL）；
#   ShapelyAlg_bench.cpp              直接 include <pcl/*> 并使用 CloudProcessor
#                                     （该文件因 GLOB 未排除 _bench 后缀而编入库内，
#                                     属既有行为，有 PCL 时保持原样）。
# 无 PCL 时这四个 TU 与 demo/bench 目标不编入；Geom/ImageProcessor/Stereo 不依赖
# PCL，库照常构建。
list(FILTER SOURCES EXCLUDE REGEX ".*CloudIO\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*CloudProcessor\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*CloudProcessorMesh\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*ShapelyAlg_bench\.cpp$")
if(PCL_FOUND)
    list(APPEND SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/CloudIO.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CloudProcessor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/CloudProcessorMesh.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ShapelyAlg_bench.cpp)
    # file(GLOB) 按字典序返回，SORT 复原有 PCL 时的原编译顺序（导出面/符号序不变）
    list(SORT SOURCES)
endif()

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# 头文件搜索路径：模块根（全部头都在这一层）
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# 导出宏 + 可见性设置
setup_export_macro(${PROJECT_NAME})
setup_shared_library_props(${PROJECT_NAME})

# 基准实测：点-网格距离家族为唯一热点（20k×12.8 万面 29/44s）→ OpenMP 按 point 并行；
# 其余热点 <60ms 实测无需并行。⚠️ /openmp:llvm 只用于 CloudProcessorMesh.cpp（纯 POD
# 数学 TU）——对含 Eigen/PCL 重型模板的 CloudProcessor.cpp 全量开 OpenMP 会访问冲突
#（实测 segfault，崩溃点漂移）；运行时 vcomp140.dll 随 VC++ Redist 分发
set_source_files_properties(CloudProcessorMesh.cpp PROPERTIES
    COMPILE_OPTIONS "/openmp:llvm")

# PCL 可用性宏（工单 002）：ShapelyAlg.h 的点云面与测试 PCL 段以 SHAPELYALG_HAS_PCL
# 守卫（沿用 Point3DAlg 的 POINT3DALG_HAS_PCL 先例）。PUBLIC：Point3DAlg tools /
# BusinessAlg / 本测试等消费方 include ShapelyAlg.h 时需同样感知 PCL 是否可用，
# 经 target 链接关系自动传递，无需各自再定义。
if(PCL_FOUND)
    target_compile_definitions(${PROJECT_NAME} PUBLIC SHAPELYALG_HAS_PCL=1)
endif()

# 链接第三方库（OpenCV：cv::Mat/Point 数据结构；GEOS：二维几何；PCL：点云算子）
link_third_party(${PROJECT_NAME} OPENCV GEOS PCL)

# 部署配置：复制 JSON 到 output/
setup_deployment(${PROJECT_NAME})

# -----------------------------------------------------------------------------
# 测试入口：控制台 exe，验证 shapely 全部模块（只 include ShapelyAlg.h）
# 测试把中间文件写死到 D:/pmy/alg/output/，工作目录须保证该目录存在
# 点云用例段以 SHAPELYALG_HAS_PCL 守卫——定义随链接从 ShapelyAlg（PUBLIC）传入，
# 无 PCL 时该段用例自动不编译（工单 002）
# -----------------------------------------------------------------------------
add_executable(ShapelyAlg_test ShapelyAlg_test.cpp)
target_link_libraries(ShapelyAlg_test PRIVATE ${PROJECT_NAME})
add_test(NAME ShapelyAlg_test
    COMMAND ShapelyAlg_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
)

# -----------------------------------------------------------------------------
# 第三方调用实例：完整可运行的两个场景（2D 缺陷统计 / 3D 凸台测量）
# 仅演示输出 png/ply、恒返回 0，无断言，不做 ctest
# 源直接 include <pcl/*> 并使用 CloudProcessor/CloudIO——仅 PCL_FOUND 时构建
# -----------------------------------------------------------------------------
if(PCL_FOUND)
    add_executable(ShapelyAlg_demo ShapelyAlg_demo.cpp)
    target_link_libraries(ShapelyAlg_demo PRIVATE ${PROJECT_NAME})
endif()

# -----------------------------------------------------------------------------
# 热点基准（方向 D 第 1 步：OpenMP 决策依据）——确定性数据 + 单次预热多轮取优；
# 计时类不宜 CI，不注册 ctest；规模注释见各 case
# 源直接 include <pcl/*> 并使用 CloudProcessor——仅 PCL_FOUND 时构建
# -----------------------------------------------------------------------------
if(PCL_FOUND)
    add_executable(ShapelyAlg_bench ShapelyAlg_bench.cpp)
    target_link_libraries(ShapelyAlg_bench PRIVATE ${PROJECT_NAME})
endif()
