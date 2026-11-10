#pragma once
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// Milestone G2 (docs/gameplay-roadmap.md): screen effects, after FNV's
// imagespace modifiers (TESImageSpaceModifier, ImageSpaceModifierInstance).
//
// A preset is a named set of curves over time for the post chain's values:
// saturation, brightness, contrast, a tint, a fade to a colour, coloured
// edges, blur, double vision and radial blur; and, since milestone G3, the
// splats on the screen: water drops, blood and rain. Gameplay starts instances of
// them; every frame the running instances are combined into one PostLook,
// which the renderer puts on top of the player's tone map settings (and so
// on top of G1's weather, which holds one too).
//
// Two kinds of instance:
//   trigger  runs the preset's curves once, from time 0 to its duration (a
//            hit, a splash). Several can run at once; they stack.
//   hold     runs while something calls hold() for it every frame (low
//            health, poison, the overcast sky), at a strength that eases
//            toward the one asked for and back to 0 when the calls stop.
//            Its curves loop over the preset's duration (a pulse).
//
// How instances combine, as FNV's ImageSpaceManager does it: multipliers
// (saturation, brightness, contrast) multiply; the colours (tint, fade,
// edges) add their amounts and average their colours by amount; blur and
// radial blur take the largest; double vision adds; the splats take the
// largest.
//
// Presentation only, client side: what starts them (damage, the life left,
// the effects, the weather) is state the server already sends.
namespace postModifiers
{
	enum Preset
	{
		hit,        // taking damage: a red flash, red edges, a little blur
		lowHealth,  // held below a third of the life: a heartbeat in red edges, less colour
		poisoned,   // held while Effects::Poisoned runs: green, swimming double vision
		splash,     // going under water: a blur and a cold flash that clear
		overcast,   // held by the weather's grey sky: less colour, flatter
		// Milestone G3, the splats.
		heavyHit,   // a quarter of the life at once: blood on the screen that stays a while
		surfacing,  // coming out of the water: drops that run off over 4 s
		rainOnLens, // held in the rain under open sky: drops landing and fading

		PRESET_COUNT
	};

	const char *getPresetName(int preset);
	bool isHeldPreset(int preset); // hold() presets, as opposed to trigger() ones

	// The combined values for one frame. The identity changes nothing.
	struct PostLook
	{
		float saturation = 1;   // times the tone map's saturation
		float brightness = 1;   // times the exposure
		float contrast = 1;     // around mid grey, after the tone mapper
		glm::vec4 tint = {1, 1, 1, 0};  // rgb, amount: toward the colour, keeping the brightness
		glm::vec4 fade = {0, 0, 0, 0};  // rgb, amount: toward the colour itself (a flash)
		glm::vec4 edges = {0, 0, 0, 0}; // rgb, amount: the screen's edges toward the colour
		float blur = 0;         // radius in pixels, at 1080 lines (scaled to the screen)
		float radialBlur = 0;   // 0..1, smears toward the centre
		glm::vec2 doubleVision = {0, 0}; // the ghost's offset in pixels, at 1080 lines
		// Milestone G3, the splats, 0..1 each: hitLensDirt.png's blood over
		// the picture, waterLensDirt.png's drops and the rain's procedural
		// ones bending it.
		float blood = 0;
		float waterDrops = 0;
		float rainDrops = 0;
	};

	// Once a frame, before anything calls hold() for the frame: advances the
	// instances, drops finished ones and lets go of holds nobody renewed.
	void update(float deltaTime);

	// Starts a run of a trigger preset; strength scales all of it, 0..1.
	void trigger(int preset, float strength = 1);

	// Keeps a held preset on this frame at `strength`, 0..1 (0 lets it go).
	void hold(int preset, float strength = 1);

	// Ends every running trigger of a preset at once (the water drops going
	// back under water).
	void stop(int preset);

	// Every instance off at once (leaving a world).
	void clear();

	// This frame's look. The identity when the player turned screen effects
	// off (ShadingSettings::screenEffects).
	PostLook current();

	// The stack, the presets and a preview in the debug panel.
	void debugUi();
}
