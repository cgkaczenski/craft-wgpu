#if RENDERER_WEBGPU

// The WebGPU-Cpp wrapper is header-only; exactly one source file must define
// this before including it.
#define WEBGPU_CPP_IMPLEMENTATION
#include <webgpu/webgpu.hpp>

#include "wgpuContext.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glfw3webgpu.h>

#include <cstdint>
#include <iostream>
#include <string>

namespace wgpuContext
{

static GLFWwindow *window = nullptr;
static wgpu::Instance instance = nullptr;
static wgpu::Surface surface = nullptr;
static wgpu::Adapter adapter = nullptr;
static wgpu::Device device = nullptr;
static wgpu::Queue queue = nullptr;

static wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::Undefined;
static int configuredWidth = 0;
static int configuredHeight = 0;

static const char *backendName(wgpu::BackendType type)
{
	switch (type)
	{
	case wgpu::BackendType::Null: return "Null";
	case wgpu::BackendType::WebGPU: return "WebGPU";
	case wgpu::BackendType::D3D11: return "D3D11";
	case wgpu::BackendType::D3D12: return "D3D12";
	case wgpu::BackendType::Metal: return "Metal";
	case wgpu::BackendType::Vulkan: return "Vulkan";
	case wgpu::BackendType::OpenGL: return "OpenGL";
	case wgpu::BackendType::OpenGLES: return "OpenGLES";
	default: return "Undefined";
	}
}

static const char *adapterTypeName(wgpu::AdapterType type)
{
	switch (type)
	{
	case wgpu::AdapterType::DiscreteGPU: return "Discrete GPU";
	case wgpu::AdapterType::IntegratedGPU: return "Integrated GPU";
	case wgpu::AdapterType::CPU: return "CPU";
	default: return "Unknown";
	}
}

struct FeatureEntry
{
	WGPUFeatureName feature;
	const char *name;
	const char *whyWeCare; // nullptr if the port doesn't plan to use it
};

#define STANDARD(Name, why) { wgpu::FeatureName::Name, #Name, why }
#define NATIVE(Name, why) { static_cast<WGPUFeatureName>(wgpu::NativeFeature::Name), #Name, why }

// Every feature the vendored webgpu.hpp (wgpu-native v24.0.3.1) knows about.
// The ones with a reason are the ones docs/roadmap.md depends on.
static const FeatureEntry features[] =
{
	STANDARD(DepthClipControl, nullptr),
	STANDARD(Depth32FloatStencil8, nullptr),
	STANDARD(TimestampQuery, "GPU profiler timers (profiler.cpp)"),
	STANDARD(TextureCompressionBC, nullptr),
	STANDARD(TextureCompressionBCSliced3D, nullptr),
	STANDARD(TextureCompressionETC2, nullptr),
	STANDARD(TextureCompressionASTC, nullptr),
	STANDARD(TextureCompressionASTCSliced3D, nullptr),
	STANDARD(IndirectFirstInstance, "firstInstance in indirect chunk draws (milestone 8)"),
	STANDARD(ShaderF16, nullptr),
	STANDARD(RG11B10UfloatRenderable, "rg11b10ufloat HDR/bloom targets, else rgba16float (milestone 11)"),
	STANDARD(BGRA8UnormStorage, nullptr),
	STANDARD(Float32Filterable, nullptr),
	STANDARD(Float32Blendable, nullptr),
	STANDARD(ClipDistances, nullptr),
	STANDARD(DualSourceBlending, nullptr),

	NATIVE(PushConstants, nullptr),
	NATIVE(TextureAdapterSpecificFormatFeatures, nullptr),
	NATIVE(MultiDrawIndirect, "one call for all opaque chunks (milestone 8)"),
	NATIVE(MultiDrawIndirectCount, nullptr),
	NATIVE(VertexWritableStorage, nullptr),
	NATIVE(TextureBindingArray, "binding_array of block textures instead of bindless (milestone 7)"),
	NATIVE(SampledTextureAndStorageBufferArrayNonUniformIndexing, "per-face index into the texture binding array (milestone 7)"),
	NATIVE(PipelineStatisticsQuery, nullptr),
	NATIVE(StorageResourceBindingArray, nullptr),
	NATIVE(PartiallyBoundBindingArray, "binding array with unused slots (milestone 7)"),
	NATIVE(TextureFormat16bitNorm, nullptr),
	NATIVE(TextureCompressionAstcHdr, nullptr),
	NATIVE(MappablePrimaryBuffers, nullptr),
	NATIVE(BufferBindingArray, nullptr),
	NATIVE(UniformBufferAndStorageTextureArrayNonUniformIndexing, nullptr),
	NATIVE(SpirvShaderPassthrough, nullptr),
	NATIVE(VertexAttribute64bit, nullptr),
	NATIVE(TextureFormatNv12, nullptr),
	NATIVE(RayTracingAccelerationStructure, nullptr),
	NATIVE(RayQuery, nullptr),
	NATIVE(ShaderF64, nullptr),
	NATIVE(ShaderI16, nullptr),
	NATIVE(ShaderPrimitiveIndex, nullptr),
	NATIVE(ShaderEarlyDepthTest, nullptr),
	NATIVE(Subgroup, nullptr),
	NATIVE(SubgroupVertex, nullptr),
	NATIVE(SubgroupBarrier, nullptr),
	NATIVE(TimestampQueryInsideEncoders, "timers around individual passes (profiler.cpp)"),
	NATIVE(TimestampQueryInsidePasses, nullptr),
};

#undef STANDARD
#undef NATIVE

// wgpu-native v24 leaves some adapter limits at WGPU_LIMIT_U32_UNDEFINED
// (seen with maxColorAttachments). That means "not reported", not "unlimited".
static std::string limitText(uint32_t value)
{
	return value == WGPU_LIMIT_U32_UNDEFINED ? "not reported" : std::to_string(value);
}

static void printAdapterReport()
{
	std::cout << "\n=== WebGPU adapter ===\n";

	wgpu::AdapterInfo info = wgpu::Default;
	if (adapter.getInfo(&info) == wgpu::Status::Success)
	{
		std::cout << "Backend:      " << backendName(info.backendType) << "\n";
		std::cout << "Vendor:       " << wgpu::StringView(info.vendor) << "\n";
		std::cout << "Architecture: " << wgpu::StringView(info.architecture) << "\n";
		std::cout << "Device:       " << wgpu::StringView(info.device) << "\n";
		std::cout << "Description:  " << wgpu::StringView(info.description) << "\n";
		std::cout << "Type:         " << adapterTypeName(info.adapterType) << "\n";
		std::cout << "IDs:          vendor 0x" << std::hex << info.vendorID
			<< ", device 0x" << info.deviceID << std::dec << "\n";
		info.freeMembers();
	}
	else
	{
		std::cout << "wgpuAdapterGetInfo failed\n";
	}

	std::cout << "\nFeatures the port depends on:\n";
	for (const FeatureEntry &f : features)
	{
		if (!f.whyWeCare) { continue; }
		bool has = adapter.hasFeature(f.feature);
		std::cout << "  [" << (has ? "yes" : " no") << "] " << f.name << " - " << f.whyWeCare << "\n";
	}

	std::cout << "\nOther supported features:\n";
	for (const FeatureEntry &f : features)
	{
		if (f.whyWeCare) { continue; }
		if (adapter.hasFeature(f.feature)) { std::cout << "  " << f.name << "\n"; }
	}

	wgpu::Limits limits = wgpu::Default;
	if (adapter.getLimits(&limits) == wgpu::Status::Success)
	{
		std::cout << "\nLimits the port depends on:\n";
		std::cout << "  maxColorAttachments              " << limitText(limits.maxColorAttachments) << " (fboMain uses 5)\n";
		std::cout << "  maxColorAttachmentBytesPerSample " << limitText(limits.maxColorAttachmentBytesPerSample) << " (G-buffer budget, milestone 11)\n";
		std::cout << "  maxTextureDimension2D            " << limits.maxTextureDimension2D << "\n";
		std::cout << "  maxTextureArrayLayers            " << limits.maxTextureArrayLayers << " (block textures per array, milestone 7)\n";
		std::cout << "  maxSampledTexturesPerShaderStage " << limits.maxSampledTexturesPerShaderStage << "\n";
		std::cout << "  maxStorageBuffersPerShaderStage  " << limits.maxStorageBuffersPerShaderStage << "\n";
		std::cout << "  maxStorageBufferBindingSize      " << limits.maxStorageBufferBindingSize << " (chunk face arena, milestone 6)\n";
		std::cout << "  maxBufferSize                    " << limits.maxBufferSize << "\n";
		std::cout << "  maxBindGroups                    " << limits.maxBindGroups << "\n";
	}
	else
	{
		std::cout << "wgpuAdapterGetLimits failed\n";
	}

	std::cout << "======================\n\n";
}

static const char *errorTypeName(WGPUErrorType type)
{
	switch (type)
	{
	case WGPUErrorType_Validation: return "Validation";
	case WGPUErrorType_OutOfMemory: return "OutOfMemory";
	case WGPUErrorType_Internal: return "Internal";
	default: return "Unknown";
	}
}

// The swapchain is plain unorm, not sRGB: like the OpenGL path (no
// GL_FRAMEBUFFER_SRGB), shaders write gamma-encoded values themselves
// (postprocess/toneMap.frag), and gl2d colors are already sRGB.
static wgpu::TextureFormat chooseSurfaceFormat()
{
	wgpu::SurfaceCapabilities caps = wgpu::Default;
	if (surface.getCapabilities(adapter, &caps) != wgpu::Status::Success || caps.formatCount == 0)
	{
		std::cout << "WebGPU: could not read surface capabilities.\n";
		return wgpu::TextureFormat::Undefined;
	}

	wgpu::TextureFormat chosen = caps.formats[0];
	for (size_t i = 0; i < caps.formatCount; i++)
	{
		if (caps.formats[i] == wgpu::TextureFormat::BGRA8Unorm) { chosen = caps.formats[i]; break; }
		if (caps.formats[i] == wgpu::TextureFormat::RGBA8Unorm) { chosen = caps.formats[i]; }
	}

	caps.freeMembers();
	return chosen;
}

// (Re)configures the swapchain at the framebuffer size, which is in pixels
// (twice the window size on Retina displays).
static void configureSurface(int width, int height)
{
	wgpu::SurfaceConfiguration config = wgpu::Default;
	config.device = device;
	config.format = surfaceFormat;
	config.usage = wgpu::TextureUsage::RenderAttachment;
	config.width = static_cast<uint32_t>(width);
	config.height = static_cast<uint32_t>(height);
	config.alphaMode = wgpu::CompositeAlphaMode::Auto;
	config.presentMode = wgpu::PresentMode::Fifo; // vsync; the only mode every surface supports
	surface.configure(config);

	configuredWidth = width;
	configuredHeight = height;
}

bool init(GLFWwindow *glfwWindow)
{
	window = glfwWindow;

	// wgpuGetVersion() (wgpu.h) packs major.minor.patch.build into one byte each.
	uint32_t v = wgpuGetVersion();
	std::cout << "WebGPU: wgpu-native " << ((v >> 24) & 0xff) << "." << ((v >> 16) & 0xff)
		<< "." << ((v >> 8) & 0xff) << "." << (v & 0xff) << "\n";

	instance = wgpu::createInstance();
	if (!instance)
	{
		std::cout << "WebGPU: could not create an instance.\n";
		return false;
	}

	// On macOS this attaches a CAMetalLayer to the window's content view.
	surface = glfwCreateWindowWGPUSurface(instance, window);
	if (!surface)
	{
		std::cout << "WebGPU: could not create a surface for the window.\n";
		return false;
	}
	pinMetalLayerToSRGB(window);

	wgpu::RequestAdapterOptions options = wgpu::Default;
	options.powerPreference = wgpu::PowerPreference::HighPerformance;
	options.compatibleSurface = surface;

	// wgpu-native answers synchronously; the wrapper waits for the callback.
	adapter = instance.requestAdapter(options);
	if (!adapter)
	{
		std::cout << "WebGPU: no adapter found.\n";
		return false;
	}

	printAdapterReport();

	// No optional features or raised limits yet; later milestones add the ones
	// in the report (texture binding arrays, multi-draw indirect, ...).
	wgpu::DeviceDescriptor deviceDesc = wgpu::Default;
	deviceDesc.label = wgpu::StringView("ourCraft device");
	deviceDesc.defaultQueue.label = wgpu::StringView("ourCraft queue");

	deviceDesc.deviceLostCallbackInfo.mode = wgpu::CallbackMode::AllowSpontaneous;
	deviceDesc.deviceLostCallbackInfo.callback = [](WGPUDevice const *, WGPUDeviceLostReason reason,
		WGPUStringView message, void *, void *)
	{
		// Destroyed is the normal shutdown path; anything else is a real loss.
		if (reason == WGPUDeviceLostReason_Destroyed) { return; }
		std::cout << "WebGPU device lost (reason " << reason << "): " << wgpu::StringView(message) << "\n";
	};

	// Validation errors land here instead of crashing. Keep this on: it is how
	// most WebGPU mistakes show up.
	deviceDesc.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const *, WGPUErrorType type,
		WGPUStringView message, void *, void *)
	{
		std::cout << "WebGPU " << errorTypeName(type) << " error: " << wgpu::StringView(message) << "\n";
	};

