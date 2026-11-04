#if RENDERER_WEBGPU

// Milestone 5: the first 3D pass. See include/gameLayer/rendering/wgpuWorld.h.
//
// What is new compared with the 2D layer:
//   - a depth texture, cleared to 1 (far) each frame and tested Less, so the
//     nearest surface wins whatever order things are drawn in;
//   - a perspective projection with WebGPU's 0..1 depth range
//     (perspectiveRH_ZO), built from the game's own Camera fields;
//   - a uniform buffer the vertex shader reads, rewritten every frame;
//   - back-face culling.

#include "rendering/wgpuWorld.h"
#include "rendering/camera.h"

#include <render/wgpuFrame.h>
#include <webgpu/webgpu.hpp>

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>

#include <cmath>
#include <iostream>

using namespace wgpu;
using namespace render; // wgpu2d's device, queue and frame

namespace wgpuWorld
{

namespace
{
	// Must match the WGSL Uniforms struct in testCubes.wgsl. A mat4x4f is 64
	// bytes and an array<vec4f, 2> another 32, with no padding between.
	struct Uniforms
	{
		glm::mat4 viewProjection;
		glm::vec4 cubeOffsets[2];
	};
	static_assert(sizeof(Uniforms) == 96, "uniform layout must match testCubes.wgsl");

	// The depth format and clear value are the depth convention: 0..1, near
	// at 0, Less (docs/roadmap.md, "Conventions").
	constexpr TextureFormat depthFormat = TextureFormat::Depth32Float;
	constexpr float depthClear = 1.0f;

	struct World
	{
		bool initialized = false;
		bool failed = false; // creation failed once; don't retry every frame

		RenderPipeline pipeline = nullptr;
		PipelineLayout pipelineLayout = nullptr;
		BindGroupLayout bindGroupLayout = nullptr;
		BindGroup bindGroup = nullptr;
		Buffer uniformBuffer = nullptr;

		// Sized to the surface, remade when the surface changes size.
		Texture depthTexture = nullptr;
		TextureView depthView = nullptr;
		int depthWidth = 0;
		int depthHeight = 0;

		// Where the test cubes sit: fixed in the world, so moving the camera
		// shows them in 3D, and placed again in front of the camera when it
		// gets far away (the player spawns in the air and falls).
		bool anchorChosen = false;
		glm::dvec3 anchor = {};

		bool firstRenderReported = false;
	};

	World w;

	bool createUniformResources()
	{
		ErrorScope scope("world uniform resources");

		BindGroupLayoutEntry entry = Default;
		entry.binding = 0;
		entry.visibility = ShaderStage::Vertex;
		entry.buffer.type = BufferBindingType::Uniform;
		entry.buffer.hasDynamicOffset = false;
		entry.buffer.minBindingSize = sizeof(Uniforms);
		entry.sampler.type = SamplerBindingType::BindingNotUsed;
		entry.texture.sampleType = TextureSampleType::BindingNotUsed;
		entry.storageTexture.access = StorageTextureAccess::BindingNotUsed;

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("world uniform bind group layout");
		layoutDesc.entryCount = 1;
		layoutDesc.entries = &entry;
		w.bindGroupLayout = wgpuDevice().createBindGroupLayout(layoutDesc);
		if (!w.bindGroupLayout) { return false; }

		BufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("world uniforms");
		bufferDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		bufferDesc.size = sizeof(Uniforms);
		bufferDesc.mappedAtCreation = false;
		w.uniformBuffer = wgpuDevice().createBuffer(bufferDesc);
		if (!w.uniformBuffer) { return false; }

		BindGroupEntry groupEntry = Default;
		groupEntry.binding = 0;
		groupEntry.buffer = w.uniformBuffer;
		groupEntry.offset = 0;
		groupEntry.size = sizeof(Uniforms);
		groupEntry.sampler = nullptr;
		groupEntry.textureView = nullptr;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("world uniform bind group");
		groupDesc.layout = w.bindGroupLayout;
		groupDesc.entryCount = 1;
		groupDesc.entries = &groupEntry;
		w.bindGroup = wgpuDevice().createBindGroup(groupDesc);
		return (bool)w.bindGroup && !scope.failed();
	}

