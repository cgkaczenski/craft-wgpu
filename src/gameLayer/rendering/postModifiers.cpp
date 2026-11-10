#include <rendering/postModifiers.h>
#include <rendering/renderSettings.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace postModifiers
{

namespace
{
	// A curve is keys at times in seconds, linear between them and flat
	// past the ends (FNV's TimedFloatKey and ColorAnimationKey). An empty
	// curve leaves its value alone.
	struct FloatKey { float time; float value; };
	struct ColorKey { float time; glm::vec4 color; }; // rgb, a = amount
	using Curve = std::vector<FloatKey>;
	using ColorCurve = std::vector<ColorKey>;

	struct PresetData
	{
		const char *name = "";
		bool held = false;
		// A trigger's length; a hold's loop (0: no loop, the curves' start).
		float duration = 0;
		// A hold's easing: seconds from 0 to full strength, and back.
		float easeIn = 0.5f;
		float easeOut = 0.5f;
		// A trigger's new run on top of a running one: stack them (true), or
		// start the running one over (false).
		bool stacks = true;

		Curve saturation;
		Curve brightness;
		Curve contrast;
		ColorCurve tint;
		ColorCurve fade;
		ColorCurve edges;
		Curve blur;         // pixels at 1080 lines
		Curve radialBlur;   // 0..1
		Curve doubleVision; // the ghost's distance, pixels at 1080 lines
		float doubleVisionSpin = 0; // radians a second the ghost turns around the picture
		// Milestone G3, the splats, 0..1.
		Curve blood;
		Curve waterDrops;
		Curve rainDrops;
	};

	// The colours are display values, since they are mixed in after the
	// tone mapper.
	std::vector<PresetData> makePresets()
	{
		std::vector<PresetData> presets(PRESET_COUNT);

		{
			PresetData &p = presets[hit];
			p.name = "hit";
			p.duration = 0.6f;
			p.fade = { {0.f, {0.65f, 0.03f, 0.02f, 0.30f}}, {0.08f, {0.65f, 0.03f, 0.02f, 0.18f}},
				{0.35f, {0.65f, 0.03f, 0.02f, 0.f}} };
			p.edges = { {0.f, {0.45f, 0.f, 0.f, 0.85f}}, {0.15f, {0.45f, 0.f, 0.f, 0.6f}},
				{0.6f, {0.45f, 0.f, 0.f, 0.f}} };
			p.blur = { {0.f, 2.5f}, {0.25f, 0.f} };
			p.saturation = { {0.f, 0.7f}, {0.45f, 1.f} };
			// GL's hit lens dirt: full, gone in 0.83 s.
			p.blood = { {0.f, 0.6f}, {0.83f, 0.f} };
		}

		{
			// A heartbeat a second: two beats close together, then a rest.
			PresetData &p = presets[lowHealth];
			p.name = "low health";
			p.held = true;
			p.duration = 1.0f;
			p.easeIn = 0.8f;
			p.easeOut = 1.5f;
			const glm::vec3 red = {0.35f, 0.f, 0.f};
			p.edges = { {0.f, {red, 0.35f}}, {0.08f, {red, 0.75f}}, {0.2f, {red, 0.42f}},
				{0.3f, {red, 0.62f}}, {0.55f, {red, 0.35f}}, {1.0f, {red, 0.35f}} };
			p.saturation = { {0.f, 0.55f} };
			p.contrast = { {0.f, 1.06f} };
			// GL kept 0.2-0.3 of the hit dirt on at low health.
			p.blood = { {0.f, 0.3f} };
		}

		{
			// Green and swimming: the ghost drifts out and back while it turns.
			PresetData &p = presets[poisoned];
			p.name = "poisoned";
			p.held = true;
			p.duration = 6.f;
			p.easeIn = 2.f;
			p.easeOut = 2.5f;
			const glm::vec3 green = {0.55f, 1.f, 0.3f};
			p.tint = { {0.f, {green, 0.28f}}, {3.f, {green, 0.4f}}, {6.f, {green, 0.28f}} };
			p.edges = { {0.f, {0.04f, 0.16f, 0.02f, 0.35f}} };
			p.saturation = { {0.f, 0.85f} };
			p.doubleVision = { {0.f, 2.f}, {3.f, 7.f}, {6.f, 2.f} };
			p.doubleVisionSpin = 0.9f;
		}

		{
			PresetData &p = presets[splash];
			p.name = "splash";
			p.duration = 1.2f;
			p.stacks = false;
			p.blur = { {0.f, 6.f}, {0.25f, 4.f}, {1.2f, 0.f} };
			p.radialBlur = { {0.f, 0.3f}, {0.5f, 0.f} };
			p.fade = { {0.f, {0.62f, 0.8f, 1.f, 0.3f}}, {0.3f, {0.62f, 0.8f, 1.f, 0.f}} };
			p.brightness = { {0.f, 1.3f}, {0.4f, 1.f} };
		}

		{
			// The weather's own strength (its overcast, which already blends
			// over the weather's change) is the hold's; the easing only
			// smooths a jump.
			PresetData &p = presets[overcast];
			p.name = "overcast";
			p.held = true;
			p.easeIn = 1.f;
			p.easeOut = 1.f;
			p.saturation = { {0.f, 0.8f} };
			p.contrast = { {0.f, 0.94f} };
		}

		{
			PresetData &p = presets[heavyHit];
			p.name = "heavy hit";
			p.duration = 4.f;
			p.blood = { {0.f, 1.f}, {1.5f, 0.85f}, {4.f, 0.f} };
		}

		{
			// GL's water drops: full for a second, then gone over three.
			PresetData &p = presets[surfacing];
			p.name = "surfacing";
			p.duration = 4.f;
			p.stacks = false;
			p.waterDrops = { {0.f, 1.f}, {1.f, 1.f}, {4.f, 0.f} };
		}

		{
			// The strength is how hard it rains, less looking down; drops
			// keep landing while it lasts and dry off after.
			PresetData &p = presets[rainOnLens];
			p.name = "rain on lens";
			p.held = true;
			p.easeIn = 3.f;
			p.easeOut = 4.f;
			p.rainDrops = { {0.f, 1.f} };
		}

		return presets;
	}

	const std::vector<PresetData> presets = makePresets();

	struct Instance
	{
		int preset = 0;
		float time = 0;       // seconds since it started
		float strength = 0;   // now, 0..1
		float target = 0;     // a hold eases toward this
		bool renewed = false; // a hold was asked for since the last update
		bool frozen = false;  // a preview: time and strength stay put
	};

	std::vector<Instance> stack;
	constexpr size_t maxInstances = 24;

	// The debug panel's and OURCRAFT_POST_MODIFIER's previews: per preset,
	// a time to freeze at (below 0: none) and, for holds, a strength.
	float previewTime[PRESET_COUNT];
	float debugHold[PRESET_COUNT] = {};
	bool previewsRead = false;

	bool validPreset(int preset) { return preset >= 0 && preset < PRESET_COUNT; }

	float evaluate(const Curve &curve, float time, float identity)
	{
		if (curve.empty()) { return identity; }
		if (time <= curve.front().time) { return curve.front().value; }
		for (size_t i = 1; i < curve.size(); i++)
		{
			if (time <= curve[i].time)
			{
				const float span = curve[i].time - curve[i - 1].time;
				const float t = span > 0 ? (time - curve[i - 1].time) / span : 1.f;
				return glm::mix(curve[i - 1].value, curve[i].value, t);
			}
		}
		return curve.back().value;
	}

	glm::vec4 evaluate(const ColorCurve &curve, float time)
	{
		if (curve.empty()) { return glm::vec4(0); }
		if (time <= curve.front().time) { return curve.front().color; }
		for (size_t i = 1; i < curve.size(); i++)
		{
			if (time <= curve[i].time)
			{
				const float span = curve[i].time - curve[i - 1].time;
				const float t = span > 0 ? (time - curve[i - 1].time) / span : 1.f;
				return glm::mix(curve[i - 1].color, curve[i].color, t);
			}
		}
		return curve.back().color;
	}

	// Where on its curves an instance is: a hold loops over its duration.
	float curveTime(const Instance &instance)
	{
		const PresetData &p = presets[instance.preset];
		if (p.held && p.duration > 0) { return std::fmod(instance.time, p.duration); }
		return instance.time;
	}

	Instance *findHold(int preset, bool frozen)
	{
		for (Instance &instance : stack)
		{
			if (instance.preset == preset && instance.frozen == frozen) { return &instance; }
		}
		return nullptr;
	}

	// False when the stack is full of holds and previews.
	bool push(const Instance &instance)
	{
		// Full: the oldest run of a trigger goes first, holds stay.
		if (stack.size() >= maxInstances)
		{
			auto oldest = std::find_if(stack.begin(), stack.end(), [](const Instance &i)
				{ return !presets[i.preset].held && !i.frozen; });
			if (oldest == stack.end()) { return false; }
			stack.erase(oldest);
		}
		stack.push_back(instance);
		return true;
	}

	// OURCRAFT_POST_MODIFIER=name[@seconds],... freezes presets on screen for
	// scripted screenshots: a trigger at that time of its run (default 0), a
	// hold at full strength at that time of its loop. Names as
	// getPresetName gives them, spaces or underscores.
	void readPreviews()
	{
		previewsRead = true;
		for (float &t : previewTime) { t = -1; }
		const char *env = std::getenv("OURCRAFT_POST_MODIFIER");
		if (!env) { return; }
		std::stringstream list(env);
		std::string item;
		while (std::getline(list, item, ','))
		{
			float time = 0;
			const size_t at = item.find('@');
			if (at != std::string::npos)
			{
				time = (float)std::atof(item.c_str() + at + 1);
				item = item.substr(0, at);
			}
			std::replace(item.begin(), item.end(), '_', ' ');
			bool found = false;
			for (int i = 0; i < PRESET_COUNT; i++)
			{
				if (item == presets[i].name) { previewTime[i] = std::max(time, 0.f); found = true; }
			}
			if (!found) { std::cerr << "OURCRAFT_POST_MODIFIER: no preset \"" << item << "\"\n"; }
		}
	}

	void applyPreviews()
	{
		for (int i = 0; i < PRESET_COUNT; i++)
		{
			Instance *frozen = findHold(i, true);
			if (previewTime[i] < 0)
			{
				if (frozen) { stack.erase(stack.begin() + (frozen - stack.data())); }
				continue;
			}
			if (!frozen)
			{
				Instance instance;
				instance.preset = i;
				instance.frozen = true;
				if (!push(instance)) { continue; }
				frozen = &stack.back();
			}
			frozen->time = previewTime[i];
			frozen->strength = 1;
		}
	}
}

const char *getPresetName(int preset)
{
	return validPreset(preset) ? presets[preset].name : "";
}

bool isHeldPreset(int preset)
{
	return validPreset(preset) && presets[preset].held;
}

void update(float deltaTime)
{
	if (!previewsRead) { readPreviews(); }
	deltaTime = glm::clamp(deltaTime, 0.f, 0.25f);

	for (Instance &instance : stack)
	{
		if (instance.frozen) { continue; }
		instance.time += deltaTime;
		const PresetData &p = presets[instance.preset];
		if (!p.held) { continue; }
		if (!instance.renewed) { instance.target = 0; }
		instance.renewed = false;
		const bool rising = instance.target > instance.strength;
		const float seconds = rising ? p.easeIn : p.easeOut;
		const float step = seconds > 0 ? deltaTime / seconds : 1.f;
		instance.strength = rising ? std::min(instance.strength + step, instance.target)
			: std::max(instance.strength - step, instance.target);
	}

	stack.erase(std::remove_if(stack.begin(), stack.end(), [](const Instance &i)
		{
			if (i.frozen) { return false; }
			const PresetData &p = presets[i.preset];
			if (p.held) { return i.strength <= 0 && i.target <= 0; }
			return i.time >= p.duration;
		}), stack.end());

	applyPreviews();
	for (int i = 0; i < PRESET_COUNT; i++)
	{
		if (debugHold[i] > 0) { hold(i, debugHold[i]); }
	}
}

void trigger(int preset, float strength)
{
	if (!validPreset(preset) || presets[preset].held) { return; }
	strength = glm::clamp(strength, 0.f, 1.f);
	if (strength <= 0) { return; }
	if (!presets[preset].stacks)
	{
		for (Instance &running : stack)
		{
			if (running.preset == preset && !running.frozen)
			{
				running.time = 0;
				running.strength = std::max(running.strength, strength);
				return;
			}
		}
	}
	Instance instance;
	instance.preset = preset;
	instance.strength = strength;
	push(instance);
}

void hold(int preset, float strength)
{
	if (!validPreset(preset) || !presets[preset].held) { return; }
	strength = glm::clamp(strength, 0.f, 1.f);
	Instance *instance = findHold(preset, false);
	if (!instance)
	{
		if (strength <= 0) { return; }
		Instance fresh;
		fresh.preset = preset;
		if (!push(fresh)) { return; }
		instance = &stack.back();
	}
	// Two callers holding the same preset in a frame: the stronger wins.
	instance->target = instance->renewed ? std::max(instance->target, strength) : strength;
	instance->renewed = true;
}

void stop(int preset)
{
	stack.erase(std::remove_if(stack.begin(), stack.end(), [preset](const Instance &i)
		{ return i.preset == preset && !i.frozen && !presets[i.preset].held; }), stack.end());
}

void clear()
{
	stack.erase(std::remove_if(stack.begin(), stack.end(), [](const Instance &i) { return !i.frozen; }),
		stack.end());
}

PostLook current()
{
	PostLook look;
	if (!getShadingSettings().screenEffects) { return look; }

	glm::vec3 tint = {0, 0, 0}, fade = {0, 0, 0}, edges = {0, 0, 0};
	float tintAmount = 0, fadeAmount = 0, edgesAmount = 0;

	for (const Instance &instance : stack)
	{
		const float s = instance.strength;
		if (s <= 0) { continue; }
		const PresetData &p = presets[instance.preset];
		const float t = curveTime(instance);

		look.saturation *= glm::mix(1.f, evaluate(p.saturation, t, 1.f), s);
		look.brightness *= glm::mix(1.f, evaluate(p.brightness, t, 1.f), s);
		look.contrast *= glm::mix(1.f, evaluate(p.contrast, t, 1.f), s);

		auto addColour = [&](const ColorCurve &curve, glm::vec3 &sum, float &amount)
		{
			if (curve.empty()) { return; }
			const glm::vec4 c = evaluate(curve, t);
			const float a = c.a * s;
			sum += glm::vec3(c) * a;
			amount += a;
		};
		addColour(p.tint, tint, tintAmount);
		addColour(p.fade, fade, fadeAmount);
		addColour(p.edges, edges, edgesAmount);

		look.blur = std::max(look.blur, evaluate(p.blur, t, 0.f) * s);
		look.radialBlur = std::max(look.radialBlur, evaluate(p.radialBlur, t, 0.f) * s);
		look.blood = std::max(look.blood, evaluate(p.blood, t, 0.f) * s);
		look.waterDrops = std::max(look.waterDrops, evaluate(p.waterDrops, t, 0.f) * s);
		look.rainDrops = std::max(look.rainDrops, evaluate(p.rainDrops, t, 0.f) * s);
		const float ghost = evaluate(p.doubleVision, t, 0.f) * s;
		if (ghost > 0)
		{
			// Turning with the instance's own clock, so two never line up.
			const float angle = instance.time * p.doubleVisionSpin + instance.preset * 1.3f;
			look.doubleVision += ghost * glm::vec2(std::cos(angle), std::sin(angle) * 0.6f);
		}
	}

	if (tintAmount > 0) { look.tint = glm::vec4(tint / tintAmount, std::min(tintAmount, 1.f)); }
	if (fadeAmount > 0) { look.fade = glm::vec4(fade / fadeAmount, std::min(fadeAmount, 1.f)); }
	if (edgesAmount > 0) { look.edges = glm::vec4(edges / edgesAmount, std::min(edgesAmount, 1.f)); }
	look.saturation = glm::clamp(look.saturation, 0.f, 2.f);
	look.brightness = glm::clamp(look.brightness, 0.f, 4.f);
	look.contrast = glm::clamp(look.contrast, 0.5f, 1.5f);
	look.blur = std::min(look.blur, 16.f);
	look.radialBlur = std::min(look.radialBlur, 1.f);
	look.blood = std::min(look.blood, 1.f);
	look.waterDrops = std::min(look.waterDrops, 1.f);
	look.rainDrops = std::min(look.rainDrops, 1.f);
	if (glm::length(look.doubleVision) > 24.f) { look.doubleVision = glm::normalize(look.doubleVision) * 24.f; }
	return look;
}

void debugUi()
{
	if (!ImGui::CollapsingHeader("Screen effects", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding))
	{
		return;
	}

	ImGui::TextDisabled("On/off: Settings > Rendering > Screen effects");
	if (!getShadingSettings().screenEffects) { ImGui::TextColored({1, 0.6f, 0.3f, 1}, "Off in the settings"); }

	for (int i = 0; i < PRESET_COUNT; i++)
	{
		ImGui::PushID(i);
		if (presets[i].held)
		{
			ImGui::SetNextItemWidth(120);
			ImGui::SliderFloat("##hold", &debugHold[i], 0.f, 1.f, "%.2f");
			ImGui::SameLine();
			ImGui::Text("hold %s", presets[i].name);
		}
		else
		{
			if (ImGui::Button("Trigger")) { trigger(i); }
			ImGui::SameLine();
			ImGui::Text("%s (%.1f s)", presets[i].name, presets[i].duration);
		}
		ImGui::PopID();
	}

	ImGui::Separator();
	if (stack.empty()) { ImGui::TextDisabled("Nothing running"); }
	for (const Instance &instance : stack)
	{
		ImGui::Text("%s%s  t %.2f  strength %.2f", presets[instance.preset].name,
			instance.frozen ? " (preview)" : "", instance.time, instance.strength);
	}
	if (ImGui::Button("Clear")) { clear(); for (float &h : debugHold) { h = 0; } }

	const PostLook look = current();
	ImGui::TextDisabled("sat %.2f  bright %.2f  contrast %.2f", look.saturation, look.brightness, look.contrast);
	ImGui::TextDisabled("tint %.2f  fade %.2f  edges %.2f", look.tint.a, look.fade.a, look.edges.a);
	ImGui::TextDisabled("blur %.1f px  radial %.2f  ghost %.1f px", look.blur, look.radialBlur,
		glm::length(look.doubleVision));
	ImGui::TextDisabled("blood %.2f  water drops %.2f  rain %.2f", look.blood, look.waterDrops, look.rainDrops);
}

}
