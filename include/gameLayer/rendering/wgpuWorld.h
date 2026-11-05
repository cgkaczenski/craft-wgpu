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
#include <glm/mat4x4.hpp>

#include <cstddef>
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

	// GL's entity geometry, 8 floats a vertex (position, normal, uv), as
	// Renderer builds it for blockEntityshader.vaoCube: the cube's 36 vertices
	// first, then each block model's (blockGeometryForEntities ranges).
	void setBlockCubeGeometry(const std::vector<float> &vertices);

	// One inventory icon (Renderer::renderAllBlocksUiTextures): a range of
	// the entity geometry drawn into a wgpu2d render target.
	struct BlockIcon
	{
		glm::mat4 viewProjection = glm::mat4(1.f); // GL's, -1..1 depth
		glm::uvec4 slotsA = {};                    // faces 0-3
		glm::uvec4 slotsB = {};                    // faces 4-5 in xy
		bool oneTexture = false;                   // every face takes slotsA.x
		uint32_t firstVertex = 0;
		uint32_t vertexCount = 0;
		uint32_t target = 0;                       // wgpu2d::FrameBuffer texture id
	};

	// Draws the icons now, on a command buffer of their own: needs no open
	// frame. Each target is cleared to transparent first. Every target must
	// be `size` pixels square, in the surface's format.
	void renderBlockIcons(const std::vector<BlockIcon> &icons, int size);

	// ---- Models and items (milestone 10b) ------------------------------
	//
	// Meshes are named by the GL vertex array their data was made for (the
	// null GL scaffold hands out unique names), so the draw lists below can
	// name them the way the GL code binds them.

	// A mesh's vertices and, if indexed, indices (indexBytes 2 or 4; 0 for
	// none). Models use model.cpp's 36-byte vertex, items 8 floats.
	void setMesh(unsigned int vao, const void *vertices, size_t vertexBytes,
		const void *indices, size_t indexCount, unsigned int indexBytes);
	void removeMesh(unsigned int vao);

	// One skinned model instance (basicEntity.vert's PerEntityData).
	struct ModelInstance
	{
		unsigned int mesh = 0;         // the model's vao
		uint32_t boneBase = 0;         // its first matrix in the frame's bones
		glm::ivec3 positionInt = {};
		glm::vec3 positionFloat = {};
		glm::vec3 brightness = {1, 1, 1}; // output-space color factor
		uint32_t textures[4] = {};     // wgpu2d texture ids; 0 draws white
	};

	// One item (itemEntityShader's uniforms for one draw).
	struct ItemInstance
	{
		unsigned int mesh = 0;         // the item geometry's vao
		uint32_t texture = 0;          // wgpu2d texture id; 0 draws white
		glm::mat4 model = glm::mat4(1.f);
		glm::ivec3 positionInt = {};
		glm::vec3 positionFloat = {};
		float brightness = 1.f;        // output-space factor
	};

	// The entity pass (Renderer::renderEntities): skinned models (their
	// bone matrices, concatenated), then items. Call after render and
	// before renderTransparent, as GL draws entities between its solid pass
	// and the transparent ones.
	void renderEntities(const std::vector<glm::mat4> &bones, const std::vector<ModelInstance> &models,
		const std::vector<ItemInstance> &items);

	// The transparent passes render prepared (water peel, glass), then the
	// composite: the HDR color onto the screen (WGPU_GBUFFER_VIEW=1-5 shows
	// another attachment instead). Call once after renderEntities; the
	// decal and the gizmo lines draw after it, on the screen.
	void renderTransparent();

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

	// The chunk is going away: frees its faces and its lights.
	void removeChunk(const Chunk *chunk);

	// The chunk's light blocks (its `lights`, as GL uploads them to the
	// chunk's lightsBuffer): xyz block positions. render gathers the visible
	// chunks' lists into the point lights.
	void setChunkLights(const Chunk *chunk, const std::vector<glm::ivec4> &lights);

	// What lights the world this frame (milestone 12): the values
	// renderFromBakedData computes for defaultShader's uniforms.
	struct Lighting
	{
		glm::vec3 sunDirection = {0, 1, 0};  // toward the sun or moon (mainLightPosition)
		glm::vec3 sunLightColor = {1, 1, 1}; // linear, from the time of day
		glm::vec3 ambientColor = {1, 1, 1};  // linear, from the time of day
		int skyLightIntensity = 15;          // 0-15, the time of day's sky light
		float baseAmbientExtra = 0;          // adaptiveExposure.bonusAmbient
		float exposure = 1;                  // adaptiveExposure.currentExposure
		bool useLights = true;               // the useLights setting
		int maxLights = 40;                  // the maxLights setting
		uint32_t aoTexture = 0;              // ao.png, a wgpu2d texture id
	};

	// Records the world's opaque pass for this frame and prepares the
	// transparent ones (renderTransparent records them): clears the
	// G-buffer (GL's fboMain: HDR color, positions with the roughness in
	// their alpha, normals, bloom) and depth, lit by `lighting`, draws the opaque faces of `visibleChunks` (the
	// renderer's frustum-culled list, sorted far to near) and the dropped
	// block cubes into it, then, later, their transparent faces blended back
	// to front. With `depthPrepass` (the renderer's zprepass) the faces go
	// through a depth-only pass first and are shaded where depth is Equal.
	// With `waterPeel` (GL's waterType 1) water shows what is under it
	// through a depth peel; without it, water is plain see-through.
	// `waterColor` is GL's u_waterColor, in linear space. Call inside an open
	// frame, before the 2D layer draws.
	void render(Camera &camera, const std::vector<Chunk *> &visibleChunks,
		const std::vector<BlockCube> &blockCubes, const glm::vec3 &waterColor, bool waterPeel,
		bool depthPrepass, const Lighting &lighting);

	// Milestone 11's debug views of the G-buffer, for ImGui: view `view`
	// (0 to gBufferViewCountTotal() - 1, named by gBufferViewName) as a
	// wgpu2d texture id, or 0 before its first frame. Asking is what makes
	// the next frame draw it, so a closed ImGui header costs nothing. Rows
	// are top first (ImGui's default uvs).
	uint32_t gBufferPreview(int view);
	const char *gBufferViewName(int view);
	int gBufferViewCountTotal();

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
