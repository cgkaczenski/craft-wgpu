// The WebGPU render context (render::wgpu*) and the gl2d-shaped API on top
// of it (wgpu2d::*). One translation unit on purpose: the API's methods
// need the context's internals and there is one renderer.

#include <render/quadShaderSource.h>   // generated from resources/shaders/quad.wgsl
#include <render/mipmapShaderSource.h> // generated from resources/shaders/mipmap.wgsl
#include <render/brightpassShaderSource.h>
#include <render/blurShaderSource.h>
#include <render/wgpuContext.h>
#include <render/wgpu2d.h>
#include <render/wgpuFrame.h>
#ifdef __APPLE__
#endif

#include <webgpu/webgpu.hpp> // WebGPU-Cpp wrapper over webgpu.h (+ wgpu.h extensions)
#include <stb_image/stb_image.h>
#include <glm/glm.hpp>       // column-major like WGSL's mat4x4f

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>   // offsetof
#include <cstdlib>   // getenv
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// File scope only, never in a header: wgpu:: names would otherwise leak into
// every includer. Inside this file, Device, Default, StringView, and the
// scoped enums all come from the wrapper. wgpu::Texture is always qualified
// because wgpu2d::Texture is the game-facing handle.
using namespace wgpu;

namespace render
{

namespace
{
	// What actually varies between sprite pipelines. Everything else the
	// descriptor sets -- topology, cull mode, vertex layout, bind group layout
	// -- is the same for every sprite draw, so it stays hardcoded in
	// createQuadPipelineVariant rather than becoming a key nobody varies.
	//
	// Deliberately narrow. Sample count (N6), depth state (N7) and the vertex
	// layout (N5) all belong here eventually; adding a field then is a few
	// lines, and guessing at four futures now is how the wrong key gets built.
	// What an effect fragment shader can read. One block per effect draw.
	// std140-ish by construction: vec4s only, 16-byte aligned throughout, so
	// the WGSL struct and this one cannot drift apart silently.
	struct EffectUniforms
	{
		float resolution[4] = {}; // xy = target pixels, zw = 1 / pixels
		float time[4] = {};       // x = seconds since init
		float a[4] = {};          // whatever the effect wants
		float b[4] = {};
		float c[4] = {};
		float d[4] = {};
	};
	static_assert(sizeof(EffectUniforms) == 96, "effect uniforms must stay 6 x vec4");

	struct PipelineKey
	{
		TextureFormat format = TextureFormat::Undefined;
		wgpu2d::BlendMode blend = wgpu2d::BlendMode::Alpha;

		// F2: which shader this pipeline runs. 0 is the sprite shader that
		// every ordinary draw uses; higher values index registered effects.
		//
		// This is the field outline 14 said would arrive with the first effect
		// that needs its own WGSL, and it is why the key was left at two
		// fields rather than guessed wider: a shader is a genuinely different
		// pipeline, and until something needed one there was nothing to key
		// on. Sample count (N6), depth state (N7) and the vertex layout (N5)
		// are still absent for the same reason.
		uint32_t shader = 0;

		bool operator==(const PipelineKey &other) const
		{
			return format == other.format && blend == other.blend
				&& shader == other.shader;
		}
	};

	struct PipelineEntry
	{
		PipelineKey key;
		// Null means this variant was tried and could not be built. That is
		// cached deliberately: flushBatch asks for a pipeline per *draw run*,
		// so a failure that is not remembered becomes a fresh
		// createRenderPipeline on every run of every frame -- a handful of
		// pipeline compiles per frame, each with a blocking error-scope drain,
		// which is enough to take a 13 ms frame past 50.
		RenderPipeline pipeline = nullptr;
		std::string label; // owns the text; useful in logs after creation
	};

	// ---------------------------------------------------------------------
	// Context: everything created once, plus the state of the current frame.
	// ---------------------------------------------------------------------
	struct Context
	{
		// 1a
		Instance instance = nullptr;
		Surface surface = nullptr; // borrowed: the app creates and releases it
		Adapter adapter = nullptr;

		// 1b
		Device device = nullptr;
		Queue queue = nullptr;
		TextureFormat surfaceFormat = TextureFormat::Undefined;
		int surfaceWidth = 0;
		int surfaceHeight = 0;
		bool surfaceConfigured = false;
		bool firstFramePresented = false;

		// 2, then R2: a render pipeline bakes in its colour target's format and
		// its blend state, so "the pipeline" is really one per (format, blend)
		// pair. They are built on demand and kept; there are a handful at most.
		// The shader module is compiled once and shared by every variant --
		// with one pipeline it could be released immediately, with N it cannot.
		ShaderModule quadShaderModule = nullptr;
		std::vector<PipelineEntry> quadPipelines;

		// 6a: the vertex buffer is rewritten every flush from the CPU batch
		// and grows (never shrinks) when a flush needs more than it holds.
		Buffer vertexBuffer = nullptr;
		uint64_t vertexBufferCapacityBytes = 0;

		// 10: how much of the vertex buffer and how many camera slots this
		// frame has already used. A frame can flush more than once now (a
		// render target, then the screen), and every draw in the frame reads
		// these buffers as they are at submit time -- a second flush writing
		// at offset 0 would silently rewrite what the first pass's draws
		// point at. So each flush appends instead, and both reset at
		// wgpuBeginFrame.
		uint64_t vertexBufferUsedBytes = 0;
		uint32_t cameraSlotsUsed = 0;

		// 4: created once at init, shared by every texture. Two samplers:
		// gl2d's "pixelated" (nearest) and its default (linear) filtering.
		Sampler samplerPixelated = nullptr;
		Sampler samplerLinear = nullptr;
		// The same two filters, repeating past the edge instead of clamping:
		// for textures laid over a mesh at a fixed scale (asteroids, A1), where
		// a big shape runs off the image and has to wrap onto it again.
		Sampler samplerPixelatedRepeat = nullptr;
		Sampler samplerLinearRepeat = nullptr;
		BindGroupLayout textureBindGroupLayout = nullptr; // group 0: texture + sampler
		PipelineLayout pipelineLayout = nullptr;

		// N4: the compute pipeline that builds mip levels. Created on the first
		// mipped texture rather than at init, so a game that never asks for
		// mips never compiles it -- and the failure is remembered, because a
		// pipeline that could not be built will not build on the next texture
		// either and retrying it once per image is how milestone 12 learned to
		// turn one broken pipeline into 783 of them.
		ShaderModule mipmapShaderModule = nullptr;
		BindGroupLayout mipmapBindGroupLayout = nullptr;
		PipelineLayout mipmapPipelineLayout = nullptr;
		ComputePipeline mipmapPipeline = nullptr;
		bool mipmapPipelineFailed = false;

		// 5/6b: the camera uniform. One 64-byte matrix per camera used in a
		// frame, each in its own slot of cameraSlotStride bytes (the device's
		// minUniformBufferOffsetAlignment), selected per draw with a dynamic
		// offset. Buffer and bind group are recreated together when the slot
		// count grows.
		BindGroupLayout cameraBindGroupLayout = nullptr; // group 1: uniform buffer, dynamic offset
		BindGroupLayout effectBindGroupLayout = nullptr; // group 2: effect parameters, dynamic offset
		Buffer effectBuffer = nullptr;
		BindGroup effectBindGroup = nullptr;
		uint32_t effectSlotStride = 0;
		uint32_t effectSlotCapacity = 0;
		uint32_t effectSlotsUsed = 0;
		Buffer uniformBuffer = nullptr;
		BindGroup cameraBindGroup = nullptr;
		uint32_t cameraSlotStride = 256;
		uint32_t cameraSlotCapacity = 0;
		uint32_t minUniformBufferOffsetAlignment = 256;
		bool runStatsPrinted = false;

		// 7: the frame in progress, between wgpuBeginFrame and wgpuEndFrame.
		bool frameOpen = false;          // begin succeeded; end must submit
		wgpu::Texture frameTexture = nullptr;
		TextureView frameView = nullptr;
		CommandEncoder frameEncoder = nullptr;
		glm::vec4 clearColor = {0, 0, 0, 1};   // recorded by Renderer2D::clearScreen

		// 10: a render pass belongs to one attachment, so the frame no longer
		// has "the" pass: it has the pass for whichever target was drawn into
		// last. Switching target ends the open pass and begins another.
		// framePassTarget is a texture registry id, 0 for the surface.
		RenderPassEncoder framePass = nullptr; // begun lazily by the first flush
		uint32_t framePassTarget = 0;
		bool surfaceCleared = false;     // the frame's first surface pass clears
		bool scaledCleared = false;      // likewise for the low-res stand-in

		// 10: optional low-resolution rendering. WGPU_RENDER_SCALE=0.25 makes
		// the game's draws land in a quarter-size target that is upscaled to
		// the surface at the end of the frame (ImGui still draws to the
		// surface at native size). The projection keeps using the surface's
		// dimensions, so the framing, the HUD layout and the mouse mapping
		// are untouched: only the rasterized resolution changes.
		float renderScale = 1.f;
		uint32_t scaledTargetId = 0;     // 0 when rendering straight to the surface
		// The registry slot the frame's target lives in, kept even while
		// scaledTargetId is 0. Without it, switching a final effect off and on
		// again allocated a fresh screen-sized texture every time and orphaned
		// the last one -- and handed out a new id, which is worse than the leak
		// because anything holding a bind group built from the old view keeps
		// reading a texture nothing writes to any more.
		uint32_t frameTargetSlot = 0;
		// Bumped whenever the texture in that slot is replaced. The id alone is
		// not enough to cache against: reusing the slot keeps the id and swaps
		// the view underneath it, which is exactly the case that leaves a bind
		// group pointing at a released view.
		uint32_t frameTargetGeneration = 0;
		bool scaledTargetDrawn = false;  // has content this frame, needs compositing
		bool compositing = false;        // guard while the composite draws
		bool scaledTargetLinear = false; // how the target is sampled on the way back

		// F7: an effect the composite runs through. Non-zero also *causes* the
		// target to exist, at full size, which is the whole trick: milestone
		// 10 already routes the frame through a target and composites it at
		// the right moment, and all a final effect needs is for that to happen
		// when the render scale is 1 too.
		uint32_t finalEffectId = 0;
		wgpu2d::EffectParams finalEffectParams;

		// N4 step two: the phosphor glow. Two half-resolution scratch textures
		// to ping-pong between, because a separable blur cannot read and write
		// the same texels it is walking over -- the one thing the mip generator
		// never had to worry about, since every dispatch there wrote a level
		// nothing in that dispatch read.
		//
		// Both compute stages have the same binding shape (a texture in, a
		// storage texture out, one vec4 of parameters), so they share a layout
		// and differ only in which module they run.
		ShaderModule brightpassShaderModule = nullptr;
		ShaderModule blurShaderModule = nullptr;
		BindGroupLayout glowBindGroupLayout = nullptr;
		PipelineLayout glowPipelineLayout = nullptr;
		ComputePipeline brightpassPipeline = nullptr;
		ComputePipeline blurPipeline = nullptr;
		bool glowPipelineFailed = false;
		Buffer glowParamBuffer = nullptr;
		uint32_t glowParamStride = 256;
		uint32_t glowScratchA = 0;
		uint32_t glowScratchB = 0;
		BindGroup glowGroups[3] = {};
		int glowWidth = 0;
		int glowHeight = 0;
		// The frame's size as well as the scratch's, because the first bind
		// group names the frame target's view. Halving means two frame sizes
		// can share one scratch size, and the group would then hold a view of
		// a target that no longer exists.
		int glowFrameWidth = 0;
		int glowFrameHeight = 0;
		// And which target, not just how big. The size can be unchanged while
		// the texture underneath it is a different one.
		uint32_t glowFrameGeneration = 0;
		bool finalGlowSet = false;
		wgpu2d::FinalGlow finalGlow;
	};

	Context g;

	// Per-frame diagnostic counters. Reset in wgpuBeginFrame, folded into
	// FrameStats at the end. These exist to answer one question after the
	// fact: when a frame took five times as long as its neighbours, what did
	// it do that a normal frame does not?
	struct FramePerf
	{
		int pipelineBuilds = 0;
		int pipelineFailures = 0;
		int validationErrors = 0;
		int texturesCreated = 0;
		float blockedMs = 0.f;
		float acquireMs = 0.f;
		float presentMs = 0.f;
	};
	FramePerf framePerf;

	// Wall clock since init, for the effect uniform's time. Wall rather than
	// game time: an effect is a property of the picture, and the debug speed
	// slider should not slow a shimmer down.
	std::chrono::steady_clock::time_point startedAt;


	// 10: defined below, needed by the pass bookkeeping above them.
	void flushBatch(uint32_t target);
	void compositeScaledTarget();

	// The target a plain flush goes to: the low-res stand-in when that is
	// enabled, otherwise the surface.
	uint32_t resolveTarget(uint32_t target)
	{
		if (target == 0 && g.scaledTargetId) { return g.scaledTargetId; }
		return target;
	}

	// The surface and the low-res stand-in are rebuilt every frame, so their
	// first pass clears; a user's FrameBuffer keeps its contents like gl2d's.
	bool isFrameTarget(uint32_t target)
	{
		return target == 0 || (g.scaledTargetId != 0 && target == g.scaledTargetId);
	}

	// One vertex as the GPU reads it: position, color, texture coordinate,
	// back to back. The pipeline's vertex layout below must describe exactly this.
	struct Vertex
	{
		float x, y;
		float r, g, b, a;
		float u, v;
	};
	static_assert(sizeof(Vertex) == 32, "vertex layout stride assumes tightly packed floats");

	// What the shader's Camera struct reads: one column-major mat4x4f.
	struct CameraUniforms
	{
		glm::mat4 viewProj;
	};
	static_assert(sizeof(CameraUniforms) == 64, "uniform layout must match the WGSL Camera struct");

	// The registry behind wgpu2d::Texture handles, indexed by id - 1.
	struct TextureEntry
	{
		wgpu::Texture texture = nullptr;
		TextureView view = nullptr;
		BindGroup bindGroup = nullptr; // group 0 for this texture + its sampler

		// N4 step two: a second view of the same texels, declared for writing.
		// A view carries the usages it may be bound for, and a compute shader
		// writing through the sampling view is refused -- so a texture that is
		// both read as a sprite and written by a dispatch needs one of each.
		TextureView storageView = nullptr;
		int width = 0;
		int height = 0;

		// R2: the format a pipeline must be built for to draw into this as an
		// attachment. Sampled-only textures carry it too, so nothing has to
		// guess which kind it is holding.
		TextureFormat format = TextureFormat::Undefined;

		// 10: render targets can also be drawn into. needsClear makes the
		// next pass on it clear instead of load, which is how FrameBuffer's
		// deferred clear and a freshly created (undefined) target work.
		bool pixelated = false;
		bool renderTarget = false;
		bool needsClear = false;
	};

	std::vector<TextureEntry> textures;

	// The CPU batch: gl2d's spritePositions / spriteColors / texturePositions
	// / spriteTextures, collapsed into one interleaved vertex vector plus,
	// per quad, its texture and the camera it was drawn under.
	std::vector<Vertex> batchVertices;
	std::vector<uint32_t> batchQuadTextures;
	std::vector<uint32_t> batchQuadCameras;   // index into frameCameras
	std::vector<wgpu2d::BlendMode> batchQuadBlends; // R2: the third run key

	// F6: which shader each quad runs, and which parameter block it reads.
	// Both join the run key -- a shader is a different pipeline and a slot is
	// a different dynamic offset, and neither can change inside a draw.
	std::vector<uint32_t> batchQuadShaders;
	std::vector<uint32_t> batchQuadEffectSlots; // index into frameEffectParams

	// Where each record's vertices start in batchVertices. A "quad" record is
	// six vertices; a renderTriangles record is however many it was given
	// (asteroids, A1). The flush draws a run from its first record's start to
	// the next run's start, so it never needs to know which kind it holds.
	std::vector<uint32_t> batchQuadFirstVertex;

	// N2a: what the frame cost. Accumulated as it is recorded, snapshotted at
	// the end so a reader mid-frame sees a whole frame rather than a partial.
	wgpu2d::FrameStats statsInProgress;
	wgpu2d::FrameStats statsLastFrame;

	// N2b: submit-to-work-done, the closest this adapter offers to a GPU
	// figure. One outstanding at a time; a frame that submits while the last
	// is still pending simply does not measure itself.
	std::chrono::steady_clock::time_point submitAt;
	bool workDonePending = false;
	float lastGpuMillis = -1.f;

	void onQueueWorkDone(WGPUQueueWorkDoneStatus status, void *, void *)
	{
		workDonePending = false;
		if (status != QueueWorkDoneStatus::Success) { return; }
		const auto now = std::chrono::steady_clock::now();
		lastGpuMillis = std::chrono::duration<float, std::milli>(now - submitAt).count();
	}
	std::vector<wgpu2d::Camera> frameCameras; // every distinct camera used this frame, in order

	// Every distinct effect parameter block used this frame, deduped the same
	// way frameCameras is: a new slot only when the values changed since the
	// last recorded one. A hundred quads sharing one set of parameters cost
	// one slot, which is the common case.
	std::vector<wgpu2d::EffectParams> frameEffectParams;

	bool sameEffectParams(const wgpu2d::EffectParams &a, const wgpu2d::EffectParams &b)
	{
		return a.a == b.a && a.b == b.b && a.c == b.c && a.d == b.d;
	}
	wgpu2d::Texture white1pxSquareTexture;    // gl2d's untextured path samples this

	// ---------------------------------------------------------------------
	// Small helpers
	// ---------------------------------------------------------------------
	const char *backendName(WGPUBackendType t)
	{
		switch (t)
		{
			case BackendType::Null: return "Null";
			case BackendType::WebGPU: return "WebGPU";
			case BackendType::D3D11: return "D3D11";
			case BackendType::D3D12: return "D3D12";
			case BackendType::Metal: return "Metal";
			case BackendType::Vulkan: return "Vulkan";
			case BackendType::OpenGL: return "OpenGL";
			case BackendType::OpenGLES: return "OpenGLES";
			default: return "Undefined";
		}
	}

	const char *adapterTypeName(WGPUAdapterType t)
	{
		switch (t)
		{
			case AdapterType::DiscreteGPU: return "discrete GPU";
			case AdapterType::IntegratedGPU: return "integrated GPU";
			case AdapterType::CPU: return "CPU";
			default: return "unknown";
		}
	}

	const char *formatName(WGPUTextureFormat f)
	{
		switch (f)
		{
			case TextureFormat::BGRA8Unorm: return "BGRA8Unorm";
			case TextureFormat::BGRA8UnormSrgb: return "BGRA8UnormSrgb";
			case TextureFormat::RGBA8Unorm: return "RGBA8Unorm";
			case TextureFormat::RGBA8UnormSrgb: return "RGBA8UnormSrgb";
			default: return "other";
		}
	}

	uint32_t ceilToNextMultiple(uint32_t value, uint32_t step)
	{
		uint32_t divide_and_ceil = value / step + (value % step == 0 ? 0 : 1);
		return step * divide_and_ceil;
	}

	// wgpu-native's own diagnostics (wgpu.h extension, C API: the wrapper does
	// not cover it). Warn and above is enough for now.
	void logCallback(WGPULogLevel level, WGPUStringView message, void *)
	{
		const char *tag = level == WGPULogLevel_Error ? "error"
			: level == WGPULogLevel_Warn ? "warn"
			: level == WGPULogLevel_Info ? "info"
			: level == WGPULogLevel_Debug ? "debug" : "trace";
		std::cerr << "[wgpu " << tag << "] " << StringView(message) << "\n";
	}

	// The wrapper keeps callbacks as plain C function pointers inside the
	// CallbackInfo structs (its std::function typedefs are unused in this
	// generation), so context still travels through userdata1.

	// Filled in by the request-adapter callback. Unsynchronized: the callback
	// only runs on this thread, inside wgpuInstanceProcessEvents (see mode below).
	struct AdapterRequest
	{
		Adapter adapter = nullptr;
		bool done = false;
	};

	void onAdapterRequest(WGPURequestAdapterStatus status, WGPUAdapter adapter,
		WGPUStringView message, void *userdata1, void *)
	{
		auto *req = static_cast<AdapterRequest *>(userdata1);
		if (status == RequestAdapterStatus::Success)
		{
			req->adapter = adapter;
		}
		else
		{
			std::cerr << "WebGPU: requestAdapter failed (status " << status << "): "
				<< StringView(message) << "\n";
		}
		req->done = true;
	}

	// Filled in by the request-device callback. Unsynchronized for the same
	// reason as AdapterRequest: it only runs inside wgpuInstanceProcessEvents.
	struct DeviceRequest
	{
		Device device = nullptr;
		bool done = false;
	};

	void onDeviceRequest(WGPURequestDeviceStatus status, WGPUDevice device,
		WGPUStringView message, void *userdata1, void *)
	{
		auto *req = static_cast<DeviceRequest *>(userdata1);
		if (status == RequestDeviceStatus::Success)
		{
			req->device = device;
		}
		else
		{
			std::cerr << "WebGPU: requestDevice failed (status " << status << "): "
				<< StringView(message) << "\n";
		}
		req->done = true;
	}

