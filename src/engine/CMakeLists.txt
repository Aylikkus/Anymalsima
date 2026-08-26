add_library(engine
    core/log.cpp
    core/input_system.cpp
    core/settings.cpp
    render/render_target.cpp
    serialization/file_io.cpp
    serialization/saveable.cpp
    animation/tween.cpp
    animation/animator.cpp
)

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(engine PUBLIC bgfx bx bimg)
target_link_libraries(engine PUBLIC SDL3::SDL3)
target_link_libraries(engine PUBLIC BS_thread_pool)
target_link_libraries(engine PUBLIC i18n)
target_link_libraries(engine PUBLIC fmt)
target_link_libraries(engine PUBLIC nlohmann_json)