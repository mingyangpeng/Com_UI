cmake_minimum_required(VERSION 3.16)
project(Point3DAlg LANGUAGES CXX)

# 算法源文件（本目录，不含 ComUI 与 tools/；排除测试入口，测试 exe 单独成目标）
#   tools/ 下是配准交互工具（exe）+ 其算法层（Regist*.cpp 编入库）
file(GLOB_RECURSE SOURCES "*.cpp" "*.cc" "*.cxx")
list(FILTER SOURCES EXCLUDE REGEX ".*_test\\.cpp$")
list(FILTER SOURCES EXCLUDE REGEX ".*[/\\\\]tools[/\\\\].*")
# 点云算子 C 封装依赖 PCL（CloudIO/CloudProcessor）——从 GLOB 摘出，按 PCL_FOUND 条件编入
list(FILTER SOURCES EXCLUDE REGEX ".*Point3DAlg_CloudOps\\.cpp$")
file(GLOB_RECURSE HEADERS "*.h" "*.hpp" "*.hxx")
list(FILTER HEADERS EXCLUDE REGEX ".*[/\\\\]tools[/\\\\].*")

# 配准算法层（纯函数、零 UI）编入库，供 ComUI 节点调用
# 含 <pcl/point_cloud.h>，必须与 RegistTool/RegistTest 一致地用 PCL_FOUND 保护：
# PCL 未找到时库内不含配准层，ComUI 配准节点相应不可用
if(PCL_FOUND)
    # 点云算子 C 封装（LoadCloud/SaveCloudEx/Filter/Extract/Transform；
    # 依赖 PCL——无 PCL 构建时整组符号不导出，见 Point3DAlg_api.h 守卫说明）
    list(APPEND SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/Point3DAlg_CloudOps.cpp)
    list(APPEND SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_Regist.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_DomeFit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_RegionPlane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_CloudDiff.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_CircleFit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_Profile.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tools/Point3DAlg_BumpReport.cpp
    )
endif()

# ComUI 适配层（隔离在 integration/comui/，算法代码不感知）
# 不需要 ComUI 时注释掉下面一行即可，算法库照常工作。
list(APPEND SOURCES ${COMUI_ADAPTER_POINT3D})

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/integration/comui
        ${CMAKE_SOURCE_DIR}/src/ShapelyAlg           # 配准算法层复用基础库算子
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools>
)

# 导出宏 + 可见性设置
setup_export_macro(${PROJECT_NAME})
setup_shared_library_props(${PROJECT_NAME})

# ABI 契约：error/消息字符串统一 UTF-8（MSVC 默认执行字符集是 GBK，需显式指定）
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# 通用基础模块（JSON 封装 / 文件工具 / 日志）
target_link_libraries(${PROJECT_NAME} PUBLIC CommonAlg)

# 配准算法层复用 ShapelyAlg 的 CloudProcessor（ICP 家族）——仅 PCL 可用时编译
if(PCL_FOUND)
    target_link_libraries(${PROJECT_NAME} PRIVATE ShapelyAlg)
    # 通知 ComUI 适配层：本库含 PCL 依赖入口（无 PCL 时这些入口不编译、符号不导出——
    # 宿主加载相应节点会得到明确的"方法不存在"，而非 DLL 链接失败；见适配层各守卫块）
    target_compile_definitions(${PROJECT_NAME} PRIVATE POINT3DALG_HAS_PCL=1)
endif()

# 链接第三方库（OpenCV：FOV 标注画框/文字；JSON：配置解析；PCL：配准 + 可视化）
link_third_party(${PROJECT_NAME} OPENCV JSON PCL)

# 部署：ComUI 节点声明（来自 integration/comui）
deploy_comui_nodes(${PROJECT_NAME})

