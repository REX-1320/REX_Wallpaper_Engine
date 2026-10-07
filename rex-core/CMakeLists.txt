cmake_minimum_required(VERSION 3.20)
project(rex-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)

# ─── vcpkg ────────────────────────────────────────────────────────────────

if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR "vcpkg toolchain not found at: ${CMAKE_TOOLCHAIN_FILE}")
endif()

# ─── Dependencies ──────────────────────────────────────────────────────────

add_subdirectory(../rex-shared ${CMAKE_BINARY_DIR}/rex-shared)

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# FFmpeg (GPU-native video decode: D3D11VA primary, software fallback).
# vcpkg's ffmpeg port ships a find-module (FindFFMPEG.cmake) that sets
# FFMPEG_LIBRARIES / FFMPEG_INCLUDE_DIRS (no imported targets).
find_package(FFMPEG REQUIRED)

option(REX_ENABLE_MARKETPLACE "Enable marketplace client (requires curl)" OFF)
option(REX_BUILD_SAMPLE_PLUGINS "Build sample plugins" OFF)
option(REX_ENABLE_SIGNATURES "Enable OpenSSL signature verification" OFF)
if(REX_ENABLE_MARKETPLACE)
    find_package(CURL CONFIG REQUIRED)
endif()
if(REX_ENABLE_SIGNATURES)
    find_package(OpenSSL REQUIRED)
endif()

# ─── Core Runtime Executable ───────────────────────────────────────────────

# ⚠ rex-core is an executable. Tests list sources directly instead
# of linking to it, so file lists are duplicated below.

set(PLATFORM_WINDOWS_SOURCES
    src/platform/windows/WorkerWManager.cpp
    src/platform/windows/DesktopWindowThread.cpp
    src/platform/windows/Windows.cpp
    src/platform/windows/WorkerWHost.cpp
    src/platform/windows/ProgmanHost.cpp
    src/platform/windows/DesktopHostFactory.cpp
    src/platform/windows/MultiMonitorDesktopHost.cpp
    src/platform/windows/MonitorEdid.cpp
)

set(WALLPAPER_SOURCES
    src/wallpaper/engine/Engine.cpp
    src/wallpaper/engine/EngineFactory.cpp
    src/wallpaper/decoder/WICTextureLoader.cpp
    src/wallpaper/decoder/FfmpegVideoDecoder.cpp
    src/wallpaper/playback/FrameScheduler.cpp
    src/wallpaper/playback/VideoPlaybackController.cpp
    src/wallpaper/audio/AudioPlayer.cpp
    src/wallpaper/audio/AudioCapture.cpp
    src/wallpaper/video/VideoManager.cpp
    src/wallpaper/monitor/MonitorCompositor.cpp
    src/wallpaper/loader/ImageLoader.cpp
    src/wallpaper/scene/StaticImageScene.cpp
    src/wallpaper/runtime/RendererAdapters.cpp
    src/wallpaper/runtime/RuntimeSettings.cpp
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
    src/graphics/effects/NoiseTexture.cpp
    # Phase 1: GPU resource management
    src/graphics/resource/TextureCache.cpp
)

set(CORE_SOURCES
    src/core/scheduler/render_scheduler.cpp
    src/core/sandbox/sandbox.cpp
)

set(IPC_SOURCES
    src/ipc/ipc_server.cpp
)

set(MODULE_SOURCES
    src/modules/ModuleState.cpp
    src/modules/ModuleRegistry.cpp
    src/modules/DependencyResolver.cpp
    src/modules/ModuleLoader.cpp
    src/modules/ModuleManager.cpp
    src/modules/ModuleContext.cpp
    src/modules/LifecycleCoordinator.cpp
    src/modules/ModuleOrchestrator.cpp
    src/modules/HealthMonitor.cpp
    src/modules/ConfigurationStore.cpp
    src/modules/PluginSandbox.cpp
    src/modules/SignatureVerifier.cpp
    src/modules/UpdateManager.cpp
    src/modules/builtin/AboutModule/AboutModule.cpp
)

