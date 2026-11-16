#include <rendering/snowCover.h>
#include <chunkSystem.h>
#include <blocks.h>
#include <gameplay/precipitation.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace snowCover
{

namespace
{
	// How deep the snow lies, in blocks: what the snow biome's ground always
	// has, and what a whole snowfall adds (anywhere it snows).
	constexpr float groundDepth = 0.14f;
	constexpr float fallenDepth = 0.18f;
	// Seconds of full snowfall until all of it lies, and until it has melted
	// once it stops; where it is cold (the snow biome, high up) it melts
	// slower.
	constexpr float secondsToLie = 150.f;
	constexpr float secondsToMelt = 240.f;
	constexpr float secondsToMeltCold = 600.f;
	// Seconds of full snowfall until a track pressed to the ground is full again.
	constexpr float secondsToFill = 110.f;
	// The mesh's height for a texel's value (snow.wgsl's trailHeight): a
	// track's bottom is this much of the snow's depth, packed.
	constexpr float packed = 0.12f;

	std::vector<glm::vec4> columns;
	glm::ivec2 corner = {};
	bool columnsKnown = false;
	bool anySnow = false;
	bool coldHere = false;      // at the camera, what falls is snow whatever the weather
	float sinceColumns = 0;

	std::vector<std::uint8_t> trail;
	std::vector<char> rowChanged;
	bool disturbed = false;     // some texel is not trailLevel
	glm::ivec2 window = {};     // the block at the tracks' square's low corner
	bool windowKnown = false;
	float fillTimer = 0;

	// How much of a snowfall lies: anywhere it snowed, and where it is cold
	// (there rain falls as snow too).
	float lyingFallen = 0;
	float lyingCold = 0;
	bool started = false;

	struct Body
	{
		glm::dvec3 last = {};
		glm::dvec2 lastStamp = {};
		bool known = false;    // seen in an earlier frame
		bool seen = false;     // this frame
		bool inSnow = false;
		bool left = false;     // which foot is next
		float travelled = 0;   // blocks since its last footprint
	};
	std::unordered_map<std::uint64_t, Body> bodies;

	int wrap(int texel)
	{
		return ((texel % trailSize) + trailSize) % trailSize;
	}

	bool holdsSnow(BlockType type)
	{
		if (type == BlockTypes::water) { return false; }
		if (isAnyLeaves(type)) { return true; }
		return isColidable(type) && isOpaque(type) && !isSlabMesh(type) && !isStairsMesh(type)
			&& !isWallMesh(type) && !isDecorativeFurniture(type);
	}

	const glm::vec4 *columnAt(int x, int z)
	{
		if (!columnsKnown) { return nullptr; }
		const int i = x - corner.x, j = z - corner.y;
		if (i < 0 || j < 0 || i >= columnsAcross || j >= columnsAcross) { return nullptr; }
		return &columns[i + j * columnsAcross];
	}

	void refreshColumns(glm::ivec3 cameraBlock, ChunkSystem &chunkSystem)
	{
		columns.assign(columnsAcross * columnsAcross, glm::vec4(0));
		corner = glm::ivec2(cameraBlock.x, cameraBlock.z) - glm::ivec2(radius + 1);
		columnsKnown = true;
		anySnow = false;
		coldHere = false;

		for (int j = 0; j < columnsAcross; j++)
			for (int i = 0; i < columnsAcross; i++)
			{
				const int x = corner.x + i;
				const int z = corner.y + j;
				Chunk *chunk = chunkSystem.getChunkSafeFromBlockPos(x, z);
				if (!chunk) { continue; }

				const int lx = modBlockToChunk(x);
				const int lz = modBlockToChunk(z);
				int y = CHUNK_HEIGHT - 1;
				for (; y >= 0; y--)
				{
					if (blockStopsRain(chunk->data.unsafeGet(lx, y, lz).getType())) { break; }
				}
				if (y < 0) { continue; }

				const BlockType type = chunk->data.unsafeGet(lx, y, lz).getType();
				const int kind = precipitationForColumn(chunk->data.biome, y + 1);
				if (x == cameraBlock.x && z == cameraBlock.z) { coldHere = kind == precipitationSnow; }

				float depth = 0;
				if (holdsSnow(type))
				{
					if (type == BlockTypes::snow_dirt || type == BlockTypes::snow_block) { depth += groundDepth; }
					if (kind == precipitationSnow) { depth += fallenDepth * std::max(lyingFallen, lyingCold); }
					else if (kind == precipitationRain) { depth += fallenDepth * lyingFallen; }
				}
				if (depth > 0.005f) { anySnow = true; } else { depth = 0; }

				// The light above it, as the faces have it baked.
				float light = 15 << 4;
				if (y + 1 < CHUNK_HEIGHT)
				{
					auto &above = chunk->data.unsafeGet(lx, y + 1, lz);
					light = (float)((above.getSkyLight() << 4) | above.getLight());
				}
				// A block is centred on its whole number: its top face is
				// half a block over it.
				columns[i + j * columnsAcross] = glm::vec4((float)y + 0.5f, depth, light, 0.f);
			}
	}

	void setTexel(int x, int z, std::uint8_t value)
	{
		std::uint8_t &v = trail[x + z * trailSize];
		if (v == value) { return; }
		v = value;
		rowChanged[z] = 1;
	}

	// The blocks from `first` on along x (or z) are new to the tracks'
	// square: what was drawn where they wrap to is smoothed over.
	void clearBlocks(bool alongX, int first, int count)
	{
		for (int b = first; b < first + count; b++)
		{
			const int start = wrap(b * trailTexelsPerBlock);
			for (int t = start; t < start + trailTexelsPerBlock; t++)
				for (int other = 0; other < trailSize; other++)
				{
					if (alongX) { setTexel(t, other, trailLevel); }
					else { setTexel(other, t, trailLevel); }
				}
		}
	}

	void moveWindow(glm::ivec3 cameraBlock)
	{
		const glm::ivec2 now = glm::ivec2(cameraBlock.x, cameraBlock.z) - glm::ivec2(trailBlocks / 2);
		if (!windowKnown)
		{
			window = now;
			windowKnown = true;
			return;
		}
		const glm::ivec2 step = now - window;
		if (step == glm::ivec2(0)) { return; }
		if (std::abs(step.x) >= trailBlocks || std::abs(step.y) >= trailBlocks)
		{
			for (int z = 0; z < trailSize; z++)
				for (int x = 0; x < trailSize; x++) { setTexel(x, z, trailLevel); }
			disturbed = false;
		}
		else
		{
			if (step.x > 0) { clearBlocks(true, window.x + trailBlocks, step.x); }
			if (step.x < 0) { clearBlocks(true, now.x, -step.x); }
			if (step.y > 0) { clearBlocks(false, window.y + trailBlocks, step.y); }
			if (step.y < 0) { clearBlocks(false, now.y, -step.y); }
		}
		window = now;
	}

	// New snow in the tracks: every texel a step back to untouched.
	void fill()
	{
		bool left = false;
		for (int z = 0; z < trailSize; z++)
		{
			std::uint8_t *row = &trail[z * trailSize];
			bool changed = false;
			for (int x = 0; x < trailSize; x++)
			{
				const std::uint8_t v = row[x];
				if (v == trailLevel) { continue; }
				row[x] = v < trailLevel ? v + 1 : v - 1;
				changed = true;
				if (row[x] != trailLevel) { left = true; }
			}
			if (changed) { rowChanged[z] = 1; }
		}
		disturbed = left;
	}

	// Presses the snow down round `at` (world x, z) to `level`, in an
	// ellipse `halfLong` along `direction` and `halfWide` across, soft at
	// its edge, and piles a rim `rim` high beside it where the snow is still
	// untouched. Only ever down inside, and up in the rim: the same place
	// twice changes nothing.
	void stamp(glm::dvec2 at, glm::vec2 direction, float halfLong, float halfWide, int level, int rim)
	{
		constexpr float rimOuter = 1.75f;
		const float reach = std::max(halfLong, halfWide) * (rim > 0 ? rimOuter : 1.f);
		// Texel n spans n..n+1 of these; block b starts at b - 0.5.
		const double tx = (at.x + 0.5) * trailTexelsPerBlock;
		const double tz = (at.y + 0.5) * trailTexelsPerBlock;
		const int x0 = (int)std::floor(tx - reach * trailTexelsPerBlock);
		const int x1 = (int)std::floor(tx + reach * trailTexelsPerBlock);
		const int z0 = (int)std::floor(tz - reach * trailTexelsPerBlock);
		const int z1 = (int)std::floor(tz + reach * trailTexelsPerBlock);
		if (x1 - x0 > 96 || z1 - z0 > 96) { return; }

		for (int z = z0; z <= z1; z++)
			for (int x = x0; x <= x1; x++)
			{
				const float dx = (float)((x + 0.5 - tx) / trailTexelsPerBlock);
				const float dz = (float)((z + 0.5 - tz) / trailTexelsPerBlock);
				const float along = (dx * direction.x + dz * direction.y) / halfLong;
				const float across = (-dx * direction.y + dz * direction.x) / halfWide;
				const float q = std::sqrt(along * along + across * across);
				if (q >= rimOuter) { continue; }

				const int wx = wrap(x), wz = wrap(z);
				const int v = trail[wx + wz * trailSize];
				if (q < 1.f)
				{
					float edge = glm::clamp((q - 0.62f) / 0.38f, 0.f, 1.f);
					edge = edge * edge * (3.f - 2.f * edge);
					const int target = (int)std::lround(level + (trailLevel - level) * edge);
					if (target < v) { setTexel(wx, wz, (std::uint8_t)std::max(target, 0)); disturbed = true; }
				}
				else if (rim > 0 && v >= trailLevel - 8)
				{
					const float bump = std::sin(3.14159265f * (q - 1.f) / (rimOuter - 1.f));
					const int target = (int)std::lround(trailLevel + rim * bump);
					if (target > v) { setTexel(wx, wz, (std::uint8_t)std::min(target, 255)); disturbed = true; }
				}
			}
	}

	// The texel value that puts the snow's surface `above` blocks over the
	// ground, where it is `depth` deep.
	int levelFor(float above, float depth)
	{
		const float height = glm::clamp(above / depth, packed, 1.f);
		return (int)std::lround((height - packed) / (1.f - packed) * trailLevel);
	}

	// OURCRAFT_SNOW_LYING=0..1 holds how much snow lies (for screenshots).
	float lyingFromEnvironment()
	{
		static const float value = []()
		{
			const char *env = std::getenv("OURCRAFT_SNOW_LYING");
			return env ? glm::clamp((float)std::atof(env), 0.f, 1.f) : -1.f;
		}();
		return value;
	}
}

void beginFrame(float deltaTime, glm::dvec3 camera, ChunkSystem &chunkSystem, float rain, float snow)
{
	if (trail.empty())
	{
		trail.assign((size_t)trailSize * trailSize, trailLevel);
		rowChanged.assign(trailSize, 1);
	}
	for (auto &b : bodies) { b.second.seen = false; }

	const glm::ivec3 cameraBlock = glm::ivec3(glm::floor(camera + glm::dvec3(0.5)));
	const float seconds = glm::clamp(deltaTime, 0.f, 0.25f);

	// It lies deeper while it snows, and melts when it has stopped.
	const float cold = std::max(rain, snow);
	if (!started)
	{
		// Into a snowfall already going: the snow is there.
		started = true;
		lyingFallen = snow;
		lyingCold = cold;
	}
	auto settle = [&](float &lying, float falling, float meltSeconds)
	{
		if (falling > 0.05f) { lying += falling * seconds / secondsToLie; }
		else { lying -= seconds / meltSeconds; }
		lying = glm::clamp(lying, 0.f, 1.f);
	};
	settle(lyingFallen, snow, secondsToMelt);
	settle(lyingCold, cold, secondsToMeltCold);
	if (lyingFromEnvironment() >= 0) { lyingFallen = lyingCold = lyingFromEnvironment(); }

	moveWindow(cameraBlock);

	// The columns follow the camera's block, and the blocks as they change.
	sinceColumns += deltaTime;
	if (!columnsKnown || sinceColumns >= 0.2f
		|| corner != glm::ivec2(cameraBlock.x, cameraBlock.z) - glm::ivec2(radius + 1))
	{
		sinceColumns = 0;
		refreshColumns(cameraBlock, chunkSystem);
	}

	// New snow fills the tracks.
	const float falling = coldHere ? cold : snow;
	if (disturbed && falling > 0.05f)
	{
		fillTimer += seconds * falling;
		const float step = secondsToFill / trailLevel;
		if (fillTimer >= step)
		{
			fillTimer = 0;
			fill();
		}
	}
}

void touch(std::uint64_t key, glm::dvec3 feet, glm::vec3 size)
{
	Body &body = bodies[key];
	body.seen = true;

	const glm::dvec2 at(feet.x, feet.z);
	const glm::vec4 *column = columnAt((int)std::floor(feet.x + 0.5), (int)std::floor(feet.z + 0.5));
	float depth = 0, above = 0;
	bool inSnow = false;
	if (column && column->y > 0.02f)
	{
		depth = column->y;
		above = (float)(feet.y - column->x);
		inSnow = above > -0.3f && above < depth + 0.05f;
	}

	const bool carried = !body.known || glm::length(feet - body.last) > 4.0;
	const bool wasInSnow = body.inSnow && !carried;
	const glm::dvec2 from(body.last.x, body.last.z);
	body.last = feet;
	body.known = true;
	body.inSnow = inSnow;
	if (!inSnow) { return; }

	const int level = levelFor(above, depth);
	const float across = std::max(size.x, size.z);

	// Something small (a dropped item) leaves a dent where it lies.
	if (size.y < 0.45f)
	{
		if (!wasInSnow || glm::length(at - body.lastStamp) > 0.04)
		{
			const float r = glm::clamp(across * 0.5f + 0.03f, 0.08f, 0.3f);
			stamp(at, {1, 0}, r, r, level, 6);
			body.lastStamp = at;
		}
		return;
	}

	// What walks: the player is 1.
	const float scale = glm::clamp(across / 0.8f, 0.35f, 1.6f);
	if (!wasInSnow)
	{
		// It lands, or steps in.
		stamp(at, {1, 0}, 0.3f * scale, 0.3f * scale, level, 16);
		body.lastStamp = at;
		body.travelled = 0;
		return;
	}

	const float moved = (float)glm::length(at - from);
	if (moved < 0.0001f) { return; }
	const glm::vec2 direction = glm::vec2(at - from) / moved;

	// Deep snow it wades through: a furrow behind it, deeper the deeper the
	// snow. Stamped every sixteenth of a block along its way.
	const float wade = glm::smoothstep(0.1f, 0.26f, depth);
	if (wade > 0.01f)
	{
		constexpr double spacing = 1.0 / 16.0;
		const glm::dvec2 toHere = at - body.lastStamp;
		const double gap = glm::length(toHere);
		const int count = std::min((int)(gap / spacing), 64);
		const int furrow = std::max(level, (int)std::lround(glm::mix((float)trailLevel, 52.f, wade)));
		for (int k = 1; k <= count; k++)
		{
			const glm::dvec2 p = body.lastStamp + toHere / gap * (spacing * k);
			stamp(p, direction, 0.21f * scale, 0.21f * scale, furrow, 9);
		}
		if (count > 0) { body.lastStamp = body.lastStamp + toHere / gap * (spacing * count); }
		if (gap > 4.0) { body.lastStamp = at; }
	}
	else
	{
		body.lastStamp = at;
	}

	// A footprint a stride: left, right.
	body.travelled += moved;
	if (body.travelled >= 0.55f * scale)
	{
		body.travelled = 0;
		body.left = !body.left;
		const glm::dvec2 side = glm::dvec2(-direction.y, direction.x) * (double)(0.13f * scale * (body.left ? 1.f : -1.f));
		stamp(at + side, direction, 0.17f * scale, 0.085f * scale, level, 12);
	}
}

void endFrame()
{
	for (auto it = bodies.begin(); it != bodies.end();)
	{
		if (!it->second.seen) { it = bodies.erase(it); }
		else { ++it; }
	}
}

float lying()
{
	return lyingFallen;
}

void reset()
{
	bodies.clear();
	columnsKnown = false;
	anySnow = false;
	windowKnown = false;
	started = false;
	lyingFallen = lyingCold = 0;
	if (!trail.empty())
	{
		std::fill(trail.begin(), trail.end(), trailLevel);
		std::fill(rowChanged.begin(), rowChanged.end(), 1);
	}
	disturbed = false;
}

Layer layer()
{
	Layer l;
	l.corner = corner;
	l.columns = columnsKnown ? &columns : nullptr;
	l.any = columnsKnown && anySnow;
	l.trail = trail.empty() ? nullptr : trail.data();
	for (int z = 0; z < (int)rowChanged.size();)
	{
		if (!rowChanged[z]) { z++; continue; }
		int end = z;
		while (end < (int)rowChanged.size() && rowChanged[end]) { rowChanged[end] = 0; end++; }
		l.changedRows.push_back({z, end - z});
		z = end;
	}
	return l;
}

}
