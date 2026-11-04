# WebGPU learning outline

A table of contents for the render port. Each section is one concept cluster: LearnWebGPU chapter, where it landed in this repo, and the commit that introduced it. Expand in place later.

The game-facing API is `wgpu2d` (`include/render/wgpu2d.h`). Almost all GPU work lives in `src/render/wgpuContext.cpp`, with the ImGui half in `src/render/wgpuImgui.cpp`. The GLFW loop in `src/platform/glfwMain.cpp` only owns init / begin / end / ImGui.

**The rule the port followed:** mirror gl2d's *signatures* name for name, never its implementation. gl2d transforms every corner on the CPU and hands the shader NDC; here the vertex buffer keeps world pixels and the camera is a matrix on the GPU. That is why the game files changed by an include and a namespace only.

*That was a port tactic and it finished its job at 7, once the game was running on `wgpu2d`. It was never a ceiling on what a 2D drawing library may grow: `BlendMode` in 12 was the first addition past it, `LayerEffect` the second. New drawing capabilities go in `wgpu2d.h` rather than beside it, because one public header is a boundary the build can enforce and a second game-facing render header would need an exception — which is how `hudShake.cpp` came to be a general mechanism in the library with one game's numbers in it.*

Guide: [Learn WebGPU for C++](https://eliemichel.github.io/LearnWebGPU/index.html). Use the **With webgpu.hpp** tab. Our wrapper is compiled in one TU: `src/render/webgpuImpl.cpp`.

The guide is 3D; this project is a 2D sprite batch. Same objects, different use.

---

## How to read this

Each block is: **concepts → LearnWebGPU chapters → where it landed → commit**.

---

## 0. Stack and window (before drawing)

**Concepts:** CMake + FetchContent, wgpu-native vs Dawn, GLFW with `GLFW_NO_API`, glfw3webgpu surface, C++ wrapper vs `webgpu.h`.

**LearnWebGPU:** [Project setup](https://eliemichel.github.io/LearnWebGPU/getting-started/project-setup.html) · [Hello WebGPU](https://eliemichel.github.io/LearnWebGPU/getting-started/hello-webgpu.html) · [Opening a window](https://eliemichel.github.io/LearnWebGPU/getting-started/opening-a-window.html) · [C++ idioms](https://eliemichel.github.io/LearnWebGPU/getting-started/cpp-idioms.html) (the wrapper, `Default`, `StringView`)

**Code:** `CMakeLists.txt` (FetchContent of WebGPU-distribution + glfw3webgpu) · `src/platform/glfwMain.cpp` window hints · `src/render/webgpuImpl.cpp`

**Pins:** WebGPU-distribution `v0.3.0-gamma`, which fetches prebuilt **wgpu-native v24.0.3.1** · glfw3webgpu `v1.3.0-alpha` · GLFW 3.4 (`thirdparty/glfw-3.4`; the unused 3.3.2 tree is still in the repo). The two tags were tested together on the GLFW 3.4 line.

**Read the header, not your memory.** The C API churns between wgpu-native releases (string views instead of `const char*`, surface configuration instead of swapchains, `ShaderSourceWGSL` instead of the old chained descriptors). The vendored copies:

- `build/_deps/wgpu-macos-x86_64-release-src/include/webgpu/webgpu.h` — the spec header
- `.../include/webgpu/wgpu.h` — wgpu-native's own extensions
- `build/_deps/webgpu-distribution-src/wgpu-native/include/webgpu/webgpu.hpp` — the C++ wrapper

**Build directory:** `build/`

**Commits:** `43b3255` GLFW 3.4 · `5861a4d` 1a · `21d6524` wrapper

---

## 1a. Instance, surface, adapter

**Concepts:** Instance, surface (window → GPU), adapter (which GPU), async `requestAdapter` + process events, adapter limits/properties.

**LearnWebGPU:** [The Adapter](https://eliemichel.github.io/LearnWebGPU/getting-started/adapter-and-device/the-adapter.html)

**Code:** `render::wgpuInit` · `onAdapterRequest` · `printAdapter` · `glfwCreateWindowWGPUSurface`

**Commit:** `5861a4d`

---

## 1b. Device, queue, surface config, clear, present

**Concepts:** Device + lost/error callbacks, queue, **surface configuration** (the guide’s old swapchain), command encoder, render pass clear, submit, present. Framebuffer size vs window size (Retina).

**LearnWebGPU:** [The Device](https://eliemichel.github.io/LearnWebGPU/getting-started/adapter-and-device/the-device.html) · [The Command Queue](https://eliemichel.github.io/LearnWebGPU/getting-started/the-command-queue.html) · [First Color](https://eliemichel.github.io/LearnWebGPU/getting-started/first-color.html)

**Code:** `onDeviceRequest` / `onUncapturedError` / `onDeviceLost` · `configureSurface` · `wgpuBeginFrame` / `wgpuEndFrame` · `Context` fields tagged `1a` / `1b`

**Commit:** `ad94126`

---

## 2. Shader module + render pipeline (one triangle)

**Concepts:** WGSL `@vertex` / `@fragment`, shader module from source, **render pipeline** as immutable GPU state (topology, blend, color target format). Hardcoded verts in the shader at this step; later moved to a buffer.

**LearnWebGPU:** [Hello Triangle](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/hello-triangle.html)

**Code:** `createShaderModuleFromFile` · `createQuadPipeline` · `resources/shaders/quad.wgsl`

**Commit:** `8fee100`

---

## 3. Vertex buffers and attributes (colored quad)

**Concepts:** GPU buffer, `queue.writeBuffer`, vertex layout (stride, `@location`, formats), interleaved vs separate attributes. Two triangles = one quad, six vertices, no index buffer (ImGui later adds indices).

**LearnWebGPU:** [Playing with buffers](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/playing-with-buffers.html) · [A first Vertex Attribute](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/a-first-vertex-attribute.html) · [Multiple Attributes](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/multiple-attributes.html)

**Code:** `struct Vertex` · vertex layout inside `createQuadPipeline` · `ensureVertexBufferCapacity` · `pushQuad`

**Commit:** `3872e7b`

---

## 4. Textures, samplers, bind groups (textured quad)

**Concepts:** Texture vs buffer, `writeTexture`, texture view, sampler (filter / address / mips), **bind group layout** = shader contract (`@group` / `@binding`), bind group = the actual resources. Y/UV origin: WebGPU top-left; gl2d was bottom-left. Converted at the API boundary, no flip on load.

**LearnWebGPU:** [A first texture](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/a-first-texture.html) · [Texture mapping](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/texture-mapping.html) · [Sampler](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/sampler.html) · [Loading from file](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/loading-from-file.html)

**Code:** `createSamplers` · `createLayouts` (group 0) · `createTextureFromPixels` · `Texture::loadFromFile` · `quad.wgsl` group 0

**Commit:** `5f83600`

---

## 5. Uniforms, projection, resize (pixel-space camera)

**Concepts:** Uniform buffer, bind group 1, orthographic matrix in the vertex shader (world pixels, y-down → clip space). Resize = reconfigure surface from **framebuffer** size, and on Metal the surface never reports itself `Outdated`, so the size is compared every frame in `wgpuBeginFrame` instead of waiting to be told (symptom when missing: sprites stretch and the visible world does not change). Color space: pick a non-sRGB surface format; pin the Metal layer.

**LearnWebGPU:** [A first uniform](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/shader-uniforms/a-first-uniform.html) · [Multiple uniforms](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/shader-uniforms/multiple-uniforms.html) · [Transformation matrices](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/transformation-matrices.html) · [Projection matrices](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/projection-matrices.html) · [Resizing the window](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/some-interaction/resizing-window.html) · [Camera control](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/some-interaction/camera-control.html)

**Code:** `buildViewProj` · `CameraUniforms` · `configureSurface` called from `wgpuBeginFrame` · `include/render/wgpuMetalLayer.h`

**Commits:** `5508972` camera · `095afaf` Metal sRGB pin

---

## 6a. CPU batch, one texture, blend

**Concepts:** Growable vertex buffer, accumulate quads on the CPU, one upload + one draw per flush. Blend state matching gl2d (`SrcAlpha` / `OneMinusSrcAlpha`, alpha `One` / `OneMinusSrcAlpha`). Origin, rotation, per-quad color still CPU-side.

**LearnWebGPU:** No dedicated chapter — Hello Triangle + buffers + pipeline blend. Closest “many draws” idea is later instancing (TODO in the guide).

**Code:** `batchVertices` · `pushQuad` · `flushBatch` · blend block in `createQuadPipeline` · `Renderer2D` in `include/render/wgpu2d.h`

**Commit:** `086bf22`

---

## 6b. Texture runs + camera stack (dynamic uniforms)

**Concepts:** Break the batch into **runs** (same texture + same camera). `setBindGroup` per run. **Dynamic uniform offsets** so several cameras share one buffer (`minUniformBufferOffsetAlignment`, slot stride). `pushCamera` / `popCamera`.

**LearnWebGPU:** [Dynamic uniforms](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/shader-uniforms/dynamic-uniforms.html)

**Code:** `ensureCameraSlotCapacity` · `batchQuadTextures` / `batchQuadCameras` / `frameCameras` · run loop in `flushBatch` · `Renderer2D::pushCamera` / `popCamera` / `getViewRect`

**Commit:** `f013edc`

---

## 7. Atlas, padded loader, mipmaps, game wiring

**Concepts:** Atlas UV math (CPU only). Pixel padding so filtering does not bleed cells. **CPU mipmaps** (WebGPU has no `glGenerateMipmap`; the guide’s GPU version is compute). Game files switched onto `wgpu2d` under gl2d's signatures. *(This is where the signature freeze stopped earning its keep — the switch is what it existed for. See the note at the top.)*

**LearnWebGPU:** [Loading from file](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/loading-from-file.html) (stb_image) · [Mipmap Generation](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/mipmap-generation.html) (CPU half of that chapter, not the compute pass)

**Code:** `downsampleRGBA8` · `createPaddedTextureFromFileData` · `computeTextureAtlas*` · `Texture::loadFromFileWithPixelPadding` · `include/render/wgpu2d.h` · game: `include/gameLayer/{tiledRenderer,bullet,enemy}.h` and `src/gameLayer/{gameLayer,tiledRenderer,bullet,enemy}.cpp`

**Commit:** `b01187f`

**Still open in the plan:** 7-parity screenshot comparison.

---

## 8. ImGui on WebGPU

**Concepts:** Platform backend (GLFW) vs renderer backend (us). Same render pass, after the game. Index buffer (`drawIndexed`, with `ImDrawCmd::VtxOffset` as `baseVertex` so 16-bit indices survive past 64k vertices), scissor rectangles, packed `Unorm8x4` color. Reuse group-0 texture layout; own group-1 ortho, static, no dynamic offset.

Details worth remembering: ImGui positions are **screen points**, scissors are **framebuffer pixels**, so clip rectangles are the only thing multiplied by `FramebufferScale` (2× on the Retina panel) and clamped to the surface. `writeBuffer` copies whole 4-byte words, so an odd 16-bit index count is padded. `ImTextureID` is just a `wgpu2d::Texture` id, which is why the font atlas goes through `createFromBuffer` and its bind group comes from the texture registry. Colors are written unconverted to the non-sRGB surface, exactly as `imgui_impl_opengl3` writes them.

Docking is core ImGui and renderer-independent, so it is on (`DockSpaceOverViewport` with `PassthruCentralNode`, which keeps the host window and the empty central node from drawing over the game); `WindowBg` alpha is zeroed so the debug window's body is transparent. **Multi-viewport stays off** — every torn-off window would need its own WebGPU surface.

Bundled `imgui_impl_wgpu` (ImGui 1.89.5) is too old for wgpu-native v24: SPIR-V shader modules, `WGPUProgrammableStageDescriptor`, `const char*` labels. Upgrading ImGui would have churned the OpenGL path too, so the backend is hand written.

**LearnWebGPU:** [Simple GUI](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/some-interaction/simple-gui.html) (their `imgui_impl_wgpu`; this port reimplements the same job) · [Index Buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/index-buffer.html)

**Code:** `include/render/wgpuImgui.h` · `src/render/wgpuImgui.cpp` · `include/render/wgpuFrame.h` (shared device/pass/texture groups) · `resources/shaders/imgui.wgsl` · `src/platform/glfwMain.cpp` around `wgpuImgui*`

**Commit:** `fa0e022`

---

## 10. Render targets (and a low-res render scale)

**Concepts:** There is no framebuffer object in WebGPU: a render pass's colour attachment *is* a texture view, so drawing to the screen and drawing to a texture are one operation with a different attachment. `wgpu2d::FrameBuffer` mirrors gl2d's shape over that, and its `texture` is an ordinary handle, so the result is drawn back with `renderRectangle` and needs no new pipeline or shader.

Three constraints that only show up here:

- **A pipeline is bound to its target's format.** `createQuadPipeline` bakes `colorTarget.format`, so a render target must use the surface's format for the sprite pipeline to draw into it. Wrong format is a validation error, not a wrong picture. *(Superseded in 12: the pipeline cache is keyed on the format, so a target may now use any format. 11 also corrected the second half — the mismatch is **not** reported at creation; it is reported when the pipeline meets the attachment inside a pass.)*
- **A pass belongs to one attachment and passes cannot nest.** `ensurePassBegun(target)` ends the open pass when the target changes and chooses the load operation: the surface and the low-res stand-in clear on their first pass of a frame, a user's FrameBuffer loads its contents like gl2d's. `wgpuCurrentRenderPass` therefore means *the surface* specifically, and requesting it while the frame sits in the low-res target composites that first, so the UI lands on top of the world.
- **More than one flush per frame means more than one buffer slice.** `writeBuffer` is ordered against the submit, not against the recorded draws, so two flushes writing at offset 0 leave the first pass's draws reading the second flush's data. Each flush appends its own slice of the vertex buffer and its own camera slots, both reset in `wgpuBeginFrame`. The symptom of getting this wrong was the whole atlas stretched across the target with the scene one quad out of step.

Also worth knowing: colours are straight, not premultiplied, alpha. An opaque scene round-trips through a same-size target byte-identically; a translucent draw that goes through a target and is then composited is multiplied by its coverage twice and comes out darker. *(Fixed in 12. The diagnosis here was half right: the source is straight alpha, but what a target **holds** is premultiplied, and the double multiply was in the composite, not the draw.)*

`WGPU_RENDER_SCALE=0.25` uses all of it for something real: the world is drawn into a quarter-size target and upscaled with nearest filtering, while the projection keeps using the surface's dimensions so framing, HUD layout and mouse mapping are unchanged. ImGui still draws to the surface at native size.

**LearnWebGPU:** no dedicated chapter — it is the pass and attachment machinery from [First Color](https://eliemichel.github.io/LearnWebGPU/getting-started/first-color.html) and [Hello Triangle](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/hello-triangle.html) pointed at a texture from [A first texture](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/a-first-texture.html).

**Code:** `createRenderTarget` · `ensureScaledTarget` · `ensurePassBegun(target)` · `compositeScaledTarget` · `flushBatch(target)` · `FrameBuffer` and `Renderer2D::flushFBO` in `include/render/wgpu2d.h`

**Commit:** `b15b5bb`

---

## 11. Error scopes and debug groups

**Concepts:** The device's uncaptured-error callback is a **catch-all** — it reports errors no scope claimed, cannot say which call produced them, and can arrive after the null handle it explains. An **error scope** is a stack on the device: `pushErrorScope(filter)` claims errors of one class, `popErrorScope` reports the first one captured. What it buys is not the text but **attribution**. **Debug groups** are the same idea one level down: a label names an object, a group names a *span of recorded commands*, which is what turns a GPU capture from a flat list of draws into a tree.

Three things this turned up that are worth more than the feature itself:

- **A rejected pipeline is not null.** `createRenderPipeline` returns a live handle in an *invalid* state, so `if (!pipeline)` passes and the failure only appears later as a per-frame cascade — `setPipeline` reports it, then `draw` reports "Render pipeline must be set". Only a scope can tell the difference at the point of creation.
- **Popping a scope is asynchronous.** The callback fires inside `instance.processEvents()`, the same mechanism `requestAdapter` and `requestDevice` use, so draining it costs a stall. That single fact decides the whole design: scopes wrap object **creation** and never per-frame recording. The two that sit on per-frame functions are past the early return that makes them growth steps.
- **A debug group must balance before its pass ends**, and must not span a target switch — `ensurePassBegun` ends the open pass whenever the target changes. `wgpuImguiRenderDrawData` also returns early between where a group would be pushed and popped. Both are why these are RAII guards rather than call pairs.

**What was already there, contrary to the plan:** this milestone was written believing a null handle was the whole diagnosis. It was not. wgpu-native's logger (`wgpuSetLogCallback` + `wgpuSetLogLevel`) and the uncaptured-error callback were both wired from the start, and labels were essentially complete — 27 of them across the two render TUs. Validation *text* already reached stderr. Only the attribution was missing. The lesson was cheap and worth having: check the code before writing the plan.

**LearnWebGPU:** [Debugging](https://eliemichel.github.io/LearnWebGPU/appendices/debugging.html) — still WIP; error scopes are covered, the capture-tool half is thin.

**Code:** `ErrorScope` and `DebugGroup` in `include/render/wgpuFrame.h` (header, so both render TUs share them) · `wgpuBeginErrorScope` / `wgpuEndErrorScope` / `onPopErrorScope` in `src/render/wgpuContext.cpp` · scopes on the creation sites in both TUs · groups in `flushBatch` and `wgpuImguiRenderDrawData` · `WGPU_LOG_LEVEL`

**Commit:** `d779a1c`

---

## 12. A pipeline cache, blend modes, and a correct composite

*(14 is the companion: why the API forces this, and what it means for batching. Read that one for the concept; this one is what the code did about it.)*

**Concepts:** Blending and the colour target's format are **baked into a render pipeline** and cannot be set on a pass, so "the pipeline" was always really one per (format, blend) pair. WebGPU has no pipeline-cache object — Vulkan does, `webgpu.h` does not — so this is a small vector of entries built on demand and scanned linearly at run boundaries. `flushBatch`'s run key gains a third component beside texture and camera: texture and camera change which *resources* are bound, blend changes which *pipeline* is bound.

**The key is deliberately two fields.** Sample count, depth state and the vertex layout all belong in it eventually; adding a field then is a few lines, and designing around four futures at once is how the wrong key gets built.

Both halves have a real consumer:

- **Blend.** `wgpu2d::BlendMode { Alpha, Additive, Premultiplied }` — an addition to gl2d's shape, not a mirror of it, since gl2d had one blend state for the whole program.
- **Format.** Milestone 10 forced a render target to use the surface's format because there was one pipeline built for it. Keying on the format lifts that, and makes the rule structural instead of a comment.

Two consequences of 11 are built in: the shader module is compiled once and shared (with one pipeline it could be released immediately; with N it cannot), and the cache **validates each variant with an error scope before storing it**, because a rejected pipeline is non-null and a cached invalid one would be re-served every frame.

**The composite was wrong, and not in the way milestone 10 described.** Drawing `(C, a)` into a cleared target leaves `rgb = C·a`, `a = a` — the alpha channel is `One / OneMinusSrcAlpha` in every mode, so **what a target holds is premultiplied whatever the source was**. Compositing it back with alpha blending applies coverage a second time: `C·a²`. `BlendMode::Premultiplied` (`src + dst·(1-a)`) is what compositing wants. `hudShake` is where it showed — its target is cleared transparent, so the HUD darkened for the length of every shake, and only during a shake. `compositeScaledTarget` is numerically identical today because the world target is cleared opaque and `a = 1` makes both modes agree; it is now correct rather than accidentally correct.

Measured rather than argued, with temporary `copyTextureToBuffer` + `mapAsync` scaffolding — 50% grey at alpha 0.5 over opaque black, centre pixel of a 64×64 target:

| | value |
|---|---|
| direct | 64 |
| via target, premultiplied | 64 |
| via target, alpha (the old path) | 32 |

Predicted `0.25 → 64` and `0.125 → 32`. Exactly half, which is the double multiply.

**Still short of exact:** the source is not premultiplied, so overlapping translucent draws *inside* a target are very close rather than exactly right. Fixing that means the shader emitting `rgb·a` on every draw.

**LearnWebGPU:** no chapter — this is the pipeline immutability from [Hello Triangle](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/hello-triangle.html) taken to its conclusion once one pipeline stopped being enough.

**Code:** `PipelineKey` / `PipelineEntry` / `getQuadPipeline` / `createQuadPipelineVariant` · the run loop in `flushBatch` · `batchQuadBlends` · `BlendMode` and `setBlendMode` in `include/render/wgpu2d.h` · `compositeScaledTarget` · `src/render/hudShake.cpp`

**Commits:** `002516c` cache and run key · `54a76b2` premultiplied composite

---

## 13. Extracting the library

**Concepts:** Most of this cluster is architecture rather than WebGPU, and it lives in `AGENTS.md` and `docs/roadmap.md` rather than here. What belongs in this file is the part that is WebGPU's fault — the places where the API's shape decided the design.

**WebGPU has no current context, so bring-up cannot be one call.** gl2d is handed a live GL context because a GL context is thread-global; there is no equivalent to be handed. Creating a surface needs an instance, and only the application knows what a window is, so bring-up splits: the library makes the instance, the application makes the surface from it (`glfwCreateWindowWGPUSurface(instance, window)`), and hands it back to `wgpuInit(surface, w, h)`. The adapter request then needs the surface too — `options.compatibleSurface` — which fixes the order as instance → surface → adapter → device → configure.

**Borrowing is not owning.** The library used to hold a `GLFWwindow*`; when GLFW left, it held a `WGPUSurface` the same way. Holding something for the process lifetime is not owning it. `wgpuShutdown` now `unconfigure`s the surface and drops the pointer without releasing, and the application releases it beside `glfwDestroyWindow`. The unconfigure has to happen before the device is released. A destructor guard covers every `return false` in `wgpuInit` at once, rather than auditing each one for a leaked ownership transfer.

**A library cannot read an application's resource layout.** `RESOURCES_PATH` is the app's. The sprite shader is now compiled in — CMake reads `resources/shaders/quad.wgsl` and generates a raw-string header — while `createShaderModuleFromFile` survives for the application, which legitimately has a resource layout and uses it for the ImGui shader. Watch the newline: the WGSL must follow `R"WGSL(` with nothing between, or every compile error reports one line past the file it came from.

**The C header is enough at the seam.** `wgpuContext.h` names `WGPUInstance` and `WGPUSurface`, not the C++ wrapper's types, so the application never compiles `webgpu.hpp` — which also sidesteps the one-TU `WEBGPU_CPP_IMPLEMENTATION` rule entirely.

**What the CMake targets do and do not enforce.** `wgpu2d` and `engine` are libraries, and a library cannot see the game: `#include <hud.h>` from either fails to *compile*, because neither target carries `include/gameLayer/` on its include path. That is stronger than a link error and arrives sooner. What is **not** enforced is the sideways direction — `engine` can include `render/wgpu2d.h`, use its symbols, and link, because both headers sit under one `include/` root and the executable links both libraries. Making that a wall as well needs per-library include roots. Tested in both directions rather than assumed; the asymmetry is real and the claim "a target is a wall" is only half true.

**Where things went:** the drawing library kept sprites, cameras, targets and layer effects. The ImGui backend and the macOS colour-space pin are the application's — an ImGui backend is not a 2D drawing API, and the pin needs a window. Reusable gameplay systems that draw nothing got a fourth home in `src/engine/`.

**Also settled here:** `wgpu2d` is not frozen at gl2d's signatures. That was a port tactic so the game could switch by include and namespace, and it finished its job at 7. `BlendMode` (12) and `LayerEffect` were the first two additions past it.

**LearnWebGPU:** no chapter — the guide builds one program and never asks where the seam goes.

**Code:** `wgpuInitInstance` / `wgpuInit(surface, w, h)` / `wgpuResize` / `forgetSurface` · `include/render/wgpuContext.h` · `wgpu2d::LayerEffect` · `src/gameLayer/hud.{h,cpp}` · `src/engine/` · the `wgpu2d` and `engine` targets and the shader generator in `CMakeLists.txt`

**Commits:** `e6fec09` layer effect · `0d73c53` HUD module, and gl2d unfrozen · `cb2348c` camera as transform · `cfc5755` a NaN guard · `cfdf6c2` the library target · `7e0ec55` the engine home

---

## 14. What blending is, and why it lives in the pipeline

12 built a pipeline cache and a third run key because the code needed them. This is the part that explains *why the API forces that*, which only became concrete once something actually needed a second blend mode: the engine plume in `gameLayer/shipThruster`.

**Concepts:** A fragment shader does not write to the screen. It returns a colour, and a **fixed-function blend unit** then combines that colour with whatever is already in the target, by an equation of the form `srcFactor × src  OP  dstFactor × dst`. "Alpha" and "additive" are not different shaders or different colours — they are different values of `srcFactor`/`dstFactor`:

| | colour srcFactor | colour dstFactor | result |
|---|---|---|---|
| Alpha | `SrcAlpha` | `OneMinusSrcAlpha` | `C·a + dst·(1-a)` — the source *replaces* in proportion to coverage |
| Additive | `SrcAlpha` | `One` | `C·a + dst` — the destination survives whole, so light accumulates |
| Premultiplied | `One` | `OneMinusSrcAlpha` | `C + dst·(1-a)` — for a source already scaled by its coverage (see 12) |
| Mask *(added in S7)* | `Zero` | `SrcAlpha` | `dst·a` — the source's colour is ignored; what is there survives only as far as the source covers it |

**Why it cannot be done in the shader.** The fragment shader cannot read the pixel it is about to write — core WebGPU exposes no framebuffer fetch. Blending is the hardware's answer to that, and it is why the operation is a fixed menu of factors rather than arbitrary code.

**Why it cannot be set on a pass.** There is no `glBlendFunc` here. `BlendState` is a field of `ColorTargetState`, which lives inside `RenderPipelineDescriptor`, and a pipeline is immutable once created. The render pass encoder has `setBlendConstant`, but that only supplies the constant operand for `BlendFactor::Constant` — not the equation, not the factors.

**So a second blend mode is a second pipeline object, and that is the whole reason 12 exists.** Before it there was exactly one pipeline, which meant the renderer could only ever draw with one blend equation. A glow was not unimplemented; it was inexpressible.

**And it is why the batch splits where it does.** Quads accumulate into one buffer and are drawn as runs of consecutive quads that share everything the GPU must have bound. A run already broke on texture (a different bind group) and camera (a different dynamic offset). Blend joins them, because a pipeline cannot change inside a draw. Generalised: **a draw call breaks exactly where immutable state changes**, which means the list of things that force a break *is* the renderer's cost model.

### Three things this only taught by being used

- **A state change costs nothing when it coincides with a break you were already paying for.** Switching blend around the bullets, and again around the plume, added **zero** extra draw runs — both use their own texture, so the run was breaking at exactly that point anyway. The intuition "state changes are expensive, minimise them" is too coarse; what matters is whether a change lands on an existing boundary.
- **Blend mode changes what "too bright" means.** `Bullet::render` ramps its brightness to 1.6. Under alpha that was invisible: the head quad has alpha 1, so it replaced everything under it and the excess merely clamped on write. Under additive nothing is replaced, five overlapping quads summed to **4×**, every channel clipped, and the sprite vanished into a featureless white blob. Values above 1 are latent under one blend equation and load-bearing under another.
- **Additive is invisible on the wrong art.** Measured across this game's textures: the backgrounds and sprites are 0% partially transparent — every pixel fully opaque or fully clear. A clear pixel contributes nothing under either mode and an opaque one either replaces or adds, so re-blending existing art changed almost nothing. Additive earns its place on *soft-edged, overlapping* content over a dark ground. That is why the plume's glow is generated as a radial gradient rather than reusing a sprite: the effect needs art shaped for it.

### One pipeline per (format, blend), not per effect

The intuition that each new effect needs its own pipeline is wrong, and worth correcting explicitly. The cache key is `(target format, blend mode)` and nothing else, so every additive effect in the game shares **one** pipeline object:

| effect | blend | pipeline |
|---|---|---|
| engine plume | additive | variant #2 |
| a shield bubble | additive | variant #2 — the same object |
| additive projectiles | additive | variant #2 — the same object |
| a fade-out cloak | alpha | variant #1 — the existing one |
| ships, background, HUD | alpha | variant #1 |

What differs between those effects is which texture is bound and what vertex colours are pushed. Neither is pipeline state: one is a bind group, the other is vertex data. Ten additive effects cost one pipeline.

What *does* scale with the number of effects is **run breaks** — every alpha → additive → alpha transition in the draw order splits a run. Which argues for sorting draws by blend mode to group them, except that effects almost always bring their own texture, so the run was breaking there anyway. Measured twice now, and both times the grouping would have bought nothing.

What genuinely needs a new pipeline is a change to something else the descriptor bakes: a different target format (N9), a sample count (N6), a depth state (N7), a vertex layout (N5) — or **a different shader**. The key deliberately omits the shader because there is only one today; the first effect that needs its own WGSL is what extends the key.

**Where the plume put that to work:** four gradient quads behind the hull, sizes and intensities tapering, summing into a hot core — which no single quad can do, because a single quad has one colour per pixel and this needs an accumulation. Its colours are deliberately below 1 per channel, which is the bullets' lesson applied.

### The art has to be built for it

Three effects were added on top of this — an engine plume, a shield bubble, a bullet glow — and all three had to **generate** their textures, because none of the game's existing art suits additive at all. Measured across every texture in the project: 0% partially transparent pixels. Every one is fully opaque or fully clear, so re-blending existing sprites only changes which equation a fully opaque pixel goes through, and the visible difference is almost nothing. Additive lives on soft-edged, overlapping content over a dark ground.

So each of them is a formula: a radial disc for the plume, a fresnel sphere plus a ring for the shield, a capsule distance field for the bullet glow — with the shape in the texture's alpha and the intensity in the vertex colour, which is what lets one white texture serve every colour. **Three modules now hand-roll a gradient generator**, which is a pattern becoming visible rather than a duplication worth extracting yet.

Two things they taught that the blending theory does not:

- **Under additive, "too bright" does not look brighter — it looks thicker.** Making the shield's rim narrower barely moved it, because the rim was clipping: a wide plateau reads as solid white however narrow the gradient underneath is. Reweighting so the peak just reaches 1 took the clipped plateau from 12 px to 0 and made the falloff visible. Clipping spreads a shape outward instead of intensifying it.
- **Physical correctness and legibility are different targets, and screen size decides which you get.** The shield's fresnel exponent wants to be 4 or 5. At 4 the curve is still under 0.1 at r = 0.92, so nearly all the brightness sits in the outermost 5% of the radius — on a bubble 110 px across, about three pixels. Correct and invisible. The exponent is 2.

**LearnWebGPU:** no chapter. The guide sets a blend state once in [Hello Triangle](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/hello-triangle.html) and never needs a second one, so the consequences for batching never come up.

**Code:** `createQuadPipelineVariant`'s `switch (key.blend)` · the run boundary in `flushBatch` · `include/gameLayer/shipThruster.h` · `src/gameLayer/shipThruster.cpp`

**Commit:** `fce9991`

---

## 15. Reading a frame back

**Concepts:** A rendered frame lives in a texture the GPU owns. Getting it to the CPU is `copyTextureToBuffer` into a buffer created with `MapRead`, then `mapAsync` and `getConstMappedRange`. Three steps that cannot happen together, because the GPU is not synchronous: **record** the copy after the pass ends and before the encoder is finished — a copy cannot be recorded *inside* a render pass — then **submit**, then **map**, which is asynchronous like every other WebGPU callback and is drained with the same bounded pump the error scopes use.

**Two rules the copy imposes, and both belong to the copy rather than the caller.** `bytesPerRow` must be a multiple of 256, so the buffer's rows are padded and have to be unpadded on the way out; and the surface is `BGRA8Unorm` here, so the channels are swizzled. `wgpuTakeFrameCapture` absorbs both and hands back tightly packed RGBA, because a caller that had to know either would be a caller that leaks the GPU's constraints into an image file.

**Where the boundary went:** the library returns pixels, the application writes the file. A drawing library has no business owning an image encoder or a path — the same reason its shaders are compiled in rather than read from `RESOURCES_PATH` (13). `stb_image_write` joined the existing `stb_image` target so the encoder is a normal dependency.

**`CopySrc` is now permanent** on the surface and on every render target. That is a deliberate standing cost, and the justification is history rather than principle: this capability had been rebuilt as throwaway scaffolding **five times** — the milestone-7 parity check, milestone 12's blend numbers, and three times over the visual features — and the fifth time it was lost mid-task to a cleared scratchpad and had to be rewritten before the work could continue.

**A key binding is not enough.** `F12` serves a human; `WGPU_SCREENSHOT_FRAME=N` is what makes this usable from a script or an agent session, which is what the verification norm in `AGENTS.md` actually asks for — a fixed scene rendered to a PNG with nobody at the keyboard.

**Headless was substituted, not built.** A surfaceless context is tractable in the renderer — eight uses of `g.surface`, each with an obvious offscreen branch, no `getCurrentTexture`, no `present`. The application half is the real cost: `glfwMain` is built around a window, ImGui's GLFW backend needs one, and the game reads input through `platform::`, which is wired to GLFW callbacks. `WGPU_OFFSCREEN=1` hides the window instead — one hint — and what a surfaceless context would add over that is exactly one thing: running where there is no window system at all.

**LearnWebGPU:** [Screen capture](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/screen-capture.html) (WIP) · [Headless context](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/headless.html)

**Code:** `wgpuRequestFrameCapture` / `wgpuTakeFrameCapture` / `captureRecordCopy` / `captureResolve` · `include/render/wgpuContext.h` · the PNG write and both triggers in `src/platform/glfwMain.cpp`

**Commit:** `78bd845`

---

## 16. Measuring, when the hardware will not let you

**The plan was timestamp queries, and the plan was checked before it was built.** `printAdapter` now lists adapter features by name rather than counting them, and this machine reports 22 without the one that mattered: `TimestampQuery` is absent, and so are wgpu-native's `TimestampQueryInsideEncoders` and `TimestampQueryInsidePasses`. A feature that is not advertised cannot be requested at device creation, so `QuerySet`, `timestampWrites` and `resolveQuerySet` are all unreachable. **This is a refusal, not a difficulty**, and it is the reason the guide's Benchmarking chapter cannot be followed here.

**The general lesson is the cheap one:** check the feature list before designing around a feature. The check cost one function; designing around it and discovering this at the end would have cost the item.

**What replaced it, and what each is honestly worth:**

- **Frame stats in the debug panel.** The counts are the renderer's own and exact — quads, draw runs, flushes, cameras, pipeline variants. Most of the data already existed and none of it was surfaced: `glfwMain` computed `deltaTime` and never showed it, and the batch counted its runs but printed them once, to stdout, on the first flush.
- **`wgpuQueueOnSubmittedWorkDone`.** Wall time from submit to the queue reporting done. The first reading settled what it is: 8.56 ms against a CPU frame of 8.30 ms, which is frame pacing and not GPU work. The panel says `submit->done` rather than "gpu" for that reason. **A number that is mislabelled is worse than no number.**
- **A Metal frame capture.** The one that gives genuine per-pass GPU timings. `F11` or `WGPU_GPUTRACE_FRAME=N` writes a `.gputrace` for Xcode, and it needs `MTL_CAPTURE_ENABLED=1` because macOS refuses programmatic capture that was not enabled before Metal started.

**The capture has a bet in it, and the bet is documented.** wgpu-native exposes no `MTLDevice` — there is no HAL escape hatch anywhere in `wgpu.h` — so the capture targets `MTLCreateSystemDefaultDevice()` and relies on Metal device objects being per-GPU singletons *and* on wgpu having taken the default GPU. Verified rather than assumed: the trace came back at 16 MB, and one of its buffers is exactly 32768 bytes, which is what the renderer's own log reports for the ImGui vertex buffer. Those are our resources. If wgpu ever picks a non-default GPU the trace will come back empty, and the header says so.

**This is also where R1 paid off.** Its 28 object labels and two debug groups are what make a trace readable — "batch -> surface", "composite scaled target", "imgui" instead of anonymous draws — and until this item there was no way to *start* a capture, so that investment sat idle. Groundwork and trigger were two separate halves and only one had been built.

### What a frame rate tells you about a frame

`PresentMode::Fifo` does not degrade smoothly. It quantises to divisors of the refresh rate, so on a 75 Hz panel the only rates available are 75, 37.5, 25, 18.75, 15 — and a reported **15 fps means a frame took between 53 and 67 ms**, not "a bit over 13". Reading a frame rate as a budget rather than a speed narrows a performance question enormously before any profiling starts.

**And a slow frame should be able to describe itself.** The per-frame counters exist so that a frame far above its neighbours can say what it did that a normal one does not: a pipeline compiled mid-frame, a validation error, an allocation, or time spent blocking in an async drain. Every counter is zero in a steady frame, so a non-zero one names the cause — and **all zeros is a result too**, because it puts the cost outside the process.

### The CPU half: the build type was the frame time (W1)

A level that was 80% asteroid field ran at 30 ms of game logic a frame at
2560×1440. macOS's sampler (`sample <pid> 5`, no setup, works on a running
process) put three quarters of it in `asteroids::drawFields`, and most of
that inside `std::vector::push_back` and `glm::vec2`'s constructor: building
every visible rock's triangles on the CPU, every frame. Those are one-line
functions that an optimiser inlines away. The build had no `CMAKE_BUILD_TYPE`,
so nothing was inlined.

- **The same code at `-O2` ran the same level at 2.9 ms**, and loaded it in
  49 ms instead of 280. The default build is now `RelWithDebInfo` with
  `-O2 -g` and no `NDEBUG` (the renderer's `assert`s stay on). A debugger
  steps less faithfully through it, so a Debug build is a second build
  directory.
- **Profile before optimising, then check the boring fix first.** The plan
  had been to make rocks only near the camera; the measurement said that
  could wait. (It came back in W3, for a different reason: memory.)
- **Cost that grows with what exists, not what is drawn.** Even idle, frame
  time rose with every rock in the level (2.9 ms at 67,000, 6.9 ms at
  605,000), because three per-frame loops (heat, awake, sleep) walked them
  all. Drawing was already culled by the buckets (S1b); simulation wasn't.

**LearnWebGPU:** [Benchmarking / Time](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/benchmarking/time.html) — written, and not applicable on this adapter.

**Code:** `featureName` and `printAdapter` · `wgpu2d::FrameStats` / `frameStats` · `FramePerf` and its counters · `onQueueWorkDone` · `metalCaptureBegin` / `metalCaptureEnd` in `src/platform/wgpuMetalLayer.mm` · the panel in `gameLayer.cpp` and the slow-frame recorder in `glfwMain.cpp`

**Commit:** `97ed2de`

---

## This machine, and what it does differently

Facts established while porting, all specific to an Intel Mac with an AMD Radeon Pro 560X on Metal via wgpu-native. None of them are bugs in the renderer; each one cost time before it was understood.

| What you see | What it is |
|---|---|
| Adapter vendor, architecture and description strings are empty, `vendorID` is 0 | Normal on Metal; wgpu-native does not fill them |
| `minUniformBufferOffsetAlignment` is 256 | Sets the per-camera slot stride in 6b (`maxBindGroups` 8, `maxVertexAttributes` 31) |
| Surface never reports `Outdated` after a resize; the layer stretches the old drawable | Metal keeps presenting the configured size — compare `glfwGetFramebufferSize` every frame (see 5) |
| Surface formats offered: `BGRA8UnormSrgb`, `BGRA8Unorm`, two 10/16-bit ones | The port picks `BGRA8Unorm` so nothing gamma-corrects behind gl2d's back |
| Fullscreen on one secondary monitor looks slightly lighter until the menu bar is revealed | That monitor's profile on macOS's direct-to-display path. Pinning the `CAMetalLayer` color space (`095afaf`) did not change it; the OpenGL build is always composited, so it never shows it. **Run parity screenshots windowed on the main display.** |
| `TimestampQuery` is not advertised, nor either of wgpu-native's timestamp extensions | No GPU timing on this adapter at all. The guide's Benchmarking chapter cannot be followed here; see 16 for what replaces it |
| The panel refreshes at 75 Hz, and `PresentMode::Fifo` quantises to divisors of it | The only frame rates available are 75, 37.5, 25, 18.75, 15. A reported 15 fps means a frame cost 53–67 ms, not "a bit over 13" |
| wgpu-native exposes no `MTLDevice`, and `wgpu.h` has no HAL escape hatch | A Metal capture has to target `MTLCreateSystemDefaultDevice()` and rely on device objects being per-GPU singletons (16) |
| Redirecting the binary's output loses the last lines | stdout is fully buffered when redirected — the reports flush explicitly. macOS has no `timeout`; `perl -e 'alarm N; exec @ARGV' ./spaceGame` works |

---

## Not in the port (guide chapters skipped)

Skipped on the way through the milestone path. Several turned out to have a real
2D use once the port was finished; those rows say where they are picked up again
in [`roadmap.md`](roadmap.md).

| Guide | Why skipped |
|---|---|
| Lighting, PBR, cube maps, 3D meshes | 2D — cube maps and PBR stay out for good; lighting comes back in a 2D form as N8 |
| Depth buffer | depth off — but see N7, where depth is a sort key rather than an occlusion test |
| Compute pipeline (except as mipmap reading) | CPU mips instead — see N4 |
| Instancing | one growable buffer + draw runs — see N5 |
| Render bundles, MSAA | same — MSAA is N6; render bundles stay marginal |
| Building for the Web | native Metal only; `wgpuMetalLayer.mm` and the wgpu-native pin both stand in the way |
| Milestone 9 (text) | skipped by the port: gl2d's font path is almost all CPU (stb_truetype pack, glyph quads through the existing batch), and the game drew no gl2d text then — glui is layout only and every string was ImGui. Built later as U1 (below, "Text: the concepts") |

---

## Asteroids: the concepts

*The one section here written ahead of the work, at the author's request. The
rest of this file only describes things that exist. So each concept below
says whether it is **built** or **ahead** of the work, and as each of A1–A4
lands, its part becomes an ordinary block with a Code line. The items, and
what is still open about them, are in
[`gameplay-roadmap.md`](gameplay-roadmap.md) under "Asteroids (A1–A5)".*

### Getting textures into the game — *built*

**Concepts:** A **material** is several textures describing one surface:
colour (what it looks like), height (how high each point is), normal (which
way each point faces), and roughness (how shiny). The first is **colour data**
and is stored as sRGB. The rest are **measurements** and must stay linear:
putting a normal map through a gamma curve bends every normal. That is why the
EXR normal map could not simply be converted by a tool that treats files as
photos.

**Shrinking with a box filter.** Going from 4096 to 512 averages each 8×8 block
into one texel. Keeping every eighth texel instead would **alias**: detail finer
than the new texel size comes out as noise rather than as its average. It is
the same reason mipmaps are averages (7, and N4's compute version).

**Normals from height.** A normal map is the height map's slope. With central
differences, dh/dx is the texel to the right minus the texel to the left, over
two. The surface tilts away from the rise, so the normal is (−dh/dx, −dh/dy, 1)
normalized. The height's 0..1 range says nothing about how tall that is in
texels, so a **strength** factor sets the bumpiness: the tool reports the mean
normal z (1 is flat; about 0.9 is a bumpy rock). The material **tiles**, so
slopes at an edge **wrap around** to the far side instead of meeting a cliff.

**Conventions a normal map carries.** It is stored as n × 0.5 + 0.5, so 128 is
zero and a flat surface is (128, 128, 255), the lilac-blue every normal map
has. `_gl` means **OpenGL's convention**: +y points *up* the image. DirectX
maps flip green. This game's world is y-down, so the shader has to flip y once,
on purpose (A3).

**Code:** `tools/asteroidTextures.cpp` · `tools/asteroidTextures.sh` ·
`resources/asteroid/`

### A1. Shape, triangles, and hit tests — *built*

- **A seeded generator:** the same seed gives the same rock, so a level stores
  four numbers, not a mesh. **Periodic noise** around the loop: a sum of sines
  with *whole-number* frequencies returns to its start after one turn, so the
  outline has no seam where the angle wraps.
- **Star-shaped polygons**, and why a **triangle fan** works on them: a fan from
  a point is a valid triangulation exactly when every edge can be seen from
  that point. The set of such points is the polygon's **kernel**. A convex
  polygon's kernel is the whole polygon; a star-shaped one's kernel includes
  its centre.
- **Ear clipping**, the general alternative. An ear is three consecutive
  corners whose triangle is inside the polygon and holds no other corner. Cut
  it off and repeat: O(n²), and it works on any simple polygon. It tells convex
  corners from reflex ones by the **sign of a 2D cross product**, which is also
  what **winding order** (clockwise or counter-clockwise) is.
- **Texture coordinates in the body's own frame:** each corner's uv is fixed
  where it sits on the rock, so the texture turns with the rock. Planar
  mapping: uv = local position ÷ size + a window offset. A **clamp-to-edge
  sampler** (4) is why each rock takes a *window* of the texture rather than
  tiling it; a repeat sampler is the alternative.
- **The batch taking triangles** (library): today the batch is six vertices
  per rectangle with no index buffer (3, 6a), grouped into runs by texture,
  camera, blend and effect (6b, 12). Triangles mean runs measured in vertices,
  not in rectangles. The shortcut is a **degenerate triangle**: a rectangle
  with one corner repeated, so its second triangle has no area and draws
  nothing.
- **Hit tests on the same triangles:**
  - **point in triangle** by the signs of three cross products (or barycentric
    coordinates);
  - **circle against triangle** by the closest point on the triangle;
  - **ray against the outline's edges** for the beam, and for **line of
    sight**, which is what makes a rock a hiding place.
  - A **bounding circle** first (the broad phase) rules out almost every pair
    before any triangle is tested (the narrow phase).
- **Local space to world space:** the rock's corners are stored around its own
  centre and rotated and moved each frame. That is a 2D model matrix, done
  on the CPU because the batch takes world pixels (the port's rule, above).

**What the build taught.**
- **Cyclic order is the invariant.** Corner angles only have to increase
  *around the loop*: the first corner may sit just below 0 and read as nearly
  2π. What guarantees a valid fan is every fan triangle's cross product
  having the same sign, which is how the generator is tested (2000 seeds).
- **The batch needed one number per record.** Where its vertices start. Once
  runs are measured in vertices, rectangles and triangles share one path and
  one draw call per run.
- **Queries move into the body's frame.** Moving one point into the rock's
  frame (the inverse rotation) is cheaper than moving thirty corners out of
  it.

**Code:** `include/engine/polygon.h` · `src/engine/polygon.cpp` ·
`Renderer2D::renderTriangles`, `pushTriangles`, `recordBatchEntry`,
`batchQuadFirstVertex` and the repeating samplers in
`src/render/wgpuContext.cpp` · `include/gameLayer/asteroids.h` ·
`src/gameLayer/asteroids.cpp` · the `asteroid` line in
`src/gameLayer/level.cpp` · the Asteroid tool in
`src/gameLayer/levelEditor.cpp`

### A1b. Fields: scatter, painted areas, and an outline — *built*

- **Scatter by cell hash:** one item per grid cell, and everything about it
  comes from a hash of (seed, cell x, cell y). No cell depends on another, so
  growing the region only fills new cells. The cost is spacing that is a
  little less even than **Poisson-disk** sampling (dart throwing with a
  minimum distance), which is the usual choice when stability does not matter.
- **A size that leans small:** for u uniform in 0..1, u^k with k > 1 crowds the
  results toward 0. min + (max − min) · u^2.5 has its median near a sixth of the
  way up.
- **No overlap by construction:** a rock's radius plus its offset from the cell
  centre plus half the gap never exceeds half a cell. So it cannot leave its
  cell, or come within the gap of a neighbour's rock.
- **One grid is not enough, so layers.** A single grid's cells must fit its
  largest rock, so small rocks sit alone in cells built for big ones: measured,
  about 280 of empty space to the nearest rock even at gap 0. So there are
  several grids, coarse to fine, each with rocks half the size of the one
  before. A fine rock is kept only if it clears every coarser rock by the gap.
  With three layers that falls to about 70. It is the same idea as octaves of
  noise, or a mipmap chain: each level fills in the detail the one above
  cannot hold.
- **A floor becomes the norm when you fill to it.** With one gap for every rock
  and layers packing every space that clears it, nearly every spacing lands on
  the gap itself (measured at gap 300: the closest 10% were 300 to 307), and
  even spacing reads as a grid. Giving each rock its own clearance, 0 to the
  gap from its cell's hash, and keeping the average of two clearances between
  them spreads the spacings from nearly touching to the full gap. The gap stays
  a cap on what any rock asks for, not a value every pair meets.
- **Clumping with value noise, and why it had to be flattened.** Value noise
  is random values on a lattice, blended smoothly across each square. It
  drifts, with no seams and no direction. A threshold on it makes gaps
  between clumps. But blending and averaging octaves both squeeze values
  toward the middle: measured, 80% sat between 0.29 and 0.71, so a threshold
  at 0.3 emptied almost nothing and one at 0.55 emptied most of the field.
  Passing the noise through its own approximate **cumulative distribution** (a
  logistic with the same spread) turns each value into its rank, close to even
  over 0..1. A threshold t then empties about a fraction t: the slider means
  what it says. *(Built and measured, then taken out of hand-painted fields:
  a gap inside the paint still counted as cover. It waits in `engine/scatter`
  for procedural levels, which will use it to decide where to paint.)*
- **Thinning layers evenly does not thin the result.** Cutting every layer's
  fill by the same fraction barely showed, because the finer layers have
  4× and 16× the cells and packed the thin parts back in. Gaps had to be
  empty in *every* layer at once.
- **A spatial hash for "what is near here".** Accepted rocks are bucketed by a
  grid as coarse as the largest cells. Anything that could touch a candidate
  is within one bucket of it, so each check looks at 9 buckets, not every
  rock.
- **A painted area as an ordered list of circles:** the last stamp covering a
  point decides whether it is in. It's cheap to test, smooth at any zoom, and
  plain text on disk. A bitmap mask is the alternative, and makes erasing
  exact but costs memory and resolution.
- **Outline by neighbour sampling:** a fragment is on the sprite's edge if it
  is solid and some texel a line-width away is clear. Nine taps with
  `textureSampleLevel`, all inside the sprite's own rectangle.
- **Screen-space derivatives:** `fwidth(uv)` is how much uv changes from one
  screen pixel to the next, measured from neighbouring fragments. A
  minification-proof line width is max(texels, fwidth · pixels). Without it the
  outline broke into dashes wherever the ship was drawn smaller than its
  texture. Derivatives are only defined in **uniform control flow**, so they
  are taken before any branch.
- **What an overlay may draw over something that covers it.** Drawn after the
  rocks, the outline pass sits over them. Its first fill used the sprite's own
  colours at 18%, which tinted the rock above the ship and read as the two
  blending. An overlay meant to say "behind" should only add things that are
  clearly not the object: a line, or a flat silhouette.
- **Draw order as depth:** field rocks are simply drawn after the ships.
  Hiding "behind" is the painter's algorithm, with no depth buffer; the outline
  is drawn after the rocks for the same reason.

**Code:** `include/engine/scatter.h` · `src/engine/scatter.cpp` ·
`AsteroidField` in `include/gameLayer/level.h` · fields in
`src/gameLayer/asteroids.cpp` · `include/gameLayer/outline.h` ·
`src/gameLayer/outline.cpp` · `resources/shaders/outline.wgsl` · the Paint
tool in `src/gameLayer/levelEditor.cpp`

### A2. Rigid bodies — *built*

- **State:** position, velocity, angle, angular velocity. **Properties:** mass,
  centre of mass, moment of inertia, all worked out once from the fan
  triangles:
  - each triangle's area is half a cross product;
  - its centroid is the average of its corners;
  - its inertia about its own centroid has a closed form;
  - the **parallel axis theorem** moves each one to the body's centre of mass.
- **Impulse at a point:** Δv = J / m, and Δω = (r × J) / I, where r runs from
  the centre of mass to the hit, and the 2D cross product is a scalar. A hit
  through the centre only pushes; a hit at the edge pushes and spins.
- **Force against impulse:** a bullet is an instant change of momentum. The
  beam is a force, applied as force × dt each frame, so it depends on the time
  step and an impulse does not.
- **Integration:** semi-implicit Euler (velocity first, then position with the
  new velocity) for both the straight-line and the spinning halves. Damping,
  if any, is an exponential falloff, like `movement`'s drag (R8).
- **If rocks collide with each other:** the separating axis theorem on convex
  pieces, a contact point and normal, and an impulse with **restitution** (how
  bouncy) along the normal.

**What the build taught.**
- **The second moment in one formula.** A triangle's polar second moment about
  the origin is area/6 · (a·a + b·b + c·c + a·b + b·c + c·a). Summed over the
  fan, then moved to the centroid by the parallel axis theorem *backwards*
  (I_centroid = I_origin − m·d²). Checked against a disc (m r²/2) and a square
  (m (w² + h²)/12).
- **The centre of mass is not the fan's apex.** A rock turns about its centre
  of mass, but the fan must stay rooted at the point every edge can be seen
  from. So the outline keeps its own frame, and each step the body places that
  frame: its origin sits at the body's position plus the rotated offset back
  to the apex. Two points, two jobs: one to turn about, one to draw from.
- **Inverse mass.** Bodies store 1/m and 1/I, because every formula divides by
  them, and something immovable is then a clean 0 rather than an infinity.
- **Collision in two steps:** first push the overlapping pair apart, the
  lighter one further; then, only if they are still closing, apply the impulse
  that turns the closing speed into −e times itself. Without the first step,
  two overlapping rocks with no closing speed stay stuck together.
- **Circles stand in for polygons** between rocks: a circle of the same area.
  It is cheap, and wrong only at the corners of long rocks.
- **Friction is what makes collisions tumble.** A bounce along the line
  through two centres cannot spin either body. The surfaces' sliding at the
  contact point (velocity + ω × r for each) is resisted by a tangential
  impulse. It is sized to stop the sliding, where turning resists too (the
  r²/I terms), and capped at μ × the bounce's impulse (Coulomb's law). Applied
  at the contact point, it hands spin across.
- **Resting contact.** Bodies pressed gently together should touch, not bounce;
  bouncing on every tiny overlap makes them buzz. The standard fix is no
  restitution below a small closing speed.
- **The direction of an impact is the contact normal.** Pushing everything
  along the attacker's motion moves a crowd as one block. Pushing along the
  line from the contact to each body's centre scatters it, like a break shot
  in billiards.
- **Immovable is zero inverse mass.** A field's core is the same rigid body
  as any rock, with 1/m and 1/I set to 0. Every push multiplies by them and
  does nothing, and a collision's split of the overlap by lightness gives the
  core none of it. No special case in the physics, only in the policy (a core
  is never woken, loosened or struck).
- *(Tried and taken out: a gravity well, space bending toward a core while
  its rocks drifted home. It sampled each pixel from a little further out
  than itself, with a ripple travelling inward, and needed a fourth vec4 of
  effect parameters. WebGPU allows that without touching other shaders,
  because a binding need only be at least as large as the shader's struct.
  The look was not wanted, so both came out.)*
- **Damping hides in springs.** A damped spring's velocity term (2ζω·v) is a
  drag, and at a stiff setting it swallowed every impulse before the rock
  could move. Holding the spring off for a while after each hit, and passing
  that clock along a chain of bumps so the chain returns together, is what let
  the field go wild and still settle.
- **Springs as accelerations.** A field rock's pull home is
  a = −ω²·offset − 2ζω·v: a damped oscillator whose period (2π/ω) and settling
  (ζ) are the same for every rock, because mass never enters. ζ = 1 settles
  without overshoot; below 1 it sways a little.
- **Sleep.** Most rocks are still most of the time. Only awake ones are stepped
  and collided, so a thousand rocks cost what the few moving ones do; a push
  wakes one, and so does being bumped.
- **Rebuild the spatial hash each frame.** Moving things change buckets, and
  rebuilding a grid of a thousand is cheaper than tracking moves. A big rock
  goes in every bucket its circle covers, so bucket size need not fit the
  largest rock.

**Code:** `include/engine/rigidBody.h` · `src/engine/rigidBody.cpp` ·
`asteroids::update`, `shot`, `beam`, `blast`, `ram` in
`src/gameLayer/asteroids.cpp`

### A3. A lit rock — *built*

- **Normal mapping in 2D:** Lambert diffuse, brightness = max(N · L, 0), with
  N read from the map and L the direction to the light. **Tangent space** is the
  frame the map's normals are written in. On a flat 2D rock it is just the
  rock's rotation, so the shader rotates each normal by the rock's angle before
  lighting — the texture turns, the light does not — and flips y for the y-down
  world.
- **Mipmapped normals get shorter:** averaging unit vectors that point
  different ways gives a shorter vector. So the shader renormalizes after
  sampling.
- **Two maps, one binding:** an effect sees one texture (group 0). Packing
  colour and normal side by side and sampling both halves is a layout trick.
  A second binding is a change to the **bind group layout** (4), which is the
  shader's contract.
- **The height map as a mask:** a threshold that moves with a parameter reveals
  the map from its lowest points up. It is the shield dissolve's trick (C1),
  used for cracks and for where the beam's heat glows first. **Emissive** light
  is added after lighting, so it glows even on the dark side.
- **Interpolated vertex attributes:** anything a vertex outputs is blended
  across the triangle by the rasterizer. A value of 1 on the outline and 0 at
  the centre arrives in the fragment shader as "how near the edge". That
  gives a rim with no extra texture.
- **Per-quad effect parameters** (F6) carry each rock's angle, heat and damage.
  Rocks with different parameters split the batch into more runs (12), so the
  parameters are the price of the look.

**What the build taught.** Two of the plans above changed on contact:
- **Channel packing, not two maps side by side.** The side-by-side layout
  breaks under a repeating sampler (it wraps across the join) and mipmaps
  seam at the join. So one RGBA texture holds brightness, normal x, normal y
  and height, and a single tint puts the hue back. Games do this all the time
  (a "mask" or "ORM" texture). It only works because the mip levels here
  average each channel on its own (N4's compute shader). A mip builder that
  weighted colour by alpha would have bent the normals wherever the height
  was low.
- **Per-object data on the vertices, not per-quad parameters.** Per-rock
  effect parameters would have split the batch into one draw per rock. The
  vertex colour carries it instead, and all rocks share one parameter set
  and one draw:
  - the light's direction, turned on the CPU into each rock's frame (so the
    shader needs no angle);
  - the rim;
  - heat, faded with distance from the burn.

  Heat fits in alpha because rocks are opaque and the shader writes alpha 1.
  (A5 changed this layout: the light moved to a uniform, the rim became the
  dome's "how far to the edge", and alpha now also carries a shade.)
- **High-pass by mip difference.** "Lower than a threshold" on a height map
  that is mostly broad hills picks hollows, not cracks. The first heat was one
  flat white blob. A coarse mip level is the local average height, so average
  minus height is what stands out from its neighbourhood (a high-pass filter)
  for the price of one more texture read.
- **Coarsening without shimmer.** Snapping uv to a coarser grid has a zero
  derivative inside each step, so the hardware would choose mip 0 everywhere
  and alias. `textureSampleGrad` with the *unsnapped* coordinate's
  derivatives keeps the right mip level.
- **A shadow needs something to land on.** The first shadows were the rock's
  own fan in black, soft because its outer corners were transparent and the
  rasterizer faded across each triangle. They were drawn on the starfield,
  which is far behind everything, and read as smudges in space. Now a shadow
  is a question asked of each ship: is it inside a field rock's outline
  shifted away from the light? Nine sample points over the hull give a share,
  so the tint fades rather than snaps. It is a per-object answer, not a
  per-pixel one. An exact shadow edge across a ship would need a mask of
  where the ships are (a stencil or a render target, N7).
- **Parallax as decoration only.** Foreground debris moves further than the
  world by (position − camera) × k and is drawn (1 + k) larger. Because it is
  never solid, being drawn where it isn't costs nothing, and the rocks that
  block and hide stay in the world plane. *(Built, then turned off by default:
  it crowded the foreground.)*
- **Depth is read from speed, whatever the draw order.** The starfield was
  shifted by −view × strength, which moves a layer *faster* than the world.
  Its comment said slower, and nobody noticed until solid rocks appeared in
  the world plane, with stars drawn behind them sliding past faster than
  they did. The eye trusts relative motion over layering, so the stars read
  as in front. Shifted *with* the view (by view × strength), a layer moves at
  (1 − strength) × the world. Every background layer then moves slower than
  the play, and the play reads as nearest.

**Code:** `resources/shaders/asteroid.wgsl` · `drawFan`, `shadowOn`,
`beginRocks`, `drawForeground` and the heat in
`src/gameLayer/asteroids.cpp` · the packed output in
`tools/asteroidTextures.cpp` · `resources/asteroid/rock_packed.png`

### A4. Breaking up — *built*

- **Voronoi fracture.** Scatter a few *sites* inside the rock. Each piece is
  the part of the rock nearer its own site than any other: the rock clipped,
  one **perpendicular bisector** at a time, to the side nearer its site. A
  bisector is the line of points equally far from two sites, so the half
  nearer site i is where p · (sⱼ − sᵢ) ≤ (|sⱼ|² − |sᵢ|²) / 2.
- **Sutherland–Hodgman, one half-plane at a time.** Walk the edges; keep what
  is inside; where an edge crosses the line, add the crossing. An edge running
  *along* the line, from one crossing to the next, is a new border. Labelling
  it with the neighbour's index is what turns the clip into a **crack
  network**: the same borders are drawn as cracks before the break, and are
  where it breaks. Each crack gets a random threshold, so damage opens them
  one by one.
- **Point in any polygon: the crossing number.** A ray from the point crosses
  the outline an odd number of times exactly when the point is inside. The fan
  test used until now only holds for star-shaped shapes; pieces need not be.
- **Ear clipping.** An ear is three neighbouring corners whose triangle turns
  the polygon's way and holds no other corner. Clip it and repeat; O(n²). It
  is only needed when a fan fails, and the test for that is whether every fan
  triangle turns the same way.
- **Mass from any triangles** is the same sum as from a fan: area, centroid and
  second moment per triangle, then the parallel axis theorem. It matched the
  fan on a real rock to five digits. (Built for pieces that were rocks; the
  shards that replaced them have no mass, so nothing in the game calls it
  now. It stays in `engine/rigidBody`.)
- **Handing on the motion:** a piece's velocity is the parent's plus the
  parent's spin at the piece's centre, v + ω × r, plus a kick outward. Each
  piece's texture offset moves by its centroid, so it wears exactly the stone
  it was.
- **Scaling laws make tuning honest.** Ore goes with area, so a rock twice
  as wide holds four times as much. Health goes with area^0.75, so a big rock
  is tougher without taking minutes.
- **Don't change what you're iterating.** Breaking removes a rock. Done
  inside the loop that hurt it, that would remove rocks under the loop, and a
  reference held across it would dangle. So damage only
  marks rocks, and `processBreaks` runs after the loop. One call site still
  broke and then touched the old rock; the fix was to break last.
- **A thing in play vs. a picture of one.** A broken rock's pieces became
  *shards*: a separate list with only a shape, a place, a velocity and a
  spin -- no body, no health, no hit test. Leaving them out of the rock list
  is what takes them out of play; nothing has to check a flag.
- **A spring to a slot.** Each shard remembers where it sat in the rock and
  where the rock's home is, and a critically damped spring (the A2 one, eased
  in after the flight) pulls it there -- the pieces reassemble into a broken
  silhouette of the rock.
- **Clamping only what you added.** The beam's push is held to a creep by
  removing any velocity along the beam above the cap -- but only above
  whatever the rock already had, so the beam never brakes a rock a shot sent
  flying.
- **Pools and caps:** at most 400 shards, the oldest shrinking away, like the
  wreck field (C4a).

**Code:** `voronoiFracture`, `earClip`, `fanWorksFrom`, `contains`,
`signedArea` and `centroid` in `src/engine/polygon.cpp` · `fromTriangles` in
`src/engine/rigidBody.cpp` · `ensureCracks`, `hurt`, `breakInto`,
`processBreaks`, `updateShards`, `drawShards`, `drawCracks` and the mining in
`src/gameLayer/asteroids.cpp` · `BeamImpact::Deflect` in `bulletLook` ·
`effects::rockBurst` · `resources::emitOrb`

### A5. Volume: a height field, lit and self-shadowed — *built*

- **A surface as a height field.** The rock is treated as a height at every
  point: a *dome* for its overall shape, plus the texture's height map for
  detail. Lighting and shadows both come from that one idea.
- **The dome's normal from the fan.** Each fan triangle runs from the rock's
  centre to one straight edge, so a value of 0 at the centre and 1 at the
  corners, blended across the triangle by the GPU, is *exactly* how far this
  pixel is toward the edge. The corners also carry their own outward
  direction. The dome's normal tilts along that direction by t², so the
  middle is flat and the edges turn away.
- **Blending normal maps ("whiteout").** To lay a detail normal over a base
  one, add their tilts (xy) and multiply their ups (z). A bump on a slope
  tilts the slope further, which adding and renormalizing would flatten. It
  is applied twice: the dome, then the large-scale layer, then the detail.
- **Screen-space derivatives recover a rotation.** The texture is laid on
  each rock at one fixed scale and turned with it. So `dpdx(uv)` and
  `dpdy(uv)` -- how the texture coordinate changes one pixel right and one
  pixel down -- are the rock's rotation, scaled. A world direction pushed
  through them comes out in the rock's own frame. The CPU used to turn the
  light per rock and send it on the vertices; now it's one uniform, and the
  two vertex slots it used carry where the pixel is on the rock instead.
- **Self-shadowing by ray marching a height map.** From each pixel, step
  toward the light across the height map, the ray climbing at the light's
  elevation. If the height map anywhere stands above the ray, the pixel is
  in shadow; how far above sets how dark, which softens the shadow's edge.
  It uses `textureSampleLevel`, which, unlike `textureSample`, is allowed
  inside a loop because it takes no derivatives.
- **Derivatives only in uniform control flow.** `dpdx`, `fwidth` and
  `textureSample` compare neighbouring pixels, so every pixel of a 2×2 quad
  must reach them together. They are all taken first, before any loop or
  branch.
- **Analytic anti-aliasing.** `fwidth(t)` is how much t changes across one
  pixel, so (1 − t) / fwidth(t) is the distance to the edge *in pixels*.
  Fading alpha over the last pixel smooths the silhouette without MSAA.
- **Blinn-Phong specular.** The highlight is brightest where the surface
  faces the *halfway* vector between the light and the eye (straight above,
  in 2D). A high power makes it small and sharp, which reads as hard, glossy
  stone. The core has a lot of it; the ordinary stone almost none.
- **Two octaves of one texture.** The texture sampled again at 1/5 the
  frequency (with its derivatives scaled to match, so the mip level stays
  right) gives big features to big rocks without a second image.
- **Packing data into what's there.** The vertex colour's alpha is heat when
  it's positive and a shade when it's negative: nothing is both hot and
  shaded, so one float carries either. A shard cut into triangles with no
  centre marks its edge value −1, and the shader falls back to the corners'
  directions.
- **Fitting a shape inside a region.** To fit a star-shaped outline inside a
  painted area, walk each corner's ray out from the centre until it leaves
  the area; the smallest (exit distance ÷ corner distance) is the scale that
  fits them all. Because the shape is star-shaped from its centre, the edges
  between corners stay inside too, as long as the area doesn't have a notch
  narrower than one corner spacing.
- **A wider parameter block.** `EffectParams` gained a fourth vec4, `d`. The
  uniform slot grew from 80 to 96 bytes; the dynamic offsets between slots
  were already rounded up to the device's alignment, so nothing else moved.

**Code:** `resources/shaders/asteroid.wgsl` · `drawFan`, `beginRocks`,
`Surface` in `src/gameLayer/asteroids.cpp` · `EffectParams::d` in
`include/render/wgpu2d.h`, `EffectUniforms` in `src/render/wgpuContext.cpp`


---

## Sight: the concepts

*Line of sight, the fog, and what the player and the enemies remember, from
the `line-of-sight` branch. The items, the decisions and the playtests behind
each are in [`sight-roadmap.md`](sight-roadmap.md) (S1–S7). This section is
the concepts, as built. Most of it is CPU geometry rather than GPU work; the
GPU part is S3's fog, which grew out of 10, 12 and F6. Since M1, weapons do
not follow the sight rule (only a core stops them), so "the rule" below is
sight's alone.*

### S1. A painted area as a grid — *built*

**Concepts:** A field's area is circles painted and erased in order. Asking
"is this point inside" of the stamps means replaying every stamp, about 450
on `level3`, for every question. Instead the stamps are **rasterised once**
into a grid of labels, and a lookup becomes one division and one read.

- **Order matters, so each layer gets its own scratch grid.** Within a
  field, the last stamp covering a cell's centre decides it. An eraser must
  not cut another field's paint, so each field is painted in its own
  scratch, then laid over the labels. The later field wins where two
  overlap.
- **A stamp costs its own area.** Each stamp visits only the cells under its
  bounding square, not the whole grid.
- **Aligned to the world.** The grid's origin sits on a multiple of the cell
  size, so a world point is in the same cell however the bounds come out.
- **The grid is the truth.** Its edges are stair steps a cell wide, and
  hiding, sight and the fog all read the grid, so they agree on where a
  field ends. The exact stamp test is kept for building the grid, and for
  deciding whether a field rock has been knocked out of its own field.

**What the build taught.**

- **A test can be weaker than the thing it tests.** The first check of the
  line walk (below) sampled each line every 1/50 of a cell and reported
  about 0.2% of lines as wrong. Every "error" was a corner the line clipped
  between two samples. Replacing the sampling with an **exact reference**
  fixed it: every grid-line crossing worked out in closed form, sorted, and
  the label read at each interval's midpoint. Then 18,000 lines agreed
  exactly. Before trusting a mismatch, ask whether the reference can see
  what the code sees.

**Code:** `include/engine/regionMask.h` · `src/engine/regionMask.cpp` ·
`buildMask` and `paintMask` in `src/gameLayer/asteroids.cpp`

### Walking a line through a grid: the DDA — *built*

**Concepts:** Given a segment from A to B, list every grid cell it passes
through, in order, with the distance at which it enters each.

- **The key fact.** A straight line crosses the vertical grid lines at evenly
  spaced distances along itself, and the horizontal ones at a different but
  also even spacing. It changes cell exactly at those crossings and nowhere
  else. So the walk is a **merge of two arithmetic sequences**: always take
  the nearer next crossing.
- **Four numbers.** With `P(t) = A + t·dir` and `dir` of length 1, so `t` is
  distance:
  - `tDeltaX = cell / |dir.x|` is the distance between two vertical
    crossings, and never changes;
  - `tMaxX` is the distance to the next one;
  - `stepX` (±1) is which way the column index moves;
  - likewise for Y. A line parallel to an axis has `tMax = ∞` on it.
- **The loop.** If `tMaxX < tMaxY`, step the column and add `tDeltaX`;
  otherwise step the row and add `tDeltaY`. Stop past B. One comparison and
  one addition per cell, and every cell the line touches is visited exactly
  once, a corner clip included.
- **A worked example**, with cells of 50, A = (10, 20) and B = (160, 95):
  `dir` = (0.894, 0.447), `tDeltaX` = 55.9, `tDeltaY` = 111.8, and the first
  `tMax` values are 44.7 and 67.1.

  | compare | nearer | enters | at t |
  | --- | --- | --- | --- |
  | 44.7 vs 67.1 | X | (1, 0) | 44.7 |
  | 100.6 vs 67.1 | Y | (1, 1) | 67.1 |
  | 100.6 vs 178.9 | X | (2, 1) | 100.6 |
  | 156.5 vs 178.9 | X | (3, 1) | 156.5 |
  | 212.4 vs 178.9 | — | stop: 178.9 is past B (167.7) | |

  It ends in cell (3, 1), which is where B is.
- **Clipping first.** A segment that starts outside the grid is clipped to
  the grid's rectangle (the slab test, the same one used for a ray against a
  box), so the walk never steps through empty space. Outside counts as one
  cell, "nothing".
- **Exact corners are ambiguous.** When `tMaxX == tMaxY`, the line passes
  through a grid corner, and one side cell is visited for zero length. Any
  two methods can disagree there. That turned out to be the only kind of
  disagreement left between S2's map and this walk.
- **Cost.** About (|Δx| + |Δy|) / cell steps. A 4,000-unit sight line on a
  50-unit grid is at most 113.
- **One walk, two grids.** The same function walks the painted area's labels
  (`region::march`) and the rock buckets (`spatial::raycast`), so there is
  one implementation to trust. It was moved out of `march` and re-verified
  against the exact reference.

This is Amanatides and Woo's voxel traversal (1987). Wolfenstein 3D's
raycaster used the same walk on its 2D map. DDA means digital differential
analyser, the line-drawing method it descends from.

**Code:** `include/engine/gridWalk.h` · `src/engine/gridWalk.cpp`

### S1b. Buckets: a uniform grid of items — *built*

**Concepts:** Every rock question (shots, sight, the beam, shadows, drawing)
used to loop over every rock. A **uniform grid** lists each item in every
cell its bounding square overlaps, so a question looks only at the cells it
covers. It suits items of similar size, like a field's rocks; widely mixed
sizes would want a tree (a quadtree, a BVH).

- **Flat storage, rebuilt by a counting sort.** Two arrays, not a vector per
  cell:
  1. count the items landing in each cell;
  2. prefix-sum the counts into `cellStart`;
  3. write each item's index at its cell's next free slot.

  Cell c's items are `items[cellStart[c] .. cellStart[c+1])`. A rebuild is
  linear in the items, with no hashing and almost no allocation.
- **Each item once per query.** An item listed in several cells would be
  handed out several times. A stamp per item, set to the query's number,
  skips the repeats.
- **Rays walk the buckets near to far and stop early.** Once a hit is found
  at distance t, any bucket that begins past t can be skipped. Anything
  nearer would contain the nearer hit point, that point lies in a bucket
  entered no later, and the item is listed there.
- **Slack: loose bounds.** Each rock is listed under its circle plus some
  slack. The grid is rebuilt only when some rock has moved further than the
  slack since the last build. The circle is centred on the centre of mass,
  so a rock spinning in place never counts as moving.
- **Exactly the same answers.** To make "nothing changed" checkable, the grid
  returns what the old loop over every rock did: `hitCircle` the
  lowest-numbered rock, `raycast` the later rock on an exact tie, drawing
  in stored order. A temporary mode ran every query both ways, and they
  never disagreed.

**What the build taught.**

- **Measure before tuning, then measure the build.** A rebuild of 65,000
  rocks took 8.9 ms in the game, and 0.57 ms when the same code was compiled
  at `-O2`. The algorithm was fine; the build was unoptimised. That
  measurement is why W1 made `-O2 -g` the default build.

**Code:** `include/engine/spatialGrid.h` · `src/engine/spatialGrid.cpp` ·
`rockIndex` and `checkListed` in `src/gameLayer/asteroids.cpp`

### S2. A polar map: a one-dimensional shadow map — *built*

**Concepts:** What can be seen from one point. The circle round the viewer is
cut into N slices (720 by default), each holding how far the viewer sees
along its centre line. It is a shadow map with "depth per pixel" replaced by
"distance per angle".

- **Everything that blocks is a segment.** A segment covers the slices whose
  centre lines it crosses.
  - The angle it covers is `atan2(cross(a, b), dot(a, b))`, taking `a` and
    `b` relative to the viewer.
  - Along each slice direction `u`, the distance is one line intersection:
    `t = cross(a, b − a) / cross(u, b − a)`.
  - Each slice keeps the nearest. Writing every edge of an outline leaves
    its silhouette, with its far side hidden for free.
- **Field edges are segments too.** They are the sides between cells the rule
  treats differently. A line from the viewer crosses a side going away from
  the viewer's side of it, so the cell on the viewer's side decides whether
  the side blocks: any change, or only one going into a field. That replaced
  a walk per slice (58,000 callback steps) with one scan of the box round
  the viewer.
- **The fan is the truth.** Each slice's end point is visible from the
  viewer, so the end points make a polygon that is star-shaped from there,
  and a triangle fan draws it, as A1 draws a rock. "Can the viewer see this
  point" tests the triangle the fan draws there, so the rules and the
  drawing are the same shape. The cost is a wedge about one slice wide at
  the side of a shadow, where neighbouring slices differ a lot.
- **Merging two maps is a maximum.** Two shapes seen from the same point,
  combined slice by slice by the further distance, make one shape still all
  visible from that point. That is how the scope's cone joins the all-round
  sight.
- **Building over a box.** A map can be built over any box: the square round
  the viewer, or just a cone's bounding box. The cone's box is its apex, the
  ends of its sides, and the arc's extremes where it takes in an axis. The
  scope's long cone and each enemy's cone (S6) use it.

**What the build taught.**

- **Seal the corners, by a hair.** Two cell sides meeting at a corner can let
  a ray through the gap in floating point. Extending each side by 1/1,000 of
  a cell sealed it, but caught rays merely passing near a corner too, and
  blocked about 100 times more slices than it should. 1/100,000 of a cell
  covers the rounding and nothing else.
- **Two implementations of one rule, compared slice by slice.** The map and
  the line walk were checked against each other along every slice centre,
  from random viewpoints, under every rule. What remained, about one slice
  in a million, was all exact grid corners. One "leak" was a line going
  diagonally from paint into paint past a one-cell notch, where the map was
  right and the walk's tie-break was not. A later round found a real bug: a
  viewer standing over a field rock poking out of the paint, where the map
  saw that rock's far side and the walk blocked at 0.

**Code:** `include/engine/visibility.h` · `src/engine/visibility.cpp` ·
`build`, `addEdges`, `coneBox`, `blockedAt` and `updatePlayer` in
`src/gameLayer/sight.cpp`

### Looks: cones, soft sides, and reach — *built*

**Concepts:** A viewer has a **look**: a facing, a half-angle and a range.
From inside a field, a line within the cone sees past the field's edge; from
outside, the player's cone sees into a field, with the field's own rocks
casting shadows.

- **Which crossing comes first decides.** The first edge a line crosses
  either leaves the paint (the viewer is inside) or enters it (outside). In
  the map, the sides a line leaves by go into a map of their own, so each
  slice can compare "first way out" with "first other edge".
- **Soft sides by weight.** A slice near a cone's side gets part of the reach
  past the edge, fading over a few degrees, instead of switching on or off
  as the aim moves a hair.
- **Measure from the right place.** A reach measured from where each line
  leaves the field made the cone's far end the field's ragged edge pushed
  outward. Measured from the viewer, the far end is an arc.
- **Ease an angle by the angle.** Blending two directions and normalising
  barely moves a direction facing away from its target, and does not move
  one facing exactly away at all. A capture caught the cone pointing back
  into the field while the aim pointed out. Rotating by a share of the signed
  angle between them (`atan2(cross, dot)`) eases any turn the same.

**Code:** `Look`, `coneWeight`, `crossings`, `reachPast` in
`src/gameLayer/sight.cpp`

### S3. The fog: a mask without a mask texture — *built*

**Concepts:** Grey out what the player cannot see. `worldGrade` already put
the world in a screen-sized target and brought it back as one quad through a
grading shader (10, F6). The fog adds a second draw from the same target:

1. the whole view comes back **greyed** (a third grade, "unseen");
2. the sight fan is drawn on top from the **same texture, ungraded**.

So the fan's geometry is the mask. An effect reads one texture, and a mask
texture would have needed a second binding, which is library work. Drawing
the shape with the world as its texture needed none: `renderTriangles` with
a texture, plus `setEffect`.

- **Screen-space UVs.** Each fan corner's texture coordinate is its pixel
  position over the view's size. The target is exactly the view's size, so
  every fan pixel samples the texel under it, and inside the fan the image
  is the unfogged world itself, not a resampled copy.
- **Fade premultiplied colour by scaling all four channels.** A soft edge is
  a ring of triangles past the fan, with vertex colour going from (1,1,1,1)
  to (0,0,0,0). The target holds premultiplied colour (12), so fading alpha
  alone would leave bright fringes.
- **Hide information; grey scenery.** The fog grades the whole world, so an
  enemy drawn under it comes out grey but still says where it is. Enemies,
  their shots, plumes and cones are not drawn where unseen; rocks,
  explosions and debris are greyed.
- **Smooth the drawing, not the rules.** Rays crossing stair steps make the
  fog's edge jump. The fog is drawn from a copy of the map eased slice by
  slice over a moment, while the rules use the exact map.
- **Reveals.** A circle of sight, such as the one round what the player's
  beam burns, is drawn by the same pass as another fan, and the rules count
  it as seen.

**What the build taught.**

- **Runs are not repeatable.** Two hidden-window runs with the same settings
  differed in 12% of their pixels (wall-clock time in the shaders, one
  wandering enemy), so frames from different runs cannot be compared pixel
  by pixel. Alignment was checked by cropping across an edge instead: a
  half-pixel error would show as a doubled or shifted copy there.
- **Force the scene a capture needs.** Captures that depend on input (the
  aim, the scope, an enemy losing the player) were made with temporary
  environment overrides that pinned that one thing, then removed.

**Code:** `resources/shaders/worldGrade.wgsl` · `apply` in
`src/gameLayer/worldGrade.cpp` · `playerDrawnMap`, `reveal`,
`playerSeesShip` in `src/gameLayer/sight.cpp` · the `Shown` tests in
`src/gameLayer/effects.cpp`

### S4 and S5. Memory: ghosts both ways — *built*

**Concepts:** What was seen, after it is gone.

- **A ghost is born at an edge in time.** The memory keeps each thing's last
  seen state. On the frame it goes from seen to unseen, it leaves a ghost
  holding that state: where the thing *was*, never where it is.
- **Arm before clearing.** "Clear a ghost when its spot is looked at and
  found empty" fails at once, because the spot is where the thing was last
  seen, so it is in sight the frame the ghost is made. A ghost is **armed**
  by its spot leaving sight first; only then does looking back at an empty
  spot clear it.
- **Don't keep a second copy of a truth.** For where the *enemies* think the
  player is (S5), each searching enemy already holds that spot, the place it
  flies to. Copying it into the same memory would have been a second source
  of truth, so S5 reads the AI's own state.
- **Off-screen markers.** The gate's chevron became one function: a ray from
  the screen's centre, stopped at an inset rectangle, with a chevron at the
  tip. Ghosts use the same function, smaller, in their own colour.

**Code:** `include/engine/contactMemory.h` · `src/engine/contactMemory.cpp` ·
`src/gameLayer/lastKnown.cpp` · `src/gameLayer/theirGhost.cpp` ·
`drawChevron` and `markOffScreen` in `src/gameLayer/hud.cpp`

### S4b. A periscope camera — *built*

**Concepts:** Hold V: the view zooms out and leans toward the pointer,
lagging it.

- **A lean with no feedback.** The lean is the pointer's offset from the
  screen's centre, as a share of half the screen, times a share of the view.
  Moving the view does not move the pointer on the screen, so the lean
  cannot chase itself.
- **Ease by a rate, not at a speed.** `current = target + (current −
  target) · exp(−rate · dt)` closes the same share of any gap per second,
  whatever the frame rate, so a big jump settles as quickly as a small one.
  The existing chase moves at a fixed speed with a leash, which would crawl
  across a big lean or snap to it.
- **Zoom eases in log space.** Zoom is a ratio, so it is blended as
  `normal · scope^amount` rather than linearly (as `cameraZoom` already did).
- **Aim from the ship, not the screen centre.** Once the view leans off the
  ship, "pointer minus screen centre" stops being the direction from the
  ship to the pointer, so while scoped the aim is measured from the ship's
  position on screen.

**Code:** `ease` and `pointerLead` in `include/engine/cameraFollow.h` ·
`src/gameLayer/scope.cpp`

### S7. Three levels of fog: a blend as a mask — *built*

**Concepts:** In sight, colour; seen before, the grey fog; never seen, black.
"Never seen" is W6's explored map: a target over the arena, opaque where the
player has looked and transparent where it never has. The fog needed to
**darken what is already drawn, by that texture**.

- **None of the three blend modes could.** Alpha and Premultiplied lay a
  picture over another; Additive only adds. S3's note said a mask needed "an
  effect that reads a second texture": a second binding, a new shader. But
  a blend unit already has a second input, **the destination**. One more
  pipeline variant, `Mask`, colour `(Zero, SrcAlpha)`: `out = dst · srcAlpha`.
  Drawing the explored map over the greyed view through it keeps what it
  covers and blacks out the rest. No shader at all.
- **Colour and alpha blend separately, and here that is the point.** The
  alpha half is `(Zero, One)`: the destination's alpha is kept. Masked to
  black is then *opaque* black, not a hole the screen shows through, and a
  premultiplied target stays premultiplied: `(C·a, a)` becomes
  `(C·m·a, a)`, the colour `C·m` at the same coverage.
- **Order makes the mask safe.** Grey view, then the mask, then the sight
  fan in colour from the world target (S3). What is in sight is drawn after
  the mask, so it can never be blacked out, even on a frame where the
  explored map is one frame behind.
- **Texture coordinates map screen to world.** The mask is one screen quad
  whose corners' UVs are where their world points fall in the explored map;
  the sampler clamps past its edge.
- **The texel is the fog's edge.** A 2048-square map over a 200,000-unit
  arena is 98 units a texel: linear filtering turns that into a soft edge
  about 18 px wide at the farthest zoom.

**When a rule made a feature useless.** Scoped, the black hid everything:
the scope's cone is sight, sight stops at rocks, and the start is ringed by
field. Under the grey fog the land beyond still showed; under black the
scope showed nothing. The fix was a **game decision, not a bug fix**: the
scope surveys. Its whole cone, unblocked, is drawn into the explored map
(grey, mapped), while only true sight is in colour, with its enemies.

**Code:** `BlendMode::Mask` in `include/render/wgpu2d.h` and the
`switch (key.blend)` in `src/render/wgpuContext.cpp` · `Explored` and the
mask pass in `src/gameLayer/worldGrade.cpp` · `exploredMask` and `reveal`'s
`scopeCone` in `src/gameLayer/explorationMap.cpp`

### How this work was checked

The same few moves, every time, and worth reusing:

- **List the candidate causes before fixing anything.** Then test the
  cheapest. The jitter, the missing cone, the beam stopping at the edge,
  the 884 mismatched slices and the stuck cone were each a hypothesis
  confirmed by one print, one probe or one forced frame.
- **Two implementations of one rule, compared.** Map against walk, grid
  against brute force, the new against the old. Each disagreement is either
  a bug or an ambiguity worth naming.
- **Temporary scaffolding, removed.** Checks behind an environment variable,
  a probe that fires once, a forced scene for a capture. Never committed.
- **GPU readbacks as PNGs** for anything visual (15), sent to the author, who
  is the only one who sees the window.
- **Frame the capture so it can show the effect.** The chunk views showed
  nothing until the streamed area was shrunk inside the screen; the first
  three-level fog capture showed two levels until the ship was moved into a
  field. A capture that cannot show the thing proves nothing either way.
- **Small standalone programs against one engine file.** `movement.cpp` or
  `scatter.cpp` compiled with a `main` in the scratchpad: a lane's push, a
  medium's bleed, chunks against a whole scatter. Seconds to build and run,
  and the expected numbers written beside the printed ones. A **control**
  that should fail (chunks with no margin) proves the test can.
- **Force what a capture cannot press**, from an environment variable: the
  scope held, the ship set on a lane, the full map shown.

---

## Space: the concepts

*Levels that are mostly field, bigger maps, and ways across them, from the
`line-of-sight` branch: the W items of [`sight-roadmap.md`](sight-roadmap.md).
W1's measuring is in 16 and W6 feeds S7's fog; the rest is CPU work in
`engine/` and the game.*

### W1. Painting the clearings, not the field — *built*

- **Invert the brush.** A level that is 80% field is one stamp the arena's
  size and erasers for the clearings and lanes. Stamps apply in order, so
  the file format needed nothing; **Fill the arena** inserts its stamp
  *first*, so what was already erased stays erased.
- **Cache by value, regrow by change.** The editor regrew every rock every
  frame. It now keeps a copy of each field and its rocks, and regrows only a
  field whose copy differs.
- **A rule, applied at play time, not saved.** The gate always sits in a
  clearing: `level::fieldsAsPlayed` appends an eraser round it when a round
  starts, so moving the gate moves its clearing, and the file keeps only
  what was painted. Lanes and jump gates (W4, W5) cut themselves the same
  way.

### W2. A medium: scale, don't overwrite — *built*

- **The mechanism is a multiplier on the body's own top speed** (`Medium` in
  `engine/movement`), applied as each step builds its options. Overwriting
  the options would have erased what set them: an enemy's speed rolled at
  spawn, its enraged boost, the player's flight multipliers. Scaled, they
  all survive and stack.
- **Snap or bleed.** Entering slow space fast either clamps to the new cap
  or loses the excess with a half-life, `excess · 2^(−dt/h)`, which is frame
  rate independent like the integrator itself.
- **Below 1 a medium only tightens.** A ram's aftermath in a field still
  drops to the ship's own cap first. Above 1 (a lane), the medium *is* the
  cap. One flag (`carriesExcess`) lets a ship leaving a lane keep its speed
  to bleed, rather than snap down first. That was a bug found by a probe.

### W3. Chunks that come back the same — *built*

**Concepts:** Five times the radius is 25 times the rocks, about 1.5 GB. Make
rocks only round the view, and make a chunk again exactly as it was.

- **Deterministic per-cell scatter is half of it.** A cell's rock depends
  only on its cell's hash. What *keeps* it depends on coarser rocks within
  one coarse cell, and theirs on coarser still: a chain as long as the
  layers. So `scatterPart` scatters a margin of `layers × (2·maxR + gap)`
  round the chunk and keeps the rocks inside it. Tested against whole-field
  scatters item for item; the **control**, with no margin, differed, so the
  test could fail.
- **Identity by origin.** A rock is its field and the exact spot scatter gave
  it (deterministic floats compare exactly). That key carries what must
  survive a drop: broken, damage, ore. Rocks only pushed spring home anyway.
- **A budget and hysteresis.** Chunks are made nearest first within 4 ms a
  frame (all at once on a round's first frame, or after a jump), and dropped
  a chunk further out than they are made, so a view wobbling on an edge
  doesn't make and drop the same chunk each frame.
- **Which rules need a thing decides where it must exist.** Weapons stop only
  at cores, sight walks the paint mask, ships bump only cores: no rule needs
  a field rock out of view. So cores and single rocks are always loaded, and
  field rocks need only exist where they are seen.
- **A lookup that scales with the area's detail.** `level5` has 1,400
  stamps; "is this point painted" became a bucket query over nearby stamps,
  with the same answer (0 of 300,000 points disagreed).

**Code:** `scatterPart` and `dependencyReach` in `engine/scatter` · `stream`,
`FieldArea`, `kept` in `src/gameLayer/asteroids.cpp`

### W4. A current, in the exact integrator — *built*

- **A push is just more acceleration.** Momentum integrates
  `dv/dt = a − drag·v` in closed form; a current adds to `a`, so the solution
  still holds and lanes behave the same at any frame rate.
- **Anisotropic speed from one number.** A lane's top speed is
  `1 + (speed − 1)·alignment²` times the ship's own, with alignment how well
  its motion lines up with the lane: fast along, ordinary across. Plus a
  damping of the speed across it, so a ship entering at an angle is turned
  in.
- **A current carries what is still.** The first capture found the player
  swept off the start: a lane began there. A level fix (lanes start at a
  clearing's edge), not a rule change.
- **The look reuses the renderer's pieces.** Streaks are vertex-coloured
  triangles on a white pixel, a 3×3 grid bright only at its middle, so
  interpolation draws the fade (no fourth gradient texture: see 14).
  Their intensity is 1.4, above FinalGlow's threshold, so they bloom: 14's
  "values above 1 are load-bearing", on purpose.

### W5. Hiding a teleport — *built*

- **Move at the whitest frame.** The transit raises the CRT's white and the
  hull's stretch, moves the ship at the peak, then lowers them. A cut nobody
  can see is not a cut.
- **Pay for the discontinuity while it is hidden.** The far end's rocks are
  all made on the next frame (`streamAllNext`), behind the white; the
  camera's existing chase leash snaps it after the ship.
- **Emergent following.** A searching enemy flies to where it lost the
  player: the gate. So "chasers follow" needed only a timed window at that
  gate, not new AI.

### W6. A render target as memory — *built*

**Concepts:** A map of what has been seen, built by drawing the player's
sight into a target every frame and never clearing it.

- **A target keeps what was drawn** (10) until it is cleared, so
  accumulation is free: the explored map is "draw the fan there, each
  frame".
- **Its own camera, from the projection.** For a target `W` pixels square
  over a world square of side `S` centred on `c`: zoom `= W / S`, camera
  position `= c − W/2`. Derived from the projection in 5, not guessed.
- **`flushFBO` takes everything pending.** So the map is drawn at the start
  of the frame, before the first draw, or the world would go into it too.
- **Texture coordinates as the mask, in world space.** S3 used geometry as a
  mask on screen. Here the fan is drawn into the map *textured with a base
  map* made once per round (paint, open space, lanes), its UVs being world
  positions. The target builds up the real map exactly where the player has
  looked, and stays transparent elsewhere: masking with no mask texture.
- **UVs are top-down.** Pixel rows, screen space and texture coordinates
  all run the same way here, so the base map's first row is the world's
  top. Checked by a capture: the trail lies where the ship flew.

**Code:** `src/gameLayer/explorationMap.cpp` · `src/gameLayer/laneLook.cpp` ·
`src/gameLayer/jumpGates.cpp` · `src/gameLayer/lanes.cpp` ·
`Medium` and `through` in `src/engine/movement.cpp`

---

## Text, menus and controls: the concepts

### U1. A bitmap font — *built*

**Concepts:** Text is sprites. A `.ttf` holds outlines; the GPU draws
triangles and samples textures. So the outlines are rasterised once, at load,
into an **atlas**, and a string is one textured quad per character through the
batch that already exists. No new shader, no new pipeline.

- **An atlas plus a table.** `stbtt_PackBegin` / `stbtt_PackFontRange` /
  `stbtt_PackEnd` rasterise ASCII 32..126 into one channel of coverage and
  fill a `stbtt_packedchar` per glyph: its texel rectangle, its offset from
  the pen, and its advance. The atlas starts at 128 × 64 and doubles until
  the pack fits. One pixel of padding keeps neighbours out of each other's
  texels.
- **Coverage in alpha, white in colour.** The sprite shader is `color * texel`,
  so a white atlas with the shape in alpha makes the vertex colour the tint.
  R8 would be a quarter of the memory, but the renderer only makes RGBA8 and
  the atlas is a few kilobytes.
- **Layout is a pen on a baseline.** Lines are laid out from the font's
  **ascent** (baseline to the top) and **descent** (below it, negative); line
  height is their difference plus the line gap. Each glyph is placed at
  pen + offset and moves the pen by its advance. Measuring a string is the
  same walk without drawing, which is what anchoring (centre, right) needs.
- **A pixel font has a grid, and staying on it is the whole job.**
  ProggyClean at 13 px rasterises to coverage that is only ever 0 or 255,
  with no partial texels (checked). Its outlines sit on whole pixels at that
  size. To keep that on screen, three things have to line up:
  1. **Nearest sampling.** Linear would blend each texel with its neighbour
     at every scaled edge.
  2. **Whole-number scale.** At 2.5 some font pixels cover 2 screen pixels
     and others 3.
  3. **Whole-pixel origin.** `renderText` rounds each line's start after
     anchoring.

  No oversampling either: `stbtt_PackSetOversampling` buys sub-pixel placement
  under linear filtering, which is the opposite of what a pixel font wants.
- **Why not SDF here.** A distance field rebuilds a smooth edge at any scale,
  but that rounds a pixel font's square corners. It is the right tool for a
  smooth font at many sizes, and stays open for one.
- **Screen space for world text.** Damage numbers are anchored to world
  points but drawn under a screen camera, placed with the view rectangle. The
  world camera's zoom would scale the font by a non-whole amount.
- **The batch makes text cheap.** Every glyph shares the atlas texture, so
  consecutive glyphs extend one draw run. The HUD's text added one run to the
  frame (44 → 45 in the paused capture, with damage numbers on screen).

**How it was checked:** the atlas's coverage histogram, outside the game,
using the same pack calls: 1355 texels at 255, 0 partial. Two offscreen
2560 × 1440 captures: the hold line under the energy bar, and PAUSED with
numbers frozen mid-rise. The numbers came from temporary synthetic hits
(since removed): three hits on one target merged into one "30", and one
under the fog was hidden.

**Code:** `wgpu2d::Font`, `measureText` and `Renderer2D::renderText` in
`include/render/wgpu2d.h` / `src/render/font.cpp` ·
`src/gameLayer/textLook.cpp` · `src/gameLayer/damageNumbers.cpp` ·
`hud::showHaul` in `src/gameLayer/hud.cpp` · `resources/fonts/`

### U2. An immediate-mode menu — *built*

**Concepts:** The menu runs as code, not as a tree of widgets. Every frame
the game lists the rows in order, and each call draws its row and says whether
it was chosen.

- **Immediate mode keeps almost no state.** `wgpu2d::Menu` holds only:
  - the selected row
  - last frame's row count, so up and down can wrap
  - last frame's height, so the panel behind can be drawn before the rows
  - which slider is being dragged

  Pages are just which list of calls the game makes. Each page keeps its own
  `Menu`, so coming back from Settings lands on Settings.
- **One rectangle, drawn and tested.** A row is laid out, hit-tested
  against the pointer and drawn in one call. The rectangle the mouse is
  tested against is the rectangle on screen, and there is no second copy to
  fall out of step.
- **Actions, not keys.** `MenuInput` is up, down, left, right, confirm and
  back, plus a pointer. The game maps keys onto it and converts the cursor
  from window coordinates into framebuffer pixels. They differ on a
  high-density display. The library never sees a key code, so it would work
  with a pad unchanged.
- **The frame that opens the menu must not feed it.** Escape pauses at the
  top of a frame, and the menu draws at the bottom of the same frame. Fed
  that frame's Escape, the menu would close itself immediately. So its first
  frame draws without input.
- **Act on a choice at the top of the next frame.** `flushFBO` takes every
  pending quad (W6). A restart run mid-draw starts features that draw into
  their own targets, and the menu's quads would go into a map. The choice
  is stored and acted on next frame, where restarts already happen.
- **Two owners for one knob.** The CRT's strength is the tuning file's. The
  player's setting is a multiplier on top, kept in its own file. Neither one
  overwrites the other.

**How it was checked:** two offscreen 2560 × 1440 captures, driven by
temporary injected actions (since removed):
- Escape, then down, down: SETTINGS highlighted.
- Then confirm, down: the Settings page with CRT highlighted and both
  sliders full.

A third run loaded a `settings.cfg` with `crt 0`. The picture came out flat,
and the frame used one pipeline fewer, because the CRT pass was skipped.

**Code:** `wgpu2d::Menu`, `MenuInput`, `MenuStyle` in `include/render/wgpu2d.h`
/ `src/render/menu.cpp` · `src/gameLayer/menu.cpp` ·
`src/gameLayer/playerSettings.cpp` · `gameState::resume` ·
`crt::setPlayerStrength` · `menuChoice` / `triggerHeldOver` in
`src/gameLayer/gameLayer.cpp`

### K1. An action table — *built*

**Concepts:** The game asks for an action ("is Fire held"), not for a key.
The table in between is what makes rebinding, second bindings, and hints
that name their key possible without touching the code that reads input.

- **Input through a Source.** `engine/actions` never sees a window library.
  The application hands it plain function pointers (is this key held,
  pressed, released; this mouse button; the wheel; a key's name). Plain
  pointers rather than `std::function`, so a Source is a value and costs
  nothing to call.
- **A wheel is not a button.** It has no up or down, only a frame it moved.
  So it is held, pressed and repeated on that frame, never released, and
  `steps` sums its notches. A trackpad sends fractions, and the weapons'
  accumulator still turns those into whole steps.
- **Most specific wins.** Ctrl + wheel zooms, and the plain wheel cycles
  weapons. Rather than each feature checking for the other's modifier, a
  plain binding is shadowed while any binding on the same input has its
  modifier held. It is the rule most input systems use, and it removes a
  cross-feature dependency (`weapons` used to check for Ctrl).
- **Separate actions for the same keys.** The menu's Up is not flying's
  Forward, though both default to W. Rebinding one should not move the other.
- **What did not move: who owns the input.** ImGui's capture checks stay at
  each call site. With keyboard navigation on, `WantCaptureKeyboard` is true
  whenever the debug panel is focused, not only when typing. A blanket rule
  would have changed how the game plays while tuning.

**How it was checked:** a test of the engine against a fake input layer
compiled outside the game. It checked every combination of Ctrl and the
two wheel axes against the old hand-written rule, plus held, pressed and
released, shadowing, names, and out-of-range actions.

**Code:** `include/engine/actions.h` / `src/engine/actions.cpp` ·
`platform::actionSource` / `buttonName` in `src/platform/platformInput.cpp` ·
`src/gameLayer/controls.cpp`

### K2. Rebinding — *built*

**Concepts:** Rebinding is three small problems: waiting for a key without
taking the wrong one, a conflict rule, and saving only what changed.

- **Wait for silence, then listen.** The Enter or click that opened a wait
  is still down the next frame. The wait arms only once nothing is held,
  then binds the first press. The frame it ends feeds the menu nothing, or
  the cancelling Escape would also be read as Back.
- **An empty slot is a binding.** `code` −1, skipped by every query. A
  conflict clears a slot, and the page blocks leaving until it is filled,
  so the empty state is visible and short-lived.
- **Save the difference.** Only slots that differ from the defaults are
  written. A default changed in code still reaches every player who left
  that action alone.
- **Who has the input follows the device.** A key belongs to the debug panel
  only while text is typed into it (`WantTextInput`), a mouse button or the
  wheel only while the pointer is over it (`WantCaptureMouse`). This is
  decided in the table's `Source`, so it moves with a rebinding.
- **A table in an immediate-mode menu.** `fields` keeps one column index
  across rows, so up and down move straight down a column.

**Code:** `controls::bind` / `write` / `read` / `firstPressed` ·
`Menu::fields` / `note` in `src/render/menu.cpp` · the Controls page and
`listen` in `src/gameLayer/menu.cpp`

### H1. A speech bubble — *built*

**Concepts:** A bubble is a box and a tail that read as one shape. That
comes from the order things are drawn in, not from a new primitive.

- **Frame, then fill.** The frame colour is drawn as a full rectangle and a
  full triangle, then the fill over both, inset by one font pixel. The
  tail's fill reaches back into the box, covering the frame between them,
  so no seam shows. That only works with an opaque fill: a translucent one
  would let the frame show through.
- **Where the tail leaves the box.** The line from the box's centre to the
  target crosses the box's edge. Whichever of `half.x / |d.x|` and
  `half.y / |d.y|` is smaller says which edge. The base then slides along
  that edge to stay clear of the corners.
- **Offset to the edge, not the centre.** A box's reach along a direction
  is `|d.x|·half.x + |d.y|·half.y`. Placing the centre that much further
  out keeps a wide box off its own tip. A capture found that the first
  version covered it.
- **Off screen is a clamp onto a rectangle.** The same ray-to-rectangle
  sum as the gate's chevron puts the tip on the screen's inset edge.
- **Markup instead of a layout engine.** `[4]` and `[mouse:left]` are runs
  in a line, each with a width. Laying a line out is adding widths.
- **The HUD's geometry in one place.** To end a tail on a slot, the hint
  needs the slot's rectangle. The drawing and `elementRect` share one
  layout function, so the two cannot disagree.

**Code:** `drawCallout` / `measureCallout` / `renderMarkup` in
`src/render/callout.cpp` · `hud::highlight` / `elementRect` /
`drawHighlights` · `src/gameLayer/hints.cpp`

### H2. A script of steps — *built*

**Concepts:** A tutorial is a sequence with conditions. Split it into the
part that sequences, the part that reads the game, and the part on disk.

- **The sequence knows nothing.** `engine/sequence` is told each frame
  whether the step's condition holds and whether it was skipped, and says
  what happened. One event per frame, and Entered always comes first, so a
  step that measures from its start ("kills since") takes its baseline on
  the right frame, even when its condition already holds.
- **Linger, but not on a skip.** A finished step stays up a moment, marked
  DONE, so it is seen to go. A skipped one goes at once: the player asked.
- **Conditions as words.** The level keeps `until hold 0.5` as words, and
  only `hintScript` knows what they mean. The file round-trips exactly, the
  editor edits it as text, and a word from a newer build waits for a skip
  instead of breaking the level.
- **Escaping is a decision.** `std::quoted` unescapes every backslash, so a
  hand-typed `\n` would have come back as `n`. Escaping only `"` keeps the
  file as it is typed. A round-trip test found it, in its first run.
- **Input order inside a frame.** The script runs before the trigger is
  read, so a click on its box can claim the button. The same idea as the
  menu's "a choice lands next frame", applied in the other direction.

**Code:** `include/engine/sequence.h` / `src/engine/sequence.cpp` ·
`HintStep`, `readQuoted` / `writeQuoted` in `src/gameLayer/level.cpp` ·
`src/gameLayer/hintScript.cpp` · `hintsPanel` in
`src/gameLayer/levelEditor.cpp`

### I1. A packing grid — *built*

**Concepts:** An inventory where things have shapes is a grid of ids. Every
question it is asked is a loop over a shape's squares.

- **A shape is a list of squares**, and a quarter turn clockwise on a y-down
  grid is `(x, y) → (−y, x)`. Turning moves the squares off the origin, so
  every turn is followed by sliding the smallest x and y back to 0. Four
  turns then come back exactly to where they began, which the test checks.
- **A grid of ids, not of pieces.** Each square holds the id of the piece on
  it. `fits` is a lookup per square, `remove` is clearing an id, and
  moving a piece is `fits` with its own squares counted as empty, then
  remove and place.
- **First fit in reading order**, every turn at each square: what an
  automatic pickup will use. It is not optimal packing, which is NP-hard
  in general. It is predictable, and a player can repack by hand.
- **An item is a kind, not a copy.** The stash holds "heavy with stun", and
  the numbers come from the tuned kind when it is equipped. Tuning stays
  live, and a saved item will be a few words.
- **Positions that survive gaps.** An empty slot is a weapon marked empty,
  not a shorter list, so key 3 means slot 3 whatever is missing.

**Code:** `include/engine/hold.h` / `src/engine/hold.cpp` ·
`src/gameLayer/inventory.cpp` · `weapons::refit` and `Weapon::empty`

### I2. Dragging between containers — *built*

**Concepts:** Drag and drop is a small state machine over two kinds of
place: the slots and the grid. Most of the work is making a move all or
nothing.

- **Pick up, carry, resolve.** A press over a piece starts a drag, which
  remembers where it came from and how it is turned. While the button is
  held, nothing is moved, only drawn. The release decides: hold, slot,
  panel or outside. The model changes once, on the release.
- **Hold it by its middle.** The square nearest the shape's centroid is
  kept under the pointer, and stays so when a turn changes the shape. The
  target is that square's cell minus its offset in the shape.
- **All or nothing.** A move into a slot first takes the dragged weapon out,
  then tries to put the slot's weapon where it was. If that fails, the first
  is put back exactly and the move is refused. Nothing is left half-done.
- **Two flags, not one.** "The round is being played" and "the player has
  their hands on the ship" were one flag until the menu. Splitting them
  kept the burn, the gates and death running while the inputs stopped.
- **Thrown-out ore has a memory.** An orb dropped from the hold waits until
  the ship has been out of its reach once. Without that, the room just
  freed pulls it straight back.

**Code:** `src/gameLayer/loadoutMenu.cpp` · the moves in
`src/gameLayer/inventory.cpp` · `wgpu2d::GridView` in `src/render/menu.cpp` ·
`piloting` in `src/gameLayer/gameLayer.cpp`

### I3. Crates: a second container — *built*

**Concepts:** A crate is the same grid as the hold with a different rule
for shapes. The menu learns a third place things can come from and go to.

- **One grid, two rules.** The crate is an `engine/hold` grid where every
  piece is one square, so a heavy laser takes a square in a crate and six in
  the ship. The grid does not know; the caller chooses the shape it passes.
- **Hidden information as a flag on the thing.** An unknown weapon knows
  what it is all along. Only the drawing hides it, until a drag passes over
  the player's things. The reveal is one bool, set on the crate's thing so
  it stays known.
- **Weighted picks and independent rolls.** The kind is one draw against
  the sum of the weights. Each modifier is its own chance. Checked over
  2000 rolls against what was tuned.
- **Every source, every target.** Hold, slots and crate are each a source
  and a target. Each drop is a pair, written out once, with the refusals
  that leave everything where it was.

**Code:** `src/gameLayer/crates.cpp` · the crate's half of `drop` in
`src/gameLayer/loadoutMenu.cpp` · `parseGun` / `writeGun` in
`src/gameLayer/level.cpp` · the Crate tool in `src/gameLayer/levelEditor.cpp`

### I4. A screen outside the round — *built*

**Concepts:** The hub is the game not running. It is an early return at the
top of the frame, like the editor's, with its own small picture.

- **Outside the round, not paused in it.** A pause stops the clock but keeps
  the round. The hub replaces the round: `hubFrame` draws the background
  and the hub and returns, so nothing else runs. LAUNCH builds a new round,
  through the same restart every round uses.
- **Where the flow branches.** The state machine says "restart now" when an
  extraction's white has filled the screen, and when a death's switch-off
  has. Which one it was is read just before, and only extraction goes to
  the hub.
- **Staging, then a flag.** The hub fills the slots from the stash, and the
  player repacks. Launching sets one flag so the next round keeps that
  loadout instead of re-equipping the last one. A death restarts without
  the flag, as it always did.
- **One look for things.** Colours, names and tiles moved into `itemLook`, so
  the loadout menu and the hub cannot drift apart.

**Code:** `src/gameLayer/hub.cpp` · `hubFrame` / `openHub` and the
extraction branch in `src/gameLayer/gameLayer.cpp` · `stageLast` /
`launch` in `src/gameLayer/inventory.cpp` · `src/gameLayer/itemLook.cpp`

---

## What comes next

Planned work — refactors, guide chapters still worth doing, and features that
would deepen what is here — lives in [`roadmap.md`](roadmap.md), not in this
file. The two have different lifetimes: a roadmap item is deleted when it lands,
and a milestone block appears here in its place. This file only ever grows, and
only ever describes things that exist.

---

## File cheat sheet

| File | Role |
|---|---|
| `include/render/wgpuContext.h` | platform: init / begin / end / shutdown |
| `include/render/wgpuFrame.h` | other render TUs: device, pass, texture bind groups |
| `include/render/wgpu2d.h` | gl2d-shaped game API |
| `src/render/wgpuContext.cpp` | instance → batch flush |
| `src/render/wgpuImgui.cpp` | ImGui geometry |
| `resources/shaders/quad.wgsl` | sprites |
| `resources/shaders/imgui.wgsl` | UI |
| `src/platform/glfwMain.cpp` | window + frame bracket |
| `src/render/webgpuImpl.cpp` | the one TU that defines the wrapper's bodies |
| `include/render/wgpuMetalLayer.h` / `.mm` | macOS: pin the `CAMetalLayer` color space |

## Reference shelf

Guide appendices that matter for a native wgpu-native port, none of them part of the milestone path: [Debugging](https://eliemichel.github.io/LearnWebGPU/appendices/debugging.html) · [Custom extensions with wgpu-native](https://eliemichel.github.io/LearnWebGPU/appendices/custom-extensions/with-wgpu-native.html) (what `wgpu.h` beside `webgpu.h` is for) · [Memory model](https://eliemichel.github.io/LearnWebGPU/appendices/memory-model.html) · [References](https://eliemichel.github.io/LearnWebGPU/appendices/references.html)
