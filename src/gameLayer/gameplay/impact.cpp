#include <gameplay/impact.h>
#include <gameplay/items.h>
#include <gameplay/allentities.h>
#include <rendering/debris.h>
#include <audioEngine.h>
#include <blocks.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <array>
#include <cstdlib>
#include <sstream>
#include <string>

namespace impact
{

namespace
{
	const char *materialNames[MATERIAL_COUNT] = {
		"none", "stone", "metal", "glass", "ice", "wood", "plant", "leaves", "dirt", "sand", "gravel",
		"clay", "snow", "wool", "flesh", "straw" };
	const char *sourceNames[SOURCE_COUNT] = { "hand", "pickaxe", "axe", "shovel", "blade", "blunt", "pierce" };

	// The tool a material wants: struck with it, it is louder and throws
	// more; struck by hand, softer and less. Blades want the soft things,
	// blunt the hard and brittle ones.
	bool wants(int material, int source)
	{
		switch (source)
		{
		case sourcePickaxe:
			return material == materialStone || material == materialMetal || material == materialGlass
				|| material == materialIce;
		case sourceShovel:
			return material == materialDirt || material == materialSand || material == materialGravel
				|| material == materialClay || material == materialSnow;
		case sourceAxe:
			return material == materialWood || material == materialLeaves || material == materialPlant
				|| material == materialStraw;
		case sourceBlade:
			return material == materialPlant || material == materialLeaves || material == materialWool
				|| material == materialFlesh || material == materialStraw;
		case sourceBlunt:
			return material == materialStone || material == materialGlass || material == materialIce
				|| material == materialFlesh;
		case sourcePierce:
			return material == materialFlesh || material == materialStraw;
		}
		return false;
	}

	bool hard(int material)
	{
		return material == materialStone || material == materialMetal || material == materialGlass
			|| material == materialIce;
	}

	// The table, from rules: the material's own impact, then what struck it.
	Impact makeImpact(int material, int source, int event)
	{
		Impact impact;
		if (material == materialNone) { return impact; }
		const bool breaking = event == eventBreak;

		// The material alone.
		impact.debris = breaking ? 16 : 3;
		impact.debrisSpeed = breaking ? 3.2f : 2.4f;
		impact.debrisSize = 0.11f;
		switch (material)
		{
		case materialGlass:
		case materialIce:
			impact.debris = breaking ? 28 : 2;
			impact.debrisSpeed = breaking ? 4.5f : 2.8f;
			impact.debrisSize = 0.08f;
			break;
		case materialPlant:
			impact.debris = breaking ? 9 : 3;
			impact.debrisSpeed = 2.f;
			impact.debrisSize = 0.08f;
			break;
		case materialLeaves:
			impact.debris = breaking ? 18 : 5;
			impact.debrisSpeed = 2.f;
			impact.debrisSize = 0.09f;
			break;
		case materialSand:
		case materialGravel:
		case materialSnow:
			impact.debris = breaking ? 20 : 4;
			impact.debrisSize = 0.08f;
			break;
		case materialFlesh:
			impact.debris = 6;
			impact.debrisSpeed = 2.6f;
			impact.debrisSize = 0.07f;
			break;
		case materialStraw:
			impact.debris = 9;
			impact.debrisSpeed = 2.4f;
			impact.debrisSize = 0.08f;
			break;
		}

		// What struck it.
		float debris = 1, speed = 1;
		if (source == sourceHand)
		{
			impact.volume = 0.7f;
			debris = 0.6f;
			speed = 0.8f;
		}
		else if (wants(material, source))
		{
			impact.volume = 1.f;
			debris = 1.25f;
			speed = 1.1f;
		}
		else
		{
			impact.volume = 0.85f;
			debris = 0.85f;
		}
		if (source == sourceBlunt) { speed *= 1.5f; if (hard(material)) { debris *= 1.2f; } }
		if (source == sourcePierce) { debris *= 0.6f; speed *= 1.3f; }
		if (source == sourceBlade && (material == materialPlant || material == materialLeaves)) { debris *= 1.4f; }

		// A pickaxe or a hammer on stone rings: the crack the game had and did
		// not play (gamePlayLogic's commented-out crackStone).
		if (material == materialStone && (source == sourcePickaxe || source == sourceBlunt))
		{
			impact.extraSound = AudioEngine::crackStone;
			impact.extraVolume = breaking ? 0.3f : 0.12f;
		}

		impact.debris = std::max(1, (int)std::round(impact.debris * debris));
		impact.debrisSpeed *= speed;
		return impact;
	}

