cmake_minimum_required(VERSION 3.20)
project(Anymalsima)

set(CMAKE_CXX_STANDARD 20)

include(CTest)

find_package(Intl REQUIRED)
find_package(Gettext REQUIRED)

add_library(BS_thread_pool INTERFACE)

set(TRACY_PROFILING OFF CACHE BOOL "Enable Tracy Profiling" FORCE)

if(TRACY_PROFILING)
    message("Compiling with Tracy Profiling support")
    add_compile_definitions(TRACY_ENABLE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/external/tracy-eb3df3b-0.13.1)
else()
    message("Compiling with no Tracy Profiling support")
endif()

target_include_directories(BS_thread_pool INTERFACE ${CMAKE_SOURCE_DIR}/external/thread-pool-5.1.0/include)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/stb)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/fmt-12.1.0)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/json-3.12.0)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/entt-3.16.0)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/SDL3-3.4.8)

# Every non-empty chunk, distant terrain patch, structure slab and tree wood
# slab holds a dynamic vertex and index buffer: the default 4096 handles run
# out near big trees (docs/handoff/structures.md). Handles are 16-bit.
foreach(limit VERTEX_BUFFERS DYNAMIC_VERTEX_BUFFERS INDEX_BUFFERS
        DYNAMIC_INDEX_BUFFERS)
    set(BGFX_CONFIG_MAX_${limit} 16384 CACHE STRING "" FORCE)
endforeach()
add_subdirectory(${CMAKE_SOURCE_DIR}/external/bgfx.cmake-1.143-9241-532)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/miniaudio-0.11.25)

# Save-file compression. Only the static library is built.
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/external/zstd-1.5.7/build/cmake
    ${CMAKE_BINARY_DIR}/external/zstd EXCLUDE_FROM_ALL)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
