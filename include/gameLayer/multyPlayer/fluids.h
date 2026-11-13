#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>
#include <unordered_map>
#include <cstdint>
#include <blocks.h>

struct ServerChunkStorer;

// Flowing water, run by the server. Water's level lives in the block's flags
// (Block::getWaterLevel): 0 a source, 1-7 how far it has flowed. A changed
// block wakes the water around it, which re-checks itself a few ticks later:
// flowing water takes its level from the water feeding it (or drains away
// with nothing feeding it), then flows down, or sideways where it can't.

// Ticks between a change and the water around it responding (Minecraft's 5).
constexpr int WATER_FLOW_DELAY_TICKS = 5;

// How many water blocks one chunk region updates per tick at most; the rest
// wait for the next tick.
constexpr int WATER_UPDATES_PER_TICK = 512;

// Called once per server tick, before the regions tick.
void advanceWaterTick();

// The block at pos, its six neighbours and the four beside the block above
// it, those that are water, re-check themselves after WATER_FLOW_DELAY_TICKS.
void wakeWaterAround(ServerChunkStorer &chunkCache, glm::ivec3 pos);

// The block at pos re-checks itself (if it is water) after delay ticks.
void scheduleWaterUpdate(ServerChunkStorer &chunkCache, glm::ivec3 pos,
	int delay = WATER_FLOW_DELAY_TICKS);

// A random tick landed on this water block: wake it if it isn't settled
// (flowing water, or air below or beside it). Settles world generation's
// standing walls of water near players, and flows cut short by a save.
void randomTickWater(ServerChunkStorer &chunkCache, glm::ivec3 pos);

// Runs the water updates that are due in this region, changed blocks going
// to modifiedBlocks (sent to every client at the end of the tick).
void updateWater(ServerChunkStorer &chunkCache,
	std::unordered_map<glm::ivec3, Block> &modifiedBlocks);
