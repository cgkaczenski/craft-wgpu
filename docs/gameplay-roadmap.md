# Gameplay roadmap: ideas from the FNV decompilation

Features for ourCraft taken from the systems in
`~/game-dev/fnv_decomp`, the decompilation of FNV (Steam 1.4.0.525).
Each section names the FNV system, what it does there, and where it would go
here. Drafted 2026-11-06, after milestone 19d of the WebGPU port
([`roadmap.md`](roadmap.md)).

## Ground rules

- The useful part is usually the **data model**, not the code. FNV's code is
  Gamebryo, D3D9-era and built around ESM/ESP forms; none of that transfers.
- Multiplayer first: anything with state (time, weather, needs, AI) is owned by
  the server ([`tick.cpp`](../src/gameLayer/multyPlayer/tick.cpp)) and sent
  to clients. Clients only present it.
- One milestone at a time, as in the render port.

## Order

The order follows dependencies: the presentation layer first, because it sits
on the post passes the port just finished, then the condition evaluator that
AI, perks and survival all use, then the systems built on it.

| #     | Milestone                          | Depends on             |
| ----- | ---------------------------------- | ---------------------- |
| G1    | Day/night and weather              |                        |
| G2    | Imagespace modifiers               | G1 (weather uses them) |
| G3    | Screen splatter                    | G2                     |
| G4    | Impact data                        |                        |
| G5    | Acoustic spaces and location music |                        |
| G6    | Condition evaluator                |                        |
| G7    | AI packages and schedules          | G1 (time of day), G6   |
| G8    | AI LOD tiers                       | G7                     |
| G9    | Detection and stealth              | G6, G7                 |
| G10   | Leveled lists                      | G6                     |
| G11   | Survival needs                     | G2, G6                 |
| G12   | Destructible objects               | G4                     |
| G13   | Perks, XP and challenges           | G6                     |
| Later | Distant LOD terrain                |                        |

## Presentation

### G1. Day/night and weather

**FNV:** `fallout_shared/tesweather*`, `sky`, `tesweatherlist`. A weather is a
record with its own sun, ambient and fog colors per time of day, cloud
textures, wind, precipitation and a list of sounds. The sky blends from the
current weather to the next over a transition time, and a region picks the
next weather from a weighted list.

**Here:** [`gamePlayLogic.cpp:1760`](../src/gameLayer/gamePlayLogic.cpp)
has a "weather and time" region with `dayTime` fixed at 0.25 and its advance
commented out.

- Turn the day cycle back on, with the server owning `dayTime` and sending it.
- A `Weather` struct: sun and ambient color, fog color and density, cloud
  cover, precipitation type, sound list, each keyed by time of day.
- A blend between the current and next weather, feeding the uniforms the sky,
  fog, sun and bloom passes already take.
- Per-biome weighted weather lists, chosen in
  [`worldGenerator.cpp`](../src/gameLayer/worldGenerator.cpp)'s biomes.
- Rain and snow particles; covers the "day night and skybox" item in
  [`hardertodos.md`](../hardertodos.md).

**G1a, done (branch `gameplay-g1-weather`):**

- `WorldClock` ([`weather.h`](../include/gameLayer/gameplay/weather.h)):
  world seconds, day length (20 minutes), a stop switch, the weather, the
  next weather and the transition between them. The server owns it, saves it
  as `clock.bin` every 5 seconds and at shutdown, and sends it
  (`headerUpdateWorldClock`) on join, once a second and on any change.
  Clients advance their copy between packets. Worlds without `clock.bin`
  start at 0.1 (morning), clear.
- Four weathers (clear, cloudy, rain, storm) chosen by a Markov chain (no
  storm straight out of clear), each lasting minutes, changing over 30-60 s
  with a smoothstep blend. Over 200 simulated hours: 52% clear, 28% cloudy,
  16% rain, 4% storm.
- Each weather is a `WeatherLook` applied on top of the time of day: sun and
  ambient strength, sky light levels off, the sky toward grey (`sky.wgsl`'s
  `overcastSky`, so the fog greys with it), the sun, moon, god rays and lens
  flare hidden, and GL's `rainStrength` fog term, which was a constant 0, now
  fed from the weather.
