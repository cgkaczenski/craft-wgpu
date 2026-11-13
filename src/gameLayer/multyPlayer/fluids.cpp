#include <multyPlayer/fluids.h>
#include <multyPlayer/serverChunkStorer.h>
#include <metrics.h>
#include <atomic>
#include <vector>
#include <algorithm>

namespace
{

// Counts server ticks. Advanced on the main thread before the regions tick,
// read by the region threads.
std::atomic<std::uint64_t> waterTick{0};

const glm::ivec3 sideways[4] = {{0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}};

std::uint32_t packWaterPosition(int x, int y, int z)
{
	return (std::uint32_t)x | ((std::uint32_t)z << 4) | ((std::uint32_t)y << 8);
}

glm::ivec3 unpackWaterPosition(std::uint32_t p, glm::ivec2 chunk)
{
	int x = p & 0xF;
	int z = (p >> 4) & 0xF;
	int y = p >> 8;
	return {x + chunk.x * CHUNK_SIZE, y, z + chunk.y * CHUNK_SIZE};
}

bool isWater(Block *b)
{
	return b && b->getType() == BlockTypes::water;
}

// Water flows into air only (not into plants or other non-solid blocks yet).
bool canFlowInto(Block *b)
{
	return b && b->air();
}

// The water at pos would go down rather than sideways: below it is air, or
// water that isn't a source.
bool canFlowDown(ServerChunkStorer &chunkCache, glm::ivec3 pos)
{
	Block *below = chunkCache.getBlockSafe(pos + glm::ivec3(0, -1, 0));
	return below && (below->air() || (isWater(below) && !below->isWaterSource()));
}

Block makeWater(int level, bool falling)
{
	Block w;
	w.setType(BlockTypes::water);
	w.setWaterLevelAndFalling(level, falling);
	return w;
}

void setBlock(ServerChunkStorer &chunkCache, std::unordered_map<glm::ivec3, Block> &modifiedBlocks,
	glm::ivec3 pos, Block block)
{
	SavedChunk *c = 0;
	Block *b = chunkCache.getBlockSafeAndChunk(pos, c);
	if (!b || !c) { return; }

	block.lightLevel = b->lightLevel; // the clients relight it
	*b = block;
	c->otherData.dirty = true;
	modifiedBlocks[pos] = *b;

	wakeWaterAround(chunkCache, pos);
}

void updateWaterBlock(ServerChunkStorer &chunkCache, std::unordered_map<glm::ivec3, Block> &modifiedBlocks,
	glm::ivec3 pos)
{
	Block *b = chunkCache.getBlockSafe(pos);
	if (!isWater(b)) { return; }

	Block current = *b;

	// Flowing water: what it should be, from the water feeding it.
	if (!current.isWaterSource())
	{
		Block next = current;

		Block *above = chunkCache.getBlockSafe(pos + glm::ivec3(0, 1, 0));
		if (isWater(above))
		{
			next.setWaterLevelAndFalling(0, true);
		}
		else
		{
			int best = 8;
			bool unknown = false; // a neighbour in a chunk that isn't loaded

			for (auto d : sideways)
			{
				Block *n = chunkCache.getBlockSafe(pos + d);
				if (!n) { unknown = true; continue; }
				if (!isWater(n)) { continue; }

				// Water feeds its sides when it is a source, or can't go down.
				bool feeds = n->isWaterSource() || !canFlowDown(chunkCache, pos + d);
				if (!feeds) { continue; }

				int level = (n->isWaterSource() || n->getWaterFalling()) ? 0 : n->getWaterLevel();
				best = std::min(best, level + 1);
			}

			if (best > 7)
			{
				// Nothing feeds it: drain, unless the feeder may be in an
				// unloaded chunk.
				if (!unknown) { setBlock(chunkCache, modifiedBlocks, pos, Block{}); }
				return;
			}

			next.setWaterLevelAndFalling(best, false);
		}

		if (next.typeAndFlags != current.typeAndFlags)
		{
			setBlock(chunkCache, modifiedBlocks, pos, next);
		}
		current = next;
	}

	// Spread: down first.
	glm::ivec3 belowPos = pos + glm::ivec3(0, -1, 0);
	if (canFlowInto(chunkCache.getBlockSafe(belowPos)))
	{
		setBlock(chunkCache, modifiedBlocks, belowPos, makeWater(0, true));
	}

	// Then sideways, from a source, or water that can't go down.
	if (current.isWaterSource() || !canFlowDown(chunkCache, pos))
	{
		int level = (current.isWaterSource() || current.getWaterFalling()) ? 1 : current.getWaterLevel() + 1;
		if (level <= 7)
		{
			for (auto d : sideways)
			{
				if (canFlowInto(chunkCache.getBlockSafe(pos + d)))
				{
					setBlock(chunkCache, modifiedBlocks, pos + d, makeWater(level, false));
				}
			}
		}
	}
}

}

void advanceWaterTick()
{
	waterTick++;
}

void scheduleWaterUpdate(ServerChunkStorer &chunkCache, glm::ivec3 pos, int delay)
{
	SavedChunk *c = 0;
	Block *b = chunkCache.getBlockSafeAndChunk(pos, c);
	if (!b || !c) { return; }

	std::uint32_t key = packWaterPosition(modBlockToChunk(pos.x), pos.y, modBlockToChunk(pos.z));
	std::uint64_t due = waterTick.load() + delay;

	auto [it, inserted] = c->waterUpdates.try_emplace(key, due);
	if (!inserted && it->second > due) { it->second = due; }
}

void wakeWaterAround(ServerChunkStorer &chunkCache, glm::ivec3 pos)
{
	// The block and its six neighbours, and the four beside the block above:
	// whether that block can flow down decides if it feeds them.
	const glm::ivec3 around[11] = {{0,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0},
		{0,1,1}, {0,1,-1}, {1,1,0}, {-1,1,0}};
	for (auto d : around)
	{
		if (isWater(chunkCache.getBlockSafe(pos + d)))
		{
			scheduleWaterUpdate(chunkCache, pos + d);
		}
	}
}

void randomTickWater(ServerChunkStorer &chunkCache, glm::ivec3 pos)
{
	Block *b = chunkCache.getBlockSafe(pos);
	if (!isWater(b)) { return; }

	bool unsettled = !b->isWaterSource()
		|| canFlowInto(chunkCache.getBlockSafe(pos + glm::ivec3(0, -1, 0)));

	for (auto d : sideways)
	{
		if (unsettled) { break; }
		unsettled = canFlowInto(chunkCache.getBlockSafe(pos + d));
	}

	if (unsettled) { scheduleWaterUpdate(chunkCache, pos, 1); }
}

void updateWater(ServerChunkStorer &chunkCache,
	std::unordered_map<glm::ivec3, Block> &modifiedBlocks)
{
	std::uint64_t now = waterTick.load();

	static thread_local std::vector<glm::ivec3> due;
	due.clear();

	for (auto &c : chunkCache.savedChunks)
	{
		auto &updates = c.second->waterUpdates;
		for (auto it = updates.begin(); it != updates.end(); )
		{
			if (due.size() >= WATER_UPDATES_PER_TICK) { break; }

			if (it->second <= now)
			{
				due.push_back(unpackWaterPosition(it->first, c.first));
				it = updates.erase(it);
			}
			else
			{
				++it;
			}
		}

		if (due.size() >= WATER_UPDATES_PER_TICK) { break; }
	}

	for (auto pos : due)
	{
		updateWaterBlock(chunkCache, modifiedBlocks, pos);
	}
}
