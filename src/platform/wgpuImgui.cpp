
// Milestone 4: the ImGui renderer backend, ported from space-game-wgpu (see
// include/platform/wgpuImgui.h for why it is not imgui_impl_wgpu). Its job is small and fixed: every frame
// ImGui::Render produces an ImDrawData, and this file turns that into one
// vertex buffer, one index buffer, and one draw call per ImDrawCmd with a
// scissor rectangle. Three things the sprite path never needed appear here:
// an index buffer, scissor rectangles, and a packed 8-bit color attribute.

#include "wgpuImgui.h"
#include <render/wgpuFrame.h>
#include <render/wgpu2d.h>

#include "imgui.h"

#include <webgpu/webgpu.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef> // offsetof
#include <cstdint>
#include <iostream>
#include <vector>

using namespace wgpu;
using namespace render; // wgpu2d's device, queue, frame and texture registry

namespace wgpuImgui
{

namespace
{
	// What the WGSL Uniforms struct reads: the viewport projection.
	struct Uniforms
	{
		glm::mat4 projection;
	};
	static_assert(sizeof(Uniforms) == 64, "uniform layout must match the WGSL Uniforms struct");

	struct ImguiBackend
	{
		bool initialized = false;

		RenderPipeline pipeline = nullptr;
		PipelineLayout pipelineLayout = nullptr;
		BindGroupLayout uniformBindGroupLayout = nullptr; // group 1: projection
		BindGroup uniformBindGroup = nullptr;
		Buffer uniformBuffer = nullptr;

		// Rewritten from the CPU every frame and grown (never shrunk) when a
		// frame needs more, like the sprite batch's vertex buffer.
		Buffer vertexBuffer = nullptr;
		uint64_t vertexCapacityBytes = 0;
		Buffer indexBuffer = nullptr;
		uint64_t indexCapacityBytes = 0;

		wgpu2d::Texture fontTexture;
		bool firstRenderReported = false;
	};

	ImguiBackend b;

	// ImGui's draw lists are separate allocations; the GPU wants one buffer.
	// Concatenating on the CPU also keeps every writeBuffer offset aligned.
	std::vector<ImDrawVert> cpuVertices;
	std::vector<ImDrawIdx> cpuIndices;

	// A GPU buffer cannot be resized: growth means a new, bigger one. Same
	// doubling rule as the sprite batch.
	bool ensureBufferCapacity(Buffer &buffer, uint64_t &capacity, uint64_t bytes,
		BufferUsage usage, const char *label)
	{
		if (buffer && bytes <= capacity) { return true; }

		// Past the early return, so this is a growth step, not a frame step:
		// the scope's stall happens a handful of times over a run, never in
		// the steady state.
		ErrorScope scope(label);

		uint64_t newCapacity = capacity ? capacity : 4096;
		while (newCapacity < bytes) { newCapacity *= 2; }

		BufferDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = usage | BufferUsage::CopyDst;
		desc.size = newCapacity;
		desc.mappedAtCreation = false;
		Buffer newBuffer = wgpuDevice().createBuffer(desc);
		if (!newBuffer)
		{
			std::cerr << "WebGPU imgui: createBuffer (" << label << ", " << newCapacity << " bytes) returned null\n";
			return false;
		}

		if (buffer) { buffer.release(); }
		buffer = newBuffer;
		capacity = newCapacity;
		std::cout << "WebGPU imgui " << label << " capacity: " << newCapacity << " bytes\n";
		std::cout.flush();
		return true;
	}

