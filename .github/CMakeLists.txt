cmake_minimum_required(VERSION 3.16)

project(
    Ultron
    LANGUAGES C CXX
)

# ------------------------------------------------------------
# C++ configuration
# ------------------------------------------------------------

set(
    CMAKE_CXX_STANDARD
    20
    CACHE STRING
    ""
    FORCE
)

set(
    CMAKE_CXX_STANDARD_REQUIRED
    ON
    CACHE BOOL
    ""
    FORCE
)

set(
    CMAKE_CXX_EXTENSIONS
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    CMAKE_POSITION_INDEPENDENT_CODE
    ON
)

# ------------------------------------------------------------
# SDL3
# ------------------------------------------------------------

set(
    SDL_SHARED
    ON
    CACHE BOOL
    ""
    FORCE
)

set(
    SDL_STATIC
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    SDL_TESTS
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    SDL_EXAMPLES
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    SDL_INSTALL
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    SDL_GPU
    OFF
    CACHE BOOL
    ""
    FORCE
)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/SDL"
    SDL3
    EXCLUDE_FROM_ALL
)

# ------------------------------------------------------------
# FreeType
# ------------------------------------------------------------

set(
    BUILD_SHARED_LIBS
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    FT_DISABLE_ZLIB
    ON
    CACHE BOOL
    ""
    FORCE
)

set(
    FT_DISABLE_BZIP2
    ON
    CACHE BOOL
    ""
    FORCE
)

set(
    FT_DISABLE_PNG
    ON
    CACHE BOOL
    ""
    FORCE
)

set(
    FT_DISABLE_HARFBUZZ
    ON
    CACHE BOOL
    ""
    FORCE
)

set(
    FT_DISABLE_BROTLI
    ON
    CACHE BOOL
    ""
    FORCE
)

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

# ------------------------------------------------------------
# RmlUi
# ------------------------------------------------------------

set(
    BUILD_SHARED_LIBS
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    RMLUI_SAMPLES
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    RMLUI_FONT_ENGINE
    "freetype"
    CACHE STRING
    ""
    FORCE
)

set(
    RMLUI_PRECOMPILED_HEADERS
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    RMLUI_LUA_BINDINGS
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    RMLUI_TRACY_PROFILING
    OFF
    CACHE BOOL
    ""
    FORCE
)

set(
    RMLUI_COMPILER_OPTIONS
    OFF
    CACHE BOOL
    ""
    FORCE
)

add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/RmlUi"
    RmlUi
    EXCLUDE_FROM_ALL
)

# ------------------------------------------------------------
# Ultron native application
# ------------------------------------------------------------

add_library(
    main
    SHARED

    main.cpp

    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Backends/RmlUi_Platform_SDL.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/RmlUi/Backends/RmlUi_Renderer_GL3.cpp"
)

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
    SDL3::SDL3-shared

    GLESv3
    android
    log
    ${CMAKE_DL_LIBS}
)
