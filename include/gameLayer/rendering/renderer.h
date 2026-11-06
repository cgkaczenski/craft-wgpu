#pragma once
#include <vector>
#include <cstdint>
#include "rendering/camera.h"
#include "rendering/skyBoxRenderer.h"
#include <gl2d/gl2d.h>
#include "blocks.h"
#include <unordered_map>
#include <rendering/model.h>
#include <gameplay/weather.h>

struct BlocksLoader;
struct ChunkSystem;
struct ProgramData;
struct ClientEntityManager;
struct ModelsManager;
struct BoneTransform;
struct PlayerConnectionData;

struct AdaptiveExposure
{

	float currentExposure = 1.6;
	float minExposure = 1.0;
	float maxExposure = 2.0;
	float moveSpeed = 0;

	float bonusAmbient = 0;
	float currentLuminosity = 0.5; //0 - 1

	void update(float deltaTime, float newLuminosity);

	float getLuminosityOrDefaultValueIfDisabeled();
};

// What a frame of the world needs computed on the CPU before wgpuWorld draws
// it: the lighting for the time of day, the culled and sorted chunks, the
// entities' bones and the block geometry tables.
struct Renderer
{
	float averageLuminosity = 0.5;

	struct BlockGeometryIndex
	{
		int startIndex = 0;
		int componentCount = 0;
	};

	BlockGeometryIndex blockGeometry[ModelsManager::BLOCK_MODELS_COUNT];
	BlockGeometryIndex blockGeometryForEntities[ModelsManager::BLOCK_MODELS_COUNT];


	void recreateBlockGeometryData(ModelsManager &modelsManager);

	// The world's shading values that the settings menus edit (GL kept them
	// in its default shader's uniform block).
	struct ShadingSettings
	{
		glm::vec3 waterColor = (glm::vec3(6, 42, 52) / 255.f); // (glm::vec3(6, 27, 43) / 255.f);
		int tonemapper = 0;
		glm::vec3 underWaterColor = (glm::vec3(0, 17, 25) / 255.f);
		float exposure = 1.6;
		float underwaterDarkenStrength = 0.94;
		float underwaterDarkenDistance = 29;
		float fogGradientUnderWater = 1.9;
		int shaders = true;
	}shadingSettings;

	bool zprepass = 1;
	bool frustumCulling = 1;
	bool ssao = 1;

	void recreateBlocksTexturesBuffer(BlocksLoader &blocksLoader);

	void renderAllBlocksUiTextures(BlocksLoader &blocksLoader, ModelsManager &modelsManager);

	void create(ModelsManager &modelsManager);
	bool created = false;
	void reloadShaders();

	void renderFromBakedData(ChunkSystem &chunkSystem, Camera &c,
		ProgramData &programData, BlocksLoader &blocksLoader,
		ClientEntityManager &entityManager, ModelsManager &modelsManager, 
		AdaptiveExposure &adaptiveExposure,
		bool showLightLevels, glm::dvec3 pointPos,
		bool underWater, int screenX, int screenY, float deltaTime, float dayTime,
		std::uint64_t currentSkinTextureId, bool &playerClicked, float playerRunning,
		BoneTransform &playerHand, int currentHeldItemIndex, float waterDropsStrength,
		bool showHand, std::unordered_map<std::uint64_t, PlayerConnectionData> &playersConnectionData);
	
	void renderDecal(glm::ivec3 position, Camera &c, Block b, ProgramData &programData, 
		float crack);

	void renderEntities(float deltaTime,
		Camera &c,
		ModelsManager &modelsManager,
		BlocksLoader &blocksLoader, ClientEntityManager &entityManager,
		glm::mat4 &vp, const glm::mat4 &projection, glm::mat4 &viewMatrix, glm::vec3 posFloat, glm::ivec3 posInt,
		float exposure, ChunkSystem &chunkSystem, int skyLightIntensity,
		std::uint64_t currentSkinTextureId,
		bool &playerClicked, float playerRunning, BoneTransform &playerHand,
		int currentHeldItemIndex, bool showHand, std::unordered_map<std::uint64_t, PlayerConnectionData> &playersConnectionData
		);

	float waterTimer = 0;

	glm::vec3 sunPos = glm::normalize(glm::vec3(-1, 0.84, -1));//todo change

	// Milestone G1: how the weather changes this frame's look (set each frame
	// from the world clock, before renderFromBakedData).
	WeatherLook weather;
	// Milestone G1b: 0 the biome picks rain or snow, 1 rain everywhere, 2
	// snow everywhere (the debug panel; OURCRAFT_PRECIPITATION=rain|snow).
	int precipitationPreview = 0;
};

// Debug cubes and lines, drawn by wgpuWorld as line segments.

struct GyzmosRenderer
{

	struct CubeData
	{
		int x = 0, y = 0, z = 0, unused = 0;
		float posFloatX = 0, posFloatY = 0, posFloatZ = 0, posFloat2 = 0;
		float sizeX = 0, sizeY = 0, sizeZ = 0, unused3 = 0;
	};


	struct LinesData
	{
		glm::vec3 a = {};
		glm::vec3 b = {};
	};

	std::vector<CubeData> cubes;
	std::vector<LinesData> lines;

	void drawCube(int x, int y, int z) { cubes.push_back({x, y, z}); };
	void drawCube(glm::ivec3 pos) { drawCube(pos.x, pos.y, pos.z); };
	void drawCube(glm::ivec3 pos, glm::vec3 posFloat, glm::vec3 size)
		{ cubes.push_back({pos.x, pos.y, pos.z, 0, 
		posFloat.x,posFloat.y,posFloat.z,0,
		size.x, size.y, size.z, 0}); };

	//todo not working at far distances rn
	void drawLine(glm::dvec3 a, glm::dvec3 b) { lines.push_back({a, b}); }
	void render(Camera &c, glm::ivec3 posInt, glm::vec3 posFloat);

};

constexpr int mergeShortsUnsigned(unsigned short a, unsigned short b)
{
	return (int)((unsigned int)a | ((unsigned int)b << 16));
}

constexpr int mergeShorts(short a, short b)
{
	return (int)((unsigned int)(unsigned short)a | ((unsigned int)(unsigned short)b << 16));
}

constexpr unsigned char merge4bits(unsigned char a, unsigned char b)
{
	unsigned char rez = b & 0b1111;

	a = a & 0b1111;
	a <<= 4;

	rez |= a;

	return rez;
}

constexpr unsigned short mergeChars(unsigned char a, unsigned char b)
{
	unsigned short rez = b;

	unsigned short secondA = a;
	secondA <<= 8;

	rez |= secondA;

	return rez;
}