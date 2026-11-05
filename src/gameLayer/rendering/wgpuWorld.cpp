#if RENDERER_WEBGPU

// The WebGPU world renderer. See include/gameLayer/rendering/wgpuWorld.h.
//
// Milestone 5 brought the depth texture, the 0..1 projection and the world
// pass that runs before the 2D layer. Milestone 6 adds the chunks:
//   - a face arena: one vertex buffer holding every chunk's opaque faces, a
//     contiguous range per chunk, read with instance step mode (one instance
//     per face) -- BigGpuBuffer's idea, which the GL path has switched off;
//   - the corner table as a read-only storage buffer the vertex shader
//     indexes, as OpenGL's u_vertexData;
//   - one draw per visible chunk, with firstInstance pointing at its range.
// Milestone 7 textures them without bindless:
//   - the UV table beside the corner table, both storage buffers;
//   - one 128x128 texture_2d_array, a layer per distinct block texture, filled
//     and mipmapped by blockTextures.wgsl from the loader's wgpu2d textures;
//   - a slot -> layer table standing in for the u_textureSamplerers handles.
// Milestone 8 draws the visible chunks with indirect draws, as GL's
// glMultiDrawElementsIndirect: one 16-byte argument record per chunk, built
// from the culled list and written to an indirect buffer each frame, then one
// multiDrawIndirect (or a loop of drawIndirect). OURCRAFT_WORLD_DRAWS=
// direct|indirect|multi picks the path, for comparing them.
// Milestone 9 adds the transparent faces (water, ice, glass) and GL's water
// depth peel, after the opaque pass:
//   copy depth -> peel depth; water-only depth pass into the peel depth (it
//   now holds the nearest water surface); peel pass: transparent faces beyond
//   that surface; copy color and depth; final pass: every transparent face,
//   blended, water reading the copies (what is under it, and how deep).
// Transparent faces live in the same arena, a second range per chunk.
// Milestone 10a adds the dropped block items (instanced cubes in the opaque
// pass), the block-breaking crack decal and GyzmosRenderer's lines (the
// block-selection outline), each in a pass of its own after the world.

#include "rendering/wgpuWorld.h"
#include "rendering/camera.h"
#include "chunk.h"
#include "rendering/frustumCulling.h"

#include <render/wgpuFrame.h>
#include <render/wgpu2d.h>
#include <webgpu/webgpu.hpp>

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <unordered_map>

using namespace wgpu;
using namespace render; // wgpu2d's device, queue and frame

namespace wgpuWorld
{

namespace
{
	// Must match the WGSL Uniforms struct in world.wgsl, member for member:
	// every member is 16-byte aligned, so there is no padding.
	struct Uniforms
	{
		glm::mat4 viewProjection;
		glm::ivec4 positionInt;
		glm::vec4 positionFloat;
		glm::vec4 waterColor;    // rgb, linear (GL's u_waterColor)
		glm::vec4 planes;        // x near, y far, z crack frame, w exposure
		glm::mat4 view;          // Camera::getViewMatrix, for G-buffer positions
		glm::vec4 sunDirection;  // xyz toward the sun or moon; w GL's u_baseAmbientExtra
		glm::vec4 sunLightColor; // rgb
		glm::vec4 ambientColor;  // rgb
		glm::ivec4 lightParams;  // x sky light intensity, y point lights, z shadows setting, w SSR
		glm::mat4 shadowMatrices[3]; // camera-relative to each cascade's clip space, 0..1 deep
		glm::vec4 fogParams;         // x fog distance, y near fog gradient, z underwater
		glm::mat4 projection;        // view to clip space, for the reflections (milestone 17)
		glm::vec4 underWaterColor;   // rgb the color, w darken strength (milestone 18)
		glm::vec4 underWaterParams;  // x darken distance, y fog gradient under water
		glm::vec4 animation;         // x seconds (GL's u_timeGrass), y GL's u_waterMove (18-parity)
	};
	static_assert(sizeof(Uniforms) == 576, "uniform layout must match world.wgsl");

	// Milestone 17: hbao.wgsl's HbaoUniforms.
	struct HbaoUniforms
	{
		glm::mat4 view;       // world normals to view space
		glm::mat4 projection; // view to clip space
		glm::vec4 params;     // x the view distance the AO fades out at
	};
	static_assert(sizeof(HbaoUniforms) == 144, "must match hbao.wgsl's HbaoUniforms");

	// Milestone 14: sky.wgsl's SkyUniforms and BakeParams.
	struct SkyUniforms
	{
		glm::mat4 inverseViewProjection; // a pixel back to its direction
		glm::vec4 weights;               // day, twilight, night
		glm::mat4 sunMatrix;             // SunRenderer's, GL clip space
		glm::mat4 moonMatrix;
		glm::vec4 sunParams;             // x twilight, y color multiplier
		glm::vec4 moonParams;
		glm::vec4 raySunParams;          // the same for the god rays' copies (milestone 18)
		glm::vec4 rayMoonParams;
	};
	static_assert(sizeof(SkyUniforms) == 272, "must match sky.wgsl's SkyUniforms");

	// Milestone 18: godRays.wgsl's RayUniforms and underwater.wgsl's WarpUniforms.
	struct RayUniforms
	{
		glm::vec4 center; // xy the sun on screen, GL's texture coordinates
	};
	struct WarpUniforms
	{
		glm::vec4 color;  // rgb the underwater color
		glm::vec4 params; // x time
	};

	struct SkyBakeParams
	{
		glm::vec4 lightPos;
		glm::vec4 color1;
		glm::vec4 color2;
		glm::vec4 groundColor; // w: g
		glm::uvec4 flags;      // x: use the ground color
	};
	static_assert(sizeof(SkyBakeParams) == 80, "must match sky.wgsl's BakeParams");

	// Milestone 15: post.wgsl's PostUniforms.
	struct PostUniforms
	{
		glm::vec4 grade; // exposure, saturation, vibrance, gamma
		glm::vec4 boost; // shadow boost, highlight boost, vignette
		glm::vec4 lift;
		glm::vec4 gain;
		glm::ivec4 mode; // x tone mapper
	};
	static_assert(sizeof(PostUniforms) == 80, "must match post.wgsl's PostUniforms");

	// Milestone 16: bloom.wgsl's BloomUniforms.
	struct BloomUniforms
	{
		glm::vec4 params; // exposure, threshold, multiplier
		glm::vec4 dirt;   // hit intensity, water drops
	};
	static_assert(sizeof(BloomUniforms) == 32, "must match bloom.wgsl's BloomUniforms");

	// GL's three skies, each a cubemap of 128x128 faces.
	enum SkyKind { DaySky, TwilightSky, NightSky, SkyKinds };
	constexpr uint32_t skyCubeSize = 128;

	// Milestone 13: the sun's shadow cascades, GL's SunShadow sizes, nearest
	// first.
	constexpr int shadowCascades = 3;
	constexpr uint32_t shadowMapSizes[shadowCascades] = { 2048, 2048, 1024 };

	// One face: short orientation, short texture, ivec3 position.
	constexpr uint64_t faceBytes = 4 * sizeof(int);

	// The depth convention: 0..1, near at 0, Less (docs/roadmap.md).
	constexpr TextureFormat depthFormat = TextureFormat::Depth32Float;
	constexpr float depthClear = 1.0f;

	// Milestone 11: fboMain's targets (docs/roadmap.md, "Render targets"),
	// in defaultShader.frag's output order, less one. WebGPU limits what a
	// pixel's color attachments may cost together (maxColorAttachmentBytes-
	// PerSample, 32 by default), and as a render target rg11b10ufloat costs
	// 8 bytes, not the 4 it stores: GL's five (8 + 8 + 8 + 8 + 1) come to
	// 33. GL's positions are RGB16F with no alpha, and here they are rgba16f,
	// so the roughness (GL's fifth target, an r8) rides in their alpha: four
	// targets, exactly 32. HDR color and bloom are rg11b10ufloat when the
	// device can render to it, else rgba16float (also 8 bytes).
	enum GBufferSlot { GColor, GPosition, GNormal, GBloom, GBufferSlots };
	const char *const gBufferSlotNames[GBufferSlots] =
		{ "world HDR color", "world positions and roughness", "world normals", "world bloom" };

	// The debug views (gbufferView.wgsl's `view`): the four targets, the
	// roughness (the positions' alpha) and the depth. The view shader's
	// bindings: the four targets, the depth, the uniforms.
	constexpr int gBufferViewCount = 6;
	constexpr int viewBindings = GBufferSlots + 2;
	const char *const gBufferViewLabels[gBufferViewCount] =
		{ "HDR color", "View-space position", "Normal", "Bloom", "Roughness", "Depth" };
	constexpr int previewWidth = 320; // the views' targets; height follows the aspect

	// The arena starts at a million faces (16 MiB) and doubles when full.
	constexpr uint64_t initialArenaFaces = 1u << 20;

	// What one indirect draw reads from the buffer: drawIndirect's four
	// u32s, in this order (WebGPU spec, GPURenderPassEncoder.drawIndirect).
	struct DrawArgs
	{
		uint32_t vertexCount;
		uint32_t instanceCount;
		uint32_t firstVertex;
		uint32_t firstInstance; // nonzero needs IndirectFirstInstance
	};
	static_assert(sizeof(DrawArgs) == 16, "indirect draw arguments are four u32s");

	// How the visible chunks are drawn.
	enum class DrawMode
	{
		Direct,   // one draw() per chunk, arguments in the call (milestone 6)
		Indirect, // one drawIndirect() per chunk, arguments from the buffer
		Multi,    // one multiDrawIndirect() for all of them
	};

	const char *drawModeName(DrawMode mode)
	{
		switch (mode)
		{
			case DrawMode::Direct: return "direct";
			case DrawMode::Indirect: return "indirect";
			case DrawMode::Multi: return "multi";
		}
		return "?";
	}

	// Frames skipped before timing starts: chunks are still loading then.
	constexpr uint64_t timingWarmupFrames = 300;

	// A model or item mesh: vertices, and indices when indexed.
	struct Mesh
	{
		Buffer vertices = nullptr;
		Buffer indices = nullptr;
		IndexFormat indexFormat = IndexFormat::Undefined;
		uint32_t count = 0; // indices if indexed, else vertices
	};

	// What entities.wgsl's Model and Item read; 48 and 96 bytes.
	struct ModelGpu
	{
		glm::ivec4 positionInt;  // w: first bone
		glm::vec4 positionFloat;
		glm::vec4 brightness;
	};
	static_assert(sizeof(ModelGpu) == 48, "must match entities.wgsl's Model");

	struct ItemGpu
	{
		glm::mat4 model;
		glm::ivec4 positionInt;
		glm::vec4 positionFloat; // w: brightness
	};
	static_assert(sizeof(ItemGpu) == 96, "must match entities.wgsl's Item");

	struct ChunkRange
	{
		uint32_t firstFace = 0;
		uint32_t faceCount = 0;
	};

	struct World
	{
		bool initialized = false;
		bool failed = false; // creation failed once; don't retry every frame

		RenderPipeline pipeline = nullptr;
		PipelineLayout pipelineLayout = nullptr;
		BindGroupLayout bindGroupLayout = nullptr;

		// Transparency: the water depth pass, the peel, and the final pass with
		// and without the peel's information (GL's waterType 1 and 0).
		RenderPipeline waterDepthPipeline = nullptr;
		RenderPipeline peelPipeline = nullptr;
		RenderPipeline finalPeelPipeline = nullptr;
		RenderPipeline finalPlainPipeline = nullptr;
		PipelineLayout transparentPipelineLayout = nullptr;
		BindGroupLayout peelBindGroupLayout = nullptr;
		BindGroup peelBindGroup = nullptr; // the two copies below; remade with them

		// Milestone 10a: dropped block cubes, the crack decal, gizmo lines.
		RenderPipeline cubePipeline = nullptr;
		RenderPipeline decalPipeline = nullptr;
		RenderPipeline linePipeline = nullptr;
		PipelineLayout cubePipelineLayout = nullptr;
		PipelineLayout decalPipelineLayout = nullptr;
		BindGroupLayout cubeBindGroupLayout = nullptr;
		BindGroupLayout decalBindGroupLayout = nullptr;

		// The entity geometry, 8 floats a vertex: the cube's 36, then the
		// block models' (setBlockCubeGeometry).
		std::vector<float> cubeGeometry;
		bool cubeGeometryDirty = false;
		Buffer cubeVertexBuffer = nullptr;
		Buffer cubeInstanceBuffer = nullptr;
		uint64_t cubeInstanceCapacity = 0; // in cubes
		BindGroup cubeBindGroup = nullptr;  // the two buffers above

		Buffer decalFaceBuffer = nullptr;
		BindGroup decalBindGroup = nullptr;
		uint32_t decalTextureId = 0;        // the texture decalBindGroup names

		// Milestone 10b: the inventory's block icons, drawn into wgpu2d
		// targets (renderBlockIcons). Made on first use.
		RenderPipeline iconPipeline = nullptr;

		// Milestone 11: the G-buffer and what reads it. `pipeline` is the
		// solid pass after the prepass (Equal, no depth writes).
		TextureFormat hdrFormat = TextureFormat::RGBA16Float;
		RenderPipeline prepassPipeline = nullptr;   // depth only, every target masked
		RenderPipeline solidLessPipeline = nullptr; // the solid pass with the prepass off
		Texture gBufferTextures[GBufferSlots] = {};
		TextureView gBufferViews[GBufferSlots] = {};
		TextureView depthSampleView = nullptr;      // the depth, for the depth view
		BindGroupLayout viewBindGroupLayout = nullptr;
		PipelineLayout viewPipelineLayout = nullptr;
		BindGroup viewBindGroup = nullptr;          // the targets above; remade with them
		RenderPipeline viewPipelines[gBufferViewCount] = {};
		bool frameRendered = false;  // render drew the G-buffer; renderTransparent composites it
		int compositeView = 0;       // WGPU_GBUFFER_VIEW: which view the composite shows
		wgpu2d::FrameBuffer previews[gBufferViewCount];
		uint32_t previewsWanted = 0; // bits: views asked for since the last frame
		uint32_t previewsDrawn = 0;  // bits: views with a frame drawn in them

		// Milestone 12: the point lights and the AO texture (group 0's 6-8).
		std::unordered_map<const Chunk *, std::vector<glm::ivec4>> chunkLights;
		std::vector<glm::ivec4> frameLights; // this frame's, gathered
		Buffer lightBuffer = nullptr;
		uint64_t lightCapacity = 0;          // in lights
		uint32_t aoTextureId = 0;            // the wgpu2d texture bound at 7
		Sampler aoSampler = nullptr;
		TextureView boundAoView = nullptr;   // what the bind group names (not owned)

		// 18-parity: the water's textures (GL's u_dudv, u_dudvNormal and
		// u_caustics; wgpu2d ids) at 13-15, and their sampler at 16.
		uint32_t waterTextureIds[3] = {};
		Sampler waterSampler = nullptr;
		TextureView boundWaterViews[3] = {};

		// Milestone 13: the sun's shadow maps, made the first frame shadows
		// are on. A 1x1 depth stands in for them in the bind group until
		// then, and always in `shadowPassBindGroup`, the copy the shadow
		// passes bind: a pass may not draw into a texture it also binds.
		bool shadowsWanted = false;          // render's setting, for updateResources
		Texture shadowTextures[shadowCascades] = {};
		TextureView shadowViews[shadowCascades] = {};
		Texture blankDepthTexture = nullptr;
		TextureView blankDepthView = nullptr;
		Sampler shadowSampler = nullptr;     // comparison, nearest
		TextureView boundShadowViews[shadowCascades] = {}; // not owned
		BindGroup shadowPassBindGroup = nullptr;
		RenderPipeline shadowPipelines[shadowCascades] = {};
		uint64_t lastShadowFaces[shadowCascades] = {};
		uint32_t lastShadowChunks[shadowCascades] = {};

		// Milestone 14: the sky (sky.wgsl). The three baked skies, each a
		// 2D texture of six layers: one Cube view to sample it, and a 2D
		// view of each layer to draw that face.
		Texture skyCubes[SkyKinds] = {};
		TextureView skyCubeViews[SkyKinds] = {};
		TextureView skyFaceViews[SkyKinds][6] = {};
		bool skyBaked = false;
		glm::vec3 bakedDayColor = {}, bakedDayHorizon = {};
		float bakedDayG = -1;
		Buffer skyBakeBuffer = nullptr;
		Buffer skyUniformBuffer = nullptr;
		BindGroupLayout skyBakeBindGroupLayout = nullptr;
		BindGroupLayout skyBindGroupLayout = nullptr;
		PipelineLayout skyBakePipelineLayout = nullptr;
		PipelineLayout skyPipelineLayout = nullptr;
		RenderPipeline skyBakePipeline = nullptr;
		RenderPipeline skyPipeline = nullptr;
		RenderPipeline sunPipeline = nullptr;
		BindGroup skyBakeBindGroup = nullptr;
		BindGroup skyBindGroup = nullptr;
		Sampler skySampler = nullptr;
		TextureView boundSunView = nullptr;  // what skyBindGroup names (not owned)
		TextureView boundMoonView = nullptr;
		bool sunDrawable = false;            // both textures loaded
		bool sunPending = false;             // render drew the sky; renderTransparent adds the sun
		// The sky behind the world, the size of the surface (GL's
		// fboSkyBox): what the fog mixes toward.
		Texture skyTexture = nullptr;
		TextureView skyView = nullptr;

		// Milestone 15: the post chain (post.wgsl). The tone mapper reads
		// the HDR color; with FXAA on it writes the FXAA input (GL's
		// fboCoppy, in its rg11b10ufloat), which FXAA reads onto the
		// surface, else it writes the surface itself.
		BindGroupLayout postBindGroupLayout = nullptr;
		PipelineLayout postPipelineLayout = nullptr;
		RenderPipeline toneMapToTexturePipeline = nullptr;
		RenderPipeline toneMapToSurfacePipeline = nullptr;
		RenderPipeline fxaaPipeline = nullptr;
		Buffer postUniformBuffer = nullptr;
		Sampler postSampler = nullptr;
		Texture fxaaInputTexture = nullptr; // the surface's size; remade with the targets
		TextureView fxaaInputView = nullptr;
		BindGroup toneMapBindGroup = nullptr;   // reads the HDR color
		BindGroup fxaaBindGroup = nullptr;      // reads the FXAA input
		bool fxaa = true;                       // this frame's setting

		// Milestone 16: bloom and the auto exposure (bloom.wgsl). Every
		// pass reads one mip through one of these groups (uniforms, the
		// view, the sampler); the apply's group adds the lens dirt.
		BindGroupLayout bloomBindGroupLayout = nullptr;
		BindGroupLayout bloomApplyBindGroupLayout = nullptr;
		PipelineLayout bloomPipelineLayout = nullptr;
		PipelineLayout bloomApplyPipelineLayout = nullptr;
		RenderPipeline bloomFilterPipeline = nullptr;  // into the G-buffer's bloom, added
		RenderPipeline bloomDownPipeline = nullptr;
		RenderPipeline bloomTentPipeline = nullptr;    // the copy at each level
		RenderPipeline bloomUpPipeline = nullptr;      // the same, added onto the level above
		RenderPipeline bloomBlurPipelines[2] = {};     // vertical, horizontal (GL's u_horizontal)
		RenderPipeline bloomApplyPipeline = nullptr;   // onto the HDR color, added
		RenderPipeline exposurePipeline = nullptr;     // one level of the average
		Buffer bloomUniformBuffer = nullptr;
		Sampler dirtSampler = nullptr;                 // nearest: gl2d loads the dirt pixelated
		Texture blackTexture = nullptr;                // a missing dirt texture adds nothing
		TextureView blackView = nullptr;
		bool bloom = false;                            // this frame's setting
		uint32_t dirtTextures[3] = {};                 // lens, hit, water: wgpu2d ids
		// Sized with the targets: GL's bluredColorBuffer[2], mip 0 half the
		// surface, bloomMips + 1 levels (GL's currentMips), a view and a
		// group per level; the groups reading the HDR color and the
		// G-buffer's bloom; and the apply's group, remade when its views
		// change (it reads whichever buffer the sequence ends in).
		int bloomMips = 0;
		Texture blurTextures[2] = {};
		std::vector<TextureView> blurViews[2];
		std::vector<BindGroup> blurGroups[2];
		BindGroup colorSourceGroup = nullptr;
		BindGroup bloomSourceGroup = nullptr;
		BindGroup bloomApplyGroup = nullptr;
		TextureView boundApplyViews[6] = {};           // what bloomApplyGroup names (not owned)
		// The exposure's average: rgba16float, level 0 half the surface,
		// down to 1x1 (GL's glGenerateMipmap of fboMain.color), then that
		// texel copied into a mappable buffer and read a few frames later.
		Texture exposureTexture = nullptr;
		std::vector<TextureView> exposureViews;
		std::vector<BindGroup> exposureGroups;     // each reading its level
		Buffer exposureReadBuffer = nullptr;
		enum class Readback { Idle, Copied, Mapping, Mapped };
		Readback exposureState = Readback::Idle;
		glm::vec3 exposureColor = {0, 0, 0};
		bool exposureFresh = false;                // a reading takeExposureColor has not given out
		uint32_t exposureReadings = 0;             // for the shutdown line
		int bloomLast = -1;                        // the blur buffer this frame's bloom ended in

		// Milestone 17: the ambient occlusion (hbao.wgsl) and the
		// reflections' history. The AO pass and its apply each have their
		// own group: a pass may not read the texture it draws into, even
		// through a binding its shader leaves alone.
		BindGroupLayout hbaoBindGroupLayout = nullptr;      // uniforms, positions, normals, sampler
		BindGroupLayout hbaoApplyBindGroupLayout = nullptr; // uniforms, positions, the AO, sampler
		PipelineLayout hbaoPipelineLayout = nullptr;
		PipelineLayout hbaoApplyPipelineLayout = nullptr;
		RenderPipeline hbaoPipeline = nullptr;
		RenderPipeline hbaoApplyPipeline = nullptr;   // black over the HDR color, by the AO's alpha
		Buffer hbaoUniformBuffer = nullptr;
		bool ssao = true;                             // this frame's setting
		Texture hbaoTexture = nullptr;                // half the surface, r8unorm (GL's fboHBAO, GL_RED)
		TextureView hbaoView = nullptr;
		BindGroup hbaoBindGroup = nullptr;
		BindGroup hbaoApplyBindGroup = nullptr;
		// Last frame's HDR color (GL's fboLastFrame, RGBA8: drawn, since a
		// copy cannot convert formats) and positions (copied), the size of
		// the surface; the frame group reads them.
		RenderPipeline lastColorPipeline = nullptr;
		Texture lastColorTexture = nullptr;
		TextureView lastColorView = nullptr;
		Texture lastPositionsTexture = nullptr;
		TextureView lastPositionsView = nullptr;

		// Milestone 18: the god rays (sky.wgsl's fs_sunRays, godRays.wgsl),
		// the flare's count of visible sun pixels, and the underwater warp
		// (underwater.wgsl).
		BindGroupLayout sunCountBindGroupLayout = nullptr; // the scene depth, the counter
		PipelineLayout sunRaysPipelineLayout = nullptr;    // the sky's group, then that one
		RenderPipeline sunRaysPipeline = nullptr;
		BindGroupLayout rayMaskBindGroupLayout = nullptr;  // the scene depth
		BindGroupLayout rayBlurBindGroupLayout = nullptr;  // uniforms, the masked sun, sampler
		PipelineLayout rayMaskPipelineLayout = nullptr;
		PipelineLayout rayBlurPipelineLayout = nullptr;
		RenderPipeline rayMaskPipeline = nullptr;
		RenderPipeline rayBlurPipeline = nullptr;
		Buffer rayUniformBuffer = nullptr;
		Buffer sunCountBuffer = nullptr;   // an atomic, cleared each frame
		Buffer sunReadBuffer = nullptr;    // its copy, mapped
		Readback sunReadState = Readback::Idle;
		uint32_t sunSamples = 0;           // the latest count, full-size pixels
		uint32_t sunReadings = 0;          // for the shutdown line
		BindGroupLayout warpBindGroupLayout = nullptr;
		PipelineLayout warpPipelineLayout = nullptr;
		RenderPipeline warpPipeline = nullptr;
		Buffer warpUniformBuffer = nullptr;
		bool underWater = false;           // this frame's camera
		// Sized with the targets: the half-size sun and its blur (GL's
		// fboSunForGodRays and ...Second, R11F_G11F_B10F), the warp's
		// output, and their groups.
		Texture sunRaysTexture = nullptr;
		TextureView sunRaysView = nullptr;
		Texture sunRaysBlurTexture = nullptr;
		TextureView sunRaysBlurView = nullptr;
		Texture warpTexture = nullptr;
		TextureView warpView = nullptr;
		BindGroup sunCountBindGroup = nullptr;
		BindGroup rayMaskBindGroup = nullptr;
		BindGroup rayBlurBindGroup = nullptr;
		BindGroup warpBindGroup = nullptr;
		BindGroup toneMapWarpBindGroup = nullptr; // the tone mapper reading the warp

		Buffer lineBuffer = nullptr;
		uint64_t lineCapacity = 0;          // in points

		// render's transparent passes, waiting for renderTransparent.
		bool pendingTransparent = false;
		struct
		{
			DrawMode mode = DrawMode::Direct;
			bool waterPeel = false;
			uint32_t peelFirst = 0, peelCount = 0, finalFirst = 0, finalCount = 0;
			int width = 0, height = 0;
		} pending;

		// Milestone 10b: meshes by GL vao, the model and item pipelines, and
		// the frame's bones and instances.
		std::unordered_map<unsigned int, Mesh> meshes;
		RenderPipeline modelPipeline = nullptr;
		RenderPipeline itemPipeline = nullptr;
		PipelineLayout entityPipelineLayout = nullptr;
		BindGroupLayout entityBindGroupLayout = nullptr;   // group 1: bones, models, items
		BindGroupLayout textureBindGroupLayout = nullptr;  // group 2: four textures
		Buffer boneBuffer = nullptr;
		uint64_t boneCapacity = 0;
		Buffer modelBuffer = nullptr;
		uint64_t modelCapacity = 0;
		Buffer itemBuffer = nullptr;
		uint64_t itemCapacity = 0;
		BindGroup entityBindGroup = nullptr;
		Texture whiteTexture = nullptr;      // stands in for a missing texture
		TextureView whiteView = nullptr;
		std::map<std::array<uint32_t, 4>, BindGroup> textureGroups;
		uint32_t lastModelDraws = 0;
		uint32_t lastItemDraws = 0;
		BindGroup bindGroup = nullptr; // everything below; remade when any of it changes
		Buffer uniformBuffer = nullptr;

		// The corner table: kept on the CPU until it can be uploaded, and
		// uploaded again whenever the renderer rebuilds it.
		std::vector<float> blockGeometry;
		bool blockGeometryDirty = false;
		Buffer blockGeometryBuffer = nullptr;
		std::vector<float> blockUVs; // 4 corners x 2 floats per orientation
		Buffer blockUVBuffer = nullptr;

		// The loader's handle table (BlocksLoader::gpuIds) and what is built
		// from it: the texture array, its view, the slot -> layer table.
		std::vector<uint64_t> textureHandles;
		bool texturesDirty = false;
		Texture textureArray = nullptr;
		TextureView textureArrayView = nullptr;
		Buffer textureLayerBuffer = nullptr;
		Sampler sampler = nullptr;
		ComputePipeline fillPipeline = nullptr;
		ComputePipeline downsamplePipeline = nullptr;

		// The face arena and its allocator, in faces. `freeRanges` maps the
		// first face of each free range to its length, kept merged.
		Buffer arena = nullptr;
		uint64_t arenaFaces = 0;
		std::map<uint32_t, uint32_t> freeRanges;
		std::unordered_map<const Chunk *, ChunkRange> chunks;
		std::unordered_map<const Chunk *, ChunkRange> transparentChunks;
		uint64_t facesInUse = 0;

		Texture depthTexture = nullptr;
		TextureView depthView = nullptr;

		// The peel's copies, the size of the surface: GL's fboCoppy.
		Texture peelDepthTexture = nullptr;
		TextureView peelDepthView = nullptr;
		Texture peelColorTexture = nullptr;
		TextureView peelColorView = nullptr;
		int depthWidth = 0;
		int depthHeight = 0;

		bool firstRenderReported = false;

		// Indirect draws: this frame's argument records, and the buffer they
		// are written to (capacity in records, doubled when outgrown).
		DrawMode drawMode = DrawMode::Direct;
		std::vector<DrawArgs> drawArgs;
		Buffer indirectBuffer = nullptr;
		uint64_t indirectCapacity = 0;

		// Timing, after the warm-up: CPU time recording the chunk draws
		// (argument upload, draw calls, pass.end, where wgpu encodes the
		// pass), and the interval between frames.
		uint64_t renderedFrames = 0;
		uint64_t timedFrames = 0;
		double recordMsTotal = 0;
		double frameMsTotal = 0;
		std::chrono::steady_clock::time_point lastFrameStart = {};

		// The last frame's numbers, printed at shutdown.
		uint32_t lastDrawCalls = 0;
		uint64_t lastFacesDrawn = 0;
		uint64_t lastTransparentFacesDrawn = 0;
		size_t lastVisibleChunks = 0;
	};

	World w;

	// Defined with the bloom (milestone 16), used by the targets.
	bool ensureBloomTargets(int width, int height);
	void releaseBloomTargets();
	bool ensureHbaoTargets(int width, int height);
	bool ensureRayTargets(int width, int height);
	BindGroupLayoutEntry textureEntry(uint32_t binding, TextureSampleType type);
	BindGroupLayoutEntry emptyEntry(uint32_t binding);
	BindGroupLayout makeLayout(const char *label, const BindGroupLayoutEntry *entries, uint32_t count);

	// ---- The face arena -------------------------------------------------

	void freeRange(uint32_t first, uint32_t count)
	{
		if (count == 0) { return; }
		auto it = w.freeRanges.emplace(first, count).first;

		// Merge with the range after, then with the range before.
		auto next = std::next(it);
		if (next != w.freeRanges.end() && it->first + it->second == next->first)
		{
			it->second += next->second;
			w.freeRanges.erase(next);
		}
		if (it != w.freeRanges.begin())
		{
			auto prev = std::prev(it);
			if (prev->first + prev->second == it->first)
			{
				prev->second += it->second;
				w.freeRanges.erase(it);
			}
		}
	}

