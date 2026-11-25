add_library(game
    game.cpp
    states/main_menu_state.cpp
    states/create_or_load_state.cpp
    states/create_world_state.cpp
    states/gameplay_state.cpp
    states/load_world_state.cpp
    world/world.cpp
    world/world_generator.cpp
    world/world_generation_task.cpp
    world/location_generator.cpp
    world/terrain_sampler.cpp
    world/natural_shape.cpp
    world/reservoirs.cpp
    world/generated_hydrology.cpp
    world/generated_rivers.cpp
    world/surface_sampler.cpp
    world/terrain_lod.cpp
    world/terrain_lod_manager.cpp
    world/structure_lod.cpp
    world/tile_shape.cpp
    world/tile_piece.cpp
    world/snow_settlement.cpp
    world/snow_cover.cpp
    world/chunk.cpp
    world/grounded_probe.cpp
    world/chunk_mesher.cpp
    world/chunk_manager.cpp
    world/sunlight_propagator.cpp
    world/block_light_propagator.cpp
    world/light_sources.cpp
    world/light_renderer.cpp
    world/light_shadows.cpp
    world/lod_light_map.cpp
    world/sky_occlusion_map.cpp
    world/sky_column.cpp
    world/sky_height_map.cpp
    world/light_volume.cpp
    world/voxel_light.cpp
    world/light_volume_manager.cpp
    world/world_editor.cpp
    world/terrain_edits.cpp
    world/terrain_edit_tracker.cpp
    world/location_manager.cpp
    world/save/chunk_codec.cpp
    world/save/flora_codec.cpp
    world/flora_generator.cpp
    world/flora_mesh_builder.cpp
    world/flora_simulation.cpp
    world/tree_skeleton.cpp
    world/tree_rasterizer.cpp
    world/structure_voxels.cpp
    world/structure_overrides.cpp
    world/tree_wood_lod.cpp
    world/tree_generator.cpp
    world/flora_renderer.cpp
    world/tree_foliage_builder.cpp
    world/tree_renderer.cpp
    world/tree_simulation.cpp
    world/save/region_file.cpp
    world/save/region_store.cpp
    world/save/world_map_codec.cpp
    world/save/world_meta.cpp
    world/save/save_catalog.cpp
    world/save/terrain_edits_codec.cpp
    world/save/light_sources_codec.cpp
    world/save/trees_codec.cpp
    world/save/structures_codec.cpp
    world/save/sky_codec.cpp
    items/soil.hpp
    items/soil_validation.hpp
    world/soil_generator.hpp
    world/layers_generator.hpp
)

# These CPU samplers run millions of times while deriving a default world.
# Keep Debug diagnostics elsewhere, but compile this hot, deterministic
# generation path with optimization so loading a saved map does not stall.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(
        world/terrain_sampler.cpp
        world/natural_shape.cpp
        world/generated_hydrology.cpp
        world/generated_rivers.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>"
    )
endif()

target_include_directories(game
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(game PUBLIC EnTT::EnTT)
target_link_libraries(game PUBLIC engine ui)
