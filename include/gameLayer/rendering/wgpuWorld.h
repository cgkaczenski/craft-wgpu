#pragma once

// The WebGPU side of the 3D renderer, beside the OpenGL Renderer. Milestone 5
// started it (depth buffer, camera); milestone 6 draws the chunks' opaque
// faces from one GPU buffer arena. Later milestones grow it into the
// WgpuRenderer of docs/roadmap.md.
//
// Names no WebGPU types, so renderer.cpp, chunk.cpp and glfwMain.cpp can call
// it without including webgpu.hpp.

#include <vector>

struct Camera;
struct Chunk;

namespace wgpuWorld
{
	// The corner table (4 corners x 3 floats per face orientation) that
	// Renderer::recreateBlockGeometryData builds for OpenGL's u_vertexData.
	void setBlockGeometry(const std::vector<float> &vertexData);

	// A chunk's opaque faces, 4 ints each, exactly as Chunk::bake made them
	// for OpenGL. Replaces what the chunk had; an empty vector frees it.
	void setChunkFaces(const Chunk *chunk, const std::vector<int> &faces);

	// The chunk is going away: frees its faces.
	void removeChunk(const Chunk *chunk);

	// Records the world's pass for this frame: clears color and depth and
	// draws the opaque faces of `visibleChunks` (the renderer's
	// frustum-culled list, sorted far to near), one draw per chunk. Call
	// inside an open frame, before the 2D layer draws.
	void render(Camera &camera, const std::vector<Chunk *> &visibleChunks);

	// Releases every GPU object. Call before wgpuWindow::shutdown.
	void shutdown();
}