	// First fit. Returns false when no free range is long enough.
	bool allocateRange(uint32_t count, uint32_t &first)
	{
		for (auto it = w.freeRanges.begin(); it != w.freeRanges.end(); ++it)
		{
			if (it->second < count) { continue; }
			first = it->first;
			const uint32_t rest = it->second - count;
			w.freeRanges.erase(it);
			if (rest) { w.freeRanges.emplace(first + count, rest); }
			return true;
		}
		return false;
	}

	// A buffer cannot be resized. Growing means a bigger one, a GPU copy of
	// the old contents (submitted at once, so it lands before any later
	// writeBuffer), and the new space added to the free list. Chunks keep
	// their offsets, so nothing else changes.
	bool growArena(uint64_t neededFaces)
	{
		Limits limits = Default;
		wgpuDevice().getLimits(&limits);
		const uint64_t maxFaces = limits.maxBufferSize / faceBytes;

		uint64_t newFaces = w.arenaFaces ? w.arenaFaces : initialArenaFaces;
		while (newFaces < neededFaces) { newFaces *= 2; }
		newFaces = std::min(newFaces, maxFaces);
		if (newFaces <= w.arenaFaces || newFaces < neededFaces)
		{
			std::cerr << "WebGPU world: face arena cannot grow past " << w.arenaFaces
				<< " faces (maxBufferSize " << limits.maxBufferSize << ")\n";
			return false;
		}

		ErrorScope scope("world face arena");

		BufferDescriptor desc = Default;
		desc.label = StringView("world face arena");
		desc.usage = BufferUsage::Vertex | BufferUsage::CopyDst | BufferUsage::CopySrc;
		desc.size = newFaces * faceBytes;
		desc.mappedAtCreation = false;
		Buffer bigger = wgpuDevice().createBuffer(desc);
		if (!bigger || scope.failed()) { return false; }

		if (w.arena)
		{
			CommandEncoderDescriptor encoderDesc = Default;
			encoderDesc.label = StringView("world face arena growth");
			CommandEncoder encoder = wgpuDevice().createCommandEncoder(encoderDesc);
			encoder.copyBufferToBuffer(w.arena, 0, bigger, 0, w.arenaFaces * faceBytes);
			CommandBufferDescriptor commandDesc = Default;
			CommandBuffer commands = encoder.finish(commandDesc);
			wgpuQueue().submit(1, &commands);
			commands.release();
			encoder.release();
			w.arena.release();
		}

		freeRange((uint32_t)w.arenaFaces, (uint32_t)(newFaces - w.arenaFaces));
		w.arena = bigger;
		w.arenaFaces = newFaces;
		std::cout << "WebGPU world face arena: " << newFaces << " faces ("
			<< (newFaces * faceBytes) / (1024 * 1024) << " MiB)\n";
		return true;
	}

	// ---- Pipeline and per-frame resources ----------------------------------

	bool createLayoutAndUniforms()
	{
		ErrorScope scope("world bind group layout");

		constexpr uint32_t worldBindings = 17;
		BindGroupLayoutEntry entries[worldBindings];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].visibility = ShaderStage::Vertex | ShaderStage::Fragment;
		entries[0].buffer.type = BufferBindingType::Uniform;
		entries[0].buffer.hasDynamicOffset = false;
		entries[0].buffer.minBindingSize = sizeof(Uniforms);
		entries[0].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[0].texture.sampleType = TextureSampleType::BindingNotUsed;
		entries[0].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		// 1-3: the corner, UV and slot -> layer tables, read by the vertex stage.
		for (uint32_t i = 1; i <= 3; i++)
		{
			entries[i] = entries[0];
			entries[i].binding = i;
			entries[i].buffer.type = BufferBindingType::ReadOnlyStorage;
			entries[i].buffer.minBindingSize = 0;
		}

		// 4: the block texture array; 5: its sampler. Fragment stage.
		entries[4] = entries[0];
		entries[4].binding = 4;
		entries[4].visibility = ShaderStage::Fragment;
		entries[4].buffer.type = BufferBindingType::BindingNotUsed;
		entries[4].buffer.minBindingSize = 0;
		entries[4].texture.sampleType = TextureSampleType::Float;
		entries[4].texture.viewDimension = TextureViewDimension::_2DArray;
		entries[4].texture.multisampled = false;

		entries[5] = entries[0];
		entries[5].binding = 5;
		entries[5].visibility = ShaderStage::Fragment;
		entries[5].buffer.type = BufferBindingType::BindingNotUsed;
		entries[5].buffer.minBindingSize = 0;
		entries[5].sampler.type = SamplerBindingType::Filtering;

		// 6: the point lights; 7: the AO texture; 8: its sampler (milestone
		// 12, lighting.wgsl). Fragment stage.
		entries[6] = entries[0];
		entries[6].binding = 6;
		entries[6].visibility = ShaderStage::Fragment;
		entries[6].buffer.type = BufferBindingType::ReadOnlyStorage;
		entries[6].buffer.minBindingSize = 0;

		entries[7] = entries[4];
		entries[7].binding = 7;
		entries[7].texture.viewDimension = TextureViewDimension::_2D;

		entries[8] = entries[5];
		entries[8].binding = 8;

		// 9-11: the sun's shadow maps, read as depth; 12: the comparison
		// sampler (milestone 13, lighting.wgsl). Fragment stage.
		for (uint32_t i = 9; i <= 11; i++)
		{
			entries[i] = entries[7];
			entries[i].binding = i;
			entries[i].texture.sampleType = TextureSampleType::Depth;
		}
		entries[12] = entries[5];
		entries[12].binding = 12;
		entries[12].sampler.type = SamplerBindingType::Comparison;

		// 13-15: the water's dudv, normal and caustics textures; 16: their
		// sampler (18-parity, chunkFaces.wgsl). Fragment stage.
		for (uint32_t i = 13; i <= 15; i++)
		{
			entries[i] = entries[7];
			entries[i].binding = i;
		}
		entries[16] = entries[5];
		entries[16].binding = 16;

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world bind group layout");
		layoutDesc.entryCount = worldBindings;
		layoutDesc.entries = entries;
		w.bindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		if (!w.bindGroupLayout) { return false; }

		// Group 1 of the transparent pipelines: the peel's depth and color
		// copies, read with textureLoad (no sampler).
		BindGroupLayoutEntry peelEntries[6];
		for (uint32_t i = 0; i < 6; i++)
		{
			peelEntries[i] = Default;
			peelEntries[i].binding = i;
			peelEntries[i].visibility = ShaderStage::Fragment;
			peelEntries[i].buffer.type = BufferBindingType::BindingNotUsed;
			peelEntries[i].sampler.type = SamplerBindingType::BindingNotUsed;
			peelEntries[i].storageTexture.access = StorageTextureAccess::BindingNotUsed;
			peelEntries[i].texture.viewDimension = TextureViewDimension::_2D;
			peelEntries[i].texture.multisampled = false;
		}
		peelEntries[0].texture.sampleType = TextureSampleType::Depth;
		peelEntries[1].texture.sampleType = TextureSampleType::Float;
		// 3: the sky behind the world, for the fog (milestone 14). Binding 2
		// is the decal's crack texture, in the decal's own group 1.
		peelEntries[2].binding = 3;
		peelEntries[2].texture.sampleType = TextureSampleType::Float;
		// 4, 5, 6: last frame's color and positions and their sampler, for
		// the reflections (milestone 17).
		peelEntries[3].binding = 4;
		peelEntries[3].texture.sampleType = TextureSampleType::Float;
		peelEntries[4].binding = 5;
		peelEntries[4].texture.sampleType = TextureSampleType::Float;
		peelEntries[5].binding = 6;
		peelEntries[5].texture.sampleType = TextureSampleType::BindingNotUsed;
		peelEntries[5].texture.viewDimension = TextureViewDimension::Undefined;
		peelEntries[5].sampler.type = SamplerBindingType::Filtering;

		BindGroupLayoutDescriptor peelLayoutDesc = Default;
		peelLayoutDesc.label = StringView("world frame bind group layout");
		peelLayoutDesc.entryCount = 6;
		peelLayoutDesc.entries = peelEntries;
		w.peelBindGroupLayout = wgpuDevice().createBindGroupLayout(peelLayoutDesc);
		if (!w.peelBindGroupLayout) { return false; }

		// Group 1 of the cube pipeline: the cube's vertices and the cubes.
		BindGroupLayoutEntry cubeEntries[2];
		for (uint32_t i = 0; i < 2; i++)
		{
			cubeEntries[i] = Default;
			cubeEntries[i].binding = i;
			cubeEntries[i].visibility = ShaderStage::Vertex;
			cubeEntries[i].buffer.type = BufferBindingType::ReadOnlyStorage;
			cubeEntries[i].buffer.hasDynamicOffset = false;
			cubeEntries[i].buffer.minBindingSize = 0;
			cubeEntries[i].sampler.type = SamplerBindingType::BindingNotUsed;
			cubeEntries[i].texture.sampleType = TextureSampleType::BindingNotUsed;
			cubeEntries[i].storageTexture.access = StorageTextureAccess::BindingNotUsed;
		}
		BindGroupLayoutDescriptor cubeLayoutDesc = Default;
		cubeLayoutDesc.label = StringView("world cube bind group layout");
		cubeLayoutDesc.entryCount = 2;
		cubeLayoutDesc.entries = cubeEntries;
		w.cubeBindGroupLayout = wgpuDevice().createBindGroupLayout(cubeLayoutDesc);
		if (!w.cubeBindGroupLayout) { return false; }

		// Group 1 of the decal pipeline: the crack texture, at binding 2
		// (chunkFaces.wgsl's group 1 also declares the peel's 0 and 1).
		BindGroupLayoutEntry decalEntry = Default;
		decalEntry.binding = 2;
		decalEntry.visibility = ShaderStage::Fragment;
		decalEntry.buffer.type = BufferBindingType::BindingNotUsed;
		decalEntry.sampler.type = SamplerBindingType::BindingNotUsed;
		decalEntry.storageTexture.access = StorageTextureAccess::BindingNotUsed;
		decalEntry.texture.sampleType = TextureSampleType::Float;
		decalEntry.texture.viewDimension = TextureViewDimension::_2D;
		decalEntry.texture.multisampled = false;
		BindGroupLayoutDescriptor decalLayoutDesc = Default;
		decalLayoutDesc.label = StringView("world decal bind group layout");
		decalLayoutDesc.entryCount = 1;
		decalLayoutDesc.entries = &decalEntry;
		w.decalBindGroupLayout = wgpuDevice().createBindGroupLayout(decalLayoutDesc);
		if (!w.decalBindGroupLayout) { return false; }

		// Group 1 of the model and item pipelines: the frame's bones,
		// models and items, read by the vertex stage.
		BindGroupLayoutEntry entityEntries[3];
		for (uint32_t i = 0; i < 3; i++)
		{
			entityEntries[i] = cubeEntries[0];
			entityEntries[i].binding = i;
		}
		BindGroupLayoutDescriptor entityLayoutDesc = Default;
		entityLayoutDesc.label = StringView("world entity bind group layout");
		entityLayoutDesc.entryCount = 3;
		entityLayoutDesc.entries = entityEntries;
		w.entityBindGroupLayout = wgpuDevice().createBindGroupLayout(entityLayoutDesc);
		if (!w.entityBindGroupLayout) { return false; }

		// Group 2: an entity's four textures (GL's textureSampler0-3).
		BindGroupLayoutEntry textureEntries[4];
		for (uint32_t i = 0; i < 4; i++)
		{
			textureEntries[i] = decalEntry;
			textureEntries[i].binding = i;
		}
		BindGroupLayoutDescriptor textureLayoutDesc = Default;
		textureLayoutDesc.label = StringView("world entity texture bind group layout");
		textureLayoutDesc.entryCount = 4;
		textureLayoutDesc.entries = textureEntries;
		w.textureBindGroupLayout = wgpuDevice().createBindGroupLayout(textureLayoutDesc);
		if (!w.textureBindGroupLayout) { return false; }

		// The G-buffer views' group (gbufferView.wgsl): every attachment,
		// loaded rather than sampled (no filtering: the normals are integers,
		// and the rest is shown texel for texel), and the uniforms.
		BindGroupLayoutEntry viewEntries[viewBindings];
		for (uint32_t i = 0; i < (uint32_t)viewBindings; i++)
		{
			viewEntries[i] = Default;
			viewEntries[i].binding = i;
			viewEntries[i].visibility = ShaderStage::Fragment;
			viewEntries[i].buffer.type = BufferBindingType::BindingNotUsed;
			viewEntries[i].sampler.type = SamplerBindingType::BindingNotUsed;
			viewEntries[i].storageTexture.access = StorageTextureAccess::BindingNotUsed;
			viewEntries[i].texture.sampleType = TextureSampleType::UnfilterableFloat;
			viewEntries[i].texture.viewDimension = TextureViewDimension::_2D;
			viewEntries[i].texture.multisampled = false;
		}
		viewEntries[GNormal].texture.sampleType = TextureSampleType::Uint;
		viewEntries[GBufferSlots].texture.sampleType = TextureSampleType::Depth;
		viewEntries[GBufferSlots + 1].texture.sampleType = TextureSampleType::BindingNotUsed;
		viewEntries[GBufferSlots + 1].buffer.type = BufferBindingType::Uniform;
		viewEntries[GBufferSlots + 1].buffer.hasDynamicOffset = false;
		viewEntries[GBufferSlots + 1].buffer.minBindingSize = 0;
		BindGroupLayoutDescriptor viewLayoutDesc = Default;
		viewLayoutDesc.label = StringView("world G-buffer view bind group layout");
		viewLayoutDesc.entryCount = viewBindings;
		viewLayoutDesc.entries = viewEntries;
		w.viewBindGroupLayout = wgpuDevice().createBindGroupLayout(viewLayoutDesc);
		if (!w.viewBindGroupLayout) { return false; }

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(Uniforms);
		bufferDesc.mappedAtCreation = false;
		w.uniformBuffer = wgpuDevice().createBuffer(bufferDesc);
		return (bool)w.uniformBuffer && !scope.failed();
	}

	TextureFormat gBufferFormat(int slot)
	{
		switch (slot)
		{
			case GPosition: return TextureFormat::RGBA16Float; // xyz position, w roughness
			case GNormal: return TextureFormat::RGBA16Uint;
			default: return w.hdrFormat; // color, bloom
		}
	}

	// Which color attachments a pipeline draws into; a pipeline must name
	// the formats of the pass it is used in, slot for slot.
	enum class ColorTargets
	{
		None,          // depth only (the water depth pass)
		GBuffer,       // the G-buffer's four
		GBufferMasked, // the four, nothing written: the prepass (glColorMask 0)
		Hdr,           // the HDR color alone: the sky bake and sky passes
		GBufferTransparent, // the four, the color blended, bloom masked: the transparent passes
		Surface,       // the screen, after the composite: decal, lines
	};

	uint32_t colorTargets(ColorTargets kind, const BlendState *blend, ColorTargetState (&out)[GBufferSlots])
	{
		for (ColorTargetState &t : out)
		{
			t = Default;
			t.blend = nullptr;
			t.writeMask = ColorWriteMask::All;
		}
		switch (kind)
		{
			case ColorTargets::None: return 0;
			case ColorTargets::GBuffer:
			case ColorTargets::GBufferMasked:
				for (int i = 0; i < GBufferSlots; i++)
				{
					out[i].format = gBufferFormat(i);
					if (kind == ColorTargets::GBufferMasked) { out[i].writeMask = ColorWriteMask::None; }
				}
				return GBufferSlots;
			case ColorTargets::Hdr:
				out[0].format = w.hdrFormat;
				out[0].blend = blend;
				return 1;
			case ColorTargets::GBufferTransparent:
				// GL's transparent passes: blending on the color target only,
				// the positions and normals written over, the bloom's draw
				// buffer off (milestone 17 needs the positions and normals).
				for (int i = 0; i < GBufferSlots; i++) { out[i].format = gBufferFormat(i); }
				out[GColor].blend = blend;
				out[GBloom].writeMask = ColorWriteMask::None;
				return GBufferSlots;
			case ColorTargets::Surface:
				out[0].format = wgpuSurfaceFormat();
				out[0].blend = blend;
				return 1;
		}
		return 0;
	}

	// One pipeline of the chunk face module. What GL switches between draws
	// (depth function, blending, culling, color writes) is fixed here, so
	// each combination the passes need is its own pipeline.
	struct FacePipelineDesc
	{
		const char *label = "";
		PipelineLayout layout = nullptr;
		const char *fragmentEntry = "fs_main";
		ColorTargets targets = ColorTargets::GBuffer;
		bool blend = false;       // GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA
		bool depthWrite = true;
		CompareFunction depthCompare = CompareFunction::Less;
		CullMode cull = CullMode::Back;
		int passMode = -1;        // the WGSL `passMode` override, -1 to leave it
		float zBias = 0.f;        // the WGSL `zBias` override (vertex stage)
		const char *vertexEntry = "vs_main";
		int shadowCascade = -1;   // the WGSL `shadowCascade` override, -1 to leave it
	};

	RenderPipeline createFacePipeline(ShaderModule module, const FacePipelineDesc &p)
	{
		// The face as the vertex stage sees it. Instance step mode: the GPU
		// moves to the next 16 bytes once per instance, not once per vertex,
		// so all four corners of a face read the same face.
		VertexAttribute attributes[2];
		attributes[0] = Default;
		attributes[0].shaderLocation = 0;               // orientation, texture
		attributes[0].format = VertexFormat::Sint16x2;
		attributes[0].offset = 0;
		attributes[1] = Default;
		attributes[1].shaderLocation = 1;               // block position (+ light, flags)
		attributes[1].format = VertexFormat::Sint32x3;
		attributes[1].offset = sizeof(int);

		VertexBufferLayout faceLayout = Default;
		faceLayout.arrayStride = faceBytes;
		faceLayout.stepMode = VertexStepMode::Instance;
		faceLayout.attributeCount = 2;
		faceLayout.attributes = attributes;

		RenderPipelineDescriptor desc = Default;
		desc.label = StringView(p.label);
		desc.layout = p.layout;
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView(p.vertexEntry);
		ConstantEntry vertexConstants[2];
		uint32_t vertexConstantCount = 0;
		if (p.zBias != 0.f)
		{
			vertexConstants[vertexConstantCount] = Default;
			vertexConstants[vertexConstantCount].key = StringView("zBias");
			vertexConstants[vertexConstantCount].value = (double)p.zBias;
			vertexConstantCount++;
		}
		if (p.shadowCascade >= 0)
		{
			vertexConstants[vertexConstantCount] = Default;
			vertexConstants[vertexConstantCount].key = StringView("shadowCascade");
			vertexConstants[vertexConstantCount].value = (double)p.shadowCascade;
			vertexConstantCount++;
		}
		desc.vertex.constantCount = vertexConstantCount;
		desc.vertex.constants = vertexConstantCount ? vertexConstants : nullptr;
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &faceLayout;

		// A strip per instance: 4 vertices make the face's two triangles.
		// Strips never join across instances.
		desc.primitive.topology = PrimitiveTopology::TriangleStrip;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined; // no index buffer
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = p.cull;
		desc.primitive.unclippedDepth = false;

		// Depth writes stay on for every pass but the solid pass after a
		// prepass (where Equal could only write what is already there).
		DepthStencilState depth = Default;
		depth.format = depthFormat;
		depth.depthWriteEnabled = p.depthWrite ? OptionalBool::True : OptionalBool::False;
		depth.depthCompare = p.depthCompare;
		depth.stencilFront.compare = CompareFunction::Always;
		depth.stencilFront.failOp = StencilOperation::Keep;
		depth.stencilFront.depthFailOp = StencilOperation::Keep;
		depth.stencilFront.passOp = StencilOperation::Keep;
		depth.stencilBack = depth.stencilFront;
		depth.stencilReadMask = 0;
		depth.stencilWriteMask = 0;
		depth.depthBias = 0;
		depth.depthBiasSlopeScale = 0.f;
		depth.depthBiasClamp = 0.f;
		desc.depthStencil = &depth;

		BlendState blend = Default;
		blend.color.operation = BlendOperation::Add;
		blend.color.srcFactor = BlendFactor::SrcAlpha;
		blend.color.dstFactor = BlendFactor::OneMinusSrcAlpha;
		blend.alpha = blend.color; // glBlendFunci sets both

		ColorTargetState targets[GBufferSlots];
		const uint32_t targetCount = colorTargets(p.targets, p.blend ? &blend : nullptr, targets);

		ConstantEntry constant = Default;
		constant.key = StringView("passMode");
		constant.value = (double)p.passMode;

		FragmentState fragment = Default;
		fragment.module = module;
		fragment.entryPoint = StringView(p.fragmentEntry);
		fragment.constantCount = p.passMode >= 0 ? 1 : 0;
		fragment.constants = p.passMode >= 0 ? &constant : nullptr;
		fragment.targetCount = targetCount;
		fragment.targets = targetCount ? targets : nullptr;
		desc.fragment = &fragment;

		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;

		return wgpuDevice().createRenderPipeline(desc);
	}

	PipelineLayout createPipelineLayout(const char *label, BindGroupLayout *groups, uint32_t count)
	{
		WGPUBindGroupLayout layouts[2] = {};
		for (uint32_t i = 0; i < count; i++) { layouts[i] = groups[i]; }
		PipelineLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView(label);
		layoutDesc.bindGroupLayoutCount = count;
		layoutDesc.bindGroupLayouts = layouts;
		return wgpuDevice().createPipelineLayout(layoutDesc);
	}

	bool createPipelines()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/chunkFaces.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world chunk face pipelines");

		BindGroupLayout groups[2] = { w.bindGroupLayout, w.peelBindGroupLayout };
		w.pipelineLayout = createPipelineLayout("world pipeline layout", groups, 1);
		w.transparentPipelineLayout = createPipelineLayout("world transparent pipeline layout", groups, 2);
		if (!w.pipelineLayout || !w.transparentPipelineLayout) { module.release(); return false; }

		// The opaque faces (milestone 11): the depth prepass (zpass, GL_LESS,
		// color masked), then the solid pass into the G-buffer with GL_EQUAL
		// and nothing left to write to depth. Without the prepass (the
		// renderer's zprepass off), the solid pass is GL_LESS on its own.
		FacePipelineDesc prepass;
		prepass.label = "world depth prepass";
		prepass.layout = w.pipelineLayout;
		prepass.fragmentEntry = "fs_depth";
		prepass.targets = ColorTargets::GBufferMasked;
		w.prepassPipeline = createFacePipeline(module, prepass);

		// Milestone 13: the sun's shadow maps, one pipeline per cascade.
		// Depth only, no color attachment at all; the prepass's alpha test;
		// culled as GL leaves it (GL_CULL_FACE on, back faces).
		const char *const shadowLabels[shadowCascades] =
			{ "world sun shadow, cascade 0", "world sun shadow, cascade 1", "world sun shadow, cascade 2" };
		for (int i = 0; i < shadowCascades; i++)
		{
			FacePipelineDesc shadow;
			shadow.label = shadowLabels[i];
			shadow.layout = w.pipelineLayout;
			shadow.vertexEntry = "vs_shadow";
			shadow.shadowCascade = i;
			shadow.fragmentEntry = "fs_depth";
			shadow.targets = ColorTargets::None;
			w.shadowPipelines[i] = createFacePipeline(module, shadow);
		}

		// Both with the frame group as well: the fog reads the sky in it.
		FacePipelineDesc opaque;
		opaque.label = "world opaque faces";
		opaque.layout = w.transparentPipelineLayout;
		opaque.depthCompare = CompareFunction::Equal;
		opaque.depthWrite = false;
		w.pipeline = createFacePipeline(module, opaque);

		opaque.label = "world opaque faces, no prepass";
		opaque.depthCompare = CompareFunction::Less;
		opaque.depthWrite = true;
		w.solidLessPipeline = createFacePipeline(module, opaque);

		// Pass 4: water into the peel depth, no color (glColorMask 0).
		FacePipelineDesc waterDepth;
		waterDepth.label = "world water depth";
		waterDepth.layout = w.pipelineLayout;
		waterDepth.fragmentEntry = "fs_waterDepth";
		waterDepth.targets = ColorTargets::None;
		w.waterDepthPipeline = createFacePipeline(module, waterDepth);

		// Pass 5: the peel, blended, GL_LESS, culled, into the G-buffer.
		FacePipelineDesc peel;
		peel.label = "world transparent peel";
		peel.layout = w.transparentPipelineLayout;
		peel.fragmentEntry = "fs_blend";
		peel.targets = ColorTargets::GBufferTransparent;
		peel.blend = true;
		peel.passMode = 0;
		w.peelPipeline = createFacePipeline(module, peel);

		// Pass 7: everything transparent, GL_LEQUAL, no culling.
		FacePipelineDesc final = peel;
		final.label = "world transparent final";
		final.depthCompare = CompareFunction::LessEqual;
		final.cull = CullMode::None;
		final.passMode = 1;
		w.finalPeelPipeline = createFacePipeline(module, final);

		final.label = "world transparent final, no peel";
		final.passMode = 2;
		w.finalPlainPipeline = createFacePipeline(module, final);

		// The crack decal: drawn after the world with GL_LEQUAL, culled, and
		// zpass.vert's bias.
		BindGroupLayout decalGroups[2] = { w.bindGroupLayout, w.decalBindGroupLayout };
		w.decalPipelineLayout = createPipelineLayout("world decal pipeline layout", decalGroups, 2);
		FacePipelineDesc decal;
		decal.label = "world crack decal";
		decal.layout = w.decalPipelineLayout;
		decal.fragmentEntry = "fs_decal";
		decal.targets = ColorTargets::Surface;
		decal.depthCompare = CompareFunction::LessEqual;
		decal.zBias = -0.000001f;
		w.decalPipeline = createFacePipeline(module, decal);

