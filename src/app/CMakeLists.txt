add_executable(app main.cpp)

target_link_libraries(app
    PRIVATE
    engine
    game
    platform
    render
    audio
)

if (TRACY_PROFILING EQUAL ON)
    target_link_libraries(app PRIVATE TracyClient)
endif()

add_custom_target(copy_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:app>/assets"
    COMMENT "Assets copy..."
)

add_dependencies(app copy_assets)