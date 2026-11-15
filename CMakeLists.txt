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

add_subdirectory(${CMAKE_SOURCE_DIR}/external/bgfx.cmake-1.143-9241-532)

add_subdirectory(${CMAKE_SOURCE_DIR}/external/miniaudio-0.11.25)

add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