	// Group 1: one 64-byte uniform, vertex stage, no dynamic offset (unlike
	// the camera group, there is exactly one projection per frame).
	bool createUniformResources()
	{
		BindGroupLayoutEntry entry = Default;
		entry.binding = 0;
		entry.visibility = ShaderStage::Vertex;
		entry.buffer.type = BufferBindingType::Uniform;
		entry.buffer.hasDynamicOffset = false;
		entry.buffer.minBindingSize = sizeof(Uniforms);
		entry.sampler.type = SamplerBindingType::BindingNotUsed;
		entry.texture.sampleType = TextureSampleType::BindingNotUsed;
		entry.storageTexture.access = StorageTextureAccess::BindingNotUsed;

		ErrorScope scope("imgui uniform resources");

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("imgui uniform bind group layout");
		layoutDesc.entryCount = 1;
		layoutDesc.entries = &entry;
		b.uniformBindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		if (!b.uniformBindGroupLayout)
		{
			std::cerr << "WebGPU imgui: createBindGroupLayout returned null\n";
			return false;
		}

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("imgui projection");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(Uniforms);
		bufferDesc.mappedAtCreation = false;
		b.uniformBuffer = wgpuDevice().createBuffer(bufferDesc);
		if (!b.uniformBuffer)
		{
			std::cerr << "WebGPU imgui: createBuffer (projection) returned null\n";
			return false;
		}

		BindGroupEntry groupEntry = Default;
		groupEntry.binding = 0;
		groupEntry.buffer = b.uniformBuffer;
		groupEntry.offset = 0;
		groupEntry.size = sizeof(Uniforms);
		groupEntry.sampler = nullptr;
		groupEntry.textureView = nullptr;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("imgui uniform bind group");
		groupDesc.layout = b.uniformBindGroupLayout;
		groupDesc.entryCount = 1;
		groupDesc.entries = &groupEntry;
		b.uniformBindGroup = wgpuDevice().createBindGroup(groupDesc);
		if (!b.uniformBindGroup)
		{
			std::cerr << "WebGPU imgui: createBindGroup returned null\n";
			return false;
		}
		return true;
	}

	bool createPipeline()
	{
		// The shader module has its own scope inside wgpuCreateShaderModuleFromFile,
		// so this one starts after it and claims only the pipeline's own errors.
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/imgui.wgsl");
		if (!module) { return false; }

		ErrorScope scope("imgui pipeline");

		// The vertex layout must describe ImDrawVert exactly: ImGui owns that
		// struct, so the offsets come from offsetof rather than from us.
		VertexAttribute attributes[3];
		attributes[0] = Default;
		attributes[0].shaderLocation = 0;                 // @location(0) position
		attributes[0].format = VertexFormat::Float32x2;
		attributes[0].offset = offsetof(ImDrawVert, pos);
		attributes[1] = Default;
		attributes[1].shaderLocation = 1;                 // @location(1) uv
		attributes[1].format = VertexFormat::Float32x2;
		attributes[1].offset = offsetof(ImDrawVert, uv);
		attributes[2] = Default;
		attributes[2].shaderLocation = 2;                 // @location(2) color
		attributes[2].format = VertexFormat::Unorm8x4;    // one packed RGBA8 word -> vec4f in 0..1
		attributes[2].offset = offsetof(ImDrawVert, col);

		VertexBufferLayout vertexLayout = Default;
		vertexLayout.arrayStride = sizeof(ImDrawVert);
		vertexLayout.stepMode = VertexStepMode::Vertex;
		vertexLayout.attributeCount = 3;
		vertexLayout.attributes = attributes;

		RenderPipelineDescriptor desc = Default;
		desc.label = StringView("imgui pipeline");
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView("vs_main");
		desc.vertex.constantCount = 0;
		desc.vertex.constants = nullptr;
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &vertexLayout;

		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined;
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::None; // ImGui emits both windings
		desc.primitive.unclippedDepth = false;

		// Same blend as the sprites (and as imgui_impl_opengl3): straight,
		// non-premultiplied alpha.
		BlendState blend = Default;
		blend.color.operation = BlendOperation::Add;
		blend.color.srcFactor = BlendFactor::SrcAlpha;
		blend.color.dstFactor = BlendFactor::OneMinusSrcAlpha;
		blend.alpha.operation = BlendOperation::Add;
		blend.alpha.srcFactor = BlendFactor::One;
		blend.alpha.dstFactor = BlendFactor::OneMinusSrcAlpha;

		ColorTargetState colorTarget = Default;
		colorTarget.format = wgpuSurfaceFormat();
		colorTarget.blend = &blend;
		colorTarget.writeMask = ColorWriteMask::All;

		FragmentState fragment = Default;
		fragment.module = module;
		fragment.entryPoint = StringView("fs_main");
		fragment.constantCount = 0;
		fragment.constants = nullptr;
		fragment.targetCount = 1;
		fragment.targets = &colorTarget;
		desc.fragment = &fragment;

		desc.depthStencil = nullptr;
		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;

		// Group 0 is the sprite pipeline's texture layout, so a draw command
		// can bind the group the texture registry already holds. Group 1 is
		// this pipeline's projection.
		PipelineLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("imgui pipeline layout");
		WGPUBindGroupLayout layouts[2] = { wgpuTextureBindGroupLayout(), b.uniformBindGroupLayout };
		layoutDesc.bindGroupLayoutCount = 2;
		layoutDesc.bindGroupLayouts = layouts;
		b.pipelineLayout = wgpuDevice().createPipelineLayout(layoutDesc);
		if (!b.pipelineLayout)
		{
			std::cerr << "WebGPU imgui: createPipelineLayout returned null\n";
			module.release();
			return false;
		}
		desc.layout = b.pipelineLayout;

		b.pipeline = wgpuDevice().createRenderPipeline(desc);
		const bool invalid = scope.failed(); // rejected pipelines are non-null; see createQuadPipeline
		module.release();
		if (!b.pipeline || invalid)
		{
			std::cerr << "WebGPU imgui: pipeline is not usable\n";
			return false;
		}
		return true;
	}

