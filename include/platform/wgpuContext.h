#pragma once

// WebGPU device context (docs/roadmap.md, milestone 1a onward).
// Only compiled when CMake is configured with -DRENDERER_WEBGPU=ON.

#if RENDERER_WEBGPU

struct GLFWwindow;

namespace wgpuContext
{
	// Creates the instance, the window surface, an adapter that can present to
	// it (and prints what the adapter supports), the device and its queue, and
	// configures the surface. The window must be created with GLFW_NO_API.
	bool init(GLFWwindow *window);

	// Milestone 1b: clears the window to one color and presents it.
	// Reconfigures the surface first if the framebuffer size changed.
	void clearAndPresent(float r, float g, float b);

	void shutdown();

	// macOS: sets the window's CAMetalLayer colorspace to sRGB so swapchain
	// values are shown as sRGB on wide-gamut displays, as OpenGL shows them.
	// No-op elsewhere. Implemented in wgpuMetalLayer.mm.
	void pinMetalLayerToSRGB(GLFWwindow *window);
}

#endif
