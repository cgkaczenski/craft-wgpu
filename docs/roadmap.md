# WebGPU render port roadmap

Port ourCraft's renderer from OpenGL 4.3+ (with bindless textures) to WebGPU
(wgpu-native on Metal), keeping GLFW for windowing, so the game runs on macOS.
Second port after [space-game-wgpu](../../space-game-wgpu); reuse its stack and
its lessons:

- [WebGPU-distribution](https://github.com/eliemichel/WebGPU-distribution) (CMake, fetches wgpu-native)
- [glfw3webgpu](https://github.com/eliemichel/glfw3webgpu) for surface creation
- [WebGPU-Cpp](https://github.com/eliemichel/WebGPU-Cpp) for the C++ wrapper
- `wgpu2d` from space-game-wgpu for the 2D menu and HUD layer
- [LearnWebGPU](https://eliemichel.github.io/LearnWebGPU/index.html) for the 3D concepts space-game never needed

Line numbers refer to the tree at commit `503d5b3`. Drafted 2026-11-03.

## Why a port, not an OpenGL fix

macOS stops at OpenGL 4.1. The world renderer needs:

| Feature | Requires | Where |
|---|---|---|
| Bindless textures (`glGetTextureHandleARB`, `sampler2D(handle)` in GLSL) | `GL_ARB_bindless_texture`, never on Apple | 22 call sites, 7 files; `defaultShader.frag` line 2 |
| Shader storage buffers | GL 4.3 | 53 uses; chunk vertex data, lights, texture handles |
| `glMultiDrawElementsIndirect` | GL 4.3 | Opaque chunk draw, `renderer.cpp:3303` |
| DSA and immutable storage (`glCreateBuffers`, `glNamedBuffer*`, `glBufferStorage`) | GL 4.4–4.5 | About 20 uses |
| `#version 430` shaders | GL 4.3 | 27 of 43 shader files |

`installAppleGLFallbacks()` (`glfwMain.cpp:405`) stubs these out so the
program runs without crashing. The menus draw; the world compiles to nothing.
Getting OpenGL to work here would mean rewriting the renderer for 4.1 and then
rewriting it again for WebGPU.

## Working rules

Carried over from space-game-wgpu; they worked.

- One milestone per session. Stop and wait after each.
- Explain the concepts and the API shape before writing code. Assume a graphics
  beginner who now knows 2D WebGPU but not 3D.
- Never write wgpu API calls from memory. Read the vendored `webgpu.h`,
  `wgpu.h` and `webgpu.hpp` in this repo's `build/_deps/`.
- When something is broken, list candidate causes before proposing a fix.
- Gameplay, world generation, networking and chunk meshing stay untouched.
  Only the render layer and its GL-touching call sites move.
- Work on a branch (`webgpu-port`). Never commit unless asked.
- Keep the OpenGL path compiling behind the CMake option. It is the only
  reference for what the world should look like (see open question 1).

## Inventory

### Where OpenGL lives

GL calls per file (`gl[A-Z]…(` matches):

| File | Lines | GL calls | What it does |
|---|---|---|---|
| `src/gameLayer/rendering/renderer.cpp` | 6659 | 676 | Everything 3D: chunk passes, entities, shadows, post-processing, FBOs |
| `src/gameLayer/rendering/skyBoxRenderer.cpp` | 914 | 130 | Atmosphere, HDR→cubemap, IBL convolution and prefilter, sun |
| `src/gameLayer/blocksLoader.cpp` | 2482 | 103 | Loads every block texture and makes it bindless |
| `src/gameLayer/rendering/shader.cpp` | 350 | 51 | GLSL compile, uniform and SSBO lookups |
| `src/gameLayer/rendering/model.cpp` | 1115 | 39 | assimp models, bindless model textures |
| `src/gameLayer/rendering/chunk.cpp` | 2677 | 34 | Per-chunk transparent buffers |
| `src/gameLayer/rendering/bigGpuBuffer.cpp` | 185 | 29 | One arena buffer holding every opaque chunk face |
| `src/gameLayer/rendering/sunShadow.cpp` | 73 | 19 | Cascaded shadow map textures, bindless |
| `src/gameLayer/gameLayer.cpp` | 760 | 15 | Texture params at load (93–134), `glClear` and state at frame start (488–492) |
| `src/platform/glfwMain.cpp` | 739 | 12 | Context hints (471–479), glad, Apple fallbacks, ImGui GL3 backend |
| `src/gameLayer/gamePlayLogic.cpp` | 3318 | 8 | Player skin upload and bindless handle (171–181), `glViewport` (347) |
| `src/gameLayer/multyPlayer/createConnection.cpp` | 1346 | 7 | Other players' skins, bindless (755–764) |
| `src/gameLayer/profiler.cpp` | 407 | 7 | GPU timer queries, debug groups |
| `src/platform/errorReporting.cpp` | 64 | 6 | GL debug callback |

Out of scope: `PBRTool/` and `noiseTool/` are separate GL apps. Turn them off
in CMake on the WebGPU path.

### The renderer interface the game calls

From outside `src/gameLayer/rendering/`:

- **Lifecycle.** `renderer.create`, `reloadShaders`, `sunShadow.init`,
  `gyzmosRenderer.create`, `pointDebugRenderer.create`.
- **Assets.** `blocksLoader.loadAllTextures` / `clearAllTextures` /
  `setupAllColors` / `loadAllItemsGeometry`, `modelsManager.loadAllModels` /
  `clearAllModels`, `renderer.recreateBlocksTexturesBuffer`,
  `renderAllBlocksUiTextures` (renders 3D block icons into textures for the UI).
- **World frame.** `renderFromBakedData` (one call, about 1800 lines; it calls
  `renderShadow` and `renderEntities` itself), `renderDecal`,
  `gyzmosRenderer.render`, `pointDebugRenderer.renderCubePoint`.
- **Debug lines.** `gyzmosRenderer.drawLine` (37 sites), `drawCube` (9).
- **2D.** gl2d `renderRectangle`, `renderText`, `renderCircleOutline`,
  `pushCamera` / `popCamera`, `getViewRect`, `getTextSize`. About 150 call
  sites, mostly `UiEngine.cpp`, `renderSettings.cpp`, `mapEngine.cpp`. glui
  (112 sites) is layout math only, as in space-game.

This is the seam. A WebGPU renderer that implements these methods lets the
game code stay as it is.

### How a frame works today

`Renderer::renderFromBakedData` (`renderer.cpp:2598`), in order:

1. Frustum culling and chunk sort (CPU, 2846).
2. Sky box into `fboMain` (2912).
3. **Depth prepass** with `zpassShader`, then the **solid pass** with
   `glDepthFunc(GL_EQUAL)` (3371–3394). Opaque chunks are one
   `glMultiDrawElementsIndirect` (3303).
4. Sun drawn alone, then mask and radial blur for **god rays** (3475–3532).
5. Entities (3561).
6. **Water and transparency** with depth peeling: water-only depth into
   `fboCoppy`, first transparent layer, copy color and depth, last transparent
   layer (3586–3659). Transparent chunks are one `glDrawElementsInstanced` each.
7. **SSR** (3698).
8. **Automatic exposure**: mipmap the HDR color down to 1×1, then
   `glReadPixels` into a PBO and read it a frame later (3755–3790).
9. **Bloom**: filter, downsample chain, Gaussian blur ping-pong, add mips (3843–3999).
10. **Post**: HBAO and apply, apply bloom, underwater warp, tone map, FXAA to
    the default framebuffer (4019–4248).
11. **Lens flare**, gated by sun occlusion queries (`sunFlareQueries[3]`, 3462) (4257).

`renderShadow` (5466) renders three cascades plus a sun shadow separately.

### Render targets

`fboMain` (`renderer.cpp:1737`) has five color attachments plus depth:

| Slot | GL format | Contents | WebGPU format |
|---|---|---|---|
| 0 | `R11F_G11F_B10F` | HDR color | `rg11b10ufloat`, renderable only with feature `RG11B10UfloatRenderable`; else `rgba16float` |
| 1 | `RGB16F` | View-space positions | `rgba16float` (no 3-channel targets in WebGPU) |
| 2 | `RGB16UI` | Normals | `rgba16uint` |
| 3 | `R11F_G11F_B10F` | Bloom input | as slot 0 |
| 4 | `GL_RED` | Material (roughness) | `r8unorm` |

Check this against `maxColorAttachmentBytesPerSample` (default limit 32)
before milestone 11. Other targets: `fboCoppy`, two god-ray targets,
`fboLastFrame` and `fboLastFramePositions` (history for SSR), `fboHBAO`,
`fboSkyBox`, two bloom ping-pong targets.

### Chunk geometry

Each visible block face is one 16-byte **instance**: `short` orientation,
`short` texture index, `ivec3` position (`bigGpuBuffer.cpp:5`, divisor 1). The
four corners come from a 4-entry `GL_UNSIGNED_BYTE` index buffer drawn as
`GL_TRIANGLE_FAN`. The vertex shader reads corner offsets and UVs from storage
buffers (`u_vertexData`, `u_vertexUV`) using orientation and `gl_VertexID`.
Camera precision trick: `u_positionInt` + `u_positionFloat`, so positions stay
camera-relative integers. This design maps well onto WebGPU.

### Textures

About 6.6k PNGs under `resources/assets/blocks`, in mixed sizes: 4717 at 64×64,
1417 at 128×128, 296 at 16×16, plus tall animation strips (16×512, 64×256,
128×384, …). Each block has up to four maps (albedo, normal, material,
parallax). Every map gets a bindless handle written into the
`u_textureSamplerers` storage buffer; the fragment shader indexes that buffer
and samples through the handle. Skins, models, shadow cascades and a white
fallback texture use bindless the same way.

## What changes going to WebGPU

Things WebGPU does not have, each with what replaces it:

| OpenGL | WebGPU |
|---|---|
| Bindless handles | `texture_2d_array` per size bucket plus an index table, or `binding_array<texture_2d<f32>>` (wgpu-native feature `TextureBindingArray`) |
| `GL_TRIANGLE_FAN`, `GL_UNSIGNED_BYTE` indices | Triangle strip (4 vertices, reordered) with no index buffer; corner from `vertex_index` |
| `glMultiDrawElementsIndirect` | wgpu-native `MultiDrawIndirect` feature, or a loop of `drawIndirect`; `firstInstance` in indirect args needs `IndirectFirstInstance` |
| `glGenerateMipmap` | CPU mips at load, or a render-pass mip chain |
| `glReadPixels` into a PBO | `copyTextureToBuffer`, then `mapAsync` a frame or two later |
| `glBlitFramebuffer`, FBO copy helpers | `copyTextureToTexture` (same format; depth copies whole subresources only) or a fullscreen pass |
| `glDepthFunc` and blend toggles mid-pass | Baked into each render pipeline; one pipeline per state combination |
| Loose `uniform` variables (dozens per shader) | Uniform buffer structs with WGSL alignment rules (`vec3` pads to 16) |
| GL timer queries, occlusion queries | Query sets (timestamp needs feature `TimestampQuery`) |
| GLSL 430 | WGSL, written by hand; wgpu-native also accepts GLSL through `WGPUShaderModuleGLSLDescriptor`, but not with bindless |
| Clip-space depth −1..1, bottom-left texture origin | Depth 0..1, top-left origin |

The last row needs the most care. Projection matrices need
`GLM_FORCE_DEPTH_ZERO_TO_ONE` (or the `_ZO` glm functions), and every
screen-space shader that rebuilds positions from depth will be quietly wrong
until it is updated. That includes SSR, HBAO, god rays, fog and
`defaultShader.frag:1928`. Decide the convention in milestone 5 and write it
down.

The feature names above come from space-game-wgpu's vendored headers. Re-check
them against this repo's headers once milestone 1a fetches them.

## Milestones

Each milestone is one session and ends with a visible result. Type follows
space-game: _Learning_ is new graphics concepts, _Game-specific_ is wiring
ourCraft. Phases B–D are where the new 3D concepts are.

### Phase A: a WebGPU app that reaches the world

| # | Milestone | Ends with | Type |
|---|---|---|---|
| 1a | Deps and adapter | `RENDERER_WEBGPU` CMake option (default OFF) with WebGPU-distribution, glfw3webgpu and WebGPU-Cpp, wired as in space-game. GL-only tools (`PBRTool`, `noiseTool`) off on that path. Adapter and supported features printed, including the ones in the table above. | Mixed |
| 1b | Clear color | Device, surface, resize, sRGB `CAMetalLayer` pin, all lifted from space-game. Window shows a solid color. | Game-specific |
| 2 | Null GL scaffold | On the WebGPU path, glad's function pointers are filled with generated do-nothing stubs (`tools/genNullGL.py` → `src/platform/nullGLStubs.inl`, overrides in `nullGL.cpp`), so all existing GL code runs as a no-op. ImGui runs input-only. `OURCRAFT_WORLD=<world>` boots straight into a world. Game boots, hosts its server, generates and meshes chunks, and runs its loop with a cleared screen. GL path unchanged. | Game-specific |
| 3a | Menus on wgpu2d | `wgpu2d` vendored in `thirdparty/wgpu2d` (space-game `71d8666`) and owning the device and frame. A shim `gl2d/gl2d.h` makes `gl2d` an alias for `wgpu2d` for the game and glui; gl2d compatibility in `wgpu2d.h` (gl2d text and fonts, 9-patch, `Camera::follow`, `createFromFileData`, `bind`, zero-filled `readTextureData`). `WGPU_SCREENSHOT_FRAME=N` saves a frame. Title menu and in-world HUD draw. | Game-specific |
| 3b | 2D parity | Real `readTextureData` (GPU readback, `CopySrc` on sprite textures) so block colours and skins sent over the network are right. Both builds save screenshots (`WGPU_SCREENSHOT_FRAME`; the GL one is taken before ImGui) and `OURCRAFT_INPUT` scripts clicks and keys to reach menus. Compared against GL: title, settings and rendering settings match pixel for pixel except the background photo's mip edges (odd-sized image; Apple's `glGenerateMipmap` filters differently); the in-world HUD matches. Fixed on the way: `createFromBuffer`/`readTextureData` take and return rows bottom-first as gl2d did (item icons were upside down), and quads mirrored by a negative size are culled as under the game's `GL_CULL_FACE` (slider knobs covered the bar's border). The inventory has no GL reference on the Mac (see open question 1); on WebGPU its block icons draw white until milestone 10. | Game-specific |
| 4 | ImGui on WebGPU | A renderer backend ported from space-game-wgpu (`src/platform/wgpuImgui.cpp`, `resources/shaders/webgpu/imgui.wgsl`) instead of upgrading ImGui: its bundled `imgui_impl_wgpu` targets an older `webgpu.h`, and 1.85 already has every API the backend uses, so the OpenGL path and the game's ImGui code stay as they are. Index buffer with `VtxOffset` as base vertex, scissors in framebuffer pixels, packed `Unorm8x4` color, font atlas as a wgpu2d texture. Debug windows draw and match the GL build pixel for pixel. Multi-viewport stays off on WebGPU (each OS window would need its own surface), and `ImGui::Image` of the renderer's GL framebuffers shows nothing until those are ported. | Game-specific |

### Phase B: an unlit world you can play

| # | Milestone | Ends with | Type |
|---|---|---|---|
| 5 | First 3D: depth and a camera | Depth texture, perspective projection in a uniform buffer, back-face culling. One hardcoded cube at the player's position, moving with the camera. **Decide depth range and Y conventions here.** | Learning |
| 6 | Chunk faces | Storage buffers read in the vertex shader, instance step mode, corners from `vertex_index` (no fans). `BigGpuBuffer` becomes one `wgpu::Buffer` arena; one draw per chunk. Faces colored by orientation (the `vertexColor` table). World shape visible. | Learning |
| 7 | Block textures without bindless | Choose between texture arrays and binding arrays. Load the 6.6k textures, generate mips, handle animation strips, and replace the `u_textureSamplerers` handle table with an index table. Textured, unlit world. | Learning |
| 8 | Indirect draws | Indirect buffers, `MultiDrawIndirect` or a loop, GPU-side draw args from the existing culling. Compare frame time against `slow-frames.log`. | Learning |
| 9 | Transparency and water | Blend states, alpha-tested leaves, the depth-peel passes, texture copies for depth. Glass, leaves and water render in the right order. | Mixed |
| 10 | Entities, models, debug lines | Players and zombies (assimp models, skins), item and block entities, decals, held item, the block-selection outline, `gyzmosRenderer` lines. | Game-specific |
| 10-parity | Playable on Mac | Walk, mine, place, open the inventory, join a local server. Screenshots against the reference (open question 1). Phase B is not done until this passes. | Game-specific |

### Phase C: lighting

| # | Milestone | Ends with | Type |
|---|---|---|---|
| 11 | G-buffer | Multiple render targets, the formats table above, depth prepass plus `Equal` solid pass. Each attachment shown in an ImGui debug view. | Learning |
| 12 | Sun, ambient, block light | Baked light levels from face data, sun and ambient, point lights from a storage buffer, PBR with the BRDF LUT. The big `defaultShader.frag` translation lands here; split it as you go. | Learning |
| 13 | Shadows | Depth-only pipelines, comparison samplers, three cascades, the sun shadow. | Learning |
| 14 | Sky | Atmospheric scattering, rendering into cubemap faces through per-face texture views, IBL convolution and specular prefilter, the sun disc. | Learning |

### Phase D: post-processing

| # | Milestone | Ends with | Type |
|---|---|---|---|
| 15 | Post chain skeleton | Fullscreen triangle, HDR target, ping-pong textures, ACES tone map, FXAA, present. Everything after this is one more pass. | Learning |
| 16 | Bloom and auto exposure | Mip chain render passes, blur ping-pong, GPU→CPU readback with `mapAsync` replacing the PBO. | Learning |
| 17 | HBAO and SSR | Screen-space passes, history textures from the last frame. Depth reconstruction checked against milestone 5's convention. | Learning |
| 18 | God rays, lens flare, underwater, color grading | Radial blur, occlusion query sets for the flare, warp, LUT grading. | Mixed |
| 18-parity | Full parity | Same seed, position, time of day and settings against the reference, pass by pass, using the ImGui debug views. | Game-specific |

### After parity

| # | Milestone | Notes |
|---|---|---|
| 19 | Decide the GL path's fate | Keep it for Windows/Linux or delete it along with glad, gl2d and the Apple fallbacks. |
| 20 | Hot reload | `reloadShaders` for WGSL, if it was dropped along the way. |

## Integration approach

space-game swapped gl2d for `wgpu2d` with a namespace alias. That still works
for the 2D layer (milestone 3). It does not work for the 3D renderer.
`renderer.cpp` changes GL state implicitly across thousands of lines, and
WebGPU wants that state declared up front in pipelines. So:

- Until a subsystem is ported, it keeps running on the **null GL scaffold**
  (milestone 2): no context, every GL call a stub. A milestone is done when
  its subsystem draws through WebGPU and no longer needs the scaffold. When
  nothing does, delete `nullGL.*`, `nullGLStubs.inl`, `tools/genNullGL.py`
  and glad.
- Write a new `WgpuRenderer` beside the old one, with the **same public
  methods** listed in the interface section. Pick between them at compile
  time with `RENDERER_WEBGPU`. Game code includes one header and doesn't know
  which it got.
- Port **behavior, not structure**. Each GL pass becomes a WebGPU render pass
  with its own pipelines, created at startup. Don't port GL state toggles one
  for one.
- Translate shaders pass by pass, only as far as the current milestone needs.
  Milestone 6's world shader is tens of lines, not 2292.
- Keep a debug view for every intermediate texture in ImGui. With no working
  reference on this Mac, inspecting each pass is how you find bugs.

## Risks

- **No reference image on macOS.** space-game could be compared side by side.
  Here the GL world never drew on this machine. Answer open question 1 before
  milestone 10-parity.
- **Size.** `src/gameLayer/rendering/` is about 15.7k lines of C++, plus about
  6k lines of GLSL. Expect around twenty sessions, versus about ten for
  space-game.
- **Texture memory.** Albedo alone for 6.6k textures at their native sizes is
  around 170 MB before mips and before normal and material maps. Bucket by
  size, resample, or load only the active texture pack. Decide in milestone 7.
- **Binding-array size.** On the Apple M6 the adapter reports
  `maxSampledTexturesPerShaderStage` 128, and wgpu-native v24's
  `WGPUNativeLimits` has no separate binding-array limit. A
  `binding_array` of thousands of block textures may not fit; texture arrays
  probably will. Check whether the device can request more before choosing in
  milestone 7.
- **Depth convention drift.** See above. A silently wrong screen-space effect
  is the most likely bug in Phases C–D.
- **Attachment byte budget.** Five MRTs with no 3-channel formats may exceed
  `maxColorAttachmentBytesPerSample`. Pack positions or reconstruct them from
  depth if needed.
- **ImGui upgrade.** The jump from 1.85 is bigger than space-game's. Docking
  API changes may touch the debug UI code.
- **Header churn.** Same as space-game: check every call against the vendored
  headers.
- **Multiplayer.** The server (`src/gameLayer/multyPlayer/server.cpp`) runs in
  the same executable and must keep working when the renderer is stubbed.
  `shared/blocks.h:5` includes `glad/glad.h`; remove that in milestone 2 so
  `shared/` has no GL.

## Open questions

1. **Reference renderer.** Is there a Windows or Linux machine with an
   NVIDIA/AMD GPU to run the GL build for reference screenshots? If not, the
   fallback is the YouTube devlogs and README images, which are good enough for
   "roughly right" but not pass-by-pass. (For 2D only, the Mac's OpenGL 4.1
   build is a reference: gl2d's shaders are `#version 330`, so menus and HUD
   draw there. Not the inventory: `renderAllBlocksUiTextures` returns before
   sizing `blockUiTextures` when its 4.3 shader fails, and opening an
   inventory that holds blocks then crashes.)
2. ~~**Share or copy `wgpu2d`.**~~ Decided in milestone 3a: copied into
   `thirdparty/wgpu2d`, free to diverge (device features, a 3D pass, gl2d
   compatibility). Fixes don't flow back to space-game on their own.
3. **Scope of Phase D.** Some effects (lens flare, color grading, god rays)
   could be deferred indefinitely once the game is playable and lit.
4. **AGENTS.md.** Copy space-game's agent rules into this repo so every
   session picks them up?