- Commands: `/time`, `/time set <0..1 | sunrise | day | noon | sunset |
  night | midnight>`, `/time stop`, `/time start`, `/time length <minutes>`,
  `/weather`, `/weather <clear | cloudy | rain | storm> [seconds]`. The
  client control panel shows the clock and can override the time and the
  weather locally, for looking.
- Checked: copies of `test` with each weather at 0.15 (screenshots), a 30 s
  day running from dusk into night while clear turns to rain, `clock.bin`
  saved and reloaded, and the clock logic in a standalone test.
  Not checked in the game: the commands themselves (the input script cannot
  type chat).

**G1b, done (branch `gameplay-g1-weather`):**

- Rain and snow ([`precipitation.wgsl`](../resources/shaders/webgpu/precipitation.wgsl)):
  one sheet per block column within 16 blocks, turned toward the camera,
  from the top of the column's highest blocking block (solid blocks, water,
  leaves; [`precipitation.h`](../include/gameLayer/gameplay/precipitation.h))
  to 14 blocks above the camera. The drops are procedural lanes on the sheet
  (rain streaks, swaying snow flakes), blended onto the HDR color after the
  transparent faces, depth tested. Storms lean the rain with the wind.
- What falls comes from the biome: `ChunkData::biome` is set by
  `generateChunk`, and by `computeChunkBiome` for chunks loaded from disk
  (which also fixes their vegetation and region values, which were 0 before).
  It travels with the chunk packet. None in the desert, snow in the snow
  biome or above y 160, rain elsewhere. The debug panel shows the biome under
  the player and can force rain or snow (`OURCRAFT_PRECIPITATION=rain|snow`
  for scripted runs).
- Lightning: in a storm the server strikes every 4-18 s, 32-412 blocks from
  a random player (`headerLightningStrike`), so every player sees the same
  strike. Clients flash the sky and the light (two flickers, then a fade) and
  play thunder after the sound's delay (343 blocks a second), close strikes
  with a crack. `/lightning [distance]` and the panel's Strike button make one.
- Sounds ([`weatherSounds.cpp`](../src/gameLayer/weatherSounds.cpp)) are made
  in code: there are no recordings in the game. A 9 s seamless rain loop
  streamed while it rains on the player's column (muffled under a roof or
  water), four distant rumbles and three close cracks.
  `OURCRAFT_EXPORT_WEATHER_SOUNDS=<folder>` writes them as WAV files.
  Recordings can replace them there.
- Checked: rain and forced snow in a copy of `test` (screenshots), biomes
  arriving for loaded chunks (desert, plains, snow and hay land across 1500
  chunks), a server strike in a storm (sky 1.76x brighter for about a
  second), the sounds' envelopes and loop seam from the exported WAVs, and
  the weather logic in a standalone test. Not checked: hearing it in the
  game, rain stopping under a roof in a scene, the Strike button and
  `/lightning`.

**G1c, snow weather and the PSX look (branch `fnv-roadmap`):**

