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
    ${PROJECT_SOURCE_DIR}/src/game/world/block_light_propagator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/light_sources.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/lod_light_map.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/sky_occlusion_map.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/sky_column.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/sky_height_map.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/light_volume.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/world_editor.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/terrain_edits.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/terrain_edit_tracker.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/location_manager.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/chunk_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/flora_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/flora_generator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/flora_mesh_builder.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/flora_simulation.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/tree_skeleton.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/tree_rasterizer.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/structure_voxels.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/structure_overrides.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/tree_wood_lod.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/tree_generator.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/tree_foliage_builder.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/tree_simulation.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/region_file.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/region_store.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/world_map_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/world_meta.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/save_catalog.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/terrain_edits_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/light_sources_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/trees_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/structures_codec.cpp
    ${PROJECT_SOURCE_DIR}/src/game/world/save/sky_codec.cpp
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
    unit/game/world/terrain_edit_tests.cpp
    unit/game/world/light_volume_tests.cpp
    unit/game/world/world_editor_tests.cpp
    unit/game/world/world_tests.cpp
    unit/game/world/world_wrap_tests.cpp
    unit/game/items/block_tests.cpp
    integration/world_lighting_tests.cpp
    integration/sunlight_boundary_tests.cpp
    integration/lighting_priority_tests.cpp
    integration/region_persistence_tests.cpp
    integration/block_light_tests.cpp
    unit/game/items/light_emission_tests.cpp
    unit/game/world/light_shadow_tests.cpp
    unit/engine/serialization/binary_stream_tests.cpp
    unit/game/world/save_codec_tests.cpp
    unit/game/world/ground_cover_tests.cpp
    unit/game/world/flora_tests.cpp
    unit/game/items/flora_data_tests.cpp
    unit/game/items/tree_data_tests.cpp
    unit/game/world/tree_tests.cpp
    unit/game/world/structure_voxels_tests.cpp
    unit/game/world/sky_tests.cpp
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
    mesh_local_positions_overlap
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
list(APPEND world_cases
    edit_dig_pit_lowers_ground
    edit_structure_overhang
    edit_structure_crosses_region
    edit_unloaded_slab_keeps_structure
    edit_detected_from_save edit_generated_castle_damage edit_generated_castle_revision
    structure_revision_rebases_saved_and_ready_chunks
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
set(block_light_cases
    block_light_round_falloff block_light_wall_shadow
    block_light_soft_shadow_and_translucency block_light_chunk_seam
    block_light_late_neighbor block_light_removal_clears block_light_color
    block_light_spotlight block_light_far_registry
    block_light_far_registry_removed_with_block block_light_entity_source
    block_light_wraps_x far_light_selection
    block_light_additive block_light_spot_into_wall
    block_light_sealed_registry block_light_spot_sealed_by_front
    block_light_open_faces_neighbor_arrives light_registry_buckets
    shadow_face_mapping shadow_tile_layout shadow_reachable_faces
    shadow_guard_band
    lod_light_map_splat lod_light_map_wraps block_light_far_spreads_directly
)
foreach(case IN LISTS block_light_cases)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "integration;gameplay;world;threading;lighting" TIMEOUT 15)
endforeach()
foreach(case block_default_opacity block_explicit_opacity
             light_emission_defaults light_emission_rejects_invalid
             light_emission_curve light_chroma_encoding)
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
             save_catalog_slots region_index_covers_world_height
             terrain_edits_roundtrip terrain_edits_rejects_corrupt
             structure_save_migrates_trees
             light_sources_roundtrip light_sources_rejects_corrupt
             light_sources_versions)
    add_test(NAME game.save.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.save.${case} PROPERTIES
        LABELS "unit;gameplay;save" TIMEOUT 10)
endforeach()
foreach(case region_save_and_reload region_edit_persists
             region_reload_while_writing region_corrupt_file_regenerates
             region_unload_while_opening region_gradual_unload
             region_reload_while_unloading)
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

# Ground cover and flora data (docs/handoff/flora.md).
foreach(case flora_value_ranges ground_cover_parsing foliage_material_parsing
             flora_species_parsing flora_references_validated
             ground_cover_selection_rules flora_runtime_content)
    add_test(NAME game.items.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.items.${case} PROPERTIES
        LABELS "unit;gameplay;data;flora" TIMEOUT 10)
endforeach()
# Distant structure voxels (docs/handoff/structures.md).
foreach(case structure_voxels_plan_levels structure_voxels_plan_hysteresis
             structure_voxels_plan_wraps structure_voxels_plan_chunk_handover
             structure_voxels_slab_merge structure_stamp_only_fills_air
             structure_footprint_tops structure_footprint_incremental
             sky_occlusion_matches_sunlight sky_occlusion_by_height
             sky_occlusion_wraps)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;structures" TIMEOUT 10)
endforeach()
add_test(NAME game.world.structure_chunk_stamping
    COMMAND world_logic_tests structure_chunk_stamping)
set_tests_properties(game.world.structure_chunk_stamping PROPERTIES
    LABELS "integration;gameplay;world;structures" TIMEOUT 10)
# Sky heightmap (docs/handoff/sky-light.md).
foreach(case sky_column_scan_and_direct sky_tile_build_sources
             sky_tile_overrides sky_codec_roundtrip tree_wood_tops_match_raster
             sky_heightmap_matches_chunks sky_queries_and_far_shade_without_chunks
             sky_sunlight_without_upper_slabs
             sky_spread_without_waiting)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;lighting" TIMEOUT 60)