	// Fires if the GPU connection dies (driver reset, device destroyed).
	void onDeviceLost(WGPUDevice const *, WGPUDeviceLostReason reason,
		WGPUStringView message, void *, void *)
	{
		std::cerr << "WebGPU: device lost (reason " << reason << "): "
			<< StringView(message) << "\n";
	}

	// Fires on every validation error the API catches. This is the main
	// debugging channel: a bad descriptor field, a wrong usage flag, a
	// mismatched format all show up here as text.
	void onUncapturedError(WGPUDevice const *, WGPUErrorType type,
		WGPUStringView message, void *, void *)
	{
		const char *kind = type == ErrorType::Validation ? "validation"
			: type == ErrorType::OutOfMemory ? "out of memory"
			: type == ErrorType::Internal ? "internal" : "unknown";
		std::cerr << "WebGPU " << kind << " error: " << StringView(message) << "\n";
		++framePerf.validationErrors;
	}

	// ---------------------------------------------------------------------
	// Error scopes and debug groups
	// ---------------------------------------------------------------------

	// onUncapturedError above is the catch-all: it reports errors that no
	// scope claimed, and it cannot say which call produced them. When five
	// objects are created in a row and one message appears, matching it to a
	// call site is done by eye, and the message can arrive after the null
	// handle it explains.
	//
	// An error scope is a stack on the device. pushErrorScope claims errors
	// of one class (Validation / OutOfMemory / Internal); popErrorScope ends
	// the scope and reports the first error it captured, or NoError. Errors
	// inside a scope do not reach onUncapturedError. What that buys is not
	// the text -- it is attribution: this operation, right here, failed for
	// this reason.
	//
	// popErrorScope is asynchronous. The callback fires inside
	// instance.processEvents(), the same mechanism requestAdapter and
	// requestDevice use in wgpuInit. Draining it means a stall, which is why
	// every scope in this file wraps object *creation* and none of them wrap
	// per-frame recording.
	struct ErrorScopeResult
	{
		const char *what = nullptr;
		bool done = false;
		bool failed = false;
	};

	void onPopErrorScope(WGPUPopErrorScopeStatus status, WGPUErrorType type,
		WGPUStringView message, void *userdata1, void *)
	{
		auto *result = static_cast<ErrorScopeResult *>(userdata1);
		result->done = true;

		if (status != PopErrorScopeStatus::Success)
		{
			// EmptyStack means a pop without a push; anything else is the
			// instance going away underneath us.
			std::cerr << "WebGPU: popErrorScope (" << result->what << ") failed with status "
				<< status << "\n";
			return;
		}

		if (type == ErrorType::NoError) { return; }

		const char *kind = type == ErrorType::Validation ? "validation"
			: type == ErrorType::OutOfMemory ? "out of memory"
			: type == ErrorType::Internal ? "internal" : "unknown";
		std::cerr << "WebGPU " << kind << " error creating " << result->what << ": "
			<< StringView(message) << "\n";
		std::cerr.flush();
		result->failed = true;
	}

	// The scope stack's names. Scopes are pushed and popped on one thread
	// inside creation calls, so a plain vector is enough.
	std::vector<const char *> errorScopeNames;

	// ---------------------------------------------------------------------
	// Surface
	// ---------------------------------------------------------------------

	// The surface is the application's. We configure it and present to it,
	// then drop the pointer. Calling release() here would double-free with
	// the app's wgpuSurfaceRelease.
	void forgetSurface()
	{
		if (!g.surface) { return; }
		if (g.surfaceConfigured)
		{
			g.surface.unconfigure();
			g.surfaceConfigured = false;
		}
		g.surface = nullptr;
	}

	// Configures (or reconfigures) the surface at the window's current
	// framebuffer size. Framebuffer size, not window size: on a Retina
	// display they differ by the scale factor.
	// The size comes from the caller. The library used to ask GLFW, which is
	// how it came to depend on a window system it has no business knowing
	// about; the application already has this number.
	void configureSurface(int w, int h)
	{
		if (w <= 0 || h <= 0)
		{
			return; // minimized; keep the old configuration
		}

		SurfaceConfiguration config = Default;
		config.device = g.device;
		config.format = g.surfaceFormat;
		// CopySrc so a finished frame can be read back (see wgpuRequestFrameCapture).
		config.usage = TextureUsage::RenderAttachment | TextureUsage::CopySrc;
		config.width = (uint32_t)w;
		config.height = (uint32_t)h;
		config.viewFormatCount = 0;
		config.viewFormats = nullptr;
		config.alphaMode = CompositeAlphaMode::Auto;
		config.presentMode = PresentMode::Fifo; // vsync, always supported

		g.surface.configure(config);
		g.surfaceWidth = w;
		g.surfaceHeight = h;
		g.surfaceConfigured = true;
		std::cout << "WebGPU surface configured at " << w << "x" << h << "\n";
		std::cout.flush();
	}

