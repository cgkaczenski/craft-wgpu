#pragma once
#include <cstdint>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

struct Item;

// Milestone G4 (docs/gameplay-roadmap.md): impact data, after FNV's
// BGSImpactData and BGSImpactDataSet. FNV keys a table by the surface's
// material and the weapon's type, and each entry names the decal, the
// particles and the sound of a hit.
//
// Here a material is given to every block and to every kind of entity, a
// source to whatever is in the hand, and the table, built once from rules
// over the two, says for a strike that does not break (a mining tick, a
// weapon on a block, a hit on an entity) and for a block breaking: how loud
// the material's own sound is, a second sound if any, and how much debris
// flies, how fast and how big (rendering/debris.h).
//
// The sounds stay the blocks' own (getSoundForBlockBreaking and
// getSoundForBlockStepping, which know bricks, sandstone and the rest); the
// table scales them by what struck. The crack decal is still the mining
// progress, not a table entry.
//
// Client side presentation: the local player's own strikes.
namespace impact
{
	enum Material : unsigned char
	{
		materialNone = 0, // water, air: nothing
		materialStone,
		materialMetal,
		materialGlass,
		materialIce,
		materialWood,
		materialPlant,    // grass, flowers, crops
		materialLeaves,
		materialDirt,
		materialSand,
		materialGravel,
		materialClay,
		materialSnow,
		materialWool,
		materialFlesh,    // players, zombies, goblins, pigs, cats
		materialStraw,    // training dummies, scarecrows

		MATERIAL_COUNT
	};

	enum Source : unsigned char
	{
		sourceHand = 0,  // nothing in the hand, or anything not a tool or weapon
		sourcePickaxe,
		sourceAxe,       // axes and battle axes
		sourceShovel,
		sourceBlade,     // swords, knives, scythes
		sourceBlunt,     // war hammers
		sourcePierce,    // spears

		SOURCE_COUNT
	};

	enum Event : unsigned char
	{
		eventHit = 0, // a strike that leaves it standing
		eventBreak,   // the block breaking

		EVENT_COUNT
	};

	struct Impact
	{
		float volume = 1;      // times the material's own sound's usual level
		int extraSound = 0;    // an AudioEngine sound on top, 0 none
		float extraVolume = 0;
		int debris = 0;        // pieces
		float debrisSpeed = 0; // blocks a second, the fastest
		float debrisSize = 0;  // the biggest piece's edge, blocks
	};

	const char *getMaterialName(int material);
	const char *getSourceName(int source);

	Material getBlockMaterial(std::uint16_t blockType);
	Material getEntityMaterial(int entityType);
	Source getSource(Item &item);

	// The material's colour for debris that has no block texture (flesh,
	// straw), as display values.
	glm::vec3 getMaterialColour(int material);

	const Impact &getImpact(int material, int source, int event);

	// Plays a strike: the sounds, and the debris thrown from `at` toward
	// `away` (usually back along the view). `blockType` gives the debris its
	// texture and the sound; 0 for an entity (the hit sound, debris in the
	// material's colour).
	void play(int material, int source, int event, glm::dvec3 at, glm::vec3 away, std::uint16_t blockType);

	// The table in the debug panel, and a test strike in front of the camera.
	void debugUi(glm::dvec3 cameraPosition, glm::vec3 viewDirection);

	// OURCRAFT_IMPACT=<material>,<source>,<hit|break>[,<block id>] strikes in
	// front of the camera every 0.4 s, for scripted screenshots. Once a frame.
	void updateEnvironmentTest(float deltaTime, glm::dvec3 cameraPosition, glm::vec3 viewDirection);
}
