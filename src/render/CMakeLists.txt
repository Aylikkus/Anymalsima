add_library(render
    resource_manager.cpp
    renderer.cpp
)

target_include_directories(render
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(render
    PUBLIC
    audio
    platform
    game
    SDL3::SDL3
    miniaudio
    bgfx bx bimg
    stb
)