	// ---------------------------------------------------------------------
	// Shaders, samplers, layouts, pipeline
	// ---------------------------------------------------------------------
	bool readTextFile(const char *path, std::string &out)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) { return false; }
		std::stringstream ss;
		ss << file.rdbuf();
		out = ss.str();
		return true;
	}

	// Compiles WGSL source into a shader module. `label` names it in
	// validation messages and in a GPU capture.
	ShaderModule createShaderModule(const char *source, const char *label)
	{
		// The WGSL source is a chained struct hanging off the module descriptor.
		// Default sets chain.sType = ShaderSourceWGSL and chain.next = nullptr.
		ShaderSourceWGSL wgsl = Default;
		wgsl.code = StringView(source);

		ShaderModuleDescriptor desc = Default;
		desc.label = StringView(label);
		desc.nextInChain = &wgsl.chain;

		// WGSL compile errors arrive here with line and column numbers, named
		// to the module, instead of loose on the uncaptured-error callback.
		ErrorScope scope(label);
		return g.device.createShaderModule(desc);
	}

	// Same, from a file. The library's own shaders are compiled in (see
	// quadShaderSource.h); this exists for the application, which has a
	// resource layout and may keep its shaders in it -- the ImGui backend in
	// src/platform does exactly that through wgpuCreateShaderModuleFromFile.
	ShaderModule createShaderModuleFromFile(const char *path)
	{
		std::string source;
		if (!readTextFile(path, source))
		{
			std::cerr << "WebGPU: cannot read shader file " << path << "\n";
			return nullptr;
		}
		return createShaderModule(source.c_str(), path);
	}

	// Samplers: how a shader reads a texture. Separate from the texture
	// (unlike OpenGL), so a few serve every sprite. Sprites clamp to edge like
	// gl2d. Pixelated = gl2d's GL_NEAREST / GL_NEAREST_MIPMAP_NEAREST;
	// linear = GL_LINEAR / GL_LINEAR_MIPMAP_LINEAR.
	//
	// The address mode is what happens to a coordinate outside 0..1. Clamp
	// reads the edge texel forever; repeat keeps only the fractional part, so
	// 1.25 reads what 0.25 does and a tileable image covers any area.
	Sampler createSampler(const char *label, FilterMode filter, MipmapFilterMode mipFilter,
		AddressMode address = AddressMode::ClampToEdge)
	{
		SamplerDescriptor desc = Default;
		desc.label = StringView(label);
		desc.addressModeU = address;
		desc.addressModeV = address;
		desc.addressModeW = address;
		desc.magFilter = filter;
		desc.minFilter = filter;
		desc.mipmapFilter = mipFilter;
		desc.lodMinClamp = 0.0f;
		desc.lodMaxClamp = 32.0f;
		desc.compare = CompareFunction::Undefined;
		desc.maxAnisotropy = 1;

		ErrorScope scope(label);
		return g.device.createSampler(desc);
	}

	bool createSamplers()
	{
		g.samplerPixelated = createSampler("sampler pixelated", FilterMode::Nearest, MipmapFilterMode::Nearest);
		g.samplerLinear = createSampler("sampler linear", FilterMode::Linear, MipmapFilterMode::Linear);
		g.samplerPixelatedRepeat = createSampler("sampler pixelated repeat", FilterMode::Nearest,
			MipmapFilterMode::Nearest, AddressMode::Repeat);
		g.samplerLinearRepeat = createSampler("sampler linear repeat", FilterMode::Linear,
			MipmapFilterMode::Linear, AddressMode::Repeat);
		if (!g.samplerPixelated || !g.samplerLinear || !g.samplerPixelatedRepeat || !g.samplerLinearRepeat)
		{
			std::cerr << "WebGPU: createSampler returned null\n";
			return false;
		}
		return true;
	}

	// Bind group layouts: the contract between the shader's
	// @group(n) @binding(m) declarations and the resources a bind group
	// supplies. Group 0: a sampled 2D float texture (binding 0) and a
	// filtering sampler (binding 1), fragment stage. Group 1: the camera
	// uniform with a dynamic offset, vertex stage. The pipeline layout
	// lists them by group index.
	bool createLayouts()
	{
		// One scope for the whole function: the three layouts are created
		// together and a failure in any of them means the same thing.
		ErrorScope scope("bind group layouts");

		BindGroupLayoutEntry entries[2];

		// Each entry describes exactly one kind of binding. Default leaves the
		// four sub-layouts at Undefined, which is ambiguous; the three unused
		// ones must say BindingNotUsed explicitly.
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].visibility = ShaderStage::Fragment;
		entries[0].texture.sampleType = TextureSampleType::Float;
		entries[0].texture.viewDimension = TextureViewDimension::_2D;
		entries[0].texture.multisampled = false;
		entries[0].buffer.type = BufferBindingType::BindingNotUsed;
		entries[0].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[0].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		entries[1] = Default;
		entries[1].binding = 1;
		entries[1].visibility = ShaderStage::Fragment;
		entries[1].sampler.type = SamplerBindingType::Filtering;
		entries[1].buffer.type = BufferBindingType::BindingNotUsed;
		entries[1].texture.sampleType = TextureSampleType::BindingNotUsed;
		entries[1].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("texture bind group layout");
		layoutDesc.entryCount = 2;
		layoutDesc.entries = entries;
		g.textureBindGroupLayout = g.device.createBindGroupLayout(layoutDesc);
		if (!g.textureBindGroupLayout)
		{
			std::cerr << "WebGPU: createBindGroupLayout returned null\n";
			return false;
		}

		BindGroupLayoutEntry cameraEntry = Default;
		cameraEntry.binding = 0;
		cameraEntry.visibility = ShaderStage::Vertex;
		cameraEntry.buffer.type = BufferBindingType::Uniform;
		cameraEntry.buffer.hasDynamicOffset = true;    // one buffer, one slot per camera, offset per draw
		cameraEntry.buffer.minBindingSize = sizeof(CameraUniforms);
		cameraEntry.sampler.type = SamplerBindingType::BindingNotUsed;
		cameraEntry.texture.sampleType = TextureSampleType::BindingNotUsed;
		cameraEntry.storageTexture.access = StorageTextureAccess::BindingNotUsed;

		BindGroupLayoutDescriptor cameraLayoutDesc = Default;
		cameraLayoutDesc.label = StringView("camera bind group layout");
		cameraLayoutDesc.entryCount = 1;
		cameraLayoutDesc.entries = &cameraEntry;
		g.cameraBindGroupLayout = g.device.createBindGroupLayout(cameraLayoutDesc);
		if (!g.cameraBindGroupLayout)
		{
			std::cerr << "WebGPU: createBindGroupLayout (camera) returned null\n";
			return false;
		}

		// Group 2: the effect parameter block. Dynamic offset for the same
		// reason the camera has one -- writeBuffer is ordered against the
		// submit, not against the recorded draws, so two effect draws sharing
		// one slot would both read whatever was written last. Milestone 10
		// records that bug being found the hard way with the vertex buffer;
		// this is the same trap and the same answer.
		//
		// Visible to the fragment stage, because that is where an effect runs.
		BindGroupLayoutEntry effectEntry = Default;
		effectEntry.binding = 0;
		effectEntry.visibility = ShaderStage::Fragment;
		effectEntry.buffer.type = BufferBindingType::Uniform;
		effectEntry.buffer.hasDynamicOffset = true;
		effectEntry.buffer.minBindingSize = sizeof(EffectUniforms);
		effectEntry.sampler.type = SamplerBindingType::BindingNotUsed;
		effectEntry.texture.sampleType = TextureSampleType::BindingNotUsed;
		effectEntry.storageTexture.access = StorageTextureAccess::BindingNotUsed;

		BindGroupLayoutDescriptor effectLayoutDesc = Default;
		effectLayoutDesc.label = StringView("effect bind group layout");
		effectLayoutDesc.entryCount = 1;
		effectLayoutDesc.entries = &effectEntry;
		g.effectBindGroupLayout = g.device.createBindGroupLayout(effectLayoutDesc);
		if (!g.effectBindGroupLayout)
		{
			std::cerr << "WebGPU: createBindGroupLayout (effect) returned null\n";
			return false;
		}

		// Three groups in one layout, shared by every pipeline. The sprite
		// shader never declares group 2 -- a shader may use fewer groups than
		// its layout declares -- but every draw must still *bind* it, so
		// flushBatch binds slot 0 unconditionally and sprite draws simply
		// ignore what is in it.
		PipelineLayoutDescriptor pipelineLayoutDesc = Default;
		pipelineLayoutDesc.label = StringView("quad pipeline layout");
		pipelineLayoutDesc.bindGroupLayoutCount = 3;
		WGPUBindGroupLayout layouts[3] = { g.textureBindGroupLayout, g.cameraBindGroupLayout,
			g.effectBindGroupLayout };
		pipelineLayoutDesc.bindGroupLayouts = layouts;
		g.pipelineLayout = g.device.createPipelineLayout(pipelineLayoutDesc);
		if (!g.pipelineLayout)
		{
			std::cerr << "WebGPU: createPipelineLayout returned null\n";
			return false;
		}
		return true;
	}

	// The uniform buffer holds one CameraUniforms per slot, slots spaced by
	// the device's minimum dynamic-offset alignment (256 here). The bind
	// group binds one struct's worth at offset 0; setBindGroup's dynamic
	// offset moves that window to the slot for each draw. Growing the slot
	// count means a new buffer and, because the group references the buffer,
	// a new bind group.
	bool ensureCameraSlotCapacity(uint32_t slots)
	{
		if (g.uniformBuffer && slots <= g.cameraSlotCapacity) { return true; }

		ErrorScope scope("camera uniform buffer");

		g.cameraSlotStride = ceilToNextMultiple((uint32_t)sizeof(CameraUniforms), g.minUniformBufferOffsetAlignment);
		uint32_t capacity = g.cameraSlotCapacity ? g.cameraSlotCapacity : 16;
		while (capacity < slots) { capacity *= 2; }

		BufferDescriptor desc = Default;
		desc.label = StringView("camera uniforms");
		desc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		desc.size = (uint64_t)g.cameraSlotStride * capacity;
		desc.mappedAtCreation = false;
		Buffer newBuffer = g.device.createBuffer(desc);
		if (!newBuffer)
		{
			std::cerr << "WebGPU: createBuffer (uniform, " << desc.size << " bytes) returned null\n";
			return false;
		}

		BindGroupEntry entry = Default;
		entry.binding = 0;
		entry.buffer = newBuffer;
		entry.offset = 0;
		entry.size = sizeof(CameraUniforms); // one struct, not the whole buffer
		entry.sampler = nullptr;
		entry.textureView = nullptr;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("camera bind group");
		groupDesc.layout = g.cameraBindGroupLayout;
		groupDesc.entryCount = 1;
		groupDesc.entries = &entry;
		BindGroup newGroup = g.device.createBindGroup(groupDesc);
		if (!newGroup)
		{
			std::cerr << "WebGPU: createBindGroup (camera) returned null\n";
			newBuffer.release();
			return false;
		}

		if (g.cameraBindGroup) { g.cameraBindGroup.release(); }
		if (g.uniformBuffer) { g.uniformBuffer.release(); }
		g.cameraBindGroup = newGroup;
		g.uniformBuffer = newBuffer;
		g.cameraSlotCapacity = capacity;
		std::cout << "WebGPU camera slots: " << capacity << " x " << g.cameraSlotStride << " bytes\n";
		std::cout.flush();
		return true;
	}

	// The render pipeline: every configurable stage of the GPU's fixed
	// triangle pipeline, baked into one immutable object. Selected per pass
	// with setPipeline; never mutated.
	// Shader 0 is the sprite shader; anything else is an effect the
	// application registered. Modules live for the process, because a pipeline
	// built from one keeps needing it and the cache keeps the pipeline.
	struct ShaderEntry
	{
		ShaderModule module = nullptr;
		std::string name;
	};
	std::vector<ShaderEntry> shaderRegistry;

	ShaderModule shaderModuleFor(uint32_t index)
	{
		if (index == 0) { return g.quadShaderModule; }
		const size_t at = (size_t)index - 1;
		if (at >= shaderRegistry.size()) { return nullptr; }
		return shaderRegistry[at].module;
	}

	const char *shaderNameFor(uint32_t index)
	{
		if (index == 0) { return "sprite"; }
		const size_t at = (size_t)index - 1;
		return at < shaderRegistry.size() ? shaderRegistry[at].name.c_str() : "?";
	}

	const char *blendName(wgpu2d::BlendMode mode)
	{
		switch (mode)
		{
			case wgpu2d::BlendMode::Additive: return "additive";
			case wgpu2d::BlendMode::Premultiplied: return "premultiplied";
			case wgpu2d::BlendMode::Mask: return "mask";
			default: return "alpha";
		}
	}

	// One pipeline for one (format, blend) pair. Everything the descriptor
	// sets other than those two is identical across variants, which is what
	// makes the key as small as it is.
	// The effect slot buffer, grown the way the camera's is: doubling, never
	// shrinking, one slot per effect draw in a frame at the alignment the
	// device demands.
	bool ensureEffectSlotCapacity(uint32_t slots)
	{
		if (g.effectBuffer && slots <= g.effectSlotCapacity) { return true; }

		g.effectSlotStride = ceilToNextMultiple((uint32_t)sizeof(EffectUniforms),
			g.minUniformBufferOffsetAlignment);
		uint32_t capacity = g.effectSlotCapacity ? g.effectSlotCapacity : 4;
		while (capacity < slots) { capacity *= 2; }

		ErrorScope scope("effect uniform buffer");

		BufferDescriptor desc = Default;
		desc.label = StringView("effect uniforms");
		desc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		desc.size = (uint64_t)g.effectSlotStride * capacity;
		desc.mappedAtCreation = false;
		Buffer newBuffer = g.device.createBuffer(desc);
		if (!newBuffer) { return false; }

		BindGroupEntry entry = Default;
		entry.binding = 0;
		entry.buffer = newBuffer;
		entry.offset = 0;
		entry.size = sizeof(EffectUniforms);
		entry.sampler = nullptr;
		entry.textureView = nullptr;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView("effect bind group");
		groupDesc.layout = g.effectBindGroupLayout;
		groupDesc.entryCount = 1;
		groupDesc.entries = &entry;
		BindGroup newGroup = g.device.createBindGroup(groupDesc);
		if (!newGroup) { newBuffer.release(); return false; }

		if (g.effectBindGroup) { g.effectBindGroup.release(); }
		if (g.effectBuffer) { g.effectBuffer.release(); }
		g.effectBindGroup = newGroup;
		g.effectBuffer = newBuffer;
		g.effectSlotCapacity = capacity;
		return true;
	}

	// Compiles an effect's fragment stage and gives it an index. Index 0 is
	// reserved for the sprite shader, so a registered effect is its position
	// plus one -- which also makes 0 a usable "no effect" value.
	uint32_t registerEffectShader(const char *wgsl, const char *name)
	{
		if (!g.device || !wgsl || !name) { return 0; }

		ShaderModule module = createShaderModule(wgsl, name);
		if (!module)
		{
			std::cerr << "WebGPU: effect '" << name << "' did not compile\n";
			return 0;
		}

		ShaderEntry entry;
		entry.module = module;
		entry.name = name;
		shaderRegistry.push_back(std::move(entry));

		std::cout << "WebGPU effect '" << name << "' registered as shader "
			<< shaderRegistry.size() << "\n";
		std::cout.flush();
		return (uint32_t)shaderRegistry.size();
	}

	RenderPipeline createQuadPipelineVariant(const PipelineKey &key, const char *label)
	{
		// Vertex and fragment come from separate modules, which is what lets an
		// effect be a *fragment function only*. Every effect inherits the
		// sprite vertex stage, so it inherits the vertex layout and the camera
		// transform for free and its author writes one function rather than a
		// whole pipeline's worth of WGSL.
		ShaderModule vertexModule = g.quadShaderModule;
		ShaderModule fragmentModule = shaderModuleFor(key.shader);
		if (!vertexModule || !fragmentModule) { return nullptr; }

		RenderPipelineDescriptor desc = Default;
		desc.label = StringView(label);

		// Vertex layout: how the pipeline reads bytes out of the vertex buffer.
		// One buffer, interleaved, 32 bytes per vertex. Each attribute names
		// the @location it feeds in the shader.
		VertexAttribute attributes[3];
		attributes[0] = Default;
		attributes[0].shaderLocation = 0;                // @location(0) position
		attributes[0].format = VertexFormat::Float32x2;
		attributes[0].offset = offsetof(Vertex, x);
		attributes[1] = Default;
		attributes[1].shaderLocation = 1;                // @location(1) color
		attributes[1].format = VertexFormat::Float32x4;
		attributes[1].offset = offsetof(Vertex, r);
		attributes[2] = Default;
		attributes[2].shaderLocation = 2;                // @location(2) uv
		attributes[2].format = VertexFormat::Float32x2;
		attributes[2].offset = offsetof(Vertex, u);

		VertexBufferLayout vertexLayout = Default;
		vertexLayout.arrayStride = sizeof(Vertex);
		vertexLayout.stepMode = VertexStepMode::Vertex; // advance once per vertex, not per instance
		vertexLayout.attributeCount = 3;
		vertexLayout.attributes = attributes;

		desc.vertex.module = vertexModule;
		desc.vertex.entryPoint = StringView("vs_main");
		desc.vertex.constantCount = 0;
		desc.vertex.constants = nullptr;
		desc.vertex.bufferCount = 1;
		desc.vertex.buffers = &vertexLayout;

		// Primitive assembly. Default already gives TriangleList, CCW front
		// face, no culling; set explicitly so the choice is visible.
		desc.primitive.topology = PrimitiveTopology::TriangleList;
		desc.primitive.stripIndexFormat = IndexFormat::Undefined;
		desc.primitive.frontFace = FrontFace::CCW;
		desc.primitive.cullMode = CullMode::None; // gl2d never culled; quads may be flipped by negative sizes
		desc.primitive.unclippedDepth = false;

		// Alpha is gl2d's blend, matching enableNecessaryGLFeatures():
		//   glBlendEquation(GL_FUNC_ADD)
		//   glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA)
		//
		// The alpha channel is One / OneMinusSrcAlpha in every mode, which is
		// what makes a render target usable: drawing (C, a) into a cleared
		// target leaves rgb = C*a and a = a, so the target's contents are
		// premultiplied no matter which colour mode produced them. That is
		// why Premultiplied exists and why it is the right mode to draw a
		// target back with.
		BlendState blend = Default;
		blend.color.operation = BlendOperation::Add;
		switch (key.blend)
		{
			case wgpu2d::BlendMode::Additive:
				// src*a + dst: the destination survives whole, so light adds.
				blend.color.srcFactor = BlendFactor::SrcAlpha;
				blend.color.dstFactor = BlendFactor::One;
				break;
			case wgpu2d::BlendMode::Premultiplied:
				// src + dst*(1-a): the source is already scaled by coverage.
				blend.color.srcFactor = BlendFactor::One;
				blend.color.dstFactor = BlendFactor::OneMinusSrcAlpha;
				break;
			case wgpu2d::BlendMode::Mask:
				// 0 + dst*a: the source only scales what is there, by its
				// coverage. Nothing of its colour arrives.
				blend.color.srcFactor = BlendFactor::Zero;
				blend.color.dstFactor = BlendFactor::SrcAlpha;
				break;
			default:
				// src*a + dst*(1-a): gl2d's "over".
				blend.color.srcFactor = BlendFactor::SrcAlpha;
				blend.color.dstFactor = BlendFactor::OneMinusSrcAlpha;
				break;
		}
		blend.alpha.operation = BlendOperation::Add;
		blend.alpha.srcFactor = BlendFactor::One;
		blend.alpha.dstFactor = BlendFactor::OneMinusSrcAlpha;
		if (key.blend == wgpu2d::BlendMode::Mask)
		{
			// The destination's alpha kept: masked to black is still opaque
			// black. A target stays premultiplied: (C*a, a) becomes (C*m*a, a),
			// the colour C*m at the same coverage.
			blend.alpha.srcFactor = BlendFactor::Zero;
			blend.alpha.dstFactor = BlendFactor::One;
		}

		// A pipeline may only draw into an attachment of the format it was
		// built for. R1 established that this is not caught at creation: the
		// pipeline is built happily and the mismatch is only reported when it
		// meets the attachment inside a pass. Keying on the format is what
		// makes that structural instead of a rule in a comment.
		ColorTargetState colorTarget = Default;
		colorTarget.format = key.format;
		colorTarget.blend = &blend;
		colorTarget.writeMask = ColorWriteMask::All;

		FragmentState fragment = Default;
		fragment.module = fragmentModule;
		fragment.entryPoint = StringView("fs_main");
		fragment.constantCount = 0;
		fragment.constants = nullptr;
		fragment.targetCount = 1;
		fragment.targets = &colorTarget;
		desc.fragment = &fragment;

		// No depth/stencil (gl2d disabled depth testing) and no multisampling.
		desc.depthStencil = nullptr;
		desc.multisample.count = 1;
		desc.multisample.mask = 0xFFFFFFFFu;
		desc.multisample.alphaToCoverageEnabled = false;

		// The explicit layout: group 0 texture + sampler, group 1 camera.
		desc.layout = g.pipelineLayout;

		RenderPipeline pipeline = nullptr;
		bool invalid = false;
		{
			// The densest descriptor in the file: a layout that disagrees
			// with the shader, or a bad blend factor, lands here as one
			// message naming this variant rather than as a null handle.
			ErrorScope scope(label);
			pipeline = g.device.createRenderPipeline(desc);
			// A rejected pipeline is not null -- createRenderPipeline hands
			// back a live handle in an invalid state, and every later
			// setPipeline reports it again. Only the scope can tell the
			// difference, and an invalid variant must never reach the cache
			// or it is re-served every frame.
			invalid = scope.failed();
		}

		// Neither module is released here: the vertex one is g.quadShaderModule
		// and the fragment one belongs to the shader registry. Both outlive
		// every pipeline built from them, and wgpuShutdown owns their ends.

		if (!pipeline || invalid)
		{
			std::cerr << "WebGPU: pipeline variant '" << label << "' is not usable\n";
			if (pipeline) { pipeline.release(); }
			return nullptr;
		}
		return pipeline;
	}

	// The cache. A linear scan over a handful of entries beats writing a hash
	// for a two-field key, and lookups happen at run boundaries, not per quad.
	RenderPipeline getQuadPipeline(const PipelineKey &key)
	{
		for (const PipelineEntry &entry : g.quadPipelines)
		{
			if (entry.key == key) { return entry.pipeline; }
		}

		// Built on demand. The label is what a validation message and a GPU
		// capture show, so it names both halves of the key.
		PipelineEntry entry;
		entry.key = key;
		entry.label = std::string("pipeline (") + shaderNameFor(key.shader) + ", "
			+ formatName(key.format) + ", " + blendName(key.blend) + ")";

		// Counted because compiling a pipeline mid-frame is expensive. With
		// the failure cached below, this should read 1 for a variant's first
		// frame and 0 thereafter -- anything larger means something is asking
		// for keys that keep changing.
		++framePerf.pipelineBuilds;
		entry.pipeline = createQuadPipelineVariant(key, entry.label.c_str());
		const bool built = (entry.pipeline != nullptr);

		if (!built)
		{
			++framePerf.pipelineFailures;
			// Once, loudly, and then never again: the entry is cached either
			// way, so this cannot become a per-frame message any more than it
			// can become a per-frame compile.
			std::cerr << "WebGPU: " << entry.label << " could not be built."
				<< " Draws needing it are skipped for the rest of this run.\n";
			std::cerr.flush();
		}

		g.quadPipelines.push_back(std::move(entry));
		if (built)
		{
			int usable = 0;
			for (const PipelineEntry &e : g.quadPipelines) { if (e.pipeline) { ++usable; } }
			std::cout << "WebGPU " << g.quadPipelines.back().label << " created ("
				<< usable << " variant" << (usable == 1 ? "" : "s") << " cached)\n";
			std::cout.flush();
		}
		return g.quadPipelines.back().pipeline;
	}

	// ---------------------------------------------------------------------
	// Textures
	// ---------------------------------------------------------------------

	// N4: mip levels are built by a compute pass, not on the CPU.
	//
	// What a compute pipeline *is*, next to the render pipeline above it: a
	// render pipeline is a conveyor with two programmable holes in it, and the
	// hardware decides how many times each runs -- the vertex stage once per
	// vertex, the fragment stage once per pixel the rasteriser covered. Where
	// their output goes is fixed too: a fragment shader writes the one pixel it
	// was created for and nothing else, which is the same rule that stopped the
	// cloak reading the scene behind it.
	//
	// A compute pipeline removes the conveyor. There are no vertices, no
	// rasteriser and no colour target -- one shader stage, an explicit
	// invocation count from dispatchWorkgroups, and writes that go wherever the
	// shader addresses. Everything else is the machinery already here: a bind
	// group layout, a pipeline layout, a pass on the same encoder as the render
	// pass, the same submit.

	// Levels from WxH down to 1x1, which is what the old CPU loop counted by
	// halving until it ran out.
	uint32_t mipLevelCountFor(int width, int height)
	{
		uint32_t levels = 1;
		int w = width, h = height;
		while (w > 1 || h > 1)
		{
			w = std::max(1, w / 2);
			h = std::max(1, h / 2);
			levels++;
		}
		return levels;
	}

	bool ensureMipmapPipeline()
	{
		if (g.mipmapPipeline) { return true; }
		if (g.mipmapPipelineFailed) { return false; }

		ErrorScope scope("mipmap compute pipeline");

		g.mipmapShaderModule = createShaderModule(mipmapShaderWGSL, "mipmap.wgsl");
		if (!g.mipmapShaderModule) { g.mipmapPipelineFailed = true; return false; }

		// Two bindings, and they are a matched pair the shader also declares.
		// The read side is an ordinary sampled-texture binding with no sampler:
		// textureLoad wants exact texels, and a sampler would be a filtering
		// decision this shader has no use for.
		BindGroupLayoutEntry entries[2];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].visibility = ShaderStage::Compute;
		entries[0].texture.sampleType = TextureSampleType::Float;
		entries[0].texture.viewDimension = TextureViewDimension::_2D;
		entries[0].texture.multisampled = false;
		entries[0].buffer.type = BufferBindingType::BindingNotUsed;
		entries[0].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[0].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		// The write side. A storage texture's format is part of the binding's
		// *type* -- the shader spells out rgba8unorm and so does this, and they
		// have to agree, which is not true of an ordinary sampled binding where
		// the format comes from whatever view is bound. WriteOnly is the only
		// access core WebGPU guarantees for this format.
		entries[1] = Default;
		entries[1].binding = 1;
		entries[1].visibility = ShaderStage::Compute;
		entries[1].storageTexture.access = StorageTextureAccess::WriteOnly;
		entries[1].storageTexture.format = TextureFormat::RGBA8Unorm;
		entries[1].storageTexture.viewDimension = TextureViewDimension::_2D;
		entries[1].buffer.type = BufferBindingType::BindingNotUsed;
		entries[1].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[1].texture.sampleType = TextureSampleType::BindingNotUsed;

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("mipmap bind group layout");
		layoutDesc.entryCount = 2;
		layoutDesc.entries = entries;
		g.mipmapBindGroupLayout = g.device.createBindGroupLayout(layoutDesc);

		PipelineLayoutDescriptor pipelineLayoutDesc = Default;
		pipelineLayoutDesc.label = StringView("mipmap pipeline layout");
		pipelineLayoutDesc.bindGroupLayoutCount = 1;
		WGPUBindGroupLayout layouts[1] = { g.mipmapBindGroupLayout };
		pipelineLayoutDesc.bindGroupLayouts = layouts;
		g.mipmapPipelineLayout = g.device.createPipelineLayout(pipelineLayoutDesc);

		// The whole descriptor, next to RenderPipelineDescriptor's dozen fields:
		// a layout and one programmable stage. There is nothing else to say
		// because there is no fixed-function pipeline left to configure.
		ComputePipelineDescriptor desc = Default;
		desc.label = StringView("mipmap");
		desc.layout = g.mipmapPipelineLayout;
		desc.compute.nextInChain = nullptr;
		desc.compute.module = g.mipmapShaderModule;
		desc.compute.entryPoint = StringView("cs_main");
		desc.compute.constantCount = 0;
		desc.compute.constants = nullptr;
		g.mipmapPipeline = g.device.createComputePipeline(desc);

		if (!g.mipmapPipeline || scope.failed())
		{
			std::cerr << "WebGPU: the mipmap compute pipeline could not be built; "
				"textures will have level 0 only\n";
			g.mipmapPipelineFailed = true;
			return false;
		}
		return true;
	}

	// Fills levels 1..n-1 of a texture that already holds level 0.
	//
	// One pass, one dispatch per level, in order. Level 2 reads what level 1's
	// dispatch wrote, and that is safe without anything being said here:
	// WebGPU orders dispatches within a pass and inserts the barrier itself.
	// Under Vulkan or Metal directly this is where a memory barrier would go.
	bool generateMipmaps(Texture texture, int width, int height, uint32_t levelCount,
		const char *label)
	{
		if (levelCount <= 1) { return true; }
		if (!ensureMipmapPipeline()) { return false; }

		ErrorScope scope("mipmap generation");

		CommandEncoderDescriptor encoderDesc = Default;
		encoderDesc.label = StringView(label);
		CommandEncoder encoder = g.device.createCommandEncoder(encoderDesc);
		if (!encoder) { return false; }

		ComputePassDescriptor passDesc = Default;
		passDesc.label = StringView("mipmap generation");
		passDesc.timestampWrites = nullptr;
		ComputePassEncoder pass = encoder.beginComputePass(passDesc);
		pass.setPipeline(g.mipmapPipeline);

		// A storage-texture binding is one mip level, so every level needs its
		// own view. That is the practical difference from the sampled view
		// below, which spans all of them: sampling picks a level at run time,
		// storing has to be told which one at bind time.
		std::vector<TextureView> views;
		std::vector<BindGroup> groups;
		views.reserve((size_t)levelCount * 2);
		groups.reserve(levelCount);

		int w = width, h = height;
		for (uint32_t level = 1; level < levelCount; level++)
		{
			const int dstW = std::max(1, w / 2);
			const int dstH = std::max(1, h / 2);

			TextureViewDescriptor viewDesc = Default;
			viewDesc.format = TextureFormat::RGBA8Unorm;
			viewDesc.dimension = TextureViewDimension::_2D;
			viewDesc.mipLevelCount = 1;
			viewDesc.baseArrayLayer = 0;
			viewDesc.arrayLayerCount = 1;
			viewDesc.aspect = TextureAspect::All;

			viewDesc.label = StringView("mip source");
			viewDesc.baseMipLevel = level - 1;
			viewDesc.usage = TextureUsage::TextureBinding;
			TextureView srcView = texture.createView(viewDesc);

			viewDesc.label = StringView("mip destination");
			viewDesc.baseMipLevel = level;
			viewDesc.usage = TextureUsage::StorageBinding;
			TextureView dstView = texture.createView(viewDesc);
			if (!srcView || !dstView) { break; }
			views.push_back(srcView);
			views.push_back(dstView);

			BindGroupEntry bindEntries[2];
			bindEntries[0] = Default;
			bindEntries[0].binding = 0;
			bindEntries[0].textureView = srcView;
			bindEntries[1] = Default;
			bindEntries[1].binding = 1;
			bindEntries[1].textureView = dstView;

			BindGroupDescriptor groupDesc = Default;
			groupDesc.label = StringView("mip level");
			groupDesc.layout = g.mipmapBindGroupLayout;
			groupDesc.entryCount = 2;
			groupDesc.entries = bindEntries;
			BindGroup group = g.device.createBindGroup(groupDesc);
			if (!group) { break; }
			groups.push_back(group);

			pass.setBindGroup(0, group, 0, nullptr);

			// Workgroups, not invocations. The shader is @workgroup_size(8, 8),
			// so this rounds up and the shader's bounds check throws away the
			// overhang -- a 3x3 level still launches all 64 invocations.
			pass.dispatchWorkgroups(((uint32_t)dstW + 7) / 8, ((uint32_t)dstH + 7) / 8, 1);

			w = dstW;
			h = dstH;
		}

		pass.end();

		CommandBufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView("mipmap generation");
		CommandBuffer commands = encoder.finish(bufferDesc);
		g.queue.submit(1, &commands);

		commands.release();
		pass.release();
		encoder.release();
		for (BindGroup &group : groups) { group.release(); }
		for (TextureView &view : views) { view.release(); }

		return !scope.failed();
	}

	// Uploads RGBA8 pixels (rows top-first, tightly packed) as a texture,
	// has the device build the mip levels when requested, creates its view,
	// and builds the bind group that hands the view plus the right sampler to
	// the shader. Returns a handle (id 0 on failure) into the registry.
	wgpu2d::Texture createTextureFromPixels(const unsigned char *pixels, int width, int height,
		const char *label, bool pixelated, bool useMipMaps, bool repeat = false)
	{
		if (!g.device || width <= 0 || height <= 0 || !pixels)
		{
			std::cerr << "WebGPU: createTextureFromPixels called with no device or empty image (" << label << ")\n";
			return wgpu2d::Texture{};
		}

		// Only level 0 is uploaded now; the rest are computed on the device.
		const uint32_t levelCount = useMipMaps ? mipLevelCountFor(width, height) : 1;

		// One scope for texture, view and bind group: they are created
		// together and the label names the image either way.
		ErrorScope scope(label);

		TextureEntry entry;
		entry.width = width;
		entry.height = height;

		// The texture: GPU pixel storage with a fixed format and usage.
		// TextureBinding: a shader may sample it. CopyDst: the queue may write it.
		TextureDescriptor texDesc = Default;
		texDesc.label = StringView(label);
		texDesc.dimension = TextureDimension::_2D;
		texDesc.size.width = (uint32_t)width;
		texDesc.size.height = (uint32_t)height;
		texDesc.size.depthOrArrayLayers = 1;
		texDesc.format = TextureFormat::RGBA8Unorm;
		entry.format = texDesc.format;
		++framePerf.texturesCreated;
		texDesc.mipLevelCount = levelCount;
		texDesc.sampleCount = 1;
		// StorageBinding is what lets the compute shader write into this. Usage
		// is a property of the whole texture, not of a level, so asking for
		// mips asks for it everywhere -- including level 0, which is only ever
		// written by the queue.
		// CopySrc (ourCraft): gl2d's readTextureData reads the texels back,
		// and the block loader does that to every block texture.
		texDesc.usage = TextureUsage::TextureBinding | TextureUsage::CopyDst | TextureUsage::CopySrc;
		if (levelCount > 1) { texDesc.usage = texDesc.usage | TextureUsage::StorageBinding; }
		texDesc.viewFormatCount = 0;
		texDesc.viewFormats = nullptr;
		entry.texture = g.device.createTexture(texDesc);
		if (!entry.texture)
		{
			std::cerr << "WebGPU: createTexture returned null (" << label << ")\n";
			return wgpu2d::Texture{};
		}

		// Upload level 0. This is glTexImage2D; the levels below it used to be
		// built here on the CPU and are now a compute pass, which is the whole
		// of glGenerateMipmap's job done explicitly.
		{
			TexelCopyTextureInfo destination = Default;
			destination.texture = entry.texture;
			destination.mipLevel = 0;
			destination.origin.x = 0;
			destination.origin.y = 0;
			destination.origin.z = 0;
			destination.aspect = TextureAspect::All;

			TexelCopyBufferLayout sourceLayout = Default;
			sourceLayout.offset = 0;
			sourceLayout.bytesPerRow = 4 * (uint32_t)width;
			sourceLayout.rowsPerImage = (uint32_t)height;

			Extent3D extent = Default;
			extent.width = (uint32_t)width;
			extent.height = (uint32_t)height;
			extent.depthOrArrayLayers = 1;

			g.queue.writeTexture(destination, pixels, (size_t)4 * width * height,
				sourceLayout, extent);
		}

		// The queue is ordered against itself, so the writeTexture above is
		// visible to the dispatches below without anything being waited on.
		generateMipmaps(entry.texture, width, height, levelCount, label);

		// The view the shader samples through: all mip levels.
		TextureViewDescriptor viewDesc = Default;
		viewDesc.label = StringView(label);
		viewDesc.format = TextureFormat::RGBA8Unorm;
		viewDesc.dimension = TextureViewDimension::_2D;
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = levelCount;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect = TextureAspect::All;
		viewDesc.usage = TextureUsage::TextureBinding;
		entry.view = entry.texture.createView(viewDesc);
		if (!entry.view)
		{
			std::cerr << "WebGPU: createView returned null (" << label << ")\n";
			entry.texture.release();
			return wgpu2d::Texture{};
		}

		// The bind group: this view and the sampler for its filtering mode,
		// in the slots the layout declared.
		BindGroupEntry bindEntries[2];
		bindEntries[0] = Default;
		bindEntries[0].binding = 0;
		bindEntries[0].textureView = entry.view;
		bindEntries[0].buffer = nullptr;
		bindEntries[0].sampler = nullptr;
		bindEntries[1] = Default;
		bindEntries[1].binding = 1;
		bindEntries[1].sampler = repeat
			? (pixelated ? g.samplerPixelatedRepeat : g.samplerLinearRepeat)
			: (pixelated ? g.samplerPixelated : g.samplerLinear);
		bindEntries[1].buffer = nullptr;
		bindEntries[1].textureView = nullptr;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView(label);
		groupDesc.layout = g.textureBindGroupLayout;
		groupDesc.entryCount = 2;
		groupDesc.entries = bindEntries;
		entry.bindGroup = g.device.createBindGroup(groupDesc);
		if (!entry.bindGroup)
		{
			std::cerr << "WebGPU: createBindGroup returned null (" << label << ")\n";
			entry.view.release();
			entry.texture.release();
			return wgpu2d::Texture{};
		}

		textures.push_back(entry);
		// ourCraft loads thousands of block textures; opt in with WGPU2D_LOG_TEXTURES=1.
		static const bool logTextures = std::getenv("WGPU2D_LOG_TEXTURES") != nullptr;
		if (logTextures)
		{
			std::cout << "WebGPU texture " << textures.size() << ": " << width << "x" << height
				<< ", " << levelCount << " mip level" << (levelCount == 1 ? "" : "s")
				<< (pixelated ? ", nearest" : ", linear") << " (" << label << ")\n";
		}
		wgpu2d::Texture handle;
		handle.id = (uint32_t)textures.size();
		return handle;
	}

	// 10: a texture with nothing in it that the renderer may draw into. Two
	// usages matter: RenderAttachment (a pass may target it) and
	// TextureBinding (the shader may sample it afterwards). Its format is the
	// surface's, because the sprite pipeline was built for that format and a
	// pipeline can only draw into a matching attachment.
	//
	// reuseId re-fills an existing registry slot so handles stay valid across
	// a resize; 0 appends a new one.
	// `storage` makes it writable by a compute shader instead of by a render
	// pass. That forces RGBA8Unorm rather than the surface's format: core
	// WebGPU's storage-texture formats do not include BGRA8Unorm, which is what
	// the surface is here, and nothing renders *into* a scratch texture anyway.
	wgpu2d::Texture createRenderTarget(int width, int height, bool pixelated, const char *label,
		uint32_t reuseId = 0, bool storage = false)
	{
		if (!g.device || width <= 0 || height <= 0)
		{
			std::cerr << "WebGPU: createRenderTarget with no device or empty size (" << label << ")\n";
			return wgpu2d::Texture{};
		}

		ErrorScope scope(label);

		TextureEntry entry;
		entry.width = width;
		entry.height = height;
		entry.pixelated = pixelated;
		entry.renderTarget = true;
		entry.needsClear = true; // a fresh target's contents are undefined

		TextureDescriptor texDesc = Default;
		texDesc.label = StringView(label);
		texDesc.dimension = TextureDimension::_2D;
		texDesc.size.width = (uint32_t)width;
		texDesc.size.height = (uint32_t)height;
		texDesc.size.depthOrArrayLayers = 1;
		TextureFormat scratchFormat = TextureFormat::RGBA8Unorm;
		texDesc.format = storage ? scratchFormat : g.surfaceFormat;
		entry.format = texDesc.format;
		++framePerf.texturesCreated;
		texDesc.mipLevelCount = 1;
		texDesc.sampleCount = 1;
		// CopySrc for the same reason the surface has it: a render target is a
		// frame too, and reading one back is how milestone 12's blend numbers
		// were measured.
		// Built up with statements rather than a ternary: these are flag enums
		// and a conditional between two of them collapses to the underlying
		// integer, which drops the bit rather than failing to compile.
		// CopyDst so FrameBuffer::write can put a target's texels back.
		WGPUTextureUsage usage = TextureUsage::TextureBinding | TextureUsage::CopySrc | TextureUsage::CopyDst;
		if (storage) { usage = usage | (WGPUTextureUsage)TextureUsage::StorageBinding; }
		else { usage = usage | (WGPUTextureUsage)TextureUsage::RenderAttachment; }
		texDesc.usage = usage;
		texDesc.viewFormatCount = 0;
		texDesc.viewFormats = nullptr;
		entry.texture = g.device.createTexture(texDesc);
		if (!entry.texture)
		{
			std::cerr << "WebGPU: createTexture (render target) returned null (" << label << ")\n";
			return wgpu2d::Texture{};
		}

		TextureViewDescriptor viewDesc = Default;
		viewDesc.label = StringView(label);
		viewDesc.format = entry.format;
		viewDesc.dimension = TextureViewDimension::_2D;
		viewDesc.baseMipLevel = 0;
		viewDesc.mipLevelCount = 1;
		viewDesc.baseArrayLayer = 0;
		viewDesc.arrayLayerCount = 1;
		viewDesc.aspect = TextureAspect::All;
		WGPUTextureUsage viewUsage = TextureUsage::TextureBinding;
		if (!storage) { viewUsage = viewUsage | (WGPUTextureUsage)TextureUsage::RenderAttachment; }
		viewDesc.usage = viewUsage;
		entry.view = entry.texture.createView(viewDesc);
		if (!entry.view)
		{
			std::cerr << "WebGPU: createView (render target) returned null (" << label << ")\n";
			entry.texture.release();
			return wgpu2d::Texture{};
		}

		// A second view of the same texels, declared for writing. A view
		// carries the usages it may be bound for, and wgpu refuses a compute
		// shader writing through a view that only says TextureBinding -- so a
		// texture that is both sampled as a sprite and written by a dispatch
		// needs one view of each rather than one view claiming both.
		if (storage)
		{
			viewDesc.label = StringView("storage view");
			viewDesc.usage = TextureUsage::StorageBinding;
			entry.storageView = entry.texture.createView(viewDesc);
			if (!entry.storageView)
			{
				std::cerr << "WebGPU: createView (storage) returned null (" << label << ")\n";
				entry.view.release();
				entry.texture.release();
				return wgpu2d::Texture{};
			}
		}

		BindGroupEntry bindEntries[2];
		bindEntries[0] = Default;
		bindEntries[0].binding = 0;
		bindEntries[0].textureView = entry.view;
		bindEntries[0].buffer = nullptr;
		bindEntries[0].sampler = nullptr;
		bindEntries[1] = Default;
		bindEntries[1].binding = 1;
		bindEntries[1].sampler = pixelated ? g.samplerPixelated : g.samplerLinear;
		bindEntries[1].buffer = nullptr;
		bindEntries[1].textureView = nullptr;

		BindGroupDescriptor groupDesc = Default;
		groupDesc.label = StringView(label);
		groupDesc.layout = g.textureBindGroupLayout;
		groupDesc.entryCount = 2;
		groupDesc.entries = bindEntries;
		entry.bindGroup = g.device.createBindGroup(groupDesc);
		if (!entry.bindGroup)
		{
			std::cerr << "WebGPU: createBindGroup (render target) returned null (" << label << ")\n";
			entry.view.release();
			entry.texture.release();
			return wgpu2d::Texture{};
		}

		uint32_t id = reuseId;
		if (id != 0 && id <= textures.size())
		{
			TextureEntry &old = textures[id - 1];
			if (old.bindGroup) { old.bindGroup.release(); }
			if (old.view) { old.view.release(); }
			if (old.texture) { old.texture.release(); }
			old = entry;
		}
		else
		{
			textures.push_back(entry);
			id = (uint32_t)textures.size();
		}

		std::cout << "WebGPU render target " << id << ": " << width << "x" << height
			<< (pixelated ? ", nearest" : ", linear") << " (" << label << ")\n";
		std::cout.flush();
		wgpu2d::Texture handle;
		handle.id = id;
		return handle;
	}

	// 10, then F7: keeps the frame's target matching the surface's current
	// size. It exists for two unrelated reasons now -- a render scale below 1,
	// and a final effect -- and either is enough.
	void ensureScaledTarget()
	{
		if (g.renderScale >= 1.f && g.finalEffectId == 0)
		{
			g.scaledTargetId = 0;
			return;
		}

		const int w = std::max(1, (int)std::lround(g.surfaceWidth * g.renderScale));
		const int h = std::max(1, (int)std::lround(g.surfaceHeight * g.renderScale));

		// Nearest is the pixel-perfect upscale milestone 10 wanted. A final
		// effect wants the opposite: it bends the coordinate, so it asks for
		// positions between texels, and nearest turns that into rows of
		// unequal thickness that crawl as the camera moves.
		const bool linear = g.finalEffectId != 0;

		// Checked against the *slot*, not against scaledTargetId, because that
		// is 0 whenever the target is not currently in use. Checking the id
		// meant every switch-off and switch-on rebuilt a screen-sized texture
		// that was already there and still the right size.
		if (g.frameTargetSlot && g.frameTargetSlot <= textures.size()
			&& textures[g.frameTargetSlot - 1].width == w
			&& textures[g.frameTargetSlot - 1].height == h
			&& g.scaledTargetLinear == linear)
		{
			g.scaledTargetId = g.frameTargetSlot;
			return;
		}

		const char *label = linear ? "frame target" : "low-res target";
		wgpu2d::Texture target = createRenderTarget(w, h, !linear, label, g.frameTargetSlot);
		g.scaledTargetId = target.id;
		g.frameTargetSlot = target.id;
		++g.frameTargetGeneration;
		g.scaledTargetLinear = linear;
	}

	// Reads a whole file, like gl2d's loaders do before decoding.
	bool readBinaryFile(const char *path, std::vector<unsigned char> &out)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) { return false; }
		file.seekg(0, std::ios::end);
		const std::streamoff size = file.tellg();
		file.seekg(0, std::ios::beg);
		out.resize((size_t)size);
		if (size > 0) { file.read((char *)out.data(), size); }
		return true;
	}

	// gl2d's createFromFileDataWithPixelPadding, verbatim apart from the
	// missing load-time flip (see the milestone 4 orientation decision;
	// the padding is orientation-independent). Re-lays the image out with a
	// 2-pixel gutter around every blockSize x blockSize cell and duplicates
	// each cell's edge pixels into its gutter, so filtering never bleeds a
	// neighbor in.
	wgpu2d::Texture createPaddedTextureFromFileData(const unsigned char *image_file_data, size_t image_file_size,
		int blockSize, bool pixelated, bool useMipMaps, const char *label)
	{
		stbi_set_flip_vertically_on_load(0);

		int width = 0;
		int height = 0;
		int channels = 0;

		const unsigned char *decodedImage = stbi_load_from_memory(image_file_data, (int)image_file_size, &width, &height, &channels, 4);
		if (!decodedImage)
		{
			std::cerr << "WebGPU: cannot decode image " << label << ": " << stbi_failure_reason() << "\n";
			return wgpu2d::Texture{};
		}

		int newW = width + ((width * 2) / blockSize);
		int newH = height + ((height * 2) / blockSize);

		unsigned char *newData = new unsigned char[newW * newH * 4]{};

		auto getNew = [newData, newW](int x, int y, int c)
		{
			return &newData[4 * (x + (y * newW)) + c];
		};

		int newDataCursor = 0;
		int dataCursor = 0;

		//first copy data
		for (int y = 0; y < newH; y++)
		{
			int yNo = 0;
			if ((y == 0 || y == newH - 1
				|| ((y) % (blockSize + 2)) == 0 ||
				((y + 1) % (blockSize + 2)) == 0
				))
			{
				yNo = 1;
			}

			for (int x = 0; x < newW; x++)
			{
				if (
					yNo ||

					((
						x == 0 || x == newW - 1
						|| (x % (blockSize + 2)) == 0 ||
						((x + 1) % (blockSize + 2)) == 0
						)
						)

					)
				{
					newData[newDataCursor++] = 0;
					newData[newDataCursor++] = 0;
					newData[newDataCursor++] = 0;
					newData[newDataCursor++] = 0;
				}
				else
				{
					newData[newDataCursor++] = decodedImage[dataCursor++];
					newData[newDataCursor++] = decodedImage[dataCursor++];
					newData[newDataCursor++] = decodedImage[dataCursor++];
					newData[newDataCursor++] = decodedImage[dataCursor++];
				}

			}

		}

		//then add margins


		for (int x = 1; x < newW - 1; x++)
		{
			//copy on left
			if (x == 1 ||
				(x % (blockSize + 2)) == 1
				)
			{
				for (int y = 0; y < newH; y++)
				{
					*getNew(x - 1, y, 0) = *getNew(x, y, 0);
					*getNew(x - 1, y, 1) = *getNew(x, y, 1);
					*getNew(x - 1, y, 2) = *getNew(x, y, 2);
					*getNew(x - 1, y, 3) = *getNew(x, y, 3);
				}

			}
			else //copy on rigght
				if (x == newW - 2 ||
					(x % (blockSize + 2)) == blockSize
					)
				{
					for (int y = 0; y < newH; y++)
					{
						*getNew(x + 1, y, 0) = *getNew(x, y, 0);
						*getNew(x + 1, y, 1) = *getNew(x, y, 1);
						*getNew(x + 1, y, 2) = *getNew(x, y, 2);
						*getNew(x + 1, y, 3) = *getNew(x, y, 3);
					}
				}
		}

		for (int y = 1; y < newH - 1; y++)
		{
			if (y == 1 ||
				(y % (blockSize + 2)) == 1
				)
			{
				for (int x = 0; x < newW; x++)
				{
					*getNew(x, y - 1, 0) = *getNew(x, y, 0);
					*getNew(x, y - 1, 1) = *getNew(x, y, 1);
					*getNew(x, y - 1, 2) = *getNew(x, y, 2);
					*getNew(x, y - 1, 3) = *getNew(x, y, 3);
				}
			}
			else
				if (y == newH - 2 ||
					(y % (blockSize + 2)) == blockSize
					)
				{
					for (int x = 0; x < newW; x++)
					{
						*getNew(x, y + 1, 0) = *getNew(x, y, 0);
						*getNew(x, y + 1, 1) = *getNew(x, y, 1);
						*getNew(x, y + 1, 2) = *getNew(x, y, 2);
						*getNew(x, y + 1, 3) = *getNew(x, y, 3);
					}
				}

		}

		wgpu2d::Texture t = createTextureFromPixels(newData, newW, newH, label, pixelated, useMipMaps);

		stbi_image_free((void *)decodedImage);
		delete[] newData;
		return t;
	}

	// ---------------------------------------------------------------------
	// Batch: accumulate on the CPU, upload and draw at flush
	// ---------------------------------------------------------------------

	// gl2d's per-corner CPU transform (renderRectangleAbsRotation), collapsed
	// into one matrix:
	//   1. subtract the camera position          (v.x -= cam.x; v.y += cam.y on the flipped y)
	//   2. scale about the screen center by zoom  (scaleAroundPoint with center (w/2, -h/2))
	//   3. pixels to clip space                   (x: 2x/w - 1;  y: 2y/h + 1 on the flipped y)
	// For a world point (x, y) with y down that works out to
	//   ndc.x =  (2 zoom / w) x  - 2 zoom cam.x / w - zoom
	//   ndc.y = -(2 zoom / h) y  + 2 zoom cam.y / h + zoom
	// Checked against gl2d's own functions over 100k random inputs (milestone 5).
	glm::mat4 buildViewProj(const wgpu2d::Camera &cam, float w, float h)
	{
		const float z = cam.zoom;
		glm::mat4 m(1.0f);                     // glm is column-major: m[col][row]
		m[0][0] = 2.0f * z / w;
		m[1][1] = -2.0f * z / h;
		m[3][0] = -2.0f * z * cam.position.x / w - z;
		m[3][1] = 2.0f * z * cam.position.y / h + z;
		return m;
	}

	bool sameCamera(const wgpu2d::Camera &a, const wgpu2d::Camera &b)
	{
		return a.position == b.position && a.zoom == b.zoom;
	}

	// gl2d's rotateAroundPoint, verbatim. It works in gl2d's y-flipped space
	// (callers pass corners with y negated) and negates the pivot's y to
	// match. Ported as-is so positive degrees turn the same way they do today.
	glm::vec2 rotateAroundPoint(glm::vec2 vec, glm::vec2 point, const float degrees)
	{
		point.y = -point.y;
		float a = glm::radians(degrees);
		float s = sinf(a);
		float c = cosf(a);
		vec.x -= point.x;
		vec.y -= point.y;
		float newx = vec.x * c - vec.y * s;
		float newy = vec.x * s + vec.y * c;
		// translate point back:
		vec.x = newx + point.x;
		vec.y = newy + point.y;
		return vec;
	}

	// Makes sure the vertex buffer can hold `bytes`. A GPU buffer cannot be
	// resized, so growth means creating a bigger one (doubling) and releasing
	// the old one; WebGPU keeps the old buffer alive until submitted work
	// that reads it has finished. Never shrinks.
	bool ensureVertexBufferCapacity(uint64_t bytes)
	{
		if (g.vertexBuffer && bytes <= g.vertexBufferCapacityBytes) { return true; }

		uint64_t capacity = g.vertexBufferCapacityBytes ? g.vertexBufferCapacityBytes : 64 * sizeof(Vertex);
		while (capacity < bytes) { capacity *= 2; }

		// Past the early return above, so this is a growth step, not a frame
		// step: doubling means a handful of these over a run, and the scope's
		// stall never lands in the steady state.
		ErrorScope scope("batch vertex buffer");

		BufferDescriptor desc = Default;
		desc.label = StringView("batch vertices");
		desc.usage = BufferUsage::Vertex | BufferUsage::CopyDst;
		desc.size = capacity;
		desc.mappedAtCreation = false;
		Buffer newBuffer = g.device.createBuffer(desc);
		if (!newBuffer)
		{
			std::cerr << "WebGPU: createBuffer (" << capacity << " bytes) returned null\n";
			return false;
		}

		if (g.vertexBuffer) { g.vertexBuffer.release(); }
		g.vertexBuffer = newBuffer;
		g.vertexBufferCapacityBytes = capacity;
		std::cout << "WebGPU vertex buffer capacity: " << capacity / sizeof(Vertex) << " vertices ("
			<< capacity << " bytes)\n";
		std::cout.flush();
		return true;
	}

	// Makes sure the open render pass is the one for `target` (0 = surface,
	// otherwise a texture registry id), beginning it if needed. Passes cannot
	// nest and belong to one attachment, so a change of target ends the
	// current pass and starts another; StoreOp::Store plus LoadOp::Load means
	// nothing drawn earlier is lost.
	//
	// The load operation is where gl2d's glClear went: the surface and the
	// low-res stand-in clear on their first pass of the frame (to the color
	// clearScreen recorded), a user's FrameBuffer loads its previous contents
	// unless it asked to be cleared.
	bool ensurePassBegun(uint32_t target)
	{
		if (!g.frameOpen) { return false; }
		if (g.framePass && g.framePassTarget == target) { return true; }

		// Someone wants the surface while the frame is sitting in the low-res
		// target: upscale it first, so the UI lands on top of the world.
		if (target == 0 && g.scaledTargetDrawn && !g.compositing)
		{
			compositeScaledTarget();
			if (g.framePass && g.framePassTarget == 0) { return true; }
		}

		if (g.framePass)
		{
			g.framePass.end();
			g.framePass.release();
			g.framePass = nullptr;
		}

		TextureView view = nullptr;
		bool clear = false;
		glm::vec4 clearColor = {0, 0, 0, 0};
		if (target == 0)
		{
			view = g.frameView;
			clear = !g.surfaceCleared;
			clearColor = g.clearColor;
			g.surfaceCleared = true;
		}
		else if (target <= textures.size())
		{
			TextureEntry &entry = textures[target - 1];
			view = entry.view;
			if (isFrameTarget(target))
			{
				clear = !g.scaledCleared;
				clearColor = g.clearColor;
				g.scaledCleared = true;
			}
			else
			{
				clear = entry.needsClear;
			}
			entry.needsClear = false;
		}
		if (!view)
		{
			std::cerr << "WebGPU: no view for render target " << target << "\n";
			return false;
		}

		RenderPassColorAttachment colorAttachment = Default;
		colorAttachment.view = view;
		colorAttachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED; // required for a 2D target
		colorAttachment.resolveTarget = nullptr;
		colorAttachment.loadOp = clear ? LoadOp::Clear : LoadOp::Load;
		colorAttachment.storeOp = StoreOp::Store;
		colorAttachment.clearValue.r = clearColor.r;
		colorAttachment.clearValue.g = clearColor.g;
		colorAttachment.clearValue.b = clearColor.b;
		colorAttachment.clearValue.a = clearColor.a;

		RenderPassDescriptor passDesc = Default;
		passDesc.label = StringView(target == 0 ? "surface pass" : "render target pass");
		passDesc.colorAttachmentCount = 1;
		passDesc.colorAttachments = &colorAttachment;
		passDesc.depthStencilAttachment = nullptr;
		passDesc.occlusionQuerySet = nullptr;
		passDesc.timestampWrites = nullptr;

		g.framePass = g.frameEncoder.beginRenderPass(passDesc);
		g.framePassTarget = target;
		return (bool)g.framePass;
	}

	// Sets the whole pending batch aside and puts it back on the way out.
	//
	// Two callers need to borrow the batch for one quad of their own: the
	// scaled-target composite and drawFullscreenEffect. Both used to hand-roll
	// the swap, and both got it wrong, because the batch is *eight* parallel
	// arrays and they each swapped four or five of them. What is left behind
	// does not vanish -- it stays at index 0, and the borrowed quad then reads
	// somebody else's blend mode, somebody else's shader and somebody else's
	// effect parameters.
	//
	// That is not theoretical. It is why the ship went translucent while the
	// engine was firing: the plume leaves an Additive quad at the head of the
	// pending batch, the composite inherited it, and the whole frame was added
	// to the surface instead of replacing it -- so the background showed
	// through everything drawn on top of it, ship included.
	//
	// A guard rather than a longer list of swaps, because the next array added
	// to the batch should not be able to reintroduce this.
	struct BorrowedBatch
	{
		std::vector<Vertex> vertices;
		std::vector<uint32_t> textures;
		std::vector<uint32_t> cameras;
		std::vector<wgpu2d::BlendMode> blends;
		std::vector<uint32_t> shaders;
		std::vector<uint32_t> effectSlots;
		std::vector<uint32_t> firstVertices;
		std::vector<wgpu2d::EffectParams> effectParams;
		std::vector<wgpu2d::Camera> frameCams;

		BorrowedBatch() { swapAll(); }
		~BorrowedBatch() { swapAll(); }

		BorrowedBatch(const BorrowedBatch &) = delete;
		BorrowedBatch &operator=(const BorrowedBatch &) = delete;

		void swapAll()
		{
			vertices.swap(batchVertices);
			textures.swap(batchQuadTextures);
			cameras.swap(batchQuadCameras);
			blends.swap(batchQuadBlends);
			shaders.swap(batchQuadShaders);
			effectSlots.swap(batchQuadEffectSlots);
			firstVertices.swap(batchQuadFirstVertex);
			effectParams.swap(frameEffectParams);
			frameCams.swap(frameCameras);
		}
	};

	void clearBatch()
	{
		batchVertices.clear();
		batchQuadTextures.clear();
		batchQuadCameras.clear();
		batchQuadBlends.clear();
		batchQuadShaders.clear();
		batchQuadEffectSlots.clear();
		batchQuadFirstVertex.clear();
		frameEffectParams.clear();
		frameCameras.clear();
	}

	// gl2d's internalFlush: upload the batch, then one draw per run of
	// consecutive quads that share a texture, extended to also break when
	// the camera changes. Order is preserved, so overlap and transparency
	// come out as they do today.
	//
	// `target` is where it lands: 0 for the screen (gl2d's flush), a texture
	// registry id for a render target (gl2d's flushFBO). The camera
	// projection uses the surface's dimensions for the screen -- including
	// when the low-res stand-in is standing in for it, which is what keeps
	// the framing identical at any render scale -- and a render target's own
	// dimensions otherwise.
	void flushBatch(uint32_t target)
	{
		if (batchVertices.empty()) { return; }

		float projectionWidth = (float)g.surfaceWidth;
		float projectionHeight = (float)g.surfaceHeight;
		if (target != 0 && !isFrameTarget(target) && target <= textures.size())
		{
			projectionWidth = (float)textures[target - 1].width;
			projectionHeight = (float)textures[target - 1].height;
		}

		if (!ensurePassBegun(target)) { return; }
		if (target != 0) { g.scaledTargetDrawn = g.scaledTargetDrawn || isFrameTarget(target); }

		const uint64_t bytes = batchVertices.size() * sizeof(Vertex);
		static_assert(sizeof(Vertex) % 4 == 0, "writeBuffer size must be a multiple of 4");
		const uint64_t vertexOffset = g.vertexBufferUsedBytes; // this flush's slice
		if (!ensureVertexBufferCapacity(vertexOffset + bytes)) { return; }

		// Queue-ordered: the copy lands after last frame's draw has finished
		// reading this buffer, so one buffer is enough without double
		// buffering -- as long as this frame's earlier flushes keep their
		// slices, which is what the offset is for.
		g.queue.writeBuffer(g.vertexBuffer, vertexOffset, batchVertices.data(), bytes);
		g.vertexBufferUsedBytes = vertexOffset + bytes;

		// One matrix per camera used in this flush, each in its own slot,
		// after the slots earlier flushes in this frame are still using.
		const uint32_t cameraSlotBase = g.cameraSlotsUsed;
		if (!ensureCameraSlotCapacity(cameraSlotBase + (uint32_t)frameCameras.size())) { return; }
		for (size_t i = 0; i < frameCameras.size(); i++)
		{
			CameraUniforms uniforms;
			uniforms.viewProj = buildViewProj(frameCameras[i], projectionWidth, projectionHeight);
			g.queue.writeBuffer(g.uniformBuffer, (uint64_t)(cameraSlotBase + i) * g.cameraSlotStride,
				&uniforms, sizeof(uniforms));
		}
		g.cameraSlotsUsed = cameraSlotBase + (uint32_t)frameCameras.size();

		// The effect slots for this flush, after whatever earlier flushes in
		// this frame are still using -- same reasoning as the camera slots and
		// the vertex buffer's slices (milestone 10): writeBuffer is ordered
		// against the submit, not against the recorded draws.
		const uint32_t effectSlotBase = g.effectSlotsUsed;
		if (frameEffectParams.empty()) { frameEffectParams.push_back(wgpu2d::EffectParams{}); }
		if (!ensureEffectSlotCapacity(effectSlotBase + (uint32_t)frameEffectParams.size())) { return; }
		for (size_t i = 0; i < frameEffectParams.size(); i++)
		{
			EffectUniforms uniforms;
			uniforms.resolution[0] = projectionWidth;
			uniforms.resolution[1] = projectionHeight;
			uniforms.resolution[2] = projectionWidth != 0.f ? 1.f / projectionWidth : 0.f;
			uniforms.resolution[3] = projectionHeight != 0.f ? 1.f / projectionHeight : 0.f;
			uniforms.time[0] = std::chrono::duration<float>(
				std::chrono::steady_clock::now() - startedAt).count();
			for (int k = 0; k < 4; k++)
			{
				uniforms.a[k] = frameEffectParams[i].a[k];
				uniforms.b[k] = frameEffectParams[i].b[k];
				uniforms.c[k] = frameEffectParams[i].c[k];
				uniforms.d[k] = frameEffectParams[i].d[k];
			}
			g.queue.writeBuffer(g.effectBuffer,
				(uint64_t)(effectSlotBase + i) * g.effectSlotStride, &uniforms, sizeof(uniforms));
		}
		g.effectSlotsUsed = effectSlotBase + (uint32_t)frameEffectParams.size();

		RenderPassEncoder pass = g.framePass;

		// One group per flush, so a capture reads as "world", "scaled
		// composite", "HUD target" rather than a flat run of draws. The pass
		// cannot change inside this function -- ensurePassBegun already ran --
		// so the group is safe for the whole body.
		const char *groupName = g.compositing ? "composite scaled target"
			: target == 0 ? "batch -> surface"
			: isFrameTarget(target) ? "batch -> scaled target"
			: "batch -> render target";
		DebugGroup group(pass, groupName);

		// The attachment's format is fixed for the whole flush -- every quad
		// here lands on the same view -- so it is half of every pipeline key
		// below and is resolved once.
		//
		// Note this does *not* mirror the projection above, which deliberately
		// uses the surface's dimensions for the low-res stand-in so framing
		// survives a render scale. Format has no such reason: a pipeline must
		// match the attachment it actually draws into, stand-in or not.
		TextureFormat targetFormat = g.surfaceFormat;
		if (target != 0 && target <= textures.size())
		{
			targetFormat = textures[target - 1].format;
		}

		// The bound slice starts at this flush's offset, so the per-draw
		// firstVertex below stays relative to the batch.
		pass.setVertexBuffer(0, g.vertexBuffer, vertexOffset, bytes);

		const size_t quadCount = batchQuadTextures.size();
		size_t runStart = 0;
		uint32_t runs = 0;
		RenderPipeline boundPipeline = nullptr; // only re-set when it changes
		for (size_t i = 1; i <= quadCount; i++)
		{
			// R2 adds the third component. Texture and camera change which
			// resources are bound; blend changes which *pipeline* is bound,
			// because blending is baked into the pipeline and cannot be set
			// on a pass.
			const bool boundary = (i == quadCount)
				|| batchQuadTextures[i] != batchQuadTextures[runStart]
				|| batchQuadCameras[i] != batchQuadCameras[runStart]
				|| batchQuadBlends[i] != batchQuadBlends[runStart]
				// F6: a shader is a different pipeline, and a parameter slot
				// is a different dynamic offset. Neither can change mid-draw,
				// so both end a run exactly as texture and camera do.
				|| batchQuadShaders[i] != batchQuadShaders[runStart]
				|| batchQuadEffectSlots[i] != batchQuadEffectSlots[runStart];
			if (!boundary) { continue; }

			RenderPipeline pipeline = getQuadPipeline(
				PipelineKey{ targetFormat, batchQuadBlends[runStart], batchQuadShaders[runStart] });
			if (!pipeline)
			{
				// The variant could not be built. Skipping the run loses those
				// quads; drawing with the wrong pipeline would be a validation
				// error every frame.
				runStart = i;
				continue;
			}
			if (pipeline != boundPipeline)
			{
				pass.setPipeline(pipeline);
				boundPipeline = pipeline;
			}

			const uint32_t dynamicOffset = (cameraSlotBase + batchQuadCameras[runStart]) * g.cameraSlotStride;
			pass.setBindGroup(0, textures[batchQuadTextures[runStart] - 1].bindGroup, 0, nullptr); // texture + sampler
			pass.setBindGroup(1, g.cameraBindGroup, 1, &dynamicOffset);                          // camera slot
			// Group 2 is bound on every draw, not only effect draws: a draw
			// must bind every group its pipeline layout declares, and the
			// layout is shared. Sprite shaders never read it.
			const uint32_t effectOffset =
				(effectSlotBase + batchQuadEffectSlots[runStart]) * g.effectSlotStride;
			pass.setBindGroup(2, g.effectBindGroup, 1, &effectOffset);
			// A run's vertices are contiguous: from its first record's start
			// to the next record's (or the end of the batch).
			const uint32_t first = batchQuadFirstVertex[runStart];
			const uint32_t end = i == quadCount ? (uint32_t)batchVertices.size() : batchQuadFirstVertex[i];
			pass.draw(end - first, 1, first, 0);
			runs++;
			runStart = i;
		}

		statsInProgress.quads += (int)quadCount;
		statsInProgress.drawRuns += (int)runs;
		statsInProgress.cameras += (int)frameCameras.size();
		statsInProgress.flushes += 1;

		if (!g.runStatsPrinted)
		{
			g.runStatsPrinted = true;
			std::cout << "WebGPU first flush: " << quadCount << " quads, " << frameCameras.size()
				<< " cameras, " << runs << " draw runs\n";
			std::cout.flush();
		}
	}

	// gl2d's renderRectangleAbsRotation up to (not including) its camera
	// steps, which are now the matrix. Corners are built and rotated in
	// gl2d's flipped space, then flipped back into world pixels for the
	// vertex buffer. Texture coordinates arrive in gl2d's convention
	// ({u0, v0, u1, v1} with v measured from the bottom, default {0,1,1,0})
	// and are converted to WebGPU's top-left origin here: v = 1 - v.
	// A texture a record can draw with: the one asked for, or the white pixel
	// with a warning if that one is not usable. 0 if not even that exists.
	// The warning is once per id: ourCraft draws GL texture names (block
	// icons) every frame until its 3D renderer is ported.
	uint32_t usableTexture(const wgpu2d::Texture texture)
	{
		if (texture.id == 0 || texture.id > textures.size() || !textures[texture.id - 1].bindGroup)
		{
			static std::unordered_set<uint32_t> warned;
			if (warned.insert(texture.id).second)
			{
				std::cerr << "wgpu2d: Invalid texture (id " << texture.id << "), drawn white\n";
			}
			return white1pxSquareTexture.id;
		}
		return texture.id;
	}

	// One batch record: which texture, camera, blend, shader and parameter
	// slot its vertices draw with, and where they start. Called before the
	// vertices are pushed, so `batchVertices.size()` is the start.
	void recordBatchEntry(const wgpu2d::Camera &camera, wgpu2d::BlendMode blend,
		uint32_t shader, const wgpu2d::EffectParams &effectParams, uint32_t textureId)
	{
		batchQuadFirstVertex.push_back((uint32_t)batchVertices.size());
		batchQuadTextures.push_back(textureId);

		// Record which camera this quad was drawn under. A new slot only when
		// the camera changed since the last recorded one.
		if (frameCameras.empty() || !sameCamera(frameCameras.back(), camera))
		{
			frameCameras.push_back(camera);
		}
		batchQuadCameras.push_back((uint32_t)frameCameras.size() - 1);
		batchQuadBlends.push_back(blend);

		// Same dedupe as the camera above. Quads without an effect still take
		// a slot index, because every draw binds group 2 whether it reads it
		// or not -- they just all land on the same slot.
		if (frameEffectParams.empty() || !sameEffectParams(frameEffectParams.back(), effectParams))
		{
			frameEffectParams.push_back(effectParams);
		}
		batchQuadShaders.push_back(shader);
		batchQuadEffectSlots.push_back((uint32_t)frameEffectParams.size() - 1);
	}

	// Asteroids, A1: triangles straight into the batch. Positions are world
	// pixels already, y down -- none of pushQuad's gl2d flips -- and uvs are
	// WebGPU's, top-left origin with v down, the same way up as the world.
	void pushTriangles(const wgpu2d::Camera &camera, wgpu2d::BlendMode blend,
		uint32_t shader, const wgpu2d::EffectParams &effectParams, const wgpu2d::Texture texture,
		const glm::vec2 *positions, const glm::vec2 *uvs, const glm::vec4 *colors, size_t vertexCount)
	{
		vertexCount -= vertexCount % 3; // whole triangles only
		if (vertexCount == 0 || !positions || !uvs || !colors) { return; }
		const uint32_t textureId = usableTexture(texture);
		if (textureId == 0) { return; }

		recordBatchEntry(camera, blend, shader, effectParams, textureId);
		for (size_t i = 0; i < vertexCount; i++)
		{
			const glm::vec4 &c = colors[i];
			batchVertices.push_back(Vertex{ positions[i].x, positions[i].y,
				c.r, c.g, c.b, c.a, uvs[i].x, uvs[i].y });
		}
	}

	// See wgpu2d::setCullMirroredQuads.
	static bool cullMirroredQuads = false;

	void pushQuad(const wgpu2d::Camera &camera, wgpu2d::BlendMode blend,
		uint32_t shader, const wgpu2d::EffectParams &effectParams,
		const glm::vec4 transforms, const wgpu2d::Texture texture,
		const glm::vec4 colors[4], const glm::vec2 origin, const float rotation, const glm::vec4 textureCoords)
	{
		// One negative size reverses the winding (rotation never does); two
		// cancel out, as they did under GL's back-face cull.
		if (cullMirroredQuads && (transforms.z < 0.f) != (transforms.w < 0.f)) { return; }

		const uint32_t textureId = usableTexture(texture);
		if (textureId == 0) { return; }

		//We need to flip texture_transforms.y
		const float transformsY = transforms.y * -1;

		glm::vec2 v1 = { transforms.x,				  transformsY };
		glm::vec2 v2 = { transforms.x,				  transformsY - transforms.w };
		glm::vec2 v3 = { transforms.x + transforms.z, transformsY - transforms.w };
		glm::vec2 v4 = { transforms.x + transforms.z, transformsY };

		//Apply rotations
		if (rotation != 0)
		{
			v1 = rotateAroundPoint(v1, origin, rotation);
			v2 = rotateAroundPoint(v2, origin, rotation);
			v3 = rotateAroundPoint(v3, origin, rotation);
			v4 = rotateAroundPoint(v4, origin, rotation);
		}

		// Back to world pixels (y down). gl2d continued with camera offset,
		// zoom, and NDC here; the vertex shader does that now.
		v1.y = -v1.y; v2.y = -v2.y; v3.y = -v3.y; v4.y = -v4.y;

		const float u0 = textureCoords.x, v0 = 1.0f - textureCoords.y;
		const float u1 = textureCoords.z, v1t = 1.0f - textureCoords.w;

		recordBatchEntry(camera, blend, shader, effectParams, textureId);

		auto push = [&](glm::vec2 p, const glm::vec4 &c, float u, float v)
		{
			batchVertices.push_back(Vertex{ p.x, p.y, c.r, c.g, c.b, c.a, u, v });
		};

		// gl2d's corner order: v1 v2 v4, v2 v3 v4, with gl2d's uv assignment.
		push(v1, colors[0], u0, v0);
		push(v2, colors[1], u0, v1t);
		push(v4, colors[3], u1, v0);
		push(v2, colors[1], u0, v1t);
		push(v3, colors[2], u1, v1t);
		push(v4, colors[3], u1, v0);
	}

	// 10: the upscale. One quad covering the surface, sampling the low-res
	// target with nearest filtering, drawn through the ordinary sprite path
	// (the target is a normal texture handle, so no extra pipeline or shader
	// is needed). Any quads the game has recorded but not flushed are put
	// aside and restored, so this never eats the caller's batch.
	// ---------------------------------------------------------------------
	// N4 step two: the phosphor glow
	// ---------------------------------------------------------------------

	bool ensureGlowPipelines()
	{
		if (g.brightpassPipeline && g.blurPipeline) { return true; }
		if (g.glowPipelineFailed) { return false; }

		ErrorScope scope("glow compute pipelines");

		g.brightpassShaderModule = createShaderModule(brightpassShaderWGSL, "brightpass.wgsl");
		g.blurShaderModule = createShaderModule(blurShaderWGSL, "blur.wgsl");
		if (!g.brightpassShaderModule || !g.blurShaderModule)
		{
			g.glowPipelineFailed = true;
			return false;
		}

		// Three bindings, shared by both stages. Binding 0 is loaded rather
		// than sampled, so there is no sampler here either.
		BindGroupLayoutEntry entries[3];
		entries[0] = Default;
		entries[0].binding = 0;
		entries[0].visibility = ShaderStage::Compute;
		entries[0].texture.sampleType = TextureSampleType::Float;
		entries[0].texture.viewDimension = TextureViewDimension::_2D;
		entries[0].texture.multisampled = false;
		entries[0].buffer.type = BufferBindingType::BindingNotUsed;
		entries[0].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[0].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		entries[1] = Default;
		entries[1].binding = 1;
		entries[1].visibility = ShaderStage::Compute;
		entries[1].storageTexture.access = StorageTextureAccess::WriteOnly;
		entries[1].storageTexture.format = TextureFormat::RGBA8Unorm;
		entries[1].storageTexture.viewDimension = TextureViewDimension::_2D;
		entries[1].buffer.type = BufferBindingType::BindingNotUsed;
		entries[1].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[1].texture.sampleType = TextureSampleType::BindingNotUsed;

		entries[2] = Default;
		entries[2].binding = 2;
		entries[2].visibility = ShaderStage::Compute;
		entries[2].buffer.type = BufferBindingType::Uniform;
		entries[2].buffer.hasDynamicOffset = false;
		entries[2].buffer.minBindingSize = 16;
		entries[2].sampler.type = SamplerBindingType::BindingNotUsed;
		entries[2].texture.sampleType = TextureSampleType::BindingNotUsed;
		entries[2].storageTexture.access = StorageTextureAccess::BindingNotUsed;

		BindGroupLayoutDescriptor layoutDesc = Default;
		layoutDesc.label = StringView("glow bind group layout");
		layoutDesc.entryCount = 3;
		layoutDesc.entries = entries;
		g.glowBindGroupLayout = g.device.createBindGroupLayout(layoutDesc);

		PipelineLayoutDescriptor pipelineLayoutDesc = Default;
		pipelineLayoutDesc.label = StringView("glow pipeline layout");
		pipelineLayoutDesc.bindGroupLayoutCount = 1;
		WGPUBindGroupLayout layouts[1] = { g.glowBindGroupLayout };
		pipelineLayoutDesc.bindGroupLayouts = layouts;
		g.glowPipelineLayout = g.device.createPipelineLayout(pipelineLayoutDesc);

		ComputePipelineDescriptor desc = Default;
		desc.layout = g.glowPipelineLayout;
		desc.compute.nextInChain = nullptr;
		desc.compute.entryPoint = StringView("cs_main");
		desc.compute.constantCount = 0;
		desc.compute.constants = nullptr;

		desc.label = StringView("bright pass");
		desc.compute.module = g.brightpassShaderModule;
		g.brightpassPipeline = g.device.createComputePipeline(desc);

		desc.label = StringView("separable blur");
		desc.compute.module = g.blurShaderModule;
		g.blurPipeline = g.device.createComputePipeline(desc);

		// One slot per dispatch. Three bind groups rather than one with dynamic
		// offsets, because each already differs in which textures it names.
		BufferDescriptor bufDesc = Default;
		bufDesc.label = StringView("glow parameters");
		bufDesc.usage = BufferUsage::Uniform | BufferUsage::CopyDst;
		g.glowParamStride = std::max(256u, g.minUniformBufferOffsetAlignment);
		bufDesc.size = (uint64_t)g.glowParamStride * 3;
		g.glowParamBuffer = g.device.createBuffer(bufDesc);

		if (!g.brightpassPipeline || !g.blurPipeline || !g.glowParamBuffer || scope.failed())
		{
			std::cerr << "WebGPU: the glow pipelines could not be built; no bloom\n";
			g.glowPipelineFailed = true;
			return false;
		}
		return true;
	}

	// The scratch pair, at half the frame's size, plus the three bind groups
	// that wire the dispatches together. All of it is rebuilt as one thing,
	// because a bind group naming a released view is the subtlest way to get
	// this wrong.
	bool ensureGlowTargets(int frameWidth, int frameHeight)
	{
		const int w = std::max(1, frameWidth / 2);
		const int h = std::max(1, frameHeight / 2);
		if (g.glowScratchA && g.glowWidth == w && g.glowHeight == h
			&& g.glowFrameWidth == frameWidth && g.glowFrameHeight == frameHeight
			&& g.glowFrameGeneration == g.frameTargetGeneration)
		{
			return true;
		}

		wgpu2d::Texture a = createRenderTarget(w, h, false, "glow scratch A", g.glowScratchA, true);
		wgpu2d::Texture b = createRenderTarget(w, h, false, "glow scratch B", g.glowScratchB, true);
		if (a.id == 0 || b.id == 0) { return false; }
		g.glowScratchA = a.id;
		g.glowScratchB = b.id;
		g.glowWidth = w;
		g.glowHeight = h;
		g.glowFrameWidth = frameWidth;
		g.glowFrameHeight = frameHeight;
		g.glowFrameGeneration = g.frameTargetGeneration;

		ErrorScope scope("glow bind groups");
		const uint32_t srcIds[3] = { g.scaledTargetId, g.glowScratchA, g.glowScratchB };
		const uint32_t dstIds[3] = { g.glowScratchA, g.glowScratchB, g.glowScratchA };
		for (int i = 0; i < 3; i++)
		{
			if (g.glowGroups[i]) { g.glowGroups[i].release(); g.glowGroups[i] = nullptr; }

			BindGroupEntry bindEntries[3];
			bindEntries[0] = Default;
			bindEntries[0].binding = 0;
			bindEntries[0].textureView = textures[srcIds[i] - 1].view;
			bindEntries[1] = Default;
			bindEntries[1].binding = 1;
			bindEntries[1].textureView = textures[dstIds[i] - 1].storageView;
			bindEntries[2] = Default;
			bindEntries[2].binding = 2;
			bindEntries[2].buffer = g.glowParamBuffer;
			bindEntries[2].offset = (uint64_t)i * g.glowParamStride;
			bindEntries[2].size = 16;

			BindGroupDescriptor groupDesc = Default;
			groupDesc.label = StringView("glow stage");
			groupDesc.layout = g.glowBindGroupLayout;
			groupDesc.entryCount = 3;
			groupDesc.entries = bindEntries;
			g.glowGroups[i] = g.device.createBindGroup(groupDesc);
			if (!g.glowGroups[i]) { return false; }
		}
		return !scope.failed();
	}

	// Bright pass, blur across, blur down, then add the result back.
	//
	// Called from inside the composite, after the frame's target is complete
	// and before it goes to the screen. That ordering is the point: the glow
	// lands *in* the frame, so a final effect curves and scans it along with
	// everything else rather than laying a flat bloom over a curved picture.
	void runFinalGlow()
	{
		if (!g.finalGlowSet || !g.scaledTargetId || !g.frameEncoder) { return; }
		if (!ensureGlowPipelines()) { return; }

		const TextureEntry &frame = textures[g.scaledTargetId - 1];
		if (!ensureGlowTargets(frame.width, frame.height)) { return; }

		const float params[3][4] = {
			{ g.finalGlow.threshold, g.finalGlow.knee, 0.f, 0.f },
			{ 1.f, 0.f, g.finalGlow.sigma, 0.f },  // blur along x
			{ 0.f, 1.f, g.finalGlow.sigma, 0.f },  // then along y
		};
		for (int i = 0; i < 3; i++)
		{
			g.queue.writeBuffer(g.glowParamBuffer, (uint64_t)i * g.glowParamStride,
				params[i], sizeof(params[i]));
		}

		// A compute pass cannot be nested in a render pass, so the target's
		// pass has to close before this one opens. Closing it is also what
		// makes the target's contents visible to the dispatches below.
		if (g.framePass)
		{
			g.framePass.end();
			g.framePass.release();
			g.framePass = nullptr;
			g.framePassTarget = 0;
		}

		const uint32_t w = (uint32_t)g.glowWidth;
		const uint32_t h = (uint32_t)g.glowHeight;

		ComputePassDescriptor passDesc = Default;
		passDesc.label = StringView("phosphor glow");
		passDesc.timestampWrites = nullptr;
		ComputePassEncoder pass = g.frameEncoder.beginComputePass(passDesc);

		pass.setPipeline(g.brightpassPipeline);
		pass.setBindGroup(0, g.glowGroups[0], 0, nullptr);
		pass.dispatchWorkgroups((w + 7) / 8, (h + 7) / 8, 1);

		// Both blur passes are the same pipeline; only the axis in the uniform
		// differs. The second reads what the first wrote, and WebGPU orders
		// dispatches within a pass, so nothing has to be said about it here.
		pass.setPipeline(g.blurPipeline);
		pass.setBindGroup(0, g.glowGroups[1], 0, nullptr);
		pass.dispatchWorkgroups((w + 63) / 64, h, 1);
		pass.setBindGroup(0, g.glowGroups[2], 0, nullptr);
		pass.dispatchWorkgroups((h + 63) / 64, w, 1);

		pass.end();
		pass.release();

		// Add it back. Additive, because light adds -- the same equation the
		// plume and the shield rim use, and the reason a bloom brightens what
		// is under it instead of replacing it.
		wgpu2d::Texture handle;
		handle.id = g.glowScratchA;
		// Intensity and tint are one multiply on the quad's vertex colour, so a
		// coloured glow costs nothing over a grey one.
		const glm::vec3 k = g.finalGlow.intensity * g.finalGlow.tint;
		const glm::vec4 c = {k.r, k.g, k.b, 1.f};
		const glm::vec4 tint[4] = { c, c, c, c };
		pushQuad(wgpu2d::Camera{}, wgpu2d::BlendMode::Additive, 0, wgpu2d::EffectParams{},
			glm::vec4{0, 0, (float)frame.width, (float)frame.height},
			handle, tint, {}, 0.f, WGPU2D_DefaultTextureCoords);
		flushBatch(g.scaledTargetId);
	}

	void compositeScaledTarget()
	{
		if (!g.scaledTargetId || !g.scaledTargetDrawn || g.compositing) { return; }
		g.compositing = true;

		BorrowedBatch borrowed;

		// The glow goes into the target, before the target goes to the screen.
		runFinalGlow();

		wgpu2d::Texture handle;
		handle.id = g.scaledTargetId;
		const glm::vec4 white[4] = { {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1} };
		// The target's pixels are premultiplied by their own coverage, so
		// this is Premultiplied regardless of what the quads inside it used.
		// The world target is cleared opaque, so today this is numerically
		// identical to Alpha -- it stops being identical the moment anything
		// clears it to anything translucent.
		// F7: the same quad, through the final effect when one is set. This is
		// the only place the frame is a texture and the surface is the target,
		// which is what makes it the right and only home for such a filter.
		pushQuad(wgpu2d::Camera{}, wgpu2d::BlendMode::Premultiplied,
			g.finalEffectId, g.finalEffectParams,
			glm::vec4{0, 0, (float)g.surfaceWidth, (float)g.surfaceHeight},
			handle, white, {}, 0.f, WGPU2D_DefaultTextureCoords);
		g.scaledTargetDrawn = false; // the pass below is the surface's, not the target's
		flushBatch(0);

		g.compositing = false;
	}

	// ---------------------------------------------------------------------
	// Frame capture
	// ---------------------------------------------------------------------
	//
	// Three steps, split across the frame because the GPU is not synchronous:
	// record a copy from the frame's texture into a buffer (before submit),
	// submit, then map the buffer and read it (after submit). The map is
	// asynchronous like every other WebGPU callback, and is drained with the
	// same bounded pump the error scopes use.
	//
	// Two rules the copy imposes, and both are the caller's problem if they
	// leak: bytesPerRow must be a multiple of 256, so the buffer's rows are
	// padded and unpadded here; and the surface is BGRA on this backend, so
	// the channels are swizzled here. What comes out is tightly packed RGBA.
	struct FrameCapture
	{
		bool requested = false;
		bool copied = false;
		bool ready = false;
		Buffer buffer = nullptr;
		uint64_t bufferBytes = 0;
		uint32_t rowBytes = 0;
		int width = 0;
		int height = 0;
		std::vector<unsigned char> pixels;
		bool mapDone = false;
	};
	FrameCapture capture;

	// Records the copy. Called from wgpuEndFrame after the pass has ended and
	// before the encoder is finished -- a copy cannot be recorded inside a
	// render pass.
	void captureRecordCopy()
	{
		if (!capture.requested || !g.frameTexture || g.surfaceWidth <= 0 || g.surfaceHeight <= 0)
		{
			return;
		}

		capture.width = g.surfaceWidth;
		capture.height = g.surfaceHeight;
		capture.rowBytes = ceilToNextMultiple((uint32_t)capture.width * 4, 256);
		const uint64_t needed = (uint64_t)capture.rowBytes * capture.height;

		if (!capture.buffer || capture.bufferBytes < needed)
		{
			if (capture.buffer) { capture.buffer.release(); }
			BufferDescriptor desc = Default;
			desc.label = StringView("frame capture");
			desc.usage = BufferUsage::CopyDst | BufferUsage::MapRead;
			desc.size = needed;
			desc.mappedAtCreation = false;

			ErrorScope scope("frame capture buffer");
			capture.buffer = g.device.createBuffer(desc);
			if (!capture.buffer || scope.failed())
			{
				std::cerr << "WebGPU: frame capture buffer could not be created\n";
				capture.requested = false;
				return;
			}
			capture.bufferBytes = needed;
		}

		TexelCopyTextureInfo source = Default;
		source.texture = g.frameTexture;
		source.mipLevel = 0;
		source.origin = { 0, 0, 0 };
		source.aspect = TextureAspect::All;

		TexelCopyBufferInfo destination = Default;
		destination.buffer = capture.buffer;
		destination.layout.offset = 0;
		destination.layout.bytesPerRow = capture.rowBytes;
		destination.layout.rowsPerImage = (uint32_t)capture.height;

		Extent3D size = { (uint32_t)capture.width, (uint32_t)capture.height, 1 };
		g.frameEncoder.copyTextureToBuffer(source, destination, size);
		capture.copied = true;
	}

	void onCaptureMapped(WGPUMapAsyncStatus status, WGPUStringView message, void *userdata1, void *)
	{
		*static_cast<bool *>(userdata1) = true;
		if (status != MapAsyncStatus::Success)
		{
			std::cerr << "WebGPU: frame capture mapAsync failed: " << StringView(message) << "\n";
		}
	}

	// Maps the buffer and unpacks it. Called from wgpuEndFrame after submit.
	// This blocks, which is why a capture is a one-off the application asks
	// for rather than something that happens every frame.
	void captureResolve()
	{
		if (!capture.copied) { return; }
		capture.copied = false;
		capture.requested = false;

		const size_t total = (size_t)capture.bufferBytes;
		capture.mapDone = false;

		BufferMapCallbackInfo info = Default;
		info.mode = CallbackMode::AllowProcessEvents;
		info.callback = onCaptureMapped;
		info.userdata1 = &capture.mapDone;
		info.userdata2 = nullptr;
		capture.buffer.mapAsync(MapMode::Read, 0, total, info);

		for (int i = 0; !capture.mapDone && i < 3000; i++)
		{
			g.instance.processEvents();
			if (!capture.mapDone) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		}
		if (!capture.mapDone)
		{
			std::cerr << "WebGPU: frame capture never mapped\n";
			return;
		}

		const unsigned char *src = static_cast<const unsigned char *>(
			capture.buffer.getConstMappedRange(0, total));
		if (!src)
		{
			std::cerr << "WebGPU: frame capture mapped range was null\n";
			capture.buffer.unmap();
			return;
		}

		capture.pixels.resize((size_t)capture.width * capture.height * 4);
		const bool bgra = (g.surfaceFormat == TextureFormat::BGRA8Unorm
			|| g.surfaceFormat == TextureFormat::BGRA8UnormSrgb);
		for (int y = 0; y < capture.height; y++)
		{
			const unsigned char *row = src + (size_t)y * capture.rowBytes;
			unsigned char *out = capture.pixels.data() + (size_t)y * capture.width * 4;
			for (int x = 0; x < capture.width; x++)
			{
				const unsigned char *p = row + (size_t)x * 4;
				unsigned char *q = out + (size_t)x * 4;
				q[0] = bgra ? p[2] : p[0];
				q[1] = p[1];
				q[2] = bgra ? p[0] : p[2];
				q[3] = 255; // the surface has no meaningful alpha to hand on
			}
		}

		capture.buffer.unmap();
		capture.ready = true;
	}

	// Adapter features are printed by name, not just counted: whether a
	// machine has TimestampQuery decides whether GPU timing is possible at
	// all, and that is worth knowing from a run's own output rather than from
	// a one-off probe. Unknown values print as numbers -- wgpu-native has its
	// own beyond the spec's list.
	const char *featureName(WGPUFeatureName f)
	{
		switch (f)
		{
			case WGPUFeatureName_DepthClipControl: return "DepthClipControl";
			case WGPUFeatureName_Depth32FloatStencil8: return "Depth32FloatStencil8";
			case WGPUFeatureName_TimestampQuery: return "TimestampQuery";
			case WGPUFeatureName_TextureCompressionBC: return "TextureCompressionBC";
			case WGPUFeatureName_TextureCompressionETC2: return "TextureCompressionETC2";
			case WGPUFeatureName_TextureCompressionASTC: return "TextureCompressionASTC";
			case WGPUFeatureName_IndirectFirstInstance: return "IndirectFirstInstance";
			case WGPUFeatureName_ShaderF16: return "ShaderF16";
			case WGPUFeatureName_RG11B10UfloatRenderable: return "RG11B10UfloatRenderable";
			case WGPUFeatureName_BGRA8UnormStorage: return "BGRA8UnormStorage";
			case WGPUFeatureName_Float32Filterable: return "Float32Filterable";
			default: return nullptr;
		}
	}

	void printAdapter(Adapter adapter)
	{
		AdapterInfo info = Default;
		if (adapter.getInfo(&info) == Status::Success)
		{
			std::cout << "WebGPU adapter\n"
				<< "  device:       " << StringView(info.device) << "\n"
				<< "  vendor:       " << StringView(info.vendor) << " (0x" << std::hex << info.vendorID << std::dec << ")\n"
				<< "  architecture: " << StringView(info.architecture) << "\n"
				<< "  description:  " << StringView(info.description) << "\n"
				<< "  backend:      " << backendName(info.backendType) << "\n"
				<< "  type:         " << adapterTypeName(info.adapterType) << "\n";
			info.freeMembers();
		}
		else
		{
			std::cerr << "WebGPU: adapter.getInfo failed\n";
		}

		// The limits that matter for a 2D sprite batcher. minUniformBufferOffsetAlignment
		// decides the stride of the per-camera uniform slots.
		Limits limits = Default;
		if (adapter.getLimits(&limits) == Status::Success)
		{
			g.minUniformBufferOffsetAlignment = limits.minUniformBufferOffsetAlignment;
			std::cout << "  limits:\n"
				<< "    maxTextureDimension2D:          " << limits.maxTextureDimension2D << "\n"
				<< "    maxBindGroups:                  " << limits.maxBindGroups << "\n"
				<< "    maxVertexBuffers:               " << limits.maxVertexBuffers << "\n"
				<< "    maxVertexAttributes:            " << limits.maxVertexAttributes << "\n"
				<< "    maxBufferSize:                  " << limits.maxBufferSize << "\n"
				<< "    maxUniformBufferBindingSize:    " << limits.maxUniformBufferBindingSize << "\n"
				<< "    minUniformBufferOffsetAlignment: " << limits.minUniformBufferOffsetAlignment << "\n";
		}
		else
		{
			std::cerr << "WebGPU: adapter.getLimits failed\n";
		}

		SupportedFeatures features = Default;
		adapter.getFeatures(&features);
		std::cout << "  features: " << features.featureCount << "\n";
		for (size_t i = 0; i < features.featureCount; i++)
		{
			const WGPUFeatureName f = features.features[i];
			const char *name = featureName(f);
			std::cout << "    ";
			if (name) { std::cout << name; }
			else { std::cout << "0x" << std::hex << (uint32_t)f << std::dec; }
			std::cout << "\n";
		}
		features.freeMembers();

		// Flush so the report survives even if the process is killed while the
		// window is open (stdout is fully buffered when redirected to a file).
		std::cout.flush();
	}
}

