#pragma once
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <chunk.h>
#include <cmath>



struct ColidableEntry
{
	std::uint64_t eid = 0;
	glm::dvec3 position = {};
	glm::vec3 collider = {};
};


struct PhysicalSettings
{

	float gravityModifier = 1;
	float sideFriction = 0;

	// Buoyancy (applyBuoyancy). The body's density by water's: under 1 it
	// floats, with that share of its height under the surface; over 1 it
	// sinks, slower than in air. 0: water does nothing to it.
	float density = 0;
	// How fast water slows it, per second, wholly under.
	float waterDrag = 0;

};

constexpr static float MAX_AIR_DRAG = 12.f;
constexpr static float AIR_DRAG_COEFICIENT = 0.1f;
constexpr static float GRAVITY = -9.8 * 2.9;
constexpr static float MAX_ACCELERATION = 1000;
constexpr static float MAX_VELOCITY = 1000;
constexpr static float BASIC_JUMP_IMPULSE = 9.4;
constexpr static float OTHERS_PUSHING_YOU_FORCE = 0.7f;
constexpr static float OTHERS_PUSHING_YOU_BUAS_Y_DOWN = 0.1f;



ChunkData *chunkGetterSignature(glm::ivec2 chunkPos);

struct MotionState
{
	glm::vec3 velocity = {};
	glm::vec3 acceleration = {};
	unsigned char colides = {}; //front back top bottom left right

	bool colidesTopBottom() { return colidesTop() || colidesBottom(); }
	bool colidesLeftRight() { return colidesLeft() || colidesRight(); }
	bool colidesFrontBack() { return colidesFront() || colidesBack(); }

	bool colidesFront() { return colides & 0b100000; }
	bool colidesBack() { return colides &  0b010000; }
	bool colidesTop() { return colides &   0b001000; }
	bool colidesBottom() { return colides &0b000100; }
	bool colidesLeft() { return colides &  0b000010; }
	bool colidesRight() { return colides & 0b000001; }

	void setColidesFront(bool b) { colides ^= 0b100000; }
	void setColidesBack(bool b) { colides ^=  0b010000; }
	void setColidesTop(bool b) { colides ^=   0b001000; }
	void setColidesBottom(bool b) { colides ^=0b000100; }
	void setColidesLeft(bool b) { colides ^=  0b000010; }
	void setColidesRight(bool b) { colides ^= 0b000001; }

	void jump(float impulse = BASIC_JUMP_IMPULSE);
};


//returns false if chunk was not loaded
bool resolveConstrains(
	glm::dvec3 &pos, glm::dvec3 &lastPos,
	decltype(chunkGetterSignature) *chunkGetter,
	MotionState *forces, float deltaTime, glm::vec3 colliderSize, PhysicalSettings physicalSettings = {});

//returns false if chunk was not loaded
bool checkCollisionBrute(glm::dvec3 &pos, glm::dvec3 lastPos,
	decltype(chunkGetterSignature) *chunkGetter, MotionState *forces, float deltaTime,
	glm::vec3 colliderSize, PhysicalSettings physicalSettings);


glm::dvec3 performCollision(glm::dvec3 pos, glm::dvec3 lastPos, glm::vec3 size, glm::dvec3 delta,
	decltype(chunkGetterSignature) *chunkGetter, bool &chunkLoaded, MotionState *forces, float deltaTime,
	glm::vec3 &drag, PhysicalSettings physicalSettings);

void updateForces(glm::dvec3 &pos, MotionState &forces, float deltaTime, bool applyGravity, PhysicalSettings physicalSettings = {});

void updateForces(glm::dvec3 &pos, glm::vec3 &velocity, glm::vec3 &acceleration, float deltaTime, bool applyGravity, PhysicalSettings physicalSettings = {});

void applyImpulse(MotionState &force, glm::vec3 impulse, float mass = 1.f);


// ---- Water -----------------------------------------------------------------
// Still water's surface is this far above its block's position (a block's
// top is half a block above it, and the surface sits 0.08 under that), and
// flowing water's is lower by this much for each of its levels: the numbers
// chunkFaces.wgsl draws it by. The waves are not in it: they are drawn, and
// what floats rides them when drawn (waterWaves.h), but the physics runs on
// the server too, which has no waves.
constexpr double WATER_SURFACE_IN_BLOCK = 0.5 - 0.08;
constexpr double WATER_LEVEL_DROP = 0.11;

