add_library(platform
    platform.cpp
)

target_include_directories(platform
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(platform 
    PUBLIC 
    engine
    bgfx bx bimg)