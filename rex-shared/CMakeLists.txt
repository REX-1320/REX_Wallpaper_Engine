cmake_minimum_required(VERSION 3.20)
project(rex-shared VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# ─── Dependencies ──────────────────────────────────────────────────────────

find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# ─── Public Headers ──────────────────────────────────────────────────────

set(PUBLIC_HEADERS
    include/shared/Logger.hpp
    include/shared/Result.hpp
    include/shared/UUID.hpp
    include/shared/Stopwatch.hpp
)

# ─── Library ──────────────────────────────────────────────────────────────

add_library(rex-shared STATIC
    src/Logger.cpp
    src/UUID.cpp
    src/Stopwatch.cpp
)

target_include_directories(rex-shared
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(rex-shared
    PUBLIC
        fmt::fmt
        spdlog::spdlog
)

target_compile_definitions(rex-shared
    PUBLIC
        REX_SHARED_BUILD
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

# ─── Install ──────────────────────────────────────────────────────────────

install(TARGETS rex-shared
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES ${PUBLIC_HEADERS}
    DESTINATION include/shared
)
