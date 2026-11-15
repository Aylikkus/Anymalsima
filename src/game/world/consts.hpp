#pragma once

// Vertical layout (see height_profile.hpp): 0-256 underground, 256-384 ocean,
// 384 sea level, up to 896 peaks, 896-1024 buildable sky.
static constexpr int SEA_LEVEL_Y = 384;

// How many XY chunks cover one region side
static constexpr int CHUNKS_PER_REGION = 8;  // 8×8 chunks = 128×128 tiles
static constexpr int Y_CHUNKS = 64;          // 64 * 16 = 1024
static constexpr int WORLD_HEIGHT_Y = Y_CHUNKS * 16;

// Chunks drawn as real voxel meshes: at most this many chunks from the
// camera chunk on every axis (GameplayState requests and draws this box).
static constexpr int DETAIL_CHUNK_RADIUS = 8;
