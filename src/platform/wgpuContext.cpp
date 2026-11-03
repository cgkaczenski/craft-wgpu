#if RENDERER_WEBGPU

// The WebGPU-Cpp wrapper is header-only; exactly one source file must define
// this before including it.
#define WEBGPU_CPP_IMPLEMENTATION
#include <webgpu/webgpu.hpp>

#include "wgpuContext.h"

#include <cstdint>
#include <iostream>
#include <string>

namespace wgpuContext
{

static wgpu::Instance instance = nullptr;
static wgpu::Adapter adapter = nullptr;

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

bool init()
{
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

	// No surface yet (milestone 1b), so the adapter isn't tied to the window.
	wgpu::RequestAdapterOptions options = wgpu::Default;
	options.powerPreference = wgpu::PowerPreference::HighPerformance;

	// wgpu-native answers synchronously; the wrapper waits for the callback.
	adapter = instance.requestAdapter(options);
	if (!adapter)
	{
		std::cout << "WebGPU: no adapter found.\n";
		return false;
	}

	printAdapterReport();
	return true;
}

}

#endif