		const bool invalid = scope.failed(); // a rejected pipeline can still be non-null
		module.release();
		return w.pipeline && w.waterDepthPipeline && w.peelPipeline && w.finalPeelPipeline
			&& w.finalPlainPipeline && w.decalPipeline && w.shadowPipelines[0] && w.shadowPipelines[1]
			&& w.shadowPipelines[2] && !invalid;
	}

	// The dropped block cubes and the gizmo lines (entities.wgsl).
	bool createEntityPipelines()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/entities.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world entity pipelines");

		BindGroupLayout cubeGroups[2] = { w.bindGroupLayout, w.cubeBindGroupLayout };
		w.cubePipelineLayout = createPipelineLayout("world cube pipeline layout", cubeGroups, 2);
		if (!w.cubePipelineLayout) { module.release(); return false; }

		DepthStencilState depth = Default;
		depth.format = depthFormat;
		depth.depthWriteEnabled = OptionalBool::True;
		depth.depthCompare = CompareFunction::Less;
		depth.stencilFront.compare = CompareFunction::Always;
		depth.stencilFront.failOp = StencilOperation::Keep;
		depth.stencilFront.depthFailOp = StencilOperation::Keep;
		depth.stencilFront.passOp = StencilOperation::Keep;
		depth.stencilBack = depth.stencilFront;
		depth.stencilReadMask = 0;
		depth.stencilWriteMask = 0;
		depth.depthBias = 0;
		depth.depthBiasSlopeScale = 0.f;
		depth.depthBiasClamp = 0.f;

		// Cubes, models and items write the G-buffer, as GL's entity shaders
		// write fboMain's five targets; lines draw on the screen.
		ColorTargetState gBufferTargets[GBufferSlots];
		ColorTargetState surfaceTargets[GBufferSlots];
		const uint32_t gBufferTargetCount = colorTargets(ColorTargets::GBuffer, nullptr, gBufferTargets);
		colorTargets(ColorTargets::Surface, nullptr, surfaceTargets);

		FragmentState fragment = Default;
		fragment.module = module;
		fragment.constantCount = 0;
		fragment.constants = nullptr;
		fragment.targetCount = gBufferTargetCount;
		fragment.targets = gBufferTargets;

		// Cubes: no vertex buffers, everything from storage. Back faces culled,
		// as GL draws entities: every triangle of vertexDataOriginal winds
		// counter-clockwise seen from outside, so cut-out textures show the
		// ground through the cube, not its inside.
		RenderPipelineDescriptor desc = Default;
		desc.label = StringView("world block cubes");
		desc.layout = w.cubePipelineLayout;
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView("vs_cube");
		desc.vertex.constantCount = 0;
		desc.vertex.constants = nullptr;
		desc.vertex.bufferCount = 0;
		desc.vertex.buffers = nullptr;
		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined;
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::Back;
		desc.primitive.unclippedDepth = false;
		desc.depthStencil = &depth;
		fragment.entryPoint = StringView("fs_cube");
		desc.fragment = &fragment;
		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;
		w.cubePipeline = wgpuDevice().createRenderPipeline(desc);

		// Lines: camera-relative points, two per segment, GL_LEQUAL. WebGPU
		// lines are one pixel wide (GL asked for 10, which macOS ignores too).
		VertexAttribute pointAttribute = Default;
		pointAttribute.shaderLocation = 0;
		pointAttribute.format = VertexFormat::Float32x3;
		pointAttribute.offset = 0;
		VertexBufferLayout pointLayout = Default;
		pointLayout.arrayStride = sizeof(glm::vec3);
		pointLayout.stepMode = VertexStepMode::Vertex;
		pointLayout.attributeCount = 1;
		pointLayout.attributes = &pointAttribute;

		desc.label = StringView("world gizmo lines");
		desc.layout = w.pipelineLayout; // group 0 only
		desc.vertex.entryPoint = StringView("vs_line");
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &pointLayout;
		desc.primitive.topology = PrimitiveTopology::LineList;
		desc.primitive.cullMode = CullMode::None; // lines have no facing
		depth.depthCompare = CompareFunction::LessEqual;
		fragment.entryPoint = StringView("fs_line");
		fragment.targetCount = 1;
		fragment.targets = surfaceTargets;
		w.linePipeline = wgpuDevice().createRenderPipeline(desc);
		fragment.targetCount = gBufferTargetCount;
		fragment.targets = gBufferTargets;

		// Models and items: indexed or not, both culled like nothing (GL's
		// hand is mirrored with scale(-1, 1, 1), which turns its winding
		// inside out; depth sorts closed meshes either way).
		BindGroupLayout entityGroups[3] = { w.bindGroupLayout, w.entityBindGroupLayout, w.textureBindGroupLayout };
		{
			WGPUBindGroupLayout layouts[3] = { entityGroups[0], entityGroups[1], entityGroups[2] };
			PipelineLayoutDescriptor layoutDesc = Default;
			layoutDesc.label = StringView("world entity pipeline layout");
			layoutDesc.bindGroupLayoutCount = 3;
			layoutDesc.bindGroupLayouts = layouts;
			w.entityPipelineLayout = wgpuDevice().createPipelineLayout(layoutDesc);
		}
		if (!w.entityPipelineLayout) { module.release(); return false; }

		VertexAttribute modelAttributes[4];
		for (int i = 0; i < 4; i++) { modelAttributes[i] = Default; modelAttributes[i].shaderLocation = i; }
		modelAttributes[0].format = VertexFormat::Float32x3; modelAttributes[0].offset = 0;  // position
		modelAttributes[1].format = VertexFormat::Float32x3; modelAttributes[1].offset = 12; // normal
		modelAttributes[2].format = VertexFormat::Float32x2; modelAttributes[2].offset = 24; // uv
		modelAttributes[3].format = VertexFormat::Sint16x2;  modelAttributes[3].offset = 32; // bone, texture
		VertexBufferLayout modelLayout = Default;
		modelLayout.arrayStride = 36; // model.cpp's Data
		modelLayout.stepMode = VertexStepMode::Vertex;
		modelLayout.attributeCount = 4;
		modelLayout.attributes = modelAttributes;

		depth.depthCompare = CompareFunction::Less;
		desc.label = StringView("world models");
		desc.layout = w.entityPipelineLayout;
		desc.vertex.entryPoint = StringView("vs_model");
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &modelLayout;
		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.cullMode = CullMode::None;
		fragment.entryPoint = StringView("fs_model");
		w.modelPipeline = wgpuDevice().createRenderPipeline(desc);

		VertexBufferLayout itemLayout = modelLayout;
		itemLayout.arrayStride = 8 * sizeof(float);
		itemLayout.attributeCount = 3;
		desc.label = StringView("world items");
		desc.vertex.entryPoint = StringView("vs_item");
		desc.vertex.buffers = &itemLayout;
		fragment.entryPoint = StringView("fs_item");
		w.itemPipeline = wgpuDevice().createRenderPipeline(desc);

		const bool invalid = scope.failed();
		module.release();
		return w.cubePipeline && w.linePipeline && w.modelPipeline && w.itemPipeline && !invalid;
	}

	// The composite and the debug views (gbufferView.wgsl): one pipeline per
	// view, the `view` override picking the attachment. All draw into
	// targets of the surface's format (the surface, or wgpu2d render
	// targets), with no depth and no blending.
	bool createViewPipelines()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/gbufferView.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world G-buffer view pipelines");

		w.viewPipelineLayout = createPipelineLayout("world G-buffer view pipeline layout", &w.viewBindGroupLayout, 1);
		if (!w.viewPipelineLayout) { module.release(); return false; }

		ColorTargetState target = Default;
		target.format = wgpuSurfaceFormat();
		target.blend = nullptr;
		target.writeMask = ColorWriteMask::All;

		for (int i = 0; i < gBufferViewCount; i++)
		{
			ConstantEntry constant = Default;
			constant.key = StringView("view");
			constant.value = (double)i;

			FragmentState fragment = Default;
			fragment.module = module;
			fragment.entryPoint = StringView("fs_view");
			fragment.constantCount = 1;
			fragment.constants = &constant;
			fragment.targetCount = 1;
			fragment.targets = &target;

			RenderPipelineDescriptor desc = Default;
			desc.label = StringView(gBufferViewLabels[i]);
			desc.layout = w.viewPipelineLayout;
			desc.vertex.module = module;
			desc.vertex.entryPoint = StringView("vs_fullscreen");
			desc.vertex.constantCount = 0;
			desc.vertex.constants = nullptr;
			desc.vertex.bufferCount = 0;
			desc.vertex.buffers = nullptr;
			desc.primitive.topology = PrimitiveTopology::TriangleList;
			desc.primitive.stripIndexFormat = IndexFormat::Undefined;
			desc.primitive.frontFace = FrontFace::CCW;
			desc.primitive.cullMode = CullMode::None;
			desc.primitive.unclippedDepth = false;
			desc.depthStencil = nullptr;
			desc.fragment = &fragment;
			desc.multisample.count = 1;
			desc.multisample.mask = 0xFFFFFFFFu;
			desc.multisample.alphaToCoverageEnabled = false;
			w.viewPipelines[i] = wgpuDevice().createRenderPipeline(desc);
			if (!w.viewPipelines[i]) { break; }
		}

		const bool invalid = scope.failed();
		module.release();
		for (RenderPipeline &pipeline : w.viewPipelines) { if (!pipeline) { return false; } }
		return !invalid;
	}

	// A render pass on the G-buffer's five targets and the depth: the opaque
	// pass (clearing them) and the entity pass (keeping what is there).
	// With `keepColor`, a clearing pass still loads the HDR color: the sky
	// pass has drawn it (milestone 14).
	RenderPassEncoder beginGBufferPass(CommandEncoder encoder, bool clear, const char *label, bool keepColor = false)
	{
		RenderPassColorAttachment colors[GBufferSlots];
		for (int i = 0; i < GBufferSlots; i++)
		{
			colors[i] = Default;
			colors[i].view = w.gBufferViews[i];
			colors[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			colors[i].resolveTarget = nullptr;
			colors[i].loadOp = clear && !(keepColor && i == GColor) ? LoadOp::Clear : LoadOp::Load;
			colors[i].storeOp = StoreOp::Store;
			// Nothing drawn: no position, no bloom; for the normals, 0 is
			// "no normal". The color is the sky's.
			colors[i].clearValue = Color{ 0.0, 0.0, 0.0, 0.0 };
		}

		RenderPassDepthStencilAttachment depthAttachment = Default;
		depthAttachment.view = w.depthView;
		depthAttachment.depthLoadOp = clear ? LoadOp::Clear : LoadOp::Load;
		depthAttachment.depthStoreOp = StoreOp::Store;
		depthAttachment.depthClearValue = depthClear;
		depthAttachment.depthReadOnly = false;
		depthAttachment.stencilLoadOp = LoadOp::Undefined;
		depthAttachment.stencilStoreOp = StoreOp::Undefined;
		depthAttachment.stencilClearValue = 0;
		depthAttachment.stencilReadOnly = false;

		RenderPassDescriptor passDesc = Default;
		passDesc.label = StringView(label);
		passDesc.colorAttachmentCount = GBufferSlots;
		passDesc.colorAttachments = colors;
		passDesc.depthStencilAttachment = &depthAttachment;
		passDesc.occlusionQuerySet = nullptr;
		passDesc.timestampWrites = nullptr;
		return encoder.beginRenderPass(passDesc);
	}

	// ---- Milestone 16: bloom and the auto exposure, each frame -----------

	// A fullscreen triangle into one view (one mip), reading `group`. Loads
	// what is there when the pipeline adds onto it.
	void fullscreenPass(CommandEncoder encoder, TextureView target, bool load, RenderPipeline pipeline,
		BindGroup group, const char *label)
	{
		RenderPassColorAttachment color = Default;
		color.view = target;
		color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		color.resolveTarget = nullptr;
		color.loadOp = load ? LoadOp::Load : LoadOp::Clear;
		color.storeOp = StoreOp::Store;
		color.clearValue = Color{ 0.0, 0.0, 0.0, 1.0 };
		RenderPassDescriptor passDesc = Default;
		passDesc.label = StringView(label);
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &color;
		passDesc.depthStencilAttachment = nullptr;
		passDesc.occlusionQuerySet = nullptr;
		passDesc.timestampWrites = nullptr;
		RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
		pass.setPipeline(pipeline);
		pass.setBindGroup(0, group, 0, nullptr);
		pass.draw(3, 1, 0, 0);
		pass.end();
		pass.release();
	}

	// The exposure's average: the HDR color halved level by level to 1x1,
	// that texel copied out. Only while no reading is in flight: the buffer
	// may not be used by a submit while it is mapped or being mapped.
	void recordExposure(CommandEncoder encoder)
	{
		if (w.exposureState != World::Readback::Idle || w.exposureViews.empty() || !w.exposureReadBuffer) { return; }
		for (size_t level = 0; level < w.exposureViews.size(); level++)
		{
			fullscreenPass(encoder, w.exposureViews[level], false, w.exposurePipeline,
				level == 0 ? w.colorSourceGroup : w.exposureGroups[level - 1], "world exposure average");
		}

		TexelCopyTextureInfo from = Default;
		from.texture = w.exposureTexture;
		from.mipLevel = (uint32_t)w.exposureViews.size() - 1;
		from.origin = { 0, 0, 0 };
		from.aspect = TextureAspect::All;
		TexelCopyBufferInfo to = Default;
		to.buffer = w.exposureReadBuffer;
		to.layout.offset = 0;
		to.layout.bytesPerRow = 256;
		to.layout.rowsPerImage = 1;
		Extent3D size = { 1, 1, 1 };
		encoder.copyTextureToBuffer(from, to, size);
		w.exposureState = World::Readback::Copied;
	}

	// One readback (milestone 16): the copy an earlier frame made, now
	// submitted, asked for; the device polled so a finished mapping calls
	// back; a mapped reading given to `read` and the buffer freed. Called at
	// the start of render, so a few frames pass from the copy to the
	// reading (GL's pixel buffer and queries take one to three).
	template <class Read>
	void serviceReadback(Buffer buffer, World::Readback &state, size_t size, Read read)
	{
		if (!buffer) { return; }
		if (state == World::Readback::Copied)
		{
			WGPUBufferMapCallbackInfo info = {};
			info.nextInChain = nullptr;
			info.mode = WGPUCallbackMode_AllowProcessEvents;
			info.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void *target, void *)
			{
				// Only the state changes here: no webgpu call may be made
				// from inside the poll that calls this.
				*static_cast<World::Readback *>(target) = status == WGPUMapAsyncStatus_Success
					? World::Readback::Mapped : World::Readback::Idle;
			};
			info.userdata1 = &state;
			info.userdata2 = nullptr;
			state = World::Readback::Mapping;
			wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, size, info);
		}
		if (state == World::Readback::Mapping)
		{
			wgpuDevicePoll(wgpuDevice(), false, nullptr);
		}
		if (state == World::Readback::Mapped)
		{
			if (const void *data = wgpuBufferGetConstMappedRange(buffer, 0, size)) { read(data); }
			buffer.unmap();
			state = World::Readback::Idle;
		}
	}

	void serviceReadbacks()
	{
		// The exposure's texel: three halves (and an alpha).
		serviceReadback(w.exposureReadBuffer, w.exposureState, 256, [](const void *data)
		{
			const uint16_t *half = static_cast<const uint16_t *>(data);
			w.exposureColor = glm::vec3(glm::unpackHalf1x16(half[0]), glm::unpackHalf1x16(half[1]),
				glm::unpackHalf1x16(half[2]));
			w.exposureFresh = true;
			w.exposureReadings++;
		});
		// Milestone 18: the sun's visible pixels.
		serviceReadback(w.sunReadBuffer, w.sunReadState, 4, [](const void *data)
		{
			w.sunSamples = *static_cast<const uint32_t *>(data);
			w.sunReadings++;
		});
	}

	// The apply's group: the blurred bloom (whichever buffer the sequence
	// ends in) and the three dirt textures, black where one is missing.
	BindGroup bloomApplyGroup(TextureView bloomView)
	{
		TextureView views[6] = { bloomView, nullptr, nullptr, nullptr, w.sunRaysBlurView, w.depthSampleView };
		for (int i = 0; i < 3; i++)
		{
			views[i + 1] = w.dirtTextures[i] ? wgpuTextureView(w.dirtTextures[i]) : nullptr;
			if (!views[i + 1]) { views[i + 1] = w.blackView; }
		}
		if (!views[4] || !views[5]) { return nullptr; }
		if (w.bloomApplyGroup && std::equal(std::begin(views), std::end(views), std::begin(w.boundApplyViews)))
		{
			return w.bloomApplyGroup;
		}
		if (w.bloomApplyGroup) { w.bloomApplyGroup.release(); w.bloomApplyGroup = nullptr; }

		BindGroupEntry entries[9];
		for (uint32_t i = 0; i < 9; i++) { entries[i] = Default; entries[i].binding = i; }
		entries[0].buffer = w.bloomUniformBuffer;
		entries[0].size = sizeof(BloomUniforms);
		entries[1].textureView = views[0];
		entries[2].sampler = w.postSampler;
		entries[3].textureView = views[1];
		entries[4].textureView = views[2];
		entries[5].textureView = views[3];
		entries[6].sampler = w.dirtSampler;
		entries[7].textureView = views[4]; // milestone 18: the god rays
		entries[8].textureView = views[5]; // and the depth
		BindGroupDescriptor desc = Default;
		desc.label = StringView("world bloom apply bind group");
		desc.layout = w.bloomApplyBindGroupLayout;
		desc.entryCount = 9;
		desc.entries = entries;
		w.bloomApplyGroup = wgpuDevice().createBindGroup(desc);
		std::copy(std::begin(views), std::end(views), std::begin(w.boundApplyViews));
		return w.bloomApplyGroup;
	}

	// GL's bloom, pass for pass: the bright pixels added into the G-buffer's
	// bloom; then, level by level, a downsample, a copy, and a Gaussian each
	// way, ping-ponging between the two buffers; the levels added back up,
	// smallest first; and the result, with its lens dirt, added onto the HDR
	// color. `horizontal`, `firstTime` and `last` (GL's lastBloomChannel)
	// run as in GL, so each pass reads and writes what GL's does.
	void recordBloom(CommandEncoder encoder)
	{
		w.bloomLast = -1;
		if (!w.bloom || w.blurViews[0].empty() || w.blurViews[1].empty()) { return; }

		fullscreenPass(encoder, w.gBufferViews[GBloom], true, w.bloomFilterPipeline, w.colorSourceGroup,
			"world bloom filter");

		bool horizontal = false;
		bool firstTime = true;
		int last = 1;
		for (int i = 0; i <= w.bloomMips; i++)
		{
			fullscreenPass(encoder, w.blurViews[horizontal][i], false, w.bloomDownPipeline,
				firstTime ? w.bloomSourceGroup : w.blurGroups[last][i - 1], "world bloom down");
			last = horizontal;

			fullscreenPass(encoder, w.blurViews[!horizontal][i], false, w.bloomTentPipeline,
				w.blurGroups[last][i], "world bloom copy");
			last = !horizontal;

			for (int j = 0; j < 2; j++)
			{
				fullscreenPass(encoder, w.blurViews[horizontal][i], false, w.bloomBlurPipelines[horizontal],
					w.blurGroups[last][i], "world bloom blur");
				last = horizontal;
				horizontal = !horizontal;
				firstTime = false;
			}
			horizontal = !horizontal;
		}

		for (int finalMip = w.bloomMips; finalMip > 0; finalMip--)
		{
			fullscreenPass(encoder, w.blurViews[!last][finalMip - 1], true, w.bloomUpPipeline,
				w.blurGroups[last][finalMip], "world bloom up");
			last = !last;
		}

		w.bloomLast = last;
	}

	// The bloom's last step, after the AO as in GL's post process: the
	// result, with its lens dirt, added onto the HDR color.
	void recordBloomApply(CommandEncoder encoder)
	{
		if (w.bloomLast < 0) { return; }
		BindGroup apply = bloomApplyGroup(w.blurViews[w.bloomLast][0]);
		w.bloomLast = -1;
		if (!apply) { return; }
		fullscreenPass(encoder, w.gBufferViews[GColor], true, w.bloomApplyPipeline, apply, "world bloom apply");
	}

	// Milestone 17: the AO at half size, then laid over the HDR color.
	void recordHbao(CommandEncoder encoder)
	{
		if (!w.ssao || !w.hbaoBindGroup || !w.hbaoApplyBindGroup) { return; }
		fullscreenPass(encoder, w.hbaoView, false, w.hbaoPipeline, w.hbaoBindGroup, "world hbao");
		fullscreenPass(encoder, w.gBufferViews[GColor], true, w.hbaoApplyPipeline, w.hbaoApplyBindGroup,
			"world hbao apply");
	}

	// The frame's HDR color and positions kept for the next frame's
	// reflections (GL's copyToMainFboOnlyLastFrameStuff, after its tone
	// mapping, which leaves the HDR color as it is).
	void recordLastFrame(CommandEncoder encoder)
	{
		if (!w.lastColorView || !w.lastPositionsTexture || !w.colorSourceGroup) { return; }
		fullscreenPass(encoder, w.lastColorView, false, w.lastColorPipeline, w.colorSourceGroup, "world last frame color");
		TexelCopyTextureInfo from = Default;
		from.texture = w.gBufferTextures[GPosition];
		from.mipLevel = 0;
		from.origin = { 0, 0, 0 };
		from.aspect = TextureAspect::All;
		TexelCopyTextureInfo to = from;
		to.texture = w.lastPositionsTexture;
		Extent3D size = { (uint32_t)w.depthWidth, (uint32_t)w.depthHeight, 1 };
		encoder.copyTextureToTexture(from, to, size);
	}

	// The asked-for debug views, each into its own small wgpu2d target, and
	// then the composite: the HDR color (or WGPU_GBUFFER_VIEW's attachment)
	// onto the screen, in the frame's 2D pass, under the UI. The views go
	// first: they need the frame encoder, which ends the 2D pass.
	void compositeFrame()
	{
		if (!w.frameRendered || !w.viewBindGroup) { return; }
		w.frameRendered = false;

		// Milestone 16: the exposure's average, of the HDR color as the
		// world left it (GL reads it before the bloom), then the bloom's
		// blur; milestone 17's AO; the bloom added on; and the frame kept
		// for the next one's reflections. GL's order.
		{
			CommandEncoder encoder = wgpuFrameEncoder();
			if (encoder)
			{
				recordExposure(encoder);
				recordBloom(encoder);
				recordHbao(encoder);
				recordBloomApply(encoder);
				if (w.underWater && w.warpBindGroup)
				{
					// Milestone 18: GL's warp, after the bloom, into its own texture.
					fullscreenPass(encoder, w.warpView, false, w.warpPipeline, w.warpBindGroup, "world underwater warp");
				}
				recordLastFrame(encoder);
			}
		}

		const uint32_t wanted = w.previewsWanted;
		w.previewsWanted = 0;
		if (wanted)
		{
			const int previewHeight = std::max(1, previewWidth * w.depthHeight / std::max(1, w.depthWidth));
			CommandEncoder encoder = wgpuFrameEncoder();
			for (int i = 0; i < gBufferViewCount && encoder; i++)
			{
				if (!(wanted & (1u << i))) { continue; }
				w.previews[i].resize(previewWidth, previewHeight);
				TextureView target = wgpuTextureView(w.previews[i].texture.id);
				if (!target) { continue; }

				RenderPassColorAttachment color = Default;
				color.view = target;
				color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
				color.resolveTarget = nullptr;
				color.loadOp = LoadOp::Clear;
				color.storeOp = StoreOp::Store;
				color.clearValue = Color{ 0.0, 0.0, 0.0, 1.0 };
				RenderPassDescriptor passDesc = Default;
				passDesc.label = StringView("world G-buffer view");
				passDesc.colorAttachmentCount = 1;
				passDesc.colorAttachments = &color;
				passDesc.depthStencilAttachment = nullptr;
				passDesc.occlusionQuerySet = nullptr;
				passDesc.timestampWrites = nullptr;
				RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
				pass.setPipeline(w.viewPipelines[i]);
				pass.setBindGroup(0, w.viewBindGroup, 0, nullptr);
				pass.draw(3, 1, 0, 0);
				pass.end();
				pass.release();
				wgpuMarkTargetDrawn(w.previews[i].texture.id);
				w.previewsDrawn |= 1u << i;
			}
		}

		// The post chain (milestone 15): tone map, then FXAA onto the
		// surface; or, with FXAA off, the tone map onto the surface. A
		// WGPU_GBUFFER_VIEW other than 0 shows that attachment instead.
		const bool chain = w.compositeView == 0;
		// Under water the tone mapper reads the warp's output (milestone 18).
		BindGroup toneMapGroup = w.underWater && w.toneMapWarpBindGroup ? w.toneMapWarpBindGroup : w.toneMapBindGroup;
		if (chain && w.fxaa)
		{
			CommandEncoder encoder = wgpuFrameEncoder();
			if (!encoder) { return; }
			RenderPassColorAttachment color = Default;
			color.view = w.fxaaInputView;
			color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			color.resolveTarget = nullptr;
			color.loadOp = LoadOp::Clear;
			color.storeOp = StoreOp::Store;
			color.clearValue = Color{ 0.0, 0.0, 0.0, 1.0 };
			RenderPassDescriptor passDesc = Default;
			passDesc.label = StringView("world tone map pass");
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments = &color;
			passDesc.depthStencilAttachment = nullptr;
			passDesc.occlusionQuerySet = nullptr;
			passDesc.timestampWrites = nullptr;
			RenderPassEncoder toneMapPass = encoder.beginRenderPass(passDesc);
			toneMapPass.setPipeline(w.toneMapToTexturePipeline);
			toneMapPass.setBindGroup(0, toneMapGroup, 0, nullptr);
			toneMapPass.draw(3, 1, 0, 0);
			toneMapPass.end();
			toneMapPass.release();
		}

		RenderPassEncoder pass = wgpuCurrentRenderPass();
		if (!pass) { return; }
		if (!chain)
		{
			pass.setPipeline(w.viewPipelines[w.compositeView]);
			pass.setBindGroup(0, w.viewBindGroup, 0, nullptr);
		}
		else if (w.fxaa)
		{
			pass.setPipeline(w.fxaaPipeline);
			pass.setBindGroup(0, w.fxaaBindGroup, 0, nullptr);
		}
		else
		{
			pass.setPipeline(w.toneMapToSurfacePipeline);
			pass.setBindGroup(0, toneMapGroup, 0, nullptr);
		}
		pass.draw(3, 1, 0, 0);
		// The 2D pass is wgpu2d's: it ends it, and its own draws rebind.
	}

	Buffer makeStorageBuffer(const void *data, uint64_t bytes, const char *label)
	{
		BufferDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = BufferUsage::Storage | BufferUsage::CopyDst;
		desc.size = std::max<uint64_t>(bytes, 4);
		desc.mappedAtCreation = false;
		Buffer buffer = wgpuDevice().createBuffer(desc);
		if (buffer && bytes) { wgpuQueue().writeBuffer(buffer, 0, data, bytes); }
		return buffer;
	}

	// The block icon pipeline (blockIcons.wgsl): the cube pipeline's layout,
	// depth as GL's GL_LESS, no blending (GL disables it: a texel's alpha is
	// stored as it is, so glass icons stay see-through).
	bool createIconPipeline()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/blockIcons.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world block icon pipeline");

		DepthStencilState depth = Default;
		depth.format = depthFormat;
		depth.depthWriteEnabled = OptionalBool::True;
		depth.depthCompare = CompareFunction::Less;
		depth.stencilFront.compare = CompareFunction::Always;
		depth.stencilFront.failOp = StencilOperation::Keep;
		depth.stencilFront.depthFailOp = StencilOperation::Keep;
		depth.stencilFront.passOp = StencilOperation::Keep;
		depth.stencilBack = depth.stencilFront;
		depth.stencilReadMask = 0;
		depth.stencilWriteMask = 0;
		depth.depthBias = 0;
		depth.depthBiasSlopeScale = 0.f;
		depth.depthBiasClamp = 0.f;

		ColorTargetState colorTarget = Default;
		colorTarget.format = wgpuSurfaceFormat(); // wgpu2d render targets' format
		colorTarget.blend = nullptr;
		colorTarget.writeMask = ColorWriteMask::All;

		FragmentState fragment = Default;
		fragment.module = module;
		fragment.entryPoint = StringView("fs_icon");
		fragment.constantCount = 0;
		fragment.constants = nullptr;
		fragment.targetCount = 1;
		fragment.targets = &colorTarget;

		RenderPipelineDescriptor desc = Default;
		desc.label = StringView("world block icons");
		desc.layout = w.cubePipelineLayout;
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView("vs_icon");
		desc.vertex.constantCount = 0;
		desc.vertex.constants = nullptr;
		desc.vertex.bufferCount = 0;
		desc.vertex.buffers = nullptr;
		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined;
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::Back;
		desc.primitive.unclippedDepth = false;
		desc.depthStencil = &depth;
		desc.fragment = &fragment;
		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;
		w.iconPipeline = wgpuDevice().createRenderPipeline(desc);
		module.release();
		return w.iconPipeline && !scope.failed();
	}

	// The corner and UV tables, as storage buffers.
	bool uploadBlockGeometry()
	{
		ErrorScope scope("world corner and UV tables");

		if (w.blockGeometryBuffer) { w.blockGeometryBuffer.release(); w.blockGeometryBuffer = nullptr; }
		if (w.blockUVBuffer) { w.blockUVBuffer.release(); w.blockUVBuffer = nullptr; }

		w.blockGeometryBuffer = makeStorageBuffer(w.blockGeometry.data(),
			w.blockGeometry.size() * sizeof(float), "world corner table");
		w.blockUVBuffer = makeStorageBuffer(w.blockUVs.data(),
			w.blockUVs.size() * sizeof(float), "world UV table");
		if (!w.blockGeometryBuffer || !w.blockUVBuffer || scope.failed()) { return false; }

		std::cout << "WebGPU world corner table: " << w.blockGeometry.size() / 12
			<< " orientations, UV table " << w.blockUVs.size() / 8 << "\n";
		return true;
	}

	// ---- Block textures --------------------------------------------------

	constexpr uint32_t blockTextureSize = 128;
	constexpr uint32_t blockTextureLevels = 5; // 128 down to 8: GL's GL_TEXTURE_MAX_LOD 4

	bool ensureBlockTexturePipelines()
	{
		if (w.fillPipeline && w.downsamplePipeline) { return true; }

		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/blockTextures.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world block texture pipelines");

		// layout = null: an automatic layout, built from the bindings the
		// entry point uses, read back with getBindGroupLayout(0).
		ComputePipelineDescriptor desc = Default;
		desc.layout = nullptr;
		desc.compute.nextInChain = nullptr;
		desc.compute.module = module;
		desc.compute.constantCount = 0;
		desc.compute.constants = nullptr;

		desc.label = StringView("block texture fill");
		desc.compute.entryPoint = StringView("fill");
		w.fillPipeline = wgpuDevice().createComputePipeline(desc);

		desc.label = StringView("block texture downsample");
		desc.compute.entryPoint = StringView("downsample");
		w.downsamplePipeline = wgpuDevice().createComputePipeline(desc);

		module.release();
		return w.fillPipeline && w.downsamplePipeline && !scope.failed();
	}

	TextureView makeArrayView(uint32_t baseLevel, uint32_t levels, uint32_t baseLayer, uint32_t layers,
		TextureViewDimension dimension, TextureUsage usage, const char *label)
	{
		TextureViewDescriptor viewDesc = Default;
		viewDesc.label = StringView(label);
		viewDesc.format = TextureFormat::RGBA8Unorm;
		viewDesc.dimension = dimension;
		viewDesc.baseMipLevel = baseLevel;
		viewDesc.mipLevelCount = levels;
		viewDesc.baseArrayLayer = baseLayer;
		viewDesc.arrayLayerCount = layers;
		viewDesc.aspect = TextureAspect::All;
		viewDesc.usage = usage;
		return w.textureArray.createView(viewDesc);
	}

	// Builds the texture array and the slot -> layer table from the handle
	// table. One layer per distinct texture: missing maps all point at the
	// loader's four defaults, so ~2000 slots share far fewer textures.
	bool buildBlockTextures()
	{
		if (w.textureHandles.empty()) { return false; }
		if (!ensureBlockTexturePipelines()) { return false; }
		const auto start = std::chrono::steady_clock::now();

		// A handle names a texture in its low 32 bits. Anything that is not a
		// live wgpu2d texture (a GL-only name, a zero) shows the loader's
		// default texture, slot 0, as a missing bindless handle would not.
		auto textureOf = [](uint64_t handle) -> uint32_t
		{
			const uint32_t id = (uint32_t)(handle & 0xFFFFFFFFu);
			return ((handle >> 32) && wgpuTextureView(id)) ? id : 0;
		};
		const uint32_t fallback = textureOf(w.textureHandles[0]);
		if (!fallback)
		{
			std::cerr << "WebGPU world: the default block texture is not a wgpu2d texture\n";
			return false;
		}

		std::vector<uint32_t> layerOfSlot(w.textureHandles.size());
		std::vector<uint32_t> textureOfLayer;
		std::unordered_map<uint32_t, uint32_t> layerOfTexture;
		for (size_t slot = 0; slot < w.textureHandles.size(); slot++)
		{
			uint32_t id = textureOf(w.textureHandles[slot]);
			if (!id) { id = fallback; }
			auto found = layerOfTexture.find(id);
			if (found == layerOfTexture.end())
			{
				found = layerOfTexture.emplace(id, (uint32_t)textureOfLayer.size()).first;
				textureOfLayer.push_back(id);
			}
			layerOfSlot[slot] = found->second;
		}

		Limits limits = Default;
		wgpuDevice().getLimits(&limits);
		const uint32_t layers = (uint32_t)textureOfLayer.size();
		if (layers > limits.maxTextureArrayLayers)
		{
			std::cerr << "WebGPU world: " << layers << " block textures, but maxTextureArrayLayers is "
				<< limits.maxTextureArrayLayers << "\n";
			return false;
		}

		ErrorScope scope("world block texture array");

		if (w.textureArrayView) { w.textureArrayView.release(); w.textureArrayView = nullptr; }
		if (w.textureArray) { w.textureArray.destroy(); w.textureArray.release(); w.textureArray = nullptr; }
		if (w.textureLayerBuffer) { w.textureLayerBuffer.release(); w.textureLayerBuffer = nullptr; }

		TextureDescriptor texDesc = Default;
		texDesc.label = StringView("world block textures");
		texDesc.usage = TextureUsage::TextureBinding | TextureUsage::StorageBinding;
		texDesc.dimension = TextureDimension::_2D; // an array is a 2D texture with layers
		texDesc.size.width = blockTextureSize;
		texDesc.size.height = blockTextureSize;
		texDesc.size.depthOrArrayLayers = layers;
		texDesc.format = TextureFormat::RGBA8Unorm;
		texDesc.mipLevelCount = blockTextureLevels;
		texDesc.sampleCount = 1;
		texDesc.viewFormatCount = 0;
		texDesc.viewFormats = nullptr;
		w.textureArray = wgpuDevice().createTexture(texDesc);
		if (!w.textureArray) { return false; }

		CommandEncoderDescriptor encoderDesc = Default;
		encoderDesc.label = StringView("world block textures");
		CommandEncoder encoder = wgpuDevice().createCommandEncoder(encoderDesc);
		ComputePassDescriptor passDesc = Default;
		passDesc.label = StringView("world block textures");
		passDesc.timestampWrites = nullptr;
		ComputePassEncoder pass = encoder.beginComputePass(passDesc);

		std::vector<TextureView> views;
		std::vector<BindGroup> groups;
		views.reserve(layers + 2 * blockTextureLevels);
		groups.reserve(layers + blockTextureLevels);

		// Level 0: one dispatch per texture, into its own layer. A storage
		// binding is one level of one layer here, so each gets a 2D view.
		BindGroupLayout fillLayout = w.fillPipeline.getBindGroupLayout(0);
		pass.setPipeline(w.fillPipeline);
		for (uint32_t layer = 0; layer < layers; layer++)
		{
			TextureView dst = makeArrayView(0, 1, layer, 1, TextureViewDimension::_2D,
				TextureUsage::StorageBinding, "block texture layer");
			views.push_back(dst);

			BindGroupEntry entries[2];
			entries[0] = Default;
			entries[0].binding = 0;
			entries[0].textureView = wgpuTextureView(textureOfLayer[layer]);
			entries[1] = Default;
			entries[1].binding = 1;
			entries[1].textureView = dst;

			BindGroupDescriptor groupDesc = Default;
			groupDesc.label = StringView("block texture fill");
			groupDesc.layout = fillLayout;
			groupDesc.entryCount = 2;
			groupDesc.entries = entries;
			BindGroup group = wgpuDevice().createBindGroup(groupDesc);
			groups.push_back(group);

			pass.setBindGroup(0, group, 0, nullptr);
			pass.dispatchWorkgroups(blockTextureSize / 8, blockTextureSize / 8, 1);
		}
		fillLayout.release();

		// Levels 1-4: every layer at once, through 2D-array views of one
		// level each. WebGPU orders the dispatches; no barrier to write.
		BindGroupLayout downsampleLayout = w.downsamplePipeline.getBindGroupLayout(0);
		pass.setPipeline(w.downsamplePipeline);
		for (uint32_t level = 1; level < blockTextureLevels; level++)
		{
			TextureView upper = makeArrayView(level - 1, 1, 0, layers, TextureViewDimension::_2DArray,
				TextureUsage::TextureBinding, "block texture upper level");
			TextureView lower = makeArrayView(level, 1, 0, layers, TextureViewDimension::_2DArray,
				TextureUsage::StorageBinding, "block texture lower level");
			views.push_back(upper);
			views.push_back(lower);

			BindGroupEntry entries[2];
			entries[0] = Default;
			entries[0].binding = 2;
			entries[0].textureView = upper;
			entries[1] = Default;
			entries[1].binding = 3;
			entries[1].textureView = lower;

			BindGroupDescriptor groupDesc = Default;
			groupDesc.label = StringView("block texture downsample");
			groupDesc.layout = downsampleLayout;
			groupDesc.entryCount = 2;
			groupDesc.entries = entries;
			BindGroup group = wgpuDevice().createBindGroup(groupDesc);
			groups.push_back(group);

			const uint32_t size = blockTextureSize >> level;
			pass.setBindGroup(0, group, 0, nullptr);
			pass.dispatchWorkgroups((size + 7) / 8, (size + 7) / 8, layers);
		}
		downsampleLayout.release();

		pass.end();
		CommandBufferDescriptor commandDesc = Default;
		commandDesc.label = StringView("world block textures");
		CommandBuffer commands = encoder.finish(commandDesc);
		wgpuQueue().submit(1, &commands);
		commands.release();
		pass.release();
		encoder.release();
		for (BindGroup &group : groups) { if (group) { group.release(); } }
		for (TextureView &view : views) { if (view) { view.release(); } }

		w.textureArrayView = makeArrayView(0, blockTextureLevels, 0, layers, TextureViewDimension::_2DArray,
			TextureUsage::TextureBinding, "world block textures");
		w.textureLayerBuffer = makeStorageBuffer(layerOfSlot.data(),
			layerOfSlot.size() * sizeof(uint32_t), "world texture layer table");

		if (!w.sampler)
		{
			SamplerDescriptor samplerDesc = Default;
			samplerDesc.label = StringView("world block sampler");
			samplerDesc.addressModeU = AddressMode::Repeat;
			samplerDesc.addressModeV = AddressMode::Repeat;
			samplerDesc.addressModeW = AddressMode::Repeat;
			samplerDesc.magFilter = FilterMode::Nearest;
			samplerDesc.minFilter = FilterMode::Nearest;
			samplerDesc.mipmapFilter = MipmapFilterMode::Nearest;
			samplerDesc.lodMinClamp = 0.0f;
			samplerDesc.lodMaxClamp = (float)(blockTextureLevels - 1);
			samplerDesc.compare = CompareFunction::Undefined;
			samplerDesc.maxAnisotropy = 1; // GL asked for 4; WebGPU allows it only with linear filters
			w.sampler = wgpuDevice().createSampler(samplerDesc);
		}

		if (!w.textureArrayView || !w.textureLayerBuffer || !w.sampler || scope.failed()) { return false; }

		const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
		std::cout << "WebGPU world block textures: " << layers << " layers for " << w.textureHandles.size()
			<< " slots, " << blockTextureSize << "x" << blockTextureSize << " x " << blockTextureLevels
			<< " levels, recorded in " << ms << " ms\n";
		return true;
	}

	// Brings the tables, the texture array and the bind group naming them up
	// to date. The bind group holds the buffers and views themselves, so it
	// is remade after any of them is.
	// Defined further down.
	bool ensureBuffer(Buffer &buffer, uint64_t &capacity, uint64_t needed, uint64_t unitBytes,
		BufferUsage usage, const char *label, bool &remade);
	bool ensureWhiteTexture();
	Texture createTarget(const char *label, TextureFormat format, TextureUsage usage, int width, int height);
	TextureView createTargetView(Texture texture, const char *label, TextureFormat format,
		TextureAspect aspect, TextureUsage usage);

	// The shadow sampler and the 1x1 stand-in always; the three maps once
	// `maps` asks for them (they stay after shadows are switched off).
	bool ensureShadowResources(bool maps)
	{
		if (!w.shadowSampler)
		{
			// GL's GL_NEAREST, compared in hardware: LessEqual returns 1
			// where the reference is no deeper than the stored depth.
			SamplerDescriptor samplerDesc = Default;
			samplerDesc.label = StringView("world shadow sampler");
			samplerDesc.addressModeU = AddressMode::ClampToEdge;
			samplerDesc.addressModeV = AddressMode::ClampToEdge;
			samplerDesc.addressModeW = AddressMode::ClampToEdge;
			samplerDesc.magFilter = FilterMode::Nearest;
			samplerDesc.minFilter = FilterMode::Nearest;
			samplerDesc.mipmapFilter = MipmapFilterMode::Nearest;
			samplerDesc.lodMinClamp = 0.0f;
			samplerDesc.lodMaxClamp = 32.0f;
			samplerDesc.compare = CompareFunction::LessEqual;
			samplerDesc.maxAnisotropy = 1;
			w.shadowSampler = wgpuDevice().createSampler(samplerDesc);
			if (!w.shadowSampler) { return false; }
		}
		if (!w.blankDepthView)
		{
			w.blankDepthTexture = createTarget("world blank shadow map", depthFormat,
				TextureUsage::TextureBinding, 1, 1);
			if (!w.blankDepthTexture) { return false; }
			w.blankDepthView = createTargetView(w.blankDepthTexture, "world blank shadow map view", depthFormat,
				TextureAspect::DepthOnly, TextureUsage::TextureBinding);
			if (!w.blankDepthView) { return false; }
		}
		if (maps && !w.shadowViews[0])
		{
			const char *const labels[shadowCascades] =
				{ "world sun shadow map 0", "world sun shadow map 1", "world sun shadow map 2" };
			for (int i = 0; i < shadowCascades; i++)
			{
				const TextureUsage usage = TextureUsage::RenderAttachment | TextureUsage::TextureBinding;
				w.shadowTextures[i] = createTarget(labels[i], depthFormat, usage,
					(int)shadowMapSizes[i], (int)shadowMapSizes[i]);
				if (!w.shadowTextures[i]) { return false; }
				w.shadowViews[i] = createTargetView(w.shadowTextures[i], labels[i], depthFormat,
					TextureAspect::DepthOnly, usage);
				if (!w.shadowViews[i]) { return false; }
			}
		}
		return true;
	}

	TextureView shadowBindView(int cascade)
	{
		return w.shadowViews[cascade] ? w.shadowViews[cascade] : w.blankDepthView;
	}

	bool updateResources()
	{
		bool changed = false;

		if (w.blockGeometryDirty)
		{
			if (w.blockGeometry.empty() || !uploadBlockGeometry()) { return false; }
			w.blockGeometryDirty = false;
			changed = true;
		}
		if (w.texturesDirty)
		{
			if (!buildBlockTextures()) { w.texturesDirty = false; w.failed = true; return false; }
			w.texturesDirty = false;
			changed = true;
		}
		// The point lights' buffer: room for at least one (a storage binding
		// can't be empty), grown to the frame's count.
		bool grownLights = false;
		if (!ensureBuffer(w.lightBuffer, w.lightCapacity, std::max<size_t>(w.frameLights.size(), 1),
			sizeof(glm::ivec4), BufferUsage::Storage, "world point lights", grownLights)) { return false; }
		if (!w.frameLights.empty())
		{
			wgpuQueue().writeBuffer(w.lightBuffer, 0, w.frameLights.data(), w.frameLights.size() * sizeof(glm::ivec4));
		}
		if (grownLights) { changed = true; }

		// GL's ao.png, mirrored in u (GL_MIRRORED_REPEAT on S), clamped in v
		// (gl2d's default), linear with mips (loaded with mipmaps, not
		// pixelated).
		if (!w.aoSampler)
		{
			SamplerDescriptor samplerDesc = Default;
			samplerDesc.label = StringView("world AO sampler");
			samplerDesc.addressModeU = AddressMode::MirrorRepeat;
			samplerDesc.addressModeV = AddressMode::ClampToEdge;
			samplerDesc.addressModeW = AddressMode::ClampToEdge;
			samplerDesc.magFilter = FilterMode::Linear;
			samplerDesc.minFilter = FilterMode::Linear;
			samplerDesc.mipmapFilter = MipmapFilterMode::Linear;
			samplerDesc.lodMinClamp = 0.0f;
			samplerDesc.lodMaxClamp = 32.0f;
			samplerDesc.compare = CompareFunction::Undefined;
			samplerDesc.maxAnisotropy = 1;
			w.aoSampler = wgpuDevice().createSampler(samplerDesc);
			if (!w.aoSampler) { return false; }
		}
		TextureView aoView = wgpuTextureView(w.aoTextureId);
		if (!aoView)
		{
			// No AO texture: the white texture reads as no darkening.
			if (!ensureWhiteTexture()) { return false; }
			aoView = w.whiteView;
		}
		if (aoView != w.boundAoView) { changed = true; }

		// The water's textures: gl2d's defaults for them (linear, mipmapped,
		// repeat), as loadFromFile(path, false, true) sets.
		if (!w.waterSampler)
		{
			SamplerDescriptor samplerDesc = Default;
			samplerDesc.label = StringView("world water sampler");
			samplerDesc.addressModeU = AddressMode::Repeat;
			samplerDesc.addressModeV = AddressMode::Repeat;
			samplerDesc.addressModeW = AddressMode::Repeat;
			samplerDesc.magFilter = FilterMode::Linear;
			samplerDesc.minFilter = FilterMode::Linear;
			samplerDesc.mipmapFilter = MipmapFilterMode::Linear;
			samplerDesc.lodMinClamp = 0.0f;
			samplerDesc.lodMaxClamp = 32.0f;
			samplerDesc.compare = CompareFunction::Undefined;
			samplerDesc.maxAnisotropy = 1;
			w.waterSampler = wgpuDevice().createSampler(samplerDesc);
			if (!w.waterSampler) { return false; }
		}
		TextureView waterViews[3];
		for (int i = 0; i < 3; i++)
		{
			waterViews[i] = wgpuTextureView(w.waterTextureIds[i]);
			if (!waterViews[i])
			{
				if (!ensureWhiteTexture()) { return false; }
				waterViews[i] = w.whiteView;
			}
			if (waterViews[i] != w.boundWaterViews[i]) { changed = true; }
		}

		if (!ensureShadowResources(w.shadowsWanted)) { return false; }
		for (int i = 0; i < shadowCascades; i++)
		{
			if (shadowBindView(i) != w.boundShadowViews[i]) { changed = true; }
		}

		if (!changed && w.bindGroup && w.shadowPassBindGroup) { return true; }
		if (!w.blockGeometryBuffer || !w.textureArrayView) { return false; }

		ErrorScope scope("world bind group");
		if (w.bindGroup) { w.bindGroup.release(); w.bindGroup = nullptr; }
		if (w.shadowPassBindGroup) { w.shadowPassBindGroup.release(); w.shadowPassBindGroup = nullptr; }

		constexpr uint32_t worldBindings = 17;
		BindGroupEntry entries[worldBindings];
		for (uint32_t i = 0; i < worldBindings; i++) { entries[i] = Default; entries[i].binding = i; }
		entries[0].buffer = w.uniformBuffer;
		entries[0].size = sizeof(Uniforms);
		entries[1].buffer = w.blockGeometryBuffer;
		entries[1].size = w.blockGeometryBuffer.getSize();
		entries[2].buffer = w.blockUVBuffer;
		entries[2].size = w.blockUVBuffer.getSize();
		entries[3].buffer = w.textureLayerBuffer;
		entries[3].size = w.textureLayerBuffer.getSize();
		entries[4].textureView = w.textureArrayView;
		entries[5].sampler = w.sampler;
		entries[6].buffer = w.lightBuffer;
		entries[6].size = w.lightBuffer.getSize();
		entries[7].textureView = aoView;
		entries[8].sampler = w.aoSampler;
		w.boundAoView = aoView;
		for (int i = 0; i < shadowCascades; i++)
		{
			entries[9 + i].textureView = shadowBindView(i);
			w.boundShadowViews[i] = shadowBindView(i);
		}
		entries[12].sampler = w.shadowSampler;
		for (int i = 0; i < 3; i++)
		{
			entries[13 + i].textureView = waterViews[i];
			w.boundWaterViews[i] = waterViews[i];
		}
		entries[16].sampler = w.waterSampler;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world bind group");
		groupDesc.layout = w.bindGroupLayout;
		groupDesc.entryCount = worldBindings;
		groupDesc.entries = entries;
		w.bindGroup = wgpuDevice().createBindGroup(groupDesc);

		// The same with the stand-in for the maps, for the shadow passes.
		for (int i = 0; i < shadowCascades; i++) { entries[9 + i].textureView = w.blankDepthView; }
		groupDesc.label = StringView("world shadow pass bind group");
		w.shadowPassBindGroup = wgpuDevice().createBindGroup(groupDesc);
		return w.bindGroup && w.shadowPassBindGroup && !scope.failed();
	}

	Texture createTarget(const char *label, TextureFormat format, TextureUsage usage, int width, int height)
	{
		TextureDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = usage;
		desc.dimension = TextureDimension::_2D;
		desc.size.width = (uint32_t)width;
		desc.size.height = (uint32_t)height;
		desc.size.depthOrArrayLayers = 1;
		desc.format = format;
		desc.mipLevelCount = 1;
		desc.sampleCount = 1;
		desc.viewFormatCount = 0;
		desc.viewFormats = nullptr;
		return wgpuDevice().createTexture(desc);
	}

	TextureView createTargetView(Texture texture, const char *label, TextureFormat format,
		TextureAspect aspect, TextureUsage usage)
	{
		TextureViewDescriptor viewDesc = Default;
		viewDesc.label = StringView(label);
		viewDesc.format = format;
		viewDesc.dimension = TextureViewDimension::_2D;
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = 1;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect = aspect;
		viewDesc.usage = usage;
		return texture.createView(viewDesc);
	}

	void releaseTargets()
	{
		releaseBloomTargets();
		for (BindGroup *group : { &w.peelBindGroup, &w.viewBindGroup, &w.toneMapBindGroup, &w.fxaaBindGroup,
			&w.hbaoBindGroup, &w.hbaoApplyBindGroup, &w.sunCountBindGroup, &w.rayMaskBindGroup, &w.rayBlurBindGroup,
			&w.warpBindGroup, &w.toneMapWarpBindGroup })
		{
			if (*group) { group->release(); *group = nullptr; }
		}
		for (TextureView *view : { &w.depthView, &w.peelDepthView, &w.peelColorView, &w.depthSampleView, &w.skyView,
			&w.fxaaInputView, &w.hbaoView, &w.lastColorView, &w.lastPositionsView, &w.sunRaysView, &w.sunRaysBlurView,
			&w.warpView })
		{
			if (*view) { view->release(); *view = nullptr; }
		}
		for (Texture *texture : { &w.depthTexture, &w.peelDepthTexture, &w.peelColorTexture, &w.skyTexture,
			&w.fxaaInputTexture, &w.hbaoTexture, &w.lastColorTexture, &w.lastPositionsTexture, &w.sunRaysTexture,
			&w.sunRaysBlurTexture, &w.warpTexture })
		{
			if (*texture) { texture->destroy(); texture->release(); *texture = nullptr; }
		}
		for (int i = 0; i < GBufferSlots; i++)
		{
			if (w.gBufferViews[i]) { w.gBufferViews[i].release(); w.gBufferViews[i] = nullptr; }
			if (w.gBufferTextures[i]) { w.gBufferTextures[i].destroy(); w.gBufferTextures[i].release(); w.gBufferTextures[i] = nullptr; }
		}
	}


	// The depth texture and the peel's copies have to match the surface's
	// size exactly (copies and attachments both demand it), so they follow it.
	bool ensureTargets(int width, int height)
	{
		if (w.depthTexture && w.depthWidth == width && w.depthHeight == height) { return true; }
		releaseTargets();

		ErrorScope scope("world render targets");

		// The depth buffer: also a copy source, for the peel, and sampled by
		// the depth view.
		w.depthTexture = createTarget("world depth", depthFormat,
			TextureUsage::RenderAttachment | TextureUsage::CopySrc | TextureUsage::TextureBinding, width, height);
		// The peel depth: a copy of it, drawn into (water) and then read.
		w.peelDepthTexture = createTarget("world peel depth", depthFormat,
			TextureUsage::RenderAttachment | TextureUsage::CopyDst | TextureUsage::TextureBinding, width, height);
		// The peel color: a copy of the HDR color, in its format (a texture
		// copy keeps the format; it cannot convert).
		w.peelColorTexture = createTarget("world peel color", w.hdrFormat,
			TextureUsage::CopyDst | TextureUsage::TextureBinding, width, height);
		// The sky behind the world (milestone 14, GL's fboSkyBox): drawn by
		// the sky pass beside the HDR color, then read by the fog.
		w.skyTexture = createTarget("world sky", w.hdrFormat,
			TextureUsage::RenderAttachment | TextureUsage::TextureBinding, width, height);
		// The FXAA input (milestone 15): the tone mapped image, in GL's
		// format for it (fboCoppy is R11F_G11F_B10F).
		w.fxaaInputTexture = createTarget("world FXAA input", w.hdrFormat,
			TextureUsage::RenderAttachment | TextureUsage::TextureBinding, width, height);
		if (!w.depthTexture || !w.peelDepthTexture || !w.peelColorTexture || !w.skyTexture
			|| !w.fxaaInputTexture) { return false; }

		// The G-buffer: drawn into, then read by the debug views (and, from
		// milestone 12 on, the lighting). The HDR color is also the peel's
		// copy source.
		for (int i = 0; i < GBufferSlots; i++)
		{
			TextureUsage usage = TextureUsage::RenderAttachment | TextureUsage::TextureBinding;
			if (i == GColor || i == GPosition) { usage = usage | TextureUsage::CopySrc; }
			w.gBufferTextures[i] = createTarget(gBufferSlotNames[i], gBufferFormat(i), usage, width, height);
			if (!w.gBufferTextures[i]) { return false; }
			w.gBufferViews[i] = createTargetView(w.gBufferTextures[i], gBufferSlotNames[i], gBufferFormat(i),
				TextureAspect::All, TextureUsage::RenderAttachment | TextureUsage::TextureBinding);
			if (!w.gBufferViews[i]) { return false; }
		}
		w.depthSampleView = createTargetView(w.depthTexture, "world depth sample view", depthFormat,
			TextureAspect::DepthOnly, TextureUsage::TextureBinding);
		if (!w.depthSampleView) { return false; }

		w.depthView = createTargetView(w.depthTexture, "world depth view", depthFormat,
			TextureAspect::DepthOnly, TextureUsage::RenderAttachment);
		// One view of the peel depth serves both uses (attachment, then
		// sampled) in different passes.
		w.peelDepthView = createTargetView(w.peelDepthTexture, "world peel depth view", depthFormat,
			TextureAspect::DepthOnly, TextureUsage::RenderAttachment | TextureUsage::TextureBinding);
		w.peelColorView = createTargetView(w.peelColorTexture, "world peel color view", w.hdrFormat,
			TextureAspect::All, TextureUsage::TextureBinding);
		w.skyView = createTargetView(w.skyTexture, "world sky view", w.hdrFormat,
			TextureAspect::All, TextureUsage::RenderAttachment | TextureUsage::TextureBinding);
		w.fxaaInputView = createTargetView(w.fxaaInputTexture, "world FXAA input view", w.hdrFormat,
			TextureAspect::All, TextureUsage::RenderAttachment | TextureUsage::TextureBinding);
		if (!w.depthView || !w.peelDepthView || !w.peelColorView || !w.skyView || !w.fxaaInputView) { return false; }

		// Milestone 17: the AO target and last frame's copies, before the
		// frame group that reads the copies.
		if (!ensureHbaoTargets(width, height) || scope.failed()) { return false; }
		// Milestone 18: the god rays' and the warp's.
		if (!ensureRayTargets(width, height) || scope.failed()) { return false; }

		// The frame group: the peel's copies, the sky and last frame's
		// color and positions, all the size of the surface, so remade with
		// them.
		BindGroupEntry entries[6];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].textureView = w.peelDepthView;
		entries[1] = Default;
		entries[1].binding = 1;
		entries[1].textureView = w.peelColorView;
		entries[2] = Default;
		entries[2].binding = 3;
		entries[2].textureView = w.skyView;
		for (int i = 3; i < 6; i++) { entries[i] = Default; entries[i].binding = (uint32_t)i + 1; }
		entries[3].textureView = w.lastColorView;
		entries[4].textureView = w.lastPositionsView;
		entries[5].sampler = w.postSampler;
		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world frame bind group");
		groupDesc.layout = w.peelBindGroupLayout;
		groupDesc.entryCount = 6;
		groupDesc.entries = entries;
		w.peelBindGroup = wgpuDevice().createBindGroup(groupDesc);
		if (!w.peelBindGroup || scope.failed()) { return false; }

		// The post passes' groups (milestone 15): each the uniforms, its
		// source, the sampler.
		auto postGroup = [&](TextureView sourceView, const char *label)
		{
			BindGroupEntry postEntries[3];
			for (uint32_t i = 0; i < 3; i++) { postEntries[i] = Default; postEntries[i].binding = i; }
			postEntries[0].buffer = w.postUniformBuffer;
			postEntries[0].size = sizeof(PostUniforms);
			postEntries[1].textureView = sourceView;
			postEntries[2].sampler = w.postSampler;
			BindGroupDescriptor postDesc = Default;
			postDesc.label = StringView(label);
			postDesc.layout = w.postBindGroupLayout;
			postDesc.entryCount = 3;
			postDesc.entries = postEntries;
			return wgpuDevice().createBindGroup(postDesc);
		};
		w.toneMapBindGroup = postGroup(w.gBufferViews[GColor], "world tone map bind group");
		w.fxaaBindGroup = postGroup(w.fxaaInputView, "world FXAA bind group");
		// Milestone 18: under water the tone mapper reads the warp's output.
		w.toneMapWarpBindGroup = postGroup(w.warpView, "world tone map bind group, under water");
		if (!w.toneMapBindGroup || !w.fxaaBindGroup || !w.toneMapWarpBindGroup) { return false; }

		// What the composite and the debug views read: the four targets,
		// the depth, the uniforms.
		BindGroupEntry viewEntries[viewBindings];
		for (int i = 0; i < viewBindings; i++) { viewEntries[i] = Default; viewEntries[i].binding = i; }
		for (int i = 0; i < GBufferSlots; i++) { viewEntries[i].textureView = w.gBufferViews[i]; }
		viewEntries[GBufferSlots].textureView = w.depthSampleView;
		viewEntries[GBufferSlots + 1].buffer = w.uniformBuffer;
		viewEntries[GBufferSlots + 1].size = sizeof(Uniforms);
		BindGroupDescriptor viewGroupDesc = Default;
		viewGroupDesc.label = StringView("world G-buffer view bind group");
		viewGroupDesc.layout = w.viewBindGroupLayout;
		viewGroupDesc.entryCount = viewBindings;
		viewGroupDesc.entries = viewEntries;
		w.viewBindGroup = wgpuDevice().createBindGroup(viewGroupDesc);
		if (!w.viewBindGroup || scope.failed()) { return false; }

		// Milestone 16: bloom's two blur chains and the exposure's average.
		if (!ensureBloomTargets(width, height) || scope.failed()) { return false; }

		w.depthWidth = width;
		w.depthHeight = height;
		std::cout << "WebGPU world targets: " << width << "x" << height
			<< " (G-buffer, HDR " << (w.hdrFormat == TextureFormat::RG11B10Ufloat ? "rg11b10ufloat" : "rgba16float")
			<< "; depth, peel depth, peel color)\n";
		return true;
	}

	// A whole-texture copy between two textures of one size and format. For
	// depth, WebGPU only copies whole subresources, which this always is.
	void copyWholeTexture(CommandEncoder encoder, Texture from, Texture to, int width, int height)
	{
		TexelCopyTextureInfo source = Default;
		source.texture = from;
		source.mipLevel = 0;
		source.origin = { 0, 0, 0 };
		source.aspect = TextureAspect::All;
		TexelCopyTextureInfo destination = source;
		destination.texture = to;
		Extent3D size = { (uint32_t)width, (uint32_t)height, 1 };
		encoder.copyTextureToTexture(source, destination, size);
	}

	// Draws `count` argument records starting at `first`, by the frame's
	// draw path. The records are already in w.drawArgs and, for the indirect
	// paths, in the indirect buffer at the same index.
	void drawRecords(RenderPassEncoder pass, DrawMode mode, uint32_t first, uint32_t count)
	{
		if (!count) { return; }
		if (mode == DrawMode::Direct)
		{
			for (uint32_t i = first; i < first + count; i++)
			{
				const DrawArgs &a = w.drawArgs[i];
				pass.draw(a.vertexCount, a.instanceCount, a.firstVertex, a.firstInstance);
			}
		}
		else if (mode == DrawMode::Multi)
		{
			pass.multiDrawIndirect(w.indirectBuffer, (uint64_t)first * sizeof(DrawArgs), count);
		}
		else
		{
			for (uint32_t i = first; i < first + count; i++)
			{
				pass.drawIndirect(w.indirectBuffer, (uint64_t)i * sizeof(DrawArgs));
			}
		}
	}

	// Binds what every chunk face pass binds.
	void bindFaces(RenderPassEncoder pass, RenderPipeline pipeline, bool peelGroup)
	{
		pass.setPipeline(pipeline);
		pass.setBindGroup(0, w.bindGroup, 0, nullptr);
		if (peelGroup) { pass.setBindGroup(1, w.peelBindGroup, 0, nullptr); }
		pass.setVertexBuffer(0, w.arena, 0, w.arenaFaces * faceBytes);
	}

	// Gives a chunk's faces a range of the arena, replacing the range it had
	// in `ranges`. An empty vector only frees.
	void placeFaces(std::unordered_map<const Chunk *, ChunkRange> &ranges, const Chunk *chunk,
		const std::vector<int> &faces)
	{
		auto it = ranges.find(chunk);
		if (it != ranges.end())
		{
			freeRange(it->second.firstFace, it->second.faceCount);
			w.facesInUse -= it->second.faceCount;
			ranges.erase(it);
		}

		const uint32_t count = (uint32_t)(faces.size() * sizeof(int) / faceBytes);
		if (count == 0) { return; }

		uint32_t first = 0;
		while (!allocateRange(count, first))
		{
			if (!growArena(w.arenaFaces + count)) { return; }
		}

		wgpuQueue().writeBuffer(w.arena, (uint64_t)first * faceBytes, faces.data(), (uint64_t)count * faceBytes);
		ranges[chunk] = ChunkRange{ first, count };
		w.facesInUse += count;
	}

	// Makes `buffer` hold at least `bytes`, doubling from `capacity`; false if
	// creation failed. The old buffer is released (draws already recorded
	// keep it alive until they ran). Returns true when it made a new one in
	// `remade`, so bind groups naming it can be remade.
	bool ensureBuffer(Buffer &buffer, uint64_t &capacity, uint64_t needed, uint64_t unitBytes,
		BufferUsage usage, const char *label, bool &remade)
	{
		remade = false;
		if (buffer && needed <= capacity) { return true; }
		uint64_t newCapacity = capacity ? capacity : 64;
		while (newCapacity < needed) { newCapacity *= 2; }

		BufferDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = usage | BufferUsage::CopyDst;
		desc.size = newCapacity * unitBytes;
		desc.mappedAtCreation = false;
		Buffer bigger = wgpuDevice().createBuffer(desc);
		if (!bigger) { return false; }
		if (buffer) { buffer.release(); }
		buffer = bigger;
		capacity = newCapacity;
		remade = true;
		return true;
	}

	// The cube vertices and this frame's cubes, and the bind group naming
	// both. False when there is nothing to draw.
	// The entity geometry's storage buffer, (re)made when it changed;
	// `remade` says the bind groups naming it are stale.
	bool ensureCubeVertices(bool &remade)
	{
		if (w.cubeGeometry.empty()) { return false; }
		if (w.cubeGeometryDirty || !w.cubeVertexBuffer)
		{
			if (w.cubeVertexBuffer) { w.cubeVertexBuffer.release(); w.cubeVertexBuffer = nullptr; }
			w.cubeVertexBuffer = makeStorageBuffer(w.cubeGeometry.data(),
				w.cubeGeometry.size() * sizeof(float), "world entity geometry");
			if (!w.cubeVertexBuffer) { return false; }
			w.cubeGeometryDirty = false;
			remade = true;
		}
		return true;
	}

	bool prepareCubes(const std::vector<BlockCube> &cubes)
	{
		if (cubes.empty()) { return false; }

		bool remade = false;
		if (!ensureCubeVertices(remade)) { return false; }

		bool grown = false;
		if (!ensureBuffer(w.cubeInstanceBuffer, w.cubeInstanceCapacity, cubes.size(), sizeof(BlockCube),
			BufferUsage::Storage, "world block cubes", grown)) { return false; }
		wgpuQueue().writeBuffer(w.cubeInstanceBuffer, 0, cubes.data(), cubes.size() * sizeof(BlockCube));

		if (remade || grown || !w.cubeBindGroup)
		{
			if (w.cubeBindGroup) { w.cubeBindGroup.release(); w.cubeBindGroup = nullptr; }
			BindGroupEntry entries[2];
			entries[0] = Default;
			entries[0].binding = 0;
			entries[0].buffer = w.cubeVertexBuffer;
			entries[0].size = w.cubeVertexBuffer.getSize();
			entries[1] = Default;
			entries[1].binding = 1;
			entries[1].buffer = w.cubeInstanceBuffer;
			entries[1].size = w.cubeInstanceBuffer.getSize();
			BindGroupDescriptor groupDesc = Default;
			groupDesc.label = StringView("world cube bind group");
			groupDesc.layout = w.cubeBindGroupLayout;
			groupDesc.entryCount = 2;
			groupDesc.entries = entries;
			w.cubeBindGroup = wgpuDevice().createBindGroup(groupDesc);
		}
		return (bool)w.cubeBindGroup;
	}

	// A 1x1 white texture, where GL used its tempWhite for items with none.
	bool ensureWhiteTexture()
	{
		if (w.whiteView) { return true; }
		w.whiteTexture = createTarget("world white texture", TextureFormat::RGBA8Unorm,
			TextureUsage::TextureBinding | TextureUsage::CopyDst, 1, 1);
		if (!w.whiteTexture) { return false; }
		const uint8_t white[4] = { 255, 255, 255, 255 };
		TexelCopyTextureInfo destination = Default;
		destination.texture = w.whiteTexture;
		destination.mipLevel = 0;
		destination.origin = { 0, 0, 0 };
		destination.aspect = TextureAspect::All;
		TexelCopyBufferLayout layout = Default;
		layout.offset = 0;
		layout.bytesPerRow = 4;
		layout.rowsPerImage = 1;
		Extent3D size = { 1, 1, 1 };
		wgpuQueue().writeTexture(destination, white, sizeof(white), layout, size);
		w.whiteView = createTargetView(w.whiteTexture, "world white texture view", TextureFormat::RGBA8Unorm,
			TextureAspect::All, TextureUsage::TextureBinding);
		return (bool)w.whiteView;
	}

	// The bind group of four entity textures, made once per combination.
	BindGroup textureGroup(const uint32_t (&ids)[4])
	{
		const std::array<uint32_t, 4> key = { ids[0], ids[1], ids[2], ids[3] };
		auto found = w.textureGroups.find(key);
		if (found != w.textureGroups.end()) { return found->second; }

		BindGroupEntry entries[4];
		for (uint32_t i = 0; i < 4; i++)
		{
			TextureView view = ids[i] ? wgpuTextureView(ids[i]) : TextureView(nullptr);
			entries[i] = Default;
			entries[i].binding = i;
			entries[i].textureView = view ? view : w.whiteView;
		}
		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world entity textures");
		groupDesc.layout = w.textureBindGroupLayout;
		groupDesc.entryCount = 4;
		groupDesc.entries = entries;
		BindGroup group = wgpuDevice().createBindGroup(groupDesc);
		if (group) { w.textureGroups.emplace(key, group); }
		return group;
	}

	void releaseMesh(Mesh &mesh)
	{
		if (mesh.vertices) { mesh.vertices.release(); }
		if (mesh.indices) { mesh.indices.release(); }
		mesh = {};
	}

	// The fastest path the device allows, unless OURCRAFT_WORLD_DRAWS asks for
	// another. Indirect needs IndirectFirstInstance: each chunk's record
	// points at its range of the arena through firstInstance.
	DrawMode chooseDrawMode()
	{
		const bool firstInstance = wgpuDevice().hasFeature(FeatureName::IndirectFirstInstance);
		const bool multi = wgpuDevice().hasFeature((WGPUFeatureName)WGPUNativeFeature_MultiDrawIndirect);

		DrawMode mode = multi && firstInstance ? DrawMode::Multi
			: firstInstance ? DrawMode::Indirect : DrawMode::Direct;
		if (const char *env = std::getenv("OURCRAFT_WORLD_DRAWS"))
		{
			const std::string wanted = env;
			if (wanted == "direct") { mode = DrawMode::Direct; }
			else if (wanted == "indirect" && firstInstance) { mode = DrawMode::Indirect; }
			else if (wanted == "multi" && firstInstance && multi) { mode = DrawMode::Multi; }
			else { std::cerr << "WebGPU world: OURCRAFT_WORLD_DRAWS=" << wanted << " not available here\n"; }
		}
		std::cout << "WebGPU world draws: " << drawModeName(mode) << " (IndirectFirstInstance "
			<< (firstInstance ? "yes" : "no") << ", MultiDrawIndirect " << (multi ? "yes" : "no") << ")\n";
		return mode;
	}

	bool ensureIndirectBuffer(uint64_t records)
	{
		if (records <= w.indirectCapacity) { return true; }

		uint64_t capacity = w.indirectCapacity ? w.indirectCapacity : 1024;
		while (capacity < records) { capacity *= 2; }

		ErrorScope scope("world indirect buffer");
		BufferDescriptor desc = Default;
		desc.label = StringView("world indirect draws");
		desc.usage = BufferUsage::Indirect | BufferUsage::CopyDst;
		desc.size = capacity * sizeof(DrawArgs);
		desc.mappedAtCreation = false;
		Buffer buffer = wgpuDevice().createBuffer(desc);
		if (!buffer || scope.failed()) { return false; }

		// Draws already recorded keep the old buffer alive until they ran.
		if (w.indirectBuffer) { w.indirectBuffer.release(); }
		w.indirectBuffer = buffer;
		w.indirectCapacity = capacity;
		return true;
	}

	// ---- Milestone 15: the post chain (post.wgsl) -------------------------

	// The post chain's uniforms, sampler and bind group layout: made once.
	// createPost makes its pipelines (again on a shader reload).
	bool createPostResources()
	{
		ErrorScope scope("world post chain resources");

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world post uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(PostUniforms);
		bufferDesc.mappedAtCreation = false;
		w.postUniformBuffer = wgpuDevice().createBuffer(bufferDesc);

		// GL's FBO textures: linear, clamped. FXAA reads between texels.
		SamplerDescriptor samplerDesc = Default;
		samplerDesc.label = StringView("world post sampler");
		samplerDesc.addressModeU = AddressMode::ClampToEdge;
		samplerDesc.addressModeV = AddressMode::ClampToEdge;
		samplerDesc.addressModeW = AddressMode::ClampToEdge;
		samplerDesc.magFilter = FilterMode::Linear;
		samplerDesc.minFilter = FilterMode::Linear;
		samplerDesc.mipmapFilter = MipmapFilterMode::Nearest;
		samplerDesc.lodMinClamp = 0.0f;
		samplerDesc.lodMaxClamp = 32.0f;
		samplerDesc.compare = CompareFunction::Undefined;
		samplerDesc.maxAnisotropy = 1;
		w.postSampler = wgpuDevice().createSampler(samplerDesc);
		if (!w.postUniformBuffer || !w.postSampler) { return false; }

		BindGroupLayoutEntry entries[3];
		for (uint32_t i = 0; i < 3; i++)
		{
			entries[i] = Default;
			entries[i].binding = i;
			entries[i].visibility = ShaderStage::Fragment;
			entries[i].buffer.type = BufferBindingType::BindingNotUsed;
			entries[i].sampler.type = SamplerBindingType::BindingNotUsed;
			entries[i].texture.sampleType = TextureSampleType::BindingNotUsed;
			entries[i].storageTexture.access = StorageTextureAccess::BindingNotUsed;
		}
		entries[0].buffer.type = BufferBindingType::Uniform;
		entries[0].buffer.hasDynamicOffset = false;
		entries[0].buffer.minBindingSize = sizeof(PostUniforms);
		entries[1].texture.sampleType = TextureSampleType::Float;
		entries[1].texture.viewDimension = TextureViewDimension::_2D;
		entries[1].texture.multisampled = false;
		entries[2].sampler.type = SamplerBindingType::Filtering;
		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world post bind group layout");
		layoutDesc.entryCount = 3;
		layoutDesc.entries = entries;
		w.postBindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		if (!w.postBindGroupLayout) { return false; }
		return !scope.failed();
	}

	bool createPost()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/post.wgsl");
		if (!module) { return false; }
		ErrorScope scope("world post chain");

		w.postPipelineLayout = createPipelineLayout("world post pipeline layout", &w.postBindGroupLayout, 1);

		// Each pass a fullscreen triangle into one target, no blending.
		auto pass = [&](const char *label, const char *fragmentEntry, TextureFormat format)
		{
			ColorTargetState target = Default;
			target.format = format;
			target.blend = nullptr;
			target.writeMask = ColorWriteMask::All;
			FragmentState fragment = Default;
			fragment.module = module;
			fragment.entryPoint = StringView(fragmentEntry);
			fragment.targetCount = 1;
			fragment.targets = &target;
			RenderPipelineDescriptor desc = Default;
			desc.label = StringView(label);
			desc.layout = w.postPipelineLayout;
			desc.vertex.module = module;
			desc.vertex.entryPoint = StringView("vs_fullscreen");
			desc.vertex.bufferCount = 0;
			desc.vertex.buffers = nullptr;
			desc.primitive.topology = PrimitiveTopology::TriangleList;
			desc.primitive.stripIndexFormat = IndexFormat::Undefined;
			desc.primitive.frontFace = FrontFace::CCW;
			desc.primitive.cullMode = CullMode::None;
			desc.depthStencil = nullptr;
			desc.fragment = &fragment;
			desc.multisample.count = 1;
			desc.multisample.mask = 0xFFFFFFFFu;
			desc.multisample.alphaToCoverageEnabled = false;
			return wgpuDevice().createRenderPipeline(desc);
		};
		w.toneMapToTexturePipeline = pass("world tone map, to the FXAA input", "fs_toneMap", w.hdrFormat);
		w.toneMapToSurfacePipeline = pass("world tone map, to the surface", "fs_toneMap", wgpuSurfaceFormat());
		w.fxaaPipeline = pass("world FXAA", "fs_fxaa", wgpuSurfaceFormat());

		const bool invalid = scope.failed();
		module.release();
		return w.toneMapToTexturePipeline && w.toneMapToSurfacePipeline && w.fxaaPipeline && !invalid;
	}

	// ---- Milestone 16: bloom and the auto exposure (bloom.wgsl) ----------

	// Bloom's buffers, sampler, black texture and layouts: made once.
	bool createBloomResources()
	{
		ErrorScope scope("world bloom resources");

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world bloom uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(BloomUniforms);
		bufferDesc.mappedAtCreation = false;
		w.bloomUniformBuffer = wgpuDevice().createBuffer(bufferDesc);

		// The exposure's reading: one texel, in a row padded to the 256
		// bytes a texture-to-buffer copy asks of each row.
		bufferDesc.label = StringView("world exposure readback");
		bufferDesc.usage = BufferUsage::MapRead | BufferUsage::CopyDst;
		bufferDesc.size = 256;
		w.exposureReadBuffer = wgpuDevice().createBuffer(bufferDesc);

		SamplerDescriptor samplerDesc = Default;
		samplerDesc.label = StringView("world lens dirt sampler");
		samplerDesc.addressModeU = AddressMode::ClampToEdge;
		samplerDesc.addressModeV = AddressMode::ClampToEdge;
		samplerDesc.addressModeW = AddressMode::ClampToEdge;
		samplerDesc.magFilter = FilterMode::Nearest;
		samplerDesc.minFilter = FilterMode::Nearest;
		samplerDesc.mipmapFilter = MipmapFilterMode::Nearest;
		samplerDesc.lodMinClamp = 0.0f;
		samplerDesc.lodMaxClamp = 32.0f;
		samplerDesc.compare = CompareFunction::Undefined;
		samplerDesc.maxAnisotropy = 1;
		w.dirtSampler = wgpuDevice().createSampler(samplerDesc);

		// Black, as WebGPU zeroes a new texture.
		w.blackTexture = createTarget("world black", TextureFormat::RGBA8Unorm, TextureUsage::TextureBinding, 1, 1);
		if (w.blackTexture)
		{
			w.blackView = createTargetView(w.blackTexture, "world black view", TextureFormat::RGBA8Unorm,
				TextureAspect::All, TextureUsage::TextureBinding);
		}
		if (!w.bloomUniformBuffer || !w.exposureReadBuffer || !w.dirtSampler || !w.blackView) { return false; }

		// Uniforms, one source, its sampler; the apply adds three dirt
		// textures and their sampler.
		BindGroupLayoutEntry entries[9];
		for (uint32_t i = 0; i < 9; i++)
		{
			entries[i] = Default;
			entries[i].binding = i;
			entries[i].visibility = ShaderStage::Fragment;
			entries[i].buffer.type = BufferBindingType::BindingNotUsed;
			entries[i].sampler.type = SamplerBindingType::BindingNotUsed;
			entries[i].texture.sampleType = TextureSampleType::BindingNotUsed;
			entries[i].storageTexture.access = StorageTextureAccess::BindingNotUsed;
			if (i == 0)
			{
				entries[i].buffer.type = BufferBindingType::Uniform;
				entries[i].buffer.hasDynamicOffset = false;
				entries[i].buffer.minBindingSize = sizeof(BloomUniforms);
			}
			else if (i == 2 || i == 6)
			{
				entries[i].sampler.type = SamplerBindingType::Filtering;
			}
			else
			{
				// 8: milestone 18's depth, the rest color textures.
				entries[i].texture.sampleType = i == 8 ? TextureSampleType(TextureSampleType::Depth)
					: TextureSampleType(TextureSampleType::Float);
				entries[i].texture.viewDimension = TextureViewDimension::_2D;
				entries[i].texture.multisampled = false;
			}
		}
		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world bloom bind group layout");
		layoutDesc.entryCount = 3;
		layoutDesc.entries = entries;
		w.bloomBindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		layoutDesc.label = StringView("world bloom apply bind group layout");
		layoutDesc.entryCount = 9;
		w.bloomApplyBindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		if (!w.bloomBindGroupLayout || !w.bloomApplyBindGroupLayout) { return false; }
		return !scope.failed();
	}

	bool createBloom()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/bloom.wgsl");
		if (!module) { return false; }
		ErrorScope scope("world bloom");

		w.bloomPipelineLayout = createPipelineLayout("world bloom pipeline layout", &w.bloomBindGroupLayout, 1);
		w.bloomApplyPipelineLayout = createPipelineLayout("world bloom apply pipeline layout",
			&w.bloomApplyBindGroupLayout, 1);

		// Each pass a fullscreen triangle into one target; GL's
		// glBlendFunc(GL_ONE, GL_ONE) where it adds.
		BlendState add = Default;
		add.color.operation = BlendOperation::Add;
		add.color.srcFactor = BlendFactor::One;
		add.color.dstFactor = BlendFactor::One;
		add.alpha = add.color;
		auto pass = [&](const char *label, const char *fragmentEntry, TextureFormat format, bool blend,
			PipelineLayout layout, const ConstantEntry *constant)
		{
			ColorTargetState target = Default;
			target.format = format;
			target.blend = blend ? &add : nullptr;
			target.writeMask = ColorWriteMask::All;
			FragmentState fragment = Default;
			fragment.module = module;
			fragment.entryPoint = StringView(fragmentEntry);
			fragment.constantCount = constant ? 1 : 0;
			fragment.constants = constant;
			fragment.targetCount = 1;
			fragment.targets = &target;
			RenderPipelineDescriptor desc = Default;
			desc.label = StringView(label);
			desc.layout = layout;
			desc.vertex.module = module;
			desc.vertex.entryPoint = StringView("vs_fullscreen");
			desc.vertex.bufferCount = 0;
			desc.vertex.buffers = nullptr;
			desc.primitive.topology = PrimitiveTopology::TriangleList;
			desc.primitive.stripIndexFormat = IndexFormat::Undefined;
			desc.primitive.frontFace = FrontFace::CCW;
			desc.primitive.cullMode = CullMode::None;
			desc.depthStencil = nullptr;
			desc.fragment = &fragment;
			desc.multisample.count = 1;
			desc.multisample.mask = 0xFFFFFFFFu;
			desc.multisample.alphaToCoverageEnabled = false;
			return wgpuDevice().createRenderPipeline(desc);
		};
		const PipelineLayout layout = w.bloomPipelineLayout;
		w.bloomFilterPipeline = pass("world bloom filter", "fs_filter", w.hdrFormat, true, layout, nullptr);
		w.bloomDownPipeline = pass("world bloom down", "fs_down", w.hdrFormat, false, layout, nullptr);
		w.bloomTentPipeline = pass("world bloom copy", "fs_tent", w.hdrFormat, false, layout, nullptr);
		w.bloomUpPipeline = pass("world bloom up", "fs_tent", w.hdrFormat, true, layout, nullptr);
		for (int h = 0; h < 2; h++)
		{
			ConstantEntry constant = Default;
			constant.key = StringView("horizontal");
			constant.value = h;
			w.bloomBlurPipelines[h] = pass(h ? "world bloom blur, horizontal" : "world bloom blur, vertical",
				"fs_blur", w.hdrFormat, false, layout, &constant);
		}
		w.bloomApplyPipeline = pass("world bloom apply", "fs_apply", w.hdrFormat, true,
			w.bloomApplyPipelineLayout, nullptr);
		w.exposurePipeline = pass("world exposure average", "fs_average", TextureFormat::RGBA16Float, false,
			layout, nullptr);
		// Milestone 17: the HDR color into last frame's RGBA8 copy, texel
		// for texel (the same size, so the read lands on texel centers).
		w.lastColorPipeline = pass("world last frame color", "fs_average", TextureFormat::RGBA8Unorm, false,
			layout, nullptr);

		const bool invalid = scope.failed();
		module.release();
		return w.bloomFilterPipeline && w.bloomDownPipeline && w.bloomTentPipeline && w.bloomUpPipeline
			&& w.bloomBlurPipelines[0] && w.bloomBlurPipelines[1] && w.bloomApplyPipeline && w.exposurePipeline
			&& w.lastColorPipeline && !invalid;
	}

	// A bloom group: the uniforms, one view, the linear sampler.
	BindGroup bloomGroup(TextureView view, const char *label)
	{
		BindGroupEntry entries[3];
		for (uint32_t i = 0; i < 3; i++) { entries[i] = Default; entries[i].binding = i; }
		entries[0].buffer = w.bloomUniformBuffer;
		entries[0].size = sizeof(BloomUniforms);
		entries[1].textureView = view;
		entries[2].sampler = w.postSampler;
		BindGroupDescriptor desc = Default;
		desc.label = StringView(label);
		desc.layout = w.bloomBindGroupLayout;
		desc.entryCount = 3;
		desc.entries = entries;
		return wgpuDevice().createBindGroup(desc);
	}

	// One texture with `levels` mips, a view and a reading group per mip.
	Texture mipChain(const char *label, TextureFormat format, int width, int height, int levels,
		std::vector<TextureView> &views, std::vector<BindGroup> &groups)
	{
		TextureDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = TextureUsage::RenderAttachment | TextureUsage::TextureBinding | TextureUsage::CopySrc;
		desc.dimension = TextureDimension::_2D;
		desc.size = { (uint32_t)width, (uint32_t)height, 1 };
		desc.format = format;
		desc.mipLevelCount = (uint32_t)levels;
		desc.sampleCount = 1;
		desc.viewFormatCount = 0;
		desc.viewFormats = nullptr;
		Texture texture = wgpuDevice().createTexture(desc);
		if (!texture) { return nullptr; }
		for (int m = 0; m < levels; m++)
		{
			TextureViewDescriptor viewDesc = Default;
			viewDesc.label = StringView(label);
			viewDesc.format = format;
			viewDesc.dimension = TextureViewDimension::_2D;
			viewDesc.baseMipLevel = (uint32_t)m;
			viewDesc.mipLevelCount = 1;
			viewDesc.baseArrayLayer = 0;
			viewDesc.arrayLayerCount = 1;
			viewDesc.aspect = TextureAspect::All;
			viewDesc.usage = TextureUsage::RenderAttachment | TextureUsage::TextureBinding;
			views.push_back(texture.createView(viewDesc));
			groups.push_back(views.back() ? bloomGroup(views.back(), label) : nullptr);
		}
		return texture;
	}

	// GL's bloom levels for a surface size: from half the surface, halving
	// while both sides stay above 4 and one at least 12 (the last level
	// kept is the one before that test fails).
	int bloomMipsFor(int width, int height)
	{
		int mips = 0;
		int mipW = width / 2;
		int mipH = height / 2;
		for (int i = 0; i < 16; i++)
		{
			if (mipW <= 4 || mipH <= 4 || (mipW < 12 && mipH < 12)) { break; }
			mipW /= 2;
			mipH /= 2;
			mips = i;
		}
		return mips;
	}

	bool ensureBloomTargets(int width, int height)
	{
		releaseBloomTargets();
		if (!w.bloomBindGroupLayout) { return false; }
		// Too small to halve: no bloom, no exposure readings (both skip
		// empty chains), the frame itself still drawn.
		if (width < 2 || height < 2) { return true; }

		w.colorSourceGroup = bloomGroup(w.gBufferViews[GColor], "world bloom source: HDR color");
		w.bloomSourceGroup = bloomGroup(w.gBufferViews[GBloom], "world bloom source: G-buffer bloom");

		w.bloomMips = bloomMipsFor(width, height);
		for (int k = 0; k < 2; k++)
		{
			w.blurTextures[k] = mipChain(k ? "world bloom blur 1" : "world bloom blur 0", w.hdrFormat,
				width / 2, height / 2, w.bloomMips + 1, w.blurViews[k], w.blurGroups[k]);
		}

		// floor(log2) of the larger side: from half the surface down to 1x1.
		int levels = 0;
		while ((std::max(width, height) >> (levels + 1)) > 0) { levels++; }
		w.exposureTexture = mipChain("world exposure average", TextureFormat::RGBA16Float, width / 2, height / 2,
			levels, w.exposureViews, w.exposureGroups);

		auto all = [](const auto &list) { return std::all_of(list.begin(), list.end(), [](const auto &x) { return bool(x); }); };
		return w.colorSourceGroup && w.bloomSourceGroup && w.blurTextures[0] && w.blurTextures[1]
			&& w.exposureTexture && all(w.blurViews[0]) && all(w.blurViews[1]) && all(w.blurGroups[0])
			&& all(w.blurGroups[1]) && all(w.exposureViews) && all(w.exposureGroups);
	}

	void releaseBloomTargets()
	{
		if (w.bloomApplyGroup) { w.bloomApplyGroup.release(); w.bloomApplyGroup = nullptr; }
		std::fill(std::begin(w.boundApplyViews), std::end(w.boundApplyViews), nullptr);
		for (BindGroup *group : { &w.colorSourceGroup, &w.bloomSourceGroup })
		{
			if (*group) { group->release(); *group = nullptr; }
		}
		auto releaseChain = [](Texture &texture, std::vector<TextureView> &views, std::vector<BindGroup> &groups)
		{
			for (BindGroup &group : groups) { if (group) { group.release(); } }
			for (TextureView &view : views) { if (view) { view.release(); } }
			groups.clear();
			views.clear();
			if (texture) { texture.destroy(); texture.release(); texture = nullptr; }
		};
		releaseChain(w.blurTextures[0], w.blurViews[0], w.blurGroups[0]);
		releaseChain(w.blurTextures[1], w.blurViews[1], w.blurGroups[1]);
		// A copy recorded into the old chain but not yet submitted is still
		// fine: the texture is destroyed only once the queue is done with it.
		releaseChain(w.exposureTexture, w.exposureViews, w.exposureGroups);
	}

	// ---- Milestone 17: ambient occlusion (hbao.wgsl) ----------------------

	// The AO's uniforms and its two passes' layouts: made once.
	bool createHbaoResources()
	{
		ErrorScope scope("world hbao resources");

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world hbao uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(HbaoUniforms);
		bufferDesc.mappedAtCreation = false;
		w.hbaoUniformBuffer = wgpuDevice().createBuffer(bufferDesc);
		if (!w.hbaoUniformBuffer) { return false; }

		// Bindings 0 uniforms, 1 positions, 2 normals or 3 the AO, 4 the
		// sampler: two layouts, one for each pass.
		auto layout = [&](uint32_t textureBinding, TextureSampleType textureType, const char *label)
		{
			BindGroupLayoutEntry entries[4];
			const uint32_t bindings[4] = { 0, 1, textureBinding, 4 };
			for (uint32_t i = 0; i < 4; i++)
			{
				entries[i] = Default;
				entries[i].binding = bindings[i];
				entries[i].visibility = ShaderStage::Fragment;
				entries[i].buffer.type = BufferBindingType::BindingNotUsed;
				entries[i].sampler.type = SamplerBindingType::BindingNotUsed;
				entries[i].texture.sampleType = TextureSampleType::BindingNotUsed;
				entries[i].storageTexture.access = StorageTextureAccess::BindingNotUsed;
			}
			entries[0].buffer.type = BufferBindingType::Uniform;
			entries[0].buffer.hasDynamicOffset = false;
			entries[0].buffer.minBindingSize = sizeof(HbaoUniforms);
			for (uint32_t i : { 1u, 2u })
			{
				entries[i].texture.viewDimension = TextureViewDimension::_2D;
				entries[i].texture.multisampled = false;
			}
			entries[1].texture.sampleType = TextureSampleType::Float;
			entries[2].texture.sampleType = textureType;
			entries[3].sampler.type = SamplerBindingType::Filtering;
			BindGroupLayoutDescriptor desc = Default;
			desc.label = StringView(label);
			desc.entryCount = 4;
			desc.entries = entries;
			return wgpuDevice().createBindGroupLayout(desc);
		};
		w.hbaoBindGroupLayout = layout(2, TextureSampleType::Uint, "world hbao bind group layout");
		w.hbaoApplyBindGroupLayout = layout(3, TextureSampleType::Float, "world hbao apply bind group layout");
		if (!w.hbaoBindGroupLayout || !w.hbaoApplyBindGroupLayout) { return false; }
		return !scope.failed();
	}

	bool createHbao()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/hbao.wgsl");
		if (!module) { return false; }
		ErrorScope scope("world hbao");

		w.hbaoPipelineLayout = createPipelineLayout("world hbao pipeline layout", &w.hbaoBindGroupLayout, 1);
		w.hbaoApplyPipelineLayout = createPipelineLayout("world hbao apply pipeline layout",
			&w.hbaoApplyBindGroupLayout, 1);

		// The apply: GL's glBlendFunci(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
		BlendState over = Default;
		over.color.operation = BlendOperation::Add;
		over.color.srcFactor = BlendFactor::SrcAlpha;
		over.color.dstFactor = BlendFactor::OneMinusSrcAlpha;
		over.alpha = over.color;
		auto pass = [&](const char *label, const char *fragmentEntry, TextureFormat format, const BlendState *blend,
			PipelineLayout pipelineLayout)
		{
			ColorTargetState target = Default;
			target.format = format;
			target.blend = blend;
			target.writeMask = ColorWriteMask::All;
			FragmentState fragment = Default;
			fragment.module = module;
			fragment.entryPoint = StringView(fragmentEntry);
			fragment.targetCount = 1;
			fragment.targets = &target;
			RenderPipelineDescriptor desc = Default;
			desc.label = StringView(label);
			desc.layout = pipelineLayout;
			desc.vertex.module = module;
			desc.vertex.entryPoint = StringView("vs_fullscreen");
			desc.vertex.bufferCount = 0;
			desc.vertex.buffers = nullptr;
			desc.primitive.topology = PrimitiveTopology::TriangleList;
			desc.primitive.stripIndexFormat = IndexFormat::Undefined;
			desc.primitive.frontFace = FrontFace::CCW;
			desc.primitive.cullMode = CullMode::None;
			desc.depthStencil = nullptr;
			desc.fragment = &fragment;
			desc.multisample.count = 1;
			desc.multisample.mask = 0xFFFFFFFFu;
			desc.multisample.alphaToCoverageEnabled = false;
			return wgpuDevice().createRenderPipeline(desc);
		};
		w.hbaoPipeline = pass("world hbao", "fs_hbao", TextureFormat::R8Unorm, nullptr, w.hbaoPipelineLayout);
		w.hbaoApplyPipeline = pass("world hbao apply", "fs_apply", w.hdrFormat, &over, w.hbaoApplyPipelineLayout);

		const bool invalid = scope.failed();
		module.release();
		return w.hbaoPipeline && w.hbaoApplyPipeline && !invalid;
	}

	// The AO target, half the surface, and the two passes' groups; and
	// last frame's color and positions.
	bool ensureHbaoTargets(int width, int height)
	{
		w.lastColorTexture = createTarget("world last frame color", TextureFormat::RGBA8Unorm,
			TextureUsage::RenderAttachment | TextureUsage::TextureBinding, width, height);
		w.lastPositionsTexture = createTarget("world last frame positions", gBufferFormat(GPosition),
			TextureUsage::CopyDst | TextureUsage::TextureBinding, width, height);
		w.hbaoTexture = createTarget("world hbao", TextureFormat::R8Unorm,
			TextureUsage::RenderAttachment | TextureUsage::TextureBinding, std::max(1, width / 2), std::max(1, height / 2));
		if (!w.lastColorTexture || !w.lastPositionsTexture || !w.hbaoTexture) { return false; }
		w.lastColorView = createTargetView(w.lastColorTexture, "world last frame color view", TextureFormat::RGBA8Unorm,
			TextureAspect::All, TextureUsage::RenderAttachment | TextureUsage::TextureBinding);
		w.lastPositionsView = createTargetView(w.lastPositionsTexture, "world last frame positions view",
			gBufferFormat(GPosition), TextureAspect::All, TextureUsage::TextureBinding);
		w.hbaoView = createTargetView(w.hbaoTexture, "world hbao view", TextureFormat::R8Unorm,
			TextureAspect::All, TextureUsage::RenderAttachment | TextureUsage::TextureBinding);
		if (!w.lastColorView || !w.lastPositionsView || !w.hbaoView) { return false; }

		auto group = [&](BindGroupLayout layout, uint32_t textureBinding, TextureView view, const char *label)
		{
			BindGroupEntry entries[4];
			const uint32_t bindings[4] = { 0, 1, textureBinding, 4 };
			for (uint32_t i = 0; i < 4; i++) { entries[i] = Default; entries[i].binding = bindings[i]; }
			entries[0].buffer = w.hbaoUniformBuffer;
			entries[0].size = sizeof(HbaoUniforms);
			entries[1].textureView = w.gBufferViews[GPosition];
			entries[2].textureView = view;
			entries[3].sampler = w.postSampler;
			BindGroupDescriptor desc = Default;
			desc.label = StringView(label);
			desc.layout = layout;
			desc.entryCount = 4;
			desc.entries = entries;
			return wgpuDevice().createBindGroup(desc);
		};
		w.hbaoBindGroup = group(w.hbaoBindGroupLayout, 2, w.gBufferViews[GNormal], "world hbao bind group");
		w.hbaoApplyBindGroup = group(w.hbaoApplyBindGroupLayout, 3, w.hbaoView, "world hbao apply bind group");
		return w.hbaoBindGroup && w.hbaoApplyBindGroup;
	}

	// ---- Milestone 18: the god rays and the underwater warp ---------------

	// A fullscreen pipeline of `module` into one target.
	RenderPipeline fullscreenPipeline(ShaderModule module, const char *label, const char *fragmentEntry,
		TextureFormat format, PipelineLayout layout)
	{
		ColorTargetState target = Default;
		target.format = format;
		target.blend = nullptr;
		target.writeMask = ColorWriteMask::All;
		FragmentState fragment = Default;
		fragment.module = module;
		fragment.entryPoint = StringView(fragmentEntry);
		fragment.targetCount = 1;
		fragment.targets = &target;
		RenderPipelineDescriptor desc = Default;
		desc.label = StringView(label);
		desc.layout = layout;
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView("vs_fullscreen");
		desc.vertex.bufferCount = 0;
		desc.vertex.buffers = nullptr;
		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined;
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::None;
		desc.depthStencil = nullptr;
		desc.fragment = &fragment;
		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;
		return wgpuDevice().createRenderPipeline(desc);
	}

	// A fragment-stage layout entry with nothing set.
	BindGroupLayoutEntry emptyEntry(uint32_t binding)
	{
		BindGroupLayoutEntry e = Default;
		e.binding = binding;
		e.visibility = ShaderStage::Fragment;
		e.buffer.type = BufferBindingType::BindingNotUsed;
		e.sampler.type = SamplerBindingType::BindingNotUsed;
		e.texture.sampleType = TextureSampleType::BindingNotUsed;
		e.storageTexture.access = StorageTextureAccess::BindingNotUsed;
		return e;
	}

	BindGroupLayoutEntry textureEntry(uint32_t binding, TextureSampleType type)
	{
		BindGroupLayoutEntry e = emptyEntry(binding);
		e.texture.sampleType = type;
		e.texture.viewDimension = TextureViewDimension::_2D;
		e.texture.multisampled = false;
		return e;
	}

	BindGroupLayoutEntry uniformEntry(uint32_t binding, uint64_t size)
	{
		BindGroupLayoutEntry e = emptyEntry(binding);
		e.buffer.type = BufferBindingType::Uniform;
		e.buffer.hasDynamicOffset = false;
		e.buffer.minBindingSize = size;
		return e;
	}

	BindGroupLayoutEntry samplerEntry(uint32_t binding)
	{
		BindGroupLayoutEntry e = emptyEntry(binding);
		e.sampler.type = SamplerBindingType::Filtering;
		return e;
	}

	BindGroupLayout makeLayout(const char *label, const BindGroupLayoutEntry *entries, uint32_t count)
	{
		BindGroupLayoutDescriptor desc = Default;
		desc.label = StringView(label);
		desc.entryCount = count;
		desc.entries = entries;
		return wgpuDevice().createBindGroupLayout(desc);
	}

	Buffer makeUniformBuffer(const char *label, uint64_t size)
	{
		BufferDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		desc.size = size;
		desc.mappedAtCreation = false;
		return wgpuDevice().createBuffer(desc);
	}

	// The god rays' and the warp's uniforms and layouts: made once.
	bool createRaysAndWarpResources()
	{
		ErrorScope scope("world god rays and warp resources");

		w.rayUniformBuffer = makeUniformBuffer("world god ray uniforms", sizeof(RayUniforms));
		w.warpUniformBuffer = makeUniformBuffer("world warp uniforms", sizeof(WarpUniforms));

		const BindGroupLayoutEntry maskEntries[1] = { textureEntry(3, TextureSampleType::Depth) };
		w.rayMaskBindGroupLayout = makeLayout("world god ray mask bind group layout", maskEntries, 1);
		const BindGroupLayoutEntry blurEntries[3] = { uniformEntry(0, sizeof(RayUniforms)),
			textureEntry(1, TextureSampleType::Float), samplerEntry(2) };
		w.rayBlurBindGroupLayout = makeLayout("world god ray blur bind group layout", blurEntries, 3);
		const BindGroupLayoutEntry warpEntries[4] = { uniformEntry(0, sizeof(WarpUniforms)),
			textureEntry(1, TextureSampleType::Float), textureEntry(2, TextureSampleType::Float), samplerEntry(3) };
		w.warpBindGroupLayout = makeLayout("world warp bind group layout", warpEntries, 4);
		if (!w.rayUniformBuffer || !w.warpUniformBuffer || !w.rayMaskBindGroupLayout || !w.rayBlurBindGroupLayout
			|| !w.warpBindGroupLayout)
		{
			return false;
		}
		return !scope.failed();
	}

	bool createRaysAndWarp()
	{
		ShaderModule rays = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/godRays.wgsl");
		ShaderModule warp = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/underwater.wgsl");
		if (!rays || !warp)
		{
			if (rays) { rays.release(); }
			if (warp) { warp.release(); }
			return false;
		}
		ErrorScope scope("world god rays and warp");

		w.rayMaskPipelineLayout = createPipelineLayout("world god ray mask pipeline layout", &w.rayMaskBindGroupLayout, 1);
		w.rayBlurPipelineLayout = createPipelineLayout("world god ray blur pipeline layout", &w.rayBlurBindGroupLayout, 1);
		w.warpPipelineLayout = createPipelineLayout("world warp pipeline layout", &w.warpBindGroupLayout, 1);

		w.rayMaskPipeline = fullscreenPipeline(rays, "world god ray mask", "fs_mask", w.hdrFormat, w.rayMaskPipelineLayout);
		w.rayBlurPipeline = fullscreenPipeline(rays, "world god ray blur", "fs_radialBlur", w.hdrFormat,
			w.rayBlurPipelineLayout);
		w.warpPipeline = fullscreenPipeline(warp, "world underwater warp", "fs_warp", w.hdrFormat, w.warpPipelineLayout);

		const bool invalid = scope.failed();
		rays.release();
		warp.release();
		return w.rayMaskPipeline && w.rayBlurPipeline && w.warpPipeline && !invalid;
	}

	// The half-size god ray targets, the warp's output, and their groups.
	bool ensureRayTargets(int width, int height)
	{
		const int halfWidth = std::max(1, width / 2), halfHeight = std::max(1, height / 2);
		const TextureUsage usage = TextureUsage::RenderAttachment | TextureUsage::TextureBinding;
		w.sunRaysTexture = createTarget("world god rays sun", w.hdrFormat, usage, halfWidth, halfHeight);
		w.sunRaysBlurTexture = createTarget("world god rays", w.hdrFormat, usage, halfWidth, halfHeight);
		w.warpTexture = createTarget("world underwater warp", w.hdrFormat, usage, width, height);
		if (!w.sunRaysTexture || !w.sunRaysBlurTexture || !w.warpTexture) { return false; }
		w.sunRaysView = createTargetView(w.sunRaysTexture, "world god rays sun view", w.hdrFormat, TextureAspect::All, usage);
		w.sunRaysBlurView = createTargetView(w.sunRaysBlurTexture, "world god rays view", w.hdrFormat, TextureAspect::All, usage);
		w.warpView = createTargetView(w.warpTexture, "world underwater warp view", w.hdrFormat, TextureAspect::All, usage);
		if (!w.sunRaysView || !w.sunRaysBlurView || !w.warpView) { return false; }

		auto group = [](BindGroupLayout layout, BindGroupEntry *entries, uint32_t count, const char *label)
		{
			BindGroupDescriptor desc = Default;
			desc.label = StringView(label);
			desc.layout = layout;
			desc.entryCount = count;
			desc.entries = entries;
			return wgpuDevice().createBindGroup(desc);
		};

		BindGroupEntry count[2];
		for (uint32_t i = 0; i < 2; i++) { count[i] = Default; count[i].binding = i; }
		count[0].textureView = w.depthSampleView;
		count[1].buffer = w.sunCountBuffer;
		count[1].size = 4;
		w.sunCountBindGroup = group(w.sunCountBindGroupLayout, count, 2, "world sun count bind group");

		BindGroupEntry mask = Default;
		mask.binding = 3;
		mask.textureView = w.depthSampleView;
		w.rayMaskBindGroup = group(w.rayMaskBindGroupLayout, &mask, 1, "world god ray mask bind group");

		BindGroupEntry blur[3];
		for (uint32_t i = 0; i < 3; i++) { blur[i] = Default; blur[i].binding = i; }
		blur[0].buffer = w.rayUniformBuffer;
		blur[0].size = sizeof(RayUniforms);
		blur[1].textureView = w.sunRaysView;
		blur[2].sampler = w.postSampler;
		w.rayBlurBindGroup = group(w.rayBlurBindGroupLayout, blur, 3, "world god ray blur bind group");

		BindGroupEntry warpEntries[4];
		for (uint32_t i = 0; i < 4; i++) { warpEntries[i] = Default; warpEntries[i].binding = i; }
		warpEntries[0].buffer = w.warpUniformBuffer;
		warpEntries[0].size = sizeof(WarpUniforms);
		warpEntries[1].textureView = w.gBufferViews[GColor];
		warpEntries[2].textureView = w.gBufferViews[GPosition];
		warpEntries[3].sampler = w.postSampler;
		w.warpBindGroup = group(w.warpBindGroupLayout, warpEntries, 4, "world warp bind group");

		return w.sunCountBindGroup && w.rayMaskBindGroup && w.rayBlurBindGroup && w.warpBindGroup;
	}

	// ---- Milestone 14: the sky (sky.wgsl) -------------------------------

	BindGroupLayoutEntry skyLayoutEntry(uint32_t binding, ShaderStage visibility)
	{
		BindGroupLayoutEntry e = Default;
		e.binding = binding;
		e.visibility = visibility;
		e.buffer.type = BufferBindingType::BindingNotUsed;
		e.sampler.type = SamplerBindingType::BindingNotUsed;
		e.texture.sampleType = TextureSampleType::BindingNotUsed;
		e.storageTexture.access = StorageTextureAccess::BindingNotUsed;
		return e;
	}

	// The skies' cubemaps and views, buffers, sampler, layouts and the bake's
	// group: made once. createSky makes the pipelines (again on a reload).
	bool createSkyResources()
	{
		ErrorScope scope("world sky resources");

		// A cubemap is a 2D texture with six layers, +X -X +Y -Y +Z -Z.
		// It is sampled through one view of dimension Cube, and each face
		// is drawn into through a 2D view of that one layer.
		const char *const skyNames[SkyKinds] = { "world day sky", "world twilight sky", "world night sky" };
		for (int k = 0; k < SkyKinds; k++)
		{
			TextureDescriptor desc = Default;
			desc.label = StringView(skyNames[k]);
			desc.usage = TextureUsage::RenderAttachment | TextureUsage::TextureBinding;
			desc.dimension = TextureDimension::_2D;
			desc.size = { skyCubeSize, skyCubeSize, 6 };
			desc.format = w.hdrFormat; // GL's GL_R11F_G11F_B10F
			desc.mipLevelCount = 1;
			desc.sampleCount = 1;
			desc.viewFormatCount = 0;
			desc.viewFormats = nullptr;
			w.skyCubes[k] = wgpuDevice().createTexture(desc);
			if (!w.skyCubes[k]) { return false; }

			TextureViewDescriptor viewDesc = Default;
			viewDesc.label = StringView(skyNames[k]);
			viewDesc.format = w.hdrFormat;
			viewDesc.dimension = TextureViewDimension::Cube;
			viewDesc.baseMipLevel = 0;
			viewDesc.mipLevelCount = 1;
			viewDesc.baseArrayLayer = 0;
			viewDesc.arrayLayerCount = 6;
			viewDesc.aspect = TextureAspect::All;
			viewDesc.usage = TextureUsage::TextureBinding;
			w.skyCubeViews[k] = w.skyCubes[k].createView(viewDesc);

			for (uint32_t face = 0; face < 6; face++)
			{
				viewDesc.dimension = TextureViewDimension::_2D;
				viewDesc.baseArrayLayer = face;
				viewDesc.arrayLayerCount = 1;
				viewDesc.usage = TextureUsage::RenderAttachment;
				w.skyFaceViews[k][face] = w.skyCubes[k].createView(viewDesc);
			}
		}

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world sky bake settings");
		bufferDesc.usage = BufferUsage::Storage | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(SkyBakeParams) * SkyKinds;
		bufferDesc.mappedAtCreation = false;
		w.skyBakeBuffer = wgpuDevice().createBuffer(bufferDesc);
		bufferDesc.label = StringView("world sky uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(SkyUniforms);
		w.skyUniformBuffer = wgpuDevice().createBuffer(bufferDesc);
		if (!w.skyBakeBuffer || !w.skyUniformBuffer) { return false; }

		// Linear and clamped: GL's cubemap parameters, and gl2d's for the
		// sun and moon (loaded without pixelation or mipmaps).
		SamplerDescriptor samplerDesc = Default;
		samplerDesc.label = StringView("world sky sampler");
		samplerDesc.addressModeU = AddressMode::ClampToEdge;
		samplerDesc.addressModeV = AddressMode::ClampToEdge;
		samplerDesc.addressModeW = AddressMode::ClampToEdge;
		samplerDesc.magFilter = FilterMode::Linear;
		samplerDesc.minFilter = FilterMode::Linear;
		samplerDesc.mipmapFilter = MipmapFilterMode::Nearest;
		samplerDesc.lodMinClamp = 0.0f;
		samplerDesc.lodMaxClamp = 32.0f;
		samplerDesc.compare = CompareFunction::Undefined;
		samplerDesc.maxAnisotropy = 1;
		w.skySampler = wgpuDevice().createSampler(samplerDesc);
		if (!w.skySampler) { return false; }

		// The bake's group: the three skies' settings.
		BindGroupLayoutEntry bakeEntry = skyLayoutEntry(0, ShaderStage::Fragment);
		bakeEntry.buffer.type = BufferBindingType::ReadOnlyStorage;
		bakeEntry.buffer.hasDynamicOffset = false;
		bakeEntry.buffer.minBindingSize = 0;
		BindGroupLayoutDescriptor bakeLayoutDesc = Default;
		bakeLayoutDesc.label = StringView("world sky bake bind group layout");
		bakeLayoutDesc.entryCount = 1;
		bakeLayoutDesc.entries = &bakeEntry;
		w.skyBakeBindGroupLayout = wgpuDevice().createBindGroupLayout(bakeLayoutDesc);

		// The sky's and the sun's group: the uniforms, the three cubes, the
		// sampler, the sun and moon textures.
		BindGroupLayoutEntry skyEntries[7];
		skyEntries[0] = skyLayoutEntry(0, ShaderStage::Vertex | ShaderStage::Fragment);
		skyEntries[0].buffer.type = BufferBindingType::Uniform;
		skyEntries[0].buffer.hasDynamicOffset = false;
		skyEntries[0].buffer.minBindingSize = sizeof(SkyUniforms);
		for (uint32_t i = 1; i <= 3; i++)
		{
			skyEntries[i] = skyLayoutEntry(i, ShaderStage::Fragment);
			skyEntries[i].texture.sampleType = TextureSampleType::Float;
			skyEntries[i].texture.viewDimension = TextureViewDimension::Cube;
			skyEntries[i].texture.multisampled = false;
		}
		skyEntries[4] = skyLayoutEntry(4, ShaderStage::Fragment);
		skyEntries[4].sampler.type = SamplerBindingType::Filtering;
		for (uint32_t i = 5; i <= 6; i++)
		{
			skyEntries[i] = skyLayoutEntry(i, ShaderStage::Fragment);
			skyEntries[i].texture.sampleType = TextureSampleType::Float;
			skyEntries[i].texture.viewDimension = TextureViewDimension::_2D;
			skyEntries[i].texture.multisampled = false;
		}
		BindGroupLayoutDescriptor skyLayoutDesc = Default;
		skyLayoutDesc.label = StringView("world sky bind group layout");
		skyLayoutDesc.entryCount = 7;
		skyLayoutDesc.entries = skyEntries;
		w.skyBindGroupLayout = wgpuDevice().createBindGroupLayout(skyLayoutDesc);
		if (!w.skyBakeBindGroupLayout || !w.skyBindGroupLayout) { return false; }

		BindGroupEntry bakeGroupEntry = Default;
		bakeGroupEntry.binding = 0;
		bakeGroupEntry.buffer = w.skyBakeBuffer;
		bakeGroupEntry.size = sizeof(SkyBakeParams) * SkyKinds;
		BindGroupDescriptor bakeGroupDesc = Default;
		bakeGroupDesc.label = StringView("world sky bake bind group");
		bakeGroupDesc.layout = w.skyBakeBindGroupLayout;
		bakeGroupDesc.entryCount = 1;
		bakeGroupDesc.entries = &bakeGroupEntry;
		w.skyBakeBindGroup = wgpuDevice().createBindGroup(bakeGroupDesc);

		// Milestone 18: the god rays' second group, the scene's depth and the
		// flare's counter, and the counter's buffers.
		BindGroupLayoutEntry countEntries[2] = { textureEntry(0, TextureSampleType::Depth), emptyEntry(1) };
		countEntries[1].buffer.type = BufferBindingType::Storage;
		countEntries[1].buffer.hasDynamicOffset = false;
		countEntries[1].buffer.minBindingSize = 4;
		w.sunCountBindGroupLayout = makeLayout("world sun count bind group layout", countEntries, 2);
		bufferDesc.label = StringView("world sun count");
		bufferDesc.usage = BufferUsage::Storage | BufferUsage::CopySrc | BufferUsage::CopyDst;
		bufferDesc.size = 4;
		w.sunCountBuffer = wgpuDevice().createBuffer(bufferDesc);
		bufferDesc.label = StringView("world sun count readback");
		bufferDesc.usage = BufferUsage::MapRead | BufferUsage::CopyDst;
		w.sunReadBuffer = wgpuDevice().createBuffer(bufferDesc);
		if (!w.sunCountBindGroupLayout || !w.sunCountBuffer || !w.sunReadBuffer || !w.skyBakeBindGroup) { return false; }
		return !scope.failed();
	}

	bool createSky()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/sky.wgsl");
		if (!module) { return false; }
		ErrorScope scope("world sky");

		w.skyBakePipelineLayout = createPipelineLayout("world sky bake pipeline layout", &w.skyBakeBindGroupLayout, 1);
		w.skyPipelineLayout = createPipelineLayout("world sky pipeline layout", &w.skyBindGroupLayout, 1);

		// Three pipelines, each a fullscreen triangle or a quad, no vertex
		// buffers.
		auto pipeline = [&](const char *label, PipelineLayout layout, const char *vertexEntry,
			const char *fragmentEntry, ColorTargetState *targets, uint32_t targetCount,
			const DepthStencilState *depth, PrimitiveTopology topology)
		{
			RenderPipelineDescriptor desc = Default;
			desc.label = StringView(label);
			desc.layout = layout;
			desc.vertex.module = module;
			desc.vertex.entryPoint = StringView(vertexEntry);
			desc.vertex.bufferCount = 0;
			desc.vertex.buffers = nullptr;
			desc.primitive.topology = topology;
			desc.primitive.stripIndexFormat = IndexFormat::Undefined;
			desc.primitive.frontFace = FrontFace::CCW;
			desc.primitive.cullMode = CullMode::None;
			desc.depthStencil = depth;
			desc.multisample.count = 1;
			desc.multisample.mask = 0xFFFFFFFFu;
			desc.multisample.alphaToCoverageEnabled = false;
			FragmentState fragment = Default;
			fragment.module = module;
			fragment.entryPoint = StringView(fragmentEntry);
			fragment.targetCount = targetCount;
			fragment.targets = targets;
			desc.fragment = &fragment;
			return wgpuDevice().createRenderPipeline(desc);
		};

		ColorTargetState one[GBufferSlots];
		colorTargets(ColorTargets::Hdr, nullptr, one);
		w.skyBakePipeline = pipeline("world sky bake", w.skyBakePipelineLayout, "vs_fullscreen", "fs_bake",
			one, 1, nullptr, PrimitiveTopology::TriangleList);

		// The sky copy and the HDR color: the same format, the same color.
		ColorTargetState two[2] = { one[0], one[0] };
		w.skyPipeline = pipeline("world sky", w.skyPipelineLayout, "vs_fullscreen", "fs_sky",
			two, 2, nullptr, PrimitiveTopology::TriangleList);

		// The sun and moon (renderSun): onto the G-buffer, the color added
		// by alpha (GL_SRC_ALPHA, GL_ONE), positions and normals untouched,
		// the bloom written over; at the far plane, LessEqual, so only
		// where nothing was drawn; no depth writes.
		BlendState add = Default;
		add.color.operation = BlendOperation::Add;
		add.color.srcFactor = BlendFactor::SrcAlpha;
		add.color.dstFactor = BlendFactor::One;
		add.alpha = add.color;
		ColorTargetState sunTargets[GBufferSlots];
		colorTargets(ColorTargets::GBuffer, nullptr, sunTargets);
		sunTargets[GColor].blend = &add;
		sunTargets[GPosition].writeMask = ColorWriteMask::None;
		sunTargets[GNormal].writeMask = ColorWriteMask::None;
		DepthStencilState depth = Default;
		depth.format = depthFormat;
		depth.depthWriteEnabled = OptionalBool::False;
		depth.depthCompare = CompareFunction::LessEqual;
		depth.stencilFront.compare = CompareFunction::Always;
		depth.stencilFront.failOp = StencilOperation::Keep;
		depth.stencilFront.depthFailOp = StencilOperation::Keep;
		depth.stencilFront.passOp = StencilOperation::Keep;
		depth.stencilBack = depth.stencilFront;
		depth.stencilReadMask = 0;
		depth.stencilWriteMask = 0;
		w.sunPipeline = pipeline("world sun and moon", w.skyPipelineLayout, "vs_sun", "fs_sun",
			sunTargets, GBufferSlots, &depth, PrimitiveTopology::TriangleStrip);

		// Milestone 18: the god rays' sun and moon, into one half-size
		// target, no blending or depth (GL's state for them), with a second
		// group: the scene's depth and the flare's counter.
		BindGroupLayout rayGroups[2] = { w.skyBindGroupLayout, w.sunCountBindGroupLayout };
		w.sunRaysPipelineLayout = createPipelineLayout("world sun rays pipeline layout", rayGroups, 2);
		w.sunRaysPipeline = pipeline("world sun and moon for the god rays", w.sunRaysPipelineLayout, "vs_sun",
			"fs_sunRays", one, 1, nullptr, PrimitiveTopology::TriangleStrip);

		const bool invalid = scope.failed();
		module.release();
		return w.skyBakePipeline && w.skyPipeline && w.sunPipeline && w.sunRaysPipeline && !invalid;
	}

	// GL's three skies (SkyBoxLoaderAndDrawer::createSkyTextures), baked
	// into their cubemaps when first needed and again when the day sky's
	// settings change (the ImGui "Sky" colors). Only the day sky takes
	// them; twilight and night are fixed. GL's bake also convolves and
	// prefilters each sky for image-based lighting; nothing reads those
	// (only the physicallyAccurateReflections path would, and a constant
	// turns it off), so they are not made.
	void bakeSkies(CommandEncoder encoder, const Lighting &lighting)
	{
		if (w.skyBaked && w.bakedDayColor == lighting.daySkyColor && w.bakedDayHorizon == lighting.daySkyHorizon
			&& w.bakedDayG == lighting.daySkyG) { return; }

		SkyBakeParams params[SkyKinds];
		params[DaySky].lightPos = glm::vec4(0, -1, 0, 0);
		params[DaySky].color1 = glm::vec4(lighting.daySkyColor, 0);
		params[DaySky].color2 = glm::vec4(lighting.daySkyHorizon, 0);
		params[DaySky].groundColor = glm::vec4(1, 1, 1, lighting.daySkyG);
		params[DaySky].flags = glm::uvec4(0);
		params[TwilightSky].lightPos = glm::vec4(glm::normalize(glm::vec3(0, -0.1f, 1)), 0);
		params[TwilightSky].color1 = glm::vec4(glm::vec3(22, 49, 111) * 0.9f / 255.f, 0);
		params[TwilightSky].color2 = glm::vec4(glm::vec3(35, 22, 36) * 0.9f / 255.f, 0);
		params[TwilightSky].groundColor = glm::vec4(1, 1, 1, 0.995f);
		params[TwilightSky].flags = glm::uvec4(0);
		params[NightSky].lightPos = glm::vec4(0, -1, 0, 0);
		params[NightSky].color1 = glm::vec4(glm::vec3(9, 15, 23) / 255.f * 0.2f, 0);
		params[NightSky].color2 = glm::vec4(glm::vec3(4, 8, 10) / 255.f * 0.2f, 0);
		params[NightSky].groundColor = glm::vec4(1, 1, 1, 0.850f);
		params[NightSky].flags = glm::uvec4(0);
		wgpuQueue().writeBuffer(w.skyBakeBuffer, 0, params, sizeof(params));

		// Eighteen small passes, a face each: the face's view is the
		// target, and the draw's instance tells the shader which sky and
		// which face (a direct draw may start at any instance).
		for (uint32_t k = 0; k < SkyKinds; k++)
		{
			for (uint32_t face = 0; face < 6; face++)
			{
				RenderPassColorAttachment color = Default;
				color.view = w.skyFaceViews[k][face];
				color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
				color.resolveTarget = nullptr;
				color.loadOp = LoadOp::Clear;
				color.storeOp = StoreOp::Store;
				color.clearValue = Color{ 0.0, 0.0, 0.0, 1.0 };
				RenderPassDescriptor passDesc = Default;
				passDesc.label = StringView("world sky bake pass");
				passDesc.colorAttachmentCount = 1;
				passDesc.colorAttachments = &color;
				passDesc.depthStencilAttachment = nullptr;
				passDesc.occlusionQuerySet = nullptr;
				passDesc.timestampWrites = nullptr;
				RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
				pass.setPipeline(w.skyBakePipeline);
				pass.setBindGroup(0, w.skyBakeBindGroup, 0, nullptr);
				pass.draw(3, 1, 0, k * 6 + face);
				pass.end();
				pass.release();
			}
		}

		if (!w.skyBaked) { std::cout << "WebGPU world: baked the three skies, 6 x 128x128 faces each\n"; }
		w.skyBaked = true;
		w.bakedDayColor = lighting.daySkyColor;
		w.bakedDayHorizon = lighting.daySkyHorizon;
		w.bakedDayG = lighting.daySkyG;
	}

	// The sky's group, remade when the sun or moon texture changes.
	bool ensureSkyBindGroup(uint32_t sunTexture, uint32_t moonTexture)
	{
		TextureView sunView = wgpuTextureView(sunTexture);
		TextureView moonView = wgpuTextureView(moonTexture);
		w.sunDrawable = sunView && moonView;
		if (!sunView || !moonView)
		{
			if (!ensureWhiteTexture()) { return false; }
			if (!sunView) { sunView = w.whiteView; }
			if (!moonView) { moonView = w.whiteView; }
		}
		if (w.skyBindGroup && sunView == w.boundSunView && moonView == w.boundMoonView) { return true; }
		if (w.skyBindGroup) { w.skyBindGroup.release(); w.skyBindGroup = nullptr; }

		BindGroupEntry entries[7];
		for (uint32_t i = 0; i < 7; i++) { entries[i] = Default; entries[i].binding = i; }
		entries[0].buffer = w.skyUniformBuffer;
		entries[0].size = sizeof(SkyUniforms);
		for (int k = 0; k < SkyKinds; k++) { entries[1 + k].textureView = w.skyCubeViews[k]; }
		entries[4].sampler = w.skySampler;
		entries[5].textureView = sunView;
		entries[6].textureView = moonView;
		BindGroupDescriptor desc = Default;
		desc.label = StringView("world sky bind group");
		desc.layout = w.skyBindGroupLayout;
		desc.entryCount = 7;
		desc.entries = entries;
		w.skyBindGroup = wgpuDevice().createBindGroup(desc);
		w.boundSunView = sunView;
		w.boundMoonView = moonView;
		return (bool)w.skyBindGroup;
	}

	// SunRenderer's createTBNMatrix: turns what faces (0, 0, -1) toward
	// `direction`.
	glm::mat4 sunFrame(const glm::vec3 &direction)
	{
		const glm::vec3 up(0.0f, 1.0f, 0.0f);
		const glm::vec3 forward = -glm::normalize(direction);
		const glm::vec3 right = glm::normalize(glm::cross(up, forward));
		const glm::vec3 newUp = glm::cross(forward, right);
		return glm::mat4(glm::mat3(right, newUp, forward));
	}

	// SunRenderer::render's matrix, scale included (GL scales two entries
	// of the final matrix rather than the quad).
	glm::mat4 sunMatrix(Camera &camera, const glm::vec3 &direction, float scale)
	{
		glm::mat4 m = camera.getProjectionMatrix() * camera.getViewMatrix() * sunFrame(direction);
		m[0][0] *= scale;
		m[1][1] *= scale;
		return m;
	}

	// The sun and the moon (renderSun), onto the G-buffer after the
	// entities and before the transparent passes, as GL draws them.
	// Milestone 18: renderSun's god ray passes, against the depth as the
	// opaque faces and entities left it (the transparent ones come after,
	// so water does not block the rays). At half size: the sun and moon
	// again (the sun also counting its visible pixels for the flare), the
	// geometry masked out, then the radial blur. Always run, so the rays
	// clear when the sun cannot be drawn.
	void recordSunRays(CommandEncoder encoder, bool drawable)
	{
		if (!w.sunRaysView || !w.sunRaysBlurView || !w.rayMaskBindGroup || !w.rayBlurBindGroup) { return; }
		encoder.clearBuffer(w.sunCountBuffer, 0, 4);

		RenderPassColorAttachment color = Default;
		color.view = w.sunRaysView;
		color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
		color.resolveTarget = nullptr;
		color.loadOp = LoadOp::Clear;
		color.storeOp = StoreOp::Store;
		color.clearValue = Color{ 0.0, 0.0, 0.0, 0.0 };
		RenderPassDescriptor passDesc = Default;
		passDesc.label = StringView("world god rays sun pass");
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &color;
		passDesc.depthStencilAttachment = nullptr;
		passDesc.occlusionQuerySet = nullptr;
		passDesc.timestampWrites = nullptr;
		RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
		if (drawable && w.sunCountBindGroup)
		{
			pass.setPipeline(w.sunRaysPipeline);
			pass.setBindGroup(0, w.skyBindGroup, 0, nullptr);
			pass.setBindGroup(1, w.sunCountBindGroup, 0, nullptr);
			pass.draw(4, 2, 0, 2); // instance 2 the sun, 3 the moon
		}
		pass.end();
		pass.release();

		fullscreenPass(encoder, w.sunRaysView, true, w.rayMaskPipeline, w.rayMaskBindGroup, "world god rays mask");
		fullscreenPass(encoder, w.sunRaysBlurView, false, w.rayBlurPipeline, w.rayBlurBindGroup, "world god rays blur");

		// The count out, when no earlier one is still in flight.
		if (w.sunReadState == World::Readback::Idle)
		{
			encoder.copyBufferToBuffer(w.sunCountBuffer, 0, w.sunReadBuffer, 0, 4);
			w.sunReadState = World::Readback::Copied;
		}
	}

	void drawSunAndMoon()
	{
		if (!w.sunPending) { return; }
		w.sunPending = false;
		CommandEncoder encoder = wgpuFrameEncoder();
		if (!encoder) { return; }
		const bool drawable = w.sunDrawable && w.skyBindGroup;
		if (drawable)
		{
			RenderPassEncoder pass = beginGBufferPass(encoder, false, "world sun and moon pass");
			pass.setPipeline(w.sunPipeline);
			pass.setBindGroup(0, w.skyBindGroup, 0, nullptr);
			pass.draw(4, 2, 0, 0); // instance 0 the sun, 1 the moon
			pass.end();
			pass.release();
		}
		recordSunRays(encoder, drawable);
	}

	// Everything made from the WGSL files: the pipelines and their layouts
	// (milestone 20). Bind group layouts, buffers, samplers and bind groups
	// come from the *Resources functions and createLayoutAndUniforms, once.
	bool createPipelineSet()
	{
		return createPipelines() && createEntityPipelines() && createViewPipelines() && createSky()
			&& createPost() && createBloom() && createHbao() && createRaysAndWarp();
	}

	// Every handle createPipelineSet assigns, for a reload to move aside,
	// put back or release. The block icons' and block textures' pipelines
	// are not here: they run once, at load.
	template <typename OnPipeline, typename OnLayout>
	void forEachReloadable(OnPipeline &&onPipeline, OnLayout &&onLayout)
	{
		RenderPipeline *const pipelines[] = {
			&w.pipeline, &w.prepassPipeline, &w.solidLessPipeline, &w.waterDepthPipeline, &w.peelPipeline,
			&w.finalPeelPipeline, &w.finalPlainPipeline, &w.decalPipeline,
			&w.cubePipeline, &w.linePipeline, &w.modelPipeline, &w.itemPipeline,
			&w.skyBakePipeline, &w.skyPipeline, &w.sunPipeline, &w.sunRaysPipeline,
			&w.toneMapToTexturePipeline, &w.toneMapToSurfacePipeline, &w.fxaaPipeline,
			&w.bloomFilterPipeline, &w.bloomDownPipeline, &w.bloomTentPipeline, &w.bloomUpPipeline,
			&w.bloomBlurPipelines[0], &w.bloomBlurPipelines[1], &w.bloomApplyPipeline, &w.exposurePipeline,
			&w.lastColorPipeline, &w.hbaoPipeline, &w.hbaoApplyPipeline,
			&w.rayMaskPipeline, &w.rayBlurPipeline, &w.warpPipeline };
		for (RenderPipeline *pipeline : pipelines) { onPipeline(*pipeline); }
		for (RenderPipeline &pipeline : w.shadowPipelines) { onPipeline(pipeline); }
		for (RenderPipeline &pipeline : w.viewPipelines) { onPipeline(pipeline); }

		PipelineLayout *const layouts[] = {
			&w.pipelineLayout, &w.transparentPipelineLayout, &w.decalPipelineLayout, &w.cubePipelineLayout,
			&w.entityPipelineLayout, &w.viewPipelineLayout, &w.skyBakePipelineLayout, &w.skyPipelineLayout,
			&w.sunRaysPipelineLayout, &w.postPipelineLayout, &w.bloomPipelineLayout, &w.bloomApplyPipelineLayout,
			&w.hbaoPipelineLayout, &w.hbaoApplyPipelineLayout, &w.rayMaskPipelineLayout, &w.rayBlurPipelineLayout,
			&w.warpPipelineLayout };
		for (PipelineLayout *layout : layouts) { onLayout(*layout); }
	}

	bool initialize()
	{
		if (w.initialized) { return true; }
		if (w.failed || !wgpuDevice()) { return false; }

		// The G-buffer's HDR format first: the pipelines name it.
		w.hdrFormat = wgpuDevice().hasFeature(FeatureName::RG11B10UfloatRenderable)
			? TextureFormat::RG11B10Ufloat : TextureFormat::RGBA16Float;
		if (const char *view = std::getenv("WGPU_GBUFFER_VIEW"))
		{
			w.compositeView = std::clamp(std::atoi(view), 0, gBufferViewCount - 1);
			std::cout << "WebGPU world: the composite shows " << gBufferViewLabels[w.compositeView] << "\n";
		}

		if (!createLayoutAndUniforms() || !createSkyResources() || !createPostResources() || !createBloomResources()
			|| !createHbaoResources() || !createRaysAndWarpResources() || !createPipelineSet())
		{
			std::cerr << "WebGPU world: could not create the chunk face pipeline\n";
			w.failed = true;
			return false;
		}

		w.drawMode = chooseDrawMode();
		w.initialized = true;
		std::cout << "WebGPU world pipeline created\n";
		return true;
	}
}

