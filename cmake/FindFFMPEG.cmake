# FindFFMPEG.cmake
# Locates pre-built FFmpeg development binaries (BtbN releases or manual install)
#
# This module searches for FFmpeg in the following locations:
#   1. FFMPEG_DIR cache variable (recommended for CI)
#   2. C:/ffmpeg (default Windows path)
#
# Usage:
#   set(FFMPEG_DIR "C:/path/to/ffmpeg" CACHE PATH "")
#   find_package(FFMPEG REQUIRED COMPONENTS avcodec avformat avutil swresample swscale)
#
# Sets:
#   FFMPEG_FOUND         - True if all required components found
#   FFMPEG_INCLUDE_DIRS  - Include directories for FFmpeg headers
#   FFMPEG_LIBRARY_DIRS  - Directory containing FFmpeg .lib files
#   FFMPEG_LIBRARIES     - List of all found FFmpeg libraries
#   FFMPEG_<component>_FOUND - Per-component flag
#   FFMPEG_<component>_INCLUDE_DIR - Per-component include dir
#   FFMPEG_<component>_LIBRARY    - Per-component library path

# ── Initial hint ─────────────────────────────────────────────────────────────
if(NOT FFMPEG_DIR)
    set(FFMPEG_DIR "C:/ffmpeg" CACHE PATH "Path to FFmpeg development binaries root (containing include/ and lib/)")
endif()

# ── Components ───────────────────────────────────────────────────────────────
set(_FFMPEG_KNOWN_COMPONENTS avcodec avdevice avfilter avformat avutil swresample swscale)

# Reverse the list so loop order matches component list but we can track
foreach(_component IN ITEMS avcodec avformat avutil swresample)
    list(APPEND _FFMPEG_REQUIRED_COMPONENTS ${_component})
endforeach()

# ── Find each component ─────────────────────────────────────────────────────
foreach(_component ${_FFMPEG_KNOWN_COMPONENTS})
    # Header
    find_path(FFMPEG_${_component}_INCLUDE_DIR
        NAMES
            lib${_component}/${_component}.h
            ${_component}/${_component}.h
        PATHS
            ${FFMPEG_DIR}
        PATH_SUFFIXES
            include
        NO_DEFAULT_PATH
    )

    # Library (import library for shared builds)
    find_library(FFMPEG_${_component}_LIBRARY
        NAMES
            ${_component}
            lib${_component}
        PATHS
            ${FFMPEG_DIR}
        PATH_SUFFIXES
            lib
            lib/x64
        NO_DEFAULT_PATH
    )

    if(FFMPEG_${_component}_INCLUDE_DIR AND FFMPEG_${_component}_LIBRARY)
        set(FFMPEG_${_component}_FOUND TRUE)
        list(APPEND _FFMPEG_INCLUDE_DIRS ${FFMPEG_${_component}_INCLUDE_DIR})
        list(APPEND _FFMPEG_LIBRARIES ${FFMPEG_${_component}_LIBRARY})
    else()
        set(FFMPEG_${_component}_FOUND FALSE)
    endif()
endforeach()

# ── Deduplicate include dirs ─────────────────────────────────────────────────
if(_FFMPEG_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES _FFMPEG_INCLUDE_DIRS)
endif()

# ── Set output variables ────────────────────────────────────────────────────
set(FFMPEG_INCLUDE_DIRS ${_FFMPEG_INCLUDE_DIRS})
set(FFMPEG_LIBRARIES ${_FFMPEG_LIBRARIES})

if(FFMPEG_INCLUDE_DIRS AND FFMPEG_LIBRARIES)
    # Pick the first library's directory as the library dir
    list(GET FFMPEG_LIBRARIES 0 _FFMPEG_FIRST_LIB)
    get_filename_component(FFMPEG_LIBRARY_DIRS "${_FFMPEG_FIRST_LIB}" DIRECTORY)
endif()

# ── Check required components ───────────────────────────────────────────────
set(FFMPEG_FOUND TRUE)
foreach(_component IN LISTS _FFMPEG_REQUIRED_COMPONENTS)
    if(NOT FFMPEG_${_component}_FOUND)
        set(FFMPEG_FOUND FALSE)
        if(FFMPEG_FIND_REQUIRED)
            message(WARNING "FFmpeg component '${_component}' not found. "
                    "Set FFMPEG_DIR to the root of the FFmpeg dev binaries "
                    "(containing include/ and lib/).")
        endif()
    endif()
endforeach()

# ── Report ───────────────────────────────────────────────────────────────────
if(FFMPEG_FOUND)
    message(STATUS "FFmpeg found via FindFFMPEG.cmake")
    message(STATUS "  Include dirs: ${FFMPEG_INCLUDE_DIRS}")
    message(STATUS "  Library dirs: ${FFMPEG_LIBRARY_DIRS}")
    foreach(_component IN LISTS _FFMPEG_KNOWN_COMPONENTS)
        if(FFMPEG_${_component}_FOUND)
            message(STATUS "    ${_component}: ${FFMPEG_${_component}_LIBRARY}")
        endif()
    endforeach()
else()
    if(FFMPEG_FIND_REQUIRED)
        message(FATAL_ERROR "FFmpeg not found. Set -DFFMPEG_DIR=<path> "
                "pointing to FFmpeg dev binaries (include/ and lib/ folders).\n"
                "Download from: https://github.com/BtbN/FFmpeg-Builds/releases\n"
                "Example: ffmpeg-master-latest-win64-gpl-shared.zip")
    endif()
endif()

# ── Cleanup internal variables ──────────────────────────────────────────────
mark_as_advanced(FFMPEG_DIR)
foreach(_component ${_FFMPEG_KNOWN_COMPONENTS})
    mark_as_advanced(FFMPEG_${_component}_INCLUDE_DIR FFMPEG_${_component}_LIBRARY)
endforeach()
