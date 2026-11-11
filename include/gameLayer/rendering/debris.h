#pragma once
#include <cstdint>
#include <vector>
#include <glm/vec3.hpp>

struct ChunkSystem;

// Milestone G4: debris, the pieces a strike knocks off (gameplay/impact.h).
// Small spinning cubes, drawn as wgpuWorld's block cubes (the dropped items'
// renderer) at their own size and turn, lit where they are like the
// dropped items. A piece of a block shows a quarter of the block's texture,
// a piece of an entity a flat colour. They fall, bounce on solid blocks,
// slide to a stop and shrink away.
//
// Client side only: presentation of the local player's strikes.
namespace debris
{
	struct Piece
	{
		glm::dvec3 position = {};
		glm::vec3 velocity = {};
		glm::vec3 spin = {};       // yaw, pitch now, radians
		glm::vec3 spinSpeed = {};  // radians a second
		float size = 0.1f;         // edge, blocks
		float age = 0;
		float life = 1;            // seconds
		std::uint16_t blockType = 0; // the texture, or 0 for `colour`
		unsigned char quarter = 0;   // which quarter of the texture: u in bits 0-1, v in 2-3
		bool resting = false;
		glm::vec3 colour = {1, 1, 1};
	};

	// `count` pieces from `at`, thrown around `away` (a direction) at up to
	// `speed`, up to `size` big.
	void spawn(glm::dvec3 at, glm::vec3 away, int count, float speed, float size,
		std::uint16_t blockType, glm::vec3 colour);

	// Once a frame: gravity, bouncing on the world's solid blocks, ageing.
	void update(float deltaTime, ChunkSystem &chunkSystem);

	// What there is to draw: pieces at their current size (they shrink at
	// the end of their life).
	const std::vector<Piece> &pieces();
	float currentSize(const Piece &piece);

	void clear();
}
