cmake_minimum_required(VERSION 3.16)
project(CommonAlg LANGUAGES CXX)

# CommonAlg —— 通用基础模块（算法与 UI 共用，零业务语义）
#   Json       JSON 读取（nlohmann 封装 + GBK/UTF-8 编码自动检测）
#   FileUtil   文件/目录枚举、读写、路径操作（跨平台）
#   Log        分级日志（时间戳 + 可选文件输出，线程安全）
#   Yaml       预留，需在 thrid 引入 yaml-cpp 后实现（暂无计划，不写代码桩）

file(GLOB SOURCES "*.cpp")
file(GLOB HEADERS "include/*.h")

add_library(${PROJECT_NAME} SHARED ${SOURCES} ${HEADERS})

# include 目录（本目录 / include/ / src 根的 AlgExport.h）由 setup_export_macro 统一设置
setup_export_macro(${PROJECT_NAME})
setup_shared_library_props(${PROJECT_NAME})

# JSON 需要 nlohmann
target_link_libraries(${PROJECT_NAME} PUBLIC nlohmann_json_headers)

# -----------------------------------------------------------------------------
# 测试入口
# -----------------------------------------------------------------------------
add_executable(CommonAlg_test CommonAlg_test.cpp)
target_link_libraries(CommonAlg_test PRIVATE ${PROJECT_NAME})

# ctest：测试 exe 与全部运行时 DLL 同在 output/Release，工作目录指向那里
add_test(NAME CommonAlg_test
    COMMAND CommonAlg_test
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/output/Release
)