if(REX_ENABLE_MARKETPLACE)
    list(APPEND MODULE_SOURCES src/modules/CurlMarketplaceClient.cpp)
endif()

set(COMMAND_SOURCES
    src/modules/Command/CommandModule.cpp
    src/modules/Command/services/CommandRegistry.cpp
    src/modules/Command/services/CommandValidator.cpp
    src/modules/Command/services/CommandExecutor.cpp
    src/modules/Command/services/CommandDispatcher.cpp
    src/modules/Command/services/CommandService.cpp
    src/modules/Command/history/CommandHistory.cpp
    src/modules/Command/shortcut/ShortcutManager.cpp
)

set(SEARCH_SOURCES
    src/modules/Search/SearchModule.cpp
    src/modules/Search/services/ProviderRegistry.cpp
    src/modules/Search/services/QueryEngine.cpp
    src/modules/Search/services/RankingEngine.cpp
    src/modules/Search/services/SuggestionService.cpp
    src/modules/Search/services/SearchService.cpp
    src/modules/Search/services/MetricsCollector.cpp
    src/modules/Search/history/SearchHistory.cpp
    src/modules/Search/index/IndexCoordinator.cpp
    src/modules/Search/semantic/SemanticIndex.cpp
)

set(PROFILES_SOURCES
    src/profiles/services/ProfileManager.cpp
)

set(NOTIFICATION_SOURCES
    src/modules/Notification/NotificationModule.cpp
    src/modules/Notification/services/NotificationService.cpp
    src/modules/Notification/services/NotificationManager.cpp
    src/modules/Notification/services/NotificationQueue.cpp
    src/modules/Notification/history/NotificationHistory.cpp
    src/modules/Notification/renderer/NotificationRenderer.cpp
    src/modules/Notification/animation/NotificationAnimation.cpp
    src/modules/Notification/sound/NotificationSound.cpp
)

set(UI_SOURCES
)

set(SETTINGS_SOURCES
    src/modules/Settings/SettingsModule.cpp
    src/modules/Settings/registry/SettingsRegistry.cpp
    src/modules/Settings/services/SettingsService.cpp
    src/modules/Settings/services/SettingsTransaction.cpp
    src/modules/Settings/validation/ValidationEngine.cpp
    src/modules/Settings/validation/Validators.cpp
    src/modules/Settings/search/SettingsSearch.cpp
    src/modules/Settings/history/SettingsHistory.cpp
    src/modules/Settings/backup/BackupManager.cpp
    src/modules/Settings/schema/SchemaManager.cpp
    src/modules/Settings/io/ImportExportManager.cpp
    src/modules/Settings/ui/UIModelGenerator.cpp
)

set(DESKTOP_PERSONALIZATION_SOURCES
    src/desktop_personalization/DesktopPersonalizationModule.cpp
    src/desktop_personalization/platform/Win32PlatformAdapter.cpp
    src/desktop_personalization/services/PersonalizationService.cpp
    src/desktop_personalization/services/ThemeManager.cpp
    src/desktop_personalization/services/AppearanceManager.cpp
    src/desktop_personalization/services/EffectsManager.cpp
    src/desktop_personalization/services/DesktopManager.cpp
    src/desktop_personalization/services/ExplorerManager.cpp
    src/desktop_personalization/services/TaskbarManager.cpp
    src/desktop_personalization/services/ProfileManager.cpp
    src/desktop_personalization/services/SettingsManager.cpp
)

set(WIDGET_SOURCES
    src/modules/Widgets/WidgetModule.cpp
    src/modules/Widgets/services/WidgetRegistry.cpp
    src/modules/Widgets/services/WidgetHost.cpp
    src/modules/Widgets/services/WidgetService.cpp
    src/modules/Widgets/services/WidgetRuntime.cpp
)

