cmake_minimum_required(VERSION 3.20)
project(gpu-screen-recorder-windows
    VERSION 1.0.0
    DESCRIPTION "A ShadowPlay-like GPU screen recorder for Windows. The fastest screen recorder for Windows."
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Options
option(GSR_BUILD_CLI "Build the CLI recorder" ON)
option(GSR_BUILD_UI "Build the overlay UI" ON)
option(GSR_BUILD_NOTIFICATION "Build the notification overlay" ON)
option(GSR_ENABLE_NVENC "Enable NVIDIA NVENC support" ON)
option(GSR_ENABLE_AMF "Enable AMD AMF support" ON)
option(GSR_ENABLE_QSV "Enable Intel QuickSync support" ON)

# ── Dependencies ────────────────────────────────────────────────────────────
find_package(FFMPEG QUIET COMPONENTS avcodec avformat avutil swresample)
if(NOT FFMPEG_FOUND)
    message(STATUS "FFmpeg not found via find_package, using PkgConfig or vcpkg...")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(AVCODEC libavcodec REQUIRED IMPORTED_TARGET)
        pkg_check_modules(AVFORMAT libavformat REQUIRED IMPORTED_TARGET)
        pkg_check_modules(AVUTIL libavutil REQUIRED IMPORTED_TARGET)
        pkg_check_modules(SWRESAMPLE libswresample REQUIRED IMPORTED_TARGET)
    endif()
endif()

# Required Windows SDK libraries
set(WIN_LIBS
    d3d11
    dxgi
    d3dcompiler
    dxguid
    uuid
    ole32
    oleaut32
    avrt
    ws2_32
    user32
    gdi32
    shell32
    shlwapi
    comctl32
    imm32
    winmm
    version
)

# ── Source files ─────────────────────────────────────────────────────────────

set(COMMON_SOURCES
    src/common/log.cpp
    src/common/config.cpp
)

set(CAPTURE_SOURCES
    src/capture/screen_capture.cpp
    src/capture/cursor_capture.cpp
    src/capture/audio_capture.cpp
)

set(ENCODE_SOURCES
    src/encode/video_encoder.cpp
    src/encode/audio_encoder.cpp
    src/encode/muxer.cpp
)

set(REPLAY_SOURCES
    src/replay/replay_buffer.cpp
)

set(RECORDER_SOURCES
    src/recorder/recorder.cpp
    src/ui/notification.cpp
)

set(COMMON_HEADERS
    src/common/types.h
    src/common/log.h
    src/common/config.h
)

set(CAPTURE_HEADERS
    src/capture/screen_capture.h
    src/capture/cursor_capture.h
    src/capture/audio_capture.h
)

set(ENCODE_HEADERS
    src/encode/video_encoder.h
    src/encode/audio_encoder.h
    src/encode/muxer.h
)

set(REPLAY_HEADERS
    src/replay/replay_buffer.h
)

set(RECORDER_HEADERS
    src/recorder/recorder.h
)

# ── Core static library ──────────────────────────────────────────────────────

add_library(gsr_core STATIC
    ${COMMON_SOURCES}
    ${CAPTURE_SOURCES}
    ${ENCODE_SOURCES}
    ${REPLAY_SOURCES}
    ${RECORDER_SOURCES}
)

target_include_directories(gsr_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external
)

target_link_libraries(gsr_core PUBLIC
    ${WIN_LIBS}
)

if(FFMPEG_FOUND)
    target_include_directories(gsr_core PUBLIC ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(gsr_core PUBLIC ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(gsr_core PUBLIC ${FFMPEG_LIBRARIES})
else()
    if(AVCODEC_FOUND)
        target_link_libraries(gsr_core PUBLIC PkgConfig::AVCODEC)
    endif()
    if(AVFORMAT_FOUND)
        target_link_libraries(gsr_core PUBLIC PkgConfig::AVFORMAT)
    endif()
    if(AVUTIL_FOUND)
        target_link_libraries(gsr_core PUBLIC PkgConfig::AVUTIL)
    endif()
    if(SWRESAMPLE_FOUND)
        target_link_libraries(gsr_core PUBLIC PkgConfig::SWRESAMPLE)
    endif()
endif()

if(MSVC)
    target_compile_options(gsr_core PRIVATE /W4 /utf-8)
else()
    target_compile_options(gsr_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

target_compile_definitions(gsr_core PRIVATE
    $<$<BOOL:${GSR_ENABLE_NVENC}>:GSR_ENABLE_NVENC>
    $<$<BOOL:${GSR_ENABLE_AMF}>:GSR_ENABLE_AMF>
    $<$<BOOL:${GSR_ENABLE_QSV}>:GSR_ENABLE_QSV>
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# ── CLI tool ─────────────────────────────────────────────────────────────────

if(GSR_BUILD_CLI)
    add_executable(gpu-screen-recorder
        src/cli/main.cpp
    )

    target_link_libraries(gpu-screen-recorder PRIVATE
        gsr_core
        ${WIN_LIBS}
    )

    if(FFMPEG_FOUND)
        target_include_directories(gpu-screen-recorder PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(gpu-screen-recorder PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(gpu-screen-recorder PRIVATE ${FFMPEG_LIBRARIES})
    else()
        if(AVCODEC_FOUND)
            target_link_libraries(gpu-screen-recorder PRIVATE PkgConfig::AVCODEC)
        endif()
        if(AVFORMAT_FOUND)
            target_link_libraries(gpu-screen-recorder PRIVATE PkgConfig::AVFORMAT)
        endif()
        if(AVUTIL_FOUND)
            target_link_libraries(gpu-screen-recorder PRIVATE PkgConfig::AVUTIL)
        endif()
    endif()

    if(MSVC)
        target_compile_options(gpu-screen-recorder PRIVATE /W4 /utf-8)
    endif()

    install(TARGETS gpu-screen-recorder
        RUNTIME DESTINATION bin
    )
endif()

# ── Overlay UI ───────────────────────────────────────────────────────────────

if(GSR_BUILD_UI)
    # Download and build ImGui if not found
    include(FetchContent)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.9
    )
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_MakeAvailable(imgui)

    # Create a static library from ImGui sources needed for DX11
    set(IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
    )

    add_library(gsr_imgui STATIC ${IMGUI_SOURCES})
    target_include_directories(gsr_imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(gsr_imgui PUBLIC d3d11 dxgi d3dcompiler)

    add_executable(gsr-ui
        src/ui/main.cpp
        src/ui/overlay.cpp
        src/ui/ui_renderer.cpp
        src/ui/hotkeys.cpp
        src/ui/notification.cpp
    )

    set_target_properties(gsr-ui PROPERTIES
        WIN32_EXECUTABLE TRUE
    )

    target_include_directories(gsr-ui PRIVATE
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )

    target_link_libraries(gsr-ui PRIVATE
        gsr_core
        gsr_imgui
        ${WIN_LIBS}
    )

    if(FFMPEG_FOUND)
        target_include_directories(gsr-ui PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_directories(gsr-ui PRIVATE ${FFMPEG_LIBRARY_DIRS})
        target_link_libraries(gsr-ui PRIVATE ${FFMPEG_LIBRARIES})
    else()
        if(AVCODEC_FOUND)
            target_link_libraries(gsr-ui PRIVATE PkgConfig::AVCODEC)
        endif()
        if(AVFORMAT_FOUND)
            target_link_libraries(gsr-ui PRIVATE PkgConfig::AVFORMAT)
        endif()
    endif()

    if(MSVC)
        target_compile_options(gsr-ui PRIVATE /W4 /utf-8)
    endif()

    install(TARGETS gsr-ui
        RUNTIME DESTINATION bin
    )
endif()

# ── Notification tool ────────────────────────────────────────────────────────

if(GSR_BUILD_NOTIFICATION)
    add_executable(gsr-notification
        src/ui/notification_main.cpp
        src/ui/notification.cpp
    )

    target_include_directories(gsr-notification PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_link_libraries(gsr-notification PRIVATE
        gsr_core
        ${WIN_LIBS}
    )

    if(MSVC)
        target_compile_options(gsr-notification PRIVATE /W4 /utf-8)
    endif()

    install(TARGETS gsr-notification
        RUNTIME DESTINATION bin
    )
endif()

# ── Install headers (for plugin SDK) ─────────────────────────────────────────
install(DIRECTORY src/ DESTINATION include/gsr
    FILES_MATCHING PATTERN "*.h"
)

# ── Install license ───────────────────────────────────────────────────────────
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" DESTINATION .)

# ── CPack packaging ────────────────────────────────────────────────────────────
set(CPACK_PACKAGE_NAME "GPU Screen Recorder")
set(CPACK_PACKAGE_VENDOR "Sensei002")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A ShadowPlay-like GPU screen recorder for Windows")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "gpu-screen-recorder-windows-${PROJECT_VERSION}-win64")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")

# NSIS installer settings
set(CPACK_NSIS_DISPLAY_NAME "GPU Screen Recorder")
set(CPACK_NSIS_INSTALL_ROOT "C:\\Program Files")
set(CPACK_NSIS_MODIFY_PATH ON)
set(CPACK_NSIS_MENU_LINKS
    "bin/gsr-ui.exe" "GPU Screen Recorder"
    "bin/gpu-screen-recorder.exe" "GPU Screen Recorder (CLI)"
)

include(CPack)

# ── Custom target for downloading FFmpeg dev binaries on Windows ─────────────
if(WIN32)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/setup_deps.bat" [[
@echo off
echo Downloading and setting up FFmpeg development binaries for Windows...
echo.
REM Check if FFmpeg is already available
if exist "C:\ffmpeg\include" (
    echo FFmpeg already found at C:\ffmpeg
    goto :eof
)
echo Downloading FFmpeg 7.0.2 full build shared dev...
echo Please install FFmpeg via vcpkg or manually from https://github.com/BtbN/FFmpeg-Builds/releases
echo.
echo Recommended: Use vcpkg to install ffmpeg:
echo   vcpkg install ffmpeg[x264,x265,nvcodec,amf]:x64-windows
echo.
echo Or download from: https://github.com/BtbN/FFmpeg-Builds/releases
echo Extract to C:\ffmpeg and set FFMPEG_DIR environment variable.
echo.
echo Then reconfigure: cmake -B build -DFFMPEG_DIR=C:/ffmpeg
]])
endif()

# ── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS " GPU Screen Recorder for Windows")
message(STATUS "═══════════════════════════════════════════════════════════")
message(STATUS " Version: ${PROJECT_VERSION}")
message(STATUS " Build CLI: ${GSR_BUILD_CLI}")
message(STATUS " Build UI: ${GSR_BUILD_UI}")
message(STATUS " Build Notification: ${GSR_BUILD_NOTIFICATION}")
message(STATUS " NVENC: ${GSR_ENABLE_NVENC}")
message(STATUS " AMF: ${GSR_ENABLE_AMF}")
message(STATUS " QuickSync: ${GSR_ENABLE_QSV}")
message(STATUS "═══════════════════════════════════════════════════════════")