endforeach()
# Tree data (docs/handoff/trees.md).
foreach(case tree_species_parsing tree_size_classes tree_references_validated
             tree_runtime_content)
    add_test(NAME game.items.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.items.${case} PROPERTIES
        LABELS "unit;gameplay;data;flora;trees" TIMEOUT 10)
endforeach()
foreach(case tree_skeleton_deterministic tree_sizes_follow_species
             tree_growth_only_adds_wood tree_size_classes_in_blocks
             tree_height_cap tree_ground_pruning tree_cuts_and_regrowth
             tree_cut_flat_on_thick_trunks tree_regrowth_coherent
             tree_skeleton_cache tree_region_sets tree_column_wraps_seam
             tree_chunks_order_independent tree_chunks_air_and_tops
             tree_saved_chunks_not_edits tree_flora_avoids_trunks
             tree_landmarks tree_rotation_exact tree_foliage_meshes
             trees_codec_roundtrip tree_cuts_incremental tree_repeated_branch_cuts
             tree_cut_delta_local tree_cut_angled_branch tree_cut_slanted_slit
             tree_cut_relights_below
             tree_cut_and_regrowth
             tree_reload_catches_up tree_reload_same_day_cut
             tree_planted_indoors tree_billboards
             tree_wood_lod_slabs tree_wood_lod_occupancy
             tree_wood_lod_cut_is_local tree_settle_closes_gap tree_settle_in_world
             tree_waiting_chunk_does_not_stall_streaming
             tree_dies_without_ground tree_dies_without_base_wood
             tree_trunk_foliage_on_stem tree_far_crowns_stay_attached
             tree_sets_wanted_by_renderer streaming_progresses_with_huge_queue
             streaming_withdraws_stale_requests tree_regrowth_starts_on_stump
             streaming_new_chunks_sky_lit tree_regrowth_takes_time
             tree_crown_mesh_never_ahead_of_wood tree_immortal_landmark
             tree_elder_undercut_is_excavation tree_landmark_wounds_persist_in_lod
             tree_wood_layers tree_legacy_bark_migrates_and_core_supports_cut
             tree_roots_wrap_and_preserve_substrate
             tree_elder_roots_backfill_uphill_contact
             tree_live_roots_respect_edits_and_support)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;flora;trees" TIMEOUT 30)
endforeach()
foreach(case cover_generation_surface_only cover_wraps_seam cover_mesh_faces
             cover_volume_dilation cover_cleared_by_edits
             cover_lod_matches_detail cover_tint_packing)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;flora" TIMEOUT 15)
endforeach()
foreach(case flora_growth_stages flora_generation_borders
             flora_generation_wraps_seam flora_edits flora_meshes
             flora_world_clock flora_life_cycle
             flora_catchup_uses_direct_sky flora_simulation_spread_harvest_regrow
             flora_rare_species
             flora_rare_species_persists)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;flora" TIMEOUT 30)
endforeach()
foreach(case flora_codec_roundtrip flora_streaming_persists)
    add_test(NAME game.save.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.save.${case} PROPERTIES
        LABELS "unit;gameplay;save;flora" TIMEOUT 30)
endforeach()
add_test(NAME game.save.cover_codec_versions
    COMMAND world_logic_tests cover_codec_versions)
set_tests_properties(game.save.cover_codec_versions PROPERTIES
    LABELS "unit;gameplay;save;flora" TIMEOUT 10)

foreach(case terrain_caps_repeatability terrain_shared_edges_corners
             terrain_relief_strength terrain_cliff_segments
             terrain_coast_dry_inland terrain_world_bounds
             terrain_surface_continuous_at_region_borders
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
             lod_selection_coverage_balance lod_cover_fallback lod_edits_lower_ground
             lod_loaded_water_mesh lod_detail_survives_refresh
             lod_detail_hysteresis lod_detail_low_handoff_wraps
             lod_mesh_height_matches_triangles
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

# DEVICE TARGET (GPU + display): explicit opt-in, exercises real Vulkan
# resources and shader programs, without a window or audio device. Never
# registered in CTest, so the default suite stays headless.
add_executable(lod_render_smoke EXCLUDE_FROM_ALL benchmarks/lod_render_smoke.cpp)
target_link_libraries(lod_render_smoke PRIVATE render game)
target_include_directories(lod_render_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)

# Dev tool: PPM render of seeded terrain for inspecting coasts and mountains.
add_executable(terrain_heightmap EXCLUDE_FROM_ALL benchmarks/terrain_heightmap.cpp)
target_link_libraries(terrain_heightmap PRIVATE world_logic_cpu)
target_include_directories(terrain_heightmap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)

foreach(case wrap_helper_math wrap_noise_periodic wrap_terrain_seam
             wrap_chunks_tiles_ray_editor wrap_sunlight_crosses_seam
             wrap_lod_selection_across_seam wrap_lod_build_has_no_seam_walls)
    add_test(NAME game.world.${case} COMMAND world_logic_tests ${case})
    set_tests_properties(game.world.${case} PROPERTIES
        LABELS "unit;gameplay;world;wrap" TIMEOUT 30)
    if(ANYMALSIMA_TEST_SANITIZERS)
        set_tests_properties(game.world.${case} PROPERTIES
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endforeach()

# The elder as the tree optimization sample: break, cut, crown and voxel
# costs of one massive tree (docs/handoff/trees.md).
add_executable(tree_benchmark EXCLUDE_FROM_ALL benchmarks/tree_benchmark.cpp)
target_link_libraries(tree_benchmark PRIVATE world_logic_cpu)
target_include_directories(tree_benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/support)
target_compile_definitions(tree_benchmark PRIVATE
    SOIL_CONTENT_DIR="${PROJECT_SOURCE_DIR}/assets/data")