	// The font atlas: ImGui rasterizes it on the CPU and hands over RGBA8
	// pixels. Uploaded through the same path as a sprite (linear filtering,
	// no mipmaps: the UI is drawn 1:1), which means its id is usable as an
	// ImTextureID and its bind group comes from the registry.
	bool createFontTexture()
	{
		ImGuiIO &io = ImGui::GetIO();
		unsigned char *pixels = nullptr;
		int width = 0, height = 0;
		io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
		if (!pixels || width <= 0 || height <= 0)
		{
			std::cerr << "WebGPU imgui: GetTexDataAsRGBA32 produced no atlas\n";
			return false;
		}

		// Top-first: ImGui's rows, not gl2d's bottom-first createFromBuffer.
		b.fontTexture.createFromBufferTopFirst((const char *)pixels, width, height, false, false);
		if (b.fontTexture.id == 0) { return false; }

		// ImTextureID is a void*; ids start at 1, so 0 stays "no texture".
		io.Fonts->SetTexID((ImTextureID)(intptr_t)b.fontTexture.id);
		std::cout << "WebGPU imgui font atlas: " << width << "x" << height
			<< " (texture id " << b.fontTexture.id << ")\n";
		std::cout.flush();
		return true;
	}

	// ImGui's orthographic projection, from screen points to clip space.
	// Left/top come from DisplayPos (0,0) here; the whole matrix is the CPU
	// half of what the camera matrix does for sprites.
	glm::mat4 buildProjection(const ImDrawData *drawData)
	{
		const float l = drawData->DisplayPos.x;
		const float r = drawData->DisplayPos.x + drawData->DisplaySize.x;
		const float t = drawData->DisplayPos.y;
		const float bo = drawData->DisplayPos.y + drawData->DisplaySize.y;

		glm::mat4 m(1.0f); // glm is column-major: m[col][row]
		m[0][0] = 2.0f / (r - l);
		m[1][1] = 2.0f / (t - bo); // y down in ImGui, y up in clip space
		m[3][0] = (r + l) / (l - r);
		m[3][1] = (t + bo) / (bo - t);
		return m;
	}
}

bool init()
{
	if (b.initialized) { return true; }
	if (!wgpuDevice())
	{
		std::cerr << "WebGPU imgui: no device (call wgpuInit first)\n";
		return false;
	}

	ImGuiIO &io = ImGui::GetIO();
	io.BackendRendererName = "wgpu2d";
	// We pass ImDrawCmd::VtxOffset as drawIndexed's baseVertex, so ImGui may
	// keep 16-bit indices in draw lists with more than 64k vertices.
	io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

	if (!createUniformResources() || !createPipeline() || !createFontTexture())
	{
		shutdown();
		return false;
	}

	b.initialized = true;
	std::cout << "WebGPU imgui pipeline created\n";
	std::cout.flush();
	return true;
}

void renderDrawData()
{
	if (!b.initialized) { return; }

	ImDrawData *drawData = ImGui::GetDrawData();
	if (!drawData || drawData->CmdListsCount == 0) { return; }
	if (drawData->TotalVtxCount <= 0 || drawData->TotalIdxCount <= 0) { return; }

	// Points times FramebufferScale = framebuffer pixels (2x on Retina).
	// Clip rectangles are the only thing in this file that works in pixels.
	int surfaceWidth = 0, surfaceHeight = 0;
	wgpuSurfaceSize(surfaceWidth, surfaceHeight);
	const float fbScaleX = drawData->FramebufferScale.x;
	const float fbScaleY = drawData->FramebufferScale.y;
	const float fbWidth = drawData->DisplaySize.x * fbScaleX;
	const float fbHeight = drawData->DisplaySize.y * fbScaleY;
	if (fbWidth <= 0 || fbHeight <= 0 || surfaceWidth <= 0 || surfaceHeight <= 0) { return; }

	// The UI goes on top of whatever the game drew, in the same pass.
	RenderPassEncoder pass = wgpuCurrentRenderPass();
	if (!pass) { return; }

	// Everything below is one group in a capture. It has to be a guard: the
	// buffer-capacity checks further down return early, and a push left
	// unmatched when the pass ends is a validation error.
	DebugGroup group(pass, "imgui");

	// One buffer out of every draw list, keeping their order.
	cpuVertices.clear();
	cpuIndices.clear();
	cpuVertices.reserve((size_t)drawData->TotalVtxCount);
	cpuIndices.reserve((size_t)drawData->TotalIdxCount + 1);
	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList *list = drawData->CmdLists[n];
		cpuVertices.insert(cpuVertices.end(), list->VtxBuffer.Data, list->VtxBuffer.Data + list->VtxBuffer.Size);
		cpuIndices.insert(cpuIndices.end(), list->IdxBuffer.Data, list->IdxBuffer.Data + list->IdxBuffer.Size);
	}
	// writeBuffer copies whole 4-byte words; an odd number of 16-bit indices
	// would be a half word. The pad index is never drawn.
	const uint64_t indexCount = cpuIndices.size();
	if (cpuIndices.size() % 2 != 0) { cpuIndices.push_back(0); }

