# Compile the real CPU algorithms without linking game, engine, UI or starting
# any devices. The asset API exposes bgfx handles, so headers are still needed.
find_package(Threads REQUIRED)
add_library(world_logic_cpu STATIC
    ${PROJECT_SOURCE_DIR}/src/game/world/location_generator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/terrain_sampler.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/surface_sampler.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/terrain_lod.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/chunk_mesher.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/chunk_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/sunlight_propagator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/light_volume.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/world_editor.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/location_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/chunk_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/region_file.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/region_store.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/world_map_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/world_meta.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/save_catalog.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/serialization/binary_stream.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/serialization/compression.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/serialization/section_container.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/serialization/file_io.cpp
)
target_include_directories(world_logic_cpu PUBLIC
    ${PROJECT_SOURCE_DIR}/src/game
    ${PROJECT_SOURCE_DIR}/src/engine
)
target_include_directories(world_logic_cpu SYSTEM PUBLIC
    "$<TARGET_PROPERTY:bgfx,INTERFACE_INCLUDE_DIRECTORIES>"
)
target_link_libraries(world_logic_cpu PUBLIC
    BS_thread_pool nlohmann_json i18n Threads::Threads libzstd_static
)
add_executable(world_logic_tests
    support/test_main.cpp
    unit/game/world/generation_tests.cpp
    unit/game/world/soil_tests.cpp
    unit/game/world/terrain_tests.cpp
    unit/game/world/lod_tests.cpp
    unit/game/world/mesh_tests.cpp
    unit/game/world/light_volume_tests.cpp
    unit/game/world/world_editor_tests.cpp
    unit/game/world/world_tests.cpp
    unit/game/items/block_tests.cpp
    integration/world_lighting_tests.cpp
    integration/sunlight_boundary_tests.cpp
    integration/lighting_priority_tests.cpp
    integration/region_persistence_tests.cpp
    unit/engine/serialization/binary_stream_tests.cpp
    unit/game/world/save_codec_tests.cpp
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
    mesh_merges_across_light
    mesh_winding
    mesh_skips_hidden_chunks
    light_volume_encoding
    light_volume_neighbor_borders
    light_volume_missing_neighbor
    light_volume_shared_face
    light_volume_signals
    editor_place_remove
    editor_raises_column_top
    editor_bulk_statuses
    editor_relights_column
    ray_reports_face
    editor_relight_keeps_indirect
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
# Save/load coverage. Scratch worlds live under the build tree.
target_compile_definitions(world_logic_tests PRIVATE
    TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}/scratch")
foreach(case binary_stream_roundtrip binary_stream_truncation
             compression_roundtrip section_container_roundtrip
             section_container_rejects_bad_input)
    add_test(NAME engine.serialization.${case}
        COMMAND world_logic_tests ${case})
    set_tests_properties(engine.serialization.${case} PROPERTIES
        LABELS "unit;engine;save" TIMEOUT 10)
endforeach()
foreach(case chunk_codec_uniform chunk_codec_mixed_strips_light
             chunk_codec_wide_palette chunk_codec_rejects_corrupt
             region_file_roundtrip world_seed_derivation world_map_roundtrip
             save_catalog_slots region_index_covers_world_height)
    add_test(NAME game.save.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.save.${case} PROPERTIES
        LABELS "unit;gameplay;save" TIMEOUT 10)
endforeach()
foreach(case region_save_and_reload region_edit_persists
             region_reload_while_writing region_corrupt_file_regenerates
             region_unload_while_opening)
    add_test(NAME game.save.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.save.${case} PROPERTIES
        LABELS "integration;gameplay;save;threading" TIMEOUT 30)
endforeach()

if(ANYMALSIMA_TEST_SANITIZERS)
    get_property(logic_tests DIRECTORY PROPERTY TESTS)
    set_tests_properties(${logic_tests} PROPERTIES
        ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()

target_compile_definitions(world_logic_tests PRIVATE
    SOIL_CONTENT_DIR="${PROJECT_SOURCE_DIR}/assets/data")
foreach(case soil_data_validation soil_loading_references_capacity
             soil_placement_priority soil_selection_order_topsoil
             soil_horizons_exposure soil_generation_seams_geology
             soil_runtime_content soil_streaming_profile)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;data" TIMEOUT 15)
    if(ANYMALSIMA_TEST_SANITIZERS)
        set_tests_properties(game.world.${case} PROPERTIES
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endforeach()

foreach(case terrain_caps_repeatability terrain_shared_edges_corners
             terrain_relief_strength terrain_cliff_segments
             terrain_coast_dry_inland terrain_world_bounds
             terrain_air_chunks_skip_jobs
             terrain_geology_erosion terrain_soil_transition
             terrain_streaming_cache terrain_column_inputs_seams
             terrain_height_profile terrain_coast_organic
             terrain_surface_ceiling terrain_mountain_relief)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;terrain" TIMEOUT 15)
    if(ANYMALSIMA_TEST_SANITIZERS)
        set_tests_properties(game.world.${case} PROPERTIES
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endforeach()

foreach(case lod_surface_matches_chunks lod_geology_order_independent
             lod_stepped_surface_and_bands lod_smooth_skirts_cover_edges
             lod_stepped_boundary_walls lod_water_and_bounds
             lod_selection_coverage_balance lod_cover_fallback
             lod_loaded_water_mesh lod_detail_survives_refresh
             lod_detail_hysteresis lod_mesh_height_matches_triangles
             lod_detail_coarse_joins)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;lod" TIMEOUT 30)
    if(ANYMALSIMA_TEST_SANITIZERS)
        set_tests_properties(game.world.${case} PROPERTIES
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endforeach()

add_executable(lod_benchmark EXCLUDE_FROM_ALL benchmarks/lod_benchmark.cpp)
target_link_libraries(lod_benchmark PRIVATE world_logic_cpu)
target_include_directories(lod_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
if(ANYMALSIMA_TEST_SANITIZERS)
    target_compile_options(lod_benchmark PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(lod_benchmark PRIVATE -fsanitize=address,undefined)
endif()

# Explicit opt-in: exercises real Vulkan resources and shader programs, without
# a window or audio device. Kept out of the default headless logic suite.
add_executable(lod_render_smoke EXCLUDE_FROM_ALL benchmarks/lod_render_smoke.cpp)
target_link_libraries(lod_render_smoke PRIVATE render game)
target_include_directories(lod_render_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)

# Dev tool: PPM render of seeded terrain for inspecting coasts and mountains.
add_executable(terrain_heightmap EXCLUDE_FROM_ALL benchmarks/terrain_heightmap.cpp)
target_link_libraries(terrain_heightmap PRIVATE world_logic_cpu)
target_include_directories(terrain_heightmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