// -------------------------------------------------------------------------
// Context lifecycle, called from the platform loop
// -------------------------------------------------------------------------

WGPUInstance wgpuInitInstance()
{
	if (g.instance) { return g.instance; }

	wgpuSetLogCallback(logCallback, nullptr);
	// Warn and above by default. WGPU_LOG_LEVEL raises it without a rebuild;
	// "info" and "debug" say what the backend is doing between our calls,
	// which is the level worth reaching for when a scope reports something
	// that the descriptor alone does not explain.
	WGPULogLevel logLevel = WGPULogLevel_Warn;
	if (const char *level = getenv("WGPU_LOG_LEVEL"))
	{
		const std::string wanted = level;
		if (wanted == "off") { logLevel = WGPULogLevel_Off; }
		else if (wanted == "error") { logLevel = WGPULogLevel_Error; }
		else if (wanted == "warn") { logLevel = WGPULogLevel_Warn; }
		else if (wanted == "info") { logLevel = WGPULogLevel_Info; }
		else if (wanted == "debug") { logLevel = WGPULogLevel_Debug; }
		else if (wanted == "trace") { logLevel = WGPULogLevel_Trace; }
		else { std::cerr << "WGPU_LOG_LEVEL: unknown level '" << wanted << "', keeping warn\n"; }
	}
	wgpuSetLogLevel(logLevel);

	// The instance is the library itself: no GPU, no window. It exists before
	// wgpuInit because creating a surface needs it, and creating a surface is
	// the application's job -- only the application knows what a window is.
	startedAt = std::chrono::steady_clock::now();

	InstanceDescriptor instanceDesc = Default;
	g.instance = createInstance(instanceDesc);
	if (!g.instance)
	{
		std::cerr << "WebGPU: createInstance returned null\n";
		return nullptr;
	}
	return g.instance;
}

