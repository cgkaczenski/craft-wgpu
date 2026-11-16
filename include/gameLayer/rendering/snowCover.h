#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

struct ChunkSystem;

// Snow that lies on the ground, around the camera: how deep it is on each
// block column (it builds up while it snows and melts after; the snow
// biome's ground always has some), and the tracks in it. The tracks are a
// picture of the snow's surface seen from above, kept here on the CPU: what
// walks through the snow presses it down and piles a rim beside, and new
// snow fills the tracks again. The renderer tells this where the bodies are
// each frame, as it tells waterRipples; snow.wgsl draws the snow as a mesh
// lifted by the depth and the tracks.
namespace snowCover
{
	// The columns the mesh covers reach this far from the camera's block;
	// the map has one more all round, for the mesh's edges to look at.
	constexpr int radius = 24;
	constexpr int columnsAcross = radius * 2 + 3;

	// The tracks: a square of the world that wraps (a block's place in it is
	// its position modulo trailBlocks), so nothing moves when the camera
	// does; what leaves the square round the camera is smoothed over.
	constexpr int trailBlocks = 64;
	constexpr int trailTexelsPerBlock = 16;
	constexpr int trailSize = trailBlocks * trailTexelsPerBlock;
	// A texel's value: this is untouched snow, less pressed down, more piled up.
	constexpr std::uint8_t trailLevel = 128;

	// Once a frame, before the bodies. `rain` and `snow` are the weather's
	// (WeatherLook), 0..1.
	void beginFrame(float deltaTime, glm::dvec3 camera, ChunkSystem &chunkSystem, float rain, float snow);

	// A body this frame: `feet` the middle of its base, `size` its collider
	// (x and z across, y tall), in blocks. `key` tells it from the others.
	void touch(std::uint64_t key, glm::dvec3 feet, glm::vec3 size);

	// Once a frame, after the bodies: the ones not seen are forgotten.
	void endFrame();

	// How much of the snow weather's snow lies on the ground, 0..1: it stays
	// a while after the snowfall.
	float lying();

	// Forget everything (another world).
	void reset();

	// What the renderer draws this frame.
	struct Layer
	{
		glm::ivec2 corner = {};  // world block x, z of column (0, 0)
		// Per column, x fastest: x the world y of the top face the snow lies
		// on, y how deep the snow is there in blocks (0 none), z the light
		// above it as baked (sky light << 4 | block light).
		const std::vector<glm::vec4> *columns = nullptr;
		bool any = false;        // some column has snow
		const std::uint8_t *trail = nullptr; // trailSize squared, x fastest
		// The rows of `trail` changed since the last call: first, count.
		std::vector<glm::ivec2> changedRows;
	};
	Layer layer();
}
