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

// The same in a weather (WeatherLook's rain and snow): while it snows more
// than it rains (the snow weather), snow falls wherever rain would.
int precipitationForColumn(int biome, int topY, float rain, float snow);

constexpr int PRECIPITATION_SNOW_LINE = 160;
