# Terrain generation learning outline

How this game turns a seed into a world: the noise library, the regions that
decide where sea, plains and mountains go, the height of each column, the 3D
density that makes cliffs, then water, caves, surface blocks, plants and
buildings. It also covers what the commented-out code in the generator was
for, what a review of the code found, and what to build or study next.

Everything here was read from the code at commit `cd388f3`. The percentages
marked _measured_ come from a throwaway program that linked the game's own
`shared/` generator code and FastNoiseSIMD, loaded
`resources/gameData/worldGenerator/default.wgenerator` with seed 1234, and
counted: 2D noises over 2048×2048 blocks, 3D noises over 8.4 million
cells, regions over 256×256 chunks, structure chunks over a million
chunks. They describe the default settings and will move if you change
them.

The companion files are about drawing:
[`webgpu-learning-outline.md`](webgpu-learning-outline.md),
[`webgpu-3d-learning-outline.md`](webgpu-3d-learning-outline.md) and
[`shader-effects-learning-outline.md`](shader-effects-learning-outline.md).
Nothing in this one touches the GPU. The generator is plain C++ that runs on
the server.

---

## How to read this

Each block is: **what it does → how it works → in this game → read**. "In
this game" names the file and function, or says plainly that the game does
not have it.

Words used throughout:

| Word | Meaning here |
| --- | --- |
| Chunk | 16×16 columns, 256 blocks tall (`CHUNK_SIZE`, `CHUNK_HEIGHT`). The unit the generator works in |
| Column | One (x, z) position in a chunk, bottom to top |
| Water level | y = 65 (`waterLevel` in `worldGenerator.cpp`). All water in the world sits at this height |
| Region | One Voronoi cell of chunks, about 9.5 chunks (150 blocks) across. Has one height level and one biome |
| Level | A region's height class, 0 to 5: deep water, water, plains, hills, mountain, big mountain |
| Shaped noise | A noise value after the game's three steps: move to 0..1, raise to a power, pass through a spline |

The files:

| File | What is in it |
| --- | --- |
| `src/gameLayer/worldGenerator.cpp` | `generateChunk`, the whole per-chunk generator, and `calculateBlockPass1`, the surface and water pass |
| `shared/worldGeneratorSettings.h`, `.cpp` | The noise objects, their settings, the settings file's reader and writer, and `getRegionHeightAndBlendingsForChunk` |
| `shared/splines.h`, `.cpp` | The piecewise-linear curve every noise passes through |
| `shared/biome.h`, `.cpp` | Biomes, their surface blocks, and the tree and grass tables |
| `resources/gameData/worldGenerator/default.wgenerator` | Every tunable number, as text |
| `include/gameLayer/worldGenerator.h` | `StructureToGenerate`: a request to stamp a tree or building |
| `src/gameLayer/structure.cpp` | Loads the `.structure` files |
| `src/gameLayer/multyPlayer/serverChunkStorer.cpp` | `getOrCreateChunk` calls the generator, then `generateStructure` stamps the requests |
| `thirdparty/FastNoiseSIMD/` | The noise library |

Line numbers are given only where there is no function name to point at.
They are for commit `cd388f3` and will drift.

---

## 1. The whole pipeline on one page

**What it does:** `generateChunk` fills one chunk's 16×16×256 blocks and
returns a list of structures (trees, stones, buildings) to stamp afterwards.
It is a pure function of the seed, the settings and the chunk's position, so
any chunk can be made at any time, in any order, and neighbours line up.

**The order, for one chunk:**

1. **Super flat?** If the world type is super flat: stone to y = 4, dirt to
   y = 7, grass at y = 8, and return.
2. **Region query.** `getRegionHeightAndBlendingsForChunk` says which region
   the chunk is in, its level (0 to 5), a blended level for each column, how
   close each column is to a region border, the region's centre and one
   random number for the region. (Section 4.)
3. **Biome.** `pickChunkBiome` turns the region's random number into a
   biome. (Section 5.)
4. **Noise.** About thirty arrays are filled for the chunk, each shaped by
   its power and spline. (Section 2.)
5. **Structure chunk?** A hash of the chunk position decides whether this
   chunk may hold a building. (Section 14.)
6. **For each of the 256 columns:**
   1. Height: the level's range, changed by rivers, lakes, canyons, swamps,
      roads, peaks and hills. (Section 6.)
   2. Fill the column with stone up to that height wherever the 3D density
      allows, with pockets of sand, gravel and clay. (Sections 7, 11.)
   3. Carve the underground river tunnel, if there is one. (Section 8.)
   4. `calculateBlockPass1`: surface block, three blocks of dirt, road and
      bridge blocks, then water and the block under the water.
      (Sections 8, 9, 11.)
   5. Carve caves. (Section 10.)
   6. Ask for a stone, a tree or a tuft of grass on top. (Section 13.)
7. **Building.** If this is a structure chunk, pick a building and ask for
   it at the chunk's lowest column. (Section 14.)

Then, back in `ServerChunkStorer::getOrCreateChunk`: blocks that earlier
structures left for this chunk ("ghost blocks") are applied, and each
requested structure is stamped, spilling into neighbouring chunks where it
needs to.

**Two things to notice about the order.** Water goes in before caves, so
caves are told not to remove water. Trees go in after caves, so a tree can
stand on a surface block with a cave directly under it.

**In this game:** the server generates at most one chunk per frame
(`MAX_GENERATE` in `server.cpp`). Chunks are saved as blocks only. When a
chunk is loaded from disk, `computeChunkBiome` repeats steps 2 and 3 to get
its biome back.

---

## 2. Noise, and FastNoiseSIMD

**What it is:** A noise function takes a position and returns a number that
looks random but changes smoothly: nearby positions give nearby values. The
same seed and position always give the same value. That is what lets each
chunk be generated alone and still match its neighbours.

**The kinds, and what each looks like:**

| Type | Looks like | In this game |
| --- | --- | --- |
| White noise | Static. No relation between neighbours | Per-column dice: tree chances, block mixes, structure variants |
| Value | Smooth blobs with a visible grid | Not used |
| Perlin | Smooth hills, slightly grid-aligned | Peaks and valleys; the region level lookup |
| Simplex | Like Perlin with less grid, cheaper in 3D | Almost everything |
| Cubic | Smoother and rounder than value noise | The spaghetti caves |
| Cellular (Voronoi, Worley) | Cells around scattered points | Regions, tree types, patches of alternative blocks |
| Fractal versions (`…Fractal`) | Several copies added, each finer and weaker | Most terrain noises |

**The numbers that control a noise:**

- **Frequency**: how fast it changes. Features are about `1 / frequency`
  blocks across.
- **Axis scale**: a multiplier on position, per axis, before frequency. The
  game sets it on x and z only (`SetAxisScales(scale, 1, scale)`), so
  horizontal feature size is about `1 / (frequency × scale)` blocks and
  vertical size is `1 / frequency`.
- **Octaves**: how many copies a fractal noise adds. Each copy has twice the
  frequency (lacunarity 2) and half the strength (gain 0.5). One octave
  gives smooth blobs; four give blobs with rough edges.
- **Seed**: which of the endless possible patterns you get.

**Cellular noise returns different things from the same cells**, and the
game uses five of them on one set of cells:

| Return type | Value | Used for |
| --- | --- | --- |
| `NoiseLookup` | A second noise sampled at the cell's centre: the same value across the whole cell | The region's level |
| `CellValue` | A random number per cell | The region's biome and vegetation; tree type |
| `Distance` | Distance to the cell's centre, growing toward its edges | "How close to a region border am I" |
| `CellX`, `CellZ` | The cell centre's coordinates | Identifying a region; hay land; the map |

`CellX` and `CellZ` are not in the original library. The copy in
`thirdparty/` adds them.

**How the library is called:** FastNoiseSIMD computes 4 or 8 samples at
once with CPU vector instructions (`WorldGenerator::init` picks NEON on ARM,
AVX2 on x86). You never ask for one value. You ask for a box:

```cpp
noise->FillNoiseSet(out, xStart, yStart, zStart, xSize, ySize, zSize);
float v = out[x * ySize * zSize + y * zSize + z];   // -1..1
```

`generateChunk` fills a 16×1×16 box for a 2D noise and a 16×256×16 box for a
3D one, then reads it through small lambdas such as `getRivers(x, z)` and
`getDensityNoiseVal(x, y, z)`.

**Shaping: the three steps every noise goes through**

```cpp
v = (v + 1) / 2;                 // -1..1 becomes 0..1
v = powf(v, power);              // bend: above 1 pushes values down, below 1 up
v = spline.applySpline(v);       // remap through a curve
```

The spline (`shared/splines.cpp`) is up to 20 points joined by straight
lines. The input is x, the output is y. Despite the name it is not a smooth
curve, and that is useful: steps are allowed.

Raw noise decides _where_ things vary. The spline decides _what that means_.
Four shapes do nearly all the work in this game:

| Spline shape | Result | Used for |
| --- | --- | --- |
| A ramp with bends | Stretch some ranges, flatten others | Height (`continentalnessNoise`), peaks |
| 0 until a threshold, then rising | Nothing most places, something in patches | Lakes, random hills, swamp mask, stone spike mask |
| 1 everywhere except a narrow dip to 0 | A thin line where the noise crosses one value | Rivers, roads, canyons, spaghetti caves |
| A staircase | A few flat levels | The 3D density: terraces |

