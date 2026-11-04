#pragma once

// Milestone 2 scaffold (docs/roadmap.md). The WebGPU path has no OpenGL
// context, but gl2d, glui, the block and model loaders and the 3D renderer
// still call OpenGL. install() points every glad function at a stub that does
// nothing, so that code runs as a no-op while WebGPU draws the window.
//
// Each later milestone moves one subsystem off OpenGL. When none is left,
// delete this, nullGL.cpp, nullGLStubs.inl, tools/genNullGL.py and glad.

#if RENDERER_WEBGPU

namespace nullGL
{
	// Call once, instead of gladLoadGL(), before any GL call.
	void install();
}

#endif
