#if RENDERER_WEBGPU

// wgpu2d's webgpuImpl.cpp defines WEBGPU_CPP_IMPLEMENTATION; this file only
// uses the wrapper.
#include <webgpu/webgpu.hpp>
#include <render/wgpuContext.h>
#include <render/wgpu2d.h>

#include "wgpuWindow.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glfw3webgpu.h>

#include <stb_image_write/stb_image_write.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace wgpuWindow
{

static GLFWwindow *window = nullptr;
static WGPUSurface surface = nullptr;

struct FeatureEntry
{
	WGPUFeatureName feature;
	const char *name;
	const char *whyWeCare;
};

#define STANDARD(Name, why) { wgpu::FeatureName::Name, #Name, why }
#define NATIVE(Name, why) { static_cast<WGPUFeatureName>(wgpu::NativeFeature::Name), #Name, why }

// The features docs/roadmap.md depends on. wgpu2d prints the adapter and
// everything it supports; this says which of those matter for the port.
static const FeatureEntry portFeatures[] =
{
	STANDARD(TimestampQuery, "GPU profiler timers (profiler.cpp)"),
	STANDARD(IndirectFirstInstance, "firstInstance in indirect chunk draws (milestone 8)"),
	STANDARD(RG11B10UfloatRenderable, "rg11b10ufloat HDR/bloom targets, else rgba16float (milestone 11)"),
	NATIVE(MultiDrawIndirect, "one call for all opaque chunks (milestone 8)"),
	NATIVE(TextureBindingArray, "binding_array of block textures instead of bindless (milestone 7)"),
	NATIVE(SampledTextureAndStorageBufferArrayNonUniformIndexing, "per-face index into the texture binding array (milestone 7)"),
	NATIVE(PartiallyBoundBindingArray, "binding array with unused slots (milestone 7)"),
	NATIVE(TimestampQueryInsideEncoders, "timers around individual passes (profiler.cpp)"),
};

#undef STANDARD
#undef NATIVE

// wgpu-native v24 leaves some adapter limits at WGPU_LIMIT_U32_UNDEFINED
// (seen with maxColorAttachments). That means "not reported", not "unlimited".
static std::string limitText(uint32_t value)
{
	return value == WGPU_LIMIT_U32_UNDEFINED ? "not reported" : std::to_string(value);
}

static void printPortReport(wgpu::Adapter adapter)
{
	std::cout << "\n=== What the port needs from this adapter ===\n";

	for (const FeatureEntry &f : portFeatures)
	{
		bool has = adapter.hasFeature(f.feature);
		std::cout << "  [" << (has ? "yes" : " no") << "] " << f.name << " - " << f.whyWeCare << "\n";
	}

	wgpu::Limits limits = wgpu::Default;
	if (adapter.getLimits(&limits) == wgpu::Status::Success)
	{
		std::cout << "  maxColorAttachments              " << limitText(limits.maxColorAttachments) << " (fboMain uses 5)\n";
		std::cout << "  maxColorAttachmentBytesPerSample " << limitText(limits.maxColorAttachmentBytesPerSample) << " (G-buffer budget, milestone 11)\n";
		std::cout << "  maxTextureArrayLayers            " << limits.maxTextureArrayLayers << " (block textures per array, milestone 7)\n";
		std::cout << "  maxSampledTexturesPerShaderStage " << limits.maxSampledTexturesPerShaderStage << " (binding array size, milestone 7)\n";
		std::cout << "  maxStorageBufferBindingSize      " << limits.maxStorageBufferBindingSize << " (chunk face arena, milestone 6)\n";
	}

	std::cout << "=============================================\n\n";
}

bool init(GLFWwindow *glfwWindow)
{
	window = glfwWindow;

	// wgpuGetVersion() (wgpu.h) packs major.minor.patch.build into one byte each.
	uint32_t v = wgpuGetVersion();
	std::cout << "WebGPU: wgpu-native " << ((v >> 24) & 0xff) << "." << ((v >> 16) & 0xff)
		<< "." << ((v >> 8) & 0xff) << "." << (v & 0xff) << "\n";

	WGPUInstance instance = render::wgpuInitInstance();
	if (!instance) { return false; }

	// On macOS this attaches a CAMetalLayer to the window's content view.
	surface = glfwCreateWindowWGPUSurface(instance, window);
	if (!surface)
	{
		std::cout << "WebGPU: could not create a surface for the window.\n";
		return false;
	}
	pinMetalLayerToSRGB(window);

	// Framebuffer pixels, not window points: twice the window size on Retina.
	int width = 0, height = 0;
	glfwGetFramebufferSize(window, &width, &height);
	if (!render::wgpuInit(surface, width, height)) { return false; }

	printPortReport(render::wgpuAdapter());

	// The game keeps GL_CULL_FACE on while it draws gl2d (gameLayer.cpp), and
	// its UI looks right only with mirrored quads culled.
	wgpu2d::setCullMirroredQuads(true);
	return true;
}

void beginFrame()
{
	int width = 0, height = 0;
	glfwGetFramebufferSize(window, &width, &height);

	// Cheap when the size is unchanged. On Metal the surface never reports
	// Outdated, so this is the only thing that follows a window resize.
	render::wgpuResize(width, height);
	render::wgpuBeginFrame();
}

void endFrame()
{
	// WGPU_SCREENSHOT_FRAME=N saves frame N as screenshot-frameN.png in the
	// working directory: a way to see what WebGPU drew from a script, with no
	// screen capture permission involved.
	// A comma-separated list (N,M,...) saves each of those frames.
	static long long frameIndex = 0;
	static const char *shotAt = std::getenv("WGPU_SCREENSHOT_FRAME");
	++frameIndex;
	bool shoot = false;
	for (const char *p = shotAt; p && *p; )
	{
		char *end = nullptr;
		const long long frame = std::strtoll(p, &end, 10);
		if (end == p) { break; }
		if (frame == frameIndex) { shoot = true; }
		p = (*end == ',') ? end + 1 : end;
	}

	// The capture is taken inside wgpuEndFrame, so ask before it.
	if (shoot) { render::wgpuRequestFrameCapture(); }

	render::wgpuEndFrame();

	std::vector<unsigned char> pixels;
	int width = 0, height = 0;
	if (shoot && render::wgpuTakeFrameCapture(pixels, width, height))
	{
		std::string name = "screenshot-frame" + std::to_string(frameIndex) + ".png";
		if (stbi_write_png(name.c_str(), width, height, 4, pixels.data(), width * 4))
		{
			std::cout << "screenshot: " << name << " (" << width << "x" << height << ")\n";
		}
		else
		{
			std::cout << "screenshot: could not write " << name << "\n";
		}
	}
}

void shutdown()
{
	render::wgpuShutdown();
	if (surface) { wgpuSurfaceRelease(surface); surface = nullptr; }
}

#ifndef __APPLE__
void pinMetalLayerToSRGB(GLFWwindow *) {}
#endif

}

#endif
