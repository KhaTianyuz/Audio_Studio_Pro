cmake_minimum_required(VERSION 3.20)
project(AudioStudioProCPP LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 兼容 CMake 3.24+ / 4.x 的 URL 解压时间戳策略
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

# 关闭 FetchContent 默认静默模式，实时在 VS 输出窗口打印下载与配置进度
set(FETCHCONTENT_QUIET OFF CACHE BOOL "Show FetchContent download progress" FORCE)

if(MSVC)
    # 通用 MSVC 编译选项：UTF-8 编码、警告等级 3、标准 C++ 异常处理
    add_compile_options(/utf-8 /W3 /EHsc)
    # 仅向 Release / RelWithDebInfo 编译标志追加 /O2 优化（不使用任何尖括号表达式，
    # 彻底避免复制粘贴时尖括号丢失报错，同时解决 Debug 模式下 /RTC1 与 /O2 冲突的 D8016 报错）
    string(APPEND CMAKE_CXX_FLAGS_RELEASE " /O2")
    string(APPEND CMAKE_C_FLAGS_RELEASE " /O2")
    string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " /O2")
    string(APPEND CMAKE_C_FLAGS_RELWITHDEBINFO " /O2")
endif()

include(FetchContent)

# ==============================================================================
# 多镜像高速依赖拉取与本地 third_party/ 持久化缓存机制（含损坏 HTML 自动校验修复）
# ==============================================================================

set(URL_PROTO "https")
set(CDN_JSDELIVR "${URL_PROTO}://cdn.jsdelivr.net/gh")
set(CDN_FASTLY   "${URL_PROTO}://fastly.jsdelivr.net/gh")
set(CDN_GCORE    "${URL_PROTO}://gcore.jsdelivr.net/gh")
set(RAW_GITHUB   "${URL_PROTO}://raw.githubusercontent.com")
set(WEB_GITHUB   "${URL_PROTO}://github.com")
set(PROXY_GHFAST "${URL_PROTO}://ghfast.top/${URL_PROTO}://")

# 校验源码/头文件是否有效（防止网关错误页或 HTML 搜索页污染本地缓存）
function(validate_source_file FILE_PATH OUT_VALID)
    set(${OUT_VALID} FALSE PARENT_SCOPE)
    if(NOT EXISTS "${FILE_PATH}")
        return()
    endif()
    file(SIZE "${FILE_PATH}" FILE_BYTES)
    if(FILE_BYTES LESS 256)
        return()
    endif()
    file(READ "${FILE_PATH}" HEAD_TEXT LIMIT 1024)
    string(TOLOWER "${HEAD_TEXT}" HEAD_LOWER)
    if(HEAD_LOWER MATCHES "doctype html" OR HEAD_LOWER MATCHES "<html" OR HEAD_LOWER MATCHES "google\.com/search" OR HEAD_LOWER MATCHES "404: not found")
        return()
    endif()
    set(${OUT_VALID} TRUE PARENT_SCOPE)
endfunction()

function(download_file_with_mirrors REL_DESC DST_PATH)
    validate_source_file("${DST_PATH}" IS_VALID_EXISTING)
    if(IS_VALID_EXISTING)
        return()
    elseif(EXISTS "${DST_PATH}")
        message(STATUS "[AudioStudioPro] 检测到无效缓存文件，正在清理并重新获取: ${REL_DESC}")
        file(REMOVE "${DST_PATH}")
    endif()

    get_filename_component(DST_DIR "${DST_PATH}" DIRECTORY)
    file(MAKE_DIRECTORY "${DST_DIR}")

    set(URL_LIST ${ARGN})
    foreach(CANDIDATE_URL IN LISTS URL_LIST)
        message(STATUS "[AudioStudioPro] 正在获取 ${REL_DESC} -> ${CANDIDATE_URL}")
        file(DOWNLOAD
            "${CANDIDATE_URL}"
            "${DST_PATH}.tmp"
            INACTIVITY_TIMEOUT 10
            TIMEOUT 25
            STATUS DL_STATUS
            TLS_VERIFY OFF
        )
        list(GET DL_STATUS 0 DL_CODE)
        if(DL_CODE EQUAL 0)
            validate_source_file("${DST_PATH}.tmp" IS_VALID_TMP)
            if(IS_VALID_TMP)
                file(SIZE "${DST_PATH}.tmp" TMP_SIZE)
                file(RENAME "${DST_PATH}.tmp" "${DST_PATH}")
                message(STATUS "[AudioStudioPro] 已缓存 ${REL_DESC} (${TMP_SIZE} bytes)")
                return()
            endif()
        endif()
        file(REMOVE "${DST_PATH}.tmp")
    endforeach()

    message(WARNING "[AudioStudioPro] 无法从镜像下载有效文件 ${REL_DESC}，将尝试备用压缩包")
