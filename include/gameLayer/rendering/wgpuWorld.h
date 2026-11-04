#pragma once

// The WebGPU side of the 3D renderer, beside the OpenGL Renderer. Milestone 5
// started it (depth buffer, camera); milestone 6 draws the chunks' opaque
// faces from one GPU buffer arena; milestone 7 textures them, milestone 9
// adds the transparent faces and water. Later milestones grow it into the
// WgpuRenderer of docs/roadmap.md.
//
// Names no WebGPU types, so renderer.cpp, chunk.cpp and glfwMain.cpp can call
// it without including webgpu.hpp.

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <vector>

struct Camera;
struct Chunk;

namespace wgpuWorld
{
	// A dropped block item, drawn as a 0.4-scale cube in the world pass
	// (milestone 10a). Must match entities.wgsl's Cube: 64 bytes.
	struct BlockCube
	{
		glm::ivec4 positionInt = {};   // xyz; w unused
		glm::vec4 positionFloat = {};  // xyz the fraction; w brightness (0..1)
		glm::uvec4 slotsA = {};        // BlocksLoader::gpuIds slots of faces 0-3
		glm::uvec4 slotsB = {};        // faces 4-5 in xy
	};

	// The 36 vertices of GL's cube entity, 8 floats each (position, normal,
	// uv), as Renderer builds them for blockEntityshader.vaoCube.
	void setBlockCubeGeometry(const std::vector<float> &vertices);

	// The corner table (4 corners x 3 floats per face orientation) and the UV
	// table (4 corners x 2 floats) that Renderer::recreateBlockGeometryData
	// builds for OpenGL's u_vertexData and vertexUV.
	void setBlockGeometry(const std::vector<float> &vertexData, const std::vector<float> &uvData);

	// BlocksLoader::gpuIds, the bindless handle table the GL shader indexes
	// with a face's texture index. On WebGPU each handle carries a wgpu2d
	// texture id in its low 32 bits (nullGL's getTextureHandle); the world
	// copies those textures into one texture array.
	void setBlockTextures(const std::vector<uint64_t> &handles);

	// A chunk's opaque faces, 4 ints each, exactly as Chunk::bake made them
	// for OpenGL. Replaces what the chunk had; an empty vector frees it.
	void setChunkFaces(const Chunk *chunk, const std::vector<int> &faces);

	// A chunk's transparent faces (water, ice, glass, and decals), as
	// Chunk::bake made them: sorted far to near from the player when baked.
	// Replaces what the chunk had; an empty vector frees it.
	void setChunkTransparentFaces(const Chunk *chunk, const std::vector<int> &faces);

	// The chunk is going away: frees its faces.
	void removeChunk(const Chunk *chunk);

	// Records the world's passes for this frame: clears color and depth,
	// draws the opaque faces of `visibleChunks` (the renderer's
	// frustum-culled list, sorted far to near) and the dropped block cubes,
	// then their transparent faces
	// blended back to front. With `waterPeel` (GL's waterType 1) water shows
	// what is under it through a depth peel; without it, water is plain
	// see-through. `waterColor` is GL's u_waterColor, in linear space. Call
	// inside an open frame, before the 2D layer draws.
	void render(Camera &camera, const std::vector<Chunk *> &visibleChunks,
		const std::vector<BlockCube> &blockCubes, const glm::vec3 &waterColor, bool waterPeel);

	// The block-breaking crack over a block's faces (Renderer::renderDecal):
	// `faces` in the chunk face format, `crackFrame` 0-10, `crackTexture` a
	// wgpu2d texture id. Draws at once, after the world, on its depth. Call
	// after render in the same frame (it reuses its camera).
	void renderDecal(const std::vector<int> &faces, int crackFrame, unsigned int crackTexture);

	// Line segments, two camera-relative points each (GyzmosRenderer's cubes
	// and lines). Draws at once, after the world, on its depth.
	void renderLines(const std::vector<glm::vec3> &points);

	// Releases every GPU object. Call before wgpuWindow::shutdown.
	void shutdown();
}