set(PERSONALIZATION_SOURCES
    src/modules/Personalization/PersonalizationModule.cpp
    src/modules/Personalization/services/PersonalizationService.cpp
    src/modules/Personalization/platform/WinTaskbarAdapter.cpp
    src/modules/Personalization/platform/WinExplorerAdapter.cpp
    src/modules/Personalization/platform/WinStartMenuAdapter.cpp
)

set(SYNC_SOURCES
    src/modules/Sync/crypto/CryptoService.cpp
    src/modules/Sync/storage/SyncStore.cpp
    src/modules/Sync/transport/SyncRestClient.cpp
    src/modules/Sync/engine/SyncEngine.cpp
)

add_executable(rex-core
    src/main.cpp
    ${PLATFORM_WINDOWS_SOURCES}
    ${WALLPAPER_SOURCES}
    ${GRAPHICS_SOURCES}
    ${CORE_SOURCES}
    ${IPC_SOURCES}
    ${MODULE_SOURCES}
    ${DESKTOP_PERSONALIZATION_SOURCES}
    ${PROFILES_SOURCES}
    ${SETTINGS_SOURCES}
    ${NOTIFICATION_SOURCES}
    ${COMMAND_SOURCES}
    ${SEARCH_SOURCES}
    ${WIDGET_SOURCES}
    ${PERSONALIZATION_SOURCES}
    ${SYNC_SOURCES}
)

