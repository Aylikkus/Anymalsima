add_library(audio
    audio_system.cpp
)

target_include_directories(audio
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(audio
    PUBLIC
    engine
    miniaudio
    render
)