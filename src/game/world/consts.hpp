#pragma once

static constexpr int SEA_LEVEL_Y = 256 + (256 / 2);

// How many XY chunks cover one region side
static constexpr int CHUNKS_PER_REGION = 8;  // 8×8 chunks = 128×128 tiles
static constexpr int Y_CHUNKS = 48;          // 48 * 16 = 768