cmake_minimum_required(VERSION 3.16)

# =============================================================================
# 通用配置：第三方库查找、动态库导出设置、编译选项、部署文件收集
# =============================================================================

# -----------------------------------------------------------------------------
# 第三方库路径
# -----------------------------------------------------------------------------
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/thrid)

# -----------------------------------------------------------------------------
# OpenCV 查找
# -----------------------------------------------------------------------------
find_package(OpenCV REQUIRED PATHS ${THIRD_PARTY_DIR}/opencv-4.4.0 NO_DEFAULT_PATH)

# -----------------------------------------------------------------------------
# PCL 查找（当前代码未使用 PCL——适配层自含体素实现；接入 PCL 时改回 REQUIRED）
# -----------------------------------------------------------------------------
find_package(PCL QUIET PATHS ${THIRD_PARTY_DIR}/PCL-1.9.1 NO_DEFAULT_PATH)

# -----------------------------------------------------------------------------
# nlohmann/json（header-only，直接引头文件目录）
# -----------------------------------------------------------------------------
add_library(nlohmann_json_headers INTERFACE)
target_include_directories(nlohmann_json_headers INTERFACE ${THIRD_PARTY_DIR}/nlohmann_json/single_include)

# -----------------------------------------------------------------------------
# GEOS（预编译二进制：include/lib/bin，C API geos_c）
# -----------------------------------------------------------------------------
set(GEOS_DIR ${THIRD_PARTY_DIR}/geos-3.15.0)
add_library(GEOS::GEOS INTERFACE IMPORTED)
target_include_directories(GEOS::GEOS INTERFACE ${GEOS_DIR}/include)
target_link_libraries(GEOS::GEOS INTERFACE ${GEOS_DIR}/lib/geos_c.lib ${GEOS_DIR}/lib/geos.lib)

# -----------------------------------------------------------------------------
# 通用编译选项
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 警告等级（COMMON_OPTIONS = 接口库，供各子目标 link 进编译选项）
add_library(COMMON_OPTIONS INTERFACE)
if(MSVC)
    target_compile_options(COMMON_OPTIONS INTERFACE
        /W4 /permissive- /MP
    )
else()
    target_compile_options(COMMON_OPTIONS INTERFACE
        -Wall -Wextra -Wpedantic -Wno-unused-parameter
    )
endif()

# -----------------------------------------------------------------------------
# 导出宏设置辅助函数
# 机制：源码树静态头 AlgExport.h 定义 <MODULE>_EXPORT 属性宏；
#       编译本 DLL 时定义 <MODULE>_BUILD_DLL → dllexport，外部使用 → dllimport。
#       不再使用 GenerateExportHeader（少一个生成步骤、少一处构建产物）。
# -----------------------------------------------------------------------------
function(setup_export_macro target_name)
    string(TOUPPER "${target_name}" TARGET_UPPER)   # 与各头文件的 <TARGET>_EXPORT 用法一致

    # 编译本库：定义 <MODULE>_BUILD_DLL，属性宏展开为 dllexport
    target_compile_definitions(${target_name} PRIVATE ${TARGET_UPPER}_BUILD_DLL)

    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src>   # AlgExport.h 所在目录
    )
endfunction()

