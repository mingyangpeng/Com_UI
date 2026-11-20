cmake_minimum_required(VERSION 3.16)
project(Image2DAlg LANGUAGES CXX)

# 算法源文件（本目录，不含 ComUI 相关代码）
file(GLOB_RECURSE SOURCES "*.cpp" "*.cc" "*.cxx")
file(GLOB_RECURSE HEADERS "*.h" "*.hpp" "*.hxx")

# 测试入口含 main()，不应编进主 DLL（GLOB_RECURSE 会误收，排除规则与 Point3DAlg 一致）
list(FILTER SOURCES EXCLUDE REGEX ".*_test\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*_CalibTest\\.cpp$")

# ComUI 适配层（隔离在 integration/comui/，算法代码不感知）
# 不需要 ComUI 时注释掉下面两行即可，算法库照常工作。
list(APPEND SOURCES ${COMUI_ADAPTER_IMAGE2D})

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# ComUI 适配层需要的头：AlgExport.h（导出宏）、ABI 定义、本模块 public 头
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/integration/comui
    ${CMAKE_SOURCE_DIR}/src/CommonAlg/include   # CommonAlg_FileUtil（持久化用）
)

# 导出宏 + 可见性设置
setup_export_macro(${PROJECT_NAME})
setup_shared_library_props(${PROJECT_NAME})

# ABI 契约：error/消息字符串统一 UTF-8（MSVC 默认执行字符集是 GBK，需显式指定）
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# Eigen（标定求解用，头文件随 PCL 3rdParty）
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/thrid/PCL-1.9.1/3rdParty/Eigen/eigen3
)

# CommonAlg（标定持久化的文件读写）
target_link_libraries(${PROJECT_NAME} PUBLIC CommonAlg)

# ShapelyAlg：通用 2D 是 ImageProcessor 的上层，图像算子复用其实现（不重复造轮子）。
# 注意：这会让 Image2DAlg.dll 运行期依赖 ShapelyAlg.dll（及其 PCL/GEOS/OpenCV 运行时），
# 交付时与现有 output/Release 一样整目录平铺即可。
target_link_libraries(${PROJECT_NAME} PRIVATE ShapelyAlg)

# 链接第三方库
link_third_party(${PROJECT_NAME} OPENCV)

# 部署：ComUI 节点声明（来自 integration/comui）
deploy_comui_nodes(${PROJECT_NAME})

# -----------------------------------------------------------------------------
# 标定验证：模拟数据 + 真值对照（无 UI）
# -----------------------------------------------------------------------------
add_executable(Image2DAlg_CalibTest Image2DAlg_CalibTest.cpp)
target_link_libraries(Image2DAlg_CalibTest PRIVATE ${PROJECT_NAME})
target_include_directories(Image2DAlg_CalibTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# 测试代码允许用 calib3d（cv::Rodrigues 等），主库保持零 calib3d 依赖
if(MSVC)
    target_link_libraries(Image2DAlg_CalibTest PRIVATE ${OpenCV_LIBS})
endif()

# ctest：CalibTest 与运行时 DLL 同在 output/Release（无外部数据文件依赖——
# 模拟数据自生成，持久化测试把临时文件写到工作目录）
add_test(NAME Image2DAlg_CalibTest
    COMMAND Image2DAlg_CalibTest
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
)

# -----------------------------------------------------------------------------
# C 接口测试入口（ctest 第 5 项）
# -----------------------------------------------------------------------------
add_executable(Image2DAlg_test Image2DAlg_test.cpp)
target_link_libraries(Image2DAlg_test PRIVATE ${PROJECT_NAME})
target_include_directories(Image2DAlg_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
# 测试自写 png（imwrite），需要 imgcodecs；与 CalibTest 同样补 OpenCV 链接
if(MSVC)
    target_link_libraries(Image2DAlg_test PRIVATE ${OpenCV_LIBS})
endif()
# ABI 契约：源文件 UTF-8 BOM，MSVC 执行字符集显式 UTF-8（与主库一致）
if(MSVC)
    target_compile_options(Image2DAlg_test PRIVATE /utf-8)
endif()

# ctest：测试 exe 与全部运行时 DLL 同在 output/Release，工作目录指向那里
add_test(NAME Image2DAlg_test
    COMMAND Image2DAlg_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
)
