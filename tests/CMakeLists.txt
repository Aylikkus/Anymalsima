# Compile the real CPU algorithms without linking game, engine, UI or starting
# any devices. The asset API exposes bgfx handles, so headers are still needed.
find_package(Threads REQUIRED)
add_executable(world_logic_tests
    support/test_main.cpp
    unit/game/world/generation_tests.cpp
    unit/game/world/mesh_tests.cpp
    unit/game/world/world_tests.cpp
    unit/game/items/block_tests.cpp
    integration/world_lighting_tests.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/location_generator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/chunk_mesher.cpp
)
target_include_directories(world_logic_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/support
    ${PROJECT_SOURCE_DIR}/src/game
    ${PROJECT_SOURCE_DIR}/src/engine
)
target_include_directories(world_logic_tests SYSTEM PRIVATE
    "$<TARGET_PROPERTY:bgfx,INTERFACE_INCLUDE_DIRECTORIES>"
)
target_link_libraries(world_logic_tests PRIVATE
    BS_thread_pool nlohmann_json i18n Threads::Threads
)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(world_logic_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()
option(ANYMALSIMA_TEST_SANITIZERS "Instrument logic tests with ASan and UBSan" OFF)
if(ANYMALSIMA_TEST_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Test sanitizers require GCC or Clang")
    endif()
    target_compile_options(world_logic_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(world_logic_tests PRIVATE -fsanitize=address,undefined)
endif()

set(world_cases
    tile_classification
    generation_surface_layers
    generation_sea_level
    generation_chunk_seam
    generation_reused_chunk
    generation_repeatable
    layers_cover_depth
    layers_depth_limit
    layers_missing_rock
    chunk_lookup_boundaries
    chunk_missing
    chunk_unregister_lifetime
    chunk_top_bound
    ray_first_hit
    ray_distance_limit
    ray_liquid_hit
    ray_chunk_seam
    ray_negative_direction
    camera_direction
    mesh_empty
    mesh_isolated_block
    mesh_greedy_prism
    mesh_chunk_seam
    mesh_material_light
    mesh_winding
    sunlight_open_column
    sunlight_opaque_roof
    sunlight_partial_opacity
    sunlight_water
    sunlight_slab_seam
    sunlight_late_slab
    sunlight_horizontal
    sunlight_dirty_column
    sunlight_stale_result
)
foreach(case IN LISTS world_cases)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world" TIMEOUT 15)
endforeach()
add_test(NAME game.world.sunlight_generated_ocean
    COMMAND world_logic_tests sunlight_generated_ocean)
set_tests_properties(game.world.sunlight_generated_ocean PROPERTIES
    LABELS "integration;gameplay;world;threading" TIMEOUT 15)
foreach(case block_default_opacity block_explicit_opacity)
    add_test(NAME game.items.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.items.${case} PROPERTIES
        LABELS "unit;gameplay;data" TIMEOUT 10)
endforeach()