The third one deserves a name: the **level-set trick**. Smooth noise crosses
any given value along winding curves, like contour lines on a map. A spline
that is 0 only near that value turns the contour into a line. In 3D the same
trick gives a thin curved sheet, and two sheets cross in a tube (section 10).
The lines it draws are closed loops and blobs, because contour lines are. They
do not branch like real rivers or lead anywhere like real roads.

**The noises, with the default settings:**

| Settings key | Seed + | Type | Feature size, blocks | Used for |
| --- | --- | --- | --- | --- |
| `continentalnessNoise` | 2 | Simplex fractal, 4 octaves | 365 | Where a column sits inside its level's height range |
| `continentalnessNoise2` | 33 | Simplex fractal, 3 | 667 | A second height noise |
| `continentalnessPickNoise` | 43 | Simplex fractal, 4 | 1333 | Meant to blend the two above (see section 16) |
| `peaksAndValies` | 5 | Perlin fractal, 3 | 60 | Small bumps on the height |
| `randomHills` | 45 | Simplex fractal, 3 | 247 | Occasional hills; 0 in 92% of columns |
| `wierdness` | 6 | Simplex fractal, 3 | 463 | How much the 3D density may carve |
| `stonetDnoise` | 3 | Simplex fractal, 4, 3D | 84 wide, 143 tall | The 3D density |
| `riversNoise` | 9 | Simplex fractal, 3 | 394 | Rivers and underground rivers |
| `lakesNoise` | 17 | Simplex fractal, 3 | 111 | Lakes |
| `swampNoise`, `swampMask` | 18, 20 | Simplex fractal, 5 and 4 | 102, 667 | Swamps: detail × where |
| `hillsDrop` | 11 | Simplex fractal, 3 | 1042 | Canyons through hills |
| `roadsNoise` | 10 | Simplex fractal, 2 | 595 | Roads |
| `stoneSpikesNoise`, `stoneSpikesMask` | 19, 21 | Simplex fractal, 4 | 33, 826 | Bare rock spikes: detail × where |
| `stonePatches` | 13 | Simplex fractal, 4 | 38,000 | Bare stone ground; how many loose stones |
| `cavesNoise` | 16 | Simplex fractal, 3, 3D | 79 wide, 71 tall | Cheese caves |
| `spagettiNoise` | 4 | Cubic fractal, 2, 3D | 44 wide, 40 tall | Spaghetti caves |
| `treesAmountNoise` | 14 | Simplex fractal, 3 | 513 | Where forests are |
| `treesTypeNoise` | 15 | Cellular, cell value | 509 | Which kind of forest |
| `iceNoise` | 22 | Simplex fractal, 8 | 117 | Ice sheets on water in icy biomes |
| `randomSand` | 12 | — | — | Only its power and spline are used (section 11) |
| `vegetationNoise` | — | — | — | Saved and loaded, never used |

Five more are set up in code (`WorldGenerator::applySettings`) and have no
entry in the settings file: `whiteNoise` (seed + 0), `whiteNoise2` (+ 1),
`randomStonesNoise` (+ 100, plain simplex, 83 blocks, the same in all three
axes), `alternativePatchesOfBlocks` (+ 101, cellular, cells of 62 blocks)
and the region noise (seed + 0, section 4).

Every noise gets the world seed plus its own offset. Without the offsets
every noise would be the same pattern, and rivers would follow roads.

**The settings file** is text. One block per noise:

```
riversNoise:
	{
	scale: 0.141000;
	type: SimplexFractal;
	celularReturnType: Distance;
	frequency: 0.018000;
	octaves: 3;
	perturbFractalOctaves: 1;
	power: 1.400000;
	spline:
		{
		0.000000, 1.000000;
		0.364378, 1.000000;
		...
		}
	}
```

`celularReturnType` only matters when `type` is `Cellular`.
`perturbFractalOctaves` is read and passed on, but no perturb type is ever
set, so it changes nothing. The reader is strict: an unknown key makes the
whole load fail, and the server prints `NOISE LOADING ERROR` and exits.

**Read:**

