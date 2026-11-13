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

# 链接第三方库（OpenCV：cv::Mat/Point 数据结构；GEOS：二维几何；PCL：点云算子）
link_third_party(${PROJECT_NAME} OPENCV GEOS PCL)

# 部署配置：复制 JSON 到 output/
setup_deployment(${PROJECT_NAME})

# -----------------------------------------------------------------------------
# 测试入口：控制台 exe，验证 shapely 全部模块（只 include ShapelyAlg.h）
# 测试把中间文件写死到 D:/pmy/alg/output/，工作目录须保证该目录存在
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
# -----------------------------------------------------------------------------
add_executable(ShapelyAlg_demo ShapelyAlg_demo.cpp)
target_link_libraries(ShapelyAlg_demo PRIVATE ${PROJECT_NAME})

# -----------------------------------------------------------------------------
# 热点基准（方向 D 第 1 步：OpenMP 决策依据）——确定性数据 + 单次预热多轮取优；
# 计时类不宜 CI，不注册 ctest；规模注释见各 case
# -----------------------------------------------------------------------------
add_executable(ShapelyAlg_bench ShapelyAlg_bench.cpp)
target_link_libraries(ShapelyAlg_bench PRIVATE ${PROJECT_NAME})
