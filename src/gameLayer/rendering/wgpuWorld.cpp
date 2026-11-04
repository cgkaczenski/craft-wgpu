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

#include "rendering/wgpuWorld.h"
#include "rendering/camera.h"
#include "chunk.h"

#include <render/wgpuFrame.h>
#include <webgpu/webgpu.hpp>

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
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
	// Must match the WGSL Uniforms struct in chunkFaces.wgsl: a mat4x4f, then
	// a vec4i and a vec4f, 96 bytes with no padding.
	struct Uniforms
	{
		glm::mat4 viewProjection;
		glm::ivec4 positionInt;
		glm::vec4 positionFloat;
	};
	static_assert(sizeof(Uniforms) == 96, "uniform layout must match chunkFaces.wgsl");

	// One face: short orientation, short texture, ivec3 position.
	constexpr uint64_t faceBytes = 4 * sizeof(int);

	// The depth convention: 0..1, near at 0, Less (docs/roadmap.md).
	constexpr TextureFormat depthFormat = TextureFormat::Depth32Float;
	constexpr float depthClear = 1.0f;

	// The arena starts at a million faces (16 MiB) and doubles when full.
	constexpr uint64_t initialArenaFaces = 1u << 20;

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
		BindGroup bindGroup = nullptr; // uniforms + corner table; remade when the table changes
		Buffer uniformBuffer = nullptr;

		// The corner table: kept on the CPU until it can be uploaded, and
		// uploaded again whenever the renderer rebuilds it.
		std::vector<float> blockGeometry;
		bool blockGeometryDirty = false;
		Buffer blockGeometryBuffer = nullptr;

		// The face arena and its allocator, in faces. `freeRanges` maps the
		// first face of each free range to its length, kept merged.
		Buffer arena = nullptr;
		uint64_t arenaFaces = 0;
		std::map<uint32_t, uint32_t> freeRanges;
		std::unordered_map<const Chunk *, ChunkRange> chunks;
		uint64_t facesInUse = 0;

		Texture depthTexture = nullptr;
		TextureView depthView = nullptr;
		int depthWidth = 0;
		int depthHeight = 0;

		bool firstRenderReported = false;

		// The last frame's numbers, printed at shutdown.
		uint32_t lastDrawCalls = 0;
		uint64_t lastFacesDrawn = 0;
		size_t lastVisibleChunks = 0;
	};

	World w;

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

		BindGroupLayoutEntry entries[2];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].visibility = ShaderStage::Vertex;
		entries[0].buffer.type = BufferBindingType::Uniform;
		entries[0].buffer.hasDynamicOffset = false;
		entries[0].buffer.minBindingSize = sizeof(Uniforms);
		entries[0].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[0].texture.sampleType = TextureSampleType::BindingNotUsed;
		entries[0].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		entries[1] = entries[0];
		entries[1].binding = 1;
		entries[1].buffer.type = BufferBindingType::ReadOnlyStorage;
		entries[1].buffer.minBindingSize = 0;

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world bind group layout");
		layoutDesc.entryCount = 2;
		layoutDesc.entries = entries;
		w.bindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		if (!w.bindGroupLayout) { return false; }

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(Uniforms);
		bufferDesc.mappedAtCreation = false;
		w.uniformBuffer = wgpuDevice().createBuffer(bufferDesc);
		return (bool)w.uniformBuffer && !scope.failed();
	}

	bool createPipeline()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/chunkFaces.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world chunk face pipeline");

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
		desc.label = StringView("world chunk face pipeline");
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView("vs_main");
		desc.vertex.constantCount = 0;
		desc.vertex.constants = nullptr;
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &faceLayout;

		// A strip per instance: 4 vertices make the face's two triangles.
		// Strips never join across instances. Culling as in GL's solid pass.
		desc.primitive.topology = PrimitiveTopology::TriangleStrip;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined; // no index buffer
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::Back;
		desc.primitive.unclippedDepth = false;

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
		desc.depthStencil = &depth;

		ColorTargetState colorTarget = Default;
		colorTarget.format = wgpuSurfaceFormat();
		colorTarget.blend = nullptr; // opaque
		colorTarget.writeMask = ColorWriteMask::All;

		FragmentState fragment = Default;
		fragment.module = module;
		fragment.entryPoint = StringView("fs_main");
		fragment.constantCount = 0;
		fragment.constants = nullptr;
		fragment.targetCount = 1;
		fragment.targets = &colorTarget;
		desc.fragment = &fragment;

		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;

		PipelineLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world pipeline layout");
		WGPUBindGroupLayout layouts[1] = { w.bindGroupLayout };
		layoutDesc.bindGroupLayoutCount = 1;
		layoutDesc.bindGroupLayouts = layouts;
		w.pipelineLayout = wgpuDevice().createPipelineLayout(layoutDesc);
		if (!w.pipelineLayout) { module.release(); return false; }
		desc.layout = w.pipelineLayout;

		w.pipeline = wgpuDevice().createRenderPipeline(desc);
		const bool invalid = scope.failed(); // a rejected pipeline can still be non-null
		module.release();
		return w.pipeline && !invalid;
	}

	// The corner table and the bind group that points at it. The bind group
	// names the buffer, so a new table means a new bind group too.
	bool uploadBlockGeometry()
	{
		if (!w.blockGeometryDirty && w.bindGroup) { return true; }
		if (w.blockGeometry.empty()) { return false; }

		ErrorScope scope("world corner table");

		if (w.bindGroup) { w.bindGroup.release(); w.bindGroup = nullptr; }
		if (w.blockGeometryBuffer) { w.blockGeometryBuffer.release(); w.blockGeometryBuffer = nullptr; }

		const uint64_t bytes = w.blockGeometry.size() * sizeof(float);
		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world corner table");
		bufferDesc.usage = BufferUsage::Storage | BufferUsage::CopyDst;
		bufferDesc.size = bytes;
		bufferDesc.mappedAtCreation = false;
		w.blockGeometryBuffer = wgpuDevice().createBuffer(bufferDesc);
		if (!w.blockGeometryBuffer) { return false; }
		wgpuQueue().writeBuffer(w.blockGeometryBuffer, 0, w.blockGeometry.data(), bytes);

		BindGroupEntry entries[2];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].buffer = w.uniformBuffer;
		entries[0].offset = 0;
		entries[0].size = sizeof(Uniforms);
		entries[1] = Default;
		entries[1].binding = 1;
		entries[1].buffer = w.blockGeometryBuffer;
		entries[1].offset = 0;
		entries[1].size = bytes;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world bind group");
		groupDesc.layout = w.bindGroupLayout;
		groupDesc.entryCount = 2;
		groupDesc.entries = entries;
		w.bindGroup = wgpuDevice().createBindGroup(groupDesc);
		if (!w.bindGroup || scope.failed()) { return false; }

		w.blockGeometryDirty = false;
		std::cout << "WebGPU world corner table: " << w.blockGeometry.size() / 12
			<< " orientations (" << bytes << " bytes)\n";
		return true;
	}

	// The depth texture has to match the color attachment's size exactly, so
	// it follows the surface. RenderAttachment only: nothing samples it yet.
	bool ensureDepthTexture(int width, int height)
	{
		if (w.depthTexture && w.depthWidth == width && w.depthHeight == height) { return true; }

		if (w.depthView) { w.depthView.release(); w.depthView = nullptr; }
		if (w.depthTexture) { w.depthTexture.destroy(); w.depthTexture.release(); w.depthTexture = nullptr; }

		ErrorScope scope("world depth texture");

		TextureDescriptor desc = Default;
		desc.label = StringView("world depth");
		desc.usage = TextureUsage::RenderAttachment;
		desc.dimension = TextureDimension::_2D;
		desc.size.width = (uint32_t)width;
		desc.size.height = (uint32_t)height;
		desc.size.depthOrArrayLayers = 1;
		desc.format = depthFormat;
		desc.mipLevelCount = 1;
		desc.sampleCount = 1;
		desc.viewFormatCount = 0;
		desc.viewFormats = nullptr;
		w.depthTexture = wgpuDevice().createTexture(desc);
		if (!w.depthTexture) { return false; }

		TextureViewDescriptor viewDesc = Default;
		viewDesc.label = StringView("world depth view");
		viewDesc.format = depthFormat;
		viewDesc.dimension = TextureViewDimension::_2D;
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = 1;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect = TextureAspect::DepthOnly;
		viewDesc.usage = TextureUsage::RenderAttachment;
		w.depthView = w.depthTexture.createView(viewDesc);
		if (!w.depthView || scope.failed()) { return false; }

		w.depthWidth = width;
		w.depthHeight = height;
		std::cout << "WebGPU world depth texture: " << width << "x" << height << "\n";
		return true;
	}

	bool initialize()
	{
		if (w.initialized) { return true; }
		if (w.failed || !wgpuDevice()) { return false; }

		if (!createLayoutAndUniforms() || !createPipeline())
		{
			std::cerr << "WebGPU world: could not create the chunk face pipeline\n";
			w.failed = true;
			return false;
		}

		w.initialized = true;
		std::cout << "WebGPU world pipeline created\n";
		return true;
	}
}

