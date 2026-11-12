#pragma once
#include <glm/vec3.hpp>
#include <deque>

//donotupdatelightsystem
//donotoptimizelightsystem
// 1 turns the light system off: every face is baked as under the open sky,
// caves included, and light blocks give no light. On since the sky light
// is filled in a column at a time (setSunlightForAnEntireChunk).
bool constexpr dontUpdateLightSystem = 0;

struct ChunkSystem;
struct Chunk;

struct LightSystem
{
	struct Light
	{
		glm::ivec3 pos = {};
		char intensity = 0; //0..15
	};

	std::deque<Light> sunLigtsToAdd;
	std::deque<Light> sunLigtsToRemove;

	std::deque<Light> ligtsToAdd;
	std::deque<Light> ligtsToRemove;

	void update(ChunkSystem &chunkSystem);
	
	void addSunLight(ChunkSystem &chunkSystem, glm::ivec3 pos, char intensity);
	void addSunLightAndPropagateDown(ChunkSystem &chunkSystem, glm::ivec3 pos, char intensity);
	void removeSunLight(ChunkSystem &chunkSystem, glm::ivec3 pos, char oldVal);

	void addLight(ChunkSystem &chunkSystem, glm::ivec3 pos, char intensity);
	void removeLight(ChunkSystem &chunkSystem, glm::ivec3 pos, char oldVal);

	void setSunlightForAnEntireChunk(Chunk &chunk, ChunkSystem &chunkSystem);
};