- A fifth weather, snow (`/weather snow`): rarer than rain, mostly out of
  clouds, 4-10 minutes. `WeatherLook::snow` makes snow fall wherever rain
  would (`precipitationForColumn` with the weather; still none in the
  desert), heavier and with bigger flakes, and whitens the tops of blocks
  under the open sky as it comes (`chunkFaces.wgsl`'s `shade`).
- The whiteout ([`world.wgsl`](../resources/shaders/webgpu/world.wgsl)'s
  `applySnowFog`): a PlayStation-style fog by distance alone, toward one
  colour, from 3 blocks to nothing left at 40, on the chunk faces and the
  entities (which had no fog). The sky goes to the same colour, so the
  world closes into it. The colour follows the light: grey-white by day,
  dark blue at night.
- The PSX look, a setting under Screen effects (`psxLook`: off, in snow,
  always; `OURCRAFT_PSX=0|1|2` for scripted runs), after Acerola's Godot
  PS1 shader with its faults fixed:
  - vertices snapped to a 240-line grid (whole cells: the Godot shader
    snaps to half pixels of the full window, so it barely wobbles), left
    alone behind the camera (it divides by w unguarded);
  - textures mapped without perspective (WGSL's `@interpolate(linear)`
    instead of the Godot shader's multiply-and-divide by w) on blocks,
    creatures and items, with the depth prepass reading the same uv so the
    cut-outs still match;
  - the picture drawn on the same grid and reduced to 15-bit colour with
    the PlayStation's 4x4 dither in the tone map pass, after the tone
    mapper and the sRGB curve (the Godot shader reduces before Godot's
    lighting and tone mapping, in linear colour, and without the floor that
    drops the low bits, so it never reduces at all); FXAA is skipped.
  - The first-person hand and the held item keep their textures in
    perspective (they are still snapped).
- Indoors (`Lighting::indoors`, from the sky light where the camera is: 15
  under the open sky, 12 or less a few blocks into a room or a cave, eased
  over time): the whiteout is only on what the open sky lights, so the
  room's walls are clear and the view out of a door or a window is still
  white; the PSX grid goes to three times as fine (a third of the
  wobble), and past halfway in the textures stop warping and the colour
  goes to 18-bit (the low two bits dropped, the dither halved). Snow doesn't fall indoors anyway: each
  column's snow stops at its highest block that stops rain. Leaves and
  glass don't dim sky light, so under a tree or a glass roof is outdoors.
  `OURCRAFT_SPAWN=x,y,z` moves the spawn for scripted runs (with y under
  the ground, the player comes up in the first cave above it).
- Checked: screenshots of a copy of `test` in snow at noon, dusk and night,
  with the look on and off, and clear with it always on; in two caves with
  the indoor handling on and off (the haze on the far walls gone). Not
  checked: the wobble and the warping in motion, creatures and water in
  the fog, the menu entry, a weather change running into snow, a house
  with a door or window, walking from outside in.

Not done, and why:

- Snow that stays: the cover is the weather's, gone when the snow stops;
  no snow layers are placed.
- Per-biome weather weights: the weather is one state for the world, so a
  biome can only change what falls (above), not which weather comes.
- A drawn lightning bolt: only the flash for now.

### G2. Imagespace modifiers

**FNV:** `fallout_shared/imagespacemodifierinstance*`, `tesimagespace*`. Named
presets of post-processing values (tint, saturation, blur, bloom, exposure,
radial blur, double vision) with curves over time. Gameplay starts an instance
(hit, low health, a drug, entering water); instances run, fade out and stack.

**Here:** the WebGPU post chain (tone map, bloom, auto exposure, underwater,
god rays) already reads its settings from uniforms.

- A `PostModifier` preset: target values plus a duration and fade curve.
- A stack of active instances, combined each frame on top of the base settings
  (from G1's weather), written into the post uniforms.
- First uses: damage flash (replace or join `playSoundAndShakeForPlayerTakingDamage`),
  low health vignette, poison tint from `Effects::Poisoned`, entering water.

**G2, done (branch `fnv-roadmap`):**

- [`postModifiers.h`](../include/gameLayer/rendering/postModifiers.h): a
  preset is curves over time (linear keys, like FNV's `TimedFloatKey`) for
  saturation, brightness, contrast, a tint (keeps the brightness), a fade to
  a colour (a flash), coloured screen edges, blur, radial blur and double
  vision. `trigger()` runs one once; `hold()` keeps one on while it is called
  every frame, easing in and out, its curves looping (a pulse). Instances
  combine as FNV's `ImageSpaceManager` does: multipliers multiply, colours
  average by amount, blur takes the largest, double vision adds. At most 24.
- [`post.wgsl`](../resources/shaders/webgpu/post.wgsl)'s tone map pass does
  it: blur, radial blur and the ghost on the HDR read (16 taps on a disc,
  only when one runs), the colours after the tone mapper.
- Five presets: `hit` (joins the hurt sound and camera shake in
  `playSoundAndShakeForPlayerTakingDamage`, so local and server damage both
  flash), `low health` (held below a third of the life in survival, a
  heartbeat in red edges, less colour), `poisoned` (held while
  `Effects::Poisoned` runs: green, a turning ghost), `splash` (a dive after
  a second out of the water: blur and a cold flash; a new one restarts the
  run rather than stacking) and `overcast` (held at G1's overcast: a little
  less colour and contrast under a grey sky).
- Settings > Rendering > Screen effects turns all of it off (saved). The
  client control panel triggers and holds each preset and shows the stack.
  `OURCRAFT_POST_MODIFIER=name[@seconds],...` freezes presets on screen for
  scripted screenshots.
- Checked: each preset frozen in a copy of `test` (screenshots), and the
  stack in a standalone test (a hit fading by 0.6 s, two hits stacking, holds
  easing in and out, the setting, the cap, splashes restarting). Not checked
  in the game: taking real damage, poison from food, diving, and the menu
  toggle.

### G3. Screen splatter

**FNV:** `fallout_shared/screensplatter`, `screencustomsplatter`. Textured
splats drawn over the camera that fade out: blood, dirt, water.

**Here:** one fullscreen overlay pass after tone mapping. Rain drops from G1,
blood on heavy hits, water when surfacing. Triggered like G2 instances.

**G3, done (branch `fnv-roadmap`):**

- There were two already: GL's hit lens dirt (`hitLensDirt.png` on damage,
  kept faintly on at low health) and the water drops coming out of the water
  (`waterLensDirt.png`, full for a second, gone over three). Both were added
  in the bloom pass as lens dirt, times the scene's colour, so they only
  showed over bright pixels and not at all with bloom off. They leave
  `bloom.wgsl` (which keeps its always-on `lensDirt.png`), and the timers in
  `gamePlayLogic.cpp` (`hitLensDirt`, `dropsStrength`, `getHitLensDirt`)
  go with them.
- The splats are G2 channels now (`blood`, `waterDrops`, `rainDrops`, the
  largest instance wins), drawn in
  [`post.wgsl`](../resources/shaders/webgpu/post.wgsl)'s tone map from a
  second bind group with the two textures. The drops are lenses: the water
  drops' slopes bend the picture, with darker rims; the rain's are
  procedural: resting drops (mostly tiny, some larger, not quite round,
  landing and then shrinking as they dry) and a few running ones (one per
  column, down the screen in fits and starts, wandering, leaving droplets
  behind). Each is an out-of-focus ball lens: a third-size flipped, blurred
  view of what is behind it, bending harder at the edge, with a ring dark at
  the top and the light gathered at the bottom. The blood is painted over
  the finished picture, so it shows in the dark.
- Presets: `hit` carries GL's dirt (0.6, gone in 0.83 s) and `low health`
  its 0.3; new `heavy hit` (a quarter of the life in one hit: blood that
  stays 4 s; damage from the server is measured from the life before the
  packet), `surfacing` (GL's timing; diving back in clears it) and
  `rain on lens` (held in rain under open sky, from the same column check
  as the rain sound, a third as much looking down; easing in over 3 s and
  drying over 4).
- All of it follows Settings > Rendering > Screen effects, which the old
  dirt did not.
- Bloom's always-on lens dirt (`lensDirt.png`) was lit by any bright pixel,
  so its specks showed over all the sky. It is lit by the sun now
  (`Renderer::sunGlare`: on screen, toward the middle, not behind blocks or
  clouds, measured by the lens flare and used the next frame), so the dirt
  shows looking toward the sun and the plain sky is clean.
- Checked: each frozen in a copy of `test` (screenshots: the drops and the
  blood frame), and the timings in a standalone test. Not checked in the
  game: real damage, surfacing, rain, and with bloom off.

### G4. Impact data

**FNV:** `fallout_shared/bgsimpactdata`, `bgsdecalmanager`, `decaldata`. A table
keyed by surface material and weapon type gives the decal, particle effect and
sound for a hit.

**Here:** crack decals exist (milestone 10a), and blocks have sounds.

- A material per block type, and an impact table: material × tool or weapon
  → decal, particles, sound.
- Use it for mining hits, weapon hits on blocks, and hits on entities.

**G4, done (branch `fnv-roadmap`):**

- [`impact.h`](../include/gameLayer/gameplay/impact.h): 15 materials (stone,
  metal, glass, ice, wood, plant, leaves, dirt, sand, gravel, clay, snow,
  wool, and flesh and straw for entities), given to blocks in the order
  `getSoundForBlockBreaking` tells them apart, and to entities by type
  (dummies and scarecrows straw, the rest flesh). 7 sources from the item in
  the hand: hand, pickaxe, axe (battle axes too), shovel, blade (swords,
  knives, scythes), blunt (war hammers), pierce (spears).
- The table, material × source × hit or break, is built once from rules:
  the material's own debris (glass shatters into many fast shards, sand and
  snow into many small grains), then the source (the tool a material wants
  is louder and throws more and faster, a hand softer and less; blunt throws
  harder, pierce fewer; a blade cuts more out of plants). A pickaxe or hammer
  on stone adds the crack the game had and never played. The sounds stay
  the blocks' own, scaled by the entry. The crack decal is still the mining
  progress, not an entry.
- [`debris.h`](../include/gameLayer/rendering/debris.h): the pieces, small
  spinning cubes (up to 600) that fall, bounce on solid blocks, slide to a
  stop and shrink away in about a second. They are wgpuWorld's block cubes,
  the dropped items' renderer, which takes a size, a turn, a quarter of the
  block's texture, or a flat colour now (`entities.wgsl`'s `Cube`; the
  dropped items are unchanged), lit where they are.
- Used for every mining tick and the break, for a weapon swung at a block
  (it does not mine, but now it strikes: sound and a few pieces), and for
  hits on entities (blood or straw, in place of the plain hit sound).
- The client control panel's Impacts section shows the table for a material
  and source and strikes in front of the camera. `OURCRAFT_IMPACT=<material>,<source>,<hit|break>[,<block id>]`
  strikes there every 0.4 s for scripted screenshots.
- Checked: stone, dirt and glass breaks, wood, flesh and straw hits in a copy
  of `test` (screenshots). Not checked in the game: the sounds, real mining,
  a weapon on a block, hitting a creature.
- Not done: other players' strikes (there is no packet for them, so only
  your own show), and decals per material (FNV's bullet holes).

### G5. Acoustic spaces and location music

**FNV:** `fallout_shared/bgsacousticspace`, `fallout/audio/impactmixer`,
`mediaset`, `medialocationcontroller`. A region sets ambient loops and a
reverb type (cave, interior, open). Music sets change with location and
combat state.

**Here:** [`audioEngineRaudioBackend.cpp`](../src/gameLayer/audioEngineRaudioBackend.cpp)
and [`soundsTodo.md`](../soundsTodo.md).

- Classify the player's surroundings (open sky, enclosed, underground,
  underwater) from the chunk data, and pick ambience and a reverb or low-pass
  setting from it.
- Music per biome, with a combat set when an enemy has locked on
  (`BasicEnemyBehaviour::playerLockedOn`).

## Foundation

### G6. Condition evaluator

**FNV:** `fallout_shared/tescondition*`, `tesconditionfunctions*`. A condition
is a function (a stat, time of day, distance, has item, in biome…), a
comparison and a value; a list of them joins with AND and OR. Dialogue, AI
packages, perks, quests and effects all use the same evaluator.

**Here:** a small `Condition` list with a fixed set of functions over the
player, an entity and the world (time of day, biome, light level, health,
inventory, `EntityStats` fields). Serializable through
[`ourJson.cpp`](../src/gameLayer/ourJson.cpp) so rules live in data files.
G7, G9, G10, G11 and G13 all use it.

## AI

### G7. AI packages and schedules

**FNV:** `fallout/ai/*package*`, `fallout_shared/package*`,
`packageschedule`. An actor has a list of packages (sandbox, patrol, guard,
follow, escort, flee, eat, sleep, use item). Each has a schedule (time of
day, day of week), a location, a target and conditions. The first package
whose schedule and conditions pass runs.

**Here:** [`basicEnemyBehaviour.h`](../include/gameLayer/gameplay/basicEnemyBehaviour.h)
is a three-state machine (staying, walking randomly, targeting a player), and
[`ai.h`](../include/gameLayer/gameplay/ai.h) has per-animal settings.

- A `Package` interface with the first few: wander, guard a position or
  structure, follow, flee, go home at night.
- A per-entity package list, picked with G6 conditions and G1's time of day.
- Rewrite the zombie, goblin, pig and cat behaviours as package lists. Keep the
  current behaviour as the default package so nothing regresses.

### G8. AI LOD tiers

**FNV:** `fallout/ai/highprocess`, `middlehighprocess`, `middlelowprocess`,
`lowprocess`, `highactorculler`, `processlists`. Actors near the player run
full AI, pathing and animation; farther ones move to tiers that update less
often and simulate coarsely; the farthest only advance their schedule.

**Here:** the server ticks every entity in loaded chunks. Add tiers by
distance to the nearest player: full tick, reduced rate with no pathfinding,
and schedule-only. This keeps tick cost flat as players spread out.

### G9. Detection and stealth

**FNV:** `fallout_shared/combatformulas*`, `aiformulas`. Detection checks
combine the target's noise (movement, sneaking, armor) and visibility (light
level, distance, view cone) against the observer's perception.