void setBlockGeometry(const std::vector<float> &vertexData)
{
	w.blockGeometry = vertexData;
	w.blockGeometryDirty = true;
}

void setChunkFaces(const Chunk *chunk, const std::vector<int> &faces)
{
	if (!wgpuDevice()) { return; }

	removeChunk(chunk);

	const uint32_t count = (uint32_t)(faces.size() * sizeof(int) / faceBytes);
	if (count == 0) { return; }

	uint32_t first = 0;
	while (!allocateRange(count, first))
	{
		if (!growArena(w.arenaFaces + count)) { return; }
	}

	wgpuQueue().writeBuffer(w.arena, (uint64_t)first * faceBytes, faces.data(), (uint64_t)count * faceBytes);
	w.chunks[chunk] = ChunkRange{ first, count };
	w.facesInUse += count;
}

void removeChunk(const Chunk *chunk)
{
	auto it = w.chunks.find(chunk);
	if (it == w.chunks.end()) { return; }
	freeRange(it->second.firstFace, it->second.faceCount);
	w.facesInUse -= it->second.faceCount;
	w.chunks.erase(it);
}

void render(Camera &camera, const std::vector<Chunk *> &visibleChunks)
{
	if (!initialize()) { return; }
	if (!uploadBlockGeometry()) { return; }

	int width = 0, height = 0;
	wgpuSurfaceSize(width, height);
	if (width <= 0 || height <= 0) { return; }
	if (!ensureDepthTexture(width, height)) { return; }

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
	wgpuQueue().writeBuffer(w.uniformBuffer, 0, &uniforms, sizeof(uniforms));

	RenderPassEncoder pass = wgpuBeginSurfacePassWithDepth(w.depthView, depthClear, "world pass");
	if (!pass) { return; }

	uint32_t drawCalls = 0;
	uint64_t facesDrawn = 0;
	if (w.arena)
	{
		pass.setPipeline(w.pipeline);
		pass.setBindGroup(0, w.bindGroup, 0, nullptr);
		pass.setVertexBuffer(0, w.arena, 0, w.arenaFaces * faceBytes);

		// The renderer's list is frustum-culled and sorted far to near; walk
		// it backwards, as the GL loop does, so near chunks fill the depth
		// buffer first and hide what is behind them early.
		for (auto c = visibleChunks.rbegin(); c != visibleChunks.rend(); ++c)
		{
			auto it = w.chunks.find(*c);
			if (it == w.chunks.end()) { continue; }
			pass.draw(4, it->second.faceCount, 0, it->second.firstFace);
			drawCalls++;
			facesDrawn += it->second.faceCount;
		}
	}

	pass.end();
	pass.release();

	w.lastDrawCalls = drawCalls;
	w.lastFacesDrawn = facesDrawn;
	w.lastVisibleChunks = visibleChunks.size();

	if (!w.firstRenderReported && drawCalls > 0)
	{
		w.firstRenderReported = true;
		std::cout << "WebGPU world first render: " << drawCalls << " chunks of " << visibleChunks.size()
			<< " visible, " << facesDrawn << " faces (" << w.chunks.size() << " chunks, "
			<< w.facesInUse << " faces in the arena)\n";
	}
}