# -----------------------------------------------------------------------------
# 通用动态库属性设置
# -----------------------------------------------------------------------------
function(setup_shared_library_props target_name)
    if(NOT MSVC)
        set_target_properties(${target_name} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
    else()
        set_target_properties(${target_name} PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS OFF
        )
    endif()

    set_target_properties(${target_name} PROPERTIES
        POSITION_INDEPENDENT_CODE ON
    )
endfunction()

# -----------------------------------------------------------------------------
# 通用第三方库链接设置
# -----------------------------------------------------------------------------
function(link_third_party target_name)
    set(options OPENCV PCL JSON GEOS)
    set(oneValueArgs)
    set(multiValueArgs)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(ARG_OPENCV)
        target_link_libraries(${target_name} PUBLIC ${OpenCV_LIBS})
        target_include_directories(${target_name} PRIVATE ${OpenCV_INCLUDE_DIRS})
    endif()

    if(ARG_PCL AND PCL_FOUND)
        target_link_libraries(${target_name} PUBLIC ${PCL_LIBRARIES})
        target_include_directories(${target_name} PRIVATE ${PCL_INCLUDE_DIRS})
        target_compile_options(${target_name} PRIVATE ${PCL_CXX_FLAGS})
        if(MSVC)
            # PCL 模板展开段数超 COFF 对象格式限制（Debug 无优化时更甚），必须 /bigobj
            target_compile_options(${target_name} PRIVATE /bigobj)
        endif()
    endif()

    if(ARG_JSON)
        target_link_libraries(${target_name} PUBLIC nlohmann_json_headers)
    endif()

    if(ARG_GEOS)
        target_link_libraries(${target_name} PUBLIC GEOS::GEOS)
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 部署文件收集：复制 JSON、源码交付物（src/）到 output/
# -----------------------------------------------------------------------------
function(setup_deployment target_name)
    # 复制当前目录下的 *.json 文件到 output/（算子参数/配置，非 ComUI 声明）
    # ComUI 节点声明由 integration/comui 的 deploy_comui_nodes() 单独负责。
    file(GLOB JSON_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.json)
    if(JSON_FILES)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${JSON_FILES}
            $<TARGET_FILE_DIR:${target_name}>
            COMMENT "Copy JSON files to output directory"
        )
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 收集第三方运行时 DLL 到 output/
# 在项目根目录调用一次即可
# -----------------------------------------------------------------------------
function(collect_third_party_runtime_dlls)
    if(NOT MSVC)
        return()
    endif()
    # 注意：本函数无参数；各 copy_*_dlls 目标在下方用 add_dependencies 逐一挂到汇总目标上

    set(DEPLOY_DIR ${CMAKE_BINARY_DIR}/output/$<CONFIG>)

    # OpenCV DLLs
    file(GLOB OPENCV_DLLS ${THIRD_PARTY_DIR}/opencv-4.4.0/x64/vc16/bin/*.dll
                        ${THIRD_PARTY_DIR}/opencv-4.4.0/bin/*.dll
                        ${THIRD_PARTY_DIR}/opencv-4.4.0/*.dll)
    if(OPENCV_DLLS)
        add_custom_command(OUTPUT ${DEPLOY_DIR}/opencv_copied.stamp
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OPENCV_DLLS} ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${DEPLOY_DIR}/opencv_copied.stamp
            COMMENT "Copy OpenCV runtime DLLs to output"
        )
        add_custom_target(copy_opencv_dlls DEPENDS ${DEPLOY_DIR}/opencv_copied.stamp)
    endif()

    # PCL DLLs（含 3rdParty：OpenNI2 被 pcl_io 直接依赖，缺失会导致整链加载失败）
    file(GLOB PCL_DLLS ${THIRD_PARTY_DIR}/PCL-1.9.1/bin/*.dll
                      ${THIRD_PARTY_DIR}/PCL-1.9.1/*.dll
                      ${THIRD_PARTY_DIR}/PCL-1.9.1/3rdParty/OpenNI2/Redist/*.dll)
    if(PCL_DLLS)
        add_custom_command(OUTPUT ${DEPLOY_DIR}/pcl_copied.stamp
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${PCL_DLLS} ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${DEPLOY_DIR}/pcl_copied.stamp
            COMMENT "Copy PCL runtime DLLs to output"
        )
        add_custom_target(copy_pcl_dlls DEPENDS ${DEPLOY_DIR}/pcl_copied.stamp)
    endif()

    # Boost DLLs (PCL 依赖)
    file(GLOB BOOST_DLLS ${THIRD_PARTY_DIR}/PCL-1.9.1/3rdParty/Boost/bin/*.dll
                          ${THIRD_PARTY_DIR}/boost_*/lib64-msvc-*/*.dll
                          ${THIRD_PARTY_DIR}/boost_*/stage/lib/*.dll)
    if(BOOST_DLLS)
        add_custom_command(OUTPUT ${DEPLOY_DIR}/boost_copied.stamp
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BOOST_DLLS} ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${DEPLOY_DIR}/boost_copied.stamp
            COMMENT "Copy Boost runtime DLLs to output"
        )
        add_custom_target(copy_boost_dlls DEPENDS ${DEPLOY_DIR}/boost_copied.stamp)
    endif()

    # GEOS DLLs
    file(GLOB GEOS_DLLS ${THIRD_PARTY_DIR}/geos-*/bin/*.dll)
    if(GEOS_DLLS)
        add_custom_command(OUTPUT ${DEPLOY_DIR}/geos_copied.stamp
            COMMAND ${CMAKE_COMMAND} -E make_directory ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${GEOS_DLLS} ${DEPLOY_DIR}
            COMMAND ${CMAKE_COMMAND} -E touch ${DEPLOY_DIR}/geos_copied.stamp
            COMMENT "Copy GEOS runtime DLLs to output"
        )
        add_custom_target(copy_geos_dlls DEPENDS ${DEPLOY_DIR}/geos_copied.stamp)
    endif()

    # 汇总目标
    if(TARGET copy_opencv_dlls OR TARGET copy_pcl_dlls OR TARGET copy_boost_dlls OR TARGET copy_geos_dlls)
        add_custom_target(deploy_runtime_dlls)
        if(TARGET copy_opencv_dlls)
            add_dependencies(deploy_runtime_dlls copy_opencv_dlls)
        endif()
        if(TARGET copy_pcl_dlls)
            add_dependencies(deploy_runtime_dlls copy_pcl_dlls)
        endif()
        if(TARGET copy_boost_dlls)
            add_dependencies(deploy_runtime_dlls copy_boost_dlls)
        endif()
        if(TARGET copy_geos_dlls)
            add_dependencies(deploy_runtime_dlls copy_geos_dlls)
        endif()
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 包含子项目
# -----------------------------------------------------------------------------
# 包含子项目（顺序：基础模块在前，业务层依赖它们）
# ComUI 集成层（仅提供适配源与 node.json，不建 target）——必须先于算法模块，
# 算法模块要用它导出的 COMUI_ADAPTER_* 变量
add_subdirectory(${CMAKE_SOURCE_DIR}/integration/comui comui_integration)

add_subdirectory(CommonAlg)
add_subdirectory(ShapelyAlg)
add_subdirectory(Image2DAlg)
add_subdirectory(Point3DAlg)
add_subdirectory(BusinessAlg)