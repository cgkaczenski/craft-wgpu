#pragma once

// Connects the GLFW window to wgpu2d's WebGPU context (docs/roadmap.md,
// milestones 1 and 3). wgpu2d owns the instance, adapter, device and frame;
// this file only knows the window: it makes the surface, pins the Metal
// layer's colorspace, and passes framebuffer sizes along.
// Only compiled when CMake is configured with -DRENDERER_WEBGPU=ON.

#if RENDERER_WEBGPU

struct GLFWwindow;

namespace wgpuWindow
{
	// Creates the instance and the window's surface, then the adapter, device
	// and surface configuration (render::wgpuInit), and prints which features
	// the port depends on. The window must be created with GLFW_NO_API.
	bool init(GLFWwindow *window);

	// Frame bracket around everything that draws. beginFrame resizes the
	// surface to the framebuffer if it changed and acquires the next texture;
	// endFrame submits and presents. Between them, wgpu2d's flushes draw.
	void beginFrame();
	void endFrame();

	void shutdown();

	// macOS: sets the window's CAMetalLayer colorspace to sRGB so swapchain
	// values are shown as sRGB on wide-gamut displays, as OpenGL shows them.
	// No-op elsewhere. Implemented in wgpuMetalLayer.mm.
	void pinMetalLayerToSRGB(GLFWwindow *window);
}

#endif