# -----------------------------------------------------------------------------
# 配准交互工具（PCLVisualizer + 键盘，独立 exe，不属 ComUI 算子）
#   PCL 1.9.1 的 pcl_visualization 静态链接了 VTK，无需额外 VTK 运行时 DLL
# -----------------------------------------------------------------------------
if(PCL_FOUND)
    add_executable(Point3DAlg_RegistTool tools/Point3DAlg_RegistTool.cpp)
    target_link_libraries(Point3DAlg_RegistTool PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_RegistTool PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_SOURCE_DIR}/src/ShapelyAlg          # BoostCompat.h（Boost 1.68 垫片）
    )
    if(MSVC)
        # PCL visualization 模板展开段数大，必须 /bigobj
        target_compile_options(Point3DAlg_RegistTool PRIVATE /bigobj)
    endif()
endif()

# -----------------------------------------------------------------------------
# 配准算法层单元测试（无 UI，验证 6 种策略）
# 测试源 include Point3DAlg_Regist.h（直接依赖 PCL）——仅 PCL_FOUND 时构建，
# 与 RegistTool 及上方工具测试目标的守卫方式一致（工单 002 无 PCL 构建）
# -----------------------------------------------------------------------------
if(PCL_FOUND)
    add_executable(Point3DAlg_RegistTest tools/Point3DAlg_RegistTest.cpp)
    target_link_libraries(Point3DAlg_RegistTest PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_RegistTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tools)
    if(MSVC)
        target_compile_options(Point3DAlg_RegistTest PRIVATE /bigobj)
    endif()

    # ctest：自造演示数据，无外部依赖；SaveTransform 临时文件写在工作目录，
    # 指到 output/Release（与 DLL 同目录，避免 PATH 依赖）
    add_test(NAME Point3DAlg_RegistTest
        COMMAND Point3DAlg_RegistTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )
endif()

