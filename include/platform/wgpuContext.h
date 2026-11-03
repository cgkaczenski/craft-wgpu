#pragma once

// WebGPU device context (docs/roadmap.md, milestone 1a onward).
// Only compiled when CMake is configured with -DRENDERER_WEBGPU=ON.

#if RENDERER_WEBGPU

namespace wgpuContext
{
	// Creates the WebGPU instance, requests a high-performance adapter and
	// prints what it supports. Returns false if no adapter was found.
	// The instance and adapter stay alive for the device and surface (milestone 1b).
	bool init();
}

#endif