**Here:** `EntityStats` already has `stealthSound` and `stealthVisibility`,
and nothing reads them. Feed them, the player's movement and the block light
level from [`lightSystem.cpp`](../src/gameLayer/lightSystem.cpp) into the
unaware-to-aware change in `BasicEnemyBehaviour` (`isUnaware`,
`worriedTimer`). Add a sneak state for the player.

## Systems

### G10. Leveled lists

**FNV:** `fallout_shared/tesleveledlist`, `teslevitem`, `teslevcreature`,
`teslevcharacter`. A list of entries, each with a minimum level, count and
chance; an entry can be another list. "Use all" or "pick one" per list.

**Here:** extend [`lootTables`](../include/gameLayer/gameplay/lootTables.h)
with nested tables, a level per entry (from depth, distance from spawn, or
biome difficulty) and G6 conditions. Use the same thing for mob spawns and
structure chests ([`chestBlock.cpp`](../src/gameLayer/gameplay/blocks/chestBlock.cpp)).

### G11. Survival needs

**FNV:** `fallout_shared/bgshunger`, `bgsdehydration`, `bgssleepdeprevation`,
`bgsradiation`. Each need is a meter with stages; each stage applies effects
until the need is met.

**Here:** [`food.cpp`](../src/gameLayer/gameplay/food.cpp) and
`Effects::Saturated` exist.

