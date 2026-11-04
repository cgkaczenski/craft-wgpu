#pragma once

// The render layer's internal view of the WebGPU context. wgpuContext.h is
// the platform layer's view and deliberately mentions no WebGPU types; this
// header does the opposite and is only for files under src/render, which
// already include <webgpu/webgpu.hpp>.
//
// Milestone 8 added it: the ImGui backend lives in its own translation unit
// and needs the device, the queue, the surface format, the frame's render
// pass, and the bind group behind a wgpu2d::Texture id.

#include <webgpu/webgpu.hpp>
#include <cstdint>

namespace render
{
	// Created once by wgpuInit, valid until wgpuShutdown. Null before init.
	wgpu::Device wgpuDevice();
	wgpu::Queue wgpuQueue();
	wgpu::TextureFormat wgpuSurfaceFormat();

	// Group 0 of the sprite pipeline: a sampled 2D texture plus its sampler.
	// Any pipeline that samples a wgpu2d::Texture reuses this layout so it
	// can bind the group the texture registry already built for it.
	wgpu::BindGroupLayout wgpuTextureBindGroupLayout();

	// The bind group for a wgpu2d::Texture handle's id, or null if the id is
	// not a live texture.
	wgpu::BindGroup wgpuTextureBindGroup(uint32_t textureId);

	// The size the surface is currently configured at, in framebuffer pixels.
	void wgpuSurfaceSize(int &width, int &height);

	// Reads a WGSL file and compiles it. Errors arrive through the device's
	// uncaptured-error callback. Returns null if the file cannot be read.
	wgpu::ShaderModule wgpuCreateShaderModuleFromFile(const char *path);

	// The frame's single render pass, begun (clearing to the recorded color)
	// if nothing has drawn yet. Null when no frame is open. Whatever is
	// recorded into it after the game's flush draws on top.
	wgpu::RenderPassEncoder wgpuCurrentRenderPass();

	// ---- Diagnostics ----------------------------------------------------
	//
	// The device's uncaptured-error callback is the catch-all: it reports
	// errors no scope claimed, and it cannot say which call produced them --
	// it can even arrive after the null handle it explains. An error scope
	// claims errors of one class for one span of work, so the message comes
	// back attributed to a named operation.
	//
	// Ending a scope is asynchronous, and wgpuEndErrorScope drains it by
	// pumping the instance, so a scope costs a stall. Use them around object
	// *creation*, never around per-frame recording.
	//
	// Prefer the ErrorScope guard below; these two exist so it can live in a
	// header while the implementation stays with the context.
	void wgpuBeginErrorScope(const char *what);
	bool wgpuEndErrorScope(); // true if the scope caught a validation error

	// RAII for the pair. `what` names the thing being created and must
	// outlive the guard -- every call site passes a literal or a path.
	class ErrorScope
	{
	public:
		explicit ErrorScope(const char *what) { wgpuBeginErrorScope(what); }
		~ErrorScope() { finish(); }

		ErrorScope(const ErrorScope &) = delete;
		ErrorScope &operator=(const ErrorScope &) = delete;

		// Ends the scope early and reports whether it caught anything, for
		// callers that want to fail rather than only log. Idempotent.
		bool failed()
		{
			finish();
			return caught;
		}

	private:
		void finish()
		{
			if (!open) { return; }
			open = false;
			caught = wgpuEndErrorScope();
		}

		bool open = true;
		bool caught = false;
	};

	// A label names an object; a debug group names a *span of recorded
	// commands*. A GPU capture (Xcode's Metal frame capture here) shows them
	// as a tree instead of a flat list of draws.
	//
	// Groups nest, and every push must be matched by a pop before the pass
	// ends -- an unbalanced group is a validation error. The render pass is
	// ended and restarted whenever the draw target changes, so a group must
	// never span a target switch, and a function that returns early between
	// the push and the pop must not leak one. Both are why this is a guard
	// and not a pair of calls.
	class DebugGroup
	{
	public:
		DebugGroup(wgpu::RenderPassEncoder pass, const char *name) : pass(pass)
		{
			if (!this->pass) { return; }
			this->pass.pushDebugGroup(wgpu::StringView(name));
			open = true;
		}

		~DebugGroup()
		{
			if (open) { pass.popDebugGroup(); }
		}

		DebugGroup(const DebugGroup &) = delete;
		DebugGroup &operator=(const DebugGroup &) = delete;

	private:
		wgpu::RenderPassEncoder pass = nullptr;
		bool open = false;
	};
}
