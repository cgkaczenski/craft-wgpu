#include <rendering/crt.h>
#include <rendering/renderSettings.h>
#include <gameLayer.h>
#include <render/wgpu2d.h>
#include <glm/glm.hpp>
#include <imgui.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace crt
{

namespace
{
	wgpu2d::Effect effect;

	// The parts the intensity scales. The curvature is the first thing to
	// look broken, so it is the smallest. The vignette is lower than a CRT's
	// usually is, because the tone mapper already has one.
	float curvature = 0.06f;
	float scanlines = 0.14f;
	float mask = 0.08f;
	float fringing = 0.5f;
	float vignette = 0.2f;

	// In whole output pixels, so the dark band cannot drift against the
	// picture's own pixel grid.
	float period = 8.f;

	bool glowOn = true;
	wgpu2d::FinalGlow glow;

	// How far the bright end runs toward warmColour, which is kept in step
	// with warmTint in crt.wgsl.
	float warmth = 0.5f;
	const glm::vec3 warmColour = {1.0f, 0.80f, 0.58f};

	// The menu's intensity, 1..100, as the shader's master, 0..2: 50 is the
	// look above as tuned.
	float master()
	{
		const ShadingSettings &settings = getShadingSettings();
		if (effect.id == 0 || !settings.crt) { return 0; }
		return glm::clamp(settings.crtIntensity, 1, 100) / 50.f;
	}
}

bool init()
{
	const char *path = RESOURCES_PATH "shaders/webgpu/crt.wgsl";
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		std::cerr << "crt: cannot read " << path << "\n";
		return false;
	}
	std::stringstream source;
	source << file.rdbuf();

	effect = wgpu2d::createEffect(source.str().c_str(), "crt");
	if (effect.id == 0)
	{
		std::cerr << "crt: effect did not compile\n";
		return false;
	}
	return true;
}

void cleanup()
{
	wgpu2d::clearFinalEffect();
	wgpu2d::clearFinalGlow();
	effect = {};
}

void apply()
{
	const float amount = master();
	if (amount <= 0)
	{
		wgpu2d::clearFinalEffect();
		wgpu2d::clearFinalGlow();
		return;
	}

	if (glowOn)
	{
		// The tint keeps the glow's brightness and moves only its hue.
		const float warmLuma = glm::dot(warmColour, glm::vec3(0.2126f, 0.7152f, 0.0722f));
		wgpu2d::FinalGlow scaled = glow;
		scaled.tint = glm::mix(glm::vec3(1.f), warmColour / warmLuma, warmth);
		scaled.intensity *= glm::min(amount, 1.f);
		wgpu2d::setFinalGlow(scaled);
	}
	else
	{
		wgpu2d::clearFinalGlow();
	}

	wgpu2d::EffectParams params;
	params.a = {amount, curvature, scanlines, mask};
	params.b = {period, vignette, fringing, warmth};
	wgpu2d::setFinalEffect(effect, params);
}

glm::vec2 toPicture(glm::vec2 screen, glm::vec2 size)
{
	const float amount = master();
	if (amount <= 0 || size.x <= 0 || size.y <= 0) { return screen; }
	const float k = curvature * amount;
	const glm::vec2 centred = screen / size * 2.f - 1.f;
	const float r2 = glm::dot(centred, centred);
	return ((centred * (1.f + k * r2)) * 0.5f + 0.5f) * size;
}

glm::ivec2 mouseOnPicture()
{
	return glm::ivec2(toPicture(glm::vec2(platform::getRelMousePosition()), glm::vec2(platform::getWindowSize())));
}

void debugUi()
{
	if (!ImGui::CollapsingHeader("CRT", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_FramePadding)) { return; }

	ImGui::TextDisabled("On/off and intensity: Settings > Rendering > CRT effect");
	ImGui::SliderFloat("Warmth", &warmth, 0.f, 1.f);
	ImGui::SliderFloat("Curvature", &curvature, 0.f, 0.3f);
	ImGui::SliderFloat("Scanlines", &scanlines, 0.f, 1.f);
	ImGui::SliderFloat("Scanline period", &period, 2.f, 32.f, "%.0f px");
	ImGui::SliderFloat("Aperture mask", &mask, 0.f, 0.5f);
	ImGui::SliderFloat("Fringing", &fringing, 0.f, 2.f);
	ImGui::SliderFloat("CRT vignette", &vignette, 0.f, 1.f);

	ImGui::Checkbox("Phosphor glow", &glowOn);
	if (glowOn)
	{
		ImGui::SliderFloat("Glow threshold", &glow.threshold, 0.f, 1.f);
		ImGui::SliderFloat("Glow knee", &glow.knee, 0.01f, 0.5f);
		ImGui::SliderFloat("Glow width", &glow.sigma, 0.5f, 8.f);
		ImGui::SliderFloat("Glow intensity", &glow.intensity, 0.f, 2.f);
	}
}

}
