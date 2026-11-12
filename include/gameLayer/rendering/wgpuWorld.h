#pragma once
#include <rendering/postModifiers.h>
#include <unordered_map>

// The WebGPU side of the 3D renderer, beside the OpenGL Renderer. Milestone 5
// started it (depth buffer, camera); milestone 6 draws the chunks' opaque
// faces from one GPU buffer arena; milestone 7 textures them, milestone 9
// adds the transparent faces and water. Later milestones grow it into the
// WgpuRenderer of docs/roadmap.md.
//
// Names no WebGPU types, so renderer.cpp, chunk.cpp and glfwMain.cpp can call
// it without including webgpu.hpp.

#include <glm/vec2.hpp>
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
		glm::vec4 positionFloat = {};  // xyz the fraction; w the light as baked (sky << 4 | block)
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
	// Meshes are named by ids from newMeshId, which the models and items
	// keep (their `mesh` member) and the draw lists below name.

	// A new mesh id, never 0 and never handed out before.
	unsigned int newMeshId();

	// A mesh's vertices and, if indexed, indices (indexBytes 2 or 4; 0 for
	// none). Models use model.cpp's 36-byte vertex, items 8 floats.
	void setMesh(unsigned int id, const void *vertices, size_t vertexBytes,
		const void *indices, size_t indexCount, unsigned int indexBytes);
	void removeMesh(unsigned int id);

	// One skinned model instance (basicEntity.vert's PerEntityData).
	struct ModelInstance
	{
		unsigned int mesh = 0;         // the model's mesh id
		uint32_t boneBase = 0;         // its first matrix in the frame's bones
		glm::ivec3 positionInt = {};
		glm::vec3 positionFloat = {};
		glm::vec3 color = {1, 1, 1};   // linear tint (a killed creature's red)
		// The light where it stands, as baked: sky light << 4 | block light.
		// entities.wgsl lights it as chunkFaces.wgsl lights a block face, so
		// the time of day and the weather reach it.
		uint32_t light = 0xF0;
		uint32_t textures[4] = {};     // wgpu2d texture ids; 0 draws white
		// The first-person hand: the PSX look keeps its textures in
		// perspective (it would warp right in front of the eye).
		bool firstPerson = false;
	};

	// One item (itemEntityShader's uniforms for one draw).
	struct ItemInstance
	{
		unsigned int mesh = 0;         // the item geometry's mesh id
		uint32_t texture = 0;          // wgpu2d texture id; 0 draws white
		glm::mat4 model = glm::mat4(1.f);
		glm::ivec3 positionInt = {};
		glm::vec3 positionFloat = {};
		uint32_t light = 0xF0;         // the light as baked: sky light << 4 | block light
		bool firstPerson = false;      // the held item: textures kept in perspective in the PSX look
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

	// Milestone G1b: this frame's rain and snow, drawn by renderTransparent
	// after the transparent faces (precipitation.wgsl). Call before it, each
	// frame; intensity 0 (or no columns) draws nothing.
	struct Precipitation
	{
		float intensity = 0;           // the weather's rain, 0..1
		float brightness = 1;          // the light the drops catch (sky light and time of day)
		float wind = 0;                // the rain's lean, blocks sideways per block down
		glm::ivec2 corner = {};        // world block x, z of column (0, 0)
		int size = 0;                  // columns per side
		// Per column, x fastest: x the world y the rain stops at (the top of
		// the highest block that stops it), y 0 none, 1 rain, 2 snow.
		std::vector<glm::vec2> columns;
	};
	void setPrecipitation(const Precipitation &precipitation);

	// The corner table (4 corners x 3 floats per face orientation) and the UV
	// table (4 corners x 2 floats) that Renderer::recreateBlockGeometryData
	// builds for OpenGL's u_vertexData and vertexUV.
	void setBlockGeometry(const std::vector<float> &vertexData, const std::vector<float> &uvData);

	// BlocksLoader::gpuIds, the texture slots a face's texture index picks
	// (4 per block), each a wgpu2d texture id. The world copies those
	// textures into one texture array.
	// `spanned`: BlocksLoader::spannedTextures, the slices of the textures
	// that cover several blocks.
	void setBlockTextures(const std::vector<uint64_t> &slots,
		const std::unordered_map<unsigned int, std::vector<unsigned int>> &spanned);

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
		// Milestone 13: the shadows setting (0 off, 1 hard, 2 soft) and,
		// when on, each cascade's sun matrix as GL builds it: OpenGL clip
		// space (-1..1 deep) from world coordinates. Nearest cascade first.
		int shadows = 0;
		glm::dmat4 shadowCascades[3] = { glm::dmat4(1.0), glm::dmat4(1.0), glm::dmat4(1.0) };

		// Milestone 14: the sky. How much of each of GL's three baked skies
		// (day, twilight, night) the time of day shows (drawBefore's two
		// textures and mix, as weights); the day sky's settings
		// (SkyBoxLoaderAndDrawer::skyConfig: sky color, horizon color, g),
		// which re-bake it when changed; the sun's direction (sunPos, not
		// the main light: the moon is drawn opposite it) and its twilight
		// tint; the sun and moon textures (wgpu2d ids); and the fog.
		glm::vec3 skyWeights = {1, 0, 0};
		glm::vec3 daySkyColor = glm::vec3{9, 47, 116} / 255.f;
		glm::vec3 daySkyHorizon = glm::vec3{33, 38, 44} / 255.f;
		float daySkyG = 0.4f;
		glm::vec3 sunPosition = {0, 1, 0};
		float sunTwilight = 0;
		uint32_t sunTexture = 0;
		uint32_t moonTexture = 0;
		float fogDistance = 100;  // GL's u_fogDistance: the view distance less a chunk
		float fogGradient = 16;   // the fogGradient setting; 0 turns the near fog off
		bool underWater = false;  // GL fogs only above water
		// Milestone G1, the weather: how far the sky goes to grey (and the sun
		// and moon fade) and how much darker that grey is, both 0..1; and
		// GL's rainStrength for the near fog, 0..1.
		float overcast = 0;
		float skyDarken = 0;
		float rain = 0;
		// The snow weather: how much (0..1) of the whiteout fog, which closes
		// into `snowFogColour` (linear) over a few dozen blocks, the sky too.
		float snow = 0;
		glm::vec3 snowFogColour = {0.8f, 0.82f, 0.86f};
		// The PSX look (the psxLook setting): the vertices snapped to a
		// 240-line grid, the textures mapped without perspective correction,
		// the picture drawn on that grid in 15-bit colour with the
		// PlayStation's dither. FXAA is skipped while it is on.
		bool psx = false;
		// How far indoors the camera is, 0..1 (from the sky light where it
		// stands, eased over time). Indoors the whiteout is only on what the
		// open sky lights (out of a window or a door), and the PSX look is
		// lighter: a grid twice as fine (so half the wobble) and the
		// textures in perspective.
		float indoors = 0;
		// The light of what the player holds, as a block light level at the
		// player (0-15; 0 for none, a torch 14): heldLight.wgsl.
		int heldLight = 0;

		// Milestone 15: the post chain's settings (toneMap.frag's uniforms,
		// as the renderer computes them). Lift and gain already mapped
		// (* 0.2 - 0.1 and * 0.4 + 0.8).
		int tonemapper = 0;       // 0 ACES, 1 AgX, 2 ZCAM, 3 Uncharted 2, 4 PBR Neutral
		float saturation = 1;
		float vibrance = 1;
		float gamma = 1;
		float shadowBoost = 0;
		float highlightBoost = 0;
		float vignette = 0.15f;
		glm::vec3 lift = {0, 0, 0};
		glm::vec3 gain = {1, 1, 1};
		bool fxaa = true;
		// Milestone G2: the screen effects running this frame
		// (postModifiers::current()); saturation is already in `saturation`.
		postModifiers::PostLook screenEffects;

		// Milestone 16: bloom, when the bloom setting is on and its
		// multiplier above 0, with filterBloomData's and applyBloomData's
		// uniforms as the renderer computes them: the exposure (the
		// adaptive one plus the setting), the threshold (already halved
		// under water) and the multiplier (already / 10000); the lens dirt
		// textures (wgpu2d ids: lens for bloom; hit and water for the tone
		// map's splats, milestone G3, whose strengths are in screenEffects).
		bool bloom = false;
		float bloomExposure = 1;
		float bloomThreshold = 0.005f;
		float bloomMultiplier = 0;
		uint32_t lensDirtTexture = 0;
		float lensDirtGlare = 0;  // how much the lens dirt shows: Renderer::sunGlare
		uint32_t hitDirtTexture = 0;
		uint32_t waterDirtTexture = 0;

		// Milestone 17: the renderer's ssao switch (GL's HBAO pass) and the
		// view distance its AO fades out at ((squareSize / 2) * 16); the
		// SSR setting (the lit shader's c_SSR).
		bool ssao = true;
		float aoViewDistance = 128;
		bool ssr = true;

		// Milestone 18: under water (`underWater` above), GL's shading
		// settings for the darkening (used as GL uses them, unconverted).
		glm::vec3 underWaterColor = glm::vec3(0, 17, 25) / 255.f;
		float underwaterDarkenStrength = 0.94f;
		float underwaterDarkenDistance = 29;
		float fogGradientUnderWater = 1.9f;

		// 18-parity: the animation. `animationTime` is seconds since start
		// (GL's timeGrass, for the sway, the waves and the underwater warp;
		// GL takes it from std::clock, which is wall time only on Windows);
		// `waterMove` GL's u_waterMove (the renderer's waterTimer). The
		// water's textures (wgpu2d ids): waterDUDV, its normal map, caustics.
		float animationTime = 0;
		float waterMove = 0;
		uint32_t dudvTexture = 0;
		uint32_t dudvNormalTexture = 0;
		uint32_t causticsTexture = 0;
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

	// Milestone 16: the auto exposure's input. The HDR color averaged to
	// one texel (GL's last mip of fboMain.color), linear. A reading takes a
	// few frames to come back; true, with the color, once for each one, so
	// the renderer updates averageLuminosity only when there is a new one,
	// as GL does when its pixel buffer maps.
	bool takeExposureColor(glm::vec3 &color);

	// Milestone 18: how many of the sun's full-size pixels were in front of
	// the sky, a few frames ago: what GL's sunFlareQueries count
	// (GL_SAMPLES_PASSED), for the lens flare.
	int sunVisibleSamples();

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

	// Milestone 20: rebuilds every pipeline from the WGSL files on disk
	// (Renderer::reloadShaders: the R key, or a shading setting that GL
	// recompiles for). All or nothing: if any file fails to compile or a
	// pipeline is rejected, the previous shaders stay. False then, or before
	// the first frame.
	bool reloadShaders();

	// Releases every GPU object. Call before wgpuWindow::shutdown.
	void shutdown();
}