endfunction()

# ------------------------------------------------------------------------------
# 1. Dear ImGui (v1.90.8)
# ------------------------------------------------------------------------------
set(IMGUI_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(IMGUI_REQUIRED_FILES
    "imgui.h"
    "imconfig.h"
    "imgui_internal.h"
    "imstb_rectpack.h"
    "imstb_textedit.h"
    "imstb_truetype.h"
    "imgui.cpp"
    "imgui_draw.cpp"
    "imgui_tables.cpp"
    "imgui_widgets.cpp"
    "backends/imgui_impl_win32.h"
    "backends/imgui_impl_win32.cpp"
    "backends/imgui_impl_dx11.h"
    "backends/imgui_impl_dx11.cpp"
)

foreach(IMGUI_FILE IN LISTS IMGUI_REQUIRED_FILES)
    download_file_with_mirrors(
        "imgui/${IMGUI_FILE}"
        "${IMGUI_LOCAL_DIR}/${IMGUI_FILE}"
        "${CDN_JSDELIVR}/ocornut/imgui@v1.90.8/${IMGUI_FILE}"
        "${CDN_FASTLY}/ocornut/imgui@v1.90.8/${IMGUI_FILE}"
        "${CDN_GCORE}/ocornut/imgui@v1.90.8/${IMGUI_FILE}"
        "${PROXY_GHFAST}raw.githubusercontent.com/ocornut/imgui/v1.90.8/${IMGUI_FILE}"
        "${RAW_GITHUB}/ocornut/imgui/v1.90.8/${IMGUI_FILE}"
    )
endforeach()

validate_source_file("${IMGUI_LOCAL_DIR}/imgui.h" IMGUI_H_OK)
validate_source_file("${IMGUI_LOCAL_DIR}/backends/imgui_impl_dx11.cpp" IMGUI_DX11_OK)
if(IMGUI_H_OK AND IMGUI_DX11_OK)
    set(imgui_SOURCE_DIR "${IMGUI_LOCAL_DIR}")
    message(STATUS "[AudioStudioPro] 使用本地缓存 Dear ImGui: ${imgui_SOURCE_DIR}")
else()
    message(STATUS "[AudioStudioPro] 正在通过 FetchContent 下载 Dear ImGui v1.90.8 压缩包...")
    FetchContent_Declare(
        imgui
        URL
            "${PROXY_GHFAST}github.com/ocornut/imgui/archive/refs/tags/v1.90.8.zip"
            "${WEB_GITHUB}/ocornut/imgui/archive/refs/tags/v1.90.8.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        INACTIVITY_TIMEOUT 15
        TIMEOUT 60
    )
    FetchContent_MakeAvailable(imgui)
endif()

# ------------------------------------------------------------------------------
# 2. ImPlot (v0.16)
# ------------------------------------------------------------------------------
set(IMPLOT_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot")
set(IMPLOT_REQUIRED_FILES
    "implot.h"
    "implot_internal.h"
    "implot.cpp"
    "implot_items.cpp"
)

foreach(IMPLOT_FILE IN LISTS IMPLOT_REQUIRED_FILES)
    download_file_with_mirrors(
        "implot/${IMPLOT_FILE}"
        "${IMPLOT_LOCAL_DIR}/${IMPLOT_FILE}"
        "${CDN_JSDELIVR}/epezent/implot@v0.16/${IMPLOT_FILE}"
        "${CDN_FASTLY}/epezent/implot@v0.16/${IMPLOT_FILE}"
        "${CDN_GCORE}/epezent/implot@v0.16/${IMPLOT_FILE}"
        "${PROXY_GHFAST}raw.githubusercontent.com/epezent/implot/v0.16/${IMPLOT_FILE}"
        "${RAW_GITHUB}/epezent/implot/v0.16/${IMPLOT_FILE}"
    )
endforeach()

validate_source_file("${IMPLOT_LOCAL_DIR}/implot.h" IMPLOT_H_OK)
validate_source_file("${IMPLOT_LOCAL_DIR}/implot_items.cpp" IMPLOT_CPP_OK)
if(IMPLOT_H_OK AND IMPLOT_CPP_OK)
    set(implot_SOURCE_DIR "${IMPLOT_LOCAL_DIR}")
    message(STATUS "[AudioStudioPro] 使用本地缓存 ImPlot: ${implot_SOURCE_DIR}")
else()
    message(STATUS "[AudioStudioPro] 正在通过 FetchContent 下载 ImPlot v0.16 压缩包...")
    FetchContent_Declare(
        implot
        URL
            "${PROXY_GHFAST}github.com/epezent/implot/archive/refs/tags/v0.16.zip"
            "${WEB_GITHUB}/epezent/implot/archive/refs/tags/v0.16.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        INACTIVITY_TIMEOUT 15
        TIMEOUT 60
    )
    FetchContent_MakeAvailable(implot)
endif()

# ------------------------------------------------------------------------------
# 3. miniaudio (0.11.21 单头文件音频库)
# ------------------------------------------------------------------------------
set(MINIAUDIO_LOCAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio")
download_file_with_mirrors(
    "miniaudio/miniaudio.h"
    "${MINIAUDIO_LOCAL_DIR}/miniaudio.h"
    "${CDN_JSDELIVR}/mackron/miniaudio@0.11.21/miniaudio.h"
    "${CDN_FASTLY}/mackron/miniaudio@0.11.21/miniaudio.h"
    "${CDN_GCORE}/mackron/miniaudio@0.11.21/miniaudio.h"
    "${PROXY_GHFAST}raw.githubusercontent.com/mackron/miniaudio/0.11.21/miniaudio.h"
    "${RAW_GITHUB}/mackron/miniaudio/0.11.21/miniaudio.h"
)

validate_source_file("${MINIAUDIO_LOCAL_DIR}/miniaudio.h" MINIAUDIO_H_OK)
if(MINIAUDIO_H_OK)
    set(miniaudio_SOURCE_DIR "${MINIAUDIO_LOCAL_DIR}")
    message(STATUS "[AudioStudioPro] 使用本地缓存 miniaudio: ${miniaudio_SOURCE_DIR}")
else()
    message(STATUS "[AudioStudioPro] 正在通过 FetchContent 下载 miniaudio 0.11.21 压缩包...")
    FetchContent_Declare(
        miniaudio
        URL
            "${PROXY_GHFAST}github.com/mackron/miniaudio/archive/refs/tags/0.11.21.zip"
            "${WEB_GITHUB}/mackron/miniaudio/archive/refs/tags/0.11.21.zip"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        INACTIVITY_TIMEOUT 15
        TIMEOUT 60
    )
    FetchContent_GetProperties(miniaudio)
    if(NOT miniaudio_POPULATED)
        FetchContent_Populate(miniaudio)
    endif()
endif()

# ==============================================================================
# 目标可执行程序与链接配置
# ==============================================================================

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)

set(APP_ICON_RC "")
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.rc" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.ico")
    set(APP_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.rc")
endif()

add_executable(AudioStudioPro WIN32
    audio_studio_pro.cpp
    ${APP_ICON_RC}
    ${IMGUI_SOURCES}
)

target_include_directories(AudioStudioPro PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${implot_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}
)

target_link_libraries(AudioStudioPro PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
    comdlg32
    shell32
    ole32
    winmm
)

# 自动将 DISCLAIMER.md 与 app_icon.ico 复制到编译输出目录（不使用尖括号表达式）
foreach(OUT_SUBDIR "." "Release" "Debug" "RelWithDebInfo" "MinSizeRel")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/DISCLAIMER.md")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/DISCLAIMER.md" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${OUT_SUBDIR}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../DISCLAIMER.md")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/../DISCLAIMER.md" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${OUT_SUBDIR}")
    endif()
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.ico")
        file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/app_icon.ico" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/${OUT_SUBDIR}")
    endif()
endforeach()

message(STATUS "[AudioStudioPro] CMake 项目配置完成！现在可以直接点击生成/启动 AudioStudioPro.exe。")
