#include <rendering/debris.h>
#include <chunkSystem.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <random>

namespace debris
{

namespace
{
	std::vector<Piece> all;
	std::minstd_rand rng(1234);

	// Enough for a few breaks at once; the oldest go first past it.
	constexpr size_t maxPieces = 600;

	constexpr float gravity = 20.f;     // blocks a second squared
	constexpr float airDrag = 0.6f;     // of the speed lost a second
	constexpr float shrinkSeconds = 0.25f;

	float random01() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); }

	glm::vec3 randomInSphere()
	{
		for (;;)
		{
			const glm::vec3 v(random01() * 2 - 1, random01() * 2 - 1, random01() * 2 - 1);
			if (glm::dot(v, v) <= 1.f) { return v; }
		}
	}

	bool solidAt(ChunkSystem &chunkSystem, glm::dvec3 position)
	{
		Block *b = chunkSystem.getBlockSafe(position);
		return b && b->isColidable();
	}
}

void spawn(glm::dvec3 at, glm::vec3 away, int count, float speed, float size,
	std::uint16_t blockType, glm::vec3 colour)
{
	if (count <= 0) { return; }
	const float awayLength = glm::length(away);
	const glm::vec3 direction = awayLength > 0 ? away / awayLength : glm::vec3(0, 1, 0);

	for (int i = 0; i < count; i++)
	{
		if (all.size() >= maxPieces) { all.erase(all.begin()); }
		Piece p;
		p.position = at + glm::dvec3(randomInSphere() * 0.08f);
		// Mostly away from what was struck, some sideways, a little up.
		glm::vec3 throwDirection = direction + randomInSphere() * 0.9f + glm::vec3(0, 0.35f, 0);
		const float l = glm::length(throwDirection);
		throwDirection = l > 0 ? throwDirection / l : glm::vec3(0, 1, 0);
		p.velocity = throwDirection * speed * (0.35f + 0.65f * random01());
		p.spin = glm::vec3(random01(), random01(), 0) * 6.2831853f;
		p.spinSpeed = glm::vec3(random01() * 2 - 1, random01() * 2 - 1, 0) * 14.f;
		p.size = size * (0.45f + 0.55f * random01());
		p.life = 0.7f + 0.9f * random01();
		p.blockType = blockType;
		p.quarter = (unsigned char)(rng() & 15u);
		// A little variation in the flat colours.
		p.colour = colour * (0.8f + 0.35f * random01());
		all.push_back(p);
	}
}

void update(float deltaTime, ChunkSystem &chunkSystem)
{
	deltaTime = glm::clamp(deltaTime, 0.f, 0.1f);

	for (Piece &p : all)
	{
		p.age += deltaTime;
		if (p.resting) { continue; }

		p.velocity.y -= gravity * deltaTime;
		p.velocity *= std::max(0.f, 1.f - airDrag * deltaTime);
		p.spin += p.spinSpeed * deltaTime;

		// Axis by axis, so a piece slides along a wall or the ground.
		const float half = currentSize(p) * 0.5f;
		for (int axis = 0; axis < 3; axis++)
		{
			glm::dvec3 next = p.position;
			next[axis] += p.velocity[axis] * deltaTime;
			glm::dvec3 edge = next;
			edge[axis] += (p.velocity[axis] < 0 ? -half : half);
			if (!solidAt(chunkSystem, edge))
			{
				p.position = next;
				continue;
			}

			if (axis == 1 && p.velocity.y < 0)
			{
				// Down onto a block: settle on its top (blocks are centred on
				// whole numbers, their tops at .5), bounce a little, and the
				// ground's friction slows the slide.
				const double top = std::floor(edge.y + 0.5) + 0.5;
				p.position.y = top + half + 0.001;
				p.velocity.y = -p.velocity.y * 0.3f;
				p.velocity.x *= 0.55f;
				p.velocity.z *= 0.55f;
				p.spinSpeed *= 0.5f;
				if (std::abs(p.velocity.y) < 0.6f && glm::length(glm::vec2(p.velocity.x, p.velocity.z)) < 0.25f)
				{
					p.velocity = {};
					p.resting = true;
				}
			}
			else
			{
				p.velocity[axis] = -p.velocity[axis] * 0.3f;
			}
		}
	}

	all.erase(std::remove_if(all.begin(), all.end(), [](const Piece &p) { return p.age >= p.life; }), all.end());
}

const std::vector<Piece> &pieces()
{
	return all;
}

float currentSize(const Piece &piece)
{
	const float left = piece.life - piece.age;
	return piece.size * glm::clamp(left / shrinkSeconds, 0.f, 1.f);
}

void clear()
{
	all.clear();
}

}
