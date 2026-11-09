#pragma once
#include <glm/vec2.hpp>

// A CRT filter over the finished picture: curvature, scanlines, an aperture
// mask, colour fringing, a vignette and a phosphor glow. The world and the HUD
// go through it, the ImGui debug windows do not.
//
// wgpu2d does the routing (setFinalEffect: everything drawn lands in a frame
// target, which comes back to the surface through the shader); the shader is
// resources/shaders/webgpu/crt.wgsl. The player's on/off and 1-100 intensity
// are in ShadingSettings (crt, crtIntensity), under Rendering in the menu.
namespace crt
{
	// Compiles the effect. Once, after the renderer exists. False if the
	// shader is missing or does not compile; the game then runs without it.
	bool init();

	void cleanup();

	// Hands the settings to the renderer, once a frame.
	void apply();

	// Where on the picture the screen point `screen` (of a `size` window)
	// shows through the glass: the shader's bend(), on the CPU. With the
	// filter off, `screen` itself.
	glm::vec2 toPicture(glm::vec2 screen, glm::vec2 size);

	// The mouse on the picture, for anything drawn through the filter that
	// reads the pointer: menus, inventory, the map. Not for the camera, and
	// not for ImGui, which is on the flat glass.
	glm::ivec2 mouseOnPicture();

	// The parts of the look, in the debug panel.
	void debugUi();
}
