# Gameplay roadmap: ideas from the FNV decompilation

Features for ourCraft taken from the systems in
`~/game-dev/fnv_decomp`, the decompilation of FNV (Steam 1.4.0.525).
Each section names the FNV system, what it does there, and where it would go
here. Drafted 2026-11-06, after milestone 19d of the WebGPU port
([`roadmap.md`](roadmap.md)).

## Ground rules

- **Use the decomp to read designs. Write the code yourself.** It is rebuilt
  from a commercial executable, and its own README limits it to research,
  modding and preservation. Read how a system is shaped (its data, its states,
  how it is driven), then write that from scratch here. Do not copy functions,
  struct layouts or names.
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

| # | Milestone | Depends on |
|---|---|---|
| G1 | Day/night and weather | |
| G2 | Imagespace modifiers | G1 (weather uses them) |
| G3 | Screen splatter | G2 |
| G4 | Impact data | |
| G5 | Acoustic spaces and location music | |
| G6 | Condition evaluator | |
| G7 | AI packages and schedules | G1 (time of day), G6 |
| G8 | AI LOD tiers | G7 |
| G9 | Detection and stealth | G6, G7 |
| G10 | Leveled lists | G6 |
| G11 | Survival needs | G2, G6 |
| G12 | Destructible objects | G4 |
| G13 | Perks, XP and challenges | G6 |
| Later | Distant LOD terrain | |

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

### G3. Screen splatter

**FNV:** `fallout_shared/screensplatter`, `screencustomsplatter`. Textured
splats drawn over the camera that fade out: blood, dirt, water.

**Here:** one fullscreen overlay pass after tone mapping. Rain drops from G1,
blood on heavy hits, water when surfacing. Triggered like G2 instances.

### G4. Impact data

**FNV:** `fallout_shared/bgsimpactdata`, `bgsdecalmanager`, `decaldata`. A table
keyed by surface material and weapon type gives the decal, particle effect and
sound for a hit.

**Here:** crack decals exist (milestone 10a), and blocks have sounds.

- A material per block type, and an impact table: material × tool or weapon
  → decal, particles, sound.
- Use it for mining hits, weapon hits on blocks, and hits on entities.

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
