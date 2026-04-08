cmake_minimum_required(VERSION 3.15)
project(ToLissMCDU)

# C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# MSVC UTF-8 源文件编码
if (MSVC)
    add_compile_options(/utf-8)
endif()

# 平台标准输出名（X-Plane 要求）
if (WIN32)
    set(PLUGIN_OUTPUT_NAME "win.xpl")
elseif(APPLE)
    set(PLUGIN_OUTPUT_NAME "mac.xpl")
elseif(UNIX)
    set(PLUGIN_OUTPUT_NAME "lin.xpl")
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

# SDK 头文件路径
if(NOT DEFINED XPLM_SDK_PATH)
    set(XPLM_SDK_PATH "${CMAKE_SOURCE_DIR}/../X-plane-SDK" CACHE PATH "Path to X-Plane SDK")
endif()

# 包含目录
include_directories(${XPLM_SDK_PATH}/CHeaders/XPLM)
include_directories(${XPLM_SDK_PATH}/CHeaders/Widgets)
include_directories(${XPLM_SDK_PATH}/CHeaders/Wrappers)

set(BCDU_FONT_HEADER "${CMAKE_BINARY_DIR}/generated/bcdu_font.h")
set(BCDU_FONT_EMBED_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/embed_bcdu_font.cmake")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")

set(BCDU_FONT_DEPENDS "${BCDU_FONT_EMBED_SCRIPT}")
if(EXISTS "${CMAKE_SOURCE_DIR}/BCDU.otf")
    list(APPEND BCDU_FONT_DEPENDS "${CMAKE_SOURCE_DIR}/BCDU.otf")
    set(BCDU_FONT_INPUT "${CMAKE_SOURCE_DIR}/BCDU.otf")
else()
    set(BCDU_FONT_INPUT "")
endif()

add_custom_command(
    OUTPUT "${BCDU_FONT_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT="${BCDU_FONT_INPUT}"
        -DOUTPUT="${BCDU_FONT_HEADER}"
        -P "${BCDU_FONT_EMBED_SCRIPT}"
    DEPENDS ${BCDU_FONT_DEPENDS}
    COMMENT "Embedding BCDU font into generated header"
    VERBATIM
)

add_custom_target(generate_bcdu_font_header DEPENDS "${BCDU_FONT_HEADER}")

# 源文件
file(GLOB SRC_FILES "${CMAKE_SOURCE_DIR}/src/*.cpp")

# 生成共享库
add_library(${PROJECT_NAME} SHARED ${SRC_FILES})

# XPLM 版本定义
if(NOT DEFINED XPLM_VERSION)
    set(XPLM_VERSION "301" CACHE STRING "XPLM API version")
endif()

add_dependencies(${PROJECT_NAME} generate_bcdu_font_header)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_BINARY_DIR}/generated")

# 全局定义：避免 winsock.h / winsock2.h 冲突
if (WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE WIN32_LEAN_AND_MEAN)
endif()

# 平台定义和 XPLM 版本定义（累积）
if (WIN32)
    if(XPLM_VERSION STREQUAL "400" OR XPLM_VERSION STREQUAL "410")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            IBM=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM${XPLM_VERSION}=1
        )
    elseif(XPLM_VERSION STREQUAL "303")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            IBM=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1
        )
    elseif(XPLM_VERSION STREQUAL "301")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            IBM=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1
        )
    elseif(XPLM_VERSION STREQUAL "300")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            IBM=1
            XPLM200=1 XPLM210=1 XPLM300=1
        )
    endif()
elseif(APPLE)
    if(XPLM_VERSION STREQUAL "400" OR XPLM_VERSION STREQUAL "410")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            APL=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM${XPLM_VERSION}=1
        )
    elseif(XPLM_VERSION STREQUAL "303")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            APL=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1
        )
    elseif(XPLM_VERSION STREQUAL "301")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            APL=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1
        )
    elseif(XPLM_VERSION STREQUAL "300")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            APL=1
            XPLM200=1 XPLM210=1 XPLM300=1
        )
    endif()
elseif(UNIX)
    if(XPLM_VERSION STREQUAL "400" OR XPLM_VERSION STREQUAL "410")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            LIN=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1 XPLM${XPLM_VERSION}=1
        )
    elseif(XPLM_VERSION STREQUAL "303")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            LIN=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM303=1
        )
    elseif(XPLM_VERSION STREQUAL "301")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            LIN=1
            XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1
        )
    elseif(XPLM_VERSION STREQUAL "300")
        target_compile_definitions(${PROJECT_NAME} PRIVATE
            LIN=1
            XPLM200=1 XPLM210=1 XPLM300=1
        )
    endif()
endif()

# 链接库
if (WIN32)
    target_link_libraries(${PROJECT_NAME}
        "${XPLM_SDK_PATH}/Libraries/Win/XPLM_64.lib"
        ws2_32
    )
elseif(APPLE)
    target_link_libraries(${PROJECT_NAME}
        "-framework OpenGL"
    )
elseif(UNIX)
    target_link_libraries(${PROJECT_NAME}
        pthread
    )
endif()

# 设置输出文件名（不带 lib 前缀）
set_target_properties(${PROJECT_NAME} PROPERTIES
    PREFIX ""
    SUFFIX ""
    OUTPUT_NAME ${PLUGIN_OUTPUT_NAME}
)

# 输出目录
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/${CMAKE_BUILD_TYPE}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/${CMAKE_BUILD_TYPE}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist/Release"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist/Release"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/dist/Debug"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/dist/Debug"
)

# Windows 下去掉控制台窗口
if (WIN32)
    target_link_options(${PROJECT_NAME} PRIVATE "/SUBSYSTEM:WINDOWS")
endif()

# 自动复制到 X-Plane 插件目录（可选）
if(DEFINED XPLANE_PATH AND EXISTS "${XPLANE_PATH}")
    message(STATUS "X-Plane installation found at: ${XPLANE_PATH}")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${XPLANE_PATH}/Resources/plugins/${PROJECT_NAME}"
        COMMAND ${CMAKE_COMMAND} -E copy
            "$<TARGET_FILE:${PROJECT_NAME}>"
            "${XPLANE_PATH}/Resources/plugins/${PROJECT_NAME}/${PLUGIN_OUTPUT_NAME}"
        COMMENT "Copying plugin to X-Plane installation"
    )
else()
    message(STATUS "XPLANE_PATH not set or doesn't exist. Plugin will only be built to dist/ directory.")
endif()
