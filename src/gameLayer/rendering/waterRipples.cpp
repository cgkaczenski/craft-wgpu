#include <rendering/waterRipples.h>
#include <rendering/camera.h>
#include <chunkSystem.h>
#include <blocks.h>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace waterRipples
{

namespace
{
	// Where a body is by the water at its place.
	enum Side { above, touching, under };

	struct Body
	{
		glm::dvec3 last = {};
		int side = above;
		bool known = false;   // seen in an earlier frame, so `last` and `side` mean something
		bool seen = false;    // this frame
		float sinceRing = 0;  // seconds
		float travelled = 0;  // blocks along the surface since its last ring
	};

	std::unordered_map<std::uint64_t, Body> bodies;
	std::vector<Ring> allRings;

	// The water's surface sits this far under its block's top (the faces'
	// 0.08 in chunkFaces.wgsl), and a block's top is half a block above its
	// position.
	constexpr double surfaceInBlock = 0.5 - 0.08;

	bool isWater(ChunkSystem &chunkSystem, int x, int y, int z)
	{
		Block *b = chunkSystem.getBlockSafe(x, y, z);
		return b && b->getType() == BlockTypes::water;
	}

	// The height of the water's surface in the column at x, z, looking at
	// the blocks from `fromY` to `toY`: water with no water over it.
	bool surfaceBetween(ChunkSystem &chunkSystem, int x, int z, int fromY, int toY, double &surface)
	{
		for (int y = fromY; y <= toY; y++)
		{
			if (isWater(chunkSystem, x, y, z) && !isWater(chunkSystem, x, y + 1, z))
			{
				surface = y + surfaceInBlock;
				return true;
			}
		}
		return false;
	}

	void add(const Ring &ring)
	{
		if ((int)allRings.size() >= maxRings)
		{
			// The one nearest its end makes room.
			auto oldest = std::max_element(allRings.begin(), allRings.end(),
				[](const Ring &a, const Ring &b) { return a.age / a.life < b.age / b.life; });
			*oldest = ring;
			return;
		}
		allRings.push_back(ring);
	}
}

void beginFrame(float deltaTime)
{
	for (auto &r : allRings) { r.age += deltaTime; }
	allRings.erase(std::remove_if(allRings.begin(), allRings.end(),
		[](const Ring &r) { return r.age >= r.life; }), allRings.end());
	for (auto &b : bodies) { b.second.seen = false; }
}

void touch(std::uint64_t key, glm::dvec3 feet, float height, ChunkSystem &chunkSystem, float deltaTime)
{
	Body &body = bodies[key];
	body.seen = true;

	const glm::dvec3 head = feet + glm::dvec3(0, height, 0);
	const glm::ivec3 feetBlock = from3DPointToBlock(feet);
	const glm::ivec3 headBlock = from3DPointToBlock(head);

	// Where it is by the water: the surface crosses it, or it is wholly
	// under (then the surface is somewhere over its head), or out of it.
	int side = above;
	double surface = 0;
	if (surfaceBetween(chunkSystem, feetBlock.x, feetBlock.z, feetBlock.y, headBlock.y, surface)
		&& surface >= feet.y - 0.05 && surface <= head.y + 0.05)
	{
		side = touching;
	}
	else if (isWater(chunkSystem, headBlock.x, headBlock.y, headBlock.z))
	{
		side = under;
		if (!surfaceBetween(chunkSystem, headBlock.x, headBlock.z, headBlock.y, headBlock.y + 3, surface))
		{
			surface = head.y; // too deep to matter: nothing below uses it
		}
	}

	// A body the size of the player makes rings of full size; a dropped
	// item's are a third of that.
	const float size = std::clamp(height / 1.8f, 0.3f, 1.2f);
	const glm::dvec3 moved = body.known ? feet - body.last : glm::dvec3(0);
	const float seconds = std::max(deltaTime, 0.001f);
	// Carried from far away (a respawn, a teleport) is not a movement.
	const bool carried = glm::length(moved) > 8.0;

	if (body.known && !carried)
	{
		const float verticalSpeed = (float)std::abs(moved.y) / seconds;
		const bool wentIn = body.side == above && side != above;
		const bool cameOut = body.side != above && side == above;
		const bool through = body.side == under && side == touching && verticalSpeed > 2.f;

		if (wentIn || cameOut || through)
		{
			// Coming out, the surface is where it was a frame ago.
			double at = surface;
			if (cameOut)
			{
				const glm::ivec3 lastBlock = from3DPointToBlock(body.last);
				if (!surfaceBetween(chunkSystem, lastBlock.x, lastBlock.z, lastBlock.y - 1, lastBlock.y + 3, at))
				{
					at = feet.y;
				}
			}
			Ring ring;
			ring.position = glm::dvec3(feet.x, at, feet.z);
			ring.splash = true;
			ring.size = 0.45f * size;
			// Harder the faster it falls; coming out, less.
			ring.strength = std::clamp(0.45f + verticalSpeed * 0.07f, 0.45f, 1.2f) * (wentIn ? 1.f : 0.7f)
				* (0.6f + 0.4f * size);
			ring.life = 1.6f + 0.9f * size;
			add(ring);
			body.sinceRing = 0;
			body.travelled = 0;
		}
		else if (side == touching)
		{
			// A wake: a ring every half block along the surface, stronger
			// the faster it goes. Still, a faint ring now and then (not
			// round something as small as a dropped item, which lies still).
			body.sinceRing += deltaTime;
			body.travelled += (float)glm::length(glm::dvec2(moved.x, moved.z));
			const float speed = (float)glm::length(glm::dvec2(moved.x, moved.z)) / seconds;
			if (body.travelled >= 0.5f && body.sinceRing >= 0.1f)
			{
				Ring ring;
				ring.position = glm::dvec3(feet.x, surface, feet.z);
				ring.size = 0.3f * size;
				ring.strength = std::clamp(0.55f + speed * 0.12f, 0.55f, 1.f) * (0.6f + 0.4f * size);
				ring.life = 1.5f;
				add(ring);
				body.sinceRing = 0;
				body.travelled = 0;
			}
			else if (body.sinceRing >= 1.5f && height > 0.6f)
			{
				Ring ring;
				ring.position = glm::dvec3(feet.x, surface, feet.z);
				ring.size = 0.3f * size;
				ring.strength = 0.4f;
				ring.life = 1.8f;
				add(ring);
				body.sinceRing = 0;
				body.travelled = 0;
			}
		}
	}

	body.last = feet;
	body.side = side;
	body.known = true;
}

void endFrame()
{
	for (auto it = bodies.begin(); it != bodies.end();)
	{
		if (!it->second.seen) { it = bodies.erase(it); }
		else { ++it; }
	}
}

const std::vector<Ring> &rings()
{
	return allRings;
}

}
