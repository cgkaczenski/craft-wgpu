# WebGPU 3D learning outline

A table of contents for Phase B of the render port: the first three milestones
that draw the world rather than the menus. Each section is one concept
cluster: what the concept is, the LearnWebGPU chapter for it, where it landed
in this repo, and the commit that introduced it. The companion file,
[`webgpu-learning-outline.md`](webgpu-learning-outline.md), is space-game's
outline for the 2D stack that `wgpu2d` brought with it; nothing in it is
repeated here. The plan these milestones came from, and the conventions they
settled, are in [`roadmap.md`](roadmap.md).

The 3D side lives in one translation unit, `src/gameLayer/rendering/wgpuWorld.cpp`,
behind a header that names no WebGPU types
(`include/gameLayer/rendering/wgpuWorld.h`), so `renderer.cpp`, `chunk.cpp`
and `glfwMain.cpp` call it without compiling `webgpu.hpp`. Two shaders:
`resources/shaders/webgpu/chunkFaces.wgsl` draws, `blockTextures.wgsl` builds
the texture array. `wgpu2d` still owns the device, the surface and the frame;
the world borrows them through `thirdparty/wgpu2d/include/render/wgpuFrame.h`.

**The rule these milestones followed:** port _behaviour_, not structure. The
GL renderer's data stays exactly what it was — the 16-byte face `Chunk::bake`
writes, the corner and UV tables `recreateBlockGeometryData` builds, the handle
table `BlocksLoader` fills — and only what the GPU does with it changes. That
is why the game code changed by a handful of `#if RENDERER_WEBGPU` lines that
hand the same vectors to `wgpuWorld`.

