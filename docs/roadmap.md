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
| 4 | `GL_RED` | Material (roughness) | the alpha of slot 1 |

Milestone 11 checked this against `maxColorAttachmentBytesPerSample` (32 by
default; wgpu-native v24 does not report the adapter's own): as a render
target `rg11b10ufloat` costs 8 bytes, not the 4 it stores, so GL's five
slots come to 8 + 8 + 8 + 8 + 1 = 33. The roughness moved into slot 1's
alpha, which GL's `RGB16F` never had: four targets, exactly 32. Other targets: `fboCoppy`, two god-ray targets,
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
`defaultShader.frag:1928`. Decided in milestone 5:

### Conventions (decided in milestone 5)

- **Depth: standard 0..1.** Projection is `glm::perspectiveRH_ZO` from the
  camera's fov and planes; near maps to 0, far to 1. `Depth32Float`, compare
  `Less` (where GL said `GL_LESS`; `GL_LEQUAL` becomes `LessEqual`), cleared
  to 1. Porting a shader that rebuilds a position from depth means dropping
  its `depth * 2.0 - 1.0`: WebGPU's NDC z is the stored depth. Reversed-Z was
  considered and not taken, to keep ported shaders one step from the GL ones.
  `Camera::getProjectionMatrix` stays −1..1, because the OpenGL path and the
  CPU frustum culling use it; the WebGPU renderer builds its own.
- **Clip-space y points up**, as in GL, so projections need no flip and a
  triangle counter-clockwise in GL is counter-clockwise here (`FrontFace::CCW`,
  `CullMode::Back`).
- **Render targets are top-first.** Row 0 of a framebuffer or render target is
  the top of the image in WebGPU and the bottom in GL. A screen-space shader
  turns NDC into a texture coordinate with `uv = (0.5 + 0.5*x, 0.5 - 0.5*y)`
  (GL: `0.5 + 0.5*y`), and `@builtin(position).y` counts down from the top
  (GL's `gl_FragCoord.y` counts up).
- **Sampled textures are not flipped.** In both APIs memory row 0 is v = 0.
  Upload block textures with the rows the GL path uploads (bottom-first after
  `stbi_set_flip_vertically_on_load(true)`), and every mesh UV carries over
  unchanged.
- **Positions are camera-relative.** The CPU subtracts the camera position in
  doubles and the GPU gets small floats, as GL does with `u_positionInt` +
  `u_positionFloat`; the view matrix is `Camera::getViewMatrix` (rotation only).

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
| 5 | First 3D: depth and a camera | `wgpuWorld` (`src/gameLayer/rendering/wgpuWorld.cpp`, `resources/shaders/webgpu/testCubes.wgsl`), called from `Renderer::renderFromBakedData`: a `Depth32Float` texture sized to the surface, `perspectiveRH_ZO` times the camera's view in a uniform buffer, `CullMode::Back`, and two interpenetrating test cubes near the player built from `vertex_index`. wgpu2d gained `wgpuBeginSurfacePassWithDepth`, a world pass before the 2D one. Checked: face colors show only outside faces; the nearer cube stays in front though drawn first; cube bounds in screenshots match a CPU projection to a pixel, before and after a scripted camera turn (`OURCRAFT_INPUT` gained `move@x,y` and `key@F9`). Conventions are written down above. | Learning |
| 6 | Chunk faces | Opaque faces in one growable `wgpu::Buffer` arena (first-fit allocator in faces, doubling with a GPU copy), read with instance step mode; the corner table from `recreateBlockGeometryData` in a read-only storage buffer; a 4-vertex triangle strip per face (corners 0,1,3,2, the fan's winding); one `draw(4, faces, 0, firstFace)` per frustum-visible chunk, near to far. `Chunk::sendDataToOpenGL` and `clearGpuData` feed and free the arena (GL's own `BigGpuBuffer` is switched off, so this is the first arena that runs). Faces shaded by the `vertexColor` table times a flat color hashed from the texture index (`resources/shaders/webgpu/chunkFaces.wgsl`). World shape visible: about 160-260 draws and 250k-440k faces a frame at the start of `test`; culled and unculled renders differ by 11 pixels; shutdown frees every range back to one. No animation, no alpha test (tall grass draws as solid quads), no transparent faces yet. `OURCRAFT_INPUT` gained `quit`. | Learning |
| 7 | Block textures without bindless | One 128×128 `texture_2d_array`, not a binding array: a binding array is capped at `maxSampledTexturesPerShaderStage` (128 here), while the array holds up to `maxTextureArrayLayers` (2048 on the adapter; the device now asks for it, since the default is 256). About 1,600 PNGs on disk, mostly 128×128, some 64×64 and 16×16, and tall 128×256 to 128×1280 strips; the GL shader samples the strips whole, so there is no animation to handle. The loader keeps making wgpu2d textures; nullGL's `getTextureHandle` now carries each texture's id in the handle's low 32 bits, so `BlocksLoader::gpuIds` (4 slots per block) still says which texture each slot shows. `wgpuWorld::setBlockTextures` gives every distinct texture a layer (448 layers for 588 slots in `test`; unloadable slots fall back to slot 0's default) and builds the array on the GPU (`resources/shaders/webgpu/blockTextures.wgsl`): `fill` copies each texture into its layer with a row flip (nearest enlargement, box-averaged reduction), and `downsample` makes mips 1-4 for all layers at once, in about 27 ms. A slot → layer storage buffer replaces `u_textureSamplerers`; the UV table joins the corner table. Sampler nearest, nearest mip, repeat, max LOD 4, as GL; GL's 4× anisotropy is dropped (WebGPU allows it only with linear filters). Alpha-tested cut-outs for grass, flowers and leaves. About 39 MB of GPU memory duplicates the wgpu2d copies, which the UI still uses. GL can't draw the world on this Mac to compare against, so the check is the frame itself: grass sides upright, cut-outs clean, no validation errors. Textured, unlit world. | Learning |
| 8 | Indirect draws | As GL's `glMultiDrawElementsIndirect`: the frustum-culled list becomes one 16-byte argument record per chunk (4 vertices, its faces, `firstInstance` = its arena range), written to an `Indirect` buffer each frame (one buffer, doubled when outgrown; queue order makes reuse safe), then one `multiDrawIndirect`. wgpu2d now asks the device for `IndirectFirstInstance` and wgpu-native's `MultiDrawIndirect` when the adapter has them; without `IndirectFirstInstance` the world falls back to direct draws. `OURCRAFT_WORLD_DRAWS=direct|indirect|multi` picks the path, and shutdown prints the average CPU time spent recording the chunk draws. All three give pixel-identical frames. On this Mac the difference is in the noise: ~150 chunks record in 25-30 µs with vsync, and in two uncapped runs each multi took 124-149 µs, direct 145-156 and a loop of `drawIndirect` 161-168, against a 13.3 ms frame. Metal has no native multi-draw, so wgpu loops `drawIndirect` underneath; the GPU work is the same. Frame time stayed 13.3 ms (the display's 75 Hz) even with `PresentMode::Immediate`: macOS composites windowed apps at the display rate, so `nextDrawable` waits regardless. (`slow-frames.log` is space-game's log, not a baseline here.) The indirect buffer is the place a later GPU-culling pass would write its arguments. | Learning |
| 9 | Transparency and water | Transparent faces (water, ice, glass, decals) get a second arena range per chunk (`Chunk::sendDataToOpenGL` → `wgpuWorld::setChunkTransparentFaces`), drawn after the opaque pass far to near (chunks in list order, faces as the baker sorted them). GL's water depth peel, step for step: copy depth → peel depth; a depth-only pass draws LOD-0 water into it (the nearest water surface); the peel pass draws LOD-0 transparent faces beyond that surface onto the frame; color and depth are copied (`copyTextureToTexture`, between passes); the final pass draws every transparent face blended, water reading the copies. Four pipelines replace GL's state toggles: water depth (Less, no color), peel (alpha blend, Less, back culling), final (alpha blend, LessEqual, no culling) and final without peel (`waterType` 0); a WGSL `override` picks the fragment mode. The copies are read with `textureLoad` at the fragment's pixel. Unlit adaptation: water is `u_waterColor` converted to sRGB, mixed with what is under it by a view-angle Fresnel term and darkened with depth; glass alpha rises at grazing angles as in GL. Water's surface sits 0.08 below the block top; the waves and the dudv refraction wait for animation. Leaves were already alpha-tested (milestone 7). wgpu2d gained `wgpuFrameEncoder`/`wgpuFrameTexture` and a load-depth option. Checked in a copy of `test2` (a creative world, reached by flying): pond bed visible through the water, clean edges, no validation errors. Not yet checked in a scene: glass, and the no-peel path. `OURCRAFT_INPUT` gained `hold@W,N`, `key@Space`/`key@LShift` and negative `move@` coordinates; `WGPU_SCREENSHOT_FRAME` takes a list. | Mixed |
| 10a | Block cubes, decals, debug lines | Done: the world-data half of row 10. Dropped block items are instanced 0.4-scale cubes in the opaque pass (`entities.wgsl`): a 64-byte record each (position, brightness, six face slots into the milestone 7 layer table), the 36 vertices from GL's `vertexDataOriginal`, back faces culled as GL does. GL's `pow(x, 2.2)` ambient on a linear color is just `x` on this world's sRGB output. The block-breaking crack reuses `chunkFaces.wgsl`'s vertex stage with a `zBias` override (half GL's bias, for 0..1 depth) and `fs_decal` (one of crack.png's 10 frames, v flipped: wgpu2d stores images top row first, gl2d bottom first), in a pass of its own after the world, as GL draws it. GyzmosRenderer's cubes (the block-selection outline) and lines become camera-relative line segments: WebGPU has no polygon mode, so each cube triangle's three edges are emitted, which is what GL's `glPolygonMode(GL_LINE)` shows (face diagonals included); lines are 1 px. Checked in copies of `test`: outline, crack frames growing while a block breaks, dropped cubes textured per face. `OURCRAFT_INPUT` gained `hold@Mouse,N` and digit keys. | Game-specific |
| 10b | Models and items | Done: players and creatures, the right hand, item entities and the held item. `model.cpp` and `BlocksLoader` now also hand their CPU vertex data to `wgpuWorld::setMesh`, keyed by the GL VAO name (`removeMesh` on cleanup), so the renderer's existing draw lists find their WebGPU meshes. Skinned models read the frame's bones from a storage buffer (`vs_model`, a base bone per instance); each entity's four textures (GL's bindless handles, wgpu2d ids here) form a group-2 bind group, cached per combination; one indexed draw per entity, `firstInstance` picking its record. Items (`vs_item`) carry their model matrix and brightness in a record each. Entity and item UVs are v-flipped like the crack decal. Brightness is GL's linear `v_color` taken to sRGB (`pow(c, 1/2.2)`). The world's `render()` now stops after the opaque pass so entities draw before the transparency passes (`renderTransparent()`), GL's order. Models are drawn without culling, because the hand is mirrored by a negative scale. Checked in copies of `test` and `test2`: the hand in the mage skin, the held sword and apple, a dropped apple the right way up, and a training dummy moved in front of the camera (a debug-only placement, since none stood in view): burlap head, straw arms and post, which only map like that with the right v orientation (the texture's lower half is empty). Not checked: other creatures (none in these worlds), items held in third person, multiplayer skins (`model.cpp` decodes them with stbi's global flip state). The inventory's block icons (`renderAllBlocksUiTextures`) were white: GL draws each block once into a 64x64 texture, and on WebGPU those calls were stubs that left GL names wgpu2d did not know. Each icon is now a `wgpu2d::FrameBuffer` that `wgpuWorld::renderBlockIcons` draws into (`blockIcons.wgsl`), on a command buffer of its own at load time: GL's cameras (ortho, depth remapped to 0..1), the whole entity geometry (cube, stairs, slab, wall, furniture ranges), the per-face texture slots and GL's normal lighting, alpha kept. `wgpuMarkTargetDrawn` (wgpu2d) tells wgpu2d a target drawn by an outside pass needs no first clear, which otherwise also made it read back empty. Checked: blocks, stairs, slabs and walls in the creative tab, and furniture icons read back to a PNG. | Game-specific |
| 10-parity | Playable on Mac | Walk, mine, place, open the inventory, join a local server. Screenshots against the reference (open question 1). Phase B is not done until this passes. | Game-specific |

### Phase C: lighting

| # | Milestone | Ends with | Type |
|---|---|---|---|
| 11 | G-buffer | Done. The 3D world draws into off-screen targets instead of the surface: the G-buffer (`rg11b10ufloat` HDR color, `rgba16float` view-space positions with the roughness in their alpha, `rgba16uint` packed normals, `rg11b10ufloat` bloom) and the depth. Four targets, not GL's five: see "Render targets" for the 32-byte budget. wgpu2d requests `RG11B10UfloatRenderable`. Opaque chunks: the depth prepass (`fs_depth`, alpha test, every color target `writeMask` None, GL's `glColorMask(0)`) and the solid pass (`Equal`, no depth writes) in one render pass, the shared vertex output `@invariant`; the renderer's `zprepass` off gives one `Less` pass. Cubes and entities write the G-buffer too (GL's entity shaders write all of fboMain); glass and water blend into the HDR color alone, and the peel copies it rather than the surface. The HDR color holds linear light like GL's: each world shader keeps its unlit math and converts its result with GL's sRGB curve, and the composite (`gbufferView.wgsl`, a fullscreen triangle into wgpu2d's 2D pass) converts back; the crack decal and gizmo lines still draw on the surface after it, as GL draws them after its post. The same shader draws each attachment, the roughness and the depth into small wgpu2d targets for the ImGui "G-buffer" header (client controll window), only while it is open; `WGPU_GBUFFER_VIEW=0-5` puts one on the screen instead. Normals are the face's (GL's cross product for non-cube shapes; entities by the inverse transpose, via cofactors, sign kept for the mirrored hand); normal and material maps are not loaded yet, so the roughness is GL's 0.9 and bloom is 0. Checked against a build of milestone 10b on the same frames: within 2 levels everywhere (`rg11b10` rounding) but for a gizmo line lying in a block face (a depth tie) and a few edge pixels, water and its peel included; prepass off differs by 4 pixels. Transparent faces write no positions or normals yet (SSR, milestone 17). | Learning |
| 12 | Sun, ambient, block light | Done. `defaultShader.frag`'s lit path, split three ways: `world.wgsl` (the shared uniforms, now 256 bytes, and the sRGB helpers), `lighting.wgsl` (GL's BRDF: GGX, Smith, Schlick, Oren-Nayar; point lights; the AO shapes from `ao.png`; normal-map rotation; paint) and `chunkFaces.wgsl`, which reads every texture up front (`textureSample` must stay in uniform control flow) and then shades. WGSL has no includes, so wgpu2d's shader loader now expands lines `#include "name.wgsl"`, relative to the including file, each file once. Light levels come from the face data (sky and block nibbles), with GL's ambient formula and `u_baseAmbientExtra`; the sun is GL's `mainLightPosition` with its color. Normal and material maps are the texture array's slots +1 and +2: the roughness now comes from the material map, and emissive texels feed the bloom target. Point lights: each chunk's list goes to `wgpuWorld::setChunkLights` with its faces, and each frame the visible chunks' lights, nearest first up to `maxLights`, fill a storage buffer (`lightParams.y` counts them; the `useLights` setting gives 0). Glass and water are lit the same way in `fs_blend`. The composite applies the exposure, `ACESFitted` and the sRGB curve, as `toneMap.frag` does by default. Entities keep GL's per-entity light factor. No BRDF LUT: GL samples it only under `physicallyAccurateReflections`, a false constant. Auto-exposure is held at GL's starting 1.6 and 0 bonus ambient: GL's luminance readback is a stub on this path, so updating it drifted the exposure to 2.0 (milestone 16 ports the readback and turns the update back on). Deferred: shadows (`shadows` defaults to 0, so shadow = 1; milestone 13), fog (14), SSR (17), underwater, caustics and the animated water normals (18), `showLightLevels`, dither and vignette (15). Pitfall: naga does not rename MSL reserved words, so a struct member called `fragment` fails Metal's compile ("'user' attribute cannot be applied to types"). Checked by eye on `test`, `test2` and `appletest` (sunlit tops brighter than sides, AO in corners, snow not clipped, 9-41 point lights a frame) and in the normal and roughness views; GL's capture on this Mac shows the UI over a black world, so there is no GL frame to diff against. | Learning |
| 13 | Shadows | Done. GL's three sun cascades (2048, 2048 and 1024 texels, `Depth32Float`), drawn before the opaque pass when the `shadows` setting is on (GL's default is off; `OURCRAFT_SHADOWS=0|1|2` overrides it for test runs). The matrices are renderShadow's: the camera frustum from GL depth -1 to 0.9985, 0.9995 and 0.99994, turned so the sun is +z, made a cube as long as its diagonal, then an orthographic box 500 blocks deeper both ways. `renderer.cpp`'s `sunCascadeMatrix` builds them in double from world coordinates, and `wgpuWorld` folds in the camera position (so the shader keeps camera-relative positions) and remaps depth by z/2 + 1/2, which gives exactly the numbers GL's shader compares, so its biases carry over. One deliberate difference: GL starts its box's running maximum at `-numeric_limits<double>::min()` (about -2e-308, not the most negative double), so its box always reaches back to the world origin on each axis, which spreads the first cascade over hundreds of blocks a few hundred blocks from spawn; here it starts at `lowest()`. Each cascade is a depth-only render pass (no color attachment) with its own pipeline: `vs_shadow` (the face placed by `u.shadowMatrices[shadowCascade]`, an override) and the prepass's `fs_depth`, so leaves and grass cast their cut-outs; back faces culled as GL leaves `GL_CULL_FACE`. Every loaded chunk's opaque faces are culled against each cascade's box, as GL loops over `loadedChunks`, and the records join the frame's indirect buffer. Sampling (`lighting.wgsl`'s `sunShadow`, GL's `shadowCalc2`): the cascade by view depth (16 and 44 blocks), GL's biases, and a comparison sampler (nearest, `LessEqual`, reference z - bias) through `textureSampleCompareLevel`, which needs no derivatives and so is allowed in branches and loops; off the map counts as lit (GL's border of 1; WebGPU has no border color). Hard is one tap; soft is GL's nine Vogel-disk taps turned by interleaved gradient noise, the outer five first, to the power 2.2. The shadow scales the sun term and the sky part of the ambient (up to 5 levels on sun-facing faces), on glass and water as well. A pass may not draw into a texture its bind groups also bind, so the shadow passes bind a copy of the world group with a 1x1 depth in the three map slots, and that stand-in fills them in the main group until shadows are first switched on (36 MB of maps, made then). Pitfall: naga's override pass rejected GL's noise written as a `vec3` constant with swizzles (`ExpressionAlreadyInScope`, failing every face pipeline), so it is written with literals. Checked with a temporary 0.15 day time, rendering the shadow factor alone: the stone pile's far side, houses, trees and grass blades cast where they should, no acne on lit faces, no seam between cascades; `appletest` with soft shadows and water has no validation errors. Frame time stays at the 13.3 ms vsync; recording the draws goes from 40 µs to 144 µs (hard) and 178 µs (soft), mostly the CPU culling of all loaded chunks three times. Not done: GL's shadow preview (`renderShadowIntoTexture`) and the caustics' use of the shadow (milestone 18); GL's own `renderShadow` still runs harmlessly against the null GL when the setting is on. Testing note: in a scripted run the player is sometimes still in the air at the capture frame, and the frame shows only the black sky; check the hand's pose (lower while falling) before blaming the shader. | Learning |
| 14 | Sky | Done (`sky.wgsl`). The bake: GL's three skies (day, twilight, night; `createSkyTextures`' settings, the day sky's from `skyConfig`, re-baked when they change) into three 128x128 cubemaps in the HDR format. A cubemap is a 2D texture of six layers, sampled through a `Cube` view and drawn into through a 2D view of each layer: 18 small passes, each a fullscreen triangle whose instance names the sky and face (a direct draw may start at any instance). The bake shader finds each texel's direction by running the cube-face table (shared by OpenGL, Vulkan, Metal and WebGPU) backward, not through GL's `lookAt` capture matrices, whose up vectors make up for OpenGL's bottom-up framebuffers and would turn the side faces over on WebGPU's top-down ones. The scattering is GL's modified O'Neil shader, in which `fCosSun` is forced to 0, so each sky depends only on how high the direction points. Every frame, before the world: `drawBefore`'s two skies and mix (as three weights) times 1.2, drawn by one fullscreen triangle into two targets at once, the HDR color and a screen-sized sky copy (GL's `fboSkyBox`, which GL fills by copying); the opaque pass then loads the color instead of clearing it. GL's turning of each sky about the vertical by `clock()` is left out: a sky that only changes with height looks the same turned. Not made: GL's diffuse convolution and specular prefilter of each sky (and the BRDF LUT, milestone 12): only `physicallyAccurateReflections`, a false constant, would read them. Fog (`chunkFaces.wgsl`'s `computeFog`, GL's): the lit color mixed toward the sky copy at the pixel, by a near haze thinning with height and a hard far edge at the view distance less a chunk; above water only; on opaque faces, glass and water. The sky copy rides in the frame group (group 1, with the peel's copies), which the opaque lit pipelines now bind too. GL divides by the near-fog density, so its setting of 0 ("disables it", by the menu) gives a NaN; here 0 skips the near fog. The sun and moon (`renderSun`, `sun.frag`): quads 4 units out along `sunPos` and opposite it, scaled as GL scales two matrix entries, at the far plane (LessEqual, no depth writes) so terrain hides them, the color added by alpha, the bloom written over, positions and normals masked; drawn at the start of `renderTransparent`, after the entities and before glass and water, as GL does, with the twilight hue shift. Checked with temporary day times of 0.10 and 0.60 (morning sun with its halo cut by a hill's silhouette; night sky with the moon and a purple horizon from the twilight blend) and at noon in `test` and `appletest` (blue sky lightening toward the horizon, distant hills and water fading into it), with no validation errors; frame time unchanged at the 13.3 ms vsync. GL's own sky draw still runs harmlessly against the null GL. Not done: the sun for the god rays and the lens flare (milestone 18). | Learning |

### Phase D: post-processing

| # | Milestone | Ends with | Type |
|---|---|---|---|
| 15 | Post chain skeleton | Done (`post.wgsl`). The end of GL's frame: the tone mapper (`toneMap.frag`: exposure; `adjustColor`'s saturation, vibrance, shadow and highlight boost, lift and gain, gamma; the five tone mappers, ACES, AgX "punchy", ZCAM with its gamut search, Uncharted 2, PBR Neutral; the sRGB curve but for AgX; the 1/255 gradient-noise dither; the vignette), then FXAA (`fxaa.frag`, 12 search steps). With FXAA on, as by default, the tone mapper writes an intermediate target that FXAA reads onto the surface in wgpu2d's 2D pass; with it off, the tone mapper writes the surface. Each pass is a fullscreen triangle with one bind group (uniforms, source, a linear clamped sampler) and one pipeline per target format. Every read is `textureSampleLevel` (GL's `textureOffset` is its offset argument): FXAA branches and loops on what it reads, where `textureSample`, which takes derivatives, is not allowed. The intermediate target is `rg11b10ufloat` as GL's `fboCoppy` is, so FXAA sees GL's 5-6 mantissa-bit quantization of the tone mapped image (about 1-2 levels in the sky); `rgba8unorm` would be a free improvement after parity. No ping-pong pair yet: this chain's steps all change format, bloom (16) adds into the HDR color by blending, and the first pass that must read and write HDR color is the underwater warp (18), which will add the second texture. The renderer passes the shading settings as GL sets the uniforms (lift * 0.2 - 0.1, gain * 0.4 + 0.8). `WGPU_GBUFFER_VIEW=1-5` still replaces the chain with a debug view; `gbufferView.wgsl`'s view 0 remains the simple preview. Checked from one camera by switching the composite per frame (a temporary hack, removed): against milestone 14's composite the center is identical and the corners darken by the vignette GL's formula predicts (109 to 94 at a corner, 133 to 119 at the top center), nothing differs by more than 32; FXAA changes 0.6% of pixels by more than 8 levels, smoothing the hand's stair-stepped edges; the five tone mappers each render a plausible frame. The dither's noise pattern is mirrored vertically against GL's (WebGPU's pixel rows run down). Frame time stays at the 13.3 ms vsync. | Learning |
| 16 | Bloom and auto exposure | Done (`bloom.wgsl`). Both run in the composite, on the frame encoder, before the debug views and the tone mapper. **Auto exposure:** GL's `glGenerateMipmap` of the HDR color becomes an `rgba16float` mip chain from half the surface down to 1x1, one render pass per level into a single-mip view (a linear read at a target texel's center averages the 2x2 under it). The 1x1 texel is copied into a 256-byte `MapRead` buffer (a copy row is padded to 256 bytes). A mapped buffer, or one waiting to map, may not be used by a submit, so the copy is recorded in one frame and `wgpuBufferMapAsync` is called at the start of the next `render`, after that frame's submit. `wgpuDevicePoll` (non-blocking) then fires the callback, which only sets a state, and the halves are read and the buffer unmapped. No new copy is recorded while a reading is in flight. On this Mac a reading arrives every frame (398 in 400 frames); GL's pixel buffer gives one every other frame. `wgpuWorld::takeExposureColor` hands each reading to the renderer once, and GL's arithmetic follows: the 1/2.2 power, the luminance and `adaptiveExposure.update`, which runs again (milestone 12 had skipped it). A bright snow scene averages about (1.09, 0.89, 0.72), so the exposure heads toward 1.05; a grass scene reads 0.39 and holds near 1.61. **Bloom:** GL's sequence pass for pass. The filter adds the HDR color's bright pixels into the G-buffer's bloom target (blend One, One). Two `rg11b10ufloat` blur chains follow (GL's `bluredColorBuffer`, mip 0 at half the surface, GL's level count: 5 at 500x500), with a view and a bind group per level. At each level come Call of Duty's 13-tap downsample, the tent copy (divided by 12, as GL's) and a Gaussian each way. The levels are added back up smallest first, and the result with its three lens dirt textures (lens, hit, water; nearest sampled, as gl2d loads them pixelated) is added onto the HDR color. `horizontal`, `firstTime` and `lastBloomChannel` run on the CPU as in GL, which picks each pass's group. Each pass reads one mip and writes another view, so a texture is never read and written in one pass. Differences: WebGPU has no border color, so the blur's reads outside 0..1 return black in the shader (GL also fades the outermost half texel). The SSGR term waits for milestone 18 (it adds 0 while god rays are off). The renderer computes the threshold and multiplier before `render`, from the last frame's luminosity, one frame earlier than GL. GL's apply binds `fboMain.color`, a texture name, as a framebuffer. It only lands on the HDR color because HBAO's pass left that bound (with SSAO off it would draw elsewhere); this port draws on the HDR color always. naga's override pass rejected the Gaussian's weight array (`ExpressionAlreadyInScope`, as milestone 13's noise), so its loop is written out. Checked by switching bloom per frame (a temporary hack, removed). At the default settings it lifts the mean by about 8 levels, a haze. GL's numbers predict the same order: a threshold near 0.005 lets almost everything through, and the tent copies and upsamples gain about 13x over 5 levels. At 100x the multiplier the frame washes out, with the lens dirt's blobs at the edges. | Learning |
| 17 | HBAO and SSR | Done (`hbao.wgsl`, SSR in `chunkFaces.wgsl`). **A parity fix first:** GL's transparent passes draw into all of `fboMain`, with blending on the color target only, the positions, normals and materials written over and the bloom's draw buffer off. WebGPU's had written the HDR color alone, so AO over a lake would have read the lake bed. They now draw into the four G-buffer targets (`ColorTargets::GBufferTransparent`; bloom's write mask off) and `fs_blend` writes what `fs_main` does. **AO:** GL calls it HBAO, but its live code is SSAO (the horizon-based version is commented out). At half the surface's size into `r8unorm` (GL's `fboHBAO`, `GL_RED`) come 20 of 32 hemisphere samples 0.5 blocks around the view-space normal, each projected and counted when the G-buffer's position there is in front of it (bias 0.2, range-checked). The apply is a 4x4 box of that, faded out near the view distance, laid over the HDR color as black with alpha AO x 1.45 (SrcAlpha, OneMinusSrcAlpha). It runs where GL's does, after the bloom's blur and before the bloom is added; `compositeFrame` now records exposure, bloom blur, AO, bloom apply, last frame. Positions are read through the linear sampler, as GL's `GL_LINEAR` targets are (a half-size pixel's center falls between four). The integer normals are loaded at the texel: GL samples its `RGB16UI` normals with an `isampler2D` and `GL_LINEAR`, which the GL spec makes incomplete, and drivers that allow it return the texel. The pass and its apply each get their own bind group, since a pass may not bind the texture it draws into. The 4x4 box's y offsets are negated, as GL's run up the screen. **SSR:** GL's standalone SSR pass is `if(0)`; its reflections live in the lit shader (`c_SSR`, on by default), for faces smoother than 0.45. The port is GL's: reflect the view ray about the mapped normal, jitter it by `hash33` of the position (murmur hash on the float bits, `bitcast` here), step up to 50 times by a length set by how squarely the face is seen, compare against last frame's view-space positions, binary-search 20 steps, fade at the screen's edges, and take last frame's color. Water takes the reflection whole before its peel mix (no-peel water darkens 0.2 instead of 0.4 where it reflects); other faces mix by fresnel and roughness. The ray stays in GL's texture coordinates (v up, so its -1..1 bounds test reads the same) and v is flipped only to sample. Nothing is rebuilt from depth (positions are stored), and the x, y and w rows of the 0..1 projection, now in the world uniforms (528 bytes), equal GL's. **History:** after the bloom apply, the HDR color is drawn into an `rgba8unorm` copy (GL's `fboLastFrame` is RGBA8, so reflections are clamped to 0..1; a texture copy cannot convert formats) and the positions are copied as they are. Both join the frame group (bindings 4-6, with the linear sampler). GL takes the face's view position from last frame's positions at the pixel; this uses the face's own, the same while the camera is still. GL's `textureLod(..., 2)` on that one-mip, non-mipmap texture reads level 0. Checked by switching AO or SSR per frame (a temporary hack, removed). AO darkens 5% of `test`'s pixels by more than 8 levels, at grass bases, between plants and on the held hand. SSR changes a pond in `test` from flat cyan to the dark cobblestone wall behind it, with cyan left where rays miss; elsewhere little is smoother than 0.45. Neither `appletest` nor `test2` has water near its spawn. Frame time stays at the 13.3 ms vsync. | Learning |
| 18 | God rays, lens flare, underwater, color grading | Done (`godRays.wgsl`, `underwater.wgsl`, `fs_sunRays` in `sky.wgsl`). **God rays** (GL's SSGR, in `renderSun`): right after the sun pass, against the depth as the opaque faces and entities left it, so water does not block them. The sun and moon are drawn again at half size into an `rg11b10ufloat` target (GL's `fboSunForGodRays`), with GL's settings for this copy (twilight at least 0.5, moon at 0.35), as instances 2 and 3 of the sun's draw. The geometry is masked to black from the depth, then a 200-tap radial blur runs toward the sun's screen position, with 20 rays from a sine of the angle. The blur runs in GL's coordinates (v up) so that sine is not mirrored. The bloom apply adds the result as GL's `applyBloomData` does: squared over the sky, linear over geometry. So, as in GL, the rays show only with bloom on. **Lens flare:** GL's flare is drawn by the 2D renderer, which already runs on WebGPU; what was missing is its occlusion query count (`GL_SAMPLES_PASSED` around the sun, its stub returned 0, so no flare). WebGPU's occlusion queries only promise zero or non-zero, so the god rays' sun counts instead. Each half-size fragment of the sun's quad checks the four full-size depth texels under it against the far plane, by hand (a shader that writes memory cannot rely on the depth test having run first), and adds those in front of the sky to a storage-buffer atomic. The atomic is cleared each frame and read back with milestone 16's `mapAsync` scheme, now a shared `serviceReadback`. `wgpuWorld::sunVisibleSamples` feeds GL's flare code its `queryDataForSunFlare`. **Underwater:** in the lit shader, the color darkens toward the underwater color with distance (`computeFogUnderWater`, strength 0.94) before the reflections, as GL. The warp pass follows the bloom apply: a sine wobble of the coordinates (in GL's, v up), a 10% tint, and the sky painted outright. It writes its own texture, which the tone mapper then reads (the ping-pong milestone 15 anticipated). The settings are used as GL uses them, unconverted (world uniforms 560 bytes). Positions and color are both sampled linearly, as GL's are, so a light fringe follows silhouettes against the sky. GL's code shows the same, though there is no GL frame to compare. GL with FXAA on and under water tone maps into `fboMain.color` before its last-frame copy, so its next frame's reflections read tone-mapped color; this keeps the HDR copy. **Color grading:** GL has no LUT. Its grading is `adjustColor` in the tone mapper, ported in milestone 15. Left for 18-parity: caustics and the water's animated normals and distortion (`u_waterMove`, the dudv maps; `caustics.png` is also missing from this checkout's resources, which in GL would zero the sun on underwater faces), and the grass and leaves' sway. Checked with the sun pinned overhead (`dayTime` 0.22) and the camera pitched up, all temporary and removed. 376 counts arrived in 380 frames, the last 28148 pixels, and the flare draws. Dimming the god-ray copy every other frame shows a halo with the 20-spoke modulation around the sun, stronger over the held hand than over the sky. Forcing `underWater` paints the sky the underwater color, fades far terrain into it, and tints and wobbles the near grass. Frame time stays at the 13.3 ms vsync. | Mixed |
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
- ~~**ImGui upgrade.**~~ Avoided in milestone 4: the backend was ported
  from space-game, and ImGui stays at 1.85.
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