	const uint64_t vertexBytes = cpuVertices.size() * sizeof(ImDrawVert);
	const uint64_t indexBytes = cpuIndices.size() * sizeof(ImDrawIdx);
	static_assert(sizeof(ImDrawVert) % 4 == 0, "vertex writeBuffer size must be a multiple of 4");
	static_assert(sizeof(ImDrawIdx) == 2, "index buffer is bound as Uint16");

	if (!ensureBufferCapacity(b.vertexBuffer, b.vertexCapacityBytes, vertexBytes, BufferUsage::Vertex, "vertices")) { return; }
	if (!ensureBufferCapacity(b.indexBuffer, b.indexCapacityBytes, indexBytes, BufferUsage::Index, "indices")) { return; }

	Queue queue = wgpuQueue();
	queue.writeBuffer(b.vertexBuffer, 0, cpuVertices.data(), vertexBytes);
	queue.writeBuffer(b.indexBuffer, 0, cpuIndices.data(), indexBytes);

	Uniforms uniforms;
	uniforms.projection = buildProjection(drawData);
	queue.writeBuffer(b.uniformBuffer, 0, &uniforms, sizeof(uniforms));

	pass.setPipeline(b.pipeline);
	pass.setVertexBuffer(0, b.vertexBuffer, 0, vertexBytes);
	pass.setIndexBuffer(b.indexBuffer, IndexFormat::Uint16, 0, indexBytes);
	pass.setBindGroup(1, b.uniformBindGroup, 0, nullptr);