Guide: [Learn WebGPU for C++](https://eliemichel.github.io/LearnWebGPU/index.html),
**With webgpu.hpp** tab. The guide builds one textured, lit mesh viewer; the
world here is tens of thousands of faces a frame from one buffer, so the guide
covers the _objects_ (depth texture, projection, storage buffer, compute pass)
and this file covers what the game does with them. Where the guide has no
chapter, or a 🔴 TODO stub, the section says so and points elsewhere —
mostly [WebGPU Fundamentals](https://webgpufundamentals.org/), which is
JavaScript but describes the same API, and the
[WebGPU](https://www.w3.org/TR/webgpu/) and [WGSL](https://www.w3.org/TR/WGSL/)
specs for the rules.

---

## How to read this

Each block is: **concepts → LearnWebGPU chapters → where it landed → commit**.
Sub-blocks expand one idea when the milestone taught more than one. Numbers
in the text (draw counts, layer counts, milliseconds) are what the build
printed on this machine in the `test` world, not estimates.

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
side.

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

### 7h. What the frame shows, and what it does not

GL cannot draw the world on this Mac, so the check was the frame itself:
grass sides upright (the flip in `fill` is the right way round), cut-outs
clean, no validation errors across the build and the first frames, and the
numbers above in the log. **Not done:** lighting of any kind — the shade is
still `vertexColor` per orientation; the grass and leaf colour tint in the
texture index's high bits; the sway animation; transparent and water faces;
and the UI's block icons, which still draw white (roadmap 3b) because
`renderAllBlocksUiTextures` renders through GL.

---

## This adapter, and what it does differently

Facts that decided something in 5–7, all specific to this machine on Metal
through wgpu-native v24. space-game's table in the companion file still
applies (Metal never reports `Outdated`, no `TimestampQuery`, and so on).

| What you see                                                            | What it is                                                                                                                           |
| ----------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `maxSampledTexturesPerShaderStage` is 128                               | A `binding_array` of block textures cannot hold 448, so no bindless-shaped solution (7a)                                             |
| `maxTextureArrayLayers` is 2048 on the adapter, 256 on a default device | Every layer count between is a device-creation request (7b)                                                                          |
| `Depth32Float` with stencil ops set is a validation error               | Stencil load/store must be `Undefined` on a depth-only format (5a)                                                                   |
| `maxAnisotropy > 1` with a `Nearest` filter is a validation error       | GL's 4× anisotropy on pixel art is dropped (7e)                                                                                      |
| No GL world on this Mac                                                 | Every check in 5–7 is a self-consistency check: CPU projection vs screenshot, culled vs unculled, flip direction by eye (5f, 6e, 7h) |

---

## Not in these milestones (guide chapters skipped or deferred)

| Guide                                                                                                                            | Why                                                                                                                 |
| -------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| [Index Buffer](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/input-geometry/index-buffer.html)                     | Faces are a strip per instance; nothing shares vertices. ImGui (space-game 8) is the only index buffer in the build |
| [Basic shading](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/basic-shading.html), Lighting and Material | Phase C. The `vertexColor` table is the stand-in                                                                    |
| [Loading from file](https://eliemichel.github.io/LearnWebGPU/basic-3d-rendering/3d-meshes/loading-from-file.html) (OBJ)          | Milestone 10 (assimp models)                                                                                        |
| [Instanced Drawing](https://eliemichel.github.io/LearnWebGPU/advanced-techniques/instanced-drawing.html)                         | Done here (6a), but the chapter is a stub; WebGPU Fundamentals filled in                                            |
| Multi-draw / indirect                                                                                                            | Milestone 8. wgpu-native's `MultiDrawIndirect` is an extension in `wgpu.h`, not core                                |
| Transparency, depth peeling                                                                                                      | Milestone 9. The depth buffer's "About transparency" aside in the guide is the problem statement                    |
| [Convolution Filters](https://eliemichel.github.io/LearnWebGPU/basic-compute/image-processing/convolution-filters.html)          | Phase D (bloom, blur)                                                                                               |
| Cube maps, IBL                                                                                                                   | Milestone 14 (sky)                                                                                                  |

---

## File cheat sheet

| File                                           | Role                                                                                                                                                  |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `include/gameLayer/rendering/wgpuWorld.h`      | the seam: five functions, no WebGPU types                                                                                                             |
| `src/gameLayer/rendering/wgpuWorld.cpp`        | arena, tables, texture array build, depth texture, the world pass                                                                                     |
| `resources/shaders/webgpu/chunkFaces.wgsl`     | vertex: corner from table, camera-relative position; fragment: sample the array, alpha test                                                           |
| `resources/shaders/webgpu/blockTextures.wgsl`  | compute: `fill` (resample + flip into a layer), `downsample` (mips for all layers)                                                                    |
| `thirdparty/wgpu2d/include/render/wgpuFrame.h` | what the world borrows: device, queue, surface size and format, `wgpuBeginSurfacePassWithDepth`, `wgpuTextureView`, `ErrorScope`                      |
| `thirdparty/wgpu2d/src/wgpuContext.cpp`        | the depth pass; `requiredLimits.maxTextureArrayLayers` at device creation                                                                             |
| `src/platform/nullGL.cpp`                      | `getTextureHandle` carrying the texture id                                                                                                            |
| `src/gameLayer/rendering/renderer.cpp`         | `recreateBlockGeometryData` → `setBlockGeometry`; `recreateBlocksTexturesBuffer` → `setBlockTextures`; `renderFromBakedData` → `render` after culling |
| `src/gameLayer/rendering/chunk.cpp`            | `sendDataToOpenGL` → `setChunkFaces`; `clearGpuData` → `removeChunk`                                                                                  |
| `src/platform/glfwMain.cpp`                    | `wgpuWorld::shutdown`; `OURCRAFT_INPUT` `move@x,y`, `key@Fn`, `quit`                                                                                  |
| `docs/roadmap.md`                              | the conventions (depth 0..1, y up, top-first targets, unflipped textures, camera-relative positions)                                                  |

## Reference shelf

Beyond the guide. [WebGPU Fundamentals](https://webgpufundamentals.org/) for
the API explained from the JavaScript side — [Storage Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-storage-buffers.html),
[Vertex Buffers](https://webgpufundamentals.org/webgpu/lessons/webgpu-vertex-buffers.html),
[Storage Textures](https://webgpufundamentals.org/webgpu/lessons/webgpu-storage-textures.html),
[Compute Shaders](https://webgpufundamentals.org/webgpu/lessons/webgpu-compute-shaders.html),
[Memory Layout](https://webgpufundamentals.org/webgpu/lessons/webgpu-memory-layout.html),
[Limits and Features](https://webgpufundamentals.org/webgpu/lessons/webgpu-limits-and-features.html).
Brandon Jones's [WebGPU best practices](https://toji.dev/webgpu-best-practices/)
for the costs — [bind groups](https://toji.dev/webgpu-best-practices/bind-groups.html),
[buffer uploads](https://toji.dev/webgpu-best-practices/buffer-uploads.html),
[image textures](https://toji.dev/webgpu-best-practices/img-textures.html).
The [WebGPU spec](https://www.w3.org/TR/webgpu/) for limits and validation
rules, the [WGSL spec](https://www.w3.org/TR/WGSL/) for `textureSample` with
an array index, `@interpolate(flat)`, and the storage texture types. And,
as always, the vendored `webgpu.h`, `wgpu.h` and `webgpu.hpp` under
`build/_deps/`, which are the only authority on what this wgpu-native
accepts.
