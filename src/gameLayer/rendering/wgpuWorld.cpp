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

#include "rendering/wgpuWorld.h"
#include "rendering/camera.h"
#include "chunk.h"

#include <render/wgpuFrame.h>
#include <webgpu/webgpu.hpp>

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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
		uint64_t facesInUse = 0;

		Texture depthTexture = nullptr;
		TextureView depthView = nullptr;
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

		BindGroupLayoutEntry entries[6];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].visibility = ShaderStage::Vertex;
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

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world bind group layout");
		layoutDesc.entryCount = 6;
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
		if (!changed && w.bindGroup) { return true; }
		if (!w.blockGeometryBuffer || !w.textureArrayView) { return false; }

		ErrorScope scope("world bind group");
		if (w.bindGroup) { w.bindGroup.release(); w.bindGroup = nullptr; }

		BindGroupEntry entries[6];
		for (int i = 0; i < 6; i++) { entries[i] = Default; entries[i].binding = i; }
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

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world bind group");
		groupDesc.layout = w.bindGroupLayout;
		groupDesc.entryCount = 6;
		groupDesc.entries = entries;
		w.bindGroup = wgpuDevice().createBindGroup(groupDesc);
		return w.bindGroup && !scope.failed();
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
	if (!updateResources()) { return; }

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

	const auto recordStart = std::chrono::steady_clock::now();

	// One record per visible chunk that has faces. The renderer's list is
	// frustum-culled and sorted far to near; walk it backwards, as the GL loop
	// does, so near chunks fill the depth buffer first and hide what is
	// behind them early. Records run in buffer order, so that holds for the
	// indirect paths too.
	w.drawArgs.clear();
	uint64_t facesDrawn = 0;
	for (auto c = visibleChunks.rbegin(); c != visibleChunks.rend(); ++c)
	{
		auto it = w.chunks.find(*c);
		if (it == w.chunks.end()) { continue; }
		w.drawArgs.push_back(DrawArgs{ 4, it->second.faceCount, 0, it->second.firstFace });
		facesDrawn += it->second.faceCount;
	}
	const uint32_t drawCalls = (uint32_t)w.drawArgs.size();

	if (w.arena && drawCalls)
	{
		pass.setPipeline(w.pipeline);
		pass.setBindGroup(0, w.bindGroup, 0, nullptr);
		pass.setVertexBuffer(0, w.arena, 0, w.arenaFaces * faceBytes);

		DrawMode mode = w.drawMode;
		if (mode != DrawMode::Direct && !ensureIndirectBuffer(drawCalls)) { mode = DrawMode::Direct; }

		if (mode == DrawMode::Direct)
		{
			for (const DrawArgs &a : w.drawArgs)
			{
				pass.draw(a.vertexCount, a.instanceCount, a.firstVertex, a.firstInstance);
			}
		}
		else
		{
			// A queue write lands before the commands of any later submit,
			// this frame's included, and after every earlier one, so one
			// buffer serves every frame.
			wgpuQueue().writeBuffer(w.indirectBuffer, 0, w.drawArgs.data(), drawCalls * sizeof(DrawArgs));
			if (mode == DrawMode::Multi)
			{
				pass.multiDrawIndirect(w.indirectBuffer, 0, drawCalls);
			}
			else
			{
				for (uint32_t i = 0; i < drawCalls; i++)
				{
					pass.drawIndirect(w.indirectBuffer, (uint64_t)i * sizeof(DrawArgs));
				}
			}
		}
	}

	pass.end();
	pass.release();

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
		if (w.timedFrames)
		{
			std::cout << "WebGPU world timing (" << drawModeName(w.drawMode) << ", " << w.timedFrames
				<< " frames after " << timingWarmupFrames << "): recording the chunk draws "
				<< 1000.0 * w.recordMsTotal / w.timedFrames << " us, frame "
				<< w.frameMsTotal / w.timedFrames << " ms\n";
		}
	}

	if (w.depthView) { w.depthView.release(); w.depthView = nullptr; }
	if (w.depthTexture) { w.depthTexture.destroy(); w.depthTexture.release(); w.depthTexture = nullptr; }
	if (w.pipeline) { w.pipeline.release(); w.pipeline = nullptr; }
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