void setBlockGeometry(const std::vector<float> &vertexData, const std::vector<float> &uvData)
{
	w.blockGeometry = vertexData;
	w.blockUVs = uvData;
	w.blockGeometryDirty = true;
}

void setBlockTextures(const std::vector<uint64_t> &handles)
{
	w.textureHandles = handles;
	w.texturesDirty = true;
}

void setBlockCubeGeometry(const std::vector<float> &vertices)
{
	w.cubeGeometry = vertices;
	w.cubeGeometryDirty = true;
}

void renderBlockIcons(const std::vector<BlockIcon> &icons, int size)
{
	if (icons.empty() || size <= 0) { return; }
	if (!initialize() || !updateResources())
	{
		std::cerr << "WebGPU world: block icons need the block geometry and textures first\n";
		return;
	}
	if (!w.iconPipeline && !createIconPipeline()) { return; }
	bool remade = false;
	if (!ensureCubeVertices(remade)) { return; }
	if (remade && w.cubeBindGroup) { w.cubeBindGroup.release(); w.cubeBindGroup = nullptr; }

	ErrorScope scope("world block icons");

	// blockIcons.wgsl's Icon.
	struct IconRecord
	{
		glm::mat4 viewProjection;
		glm::uvec4 slotsA, slotsB, flags;
	};
	static_assert(sizeof(IconRecord) == 112, "must match blockIcons.wgsl's Icon");

	// glm::ortho gives GL's -1..1 depth; WebGPU clips to 0..1.
	glm::mat4 toZeroOne(1.f);
	toZeroOne[2][2] = 0.5f;
	toZeroOne[3][2] = 0.5f;

	std::vector<IconRecord> records(icons.size());
	for (size_t i = 0; i < icons.size(); i++)
	{
		records[i].viewProjection = toZeroOne * icons[i].viewProjection;
		records[i].slotsA = icons[i].slotsA;
		records[i].slotsB = icons[i].slotsB;
		records[i].flags = glm::uvec4(icons[i].oneTexture ? 1u : 0u, 0, 0, 0);
	}
	Buffer recordBuffer = makeStorageBuffer(records.data(), records.size() * sizeof(IconRecord), "world block icon records");

	BindGroupEntry entries[2];
	entries[0] = Default;
	entries[0].binding = 0;
	entries[0].buffer = w.cubeVertexBuffer;
	entries[0].size = w.cubeVertexBuffer.getSize();
	entries[1] = Default;
	entries[1].binding = 1;
	entries[1].buffer = recordBuffer;
	entries[1].size = recordBuffer ? recordBuffer.getSize() : 0;
	BindGroupDescriptor groupDesc = Default;
	groupDesc.label = StringView("world block icon bind group");
	groupDesc.layout = w.cubeBindGroupLayout;
	groupDesc.entryCount = 2;
	groupDesc.entries = entries;
	BindGroup group = recordBuffer ? wgpuDevice().createBindGroup(groupDesc) : BindGroup(nullptr);

	// One depth buffer serves every icon: each pass clears it.
	Texture depthTexture = createTarget("world block icon depth", depthFormat, TextureUsage::RenderAttachment, size, size);
	TextureView depthView = depthTexture ? createTargetView(depthTexture, "world block icon depth view", depthFormat,
		TextureAspect::DepthOnly, TextureUsage::RenderAttachment) : TextureView(nullptr);

	uint32_t drawn = 0;
	if (group && depthView)
	{
		CommandEncoderDescriptor encoderDesc = Default;
		encoderDesc.label = StringView("world block icons");
		CommandEncoder encoder = wgpuDevice().createCommandEncoder(encoderDesc);

		const uint32_t vertexTotal = (uint32_t)(w.cubeGeometry.size() / 8);
		for (uint32_t i = 0; i < icons.size(); i++)
		{
			const BlockIcon &icon = icons[i];
			TextureView target = wgpuTextureView(icon.target);
			if (!target || icon.vertexCount == 0 || icon.firstVertex + icon.vertexCount > vertexTotal) { continue; }

			RenderPassColorAttachment color = Default;
			color.view = target;
			color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
			color.resolveTarget = nullptr;
			color.loadOp = LoadOp::Clear;
			color.storeOp = StoreOp::Store;
			color.clearValue = Color{ 0.0, 0.0, 0.0, 0.0 };

			RenderPassDepthStencilAttachment depthAttachment = Default;
			depthAttachment.view = depthView;
			depthAttachment.depthLoadOp = LoadOp::Clear;
			depthAttachment.depthStoreOp = StoreOp::Discard;
			depthAttachment.depthClearValue = 1.f;
			depthAttachment.depthReadOnly = false;
			depthAttachment.stencilLoadOp = LoadOp::Undefined;
			depthAttachment.stencilStoreOp = StoreOp::Undefined;
			depthAttachment.stencilClearValue = 0;
			depthAttachment.stencilReadOnly = false;

			RenderPassDescriptor passDesc = Default;
			passDesc.label = StringView("world block icon pass");
			passDesc.colorAttachmentCount = 1;
			passDesc.colorAttachments = &color;
			passDesc.depthStencilAttachment = &depthAttachment;
			passDesc.occlusionQuerySet = nullptr;
			passDesc.timestampWrites = nullptr;
			RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
			pass.setPipeline(w.iconPipeline);
			pass.setBindGroup(0, w.bindGroup, 0, nullptr);
			pass.setBindGroup(1, group, 0, nullptr);
			pass.draw(icon.vertexCount, 1, icon.firstVertex, i);
			pass.end();
			pass.release();
			wgpuMarkTargetDrawn(icon.target); // this pass cleared it
			drawn++;
		}

		CommandBufferDescriptor commandDesc = Default;
		commandDesc.label = StringView("world block icons");
		CommandBuffer commands = encoder.finish(commandDesc);
		wgpuQueue().submit(1, &commands);
		commands.release();
		encoder.release();
	}

	if (depthView) { depthView.release(); }
	if (depthTexture) { depthTexture.release(); } // freed once the submitted passes are done
	if (group) { group.release(); }
	if (recordBuffer) { recordBuffer.release(); }
	std::cout << "WebGPU world: drew " << drawn << " of " << icons.size() << " block icons\n";
}