// The surface of the water a body is in: `base` the middle of its bottom,
// `height` how tall it is. `waterAt(x, y, z)` says what a block is: -1 not
// water, else flowing water's level (0 for a source or falling water).
// False if no part of the body is in water.
template<class WaterAt>
bool waterSurfaceAround(glm::dvec3 base, float height, WaterAt &&waterAt, double &surface)
{
	const int x = (int)std::floor(base.x + 0.5);
	const int z = (int)std::floor(base.z + 0.5);
	const int from = (int)std::floor(base.y + 0.5);
	const int to = (int)std::floor(base.y + height + 0.5);
	for (int y = from; y <= to; y++)
	{
		int level = waterAt(x, y, z);
		if (level < 0) { continue; }
		// Up to the top of this water.
		int top = y;
		for (int i = 0; i < 32; i++)
		{
			const int above = waterAt(x, top + 1, z);
			if (above < 0) { break; }
			top++;
			level = above;
		}
		surface = top + WATER_SURFACE_IN_BLOCK - level * WATER_LEVEL_DROP;
		return true;
	}
	return false;
}

// How much of a body's height is under water, 0..1 (and the surface, if
// asked for).
float waterSubmersion(glm::dvec3 base, float height,
	decltype(chunkGetterSignature) *chunkGetter, double *surface = nullptr);

// Archimedes: a body in water is pushed up by the weight of the water it
// moves aside. Here that is gravity times the share of the body under the
// surface, over its density (so a body of density 0.5 comes to rest half
// under), added to its acceleration; and the water's drag against its
// velocity. Call before updateForces. Returns the share under water.
float applyBuoyancy(glm::dvec3 base, MotionState &forces, glm::vec3 colliderSize,
	decltype(chunkGetterSignature) *chunkGetter, float deltaTime, PhysicalSettings physicalSettings);

// How hard a swimmer swims, as accelerations in blocks a second squared: up,
// the extra against a bank that is enough to climb out onto it, and down.
// The player's are on the jump and sneak keys (gamePlayLogic.cpp).
constexpr float SWIM_UP = 12.f;
constexpr float SWIM_OUT = 26.f;
constexpr float SWIM_DOWN = 24.f;

// Buoyancy for an animal or a monster, which has no keys to press: it
// floats as anything does, and when the water has carried it against a bank
// (it touched a block sideways on its last step) it swims up hard enough to
// climb out, as the player does holding the jump key there. On the ground
// its own jump takes it up a block; afloat it has no ground to jump from.
float applyBuoyancyAndClimb(glm::dvec3 base, MotionState &forces, glm::vec3 colliderSize,
	decltype(chunkGetterSignature) *chunkGetter, float deltaTime, PhysicalSettings physicalSettings);


//others are already supposed to be colliding
void colideWithOthers(glm::dvec3 &pos, glm::vec3 collider, MotionState &forces, std::vector<ColidableEntry> &others);

bool boxColide(glm::dvec3 p1, glm::vec3 s1,
	glm::dvec3 p2, glm::vec3 s2);

bool pointInsideBox(glm::dvec3 p,
	glm::dvec3 box, glm::vec3 size, float delta);

bool boxColideBlock(glm::dvec3 p1, glm::vec3 s1, glm::ivec3 b);

bool boxColideBlockWithCollider(glm::dvec3 p1, glm::vec3 s1, glm::ivec3 b, BlockCollider collider);

bool lineIntersectBoxGetPos(glm::dvec3 start, glm::dvec3 dir, glm::dvec3 box, glm::dvec3 size,
	glm::dvec3 &outPos, float &outDist, int &outFace
);

bool lineIntersectBox(glm::dvec3 start, glm::dvec3 dir,
	glm::dvec3 box, glm::dvec3 size);

bool lineIntersectBoxMaxDistance(glm::dvec3 start, glm::dvec3 dir,
	glm::dvec3 box, glm::dvec3 size, float maxDistance, float &outIntersectDist, float delta);