bool wgpuInit(WGPUSurface surface, int width, int height)
{
	if (!g.instance)
	{
		std::cerr << "WebGPU: wgpuInit before wgpuInitInstance\n";
		return false;
	}
	if (!surface)
	{
		std::cerr << "WebGPU: wgpuInit given a null surface\n";
		return false;
	}
	g.surface = surface;

	// Any return false below must drop the borrowed surface without
	// releasing it. The flag is set only on the success path.
	struct DropSurfaceIfFailed
	{
		bool keep = false;
		~DropSurfaceIfFailed()
		{
			if (!keep) { forgetSurface(); }
		}
	} dropSurface;

	// The macOS colour-space pin used to happen here. It needs the window, so
	// it moved out with GLFW -- glfwMain calls it right after creating the
	// surface. See platform/wgpuMetalLayer.h.

	// Adapter: a description of one physical GPU that fits the options.
	//    The request is callback-based; on native backends it completes
	//    during processEvents, so the loop below runs at most a few times.
	RequestAdapterOptions options = Default;
	options.featureLevel = FeatureLevel::Core;
	options.powerPreference = PowerPreference::Undefined;
	options.forceFallbackAdapter = false;
	options.backendType = BackendType::Undefined;
	options.compatibleSurface = g.surface;

	AdapterRequest request;
	RequestAdapterCallbackInfo callbackInfo = Default;
	callbackInfo.mode = CallbackMode::AllowProcessEvents;
	callbackInfo.callback = onAdapterRequest;
	callbackInfo.userdata1 = &request;
	callbackInfo.userdata2 = nullptr;

	g.instance.requestAdapter(options, callbackInfo);

	for (int i = 0; !request.done && i < 1000; i++)
	{
		g.instance.processEvents();
		if (!request.done)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (!request.done)
	{
		std::cerr << "WebGPU: requestAdapter never completed\n";
		return false;
	}
	if (!request.adapter)
	{
		return false;
	}
	g.adapter = request.adapter;

	printAdapter(g.adapter);

	// Device: the working connection to the GPU. Every later object is
	//    created from it. No required features or limits: the defaults are
	//    far above what a 2D sprite batcher needs.
	DeviceDescriptor deviceDesc = Default;
	deviceDesc.label = StringView("space-game device");
	deviceDesc.requiredFeatureCount = 0;
	deviceDesc.requiredFeatures = nullptr;
	deviceDesc.requiredLimits = nullptr;
	deviceDesc.defaultQueue.label = StringView("space-game queue");
	deviceDesc.deviceLostCallbackInfo.mode = CallbackMode::AllowProcessEvents;
	deviceDesc.deviceLostCallbackInfo.callback = onDeviceLost;
	deviceDesc.deviceLostCallbackInfo.userdata1 = nullptr;
	deviceDesc.deviceLostCallbackInfo.userdata2 = nullptr;
	deviceDesc.uncapturedErrorCallbackInfo.callback = onUncapturedError;
	deviceDesc.uncapturedErrorCallbackInfo.userdata1 = nullptr;
	deviceDesc.uncapturedErrorCallbackInfo.userdata2 = nullptr;

	DeviceRequest deviceRequest;
	RequestDeviceCallbackInfo deviceCallbackInfo = Default;
	deviceCallbackInfo.mode = CallbackMode::AllowProcessEvents;
	deviceCallbackInfo.callback = onDeviceRequest;
	deviceCallbackInfo.userdata1 = &deviceRequest;
	deviceCallbackInfo.userdata2 = nullptr;

	g.adapter.requestDevice(deviceDesc, deviceCallbackInfo);

	for (int i = 0; !deviceRequest.done && i < 1000; i++)
	{
		g.instance.processEvents();
		if (!deviceRequest.done)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (!deviceRequest.done)
	{
		std::cerr << "WebGPU: requestDevice never completed\n";
		return false;
	}
	if (!deviceRequest.device)
	{
		return false;
	}
	g.device = deviceRequest.device;

	// Queue: the device's single inbox for command buffers and uploads.
	g.queue = g.device.getQueue();
	if (!g.queue)
	{
		std::cerr << "WebGPU: device.getQueue returned null\n";
		return false;
	}

	// Surface format. The first listed format is the surface's preferred
	//    one and on Metal it is normally an sRGB variant. gl2d never gamma
	//    corrected, so prefer the plain (non-sRGB) 8-bit format when offered
	//    and only fall back to the preferred one otherwise.
	SurfaceCapabilities caps = Default;
	if (g.surface.getCapabilities(g.adapter, &caps) != Status::Success || caps.formatCount == 0)
	{
		std::cerr << "WebGPU: surface.getCapabilities failed or listed no formats\n";
		return false;
	}

	g.surfaceFormat = caps.formats[0];
	std::cout << "WebGPU surface formats offered:";
	for (size_t i = 0; i < caps.formatCount; i++)
	{
		std::cout << " " << formatName(caps.formats[i]);
	}
	std::cout << "\n";
	for (size_t i = 0; i < caps.formatCount; i++)
	{
		if (caps.formats[i] == TextureFormat::BGRA8Unorm || caps.formats[i] == TextureFormat::RGBA8Unorm)
		{
			g.surfaceFormat = caps.formats[i];
			break;
		}
	}
	std::cout << "WebGPU surface format chosen: " << formatName(g.surfaceFormat) << "\n";
	caps.freeMembers();

	// Configure the surface: this is the OpenGL default framebuffer plus
	//    swap interval, expressed as an explicit object.
	configureSurface(width, height);
	if (!g.surfaceConfigured)
	{
		std::cerr << "WebGPU: surface not configured (framebuffer size "
			<< g.surfaceWidth << "x" << g.surfaceHeight << ")\n";
		return false;
	}

	// Samplers and layouts. The pipeline needs the layout.
	if (!createSamplers() || !createLayouts())
	{
		return false;
	}

	// The sprite shader, compiled once and shared by every pipeline
	//    variant, then the one variant the common case needs: the surface's
	//    format with gl2d's alpha blend. The rest are built the first time a
	//    flush asks for them. Warming this one here keeps a validation
	//    failure in the shader or the layout at startup, where it can stop
	//    init, rather than in the first frame.
	g.quadShaderModule = createShaderModule(quadShaderWGSL, "quad.wgsl");
	if (!g.quadShaderModule)
	{
		return false;
	}
	// One effect slot exists from the start: every draw must bind group 2 even
	// though the sprite shader ignores it, so there has to be something there.
	if (!ensureEffectSlotCapacity(1))
	{
		std::cerr << "WebGPU: effect uniform buffer could not be created\n";
		return false;
	}

	if (!getQuadPipeline(PipelineKey{ g.surfaceFormat, wgpu2d::BlendMode::Alpha }))
	{
		return false;
	}
	// getQuadPipeline already reported the variant; no second line for it.

	// gl2d's 1px white texture: what every untextured draw samples.
	white1pxSquareTexture.create1PxSquare();
	if (white1pxSquareTexture.id == 0)
	{
		return false;
	}

	// The camera uniform slots. Grow on demand.
	if (!ensureCameraSlotCapacity(1))
	{
		return false;
	}

	// 12. Optional low-resolution rendering (milestone 10). Off unless
	//     WGPU_RENDER_SCALE is set to something below 1.
	if (const char *scale = getenv("WGPU_RENDER_SCALE"))
	{
		g.renderScale = (float)atof(scale);
		if (!(g.renderScale > 0.f) || g.renderScale > 1.f)
		{
			std::cerr << "WebGPU: ignoring WGPU_RENDER_SCALE=" << scale << " (expected 0 < scale <= 1)\n";
			g.renderScale = 1.f;
		}
		else if (g.renderScale < 1.f)
		{
			std::cout << "WebGPU render scale: " << g.renderScale
				<< " (the world is drawn into an offscreen target and upscaled)\n";
		}
	}
	ensureScaledTarget();

	std::cout.flush();
	dropSurface.keep = true;
	return true;
}

void wgpuRequestFrameCapture()
{
	capture.requested = true;
	capture.ready = false;
}

bool wgpuTakeFrameCapture(std::vector<unsigned char> &rgba, int &width, int &height)
{
	if (!capture.ready) { return false; }
	capture.ready = false;
	rgba = std::move(capture.pixels);
	capture.pixels.clear();
	width = capture.width;
	height = capture.height;
	return true;
}

void wgpuResize(int width, int height)
{
	if (!g.surface) { return; }
	if (g.surfaceConfigured && width == g.surfaceWidth && height == g.surfaceHeight) { return; }

	configureSurface(width, height);
	if (g.surfaceConfigured)
	{
		ensureScaledTarget(); // the low-res target follows the surface's size
	}
}

void wgpuBeginFrame()
{
	g.frameOpen = false;
	statsInProgress = wgpu2d::FrameStats{};
	framePerf = FramePerf{};
	g.effectSlotsUsed = 0;

	// The surface must have been configured, which wgpuResize does. It is the
	// application's job to call that with the current framebuffer size --
	// every frame is fine and is what this game does, because on Metal
	// wgpu-native keeps presenting a drawable of the configured size and
	// stretches it to the window rather than ever reporting the surface
	// Outdated. The Outdated/Lost path below stays as a backstop for backends
	// that do report it.
	if (!g.surfaceConfigured) { return; } // minimized, or resize not pushed yet

	// F7: a final effect can be set or cleared at any time, and setting one is
	// what makes the frame's target exist. Checking here rather than in the
	// setter keeps the target's lifetime in one place with the resize path,
	// and the check is a size comparison against an existing texture.
	ensureScaledTarget();

	// Acquire: the texture that will next go on screen. Timed, because this
	// is one of the two places a frame can spend its time waiting rather than
	// working -- it blocks until the presentation engine frees a drawable.
	SurfaceTexture surfaceTexture = Default;
	const auto acquireStart = std::chrono::steady_clock::now();
	g.surface.getCurrentTexture(&surfaceTexture);
	framePerf.acquireMs = std::chrono::duration<float, std::milli>(
		std::chrono::steady_clock::now() - acquireStart).count();
	wgpu::Texture texture = surfaceTexture.texture;

	switch (surfaceTexture.status)
	{
		case SurfaceGetCurrentTextureStatus::SuccessOptimal:
		case SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
			break;

		case SurfaceGetCurrentTextureStatus::Timeout:
		case SurfaceGetCurrentTextureStatus::Outdated:
		case SurfaceGetCurrentTextureStatus::Lost:
			// Reconfigure at the size we believe and try again next frame. If
			// the surface went Outdated *because* the window changed size,
			// the application's next wgpuResize corrects it -- this path is
			// the backstop for backends that report Outdated at all, which
			// Metal does not.
			if (texture) { texture.release(); }
			configureSurface(g.surfaceWidth, g.surfaceHeight);
			return;

		default:
			std::cerr << "WebGPU: surface.getCurrentTexture failed (status "
				<< surfaceTexture.status << ")\n";
			if (texture) { texture.release(); }
			return;
	}

	// View: render passes attach to a view of a texture, never the texture.
	TextureViewDescriptor viewDesc = Default;
	viewDesc.label = StringView("surface view");
	viewDesc.format = g.surfaceFormat;
	viewDesc.dimension = TextureViewDimension::_2D;
	viewDesc.baseMipLevel = 0;
	viewDesc.mipLevelCount = 1;
	viewDesc.baseArrayLayer = 0;
	viewDesc.arrayLayerCount = 1;
	viewDesc.aspect = TextureAspect::All;
	viewDesc.usage = TextureUsage::RenderAttachment;

	// Record: a command encoder collects GPU work; nothing runs yet.
	CommandEncoderDescriptor encoderDesc = Default;
	encoderDesc.label = StringView("frame encoder");

	g.frameTexture = texture;
	g.frameView = texture.createView(viewDesc);
	g.frameEncoder = g.device.createCommandEncoder(encoderDesc);
	g.framePass = nullptr;
	g.framePassTarget = 0;
	g.surfaceCleared = false;
	g.scaledCleared = false;
	g.scaledTargetDrawn = false;
	g.vertexBufferUsedBytes = 0;
	g.cameraSlotsUsed = 0;
	g.clearColor = {0, 0, 0, 1}; // gl2d's glClear default until clearScreen says otherwise
	g.frameOpen = true;
}

void wgpuEndFrame()
{
	if (!g.frameOpen) { return; }

	// Nothing asked for the surface this frame (no UI, no screen-space draw):
	// upscale the low-res target now.
	compositeScaledTarget();

	// A frame with no draws still clears; begin the pass so the load op runs.
	ensurePassBegun(0);
	if (g.framePass)
	{
		g.framePass.end();
		g.framePass.release();
		g.framePass = nullptr;
		g.framePassTarget = 0;
	}

	captureRecordCopy(); // before finish(): a copy cannot go inside a pass

	// Seal the recording into a command buffer, submit, present.
	CommandBufferDescriptor cmdDesc = Default;
	cmdDesc.label = StringView("frame commands");
	CommandBuffer commands = g.frameEncoder.finish(cmdDesc);
	g.frameEncoder.release();
	g.frameEncoder = nullptr;

	g.queue.submit(1, &commands);
	commands.release();

	// N2b. Only one in flight: if the previous frame's work has not been
	// reported yet, this frame goes unmeasured rather than overwriting the
	// start time out from under it.
	if (!workDonePending)
	{
		QueueWorkDoneCallbackInfo workInfo = Default;
		workInfo.mode = CallbackMode::AllowProcessEvents;
		workInfo.callback = onQueueWorkDone;
		workInfo.userdata1 = nullptr;
		workInfo.userdata2 = nullptr;
		submitAt = std::chrono::steady_clock::now();
		workDonePending = true;
		g.queue.onSubmittedWorkDone(workInfo);
	}

	captureResolve(); // after submit: the copy has to have run

	// The other waiting place. With Fifo this is where a frame that arrived
	// early sits until its vsync.
	const auto presentStart = std::chrono::steady_clock::now();
	g.surface.present(); // this is glfwSwapBuffers
	framePerf.presentMs = std::chrono::duration<float, std::milli>(
		std::chrono::steady_clock::now() - presentStart).count();

	g.frameView.release();
	g.frameView = nullptr;
	g.frameTexture.release();
	g.frameTexture = nullptr;
	g.frameOpen = false;

	// Let pending callbacks (errors, device lost, queue work done) run once
	// per frame. The work-done callback for an earlier frame usually lands
	// here, which is why gpuMillis lags by a frame or two.
	g.instance.processEvents();

	statsInProgress.pipelineVariants = 0;
	for (const PipelineEntry &e : g.quadPipelines)
	{
		if (e.pipeline) { ++statsInProgress.pipelineVariants; } // known-bad entries do not count
	}
	statsInProgress.gpuMillis = lastGpuMillis;
	statsInProgress.pipelineBuilds = framePerf.pipelineBuilds;
	statsInProgress.pipelineFailures = framePerf.pipelineFailures;
	statsInProgress.validationErrors = framePerf.validationErrors;
	statsInProgress.texturesCreated = framePerf.texturesCreated;
	statsInProgress.blockedMs = framePerf.blockedMs;
	statsInProgress.acquireMs = framePerf.acquireMs;
	statsInProgress.presentMs = framePerf.presentMs;
	statsLastFrame = statsInProgress;

	if (!g.firstFramePresented)
	{
		g.firstFramePresented = true;
		std::cout << "WebGPU first frame presented\n";
		std::cout.flush();
	}
}

void wgpuShutdown()
{
	if (g.frameOpen)
	{
		// Shouldn't happen (the loop always ends the frame), but never leave
		// an encoder dangling.
		if (g.framePass) { g.framePass.end(); g.framePass.release(); g.framePass = nullptr; }
		if (g.frameEncoder) { g.frameEncoder.release(); g.frameEncoder = nullptr; }
		if (g.frameView) { g.frameView.release(); g.frameView = nullptr; }
		if (g.frameTexture) { g.frameTexture.release(); g.frameTexture = nullptr; }
		g.frameOpen = false;
	}
	clearBatch();
	if (g.effectBindGroup) { g.effectBindGroup.release(); g.effectBindGroup = nullptr; }
	if (g.effectBuffer) { g.effectBuffer.release(); g.effectBuffer = nullptr; g.effectSlotCapacity = 0; }
	if (g.effectBindGroupLayout) { g.effectBindGroupLayout.release(); g.effectBindGroupLayout = nullptr; }
	if (g.cameraBindGroup) { g.cameraBindGroup.release(); g.cameraBindGroup = nullptr; }
	if (g.uniformBuffer) { g.uniformBuffer.release(); g.uniformBuffer = nullptr; g.cameraSlotCapacity = 0; }
	if (g.vertexBuffer) { g.vertexBuffer.release(); g.vertexBuffer = nullptr; g.vertexBufferCapacityBytes = 0; }
	for (TextureEntry &t : textures)
	{
		if (t.bindGroup) { t.bindGroup.release(); }
		if (t.view) { t.view.release(); }
		if (t.texture) { t.texture.release(); }
	}
	textures.clear();
	g.scaledTargetId = 0;
	g.scaledTargetDrawn = false;
	for (PipelineEntry &entry : g.quadPipelines)
	{
		if (entry.pipeline) { entry.pipeline.release(); }
	}
	g.quadPipelines.clear();
	for (ShaderEntry &e : shaderRegistry) { if (e.module) { e.module.release(); } }
	shaderRegistry.clear();
	for (BindGroup &bg : g.glowGroups) { if (bg) { bg.release(); bg = nullptr; } }
	if (g.glowParamBuffer) { g.glowParamBuffer.release(); g.glowParamBuffer = nullptr; }
	if (g.brightpassPipeline) { g.brightpassPipeline.release(); g.brightpassPipeline = nullptr; }
	if (g.blurPipeline) { g.blurPipeline.release(); g.blurPipeline = nullptr; }
	if (g.glowPipelineLayout) { g.glowPipelineLayout.release(); g.glowPipelineLayout = nullptr; }
	if (g.glowBindGroupLayout) { g.glowBindGroupLayout.release(); g.glowBindGroupLayout = nullptr; }
	if (g.brightpassShaderModule) { g.brightpassShaderModule.release(); g.brightpassShaderModule = nullptr; }
	if (g.blurShaderModule) { g.blurShaderModule.release(); g.blurShaderModule = nullptr; }
	if (g.mipmapPipeline) { g.mipmapPipeline.release(); g.mipmapPipeline = nullptr; }
	if (g.mipmapPipelineLayout) { g.mipmapPipelineLayout.release(); g.mipmapPipelineLayout = nullptr; }
	if (g.mipmapBindGroupLayout) { g.mipmapBindGroupLayout.release(); g.mipmapBindGroupLayout = nullptr; }
	if (g.mipmapShaderModule) { g.mipmapShaderModule.release(); g.mipmapShaderModule = nullptr; }
	if (g.quadShaderModule) { g.quadShaderModule.release(); g.quadShaderModule = nullptr; }
	if (g.pipelineLayout) { g.pipelineLayout.release(); g.pipelineLayout = nullptr; }
	if (g.cameraBindGroupLayout) { g.cameraBindGroupLayout.release(); g.cameraBindGroupLayout = nullptr; }
	if (g.textureBindGroupLayout) { g.textureBindGroupLayout.release(); g.textureBindGroupLayout = nullptr; }
	if (g.samplerPixelated) { g.samplerPixelated.release(); g.samplerPixelated = nullptr; }
	if (g.samplerLinear) { g.samplerLinear.release(); g.samplerLinear = nullptr; }
	if (g.samplerPixelatedRepeat) { g.samplerPixelatedRepeat.release(); g.samplerPixelatedRepeat = nullptr; }
	if (g.samplerLinearRepeat) { g.samplerLinearRepeat.release(); g.samplerLinearRepeat = nullptr; }
	if (capture.buffer) { capture.buffer.release(); capture.buffer = nullptr; capture.bufferBytes = 0; }
	forgetSurface();
	if (g.queue) { g.queue.release(); g.queue = nullptr; }
	if (g.device) { g.device.release(); g.device = nullptr; }
	if (g.adapter) { g.adapter.release(); g.adapter = nullptr; }
	if (g.instance) { g.instance.release(); g.instance = nullptr; }
}

// -------------------------------------------------------------------------
// Accessors for the rest of the render layer (render/wgpuFrame.h). The
// context stays private to this file; these hand out exactly what another
// render translation unit needs to record into the frame.
// -------------------------------------------------------------------------

WGPUAdapter wgpuAdapter() { return g.adapter; }
Device wgpuDevice() { return g.device; }
Queue wgpuQueue() { return g.queue; }
TextureFormat wgpuSurfaceFormat() { return g.surfaceFormat; }
BindGroupLayout wgpuTextureBindGroupLayout() { return g.textureBindGroupLayout; }

BindGroup wgpuTextureBindGroup(uint32_t textureId)
{
	if (textureId == 0 || textureId > textures.size()) { return nullptr; }
	return textures[textureId - 1].bindGroup;
}

void wgpuSurfaceSize(int &width, int &height)
{
	width = g.surfaceWidth;
	height = g.surfaceHeight;
}

ShaderModule wgpuCreateShaderModuleFromFile(const char *path)
{
	return createShaderModuleFromFile(path);
}

RenderPassEncoder wgpuCurrentRenderPass()
{
	// The surface, always: the UI is drawn at native resolution on top of
	// the composited world, never inside a render target.
	if (!ensurePassBegun(0)) { return nullptr; }
	return g.framePass;
}

// The two halves of the ErrorScope guard declared in wgpuFrame.h. They live
// here because they need the device and the instance; the guard lives in the
// header so wgpuImgui.cpp gets the same one.
void wgpuBeginErrorScope(const char *what)
{
	if (!g.device) { return; }
	errorScopeNames.push_back(what ? what : "(unnamed)");
	g.device.pushErrorScope(ErrorFilter::Validation);
}

bool wgpuEndErrorScope()
{
	if (!g.device || errorScopeNames.empty()) { return false; }

	ErrorScopeResult result;
	result.what = errorScopeNames.back();
	errorScopeNames.pop_back();

	PopErrorScopeCallbackInfo info = Default;
	info.mode = CallbackMode::AllowProcessEvents;
	info.callback = onPopErrorScope;
	info.userdata1 = &result;
	info.userdata2 = nullptr;
	g.device.popErrorScope(info);

	// The same bounded pump the adapter and device requests use in wgpuInit.
	// Bounded because a scope that never resolves must not hang the game.
	//
	// This blocks, and the time is counted: a scope drained mid-frame is one
	// of the few things that can make a single frame cost tens of
	// milliseconds, so a slow frame should be able to say how much of itself
	// went here.
	const auto blockStart = std::chrono::steady_clock::now();
	for (int i = 0; !result.done && i < 1000; i++)
	{
		g.instance.processEvents();
		if (!result.done)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	framePerf.blockedMs += std::chrono::duration<float, std::milli>(
		std::chrono::steady_clock::now() - blockStart).count();
	if (!result.done)
	{
		std::cerr << "WebGPU: error scope for " << result.what << " never completed\n";
	}
	return result.failed;
}

} // namespace render

// -------------------------------------------------------------------------
// wgpu2d: gl2d's API on top of the context above
// -------------------------------------------------------------------------
namespace wgpu2d
{
	using namespace render;

	Effect createEffect(const char *wgslFragmentSource, const char *name)
	{
		Effect effect;
		effect.id = render::registerEffectShader(wgslFragmentSource, name);
		return effect;
	}

	void setFinalEffect(Effect effect, const EffectParams &params)
	{
		render::g.finalEffectId = effect.id;
		render::g.finalEffectParams = params;
	}

	void clearFinalEffect()
	{
		render::g.finalEffectId = 0;
		render::g.finalEffectParams = {};
	}

	void setRenderScale(float scale)
	{
		if (!(scale > 0.f)) { return; }
		render::g.renderScale = std::min(scale, 1.f);
	}

	float renderScale() { return render::g.renderScale; }

	void setFinalGlow(const FinalGlow &glow)
	{
		render::g.finalGlow = glow;
		render::g.finalGlowSet = true;
	}

	void clearFinalGlow() { render::g.finalGlowSet = false; }

	void Renderer2D::drawFullscreenEffect(Texture source, Effect effect, const EffectParams &params)
	{
		using namespace render;
		if (effect.id == 0 || source.id == 0) { return; }
		if (g.surfaceWidth <= 0 || g.surfaceHeight <= 0) { return; }

		// Whatever is pending belongs underneath, so it goes out first.
		flush();

		// No slot bookkeeping here any more. F6 moved that into the batch: the
		// quad below carries its effect and its parameters like any other, and
		// flushBatch allocates and writes the slot. A full-screen effect is
		// now just a quad that happens to cover the screen.
		//
		// The same swap compositeScaledTarget uses: the batch is the caller's,
		// and this borrows it for exactly one quad rather than adding a second
		// vertex buffer that would be idle most of the time.
		BorrowedBatch borrowed;

		const glm::vec4 white[4] = { {1,1,1,1}, {1,1,1,1}, {1,1,1,1}, {1,1,1,1} };
		pushQuad(Camera{}, BlendMode::Premultiplied, effect.id, params,
			glm::vec4{ 0, 0, (float)g.surfaceWidth, (float)g.surfaceHeight },
			source, white, {}, 0.f, WGPU2D_DefaultTextureCoords);

		// resolveTarget, not a literal 0. A full-screen effect is an ordinary
		// screen-space draw and has to land wherever screen-space draws are
		// going -- which is the frame's target when the frame is being routed
		// through one. Flushing to 0 put the cloak straight onto the surface
		// while everything drawn after it went into the target, and the
		// end-of-frame composite then covered the cloak with a target that had
		// never received the world: the screen went black.
		//
		// compositeScaledTarget flushes to a literal 0 and is right to: putting
		// the target *onto* the surface is the one draw that must not be
		// redirected back into it.
		flushBatch(resolveTarget(0));
	}

	FrameStats frameStats() { return render::statsLastFrame; }

	void init()
	{
		// The context is created by render::wgpuInit in the platform layer.
	}

	// ---- Texture ----
	glm::ivec2 Texture::GetSize()
	{
		if (id == 0 || id > textures.size()) { return {0, 0}; }
		return { textures[id - 1].width, textures[id - 1].height };
	}

	void Texture::createFromBufferTopFirst(const char *image_data, const int width, const int height, bool pixelated, bool useMipMaps)
	{
		*this = createTextureFromPixels((const unsigned char *)image_data, width, height, "buffer", pixelated, useMipMaps);
	}

	void Texture::create1PxSquare(const char *b)
	{
		if (b == nullptr)
		{
			const unsigned char buff[] = { 0xff, 0xff, 0xff, 0xff };
			*this = createTextureFromPixels(buff, 1, 1, "1px white", false, false);
		}
		else
		{
			*this = createTextureFromPixels((const unsigned char *)b, 1, 1, "1px", false, false);
		}
	}

	// Rows come back from stb_image top-first and are uploaded as-is: no
	// flip (see the milestone 4 orientation decision).
	void Texture::loadFromFile(const char *fileName, bool pixelated, bool useMipMaps, bool repeat)
	{
		int width = 0, height = 0, channels = 0;
		stbi_set_flip_vertically_on_load(0);
		unsigned char *pixels = stbi_load(fileName, &width, &height, &channels, 4);
		if (!pixels)
		{
			std::cerr << "wgpu2d: error openning: " << fileName << " (" << stbi_failure_reason() << ")\n";
			return;
		}
		*this = createTextureFromPixels(pixels, width, height, fileName, pixelated, useMipMaps, repeat);
		stbi_image_free(pixels);
	}

	void Texture::loadFromFileWithPixelPadding(const char *fileName, int blockSize, bool pixelated, bool useMipMaps)
	{
		std::vector<unsigned char> fileData;
		if (!readBinaryFile(fileName, fileData))
		{
			std::cerr << "wgpu2d: error openning: " << fileName << "\n";
			return;
		}
		*this = createPaddedTextureFromFileData(fileData.data(), fileData.size(), blockSize, pixelated, useMipMaps, fileName);
	}

	void Texture::cleanup()
	{
		if (id == 0 || id > textures.size()) { id = 0; return; }
		TextureEntry &t = textures[id - 1];
		if (t.bindGroup) { t.bindGroup.release(); t.bindGroup = nullptr; }
		if (t.view) { t.view.release(); t.view = nullptr; }
		if (t.texture) { t.texture.release(); t.texture = nullptr; }
		id = 0;
	}

	void Renderer2D::flushFBO(FrameBuffer frameBuffer, bool clearDrawData)
	{
		if (frameBuffer.fbo == 0)
		{
			std::cerr << "wgpu2d: Framebuffer not initialized\n";
			if (clearDrawData) { clearBatch(); }
			return;
		}
		flushBatch(frameBuffer.fbo);
		if (clearDrawData) { clearBatch(); }
	}

	// ---- FrameBuffer ----
	void FrameBuffer::create(unsigned int w, unsigned int h, bool pixelated)
	{
		cleanup();
		texture = createRenderTarget((int)w, (int)h, pixelated, "framebuffer");
		fbo = texture.id; // no framebuffer object exists; the texture is the target
	}

	void FrameBuffer::resize(unsigned int w, unsigned int h)
	{
		if (fbo == 0) { create(w, h); return; }
		if (fbo > textures.size()) { return; }
		if (textures[fbo - 1].width == (int)w && textures[fbo - 1].height == (int)h) { return; }

		// A texture cannot be resized: the slot is refilled with a new one so
		// the handles the caller is holding stay valid.
		texture = createRenderTarget((int)w, (int)h, textures[fbo - 1].pixelated, "framebuffer", fbo);
		fbo = texture.id;
	}

	void FrameBuffer::cleanup()
	{
		texture.cleanup();
		fbo = 0;
	}

	void FrameBuffer::clear()
	{
		if (fbo == 0 || fbo > textures.size()) { return; }
		textures[fbo - 1].needsClear = true;
		// gl2d clears immediately; do the same when there is a frame to
		// record into, otherwise the flag makes the next pass clear.
		if (g.frameOpen) { ensurePassBegun(fbo); }
	}

	static bool readTexels(const TextureEntry &entry, uint32_t mipLevel, int width, int height,
		std::vector<unsigned char> &texels, const char *label);

	// Texels stay in the target's format -- this is a round trip, not an
	// image to look at, so nothing is swizzled.
	bool FrameBuffer::read(std::vector<unsigned char> &texels)
	{
		if (fbo == 0 || fbo > textures.size() || !g.device) { return false; }
		const TextureEntry &entry = textures[fbo - 1];
		texels.assign((size_t)entry.width * 4 * entry.height, 0);
		// A clear still waiting is what the target holds.
		if (entry.needsClear) { return true; }

		return readTexels(entry, 0, entry.width, entry.height, texels, "framebuffer read");
	}

	// ourCraft: gl2d's readTextureData. Rows top first, RGBA, one mip level.
	// It waits for the GPU, so it is for load time (the block loader reads
	// every block texture this way), not for every frame.
	std::vector<unsigned char> Texture::readTextureData(int mipLevel, glm::ivec2 *outSize)
	{
		glm::ivec2 size = {};
		std::vector<unsigned char> texels;

		if (id != 0 && id <= textures.size() && g.device && mipLevel >= 0)
		{
			const TextureEntry &entry = textures[id - 1];
			if (entry.texture && (uint32_t)mipLevel < entry.texture.getMipLevelCount())
			{
				size.x = std::max(1, entry.width >> mipLevel);
				size.y = std::max(1, entry.height >> mipLevel);
				texels.assign((size_t)size.x * size.y * 4, 0);
				readTexels(entry, (uint32_t)mipLevel, size.x, size.y, texels, "readTextureData");

				// Render targets are in the surface's format, BGRA here.
				if (entry.format == TextureFormat::BGRA8Unorm || entry.format == TextureFormat::BGRA8UnormSrgb)
				{
					for (size_t i = 0; i + 3 < texels.size(); i += 4) { std::swap(texels[i], texels[i + 2]); }
				}

				// Stored top-first; gl2d's callers expect bottom-first.
				const size_t rowBytes = (size_t)size.x * 4;
				for (int y = 0; y < size.y / 2; y++)
				{
					std::swap_ranges(texels.begin() + y * rowBytes, texels.begin() + (y + 1) * rowBytes,
						texels.begin() + (size.y - 1 - y) * rowBytes);
				}
			}
		}

		if (outSize) { *outSize = size; }
		return texels;
	}

	// The copy's rules, the same as the frame capture's: a buffer row is a
	// multiple of 256 bytes, so rows are padded on the way and unpadded here.
	// `texels` is already sized width * height * 4 and receives the level as
	// it is stored.
	static bool readTexels(const TextureEntry &entry, uint32_t mipLevel, int width, int height,
		std::vector<unsigned char> &texels, const char *label)
	{
		const uint32_t row = (uint32_t)width * 4;
		const uint32_t paddedRow = ceilToNextMultiple(row, 256);
		const uint64_t total = (uint64_t)paddedRow * height;

		ErrorScope scope(label);
		BufferDescriptor desc = Default;
		desc.label = StringView(label);
		desc.usage = BufferUsage::CopyDst | BufferUsage::MapRead;
		desc.size = total;
		desc.mappedAtCreation = false;
		Buffer buffer = g.device.createBuffer(desc);
		if (!buffer) { return false; }

		CommandEncoderDescriptor encoderDesc = Default;
		encoderDesc.label = StringView(label);
		CommandEncoder encoder = g.device.createCommandEncoder(encoderDesc);

		TexelCopyTextureInfo source = Default;
		source.texture = entry.texture;
		source.mipLevel = mipLevel;
		source.origin = { 0, 0, 0 };
		source.aspect = TextureAspect::All;
		TexelCopyBufferInfo destination = Default;
		destination.buffer = buffer;
		destination.layout.offset = 0;
		destination.layout.bytesPerRow = paddedRow;
		destination.layout.rowsPerImage = (uint32_t)height;
		Extent3D size = { (uint32_t)width, (uint32_t)height, 1 };
		encoder.copyTextureToBuffer(source, destination, size);

		CommandBufferDescriptor bufferDesc = Default;
		bufferDesc.label = StringView(label);
		CommandBuffer commands = encoder.finish(bufferDesc);
		g.queue.submit(1, &commands);
		commands.release();
		encoder.release();

		// Mapped as the capture is: asynchronously, drained with a bounded pump.
		bool mapped = false;
		BufferMapCallbackInfo info = Default;
		info.mode = CallbackMode::AllowProcessEvents;
		info.callback = onCaptureMapped;
		info.userdata1 = &mapped;
		info.userdata2 = nullptr;
		buffer.mapAsync(MapMode::Read, 0, (size_t)total, info);
		for (int i = 0; !mapped && i < 3000; i++)
		{
			g.instance.processEvents();
			if (!mapped) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
		}
		const unsigned char *src = mapped ? static_cast<const unsigned char *>(buffer.getConstMappedRange(0, (size_t)total)) : nullptr;
		if (src)
		{
			for (int y = 0; y < height; y++)
			{
				std::memcpy(texels.data() + (size_t)y * row, src + (size_t)y * paddedRow, row);
			}
			buffer.unmap();
		}
		else { std::cerr << "WebGPU: " << label << " never mapped\n"; }
		buffer.release();
		return src != nullptr && !scope.failed();
	}

	bool FrameBuffer::write(const std::vector<unsigned char> &texels)
	{
		if (fbo == 0 || fbo > textures.size() || !g.device) { return false; }
		TextureEntry &entry = textures[fbo - 1];
		const uint32_t row = (uint32_t)entry.width * 4;
		if (texels.size() != (size_t)row * entry.height) { return false; }

		TexelCopyTextureInfo destination = Default;
		destination.texture = entry.texture;
		destination.mipLevel = 0;
		destination.origin = { 0, 0, 0 };
		destination.aspect = TextureAspect::All;
		Extent3D size = { (uint32_t)entry.width, (uint32_t)entry.height, 1 };

		ErrorScope scope("framebuffer write");
		if (!g.frameOpen)
		{
			// Nothing recorded yet: the queue's own write is first in line.
			TexelCopyBufferLayout layout = Default;
			layout.offset = 0;
			layout.bytesPerRow = row;
			layout.rowsPerImage = (uint32_t)entry.height;
			g.queue.writeTexture(destination, texels.data(), texels.size(), layout, size);
		}
		else
		{
			// A frame is being recorded, and the queue's write would run before
			// all of it -- before a clear this frame already asked for. So the
			// copy goes into the frame's own encoder, from a staging buffer,
			// which cannot happen inside a pass: the open one ends, and the
			// next draw begins another, loading.
			if (g.framePass)
			{
				g.framePass.end();
				g.framePass.release();
				g.framePass = nullptr;
			}
			const uint32_t paddedRow = ceilToNextMultiple(row, 256);
			const uint64_t total = (uint64_t)paddedRow * entry.height;
			BufferDescriptor desc = Default;
			desc.label = StringView("framebuffer write");
			desc.usage = BufferUsage::CopySrc | BufferUsage::MapWrite;
			desc.size = total;
			desc.mappedAtCreation = true;
			Buffer buffer = g.device.createBuffer(desc);
			if (!buffer) { return false; }
			unsigned char *dst = static_cast<unsigned char *>(buffer.getMappedRange(0, (size_t)total));
			if (!dst) { buffer.release(); return false; }
			for (int y = 0; y < entry.height; y++)
			{
				std::memcpy(dst + (size_t)y * paddedRow, texels.data() + (size_t)y * row, row);
			}
			buffer.unmap();

			TexelCopyBufferInfo source = Default;
			source.buffer = buffer;
			source.layout.offset = 0;
			source.layout.bytesPerRow = paddedRow;
			source.layout.rowsPerImage = (uint32_t)entry.height;
			g.frameEncoder.copyBufferToTexture(source, destination, size);
			// Kept alive by the encoder until the frame's work is done.
			buffer.release();
		}
		// What was written is the contents now, not a clear still to come.
		entry.needsClear = false;
		return !scope.failed();
	}

	// ---- Atlas math, verbatim from gl2d ----
	glm::vec4 computeTextureAtlas(int xCount, int yCount, int x, int y, bool flip)
	{
		float xSize = 1.f / xCount;
		float ySize = 1.f / yCount;

		if (flip)
		{
			return { (x + 1) * xSize, 1 - (y * ySize), (x)*xSize, 1.f - ((y + 1) * ySize) };
		}
		else
		{
			return { x * xSize, 1 - (y * ySize), (x + 1) * xSize, 1.f - ((y + 1) * ySize) };
		}
	}

	glm::vec4 computeTextureAtlasWithPadding(int mapXsize, int mapYsize,
		int xCount, int yCount, int x, int y, bool flip)
	{
		float xSize = 1.f / xCount;
		float ySize = 1.f / yCount;

		float Xpadding = 1.f / mapXsize;
		float Ypadding = 1.f / mapYsize;

		glm::vec4 noFlip = { x * xSize + Xpadding, 1 - (y * ySize) - Ypadding, (x + 1) * xSize - Xpadding, 1.f - ((y + 1) * ySize) + Ypadding };

		if (flip)
		{
			glm::vec4 flip = { noFlip.z, noFlip.y, noFlip.x, noFlip.w };

			return flip;
		}
		else
		{
			return noFlip;
		}
	}

	// ---- Renderer2D ----
	void Renderer2D::create(unsigned int fbo, size_t quadCount)
	{
		batchVertices.reserve(quadCount * 6);
		batchQuadTextures.reserve(quadCount);
		batchQuadCameras.reserve(quadCount);
		currentCamera = Camera{};
		defaultFBO = fbo; // gl2d's defaultFBO: 0 is the screen
	}

	void Renderer2D::cleanup()
	{
		clearDrawData();
	}

	void Renderer2D::pushCamera(Camera c)
	{
		cameraPushPop.push_back(currentCamera);
		currentCamera = c;
	}

	void Renderer2D::popCamera()
	{
		if (cameraPushPop.empty())
		{
			std::cerr << "wgpu2d: Pop on an empty stack on popCamera\n";
		}
		else
		{
			currentCamera = cameraPushPop.back();
			cameraPushPop.pop_back();
		}
	}

	// Verbatim from gl2d.
	glm::vec4 Renderer2D::getViewRect()
	{
		auto rect = glm::vec4{0, 0, windowW, windowH};

		glm::mat3 mat =
		{1.f, 0, currentCamera.position.x ,
		 0, 1.f, currentCamera.position.y,
		 0, 0, 1.f};
		mat = glm::transpose(mat);

		glm::vec3 pos1 = {rect.x, rect.y, 1.f};
		glm::vec3 pos2 = {rect.z + rect.x, rect.w + rect.y, 1.f};

		pos1 = mat * pos1;
		pos2 = mat * pos2;

		glm::vec2 point((pos1.x + pos2.x) / 2.f, (pos1.y + pos2.y) / 2.f);

		auto scaleAroundPoint = [](glm::vec2 vec, glm::vec2 point, float scale) { return (vec - point) * scale + point; };
		pos1 = glm::vec3(scaleAroundPoint(pos1, point, 1.f/currentCamera.zoom), 1.f);
		pos2 = glm::vec3(scaleAroundPoint(pos2, point, 1.f/currentCamera.zoom), 1.f);

		rect = {pos1.x, pos1.y, pos2.x - pos1.x, pos2.y - pos1.y};

		return rect;
	}

	void Renderer2D::clearDrawData()
	{
		clearBatch();
	}

	void setCullMirroredQuads(bool cull) { cullMirroredQuads = cull; }

	// gl2d's renderRectangle: the rotation origin is relative to the
	// rectangle's center.
	void Renderer2D::renderRectangle(const Rect transforms, const Texture texture, const Color4f colors[4],
		const glm::vec2 origin, const float rotationDegrees, const glm::vec4 textureCoords)
	{
		glm::vec2 newOrigin;
		newOrigin.x = origin.x + transforms.x + (transforms.z / 2);
		newOrigin.y = origin.y + transforms.y + (transforms.w / 2);
		renderRectangleAbsRotation(transforms, texture, colors, newOrigin, rotationDegrees, textureCoords);
	}

	void Renderer2D::renderTriangles(const glm::vec2 *positions, const glm::vec2 *uvs,
		const Color4f *colors, size_t vertexCount, const Texture texture)
	{
		pushTriangles(currentCamera, currentBlendMode, currentEffect.id, currentEffectParams,
			texture, positions, uvs, colors, vertexCount);
	}

	void Renderer2D::renderRectangleAbsRotation(const Rect transforms, const Texture texture, const Color4f colors[4],
		const glm::vec2 origin, const float rotationDegrees, const glm::vec4 textureCoords)
	{
		pushQuad(currentCamera, currentBlendMode, currentEffect.id, currentEffectParams,
			transforms, texture, colors, origin, rotationDegrees, textureCoords);
	}

	void Renderer2D::renderRectangle(const Rect transforms, const Color4f colors[4], const glm::vec2 origin, const float rotationDegrees)
	{
		renderRectangle(transforms, white1pxSquareTexture, colors, origin, rotationDegrees);
	}

	// gl2d's renderLine, both forms, verbatim.
	void Renderer2D::renderLine(const glm::vec2 position, const float angleDegrees, const float length, const Color4f color, const float width)
	{
		renderRectangle({position - glm::vec2(0,width / 2.f), length, width},
			color, {-length/2, 0}, angleDegrees);
	}

	void Renderer2D::renderLine(const glm::vec2 start, const glm::vec2 end, const Color4f color, const float width)
	{
		glm::vec2 vector = end - start;
		float length = glm::length(vector);
		float angle = std::atan2(vector.y, vector.x);
		renderLine(start, -glm::degrees(angle), length, color, width);
	}

	// gl2d's renderCircleOutline, verbatim: a polygon of lines.
	void Renderer2D::renderCircleOutline(const glm::vec2 position, const Color4f color, const float size, const float width, const unsigned int segments)
	{
		auto calcPos = [&](int p)
		{
			glm::vec2 circle = {size,0};

			float a = 3.1415926 * 2 * ((float)p / segments);

			float c = std::cos(a);
			float s = std::sin(a);

			circle = {c * circle.x - s * circle.y, s * circle.x + c * circle.y};

			return circle + position;
		};

		glm::vec2 lastPos = calcPos(1);
		renderLine(calcPos(0), lastPos, color, width);
		for (int i = 1; i < segments; i++)
		{
			glm::vec2 pos1 = lastPos;
			glm::vec2 pos2 = calcPos(i + 1);

			renderLine(pos1, pos2, color, width);

			lastPos = pos2;
		}
	}

	void Renderer2D::clearScreen(const Color4f color)
	{
		g.clearColor = color;
	}

	void Renderer2D::flush(bool clearDrawData)
	{
		if (windowW <= 0 || windowH <= 0)
		{
			if (windowW < 0 || windowH < 0)
			{
				std::cerr << "wgpu2d: Negative windowW or windowH, have you forgotten to call updateWindowMetrics(w, h)?\n";
			}
			if (clearDrawData) { clearBatch(); }
			return;
		}
		flushBatch(resolveTarget(defaultFBO));
		if (clearDrawData) { clearBatch(); }
	}
}
