cmake_minimum_required(VERSION 3.20)
project(rex-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDebugDLL")

find_package(Protobuf CONFIG REQUIRED)
find_package(DirectX REQUIRED)

add_executable(rex-core
    src/main.cpp
    src/workerw.cpp
    src/ipc_server.cpp
    src/compositor.cpp
    src/animation_engine.cpp
    src/render_scheduler.cpp
    src/effect_renderer.cpp
)

target_include_directories(rex-core PRIVATE src)
target_link_libraries(rex-core PRIVATE
    protobuf::libprotobuf
    dxgi dxguid d3d12 d3dcompiler
    runtimeobject
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
        )
        target_link_libraries(rex-core-tests PRIVATE
            GTest::GTest GTest::Main
            protobuf::libprotobuf
        )
        target_include_directories(rex-core-tests PRIVATE src)
        gtest_add_tests(TARGET rex-core-tests)
    endif()
endif()

add_custom_command(
    TARGET rex-core PRE_BUILD
    COMMAND protoc --cpp_out=${CMAKE_SOURCE_DIR}/src/generated
        -I${CMAKE_SOURCE_DIR}/../proto
        ${CMAKE_SOURCE_DIR}/../proto/rex.proto
    COMMENT "Generating protobuf sources..."
)