set_target_properties(rex-core PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_include_directories(rex-core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(rex-core
    PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        d2d1
        dwrite
        d3d11
        dcomp
        d3d12

        dxgi
        dxguid
        d3dcompiler
        windowscodecs
        runtimeobject
        dwmapi
        winmm
        shcore
        mmdevapi
        ole32
        uuid
        crypt32
        ${FFMPEG_LIBRARIES}
)

if(REX_ENABLE_MARKETPLACE)
    target_link_libraries(rex-core PRIVATE CURL::libcurl)
endif()

if(REX_ENABLE_SIGNATURES)
    target_compile_definitions(rex-core PRIVATE REX_USE_OPENSSL)
    target_link_libraries(rex-core PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()

target_compile_options(rex-core PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Z7>)

target_compile_definitions(rex-core
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# ─── Sample Plugins ──────────────────────────────────────────────────────

if(REX_BUILD_SAMPLE_PLUGINS)
    add_subdirectory(plugins/wallpaper)
endif()

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
        ${PLATFORM_WINDOWS_SOURCES}
        ${GRAPHICS_SOURCES}
        ${CORE_SOURCES}
        ${IPC_SOURCES}
        ${MODULE_SOURCES}
        src/wallpaper/decoder/WICTextureLoader.cpp
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
    gtest_add_tests(TARGET rex-core-tests)

    # Wallpaper engine integration tests
    add_executable(rex-wallpaper-tests
        tests/wallpaper/test_engine.cpp
        tests/wallpaper/test_video_pipeline.cpp
        tests/wallpaper/test_multi_monitor.cpp
        tests/wallpaper/test_stress.cpp
        tests/wallpaper/test_resource_leak.cpp
        tests/wallpaper/test_soak.cpp
        tests/wallpaper/test_frame_scheduler.cpp
        tests/wallpaper/test_descriptor_heap.cpp
        tests/wallpaper/test_json_parser.cpp
        tests/wallpaper/test_runtime_settings.cpp
        tests/wallpaper/test_resource_state_tracker.cpp
        tests/wallpaper/test_client_surface.cpp
        tests/wallpaper/test_display_topology.cpp
        ${PLATFORM_WINDOWS_SOURCES}
        ${WALLPAPER_SOURCES}
        ${GRAPHICS_SOURCES}
        ${IPC_SOURCES}
        ${CORE_SOURCES}
        ${MODULE_SOURCES}
    )

    target_include_directories(rex-wallpaper-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${FFMPEG_INCLUDE_DIRS}
    )

    target_link_libraries(rex-wallpaper-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        d3d11
        d3d12

        dxgi
        dxguid
        d3dcompiler
        windowscodecs
        runtimeobject
        dwmapi
        winmm
        shcore
        ${FFMPEG_LIBRARIES}
        GTest::gtest
        GTest::gtest_main
        psapi
    )

    gtest_add_tests(TARGET rex-wallpaper-tests)

    # Module system tests
    add_executable(rex-module-tests
        tests/test_dependency_resolver.cpp
        tests/test_lifecycle.cpp
        ${MODULE_SOURCES}
    )

    target_include_directories(rex-module-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(rex-module-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        GTest::gtest
        GTest::gtest_main
    )

    gtest_discover_tests(rex-module-tests)

    # Desktop Personalization module tests (uses mock platform adapter, no Win32 DLL dependency)
    set(DESKTOP_PERSONALIZATION_TEST_SOURCES
        tests/desktop_personalization/test_personalization_service.cpp
        src/desktop_personalization/services/PersonalizationService.cpp
        src/desktop_personalization/services/ThemeManager.cpp
        src/desktop_personalization/services/AppearanceManager.cpp
        src/desktop_personalization/services/EffectsManager.cpp
        src/desktop_personalization/services/DesktopManager.cpp
        src/desktop_personalization/services/ExplorerManager.cpp
        src/desktop_personalization/services/TaskbarManager.cpp
        src/desktop_personalization/services/ProfileManager.cpp
        src/desktop_personalization/services/SettingsManager.cpp
        src/modules/ConfigurationStore.cpp
    )

    add_executable(rex-desktop-personalization-tests
        ${DESKTOP_PERSONALIZATION_TEST_SOURCES}
    )

    target_include_directories(rex-desktop-personalization-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(rex-desktop-personalization-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
        GTest::gtest
        GTest::gtest_main
    )

    gtest_add_tests(TARGET rex-desktop-personalization-tests)

    # Settings module tests
    set(SETTINGS_TEST_SOURCES
        tests/all_settings_tests.cpp
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest_main.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-assertion-result.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-death-test.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-filepath.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-matchers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-port.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-printers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-test-part.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-typed-test.cc
        src/modules/Settings/SettingsModule.cpp
        src/modules/Settings/registry/SettingsRegistry.cpp
        src/modules/Settings/services/SettingsService.cpp
        src/modules/Settings/services/SettingsTransaction.cpp
        src/modules/Settings/validation/ValidationEngine.cpp
        src/modules/Settings/validation/Validators.cpp
        src/modules/Settings/search/SettingsSearch.cpp
        src/modules/Settings/history/SettingsHistory.cpp
        src/modules/Settings/backup/BackupManager.cpp
        src/modules/Settings/schema/SchemaManager.cpp
        src/modules/Settings/io/ImportExportManager.cpp
        src/modules/Settings/ui/UIModelGenerator.cpp

        src/modules/ConfigurationStore.cpp
    )

    add_executable(rex-settings-tests
        ${SETTINGS_TEST_SOURCES}
    )

    target_include_directories(rex-settings-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include
    )



    target_link_libraries(rex-settings-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
    )

    gtest_add_tests(TARGET rex-settings-tests)

    # Notification module tests
    set(NOTIFICATION_TEST_SOURCES
        tests/notification/test_notification_phase0.cpp
        tests/notification/test_notification_phase1.cpp
        tests/notification/test_notification_phase2.cpp
        src/modules/Notification/services/NotificationQueue.cpp
        src/modules/Notification/services/NotificationManager.cpp
        src/modules/Notification/services/NotificationService.cpp
        src/modules/Notification/history/NotificationHistory.cpp
        src/modules/Notification/renderer/NotificationRenderer.cpp
        src/modules/Notification/animation/NotificationAnimation.cpp
        src/modules/Notification/sound/NotificationSound.cpp
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest_main.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-assertion-result.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-death-test.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-filepath.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-matchers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-port.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-printers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-test-part.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-typed-test.cc
    )

    add_executable(rex-notification-tests
        ${NOTIFICATION_TEST_SOURCES}
    )

    target_include_directories(rex-notification-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include
    )

    target_link_libraries(rex-notification-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
    )

    gtest_discover_tests(rex-notification-tests)

    # Command module tests
    set(COMMAND_TEST_SOURCES
        tests/command/test_command_phase0.cpp
        tests/command/test_command_phase1.cpp
        src/modules/Command/services/CommandRegistry.cpp
        src/modules/Command/services/CommandValidator.cpp
        src/modules/Command/services/CommandExecutor.cpp
        src/modules/Command/services/CommandDispatcher.cpp
        src/modules/Command/services/CommandService.cpp
        src/modules/Command/history/CommandHistory.cpp
        src/modules/Command/shortcut/ShortcutManager.cpp
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest_main.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-assertion-result.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-death-test.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-filepath.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-matchers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-port.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-printers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-test-part.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-typed-test.cc
    )

    add_executable(rex-command-tests
        ${COMMAND_TEST_SOURCES}
    )

    target_include_directories(rex-command-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include
    )

    target_link_libraries(rex-command-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
    )

    gtest_discover_tests(rex-command-tests)

    # Search module tests
    set(SEARCH_TEST_SOURCES
        tests/search/test_search.cpp
        src/modules/Search/services/ProviderRegistry.cpp
        src/modules/Search/services/QueryEngine.cpp
        src/modules/Search/services/RankingEngine.cpp
        src/modules/Search/services/SuggestionService.cpp
        src/modules/Search/services/SearchService.cpp
        src/modules/Search/services/MetricsCollector.cpp
        src/modules/Search/history/SearchHistory.cpp
        src/modules/Search/index/IndexCoordinator.cpp
        src/modules/Search/semantic/SemanticIndex.cpp
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest_main.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-assertion-result.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-death-test.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-filepath.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-matchers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-port.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-printers.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-test-part.cc
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/src/gtest-typed-test.cc
    )

    add_executable(rex-search-tests
        ${SEARCH_TEST_SOURCES}
    )

    target_include_directories(rex-search-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows
        ${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/include
    )

    target_link_libraries(rex-search-tests PRIVATE
        rex-shared
        fmt::fmt
        spdlog::spdlog
    )

    gtest_discover_tests(rex-search-tests)
endif()

# ─── Wallpaper Runtime / CLI ────────────────────────────────────────────────
#
# rex-wallpaper-runtime: persistent native runtime (stdin/stdout JSON frames)
# plus the `--benchmark` mode (FFmpeg D3D11VA -> D3D12 interop performance
# harness). Later receives IPC commands from any REX UI.

add_executable(rex-wallpaper-runtime
    src/wallpaper_cli_main.cpp
    ${PLATFORM_WINDOWS_SOURCES}
    ${WALLPAPER_SOURCES}
    ${GRAPHICS_SOURCES}
    ${IPC_SOURCES}
    ${CORE_SOURCES}
    ${MODULE_SOURCES}
    ${SETTINGS_SOURCES}
)

target_include_directories(rex-wallpaper-runtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(rex-wallpaper-runtime PRIVATE
    rex-shared
    fmt::fmt
    spdlog::spdlog
    d3d11
    d3d12
    dxgi
    dxguid
    d3dcompiler
    windowscodecs
    runtimeobject
    dwmapi
    winmm
        shcore
    ${FFMPEG_LIBRARIES}
)

# ─── Dev tool: synthetic MP4 generator ─────────────────────────────────────
# Writes a short MPEG4+AAC test video (see tools/GenTestMedia.cpp) so the
# runtime's decode / audio / preview surfaces can be exercised without media.
add_executable(rex-gen-media tools/GenTestMedia.cpp)
target_include_directories(rex-gen-media PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(rex-gen-media PRIVATE ${FFMPEG_LIBRARIES})

# ─── Install ───────────────────────────────────────────────────────────────
install(TARGETS rex-core RUNTIME DESTINATION bin)

install(FILES
    include/rex/wallpaper/IWallpaperEngine.hpp
    DESTINATION include/rex/wallpaper
)