void shutdown()
{
	if (w.initialized)
	{
		std::cout << "WebGPU world last frame: " << w.lastDrawCalls << " draws of "
			<< w.lastVisibleChunks << " visible chunks, " << w.lastFacesDrawn << " faces; arena "
			<< w.chunks.size() << " chunks, " << w.facesInUse << " of " << w.arenaFaces << " faces used, "
			<< w.freeRanges.size() << " free ranges\n";
	}

	if (w.depthView) { w.depthView.release(); w.depthView = nullptr; }
	if (w.depthTexture) { w.depthTexture.destroy(); w.depthTexture.release(); w.depthTexture = nullptr; }
	if (w.pipeline) { w.pipeline.release(); w.pipeline = nullptr; }
	if (w.pipelineLayout) { w.pipelineLayout.release(); w.pipelineLayout = nullptr; }
	if (w.bindGroup) { w.bindGroup.release(); w.bindGroup = nullptr; }
	if (w.uniformBuffer) { w.uniformBuffer.release(); w.uniformBuffer = nullptr; }
	if (w.blockGeometryBuffer) { w.blockGeometryBuffer.release(); w.blockGeometryBuffer = nullptr; }
	if (w.bindGroupLayout) { w.bindGroupLayout.release(); w.bindGroupLayout = nullptr; }
	if (w.arena) { w.arena.release(); w.arena = nullptr; }
	w = World{};
}

} // namespace wgpuWorld

#endif
