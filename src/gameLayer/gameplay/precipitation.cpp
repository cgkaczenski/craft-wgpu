#include <gameplay/precipitation.h>
#include <biome.h>

bool blockStopsRain(BlockType type)
{
	return type != BlockTypes::air && (isColidable(type) || type == BlockTypes::water || isAnyLeaves(type));
}

int precipitationForColumn(int biome, int topY)
{
	if (biome == BiomesManager::desert) { return precipitationNone; }
	if (biome == BiomesManager::snow || topY >= PRECIPITATION_SNOW_LINE) { return precipitationSnow; }
	return precipitationRain;
}