	using Table = std::array<std::array<std::array<Impact, EVENT_COUNT>, SOURCE_COUNT>, MATERIAL_COUNT>;
	const Table table = []()
	{
		Table t{};
		for (int m = 0; m < MATERIAL_COUNT; m++)
			for (int s = 0; s < SOURCE_COUNT; s++)
				for (int e = 0; e < EVENT_COUNT; e++)
					t[m][s][e] = makeImpact(m, s, e);
		return t;
	}();

	bool valid(int material, int source, int event)
	{
		return material >= 0 && material < MATERIAL_COUNT && source >= 0 && source < SOURCE_COUNT
			&& event >= 0 && event < EVENT_COUNT;
	}

	// The debug panel's test strike.
	int testMaterial = materialStone;
	int testSource = sourcePickaxe;
	int testBlock = BlockTypes::stone;
}

const char *getMaterialName(int material)
{
	return material >= 0 && material < MATERIAL_COUNT ? materialNames[material] : "";
}

const char *getSourceName(int source)
{
	return source >= 0 && source < SOURCE_COUNT ? sourceNames[source] : "";
}

// In the order getSoundForBlockBreaking tells blocks apart, so a block's
// material agrees with its sound.
Material getBlockMaterial(std::uint16_t type)
{
	if (!isBlock(type) || type == BlockTypes::water || type == BlockTypes::air) { return materialNone; }
	if (type == BlockTypes::gravel) { return materialGravel; }
	if (type == BlockTypes::gold_block) { return materialMetal; }
	if (type == BlockTypes::grassBlock) { return materialDirt; }
	if (type == BlockTypes::snow_block || type == BlockTypes::snow_dirt) { return materialSnow; }
	if (type == BlockTypes::hayBalde) { return materialStraw; }
	if (isAnyWoddenBlock(type)) { return materialWood; }
	if (isAnyDirtBlock(type)) { return materialDirt; }
	if (isAnyClayBlock(type)) { return materialClay; }
	if (isAnySandyBlock(type)) { return materialSand; }
	if (isAnyPlant(type)) { return materialPlant; }
	if (isAnyGlass(type) || type == BlockTypes::glowstone) { return materialGlass; }
	if (type == BlockTypes::ice) { return materialIce; }
	if (isAnyLeaves(type)) { return materialLeaves; }
	if (isAnyWool(type)) { return materialWool; }
	return materialStone; // stone, bricks, sandstone, volcanic rock, the unbreakable ones
}

Material getEntityMaterial(int entityType)
{
	switch (entityType)
	{
	case EntityType::trainingDummy:
	case EntityType::scareCrow:
		return materialStraw;
	case EntityType::droppedItems:
		return materialNone;
	}
	return materialFlesh;
}

Source getSource(Item &item)
{
	if (item.isPickaxe()) { return sourcePickaxe; }
	if (item.isAxe()) { return sourceAxe; }
	if (item.isShovel()) { return sourceShovel; }
	switch (item.type)
	{
	case trainingWarHammer: case copperWarHammer: case leadWarHammer: case ironWarHammer:
	case silverWarHammer: case goldWarHammer:
		return sourceBlunt;
	case trainingSpear: case copperSpear: case leadSpear: case ironSpear: case silverSpear: case goldSpear:
		return sourcePierce;
	case trainingBattleAxe: case copperBattleAxe: case leadBattleAxe: case ironBattleAxe:
	case silverBattleAxe: case goldBattleAxe:
		return sourceAxe;
	}
	if (item.isWeapon()) { return sourceBlade; } // swords, knives, scythes
	return sourceHand;
}

glm::vec3 getMaterialColour(int material)
{
	switch (material)
	{
	case materialFlesh: return {0.3f, 0.02f, 0.02f};
	case materialStraw: return {0.78f, 0.66f, 0.32f};
	}
	return {0.5f, 0.5f, 0.5f};
}

const Impact &getImpact(int material, int source, int event)
{
	static const Impact nothing{};
	if (!valid(material, source, event)) { return nothing; }
	return table[material][source][event];
}

void play(int material, int source, int event, glm::dvec3 at, glm::vec3 away, std::uint16_t blockType)
{
	if (material == materialNone) { return; }
	const Impact &impact = getImpact(material, source, event);

	if (blockType)
	{
		const bool breaking = event == eventBreak;
		const int sound = breaking ? getSoundForBlockBreaking(blockType) : getSoundForBlockStepping(blockType);
		AudioEngine::playSound(sound, (breaking ? BREAKED_BLOCK_SOUND_VOLUME : MINING_BLOCK_SOUND_VOLUME)
			* impact.volume);
	}
	else
	{
		AudioEngine::playSound(AudioEngine::hit, HIT_SOUND_VOLUME * impact.volume);
	}
	if (impact.extraSound) { AudioEngine::playSound(impact.extraSound, impact.extraVolume); }

	debris::spawn(at, away, impact.debris, impact.debrisSpeed, impact.debrisSize, blockType,
		getMaterialColour(material));
}

void debugUi(glm::dvec3 cameraPosition, glm::vec3 viewDirection)
{
	if (!ImGui::CollapsingHeader("Impacts", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding))
	{
		return;
	}

	ImGui::Combo("Material", &testMaterial, materialNames, MATERIAL_COUNT);
	ImGui::Combo("Source", &testSource, sourceNames, SOURCE_COUNT);
	ImGui::InputInt("Block for the texture", &testBlock);
	testBlock = glm::clamp(testBlock, 0, (int)BlocksCount - 1);
	ImGui::SameLine();
	ImGui::TextDisabled("(0: a flat colour)");

	for (int e = 0; e < EVENT_COUNT; e++)
	{
		const Impact &i = getImpact(testMaterial, testSource, e);
		ImGui::Text("%s: volume %.2f, %d pieces at %.1f, size %.2f%s", e == eventHit ? "hit  " : "break",
			i.volume, i.debris, i.debrisSpeed, i.debrisSize, i.extraSound ? ", + a crack" : "");
	}

	const glm::dvec3 at = cameraPosition + glm::dvec3(viewDirection) * 2.0;
	if (ImGui::Button("Hit")) { play(testMaterial, testSource, eventHit, at, -viewDirection, (std::uint16_t)testBlock); }
	ImGui::SameLine();
	if (ImGui::Button("Break")) { play(testMaterial, testSource, eventBreak, at, -viewDirection, (std::uint16_t)testBlock); }
	ImGui::SameLine();
	ImGui::TextDisabled("%d pieces flying", (int)debris::pieces().size());
}

void updateEnvironmentTest(float deltaTime, glm::dvec3 cameraPosition, glm::vec3 viewDirection)
{
	static bool read = false;
	static bool active = false;
	static int material = 0, source = 0, event = 0, block = 0;
	static float timer = 0;
	if (!read)
	{
		read = true;
		const char *env = std::getenv("OURCRAFT_IMPACT");
		if (!env) { return; }
		std::stringstream list(env);
		std::string part;
		int index = 0;
		while (std::getline(list, part, ','))
		{
			if (index == 0) { for (int i = 0; i < MATERIAL_COUNT; i++) { if (part == materialNames[i]) { material = i; } } }
			if (index == 1) { for (int i = 0; i < SOURCE_COUNT; i++) { if (part == sourceNames[i]) { source = i; } } }
			if (index == 2) { event = part == "break" ? eventBreak : eventHit; }
			if (index == 3) { block = std::atoi(part.c_str()); }
			index++;
		}
		active = material != materialNone;
	}
	if (!active) { return; }
	timer -= deltaTime;
	if (timer > 0) { return; }
	timer = 0.4f;
	play(material, source, event, cameraPosition + glm::dvec3(viewDirection) * 2.0, -viewDirection,
		(std::uint16_t)glm::clamp(block, 0, (int)BlocksCount - 1));
}

}