	device = adapter.requestDevice(deviceDesc);
	if (!device)
	{
		std::cout << "WebGPU: could not create a device.\n";
		return false;
	}
	queue = device.getQueue();

	surfaceFormat = chooseSurfaceFormat();
	if (surfaceFormat == wgpu::TextureFormat::Undefined) { return false; }

	int width = 0, height = 0;
	glfwGetFramebufferSize(window, &width, &height);
	configureSurface(width, height);

	std::cout << "WebGPU: surface configured, format 0x" << std::hex
		<< static_cast<uint32_t>(surfaceFormat) << std::dec
		<< ", " << width << "x" << height << "\n";

	return true;
}

void clearAndPresent(float r, float g, float b)
{
	int width = 0, height = 0;
	glfwGetFramebufferSize(window, &width, &height);

	// Minimized: there is nothing to draw into, and a 0x0 configure is invalid.
	if (width == 0 || height == 0) { return; }

	if (width != configuredWidth || height != configuredHeight)
	{
		configureSurface(width, height);
	}

	wgpu::SurfaceTexture surfaceTexture;
	surface.getCurrentTexture(&surfaceTexture);

	switch (surfaceTexture.status)
	{
	case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
	case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
		break;

	case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
	case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
	case wgpu::SurfaceGetCurrentTextureStatus::Lost:
		// The window changed under us. Reconfigure and skip this frame.
		if (surfaceTexture.texture) { wgpuTextureRelease(surfaceTexture.texture); }
		configureSurface(width, height);
		return;

	default:
		std::cout << "WebGPU: could not get the surface texture (status "
			<< surfaceTexture.status << ").\n";
		if (surfaceTexture.texture) { wgpuTextureRelease(surfaceTexture.texture); }
		return;
	}

	wgpu::Texture texture = surfaceTexture.texture;
	wgpu::TextureView view = texture.createView();

	wgpu::CommandEncoder encoder = device.createCommandEncoder();

	wgpu::RenderPassColorAttachment colorAttachment = wgpu::Default;
	colorAttachment.view = view;
	colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED; // 2D target, not a 3D texture slice
	colorAttachment.loadOp = wgpu::LoadOp::Clear;
	colorAttachment.storeOp = wgpu::StoreOp::Store;
	colorAttachment.clearValue = WGPUColor{r, g, b, 1.0};

	wgpu::RenderPassDescriptor passDesc = wgpu::Default;
	passDesc.label = wgpu::StringView("clear pass");
	passDesc.colorAttachmentCount = 1;
	passDesc.colorAttachments = &colorAttachment;

	// An empty pass: the clear happens when the pass begins (loadOp = Clear).
	wgpu::RenderPassEncoder pass = encoder.beginRenderPass(passDesc);
	pass.end();
	pass.release();

	wgpu::CommandBuffer commands = encoder.finish();
	encoder.release();

	queue.submit(1, &commands);
	commands.release();

	surface.present();

	view.release();
	texture.release();
}

void shutdown()
{
	if (surface && configuredWidth) { surface.unconfigure(); }
	if (queue) { queue.release(); queue = nullptr; }
	if (device) { device.release(); device = nullptr; }
	if (adapter) { adapter.release(); adapter = nullptr; }
	if (surface) { surface.release(); surface = nullptr; }
	if (instance) { instance.release(); instance = nullptr; }
	configuredWidth = configuredHeight = 0;
}

#ifndef __APPLE__
void pinMetalLayerToSRGB(GLFWwindow *) {}
#endif

}

#endif