void setChunkFaces(const Chunk *chunk, const std::vector<int> &faces)
{
	if (!wgpuDevice()) { return; }
	placeFaces(w.chunks, chunk, faces);
}

void setChunkTransparentFaces(const Chunk *chunk, const std::vector<int> &faces)
{
	if (!wgpuDevice()) { return; }
	placeFaces(w.transparentChunks, chunk, faces);
}

void removeChunk(const Chunk *chunk)
{
	static const std::vector<int> none;
	placeFaces(w.chunks, chunk, none);
	placeFaces(w.transparentChunks, chunk, none);
	w.chunkLights.erase(chunk);
}

void setChunkLights(const Chunk *chunk, const std::vector<glm::ivec4> &lights)
{
	if (lights.empty()) { w.chunkLights.erase(chunk); }
	else { w.chunkLights[chunk] = lights; }
}

void render(Camera &camera, const std::vector<Chunk *> &visibleChunks, const std::vector<BlockCube> &blockCubes,
	const glm::vec3 &waterColor, bool waterPeel, bool depthPrepass, const Lighting &lighting)
{
	if (!initialize()) { return; }

	// Milestones 16 and 18: readings copied out in earlier frames.
	serviceReadbacks();

	// The point lights, as GL gathers them: the visible chunks' lists,
	// nearest chunk first (the list's end), until maxLights is reached
	// (the chunk that crosses it still counts whole; the shader reads at
	// most maxLights).
	w.frameLights.clear();
	if (lighting.useLights && lighting.maxLights > 0)
	{
		for (auto c = visibleChunks.rbegin(); c != visibleChunks.rend(); ++c)
		{
			auto it = w.chunkLights.find(*c);
			if (it == w.chunkLights.end()) { continue; }
			w.frameLights.insert(w.frameLights.end(), it->second.begin(), it->second.end());
			if ((int)w.frameLights.size() > lighting.maxLights) { break; }
		}
	}
	w.aoTextureId = lighting.aoTexture;
	w.waterTextureIds[0] = lighting.dudvTexture;
	w.waterTextureIds[1] = lighting.dudvNormalTexture;
	w.waterTextureIds[2] = lighting.causticsTexture;
	const int shadows = std::clamp(lighting.shadows, 0, 2);
	w.shadowsWanted = shadows != 0;

	if (!updateResources()) { return; }
	const bool drawShadows = shadows != 0 && w.shadowViews[0];

	int width = 0, height = 0;
	wgpuSurfaceSize(width, height);
	if (width <= 0 || height <= 0) { return; }
	if (!ensureTargets(width, height)) { return; }

	// The camera's own fov and planes, the surface's aspect (the ratio the
	// game puts in camera.aspectRatio), but the 0..1 depth range.
	// Camera::getProjectionMatrix stays -1..1: the OpenGL path and the CPU
	// frustum culling both use it.
	const float aspect = (float)width / (float)height;
	const glm::mat4 projection = glm::perspectiveRH_ZO(camera.fovRadians, aspect,
		camera.closePlane, camera.farPlane);

	// Camera-relative, as the GL renderer does: the shader subtracts the
	// integer part from each face's integer block position first, then the
	// fraction, so the floats stay small however far out the player is.
	glm::vec3 positionFloat = {};
	glm::ivec3 positionInt = {};
	camera.decomposePosition(positionFloat, positionInt);

	Uniforms uniforms;
	uniforms.viewProjection = projection * camera.getViewMatrix();
	uniforms.positionInt = glm::ivec4(positionInt, 0);
	uniforms.positionFloat = glm::vec4(positionFloat, 0.f);
	uniforms.waterColor = glm::vec4(waterColor, 1.f); // linear, as GL's
	uniforms.planes = glm::vec4(camera.closePlane, camera.farPlane, 0.f, lighting.exposure);
	uniforms.view = camera.getViewMatrix();
	uniforms.sunDirection = glm::vec4(lighting.sunDirection, lighting.baseAmbientExtra);
	uniforms.sunLightColor = glm::vec4(lighting.sunLightColor, 0.f);
	uniforms.ambientColor = glm::vec4(lighting.ambientColor, 0.f);
	uniforms.lightParams = glm::ivec4(lighting.skyLightIntensity,
		(int)std::min<size_t>(w.frameLights.size(), (size_t)std::max(lighting.maxLights, 0)),
		drawShadows ? shadows : 0, lighting.ssr ? 1 : 0);
	uniforms.projection = projection;
	uniforms.underWaterColor = glm::vec4(lighting.underWaterColor, lighting.underwaterDarkenStrength);
	uniforms.underWaterParams = glm::vec4(lighting.underwaterDarkenDistance, lighting.fogGradientUnderWater, 0.f, 0.f);
	uniforms.animation = glm::vec4(lighting.animationTime, lighting.waterMove, 0.f, 0.f);

	// Milestone 13: the sun's cascades, camera-relative. GL's matrices
	// take world positions to -1..1 depth; here the camera position is
	// folded in, in double, so the floats stay small however far out the
	// player is, and the depth goes to 0..1 by z / 2 + 1/2: the very
	// numbers GL's shader compares, so its biases still fit.
	glm::dmat4 depthToZeroOne(1.0);
	depthToZeroOne[2][2] = 0.5;
	depthToZeroOne[3][2] = 0.5;
	const glm::dmat4 fromCamera = glm::translate(glm::dmat4(1.0), camera.position);
	for (int i = 0; i < shadowCascades; i++)
	{
		uniforms.shadowMatrices[i] = glm::mat4(depthToZeroOne * lighting.shadowCascades[i] * fromCamera);
	}
	uniforms.fogParams = glm::vec4(lighting.fogDistance, lighting.fogGradient, lighting.underWater ? 1.f : 0.f, 0.f);
	wgpuQueue().writeBuffer(w.uniformBuffer, 0, &uniforms, sizeof(uniforms));

	// Milestone 14: the sky's uniforms. The sun at sunPos and the moon
	// opposite, at GL's sizes (renderSun's 0.9 and 0.6); the moon untinted.
	if (!ensureSkyBindGroup(lighting.sunTexture, lighting.moonTexture)) { return; }
	SkyUniforms sky;
	sky.inverseViewProjection = glm::inverse(projection * camera.getViewMatrix());
	sky.weights = glm::vec4(lighting.skyWeights, 0.f);
	sky.sunMatrix = sunMatrix(camera, lighting.sunPosition, 0.9f);
	sky.moonMatrix = sunMatrix(camera, -lighting.sunPosition, 0.6f);
	sky.sunParams = glm::vec4(lighting.sunTwilight, 1.f, 0.f, 0.f);
	sky.moonParams = glm::vec4(0.f, 1.f, 0.f, 0.f);
	// renderSun's second drawing, for the god rays.
	sky.raySunParams = glm::vec4(std::max(lighting.sunTwilight, 0.5f), 1.f, 0.f, 0.f);
	sky.rayMoonParams = glm::vec4(0.f, 0.35f, 0.f, 0.f);
	wgpuQueue().writeBuffer(w.skyUniformBuffer, 0, &sky, sizeof(sky));

	// Milestone 15: the post chain's settings, for the composite.
	PostUniforms post;
	post.grade = glm::vec4(lighting.exposure, lighting.saturation, lighting.vibrance, lighting.gamma);
	post.boost = glm::vec4(lighting.shadowBoost, lighting.highlightBoost, lighting.vignette, 0.f);
	post.lift = glm::vec4(lighting.lift, 0.f);
	post.gain = glm::vec4(lighting.gain, 0.f);
	post.mode = glm::ivec4(std::clamp(lighting.tonemapper, 0, 4), 0, 0, 0);
	wgpuQueue().writeBuffer(w.postUniformBuffer, 0, &post, sizeof(post));
	w.fxaa = lighting.fxaa;

	// Milestone 16: bloom's settings and lens dirt, for the composite.
	BloomUniforms bloom;
	bloom.params = glm::vec4(lighting.bloomExposure, std::max(lighting.bloomThreshold, 1e-6f),
		lighting.bloomMultiplier, 0.f);
	bloom.dirt = glm::vec4(lighting.hitLensDirt, lighting.waterDrops, 0.f, 0.f);
	wgpuQueue().writeBuffer(w.bloomUniformBuffer, 0, &bloom, sizeof(bloom));
	w.bloom = lighting.bloom;
	w.dirtTextures[0] = lighting.lensDirtTexture;
	w.dirtTextures[1] = lighting.hitDirtTexture;
	w.dirtTextures[2] = lighting.waterDirtTexture;

	// Milestone 17: the AO's matrices and fade distance.
	HbaoUniforms hbao;
	hbao.view = camera.getViewMatrix();
	hbao.projection = projection;
	hbao.params = glm::vec4(lighting.aoViewDistance, 0.f, 0.f, 0.f);
	wgpuQueue().writeBuffer(w.hbaoUniformBuffer, 0, &hbao, sizeof(hbao));
	w.ssao = lighting.ssao;

	// Milestone 18: where the sun is on screen (GL's sunPositionScreenSpaceUV,
	// from the sun's direction as a point; the projection's x, y and w rows
	// are GL's), and the warp's color and time.
	RayUniforms rayUniforms;
	const glm::vec4 sunClip = projection * camera.getViewMatrix() * glm::vec4(lighting.sunPosition, 1.f);
	rayUniforms.center = glm::vec4(glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f, 0.f, 0.f);
	wgpuQueue().writeBuffer(w.rayUniformBuffer, 0, &rayUniforms, sizeof(rayUniforms));
	WarpUniforms warpUniforms;
	warpUniforms.color = glm::vec4(lighting.underWaterColor, 0.f);
	warpUniforms.params = glm::vec4(lighting.animationTime, 0.f, 0.f, 0.f);
	wgpuQueue().writeBuffer(w.warpUniformBuffer, 0, &warpUniforms, sizeof(warpUniforms));
	w.underWater = lighting.underWater;

	const auto recordStart = std::chrono::steady_clock::now();

	// Every draw of the frame as argument records, in three runs:
	//   opaque: near to far (the renderer's list backwards, as GL's loop),
	//     so near chunks fill the depth buffer first;
	//   transparent, LOD 0 only: the water depth and peel passes (GL draws
	//     those with firstLod only);
	//   transparent, every LOD: the final pass.
	// The transparent runs go far to near, the list's own order, and each
	// chunk's faces were sorted far to near when baked, so blending sees
	// them back to front.
	w.drawArgs.clear();
	uint64_t facesDrawn = 0;
	for (auto c = visibleChunks.rbegin(); c != visibleChunks.rend(); ++c)
	{
		auto it = w.chunks.find(*c);
		if (it == w.chunks.end()) { continue; }
		w.drawArgs.push_back(DrawArgs{ 4, it->second.faceCount, 0, it->second.firstFace });
		facesDrawn += it->second.faceCount;
	}
	const uint32_t opaqueCount = (uint32_t)w.drawArgs.size();

	for (Chunk *c : visibleChunks)
	{
		auto it = w.transparentChunks.find(c);
		if (it == w.transparentChunks.end() || c->currentLod != 0) { continue; }
		w.drawArgs.push_back(DrawArgs{ 4, it->second.faceCount, 0, it->second.firstFace });
	}
	const uint32_t peelFirst = opaqueCount;
	const uint32_t peelCount = (uint32_t)w.drawArgs.size() - peelFirst;

	uint64_t transparentFacesDrawn = 0;
	for (Chunk *c : visibleChunks)
	{
		auto it = w.transparentChunks.find(c);
		if (it == w.transparentChunks.end()) { continue; }
		w.drawArgs.push_back(DrawArgs{ 4, it->second.faceCount, 0, it->second.firstFace });
		transparentFacesDrawn += it->second.faceCount;
	}
	const uint32_t finalFirst = peelFirst + peelCount;
	const uint32_t finalCount = (uint32_t)w.drawArgs.size() - finalFirst;

	// Then each shadow cascade's run: every loaded chunk's opaque faces
	// (not just the camera's visible ones: what is behind the camera can
	// shade what is in front), culled against that cascade's box, as GL's
	// renderShadow loops over loadedChunks.
	uint32_t shadowFirst[shadowCascades] = {};
	uint32_t shadowCount[shadowCascades] = {};
	for (int i = 0; i < shadowCascades && drawShadows; i++)
	{
		const FrustumVolume box(lighting.shadowCascades[i]);
		shadowFirst[i] = (uint32_t)w.drawArgs.size();
		uint64_t faces = 0;
		for (const auto &[chunk, range] : w.chunks)
		{
			if (const_cast<Chunk *>(chunk)->isDontDrawYet()) { continue; }
			AABBVolume bounds;
			bounds.minVertex = { chunk->data.x * CHUNK_SIZE, 0, chunk->data.z * CHUNK_SIZE };
			bounds.maxVertex = { (chunk->data.x + 1) * CHUNK_SIZE, CHUNK_HEIGHT + 1, (chunk->data.z + 1) * CHUNK_SIZE };
			if (!CheckFrustumVsAABB(box, bounds)) { continue; }
			w.drawArgs.push_back(DrawArgs{ 4, range.faceCount, 0, range.firstFace });
			faces += range.faceCount;
		}
		shadowCount[i] = (uint32_t)w.drawArgs.size() - shadowFirst[i];
		w.lastShadowFaces[i] = faces;
		w.lastShadowChunks[i] = shadowCount[i];
	}
	if (!drawShadows)
	{
		for (int i = 0; i < shadowCascades; i++) { w.lastShadowFaces[i] = 0; w.lastShadowChunks[i] = 0; }
	}

	const uint32_t drawCalls = opaqueCount * (depthPrepass ? 2 : 1) + (waterPeel ? 2 * peelCount : 0) + finalCount
		+ shadowCount[0] + shadowCount[1] + shadowCount[2];

	DrawMode mode = w.drawMode;
	if (mode != DrawMode::Direct && !w.drawArgs.empty())
	{
		if (ensureIndirectBuffer(w.drawArgs.size()))
		{
			// A queue write lands before the commands of any later submit,
			// this frame's included, and after every earlier one, so one
			// buffer serves every frame.
			wgpuQueue().writeBuffer(w.indirectBuffer, 0, w.drawArgs.data(), w.drawArgs.size() * sizeof(DrawArgs));
		}
		else { mode = DrawMode::Direct; }
	}

	// The opaque pass, into the G-buffer (milestone 11), clearing it and the
	// depth: GL's depth prepass and solid pass in one render pass, so the
	// depth stays on chip between them. The prepass writes depth only (every
	// color target masked, glColorMask 0); the solid pass then shades each
	// pixel once, where its depth is Equal to the nearest.
	{
		CommandEncoder encoder = wgpuFrameEncoder();
		if (!encoder) { return; }

		// The sun's shadow maps first (milestone 13): a pass per cascade
		// with no color attachment, only the map as depth, cleared to the
		// far plane. The stand-in bind group: the lit group binds the maps.
		const char *const shadowPassLabels[shadowCascades] =
			{ "world sun shadow pass 0", "world sun shadow pass 1", "world sun shadow pass 2" };
		for (int i = 0; i < shadowCascades && drawShadows; i++)
		{
			RenderPassDepthStencilAttachment shadowDepth = Default;
			shadowDepth.view = w.shadowViews[i];
			shadowDepth.depthLoadOp = LoadOp::Clear;
			shadowDepth.depthStoreOp = StoreOp::Store;
			shadowDepth.depthClearValue = depthClear;
			shadowDepth.depthReadOnly = false;
			shadowDepth.stencilLoadOp = LoadOp::Undefined;
			shadowDepth.stencilStoreOp = StoreOp::Undefined;
			shadowDepth.stencilClearValue = 0;
			shadowDepth.stencilReadOnly = false;

			RenderPassDescriptor shadowPassDesc = Default;
			shadowPassDesc.label = StringView(shadowPassLabels[i]);
			shadowPassDesc.colorAttachmentCount = 0;
			shadowPassDesc.colorAttachments = nullptr;
			shadowPassDesc.depthStencilAttachment = &shadowDepth;
			shadowPassDesc.occlusionQuerySet = nullptr;
			shadowPassDesc.timestampWrites = nullptr;
			RenderPassEncoder shadowPass = encoder.beginRenderPass(shadowPassDesc);
			if (w.arena && shadowCount[i])
			{
				shadowPass.setPipeline(w.shadowPipelines[i]);
				shadowPass.setBindGroup(0, w.shadowPassBindGroup, 0, nullptr);
				shadowPass.setVertexBuffer(0, w.arena, 0, w.arenaFaces * faceBytes);
				drawRecords(shadowPass, mode, shadowFirst[i], shadowCount[i]);
			}
			shadowPass.end();
			shadowPass.release();
		}

		// The sky (milestone 14): baked when first needed, then drawn into
		// the sky copy and the HDR color at once, before the world. The
		// opaque pass then clears every target but the color.
		bakeSkies(encoder, lighting);
		{
			RenderPassColorAttachment colors[2];
			TextureView targets[2] = { w.skyView, w.gBufferViews[GColor] };
			for (int i = 0; i < 2; i++)
			{
				colors[i] = Default;
				colors[i].view = targets[i];
				colors[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
				colors[i].resolveTarget = nullptr;
				colors[i].loadOp = LoadOp::Clear;
				colors[i].storeOp = StoreOp::Store;
				colors[i].clearValue = Color{ 0.0, 0.0, 0.0, 1.0 };
			}
			RenderPassDescriptor skyPassDesc = Default;
			skyPassDesc.label = StringView("world sky pass");
			skyPassDesc.colorAttachmentCount = 2;
			skyPassDesc.colorAttachments = colors;
			skyPassDesc.depthStencilAttachment = nullptr;
			skyPassDesc.occlusionQuerySet = nullptr;
			skyPassDesc.timestampWrites = nullptr;
			RenderPassEncoder skyPass = encoder.beginRenderPass(skyPassDesc);
			skyPass.setPipeline(w.skyPipeline);
			skyPass.setBindGroup(0, w.skyBindGroup, 0, nullptr);
			skyPass.draw(3, 1, 0, 0);
			skyPass.end();
			skyPass.release();
		}
		w.sunPending = true;

		RenderPassEncoder pass = beginGBufferPass(encoder, true, "world opaque pass", true);
		if (w.arena && opaqueCount)
		{
			if (depthPrepass)
			{
				bindFaces(pass, w.prepassPipeline, false);
				drawRecords(pass, mode, 0, opaqueCount);
			}
			bindFaces(pass, depthPrepass ? w.pipeline : w.solidLessPipeline, true);
			drawRecords(pass, mode, 0, opaqueCount);
		}

		// Dropped block items: opaque, so before the transparent passes (GL
		// draws entities between its solid pass and the depth copy).
		if (prepareCubes(blockCubes))
		{
			pass.setPipeline(w.cubePipeline);
			pass.setBindGroup(0, w.bindGroup, 0, nullptr);
			pass.setBindGroup(1, w.cubeBindGroup, 0, nullptr);
			pass.draw(36, (uint32_t)blockCubes.size(), 0, 0);
		}
		pass.end();
		pass.release();
	}

	// The transparent passes wait for renderTransparent, after the entities
	// (GL's order: solid pass, entities, depth copy, transparency), and
	// the composite after them.
	w.frameRendered = true;
	w.pendingTransparent = w.arena && finalCount;
	w.pending.mode = mode;
	w.pending.waterPeel = waterPeel;
	w.pending.peelFirst = peelFirst;
	w.pending.peelCount = peelCount;
	w.pending.finalFirst = finalFirst;
	w.pending.finalCount = finalCount;
	w.pending.width = width;
	w.pending.height = height;

	const auto recordEnd = std::chrono::steady_clock::now();
	w.renderedFrames++;
	if (w.renderedFrames > timingWarmupFrames)
	{
		w.timedFrames++;
		w.recordMsTotal += std::chrono::duration<double, std::milli>(recordEnd - recordStart).count();
		w.frameMsTotal += std::chrono::duration<double, std::milli>(recordStart - w.lastFrameStart).count();
	}
	w.lastFrameStart = recordStart;

	w.lastDrawCalls = drawCalls;
	w.lastFacesDrawn = facesDrawn;
	w.lastTransparentFacesDrawn = transparentFacesDrawn;
	w.lastVisibleChunks = visibleChunks.size();

	if (!w.firstRenderReported && drawCalls > 0)
	{
		w.firstRenderReported = true;
		std::cout << "WebGPU world first render: " << drawCalls << " chunks of " << visibleChunks.size()
			<< " visible, " << facesDrawn << " faces (" << w.chunks.size() << " chunks, "
			<< w.facesInUse << " faces in the arena)\n";
	}
}

void setMesh(unsigned int vao, const void *vertices, size_t vertexBytes,
	const void *indices, size_t indexCount, unsigned int indexBytes)
{
	if (!wgpuDevice() || !vao || !vertices || !vertexBytes) { return; }
	removeMesh(vao);

	auto upload = [](const void *data, uint64_t bytes, BufferUsage usage, const char *label)
	{
		BufferDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = usage | BufferUsage::CopyDst;
		desc.size = (bytes + 3) & ~uint64_t(3); // writeBuffer sizes are multiples of 4
		desc.mappedAtCreation = false;
		Buffer buffer = wgpuDevice().createBuffer(desc);
		if (buffer)
		{
			std::vector<uint8_t> padded((size_t)desc.size, 0);
			std::memcpy(padded.data(), data, (size_t)bytes);
			wgpuQueue().writeBuffer(buffer, 0, padded.data(), padded.size());
		}
		return buffer;
	};

	Mesh mesh;
	mesh.vertices = upload(vertices, vertexBytes, BufferUsage::Vertex, "world mesh vertices");
	if (indices && indexCount && (indexBytes == 2 || indexBytes == 4))
	{
		mesh.indices = upload(indices, indexCount * indexBytes, BufferUsage::Index, "world mesh indices");
		mesh.indexFormat = indexBytes == 2 ? IndexFormat::Uint16 : IndexFormat::Uint32;
		mesh.count = (uint32_t)indexCount;
	}
	else
	{
		// Unindexed: every vertex drawn in order. The vertex size is the
		// pipeline's to know; items are the only unindexed meshes.
		mesh.count = (uint32_t)(vertexBytes / (8 * sizeof(float)));
	}
	if (!mesh.vertices || (indices && indexCount && !mesh.indices)) { releaseMesh(mesh); return; }
	w.meshes[vao] = mesh;
}

void removeMesh(unsigned int vao)
{
	auto found = w.meshes.find(vao);
	if (found == w.meshes.end()) { return; }
	releaseMesh(found->second);
	w.meshes.erase(found);
}

void renderEntities(const std::vector<glm::mat4> &bones, const std::vector<ModelInstance> &models,
	const std::vector<ItemInstance> &items)
{
	if (!w.initialized || !w.bindGroup || !w.depthView || !w.frameRendered) { return; }
	if (models.empty() && items.empty()) { w.lastModelDraws = w.lastItemDraws = 0; return; }
	if (!ensureWhiteTexture()) { return; }

	// This frame's instances, in the form the shaders read.
	static std::vector<ModelGpu> modelData;
	static std::vector<ItemGpu> itemData;
	modelData.clear();
	itemData.clear();
	for (const ModelInstance &m : models)
	{
		modelData.push_back(ModelGpu{ glm::ivec4(m.positionInt, (int)m.boneBase),
			glm::vec4(m.positionFloat, 0.f), glm::vec4(m.brightness, 1.f) });
	}
	for (const ItemInstance &item : items)
	{
		itemData.push_back(ItemGpu{ item.model, glm::ivec4(item.positionInt, 0),
			glm::vec4(item.positionFloat, item.brightness) });
	}

	// Storage buffers can't be empty: at least one element each.
	bool grownBones = false, grownModels = false, grownItems = false;
	if (!ensureBuffer(w.boneBuffer, w.boneCapacity, std::max<size_t>(bones.size(), 1), sizeof(glm::mat4),
		BufferUsage::Storage, "world entity bones", grownBones)) { return; }
	if (!ensureBuffer(w.modelBuffer, w.modelCapacity, std::max<size_t>(modelData.size(), 1), sizeof(ModelGpu),
		BufferUsage::Storage, "world entity models", grownModels)) { return; }
	if (!ensureBuffer(w.itemBuffer, w.itemCapacity, std::max<size_t>(itemData.size(), 1), sizeof(ItemGpu),
		BufferUsage::Storage, "world entity items", grownItems)) { return; }
	if (!bones.empty()) { wgpuQueue().writeBuffer(w.boneBuffer, 0, bones.data(), bones.size() * sizeof(glm::mat4)); }
	if (!modelData.empty()) { wgpuQueue().writeBuffer(w.modelBuffer, 0, modelData.data(), modelData.size() * sizeof(ModelGpu)); }
	if (!itemData.empty()) { wgpuQueue().writeBuffer(w.itemBuffer, 0, itemData.data(), itemData.size() * sizeof(ItemGpu)); }

	if (grownBones || grownModels || grownItems || !w.entityBindGroup)
	{
		if (w.entityBindGroup) { w.entityBindGroup.release(); w.entityBindGroup = nullptr; }
		BindGroupEntry entries[3];
		Buffer buffers[3] = { w.boneBuffer, w.modelBuffer, w.itemBuffer };
		for (uint32_t i = 0; i < 3; i++)
		{
			entries[i] = Default;
			entries[i].binding = i;
			entries[i].buffer = buffers[i];
			entries[i].size = buffers[i].getSize();
		}
		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world entity bind group");
		groupDesc.layout = w.entityBindGroupLayout;
		groupDesc.entryCount = 3;
		groupDesc.entries = entries;
		w.entityBindGroup = wgpuDevice().createBindGroup(groupDesc);
		if (!w.entityBindGroup) { return; }
	}

	// One draw per instance: each has its own textures (one bind group per
	// combination), and firstInstance picks its record.
	// Into the G-buffer, as GL's entity shaders write fboMain.
	CommandEncoder encoder = wgpuFrameEncoder();
	if (!encoder) { return; }
	RenderPassEncoder pass = beginGBufferPass(encoder, false, "world entity pass");
	pass.setBindGroup(0, w.bindGroup, 0, nullptr);
	pass.setBindGroup(1, w.entityBindGroup, 0, nullptr);

	uint32_t draws = 0;
	auto drawMesh = [&](const Mesh &mesh, uint32_t instance)
	{
		pass.setVertexBuffer(0, mesh.vertices, 0, mesh.vertices.getSize());
		if (mesh.indices)
		{
			pass.setIndexBuffer(mesh.indices, mesh.indexFormat, 0, mesh.indices.getSize());
			pass.drawIndexed(mesh.count, 1, 0, 0, instance);
		}
		else
		{
			pass.draw(mesh.count, 1, 0, instance);
		}
		draws++;
	};

	pass.setPipeline(w.modelPipeline);
	for (uint32_t i = 0; i < models.size(); i++)
	{
		auto mesh = w.meshes.find(models[i].mesh);
		if (mesh == w.meshes.end() || !mesh->second.indices) { continue; }
		BindGroup textures = textureGroup(models[i].textures);
		if (!textures) { continue; }
		pass.setBindGroup(2, textures, 0, nullptr);
		drawMesh(mesh->second, i);
	}

	w.lastModelDraws = draws;
	pass.setPipeline(w.itemPipeline);
	for (uint32_t i = 0; i < items.size(); i++)
	{
		auto mesh = w.meshes.find(items[i].mesh);
		if (mesh == w.meshes.end()) { continue; }
		const uint32_t ids[4] = { items[i].texture, items[i].texture, items[i].texture, items[i].texture };
		BindGroup textures = textureGroup(ids);
		if (!textures) { continue; }
		pass.setBindGroup(2, textures, 0, nullptr);
		drawMesh(mesh->second, i);
	}

	pass.end();
	pass.release();
	w.lastItemDraws = draws - w.lastModelDraws;
}

void renderTransparent()
{
	// The sun and moon first: GL draws them after the entities and before
	// the transparent geometry, so glass and water show them.
	drawSunAndMoon();
	if (!w.pendingTransparent) { compositeFrame(); return; }
	w.pendingTransparent = false;

	const DrawMode mode = w.pending.mode;
	const bool waterPeel = w.pending.waterPeel;
	const uint32_t peelFirst = w.pending.peelFirst, peelCount = w.pending.peelCount;
	const uint32_t finalFirst = w.pending.finalFirst, finalCount = w.pending.finalCount;
	const int width = w.pending.width, height = w.pending.height;

	{
		CommandEncoder encoder = wgpuFrameEncoder();

		// Copy depth (GL's step 3): the water passes start from the opaque
		// depth, and the no-peel water reads it for its depth.
		copyWholeTexture(encoder, w.depthTexture, w.peelDepthTexture, width, height);

		if (waterPeel)
		{
			// Water into the peel depth (4): the nearest water surface.
			if (peelCount)
			{
				RenderPassDepthStencilAttachment depthAttachment = Default;
				depthAttachment.view = w.peelDepthView;
				depthAttachment.depthLoadOp = LoadOp::Load;
				depthAttachment.depthStoreOp = StoreOp::Store;
				depthAttachment.depthClearValue = depthClear;
				depthAttachment.depthReadOnly = false;
				depthAttachment.stencilLoadOp = LoadOp::Undefined;
				depthAttachment.stencilStoreOp = StoreOp::Undefined;
				depthAttachment.stencilClearValue = 0;
				depthAttachment.stencilReadOnly = false;

				RenderPassDescriptor passDesc = Default;
				passDesc.label = StringView("world water depth pass");
				passDesc.colorAttachmentCount = 0;
				passDesc.colorAttachments = nullptr;
				passDesc.depthStencilAttachment = &depthAttachment;
				passDesc.occlusionQuerySet = nullptr;
				passDesc.timestampWrites = nullptr;
				RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
				bindFaces(pass, w.waterDepthPipeline, false);
				drawRecords(pass, mode, peelFirst, peelCount);
				pass.end();
				pass.release();
			}

			// The peel (5): what lies beyond that surface, into the G-buffer.
			if (peelCount)
			{
				RenderPassEncoder pass = beginGBufferPass(encoder, false, "world peel pass");
				bindFaces(pass, w.peelPipeline, true);
				drawRecords(pass, mode, peelFirst, peelCount);
				pass.end();
				pass.release();
			}

			// Copy color and depth (6): what the water will show under it.
			// Always, so far-LOD water never reads an old frame.
			copyWholeTexture(encoder, w.gBufferTextures[GColor], w.peelColorTexture, width, height);
			copyWholeTexture(encoder, w.depthTexture, w.peelDepthTexture, width, height);
		}

		// The final transparency (7).
		{
			RenderPassEncoder pass = beginGBufferPass(encoder, false, "world transparent pass");
			bindFaces(pass, waterPeel ? w.finalPeelPipeline : w.finalPlainPipeline, true);
			drawRecords(pass, mode, finalFirst, finalCount);
			pass.end();
			pass.release();
		}
	}

	compositeFrame();
}

int sunVisibleSamples()
{
	return (int)w.sunSamples;
}

bool takeExposureColor(glm::vec3 &color)
{
	if (!w.exposureFresh) { return false; }
	w.exposureFresh = false;
	color = w.exposureColor;
	return true;
}

uint32_t gBufferPreview(int view)
{
	if (view < 0 || view >= gBufferViewCount) { return 0; }
	w.previewsWanted |= 1u << view;
	// Nothing until a frame has been drawn into it (the next one).
	return (w.previewsDrawn & (1u << view)) ? w.previews[view].texture.id : 0;
}

const char *gBufferViewName(int view)
{
	return (view >= 0 && view < gBufferViewCount) ? gBufferViewLabels[view] : "";
}

int gBufferViewCountTotal()
{
	return gBufferViewCount;
}

void renderDecal(const std::vector<int> &faces, int crackFrame, unsigned int crackTexture)
{
	if (!w.initialized || !w.bindGroup || !w.depthView) { return; }
	const uint32_t count = (uint32_t)(faces.size() * sizeof(int) / faceBytes);
	if (!count) { return; }

	TextureView crackView = wgpuTextureView(crackTexture);
	if (!crackView) { return; }

	bool grown = false;
	uint64_t capacity = w.decalFaceBuffer ? w.decalFaceBuffer.getSize() / faceBytes : 0;
	if (!ensureBuffer(w.decalFaceBuffer, capacity, count, faceBytes, BufferUsage::Vertex,
		"world decal faces", grown)) { return; }
	wgpuQueue().writeBuffer(w.decalFaceBuffer, 0, faces.data(), (uint64_t)count * faceBytes);

	if (!w.decalBindGroup || w.decalTextureId != crackTexture)
	{
		if (w.decalBindGroup) { w.decalBindGroup.release(); w.decalBindGroup = nullptr; }
		BindGroupEntry entry = Default;
		entry.binding = 2;
		entry.textureView = crackView;
		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world decal bind group");
		groupDesc.layout = w.decalBindGroupLayout;
		groupDesc.entryCount = 1;
		groupDesc.entries = &entry;
		w.decalBindGroup = wgpuDevice().createBindGroup(groupDesc);
		w.decalTextureId = crackTexture;
		if (!w.decalBindGroup) { return; }
	}

	// The frame rides in the uniforms' planes.z, which only fs_decal reads,
	// so writing it now changes nothing the world's passes see.
	const float frame = (float)crackFrame;
	wgpuQueue().writeBuffer(w.uniformBuffer, offsetof(Uniforms, planes) + 2 * sizeof(float), &frame, sizeof(frame));

	RenderPassEncoder pass = wgpuBeginSurfacePassWithDepth(w.depthView, false, depthClear, "world decal pass");
	if (!pass) { return; }
	pass.setPipeline(w.decalPipeline);
	pass.setBindGroup(0, w.bindGroup, 0, nullptr);
	pass.setBindGroup(1, w.decalBindGroup, 0, nullptr);
	pass.setVertexBuffer(0, w.decalFaceBuffer, 0, (uint64_t)count * faceBytes);
	pass.draw(4, count, 0, 0);
	pass.end();
	pass.release();
}

void renderLines(const std::vector<glm::vec3> &points)
{
	if (!w.initialized || !w.bindGroup || !w.depthView || points.size() < 2) { return; }
	const uint32_t count = (uint32_t)(points.size() & ~size_t(1)); // whole segments

	bool grown = false;
	if (!ensureBuffer(w.lineBuffer, w.lineCapacity, count, sizeof(glm::vec3), BufferUsage::Vertex,
		"world gizmo lines", grown)) { return; }
	wgpuQueue().writeBuffer(w.lineBuffer, 0, points.data(), (uint64_t)count * sizeof(glm::vec3));

	RenderPassEncoder pass = wgpuBeginSurfacePassWithDepth(w.depthView, false, depthClear, "world gizmo pass");
	if (!pass) { return; }
	pass.setPipeline(w.linePipeline);
	pass.setBindGroup(0, w.bindGroup, 0, nullptr);
	pass.setVertexBuffer(0, w.lineBuffer, 0, (uint64_t)count * sizeof(glm::vec3));
	pass.draw(count, 1, 0, 0);
	pass.end();
	pass.release();
}

bool reloadShaders()
{
	if (!w.initialized) { return false; }
	const auto start = std::chrono::steady_clock::now();

	// The current set aside, then a whole new one. Frames already recorded
	// hold their own references, so releasing either set is safe.
	std::vector<RenderPipeline> oldPipelines;
	std::vector<PipelineLayout> oldLayouts;
	forEachReloadable(
		[&](RenderPipeline &pipeline) { oldPipelines.push_back(pipeline); pipeline = nullptr; },
		[&](PipelineLayout &layout) { oldLayouts.push_back(layout); layout = nullptr; });

	const bool ok = createPipelineSet();

	// Keep the new set if all of it was made, else the old one (a WGSL error
	// has already been printed with its line and column).
	size_t nextPipeline = 0, nextLayout = 0;
	forEachReloadable(
		[&](RenderPipeline &pipeline)
		{
			RenderPipeline old = oldPipelines[nextPipeline++];
			RenderPipeline &loser = ok ? old : pipeline;
			if (loser) { loser.release(); }
			if (!ok) { pipeline = old; }
		},
		[&](PipelineLayout &layout)
		{
			PipelineLayout old = oldLayouts[nextLayout++];
			PipelineLayout &loser = ok ? old : layout;
			if (loser) { loser.release(); }
			if (!ok) { layout = old; }
		});

	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	if (ok)
	{
		w.skyBaked = false; // the bake reruns with the new sky.wgsl
		std::cout << "WebGPU world: reloaded the shaders (" << oldPipelines.size() << " pipelines) in " << ms << " ms\n";
	}
	else
	{
		std::cerr << "WebGPU world: shader reload failed, keeping the previous shaders\n";
	}
	return ok;
}

void shutdown()
{
	if (w.initialized)
	{
		std::cout << "WebGPU world last frame: " << w.lastDrawCalls << " draws of "
			<< w.lastVisibleChunks << " visible chunks, " << w.lastFacesDrawn << " opaque and "
			<< w.lastTransparentFacesDrawn << " transparent faces; arena "
			<< w.chunks.size() << " + " << w.transparentChunks.size() << " chunks, " << w.facesInUse << " of " << w.arenaFaces << " faces used, "
			<< w.freeRanges.size() << " free ranges; entities " << w.lastModelDraws << " model and "
			<< w.lastItemDraws << " item draws, "
			<< w.meshes.size() << " meshes; " << w.frameLights.size() << " point lights from "
			<< w.chunkLights.size() << " chunks with lights; sun shadow cascades "
			<< w.lastShadowChunks[0] << "/" << w.lastShadowChunks[1] << "/" << w.lastShadowChunks[2] << " chunks, "
			<< w.lastShadowFaces[0] << "/" << w.lastShadowFaces[1] << "/" << w.lastShadowFaces[2] << " faces; bloom "
			<< (w.bloom ? std::to_string(w.bloomMips + 1) + " levels" : std::string("off")) << "; "
			<< w.exposureReadings << " exposure readings, the last (" << w.exposureColor.x << ", "
			<< w.exposureColor.y << ", " << w.exposureColor.z << "); " << w.sunReadings
			<< " sun counts, the last " << w.sunSamples << " pixels\n";
		if (w.timedFrames)
		{
			std::cout << "WebGPU world timing (" << drawModeName(w.drawMode) << ", " << w.timedFrames
				<< " frames after " << timingWarmupFrames << "): recording the chunk draws "
				<< 1000.0 * w.recordMsTotal / w.timedFrames << " us, frame "
				<< w.frameMsTotal / w.timedFrames << " ms\n";
		}
	}

	releaseTargets();
	for (RenderPipeline *pipeline : { &w.pipeline, &w.waterDepthPipeline, &w.peelPipeline,
		&w.finalPeelPipeline, &w.finalPlainPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	if (w.transparentPipelineLayout) { w.transparentPipelineLayout.release(); w.transparentPipelineLayout = nullptr; }
	for (RenderPipeline *pipeline : { &w.prepassPipeline, &w.solidLessPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	for (RenderPipeline &pipeline : w.viewPipelines)
	{
		if (pipeline) { pipeline.release(); pipeline = nullptr; }
	}
	if (w.viewPipelineLayout) { w.viewPipelineLayout.release(); w.viewPipelineLayout = nullptr; }
	if (w.viewBindGroupLayout) { w.viewBindGroupLayout.release(); w.viewBindGroupLayout = nullptr; }
	for (wgpu2d::FrameBuffer &preview : w.previews) { preview.cleanup(); }
	if (w.lightBuffer) { w.lightBuffer.release(); w.lightBuffer = nullptr; w.lightCapacity = 0; }
	if (w.aoSampler) { w.aoSampler.release(); w.aoSampler = nullptr; }
	if (w.waterSampler) { w.waterSampler.release(); w.waterSampler = nullptr; }
	w.boundAoView = nullptr;
	for (TextureView &view : w.boundWaterViews) { view = nullptr; }
	for (int i = 0; i < shadowCascades; i++)
	{
		if (w.shadowPipelines[i]) { w.shadowPipelines[i].release(); w.shadowPipelines[i] = nullptr; }
		if (w.shadowViews[i]) { w.shadowViews[i].release(); w.shadowViews[i] = nullptr; }
		if (w.shadowTextures[i]) { w.shadowTextures[i].destroy(); w.shadowTextures[i].release(); w.shadowTextures[i] = nullptr; }
		w.boundShadowViews[i] = nullptr;
	}
	if (w.blankDepthView) { w.blankDepthView.release(); w.blankDepthView = nullptr; }
	if (w.blankDepthTexture) { w.blankDepthTexture.destroy(); w.blankDepthTexture.release(); w.blankDepthTexture = nullptr; }
	if (w.shadowSampler) { w.shadowSampler.release(); w.shadowSampler = nullptr; }
	if (w.shadowPassBindGroup) { w.shadowPassBindGroup.release(); w.shadowPassBindGroup = nullptr; }
	for (int k = 0; k < SkyKinds; k++)
	{
		for (TextureView &face : w.skyFaceViews[k]) { if (face) { face.release(); face = nullptr; } }
		if (w.skyCubeViews[k]) { w.skyCubeViews[k].release(); w.skyCubeViews[k] = nullptr; }
		if (w.skyCubes[k]) { w.skyCubes[k].destroy(); w.skyCubes[k].release(); w.skyCubes[k] = nullptr; }
	}
	for (RenderPipeline *pipeline : { &w.skyBakePipeline, &w.skyPipeline, &w.sunPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	for (BindGroup *group : { &w.skyBakeBindGroup, &w.skyBindGroup })
	{
		if (*group) { group->release(); *group = nullptr; }
	}
	for (PipelineLayout *layout : { &w.skyBakePipelineLayout, &w.skyPipelineLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (BindGroupLayout *layout : { &w.skyBakeBindGroupLayout, &w.skyBindGroupLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (Buffer *buffer : { &w.skyBakeBuffer, &w.skyUniformBuffer })
	{
		if (*buffer) { buffer->release(); *buffer = nullptr; }
	}
	if (w.skySampler) { w.skySampler.release(); w.skySampler = nullptr; }
	// Milestone 16. A reading still mapped is unmapped first; one being
	// mapped is aborted by the release (its callback only sets the state).
	if (w.exposureReadBuffer && w.exposureState == World::Readback::Mapped) { w.exposureReadBuffer.unmap(); }
	w.exposureState = World::Readback::Idle;
	for (RenderPipeline *pipeline : { &w.bloomFilterPipeline, &w.bloomDownPipeline, &w.bloomTentPipeline,
		&w.bloomUpPipeline, &w.bloomBlurPipelines[0], &w.bloomBlurPipelines[1], &w.bloomApplyPipeline,
		&w.exposurePipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	for (PipelineLayout *layout : { &w.bloomPipelineLayout, &w.bloomApplyPipelineLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (BindGroupLayout *layout : { &w.bloomBindGroupLayout, &w.bloomApplyBindGroupLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (Buffer *buffer : { &w.bloomUniformBuffer, &w.exposureReadBuffer })
	{
		if (*buffer) { buffer->release(); *buffer = nullptr; }
	}
	if (w.dirtSampler) { w.dirtSampler.release(); w.dirtSampler = nullptr; }
	// Milestone 17.
	for (RenderPipeline *pipeline : { &w.hbaoPipeline, &w.hbaoApplyPipeline, &w.lastColorPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	for (PipelineLayout *layout : { &w.hbaoPipelineLayout, &w.hbaoApplyPipelineLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (BindGroupLayout *layout : { &w.hbaoBindGroupLayout, &w.hbaoApplyBindGroupLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	if (w.hbaoUniformBuffer) { w.hbaoUniformBuffer.release(); w.hbaoUniformBuffer = nullptr; }
	// Milestone 18.
	if (w.sunReadBuffer && w.sunReadState == World::Readback::Mapped) { w.sunReadBuffer.unmap(); }
	w.sunReadState = World::Readback::Idle;
	for (RenderPipeline *pipeline : { &w.sunRaysPipeline, &w.rayMaskPipeline, &w.rayBlurPipeline, &w.warpPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	for (PipelineLayout *layout : { &w.sunRaysPipelineLayout, &w.rayMaskPipelineLayout, &w.rayBlurPipelineLayout,
		&w.warpPipelineLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (BindGroupLayout *layout : { &w.sunCountBindGroupLayout, &w.rayMaskBindGroupLayout, &w.rayBlurBindGroupLayout,
		&w.warpBindGroupLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (Buffer *buffer : { &w.rayUniformBuffer, &w.warpUniformBuffer, &w.sunCountBuffer, &w.sunReadBuffer })
	{
		if (*buffer) { buffer->release(); *buffer = nullptr; }
	}
	if (w.blackView) { w.blackView.release(); w.blackView = nullptr; }
	if (w.blackTexture) { w.blackTexture.destroy(); w.blackTexture.release(); w.blackTexture = nullptr; }
	for (RenderPipeline *pipeline : { &w.toneMapToTexturePipeline, &w.toneMapToSurfacePipeline, &w.fxaaPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	if (w.postPipelineLayout) { w.postPipelineLayout.release(); w.postPipelineLayout = nullptr; }
	if (w.postBindGroupLayout) { w.postBindGroupLayout.release(); w.postBindGroupLayout = nullptr; }
	if (w.postUniformBuffer) { w.postUniformBuffer.release(); w.postUniformBuffer = nullptr; }
	if (w.postSampler) { w.postSampler.release(); w.postSampler = nullptr; }
	w.boundSunView = nullptr;
	w.boundMoonView = nullptr;
	w.skyBaked = false;
	w.sunPending = false;
	w.chunkLights.clear();
	w.previewsDrawn = 0;
	for (RenderPipeline *pipeline : { &w.cubePipeline, &w.decalPipeline, &w.linePipeline, &w.iconPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	for (PipelineLayout *layout : { &w.cubePipelineLayout, &w.decalPipelineLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (BindGroup *group : { &w.cubeBindGroup, &w.decalBindGroup })
	{
		if (*group) { group->release(); *group = nullptr; }
	}
	for (Buffer *buffer : { &w.cubeVertexBuffer, &w.cubeInstanceBuffer, &w.decalFaceBuffer, &w.lineBuffer })
	{
		if (*buffer) { buffer->release(); *buffer = nullptr; }
	}
	for (BindGroupLayout *layout : { &w.cubeBindGroupLayout, &w.decalBindGroupLayout,
		&w.entityBindGroupLayout, &w.textureBindGroupLayout })
	{
		if (*layout) { layout->release(); *layout = nullptr; }
	}
	for (auto &mesh : w.meshes) { releaseMesh(mesh.second); }
	for (auto &group : w.textureGroups) { if (group.second) { group.second.release(); } }
	for (RenderPipeline *pipeline : { &w.modelPipeline, &w.itemPipeline })
	{
		if (*pipeline) { pipeline->release(); *pipeline = nullptr; }
	}
	if (w.entityPipelineLayout) { w.entityPipelineLayout.release(); w.entityPipelineLayout = nullptr; }
	if (w.entityBindGroup) { w.entityBindGroup.release(); w.entityBindGroup = nullptr; }
	for (Buffer *buffer : { &w.boneBuffer, &w.modelBuffer, &w.itemBuffer })
	{
		if (*buffer) { buffer->release(); *buffer = nullptr; }
	}
	if (w.whiteView) { w.whiteView.release(); w.whiteView = nullptr; }
	if (w.whiteTexture) { w.whiteTexture.destroy(); w.whiteTexture.release(); w.whiteTexture = nullptr; }
	if (w.peelBindGroupLayout) { w.peelBindGroupLayout.release(); w.peelBindGroupLayout = nullptr; }
	if (w.pipelineLayout) { w.pipelineLayout.release(); w.pipelineLayout = nullptr; }
	if (w.bindGroup) { w.bindGroup.release(); w.bindGroup = nullptr; }
	if (w.uniformBuffer) { w.uniformBuffer.release(); w.uniformBuffer = nullptr; }
	if (w.blockGeometryBuffer) { w.blockGeometryBuffer.release(); w.blockGeometryBuffer = nullptr; }
	if (w.blockUVBuffer) { w.blockUVBuffer.release(); w.blockUVBuffer = nullptr; }
	if (w.textureLayerBuffer) { w.textureLayerBuffer.release(); w.textureLayerBuffer = nullptr; }
	if (w.textureArrayView) { w.textureArrayView.release(); w.textureArrayView = nullptr; }
	if (w.textureArray) { w.textureArray.destroy(); w.textureArray.release(); w.textureArray = nullptr; }
	if (w.sampler) { w.sampler.release(); w.sampler = nullptr; }
	if (w.fillPipeline) { w.fillPipeline.release(); w.fillPipeline = nullptr; }
	if (w.downsamplePipeline) { w.downsamplePipeline.release(); w.downsamplePipeline = nullptr; }
	if (w.bindGroupLayout) { w.bindGroupLayout.release(); w.bindGroupLayout = nullptr; }
	if (w.arena) { w.arena.release(); w.arena = nullptr; }
	if (w.indirectBuffer) { w.indirectBuffer.release(); w.indirectBuffer = nullptr; }
	w = World{};
}

} // namespace wgpuWorld

#endif
