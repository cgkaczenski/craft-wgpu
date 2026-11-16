#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <gl2d/gl2d.h>
#include <unordered_map>

struct ProgramData;
struct ChunkSystem;

struct MapEngine
{
	
	void close();
	
	// playerPosition and viewDirection place and turn the player's arrow.
	void update(ProgramData &programData, float deltaTime, glm::ivec2 chunkPos,
		ChunkSystem &chunkSystem, glm::dvec3 playerPosition, glm::vec3 viewDirection);
	
	void open(ProgramData &programData, glm::ivec2 chunkPos, ChunkSystem &chunkSystem);

	glm::vec2 centerPos = {};
	gl2d::Camera camera;

	struct MapChunk
	{
		gl2d::Texture t;

		void cleanup();
	};

	std::unordered_map<glm::ivec2, MapChunk> mapChunks;

	glm::ivec2 mouseHovered = {};

	// The block column under the cursor, its top block's height (-1 when that
	// chunk isn't loaded) and type.
	glm::ivec2 hoveredBlock = {};
	int hoveredTop = -1;
	int hoveredType = 0;

	// A white pixel for the player's arrow (renderTriangles takes a texture),
	// and the clock its ring pulses by.
	gl2d::Texture whitePixel;
	float time = 0;
	std::uint64_t mouseHoveredId = UINT64_MAX;


};