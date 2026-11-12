# Shader effects learning outline

A table of contents for the _effects_: what each one does to the picture, how
it works, the kinds there are, what they are used for, where this game has
one, and where to read more. The two companion files are organised by
milestone and by API object — [`webgpu-learning-outline.md`](webgpu-learning-outline.md)
for the 2D stack, [`webgpu-3d-learning-outline.md`](webgpu-3d-learning-outline.md)
for the 3D port — and say how each pass was _built_ in WebGPU. This one is
organised by technique and says what the pass is _for_. Where the 3D outline
already walks a pass through (tone mapping in 15, bloom in 16, AO and
reflections in 17, god rays in 18, shadows in 13, fog in 14), this file gives
the idea and points there; nothing is repeated.

Nothing here is API-specific. The shaders named are the fourteen WGSL files
under `resources/shaders/webgpu/`, but every technique reads the same in
GLSL or HLSL, and most of the reading list is written in one of those.

---

## How to read this

Each block is: **what it does → how it works → kinds and uses → in this game
→ read**. "In this game" names the file and function, or says plainly that
the game does not have it. Formulas are for colours in 0..1 unless the text
says linear HDR.

Three kinds of source are worth telling apart, because they answer different
questions:

- **Explainers** teach one technique from nothing: The Book of Shaders,
  LearnOpenGL, Catlike Coding, David Lettier's
  [3D Game Shaders For Beginners](https://lettier.github.io/3d-game-shaders-for-beginners/),
  Acerola's videos. Start with these.
- **Frame breakdowns** take one frame of a shipped game apart, pass by pass:
  [Behind the Pretty Frames](https://mamoniem.com/category/behind-the-pretty-frames/),
  Adrian Courrèges's graphics studies. They show _which_ effects a real game
  runs, in _what order_, at _what resolution_ — the things an explainer
  never tells you. Read them after you know what a bloom pass is.
- **Production source** is how an engine really writes it: Unity's HDRP
  [post-processing shaders](https://github.com/Unity-Technologies/Graphics/tree/ce1905322722287c1420a6d85de14158c1648a84/Packages/com.unity.render-pipelines.high-definition/Runtime/PostProcessing/Shaders).
  Dense, full of platform cases, and the place to look when an explainer's
  version has an artefact and you want to know what the fix is.

The [resource shelf](#resource-shelf) at the end has all three, with a map
of the HDRP folder.

---

## 1. What a shader is, and where an effect can live

**What it is:** A shader is a small program the GPU runs many times at once.
A **vertex shader** runs once per vertex and says where it lands on screen.
A **fragment shader** runs once per covered pixel and says what colour it
is. A **compute shader** runs over a grid of your choosing and writes
buffers or textures directly. Every effect in this file is one of those
three doing something to positions, to colours, or to data.

**The one limit that shapes everything:** a fragment shader knows its own
pixel and nothing about its neighbours. It cannot ask "what colour is the
pixel to my left" of the image it is drawing. It _can_ read any texel of a
**texture made by an earlier pass**. That is the whole reason frames are
built from passes: blur, bloom, edge detection, reflections and upscaling
all need neighbours, so each one draws the scene into a texture first and
then reads it.

**Three places an effect can live:**

1. **In the surface's own shader**, while the surface is drawn. It knows the
   material, the normal, the world position. Lighting, shadows, fog and (in
   this game) reflections live here.
2. **In a geometry pass of its own.** Something extra is drawn into the
   scene or into a side texture: the shadow maps, particles, rain, the sun
   drawn again for god rays.
3. **In a post pass**: one triangle covering the screen, a fragment shader
   that reads the finished image (and often depth, positions or normals)
   and writes a new image. Colour correction, bloom, blur, edge detection,
   dithering, upscaling and overlays live here.

**The building blocks**, which every effect below is made of:

| Block | What it does |
| --- | --- |
| `mix(a, b, t)` | Blend two values. Fog, fades and every "strength" slider |
| `step`, `smoothstep` | Turn a number into a hard or soft mask |
| `dot(c, vec3(0.2126, 0.7152, 0.0722))` | Luma: how bright a linear colour looks. Saturation, edge tests, bloom thresholds |
| A hash or noise function | Repeatable randomness from a position. Dither, grain, rain, variation |
| A kernel | A weighted sum of a pixel's neighbours. Blur, sharpen, edge detection |
| A mip chain | The image at half size, again and again. Wide blurs for cheap, averages |
| Ping-pong | Two textures that take turns as source and target, since a pass cannot read what it writes |
| Linear vs display values | Light adds up in linear; screens want the sRGB curve. Which side of the tone mapper an effect sits on changes what it does |

**In this game:** the frame, in order. The world draws into the G-buffer
(HDR colour, positions, normals, bloom, depth), lit as it is drawn; then
the sky, god rays, transparent faces and rain; then the composite in
`wgpuWorld.cpp`'s `compositeFrame`: exposure average, bloom blur, AO, bloom
apply, the underwater warp, last frame's copy for reflections; then
`post.wgsl`'s `fs_toneMap` (grading, tone map, sRGB, screen effects,
splatter, dither, vignette, PSX colour), FXAA, the 2D UI, and `crt.wgsl`
over everything. The 3D outline's sections 11–18 are that list, one pass at
a time.

**Read:** [The Book of Shaders](https://thebookofshaders.com/) for fragment
shaders from nothing ·
[3D Game Shaders For Beginners](https://lettier.github.io/3d-game-shaders-for-beginners/),
one short page per effect with before and after pictures · LearnOpenGL's
[Framebuffers](https://learnopengl.com/Advanced-OpenGL/Framebuffers), the
chapter where "draw to a texture, then draw the texture" first appears,
with kernels · WebGPU Fundamentals'
[Post Processing](https://webgpufundamentals.org/webgpu/lessons/webgpu-post-processing.html)
· Inigo Quilez's [articles](https://iquilezles.org/articles/) for the maths
behind most tricks · [Shadertoy](https://www.shadertoy.com/) to read and
edit thousands of single-pass effects in the browser

---

## 2. Colour correction

**What it does:** Changes the colours of the finished image without knowing
what is in it. It is where a game gets its mood: warm and washed out, cold
and harsh, bright noon or dim cave. Each control is one line of maths.

**How it works**, control by control:

| Control | What it does | The line |
| --- | --- | --- |
| Exposure | Overall brightness, as a camera would. In linear HDR | `c *= exp2(stops)` |
| White balance | Makes the picture warmer or cooler by scaling channels | `c *= vec3(r, g, b)` |
| Contrast | Pushes values away from a middle grey, or toward it | `c = (c - mid) * k + mid` |
| Brightness | Adds a constant. Lifts blacks, unlike exposure | `c += b` |
| Saturation | Moves each colour away from, or toward, its own grey | `c = mix(vec3(luma), c, s)` |
| Vibrance | Saturation that leaves already-vivid colours alone | saturation masked by how saturated `c` is |
| Lift, gamma, gain | Three sliders for shadows, midtones, highlights | `c = pow(c * gain + lift, 1 / gamma)` |
| Colour filter, tint | Multiplies by a colour, or blends toward it | `c *= tint` |
| Tone mapping | Squeezes unbounded HDR light into 0..1 with a curve | Reinhard `c / (1 + c)`, filmic curves, ACES, AgX |
| Gamma | The display's transfer curve, applied last | the sRGB curve, roughly `pow(c, 1 / 2.2)` |

Three things beginners get wrong:

- **"Gamma" means two things.** The sRGB curve is not a look, it is the
  encoding a screen expects, and it must be applied exactly once. A creative
  gamma slider is a separate power that brightens or darkens midtones.
- **Order and space matter.** Exposure and white balance belong in linear
  light, before the tone mapper, because that is where doubling a number
  doubles the light. Contrast about 0.5 only makes sense on display values,
  after it. The usual order is: exposure → white balance → grading → tone
  map → sRGB → display-space effects → dither.
- **Tone mapping is not optional once the scene is HDR.** Without it,
  anything brighter than 1 clips to flat white. The curve decides how
  highlights roll off and how much colour they keep, which is most of what
  "filmic" means.

**Lookup tables (LUTs):** every control above is a function from a colour to
a colour, so all of them together can be baked into one small 3D texture
(32 or 33 texels a side) and applied with a single read. Artists grade a
screenshot in a paint program, and the same edits applied to a neutral LUT
image become the game's grade. Engines build the LUT on the GPU each frame
from the sliders; HDRP's `LutBuilder3D.compute` is that.

**In this game:** all in `post.wgsl`'s `fs_toneMap`. Exposure is a multiply
by the auto exposure (the 3D outline's 16a explains where the number comes
from). `adjustColor` is saturation, vibrance, shadow and highlight boost,
lift and gain, and gamma, in linear light. `tonemap` picks one of five
curves (ACES, AgX, ZCAM, Uncharted 2, PBR Neutral), `toDisplay` applies the
sRGB curve, and `screenEffects` then does contrast about 0.5, a tint, a fade
to a colour and coloured screen edges on display values. The gameplay
roadmap's G2 presets (hit, low health, poisoned) are curves over time
feeding those controls and the blurs of section 4. There is no LUT; the 3D
outline's 18c says why.

**Read:** Catlike Coding's
[Color Grading](https://catlikecoding.com/unity/tutorials/custom-srp/color-grading/)
builds every control in the table and then the LUT, and its
[HDR](https://catlikecoding.com/unity/tutorials/custom-srp/hdr/) chapter
does tone mapping · [Tone Mapping](https://64.github.io/tonemapping/) by
Matt Taylor compares the curves with plots · John Hable's
[Filmic Tonemapping Operators](http://filmicworlds.com/blog/filmic-tonemapping-operators/)
(the Uncharted 2 curve) · Krzysztof Narkowicz's
[ACES fit](https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/)
· Khronos's
[PBR Neutral](https://www.khronos.org/news/press/khronos-pbr-neutral-tone-mapper-released-for-true-to-life-color-rendering-of-3d-products)
· John Novak's
[What every coder should know about gamma](https://blog.johnnovak.net/2016/09/21/what-every-coder-should-know-about-gamma/)
· Lettier's [Lookup Table](https://lettier.github.io/3d-game-shaders-for-beginners/lookup-table.html)
and GPU Gems 2's
[chapter 24](https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-24-using-lookup-tables-accelerate-color)
for LUTs · [ASC CDL](https://en.wikipedia.org/wiki/ASC_CDL), the film
industry's slope, offset and power, which lift, gamma and gain descend from
· Björn Ottosson's [Oklab](https://bottosson.github.io/posts/oklab/) for a
colour space where "saturation" and "hue" behave · Inigo Quilez's
[palettes](https://iquilezles.org/articles/palettes/)

---

## 3. Quantisation and dithering

**What quantisation does:** Reduces how many different values a pixel may
take. Every image is quantised somewhere: an 8-bit screen has 256 levels a
channel. Done on purpose, with far fewer levels, it is a style.

```wgsl
// n levels per channel
let quantised = floor(c * (n - 1.0) + 0.5) / (n - 1.0);
```

- **Posterisation** is that line with a small `n`: flat bands of colour.
- **Bit depth reduction** is the same with a power of two. The PlayStation
  drew in 15-bit colour, 32 levels a channel.
- **Palette quantisation** replaces each colour with the nearest one from a
  fixed list (the Game Boy's four greens, a 16-colour pixel-art palette).
  Building a good palette _from_ an image is its own problem: median cut,
  k-means, octrees.
- **Positions quantise too.** Snapping vertices to a coarse grid is the
  PlayStation's wobble; drawing at a low resolution quantises where edges
  can be.

**The problem it causes:** banding. A smooth gradient, such as a sky or a
fog bank, becomes visible steps, because many neighbouring pixels round to
the same level and then all jump together. It shows even at 8 bits in dark
scenes.

**What dithering does:** adds a small, deliberate error _before_ rounding,
different at each pixel, so that neighbouring pixels round different ways.
A region that should be 30% between two levels ends up with about 30% of
its pixels on the upper one. The eye averages the neighbourhood and sees
the in-between value. Dithering trades banding for noise, and the kinds
differ in what the noise looks like.

```wgsl
// threshold is in 0..1 and differs per pixel
let dithered = floor(c * (n - 1.0) + threshold) / (n - 1.0);
```

**The kinds:**

| Kind | Where the threshold comes from | Looks like | Notes |
| --- | --- | --- | --- |
| White noise | A hash of the pixel | TV static, clumpy | Simplest. Clumps read as dirt |
| Ordered (Bayer) | A small matrix tiled over the screen, 2×2, 4×4 or 8×8 | A regular crosshatch | The retro look. One table read, stable from frame to frame |
| Blue noise | A precomputed texture whose noise has no low frequencies | Even, fine grain with no pattern | The best looking for the cost. Needs the texture |
| Interleaved gradient noise | One line of arithmetic (Jimenez) | Fine diagonal grain | Nearly blue-noise quality with no texture |
| Error diffusion | Each pixel's rounding error is pushed onto unvisited neighbours (Floyd–Steinberg, Atkinson, Jarvis) | Organic, wormy | Best for stills. Pixels depend on each other in scan order, so it does not suit a fragment shader, and it crawls when anything moves |
| Halftone | Dots that grow with brightness, on a rotated grid | Newsprint | Clustered dots survive printing; a style on screen |
| Temporal | Any of the above with the pattern changed each frame | Shimmering, smooth when averaged | Lets temporal anti-aliasing average the noise away |

The Bayer matrix is built by recursion: start with `[[0, 2], [3, 1]]`, and
each larger one is four copies of the last, scaled by four, plus 0, 2, 3
and 1. Divide by the number of cells to get thresholds.

**Uses**, and only the second is about style:

1. **Removing banding.** Add noise of one output step (1/255) just before
   the image is written at 8 bits. Invisible, and the sky's steps vanish.
   Every engine does this last.
2. **A retro or printed look.** Quantise hard and let the dither pattern
   show: 1-bit (Return of the Obra Dinn), console palettes, the PSX.
3. **Transparency without blending.** Discard a fraction of a surface's
   pixels by threshold ("screen-door" transparency). No sorting, works in a
   G-buffer, and is how distance fade-outs and level-of-detail crossfades
   are often done.
4. **Spreading samples.** Rotate a shadow or ambient-occlusion sampling
   pattern by a per-pixel noise value, so nine taps a pixel look like many
   more after the eye, or a blur, averages neighbours. It is the same
   trade: banding for noise.

**The hard part of the retro look is motion.** A screen-space pattern stays
fixed while the scene slides under it, which reads as a dirty window, and a
1-bit image flickers as surfaces cross thresholds. Lucas Pope's Obra Dinn
devlog is the best account of fighting that.

**In this game:**

- `fs_toneMap` adds `gradientNoise` (interleaved gradient noise) scaled to
  1/255, against banding. Use 1.
- `psxColour` is the PSX look's colour step, use 2: the display value as 8
  bits, the PlayStation's own 4×4 offsets added (−4 to +3), the low three
  bits dropped, leaving 15-bit colour. Indoors it drops two bits and halves
  the dither. It runs last so nothing smooths the steps, and FXAA is
  skipped for the same reason.
- `world.wgsl`'s `psxSnap` quantises vertex positions to the same grid, and
  `psxCellUv` makes the picture one read per cell.
- The soft shadows turn their nine taps by interleaved gradient noise (the
  3D outline's 13c). Use 4.

**Read:** Surma's [Ditherpunk](https://surma.dev/things/ditherpunk/), the
best single article, with every kind side by side · Tanner Helland's
[eleven error-diffusion algorithms](https://tannerhelland.com/2012/12/28/dithering-eleven-algorithms-source-code.html)
· Bart Wronski's three parts:
[simple quantisation](https://bartwronski.com/2016/10/30/dithering-part-one-simple-quantization/),
[golden ratio and blue noise](https://bartwronski.com/2016/10/30/dithering-part-two-golden-ratio-sequence-blue-noise-and-highpass-and-remap/),
[real-world 2D dithering](https://bartwronski.com/2016/10/30/dithering-part-three-real-world-2d-quantization-dithering/)
· Mikkel Gjøl's [Banding in Games](https://loopit.dk/banding_in_games.pdf)
(Playdead, INSIDE): where banding comes from in a whole pipeline and how to
dither each stage · Alan Wolfe's
[What the heck is blue noise](https://blog.demofox.org/2018/01/30/what-the-heck-is-blue-noise/)
and his
[interleaved gradient noise](https://blog.demofox.org/2022/01/01/interleaved-gradient-noise-a-different-kind-of-low-discrepancy-sequence/)
· Christoph Peters's [free blue noise textures](http://momentsingraphics.de/BlueNoise.html)
· Lucas Pope's
[Obra Dinn dithering devlog](https://forums.tigsource.com/index.php?topic=40832.msg1363742#msg1363742)
· psx-spx's [GPU page](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/),
the source of `psxColour`'s table · Acerola's
[Godot-PSX](https://github.com/GarrettGunnell/Godot-PSX), which G1c follows
· Wikipedia's [Ordered dithering](https://en.wikipedia.org/wiki/Ordered_dithering),
[Floyd–Steinberg](https://en.wikipedia.org/wiki/Floyd%E2%80%93Steinberg_dithering),
[Color quantization](https://en.wikipedia.org/wiki/Color_quantization) and
[Halftone](https://en.wikipedia.org/wiki/Halftone) · Lettier's
[Posterization](https://lettier.github.io/3d-game-shaders-for-beginners/posterization.html)
and [Pixelization](https://lettier.github.io/3d-game-shaders-for-beginners/pixelization.html)

---

## 4. Blur: the effect inside the other effects

**What it does:** Replaces each pixel with a weighted average of its
neighbourhood. On its own it is out-of-focus glass. Inside other effects it
is everything: bloom is a blur of the bright parts, depth of field is a
blur that varies with depth, soft shadows blur a yes/no answer, ambient
occlusion is blurred to hide its noise, sharpening and edge detection
subtract a blur from the image.

**How it works:** a **kernel**, a small grid of weights that sum to one.

- **Box:** every weight equal. Cheap, and bright points turn into squares.
- **Gaussian:** a bell curve. The standard, and **separable**: blur
  horizontally into a second texture, then blur that vertically, and a
  9×9 kernel costs 18 reads a pixel instead of 81.
- **The linear-sampling trick:** a bilinear read between two texels returns
  their weighted average for free, so a 9-tap Gaussian fits in 5 reads.
- **Downsample, blur, upsample:** a wide blur at full size is expensive; the
  same blur on a half or quarter size image is cheap and looks the same.
  Chains of half-size images are how every wide blur is made.
- **Kawase and dual filtering:** a few diagonal taps per level on the way
  down and up. Most blur per read; what mobile engines use.
- **Disc and bokeh:** taps spread over a circle, for lens-shaped blur.
- **Radial and directional:** taps along a line toward a point, or along
  the direction of motion. God rays, zoom blur, motion blur.
- **Bilateral:** a blur whose weights also fall with difference in depth or
  colour, so it stops at edges. Used to smooth AO and to upscale
  half-resolution effects without leaking across silhouettes.

**In this game:** `bloom.wgsl`'s `fs_down` (13 taps), `fs_tent` and
`fs_blur` (a separable Gaussian, ping-ponging); `post.wgsl`'s `sceneColor`
(16 taps on a golden-angle disc, each also pulled toward the centre for the
radial blur: G2's blur, radial blur and double vision); `hbao.wgsl`'s 4×4
box over the AO; `godRays.wgsl`'s `fs_radialBlur` (200 taps toward the
sun).

**Read:** Lettier's [Blur](https://lettier.github.io/3d-game-shaders-for-beginners/blur.html)
· Daniel Rákos's
[Efficient Gaussian blur with linear sampling](https://www.rastergrid.com/blog/2010/09/efficient-gaussian-blur-with-linear-sampling/)
· Catlike Coding's [Bloom](https://catlikecoding.com/unity/tutorials/advanced-rendering/bloom/),
whose first half is the downsample-and-upsample blur built step by step ·
Lettier's [Sharpen](https://lettier.github.io/3d-game-shaders-for-beginners/sharpen.html)

---

## 5. Bloom

**What it does:** Makes bright things glow past their edges, as light does
in a real lens or eye. It is what tells the viewer "this is brighter than
the screen can show": a lamp, the sun on water, lava.

**How it works:**

1. **Pick the bright parts.** Keep what is above a threshold (with a soft
   knee so the cut does not pop), or keep everything at a low weight.
2. **Blur them widely**, with a chain of half-size images (4).
3. **Add the levels back up**, smallest first, so the glow has a tight core
   and a wide halo.
4. **Add the result onto the scene**, before tone mapping.

**What makes it good or bad:**

- **It needs HDR.** With colours clamped at 1, a white wall and the sun are
  the same value and both glow. With HDR the sun is 50 and the wall is 0.8.
- **Fireflies.** One very bright pixel flickers into a blinking blob. The
  first downsample weights each sample by `1 / (1 + luma)` (the "Karis
  average") to tame it.
- **The threshold is a tuning knob, not physics.** The physically based
  version drops it: blur the whole image and blend a few percent of it
  back. Subtler and more stable.
- **Lens dirt** multiplies the glow by a smudge texture, so light sources
  seem to catch dust on the lens.

**In this game:** `bloom.wgsl`, walked through in the 3D outline's 16b:
`fs_filter` adds bright pixels to what emissive blocks already wrote,
`fs_down` and `fs_tent` and `fs_blur` build two blur chains, and `fs_apply`
adds the result with three lens dirt textures. The god rays ride in on the
same apply.

**Read:** LearnOpenGL's [Bloom](https://learnopengl.com/Advanced-Lighting/Bloom)
and [Physically Based Bloom](https://learnopengl.com/Guest-Articles/2022/Phys.-Based-Bloom)
· Catlike Coding's [Bloom](https://catlikecoding.com/unity/tutorials/advanced-rendering/bloom/)
· Jorge Jimenez's
[Next Generation Post Processing in Call of Duty: Advanced Warfare](https://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare/),
the source of the 13-tap downsample · Lettier's
[Bloom](https://lettier.github.io/3d-game-shaders-for-beginners/bloom.html)
· HDRP's `BloomPrefilter.compute`, `BloomBlur.compute` and
`BloomUpsample.compute`, the three steps as three files

---

## 6. Fog and volumetric lighting

**What it does:** Makes the air visible. Distant things fade toward a
colour, which gives depth and hides where the world stops being drawn. In
its full form, light itself shows in the air: shafts through a window,
beams under trees, a glow around a lamp in mist.

**How it works**, cheapest first:

1. **Distance fog.** Blend each surface toward a fog colour by how far away
   it is: `mix(fog, c, f)`.
   - Linear: `f = (end - d) / (end - start)`.
   - Exponential: `f = exp(-density * d)`. This is the real law for light
     crossing a uniform medium (Beer–Lambert).
   - Exponential squared: `f = exp(-pow(density * d, 2.0))`. Clear nearby,
     then a wall.
2. **Height fog.** Density falls with altitude, so valleys fill and
   hilltops stand clear. The amount along a view ray has a closed form, so
   it costs no more than distance fog.
3. **A fog colour that depends on direction.** Blend the fog toward the
   sun's colour when looking at the sun, or read it from the sky. A cheap
   way to fake light scattering.
4. **God rays as a post pass.** Draw the light source, mask it by what is
   in front, and blur radially away from its screen position. Convincing
   and cheap, and it stops working the moment the light leaves the screen.
5. **Ray-marched volumetrics.** For each pixel, step along the view ray;
   at each step ask the shadow map "is this point of air lit?", and add a
   little light if so, dimmed by the fog between there and the camera.
   This gives true shafts from any light, on or off screen. It is run at
   half resolution, with each pixel's starting distance dithered (3) and
   the result blurred across depth edges (4), or it costs too much.
6. **Froxel volumes.** A low-resolution 3D texture fitted to the view
   frustum holds fog density and light for each cell; a compute shader
   fills it and accumulates front to back; every surface and particle then
   reads its fog with one lookup. What current engines do.

The two words in the maths: **extinction** (how much light the fog removes
on the way through) and **in-scattering** (how much light the fog sends
toward the eye). A **phase function** says how much scatters in each
direction; Henyey–Greenstein is the usual one, and its forward peak is why
fog glows around the sun.

**In this game:** kinds 1 to 4. `chunkFaces.wgsl`'s `computeFog` and
`applyFog` fade faces by distance and height toward a copy of the sky (the
3D outline's 14b), so the fog colour follows the view direction.
`world.wgsl`'s `applySnowFog` is the snow's whiteout (indoors, only on what
the open sky lights). `computeFogUnderWater` darkens with distance under
water.
`godRays.wgsl` is kind 4 (the 3D outline's 18a). There are no ray-marched
volumetrics; the three shadow cascades they would read already exist.

**Read:** Inigo Quilez's [Better Fog](https://iquilezles.org/articles/fog/):
height fog and sun-tinted fog in a page · Lettier's
[Fog](https://lettier.github.io/3d-game-shaders-for-beginners/fog.html) ·
GPU Gems 3's
[Volumetric Light Scattering as a Post-Process](https://developer.nvidia.com/gpugems/gpugems3/part-ii-light-and-shadows/chapter-13-volumetric-light-scattering-post-process)
(kind 4) · Alexandre Pestana's
[Volumetric Lights](https://www.alexandre-pestana.com/volumetric-lights/)
(kind 5, with the dither and the bilateral upsample) · Bart Wronski's
[Volumetric Fog](https://bartwronski.com/wp-content/uploads/2014/08/bwronski_volumetric_fog_siggraph2014.pdf)
(kind 6, Assassin's Creed IV) and Sébastien Hillaire's Frostbite talk in the
[SIGGRAPH 2015 Advances course](https://advances.realtimerendering.com/s2015/index.html)
· HDRP keeps this outside the post folder, under
[`Runtime/Lighting`](https://github.com/Unity-Technologies/Graphics/tree/ce1905322722287c1420a6d85de14158c1648a84/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting)
(`VolumetricLighting`, `AtmosphericScattering`)

---

## 7. Edge detection

**What it does:** Finds where the image changes sharply. The output is a
mask, bright on edges and dark elsewhere. Rarely shown by itself; it is the
first step of outlines, anti-aliasing, sharpening and most painterly
filters.

**How it works:** an edge is a large difference between neighbours, so
every detector is a kernel (4) with positive weights on one side and
negative on the other.

| Operator | What it computes | Notes |
| --- | --- | --- |
| Finite difference | This pixel minus the next, each way | Two reads. Noisy |
| Roberts cross | Two diagonal differences | Four reads. Thin lines |
| Sobel | A 3×3 gradient each way, then `length(vec2(gx, gy))` | The default. Also gives the edge's direction |
| Laplacian | The centre minus the average of its neighbours | Direction-free. Amplifies noise |
| Difference of Gaussians | A narrow blur minus a wide one | Picks out one scale of detail. The base of inked and stippled looks (XDoG) |
| Canny | Blur, Sobel, thin to one pixel, keep connected strong edges | Clean single-pixel lines. Several passes |

Sobel's horizontal kernel is `[[-1, 0, 1], [-2, 0, 2], [-1, 0, 1]]`, and
the vertical one is the same turned a quarter.

**What you run it on matters more than which operator:**

- **Colour or luma** finds edges in textures and shadows too, which is
  right for anti-aliasing and wrong for outlines.
- **Depth** finds silhouettes: where one object stops and something far
  behind begins. It misses a crease between two faces at the same depth and
  fires falsely on surfaces seen at a glancing angle.
- **Normals** find creases and corners. It misses a silhouette between
  parallel surfaces.
- **Depth and normals together** are the standard outline. Object or
  material IDs, when there is a buffer of them, give the cleanest of all.

**Uses:** outlines for toon and comic styles · **anti-aliasing**: FXAA
finds luma edges and blurs along them, SMAA finds them and reconstructs
their shape · **sharpening**: add the difference between the image and its
blur back onto the image · **edge-aware filters**: blurs and upscalers that
refuse to cross an edge · **stylisation**: Kuwahara's oil-paint look,
ASCII and hatching.

**In this game:** only inside FXAA. `fs_fxaa` reads the luma of a pixel and
its neighbours, treats a range above `edgeMinThreshold` as an edge, decides
whether it runs horizontally or vertically from second differences, and
blends along it. There are no outlines. The G-buffer already holds
view-space positions and normals, which is everything a depth-and-normal
outline pass reads.

**Read:** Lettier's [Outlining](https://lettier.github.io/3d-game-shaders-for-beginners/outlining.html)
· Roystan's [Outline Shader](https://roystan.net/articles/outline-shader/),
depth and normals together, with the glancing-angle fix · Alexander Ameye's
[5 ways to draw an outline](https://ameye.dev/notes/rendering-outlines/),
which compares post-process edges with the geometric ways · Wikipedia's
[Sobel operator](https://en.wikipedia.org/wiki/Sobel_operator),
[Canny edge detector](https://en.wikipedia.org/wiki/Canny_edge_detector),
[Difference of Gaussians](https://en.wikipedia.org/wiki/Difference_of_Gaussians)
and [Kuwahara filter](https://en.wikipedia.org/wiki/Kuwahara_filter) ·
Winnemöller's [XDoG paper](https://users.cs.northwestern.edu/~sco590/winnemoeller-cag2012.pdf)
· Simon Rodriguez's
[Implementing FXAA](http://blog.simonrodriguez.fr/articles/2016/07/implementing_fxaa.html)
and [SMAA](https://www.iryoku.com/smaa/) for edges put to work

---

## 8. Overlays and blend modes

**What it does:** Puts a second image on top of the frame: a vignette, film
grain, dirt on the lens, scanlines, blood at the screen's edges, rain on
the glass. The effect is in two choices: what the layer is, and the
**blend mode** that combines it with what is underneath.

**The blend modes**, for a base `a` and a layer `b`:

| Mode | Formula | What it is for |
| --- | --- | --- |
| Normal | `mix(a, b, alpha)` | Covering. UI, decals |
| Add | `a + b` | Light. Glows, flares, sparks. Only brightens |
| Multiply | `a * b` | Shade. Vignettes, dirt, shadows. Only darkens |
| Screen | `1 - (1 - a) * (1 - b)` | A soft add that never clips |
| Overlay | `a < 0.5 ? 2ab : 1 - 2(1 - a)(1 - b)` | Multiply in the shadows, screen in the highlights. Adds contrast and texture at once; the classic way to lay grain or a paper texture over a picture |
| Soft light | A gentler overlay | Subtle texture |
| Colour dodge, burn | `a / (1 - b)`, `1 - (1 - a) / b` | Harsh brightening and darkening |

**The common layers:**

- **Vignette:** darker toward the corners. A multiply by a function of the
  distance from the centre.
- **Film grain:** animated noise, usually overlaid so mid-greys take the
  most.
- **Lens dirt:** a smudge texture that shows only where bright light hits
  it, so it is multiplied into the bloom, not laid over the image.
- **Scanlines, masks, CRT:** the picture multiplied by a line pattern and a
  red, green, blue stripe mask, with the image bent as a glass tube bends
  it.
- **Screen-space splats:** blood, mud, raindrops. The good ones are not
  overlays at all: a drop is a small lens, so the shader reads the scene
  from an **offset coordinate** inside the drop. That is refraction, and
  it is the same trick as heat haze and the underwater wobble.

**Where in the chain:** a layer that behaves like light (lens dirt, flares)
goes before the tone mapper, in linear HDR. A layer that behaves like paint
on the screen (grain, vignette, blood, UI) goes after it, on display
values. Anything laid on before FXAA gets smoothed by it; anything after
stays crisp.

**In this game:** `post.wgsl`'s `applyVignette` (a multiply);
`screenEffects`' coloured edges and fade (normal blends by a mask);
`bloodOver` (G3's hit splatter); `rainLens` and `splatLens`, which return
an offset, a shade, a glint and a blur for each pixel, so the drops bend
the picture behind them; `bloom.wgsl`'s `fs_apply` for lens dirt; and
`crt.wgsl`, a whole pass of this section: curvature, scanlines, an aperture
mask, colour fringing and a vignette over the finished frame.

**Read:** Wikipedia's [Blend modes](https://en.wikipedia.org/wiki/Blend_modes)
for every formula · Lettier's
[Film Grain](https://lettier.github.io/3d-game-shaders-for-beginners/film-grain.html)
and [Chromatic Aberration](https://lettier.github.io/3d-game-shaders-for-beginners/chromatic-aberration.html)
· Acerola's [Post-Processing](https://github.com/GarrettGunnell/Post-Processing)
repository has each blend mode as a shader, and his
[CRT-Shader](https://github.com/GarrettGunnell/CRT-Shader) · the libretro
[CRT shader collection](https://github.com/libretro/glsl-shaders/tree/master/crt)
· John Chapman's
[Pseudo Lens Flare](https://john-chapman.github.io/2017/11/05/pseudo-lens-flare.html)
· HDRP's `UberPost.compute` (lens distortion, chromatic aberration, bloom
and vignette in one pass) and `FinalPass.shader` (grain and dither)

---

## 9. Upscaling

**What it does:** Renders the scene with fewer pixels than the screen has
and fills in the rest. Shading cost follows pixel count, so drawing at 70%
of the width and height is about half the work. The whole subject is how to
hide that.

**Spatial upscalers** use one frame:

| Method | How | Result |
| --- | --- | --- |
| Nearest | Each output pixel copies the closest source pixel | Hard blocks. Right for pixel art at whole multiples; at 2.5× some source pixels come out wider than others and shimmer in motion |
| Bilinear | Blend the four closest | Smooth and blurry |
| Sharp bilinear | Nearest inside each source pixel, a blend only across the one-pixel boundary | Crisp pixel art at any scale without shimmer |
| Bicubic, Lanczos | A wider, sharper kernel of 16 or more texels | Sharper than bilinear, with slight ringing |
| Edge-adaptive (FSR 1) | Find the local edge direction (7), stretch the kernel along it, then sharpen | Clean edges from one frame. No new detail |
| Pixel-art scalers (EPX, hqx, xBR) | Pattern rules that guess curves from pixel neighbourhoods | Smooth outlines from sprites |

**Temporal upscalers** use many frames. Each frame the camera is shifted by
a fraction of a pixel, in a repeating pattern, so over several frames every
part of every pixel gets sampled. The pass keeps a full-resolution
**history**, moves it to where things are now using **motion vectors** (a
buffer of how far each pixel moved), and blends about a tenth of the new
frame in. When the history is wrong, because something was uncovered or
changed colour, it must be rejected, usually by clamping it to the range of
colours near the pixel in the new frame; getting that wrong is ghosting.
Temporal anti-aliasing is this at equal resolution. TAAU, FSR 2 and later,
DLSS and XeSS are this to a higher resolution, the last two with a neural
network doing the blending and rejection.

**Sharpening** usually follows, since both kinds soften the image. Contrast
adaptive sharpening (CAS) sharpens less where contrast is already high, so
edges do not ring.

**Dynamic resolution** changes the render size from frame to frame to hold
a frame time, with the upscaler hiding the change.

**In this game:** the world renders at the surface's size, so there is no
upscaler. Three things are related:

- The PSX look draws the picture on a coarse grid _inside_ `fs_toneMap`:
  `psxCellUv` moves each pixel's read to the centre of its grid cell, which
  is a nearest upscale without a smaller texture. It still shades every
  screen pixel, so it is a look and not a saving.
- Block textures magnify with nearest filtering, which is the first row of
  the table applied to textures.
- FXAA is spatial anti-aliasing. For anything temporal the G-buffer has no
  motion vectors; the last frame's colour and positions are kept, for
  reflections (the 3D outline's 17b).

**Read:** Catlike Coding's
[Render Scale](https://catlikecoding.com/unity/tutorials/custom-srp/render-scale/)
(rendering small and scaling up, bicubic included) · AMD's
[FSR 1 source](https://github.com/GPUOpen-Effects/FidelityFX-FSR),
[FSR 2 source](https://github.com/GPUOpen-Effects/FidelityFX-FSR2), whose
documentation explains temporal upscaling stage by stage, and
[CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) · Emilio López's
[Temporal AA and the quest for the Holy Trail](https://www.elopezr.com/temporal-aa-and-the-quest-for-the-holy-trail/),
the clearest account of temporal anti-aliasing · Brian Karis's
[High Quality Temporal Supersampling](https://de45xmedrsdbp.cloudfront.net/Resources/files/TemporalAA_small-59732822.pdf)
· Playdead's [temporal reprojection source](https://github.com/playdeadgames/temporal)
· Cole Cecil's
[Scaling pixel art without destroying it](https://colececil.dev/blog/2017/scaling-pixel-art-without-destroying-it/)
and csantosbh's
[Manual texture filtering for pixelated games](https://csantosbh.wordpress.com/2014/01/25/manual-texture-filtering-for-pixelated-games-in-webgl/)
(sharp bilinear) · GPU Gems 2's
[Fast Third-Order Texture Filtering](https://developer.nvidia.com/gpugems/gpugems2/part-iii-high-quality-rendering/chapter-20-fast-third-order-texture-filtering)
(bicubic in four reads) · Wikipedia's
[Pixel-art scaling algorithms](https://en.wikipedia.org/wiki/Pixel-art_scaling_algorithms)
and [Lanczos resampling](https://en.wikipedia.org/wiki/Lanczos_resampling)
· HDRP's `EdgeAdaptiveSpatialUpsampling.compute`,
`ContrastAdaptiveSharpen.compute`, `RTUpscale.hlsl` and
`TemporalAntiAliasing.shader`

---

## 10. Compression as an effect

**What it does:** Makes the picture look as if it had been saved as a bad
JPEG, streamed at a low bit rate or copied from tape. The artefacts of
lossy compression have become a look of their own.

**How JPEG damages an image**, since the effect is that process run on
purpose:

1. **Convert to luma and chroma** (YCbCr): one brightness channel, two
   colour channels.
2. **Subsample the chroma.** Keep colour at half resolution each way
   (4:2:0). The eye barely notices, and colours bleed across sharp edges.
3. **Cut the image into 8×8 blocks** and take each block's **discrete
   cosine transform**: 64 numbers saying how much of each stripe and
   checker pattern the block contains, from flat to finest.
4. **Quantise those numbers** (3), coarsely for the fine patterns. This is
   the only step that throws information away, and the quality setting is
   how coarse.
5. Transform back.

What survives is the look: **blocking** (each 8×8 block quantised on its
own, so their borders show), **ringing** (ripples beside sharp edges, where
the fine patterns that cancelled them were dropped), and **colour bleed**.

**As a shader:** steps 1 to 5 in a post pass. Each pixel finds its block,
and the transform is a sum over the block's 64 texels per coefficient, so a
direct version is expensive; it is done in two passes (forward, then
quantise and inverse), or in compute. A cheap imitation skips the
transform: quantise colour per block, keep chroma at a quarter of the
resolution, and add a little noise at block borders.

**Relatives:**

- **Datamoshing** imitates broken video. Video stores most frames as "move
  these blocks of the previous frame by these vectors", so a lost keyframe
  smears the old picture along the new motion. In a game: keep last
  frame's image and move it by the motion vectors without refreshing it.
- **VHS and analogue:** chroma at very low horizontal resolution and
  shifted sideways, luma noise, a tracking band rolling up the screen.
- **Pixel sorting**, a glitch-art cousin: sort runs of pixels by brightness.

**"Compression" also names three unrelated things**, worth knowing so the
word does not confuse:

- **Dynamic range compression** is tone mapping (2).
- **G-buffer packing** is storing normals, roughness and the like in as few
  bits as will do. The 3D outline's 11a is this game's byte budget.
- **Texture compression** (BC1–7, ASTC, ETC) is a file and memory format
  the GPU decodes as it samples. It is a loading-time matter, not a pass.

**In this game:** nothing of this kind as an effect. The nearest relatives
are `psxColour`'s bit-depth cut, and the `rg11b10ufloat` target between the
tone mapper and FXAA, whose five and six bit mantissas quantise the image
by a level or two (the 3D outline's 15).

**Read:** Wikipedia's [JPEG](https://en.wikipedia.org/wiki/JPEG), whose
worked example follows one block through every step,
[Discrete cosine transform](https://en.wikipedia.org/wiki/Discrete_cosine_transform)
and [Chroma subsampling](https://en.wikipedia.org/wiki/Chroma_subsampling)
· Acerola's [Pixel-Sorting](https://github.com/GarrettGunnell/Pixel-Sorting)
and his [post-processing playlist](https://www.youtube.com/playlist?list=PLUKV95Q13e_Un6ADYZ9NyWJ3W1R2cbCYv)

---

## 11. Particles and iterated function systems

**What particles do:** Draw things that have no surface: sparks, smoke,
dust, rain, debris, magic. Thousands of small pieces, each simple, which
read together as one effect.

**How a particle system works:**

1. **Emit:** make new particles with a position, a velocity, a lifetime and
   some randomness.
2. **Simulate:** each frame, move every particle by its velocity, change
   the velocity by gravity, drag, wind or noise, and age it.
3. **Render:** draw each one, usually as a quad turned toward the camera,
   with size, colour and opacity taken from curves over its age.
4. **Die:** recycle the slot.

**Where the simulation runs:**

- **On the CPU**, a loop over an array each frame. Fine for thousands, and
  particles can touch gameplay (collide with blocks, play a sound).
- **On the GPU**, a compute shader over a storage buffer, drawn with
  instancing straight from that buffer. Millions, and the data never
  crosses back to the CPU.
- **Nowhere.** A **stateless** particle's position is a formula of its seed
  and the time, evaluated in the vertex shader. No buffer to update. Good
  for anything that loops and does not react: rain, snow, embers, stars.

**What makes them look right:**

- **Blending.** Additive for anything that glows, with no sorting needed.
  Alpha blending for smoke and dust, which must be drawn back to front.
- **Soft particles.** Fade a particle where it nears the scene's depth, so
  a smoke quad does not show a hard line where it cuts the ground.
- **Flipbooks.** A sheet of animation frames stepped through over the
  lifetime.
- **Overdraw.** Many big see-through quads fill the same pixels over and
  over. Drawing particles into a quarter-size target and upsampling (9) is
  the standard cure.

**What an iterated function system is:** a handful of simple
transformations (each a scale, a turn and a shift) applied over and over.
Take a point, pick one of the transformations at random, apply it, plot the
result, and repeat a few million times. The points do not wander
everywhere; they settle onto one shape, the system's **attractor**, and
that shape is a fractal: it is made of smaller copies of itself, one for
each transformation. Four transformations give the Barnsley fern, three
give the Sierpinski triangle. This way of drawing one is the **chaos
game**.

**Why it sits next to particles:** an IFS _is_ a particle system whose
physics is "apply a random one of N fixed transformations". On the GPU:
keep a buffer of points, have a compute shader apply a randomly chosen
transformation to each every frame, and draw them as additive points. A
million particles fill in the whole fractal in a few frames, and animating
the transformations' numbers makes it flow.

**Fractal flames** are the elaborate version: non-linear transformations
("variations"), each point coloured by which transformations it went
through, and the image tone mapped by the **logarithm** of how many points
landed on each pixel, since densities span many orders of magnitude.

**In this game:**

- `precipitation.wgsl` is stateless. One sheet per block column around the
  camera, and the drops are drawn _on_ the sheet by a hash of the column
  and the time, not modelled one by one.
- `debris.cpp` (G4) is CPU-simulated: small cubes that fall, bounce, spin
  and shrink, drawn with the dropped-item cube renderer.
- The drops and blood on the screen (G3) are procedural overlays (8).
- There are no GPU-simulated particles and no IFS. The compute passes the
  game does have build mips (the block textures', the 3D outline's 7d),
  which is the same machinery a particle update would use.

**Read:** LearnOpenGL's [Particles](https://learnopengl.com/In-Practice/2D-Game/Particles)
(the CPU loop) · Catlike Coding's
[Particles](https://catlikecoding.com/unity/tutorials/custom-srp/particles/)
(flipbooks, soft particles, distortion) · the WebGPU samples'
[particles](https://webgpu.github.io/webgpu-samples/?sample=particles) and
[compute boids](https://webgpu.github.io/webgpu-samples/?sample=computeBoids),
GPU simulation in this API · WebGPU Fundamentals'
[Compute Shaders](https://webgpufundamentals.org/webgpu/lessons/webgpu-compute-shaders.html)
· GPU Gems 3's
[High-Speed, Off-Screen Particles](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles)
· Wikipedia's [Iterated function system](https://en.wikipedia.org/wiki/Iterated_function_system),
[Chaos game](https://en.wikipedia.org/wiki/Chaos_game),
[Barnsley fern](https://en.wikipedia.org/wiki/Barnsley_fern) (the four
transformations, with their numbers) and
[Fractal flame](https://en.wikipedia.org/wiki/Fractal_flame) · Scott
Draves's [The Fractal Flame Algorithm](https://flam3.com/flame_draves.pdf)
· Acerola's
[Iterated-Function-Systems](https://github.com/GarrettGunnell/Iterated-Function-Systems)
("fractal particles")

---

## 12. Avoiding visible tiling

**What the problem is:** One texture repeated across a large surface. The
eye is very good at finding a repeating feature, and from a distance the
repeats line up into a grid. More texture resolution does not help; the
period is the problem.

**The ways out**, roughly cheapest first:

0. **Make the period longer.** One picture drawn across several tiles, each
   tile showing its slice. The repeat is still there, several times
   farther apart.
1. **Vary each tile.** Hash the tile's integer coordinate and use it to
   rotate, mirror or offset the texture in that tile. Free, but the
   texture's edges must match under those changes, or seams show.
2. **Use variants.** Several versions of the texture, chosen per tile by
   the hash. Minecraft's block models do this, with rotations, for grass,
   dirt, sand and stone.
3. **Break it up at a larger scale.** Multiply by a low-frequency noise or
   a second, much larger texture ("macro variation"), so no two repeats
   have the same brightness or tint. Biome tints and height-based colour
   are this.
4. **Blend two scales.** The same texture sampled small and large, mixed by
   distance. Detail up close, no pattern far away.
5. **Blend between offset copies.** Sample the texture at two or three
   different offsets and blend between them by a noise value that changes
   slowly across the surface. No seams, but blending blurs contrast.
6. **Stochastic and hex tiling.** Cut the plane into triangles or hexagons,
   give each a random offset into the texture, and blend at the borders
   with an operator that preserves contrast. Works on any texture with no
   repeat visible at all, for three reads.
7. **Wang tiles.** A small set of tiles whose edges are colour-coded so
   they can be laid out without ever repeating a large pattern.
8. **Cover it.** Decals, edge wear, overlays where two materials meet.

Two traps. **Mip seams:** a random offset per tile makes the texture
coordinate jump at tile borders, the GPU reads that jump as "far away" and
picks a tiny mip, and a line appears; the fix is to take the derivatives
from the unbroken coordinate and pass them in (`textureSampleGrad`, the 3D
outline's "Uniformity decides which sample function"). **Triplanar
mapping** is often named here but solves a different problem: stretching on
surfaces with no good coordinates. A triplanar surface still tiles.

**In this game:** way 0, and less need than in most games: a block face is
one small picture, repetition at block scale is the style, and the world's
shapes break it up. A **spanned texture** covers several blocks each way
(`BlocksLoader::spannedTextures`); `chunkFaces.wgsl`'s `spannedLayer` picks
which slice a face shows from the block's integer position, in the vertex
stage. The same position hashed could pick a rotation or a variant layer
with no new data per face (ways 1 and 2); nothing does yet. The water's
maps tile by world position (the 3D outline's 18-parity).

**Read:** Inigo Quilez's
[Texture repetition](https://iquilezles.org/articles/texturerepetition/):
ways 1, 5 and a variant of 6 in three short shaders, mip fix included ·
Unity's [Procedural Stochastic Texturing](https://unity.com/blog/engine-platform/procedural-stochastic-texturing-in-unity)
and the research behind it, Deliot and Heitz's
[Tiling and Blending](https://eheitzresearch.wordpress.com/738-2/) and
Heitz and Neyret's
[Histogram-Preserving Blending](https://eheitzresearch.wordpress.com/722-2/)
· Morten Mikkelsen's
[Practical Real-Time Hex-Tiling](https://jcgt.org/published/0011/03/05/)
· Wikipedia's [Wang tile](https://en.wikipedia.org/wiki/Wang_tile) ·
Catlike Coding's
[Triplanar Mapping](https://catlikecoding.com/unity/tutorials/advanced-rendering/triplanar-mapping/)
· The Book of Shaders on [random](https://thebookofshaders.com/10/) and
[noise](https://thebookofshaders.com/11/), for the hash everything above
starts from

---

## 13. Reflections

**What it does:** Shows the surroundings in a surface: sky in water, a room
in a polished floor, a highlight sliding over metal. Much of what makes a
material look wet, metallic or smooth is its reflection.

**Two halves.** _What is reflected_: find the colour arriving along the
mirror direction. _How much shows_: **Fresnel** says every surface reflects
more at a glancing angle (water is a window looking down and a mirror
toward the horizon), and **roughness** says how blurred the reflection is.

**Ways to find the reflected colour:**

| Method | How | Good | Bad |
| --- | --- | --- | --- |
| Environment map | Look the direction up in a cubemap of the sky or of the scene captured from one point | Cheap, never fails | Wrong for anything near, unless corrected for the room's shape |
| Planar | Draw the scene a second time, mirrored in the surface | Exact | Only flat surfaces, and a whole extra scene draw for each |
| Screen space (SSR) | March the reflected ray through the depth buffer until it passes behind something, and take the screen's colour there | Reflects the real, moving scene for the cost of a post pass | Only what is on screen. Fades at the screen's edges and has holes behind objects |
| Ray traced | Trace the ray against the real scene | Correct | Needs hardware and a denoiser |

Real games stack them: SSR where it hits, a local cubemap where it misses,
the sky behind that. Rough surfaces read a blurrier mip of the environment
or blur the SSR result.

**In this game:** SSR inside the lit shader, for faces smoother than 0.45
and for water: `chunkFaces.wgsl`'s `ssrRayMarch`, `ssrBinarySearch` and
`reflectSurface`, reading _last frame's_ positions and colour (the 3D
outline's 17b has the march and the history). Other faces mix the
reflection in by Fresnel and roughness; water takes it whole and then mixes
with what is under the surface by a Fresnel term. The environment maps a
fallback would read are not built (the 3D outline's 12 and 14 say why), so
a missed ray keeps the surface's own colour.

**Read:** David Lettier's
[Screen Space Reflection](https://lettier.github.io/3d-game-shaders-for-beginners/screen-space-reflection.html)
· McGuire and Mara's
[Efficient GPU Screen-Space Ray Tracing](https://jcgt.org/published/0003/04/04/),
the march done properly in pixels · Sébastien Lagarde's
[parallax-corrected cubemaps](https://seblagarde.wordpress.com/2012/09/29/image-based-lighting-approaches-and-parallax-corrected-cubemap/)
· LearnOpenGL's [Specular IBL](https://learnopengl.com/PBR/IBL/Specular-IBL)
for roughness and environment maps · Catlike Coding's
[Looking Through Water](https://catlikecoding.com/unity/tutorials/flow/looking-through-water/)
and GPU Gems'
[Effective Water Simulation from Physical Models](https://developer.nvidia.com/gpugems/gpugems/part-i-natural-effects/chapter-1-effective-water-simulation-physical-models)
· Acerola's [Water](https://github.com/GarrettGunnell/Water)

---

## 14. Shadows

**What it does:** Darkens what the light cannot reach. Shadows are what fix
objects to the ground and say where the light is; a scene without them
looks as if everything floats.

**How a shadow map works:** draw the scene's depth from the light's point
of view into a texture. Then, shading a pixel from the camera, work out
where that point falls in the light's view and compare its distance from
the light with the stored one. Farther than what the light saw means
something is in the way.

**The problems, which are the whole subject:**

- **Acne.** A surface shadows itself in stripes, because the map's texels
  are coarser than the screen's pixels. Fixed with a **bias**, a small
  offset along the light's direction or the surface's normal.
- **Peter-panning.** Too much bias, and the shadow comes loose from the
  object's feet.
- **Resolution.** One map over a whole view is far too coarse near the
  camera. **Cascades** use several maps, small and sharp nearby, large and
  coarse far away.
- **Swimming.** Shadow edges crawl as the camera moves, unless the map
  moves in whole texel steps.
- **Hard, stair-stepped edges.** Averaging the _colour_ of the map does
  nothing useful; the yes-or-no _answers_ have to be averaged.
  **Percentage-closer filtering** takes several comparisons around the
  point and averages them; turning the pattern per pixel (3) hides its
  shape.

**Kinds beyond the basic map:**

| Kind | What it adds |
| --- | --- |
| PCSS | Edges that widen with distance from the caster, as real shadows do. Searches the map for blockers first |
| Variance and exponential maps (VSM, ESM) | Maps that _can_ be blurred like images. Soft for cheap, with light leaking where shadows overlap |
| Cube shadow maps | Six maps around a point light |
| Contact (screen-space) shadows | A short march through the depth buffer toward the light, for the fine detail a map misses |
| Baked light | Light worked out ahead of time and stored. Free at run time, cannot move |
| Ambient occlusion | Not a shadow from any light: a darkening of creases and corners, where less of the sky is visible. SSAO, HBAO and GTAO estimate it from the depth buffer |
| Ray traced | The comparison done against the real geometry |

**In this game:** three layers.

- **Baked light.** Every face carries a sky light and a block light level
  (packed as `sky << 4 | block`), flood-filled through the blocks as
  Minecraft does. This is what makes caves dark and torches work.
- **Sun shadow maps.** Three cascades with a comparison sampler,
  `lighting.wgsl`'s `sunShadow`; soft shadows are nine taps on a disc
  turned by interleaved gradient noise. The 3D outline's 13.
- **Ambient occlusion**, `hbao.wgsl`. The 3D outline's 17a.

**Read:** LearnOpenGL's
[Shadow Mapping](https://learnopengl.com/Advanced-Lighting/Shadows/Shadow-Mapping),
[Point Shadows](https://learnopengl.com/Advanced-Lighting/Shadows/Point-Shadows)
and [Cascaded Shadow Mapping](https://learnopengl.com/Guest-Articles/2021/CSM)
· Microsoft's
[Common Techniques to Improve Shadow Depth Maps](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/common-techniques-to-improve-shadow-depth-maps)
and [Cascaded Shadow Maps](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/cascaded-shadow-maps),
still the best list of every artefact and its fix · Matt Pettineo's
[A Sampling of Shadow Techniques](https://therealmjp.github.io/posts/shadow-maps/),
the filtering kinds compared · NVIDIA's
[Percentage-Closer Soft Shadows](https://developer.download.nvidia.com/shaderlibrary/docs/shadow_PCSS.pdf)
· Bend Studio's
[Screen Space Shadows](https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/)
· LearnOpenGL's [SSAO](https://learnopengl.com/Advanced-Lighting/SSAO) and
Jimenez's
[Practical Real-Time Strategies for Accurate Indirect Occlusion](https://www.activision.com/cdn/research/Practical_Real_Time_Strategies_for_Accurate_Indirect_Occlusion_NEW%20VERSION_COLOR.pdf)
(GTAO)

---

## 15. Other effects worth knowing

Shorter, because each is a variation on a block above.

| Effect | What it does | Built from | In this game | Read |
| --- | --- | --- | --- | --- |
| Anti-aliasing | Smooths stair-stepped edges. MSAA takes extra samples at triangle edges; FXAA and SMAA find edges in the image; TAA averages frames | Edge detection (7), temporal history (9) | FXAA, `fs_fxaa` | Catlike Coding's [FXAA](https://catlikecoding.com/unity/tutorials/advanced-rendering/fxaa/) |
| Sharpening | Makes detail crisper: add back the image minus its blur | Blur (4) | No | Lettier's [Sharpen](https://lettier.github.io/3d-game-shaders-for-beginners/sharpen.html), [CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) |
| Depth of field | Blurs what is nearer or farther than the focus distance | A blur radius per pixel from depth, then a disc blur (4) | No | Catlike Coding's [Depth of Field](https://catlikecoding.com/unity/tutorials/advanced-rendering/depth-of-field/), GPU Gems' [survey](https://developer.nvidia.com/gpugems/gpugems/part-iv-image-processing/chapter-23-depth-field-survey-techniques) |
| Motion blur | Smears what moves fast | Motion vectors, a directional blur (4) | No | GPU Gems 3's [chapter 27](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-27-motion-blur-post-processing-effect), Lettier's [Motion Blur](https://lettier.github.io/3d-game-shaders-for-beginners/motion-blur.html) |
| Chromatic aberration | Colour fringes toward the screen's edges | The red, green and blue channels read at slightly different coordinates | In `crt.wgsl` (fringing) and the water's caustics | Lettier's [Chromatic Aberration](https://lettier.github.io/3d-game-shaders-for-beginners/chromatic-aberration.html) |
| Refraction and distortion | Bends what is behind glass, water, heat or a raindrop | Reading the scene at an offset coordinate | The underwater warp, water's dudv, the drops on the lens | Catlike Coding's [Looking Through Water](https://catlikecoding.com/unity/tutorials/flow/looking-through-water/) |
| Lens flare | Ghosts and streaks from a bright light | Sprites along the line through the screen centre, or a flipped and blurred bright pass | The sun's flare (the 3D outline's 18b) | John Chapman's [Pseudo Lens Flare](https://john-chapman.github.io/2017/11/05/pseudo-lens-flare.html) |
| Auto exposure | Adapts brightness over time, as an eye does | The image's average or histogram, read back | Yes (the 3D outline's 16a) | Catlike Coding's [HDR](https://catlikecoding.com/unity/tutorials/custom-srp/hdr/) |
| Ambient occlusion | Darkens creases and contact points | Depth and normals sampled around each pixel | `hbao.wgsl` | LearnOpenGL's [SSAO](https://learnopengl.com/Advanced-Lighting/SSAO) |
| Pixelation | Big square pixels | Snapping the read coordinate to a grid | The PSX look's `psxCellUv` | Lettier's [Pixelization](https://lettier.github.io/3d-game-shaders-for-beginners/pixelization.html) |
| Toon shading | Flat bands of light, inked outlines | Quantised lighting (3), edge detection (7) | No | Roystan's [Outline Shader](https://roystan.net/articles/outline-shader/) |
| Noise | Structured randomness: clouds, terrain, wobble, variation | Hashes blended across a grid (value, gradient, simplex, cellular) | Throughout: rain, drops, dither, the SSR jitter | The Book of Shaders [10](https://thebookofshaders.com/10/) and [11](https://thebookofshaders.com/11/), Jarzynski and Olano's [Hash Functions for GPU Rendering](https://jcgt.org/published/0009/03/02/) |
| Signed distance fields | Shapes as "how far to the surface" functions: crisp text, soft shadows, whole ray-marched scenes | A distance function, marched or thresholded | No | Inigo Quilez's [distance functions](https://iquilezles.org/articles/distfunctions/) |
| Fur and grass shells | Volume from stacked see-through layers | The same mesh drawn many times, pushed out along its normal | No | Acerola's [Shell-Texturing](https://github.com/GarrettGunnell/Shell-Texturing) |

**The order of a post chain** is itself a topic, because most effects only
make sense on one side of another: depth of field and motion blur before
bloom (so blurred highlights bloom), bloom before tone mapping (it is
light), grading around the tone mapper (2), grain and dither last. Unity's
[post-processing execution order](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@14.0/manual/Post-Processing-Execution-Order.html)
is one engine's answer as a diagram; section 1 has this game's.

---

## 16. Worked example: the held torch's light

One effect built from the blocks above, with what was considered at each
choice. A torch in the hand lights the ground around the player in a few
bands that dim outward, the bands' edges dithered into each other, at night,
in rain and in snow. The code is `heldLight.wgsl` (the bands),
`world.wgsl`'s `heldLightLevel` (where the threshold comes from),
`lighting.wgsl`'s `withHeldLight` (how it joins the rest of the light) and
`precipitation.wgsl`'s `heldLightOnDrop`.

**What kind of light (14).** Three candidates:

- A physically based point light, as the light blocks are: a smooth
  falloff, brighter on surfaces that face it. Rejected: it has no bands to
  begin with, and a light at the viewer makes every visible surface face
  it, so the facing term adds nothing.
- A shadow-mapped light. Not needed: the light is where the eye is, so
  whatever the eye can see, the light can reach. A light at the viewer
  never needs a shadow map.
- **A block light that walks with the player.** Chosen. The game's baked
  light is already a level from 0 to 15 that falls with distance, and a
  level is a band. So the held light is one more block light level, by
  distance alone, and the face takes the brighter of it and its baked one.
  That one `max` gives the rest for free: daylight drowns the torch as it
  drowns a torch block, the warm tint and the per-face shading come from
  the existing ambient code, and nothing new is added to the lit colour.

**Quantisation (3).** The level is the continuous falloff quantised: twelve
blocks' reach in five bands, so levels 14, 11, 8, 6 and 3. Fewer, wider
steps than a torch block's one level a block, because a step has to be a
few blocks wide to read as a band from eye height.

**Dithering (3).** Each choice in the table of kinds was a decision:

- **Which kind.** Ordered, a 4×4 Bayer matrix. White noise looks like
  dirt, error diffusion cannot run in a fragment shader, and blue noise
  needs a texture; a Bayer pattern is also the one that looks like pixel
  art, which suits blocks.
- **Where the pattern lives.** On the world, not the screen. The threshold
  is looked up by the world's texel (16 to a block), so the pattern is
  painted on the ground and stays put when the camera turns. A screen
  pattern would slide over the world like dirt on a window, which is the
  motion problem section 3 warns about.
- **What is dithered.** Only a band's edge. Dithering the whole falloff
  would turn the bands into one smooth, grainy gradient; here 45% of each
  band's width blends into the next and the rest stays flat.
- **Per texel, not per pixel.** The distance is measured to the texel's
  centre, so a band's edge runs along texels as the block's picture does,
  and a texel is never cut in two by a ring.

One trap worth knowing: a block face lies exactly on a texel boundary, so
flooring its position flickers between two texels. The lookup steps half a
texel in from the surface first.

**Fog (6).** Checked against the numbers before writing anything. The near
haze, rain included, removes well under a thousandth of the light inside
twelve blocks, so it needed nothing. The snow's whiteout does matter: by
twelve blocks it has replaced about 40% of the lit colour with the fog's,
which at night is dark, so it swallowed the outer bands. The fix is to
light the fog as well (in-scattering): `world.wgsl`'s `applySnowFog` takes
the fog's colour, inside the torch's reach, as what snow looks like at that
band's light level, when that is brighter than the night's. The fog over a
band is then as bright as the band, and the bands hold through the
whiteout. Beyond the reach the fog is untouched, so the torch puts no veil
over the distance. Ray-marched volumetrics were considered and set aside:
the light is at the eye, so the fog it lights depends only on distance,
which a formula gives without a march.

**Particles (11).** The rain and snow are stateless, drawn on sheets, so
there is no particle to hand a light to; the fragment works out its own
distance and takes the same band. The dither threshold there is the lane's
own random number, so a whole streak falls on one side of an edge instead
of being cut by a pattern.

**Colour (2).** The warm tint is the one the ambient code already gives
block light, applied in linear light before the tone mapper, so auto
exposure and grading treat it as any other light.

Checked in scripted screenshots: clear night with and without the torch,
clear day (no change, as intended), rain at night, snow at night with the
fog lit and unlit and with the PSX look on, and down a pit by day.

---

## Exercises, smallest first

Not a plan (the plans are [`roadmap.md`](roadmap.md) and
[`gameplay-roadmap.md`](gameplay-roadmap.md)); these are ways to learn each
block by changing a few lines of a shader that already runs. Shaders hot
reload (the 3D outline's 20), so each is edit, save, look.

1. **See banding, then cure it.** In `fs_toneMap`, quantise to 16 levels
   with no dither; then with `gradientNoise`; then with a 4×4 Bayer table.
   Three looks from one line (3).
2. **Swap the dither's kind** in `psxColour`: the PlayStation's table for
   white noise, then for interleaved gradient noise, and watch what the
   pattern does when the camera moves (3).
3. **A palette.** Replace each colour with the nearest of eight fixed ones,
   before and after dithering (3).
4. **A Sobel outline** as a few lines at the top of `fs_toneMap`, first on
   luma, then on the G-buffer's positions (7).
5. **Blend modes.** Lay a noise texture over the frame with multiply,
   screen and overlay in turn (8).
6. **Vary a block face** by a hash of its block position: mirror the
   texture coordinate on half the blocks, and see where seams appear (12).

---

## Resource shelf

### Start here: explainers

- [The Book of Shaders](https://thebookofshaders.com/). Fragment shaders
  from nothing: shaping functions, colour, shapes, random, noise. Edits
  live in the page.
- [3D Game Shaders For Beginners](https://lettier.github.io/3d-game-shaders-for-beginners/)
  by David Lettier. One page per effect, each with the idea, the code and
  pictures: outlining, fog, blur, bloom, SSAO, motion blur, chromatic
  aberration, reflection, refraction, posterisation, pixelisation, sharpen,
  film grain, lookup tables, gamma, depth of field. The closest thing to
  this file's contents list with full lessons behind it.
- [LearnOpenGL](https://learnopengl.com/). The techniques as most engines,
  and this game's GL ancestor, first learned them.
- [Catlike Coding](https://catlikecoding.com/unity/tutorials/). Unity
  tutorials, but each builds the effect from an empty file and explains
  every line. The Custom SRP series
  ([Post Processing](https://catlikecoding.com/unity/tutorials/custom-srp/post-processing/),
  HDR, Color Grading, Render Scale, Particles) is a renderer's post stack
  built in order.
- [Inigo Quilez's articles](https://iquilezles.org/articles/). The maths:
  fog, noise, distance fields, texture repetition, palettes.
- Acerola's [post-processing playlist](https://www.youtube.com/playlist?list=PLUKV95Q13e_Un6ADYZ9NyWJ3W1R2cbCYv)
  and [channel](https://www.youtube.com/@Acerola_t). Videos that each take
  one effect from the idea to a working shader; most of this file's
  sections have one. The code is in
  [Post-Processing](https://github.com/GarrettGunnell/Post-Processing)
  (fog, bloom, edge detection, colour correction, tone mappers, sharpness,
  blend modes, dithering, palette swapping, Kuwahara, difference of
  Gaussians, the pixel-art upscaler, blur),
  [AcerolaFX](https://github.com/GarrettGunnell/AcerolaFX), and the
  repositories named in sections 3, 8, 10, 11, 13 and 15.
- [Shadertoy](https://www.shadertoy.com/). Thousands of effects with their
  source, to read and to break.

### Frame breakdowns

How shipped games put the pieces together. Read one beside section 1's list
of this game's passes and match them up.

- [Behind the Pretty Frames](https://mamoniem.com/category/behind-the-pretty-frames/)
  by Muhammad A.Moniem. Long captures of whole frames, pass by pass, with
  the render targets shown. Seven so far:
  [God of War](https://mamoniem.com/behind-the-pretty-frames-god-of-war/),
  [Elden Ring](https://mamoniem.com/behind-the-pretty-frames-elden-ring/),
  [Resident Evil](https://mamoniem.com/behind-the-pretty-frames-resident-evil/),
  [Death Stranding](https://mamoniem.com/behind-the-pretty-frames-death-stranding/),
  [Diablo IV](https://mamoniem.com/behind-the-pretty-frames-diablo-iv/),
  [Detroit: Become Human](https://mamoniem.com/behind-the-pretty-frames-detroit-become-human/)
  and [PRAGMATA](https://mamoniem.com/behind-the-pretty-frames-pragmata/).
  The Elden Ring one is a good first read: its frame is cascaded shadows, a
  G-buffer with dithering, decals, SSAO, then a post chain of depth of
  field, colour grading, anti-aliasing, bloom, light shafts, tone mapping
  and a LUT, which is sections 14, 3, 15, 5, 6 and 2 of this file in one
  game.
- Adrian Courrèges's graphics studies:
  [GTA V](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study/),
  [DOOM (2016)](https://www.adriancourreges.com/blog/2016/09/09/doom-2016-graphics-study/),
  and his [compilation](https://www.adriancourreges.com/blog/2020/12/29/graphics-studies-compilation/)
  of everyone else's. Shorter and more illustrated than the above.
- Emilio López's
  [The Rendering of Rise of the Tomb Raider](https://www.elopezr.com/the-rendering-of-rise-of-the-tomb-raider/).
- Steven Wittens's
  [Teardown Frame Teardown](https://acko.net/blog/teardown-frame-teardown/).
  A voxel game, and so the nearest relative of this one.
- Simon Schreibt's [Render Hell](https://simonschreibt.de/gat/renderhell/),
  for what a draw call costs and why passes are ordered as they are.
- [RenderDoc](https://renderdoc.org/), the tool most of these were made
  with. This game's own version is the pass dump in `wgpuWorld.cpp`
  (`OURCRAFT_PASS_DUMP=N` writes frame N's targets as images, step by
  step).

### Production source: Unity's HDRP

The [post-processing shader folder](https://github.com/Unity-Technologies/Graphics/tree/ce1905322722287c1420a6d85de14158c1648a84/Packages/com.unity.render-pipelines.high-definition/Runtime/PostProcessing/Shaders),
by file. Most are compute shaders; `.hlsl` files are shared code. Read the
matching explainer first, then the file to see what a shipping version adds.

| Files | What they are | Section |
| --- | --- | --- |
| `Exposure.compute`, `HistogramExposure.compute`, `ApplyExposure.compute` | Auto exposure, by average or by histogram | 2, 15 |
| `LutBuilder3D.compute` | Every grading control and the tone mapper baked into a 3D LUT each frame | 2 |
| `UberPost.compute` | One pass applying lens distortion, chromatic aberration, bloom, vignette and the LUT | 2, 8 |
| `FinalPass.shader` | The last pass: film grain, the 8-bit dither, optional FXAA, the final upscale filter | 3, 8, 9 |
| `BloomPrefilter.compute`, `BloomBlur.compute`, `BloomUpsample.compute`, `BloomCommon.hlsl` | Bloom's three steps | 5 |
| `DepthOfField*.compute`, `DoF*.compute` | Two depth of field implementations: circle of confusion, tiles, gather, combine | 15 |
| `MotionBlur*.compute` | Motion blur: velocity preparation, tile maxima, the blur | 15 |
| `FXAA.compute`, `SubpixelMorphologicalAntialiasing.*`, `TemporalAntiAliasing.shader`, `TemporalAntialiasing.hlsl` | The three anti-aliasing families | 7, 9, 15 |
| `EdgeAdaptiveSpatialUpsampling.compute`, `ContrastAdaptiveSharpen.compute`, `PostSharpenPass.compute`, `RTUpscale.hlsl`, `DLSSBiasColorMask.shader` | Upscaling and sharpening | 9 |
| `LensFlareDataDriven.shader`, `LensFlareScreenSpace.shader`, `LensFlareMergeOcclusionDataDriven.compute` | Lens flares, placed by data or found in the image | 15 |
| `PaniniProjection.compute` | Reprojects a wide field of view so the edges stretch less | — |
| `NaNKiller.compute` | Replaces broken pixels (not-a-number, infinity) before a blur spreads them across the screen | — |
| `CompositeWithUIAndOETF.shader` | For HDR displays: combines the UI with the scene and applies the display's transfer curve | 2 |

Fog, volumetrics, reflections and shadows are not post passes in HDRP; they
are next door, under
[`Runtime/Lighting`](https://github.com/Unity-Technologies/Graphics/tree/ce1905322722287c1420a6d85de14158c1648a84/Packages/com.unity.render-pipelines.high-definition/Runtime/Lighting)
(`VolumetricLighting`, `AtmosphericScattering`, `ScreenSpaceLighting`,
`Reflection`, `Shadow`). Unity's manual explains what each effect's settings
mean, starting from
[Post-processing in HDRP](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@14.0/manual/Post-Processing-Main.html).

### Talks, papers and collections

- [Advances in Real-Time Rendering](https://advances.realtimerendering.com/),
  the SIGGRAPH course, every year's slides. Where studios publish:
  volumetric fog (2014, 2015), temporal anti-aliasing (2014), stochastic
  screen-space reflections (2015).
- GPU Gems [1](https://developer.nvidia.com/gpugems/gpugems/contributors),
  [2](https://developer.nvidia.com/gpugems/gpugems2/copyright) and
  [3](https://developer.nvidia.com/gpugems/gpugems3/contributors), free
  online (the chapters are in each page's sidebar). Old, and many chapters
  are still the standard account.
- [Journal of Computer Graphics Techniques](https://jcgt.org/), free,
  short, practical papers with code.
- [Real-Time Rendering](https://www.realtimerendering.com/), the textbook,
  and its site's resource pages.
- Jendrik Illner's [Graphics Programming weekly](https://www.jendrikillner.com/tags/weekly/),
  for finding what was written this year.
- Stephen Hill's [SIGGRAPH and GDC link collections](https://blog.selfshadow.com/publications/).
