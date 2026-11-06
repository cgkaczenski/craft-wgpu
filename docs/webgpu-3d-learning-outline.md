# WebGPU 3D learning outline

A table of contents for the 3D half of the render port: milestones 5 through
20, from the first depth buffer to the day OpenGL left the tree. Each section
is one concept cluster: what the concept is, the LearnWebGPU chapter for it,
where it landed in this repo, and the commit that introduced it. The
companion file, [`webgpu-learning-outline.md`](webgpu-learning-outline.md),
is space-game's outline for the 2D stack that `wgpu2d` brought with it;
nothing in it is repeated here. The plan these milestones came from, the
conventions they settled, and the per-milestone verification notes are in
[`roadmap.md`](roadmap.md).

The 3D side lives in one translation unit, `src/gameLayer/rendering/wgpuWorld.cpp`
(about 5,200 lines by milestone 20), behind a header that names no WebGPU
types (`include/gameLayer/rendering/wgpuWorld.h`), so `renderer.cpp`,
`chunk.cpp`, `model.cpp` and `glfwMain.cpp` call it without compiling
`webgpu.hpp`. Fourteen WGSL files under `resources/shaders/webgpu/`, each
with a header comment naming the GL shader it was ported from. `wgpu2d`
still owns the device, the surface and the frame; the world borrows them
through `thirdparty/wgpu2d/include/render/wgpuFrame.h`.

**The rule these milestones followed:** port _behaviour_, not structure. The
GL renderer's data stays exactly what it was — the 16-byte face `Chunk::bake`
writes, the corner and UV tables `recreateBlockGeometryData` builds, the
texture table `BlocksLoader` fills — and only what the GPU does with it
changes. That is why the game code changed by a handful of lines that hand
the same vectors to `wgpuWorld`, and why, when the GL path was deleted in 19,
`renderer.cpp` shrank from 7,056 lines to 2,846 and kept working.