	bool createPipeline()
	{
		ShaderModule module = wgpuCreateShaderModuleFromFile(RESOURCES_PATH "shaders/webgpu/testCubes.wgsl");
		if (!module) { return false; }

		ErrorScope scope("world test cube pipeline");

		RenderPipelineDescriptor desc = Default;
		desc.label = StringView("world test cube pipeline");

		// No vertex buffers: the shader makes its corners from vertex_index.
		desc.vertex.module = module;
		desc.vertex.entryPoint = StringView("vs_main");
		desc.vertex.constantCount = 0;
		desc.vertex.constants = nullptr;
		desc.vertex.bufferCount = 0;
		desc.vertex.buffers = nullptr;

		// Counter-clockwise is the front, as in OpenGL: clip-space y points up
		// in WebGPU too, so a projection made for GL keeps its winding.
		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined;
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::Back;
		desc.primitive.unclippedDepth = false;

		// The depth test. In OpenGL these were glEnable(GL_DEPTH_TEST),
		// glDepthFunc and glDepthMask, set before any draw; here they are part
		// of the pipeline and fixed when it is created.
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
		layoutDesc.label = StringView("world test cube pipeline layout");
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

		if (!createUniformResources() || !createPipeline())
		{
			std::cerr << "WebGPU world: could not create the test cube pipeline\n";
			shutdown();
			w.failed = true;
			return false;
		}

		w.initialized = true;
		std::cout << "WebGPU world pipeline created\n";
		return true;
	}
}

void render(Camera &camera)
{
	if (!initialize()) { return; }

	int width = 0, height = 0;
	wgpuSurfaceSize(width, height);
	if (width <= 0 || height <= 0) { return; }
	if (!ensureDepthTexture(width, height)) { return; }

	// Four blocks ahead of the camera, on the block grid.
	if (!w.anchorChosen || glm::length(w.anchor - camera.position) > 16.0)
	{
		w.anchorChosen = true;
		w.anchor = glm::floor(camera.position + glm::dvec3(camera.viewDirection) * 4.0);
		std::cout << "WebGPU world test cubes at " << w.anchor.x << ", " << w.anchor.y << ", "
			<< w.anchor.z << "\n";
	}

	// The raised cube first: from a camera above, as at spawn, it is the
	// nearer one, so the second cube drawn is the farther and only the depth
	// test keeps the first in front where they overlap.
	const glm::dvec3 offsets[2] = { w.anchor + glm::dvec3(0.5, 0.5, 0.5), w.anchor };

	// Camera-relative, like the GL renderer's u_positionInt + u_positionFloat:
	// the subtraction happens in doubles, so the floats the GPU gets stay
	// small and precise however far the player is from the origin.
	Uniforms uniforms;
	for (int i = 0; i < 2; i++)
	{
		uniforms.cubeOffsets[i] = glm::vec4(glm::vec3(offsets[i] - camera.position), 0.f);
	}

	// The camera's own fov and planes, the surface's aspect (the ratio the
	// game puts in camera.aspectRatio), but the 0..1 depth range.
	// Camera::getProjectionMatrix stays -1..1: the OpenGL path and the CPU
	// frustum culling both use it.
	const float aspect = (float)width / (float)height;
	const glm::mat4 projection = glm::perspectiveRH_ZO(camera.fovRadians, aspect,
		camera.closePlane, camera.farPlane);
	uniforms.viewProjection = projection * camera.getViewMatrix();

	wgpuQueue().writeBuffer(w.uniformBuffer, 0, &uniforms, sizeof(uniforms));

	RenderPassEncoder pass = wgpuBeginSurfacePassWithDepth(w.depthView, depthClear, "world pass");
	if (!pass) { return; }
	pass.setPipeline(w.pipeline);
	pass.setBindGroup(0, w.bindGroup, 0, nullptr);
	pass.draw(36, 2, 0, 0);
	pass.end();
	pass.release();

	if (!w.firstRenderReported)
	{
		w.firstRenderReported = true;
		std::cout << "WebGPU world first render: 2 cubes, 72 vertices\n";
	}
}

void shutdown()
{
	if (w.depthView) { w.depthView.release(); w.depthView = nullptr; }
	if (w.depthTexture) { w.depthTexture.destroy(); w.depthTexture.release(); w.depthTexture = nullptr; }
	if (w.pipeline) { w.pipeline.release(); w.pipeline = nullptr; }
	if (w.pipelineLayout) { w.pipelineLayout.release(); w.pipelineLayout = nullptr; }
	if (w.bindGroup) { w.bindGroup.release(); w.bindGroup = nullptr; }
	if (w.uniformBuffer) { w.uniformBuffer.release(); w.uniformBuffer = nullptr; }
	if (w.bindGroupLayout) { w.bindGroupLayout.release(); w.bindGroupLayout = nullptr; }
	w.depthWidth = w.depthHeight = 0;
	w.initialized = false;
	w.firstRenderReported = false;
}

} // namespace wgpuWorld

#endif
