#pragma once

// Milestone 4: the Dear ImGui *renderer* backend for the WebGPU path. The
// platform backend stays imgui_impl_glfw (input, display size, cursors).
//
// Ported from space-game-wgpu (src/platform/wgpuImgui.cpp) rather than
// upgrading ImGui: the bundled imgui_impl_wgpu targets an older webgpu.h
// than wgpu-native v24, and moving 1.85 to a release that matches would
// change the OpenGL path and the game's ImGui code too.
//
// Like wgpuWindow.h this header names no WebGPU and no ImGui types.

namespace wgpuImgui
{
	// Builds the pipeline and uploads the font atlas as a wgpu2d texture.
	// Call after ImGui::CreateContext, the GLFW backend's init, the font
	// setup, and wgpuWindow::init. Returns false on failure.
	bool init();

	// Draws ImGui::GetDrawData() into the frame's render pass. Call after
	// ImGui::Render() and before wgpuWindow::endFrame, so the UI lands on
	// top of the game.
	void renderDrawData();

	// Releases the pipeline, the buffers and the font texture. Call before
	// wgpuWindow::shutdown.
	void shutdown();
}