	// Each draw command is a run of indices with one texture and one clip
	// rectangle. Vertices and indices were concatenated above, so a command's
	// own offsets are added to where its list starts.
	uint32_t globalVertexOffset = 0;
	uint32_t globalIndexOffset = 0;
	uint32_t drawCalls = 0;
	for (int n = 0; n < drawData->CmdListsCount; n++)
	{
		const ImDrawList *list = drawData->CmdLists[n];
		for (int c = 0; c < list->CmdBuffer.Size; c++)
		{
			const ImDrawCmd &cmd = list->CmdBuffer[c];

			// A widget can ask for arbitrary GPU work instead of geometry.
			// Nothing in this game does; handled so it is not silently wrong.
			if (cmd.UserCallback != nullptr)
			{
				if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
				{
					cmd.UserCallback(list, &cmd);
				}
				pass.setPipeline(b.pipeline);
				pass.setVertexBuffer(0, b.vertexBuffer, 0, vertexBytes);
				pass.setIndexBuffer(b.indexBuffer, IndexFormat::Uint16, 0, indexBytes);
				pass.setBindGroup(1, b.uniformBindGroup, 0, nullptr);
				continue;
			}

			// Clip rectangle: ImGui points relative to DisplayPos, scaled to
			// framebuffer pixels and clamped. wgpu rejects a scissor that
			// leaves the attachment, and an empty one is skipped entirely.
			float minX = (cmd.ClipRect.x - drawData->DisplayPos.x) * fbScaleX;
			float minY = (cmd.ClipRect.y - drawData->DisplayPos.y) * fbScaleY;
			float maxX = (cmd.ClipRect.z - drawData->DisplayPos.x) * fbScaleX;
			float maxY = (cmd.ClipRect.w - drawData->DisplayPos.y) * fbScaleY;
			minX = std::max(minX, 0.0f);
			minY = std::max(minY, 0.0f);
			maxX = std::min(maxX, (float)surfaceWidth);
			maxY = std::min(maxY, (float)surfaceHeight);
			if (maxX <= minX || maxY <= minY) { continue; }

			BindGroup textureGroup = wgpuTextureBindGroup((uint32_t)(intptr_t)cmd.GetTexID());
			if (!textureGroup) { continue; }

			pass.setScissorRect((uint32_t)minX, (uint32_t)minY,
				(uint32_t)(maxX - minX), (uint32_t)(maxY - minY));
			pass.setBindGroup(0, textureGroup, 0, nullptr);
			pass.drawIndexed(cmd.ElemCount, 1,
				globalIndexOffset + cmd.IdxOffset,
				(int32_t)(globalVertexOffset + cmd.VtxOffset), 0);
			drawCalls++;
		}
		globalVertexOffset += (uint32_t)list->VtxBuffer.Size;
		globalIndexOffset += (uint32_t)list->IdxBuffer.Size;
	}

	// The scissor is pass state, not draw state: leave it covering the whole
	// attachment for anything recorded after this.
	pass.setScissorRect(0, 0, (uint32_t)surfaceWidth, (uint32_t)surfaceHeight);

	if (!b.firstRenderReported)
	{
		b.firstRenderReported = true;
		std::cout << "WebGPU imgui first render: " << drawData->CmdListsCount << " draw lists, "
			<< cpuVertices.size() << " vertices, " << indexCount << " indices, "
			<< drawCalls << " draw calls\n";
		std::cout.flush();
	}
}

void shutdown()
{
	if (ImGui::GetCurrentContext())
	{
		ImGuiIO &io = ImGui::GetIO();
		if (io.Fonts) { io.Fonts->SetTexID(0); }
		io.BackendRendererName = nullptr;
	}
	b.fontTexture.cleanup();

	if (b.vertexBuffer) { b.vertexBuffer.release(); b.vertexBuffer = nullptr; b.vertexCapacityBytes = 0; }
	if (b.indexBuffer) { b.indexBuffer.release(); b.indexBuffer = nullptr; b.indexCapacityBytes = 0; }
	if (b.pipeline) { b.pipeline.release(); b.pipeline = nullptr; }
	if (b.pipelineLayout) { b.pipelineLayout.release(); b.pipelineLayout = nullptr; }
	if (b.uniformBindGroup) { b.uniformBindGroup.release(); b.uniformBindGroup = nullptr; }
	if (b.uniformBuffer) { b.uniformBuffer.release(); b.uniformBuffer = nullptr; }
	if (b.uniformBindGroupLayout) { b.uniformBindGroupLayout.release(); b.uniformBindGroupLayout = nullptr; }

	cpuVertices.clear();
	cpuIndices.clear();
	cpuVertices.shrink_to_fit();
	cpuIndices.shrink_to_fit();
	b.initialized = false;
	b.firstRenderReported = false;
}

} // namespace wgpuImgui