- [The Book of Shaders, chapters 11 to 13](https://thebookofshaders.com/11/):
  noise, cellular noise and fractal noise, with live examples.
- Red Blob Games,
  [Making maps with noise functions](https://www.redblobgames.com/maps/terrain-from-noise/):
  frequency, octaves and redistribution (this game's power step) with
  sliders.
- Inigo Quilez on [fractal noise](https://iquilezles.org/articles/fbm/).
- The library: [FastNoiseSIMD](https://github.com/Auburn/FastNoiseSIMD) and
  its [noise settings wiki page](https://github.com/Auburn/FastNoiseSIMD/wiki/Noise-Settings).

---

## 3. Seeing the noise and adjusting it

**What exists today:** there is no noise viewer in the build. The original
project has one, a separate OpenGL program called `noiseTool`. This fork
deleted it when the build became WebGPU only. The create-world menu also has
a region preview (`WorldGenerator::generateChunkPreview`), but its call is
commented out (`renderSettings.cpp`, near line 1542).

Ways to look, from least work to most:

**1. In the game.**

- **M** opens the map (`mapEngine.cpp`): the top block of each column, with
  region borders tinted red and each region's centre chunk tinted blue.
  This is the fastest way to see regions, rivers, roads and forests from
  above.
- **F9** opens the developer windows. One of them shows the current chunk's
  vegetation value and region centre.
- **Command+F3** (Ctrl+F3) shows the debug text, with your position.
- `OURCRAFT_SPAWN="x,y,z"` in the environment moves the spawn, to start a
  run where a feature is.
- World type **Super Flat** removes terrain when you want to look at
  something else.

**2. A small program that writes a noise to an image.** This is what the
measurements in this file came from. It links the same `shared/` code the
game uses, so what it draws is what the generator gets. Save as
`build/noisePeek.cpp` (the `build/` folder is git-ignored):

```cpp
// noisePeek.cpp: writes one of the world generator's noises to a PNG,
// with the game's own power and spline applied.
#include <worldGeneratorSettings.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <cmath>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write/stb_image_write.h>

int main()
{
	std::ifstream f("resources/gameData/worldGenerator/default.wgenerator");
	std::stringstream text; text << f.rdbuf();
	WorldGeneratorSettings settings;
	if (!settings.loadSettings(text.str().c_str())) { return 1; }

	WorldGenerator wg;
	wg.init();
	wg.applySettings(settings);

	// The noise to look at, and the power and spline that go with it.
	FastNoiseSIMD *noise = wg.riversNoise;
	float power = wg.riversPower;
	Spline &spline = wg.riversSplines;

	const int size = 1024;                  // blocks on a side
	const int startX = 3000, startZ = 3000; // world position of the corner
	float *set = noise->GetNoiseSet(startX, 0, startZ, size, 1, size);

	std::vector<unsigned char> pixels(size * size);
	for (int x = 0; x < size; x++)
		for (int z = 0; z < size; z++)
		{
			float v = (set[x * size + z] + 1) / 2; // -1..1 to 0..1
			v = spline.applySpline(powf(v, power));
			pixels[x + z * size] = (unsigned char)(std::fmin(std::fmax(v, 0.f), 1.f) * 255);
		}

	FastNoiseSIMD::FreeNoiseSet(set);
	stbi_write_png("noise.png", size, size, 1, pixels.data(), size);
}
```

Build and run from the repository root, after the game has been built once
(that makes the FastNoiseSIMD library):

```sh
c++ -O2 -std=c++17 \
  -Ishared -Ithirdparty/glm -Ithirdparty/FastNoiseSIMD/FastNoiseSIMD \
  -Ithirdparty/magic_enum-0.9.3/include -Ithirdparty/stb_image_write/include \
  build/noisePeek.cpp shared/worldGeneratorSettings.cpp shared/splines.cpp \
  shared/easing.cpp build/thirdparty/FastNoiseSIMD/libfastNoiseSIMD.a \
  -o build/noisePeek
./build/noisePeek        # writes noise.png: one pixel per block, white = 1
```

On macOS the compiler prints one deprecation warning from the image
writer; it is harmless.

Change the three lines under "The noise to look at" to see another one. The
names are in `shared/worldGeneratorSettings.h`. For a side view of a 3D
noise, ask for a `size × 256 × 1` set and index it `x * 256 + y`. For the
regions, call `wg.getRegionHeightAndBlendingsForChunk` once per chunk and
colour by the level it returns.

**3. The original tool.** `noiseTool/` is still in this repository's
history at the tag `opengl-final`, and on the `master` branch of
[meemknight/ourCraft](https://github.com/meemknight/ourCraft). Read it with
`git show opengl-final:noiseTool/src/gameLayer/noiseGameLayer.cpp`. It has
two windows. "Settings" edits every noise (scale, type, frequency, octaves,
power) and its spline, with add, remove and invert buttons, and saves and
loads. "Noise Viewer" shows each noise as a grey image after shaping, with a
red square the size of one chunk and a green one the size of ten, side
views of the 3D noises, and a cross-section coloured as stone, grass, dirt,
water, cave and sky. It needs OpenGL, and it has not been built against
this fork. Two things to know if you bring it back: it saves to
`default.mgenerator`, while the game reads `default.wgenerator`; and its
cross-section uses older height numbers than the game does now.

**4. Bring the viewer into the game.** ImGui already draws through WebGPU
(`src/platform/wgpuImgui.cpp`). A developer window that fills a texture from
a noise and shows it, with the spline editor from the old tool, would give
the same thing without a second program. This is the best long-term answer
and is listed under improvements.

**How to adjust:**

| To change | Edit | Takes effect |
| --- | --- | --- |
| A noise's size, type, power or spline | `resources/gameData/worldGenerator/default.wgenerator` | Worlds created afterwards. The create-world menu copies this file into the world |
| The same, for one existing world | `resources/worlds/<name>/worldGenSettings.wgenerator` | Chunks generated afterwards. Chunks already saved keep their old shape, so expect seams |
| The seed | The `seed:` line of the world's file | The same |
| Height ranges, water level, thresholds, block choices | Constants in `worldGenerator.cpp` | After a rebuild |
| Biome blocks, tree tables | `shared/biome.cpp` | After a rebuild |
| Region size and level lookup | `WorldGenerator::applySettings` (`SetFrequency(0.1)`, `SetCellularNoiseLookupFrequency(0.35)`) | After a rebuild |

**Rules of thumb when tuning:**

- **Bigger features:** lower `frequency` or `scale`. Halving either doubles
  the size.
- **More of something that has a threshold** (lakes, swamps, hills): move
  the spline's first non-zero point left, or lower `power`.
- **Wider rivers, roads or canyons:** widen the dip in the spline. Lower
  `scale` also widens them, because the noise crosses the dip more slowly.
- **Rougher edges:** more `octaves`.
- **Change one thing, then look.** Power and spline interact: power moves
  where the noise's values pile up, and the spline's thresholds do not move
  with it.

---

## 4. Regions: where sea, plains and mountains go

**What it does:** The world is divided into Voronoi cells of chunks. Each
cell is a _region_ with one level from 0 to 5. The level picks the range of
heights the terrain can take, and so decides whether the region is sea,
plains, hills or mountain.

**How it works:** `getRegionHeightAndBlendingsForChunk`
(`shared/worldGeneratorSettings.cpp`) samples cellular noise at _chunk_
coordinates, not block coordinates, so a whole chunk is always in one
region. The noise's frequency is 0.1, which puts one cell point in every
10×10 chunks, moved off the grid by a jitter of 0.31.

For the chunk and its eight neighbours it reads:

1. **The level.** The cellular noise returns a Perlin noise sampled at the
   cell's centre. That value goes through `regionsHeightSpline`, then
   `floor(value × 6)`, clamped to 5. Because the Perlin noise is smooth from
   one cell centre to the next, neighbouring regions tend to have similar
   levels: seas come in groups of cells, and mountains are surrounded by
   hills more often than by sea.
2. **The cell centre** (`CellX`, `CellZ`), in chunk coordinates. Two chunks
   are in the same region when their centres are equal.
3. **The cell's random value** (`CellValue`), 0 to 1.

Regions whose centre is within 16 chunks of the origin are forced to
plains, so a new player starts on land.

**Measured:** a region covers about 90 chunks, roughly 9.5 chunks or 150
blocks across. The levels' share of the world:

| Level | Name | Share |
| --- | --- | --- |
| 0 | Deep water | 3% |
| 1 | Water | 16% |
| 2 | Plains | 33% |
| 3 | Hills | 33% |
| 4 | Mountain | 13% |
| 5 | Big mountain | 1 to 2% |

So about a fifth of the world is sea, in bodies a few regions across. There
are no oceans in the sense of water to the horizon, and no continents: the
noise the code calls "continentalness" does something else (section 6).

**Blending.** A level that changed at a chunk edge would make a wall. So
each column gets a _blended level_: the average level over a 28×28 block
window around it, counting each neighbouring chunk's level for the part of
the window that falls in it. Deep inside a region the blended level is a
whole number. Within 14 blocks of a chunk in another region it is a
fraction, such as 2.4 between plains and hills. _Measured:_ 24% of columns
are in a blend.

**Two more outputs:**

- **Bordering factor**, per column: the cellular `Distance` value at block
  resolution. Near 0 at a region's centre, rising toward its edges
  (_measured:_ mean 0.17, above 0.5 in 5% of columns). It is used to keep
  lakes away from borders, to open river valleys near the edges of hill
  regions, and to place beach sand.
- **Tight borders**: the columns on a chunk's edge that touch a chunk from
  another region. Only the map uses them, for the red lines.

**In this game:** called once per generated chunk, and again by
`computeChunkBiome` for a chunk loaded from disk. Each call asks the
library for five freshly allocated noise sets; the `TODO optimize` comment
above the function is about that.

**Read:**

- Red Blob Games,
  [Voronoi maps tutorial](https://www.redblobgames.com/x/2022-voronoi-maps-tutorial/):
  regions from scattered points, with elevation assigned per region.
- Inigo Quilez on [Voronoi edges](https://iquilezles.org/articles/voronoilines/):
  why the plain distance value is a poor measure of "how near the border",
  and what to use instead.

---

## 5. Biomes

**What it does:** A biome decides the surface and under-surface blocks, the
bed of a swamp, and whether water freezes. Each region has one biome.

**How it works:** `pickChunkBiome` (`worldGenerator.cpp`) takes the
region's random value, 0 to 1, and indexes a list of ten entries in
`BiomesManager::biomeIndexes` (`shared/biome.cpp`): plains ×4, desert,
plains, snow, plains, plains, plains. So a value from 0.4 to 0.5 is desert,
0.6 to 0.7 is snow, and everything else is plains. _Measured:_ the values
are close to uniform, so about one region in ten is desert and one in ten is
snow.

One override: a region at level 2 (plains) has a chance of becoming **hay
land**, whatever the list said. The chance is 0.5 if the region's centre is
within 20 chunks of the origin, 0.3 within 40, and 0.2 beyond. It is drawn
from a random generator seeded by the region's centre and the world seed.

| Biome | Surface / under it | Notes |
| --- | --- | --- |
| Plains | Grass / dirt | Patches of yellow grass, one of sand, one of stone |
| Desert | Sand / sandstone | Patches of hard sandstone; one of stone and one of grass |
| Snow | Snowy dirt / dirt | `isICy`: water freezes; clay pockets become ice |
| Hay land | Mostly yellow grass / dirt | Named "plains" in the code. Hay instead of loose stones; barns |
| Waste land | Volcanic rock | Defined, never picked: its line in `biomeIndexes` is commented out |

**Patches inside a biome.** Each biome has a list of up to 40
`blockVariations`, pairs of surface and under-surface block. A cellular
noise (`alternativePatchesOfBlocks`, cells of about 62 blocks) picks one
entry per cell. Most entries are the biome's normal pair, so most cells look
normal, and a few are yellow grass, sand or bare stone.

**What a biome does not do here:**

- It does not depend on climate. There is no temperature or humidity. Snow
  can sit beside desert at sea level.
- It does not depend on height. A level 5 region can be desert.
- It does not blend. The surface block changes on the chunk line between
  two regions.
- It does not choose the trees. Every biome uses the same tree tables
  (section 13). Deserts are bare only because trees need grass to stand on.

`BiomesManager::determineBiome(temperature, humidity)` exists as an empty
function that returns 0: the place where a climate model was going to go.

**Read:**

- [Minecraft Wiki, Biome](https://minecraft.wiki/w/Biome): how Minecraft
  1.18 and later choose a biome from five noises (temperature, humidity,
  continentalness, erosion, weirdness) and depth.
- Henrik Kniberg,
  [Minecraft terrain generation in a nutshell](https://www.youtube.com/watch?v=CSa5O6knuwI).
  The names `continentalness`, `peaksAndValies` and `wierdness` in this
  code come from the scheme this video explains.

---

## 6. The height of a column

**What it does:** Gives each column a `height`: the top of the range where
stone may be placed.

**How it works:** Each level has a _start_ and a _max_. The column's height
is somewhere between them, chosen by the height noise.

| Level | Name | Start | Max | Peaks add to start / max | Hills add to start / max |
| --- | --- | --- | --- | --- | --- |
| 0 | Deep water | 22 | 40 | 5 / 5 | 5 / 5 |
| 1 | Water | 22 | 68 | 5 / 5 | 5 / 20 |
| 2 | Plains | 66 | 71 | 10 / 5 | 10 / 45 |
| 3 | Hills | 67 | 100 | 20 / 20 | 10 / 45 |
| 4 | Mountain | 68 | 227 | 20 / 10 | 10 / 30 |
| 5 | Big mountain | 80 | 237 | 20 / 0 | 4 / 4 |

The steps, in the `getHeightForBiome` lambda inside `generateChunk`:

1. **Start and max** are read from the table at the column's blended level,
   interpolating between two rows when the level is a fraction. This is the
   only place the blend is used for height.
2. **Features change start and max**: rivers, lakes, canyons, swamps and
   stone spikes each pull them toward their own values (sections 8 and 9).
3. **Peaks.** `start += peaks × add − 3`, and the same for max with its own
   `add`. On plains the peaks value is square-rooted first, which pushes it
   up and flattens it.
4. **Hills.** `start += hills × add`, and the same for max. The hills
   noise is 0 in most places (_measured:_ above 0 in 7.5% of columns), so
   this is an occasional hill, tallest on plains and hills regions where max
   gets up to 45.
5. **Height.** `height = start + c × (max − start)`, where `c` is the
   height noise, 0 to 1.

The height noise `c` is a blend of two noises (`continentalnessNoise` and
`continentalnessNoise2`) with different sizes and splines, weighted by a
third. In Minecraft "continentalness" means distance inland. Here it does
not place land and sea at all (regions do that). It is the fraction of the
way from start to max.

**What the numbers give:**

- **Plains** have a 5-block range, so they are flat: about y = 66 to 72,
  with the odd hill.
- **Hills** range over 33 blocks plus up to 20 from peaks.
- **Mountains** range over about 160 blocks. One smooth noise stretched
  over that much height is what makes the big slopes.
- **Level 1 water** runs from y = 22 to 68, so where the height noise is
  near its top the seabed rises above the water as an island.

**Stone spikes** (`stoneSpikesNoise × stoneSpikesMask`): where the product
is above 0.2, start rises by up to 25 and max by up to 40, at any level. The
detail noise is small (33 blocks) and the mask is rare, so these are tight
clusters of rock pillars in a few places (_measured:_ 0.16% of columns).
Where the product reaches 0.32 the pillars also stay bare stone.

**Near the starting tavern** (within 3 chunks of chunk (3, 4)): no rivers,
no lakes, and hills are damped, so the building has flat dry ground.

---

## 7. 3D density: cliffs, terraces and overhangs

**What it does:** A height alone gives rolling ground with no cliffs and
nothing overhanging. So the generator also asks a 3D noise, for every block
under the height, "is there stone here?"

**How it works:** In the column loop of `generateChunk`:

```cpp
density = densityNoise(x, y, z);                       // 0..1, shaped
t = clamp((y - start) / (max - start), 0, 1);          // 0 at the bottom of the range, 1 at the top
exponent = mix(0.1, remap(t, 0..1 -> 0.2..7), weirdness);
density = pow(density, exponent);
solid = y < height && density > 0.4;
```

Raising a number between 0 and 1 to a small exponent pushes it toward 1; a
large exponent pushes it toward 0. So:

- **Weirdness 0:** the exponent is 0.1 everywhere, nearly every density
  passes, and the column is solid up to its height. Plain heightmap terrain.
- **Weirdness 1:** the exponent runs from 0.2 at the bottom of the range to
  7 at the top. Low down everything is still solid. High up only the densest
  noise passes, so the upper part of the range is carved away in the shape
  of the 3D noise: cliffs, ledges, overhangs and the occasional floating
  piece.

The density's spline is a **staircase** with eight flat levels. Because the
values come in steps, whole bands of the noise pass or fail together as the
exponent grows with height, and the carved shapes come out as terraces
rather than smooth slopes.

**Measured:** weirdness averages 0.26 and is below 0.4 in 85% of columns,
so most terrain is close to a plain heightmap and the carved kind is the
exception.

The top solid block of the column is remembered as `firstH`. Everything
after this step (surface blocks, trees, structures) uses `firstH`, not
`height`. If the top of a column is a floating piece, that piece gets the
grass.

**In this game:** y = 0 is always solid. It is ordinary stone (or a sand,
gravel or clay pocket), not a special unbreakable block.

**Read:**

- [GPU Gems 3, chapter 1](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu):
  terrain as a density function, with terraces, overhangs and the tricks for
  shaping it. Written for the GPU; the ideas are the same on the CPU.
- [Minecraft Wiki, Density function](https://minecraft.wiki/w/Density_function):
  how Minecraft builds its terrain from the same idea, as data.
- Alan Zucconi,
  [The world generation of Minecraft](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/).

---

## 8. Water: seas, rivers, lakes, swamps and ice

**The one rule:** there is no water simulation at generation time and no
water above or below y = 65. Water appears wherever the ground is lower
than that. Every water feature is therefore made the same way: **lower the
ground below 65 and let the fill pass find it.**

**The fill** is the second loop of `calculateBlockPass1`. For each column
it walks down from y = 67 to y = 20:

- Air at or below 65 becomes water.
- The first stone under the water is replaced by a bed block (section 11).
- Anything else stops the walk.

Because the walk starts at 67 and stops at the first solid block, a column
whose ground is above 66 is never filled, and an air pocket under a roof
stays dry even below sea level.

### Seas

Regions at level 0 and 1. Their height ranges (22 to 40, and 22 to 68) are
mostly under 65, and the fill does the rest. Level 0 is 20 to 45 blocks
deep. Level 1 is shallower and has islands.

**Beaches:** a column gets sand as its surface and under-surface when its
blended level is between 1.1 and 1.9, which means it is in the blend
between a water region and a plains region, and its bordering factor is
above 0.1.

### Rivers

**How it works:** `riversNoise` has the level-set spline: 1 everywhere
except a narrow dip to 0. On **plains** (level 2):

```cpp
start = mix(waterLevel - 5, start, rivers);   // rivers = 0 in the channel
max   = mix(waterLevel - 2, max,   rivers);
```

In the channel the height range becomes 60 to 63 before the peaks are
added, the ground ends up a few blocks below 65, and the fill makes it a
shallow river. The banks slope because the spline's dip has sloped sides.

**What they are like:** closed loops and blobs, with pools where the noise
is flat. _Measured:_ on plains, 8.5% of columns are in a channel and 13.6%
are lowered at least a little; the channel averages 13 blocks wide, 21
counting the banks. They all sit at
y = 65. They do not flow downhill, start at a spring, or end at the sea,
except by accident. The author's `todo.txt` has both of these on the list:
rivers that enter the ocean, and rivers at different heights.

Rivers exist only on plains. At the edge of a hills region the same noise
line continues as something else:

### Underground rivers

On **hills and mountains** (level 3 and up) the river line becomes a
tunnel. Where the river value is strong enough, the generator clears air
from y = 61 to 72 at the centre of the line, narrowing toward its edges.
This happens before the fill, so the middle of the tunnel gets water up to
65 and the edges stay dry.

In hills regions (level 3 only) the ground above the tunnel is also pulled
down toward y = 69 to 83, in proportion to the bordering factor. Near the
edge of the region the valley opens and the river shows; toward the centre
it goes under the hill.

### Lakes

`lakesNoise` is 0 in most places and rises in blobs (_measured:_ above 0.1
in 1.2% of columns). On plains, the river value is multiplied by
`1 − lake`, so a lake behaves as a river channel that is a blob instead of
a line: the same depth, the same fill. Lakes fade toward region borders.
The code that lowered the ground for lakes directly is commented out; the
multiplication is what does it now.

### Swamps

On plains, where `swampNoise × swampMask` is above 0.05, start is pulled
toward 63 and max toward 65. The detail noise is small and busy, so the
ground hovers around water level and breaks into shallow pools. Where the
value reaches 0.5, the bed is the biome's swamp block list (dirt with some
clay on plains) instead of the usual stone and gravel. _Measured:_ 2.1% of
columns are swampy at all, 0.3% strongly.

### Ice

In an icy biome the top layer of water (y = 65) becomes ice where
`iceNoise` is above 0.5, and along the edges of rivers and lakes. Water
below the top layer stays water.

**Read:**

- Red Blob Games,
  [Procedural river growing](https://www.redblobgames.com/x/1723-procedural-river-growing/)
  and [mapgen4](https://www.redblobgames.com/maps/mapgen4/): rivers that
  form networks and reach the sea.
- [Minecraft Wiki, World generation](https://minecraft.wiki/w/World_generation):
  Minecraft's rivers are also a noise's level set (where "weirdness"
  crosses zero), which is why they loop too.

---

## 9. Canyons, roads and bridges

All three are level-set lines, like rivers.

### Canyons

`hillsDrop`, in hills regions (level 3) only. Where the line runs:

```cpp
start = mix(waterLevel + 1, start, dropDown);   // dropDown = 0 in the canyon
max   = mix(waterLevel + 6, max,   dropDown);
```

The range comes down to 66 to 71 before the peaks are added, which puts
the floor around y = 70 to 80, between hills that rise to 100 and more. The
effect fades out where the region blends into a mountain region.
_Measured:_ 24% of hills columns are lowered at least a little; the deep
part averages 35 blocks wide, and 85 counting the slopes. These are wide
winding valleys more than narrow slots.

### Roads

`roadsNoise`, on plains (level 2) and on the lower parts of hills (level 3
where the height noise is under 0.42).

- **Flattening.** Near the line, the peaks value is pulled toward 0.5, so
  the small bumps go away and the ground under the road is level.
- **Blocks.** At the centre of the line the surface is path block. Beside
  it, a random mix of grass, gravel, coarse dirt and path, so the edge is
  ragged.

_Measured:_ 3.9% of columns have road blocks and 2.6% are the path centre;
a road averages 11 blocks wide, with 7 of path down the middle. Roads are
loops that go nowhere in particular: nothing connects them to the
buildings.

### Bridges

There are two kinds, both made one column at a time inside
`calculateBlockPass1`. Neither is a structure file.

**River bridge.** While the fill pass walks down a column on plains, if the
column is a road and the block is air with air under it, it places wood
instead of leaving a gap: a plank at y = 66 near the banks and a slab at
y = 67 over the middle of the river. Wherever a road line crosses a river
or lake, this makes a low wooden deck the width of the road, slightly
arched.

**Canyon bridge.** Where a road line crosses a canyon (canyon value above
0.2, at the road's centre), a wooden slab is placed in the air at y = 86 to
89, lower toward the canyon's middle, top or bottom half of the block
depending on the fraction. Only where there is air: if the hillside is
there, nothing is placed. The road's own blocks are not drawn on the canyon
floor under it.

The canyon bridge's height is a constant. It does not look at how high the
canyon's rims are, so it can float above low rims or stop short of high
ones. A commented-out condition (section 15) was a first try at that.
Neither bridge has rails or supports.

---

## 10. Caves

**What it does:** Removes stone under the surface, in two shapes.

**How it works:** After the surface pass, for each block from y = 2 up to
just below the column's top block:

**Cheese caves.** `cavesNoise` is a 3D noise whose spline is 0 below a
threshold and jumps up above it. Where the shaped value is under 0.5 the
block becomes air. This is a plain threshold on blobby 3D noise, so it
gives rooms and caverns: the holes in the cheese. _Measured:_ 6.3% of the
volume.

**Spaghetti caves.** Elsewhere, `spagettiNoise` is tested. Its spline is
the level-set dip, which in 3D is a thin curved sheet. One sheet would cut
the world into slabs. So the noise is sampled twice, the second time 22, 19
and 16 blocks away, and the two are combined:

```cpp
density = 1 - (1 - a) * (1 - b);     // "screen blend": low only where both are low
if (density <= 0.5) air;
```

Two sheets cross along a curve, so the air is a winding tube. _Measured:_
3.7% of the volume. Together, a tenth of the underground is cave.

**The rules around the edges:**

- Caves never remove water. A cave can open directly under a lake's bed,
  and the generator leaves the water sitting on top of the hole.
- The loop stops below the column's top block, so that block always
  survives. A cave reaches the open air through a slope or a cliff, where
  the column next door is lower. It never punches a hole straight up through
  flat ground.
- Caves run after the dirt is placed, so they cut through dirt as well as
  stone.
- There is as much cave at y = 10 as at y = 200. A line of code meant to
  change that with height does nothing (section 16).

**Read:**

- [Minecraft Wiki, Cave](https://minecraft.wiki/w/Cave): cheese, spaghetti
  and noodle caves, the names this code borrows.
- Henrik Kniberg's talk,
  [Reinventing Minecraft world generation](https://www.youtube.com/watch?v=ob3VwY4JyzE),
  on why the two kinds are combined.

---

## 11. Block placement: grass, dirt, sand, gravel, clay

After the density step a column is stone and air. `calculateBlockPass1`
and the density loop turn that into the blocks you see.

**Surface and dirt.** Walking down from the top of the column to y = 65,
the first stone block found becomes the **surface block**, and the three
blocks under it become the **under-surface block**, air excepted. Then the
walk stops. The depth is always three; a `todo` in the code asks for
randomness there. Below that it is stone to the bottom.

The surface block is chosen in this order, each step overriding the last:

1. The biome's pair, as picked by the block-variation patch (section 5).
2. **Beach:** sand over sand.
3. **Stone patch** (`stonePatches` above 0.5): stone over stone, or
   cobblestone on a beach.
4. **River bank** (river or lake value above 0.2, on plains): a random pick
   from cobblestone, stone, gravel, coarse dirt and the surface block, with
   more of the surface block the farther from the channel.
5. **Road** (section 9).

Stone spikes at 0.32 or more skip the whole thing and stay bare.

**Under water.** The first stone under the water becomes, at random: stone
(3 parts), gravel (2), cobblestone (1), and sand (9 parts) where the
blended level is between 1.1 and 2, which is the shallows next to plains.
In a river channel it is stone, gravel and cobblestone. In a strong swamp
it is the biome's swamp list. Only that one block is changed.

**Pockets of sand, gravel and clay.** In the density loop, every solid
block is stone unless one of three 3D values is above 0.5: then it is
sand, gravel or clay, whichever value is highest. In an icy biome the clay
is ice. All three values come from one noise (`randomStonesNoise`, about 83
blocks in every direction) read at three heights 300 blocks apart, through
a spline that is 1 only at the extreme ends of the noise's range. So the
pockets are small and far apart. _Measured:_ sand 0.05% of the volume,
gravel 0.06%, clay 0.7%.

The pockets can be anywhere in the stone, including at the top of a column.
Since the surface walk looks for the first _stone_, a pocket at the top
stays as it is and the grass goes on the first stone under it.

**Stone patches** are close to switched off by their scale. The noise's
features are 38,000 blocks across, so a patch is a district kilometres
wide. _Measured:_ none in the 2048×2048 area near the origin; 4.8% of the
world when sampled every 1000 blocks.

---

## 12. Minerals

**In this game:** the generator places no ores. The blocks exist:
`goldOre`, `copperOre`, `ironOre`, `silverOre` and `leadOre` in
`shared/blocks.h`, each with a furnace recipe in `crafting.cpp`. But
nothing in `worldGenerator.cpp` names them, and a search of all 127
`.structure` files found none inside a building either. In a world as
generated, those recipes cannot be reached.

**How ores are usually placed**, three ways, simplest first:

1. **A 3D noise with a threshold**, the same mechanism as the sand, gravel
   and clay pockets in section 11. One noise per ore (or one noise read at
   different offsets, as the pockets do), a spline that is 1 only at an
   extreme, and a test of depth: iron anywhere, gold only below y = 40. It
   needs no new machinery, and each chunk can do it alone. The pockets come
   out blobby and the amount is hard to control exactly.
2. **Scattered veins**, the classic Minecraft way. For each chunk and each
   ore: make a fixed number of tries; for each try pick a random spot from a
   hash of the chunk position, with a height drawn from that ore's range;
   grow a small lump of a given size there, replacing stone only. The amount
   per chunk is exact and easy to balance. Lumps that cross a chunk edge
   need care: either keep them inside the chunk, or send the overflow
   through the ghost-block path that structures use (section 14).
3. **Large veins from noise**: the spaghetti-cave idea, filled with ore
   instead of air. Rare, long, worth an expedition.

**Where it would go:** in the density loop, next to the pockets, if ore
should be cut open by caves (ore showing in cave walls is how players find
it); after the cave loop if it should not. Either way after the surface
pass or restricted to below the dirt, so ore does not replace grass.

**Read:**

- [Minecraft Wiki, Ore (feature)](https://minecraft.wiki/w/Ore_(feature)):
  the vein algorithm, sizes and the height distributions (uniform and
  triangular) that give each ore a best depth.
- [Minecraft Wiki, Ore](https://minecraft.wiki/w/Ore), for the
  distribution charts.

---

## 13. Trees and plants

**What it does:** For each column, after caves, decides whether something
stands on its top block: a loose stone, a tree, or a tuft of grass. Blocks
of grass are written straight into the chunk. Trees and stones are
structures: the generator adds a request to the list and the stamping
happens later (section 14).

**How it works:** Two questions, answered by different kinds of noise.

- **Is this a forest, and which kind?** Smooth noise, so the answer is the
  same over a wide area. `treesAmountNoise` says how wooded the place is.
  `treesTypeNoise` is cellular with a random value per cell, about 500
  blocks across, and picks one of seven forest kinds for the whole cell.
- **Does this exact column get a tree?** White noise, so neighbours are
  independent. The tree is placed when the column's white noise value is
  under the chance.

The chance comes from the amount: below the kind's threshold there are no
trees; from the threshold up to 1 the chance per column rises between two
numbers.

| Kind | Trees | Threshold | Chance per column |
| --- | --- | --- | --- |
| 0 | Oak | 0.5 | 1% to 8% |
| 1 | Oak, oak, birch | 0.65 | 1% to 4% |
| 2 | Birch, birch, tall birch | 0.5 | 1.2% to 9% |
| 3 | Tall slim oak | 0.5 | 3% to 10% |
| 4 | Slim spruce with oak wood (8 in 9), spruce | 0.5 | 1% to 7% |
| 5 | Red birch, red birch, tall red birch | 0.5 | 1.5% to 5% |
| 6 | Spruce | 0.5 | 1% to 4% |

The tables are `greenBiomesTrees` in `shared/biome.cpp`.

**The region's vegetation value** bends the amount before the threshold:
`amount = pow(amount, p)`, where `p` runs from 1.2 for a value of 0 to 0.4
for a value of 1. A small exponent lifts the amount, so more of the region
is over the threshold. _Measured:_ the share of columns over 0.5 is 5% at
`p` = 1.2, 15% at 0.8 and 63% at 0.4. One number per region takes it from
scattered groves to nearly continuous forest.

**The order, per column**, stopping at the first that places something:

1. **Loose stones.** A small chance, higher near plains rivers. A small
   stone structure; in hay land, three times in four a hay pile instead.
   Skipped in structure chunks and near the tavern.
2. **Trees, first layer:** the kind from `treesTypeNoise`, the amount from
   `treesAmountNoise`.
3. **Trees, second layer:** the same two noises read far away (and the
   amount at half the size), so a second, different forest pattern
   overlaps the first. Where they overlap, kinds mix.
4. **Grass:** a tuft on about a third to two fifths of the remaining
   columns.

In structure chunks and near the tavern only grass is placed, so trees do
not grow through buildings.

**What can it grow on:** grass block, snowy dirt or yellow grass. That is
the only reason deserts, beaches and stone are bare.

**A tree is a structure request.** `generateTreeFunction` turns the tree
kind into a `StructureToGenerate`: a type (a folder of `.structure` files),
the position, four random numbers from white noise, and optionally "replace
the logs with this block" and "replace the leaves with that one". That is
how one slim spruce model serves as an oak-wood spruce, a red one and a
yellow one. The stamping code picks which file from the folder with the
first random number, the rotation with the second, and paints birch and red
leaves with random autumn colours.

**Not in the generator:** flowers (a rose patch is defined in `biome.cpp`
and never used), cactus or dead bushes, jungle and palm trees (their types
and structure folders exist, no forest kind lists them), and extra trunk
height for the "tall" kinds (the stamping code computes it and then sets it
to 0, with the note `todo re add this!`).

**Read:**

- Red Blob Games' noise article above has a section on placing trees with
  noise. For spacing that avoids clumps and gaps, Bridson's two-page
  [Fast Poisson Disk Sampling](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf).

---

## 14. Structures: buildings, stones and the dungeon

**What it is:** A structure is a small box of blocks saved in a file and
stamped into the world. Trees, loose stones, hay piles, houses, towers and
the dungeon's rooms are all the same thing at different sizes.

### The files

`resources/gameData/structures/<kind>/*.structure`. A file is a size
(three integers and one unused) followed by one 4-byte block per cell.
`StructuresManager` (`structure.cpp`) loads each folder into a list, and a
random number picks one from the list, so adding a variant is adding a
file.

Structures are built in the game and saved from the developer windows
(F9): "Load Save Stuff" → "Save structure" writes the box you have selected
to the file named there.

### Where buildings go

**1. Is this a structure chunk?** `generateFeature` hashes the chunk's
position to a number from 0 to 1 and says yes when it is under 0.01.
_Measured:_ 0.85% of chunks pass, one in about 118, which is one per 11×11
chunks on average. Chunks within 5 of the starting tavern are refused, and
the tavern's own chunk, (3, 4), always passes.

**2. Is there room?** While the columns are generated, the chunk keeps
track of its lowest column and of the most river and lake it saw. No
building is placed if the region is sea (level 0 or 1), if there is river or
lake in any column, or if the lowest column is under water level.

**3. Which one?** A list is built and a hash of the chunk position picks
from it:

- nothing (always in the list, so some structure chunks stay empty)
- goblin tower, small abandoned house, training camp, small stone ruins
- barn, three times, in hay land
- a second training camp entry near the origin

The tavern chunk gets the tavern, and the mines dungeon 20 blocks to the
side.

**4. Where exactly?** At the chunk's **lowest column**, with the
structure's footprint centred on it. Nothing flattens the ground first.
Buildings use "replace anything" and "clear the air inside": blocks of the
structure overwrite terrain, and within each column of the structure the
cells between its lowest and highest block are cleared, so a hillside does
not fill the rooms. A building on a slope ends up dug into the high side.

### Stamping, and ghost blocks

`ServerChunkStorer::generateStructure` walks the structure's footprint
column by column. For each column there are three cases:

1. **The chunk was created in this same batch:** write the blocks.
2. **The chunk already exists:** write the blocks, and queue them to be
   sent to players who have that chunk.
3. **The chunk does not exist yet:** store the blocks as **ghost blocks**
   for that chunk position. When `getOrCreateChunk` later makes that chunk,
   it applies them after generating it.

That third case is how a tree on a chunk's edge gets the half of its
leaves that hang over the neighbour, without generating the neighbour
early.

A structure block is placed over air, or over anything if the structure
says "replace anything". Trees do not, so a tree's leaves never eat a
hillside. Buildings do, with a priority: a dungeon (100) wins over a house
(2).

### The dungeon

The mines dungeon is assembled from pieces at stamping time. On an 11×11
grid of slots, the entrance goes in the middle. Then, for 6 to 18 steps, an
open piece is taken (half the time the newest one, otherwise a random one)
and grows neighbours: a room grows 1 to 4, mostly halls; a hall grows 1 or
2 along its own direction, mostly rooms. Finally each slot's piece is
stamped from the `minesDungeon/entrance`, `hall` and `room` folders, and
open ends are closed with pieces from `end`. It is placed below the ground
so that only the entrance reaches the surface.

There is one in the world, beside the tavern. A debug setting on the server
can spawn another under the player.

**Loaded and never asked for:** tree houses, the small pyramid and the
igloo have folders, loader code and a case in the stamping code, but the
generator never requests them.

**Read:**

- [Minecraft Wiki, Structure](https://minecraft.wiki/w/Structure) and
  [Jigsaw structure](https://minecraft.wiki/w/Jigsaw_structure): how
  Minecraft spaces structures on a grid so two are never close, and builds
  villages from pieces that declare where they connect.
- Bob Nystrom,
  [Rooms and Mazes](https://journal.stuffwithstuff.com/2014/12/21/rooms-and-mazes/):
  a dungeon generator explained step by step, with the trade-offs.

---

## 15. The commented-out code in the world generator

`worldGenerator.cpp` keeps a lot of old code in comments. It is a record of
what was tried. By area, with the line it starts at:

**Region and level tables**

| Line | What it was |
| --- | --- |
| 521–571 | Three older versions of the `interpolator` lambda: blending a table value toward the neighbour's from only one side, or toward the average of two rows. The live version is a plain interpolation between two rows |
| 575–590 | Earlier start and max tables. The oldest ("LEGACY") had hills from 72 to 120 and big mountains from 140 to 250. The one marked "working good" started hills and both mountain levels at 67 and let all three reach 220, so they differed only in their peaks. The last adds 17 to nearly everything. Next to the live table they show hills (max 100) and mountains (max 227) being made into clearly different things |
| 596–598 | `int biomes[]`: a biome per level, plains for the low four and snow for the two mountain levels. The one trace of biome-by-altitude |
| 499 | Force the snow biome everywhere, for testing ice |
| 507 | Force the vegetation value to 1, for testing forests |

**Height**

| Line | What it was |
| --- | --- |
| 652–659 | Choose one height noise or the other with a hard cut at 0.5. Replaced by the blend, which has no seam |
| 967–968 | Return only the first or only the second height noise, to look at each alone |
| 976, 1029 | Near the tavern: weaken rivers and lakes instead of removing them |
| 1193 | Force weirdness to 0: heightmap terrain everywhere, to see the height without the carving |
| 1252–1287 | Other ways to flatten ground under a road: lower the max, raise the weirdness, change the density threshold. The note `todo investigate wierdness` is here. The live code only pulls the peaks toward the middle |
| 1307–1311 | Lakes lowering start and max themselves. Lakes now work through the river value (section 8) |
| 1371–1376 | Fade canyons out toward plains regions as well as toward mountains |
| 1412, 1428 | Remap the swamp value so that only the upper half counts |
| 1465 | Store each column's biome in the chunk. The biome is per chunk now |
| 1497–1498 | Start and max straight from the chunk's level, with no blend |
| 1506–1540 | A second kind of blending: compute the height the column would have at the neighbouring level too, and mix the two heights. It would blend every feature, not only start and max. Its call no longer matches the lambda's parameters, so it cannot simply be switched back on |
| 1542–1546 | Flag border columns from the bordering factor instead of from the chunk's edges |
| 1553 | Beach sand in any plains region, instead of in the blend with water |

**Density and caves**

| Line | What it was |
| --- | --- |
| 1574–1587 | The older density shaping, "squish": measure how far the block is from the column's height, and raise the density to a power above it and to one over that power below it. Terrain was noise squeezed toward a surface. The live version shapes by position inside the level's range and by weirdness instead |
| 1592–1603 | Variations on the live version: height as a fraction of the whole world's 256 instead of the level's range, and a constant exponent |
| 1692–1694 | Also carve a block when the cave noise one or two blocks below says cave: taller cheese caves |
| 1711, 1715 | Square the height factor, and a bias power, for the spaghetti threshold |

**Surface, in `calculateBlockPass1`**

| Line | What it was |
| --- | --- |
| 38 | An extra condition on the canyon bridge: only where the ground would have reached the bridge's height. An attempt to stop bridges floating |
| 152 | Treat the first block that is not air as the surface, instead of the first stone. With it, a sand or clay pocket at the top would be turned into grass |
| 194 | The start of a lake-specific ice rule |

**Vegetation and structures**

| Line | What it was |
| --- | --- |
| 1798–1799 | Extra height and birch logs for the red birch |
| 1908 | Compute the tree chance from the amount over 0 to 1 instead of over threshold to 1 |
| 2031–2171 | The previous vegetation system: each biome carried its own list of vegetation entries (`biome.vegetationNoises`), read against four separate vegetation noises. Its replacement is the shared forest tables of section 13. The matching lines in `biome.cpp` are commented out too, along with a rose patch. This is the system to bring back, in some form, for biomes to have their own plants |
| 2351–2352 | Time one chunk's generation and print it |

In `shared/worldGeneratorSettings.cpp`: other region frequencies (0.024
is marked "original intended scale", regions four times as wide as now),
other lookup frequencies, and below the live region function an older one
that looked two chunks out instead of one, for a wider blend.

---

## 16. Code review findings

What reading and measuring turned up. None of these crash the game; they
are places where the code does something other than what it appears to
intend.

**Things that are not doing what they say**

1. **The pick noise is never sampled.** The array `continentalnessPick` is
   filled from `continentalness2Noise` (line 636), not
   `continentalnessPickNoise`. So the weight that blends the two height
   noises is the second height noise itself, through the pick's power and
   spline. The old noise tool samples the right one.
2. **The spaghetti caves' height bias is always zero.**
   `float height = y / (CHUNK_HEIGHT - 1);` divides two integers, which
   gives 0 for every y below 255. The threshold is 0.5 at every height.
3. **Structure chunks barely depend on the seed, and are not kept apart.**
   `generateFeature` hashes the candidate chunk with a constant 0 where the
   seed should be, and tests its neighbours with a different hash from the
   one that decides whether _they_ are candidates. _Measured:_ with two
   different seeds, 85% of structure chunks are the same chunks; and 35% of
   structure chunks have another within 4 chunks, which is what no spacing
   at all would give.
4. **"Near the centre" is 256 chunks.** `blockDistanceFromCenter` is
   computed from chunk coordinates, then divided by 16 to make
   `chunkDistanceFromCenter`. The extra training camp entry meant for
   within 16 chunks applies within 256.
5. **The density uses the wrong start.** `getHeightForBiome` adjusts a
   local copy of the start level and returns the adjusted range's size, but
   the caller's start stays at the table value. The density's "how far up
   the range am I" then divides a distance from the old start by the new
   range.
6. **A disabled check that is not disabled.** The "don't put trees too
   together" test begins `if (0 && A || B || C)`. `&&` binds tighter than
   `||`, so only `A` is switched off. Inside, it compares an index with
   itself and always skips the tree. The result is that a small share of
   trees is dropped by a test unrelated to their neighbours.
7. **`valueWithoutCanionDropDown` is computed after the drop**, so it does
   not hold what its name says. Only the commented-out bridge condition
   reads it.
8. **Dead branch:** `if (currentHeightLevel == 2)` inside
   `if (currentHeightLevel == 3)`, in the canyon code.

**Settings that do nothing**

9. `randomSand`'s scale, type, frequency and octaves. The sand, gravel and
   clay pockets are sampled from `randomStonesNoise`; `randomSandPatchesNoise`
   is created and configured and never read.
10. `vegetationNoise` and `peaksAndValiesContributionSpline`: saved, loaded,
    unused.
11. `perturbFractalOctaves`, as noted in section 2.
12. `stonePatches` at scale 0.001 (section 11).

**One value doing two jobs**

13. A region's biome value and its vegetation value are the same number
    (`todo change` in the code). Desert is always 0.4 to 0.5 and snow 0.6 to
    0.7, so every snow region has the same tree density, and the lushest and
    barest regions are always plains.

**Edges of the design**

14. **Only start and max are blended between regions.** Rivers, canyons,
    roads, the peaks and hills amounts, the surface block and the biome all
    switch on the chunk line. A river arriving at a hills region turns into
    a tunnel at the border.
15. **The bordering factor is not 0 to 1.** FastNoiseSIMD's "natural"
    distance is a product of two distance measures. _Measured:_ it reaches
    1.3 to 1.4, and passes 1 in 0.02% of columns, where `1 − factor` goes
    negative.
16. **Ghost blocks are not saved.** They live in memory. If the server
    stops after a structure was stamped and before the neighbouring chunk
    was generated, that chunk's part of the structure is lost: a house with
    a side missing, a tree with half its leaves.
17. **Structures are checked for water only inside their own chunk.** A
    footprint wider than the chunk can still reach a river next door.
18. **Under an ice sheet the bed is not placed** where the water is more
    than one block deep: the bed test looks for the top-layer block (ice)
    above the stone and finds water.

**Engineering**

19. **The noise arrays are `static`.** About 1.8 MB of working buffers
    shared by every call. It is safe with one generating thread, and the
    first thing to change before generating on several.
20. **Cost per chunk.** Seven 3D noise sets of 65,536 samples each, most of
    them above the terrain where the answer is always air, plus the region
    query's five allocations. A profiler call is already there, commented
    out, at the top and bottom of `generateChunk`.
21. **The dungeon's layout may differ between platforms.** It uses
    `std::shuffle`, whose result for a given random generator is not
    required to match between standard libraries. The rest of generation
    uses the game's own hashes and `std::minstd_rand`, which give the same
    numbers everywhere.
22. **`sanitize()` covers only some of the noises**, and the settings
    reader's failure mode is to exit the server.
23. **FastNoise2 is linked and included** (`#include "FastNoise/FastNoise.h"`)
    and never called. Either a planned migration or a leftover.
24. `StructuresManager::clear()` frees only some of its lists.

---

## 17. Further improvements

Ordered roughly by how much they give for the work.

**Small fixes** (each a few lines, from section 16): sample the pick noise;
make the cave height a float; put the seed in the structure hash and use one
hash for candidates and neighbours; fix the centre distance; return the
adjusted start; give vegetation its own cell value. Each changes what
existing worlds would generate in new chunks, so do them together and
before worlds matter.

**Tools**

- **A noise viewer in the game** (section 3, option 4): a developer window
  that draws any noise, the region map and a cross-section, with the spline
  editor, and reloads the settings without a restart. Everything else on
  this list is easier with it.
- **The region preview in the create-world menu.** The function exists; its
  call is commented out.
- **A regression test for generation:** generate a fixed set of chunks from
  a fixed seed and compare a hash of their blocks with a stored one. It
  turns "did my refactor change the world?" into a yes or no.

**The world**

- **Ores** (section 12). The largest missing piece for play: the recipes
  are waiting.
- **Biomes with their own plants.** Bring back per-biome vegetation lists:
  cactus and dead bush in desert, spruce in snow, the jungle and palm trees
  that are already modelled, flowers.
- **Biomes by climate.** Two more large noises, temperature and humidity,
  and fill in `determineBiome`. Snow stops bordering desert, and biomes can
  be larger than one region.
- **Blend the surface at region borders:** dither the two regions' surface
  blocks across the blend zone using the blended level and white noise.
- **Rivers that go somewhere** (section 18). Short of that: let river and
  canyon lines continue across level borders by blending their effect as
  start and max are blended.
- **Bridges that fit.** Choose the canyon bridge's height from the rims,
  add rails and supports, or make it a structure.
- **Buildings that sit on the ground.** Flatten or build up a foundation
  under the footprint; choose a flat spot instead of the lowest one.
- **Structure spacing by grid.** Divide the world into squares of N chunks
  and put at most one structure at a hashed position inside each. Spacing
  is guaranteed and it is a few lines.
- **Use what is already made:** tree houses, the pyramid, the igloo, the
  waste land biome.
- **Cave variety:** entrances that reach the surface, more cave lower
  down, water or lava below some depth.
- **Random dirt depth,** as the `todo` asks: one to four blocks from white
  noise, thinner on slopes.
- **Save ghost blocks** with the world.

**Speed**

- Stop the 3D noise at the top of the level's range instead of y = 255.
- Sample 3D noise on a coarse grid (every 4 blocks across, 8 up) and
  interpolate, as Minecraft does. A large saving for a small loss of detail.
- Cache region queries per chunk, or compute them for a block of chunks at
  once.
- Move the buffers off `static` and generate on worker threads.

---

## 18. Further research areas

Topics to study, each with the question it answers here.

**Domain warping.** Before sampling a noise, move the sample position by
another noise. Straight features become swirled, and the blobby look of
plain fractal noise goes away. _Here:_ the cheapest way to make coastlines,
canyons and region borders look less regular.
[Inigo Quilez, Domain warping](https://iquilezles.org/articles/warp/).

**Noise derivatives.** Many noises can return their slope as well as their
value. Slope lets fractal noise damp its fine detail on steep ground, which
looks eroded, and tells you where cliffs are without measuring neighbours.
_Here:_ stone on steep slopes and grass on flat ones.
[Inigo Quilez, more noise](https://iquilezles.org/articles/morenoise/).

**Erosion.** Simulate rain: droplets run downhill, pick up soil and drop
it. It produces gullies, ridges and fans that no noise formula does. It
needs a whole heightmap at once, which fights with generating chunk by
chunk, and that conflict is the interesting part.
[Sebastian Lague, Hydraulic Erosion](https://www.youtube.com/watch?v=eaXk97ujbPQ);
[Nick McDonald, Simple particle-based hydraulic erosion](https://nickmcd.me/2020/04/10/simple-particle-based-hydraulic-erosion/).

**River networks.** Real rivers form trees that drain to the sea. Methods:
flow downhill from every point and add up the flow; or grow rivers upstream
from the coast. Both need knowledge beyond one chunk, usually a coarse map
of the whole region made first. _Here:_ the region graph is that coarse map.
Rivers could be routed from region to region, from mountains to sea, and
then carved along the route.
[Red Blob Games, mapgen4](https://www.redblobgames.com/maps/mapgen4/).

**Multi-noise biomes and spline terrain.** Minecraft 1.18's design: a few
large 2D noises (continentalness, erosion, peaks and valleys) feed splines
that give a height and a "squash" factor, a 3D density is biased by them,
and the biome is looked up from the same noises plus temperature and
humidity. This game has the names and a simpler version of the idea.
Knowing the full design shows which pieces are missing and whether they are
wanted.
[Henrik Kniberg's talk](https://www.youtube.com/watch?v=ob3VwY4JyzE);
[Alan Zucconi's article](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/).

**Placement without clumps.** White noise puts trees in clumps and leaves
gaps. Poisson disk sampling and jittered grids give even spacing with a
natural look, and can be made a pure function of position so chunks still
generate alone. _Here:_ trees, stones, structures.
[Bridson's paper](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf).

**Hashing.** All per-position randomness here comes from small integer
hashes. Which hashes are good, how to test one, and how to derive several
independent numbers from one position.
[Nathan Reed, Hash functions for GPU rendering](https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/);
the survey [Hash Functions for GPU Rendering](https://jcgt.org/published/0009/03/02/).

**Structures from parts.** Beyond the dungeon's grid: pieces that declare
their connection points (Minecraft's jigsaw), and constraint solving, where
each cell's choice limits its neighbours' (wave function collapse). _Here:_
villages, larger ruins, a dungeon with more than one floor.
[Jigsaw structure](https://minecraft.wiki/w/Jigsaw_structure);
[WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse);
[Boris the Brave, Wave function collapse explained](https://www.boristhebrave.com/2020/04/13/wave-function-collapse-explained/).

**Roads with a purpose.** Path-finding between structures over the
heightmap, with a cost for slope and water, gives roads that connect places
and bridges where they must cross. It needs the structures' positions in
advance, which the grid spacing above provides.

**Noise on the GPU.** This project already runs compute shaders. A chunk's
3D noise is a natural compute job, and the same shader could draw the noise
viewer. The cost is getting results back to the server's CPU-side
generator, and keeping CPU and GPU results identical bit for bit, which is
harder than it sounds.
[GPU Gems 3, chapter 1](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu).

**Newer noise libraries.** [FastNoise2](https://github.com/Auburn/FastNoise2),
by the same author, builds noise as a graph of nodes (generators, warps,
blends) that can be designed in its own visual tool and loaded from a
string. It is already vendored here. Part of this game's shaping code could
become data. [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) has a
[web preview](https://auburn.github.io/FastNoiseLite/) for trying noise
types and settings without building anything.

**Distant terrain.** To draw mountains beyond the loaded chunks, the
generator needs a cheap answer to "what is the height here?" without the
full 3D pass. The 2D part of this generator (sections 4 and 6) nearly is
one.

---

## Exercises, smallest first

1. Build `noisePeek` (section 3) and write out `riversNoise`,
   `hillsDropsNoise` and `roadNoise`. Find a place where a road crosses a
   river, go there with `OURCRAFT_SPAWN`, and find the bridge.
2. In a copy of `default.wgenerator`, widen the dip in the rivers' spline.
   Look at the image, then make a new world and compare.
3. Change `noisePeek` to draw a side view of `stone3Dnoise`, and see the
   staircase as bands.
4. Draw the region map: one pixel per chunk, coloured by level. Then colour
   by biome.
5. Uncomment `//localWierdness = 0;` (line 1193) and walk a mountain. Then
   set it to 1.
6. Fix the cave height line (section 16, item 2) and choose a range that
   puts more caves low down.
7. Fix the pick noise (item 1). Before you look: predict what will change.
8. Give vegetation its own value: add a second cellular noise with another
   seed and use its cell value.
9. Add one ore with a 3D noise threshold and a depth limit. Count how much
   you get per chunk, as section 11's pocket numbers were counted.
10. Add the same ore as scattered veins instead, and compare the count and
    the look.
11. Replace `generateFeature` with grid spacing, and measure the nearest
    distance between structures before and after.
12. Add temperature and humidity noises, fill in `determineBiome`, and draw
    the biome map.
13. Put the noise viewer in a developer window.

---

## Resource shelf

### Start here

- Red Blob Games,
  [Making maps with noise functions](https://www.redblobgames.com/maps/terrain-from-noise/).
  The best single introduction: elevation, octaves, redistribution, biomes
  from two noises, tree placement, all interactive.
- Red Blob Games, [Noise functions and map generation](https://www.redblobgames.com/articles/noise/introduction.html):
  what noise is, starting from one dimension.
- [The Book of Shaders](https://thebookofshaders.com/11/), chapters
  [11](https://thebookofshaders.com/11/) (noise),
  [12](https://thebookofshaders.com/12/) (cellular) and
  [13](https://thebookofshaders.com/13/) (fractal).
- Henrik Kniberg,
  [Minecraft terrain generation in a nutshell](https://www.youtube.com/watch?v=CSa5O6knuwI).
  Twenty minutes, by the developer who rebuilt it. The vocabulary this
  code uses.

### How Minecraft does it

- Henrik Kniberg,
  [Reinventing Minecraft world generation](https://www.youtube.com/watch?v=ob3VwY4JyzE),
  the full conference talk.
- Alan Zucconi,
  [The world generation of Minecraft](https://www.alanzucconi.com/2022/06/05/minecraft-world-generation/),
  and the video version,
  [How Minecraft actually works](https://www.youtube.com/watch?v=YyVAaJqYAfE).
- Minecraft Wiki: [World generation](https://minecraft.wiki/w/World_generation),
  [Density function](https://minecraft.wiki/w/Density_function),
  [Biome](https://minecraft.wiki/w/Biome),
  [Cave](https://minecraft.wiki/w/Cave),
  [Ore (feature)](https://minecraft.wiki/w/Ore_(feature)),
  [Structure](https://minecraft.wiki/w/Structure),
  [Jigsaw structure](https://minecraft.wiki/w/Jigsaw_structure).

### Noise in depth

- Inigo Quilez: [fBM](https://iquilezles.org/articles/fbm/),
  [domain warping](https://iquilezles.org/articles/warp/),
  [Voronoi edges](https://iquilezles.org/articles/voronoilines/),
  [noise derivatives](https://iquilezles.org/articles/morenoise/).
- [FastNoiseSIMD](https://github.com/Auburn/FastNoiseSIMD) and its
  [wiki](https://github.com/Auburn/FastNoiseSIMD/wiki);
  [FastNoise2](https://github.com/Auburn/FastNoise2);
  [FastNoiseLite](https://github.com/Auburn/FastNoiseLite) with its
  [web preview](https://auburn.github.io/FastNoiseLite/).
- [GPU Gems 3, chapter 1: Generating complex procedural terrains using the GPU](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-1-generating-complex-procedural-terrains-using-gpu).

### Maps, rivers and erosion

- Red Blob Games:
  [Voronoi maps tutorial](https://www.redblobgames.com/x/2022-voronoi-maps-tutorial/),
  [mapgen4](https://www.redblobgames.com/maps/mapgen4/),
  [Procedural river growing](https://www.redblobgames.com/x/1723-procedural-river-growing/).
- Sebastian Lague,
  [Coding Adventure: Hydraulic Erosion](https://www.youtube.com/watch?v=eaXk97ujbPQ).
- Nick McDonald,
  [Simple particle-based hydraulic erosion](https://nickmcd.me/2020/04/10/simple-particle-based-hydraulic-erosion/).

### Placement and structures

- Robert Bridson,
  [Fast Poisson Disk Sampling in Arbitrary Dimensions](https://www.cs.ubc.ca/~rbridson/docs/bridson-siggraph07-poissondisk.pdf).
- Bob Nystrom,
  [Rooms and Mazes](https://journal.stuffwithstuff.com/2014/12/21/rooms-and-mazes/).
- [WaveFunctionCollapse](https://github.com/mxgmn/WaveFunctionCollapse) and
  Boris the Brave's
  [explanation](https://www.boristhebrave.com/2020/04/13/wave-function-collapse-explained/).
- Nathan Reed,
  [Hash functions for GPU rendering](https://www.reedbeta.com/blog/hash-functions-for-gpu-rendering/).

### Books and collections

- [Procedural Content Generation in Games](https://pcgbook.com/), the
  textbook, free online. Chapters on terrain, dungeons and constraint
  solving.
- javidx9,
  [Procedural Generation: Programming the Universe](https://www.youtube.com/watch?v=ZZY9YE7rZJw):
  generating from a hash of position, the idea underneath everything here.

### The source

- [meemknight/ourCraft](https://github.com/meemknight/ourCraft), the
  project this is a fork of. Its `noiseTool/` folder is the original
  viewer and editor.
