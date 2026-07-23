cmake_minimum_required(VERSION 3.20)
project(rex-wallpaper-engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ─── Dependencies ──────────────────────────────────────────────────────────

add_subdirectory(../rex-shared ${CMAKE_BINARY_DIR}/rex-shared)

# ─── Public Headers ──────────────────────────────────────────────────────

set(PUBLIC_HEADERS
    include/wallpaper/IWallpaperEngine.hpp
)

# ─── Engine Library ──────────────────────────────────────────────────────

add_library(rex-wallpaper-engine STATIC
    internal/core/Engine.cpp
    internal/core/EngineFactory.cpp
    internal/platform/Windows.cpp
    internal/workerw/WorkerWManager.cpp
    internal/renderer/D3D12Renderer.cpp
    internal/renderer/DescriptorHeapManager.cpp
    internal/decoder/WICTextureLoader.cpp
    internal/decoder/MediaFoundationDecoder.cpp
    internal/playback/PlaybackController.cpp
)

target_include_directories(rex-wallpaper-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
)

target_link_libraries(rex-wallpaper-engine
    PUBLIC
        rex-shared
    PRIVATE
        d3d12
        dxgi
        d3dcompiler
        windowscodecs
        mfplat
        mfreadwrite
        mfuuid
        shcore
)

target_compile_definitions(rex-wallpaper-engine
    PUBLIC
        REX_WALLPAPER_ENGINE_BUILD
)

# ─── Tests ────────────────────────────────────────────────────────────────

enable_testing()
find_package(GTest CONFIG REQUIRED)

add_executable(rex-wallpaper-tests
    tests/test_engine.cpp
)

target_link_libraries(rex-wallpaper-tests
    PRIVATE
        rex-wallpaper-engine
        GTest::gtest
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(rex-wallpaper-tests)

# ─── Standalone App ──────────────────────────────────────────────────────

add_executable(rex-wallpaper-app
    app/main.cpp
)

target_link_libraries(rex-wallpaper-app
    PRIVATE
        rex-wallpaper-engine
)

if(WIN32)
    target_link_options(rex-wallpaper-app PRIVATE /SUBSYSTEM:WINDOWS)
endif()

# ─── Install ──────────────────────────────────────────────────────────────

install(TARGETS rex-wallpaper-engine
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)

install(FILES ${PUBLIC_HEADERS}
    DESTINATION include/wallpaper
)
