cmake_minimum_required(VERSION 3.20)
project(rex-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ─── vcpkg ────────────────────────────────────────────────────────────────

if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "vcpkg toolchain not found at: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# ─── Dependencies ──────────────────────────────────────────────────────────

add_subdirectory(../rex-shared ${CMAKE_BINARY_DIR}/rex-shared)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# ─── Core Runtime Executable ───────────────────────────────────────────────

# ⚠ rex-core is an executable. Tests list sources directly instead
# of linking to it, so file lists are duplicated below.

set(WALLPAPER_SOURCES
    src/wallpaper/workerw/WorkerWManager.cpp
    src/wallpaper/platform/Windows.cpp
    src/wallpaper/engine/Engine.cpp
    src/wallpaper/engine/EngineFactory.cpp
    src/wallpaper/decoder/WICTextureLoader.cpp
    src/wallpaper/decoder/MediaFoundationDecoder.cpp
    src/wallpaper/playback/PlaybackController.cpp
    # Desktop hosting strategies (IDesktopHost + implementations)
    src/wallpaper/host/WorkerWHost.cpp
    src/wallpaper/host/ProgmanHost.cpp
    src/wallpaper/host/DesktopHostFactory.cpp
    # Phase 1: Static image wallpapers
    src/wallpaper/loader/ImageLoader.cpp
    src/wallpaper/scene/StaticImageScene.cpp
)

set(GRAPHICS_SOURCES
    src/graphics/compositor/compositor.cpp
    src/graphics/renderer/D3D12Renderer.cpp
    src/graphics/renderer/DescriptorHeapManager.cpp
    src/graphics/renderer/Device.cpp
    src/graphics/renderer/SwapChain.cpp
    src/graphics/renderer/CommandQueue.cpp
    src/graphics/animation/animation_engine.cpp
    src/graphics/effects/effect_renderer.cpp
    # Phase 1: GPU resource management
    src/graphics/resource/TextureCache.cpp
)

set(GRAPHICS_SOURCES
    src/graphics/compositor/compositor.cpp
    src/graphics/renderer/D3D12Renderer.cpp
    src/graphics/renderer/DescriptorHeapManager.cpp
    src/graphics/renderer/Device.cpp
    src/graphics/renderer/SwapChain.cpp
    src/graphics/renderer/CommandQueue.cpp
    src/graphics/animation/animation_engine.cpp
    src/graphics/effects/effect_renderer.cpp
)

set(CORE_SOURCES
    src/core/scheduler/render_scheduler.cpp
    src/core/sandbox/sandbox.cpp
)

set(IPC_SOURCES
    src/ipc/ipc_server.cpp
)

add_executable(rex-core
    src/main.cpp
    ${WALLPAPER_SOURCES}
    ${GRAPHICS_SOURCES}
    ${CORE_SOURCES}
    ${IPC_SOURCES}
)

set_target_properties(rex-core PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_include_directories(rex-core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rex-core
    PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        d3d12
        dxgi
        dxguid
        d3dcompiler
        windowscodecs
        mfplat
        mfreadwrite
        mfuuid
        runtimeobject
        dwmapi
        winmm
)

target_compile_definitions(rex-core
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# ─── Widget Host ───────────────────────────────────────────────────────────

add_executable(rex-widget-host
    src/widget/widget_host.cpp
)

set_target_properties(rex-widget-host PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_include_directories(rex-widget-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2-sdk
)

target_link_libraries(rex-widget-host PRIVATE
    dxgi
    dxguid
    d3d12
    windowscodecs
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2-sdk/WebView2Loader.dll.lib
)

add_custom_command(TARGET rex-widget-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/webview2-sdk/WebView2Loader.dll"
        "$<TARGET_FILE_DIR:rex-widget-host>/WebView2Loader.dll"
)

# ─── Tests ─────────────────────────────────────────────────────────────────

include(CTest)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)

    # Core component tests (compositor, IPC, sandbox, desktop host)
    add_executable(rex-core-tests
        tests/test_compositor.cpp
        tests/test_ipc.cpp
        tests/test_sandbox.cpp
        tests/test_desktop_host.cpp
        ${GRAPHICS_SOURCES}
        ${CORE_SOURCES}
        ${IPC_SOURCES}
        src/wallpaper/workerw/WorkerWManager.cpp
        src/wallpaper/platform/Windows.cpp
        src/wallpaper/decoder/WICTextureLoader.cpp
        # Desktop host strategies (needed for factory & host tests)
        src/wallpaper/host/WorkerWHost.cpp
        src/wallpaper/host/ProgmanHost.cpp
        src/wallpaper/host/DesktopHostFactory.cpp
        # Phase 1 modules (tested indirectly)
        src/wallpaper/loader/ImageLoader.cpp
        src/wallpaper/scene/StaticImageScene.cpp
    )

    target_include_directories(rex-core-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(rex-core-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        d3d12
        dxgi
        dxguid
        d3dcompiler
        windowscodecs
        mfplat
        mfreadwrite
        mfuuid
        runtimeobject
        dwmapi
        winmm
        GTest::gtest
        GTest::gtest_main
    )

    target_include_directories(rex-core-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(rex-core-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        d3d12
        dxgi
        dxguid
        d3dcompiler
        windowscodecs
        runtimeobject
        dwmapi
        winmm
        GTest::gtest
        GTest::gtest_main
    )

    include(GoogleTest)
    gtest_discover_tests(rex-core-tests)

    # Wallpaper engine integration tests
    add_executable(rex-wallpaper-tests
        tests/wallpaper/test_engine.cpp
        ${WALLPAPER_SOURCES}
        ${GRAPHICS_SOURCES}
        ${IPC_SOURCES}
        ${CORE_SOURCES}
    )

    target_include_directories(rex-wallpaper-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(rex-wallpaper-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        d3d12
        dxgi
        dxguid
        d3dcompiler
        windowscodecs
        mfplat
        mfreadwrite
        mfuuid
        runtimeobject
        dwmapi
        winmm
        GTest::gtest
        GTest::gtest_main
    )

    gtest_discover_tests(rex-wallpaper-tests)
endif()

# ─── Install ───────────────────────────────────────────────────────────────

install(TARGETS rex-core RUNTIME DESTINATION bin)
install(TARGETS rex-widget-host RUNTIME DESTINATION bin)

install(FILES
    include/rex/wallpaper/IWallpaperEngine.hpp
    DESTINATION include/rex/wallpaper
)