# -----------------------------------------------------------------------------
# 拱形球顶 bump 拟合测试（合成自检注册 ctest；真实数据模式带文件参数手动跑）
# -----------------------------------------------------------------------------
if(PCL_FOUND)
    add_executable(Point3DAlg_DomeFitTest tools/Point3DAlg_DomeFitTest.cpp)
    target_link_libraries(Point3DAlg_DomeFitTest PRIVATE ${PROJECT_NAME} ShapelyAlg)
    target_include_directories(Point3DAlg_DomeFitTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
        ${CMAKE_SOURCE_DIR}/src/ShapelyAlg          # ShapelyAlg.h（CloudIO 读 PLY）
    )
    if(MSVC)
        target_compile_options(Point3DAlg_DomeFitTest PRIVATE /bigobj /utf-8)
    endif()
    add_test(NAME Point3DAlg_DomeFitTest
        COMMAND Point3DAlg_DomeFitTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )

    # -----------------------------------------------------------------------------
    # ROI 平面量测测试（合成数据，注册 ctest）
    # -----------------------------------------------------------------------------
    add_executable(Point3DAlg_RegionPlaneTest tools/Point3DAlg_RegionPlaneTest.cpp)
    target_link_libraries(Point3DAlg_RegionPlaneTest PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_RegionPlaneTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    if(MSVC)
        target_compile_options(Point3DAlg_RegionPlaneTest PRIVATE /bigobj /utf-8)
    endif()
    add_test(NAME Point3DAlg_RegionPlaneTest
        COMMAND Point3DAlg_RegionPlaneTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )

    # -----------------------------------------------------------------------------
    # 点云高度差对比测试（合成数据，注册 ctest）
    # -----------------------------------------------------------------------------
    add_executable(Point3DAlg_CloudDiffTest tools/Point3DAlg_CloudDiffTest.cpp)
    target_link_libraries(Point3DAlg_CloudDiffTest PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_CloudDiffTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    if(MSVC)
        target_compile_options(Point3DAlg_CloudDiffTest PRIVATE /bigobj /utf-8)
    endif()
    add_test(NAME Point3DAlg_CloudDiffTest
        COMMAND Point3DAlg_CloudDiffTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )

    # -----------------------------------------------------------------------------
    # 高度切片圆/孔位量测测试（合成数据，注册 ctest）
    # -----------------------------------------------------------------------------
    add_executable(Point3DAlg_CircleFitTest tools/Point3DAlg_CircleFitTest.cpp)
    target_link_libraries(Point3DAlg_CircleFitTest PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_CircleFitTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    if(MSVC)
        target_compile_options(Point3DAlg_CircleFitTest PRIVATE /bigobj /utf-8)
    endif()
    add_test(NAME Point3DAlg_CircleFitTest
        COMMAND Point3DAlg_CircleFitTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )

    # -----------------------------------------------------------------------------
    # 轮廓/截面量测测试（合成数据，注册 ctest）
    # -----------------------------------------------------------------------------
    add_executable(Point3DAlg_ProfileTest tools/Point3DAlg_ProfileTest.cpp)
    target_link_libraries(Point3DAlg_ProfileTest PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_ProfileTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    if(MSVC)
        target_compile_options(Point3DAlg_ProfileTest PRIVATE /bigobj /utf-8)
    endif()
    add_test(NAME Point3DAlg_ProfileTest
        COMMAND Point3DAlg_ProfileTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )

    # -----------------------------------------------------------------------------
    # bump 全参数报表测试（合成数据，注册 ctest）
    # -----------------------------------------------------------------------------
    add_executable(Point3DAlg_BumpReportTest tools/Point3DAlg_BumpReportTest.cpp)
    target_link_libraries(Point3DAlg_BumpReportTest PRIVATE ${PROJECT_NAME})
    target_include_directories(Point3DAlg_BumpReportTest PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tools
    )
    if(MSVC)
        target_compile_options(Point3DAlg_BumpReportTest PRIVATE /bigobj /utf-8)
    endif()
    add_test(NAME Point3DAlg_BumpReportTest
        COMMAND Point3DAlg_BumpReportTest
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
    )
endif()

# -----------------------------------------------------------------------------
# SICK 拼接合成回归（自造 .dat，无外部数据依赖；验证单机列映射/多相机交织/
# 偏移对齐/FOV 框坐标——2026-11-10 交织拷贝与框坐标修复的回归守卫）
# -----------------------------------------------------------------------------
add_executable(Point3DAlg_SickSynthTest tools/Point3DAlg_SickSynthTest.cpp)
target_link_libraries(Point3DAlg_SickSynthTest PRIVATE ${PROJECT_NAME})
add_test(NAME Point3DAlg_SickSynthTest
    COMMAND Point3DAlg_SickSynthTest
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
)

# -----------------------------------------------------------------------------
# 测试入口：控制台 exe（数据目录缺省内置 Z:/ScanImage/3D 等，可经命令行传参覆盖）
# Release 也带完整调试信息（/Z7 + /DEBUG，与 Juxin6X3DAlg 的调试配置一致）
# -----------------------------------------------------------------------------
# 注意：Point3DAlg_test 不注册 ctest——依赖外部 Z: 盘扫描数据（各 ~360MB），数据不在的机器上跑只会 SKIP（退出码 77）；
# 数据齐时可手动运行验证（Point3DAlg_test.exe [相机1目录] [相机2目录 ...]），需要时临时 add_test 后再 ctest -R Point3DAlg_test。
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /Z7 /Od")
set_target_properties(${PROJECT_NAME} PROPERTIES
    LINK_FLAGS "/INCREMENTAL /DEBUG /OPT:REF"
)

add_executable(Point3DAlg_test Point3DAlg_test.cpp)
target_link_libraries(Point3DAlg_test PRIVATE ${PROJECT_NAME})
# 点云计算族测试段（LoadCloud/SaveCloudEx/Filter/Extract/Transform）调用的是
# DLL 内 PCL 守卫导出的符号——PCL 可用时才编译这些测试段，否则链接会失败
if(PCL_FOUND)
    target_compile_definitions(Point3DAlg_test PRIVATE POINT3DALG_TEST_HAS_PCL=1)
endif()
set_target_properties(Point3DAlg_test PROPERTIES
    LINK_FLAGS "/INCREMENTAL /DEBUG /OPT:REF"
)
