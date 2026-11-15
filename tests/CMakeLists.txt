# Compile the real CPU algorithms without linking game, engine, UI or starting
# any devices. The asset API exposes bgfx handles, so headers are still needed.
find_package(Threads REQUIRED)
add_library(world_logic_cpu STATIC
    ${PROJECT_SOURCE_DIR}/src/game/world/location_generator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/chunk_mesher.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/chunk_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/sunlight_propagator.cpp
)
target_include_directories(world_logic_cpu PUBLIC
    ${PROJECT_SOURCE_DIR}/src/game
    ${PROJECT_SOURCE_DIR}/src/engine
)
target_include_directories(world_logic_cpu SYSTEM PUBLIC
    "$<TARGET_PROPERTY:bgfx,INTERFACE_INCLUDE_DIRECTORIES>"
)
target_link_libraries(world_logic_cpu PUBLIC
    BS_thread_pool nlohmann_json i18n Threads::Threads
)
add_executable(world_logic_tests
    support/test_main.cpp
    unit/game/world/generation_tests.cpp
    unit/game/world/mesh_tests.cpp
    unit/game/world/world_tests.cpp
    unit/game/items/block_tests.cpp
    integration/world_lighting_tests.cpp
    integration/sunlight_boundary_tests.cpp
    integration/lighting_priority_tests.cpp
)
add_executable(lighting_benchmark benchmarks/lighting_benchmark.cpp)
option(ANYMALSIMA_TEST_SANITIZERS "Instrument logic tests with ASan and UBSan" OFF)
foreach(target world_logic_cpu world_logic_tests lighting_benchmark)
    if(NOT target STREQUAL "world_logic_cpu")
        target_link_libraries(${target} PRIVATE world_logic_cpu)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(ANYMALSIMA_TEST_SANITIZERS)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "Test sanitizers require GCC or Clang")
        endif()
        target_compile_options(${target} PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()

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
set(boundary_cases
    sunlight_cross_pos_x sunlight_cross_neg_x
    sunlight_cross_pos_y sunlight_cross_neg_y
    sunlight_cross_pos_z sunlight_cross_neg_z
    sunlight_cross_three_axis_corner
    sunlight_cross_barrier_edit sunlight_cross_partial_opacity
    sunlight_cross_water sunlight_cross_competing_sources
    sunlight_cross_late_neighbor sunlight_cross_load_order_workers
    sunlight_cross_source_unload sunlight_cross_stale_neighbor_result
    sunlight_missing_upper_slab sunlight_unknown_upper_slab
    sunlight_ready_above_bound
    sunlight_raised_bound sunlight_teardown_pending chunk_negative_coordinates
)
foreach(case IN LISTS boundary_cases)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "integration;gameplay;world;threading" TIMEOUT 15)
endforeach()
foreach(case block_default_opacity block_explicit_opacity)
    add_test(NAME game.items.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.items.${case} PROPERTIES
        LABELS "unit;gameplay;data" TIMEOUT 10)
endforeach()
set(priority_cases
    priority_progressive_near_first priority_camera_teleport
    priority_vertical_camera_move priority_arrivals_preserve_light
    priority_continuous_arrivals priority_background_progress
    priority_generation_bounded_teleport priority_sky_dependency_height
    priority_generation_replacement priority_worker_pool_urgent
    priority_dependency_camera_move
    priority_mesh_bounded_focus priority_lighting_submission_cap
    priority_queue_stable_age
)
foreach(case IN LISTS priority_cases)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "integration;gameplay;world;threading;priority" TIMEOUT 15)
endforeach()
if(ANYMALSIMA_TEST_SANITIZERS)
    get_property(logic_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${logic_tests} PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()