Guide: [Learn WebGPU for C++](https://eliemichel.github.io/LearnWebGPU/index.html),
**With webgpu.hpp** tab. The guide builds one textured, lit mesh viewer; the
world here is tens of thousands of faces a frame from one buffer, a G-buffer,
shadows and a post chain, so the guide covers the _objects_ (depth texture,
projection, storage buffer, compute pass, cubemap) and this file covers what
the game does with them. Where the guide has no chapter, or a 🔴 TODO stub,
the section says so and points elsewhere — mostly
[WebGPU Fundamentals](https://webgpufundamentals.org/), which is JavaScript
but describes the same API; [LearnOpenGL](https://learnopengl.com/) for the
rendering _techniques_ (shadow maps, bloom, SSAO, deferred shading), which
are API-independent and which the GL code this was ported from mostly
follows; and the [WebGPU](https://www.w3.org/TR/webgpu/) and
[WGSL](https://www.w3.org/TR/WGSL/) specs for the rules.

_Sections 5–7 were written when the GL path still existed and name files it
had (`nullGL.cpp`, `#if RENDERER_WEBGPU`). Those are history now (19); the
notes in italics say what replaced them._

---

## How to read this

Each block is: **concepts → LearnWebGPU chapters → where it landed → commit**.
Sub-blocks expand one idea when the milestone taught more than one. Numbers
in the text (draw counts, layer counts, microseconds) are what the build
printed on this machine in the `test`, `test2` and `appletest` worlds, not
estimates. "GL" throughout means the OpenGL renderer this was ported from,
which is at the `opengl-final` tag.

---

## 5. First 3D pass: depth and the game's camera

**Concepts:** The whole of 3D that 2D never needed, in one milestone and one
shader: a **depth buffer** so near things hide far things whatever order they
draw in; a **perspective projection** that maps the camera's frustum to clip
space; **back-face culling** by winding; a world pass that shares the frame
with the 2D pass already there. Two test cubes, no vertex buffer, every corner
built from `vertex_index`. Deliberately throwaway — the shader was deleted in
6 — because the point was to settle conventions on something small enough to
check by hand.

**LearnWebGPU:** [A simple example](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/a-simple-example.html) · [Depth buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/depth-buffer.html) · [Transformation matrices](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/transformation-matrices.html) · [Projection matrices](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/projection-matrices.html) · [Camera control](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/some-interaction/camera-control.html) (the guide's camera; here the game's `Camera` is reused)

**Code:** `ensureDepthTexture` · the `DepthStencilState` in `createPipeline` · `Uniforms` and the projection in `render` · `wgpuBeginSurfacePassWithDepth` in `thirdparty/wgpu2d/src/wgpuContext.cpp` · `Renderer::renderFromBakedData` (the one call site) · `wgpuWorld::shutdown` beside `wgpuImgui::shutdown` in `glfwMain.cpp` · the deleted `resources/shaders/webgpu/testCubes.wgsl` (read it with `git show 6098f85:resources/shaders/webgpu/testCubes.wgsl`)

**Commit:** `6098f85`

### 5a. The Z-buffer is a texture you own, and a pipeline state you set

A 2D sprite batch resolves overlap by draw order. A 3D scene cannot: faces of
one cube overlap each other, and which one is in front changes with the
camera. The GPU's answer is the **Z-buffer**: a second render attachment, one
value per pixel, holding the depth of the nearest fragment drawn so far. Each
new fragment's depth is compared against it, and the fragment is kept only if
the comparison passes. It is a fixed-function stage, so like blending (space-game 14) it is configured, not programmed, and the configuration lives in the
pipeline.

Three pieces, each in a different place:

- **The texture** — `Depth32Float`, `RenderAttachment` usage, sized to the
  surface in framebuffer pixels. The size must match the colour attachment
  exactly, so `ensureDepthTexture` is called every frame and remakes it when
  `wgpuSurfaceSize` changes. The view is `TextureAspect::DepthOnly`. The guide
  uses `Depth24Plus`; this port uses `Depth32Float`, a depth-only format with
  no stencil aspect, which is what the `Undefined` stencil ops below are
  about.
- **The pipeline's** `DepthStencilState` — `depthCompare = Less`,
  `depthWriteEnabled = True`, `format = Depth32Float`. The stencil halves are
  set to `Always` / `Keep` with masks of 0, which is how you say "no stencil"
  to a state that always describes both. A pipeline with a depth state can
  only be used in a pass that has a depth attachment of that format, and vice
  versa; this is what makes 5c necessary.
- **The pass's** `RenderPassDepthStencilAttachment` — `depthLoadOp = Clear`
  to 1.0 (the far plane), `depthStoreOp = Discard` because nothing reads the
  depth after the pass yet. `stencilLoadOp` **and** `stencilStoreOp` **stay**
  `Undefined`: `Depth32Float` has no stencil aspect, and giving it stencil
  ops is a validation error. The guide's text shows `Clear`/`Store` and then
  notes that Dawn wants `Undefined`; wgpu-native v24 agrees with Dawn.

### 5b. The conventions, and why they had to be decided here

The guide's projection chapter builds its matrices by hand. This port uses
glm, so the decision is which glm function. Every one of these was a choice
with a wrong answer that looks right on a test cube and wrong three milestones
later; the full list is in `roadmap.md` under "Conventions (decided in
milestone 5)".

- **Depth 0..1, not −1..1.** OpenGL's clip-space z runs −1..1 and
  `glm::perspective` produces that. WebGPU's runs 0..1 (as Metal, Vulkan and
  D3D do). The glm function is `perspectiveRH_ZO` — right-handed, zero-to-one
  — with the game's `camera.fovRadians` (70°), `closePlane` (0.01) and
  `farPlane` (800), and the surface's aspect. `Camera::getProjectionMatrix`
  stays −1..1 because the GL path and the CPU frustum culling use it; the
  world builds its own. The consequence that will bite later: every ported
  screen-space shader that rebuilds a position from depth must drop its
  `depth * 2.0 - 1.0`. Reversed-Z (near at 1, far at 0, which spends float
  precision where perspective division wastes it — see
  [Depth Precision Visualized](https://developer.nvidia.com/content/depth-precision-visualized))
  was considered and not taken, to keep each ported shader one step from the
  GL one rather than two.
- **Clip-space y points up**, as in GL. So the projection needs no flip, and
  a triangle counter-clockwise in GL is counter-clockwise here:
  `FrontFace::CCW`, `CullMode::Back`, and `testCubes.wgsl`'s corner table is
  written counter-clockwise _seen from outside the cube_, so only the outside
  of each face draws.
- **Render targets are top-first**, textures are not flipped. Row 0 of a
  render target is the top in WebGPU and the bottom in GL, which matters for
  screen-space shaders (`uv.y = 0.5 - 0.5*ndc.y`) and `@builtin(position).y`.
  Sampled textures are the same in both APIs — memory row 0 is v = 0 — so
  block textures are uploaded bottom-first as the GL path did and every mesh
  UV carries over unchanged. 7c leans on this.

###  5c. A pass of your own inside someone else's frame

`wgpu2d` owns the frame: one command encoder, one surface texture, a 2D pass
it opens lazily and ends at `wgpuEndFrame`. The world needs a pass with a
depth attachment, and the 2D pipelines have no `DepthStencilState`, so they
**cannot be used inside a pass that has one** (5a). Passes also cannot nest.
So `wgpuBeginSurfacePassWithDepth` ends the open 2D pass if there is one,
begins a new pass on the same surface view with the depth view attached, and
hands it to the caller, who records, ends and releases it. The next 2D draw
opens a fresh 2D pass that **loads** what the world left.

The colour attachment's load op is `Clear` only if nothing has cleared the
surface this frame (`g.surfaceCleared`), otherwise `Load` — the same rule
`ensurePassBegun` applies to the 2D pass (space-game 10). The world is called
from `renderFromBakedData`, which runs before the HUD, so in practice it
clears and the 2D layer draws over it.

_Superseded in 11: the world no longer draws on the surface at all. It draws
into the G-buffer on the frame's encoder (`wgpuFrameEncoder`, added in 9),
and a composite pass puts the result on the surface inside wgpu2d's 2D pass.
`wgpuBeginSurfacePassWithDepth` is what the first three milestones ran on._

### 5d. Geometry from `vertex_index` and `instance_index`

Neither test cube has a vertex buffer. `draw(36, 2)` runs the vertex shader
72 times; `vertex_index / 6` is the face, `vertex_index % 6` indexes a six-entry
table that turns four corners into two triangles, and `instance_index`
picks which cube's offset to add. This is the guide's Hello Triangle trick
(positions in a constant array) taken one step further, and it is a rehearsal
for 6: the chunk shader builds its corner the same way, from a table, indexed
by `vertex_index`.

### 5e. Camera-relative positions: an integer and a fraction

A block at x = 30,000 has about 2 mm of float precision; after a projection
matrix multiplies it, that is visible jitter. The GL renderer avoids it by
never giving the GPU a world position: the CPU splits the camera position
into an integer part and a fraction (`Camera::decomposePosition`, in
doubles), both go in uniforms (`u_positionInt`, `u_positionFloat`), and the
shader subtracts the integer part from each face's **integer** block position
first — exact, however far out — and the fraction second, when the number is
already small. The view matrix is then rotation only (`Camera::getViewMatrix`),
and `viewProjection = projection * view` has no translation in it. 5 does
this for two cubes; 6 does it for every face, and `chunkFaces.wgsl`'s
`relative = vec3f(blockPosition - u.positionInt.xyz) - u.positionFloat.xyz + shape`
is the whole trick in one line.

**The uniform struct must match WGSL's layout.** A `vec3` in a uniform block
is padded to 16 bytes, so both positions are `vec4`: `mat4x4f` + `vec4i` +
`vec4f` = 96 bytes with no gaps, and the C++ side `static_assert`s it. The
rules are in [Memory Layout](https://webgpufundamentals.org/webgpu/lessons/webgpu-memory-layout.html);
the guide covers them in [A first uniform](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/shader-uniforms/a-first-uniform.html).

### 5f. How it was checked without a reference image

There is no GL world on this Mac to compare against (roadmap, open question
1), so each claim was checked on its own terms. Face colours are per axis
(x red, y green, z blue; the negative side dark), so a screenshot says which
faces are showing — and only outside faces showed, so culling is right. The
nearer cube stays in front though it is drawn first, so depth is right. The
cubes' screen-space bounds in a screenshot matched a CPU projection of the
same corners to the pixel, before and after a scripted camera turn, so the
matrix is right. The scripted turn is why `OURCRAFT_INPUT` gained `move@x,y`
and `key@F9`: a fixed scene rendered to a PNG with nobody at the keyboard,
which is the only kind of check that can be rerun.

---

## 6. Chunk faces: instancing, storage buffers, and one buffer for the world

**Concepts:** The real geometry. Every visible block face is one 16-byte
record, every chunk's records sit in one GPU buffer, and a face is drawn as
one **instance** of a four-vertex **triangle strip** whose corners come from a
**storage buffer** table. One `draw` per visible chunk. This is `BigGpuBuffer`'s
design — which the GL path has switched off, so this is the first arena that
actually runs — done with WebGPU's objects. Unlit: a fixed shade per
orientation and a flat colour hashed from the texture index, until 7.

**LearnWebGPU:** [A first Vertex Attribute](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/a-first-vertex-attribute.html) and [Multiple Attributes](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/multiple-attributes.html) (vertex layouts; the guide only ever uses per-vertex step mode) · [Instanced Drawing](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/instanced-drawing.html) — a 🔴 TODO stub, so see [Vertex Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-vertex-buffers.html) (its `stepMode: 'instance'` section) · [Compute Pipeline](https://eliemichel.github.io/LearnWebGPU/basic-compute/compute-pipeline.html) is where the guide first binds a storage buffer; [Storage Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-storage-buffers.html) covers reading one from a vertex shader

**Code:** the `VertexBufferLayout` and `PrimitiveState` in `createPipeline` · `allocateRange` / `freeRange` / `growArena` · `setChunkFaces` / `removeChunk` · `uploadBlockGeometry` / `makeStorageBuffer` · `updateResources` · the draw loop in `render` · `resources/shaders/webgpu/chunkFaces.wgsl` · game: `Chunk::sendDataToOpenGL` and `Chunk::clearGpuData` in `chunk.cpp`, `Renderer::recreateBlockGeometryData` and the call after frustum culling in `renderFromBakedData`

**Commit:** `56da441`

### 6a. Instancing: a vertex buffer that steps once per face

The face record is what `Chunk::bake` already writes for OpenGL: `short`
orientation, `short` texture index, `ivec3` block position (with light packed
in y's bits 16–23 and flags in 24–31, masked off with `& 0xFFFF`). Sixteen
bytes, four of which are the "vertex" in any conventional sense. The GL path
read it with `glVertexAttribDivisor(…, 1)`; WebGPU's name for the same thing
is `VertexStepMode::Instance` on the buffer layout: the GPU advances to
the next 16 bytes once per _instance_ rather than once per vertex, so all
four vertices of a face see the same record. Two attributes — `Sint16x2` at
offset 0 and `Sint32x3` at offset 4 — and the vertex shader receives them as
`vec2i` and `vec3i`.

The alternative is the storage-buffer route WebGPU Fundamentals shows
(`array<Face>` indexed by `instance_index`). The vertex-buffer route was
chosen because the data already has a vertex layout, because `firstInstance`
then does the offsetting for free (6d), and because it keeps the face stream
out of the bind group, which otherwise would have to be remade every time
the arena grows.

### 6b. Four vertices, a strip, and the fan's winding

OpenGL drew each face as `GL_TRIANGLE_FAN` over a four-entry index buffer.
WebGPU has no fans. A **triangle strip** of four vertices makes the same two
triangles, but visits the corners in a different order: a fan from corner 0
makes (0,1,2) and (0,2,3); a strip makes (0,1,2) and (1,2,3), with the
hardware reversing every second triangle's winding so the strip faces one
way. To draw
the _same quad with the same counter-clockwise winding_ from the _same corner
table_, the strip's four vertices are mapped `0,1,3,2` — `stripToCorner` in
the shader, indexed by `vertex_index`. No index buffer:
`stripIndexFormat = Undefined`. **Strips never join across instances**, so
`draw(4, N)` is N separate quads, which is the whole reason a four-vertex
strip per instance works at all.

### 6c. The corner table as a read-only storage buffer

The vertex shader needs, per orientation, four corner offsets (3 floats each)
and later four UVs (2 floats each). GL kept these in SSBOs (`u_vertexData`,
`u_vertexUV`) indexed by orientation and `gl_VertexID`. WebGPU's equivalent is
`var<storage, read> vertexData: array<f32>` bound as
`BufferBindingType::ReadOnlyStorage`. Why not a uniform: a uniform buffer is
capped at 64 KiB by default and cannot hold a runtime-sized array; a storage
buffer can be 128 MiB and `array<f32>` with no length is legal. Why not
constants in the shader: the table is built at runtime by
`recreateBlockGeometryData` from the models manager, and rebuilt when it
changes, so it goes through `setBlockGeometry` and is re-uploaded whenever
`blockGeometryDirty` says so.

The table is 12 floats per orientation: rows 0–5 are the cube faces, 6–9
grass, 10–15 leaves — except the shader maps leaves back onto rows 0–5
(`defaultShader.vert` only adds their sway on top of the cube's corners). The
UV table in 7 is indexed by the orientation itself, leaves included, because
that is what `calculateUVs` does.

### 6d. The arena: one buffer, a first-fit allocator, growth by copy

A chunk's faces arrive whenever it is meshed and leave whenever it is
unloaded, and there are hundreds of chunks. One buffer per chunk would mean
`setVertexBuffer` per draw and hundreds of buffer objects; one buffer for the
world means one bind and one `firstInstance` per draw. So `setChunkFaces`
allocates a contiguous range of faces for the chunk in the **arena**, writes
the records there with `writeBuffer`, and the draw is
`draw(4, faceCount, 0, firstFace)` — the fourth argument, `firstInstance`,
is where the instance-stepped attribute starts reading.

- **The allocator** is first-fit over a `std::map<firstFace, length>` of free
  ranges, merged with its neighbours on every free, so the map stays as short
  as the fragmentation allows. The GL `BigGpuBuffer` did the same job.
  Measured at shutdown: every chunk freed and the free list back to one
  range, which is the test that merge is right.
- **A buffer cannot be resized.** Growing is a new buffer, a
  `copyBufferToBuffer` of the old contents recorded in its own encoder and
  **submitted at once**, and the old buffer released. Submitting immediately
  matters: `writeBuffer` is ordered against submits, so a later
  `setChunkFaces` that writes into the new buffer is guaranteed to land after
  the copy. The usage is `Vertex | CopyDst | CopySrc` from the start, because
  a buffer's usage cannot be changed either, and a buffer that may one day be
  the _source_ of a growth copy has to say so when it is created. Chunks keep
  their offsets across growth, so no other state changes.
- **The sizes.** It starts at 2²⁰ faces (16 MiB) and doubles until the
  request fits, capped at `maxBufferSize / 16`. The `test` world sat at
  250k–440k faces _drawn_ per frame, with more than that resident, so the
  first size has held so far.

[Buffer uploads](https://toji.dev/webgpu-best-practices/buffer-uploads.html)
covers when `writeBuffer` is the right upload path (it is here: the data is
small per chunk and the frequency is low) and what the alternatives cost.

### 6e. One draw per visible chunk, near to far

Culling stays on the CPU: `renderFromBakedData` already frustum-culls and
sorts `chunkVectorCopy` **far to near** for the GL transparency passes. The
world walks it **backwards**, as the GL opaque loop does, so near chunks fill
the depth buffer first and the GPU's early depth test rejects far fragments
before they are shaded. Each chunk that has a range in the arena is one
`draw`. At the start of `test` that was about 160–260 draws and 250k–440k
faces a frame. Culled and unculled renders of the same frame differed by 11
pixels — chunks on the frustum's edge — which is the check that the culling
and the projection agree.

That draw loop is the thing milestone 8 replaces: the per-chunk `draw` is
exactly what `glMultiDrawElementsIndirect` collapsed into one call on the GL
side. _(It survives as `OURCRAFT_WORLD_DRAWS=direct`, and as the fallback on
a device without `IndirectFirstInstance`.)_

### 6f. A bind group names objects, not slots

The group-0 bind group holds the uniform buffer, the corner table and (from 7) the UV table, the layer table, the texture array view and the sampler.
**It holds the objects themselves.** When the corner table is re-uploaded the
old buffer is released and a new one created, and the bind group that named
the old one is stale — not invalid, just pointing at a buffer nothing writes
to any more. So `updateResources` remakes the bind group whenever any of its
members was remade, and only then. [Bind group best practices](https://toji.dev/webgpu-best-practices/bind-groups.html)
is the longer treatment of what a bind group costs and how to group by
update frequency; at one group changed a few times per session, this one is
not yet worth splitting.

---

## 7. Block textures without bindless

**Concepts:** The milestone the port exists for. OpenGL gave every block
texture a 64-bit **bindless handle**, wrote them all into an SSBO, and the
fragment shader sampled through `sampler2D(handle)` — which is why the world
never drew on macOS (roadmap, "Why a port"). WebGPU has no bindless. The
replacement is one `texture_2d_array`, a layer per distinct texture,
built on the GPU by a **compute shader** from the sprite textures the loader
already made, and a slot → layer **storage buffer** standing where the handle
table stood. Plus the things that came with it: requesting a **device limit**
above its default, texture **views** of one layer or one level, an
**automatic pipeline layout**, a **sampler** matching the GL parameters, and
an **alpha test** by `discard`.

**LearnWebGPU:** [A first texture](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/a-first-texture.html) · [Texture mapping](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/texture-mapping.html) · [Sampler](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/texturing/sampler.html) · [Compute Pipeline](https://eliemichel.github.io/LearnWebGPU/basic-compute/compute-pipeline.html) · [Mipmap Generation](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/mipmap-generation.html) (the GPU half this time; space-game 7 did the CPU half) · texture arrays have no chapter — [Textures](https://webgpufundamentals.org/webgpu/lessons/webgpu-textures.html) and the WGSL spec's `texture_2d_array` / `textureSample` with an `array_index` · [Limits and Features](https://webgpufundamentals.org/webgpu/lessons/webgpu-limits-and-features.html) for 7b

**Code:** `buildBlockTextures` · `ensureBlockTexturePipelines` · `makeArrayView` · the sampler in `buildBlockTextures` · bindings 2–5 in `createLayoutAndUniforms` · `resources/shaders/webgpu/blockTextures.wgsl` · `fs_main` and `textureLayers` in `chunkFaces.wgsl` · `getTextureHandle` in `src/platform/nullGL.cpp` · `wgpuTextureView` and the `requiredLimits` in `thirdparty/wgpu2d/src/wgpuContext.cpp` · game: `Renderer::recreateBlocksTexturesBuffer`

**Commit:** `4e3b475`

### 7a. Three ways to sample "any of N textures", and why the array won

| Option                                             | Shader sees                                 | Capped by                                                                                                                             | Cost                                                                     |
| -------------------------------------------------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| **Binding array** `binding_array<texture_2d<f32>>` | N separate textures, any sizes              | `maxSampledTexturesPerShaderStage` — **128** on this adapter — and needs wgpu-native's `TextureBindingArray` feature; not core WebGPU | Closest to bindless; the cap kills it                                    |
| **Texture array** `texture_2d_array<f32>`          | one texture with N layers, **all one size** | `maxTextureArrayLayers` — 2048 on this adapter, 256 by default                                                                        | Resample everything to one size; one binding, core WebGPU                |
| **Atlas** `texture_2d<f32>` with UV offsets        | one big texture                             | `maxTextureDimension2D`                                                                                                               | Mip bleed between cells, `Repeat` sampling breaks, UV tables must change |

The array won on the second row alone. About 1,600 PNGs on disk, mostly
128×128, some 64×64 and 16×16, and tall 128×256 to 128×1280 animation strips.
Four slots per block (albedo, normal, material, parallax), many pointing at
the loader's defaults, so 588 slots in `test` need only **448 distinct
textures** — over 128, under 2048, and over the default 256, which is 7b. One
size for all layers means resampling, which is what `fill` does (7d). The
strips lose their height, but the GL shader samples them whole rather than
animating, so nothing visible is lost yet.

The texture memory risk the roadmap raised (170 MB for albedo at native
sizes) resolved itself: at 128×128 with five mips, 448 layers is about
39 MB, and that is the _only_ cost, because the array is built from textures
that already exist. The duplication — the UI still reads the wgpu2d copies —
is the 39 MB, and it stays until the UI has another way to draw a block icon.

### 7b. Limits: the device gets what it asks for, not what the adapter has

An **adapter** reports what the hardware can do; a **device** is created with
`requiredLimits`, and gets **the WebGPU default for anything it did not
ask for**, however capable the adapter is. `maxTextureArrayLayers` defaults
to 256. Creating a 448-layer texture on a default device is a validation
error, not a smaller texture. So `wgpuInit` now calls `adapter.getLimits`,
copies `maxTextureArrayLayers` into the required limits, and leaves every
other field undefined (which means default). Asking for exactly the adapter's
value is the pattern: asking for more than it has fails device creation, and
asking for a round number you like is asking for a different failure on a
different machine. The general lesson is space-game 16's (check the feature
list first) applied to limits, and `buildBlockTextures` re-checks
`limits.maxTextureArrayLayers` against the layer count at build time with a
readable error rather than letting the texture fail.

### 7c. A texture array, and the views that address it

A `texture_2d_array` is not a separate texture dimension: it is
`TextureDimension::_2D` with `size.depthOrArrayLayers = layers`. What makes it
an _array_ to a shader is the **view**: `TextureViewDimension::_2DArray` over
all layers and all mips is what `chunkFaces.wgsl` samples;
`TextureViewDimension::_2D` over one layer and one level is what `fill`
writes. `makeArrayView` makes both from one texture, and the same texture
has both `TextureBinding` and `StorageBinding` usage so it can be read in a
render pass and written in a compute pass.

In the shader, `textureSample(blockTextures, blockSampler, in.uv, in.layer)`
takes the layer as a fourth argument. The layer comes from the vertex shader
through `textureLayers[textureIndex]`, and the varying is declared
`@interpolate(flat)` — a `u32` cannot be interpolated across a triangle, and
WGSL refuses to compile the output without the attribute. The texture index
itself is the record's `short` masked to its low 11 bits (the high 5 are a
colour index the GL shader uses; not yet ported).

UVs need no flip because of 5b's convention: the array stores rows bottom-first
like the GL upload, so the UV table carries over unchanged. The **flip lives in**
`fill` instead (7d), once per texel at build time, rather than in a table or
a per-fragment subtraction.

### 7d. Compute: building 448 textures and 4 mip levels in one pass

A compute pipeline is a pipeline with one stage, and a compute pass is a pass
with no attachments. What replaces `draw` is `dispatchWorkgroups(x, y, z)`,
which runs the entry point once per thread in an _x × y × z_ grid of
workgroups, each workgroup `@workgroup_size(8, 8)` = 64 threads. The guide's
rules of thumb hold here: a multiple of the warp size, a power of two,
square because the data is a 2D grid. The thread learns where it is from
`@builtin(global_invocation_id)`, and the first line of each entry point
returns if it is past the texture's edge, because the dispatch is rounded up.

Two entry points in one module, two pipelines:

- `fill` — one dispatch per layer, 16×16 workgroups, reading a wgpu2d
  sprite texture (any size) and writing one 2D view of one layer of level 0.
  Each destination texel computes the source rectangle it covers, at least
  one texel each way: a 16×16 source enlarges by nearest texel, so pixel art
  stays exact; a 128×1280 strip reduces by box average. The y index is
  flipped on the way in. The source is a `texture_2d<f32>` read with
  `textureLoad` (no sampler, integer coordinates, an explicit level); the
  destination is `texture_storage_2d<rgba8unorm, write>`, written with
  `textureStore`. A storage texture binding carries its format in the type —
  the shader has to know how to encode what it writes.
- `downsample` — one dispatch per level, every layer at once, with
  `dispatchWorkgroups(…, …, layers)` and `id.z` as the layer. The views are
  `_2DArray` of a single level: level *n*−1 as `texture_2d_array` to read,
  level _n_ as `texture_storage_2d_array<rgba8unorm, write>` to write. The
  average is done on the bytes — `round(x * 255)`, sum, `floor((… + 2) / 4)`,
  back to 0..1 — so the result is bit-identical to wgpu2d's own
  `mipmap.wgsl` and to what an integer CPU downsample would give.
  Five levels, 128 down to 8, because GL clamped at `GL_TEXTURE_MAX_LOD 4`.

Four things the guide's chapter says in passing that this build ran into:

- **The views bound for reading and writing must not share a level.** Each
  `downsample` step reads level *n*−1 and writes _n_ through two
  single-level views; a view spanning both would be rejected.
- **No barriers.** WebGPU orders the dispatches in a pass: level 2 sees level
  1 complete. There is nothing to write between them, which is what makes
  the whole chain one pass.
- **A module may not declare two resources at one binding, even for
  different entry points.** `fill` uses bindings 0 and 1, `downsample` 2 and 3. Each pipeline is created with `layout = nullptr` — an \*\*automatic

  layout\*\* containing only the bindings its entry point touches — and the
  bind groups are made against `pipeline.getBindGroupLayout(0)`. For a
  pipeline created once with resources that exist only during the build,
  automatic is the right default; the render pipeline uses an explicit
  layout because its bind group outlives any one pipeline.

- **Bind groups are cheap and per-dispatch.** The build makes 448 + 4 of
  them and releases them all after submit. The guide does the same per mip
  level. A bind group is a record of handles, not an upload.

The whole thing — 448 fill dispatches, 4 downsample dispatches — records and
submits in about 27 ms on this machine, once per `recreateBlocksTexturesBuffer`.

### 7e. The sampler, and the one GL parameter WebGPU refuses

GL asked for `GL_NEAREST_MIPMAP_NEAREST` / `GL_NEAREST`, repeat, max LOD 4,
and 4× anisotropy. WebGPU's `SamplerDescriptor` takes the first three
directly: `Nearest` mag and min, `MipmapFilterMode::Nearest`, `Repeat` on
all three axes, `lodMaxClamp = 4`. **Anisotropy is only legal with linear
filters** — `maxAnisotropy > 1` with any `Nearest` filter is a validation
error — so it is 1 here. For 128×128 pixel art sampled nearest, the
difference is invisible; it is the kind of thing that is better known than
discovered from an error string.

### 7f. Alpha test by `discard`

Grass, flowers and leaves are cut-outs: opaque where the texture has alpha,
nothing where it does not. GL did this with an alpha test in the fragment
shader; WGSL has no fixed-function alpha test and no blend mode that drops a
fragment, but it has `discard`, which ends the fragment without writing
colour _or depth_. `if (albedo.a <= 0.0) { discard; }` is the whole of it,
and it is `defaultShader.frag`'s own line (`if(textureColor.a <= 0){discard;}`)
carried over, threshold included. Translucency proper — glass, water — is
milestone 9 and a blend state, not this.

### 7g. The handle trick: keeping the loader's data shape

`BlocksLoader::gpuIds` is a vector of 64-bit handles, four per block, that
the GL path filled from `glGetTextureHandleARB` and the shader indexed by a
face's texture index. The loader does not know about layers and should not
have to. On the WebGPU path `glGetTextureHandleARB` is a stub in `nullGL.cpp`,
and the stub now returns `(1 << 32) | textureName` — and a texture made
through the gl2d shim _is_ a wgpu2d texture, so its GL "name" is its wgpu2d
id. `buildBlockTextures` reads the low 32 bits back, checks the high bits
and that the id is a live texture (`wgpuTextureView`, one accessor added to
`wgpuFrame.h` for this), and maps anything else to slot 0's default, as a
missing bindless handle never would have. So the loader, the handle table
and `recreateBlocksTexturesBuffer` are untouched, and the only new line in the
renderer is the one that hands the vector to `setBlockTextures`.

This is the "port behaviour, not structure" rule from the top of the file at
its sharpest: the data structure is kept even though its contents mean
something different now, because changing it would ripple into seven files.

_Superseded in 19b, once there was no GL path to keep the shape for: a slot
in `gpuIds` is now the wgpu2d texture id itself, 0 for none, and the handle
decoding went with `nullGL.cpp`. The loader, `model.cpp` and the rest were
edited then, in one commit, with the picture unchanged — which is the point
of having deferred it._

### 7h. What the frame shows, and what it does not

GL cannot draw the world on this Mac, so the check was the frame itself:
grass sides upright (the flip in `fill` is the right way round), cut-outs
clean, no validation errors across the build and the first frames, and the
numbers above in the log. **Not done:** lighting of any kind — the shade is
still `vertexColor` per orientation; the grass and leaf colour tint in the
texture index's high bits; the sway animation; transparent and water faces;
and the UI's block icons, which still draw white (roadmap 3b) because
`renderAllBlocksUiTextures` renders through GL.

_Every item on that list has a section below: lighting is 12, the tint rides
with it, the sway is 18-parity, transparency is 9, the icons are 10c._

---

## 8. Indirect draws: the arguments move to the GPU

**Concepts:** A **draw call whose arguments live in a buffer.** `draw(4,
faces, 0, firstFace)` becomes a 16-byte record — `vertexCount`,
`instanceCount`, `firstVertex`, `firstInstance` — in a buffer with `Indirect`
usage, and the pass says `drawIndirect(buffer, offset)`. GL's
`glMultiDrawElementsIndirect` took a whole array of those in one call;
WebGPU's core API does not, but wgpu-native exposes **`multiDrawIndirect`**
as a native extension. This is the milestone where the port's per-chunk loop
collapses into one call, as GL's had been. Also the first milestone that was
_measured_ rather than only checked, and the measurement said the win is not
where the intuition puts it.

**LearnWebGPU:** no chapter. The spec entry for
[`drawIndirect`](https://www.w3.org/TR/webgpu/#dom-gpurenderpassencoder-drawindirect)
gives the record's layout; wgpu's
[`Features`](https://docs.rs/wgpu/latest/wgpu/struct.Features.html) page
describes `MULTI_DRAW_INDIRECT` and `INDIRECT_FIRST_INSTANCE`, which is what
the C `WGPUNativeFeature_MultiDrawIndirect` and
`WGPUFeatureName_IndirectFirstInstance` map to.

**Code:** `DrawArgs` and `DrawMode` · `ensureIndirectBuffer` · `drawRecords`
· the mode selection and feature check near `hasFeature(FeatureName::IndirectFirstInstance)`
· the feature requests in `wgpuInit` (`thirdparty/wgpu2d/src/wgpuContext.cpp`)

**Commit:** `a73e044`

### 8a. Two features, one of them core

- **`IndirectFirstInstance`** is a core WebGPU feature, off by default. Without
  it, a non-zero `firstInstance` in an indirect record is a validation error
  — and `firstInstance` is the whole mechanism here, since it is what points
  a record at a chunk's arena range (6d). So the world checks
  `device.hasFeature` and falls back to direct draws if it is absent. The
  pattern from 7b again: ask the adapter, request at device creation, check
  on the device before relying on it.
- **`MultiDrawIndirect`** is wgpu-native's, in `wgpu.h` not `webgpu.h`. The
  C++ wrapper has no enumerator for it, so the request is a cast:
  `(WGPUFeatureName)WGPUNativeFeature_MultiDrawIndirect`. This is the first
  use of the "custom extensions" mechanism the guide's appendix describes,
  and it is the reason the reference shelf in the 2D outline lists that
  appendix.

### 8b. One buffer a frame, and why reuse is safe

The culled list becomes an array of records, written with one `writeBuffer`
into a buffer that is reused every frame (doubled when outgrown, as the
arena is). The question is whether frame _N_+1's write can land before frame
_N_'s draws have read it. It cannot: `writeBuffer` is queued in order with
the submits, so the write is ordered after the previous frame's command
buffer. The same fact that made the arena's growth copy safe (6d) makes the
indirect buffer's reuse safe, and it is worth stating once as a rule —
**everything on the queue happens in queue order, and `writeBuffer` is on the
queue** — because it is the reason this renderer has no per-frame fences
anywhere.

### 8c. What the measurement said

`OURCRAFT_WORLD_DRAWS=direct|indirect|multi` picks the path at startup, and
shutdown prints the average CPU time spent recording the chunk draws. All
three produce pixel-identical frames. With vsync, about 150 chunks record in
25–30 µs whichever path is used. Uncapped, over two runs each:

| Path                                    | Recording time |
| --------------------------------------- | -------------- |
| `multi` (one `multiDrawIndirect`)       | 124–149 µs     |
| `direct` (a `draw` per chunk)           | 145–156 µs     |
| `indirect` (a `drawIndirect` per chunk) | 161–168 µs     |

Against a 13.3 ms frame. Three things this taught:

- **Metal has no native multi-draw.** wgpu loops `drawIndirect` underneath,
  so the GPU work is identical on this machine and the only difference is
  how many C calls the CPU makes. The feature is real on Vulkan and D3D12.
- **`PresentMode::Immediate` did not uncap the frame.** It stayed at 13.3 ms
  — the display's 75 Hz — because macOS composites windowed applications at
  the display rate and `nextDrawable` waits regardless. "Uncapped" on this
  Mac means the CPU side only. (`slow-frames.log`, which the plan named as
  the baseline, turned out to be space-game's file.)
- **The indirect buffer is still the right structure**, for a reason the
  numbers do not show: it is the place a GPU culling pass would write its
  arguments, and milestone 13 fills it from three more cull passes without
  any new draw machinery.

---

## 9. Transparency: a second range, a blend state, and the water depth peel

**Concepts:** The Z-buffer resolves opaque overlap for free and does nothing
for translucent overlap: blending is order-dependent, so transparent faces
must be **drawn after everything opaque, far to near**, and even then a
single pass cannot show what is _under_ water with the right colour and
depth. GL's answer is a **depth peel** — four passes and two texture copies —
and this milestone follows it step for step. Four **render pipelines** stand
where GL toggled state between passes, and a WGSL **`override` constant**
lets one fragment shader serve them all.

**LearnWebGPU:** the "About transparency" aside in
[Depth buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/depth-buffer.html)
is the problem statement and nothing more ·
[Transparency](https://webgpufundamentals.org/webgpu/lessons/webgpu-transparency.html)
and [Constants](https://webgpufundamentals.org/webgpu/lessons/webgpu-constants.html)
on WebGPU Fundamentals · the WGSL spec on
[override declarations](https://www.w3.org/TR/WGSL/#override-decls)

**Code:** `setChunkTransparentFaces` · `placeFaces` (the arena now holds two
ranges per chunk) · `renderTransparent` · `copyWholeTexture` · the `passMode`
override and `fs_blend` in `chunkFaces.wgsl` · the four `FacePipelineDesc`s
in `createPipelines` · `wgpuFrameEncoder` / `wgpuFrameTexture` in `wgpuFrame.h`

**Commit:** `2d709af`

### 9a. The peel, step for step

The goal: water that shows the pond bed through it, tinted and darkened by
how deep the bed is, with glass and ice blending correctly in front of and
behind it. The passes, each on the frame's encoder:

1. **Copy depth → peel depth.** `copyTextureToTexture` of the opaque pass's
   depth into a second depth texture. A copy, not a draw: depth formats
   cannot be drawn between, and a whole-subresource copy of a depth texture
   is allowed.
2. **Water-only depth pass** into the peel depth: depth `Less`, no colour
   target. Afterwards the peel depth holds the nearest water surface at each
   pixel.
3. **Peel pass:** transparent faces _beyond_ that surface, alpha-blended
   onto the frame. What is under the water, drawn with the water's depth as
   the test.
4. **Copy colour and depth.** The frame as it stands — opaque world plus
   what is under the water — into two more textures.
5. **Final pass:** every transparent face, blended, `LessEqual`, no culling;
   water reads the two copies to find what it covers and how far below it
   that is.

**Why the copies.** A pass cannot sample a texture it is drawing into. The
final pass needs the colour and depth under each water pixel _while writing
colour and depth_, so the only way to read them is from a copy made between
passes. The copies are read with `textureLoad` at the fragment's own pixel
(`@builtin(position).xy`), integer coordinates, no sampler — the pixel under
this one is exactly one texel. This "attachment ≠ binding, so copy or
stand-in" rule returns in 13, 16, 17 and 18; see "Patterns" below.

### 9b. Four pipelines where GL toggled state

| Pipeline       | Blend         | Depth compare | Depth write | Cull | Colour |
| -------------- | ------------- | ------------- | ----------- | ---- | ------ |
| water depth    | —             | `Less`        | yes         | back | none   |
| peel           | alpha         | `Less`        | yes         | back | yes    |
| final          | alpha         | `LessEqual`   | yes         | none | yes    |
| final, no peel | alpha         | `LessEqual`   | yes         | none | yes    |

In GL these were `glDepthFunc`, `glEnable(GL_CULL_FACE)` and `glColorMask`
calls between draws. Here each combination is an immutable pipeline (the
space-game 12/14 lesson, now in 3D), built once by one `createFacePipeline`
from a small description struct, all from the same shader module.

**One module, several specialisations: `override`.** `fs_blend`'s behaviour
differs between the peel and the final pass, and `vs_main`'s between the
world pass and the decal (10a) and the shadow cascades (13). WGSL has no
preprocessor. What it has is **pipeline-overridable constants**: `override
passMode: u32 = 1u;` in the shader, and a `ConstantEntry { key = "passMode",
value = … }` in the pipeline descriptor's `constants` array. The compiler
specialises the module per pipeline, dead code and all, so a branch on an
override costs nothing at run time. The port uses five of them by milestone
16 (`passMode`, `zBias`, `shadowCascade`, `view`, `horizontal`); together
with the pipeline-per-state rule they are how fourteen WGSL files cover what
forty-three GLSL files did.

### 9c. The unlit water, and what waited

Until lighting (12), water is `u_waterColor` taken to sRGB, mixed with what
is under it by a view-angle Fresnel term and darkened with depth; glass alpha
rises at grazing angles as GL's does. The surface sits 0.08 below the block
top, GL's offset. The waves, the dudv refraction and the moving normal map
waited for 18-parity, because they are animation, and animation was deferred
as a whole.

Transparent faces are a **second arena range per chunk**: `Chunk::bake`
already sorted them far to near from the player when it baked them, and
chunks are drawn in the renderer's far-to-near list order, so the within-pass
order costs nothing extra. Leaves needed nothing here — they were
alpha-tested in 7, which is the cheaper answer whenever coverage is 0 or 1.

**Scripted input grew to reach the water:** `hold@W,N`, `key@Space`,
`key@LShift`, negative `move@` coordinates, and `WGPU_SCREENSHOT_FRAME` taking
a list of frames. Checked in a copy of `test2`, reached by flying: pond bed
visible through the water, clean edges, no validation errors. Glass and the
no-peel path were built from the GL code but had no scene to check them in.

---

## 10. Entities: cubes, decals, lines, skinned models, items, and the icons

**Concepts:** Everything the world pass draws that is not a chunk face. Three
sub-milestones, each a different kind of geometry through WebGPU: **instanced
cubes from a storage-buffer record**, a **decal that reuses a vertex stage**
with a depth bias, **line lists** where GL had a polygon mode, **indexed
skinned meshes** with bones in a storage buffer and per-entity texture bind
groups, and **render-to-texture at load time** for the inventory icons.
Mostly game-specific, but each one is a WebGPU object used a way the first
seven milestones had not.

**LearnWebGPU:** [Loading from file](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/loading-from-file.html)
(the guide's meshes; `setMesh` is the same shape with the game's loaders
instead of OBJ) ·
[Index Buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/index-buffer.html)
(models are the first indexed draws in the 3D side) · no chapter on skinning,
decals or render-to-texture; space-game 10 covers the last

**Code:** `entities.wgsl` (`vs_cube`/`fs_cube`, `vs_line`/`fs_line`,
`vs_model`/`fs_model`, `vs_item`/`fs_item`) · `blockIcons.wgsl` ·
`BlockCube` / `prepareCubes` / `ensureCubeVertices` · `renderDecal` and the
`zBias` override · `renderLines` · `newMeshId` / `setMesh` / `removeMesh` /
`releaseMesh` · `renderEntities` · `textureGroup` (the per-entity bind group
cache) · `ensureWhiteTexture` · `renderBlockIcons` / `createIconPipeline` ·
`wgpuMarkTargetDrawn` in `wgpu2d`

**Commits:** `81b1324` 10a · `e5e79fc` 10b

### 10a. Cubes, a decal and lines

- **Dropped block items** are 0.4-scale cubes: a 64-byte record each
  (position split int/float, brightness, six face slots into 7's layer
  table) in a **storage buffer** indexed by `instance_index`, the 36 vertices
  from GL's `vertexDataOriginal` in a vertex buffer, back faces culled. This
  is WebGPU Fundamentals' storage-buffer instancing, used where 6a used
  per-instance vertex attributes, because here the per-instance data is 64
  bytes and the vertex data is a fixed cube.
- **The block-breaking crack** is `chunkFaces.wgsl`'s own vertex stage with
  a `zBias` override — half GL's bias, because depth is 0..1 here, not
  −1..1 — and its own fragment entry `fs_decal` sampling one of `crack.png`'s
  ten frames. Depth bias in the vertex shader rather than the pipeline's
  `depthBias` field, because that is what GL did and the goal was the same
  picture. The texture's v is flipped: wgpu2d stores images top row first,
  gl2d stored them bottom first, which is the first of several v-flips
  catalogued under "Patterns".
- **GyzmosRenderer's outline and lines.** GL drew the selection cube with
  `glPolygonMode(GL_LINE)`, which shows every triangle's edges including the
  face diagonals. WebGPU has no polygon mode. So the cube's triangles are
  emitted as line segments — three per triangle — through a
  `PrimitiveTopology::LineList` pipeline, which draws exactly what GL
  showed, diagonals included. Lines are 1 px: WebGPU has no line width.
- GL's `pow(x, 2.2)` ambient on a linear colour is just `x` on this world's
  sRGB output — the first time the colour-space question had to be answered
  on the 3D side; 11 answers it properly.

### 10b. Skinned models and items

- **Meshes get ids.** `model.cpp` and `BlocksLoader` hand their CPU vertex
  data to `setMesh`, keyed by the GL VAO name, so the renderer's existing draw
  lists find their WebGPU meshes without changing shape. Models use a 36-byte
  vertex and 16- or 32-bit indices; items 8 floats. _(19b: keyed by
  `wgpuWorld::newMeshId()` once GL names stopped existing; the field is
  `mesh`.)_
- **Bones in a storage buffer.** Every frame's bone matrices, for every
  model, concatenated into one buffer; each `ModelInstance` carries a
  `boneBase`, and `vs_model` reads `bones[boneBase + boneIndex]`. One indexed
  draw per entity with `firstInstance` picking its record — the 6d trick a
  third time.
- **Per-entity textures as a cached bind group.** Each entity has four
  textures (GL's four bindless handles; wgpu2d ids here). They form a
  group-2 bind group, made once per distinct combination and cached, because
  a bind group is cheap to hold and not free to create every frame. A
  missing texture draws from a 1×1 white stand-in (`ensureWhiteTexture`),
  which is what GL's white fallback did.
- **Models draw without culling**, because the player's right hand is
  mirrored by a negative scale, which flips its winding. Entity normals use
  the inverse transpose via cofactors, with the sign kept, for the same
  reason (11).
- **Pass order is GL's.** `render()` now stops after the opaque pass;
  `renderEntities` runs; `renderTransparent` runs. Entities between the
  solid and transparent passes, so water in front of a zombie blends over
  it.

Checked in `test` and `test2`: the hand in the mage skin, the held sword and
apple, a dropped apple the right way up, and a training dummy moved in front
of the camera — whose burlap head and straw arms only map correctly with the
right v orientation, because the texture's lower half is empty. That is the
kind of check a flip needs: an asymmetric texture where the wrong answer is
visibly wrong.

### 10c. Block icons: render to texture at load time

The inventory's block icons had drawn white since 3b. GL rendered each block
once into a 64×64 texture at load; those calls were stubs on the WebGPU
path. Now each icon is a `wgpu2d::FrameBuffer` (a render target is a texture
— space-game 10) that `renderBlockIcons` draws into with `blockIcons.wgsl`,
on a command buffer of its own at load time, before any frame is open:
GL's orthographic cameras with depth remapped to 0..1, the whole entity
geometry (cube, stairs, slab, wall, furniture ranges), per-face texture
slots, GL's normal lighting, alpha kept. One draw per icon with
`firstInstance` naming its record, through the dropped-cube pipeline layout.

The one new thing wgpu2d needed: **`wgpuMarkTargetDrawn`**. wgpu2d clears a
target on its first use in a frame; a target drawn by an outside pass has
already been drawn, and the first-clear rule was wiping it — and, since the
readback for network skins goes through the same path, reading it back empty.
A library that owns a resource's lifecycle has to be told when someone else
has touched it. Checked: blocks, stairs, slabs and walls in the creative
tab, and the furniture icons read back to a PNG.

---

## 11. The G-buffer: multiple render targets and linear light

**Concepts:** The 3D world stops drawing to the screen. It draws into a
**G-buffer** — several colour attachments written by one pass — and a
**composite** pass turns one of them into the picture. This is the structure
every later milestone hangs off: lighting reads the attachments (12), AO and
SSR read them (17), bloom adds into one (16), post reads the result (15).
What WebGPU adds to the familiar idea: a hard **byte budget per pixel** across
the attachments, a **renderable-format feature**, a **depth prepass** whose
`Equal` test only holds if two pipelines compute bit-identical depth, and
the realisation that the HDR target holds **linear light** while everything
before it had been writing sRGB.

**LearnWebGPU:** [Deferred Shading](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/deferred-shading.html)
is a 🔴 TODO stub, and
[HDR textures](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/hdr-textures.html)
is WIP. The technique is LearnOpenGL's
[Deferred Shading](https://learnopengl.com/Advanced-Lighting/Deferred-Shading),
[HDR](https://learnopengl.com/Advanced-Lighting/HDR) and
[Gamma Correction](https://learnopengl.com/Advanced-Lighting/Gamma-Correction);
the fullscreen triangle is
[A large triangle to cover clip space](https://webgpufundamentals.org/webgpu/lessons/webgpu-large-triangle-to-cover-clip-space.html)

**Code:** `ColorTargets` / `colorTargets` / `GBufferSlots` · `ensureTargets`
/ `createTarget` / `createTargetView` / `releaseTargets` · `beginGBufferPass`
· `fullscreenPass` · `compositeFrame` · `createViewPipelines` and
`gbufferView.wgsl` · `gBufferPreview` / `gBufferViewName` · `@invariant` and
`fs_depth` in `chunkFaces.wgsl` · the `RG11B10UfloatRenderable` request in
`wgpuInit`

**Commit:** `82da3a3`

### 11a. Four targets, not five: the byte budget

GL's `fboMain` had five colour attachments. WebGPU's
`maxColorAttachmentBytesPerSample` defaults to **32**, and `rg11b10ufloat`,
four bytes in memory, **counts as eight** as a render target. GL's layout
comes to 33. So the material target is folded into the position target's
alpha (roughness lives there), and the four are:

| Slot | Format          | Holds                                | Bytes counted |
| ---- | --------------- | ------------------------------------ | ------------- |
| 0    | `rg11b10ufloat` | HDR colour, linear                   | 8             |
| 1    | `rgba16float`   | view-space position, roughness in α  | 8             |
| 2    | `rgba16uint`    | packed normal                        | 8             |
| 3    | `rg11b10ufloat` | bloom input                          | 8             |

`rg11b10ufloat` is only renderable with the **`RG11B10UfloatRenderable`**
feature; wgpu2d requests it when the adapter has it, and the HDR format falls
back to `rgba16float` without (which would break the budget, and is the
reason the fallback is a format and not a fifth target). The roadmap's
"Render targets" table had predicted this budget problem from reading the
spec; this is where it bit.

### 11b. A depth prepass and `Equal`, in one pass

GL draws opaque chunks twice: a depth-only prepass, then the solid pass with
`glDepthFunc(GL_EQUAL)`, so each pixel is shaded exactly once. The port does
both inside **one render pass**: the prepass pipeline writes depth with every
colour target's `writeMask = None` (GL's `glColorMask(0)`) and the alpha-test
fragment `fs_depth`; the solid pipeline tests `Equal` and writes no depth.

**`Equal` only holds if both pipelines compute the same depth bit for bit**,
and two different pipelines compiled from the same source are not promised
to. WGSL's `@invariant` on the position output is that promise: it forbids
the compiler the reorderings that would make the two differ. Without it the
solid pass drops pixels at random where rounding disagrees — a bug that
looks like dirt. The renderer's `zprepass` setting off gives one `Less` pass
instead. (18-parity adds the other half of this rule: whatever the prepass
animates, the solid pass must animate identically.)

### 11c. Linear light, and where the conversions go

GL's HDR target holds linear light; everything from 7 to 10b had written
texture colours straight to the surface and looked right because the
surface format is not an sRGB one, so nothing gamma-corrected them on the
way out. Now each world shader keeps its
math and **converts its result with GL's sRGB curve on the way into the
G-buffer**, and the composite converts back on the way out. In 11 that is a
no-op round trip, and 12 replaces the inbound conversion with real lighting;
doing it in two steps is what let 11 be checked against 10b at all — within
2 levels everywhere, which is `rg11b10` rounding, except a gizmo line lying
in a block face (a depth tie) and a few edge pixels.

The composite is a **fullscreen triangle**: three vertices, no buffers, one
triangle large enough to cover clip space, its uv from `vertex_index`. It
runs inside wgpu2d's 2D pass on the surface (the one the HUD will draw on),
loads texels with `textureLoad`, and is the only thing that draws the world
to the screen from here on. The crack decal and gizmo lines still draw on
the surface after it, as GL drew them after its post.

### 11d. Looking at the attachments

"Keep a debug view for every intermediate texture" was a working rule in the
roadmap, and this is where it earned its keep. `gbufferView.wgsl` draws any
attachment, the roughness or the depth into a small wgpu2d target that an
ImGui "G-buffer" header shows — only while it is open, because asking for
the preview is what makes the next frame draw it. `WGPU_GBUFFER_VIEW=1-5`
puts one on the screen instead of the composite, for scripted captures. The
view index is an `override`, so it is one shader and five pipelines. Every
milestone from here used these views before anything else.

---

## 12. Light: sun, ambient, point lights, and splitting the big shader

**Concepts:** The lit path of `defaultShader.frag` — 2,292 lines of GLSL —
becomes three WGSL files. What is WebGPU's in this: **WGSL has no
`#include`**, so wgpu2d's shader loader grew one; **`textureSample` must be
in uniform control flow**, so every texture is read before any branch;
**point lights in a per-frame storage buffer**; the **normal and material
maps** as more layers of the 7 texture array; and a **naga** pitfall with a
Metal reserved word. The lighting itself — GGX, Smith, Schlick, Oren-Nayar,
baked light levels, corner AO from a texture — is GL's, carried over
function by function.

**LearnWebGPU:** [Basic shading](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/basic-shading.html)
· [Lighting control](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/some-interaction/lighting-control.html)
· [Specularity](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/specular.html)
· [Normal mapping](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/normal-mapping.html)
· [Physically-Based Materials](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/pbr.html)
(WIP) · the BRDF itself is LearnOpenGL's [PBR Theory](https://learnopengl.com/PBR/Theory)
· [Uniformity](https://www.w3.org/TR/WGSL/#uniformity) in the WGSL spec

**Code:** `world.wgsl` (the shared `Uniforms`, now 256 bytes, and the sRGB
helpers) · `lighting.wgsl` (`computeAmbient`, `computePointLightSource`,
`pointLights`, `paintAlbedo`) · `readTexels` and `fs_main` in
`chunkFaces.wgsl` · `setChunkLights` and the per-frame light gather in
`render` · `expandIncludes` in `thirdparty/wgpu2d/src/wgpuContext.cpp` ·
`Lighting` in `wgpuWorld.h`

**Commit:** `1e9d065`

### 12a. Splitting a shader when the language has no includes

Three files: `world.wgsl` for what every world shader shares, `lighting.wgsl`
for the BRDF and the light sources, `chunkFaces.wgsl` for the faces. WGSL has
no preprocessor and no modules. wgpu2d's `createShaderModuleFromFile` now
expands a line `#include "name.wgsl"`, relative to the including file, each
file once per module — a forty-line textual include, deliberately nothing
more. Two consequences: a compile error's line number is in the expanded
text (naga reports it; the reload in 20 prints it), and the `Uniforms` struct
has exactly one definition that every shader and the C++ `static_assert`
agree on. The struct grew from 96 bytes to 256 here and to 576 by 18-parity,
every member 16-byte aligned so there is never padding to get wrong.

### 12b. Read every texture first

`textureSample` takes screen-space derivatives to choose a mip level, which
means it needs all four pixels of a 2×2 quad to reach it together; WGSL
enforces that statically as the **uniformity analysis**, and a
`textureSample` inside an `if` on a per-pixel value is a compile error, not
a warning. GL let the same code through and gave undefined mips. So
`chunkFaces.wgsl` has a `readTexels` step that samples albedo, normal map,
material map and the AO shapes up front, then shades with the results. Where
a read genuinely has to sit in a branch, the three escape hatches are
`textureSampleLevel` (15, explicit level), `textureSampleGrad` (18-parity,
explicit derivatives taken earlier) and `textureSampleCompareLevel` (13).
This rule shaped more shader code than any other in Phases C and D.

### 12c. The lights, and what they read

- **Baked light** comes from the face record: sky and block nibbles in the
  position's packed bits, through GL's ambient formula with the time of
  day's sky intensity and `u_baseAmbientExtra`.
- **The sun** is GL's `mainLightPosition` and colour, through the BRDF, with
  the normal map rotated into the face's frame.
- **Point lights.** Each chunk hands its light blocks to `setChunkLights`
  with its faces; each frame the visible chunks' lists are gathered, nearest
  first, up to `maxLights`, into a storage buffer. Measured 9–41 lights a
  frame in the test worlds.
- **Normal and material maps** are the texture array's slots +1 and +2 (7a
  had reserved four slots per block for exactly this), so lighting cost no
  new textures; the roughness now comes from the material map, and emissive
  texels write the bloom target.
- **The composite** applies the exposure, `ACESFitted` and the sRGB curve,
  which is `toneMap.frag`'s default; 15 replaces it with the full tone
  mapper. **Auto-exposure is held at GL's starting 1.6** until 16 ports the
  readback — turning the update on with a stubbed luminance drifted it to
  2.0.
- **No BRDF LUT**, because GL only samples it under a constant that is
  false. Porting behaviour means porting what runs.

**Pitfall:** naga does not rename MSL reserved words, so a struct member
named `fragment` compiles as WGSL and fails inside Metal's compiler with
"'user' attribute cannot be applied to types". The error names nothing
useful; the fix is a rename. Checked by eye in three worlds and in the
normal and roughness views: sunlit tops brighter than sides, AO in corners,
snow not clipped.

---

## 13. Shadows: three cascades, depth-only passes, and a comparison sampler

**Concepts:** **Cascaded shadow maps**, as GL had them: three **depth-only
render passes** (no colour attachment at all) from the sun's point of view,
each covering a slice of the camera's frustum, and a **comparison sampler**
in the lit shader that returns "lit or shadowed" instead of a depth. What
WebGPU adds: a pass may not draw into a texture its bind groups also bind,
so the shadow passes bind a **stand-in**; there is **no border colour**, so
off-map reads need a shader rule; the sampler's compare mode is a
`SamplerBindingType::Comparison` in the layout and a `textureSampleCompareLevel`
in WGSL, which needs no derivatives and is therefore allowed in loops. And
one deliberate correction to GL: the box's running maximum starts at
`lowest()`, not GL's `-numeric_limits<double>::min()`.

**LearnWebGPU:** [Shadow maps](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/shadow-maps.html)
is a 🔴 TODO stub. [Shadows](https://webgpufundamentals.org/webgpu/lessons/webgpu-shadows.html)
on WebGPU Fundamentals has the comparison sampler; the technique is
LearnOpenGL's [Shadow Mapping](https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping)
and [Cascaded Shadow Mapping](https://learnopengl.com/Guest-Articles/2021/CSM)

**Code:** `ensureShadowResources` · `shadowBindView` · `vs_shadow` and the
`shadowCascade` override in `chunkFaces.wgsl` · `sunShadow` in
`lighting.wgsl` · `sunCascadeMatrix` in `renderer.cpp` · the per-cascade cull
and the records joining the indirect buffer in `render` · the comparison
sampler (`compare = LessEqual`) in `ensureShadowResources` ·
`Lighting::shadows` / `shadowCascades`

**Commit:** `039e346`

### 13a. The matrices, in double, camera-relative, 0..1 deep

Each cascade's matrix is `renderShadow`'s: the camera frustum sliced at GL
depths 0.9985, 0.9995 and 0.99994, turned so the sun is +z, made a cube as
long as its diagonal, then an orthographic box 500 blocks deeper both ways.
`renderer.cpp` builds them in double from world coordinates;
`wgpuWorld` folds in the camera position (so the shader keeps 5e's
camera-relative positions) and remaps depth by z/2 + 1/2. That remap is
chosen so the shader compares **exactly the numbers GL's shader compared**,
and GL's biases carry over untouched — the cheapest way to port a bias is
to not change what it is applied to.

**The correction.** GL starts its box's running maximum at
`-numeric_limits<double>::min()`, which is about −2e−308, _not_ the most
negative double. So its box always reaches back to the world origin on each
axis, spreading the first cascade over hundreds of blocks a few hundred
blocks from spawn. Here it starts at `lowest()`. This is the one place
Phases B–D knowingly differ from GL, and the roadmap says so.

### 13b. Depth-only passes, culled three more times

Each cascade is a render pass with a depth attachment (2048, 2048 and 1024
texels, `Depth32Float`) and **no colour attachment** — `colorAttachmentCount
= 0` is legal, and the pipeline has no fragment targets. The vertex stage
`vs_shadow` places the face by `u.shadowMatrices[shadowCascade]`, the cascade
index an override so it is three pipelines; the fragment is the prepass's
`fs_depth`, so leaves and grass cast their cut-outs. Back faces culled, as
GL left `GL_CULL_FACE` on.

Every loaded chunk's opaque faces are culled against each cascade's box, as
GL looped over `loadedChunks`, and the surviving records are appended to
the frame's **indirect buffer** (8) — the same buffer, more records, no new
draw code. The cost is CPU: recording went from 40 µs to 144 µs (hard) and
178 µs (soft), almost all of it culling every loaded chunk three times.
Frame time stayed at the 13.3 ms vsync.

### 13c. Sampling: compare, not read

The lit shader picks the cascade by view depth (16 and 44 blocks), applies
GL's biases, and samples through a **comparison sampler**: `Nearest`,
`compare = LessEqual`, bound as `SamplerBindingType::Comparison`, called with
`textureSampleCompareLevel(map, sampler, uv, reference)`, which returns 0 or
1 (or a filtered fraction) rather than a depth. It takes no derivatives, so
it may sit in the branches and loops the nine-tap soft mode needs — 12b's
rule, with the escape hatch built for this purpose.

- **Hard** is one tap. **Soft** is GL's nine Vogel-disk taps, turned by
  interleaved gradient noise, the outer five first, to the power 2.2.
- **Off the map counts as lit.** GL clamped to a border of 1; WebGPU has no
  border colour, so the shader checks the coordinate and returns 1 outside
  0..1. The same absence shapes 16's blur.
- The shadow scales the sun term and the sky part of the ambient (up to five
  levels on sun-facing faces), on glass and water as well.

### 13d. The stand-in: when an attachment is also a binding

The lit pipelines' bind group holds the three shadow maps. The shadow passes
use the same group layout — and **a pass may not draw into a texture its bind
groups bind**, even if the shader never reads it. So the shadow passes bind
a copy of the world group with a 1×1 depth texture in the three map slots,
and that stand-in also fills the main group until shadows are first switched
on (36 MB of maps, made then, since GL's default is off). A copy of a bind
group with one entry swapped is the cheapest way to satisfy a rule that is
about the group, not the shader.

**Pitfall:** naga's override pass rejected GL's noise written as a `vec3`
constant with swizzles (`ExpressionAlreadyInScope`), failing every face
pipeline; it is written with literals. 16 hit the same with a weight array.

Checked with a temporary 0.15 day time, rendering the shadow factor alone:
far sides, houses, trees and grass blades cast where they should, no acne,
no seam between cascades. `OURCRAFT_SHADOWS=0|1|2` overrides the setting for
scripted runs. A testing note worth keeping: in a scripted run the player is
sometimes still falling at the capture frame and the frame is all sky —
check the hand's pose before blaming the shader.

---

## 14. The sky: cubemaps baked and drawn, fog toward a copy

**Concepts:** A **cubemap** in WebGPU is a 2D texture with six layers, sampled
through a `Cube` view and drawn into through a 2D view of each layer. GL's
three scattering skies are **baked** into three such textures (once, and
again when the settings change), then every frame **two are blended into
the HDR colour and a sky copy by one fullscreen triangle writing two
targets**, the fog in the lit shaders reads the copy, and the sun and moon
are quads at the far plane. The one place the port had to reason instead of
transcribe: the bake finds each texel's direction from the **cube-face
table**, not from GL's capture matrices, whose up-vectors compensate for
GL's bottom-up framebuffers and would turn the side faces over here.

**LearnWebGPU:** [Cubemap](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/cubemap.html)
· [Cubemap Conversion](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/cubemap-conversion.html)
and [Prefiltering](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/cubemap-prefiltering.html)
(the compute route; this port renders into the faces instead) ·
[Image-Based Lighting](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/ibl.html)
(WIP, and deliberately not done — see below) · WebGPU Fundamentals'
[Cube maps](https://webgpufundamentals.org/webgpu/lessons/webgpu-cube-maps.html)
and [Skybox](https://webgpufundamentals.org/webgpu/lessons/webgpu-skybox.html)
· the scattering is O'Neil's, GPU Gems 2
[chapter 16](https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-16-accurate-atmospheric-scattering)

**Code:** `sky.wgsl` (`fs_bake`, `fs_sky`, `vs_sun`/`fs_sun`) ·
`createSkyResources` / `createSky` / `bakeSkies` / `ensureSkyBindGroup` ·
`sunFrame` / `sunMatrix` / `drawSunAndMoon` · `computeFog` in
`chunkFaces.wgsl` · the frame group (group 1) · `SkyUniforms` /
`SkyBakeParams` · `Lighting::skyWeights`, `daySky*`, `fog*`

**Commit:** `a9e48f7`

### 14a. Baking: 18 small passes and a table run backwards

Three skies × six faces = eighteen render passes, each a fullscreen triangle
into a 2D view of one layer of one 128×128 texture, each draw's
`instance_index` naming the sky and the face (a direct draw may start at any
instance, so `draw(3, 1, 0, sky * 6 + face)` carries the index for free).
The bake shader needs each texel's world direction. GL got it by rendering
with six `lookAt` capture matrices whose up-vectors are chosen to make a
bottom-up framebuffer come out right. WebGPU's render targets are top-first
(5b), so those matrices would produce faces flipped on four sides. The port
instead runs the **cube-face table** — the mapping from (face, u, v) to a
direction, shared by OpenGL, Vulkan, Metal and WebGPU because sampling a
cubemap is specified the same way everywhere — backwards. That is the
general rule: when porting a render-to-cubemap, derive directions from the
sampling spec, never from the source's capture setup.

The scattering is GL's modified O'Neil shader with `fCosSun` forced to 0, so
each sky depends only on how high the direction points — which is also why
GL's per-frame rotation of the sky about the vertical by `clock()` could be
left out: a sky that varies only with elevation looks the same turned.

### 14b. Every frame: two targets, one triangle, then the fog

`fs_sky` blends the time of day's two skies (three weights, times 1.2) and
writes **two colour targets at once**: the HDR colour and a screen-sized sky
copy (GL's `fboSkyBox`, which GL filled by copying). The opaque pass then
_loads_ the colour instead of clearing it. The sky copy joins the **frame
group** (group 1, beside 9's peel copies), which the opaque lit pipelines now
bind too, and `computeFog` mixes the lit colour toward the sky at the pixel:
a near haze thinning with height and a hard far edge at the view distance
less a chunk, above water only. GL divides by the near-fog density, so its
setting of 0 — "disables it", by the menu — is a NaN; here 0 skips the near
fog.

The **sun and moon** are quads four units out along `sunPos` and opposite it,
scaled as GL scales two matrix entries, at the far plane with `LessEqual`
and no depth writes so terrain hides them, colour added by alpha, bloom
written, positions and normals masked. They draw at the start of
`renderTransparent`, after entities and before glass and water, as GL does.

**Not made:** GL's diffuse convolution and specular prefilter of each sky,
and the BRDF LUT. Only `physicallyAccurateReflections`, a false constant,
reads them. The guide's IBL and prefiltering chapters are the right reading
if that constant ever flips.

Checked with temporary day times of 0.10 and 0.60 and at noon in two
worlds: a morning sun's halo cut by a hill's silhouette, a night sky with
the moon and a purple horizon from the twilight blend, distant hills and
water fading into blue. Frame time unchanged.

---

## 15. The post chain's end: tone mapping and FXAA

**Concepts:** A **post pass** is a fullscreen triangle with one bind group
(uniforms, a source texture, a linear clamped sampler) and **one pipeline per
target format**. Two of them end GL's frame: the tone mapper, which turns
linear HDR into display values, and FXAA, which reads the result and
smooths edges by luma. The WebGPU lesson in this one is small and
recurring: **every read is `textureSampleLevel`**, because FXAA branches and
loops on what it reads, and the derivative-taking `textureSample` is not
allowed there (12b).

**LearnWebGPU:** no chapter. WebGPU Fundamentals'
[Post Processing](https://webgpufundamentals.org/webgpu/lessons/webgpu-post-processing.html)
· Simon Rodriguez's
[Implementing FXAA](http://blog.simonrodriguez.fr/articles/2016/07/implementing_fxaa.html),
which is the write-up GL's `fxaa.frag` follows

**Code:** `post.wgsl` (`fs_toneMap`, `fs_fxaa`) · `createPostResources` /
`createPost` · `fullscreenPipeline` · `PostUniforms` · the `rg11b10ufloat`
intermediate in `ensureTargets` · `Lighting::tonemapper` through `fxaa`

**Commit:** `b9d75bc`

### What the two passes do, and the format in between

- **Tone map:** exposure; GL's `adjustColor` (saturation, vibrance, shadow
  and highlight boost, lift and gain, gamma); one of five tone mappers
  (ACES, AgX "punchy", ZCAM with its gamut search, Uncharted 2, PBR
  Neutral); the sRGB curve except for AgX; a 1/255 gradient-noise dither;
  the vignette. With FXAA on (the default) it writes an intermediate target;
  with it off, the surface.
- **FXAA 3.11** reads the intermediate onto the surface inside wgpu2d's 2D
  pass. GL's `textureOffset` is `textureSampleLevel`'s offset argument.

**The intermediate is `rg11b10ufloat` because GL's `fboCoppy` is.** That
means FXAA sees GL's 5–6 mantissa-bit quantisation of the tone-mapped image
— about 1–2 levels in the sky — and the port reproduces it on purpose, for
parity. `rgba8unorm` would be a free improvement afterwards, and the roadmap
says so. The dither's noise is mirrored vertically against GL's, because
pixel rows run down here; invisible, and noted.

No ping-pong pair yet, and the reasoning for why not is worth keeping: every
step in this chain changes format, bloom (16) adds into the HDR colour by
blending, and the first pass that must _read and write the same HDR
texture_ is the underwater warp (18), which is where the second texture
appears.

Checked by switching the composite per frame — a temporary hack, removed —
against 14's: the centre identical, the corners darkened by exactly the
vignette GL's formula predicts (109 → 94 at a corner, 133 → 119 top centre),
nothing off by more than 32; FXAA moved 0.6% of pixels by more than 8
levels, on the hand's stair-stepped edges.

---

## 16. Bloom, and a readback that does not stall

**Concepts:** Two things GL did with features WebGPU lacks. **Auto exposure**
averaged the HDR colour with `glGenerateMipmap` and read the 1×1 level
through a pixel buffer a frame later; here the mip chain is **render passes
into single-mip views**, and the readback is `copyTextureToBuffer` into a
`MapRead` buffer, **`mapAsync` in the next frame, polled without blocking**.
**Bloom** is GL's sequence pass for pass — filter, two blur chains, add back
up, apply with lens dirt — each a fullscreen pass that reads one mip view
and writes another, because a texture may never be read and written in one
pass.

**LearnWebGPU:** [Mipmap Generation](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/mipmap-generation.html)
(the compute version; this is the render-pass version, chosen because every
level is also a sample point) ·
[Screen capture](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/screen-capture.html)
(WIP) for the readback mechanics; space-game 15 is the blocking version this
improves on · LearnOpenGL's [Bloom](https://learnopengl.com/Advanced-Lighting/Bloom)
and [Physically Based Bloom](https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom)
· the 13-tap downsample is from
[Next Generation Post Processing in Call of Duty: Advanced Warfare](https://advances.realtimerendering.com/s2014/sledgehammer/Next-Generation-Post-Processing-in-Call-of-Duty-Advanced-Warfare-v17.pptx)

**Code:** `bloom.wgsl` (`fs_filter`, `fs_down`, `fs_tent`, `fs_blur`,
`fs_apply`, `fs_average`) · `recordExposure` · `serviceReadback` /
`serviceReadbacks` · `takeExposureColor` · `mipChain` / `bloomMipsFor` /
`ensureBloomTargets` · `recordBloom` / `recordBloomApply` / `bloomGroup` /
`bloomApplyGroup` · `compositeFrame`'s order · `Lighting::bloom*` and the
lens dirt ids

**Commit:** `dfbaae5`

### 16a. The readback, done the non-blocking way

The constraints, each of which decided a line of code:

1. **No `glGenerateMipmap`.** An `rgba16float` chain from half the surface
   down to 1×1, one render pass per level into a single-mip view; a linear
   read at the target texel's centre averages the 2×2 under it, which is the
   box filter.
2. **A copy row is padded to 256 bytes**, so the 1×1 texel goes into a
   256-byte `MapRead | CopyDst` buffer.
3. **A mapped buffer, or one waiting to map, may not be used by a submit.**
   So the copy is recorded in frame _N_, and `wgpuBufferMapAsync` is called
   at the start of frame _N_+1's `render`, after frame _N_'s submit.
4. **The callback only fires inside a poll.** `wgpuDevicePoll(device, false,
   nullptr)` — non-blocking — runs it; the callback only sets a state, and
   the halves are read and the buffer unmapped on the next pass through.
   No new copy is recorded while a reading is in flight.

On this Mac a reading arrives **every frame** (398 in 400); GL's pixel
buffer gave one every other frame. `takeExposureColor` hands each to the
renderer once, which runs GL's arithmetic — the 1/2.2 power, the luminance,
`adaptiveExposure.update` — again, after 12 had held it. A bright snow scene
averages about (1.09, 0.89, 0.72) and the exposure heads toward 1.05; grass
reads 0.39 and holds near 1.61.

Contrast with space-game 15, where the capture pumped `processEvents` until
the map landed: that was fine for a screenshot and would be a stall every
frame. 18 reuses this scheme as `serviceReadback` for the flare's count.

### 16b. Bloom, pass for pass

The filter adds the HDR colour's bright pixels into the G-buffer's bloom
target (blend `One, One`, on top of what emissive blocks put there in 12).
Two `rg11b10ufloat` blur chains follow — mip 0 at half the surface, GL's
level count (5 at 500×500) — with a view and a bind group per level. Per
level: the 13-tap downsample, the tent copy (divided by 12, as GL's), a
Gaussian each way, ping-ponging. The levels are added back up smallest
first, and the result with its three lens dirt textures (lens, hit, water;
nearest sampled, as gl2d loads them) is added onto the HDR colour. GL's
`horizontal`, `firstTime` and `lastBloomChannel` flags run on the CPU and
pick each pass's bind group; `horizontal` is also an override.

- **No border colour** again: GL clamped the blur textures to black; `tap()`
  returns black outside 0..1 instead (GL also fades the outermost half
  texel; this does not).
- **GL's apply pass landed on the HDR colour by accident.** It bound
  `fboMain.color`, a texture name, as a framebuffer, and only drew there
  because HBAO's pass had left that framebuffer bound; with SSAO off it
  would draw elsewhere. The port draws on the HDR colour always. Porting
  behaviour sometimes means porting the behaviour the author meant.
- The renderer computes the threshold and multiplier from the last frame's
  luminosity before `render`, one frame earlier than GL. naga rejected the
  Gaussian's weight array (`ExpressionAlreadyInScope`, as 13's noise), so
  its loop is written out.

Checked by switching bloom per frame: at default settings it lifts the mean
by about 8 levels, a haze, which GL's numbers predict — a threshold near
0.005 lets almost everything through, and the tent copies gain about 13×
over five levels. At 100× the multiplier the frame washes out with the lens
dirt's blobs at the edges. _(18-parity: the lens dirt was upside down. It is
screen space, where both libraries put the image's top at the top, so it
reads straight; the v-flip habit from 10a had been applied one time too
many.)_

---

## 17. Ambient occlusion and screen-space reflections

**Concepts:** Two screen-space effects that read the G-buffer, and the
parity fix that let them: the **transparent passes now write every
attachment** as GL's did, so AO over a lake sees the water, not the lake
bed. **SSAO** (GL calls it HBAO; its live code is SSAO) at half size into
`r8unorm`, applied as a blurred darkening. **SSR** inside the lit shader, a
ray march through _last frame's_ positions reading _last frame's_ colour —
which means the composite keeps a **history**: an `rgba8unorm` copy of the
HDR colour made by a draw, because a texture copy cannot convert formats,
and the positions copied as they are.

**LearnWebGPU:** no chapter. LearnOpenGL's
[SSAO](https://learnopengl.com/Advanced-Lighting/SSAO) is the technique;
David Lettier's
[Screen Space Reflection](https://lettier.github.io/3d-game-shaders-for-beginners/screen-space-reflection.html)
is the clearest walk through the march-and-binary-search GL's shader does

**Code:** `hbao.wgsl` (`fs_hbao`, `fs_apply`) · `createHbaoResources` /
`createHbao` / `ensureHbaoTargets` / `recordHbao` · `HbaoUniforms` ·
`ColorTargets::GBufferTransparent` · the SSR block of `fs_main` in
`chunkFaces.wgsl` · `recordLastFrame` · the frame group's bindings 4–6 ·
`Uniforms::projection` (528 bytes) · `Lighting::ssao`, `aoViewDistance`, `ssr`

**Commit:** `48abe17`

### 17a. AO

Twenty of thirty-two hemisphere samples half a block around the view-space
normal, each projected to the screen and counted when the G-buffer's
position there is in front of it (bias 0.2, range-checked), into a
half-size `r8unorm` target — GL's `fboHBAO` was `GL_RED`. The apply is a 4×4
box of that, faded out toward the view distance, drawn over the HDR colour
as black with alpha AO × 1.45 (`SrcAlpha, OneMinusSrcAlpha`), between the
bloom's blur and its add, where GL's is.

- **Positions through the linear sampler**, as GL's `GL_LINEAR` targets: a
  half-size pixel's centre falls between four full-size ones.
- **Normals by `textureLoad`.** GL samples its `RGB16UI` normals with an
  `isampler2D` and `GL_LINEAR`, which the GL spec makes an incomplete
  texture; drivers that allow it return the texel. Integer textures cannot
  be filtered in WebGPU at all, so the texel is what you get, and it is what
  GL was getting.
- The box's y offsets are negated (GL's rows run up). The pass and its apply
  each get their own bind group: 13d's rule, since the apply draws into what
  the pass read from.

### 17b. SSR, and a history of the frame

GL's standalone SSR pass is `if(0)`; its reflections live in the lit shader
under `c_SSR`, for faces smoother than 0.45. The port is GL's: reflect the
view ray about the mapped normal, jitter by `hash33` of the position (murmur
on the float bits — `bitcast` in WGSL), step up to 50 times by a length set
by how squarely the face is seen, compare against last frame's view-space
positions, binary-search 20 steps, fade at the screen's edges, take last
frame's colour. Water takes the reflection whole before its peel mix; other
faces mix by Fresnel and roughness.

- **The ray stays in GL's texture coordinates** (v up), so its −1..1 bounds
  test reads as GL's, and v is flipped only at the sample. Keeping the
  source's coordinate system inside an algorithm and converting at the
  edges is less error-prone than converting every line.
- **Nothing is rebuilt from depth** — positions are stored — so the
  depth-convention risk the roadmap flagged for Phases C–D never arrived.
  The x, y and w rows of the 0..1 projection, now in the uniforms, equal
  GL's, which is why its screen-projection math carries over.
- **History.** After the bloom apply, the HDR colour is _drawn_ into an
  `rgba8unorm` copy (GL's `fboLastFrame` is RGBA8, so reflections clamp to
  0..1; `copyTextureToTexture` requires matching formats, so a format change
  is a draw) and the positions are copied. Both join the frame group. GL's
  `textureLod(…, 2)` on that one-mip texture reads level 0, so there is
  nothing to port there.

Checked by switching each per frame: AO darkens 5% of `test`'s pixels by
more than 8 levels, at grass bases and on the hand; SSR turns a pond from
flat cyan to the cobblestone wall behind it, cyan left where rays miss.

---

## 18. God rays, a flare count without occlusion queries, and under water

**Concepts:** Three effects, and one WebGPU limitation that forced a design.
**God rays** are the sun drawn again at half size, masked by depth, and
radially blurred toward its screen position. **The lens flare** needs to know
how many of the sun's pixels are visible — GL used `GL_SAMPLES_PASSED` —
and **WebGPU occlusion queries only report zero or non-zero**, so the count
is a **storage-buffer atomic** incremented by the sun's own fragments, with a
manual depth test, read back with 16's scheme. **Under water** is a darkening
in the lit shader plus a warp pass that is the first to read and write the
HDR colour, and so brings the second texture 15 predicted.

**LearnWebGPU:** no chapter. The radial blur is GPU Gems 3
[chapter 13](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process);
atomics are in the WGSL spec; occlusion query semantics in the WebGPU spec

**Code:** `godRays.wgsl` (`fs_mask`, `fs_radialBlur`) · `fs_sunRays` and
`sunSamples: atomic<u32>` in `sky.wgsl` · `underwater.wgsl` ·
`createRaysAndWarpResources` / `createRaysAndWarp` / `ensureRayTargets` /
`recordSunRays` · `sunVisibleSamples` · `RayUniforms` / `WarpUniforms` ·
`computeFogUnderWater` in `chunkFaces.wgsl` · `Lighting::underWater*`

**Commit:** `c437147`

### 18a. God rays

Right after the sun pass, against the depth as the opaque faces and entities
left it (so water does not block them): the sun and moon drawn again as
instances 2 and 3 of the sun's draw, at half size into an `rg11b10ufloat`
target, with GL's settings for this copy. `fs_mask` blacks out wherever the
scene has depth; `fs_radialBlur` runs 200 taps from each pixel toward the
sun's screen position, falling off linearly, the length modulated by a sine
of the angle to make 20 rays. The blur runs in GL's coordinates (v up) so
the sine is not mirrored. The bloom apply adds the result as
`applyBloomData` does — squared over the sky, linear over geometry — so, as
in GL, **the rays only show with bloom on**.

### 18b. Counting the sun's pixels

GL's flare is drawn by the 2D renderer, which already ran; what it lacked was
its occlusion query count. WebGPU's `occlusionQuerySet` answers "did any
fragment pass?" and nothing finer, so the count had to be built:

- Each half-size fragment of the sun's god-ray quad checks the **four
  full-size depth texels under it** against the far plane and
  `atomicAdd`s those in front of the sky to a `var<storage, read_write>
  sunSamples: atomic<u32>`.
- **The depth test is done by hand** in the shader. A fragment shader that
  writes memory cannot assume the fixed-function depth test ran before it —
  implementations may run the test after the shader when the shader has
  side effects, so the count would include occluded fragments.
- The atomic is cleared each frame and read back through the readback
  scheme from 16, now a shared `serviceReadback`. 376 counts arrived in 380
  frames; `wgpuWorld::sunVisibleSamples` feeds GL's flare code its
  `queryDataForSunFlare`.

This is the general shape of "the API gives me a yes/no where I need a
number": count it yourself in a storage buffer, and accept the read-back
latency.

### 18c. Under water

In the lit shader, the colour darkens toward the underwater colour with
distance (`computeFogUnderWater`, strength 0.94) before the reflections. The
**warp pass** follows the bloom apply: a sine wobble of the coordinates (in
GL's, v up), a 10% tint, the sky painted outright. It **writes its own
texture, which the tone mapper then reads** — a pass cannot read what it
draws into, so this is the second HDR texture, arriving exactly where 15
said the first read-and-write pass would put it. The settings are used as
GL uses them, unconverted; positions and colour both sampled linearly, so a
light fringe follows silhouettes against the sky, which GL's code shows too.
One divergence kept on purpose: GL with FXAA on and under water tone maps
into `fboMain.color` before its last-frame copy, so its next frame's
reflections read tone-mapped colour; this keeps the HDR copy.

**Colour grading** turned out to be nothing: GL has no LUT, its grading is
`adjustColor`, ported in 15. The roadmap row had been written from the
devlogs.

### 18-parity, the porting half: animation and the water's textures

What 6–18 had left out, all of it movement:

- **Animation in every vertex pass.** `defaultShader.vert`'s
  `calculateVertexPos` is `animatedCorner`: water's two travelling waves on
  its surface corners (all 14 water orientations, GL's 56-entry mask), the
  leaves' sines on every corner, the grass's top corners pushed along one
  direction. Normals for every shape but cube faces and still water come,
  as in GL, from three animated corners. **Every vertex entry point goes
  through `faceVertex`**, so the prepass, the solid pass, the transparent
  passes and the shadow maps all move alike — 11b's `Equal` only holds if
  the prepass and the solid pass displace a vertex identically.
- **The clock is wall seconds since start.** GL used `std::clock() / 1000`,
  which is wall time only on Windows; on macOS it counts CPU milliseconds
  and would run about a thousand times fast. A unit that happens to be
  right on the author's platform is a porting hazard of its own kind.
- **Water's textures** (`waterDUDV.png`, `normal.png`, `caustics.jpg`) join
  group 0 with a linear, mipmapped, repeating sampler, as gl2d loads them.
  They tile by world position, so v is negated to read GL's texel. **The
  reads sit in branches**, so each coordinate carries the screen derivatives
  of its base, taken up front in `readTexels`, for `textureSampleGrad` —
  12b's third escape hatch — and terrain out of water pays nothing for them.
- The dudv refraction (what is under the water looked up 0.009 off along
  the map, unless that lands on something in front of the water) and the
  caustics on faces in water (flag bit 1, tinting the lights with GL's
  chromatic offset) complete 9c's list of what waited.

Checked: frames 60 apart differ only along leaf and grass edges; the pond
ripples; switching caustics off every other frame changes only the pond
bed, in bands. The comparison half of 18-parity still needs a GL frame,
which this Mac cannot produce.

**Commit:** `e430d30`

---

## 19. Deleting OpenGL

**Concepts:** Mostly not WebGPU. What belongs here is what the port's shape
made possible and what it cost to finish: four steps, each leaving the game
buildable and drawing the same, checked pixel for pixel against the step
before. The `opengl-final` tag keeps the last GL build and every GLSL file
each WGSL was ported from.

**LearnWebGPU:** no chapter — the guide never had a second renderer to
remove.

**Code:** everything that is gone. `renderer.cpp` (7,056 → 2,846 lines) ·
`shader.*`, `sunShadow.*`, `bigGpuBuffer.*`, `nullGL.*`, `nullGLStubs.inl`,
`tools/genNullGL.py`, `thirdparty/glad`, `thirdparty/gl2d`,
`imgui_impl_opengl3.*`, 43 GLSL files, `errorReporting.cpp`, `noiseTool/`,
`PBRTool/` · `GLFW_INCLUDE_NONE` as a compile definition · `newMeshId` ·
`loadSpawnChunkAndPlaceSpawn` in `server.cpp`

**Commits:** `e05fce0` + `936fdd0` 19a · `6a51738` the spawn · `b18cc0d` 19b
· `6c0496a` 19c · `0e6828f` 19d

### What the steps were, and what each taught

- **19a — the build is WebGPU only.** `RENDERER_WEBGPU` and its guards
  resolved to the WebGPU side by `unifdef -DRENDERER_WEBGPU=1` (which cannot
  parse C++14 digit separators; two guards by hand). The first commit
  recorded only the deleted directories — a failed `git add` left the edits
  unstaged — and the second added them. A commit that does not build on its
  own is worth a note in the next one.
- **The spawn race, fixed between 19a and 19b.** The world could not be
  diffed pixel for pixel because the spawn varied: the server corrected the
  spawn height only on a tick that found the spawn chunk loaded, and chunks
  load after a player joins. `loadSpawnChunkAndPlaceSpawn` now runs at
  startup, generating the spawn chunk and two each way so neighbouring
  trees have landed. **A reproducibility fix was a prerequisite for
  measurement**, and it is a gameplay change made for the renderer's sake.
- **19b — GL leaves the smaller files; fake handles become real ids.** A
  texture slot is the wgpu2d id itself (supersedes 7g); meshes come from
  `newMeshId()`; textures are freed with `cleanupTextureId` where GL deleted
  them. `glfwMain.cpp` defines `GLFW_INCLUDE_NONE`: the window is a WebGPU
  surface and needs no GL header.
- **19c — `renderer.cpp`'s GL passes go.** What stays is what a frame
  computes on the CPU and hands to `wgpuWorld`: time-of-day lighting,
  culling and sorting, the block cubes, bones and instances, the decal, the
  water timer, the exposure update, the flare, the geometry tables and the
  icons. One behaviour GL carried by accident had to be kept on purpose:
  the icon pass returned early until the GL icon shader existed, which
  skipped the first call (made before `create`); a `created` flag keeps
  that order, because without it the shader reload test shifted by one.
- **19d — the scaffold goes.** `nullGL`, glad, ImGui's GL backend, the GLSL.
  `GLFW_INCLUDE_NONE` moves to a compile definition of the target so no
  file gets the system GL header, which on Linux need not even be
  installed.

Every step was checked the same way: the title menu pixel for pixel, `test`
at 88 chunks and 193,071 opaque faces with at most 0.04% of pixels off by
more than 32 (swaying leaves), `test2`'s items and models, the shader reload
running 20's sequence, a forced texture-pack reload. Three of the runs
looked toward the horizon because the real mouse was moving; the two after
matched. **19e** — Windows and Linux — waits for real machines; expect MSVC
and gcc fixes, and naga's HLSL and SPIR-V output to differ from its Metal.

---

## 20. Hot reload: rebuilding every pipeline from disk

**Concepts:** In GL a shader is an object you relink; in WebGPU **the WGSL is
baked into the pipeline**, so a reload is a rebuild of every pipeline and
pipeline layout, and of nothing else. Which forces a separation the code had
been missing: **resources once, pipelines many times**. And since a rejected
pipeline is a live handle in an invalid state (space-game 11), a reload has
to be **all or nothing** with error scopes deciding which.

**LearnWebGPU:** no chapter. The error-scope half is the guide's
[Debugging](https://eliemichel.github.io/LearnWebGPU/appendices/debugging.html)
appendix, via space-game 11.

**Code:** `reloadShaders` · `forEachReloadable` · `createPipelineSet` and
the `create*Resources` / `create*` split (`createPostResources`/`createPost`,
`createBloomResources`/`createBloom`, `createHbaoResources`/`createHbao`,
`createRaysAndWarpResources`/`createRaysAndWarp`,
`createSkyResources`/`createSky`) · `Renderer::reloadShaders`

**Commit:** `fd93886`

### How it works, and the two things it taught

`Renderer::reloadShaders` — GL's `R` key, or a shading setting GL used to
recompile for (which also fires once on entering a world) — calls
`wgpuWorld::reloadShaders`. `forEachReloadable` lists the **42 pipelines and
17 layouts**. A reload moves them aside, builds a new set from the files on
disk through the same `createPipelineSet` that startup uses, and keeps the
new set only if every pipeline was made; otherwise it releases what it made
and puts the old set back. A WGSL error prints with its line, naga's
message, and the game keeps drawing. The skies re-bake after, as GL's reload
redid its sky textures; the block icons' and block textures' pipelines run
once at load and are not reloaded. About 20 ms.

- **The split was overdue.** Five create functions had made bind group
  layouts, buffers, samplers _and_ pipelines together, because at the time
  each was written once. A reload is the first caller that wants half of
  that, and splitting them turned out to be mechanical — which is the usual
  sign that the seam was there all along.
- **All-or-nothing is only possible because of error scopes.** Without them
  a bad pipeline is non-null and the failure arrives per frame, after the
  old pipelines are gone. With them, `createPipelineSet` can say "no" before
  anything is swapped.

Checked by script, editing `post.wgsl` between `R` presses: a red tint
appears on reload, a syntax error is reported and the tint stays, restoring
the file brings the normal image back.

---

## Patterns across 8–20

Things that came up in three or more milestones, stated once.

**Attachment ≠ binding.** A pass may neither sample a texture it draws into
nor bind one in any group, read or not. Every workaround in the port is one
of three shapes: **copy it** between passes (9's peel copies, 17's position
history), **draw it into a second texture** when the format must change or
the pass must read and write the same image (17's `rgba8` history, 18's
warp), or **bind a stand-in** when the layout demands a slot the pass does
not read (13's 1×1 depth, 17's second bind group for the AO apply).

**Uniformity decides which sample function.** `textureSample` only in
uniform control flow (12). In a branch or loop: `textureSampleLevel` with a
level (15, and every post pass), `textureSampleGrad` with derivatives taken
earlier (18-parity's water), `textureSampleCompareLevel` for shadows (13),
`textureLoad` with integer coordinates when the texel is known (9, 11, 17).

**Overrides where GL had uniforms-as-switches or separate shaders.**
`passMode`, `zBias`, `shadowCascade`, `view`, `horizontal`: five
`override`s, each turning one module into several pipelines at creation.
Together with one-pipeline-per-state (9b), they are why 14 WGSL files
replaced 43 GLSL files and no `if (u_mode == …)` runs per fragment.

**Queue order is the only synchronisation.** `writeBuffer` and submits are
ordered; passes in an encoder are ordered; dispatches in a pass are ordered.
The arena's growth copy (6d), the indirect buffer's reuse (8b), the mip
chains (7d, 16) and the whole post chain rely on it and nothing else. The
one place order is not enough — reading GPU results on the CPU — is the
readback scheme (16a, 18b), and its rule is: copy in frame _N_, map in
_N_+1, poll without blocking.

**Where v flips, and where it does not.** Sampled textures: wgpu2d stores
rows top first, gl2d stored them bottom first, so textures that _came
through gl2d's loader_ (crack, entity and item textures, water's maps) flip
v at the sample; the block texture array does not, because 7d flipped it
once at build time. Render targets are top-first, so **screen-space
algorithms written in GL's coordinates (v up) are kept in GL's coordinates
and flipped only at the read** (17's ray, 18's blur and warp), and **images
that are already screen space do not flip at all** (16's lens dirt, found
the hard way). Cubemap faces come from the sampling table, not the capture
matrices (14a). When in doubt, check with an asymmetric texture (10b's
dummy).

**naga, three times.** It compiles WGSL to MSL and is its own authority:
MSL reserved words are not renamed (`fragment`, 12); its override pass
rejects a constant array or swizzled constant used in an expression
(`ExpressionAlreadyInScope`, 13 and 16 — write the literals out); its error
lines refer to the expanded source (12a). None of these are in any guide.

**Port what runs, correct what is wrong, and say which.** The BRDF LUT and
the IBL maps are not made because a false constant guards them (12, 14).
GL's bloom apply landed where it did by accident, and the port lands there
on purpose (16b). The shadow box's `-min()` and the fog's division by zero
are corrected (13a, 14b). `std::clock` is replaced (18-parity). Each is
written down in the roadmap row, which is what makes the difference between
a port and a fork.

---

## This adapter, and what it does differently

Facts that decided something in 5–20, all specific to this machine on Metal
through wgpu-native v24. space-game's table in the companion file still
applies (Metal never reports `Outdated`, no `TimestampQuery`, and so on).

| What you see | What it is |
| --- | --- |
| `maxSampledTexturesPerShaderStage` is 128 | A `binding_array` of block textures cannot hold 448, so no bindless-shaped solution (7a) |
| `maxTextureArrayLayers` is 2048 on the adapter, 256 on a default device | Every layer count between is a device-creation request (7b) |
| `Depth32Float` with stencil ops set is a validation error | Stencil load/store must be `Undefined` on a depth-only format (5a) |
| `maxAnisotropy > 1` with a `Nearest` filter is a validation error | GL's 4× anisotropy on pixel art is dropped (7e) |
| `multiDrawIndirect` is no faster than a loop | Metal has no native multi-draw; wgpu loops `drawIndirect` underneath (8c) |
| `PresentMode::Immediate` still gives 13.3 ms frames | macOS composites windowed apps at the display's 75 Hz; `nextDrawable` waits regardless. "Uncapped" here means the CPU side only (8c) |
| `maxColorAttachmentBytesPerSample` is 32, and `rg11b10ufloat` counts as 8 | GL's five targets (33 bytes) become four; roughness rides in the positions' alpha (11a) |
| `rg11b10ufloat` is only renderable with `RG11B10UfloatRenderable` | Requested when present; the HDR format falls back to `rgba16float` without (11a) |
| No border colour on samplers | Shadow reads off the map return lit in the shader (13c); the blur's `tap()` returns black outside 0..1 (16b) |
| Occlusion queries report only zero or non-zero | The flare's visible-pixel count is a storage-buffer atomic, read back a few frames late (18b) |
| A `MapRead` buffer's reading arrives every frame (398 of 400) | GL's pixel buffer gave one every other frame; the exposure converges faster here (16a) |
| naga: MSL reserved words are not renamed | A struct member named `fragment` fails inside Metal's compiler with an unrelated message (12) |
| naga: `ExpressionAlreadyInScope` from the override pass | A constant array or a swizzled `vec3` constant in an expression; write the literals out (13, 16) |
| `std::clock()` counts CPU time | GL's animation clock ran about 1000× fast on macOS; replaced with wall seconds (18-parity) |
| No GL world on this Mac | Every check in 5–20 is a self-consistency check: CPU projection vs screenshot, culled vs unculled, per-frame toggles of one effect, pixel diffs between adjacent milestones. The comparison half of 18-parity waits for a machine whose GL draws |

---

## Guide chapters, and where each landed

The guide's 3D and compute chapters against the milestone that used them.
🔴 marks a chapter that is a stub in the guide, where this file points
elsewhere.

| Guide | Milestone |
| --- | --- |
| [A simple example](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/a-simple-example.html), [Depth buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/depth-buffer.html), [Transformation](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/transformation-matrices.html) and [Projection matrices](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/projection-matrices.html) | 5 |
| [Instanced Drawing](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/instanced-drawing.html) 🔴 | 6a (per-instance attributes), 10a and 10b (storage-buffer records) |
| [Compute Pipeline](https://eliemichel.github.io/LearnWebGPU/basic-compute/compute-pipeline.html), [Mipmap Generation](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/mipmap-generation.html) | 7d (compute), 16a (the render-pass version) |
| [Index Buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/index-buffer.html), [Loading from file](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/loading-from-file.html) | 10b (models) |
| [Deferred Shading](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/deferred-shading.html) 🔴, [HDR textures](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/hdr-textures.html) (WIP) | 11 |
| [Basic shading](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/basic-shading.html), [Lighting control](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/some-interaction/lighting-control.html), [Specularity](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/specular.html), [Normal mapping](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/normal-mapping.html), [PBR](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/pbr.html) (WIP) | 12 |
| [Shadow maps](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/shadow-maps.html) 🔴 | 13 |
| [Cubemap](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/cubemap.html), [Cubemap Conversion](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/cubemap-conversion.html) | 14 |
| [Convolution Filters](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/convolution-filters.html) | 16b, 17a — as render passes, not compute, because GL's were |
| [Screen capture](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/screen-capture.html) (WIP) | 16a (the non-blocking readback) |
| [Debugging](https://eliemichel.github.io/LearnWebGPU/appendices/debugging.html) (WIP) | 20 (error scopes making the reload all-or-nothing) |
| [Image-Based Lighting](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/lighting-and-material/ibl.html), [Cubemap Prefiltering](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/cubemap-prefiltering.html) | **Not done.** GL only reads its IBL maps under a false constant (14) |
| [Multi-Sampling](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/multi-sampling.html) 🔴 | Not done; GL used FXAA (15), and so does this |
| [Render Bundles](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/render-bundles.html) 🔴 | Not done; the indirect buffer (8) covers the same ground for the chunks |
| [Benchmarking / Time](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/benchmarking/time.html) | Not applicable on this adapter (space-game 16). Every timing in this file is CPU recording time or a frame rate |
| [Building for the Web](https://eliemichel.github.io/LearnWebGPU/appendices/building-for-the-web.html) | Not done; native only. 19e is Windows and Linux |

---

## File cheat sheet

| File | Role |
| --- | --- |
| `include/gameLayer/rendering/wgpuWorld.h` | the seam: the `Lighting` struct, the per-frame calls (`render`, `renderEntities`, `renderTransparent`), the data feeds, no WebGPU types |
| `src/gameLayer/rendering/wgpuWorld.cpp` | everything GPU-side: arena, tables, texture array, G-buffer targets, every pipeline, every pass, the readbacks, the reload |
| `resources/shaders/webgpu/world.wgsl` | the shared `Uniforms` (576 bytes) and the sRGB helpers, `#include`d by the world shaders |
| `resources/shaders/webgpu/chunkFaces.wgsl` | faces: `faceVertex` and the animation, `vs_main` / `vs_shadow`, `fs_depth` / `fs_main` / `fs_blend` / `fs_decal`, fog, SSR, underwater, water's refraction and caustics |
| `resources/shaders/webgpu/lighting.wgsl` | the BRDF, ambient from light levels, point lights, paint, the shadow sampling |
| `resources/shaders/webgpu/entities.wgsl` | block cubes, gizmo lines, skinned models, items |
| `resources/shaders/webgpu/blockTextures.wgsl` | compute: the block texture array's `fill` and `downsample` |
| `resources/shaders/webgpu/blockIcons.wgsl` | the inventory icons, rendered to targets at load |
| `resources/shaders/webgpu/sky.wgsl` | cubemap bake, the per-frame sky into two targets, the sun and moon, the god-ray copies with the flare count |
| `resources/shaders/webgpu/gbufferView.wgsl` | the composite's debug views of each attachment |
| `resources/shaders/webgpu/bloom.wgsl` | bloom's filter, downsample, tent, blur, apply; the exposure average |
| `resources/shaders/webgpu/hbao.wgsl` | SSAO and its apply |
| `resources/shaders/webgpu/godRays.wgsl` | depth mask and radial blur |
| `resources/shaders/webgpu/underwater.wgsl` | the warp pass |
| `resources/shaders/webgpu/post.wgsl` | tone mapping and FXAA |
| `thirdparty/wgpu2d/include/render/wgpuFrame.h` | what the world borrows: device, queue, frame encoder and texture, surface size and format, texture views by id, `wgpuMarkTargetDrawn`, `ErrorScope` |
| `thirdparty/wgpu2d/src/wgpuContext.cpp` | the device's features and limits (`maxTextureArrayLayers`, `IndirectFirstInstance`, `MultiDrawIndirect`, `RG11B10UfloatRenderable`); the shader loader's `#include` |
| `src/gameLayer/rendering/renderer.cpp` | the CPU half of a frame: lighting values, culling and sort, cubes, bones, the decal, the exposure update, the flare, the geometry tables, the icons; `reloadShaders` |
| `src/gameLayer/rendering/chunk.cpp` | `sendDataToOpenGL` → `setChunkFaces` / `setChunkTransparentFaces` / `setChunkLights`; `clearGpuData` → `removeChunk` |
| `src/gameLayer/rendering/model.cpp`, `src/gameLayer/blocksLoader.cpp` | `setMesh` / `removeMesh`; texture slots as wgpu2d ids |
| `src/platform/glfwMain.cpp` | `wgpuWorld::shutdown`; `OURCRAFT_INPUT` (`move@`, `key@`, `hold@`, `quit`), `WGPU_SCREENSHOT_FRAME`, `WGPU_GBUFFER_VIEW` |
| `docs/roadmap.md` | the conventions, and each milestone's row with its verification notes |

**Environment variables the port added**, since they are how a scene is
reproduced without a keyboard: `OURCRAFT_WORLD`, `OURCRAFT_INPUT`,
`WGPU_SCREENSHOT_FRAME` (a list), `WGPU_GBUFFER_VIEW=0-5`,
`OURCRAFT_WORLD_DRAWS=direct|indirect|multi`, `OURCRAFT_SHADOWS=0|1|2`.

## Reference shelf

Beyond the guide. [WebGPU Fundamentals](https://webgpufundamentals.org/) for
the API explained from the JavaScript side —
[Storage Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-storage-buffers.html),
[Vertex Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-vertex-buffers.html),
[Storage Textures](https://webgpufundamentals.org/webgpu/lessons/webgpu-storage-textures.html),
[Compute Shaders](https://webgpufundamentals.org/webgpu/lessons/webgpu-compute-shaders.html),
[Memory Layout](https://webgpufundamentals.org/webgpu/lessons/webgpu-memory-layout.html),
[Limits and Features](https://webgpufundamentals.org/webgpu/lessons/webgpu-limits-and-features.html),
[Constants](https://webgpufundamentals.org/webgpu/lessons/webgpu-constants.html),
[Transparency](https://webgpufundamentals.org/webgpu/lessons/webgpu-transparency.html),
[Shadows](https://webgpufundamentals.org/webgpu/lessons/webgpu-shadows.html),
[Cube maps](https://webgpufundamentals.org/webgpu/lessons/webgpu-cube-maps.html),
[Skybox](https://webgpufundamentals.org/webgpu/lessons/webgpu-skybox.html),
[Post Processing](https://webgpufundamentals.org/webgpu/lessons/webgpu-post-processing.html),
[A large triangle to cover clip space](https://webgpufundamentals.org/webgpu/lessons/webgpu-large-triangle-to-cover-clip-space.html).
[LearnOpenGL](https://learnopengl.com/) for the techniques as the GL code
knew them — [Deferred Shading](https://learnopengl.com/Advanced-Lighting/Deferred-Shading),
[HDR](https://learnopengl.com/Advanced-Lighting/HDR),
[Gamma Correction](https://learnopengl.com/Advanced-Lighting/Gamma-Correction),
[PBR Theory](https://learnopengl.com/PBR/Theory),
[Shadow Mapping](https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping)
and [CSM](https://learnopengl.com/Guest-Articles/2021/CSM),
[Cubemaps](https://learnopengl.com/Advanced-OpenGL/Cubemaps),
[Bloom](https://learnopengl.com/Advanced-Lighting/Bloom) and
[Physically Based Bloom](https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom),
[SSAO](https://learnopengl.com/Advanced-Lighting/SSAO).
The papers behind specific passes:
[Accurate Atmospheric Scattering](https://developer.nvidia.com/gpugems/gpugems2/part-ii-shading-lighting-and-shadows/chapter-16-accurate-atmospheric-scattering)
(14), [Volumetric Light Scattering as a Post-Process](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process)
(18), [Implementing FXAA](http://blog.simonrodriguez.fr/articles/2016/07/implementing_fxaa.html)
(15), [Next Generation Post Processing in Call of Duty: Advanced Warfare](https://advances.realtimerendering.com/s2014/sledgehammer/Next-Generation-Post-Processing-in-Call-of-Duty-Advanced-Warfare-v17.pptx)
(16), [Screen Space Reflection](https://lettier.github.io/3d-game-shaders-for-beginners/screen-space-reflection.html)
(17), [Depth Precision Visualized](https://developer.nvidia.com/content/depth-precision-visualized)
(5b). Brandon Jones's [WebGPU best practices](https://toji.dev/webgpu-best-practices/)
for the costs — [bind groups](https://toji.dev/webgpu-best-practices/bind-groups.html),
[buffer uploads](https://toji.dev/webgpu-best-practices/buffer-uploads.html),
[image textures](https://toji.dev/webgpu-best-practices/img-textures.html).
wgpu's [`Features`](https://docs.rs/wgpu/latest/wgpu/struct.Features.html)
page for what the native extensions in `wgpu.h` mean. The
[WebGPU spec](https://www.w3.org/TR/webgpu/) for limits and validation rules,
the [WGSL spec](https://www.w3.org/TR/WGSL/) for
[uniformity](https://www.w3.org/TR/WGSL/#uniformity),
[override declarations](https://www.w3.org/TR/WGSL/#override-decls),
[`@invariant`](https://www.w3.org/TR/WGSL/#invariant-attribute), atomics and
the texture functions. And, as always, the vendored `webgpu.h`, `wgpu.h` and
`webgpu.hpp` under `build/_deps/`, which are the only authority on what this
wgpu-native accepts.
