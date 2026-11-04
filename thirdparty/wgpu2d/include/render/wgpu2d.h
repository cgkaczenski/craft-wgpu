#pragma once

// wgpu2d: the 2D drawing library. The game includes this header and nothing
// else from render/. The GPU context (instance, device, surface, frame) lives
// in render/wgpuContext.h and is driven by the platform loop.
//
// It started as gl2d's signatures so the game could switch by include and
// namespace. New drawing capabilities belong here too — BlendMode, LayerEffect
// — rather than in sibling headers. Matching gl2d is how the port landed, not
// a ceiling on the API.
//
// One Renderer2D instance is supported (the game has one).

#include <glm/glm.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wgpu2d
{
	// gl2d::init() equivalent. The GPU context is created by
	// render::wgpuInit in the platform layer; this is a no-op kept for the
	// call sites.
	void init();

	// ---- gl2d compatibility (ourCraft) ----------------------------------
	//
	// gl2d's error hook. gl2d reported failed loads through it; wgpu2d
	// reports to stderr, and only the gl2d-compat font loader calls this.
	void defaultErrorFunc(const char *msg, void *userDefinedData);
	using errorFuncType = decltype(defaultErrorFunc);
	errorFuncType *setErrorFuncCallback(errorFuncType *newFunc);

	// Vsync is the surface's present mode (Fifo) here, not a swap interval.
	inline bool setVsync(bool) { return false; }

	// gl2d restored the GL state its batch needed. WebGPU bakes state into
	// pipelines, so there is none to restore.
	inline void enableNecessaryGLFeatures() {}

	// ourCraft draws gl2d with GL_CULL_FACE on (gameLayer.cpp enables it
	// every frame), so a quad mirrored in one axis by a negative width or
	// height faced away and was culled; glui's narrow slider knob depends on
	// it. With this on, renderRectangle skips such quads the same way.
	// Off by default: wgpu2d draws every quad.
	void setCullMirroredQuads(bool cull);

	using Color4f = glm::vec4;
	using Rect = glm::vec4; // x, y, width, height in world pixels, y down

	struct Renderer2D; // LayerEffect::flush takes one; the full type is below

	// An addition to gl2d's shape, not a mirror of it: gl2d had one blend
	// state for the whole program. Blending is baked into a render pipeline,
	// so a second mode means a second pipeline, and the batch has to break a
	// draw run when the mode changes -- the same way it already breaks on
	// texture and camera.
	enum class BlendMode
	{
		// gl2d's blend, and the default: src * srcAlpha + dst * (1 - srcAlpha).
		Alpha,
		// Light adds: src * srcAlpha + dst. Nothing gets darker, so overlapping
		// sprites build up. For muzzle flashes, explosions, thrusters.
		Additive,
		// src + dst * (1 - srcAlpha): "over", for a source whose colour is
		// already multiplied by its own coverage. That is exactly what a
		// render target holds -- drawing (C, a) into a transparent target with
		// Alpha leaves (C*a, a) -- so this is the mode for drawing a
		// FrameBuffer back. Using Alpha there multiplies by coverage a second
		// time and the result comes out dark.
		Premultiplied,
		// dst * srcAlpha, for colour; the destination's alpha is kept. What
		// is already drawn survives only as far as the source covers it, and
		// goes to black where it does not: a mask laid over a picture rather
		// than a picture laid over another. For darkness -- a fog of what has
		// never been seen, a light map's unlit parts -- or a cut-out. The
		// source's colour is ignored; only its coverage counts.
		Mask,
	};

	// gl2d's texture-coordinate convention: {u0, v0, u1, v1} with v measured
	// from the bottom. Converted to WebGPU's top-left origin inside the renderer.
	#define WGPU2D_DefaultTextureCoords (glm::vec4{ 0, 1, 1, 0 })

	#define Colors_Red (wgpu2d::Color4f{ 1, 0, 0, 1 })
	#define Colors_Green (wgpu2d::Color4f{ 0, 1, 0, 1 })
	#define Colors_Blue (wgpu2d::Color4f{ 0, 0, 1, 1 })
	#define Colors_Black (wgpu2d::Color4f{ 0, 0, 0, 1 })
	#define Colors_White (wgpu2d::Color4f{ 1, 1, 1, 1 })
	#define Colors_Yellow (wgpu2d::Color4f{ 1, 1, 0, 1 })
	#define Colors_Magenta (wgpu2d::Color4f{ 1, 0, 1, 1 })
	#define Colors_Turqoise (wgpu2d::Color4f{ 0, 1, 1, 1 })
	#define Colors_Orange (wgpu2d::Color4f{ 1, (float)0x7F / 255.0f, 0, 1 })
	#define Colors_Purple (wgpu2d::Color4f{ 101.0f / 255.0f, 29.0f / 255.0f, 173.0f / 255.0f, 1 })
	#define Colors_Gray (wgpu2d::Color4f{ (float)0x7F / 255.0f, (float)0x7F / 255.0f, (float)0x7F / 255.0f, 1 })
	#define Colors_Transparent (wgpu2d::Color4f{ 0,0,0,0 })

	// What the last completed frame cost, for a debug readout.
	//
	// The counts are the renderer's own and are exact. `gpuMillis` is not: it
	// is the wall time from submitting the frame to the queue reporting that
	// work done, so it includes queue waits and whatever the presentation
	// engine did, and it is a lower bound on frame latency rather than a
	// measure of GPU work. Real per-pass GPU timing wants timestamp queries,
	// which this adapter does not support -- see roadmap N2.
	struct FrameStats
	{
		int quads = 0;
		int drawRuns = 0;
		int cameras = 0;
		int flushes = 0;
		int pipelineVariants = 0;
		float gpuMillis = -1.f; // negative until the first callback lands

		// Diagnostics for a frame that took much longer than its neighbours.
		// Every one of these should be 0 in a steady frame; a non-zero value
		// names what the frame did that a normal one does not.
		int pipelineBuilds = 0;    // variants compiled this frame (expensive)
		int pipelineFailures = 0;  // builds that were rejected and discarded
		int validationErrors = 0;  // uncaptured errors reported this frame
		int texturesCreated = 0;   // textures or render targets allocated
		float blockedMs = 0.f;     // time spent draining async callbacks

		// Where a frame's wall time went outside our own work. Both are pure
		// waiting: acquire blocks until a drawable is free, present blocks
		// until the swapchain accepts one. A frame that is slow with both of
		// these near zero was slow doing something; a frame that is slow with
		// one of them large was slow *waiting*, and no amount of drawing less
		// would have helped.
		float acquireMs = 0.f;
		float presentMs = 0.f;
	};

	// The frame before this one. Reading it mid-frame gives a complete number
	// instead of a partial one, which is what a debug panel wants -- the panel
	// itself is drawn before the frame it is part of has finished.
	FrameStats frameStats();

	// ---- Effects (F2) ---------------------------------------------------
	//
	// An effect is a fragment shader the application supplies, run in place of
	// the sprite shader when a texture is drawn across the screen. It is how a
	// post-process happens: render the world into a FrameBuffer, then draw
	// that FrameBuffer back through an effect.
	//
	// **An effect is a fragment stage only.** The vertex stage is the sprite
	// one, so an effect inherits the vertex layout and the camera transform
	// and its author writes a single function. The WGSL it supplies must
	// declare exactly this much and then `fs_main`:
	//
	//     struct VertexOutput {
	//         @builtin(position) position: vec4f,
	//         @location(0) color: vec4f,
	//         @location(1) uv: vec2f,
	//     };
	//     @group(0) @binding(0) var spriteTexture: texture_2d<f32>;
	//     @group(0) @binding(1) var spriteSampler: sampler;
	//     struct EffectUniforms {
	//         resolution: vec4f,  // xy = pixels, zw = 1 / pixels
	//         time: vec4f,        // x = seconds since init
	//         a: vec4f,           // whatever this effect wants
	//         b: vec4f,
	//         c: vec4f,           // may be left out if unused
	//         d: vec4f,           // likewise
	//     };
	//     @group(2) @binding(0) var<uniform> effect: EffectUniforms;
	//
	//     @fragment
	//     fn fs_main(in: VertexOutput) -> @location(0) vec4f { ... }
	//
	// Group 1 is the camera and an effect will not normally touch it. A shader
	// may declare fewer groups than its pipeline layout has.
	struct Effect
	{
		uint32_t id = 0; // 0 means "no effect": the sprite shader
	};

	// Compiles WGSL into an effect. `name` is what a validation message and a
	// GPU capture will call it. Returns id 0 if the source does not compile,
	// and the compile error arrives on the error callback naming `name`.
	Effect createEffect(const char *wgslFragmentSource, const char *name);

	// The free vec4s an effect reads. Resolution and time are filled in by
	// the renderer, so they are not here.
	//
	// One set per draw, which is all a post-process needs: it is one draw by
	// nature. Per-*quad* parameters -- an impact point that differs per shield
	// -- are a harder problem and are roadmap F6.
	//
	// A shader that needs fewer may declare its struct without the later
	// fields: the binding is at least as large as the shader asks for, which
	// is all WebGPU requires. `c` was added when the CRT filled `a` and `b`
	// and still needed its switch-off and fade (gameplay roadmap L1); `d` when
	// a lit material -- a light, a surface and its tuning -- needed more than
	// twelve numbers (the asteroid shader, A5).
	struct EffectParams
	{
		glm::vec4 a = {};
		glm::vec4 b = {};
		glm::vec4 c = {};
		glm::vec4 d = {};
	};

	// F7: an effect the finished frame is composited through.
	//
	// While one is set, everything drawn to the screen lands in a full-size
	// target instead, and one quad brings it back through `effect` at the end
	// of the frame. What that buys over `drawFullscreenEffect` is *when*: the
	// composite happens the moment something first draws to the surface, so
	// the game and the HUD go through the filter and anything drawn after it
	// does not. The debug UI is drawn after, which is why it stays flat and
	// readable while the game curves.
	//
	// The target is sampled with linear filtering rather than the low-res
	// upscale's nearest. A filter that bends the coordinate asks for positions
	// between texels, and nearest gives rows of unequal thickness that crawl as
	// the camera moves -- which reads as a bug, not as a monitor.
	//
	// Costs a full-screen target and one extra pass while set, and exactly
	// nothing when cleared.
	void setFinalEffect(Effect effect, const EffectParams &params = {});
	void clearFinalEffect();

	// N4 step two / F7: a phosphor glow on the finished frame.
	//
	// Bright parts bleed into what is next to them, which is the one part of a
	// CRT that genuinely needs neighbouring pixels. It runs on the frame's own
	// target, before the final effect, so a CRT pass curves the glow along with
	// everything else instead of laying a flat bloom over a curved picture.
	//
	// Only meaningful while a final effect is set, because that is what puts
	// the frame in a target. Costs three compute dispatches and one extra quad
	// while set, and nothing when cleared.
	struct FinalGlow
	{
		float threshold = 0.55f; // brightness that starts to bloom, 0..1
		float knee = 0.25f;      // how soft that cut is; 0 is a hard edge
		float sigma = 3.0f;      // blur width, in half-resolution texels
		float intensity = 0.7f;  // how much of the blur is added back

		// Multiplies the glow as it is added back. White leaves the bloom the
		// colour of what bloomed; a warm tint makes a blue-white highlight
		// throw an amber halo, which is how a phosphor glow reads.
		glm::vec3 tint = {1.f, 1.f, 1.f};
	};
	// Milestone 10's low-resolution path, as a control rather than only as an
	// environment variable.
	//
	// Below 1, the game is rasterised into a smaller target and upscaled at the
	// end of the frame. The projection keeps using the surface's size, so the
	// framing, the HUD layout and the mouse mapping are untouched -- only the
	// pixel count changes. It is the remedy for a window large enough that the
	// fill rate stops keeping up, which is what fullscreen turned out to be.
	//
	// 1 renders at native size and costs nothing extra.
	void setRenderScale(float scale);
	float renderScale();

	void setFinalGlow(const FinalGlow &glow);
	void clearFinalGlow();

	// A copyable handle like gl2d's Texture { GLuint id }; 0 means invalid.
	struct Texture
	{
		uint32_t id = 0;

		Texture() {};

		glm::ivec2 GetSize();

		// RGBA8 pixels, rows top-first. Mipmaps are generated on the GPU.
		// (wgpu2d's own createFromBuffer; renamed in ourCraft, where that
		// name has gl2d's bottom-first rows, below.)
		void createFromBufferTopFirst(const char *image_data, const int width, const int height,
			bool pixelated = false, bool useMipMaps = true);

		void create1PxSquare(const char *b = 0);

		// `repeat`: coordinates outside 0..1 wrap instead of clamping to the
		// edge, for a tileable image laid over a mesh (renderTriangles).
		void loadFromFile(const char *fileName, bool pixelated = false, bool useMipMaps = true,
			bool repeat = false);

		// Like gl2d: re-lays the image out with a 2-pixel gutter around every
		// blockSize x blockSize cell (edge pixels duplicated) so filtering
		// never bleeds neighbors. Use TextureAtlasPadding for the coordinates.
		void loadFromFileWithPixelPadding(const char *fileName, int blockSize,
			bool pixelated = false, bool useMipMaps = true);

		void cleanup();

		// ---- gl2d compatibility (ourCraft) ------------------------------

		// gl2d's row order, which is OpenGL's: rows bottom-first. ourCraft
		// decodes with stbi_set_flip_vertically_on_load(true) and builds
		// buffers that way, so this flips them and calls
		// createFromBufferTopFirst.
		void createFromBuffer(const char *image_data, const int width, const int height,
			bool pixelated = false, bool useMipMaps = true);

		// An encoded image (PNG, JPG, ...) already in memory.
		void createFromFileData(const unsigned char *image_file_data, const size_t image_file_size,
			bool pixelated = false, bool useMipMaps = true);

		// gl2d bound the texture for raw GL calls that follow. Those calls
		// go to nullGL on this path, so there is nothing to bind.
		void bind(const unsigned int sample = 0) {}

		// One mip level's texels read back from the GPU: RGBA, rows
		// bottom-first like gl2d's glGetTexImage, so data read here goes back
		// through createFromBuffer the right way up. width * height * 4
		// bytes, the size in `outSize`. Waits for the GPU,
		// so for load time, not every frame. Empty, size 0, for an id wgpu2d
		// did not issue or a level the texture does not have.
		std::vector<unsigned char> readTextureData(int mipLevel = 0, glm::ivec2 *outSize = nullptr);
	};

	glm::vec4 computeTextureAtlas(int xCount, int yCount, int x, int y, bool flip = 0);
	glm::vec4 computeTextureAtlasWithPadding(int mapXsize, int mapYsize, int xCount, int yCount, int x, int y, bool flip = 0);

	struct TextureAtlas
	{
		TextureAtlas() {};
		TextureAtlas(int x, int y) :xCount(x), yCount(y) {};

		int xCount = 0;
		int yCount = 0;

		glm::vec4 get(int x, int y, bool flip = 0)
		{
			return computeTextureAtlas(xCount, yCount, x, y, flip);
		}
	};

	struct TextureAtlasPadding
	{
		TextureAtlasPadding() {};

		TextureAtlasPadding(int x, int y, int xSize, int ySize) :xCount(x), yCount(y)
			, xSize(xSize), ySize(ySize)
		{
		};

		int xCount = 0;
		int yCount = 0;
		int xSize = 0;
		int ySize = 0;

		glm::vec4 get(int x, int y, bool flip = 0)
		{
			return computeTextureAtlasWithPadding(xSize, ySize, xCount, yCount, x, y, flip);
		}
	};

	// gl2d's FrameBuffer: a texture the renderer draws into instead of the
	// screen. `texture` is an ordinary handle, so the result is drawn back
	// with renderRectangle like any sprite. In WebGPU there is no framebuffer
	// object at all -- a render pass's color attachment is just a texture
	// view -- so `fbo` is simply the texture's id, non-zero once created.
	//
	// The target's format is the surface's, because a pipeline may only draw
	// into an attachment matching the format it was built for.
	struct FrameBuffer
	{
		FrameBuffer() {};
		explicit FrameBuffer(unsigned int w, unsigned int h) { create(w, h); };

		unsigned int fbo = 0;
		Texture texture = {};

		// `pixelated` is an addition to gl2d's signature (nearest instead of
		// linear filtering when the result is drawn back), for the
		// pixel-perfect upscale case. Defaulted, so gl2d call sites compile.
		void create(unsigned int w, unsigned int h, bool pixelated = false);
		void resize(unsigned int w, unsigned int h);

		// Releases the texture. Does not touch anything drawn into it.
		void cleanup();

		// Clears to transparent black. Immediate if a frame is open,
		// otherwise applied when the target is next drawn into.
		void clear();

		// The target's texels on the CPU, and back: for keeping what a
		// target has built up (a save), not for every frame. Rows top-first,
		// 4 bytes a texel in the target's own format, unswizzled -- what
		// `read` gives, `write` puts back exactly.
		//
		// `read` waits for the GPU, and sees what was submitted before it:
		// not what this frame has drawn so far. `write` takes the target's
		// whole size, and lands in order with this frame's passes -- after
		// anything already drawn into it this frame, before anything after.
		bool read(std::vector<unsigned char> &texels);
		bool write(const std::vector<unsigned char> &texels);

		// Note on translucency: a draw of (C, a) into a transparent target
		// leaves (C*a, a) -- the target's contents are premultiplied by
		// coverage, whatever the source was. Draw the result back with
		// BlendMode::Premultiplied, which is what compositing expects; with
		// BlendMode::Alpha the coverage is applied a second time and a
		// translucent target comes out darker than the same draw made
		// directly. Opaque targets round-trip identically either way.
	};

	// How a composed layer is drawn back. Screen-space pixels, and degrees
	// about the layer's centre.
	//
	// Deliberately just these two. A tint, a scale and a per-effect shader all
	// have plausible futures, and adding a field when one arrives is a few
	// lines; a transform designed around three imagined callers is how the
	// wrong abstraction gets built.
	struct LayerTransform
	{
		glm::vec2 offsetPixels = {};
		float rotationDegrees = 0.f;

		// Exact comparison on purpose: a caller that settles its animation to
		// exactly zero at rest gets LayerEffect's identity bypass, and one
		// that decays asymptotically never does. That is a real property to
		// design to, not a float-equality bug.
		bool isIdentity() const
		{
			return offsetPixels.x == 0.f && offsetPixels.y == 0.f && rotationDegrees == 0.f;
		}
	};

	// Flush everything recorded so far through an offscreen target, then draw
	// that target back under a transform. The caller owns the transform
	// (offset, rotation, when it is identity). This is the mechanism; what
	// the layer is, and why it moves, is the caller's.
	//
	// Why the round trip is worth it: a composed layer can be moved and
	// rotated as one image. Nudging each quad separately cannot rotate a
	// group about a shared centre without shearing the layout apart.
	struct LayerEffect
	{
		// Flushes everything the renderer has recorded since its last flush
		// through the target, then records one quad drawing that target back
		// under `transform`. The caller's next flush is what puts it on screen,
		// on top of whatever was flushed before it.
		//
		// `width` and `height` are the framebuffer size -- the same values the
		// game passes to updateWindowMetrics -- and the target follows them.
		//
		// An identity transform skips the target entirely and flushes straight
		// to the screen. That is not an optimisation the caller has to
		// remember: it keeps the untransformed case pixel-exact, since a round
		// trip through a same-size target is only byte-identical for opaque
		// pixels.
		void flush(Renderer2D &renderer, int width, int height, const LayerTransform &transform);

		// Releases the target. Safe to call twice, and safe never to call.
		void cleanup();

		// The composed layer. Screen-sized, resized with the window so the
		// composite is 1:1.
		FrameBuffer target;
	};

	// ---- Text (gameplay roadmap U1) ---------------------------------------
	//
	// A bitmap font: the glyphs rasterised once, at one size, into an atlas,
	// and a table saying where each one is and how to place it. Drawing a
	// string is then one quad per character through the ordinary batch --
	// there is no text shader. The atlas is white with the glyph's coverage
	// in alpha, so the sprite shader's `color * texel` is the tint.
	//
	// Crisp at the size it was baked at and at whole multiples of it, when
	// the text sits on whole pixels; blurred or uneven anywhere else. That is
	// the trade a bitmap makes, and it is the right one for a pixel font,
	// which is only meant to be seen on its own grid.
	//
	// Printable ASCII (32..126) only, and no kerning: right for a monospace
	// pixel font. A proportional font would want stbtt's pair table.
	struct Font
	{
		// One character's placement, in pixels at the baked size, relative to
		// the pen on the baseline (y down). `uv` is gl2d's convention, ready
		// for renderRectangle.
		struct Glyph
		{
			glm::vec2 offset = {};  // pen to the glyph's top-left
			glm::vec2 size = {};
			float advance = 0.f;    // pen to the next character's pen
			glm::vec4 uv = {};
		};

		static constexpr int firstChar = 32;
		static constexpr int charCount = 95;

		Texture texture = {};
		float pixelHeight = 0.f; // the size it was baked at
		float ascent = 0.f;      // baseline to the top of the tallest glyph
		float descent = 0.f;     // baseline to the bottom of the lowest; negative
		float lineGap = 0.f;
		std::vector<Glyph> glyphs;

		// Rasterises `ttfFile` at `pixelHeight` (stb_truetype's pixel height:
		// ascender to descender). `pixelated` samples it nearest rather than
		// linear: a pixel font scaled by a whole number keeps hard edges. False
		// if the file would not load; the reason is on stderr.
		bool createFromFile(const char *ttfFile, float pixelHeight, bool pixelated = true);
		void cleanup();

		// ---- gl2d compatibility (ourCraft) ------------------------------
		//
		// gl2d's loader: bakes at 64 px into a 2000x2000 atlas with 2x2
		// oversampling, as gl2d did, and fills `glyphs` with each glyph's quad
		// (offset and size at the 64 px bake) for renderText/getTextSize.
		// Use those with a font made here, renderTextScaled with the other.
		void createFromFile(const char *ttfFile, bool monospaced = false);

		float max_height = 0.f;  // ascent - descent + line gap at 64 px
		float spaceSize = 0.f;   // a space's advance; max_height if monospaced
		bool monospaced = false;

		// Baseline to baseline, at the baked size.
		float lineHeight() const { return ascent - descent + lineGap; }
	};

	// The box `text` fills drawn at `scale` (1 is the baked size): the widest
	// line by the number of lines times the line height. '\n' starts a line.
	glm::vec2 measureText(const Font &font, const char *text, float scale = 1.f);

	// A camera is a transform: where the view sits and how far it is zoomed.
	// buildViewProj turns those into the matrix the vertex shader applies.
	//
	// It used to carry two things it should not have. `follow` chased a point
	// and drew nothing, so it is a behaviour and now lives in the game (see
	// gameLayer/cameraFollow.h, and roadmap R7 for where it goes next).
	// `rotation` was accepted and applied by nothing at all -- and `sameCamera`
	// did not compare it either, so implementing it without also fixing the
	// batch's run key would have let two cameras differing only in roll share
	// a uniform slot and silently render with the wrong matrix. A roll can be
	// added when something wants one, in buildViewProj and sameCamera
	// together, checked numerically the way milestone 5 checked this matrix.
	struct Camera
	{
		glm::vec2 position = {};
		float zoom = 1.0;

		void setDefault() { *this = Camera{}; }

		// gl2d compatibility (ourCraft): gl2d's follow, which ourCraft's map
		// screen uses. Moves toward putting `pos` at the centre of a w x h
		// view once it is more than `min` away, by `speed` (slower when
		// close), and never lets it be more than `max` away.
		void follow(glm::vec2 pos, float speed, float min, float max, float w, float h);
	};

	struct Renderer2D
	{
		Renderer2D() {};
		Renderer2D(Renderer2D &other) = delete;
		Renderer2D(Renderer2D &&other) = delete;
		Renderer2D operator=(Renderer2D other) = delete;
		Renderer2D operator=(Renderer2D &other) = delete;
		Renderer2D operator=(Renderer2D &&other) = delete;

		// The GPU objects are owned by the context; this only reserves batch
		// memory and records the default target (gl2d's defaultFBO): 0 draws
		// to the screen, a FrameBuffer's fbo draws into that texture.
		void create(unsigned int fbo = 0, size_t quadCount = 1000);
		unsigned int defaultFBO = 0;
		void cleanup();

		Camera currentCamera = {};
		std::vector<Camera> cameraPushPop;

		// Applies to every quad recorded after it, until it is set again.
		// Not a stack like the camera: nothing nests blend modes, and a
		// missing pop would be silent where a missing popCamera is obvious.
		BlendMode currentBlendMode = BlendMode::Alpha;
		void setBlendMode(BlendMode mode) { currentBlendMode = mode; }

		// The effect every quad recorded after this runs through, and the
		// parameters it reads. Set like the blend mode, for the same reason:
		// threading them through every renderRectangle overload would double
		// the signatures for something most draws never use.
		//
		// This is the per-*quad* channel a post-process did not need. A
		// full-screen effect is one draw with one parameter block; a shield
		// ripple is a quad among many, each wanting its own impact point.
		//
		// Consecutive quads sharing parameters share a slot, so setting this
		// once around a loop costs one slot rather than one per quad. Changing
		// either the effect or the parameters ends a draw run.
		Effect currentEffect = {};
		EffectParams currentEffectParams = {};
		void setEffect(Effect effect, const EffectParams &params = {})
		{
			currentEffect = effect;
			currentEffectParams = params;
		}
		void clearEffect() { currentEffect = {}; currentEffectParams = {}; }
		void pushCamera(Camera c = {});
		void popCamera();

		// The camera's visible world rectangle. Doesn't take rotation into account.
		glm::vec4 getViewRect();

		// Window metrics, should be up to date at all times. The game passes
		// the framebuffer size, which is what the camera matrix uses.
		int windowW = -1;
		int windowH = -1;
		void updateWindowMetrics(int w, int h) { windowW = w; windowH = h; }

		void clearDrawData();

		void renderRectangle(const Rect transforms, const Texture texture, const Color4f colors[4], const glm::vec2 origin = {}, const float rotationDegrees = 0.f, const glm::vec4 textureCoords = WGPU2D_DefaultTextureCoords);
		inline void renderRectangle(const Rect transforms, const Texture texture, const Color4f colors = {1,1,1,1}, const glm::vec2 origin = {}, const float rotationDegrees = 0, const glm::vec4 textureCoords = WGPU2D_DefaultTextureCoords)
		{
			Color4f c[4] = { colors,colors,colors,colors };
			renderRectangle(transforms, texture, c, origin, rotationDegrees, textureCoords);
		}

		void renderRectangleAbsRotation(const Rect transforms, const Texture texture, const Color4f colors[4], const glm::vec2 origin = {}, const float rotationDegrees = 0.f, const glm::vec4 textureCoords = WGPU2D_DefaultTextureCoords);
		inline void renderRectangleAbsRotation(const Rect transforms, const Texture texture, const Color4f colors = {1,1,1,1}, const glm::vec2 origin = {}, const float rotationDegrees = 0.f, const glm::vec4 textureCoords = WGPU2D_DefaultTextureCoords)
		{
			Color4f c[4] = { colors,colors,colors,colors };
			renderRectangleAbsRotation(transforms, texture, c, origin, rotationDegrees, textureCoords);
		}

		void renderRectangle(const Rect transforms, const Color4f colors[4], const glm::vec2 origin = { 0,0 }, const float rotationDegrees = 0);
		inline void renderRectangle(const Rect transforms, const Color4f colors, const glm::vec2 origin = { 0,0 }, const float rotationDegrees = 0)
		{
			Color4f c[4] = { colors,colors,colors,colors };
			renderRectangle(transforms, c, origin, rotationDegrees);
		}

		void renderLine(const glm::vec2 position, const float angleDegrees, const float length, const Color4f color, const float width = 2.f);
		void renderLine(const glm::vec2 start, const glm::vec2 end, const Color4f color, const float width = 2.f);
		void renderCircleOutline(const glm::vec2 position, const Color4f color, const float size, const float width = 2.f, const unsigned int segments = 16);

		// Text, in the current camera's world. `anchor` says which point of
		// the text's box (measureText) sits at `position`: {0, 0} its
		// top-left, {0.5, 0.5} its centre, {1, 0} its top-right. Lines are
		// aligned against each other by the same fraction, so a centred block
		// centres every line.
		//
		// Each line's start is rounded to whole units after anchoring, so text
		// placed on the pixel grid stays there whatever its width. With a
		// whole `scale` that keeps a pixel font crisp in screen space.
		//
		// (ourCraft: renamed from renderText, which now has gl2d's signature
		// below. Both would otherwise accept renderText(pos, text, font, color).)
		void renderTextScaled(glm::vec2 position, const char *text, const Font &font,
			const Color4f color = {1, 1, 1, 1}, float scale = 1.f, glm::vec2 anchor = {0, 0});

		// ---- gl2d compatibility (ourCraft) ------------------------------
		//
		// gl2d's text and 9-patch API, for game code and glui written against
		// gl2d. The text calls need a font made by gl2d's createFromFile
		// (monospaced flag), not the bakePixelHeight one: gl2d bakes at 64 px
		// and lays text out from glyph quads, and these reproduce that
		// layout. `sizePixels` scales from 64; spacing and line space are in
		// baked pixels.
		glm::vec2 getTextSize(const char *text, const Font &font, const float sizePixels = 64.f,
			const float spacing = 4, const float line_space = 3);

		// `position` is the left end of the baseline, or the centre of the
		// text box when showInCenter. A shadow and a highlight are extra
		// passes of the same glyphs, offset; a zero alpha skips each.
		void renderText(glm::vec2 position, const char *text, const Font &font, const Color4f color,
			const float sizePixels = 64.f, const float spacing = 4, const float line_spacePixels = 3,
			bool showInCenter = 1, const Color4f ShadowColor = {0.1, 0.1, 0.1, 1},
			const Color4f LightColor = {});

		// gl2d's argument order (size before color).
		void renderCircleOutline(const glm::vec2 position, const float size, const Color4f color,
			const float width = 2.f, const unsigned int segments = 16)
		{
			renderCircleOutline(position, color, size, width, segments);
		}

		// Fixed-width borders. Coordinates are gl2d's convention.
		void render9Patch(const Rect position, const int borderSize, const Color4f color,
			const glm::vec2 origin, const float rotationDegrees, const Texture texture,
			const glm::vec4 textureCoords, const glm::vec4 inner_texture_coords);

		// Borders scaled from the texture's proportions to the rectangle's.
		void render9Patch2(const Rect position, const Color4f color, const glm::vec2 origin,
			const float rotationDegrees, const Texture texture, const glm::vec4 textureCoords,
			const glm::vec4 inner_texture_coords);

		// Triangles rather than rectangles: any flat mesh, such as a polygon
		// fanned from its centre (asteroids, A1). `vertexCount` vertices make
		// vertexCount / 3 triangles; a remainder is ignored. Winding does not
		// matter -- nothing is culled.
		//
		// Positions are in the current camera's world, y down. **Texture
		// coordinates here are WebGPU's, not gl2d's:** (0, 0) is the image's
		// top-left and v runs down, the same way up as the world, so a planar
		// mapping is one division. Outside 0..1 they clamp, or wrap for a
		// texture loaded with `repeat`. Colours multiply the texture per
		// vertex and are blended across each triangle, like the corners of a
		// rectangle. The current blend mode and effect apply.
		void renderTriangles(const glm::vec2 *positions, const glm::vec2 *uvs, const Color4f *colors,
			size_t vertexCount, const Texture texture);

		// Records the color the frame's render pass clears to. The clear
		// itself happens when the pass begins (first flush, or end of frame).
		void clearScreen(const Color4f color = Color4f{0,0,0,0});

		void setCamera(const Camera camera) { currentCamera = camera; }

		// Uploads the accumulated quads and draws them into the current frame,
		// into defaultFBO.
		void flush(bool clearDrawData = true);

		// Same, but into the given render target instead. The camera
		// projection uses the target's own size.
		void flushFBO(FrameBuffer frameBuffer, bool clearDrawData = true);

		// Draws `source` across the whole screen through `effect`, with
		// `params` in the effect's uniform block. This is the post-process
		// entry point.
		//
		// It is a single draw and does not go through the batch: the batch's
		// run key knows about texture, camera and blend, not shaders, and
		// teaching it would be F6's problem rather than this one's. Anything
		// pending is flushed first, so the effect lands on top of it.
		//
		// Drawn with BlendMode::Premultiplied, because a render target's
		// contents are already scaled by their own coverage (outline 12).
		void drawFullscreenEffect(Texture source, Effect effect, const EffectParams &params);
	};

	// ---- Callouts (hints roadmap H1) --------------------------------------
	//
	// A speech bubble: text in a framed box, and a pointed tail from the box
	// to a target. Screen space, drawn with rectangles, triangles and
	// renderText in whatever camera is current -- the caller pushes a screen
	// camera and turns a world point into screen pixels first, as it would
	// for any HUD marker.
	//
	// The text is markup. `[4]` is a key-cap: the text in a small framed box.
	// `[!4]` is the same cap lit, for a key being held. `[mouse:left]`,
	// `[mouse:right]` and `[mouse:wheel]` draw a mouse with that part marked
	// instead of a word (`[!mouse:left]` lit), and words after it go in the
	// same cap: `[mouse:wheel WHEEL]`. '\n' starts a line. Each line is
	// centred in the box. A '[' with no ']' is drawn as it is.

	struct CalloutStyle
	{
		const Font *font = nullptr;
		float scale = 1.f;          // the font's; whole numbers for a pixel font

		// The fills are drawn over the frame colour, inset by one font pixel,
		// so they should be opaque: a translucent fill shows the frame through.
		Color4f text = {1, 1, 1, 1};
		Color4f frame = {1, 1, 1, 1};      // the box's edge and the tail's
		Color4f fill = {0, 0, 0, 1};       // inside the box and the tail
		Color4f capFrame = {1, 1, 1, 1};
		Color4f capFill = {0.15f, 0.15f, 0.15f, 1};
		Color4f capText = {1, 1, 1, 1};
		Color4f capLit = {1, 1, 1, 1};     // a lit cap's fill; its text is `fill`'s colour

		// The corner's text scale (drawCallout's `corner`); 0: the same as
		// `scale`. A whole number for a pixel font.
		float cornerScale = 0.f;

		// In font pixels, so all of it scales with the text.
		float padding = 4.f;        // text to the box's edge
		float tailWidth = 10.f;     // where the tail leaves the box
		float margin = 6.f;         // the box and the tip from the screen's edge
	};

	// The box `markup` needs at `style`, padding included.
	glm::vec2 measureCallout(const char *markup, const CalloutStyle &style);

	// Draws the bubble in the direction of `offset` from `target`, with the
	// length of `offset` between the target and the box's nearest edge, kept
	// on screen, and its tail ending on `target`. A target off screen is held
	// to the screen's edge, the tail pointing out toward it, and the box comes
	// in from there. A target under the box (pushed there by the screen's
	// edge) gets no tail. Returns the box drawn: x, y, width, height.
	//
	// `capRects`, if given, receives where each key-cap was drawn, in the
	// order they appear in the markup, then the left corner's, then the
	// right's: what a click on a cap is tested against, which makes a cap a
	// button.
	//
	// `cornerLeft` and `cornerRight`, if given, are more markup at
	// `cornerScale`, on a row of their own under the text, in the box's
	// bottom-left and bottom-right corners -- small print, such as buttons to
	// skip a bubble or switch them off. The box widens to keep them apart.
	glm::vec4 drawCallout(Renderer2D &renderer, glm::vec2 target, glm::vec2 offset,
		const char *markup, const CalloutStyle &style, std::vector<glm::vec4> *capRects = nullptr,
		const char *cornerLeft = nullptr, const char *cornerRight = nullptr);

	// The size of the markup alone, without the box's padding.
	glm::vec2 measureMarkup(const char *markup, const CalloutStyle &style);

	// The markup alone, without a box, anchored as renderText anchors: for a
	// key-cap next to a HUD element.
	void renderMarkup(Renderer2D &renderer, glm::vec2 position, const char *markup,
		const CalloutStyle &style, glm::vec2 anchor = {0, 0});

	// ---- A grid of squares on screen (inventory roadmap I2) ---------------
	//
	// Where each square of a grid laid on screen is, which square a point is
	// in, and the squares drawn. What is in them is the caller's: an
	// inventory, a board, a level-select map. Pixels in the current camera.
	struct GridView
	{
		glm::vec2 topLeft = {};
		float cell = 32.f;        // a square's side
		float gap = 2.f;          // between squares
		int columns = 1;
		int rows = 1;

		glm::vec4 bounds() const;               // the whole grid: x, y, width, height
		glm::vec4 square(glm::ivec2 c) const;   // one square
		// The square `p` is over; false outside the grid or in a gap's middle.
		bool squareAt(glm::vec2 p, glm::ivec2 &out) const;
		// The square `p` is nearest, clamped into the grid: for dropping a
		// piece by its middle, which may be over a gap.
		glm::ivec2 nearestSquare(glm::vec2 p) const;

		// Every square, `fill` with a `line` round it.
		void draw(Renderer2D &renderer, Color4f fill, Color4f line, float lineWidth) const;
		// Squares `cells` (offsets) from `at`, filled, the gaps between
		// neighbours filled too, so a piece reads as one shape. Squares
		// outside the grid are drawn where they would be.
		void fillCells(Renderer2D &renderer, const glm::ivec2 *cells, size_t count, glm::ivec2 at,
			Color4f colour) const;
	};

	// ---- Menus (gameplay roadmap U2) --------------------------------------
	//
	// An immediate-mode menu, the way ImGui is one: each frame the caller
	// says what the rows are, in order, and each row says whether it was
	// chosen. What survives between frames is only which row is selected,
	// the row count (for wrapping), and a slider being dragged.
	//
	// It reads actions, not keys: the application maps its keyboard, mouse
	// or pad onto MenuInput, so nothing here knows what a key code is. It
	// draws with renderText and plain rectangles in whatever camera is
	// current, and the pointer is in that camera's coordinates.

	struct MenuInput
	{
		bool up = false, down = false;       // move the selection, wrapping
		bool left = false, right = false;    // change a choice or slider
		bool confirm = false;                // press the selected row
		bool back = false;                   // the caller decides what this means

		// The pointer, when it is the menu's to read (not over a debug
		// panel, say). Hovering selects; pressing presses.
		bool pointerActive = false;
		glm::vec2 pointer = {};
		bool pointerMoved = false;
		bool pointerPressed = false;  // went down this frame
		bool pointerHeld = false;
	};

	struct MenuStyle
	{
		const Font *font = nullptr;
		float scale = 1.f;        // the font's; a whole number for a pixel font
		float titleScale = 2.f;
		float width = 400.f;      // of every row, centred on the menu's x
		float rowHeight = 1.8f;   // in lines of the font at `scale`
		float padding = 0.5f;     // a row's text from its edges, in lines

		Color4f title = {1, 1, 1, 1};
		Color4f text = {0.8f, 0.8f, 0.8f, 1};
		Color4f selected = {1, 1, 1, 1};
		Color4f highlight = {1, 1, 1, 0.12f};  // the bar behind the selected row
		Color4f panel = {0, 0, 0, 0};          // behind the whole menu; 0 alpha: none
		Color4f shadow = {0, 0, 0, 0};         // one font pixel down and right; 0 alpha: none
		Color4f track = {1, 1, 1, 0.2f};       // a slider's empty part
		Color4f fill = {1, 1, 1, 0.9f};        // and its filled part
		Color4f inactive = {1, 1, 1, 0.3f};    // a row that cannot be chosen
	};

	struct Menu
	{
		int selected = 0;

		// Starts this frame's rows, the first one's top edge at `topCentre`.
		// Applies up/down to the selection, wrapping by last frame's count.
		void begin(Renderer2D &renderer, const MenuInput &input, glm::vec2 topCentre, const MenuStyle &style);

		// A heading: not selectable, `titleScale`, centred.
		void title(const char *text);

		// A gap of `lines` lines, not selectable.
		void space(float lines = 1.f);

		// True on the frame it is pressed: confirm while selected, or a press
		// on it.
		bool button(const char *label);

		// A button that cannot be chosen now: drawn where a button would be,
		// in the `inactive` colour, and skipped by the selection and the
		// pointer. `detail`, if given, says why, at the row's right end --
		// "NOT YET", "NO SAVE".
		void inactive(const char *label, const char *detail = nullptr);

		// The label on the left, the current option on the right. Left and
		// right step through the options, a press steps forward; wraps. True
		// on the frame `index` changes.
		bool choice(const char *label, int &index, const char *const *options, int count);
		bool toggle(const char *label, bool &value); // a choice of OFF / ON

		// The label on the left, a bar on the right. Left and right step by
		// `step`; the pointer sets it by dragging along the bar. True on the
		// frame `value` changes.
		bool slider(const char *label, float &value, float min, float max, float step);

		// The label on the left, then `count` cells in equal columns on the
		// right, each one selectable: left and right move between them, and
		// the pointer selects the one it is over. The column is kept as the
		// selection moves up and down, so a page of these reads as a table.
		// Returns the cell pressed this frame, or -1. `colours`, if given,
		// overrides each cell's text colour; a colour with 0 alpha keeps the
		// usual one.
		int fields(const char *label, const char *const *values, int count, const Color4f *colours = nullptr);

		// A line of text, centred, not selectable: a message under a page.
		void note(const char *text, Color4f colour);

		// `back` was pressed this frame.
		bool backPressed() const { return input.back; }

		// Ends the frame: the row count and height for the next one.
		void end();

		// ---- Between begin and end ---------------------------------------
		Renderer2D *renderer = nullptr;
		MenuInput input = {};
		MenuStyle style = {};
		glm::vec2 topCentre = {};
		float y = 0.f;            // the next row's top
		int row = 0;              // the next selectable row's index
		int rowCount = 0;         // selectable rows last frame
		float lastHeight = 0.f;   // the whole menu's, last frame, for the panel
		int dragging = -1;        // the slider the pointer is dragging
		int field = 0;            // the selected cell in a fields row
	};
}
