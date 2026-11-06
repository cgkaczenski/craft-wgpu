#pragma once
#include <blocks.h>

// Milestone G1b: where rain and snow fall. Shared by the renderer's column
// map (Renderer::renderFromBakedData), the rain sound and its muffling under
// a roof (gamePlayLogic.cpp).

enum PrecipitationKind : int
{
	precipitationNone = 0,
	precipitationRain = 1,
	precipitationSnow = 2,
};

// Blocks that rain and snow stop on: anything solid, water and leaves.
// Plants, torches and other things you walk through let it pass.
bool blockStopsRain(BlockType type);

// What falls on a column of this biome (ChunkData::biome) whose rain stops at
// `topY`: none in the desert, snow in the snow biome or above the snow line,
// rain elsewhere.
int precipitationForColumn(int biome, int topY);

constexpr int PRECIPITATION_SNOW_LINE = 160;
