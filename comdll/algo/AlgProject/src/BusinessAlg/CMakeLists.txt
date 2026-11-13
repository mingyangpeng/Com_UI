cmake_minimum_required(VERSION 3.16)
project(BusinessAlg LANGUAGES CXX)

# 业务层：组合 ShapelyAlg / Point3DAlg / Image2DAlg 完成具体业务流程。
# 原则：业务层不重复造轮子——几何/点云/图像的原子能力全部调基础模块，
#       本模块只做流程编排、参数适配与业务语义。
#
# 当前状态：含首个 ComUI 算子（区域缺陷率统计，见 business_region_ratio.node.json）。
# 后续算子往 include/BusinessAlg_api.h + BusinessAlg_comui.cpp 里加。

file(GLOB_RECURSE SOURCES "*.cpp" "*.cc" "*.cxx")
file(GLOB_RECURSE HEADERS "*.h" "*.hpp" "*.hxx")

# 测试入口含 main()，不应编进主 DLL（GLOB_RECURSE 会误收，排除规则与其他模块一致）
list(FILTER SOURCES EXCLUDE REGEX ".*_test\\.cpp$")

# ComUI 适配层（隔离在 integration/comui/，算法代码不感知）
# 不需要 ComUI 时注释掉下面两行即可，算法库照常工作。
list(APPEND SOURCES ${COMUI_ADAPTER_BUSINESS})

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# ComUI 适配层需要的头：ABI 定义（AlgExport.h 由 setup_export_macro 提供）
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/integration/comui
)

# 导出宏 + 可见性设置
setup_export_macro(${PROJECT_NAME})
setup_shared_library_props(${PROJECT_NAME})

# ABI 契约：error/消息字符串统一 UTF-8（MSVC 默认执行字符集是 GBK，需显式指定）
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
endif()

# 依赖三个基础模块（PUBLIC：业务层头文件会引用它们的类型）
target_link_libraries(${PROJECT_NAME} PUBLIC ShapelyAlg Point3DAlg Image2DAlg)

# 第三方头文件同样需要（基础模块的 include 是 PRIVATE，不传递）
link_third_party(${PROJECT_NAME} OPENCV GEOS PCL JSON)

# 部署：ComUI 节点声明（来自 integration/comui）
deploy_comui_nodes(${PROJECT_NAME})

# -----------------------------------------------------------------------------
# 测试入口
# -----------------------------------------------------------------------------
add_executable(BusinessAlg_test BusinessAlg_test.cpp)
target_link_libraries(BusinessAlg_test PRIVATE ${PROJECT_NAME})

# ABI 契约：断言消息中文统一 UTF-8（与主库一致）
if(MSVC)
    target_compile_options(BusinessAlg_test PRIVATE /utf-8)
endif()

# ctest：测试 exe 与全部运行时 DLL 同在 output/Release，工作目录指向那里
add_test(NAME BusinessAlg_test
    COMMAND BusinessAlg_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
)
