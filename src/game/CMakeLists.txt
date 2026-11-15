add_library(game
    game.cpp
    states/main_menu_state.cpp
    states/create_or_load_state.cpp
    states/create_world_state.cpp
    states/gameplay_state.cpp
    world/world.cpp
    world/world_generator.cpp
    world/world_generation_task.cpp
    world/location_generator.cpp
    world/chunk_mesher.cpp
    world/chunk_manager.cpp
    world/sunlight_propagator.cpp
    world/layers_generator.hpp
)

target_include_directories(game
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(game PUBLIC EnTT::EnTT)
target_link_libraries(game PUBLIC engine ui)
