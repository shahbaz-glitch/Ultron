
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

set(
    CMAKE_POSITION_INDEPENDENT_CODE
    ON
)

# ============================================================
# Ultron source and resource directories
#
# In the Android build, this CMakeLists.txt is staged into:
# app/jni/
#
# Therefore action/ and ui/ are staged alongside this file.
# ============================================================

set(
    ULTRON_ACTION_DIR
    "${CMAKE_CURRENT_LIST_DIR}/action"
)

set(
    ULTRON_UI_DIR
    "${CMAKE_CURRENT_LIST_DIR}/ui"
)

# Automatically discover every C++ source file inside action/.
# CONFIGURE_DEPENDS asks CMake to reconfigure when files are
# added or removed, including files in nested directories.

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

# Automatically discover UI files and nested resources.
# These are resources, not C++ compilation units.
#
# The workflow packages ui/ recursively into:
# app/src/main/assets/ui/

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
        ALIAS
        freetype
    )
endif()

# ============================================================
# RmlUi
# ============================================================

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_TRACY_PROFILING OFF CACHE BOOL "" FORCE)
set(RMLUI_COMPILER_OPTIONS OFF CACHE BOOL "" FORCE)

# Enable the built-in RmlUi SVG plugin.
# The LunaSVG dependency must also be available to CMake.

set(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)

# Help CMake locate LunaSVG and PlutoVG when their build-tree
# package configurations are available under RmlUi/Dependencies.
# If the workflow installs them elsewhere, its existing package
# search configuration can continue to be used.

set(
    ULTRON_LUNASVG_BUILD_DIR
    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Dependencies/lunasvg/build"
)

if(NOT lunasvg_ROOT AND EXISTS "${ULTRON_LUNASVG_BUILD_DIR}")
    set(
        lunasvg_ROOT
        "${ULTRON_LUNASVG_BUILD_DIR}"
        CACHE PATH
        "LunaSVG build directory used by RmlUi"
    )
endif()

if(NOT plutovg_ROOT AND EXISTS "${ULTRON_LUNASVG_BUILD_DIR}/plutovg")
    set(
        plutovg_ROOT
        "${ULTRON_LUNASVG_BUILD_DIR}/plutovg"
        CACHE PATH
        "PlutoVG build directory used by LunaSVG"
    )
endif()

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

# Automatically compile all discovered action .cpp files.
# No manual target_sources entry is needed for each new action.

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
message(STATUS "Debugger     : linked")
message(STATUS "Action files : ${ULTRON_ACTION_COUNT}")
message(STATUS "UI files     : ${ULTRON_UI_COUNT}")
message(STATUS "----------------------------------------")