- Hunger and thirst meters with stages, server side.
- Stages apply `Effects` (slower regeneration, slower running) and G2
  modifiers at the bad end.
- Behind a gameplay rule ([`gameplayRules.cpp`](../src/gameLayer/gameplay/gameplayRules.cpp))
  so creative and casual worlds skip it.

### G12. Destructible objects

**FNV:** `fallout_shared/bgsdestructibleobjectform`, `bgsdebris`,
`bgsexplosion`. An object has health and a list of damage stages; each stage
can swap the model, spawn debris, explode and disable the object.

**Here:** for multi-block structures, chests, doors and models placed in the
world: health per object, stages with a model or block swap, debris through
[`droppedItem.cpp`](../src/gameLayer/gameplay/droppedItem.cpp), effects
through G4.

### G13. Perks, XP and challenges

**FNV:** `fallout_shared/bgsperk`, `bgsentrypoint*`, `experiencepoints`,
`teschallenge`. A perk modifies named "entry points" (damage, mining speed,
loot chance) under conditions. Challenges count actions and grant XP.

**Here:** XP from kills, mining and crafting; perks as `EntityStats` deltas
with G6 conditions, added through `EntityStats::add`. Challenges as counters
over the same events, shown in the UI.

## Later

### Distant LOD terrain

**FNV:** `fallout_shared/testerrainlodmanager*`, `distant terrain system`.
Low-detail terrain meshes drawn past the loaded cells.

**Here:** a coarse heightmap mesh per region beyond the view distance, drawn
before the chunks. Big, and a render task more than a gameplay one; after the
port settles.

## Not taking

- The renderer (`bsshader`, `gamebryo2.2`): ourCraft's PBR, SSR, HBAO and
  cascaded shadows are already past it.
- Havok, SpeedTree, facegen.
- ESM/ESP form loading and the `TESForm` hierarchy.
- The Pip-Boy, VATS and the menu system: too tied to FNV.
