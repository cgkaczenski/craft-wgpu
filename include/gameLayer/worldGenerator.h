#pragma once
#include "blocks.h"
#include <glm/vec2.hpp>
#include <FastNoiseSIMD.h>
#include <worldGeneratorSettings.h>
#include <structure.h>
#include <vector>
#include <chunk.h>
#include <biome.h>

enum
{
	Structure_None = 0,
	Structure_Tree,
	Structure_JungleTree,
	Structure_PalmTree,
	Structure_TreeHouse,
	Structure_Pyramid,
	Structure_Igloo,
	Structure_BirchTree,
	Structure_Spruce,
	Structure_SpruceSlim,
	Structure_SmallStone,
	Structure_TallSlimTree,
	Structure_AbandonedHouse,
	Structure_GoblinTower,
	Structure_AbandonedTrainingCamp,
	Structure_StoneRuins,
	Structure_MinesDungeon,
	Structure_Tavern,
	Structure_Hay,
	Structure_Barn,
};

//used to report back
struct StructureToGenerate
{
	glm::ivec3 pos = {};
	int type = 0;
	float randomNumber1 = 0;
	float randomNumber2 = 0;
	float randomNumber3 = 0;
	float randomNumber4 = 0;
	bool replaceBlocks = 0;
	bool addRandomTreeHeight = 0;
	BlockType replaceLeavesWith = 0;
	BlockType replaceLogWith = 0;
	bool paintLogicStuff = true; //paints leaves and others
	bool replaceEnclosedColumsWithAir = 0;
	unsigned char replaceOverAnything = 0;
	bool placeInCenter = false;

	void setDefaultSmallBuildingSettings()
	{
		replaceOverAnything = 2;
		replaceEnclosedColumsWithAir = true;
	}

	void setDefaultDungeonSettings()
	{
		replaceOverAnything = 100;
		replaceEnclosedColumsWithAir = true;
	}
};

void generateChunk(Chunk &c, WorldGenerator &wg, StructuresManager &structuresManager, BiomesManager &biomesManager
	,std::vector<StructureToGenerate> &generateStructures);

// Milestone G1b: a chunk's biome (and region values) without generating it,
// for a chunk loaded from disk.
void computeChunkBiome(ChunkData &c, WorldGenerator &wg, BiomesManager &biomesManager);

// A picture of what the generator makes, from above, a pixel to a block:
// the chunks from (x0, z0) to (x1, z1), written to `file` as a PNG. Water is
// blue, lighter the higher its surface (sea level darkest), ice white, sand
// pale, the rest green to grey by height. For looking at rivers, lakes and
// coasts without walking them (OURCRAFT_WORLDGEN_MAP="x0,z0,x1,z1,file",
// which the server runs once its settings are loaded, and then quits).
bool writeWorldGenMap(WorldGenerator &wg, StructuresManager &structuresManager, BiomesManager &biomesManager,
	int x0, int z0, int x1, int z1, const char *file);

void generateChunk(ChunkData &c, WorldGenerator &wg, StructuresManager &structuresManager, BiomesManager &biomesManager
	,std::vector<StructureToGenerate> &generateStructures);


