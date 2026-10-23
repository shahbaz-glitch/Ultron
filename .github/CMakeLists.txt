cmake_minimum_required(VERSION 3.16)

project(
    Ultron
    LANGUAGES C CXX
)

# ============================================================
# C++20
# ============================================================

set(CMAKE_CXX_STANDARD 20 CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_REQUIRED ON CACHE BOOL "" FORCE)
set(CMAKE_CXX_EXTENSIONS OFF CACHE BOOL "" FORCE)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ============================================================
# Ultron source and resource directories
# ============================================================

set(
    ULTRON_ACTION_DIR
    "${CMAKE_CURRENT_LIST_DIR}/action"
)

set(
    ULTRON_UI_DIR
    "${CMAKE_CURRENT_LIST_DIR}/ui"
)

# Automatically discover C++ action files.

set(ULTRON_ACTION_SOURCES "")

if(EXISTS "${ULTRON_ACTION_DIR}")
    file(
        GLOB_RECURSE ULTRON_ACTION_SOURCES
        CONFIGURE_DEPENDS
        "${ULTRON_ACTION_DIR}/*.cpp"
    )
endif()

list(SORT ULTRON_ACTION_SOURCES)

list(
    LENGTH
    ULTRON_ACTION_SOURCES
    ULTRON_ACTION_COUNT
)

message(
    STATUS
    "Ultron: discovered ${ULTRON_ACTION_COUNT} action source file(s)"
)

# Automatically discover UI resources.

set(ULTRON_UI_FILES "")

if(EXISTS "${ULTRON_UI_DIR}")
    file(
        GLOB_RECURSE ULTRON_UI_FILES
        CONFIGURE_DEPENDS
        LIST_DIRECTORIES false
        "${ULTRON_UI_DIR}/*"
    )
endif()

list(SORT ULTRON_UI_FILES)

list(
    LENGTH
    ULTRON_UI_FILES
    ULTRON_UI_COUNT
)

message(
    STATUS
    "Ultron: discovered ${ULTRON_UI_COUNT} UI/resource file(s)"
)

# ============================================================
# SDL3
# ============================================================

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/SDL"
    SDL3
    EXCLUDE_FROM_ALL
)

# ============================================================
# FreeType
# ============================================================

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/freetype"
    freetype
    EXCLUDE_FROM_ALL
)

if(NOT TARGET Freetype::Freetype)
    add_library(
        Freetype::Freetype
        ALIAS freetype
    )
endif()

# ============================================================
# RmlUi configuration
# ============================================================

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_TRACY_PROFILING OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)

# Enable SVG support; use LunaSVG as its rendering dependency.
set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)

# ============================================================
# FIX: Find the Android LunaSVG and PlutoVG build packages
# BEFORE adding RmlUi.
#
# The workflow builds LunaSVG in:
# RmlUi/Dependencies/lunasvg/build
#
# Expected package files:
#   build/lunasvgConfig.cmake
#   build/plutovg/plutovgConfig.cmake
# ============================================================

set(
    ULTRON_LUNASVG_BUILD_DIR
    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Dependencies/lunasvg/build"
)

set(
    ULTRON_PLUTOVG_BUILD_DIR
    "${ULTRON_LUNASVG_BUILD_DIR}/plutovg"
)

# Fail with a clear message if the dependency build step did
# not generate the expected package configuration files.

if(NOT EXISTS "${ULTRON_LUNASVG_BUILD_DIR}/lunasvgConfig.cmake")
    message(FATAL_ERROR
        "LunaSVG CMake package is missing: "
        "${ULTRON_LUNASVG_BUILD_DIR}/lunasvgConfig.cmake. "
        "Build LunaSVG before configuring Ultron."
    )
endif()

if(NOT EXISTS "${ULTRON_PLUTOVG_BUILD_DIR}/plutovgConfig.cmake")
    message(FATAL_ERROR
        "PlutoVG CMake package is missing: "
        "${ULTRON_PLUTOVG_BUILD_DIR}/plutovgConfig.cmake. "
        "Check the LunaSVG/PlutoVG Android build step."
    )
endif()

# Point CMake directly at the generated package directories.
# FORCE prevents stale paths in the Gradle CMake cache.

set(
    plutovg_DIR
    "${ULTRON_PLUTOVG_BUILD_DIR}"
    CACHE PATH "PlutoVG build-tree package directory" FORCE
)

set(
    lunasvg_DIR
    "${ULTRON_LUNASVG_BUILD_DIR}"
    CACHE PATH "LunaSVG build-tree package directory" FORCE
)

# Load PlutoVG first because LunaSVG's package config imports it.
find_package(plutovg CONFIG REQUIRED)
find_package(lunasvg CONFIG REQUIRED)

# Ensure the required imported targets really exist.
if(NOT TARGET plutovg::plutovg)
    message(FATAL_ERROR
        "PlutoVG was found, but target plutovg::plutovg is missing."
    )
endif()

if(NOT TARGET lunasvg::lunasvg)
    message(FATAL_ERROR
        "LunaSVG was found, but target lunasvg::lunasvg is missing."
    )
endif()

message(STATUS "Ultron: PlutoVG package found")
message(STATUS "Ultron: LunaSVG package found")

# ============================================================
# RmlUi
# ============================================================

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/RmlUi"
    RmlUi
    EXCLUDE_FROM_ALL
)

# ============================================================
# Ultron native application
# ============================================================

add_library(
    main
    SHARED

    main.cpp

    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Backends/RmlUi_Platform_SDL.cpp"

    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Backends/RmlUi_Renderer_GL3.cpp"
)

# Automatically compile discovered action sources.

if(ULTRON_ACTION_SOURCES)
    target_sources(
        main
        PRIVATE
        ${ULTRON_ACTION_SOURCES}
    )
endif()

target_include_directories(
    main
    PRIVATE

    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Backends"
)

target_compile_definitions(
    main
    PRIVATE

    RMLUI_SDL_VERSION_MAJOR=3
)

target_link_libraries(
    main
    PRIVATE

    RmlUi::RmlUi
    RmlUi::Debugger
    SDL3::SDL3-shared

    GLESv3
    android
    log
    ${CMAKE_DL_LIBS}
)

# ============================================================
# Build summary
# ============================================================

message(STATUS "----------------------------------------")
message(STATUS "Ultron configuration")
message(STATUS "C++ standard : C++20")
message(STATUS "SDL          : SDL3")
message(STATUS "UI engine    : RmlUi")
message(STATUS "Renderer     : OpenGL ES 3.2")
message(STATUS "SVG plugin   : ON")
message(STATUS "LunaSVG      : ${lunasvg_DIR}")
message(STATUS "PlutoVG      : ${plutovg_DIR}")
message(STATUS "Debugger     : linked")
message(STATUS "Lua bindings : OFF")
message(STATUS "Lottie       : OFF")
message(STATUS "Action files : ${ULTRON_ACTION_COUNT}")
message(STATUS "UI files     : ${ULTRON_UI_COUNT}")
message(STATUS "----------------------------------------")
