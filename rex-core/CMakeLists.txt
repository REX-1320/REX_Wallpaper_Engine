cmake_minimum_required(VERSION 3.20)
project(rex-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")

add_executable(rex-core
    src/main.cpp
    src/workerw.cpp
    src/ipc_server.cpp
    src/compositor.cpp
    src/animation_engine.cpp
    src/render_scheduler.cpp
    src/effect_renderer.cpp
    src/sandbox.cpp
)

set_target_properties(rex-core PROPERTIES
    WIN32_EXECUTABLE TRUE
)

add_executable(rex-widget-host
    src/widget_host.cpp
)

set_target_properties(rex-widget-host PROPERTIES
    WIN32_EXECUTABLE TRUE
)
target_include_directories(rex-widget-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2-sdk
)
target_link_libraries(rex-widget-host PRIVATE
    dxgi dxguid d3d12 windowscodecs
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2-sdk/WebView2Loader.dll.lib
)
# Copy WebView2Loader.dll to output directory
add_custom_command(TARGET rex-widget-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/webview2-sdk/WebView2Loader.dll"
        "$<TARGET_FILE_DIR:rex-widget-host>/WebView2Loader.dll"
)

target_include_directories(rex-core PRIVATE src)
target_link_libraries(rex-core PRIVATE
    dxgi dxguid d3d12 d3dcompiler
    runtimeobject dwmapi winmm
)

include(CTest)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest CONFIG QUIET)
    if(GTest_FOUND)
        add_executable(rex-core-tests
            tests/test_ipc.cpp
            tests/test_workerw.cpp
            tests/test_compositor.cpp
            tests/test_sandbox.cpp
        )
        target_link_libraries(rex-core-tests PRIVATE
            GTest::GTest GTest::Main
        )
        target_include_directories(rex-core-tests PRIVATE src)
        gtest_add_tests(TARGET rex-core-tests)
    endif()
endif()
