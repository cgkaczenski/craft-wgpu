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

int precipitationForColumn(int biome, int topY, float rain, float snow)
{
	const int kind = precipitationForColumn(biome, topY);
	if (kind == precipitationRain && snow > rain) { return precipitationSnow; }
	return kind;
}
