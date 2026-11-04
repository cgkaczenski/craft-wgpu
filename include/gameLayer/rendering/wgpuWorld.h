#pragma once

// The WebGPU side of the 3D renderer, beside the OpenGL Renderer. Milestone 5
// starts it with one depth buffer, one camera and two test cubes; later
// milestones grow it into the WgpuRenderer of docs/roadmap.md.
//
// Names no WebGPU types, so renderer.cpp and glfwMain.cpp can call it without
// including webgpu.hpp.

struct Camera;

namespace wgpuWorld
{
	// Records the world's pass for this frame: clears color and depth and
	// draws the test cubes with the camera. Call inside an open frame, before
	// the 2D layer draws. Creates its GPU objects on first use.
	void render(Camera &camera);

	// Releases the pipeline, buffers and depth texture. Call before
	// wgpuWindow::shutdown.
	void shutdown();
}
