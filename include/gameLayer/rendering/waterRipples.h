#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

struct ChunkSystem;

// Rings on the water where something meets its surface: a splash where a
// body goes in or comes out, a wake behind one that moves along the surface,
// and a faint ring now and then round one that stays in it. The renderer
// tells this where the bodies are each frame (the player, the other players,
// the animals and monsters, the dropped items); chunkFaces.wgsl draws the
// rings as foam (rippleFoam).
namespace waterRipples
{
	// As many as the shader takes (world.wgsl's ripples).
	constexpr int maxRings = 32;

	struct Ring
	{
		glm::dvec3 position = {}; // its middle, on the surface
		float age = 0;            // seconds
		float life = 1;           // seconds until it is gone
		float strength = 1;       // how much foam, 0..1 and a little over
		float size = 1;           // its radius when it starts, in blocks
		bool splash = false;      // a splash's churned middle, not only a ring
	};

	// Once a frame, before the bodies: the rings grow older.
	void beginFrame(float deltaTime);

	// A body this frame: `feet` the middle of its base, `height` how tall
	// it is, in blocks. `key` tells it from the others from frame to frame.
	void touch(std::uint64_t key, glm::dvec3 feet, float height, ChunkSystem &chunkSystem, float deltaTime);

	// Once a frame, after the bodies: the ones not seen are forgotten.
	void endFrame();

	const std::vector<Ring> &rings();
}
