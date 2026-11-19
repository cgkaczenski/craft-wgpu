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
| 18 | God rays, lens flare, underwater, color grading | Done (`godRays.wgsl`, `underwater.wgsl`, `fs_sunRays` in `sky.wgsl`). **God rays** (GL's SSGR, in `renderSun`): right after the sun pass, against the depth as the opaque faces and entities left it, so water does not block them. The sun and moon are drawn again at half size into an `rg11b10ufloat` target (GL's `fboSunForGodRays`), with GL's settings for this copy (twilight at least 0.5, moon at 0.35), as instances 2 and 3 of the sun's draw. The geometry is masked to black from the depth, then a 200-tap radial blur runs toward the sun's screen position, with 20 rays from a sine of the angle. The blur runs in GL's coordinates (v up) so that sine is not mirrored. The bloom apply adds the result as GL's `applyBloomData` does: squared over the sky, linear over geometry. So, as in GL, the rays show only with bloom on. **Lens flare:** GL's flare is drawn by the 2D renderer, which already runs on WebGPU; what was missing is its occlusion query count (`GL_SAMPLES_PASSED` around the sun, its stub returned 0, so no flare). WebGPU's occlusion queries only promise zero or non-zero, so the god rays' sun counts instead. Each half-size fragment of the sun's quad checks the four full-size depth texels under it against the far plane, by hand (a shader that writes memory cannot rely on the depth test having run first), and adds those in front of the sky to a storage-buffer atomic. The atomic is cleared each frame and read back with milestone 16's `mapAsync` scheme, now a shared `serviceReadback`. `wgpuWorld::sunVisibleSamples` feeds GL's flare code its `queryDataForSunFlare`. **Underwater:** in the lit shader, the color darkens toward the underwater color with distance (`computeFogUnderWater`, strength 0.94) before the reflections, as GL. The warp pass follows the bloom apply: a sine wobble of the coordinates (in GL's, v up), a 10% tint, and the sky painted outright. It writes its own texture, which the tone mapper then reads (the ping-pong milestone 15 anticipated). The settings are used as GL uses them, unconverted (world uniforms 560 bytes). Positions and color are both sampled linearly, as GL's are, so a light fringe follows silhouettes against the sky. GL's code shows the same, though there is no GL frame to compare. GL with FXAA on and under water tone maps into `fboMain.color` before its last-frame copy, so its next frame's reflections read tone-mapped color; this keeps the HDR copy. **Color grading:** GL has no LUT. Its grading is `adjustColor` in the tone mapper, ported in milestone 15. Left for 18-parity: caustics and the water's animated normals and distortion (`u_waterMove`, the dudv maps; `caustics.png` is missing from this checkout's resources, but GL falls back to `caustics.jpg`, which is there), and the grass and leaves' sway. Checked with the sun pinned overhead (`dayTime` 0.22) and the camera pitched up, all temporary and removed. 376 counts arrived in 380 frames, the last 28148 pixels, and the flare draws. Dimming the god-ray copy every other frame shows a halo with the 20-spoke modulation around the sun, stronger over the held hand than over the sky. Forcing `underWater` paints the sky the underwater color, fades far terrain into it, and tints and wobbles the near grass. Frame time stays at the 13.3 ms vsync. | Mixed |
| 18-parity | Full parity | Same seed, position, time of day and settings against the reference, pass by pass, using the ImGui debug views. **Porting half done:** what milestones 6-18 left out. **Animation:** `defaultShader.vert`'s `calculateVertexPos` is `animatedCorner` in `chunkFaces.wgsl`: water's two travelling waves on its surface corners (all 14 water orientations, GL's 56-entry mask), the leaves' sines on every corner and the grass's top corners pushed along one direction. Normals for every shape but the cube faces and still water come, as in GL, from three animated corners. Every vertex entry point goes through `faceVertex`, so the prepass, the solid and transparent passes and the shadow maps move alike (Equal depth holds; GL's `zpass.vert` animates the same way). The clock is wall seconds since start (`animation.x`; world uniforms now 576 bytes): GL's `std::clock() / 1000` is wall time only on Windows, and on macOS it would run on CPU milliseconds, about 1000 times fast. The underwater warp now uses it too. `animation.y` is GL's `u_waterMove` (the renderer's `waterTimer`). **Water textures:** `waterDUDV.png`, `normal.png` and `caustics.jpg` join group 0 at 13-15 with a linear, mipmapped, repeating sampler (16), as gl2d loads them. They tile by world position, so v is negated to read GL's texel (wgpu2d stores rows top first, gl2d bottom first). The `getDudvCoords` family is ported as is. The reads sit in branches, where `textureSample` is not allowed, so each coordinate carries the screen derivatives of its base (`-blockPos.zx + uv`, taken in `readTexels`) for `textureSampleGrad`, and terrain out of water pays nothing. The water's normal is two reads of `normal.png`, flattened to a fifth. In the final transparent pass, what is under the water is looked up 0.009 off along the dudv map (v's part reversed for v down), unless that lands on something in front of the water. The distorted read is linear, as GL's `fboCoppy` is. Caustics on faces in water (flag bit 1) tint the point lights and the sun, with GL's chromatic offset, and add to bloom where they are brightest. **Fixes:** milestone 16's lens dirt was upside down. It is screen space, where both libraries' row orders put the image's top at the top, so it now reads straight. **Checked:** frames 60 apart differ only along leaf and grass edges. At `test2`'s pond the surface ripples, and switching the caustics off every other frame (a temporary hack, removed) changes only the pond bed, in bands. No validation errors; frame time stays at the 13.3-13.6 ms vsync. The GL build compiles. **Still open:** the comparison half needs a GL frame, which this Mac cannot produce (open question 1). | Game-specific |

### After parity

| # | Milestone | Notes |
|---|---|---|
| 19 | Delete OpenGL | Decided: the project becomes WebGPU only (wgpu-native: Direct3D 12 or Vulkan on Windows, Vulkan on Linux, Metal on macOS; the CMake fetch has prebuilt binaries for all three). The last commit that builds the OpenGL renderer is tagged `opengl-final` (`cmake -DRENDERER_WEBGPU=OFF` there), for comparisons on a machine whose GL draws, and for the GLSL that each WebGPU pass was ported from. In steps, each leaving the game buildable and drawing as before: **19a** the build is WebGPU only (no `RENDERER_WEBGPU` option or guards; the vendored GLFW 3.3, gl2d and the OpenGL tools go). **19b** GL leaves the smaller files, and what they took from GL names gets real identifiers: meshes keyed by GL vertex array names, block texture slots by fake bindless handles. **19c** `renderer.cpp`'s GL passes go, keeping what computes the frame's settings. **19d** the null GL scaffold (`nullGL.*`, `nullGLStubs.inl`, `tools/genNullGL.py`), glad, ImGui's OpenGL backend and the GLSL shaders are deleted. **19e** Windows and Linux: CMake and code kept portable, tested later on real machines (expect MSVC and gcc fixes, and naga's HLSL and SPIR-V output to differ from its Metal). **19a done:** the `RENDERER_WEBGPU` option and define are gone, and its guards resolved to the WebGPU side by `unifdef -DRENDERER_WEBGPU=1` (it cannot parse C++14 digit separators, so `gamePlayLogic.cpp`'s two guards were removed by hand). Deleted: `noiseTool/`, `PBRTool/`, `thirdparty/gl2d`, `thirdparty/glfw-3.3.7` and `glfw-3.3.2`. GLFW 3.4, wgpu-native and glfw3webgpu are always fetched. Checked: the title menu matches the earlier capture pixel for pixel, the world draws, and the shader reload works, with no validation errors. The world couldn't be diffed pixel for pixel: the spawn varied from run to run. The server corrected the spawn height only on a tick that found the spawn chunk loaded, and chunks load after a player joins, so it was a race. Fixed after 19a: `loadSpawnChunkAndPlaceSpawn` (`server.cpp`) runs at server startup, before connections are handled. It generates the spawn chunk and the chunks two each way, so neighbouring trees have landed, then runs the same ground-finding, now `correctSpawnHeight`. Four runs of `test` all spawn at (0, 67, 0) with the same view; the camera's starting pitch can still follow the real mouse. **19b done:** GL calls are gone from `blocksLoader.cpp`, `model.cpp`, `chunk.cpp`, `gameLayer.cpp`, `gamePlayLogic.cpp`, `createConnection.cpp`, `renderSettings.cpp`, `UiEngine.cpp` and `glfwMain.cpp` (which now defines `GLFW_INCLUDE_NONE`: the window is a WebGPU surface and needs no GL header). New identifiers: a block or item texture slot (`gpuIds`, `gpuIdsItems`, a model's or skin's texture) is the wgpu2d texture id itself, 0 for none, in place of a bindless handle; a model's or item's mesh comes from `wgpuWorld::newMeshId()`, still in a field named `vao` until 19c. Textures are now freed (`wgpu2d::cleanupTextureId`) where GL deleted them: `clearAllTextures`, `clearAllModels`. The profiler is CPU only (the GL timer queries and `initGPUProfiler` went), and `errorReporting.cpp` (GL debug output only) is deleted. Left for 19c, as renderer machinery: chunks' `vao`, `transparentVao` and `lightsBuffer` (always 0, named only by `renderer.cpp`'s GL passes), `programData.GPUProfiler`, `skyBoxRenderer`, `shader.cpp`, `bigGpuBuffer` and `sunShadow`. Checked: the title menu matches pixel for pixel; `test` draws as before 19b (the same 88 chunks and 193071 opaque faces, with pixels differing only on swaying leaves and grass); `test2` draws its items and models; the shader reload runs the same sequence as milestone 20; no validation errors. One `test` run looked lower, with 52 chunks: the starting pitch followed the real mouse, and the two runs after it matched. A texture-pack reload (`clearAllTexturePacks`, forced once mid-game by a temporary edit) frees and reloads them, and the blocks, the hand and the hotbar draw the same afterwards. **19c done:** `renderer.cpp` (7056 lines to 2846) keeps what a frame computes on the CPU and hands to wgpuWorld: the time-of-day lighting, the frustum culling and chunk sort, the dropped block cubes, the `Lighting` values, the water timer, the entities' bones and instances, the crack decal's faces, the exposure update from the WebGPU reading, the lens flare (wgpu2d), the block geometry tables and the inventory icons. Gone with GL's passes: the shader structs and their uniforms, `FBO`, `QueryObject`, `renderShadow` and its unused frustum helpers (wgpuWorld's cascades come from `sunCascadeMatrix`), the bloom mip setup, the GL light-buffer copy, and `PointDebugRenderer` (GL points only). `renderer.h` keeps `Renderer` (its `ShadingSettings`, which the menus edit, moved up from the default shader: `renderer.shadingSettings`), `AdaptiveExposure` and `GyzmosRenderer` (lines only). Deleted: `shader.*`, `sunShadow.*` (wgpuWorld has its own cascades), `bigGpuBuffer.*` (switched off; `ChunkSystem::gpuBuffer` and the `BigGpuBuffer *` parameters went with it), chunks' `vao`, `transparentVao` and `lightsBuffer`, `ProgramData::GPUProfiler` and `sunRenderer`. `skyBoxRenderer.*` keeps only the sky colors (wgpuWorld re-bakes its skies when they change, so the "Refresh" button went), the sun and moon textures and `calculateSunPosition`. Models' and items' `vao` is now `mesh`. The debug panels lost the GL texture previews (GL names mean nothing to ImGui's WebGPU backend; the G-buffer views stay), and the metallic, roughness, "Sort chunks" and "Render Transparent" controls, which only GL's passes read. One behavior the GL code carried by accident: the icon pass returned early until the GL icon shader existed, which skipped the first call (made while the texture packs load, before `create`). A `created` flag keeps that order; without it the world started up early and `create`'s shader reload ran for real, shifting the reload test by one. Checked: the title menu matches pixel for pixel; `test` draws as before 19b (88 chunks, 193071 opaque faces, at most 0.04% of pixels off by more than 32, on swaying leaves and grass; a first run that started with the mouse elsewhere matched 19b's run with the same view); `test2` matches 19b; the shader reload runs milestone 20's sequence and its frames match 19b's; a forced texture-pack reload redraws all 208 icons and the frame after matches 19b's; no validation errors. **19d done:** nothing calls OpenGL, so its scaffolding is deleted: `nullGL.cpp`, `nullGLStubs.inl`, `nullGL.h` and `tools/genNullGL.py`; `thirdparty/glad` (and its links from the game, glui and the gl2d shim, and the includes in the shim and `shared/blocks.h`); ImGui's OpenGL backend (`imgui_impl_opengl3.*`; ImGui draws through `wgpuImgui`, with `imgui_impl_glfw` for input); the 43 GLSL files under `resources/shaders/` (the WGSL under `webgpu/` is all that is loaded; the GLSL each pass was ported from is at the `opengl-final` tag); and `ShadingSettings::formatIntoGLSLcode`. `GLFW_INCLUDE_NONE` is now a compile definition of the game target, so no file gets the system GL header through GLFW (on Linux that header need not even be installed); the per-file defines went. Left on purpose: `thirdparty/imgui-docking/imgui/examples/` (vendored, never built, many backends' samples), comments that name the GL behavior a WebGPU pass matches, and the learning outlines' history (each section is tied to its commit). Checked: it builds with no glad on the include path; the title menu matches pixel for pixel; `test` draws 88 chunks and 193071 opaque faces as before (three earlier runs looked toward the horizon because the real mouse was moving; the two after matched); `test2` matches 19b; the shader reload runs milestone 20's sequence with its frames matching 19b's; a forced texture-pack reload redraws the icons, with differences only on swaying leaves and grass. |
| 20 | Hot reload | Done. `wgpuWorld::reloadShaders`, called from `Renderer::reloadShaders`: GL's `R` key, or a shading setting GL recompiles for (that check also fires once on entering a world). WGSL is baked into pipelines, so a reload rebuilds the pipelines and their layouts and nothing else. The five create functions that mixed the two are split: `create*Resources` (bind group layouts, buffers, samplers, the sky cubes and the bake's group) runs once at startup, and `createPipelineSet` (the eight pipeline functions) runs at startup and on each reload. Bind groups, the arena and the targets are untouched. All or nothing: `forEachReloadable` lists the 42 pipelines and 17 layouts. A reload moves them aside, builds a new set, and keeps the new one only if every pipeline was made; otherwise it releases what it made and puts the old set back. A WGSL error prints with its line (naga's message) and the game keeps drawing. The skies re-bake after a reload, as GL's reload redoes its sky textures. The block icons' and block textures' pipelines run once at load and are not reloaded. A reload takes about 20 ms. Checked by script, editing `post.wgsl` between `R` presses: a red tint appears on reload, a syntax error is reported and the tint stays, and restoring the file brings the normal image back. No other validation errors; the GL build compiles. |

### Lighting: ideas from Disney's principled BRDF

Ideas; L0 is done. Drafted 2026-11-16 from `~/game-dev/Disney-PBR`
(Acerola's Unity demo: Lambert, Blinn-Phong and Disney's 2012 BRDF with a
sky sample for indirect light) and a read of
[`lighting.wgsl`](../resources/shaders/webgpu/lighting.wgsl). The aim is to
learn the model by building it in WGSL, and to keep only the lobes this game
has a surface for.

**What the game has today.** One function lights everything,
`computePointLightSource`: GGX, Smith and Schlick for the highlight and
`diffuseOrenNayar2` for the rest. The sun, every light block, the entities
(`lightEntity`) and the lying snow (`fs_snow`) all go through it, so a change
there reaches all of them. The ambient light is `albedo * ambient`: the baked
light levels and nothing else. Only the sun and the light blocks make a
highlight; a surface never reflects the sky, except puddles and water, and
faces smoother than 0.45 through SSR.

**What the reference is.** Take the math from Burley's 2012 course notes and
Disney's own `disney.brdf` (both linked in the demo's README), and use the
demo as a readable companion, not as text to port. It has no license file,
and it differs from the reference in places: the clearcoat's `SmithGGX` is
called with its arguments in the wrong order, `AcerolaBRDF.shader`'s sheen
uses the view angle and the specular colour, and its indirect light samples
the skybox along the normal, not the reflected ray.

**A finding in the current diffuse.** `diffuseOrenNayar2` is not Oren-Nayar
as Fujii published it, and GL's was the same (`fDiffuseOrenNayar2` at
`opengl-final`), so it is not a porting slip. Fujii has
`s = L·V - (N·L)(N·V)` and `B = σ·A`; this has `L·N` in place of `L·V` and
`PI` in place of `σ`. The result does not depend on the angle between the
light and the eye at all. It brightens a face the more it is seen edge-on: at
roughness 0.9, a block's top under an overhead sun is 0.79 of Lambert looking
straight down and 3.3 times Lambert looking along it, and summed over every
view direction it sends back 1.6 times the sunlight that arrives (1.8 at
roughness 0.5; L0's `tools/brdfEnergy` prints the table). Distant sunlit ground is bright
because of this. The game's sun strength and exposure were tuned
on top of it.

| # | Milestone | Depends on | Type |
|---|---|---|---|
| L0 | Lobe views and an energy check (done) | | Learning |
| L1 | Burley diffuse | L0 | Learning |
| L2 | The sky in the ambient light | L0 | Learning |
| L3 | A water film on wet ground (clearcoat) | L2 | Mixed |
| L4 | A material table per texture | | Game-specific |
| L5 | Sheen | L1, L4 | Mixed |
| L6 | Snow's soft diffuse, and light through leaves | L1 | Mixed |

#### L0. Lobe views and an energy check

**Disney:** the notes are built on looking at one lobe at a time; the demo
returns `BRDFResults` (diffuse, specular, clearcoat) and has a slider per
parameter (`Material Wizard.cs`).

**Here:** `computePointLightSource` returns its lobes separately; a setting
in the client control panel (and an environment variable, as
`WGPU_GBUFFER_VIEW`) shows one alone: diffuse, specular, ambient, later
coat and sheen. Panel sliders override roughness and metallic on every face
(19c removed GL's, which nothing read). A small standalone program integrates
each diffuse model over the hemisphere and prints the share of light sent
back, for a table of roughness and light angles: the check that found the
1.6 above would be one run.

**Done (2026-11-16).** `lighting.wgsl`'s `computePointLightSource`,
`pointLights` and `sunLight` return a `Lobes` (diffuse, specular), and
`lightShown` gives their sum with the ambient or one part alone. The block
faces, the entities and the lying snow (whose crystals' glints count as
specular) all go through it. The world uniforms gained `lightDebug` (1760
bytes).

- **Views:** "Light shown" in the panel's "Light Stuff", or
  `OURCRAFT_LIGHT_VIEW=1|2|3` (diffuse, specular, ambient). A part is shown
  as it left the surface: no fog, no reflections, no emissive bloom, water
  opaque, and the exposure held where the whole frame had it, so the part is
  as bright on the screen as it is in that frame.
- **Diffuse model:** "Diffuse model", or `OURCRAFT_DIFFUSE=1`, swaps in
  Lambert. L1 adds Burley to the same switch.
- **Overrides:** "roughness for all" and "metallic for all", or
  `OURCRAFT_ROUGHNESS` and `OURCRAFT_METALLIC` (0..1), replace the material
  map's values on block faces and the entities' constants. Water, the wet
  ground and the snow still set their own afterwards.
- **Energy check:** `tools/brdfEnergy`
  (`cmake --build build --target brdfEnergy && build/brdfEnergy`, a few
  seconds). The BRDF's functions copied line for line into C++, summed over
  the hemisphere for eight roughnesses and six light heights. A new model is
  one function and one line in `diffuseModels`.

What the check prints, beyond the finding above:

- The game's diffuse sends back 1.6 to 2.0 times what arrives under a high
  light, most for smooth surfaces, and drops to about 1 for a light 5
  degrees up. Fujii's formula stays between 0.78 and 1.
- It is not the same with the light and the eye swapped (0.91 one way,
  0.27 the other, for one pair of directions), which a real surface is.
  Lambert, Fujii's and the specular lobe are.
- `distributionGGX`'s floor under its denominator (`max(denom, 0.0000001)`)
  cuts off the highlight's peak below a roughness of about 0.11. The
  material map's smoothest (0.09) keeps 60% of its highlight and a puddle
  (0.07) 25%; still water (0.11) keeps 97%. With the floor lowered the same
  sum gives 1.
- The specular lobe keeps 99% at roughness 0.3 and 41% at 0.9: light lost
  between a rough surface's bumps, which this model never returns.

**Checked** in a copy of `test2` (dry land at 179,80,-26 and the pond,
clear, day time 0.2). With the view off the frame matches the commit before
to within what two runs of the same build differ by (1.5% of pixels by more
than 2 levels against 1.4%, on the swaying grass). The diffuse and ambient
views show the ground lit and unlit by the sun as expected. The specular
view of the same ground is black: at the block textures' roughness the
highlight adds nothing visible, so L2 and L3 are what will make it show.
With roughness 0.25 for all, texel-sized glints appear on the ground; at the
sea shore at sunrise the view shows the sun's path on the water alone; on
lying snow it shows only the crystals. Lambert in place of the game's
diffuse changes 61% of that frame's pixels by more than 8 levels, darker.
Metallic 1 leaves the ground lit by the ambient alone, plus glints. No
validation errors. Not checked: the panel's controls themselves (scripted
runs cannot reach ImGui; the environment variables set the same fields).

#### L1. Burley diffuse

**Disney:** `Fd90 = 0.5 + 2·roughness·(L·H)²`, and the diffuse is Lambert
times `mix(1, Fd90, (1-N·L)^5) · mix(1, Fd90, (1-N·V)^5)`. Rough surfaces
brighten when the light and the eye are both low and on the same side; smooth
ones darken at their edges. It can also send back more than arrives (1.03
under an overhead sun at roughness 0.9, 1.3 under one 5 degrees up);
Frostbite's version (Lagarde and de Rousiers, 2014) rescales it and stays
under 0.9.

**Here:** replaces `diffuseOrenNayar2`, behind a setting so the two can be
switched per frame. This changes the look more than any other item: for the
overhead-sun case above, Burley gives 1.0 of Lambert looking down and about
1.2-1.4 looking along the ground, against 0.79 and 3.3 now. In the sun's
diffuse light, near ground gets a quarter brighter and far ground less than
half as bright, so one multiplier cannot match the old frames. Decide by eye whether the bright
distance is wanted. If it is, keep it as a bounded, named term rather than
as the formula's accident.

**Done when:** L0's energy check agrees with those numbers, and noon and low-sun frames of `test` and `test2` are compared side
by side with `sunLight`'s 1.9 retuned.

#### L2. The sky in the ambient light

**Disney-PBR:** its fourth example adds the skybox, blurred by roughness, to
the BRDF. The standard form is the split sum: the sky blurred for the
roughness, looked up along the reflected ray, times a Fresnel term that
depends on roughness and view angle. GL had this and switched it off
(`physicallyAccurateReflections`), and the port never made the prefiltered
skies or the BRDF LUT.

**Here:** the largest gap in the lighting, and what L3 depends on. This
game's skies depend only on how high a direction points (milestone 14), so
the prefiltered sky is a small 2D table, height by roughness, rebuilt when
the time of day or the weather changes: one read, no cube mips. The Fresnel
term can be Karis's analytic fit, with no LUT. The result is scaled by how
much sky reaches the face (`skyUnchanged`, so caves get none) and by the AO.
The diffuse ambient stays the baked levels. SSR, where it finds something,
replaces the sky read as it does for puddles today (`reflectPuddle`'s
`skyToward`, which reads the screen's sky copy and clamps at its edges, can
then use the table).

**Done when:** a gold block, ice and a puddle show the sky's colour at a
shallow angle under an overcast sky, with no sun; a cave shows none.

#### L3. A water film on wet ground (clearcoat)

**Disney:** a second highlight over the base: GTR1 (a longer tail than GGX),
a fixed reflectance of 0.04 and a fixed geometry term. Two parameters, how
much and how glossy.

**Here:** rain now halves the ground's roughness (`shade`: floor 0.46, kept
just above SSR's 0.45), which makes damp stone look like a smoother stone. A
film of water over a rough stone is the two-lobe case the clearcoat exists
for: the base keeps its roughness and its darkened albedo (that part is
right), and the film adds a tight highlight.

- Reflectance 0.02, water's, not Disney's 0.04. The base is scaled by what
  the film lets through, `1 - Fcoat` (Disney omits this; glTF's clearcoat
  and Filament do it).
- The film's normal: the mapped normal when damp, going to the face's own as
  it gets wetter, as a puddle already "lies flat over the block's bumps".
- Damp ground and puddles become one thing: `wet.amount` is how much coat,
  `wet.puddle` takes its gloss to water's. `reflectPuddle` becomes the
  coat's reflection.
- It needs L2. Rain sets the sun to 0.25 and a storm to 0.1
  (`weather.cpp`), so the film's sun highlight is nearly absent while it
  rains; what a wet street shows then is the sky at a shallow angle. The
  sun's glint shows after the rain, while the ground dries.
- The same coat fits the sand and stone in the band along a shore, and ice.

**Done when:** under rain, damp ground reflects the sky looking along it and
not looking down; after rain with the sun out, a sharp glint sits on stone
that still looks rough in the diffuse view.

#### L4. A material table per texture

**Disney:** eleven parameters per material. Here the material map gives
three per texel (roughness, metallic, emission), and every dielectric has a
reflectance of 0.04.

**Here:** a storage buffer beside `textureLayers`, one record per texture
layer: sheen, subsurface, light through, coat, reflectance. Filled from a
text file in `resources` that names blocks; everything defaults to 0 and
0.04. Without it the new lobes can only go on what the shader recognises by
the weather (wet, snowed on), which is why L3 comes first. Reflectance alone
is worth having: water 0.02, ice 0.018, glass 0.04, gems up to 0.17.
Entities get one record per kind in place of `lightEntity`'s constants.

**Done when:** editing the file and reloading changes one block's look.

#### L5. Sheen

**Disney:** `(1 - L·H)^5 · sheen`, tinted toward the base colour, added to
the diffuse. Made for cloth: light scattered back from fibres at the edge.

**Here:** its home is the cloth blocks (`clothBlock`, the wool colours, their
stairs, slabs and walls) and `mossyCobblestone`. On snow it is a trial:
snow's albedo is already near white, and the term is small unless both the
sun and the eye are low, which is a low sun over a snowfield. Judge it in
L0's sheen view before keeping it.

**Done when:** a cloth block's edges lighten against the light and a stone
block's do not.

#### L6. Snow's soft diffuse, and light through leaves

**Disney:** `subsurface` blends the diffuse toward a flatter shape (after
Hanrahan and Krueger). It is not light passing through anything; it only
softens the falloff toward the shadow side. Disney's 2015 extension adds
light through thin surfaces; the demo does not have it.

**Here:** two separate things.

- Snow (the weather's cover in `shade`, `fs_snow`, `snow_block`): a little
  of the flattening, 0.3 or so. More would wash out the tracks, which read by
  their walls' shading. Scale it by `1 - pressed` in `fs_snow`: packed snow
  scatters less, and the tracks keep their contrast.
- Leaves, grass blades and plant sprites: light coming through, which is the
  larger effect. Looking toward the sun, a term of the albedo (shifted
  toward yellow-green) times the sun and its shadow times a power of
  `dot(V, -L)`, the same shape as the water's `through` glow in
  `waterColour`. Grass is lit by an up normal, so the term must come from
  the view, not from the normal.

**Done when:** a canopy seen from below toward a low sun glows and the same
canopy seen with the sun behind the camera does not; footprints in snow are
as easy to see as before.

#### Smaller, with L1 or later

- Smith's geometry term: the height-correlated form (the demo's `SmithGGX`)
  or Disney's remap `((1 + roughness) / 2)²` for direct lights, which tames
  the highlight on rough blocks at shallow angles.
- `distributionGGX`'s floor (L0's check): lower it, or raise the smoothest
  roughness to 0.11, so that smooth surfaces and puddles keep their whole
  highlight. A look change: the sun's glint on a puddle would be four times
  as bright.
- Cost: the lobes run once for the sun and once per light block in range
  (9-41 a frame in `test`). Skip a lobe whose amount is 0, and measure with
  the present mode set to Immediate, since the frame time is vsync-capped.

#### Not taking

- **Anisotropy.** It needs a grain direction per block, and nothing here is
  brushed metal. (`normalToRotation` would supply the tangent if a use
  appears: log ends, say.)
- **Specular tint and sheen tint** as parameters: constants in L5 are enough.
- **The tone mappers.** The demo has nine; `post.wgsl` has five, including
  the same ACES fit.
- **Its Blinn-Phong and Lambert shaders**, except Lambert as a comparison
  view in L0.

### Shells: ideas from Acerola's shell texturing

Ideas; nothing is built. Drafted 2026-11-16 from `~/game-dev/Shell-Texturing`
(Acerola's Unity demo, MIT: a furry ball) and a read of the grass blades
(`vs_blade` in
[`chunkFaces.wgsl`](../resources/shaders/webgpu/chunkFaces.wgsl)), the lying
snow ([`snow.wgsl`](../resources/shaders/webgpu/snow.wgsl)) and the creatures
(`vs_model` in [`entities.wgsl`](../resources/shaders/webgpu/entities.wgsl)).
The aim is to learn the technique by building it in WGSL, and to keep it only
where this game has a surface it suits.

**What the demo is.** Two files, `Shell.shader` and `SimpleShell.cs`.

- The mesh is drawn once per shell, each copy pushed out along the normal by
  `length * (i / count)^distanceAttenuation`.
- The fragment stage multiplies the uv by a density and takes the whole part
  as a strand's number. A hash of the number is the strand's height. A pixel
  is discarded when it is farther from the strand's middle than
  `thickness * (height - h)`, where `h` is the shell's own height, 0..1: each
  strand is a cone. Shell 0 is never cut, so the mesh has no holes.
- Light is half Lambert, squared, times `h^attenuation + bias`: dark at the
  roots, standing in for the light the strands block.
- A direction from the CPU moves each shell sideways by `h^curvature`: roots
  fixed, tips moved. The script lets it lag behind the ball's movement and
  fall under gravity at rest.

Three things to do differently, none of them a port's concern: it makes a
GameObject and a draw per shell and sets thirteen uniforms on each every
frame; its seed (`x + 100 * y`) repeats along diagonals once the density
passes 100; and it has no fins, so a shell seen edge on is a stack of lines.

**What WebGPU changes.** These are the parts worth learning here.

- One draw, not one per shell. The shell's number comes from the vertex or
  instance index, as a blade's number does in `vs_blade` (one instance per
  cell, the vertex index says which blade). For ground: one instance per
  top, `6 * shells` vertices, shell `vertexIndex / 6`. For a creature:
  `instances * shells` instances of its mesh, shell `instance % shells`.
- The parameters live in the world uniforms, set once.
- The cost is fill, not triangles: every pixel of a shelled surface runs the
  fragment stage once per layer, and a fragment that may discard is the kind
  Apple's GPUs remove hidden surfaces for least well. The blades already
  have the answer: their own depth prepass with a fragment stage that only
  discards (`fs_bladeDepth`), then the lit pass where the depth is Equal. A
  shell layer's prepass fragment is one hash and one compare, and the
  lighting runs once per pixel. The layers are listed top first.
- The hash is integer math (`bladeHash` is already that), and a texture read
  after a discard needs `textureSampleLevel`, as elsewhere in this file.

**A finding: shells need more layers the farther away they are.** The other
suggestion on the table was real blades near, shells in the middle band,
sprites at the edge. The gap it names is real: a bare grass top has a clump
on 12% to 70% of blocks (`bladeHash`'s `clump.z` against the patch noise) and
the flat texture everywhere else. But that is as true at the player's feet
as in the middle band, and distance works against shells. A ray that drops
one layer's spacing while crossing less than one strand's width cannot slip
between two layers of a strand; past that it can. So the layers needed are
about `height * distance / (strand width * eye height)`. For strands a texel
wide (1/16 of a block), turf 2 texels high and the eye 1.5 blocks up, that
is `1.33 * distance`: 4 layers at 3 blocks, 11 at 8, 18 at 13.6 (where the
blades start to give way to the sprites), 29 at 22 (the blades' reach).

S0 measured it (its tables are below). The direction is right and there is
no sharp line; what too few layers look like depends on the field:

- Strands standing apart come apart into separate discs, and the ground
  shows between them.
- A dense lawn shows no lines at 1080p at any distance. It gets darker with
  distance, because the view reaches down between the layers to the darker
  ones near the roots: at 10 to 14 blocks, 8 layers are 15% darker than 64
  and 16 layers 6%.

So 16 layers are enough for dense turf 2 texels high out to the blades'
whole reach, and the count is not what limits how far the shells go. Two
other things do. Each layer costs fill over every lawn pixel (S0's cost
table). And at 1080p, 13 blocks away, a strand is under 4 pixels wide and
the turf 7 high: there is little left to see. So the shells are still not a
band between the blades and the sprites. They are a layer under the blades
from the feet outward, where they are seen from above and show the most,
fading out by sinking to nothing where the blades give way to the sprites.
Sinking is the fade: layer 0 is the block's own top.

**What fits this game's look.** Strands at the texture's own grain: 16 to a
block, square, each a texel of the top's picture standing up to its own
hashed height, darker toward the root in whole steps. The blades already
take a texel of the sprite each, and the dithers and the PSX snap are at the
same grain. The demo's round cones are for S0 only.

| # | Milestone | Depends on | Type |
|---|---|---|---|
| S0 | The demo in WGSL, on the grass tops, with a layer view (done) | | Learning |
| S1 | Turf: the game's own version | S0 | Mixed |
| S2 | Moss, cloth and hay: shells off any face | S1, L4 helps | Mixed |
| S3 | A coat for the cat: fur on creatures | S0 | Mixed |
| S4 | Trampled grass | S1 | Game-specific |

#### S0. The demo in WGSL, with a layer view

**Demo:** all of it, as written: extrusion, the cone test, the root shading,
the displacement.

**Here:** a new pipeline pair beside the blades' (prepass and Equal), drawn
over the grass tops near the camera from a cell list made as
`findGrassCells` makes the blades' (flag bit 3 already marks those tops). The
demo's parameters are panel sliders and environment variables: count, length,
density, thickness, the height range, the two exponents, the bias. The
displacement is a fixed direction for now.

- **Layer view:** each pixel coloured by the layer it came from. This shows
  the lines at shallow angles, and is the check on the finding above: walk
  back from a lawn at 8, 16 and 32 layers and note where the lines appear.
- **Cost:** the frame time with the present mode Immediate (the log line is
  vsync-capped), looking down at a lawn that fills the screen, at each count,
  with and without the prepass. That table decides S1's layer count.

**Done when:** a patch of lawn looks like the demo's ball unrolled, the layer
view matches the formula or corrects it, and the cost table is in this file.

**Done (2026-11-16).** `chunkFaces.wgsl`'s "Shells" section: `vs_shell`,
`fs_shell` and `fs_shellDepth`, with the demo's reasoning in its comments.
Three pipelines (`shellPipeline`, `shellEqualPipeline`,
`shellPrepassPipeline`) on the blades' layout and the blades' cell buffer:
one draw, an instance a cell, `6 * (layers - 1)` vertices each, the top layer
first. Nothing new is uploaded. The world uniforms gained five vectors (1840
bytes), filled from `ShellSettings` (`shellSettings.h`). Off unless turned
on, and only with the Grass setting's BLADES, whose cells they stand on.

- **Controls:** "Shells" in the panel, beside the grass sliders, with the
  demo's twelve parameters and a reach. For scripted runs
  `OURCRAFT_SHELLS=<layers>`, `OURCRAFT_SHELL_VIEW=1` and
  `OURCRAFT_SHELL_SET="length=0.25,density=32"` (`ShellSettings`'s names).
  Two more were added for the checks: `OURCRAFT_GRASS_DENSITY=0` leaves the
  lawn bare but for the shells, and `OURCRAFT_PREPASS=0|1` sets the depth
  prepass.
- **Layer view:** each pixel in its layer's colour, four colours taking
  turns, the block's own top black, a white ring every 4 blocks from the
  camera.
- **Not as the demo:** layer 0 is not drawn; it is the block's own top,
  which `shade` gives layer 0's colour. A strand's number is its place in
  the world, hashed, in place of `x + 100 * y`. The light is the game's
  (`shade`), since half Lambert on a flat top is one number for the whole
  lawn; only the roots' darkening is kept. The push is a fixed direction,
  downwind. The demo's hash has a constant longer than 32 bits; what fits
  is used.
- **Values:** the demo's own scene (256 layers, density 366, thickness
  3.53) is set for a ball. On a block's top, 32 layers, 32 strands across,
  thickness 3.5 and length 0.25 give a plush lawn; the defaults here (16,
  16, 2, 0.15) give something closer to scales.

**The layer check.** Standing on a flat lawn in a copy of `test2`
(179,80,-26, looking west, pitch -10, 1920 by 1048), blades off, strands a
texel wide and 2 texels high, shells out to 22 blocks.

Strands standing apart (a quarter of the cells have one), in the layer view:
the share of pixels where the black top shows, by distance. With enough
layers it falls away with distance as the strands hide the ground behind
them. With 8 it stops falling at about 5 blocks, near the 6 the estimate
gives; with 16 it falls slowly; the difference grows steadily and there is
no distance where it starts. Part of it is there from the first block: a
strand only reaches the last layer under its height, so fewer layers make
shorter strands.

| Blocks away | 8 layers | 16 | 32 | 64 |
|---|---|---|---|---|
| 3-4 | 0.56 | 0.48 | 0.44 | 0.43 |
| 5-6 | 0.43 | 0.31 | 0.27 | 0.25 |
| 7-8 | 0.45 | 0.26 | 0.19 | 0.17 |
| 10-12 | 0.46 | 0.21 | 0.13 | 0.11 |
| 12-14 | 0.44 | 0.17 | 0.09 | 0.06 |
| 17-21 | 0.22 | 0.08 | 0.02 | 0.01 |

A dense lawn (every cell a strand), lit: the mean green level, and in
brackets the share of pixels more than 24 levels from the 64-layer frame.
Side by side the three frames look alike past 8 blocks; the 8-layer one
shows its steps on the nearest strands.

| Blocks away | 8 layers | 16 | 64 |
|---|---|---|---|
| 3-4 | 127 (8%) | 133 (2%) | 138 |
| 6-8 | 140 (28%) | 152 (6%) | 158 |
| 10-12 | 141 (46%) | 156 (16%) | 165 |
| 12-14 | 143 (51%) | 157 (20%) | 168 |
| 17-21 | 177 (17%) | 180 (8%) | 183 |

**The cost.** Looking down (pitch -60) at that lawn filling the screen, 1920
by 1048, the same dense strands, shells out to 13 blocks, an M6, the present
mode Immediate for the runs. Under about 100 layers the frame time cannot
show them: without shells it reads 8 to 12 ms from one run to the next. So
the cost is the slope where the GPU is the limit. Two or three runs each,
within 0.7 ms of each other (128 with the prepass was run once, and is
still inside that 8 to 12):

| Layers | Prepass on, ms | Prepass off, ms |
|---|---|---|
| 128 | 11.2 | 17.9 |
| 256 | 12.6 | 25.1 |
| 512 | 18.2 | 37.7 |
| 1024 | 28.7 | |

That is 0.021 ms a layer with the prepass and 0.05 without: the prepass
makes a layer 2.4 times cheaper. Carried down to the counts in question, an
estimate and not a measurement: 16 layers 0.3 ms (0.8 without the prepass),
32 layers 0.7 ms (1.6), 64 layers 1.3 ms (3.2). The panel's slider stops at
64; `OURCRAFT_SHELLS` takes up to 1024, for timing.

**Checked:** no validation errors in any run. With the blades off, the
frame with the prepass on and the frame with it off differ in 0.16% of
pixels by more than 8 levels, less than two runs of the same (0.27%). It
draws in the PSX look, at night (lit by the game's light, not glowing) and
under the snow weather, where the lying snow covers it. As expected of S0:
the lawn ends in a hard edge at the reach, and nothing is drawn along a
ledge's side. **Not checked:** the panel's controls (scripted runs cannot
reach ImGui; the environment variables set the same fields); the cost at 16
to 64 layers directly; the frame with shells off against the commit before
(by the code nothing is drawn and the top is not recoloured); walking, which
the plan named, was one standing view read by distance.

#### S1. Turf: the game's own version

**Here:** what S0 measured, made to sit under the blades.

- Strands are texels, coloured from the top's own picture; the tops under a
  tuft and under a clump get them too, and `bladeGroundShaded`'s dither
  stays as the shade of the taller grass.
- Height from the patches (`bladePatch`: taller where the blades are lush),
  sinking to nothing toward where the blades give way to the sprites.
- 16 layers (S0: 0.3 ms with the prepass, by its estimate). The roots'
  darkening is what made S0's lawn darker with distance; keep it mild, or
  ease it off with distance.
- Wind: the blades' own gust (`bladeNoise` rolling downwind, `u.grass.y`),
  moving a layer by `h^curvature`. It is worked out from each corner's world
  position, not from the cell, so the tops of neighbouring blocks stay joined.
- Fins: along a top's edge where the neighbour is lower or not grass, a strip
  as high as the turf with the edge row's strands, so a ledge seen from
  below or from the side has a ragged pixel edge and not a stack of lines.
  The demo has none.
- Lit through `shade` as the top is, taking the sun's shadow and casting
  none. Under the snow weather the strands whiten as the top does; where
  snow lies, the snow's mesh covers them by depth, and strands show through
  snow thinner than they are tall.
- With the Grass setting's BLADES, and a slider of its own.

**Done when:** looking down, no bare top texture shows on a lawn; walking,
nothing pops or crawls; the lawn is no darker in the middle distance than
near or far; a ledge's edge is ragged from below; the frame cost is written
here.

#### S2. Moss, cloth and hay: shells off any face

**Demo:** the gravity half of its displacement: at rest the direction is
straight down, so hair on the side of the ball hangs.

**Here:** `mossyCobblestone` and its stairs, slab and wall; `clothBlock` and
its shapes; `hayBalde`. Short: a texel long, 4 to 6 layers.

- A mask from the picture: on mossy cobblestone only the green texels grow
  strands, and the stone between them stays flat. The rule per texture
  (length, which texels) belongs in L4's material table; without L4, a
  switch on the block type.
- On side faces the tips hang down by the demo's term.
- The faces come from a list `Chunk::bake` makes, as the grass cells are
  found, within a short radius.
- Edges: shells pushed out of two faces of a cube leave a notch a strand
  long along the edge between them. At one texel it may read as a bevel.
  If not, push along the corner's averaged normal (the demo's ball has
  smooth normals, so it never meets this).
- L5's sheen and this are two answers to the same surface: decide on cloth
  with both in view.

**Done when:** a mossy cobblestone wall is soft on its green texels and hard
on its grey ones, from the front and looking along it.

#### S3. A coat for the cat: fur on creatures

**Demo:** this is the demo: a mesh, its shells, and hair that streams behind
it as it moves.

**Here:** the cat first (`ModelsManager::cat`); the pig, the goblin and the
zombie are bare-skinned. Any furred animal added later takes it for free.

- `vs_model` after skinning, pushed along the normal;
  `instances * shells` instances in one draw.
- The models are boxes with a normal per face, so shells split open along
  every edge of a box. `model.cpp` needs a second, averaged normal per
  vertex for the push; the lighting keeps the face's.
- Strands at the skin's texel grain. Where fur grows and how long (not the
  eyes, the nose, the paws) is a mask picture beside the skin.
- The direction: gravity, plus the creature's velocity lagging behind as
  the script's `displacementDirection` does, kept per creature on the CPU
  and sent in `Model`.
- A handful of creatures on screen, so the fill is small. Skip the shells
  beyond a distance where the coat is under a pixel.

**Done when:** the cat's outline is soft against the sky, its coat streams
back when it runs and settles when it stops, and its face is bare.

#### S4. Trampled grass

**Here:** the snow's tracks are a picture of the ground from above, 16
texels to a block, stamped under every body (`snowCover.cpp`'s `stamp`).
That is the strands' own grain. A second picture of the same kind for grass:
a strand under a footprint stands lower and leans the way the walker went,
and comes back over a minute or so, as snow fills a track again.

**Done when:** a path walked across a lawn shows, and has gone a few minutes
later.

#### Smaller, or trials

- **Leaves.** Three or four shells off a leaf block's faces, each keeping
  fewer of the picture's texels, for a bushier canopy edge. A trial after
  S2: leaf faces are many and are seen at every distance, and with so few
  layers the sparse case of S0's layer check applies (discs, with gaps).
- **Light through the tips.** L6's term for grass on the turf's top layers,
  looking toward the sun.
- **Clouds.** The weather's clouds are a grey laid over the sky
  (`sky.wgsl`), with no shape. A stack of noise layers at cloud height, each
  cut at a threshold that rises away from the middle layer, is the same
  technique and gives clouds a thickness seen from below. Toward the horizon
  it is seen edge on, so it needs its own look at S0's finding and the fog.
  Not examined further. The Volumes section's V5 is the other way to do
  this, and the likelier one.
- **One layer instead of a stack.** The turf could be drawn with no shells:
  march the view ray through the texel grid inside the top face, as parallax
  occlusion mapping does. No lines at shallow angles and one fragment per
  pixel, but nothing rises past the block's outline. Worth a switch in S1
  for the comparison, since both read the same strand heights.

#### Not taking

- **Shells as a band of their own** between the blades and the sprites.
  The finding above: the bare ground is at the feet too, and that is where
  shells show the most.
- **Snow as shells.** `snowCover` is a heightfield: a depth per column,
  pillow edges, tracks pressed in with a rim, melt. A stack of layers has no
  thickness to walk down into. Where the two meet is S1 (strands through
  thin snow) and S4 (the tracks' machinery).
- **Round cone strands** past S0, and **the demo's lighting**: everything
  here is lit by `computePointLightSource`.
- **A draw or an object per shell**, and counts past 16 or so.

### Grass: closing the gaps between the clumps

Started 2026-11-18 from bug snapshot `2026-11-18_15-57-05` (the
`debug-grass` world, a tuft on every block): the bright checkered grass top
showed between the near bush's clumps, and the regular grid of clumps left
straight lanes of ground. The focus for now is **green grass at noon**; the
list below waits until that looks right.

**What is in.** In
[`chunkFaces.wgsl`](../resources/shaders/webgpu/chunkFaces.wgsl):
- The ground under **green** grass takes the roots' colour
  (`grassRootColour`, a dark olive), keeping its own texels' light and dark
  against the picture's mean (`grassGroundTint`). Under a tuft at every
  distance; on a bare top while the short grass is drawn there. Only where
  the top's picture is green (`pictureGreen`, green over red and blue in the
  mean: the grass top 2.1, the snowy top 0.8, dirt 0.5); yellow grass and
  snow keep their own tops.
- The mean is true: the block texture array now has mips down to 1x1
  (`blockTextureLevels` 8 in `wgpuWorld.cpp`), read by `textureLoad`
  (`pictureMean`). The sampler still stops at level 4, so faces draw as
  before. (The first version read the middle texel of the 8x8 level, 1/64
  of the picture: 1.21 times the mean for the grass top, 0.30 for the snowy
  top.)
- The near bush's and far cards' feet fade to that same colour over the
  bottom `grassRootHeight` (0.32 blocks) (`grassRootFade`), only on the
  plain tuft's sprite (`grassSpriteGreen`, by slot: the snow tuft's picture
  is as green on average as the plain one, so its colour can't tell them
  apart).
- A tuft sits up to 0.1 off the block's middle and 1.0 to 1.25 times as
  wide, each its own way, by one hash: the near bush (`bushVertex`) and the
  far cards (`faceVertex`) alike, so rows aren't straight past the reach and
  a tuft doesn't jump at the hand-off. The short grass's jitter went from
  0.03 to 0.12 (±0.06).
- Not needed: bending the cards' normals up. `sunFacing` already lights
  both kinds of card by an up normal (both carry `plantSpriteFlag`).

Checked at noon in two views: the snapshot's, and `test2` at (179, 70, -26)
looking down, where the yellow grass top shows untinted between the cards.
A review of the first version found the tint on every grass top in every
world (dark green squares under `test2`'s yellow tufts), the false mean and
the far cards' straight rows; all three are fixed above.

**Later:**
- **Distance.** Look at a tuft field from far: the far cards on the tinted
  ground and the hand-off from the near bush. The light band toward the
  horizon in the snapshot's view is the far cards, not ground (the ground
  under them is tinted): the sprite's colour against the card atlas's. It
  is the hardest line in that view now. The near bush keeps a 0.075 shift
  (`clumpModelCentre`) the far cards don't have.
- **Yellow and snow tufts up close.** The near bush and the short grass are
  the green card atlas whatever the cell's sprite, so a yellow lawn turns
  green in a ring at the grass's reach (older than this work). Either a
  yellow and a snowy card atlas, or the atlas tinted toward the sprite's
  mean. Then those tufts get a root colour and a ground tint of their own.
- **Other light.** Night, rain (wet ground and puddles over the tint), and
  point lights on the tinted ground and the faded feet.
- **Short grass on bare tops.** One view (`test2`, looking down) shows no
  lines of ground at block edges with the larger jitter; not looked at
  otherwise.
- **The empty lane.** Before the change a one-block lane ran from under the
  camera to the horizon along -X with no grass in it; after it, bushes
  stand in it. Not explained. What is known: the blocks were always there
  (all 8,408 saved chunks of `debug-grass` are byte for byte the same,
  including those saved two minutes before the snapshot), so it was the
  drawing; and the jitter did not hide it (a neighbour's bush reaches about
  0.7 blocks from its middle, too little to cover a row). The snapshot was
  from the 15:47 build; `wgpuWorld.h` changed at 16:06 and the game was
  rebuilt at 16:10, so the fix may be in that change rather than this one.
- **Tuning.** The root colour is picked by eye, not taken from the card
  atlas; the fade's strength (0.9), the tuft's jitter and width, and the
  green test's 1.15 to 1.6, are constants. The fade's height is 0.32 blocks
  whatever the card's height: short-grass copies are 0.23 to 0.78 blocks
  tall, so the shortest never reach their own colour. Scale it by the
  copy's height. Panel sliders, or the colour measured from the cards'
  lower rows.
- **Any turn, not right angles.** The corner table stores the bush turned
  four ways so the wind stays one direction. A free turn needs the sway
  applied after the turn, in the world's frame.
- **Filler, if gaps remain.** S1's turf under the clumps, or a few short,
  wide cards at the roots. Only after the colour work, since filler of a
  different colour shows as much as the ground did.
- **Cost.** The tint reads the ground picture's 1x1 mip once per ground
  fragment under grass. The mips below 8x8 add about 0.1% to the block array's
  memory. Not timed.

Each of these becomes a switch or a slider in the grass test bench (G1).

### Grass test bench: techniques and their costs

Ideas; nothing is built. Drafted 2026-11-18. The `debug-grass` world becomes
a lab, not a level: one flat field, fixed camera bookmarks, noon held, the
wind under control, and every grass technique a switch in a panel, so that a
look and its cost can be compared side by side. The aim is to learn what
each technique buys and what it costs, toward grass as convincing as Ghost
of Tsushima's or Breath of the Wild's, worked out from their published
techniques, with this game's own models and pictures. Nothing of either
game's is copied.

**Pixel pictures on realistic shapes.** They can go together: the look of
grass is mostly its shape, light, density and motion, and those survive a
coarse picture. The rule is the blocks' texel density, 16 texels a block
across every model, sampled nearest; a 1024² atlas next to 16-texel dirt
reads as pasted in. Detail goes into the geometry and the light. Lighting
snapped to the texel grid (as `bladeGroundShaded`'s dither is) keeps the two
from clashing. The bench keeps both kinds of picture as a switch, to see it.

**The switches,** each on its own so they can be mixed:
- Geometry: none, the card bush, the generated blades (`vs_blade`), curved
  blades made on the GPU (G3), shells (S0); and per band (near, middle,
  far), which of them.
- Picture: pixel (16 texels a block), the card atlas, none (a colour from
  root to tip).
- Light: the up normal, the blade's own, a mix; rounded blade normals; light
  through the blade (L6); a sheen; darkening by height; the ground tint;
  grass casting shadows; grass shadowing itself.
- Field: density, reach, clumping, colour variation (dry blades), jitter,
  the hand-off distances.
- Wind: off, each blade's own sway, gusts rolling across the field.

| # | Milestone | Depends on | Type |
|---|---|---|---|
| G0 | The bench: GPU timing, bookmarks, presets, a contact sheet | | Learning |
| G1 | What exists, as switches | G0 | Learning |
| G2 | Pixel blades at the blocks' texel density | G1 | Mixed |
| G3 | Curved blades made on the GPU, clumped | G1 | Learning |
| G4 | Colour from root to tip, gusts, light through | G3, L6 helps | Learning |
| G5 | Bands: a technique per distance | G2 or G3 | Mixed |

#### G0. The bench

**Here:** the measuring, before any technique, or every comparison after it
is a guess.

- **GPU time per pass, in the panel.** There are no GPU timestamps today,
  and the frame time is held to vsync (even with `PresentMode::Immediate`,
  a light scene reads 13.34 ms on this Mac). WebGPU's timestamp queries
  (feature `TimestampQuery`, a query set, `timestampWrites` on a pass,
  resolved and read back with `serviceReadback`). Apple's GPUs time whole
  passes, not draws inside one, and the grass draws share the opaque pass:
  either the grass gets a pass of its own (loading what the pass before
  left) while the bench is on, or its cost is the opaque pass with it less
  without it. Decide by trying the first.
- **Counts:** the grass's triangles, instances and drawn fragments (an
  atomic, as the sun's count is), next to the times.
- **Bookmarks:** a few fixed views on the field (eye height looking level,
  looking down, the horizon), a key or a menu to jump between them, and the
  time of day and the wind held (`OURCRAFT_DAY_TIME`, a still wind).
- **Presets:** every switch saved by name in a file beside the world
  ("Cards", "Pixel blades", "Curved blades", ...), loaded from the panel or
  `OURCRAFT_GRASS_PRESET`.
- **The contact sheet:** a scripted run renders every preset from every
  bookmark into one picture, each tile with its GPU time and counts under
  it.

**Done when:** the card bush's time on and off can be read in the panel and
the difference is steady to a tenth of a millisecond; one command makes a
contact sheet of two presets.

#### G1. What exists, as switches

The card bush, the generated blades, the shells, the ground tint, the root
fade, the jitter and the far cards, each switched on its own and its band
chosen, through G0's presets. Some of these are tied together now (the bush
replaces the blades when it is loaded; the shells need the blades' cells):
untie them.

**Done when:** a contact sheet of each technique alone, with its time,
is in this section.

#### G2. Pixel blades at the blocks' texel density

Blades shaped as realistic blades (a taper, a bend, a twist) but coloured a
texel at a time from the grass's 16-texel picture, as `vs_blade` takes
one texel now. Their light snapped to the texel grid, as a switch.

**Done when:** at eye height the field reads as grass and as part of the
block world at once; the contact sheet against the card atlas is here.

#### G3. Curved blades made on the GPU, clumped

The technique Sucker Punch described for Ghost of Tsushima (GDC 2021,
"Procedural Grass in Ghost of Tsushima"): each blade a curve (Bézier) made
on the GPU from a seed, many to a block, grouped into clumps that share a
lean and a height; fewer vertices per blade with distance; a normal rounded
across the blade so a flat strip lights as a curved one. In WGSL, made by a
compute pass into a buffer the draw reads, or worked out in the vertex
stage as `vs_blade` does: try both, it is a cost question.

**Done when:** a field of them at eye height, its time against the card
bush at the same density of cover.

#### G4. Colour from root to tip, gusts, light through

The look Breath of the Wild is known for: little or no picture, a colour
running from a dark root to a light tip, large waves of wind rolling over the
field that brighten the blades they bend, and the sun through the blades when
looking toward it (L6). Mostly shader terms on G3's blades.

**Done when:** gusts are seen to travel across the field from a bookmark;
looking toward the sun the tips glow; the time each term adds is here.

#### G5. Bands: a technique per distance

Near, middle and far each take their own technique and density (curved
blades near, cards or pixel blades in the middle, the ground tint and far
cards beyond), and the hand-offs dissolve as `spriteDissolved` does now.

**Done when:** walking, no band's edge is seen; a preset's time is below the
near technique's alone at full reach.

#### Not taking

- **Either game's assets or look copied whole.** The techniques are
  published; the models, pictures and colours stay this game's.
- **Grass in the game's settings menu** before the bench has picked what
  works: the bench's panel is for trying, not for players.

### Volumes: ideas from Acerola's CS2 smoke

Ideas; nothing is built. Drafted 2026-11-16 from
`~/game-dev/CS2-Smoke-Grenades` (Acerola's Unity demo of Counter-Strike 2's
smoke grenade; no license file, so a companion to read and not text to port)
and a read of the fog (`computeFog` in
[`chunkFaces.wgsl`](../resources/shaders/webgpu/chunkFaces.wgsl)), the god
rays ([`godRays.wgsl`](../resources/shaders/webgpu/godRays.wgsl)), the lying
snow ([`snow.wgsl`](../resources/shaders/webgpu/snow.wgsl)) and how enemies
notice a player (`basicEnemyBehaviour.h`). The aim is to learn volume
rendering by building it in WGSL, and to keep it where this game has
something only a volume can show.

**What the demo is.** Three parts, about 900 lines.

- **The fill** (`Voxelizer.cs`, `Voxelize.compute`). A grid of quarter-unit
  cells. The level's meshes are voxelized into a "solid" grid once (a
  triangle-box test of every cell against every triangle). The smoke's seed
  cell is set to 16. Each pass, an empty cell takes its six neighbours'
  largest value less one; a solid cell beside smoke is set to 1, which stops
  the spread there. After n passes a cell holds 16 less the length of the
  shortest way to it from the seed, going round walls. An ellipsoid caps the
  reach.
- **The march** (`RenderSmoke.compute`). A compute pass over every pixel of a
  full, half or quarter size target. It steps along the view ray until it
  finds a smoke cell, then takes up to 64 steps of 0.05. At each step the
  density is the cell value read with a hand-written trilinear blend, thinned
  toward the fill's front and the ellipsoid's edge, and eaten away by a tiled
  128³ Worley noise that scrolls. Eight more steps toward the light give the
  smoke's own shadow. A phase function (Henyey-Greenstein, Mie or Rayleigh)
  weights the light by angle. Bullet holes are a list of capsule distance
  fields that cut the density.
- **The composite** (`CompositeEffects.shader`). A 9-tap Catmull-Rom
  upsample, a sharpen, and a mix over the frame by the smoke's mask.

**Findings in the demo.** Take the math from a reference (Wrenninge's
*Production Volume Rendering*, or the Frostbite and Nubis course notes) and
read the demo for the shape of the thing.

- The transmittance is `exp(-thickness * accumDensity * extinction)`, where
  `thickness` and `accumDensity` are both running sums of the density along
  the ray. The exponent grows with the square of the way through the smoke.
  Beer-Lambert has one sum: `T *= exp(-sigma * density * step)`.
- The light added at a step is not multiplied by the step's length, so
  halving the step doubles the brightness. The colour also starts at the
  smoke's own colour and not at black, and is clamped to 1.
- The light is always overhead. The shadow march goes straight up and the
  phase function is given the angle to straight up; `_SunDirection` is
  declared and never read.
- The distance to the scene comes from the x components alone
  (`-((origin - sceneWorldPos) / rayDir).x`), which fails as the ray turns
  along z.
- To find the smoke, every pixel steps 0.4 at a time for up to 200 units,
  sky included. A ray against the grid's box gives where it enters and
  leaves in six divisions.
- A pixel that crosses the smoke at the default settings makes about 4600
  buffer reads and 640 noise reads (64 steps, each 8 cell reads plus 8 light
  steps of 8 more). The README's own word for the bullet holes is "not
  performant".

**What WebGPU changes.** These are the parts worth learning here.

- **3D textures.** New to this renderer: `texture_3d<f32>`, a linear
  sampler, and repeat addressing for a noise that tiles. The demo's
  trilinear blend of eight buffer reads becomes one `textureSampleLevel`.
  Inside a march loop it is always `textureSampleLevel`: `textureSample`
  needs uniform control flow, as elsewhere in these shaders.
- **Formats.** `r8unorm`, `rgba8unorm` and `rgba16float` can be filtered;
  `r32float` cannot without the `Float32Filterable` feature. A compute
  shader can write `rgba8unorm` and `rgba16float` storage textures; `r8unorm`
  is not a storage format in core WebGPU. So a noise baked by compute goes
  into `rgba8unorm`, which is a use for the other three channels: four
  frequencies of Worley, mixed in the shader. To check against the vendored
  headers and the validation layer, not taken from memory.
- **No texture read and written in one pass.** The demo's fill writes a
  second buffer and copies it back each pass. Here two textures and two bind
  groups swap, as the bloom's levels do.
- **A fragment pass over the box, not a compute pass over the screen.** Draw
  the grid's box with its front faces culled and no depth test, so only the
  pixels the box covers run, and the camera may be inside it. The pass has
  no depth attachment, so the scene's depth can be bound as
  `texture_depth_2d` and read with `textureLoad`, as the god rays do. The
  output is premultiplied (`One, OneMinusSrcAlpha`) with `1 - T` as alpha.
- **Banding.** Few steps show as rings. The start of each ray is moved by a
  fraction of a step from `bayer4` or the tone mapper's gradient noise. With
  no temporal filter the offset stays visible as a dither, which is this
  game's grain already.
- **Cost** is steps times reads per pixel, at whatever size the target is.
  Measured with the present mode Immediate and the camera inside the smoke,
  where the box covers the screen.

**What this game changes.**

- **The world is the grid.** No voxelizer: a block is solid or not. The
  fill is a breadth-first walk of at most a few thousand blocks on the CPU,
  run again when a block beside the smoke changes, so smoke pours through a
  wall the moment it is broken. Every client runs the same fill from the
  same blocks; the server sends only a seed, a tick and a size.
- **The light is baked.** Every block has a sky level and a block-light
  level (`Block::getSkyLight`, `getLight`). The fill can carry both into the
  grid's spare channels, and the smoke is lit by the faces' own ambient
  formula: orange round a `goblinTorch`, dark in an unlit stair, white
  outdoors. That takes the place of most of the shadow march, which is eight
  ninths of the demo's cost.
- **The look.** Soft bicubic smoke would sit oddly beside dithered foam and
  PSX snap. Opacity in a few bands with dithered edges (as
  `heldLight.wgsl`'s bands), and noise read at 16 texels to a block, is the
  version to try against the smooth one.
- **It can mean something.** `basicEnemyBehaviour.h` gives an enemy a chance
  to notice a player from distance and from the angle to where it looks; a
  read found no test of what stands between them. A smoke's grid on the
  server is such a test.

| # | Milestone | Depends on | Type |
|---|---|---|---|
| V0 | The demo in WGSL: one smoke, with step views | | Learning |
| V1 | The game's smoke: baked light, the game's grain | V0 | Mixed |
| V2 | Goblins' smoke pots | V1 | Game-specific |
| V3 | A window of the world's light as a 3D texture | | Mixed |
| V4 | Mist: fog that lies in the world | V3 | Mixed |
| V5 | Clouds | V0 | Mixed |

#### V0. The demo in WGSL: one smoke, with step views

**Demo:** the fill, the march and the phase functions, with the findings
above corrected and the demo's own shadow march kept for now.

**Here:** a chat command beside `/tp` puts a smoke at the block under the
cursor. No gameplay.

- The fill on the CPU, two cells to a block, into an `r8unorm` 3D texture
  about 32 blocks across, with `writeTexture`.
- The Worley noise baked once by a compute pass (the pattern is
  `blockTextures.wgsl`'s), four frequencies in `rgba8unorm`.
- The box pass after the glass and the water and before the exposure and
  the bloom, onto the HDR colour at full size. The march ends at the scene's
  depth. To look at: whether the water writes depth, and what smoke behind
  glass and under water does.
- **Views:** steps taken per pixel, the transmittance alone, the fill's
  cell values as cubes, a slice of the noise.
- **Cost:** frame time with the camera inside the smoke, at 16, 32 and 64
  steps and at 0, 4 and 8 light steps.

**Done when:** a smoke fills a goblin tower's room, goes round a corner and
out of the door and not through the wall; walking through it shows no rings
and no hard edge at the box; the cost table is in this file.

#### V1. The game's smoke: baked light, the game's grain

**Here:** what V0 measured, made cheap enough to keep and made to look like
this game.

- The grid becomes `rgba8unorm`: density, sky light, block light, and when
  the fill reached the cell. The arrival time lets the shader grow the smoke
  outward and ease each cell in, with no upload per tick.
- Lit from the two light channels as the faces are. For the sun, a march of
  two to four steps toward where it is, only where the sky level is high.
- The fill spends a number of cells and not a number of steps, so a narrow
  stair fills further than an open room. A step up costs less than a step
  sideways, and a step down more: the smoke pools under a ceiling and leaves
  by a hole in the roof. Cells open to the sky thin faster.
- A half size target and an upsample that looks at depth: each full size
  pixel takes, of the four low ones round it, the one nearest its own depth.
  The demo's Catmull-Rom smears smoke over anything standing in front of it.
- The banded, dithered version beside the smooth one, on a switch.
- Mixed toward the sky copy by `computeFog` at its near face, so a far
  smoke fades as the terrain does.
- Up to four at once, each its own grid, drawn far to near. Two that
  overlap blend wrongly; accepted.

**Done when:** a smoke beside a goblin torch is orange on that side; a hole
knocked in the wall lets it out; a goblin in front of it has no halo; the
camera inside it costs under 2 ms at 1080p, or the number is written here
with why.

#### V2. Goblins' smoke pots

**Demo:** the reason the grenade exists: it blocks sight.

**Here:**

- A pot item. A goblin that has noticed a player from a distance throws one
  between them and closes in under it; a goblin tower drops them when
  alarmed. The player can take and throw them too.
- The server keeps each smoke's grid. The chance of being seen is scaled by
  the transmittance along the line from the enemy's eyes to the player, so
  it hides both sides. Hearing is unchanged.
- Arrows are the bullet holes: an arrow's path cuts a capsule that closes
  over a second or two. At most four, each tested against its bounds before
  its distance. A `flamingArrow` lights the smoke from inside for as long as
  it is in it.

**Done when:** a goblin loses a player who steps into smoke and finds them
again when they leave it or make a noise; a goblin's own pot covers its
approach; an arrow leaves a tunnel that the far side shows through.

#### V3. A window of the world's light as a 3D texture

**Here:** V1 gives each smoke its own copy of the light. Fog needs the light
everywhere near the camera: a 3D texture of about 96 by 64 by 96 blocks
(`rgba8unorm`, 2.4 MB): solid or not, sky level, block-light level. It wraps
as the snow's tracks do, so moving the camera uploads only the slab that
came into range, and a chunk's bake updates its part.

Other things could read it once it exists: the weather's particles, the
creatures (lit by one factor each today, as milestone 12 left them), and
anything else drawn in the air.

**Done when:** a view shows a slice of it at the camera's height, right
after a torch is placed and after walking a hundred blocks.

#### V4. Mist: fog that lies in the world

**Here:** `computeFog` is a haze by distance toward the sky copy, the same
in a cave as on a hill, and the god rays are a blur on the screen that only
exists while the sun is in view. Mist is the near field: the first 48 blocks
or so, handing over to `computeFog` beyond.

- **Froxels, not a march per pixel.** A 3D texture laid along the view
  (about 160 by 90 by 64, `rgba16float`). One compute pass writes each
  cell's density and the light scattered in it; a second sums them front to
  back. Every surface then reads its own depth's cell with one sample: the
  opaque faces, the water, the glass and the particles alike, which a march
  that ends at the opaque depth cannot do. Mist is soft, so the coarse grid
  holds. Smoke has edges and keeps its own box.
- **Where it lies:** over water and in hollows at dawn (the weather's column
  map has each column's height and kind), thicker under rain and as the
  whiteout of heavy snow, and low in caves where the sky level is 0.
- **What lights it:** the sky level, so none glows inside a sealed room; the
  block light, so a torch has a halo in it; and with the `shadows` setting
  on, the sun through the cascades, which gives shafts under trees and
  through a window whether or not the sun is on screen. `shadows` is 0 by
  default, so the shafts are the part that may not be seen.

**Done when:** a river at dawn has mist on it that a hill's shadow cuts;
a torch in a wet cave has a halo; glass and water in the mist take it
correctly; there is no seam where `computeFog` takes over; the cost is
written here.

#### V5. Clouds

**Here:** this replaces the Shells section's trial (a stack of noise layers).
A slab between two heights, entered and left by arithmetic; V0's noise with
one coarser frequency for the shapes; a march at quarter size into the sky
copy before the world is drawn. `computeFog` mixes far terrain toward that
copy, so hills fade into the clouds behind them with no further work.
Clouds are far and slow: a sixteenth of the pixels a frame, the rest
carried over, is the usual way to afford them. The weather sets the cover,
in place of the grey laid over the sky. The same cover, read flat where the
sun's ray meets the cloud height, is the clouds' shadow on the ground in
`shade`.

**Done when:** clouds have undersides and lit edges, thicken as rain comes
in, and their shadows cross the ground with them. Not examined further.

#### Smaller, or trials

- **A drift against walls.** The suggestion on the table was a small 3D
  occupancy beside vertical faces, meshed as a skirt or an isosurface. A
  drift banked against a wall is still one height at each point of the
  ground, and `snowBase` already shapes the depth inside a block from its
  neighbours: where a neighbour is a wall, let the depth rise toward it in
  the same quarter circle the pillow edge falls by. Two walls give a corner
  twice as deep, and the weather's wind picks the lee side. That is the
  whole request with no grid, and it is the first thing to try. What a
  height per column cannot hold is a second level (snow under a tree or a
  bridge as well as on it), snow on shapes that are not a block's top
  (fences, slabs, stairs), and snow plastered on a trunk's windward side. A
  second layer in the column map answers the first. An occupancy grid and
  surface nets answer all three, as a meshing technique to learn for itself;
  it shares V1's fill and nothing of the march.
- **A furnace's smoke.** V1's rising fill from a working furnace: a room
  with no chimney fills from the ceiling down.
- **Gas.** The same smoke in another colour with an effect on whoever
  stands in it, for a goblin shaman.
- **Under water.** V4's cells with the underwater colour, and shafts from
  the surface.
- **Wisps, breath, dust, the mist at a river step.** Particles (the
  weather's machinery), not volumes: each is smaller than a grid cell worth
  marching. They gain from V3's light and from fading where they meet the
  scene's depth.

#### Not taking

- **The mesh voxelizer**, and **the fill on the GPU** a pass a tick with a
  copy back.
- **The search for the volume by stepping**, and **a compute pass over the
  whole screen.**
- **The demo's transmittance, its overhead light and its depth from x.**
- **The Catmull-Rom upsample and the sharpen.**
- **A list of holes** tested at every step past a few capsules.
- **A fluid simulation.** Velocity and pressure on a 3D grid is the real
  thing and another project. The fill and a scrolling noise are what a
  smoke that blocks sight needs.
- **A volume per torch** or per small effect.

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
