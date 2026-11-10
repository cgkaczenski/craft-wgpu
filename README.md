# ourCraft

It is the third time I have tried to make Minecraft from scratch.
This time I want to add a lot of harder-to-implement features like transparent blocks, light shadows, and multi-player!

Go check out the videos on [YouTube about it](https://www.youtube.com/watch?v=StNAG_tLEoU&list=PLKUl_fMWLdH-0H-tz0S144g5xXliHOIxC&index=4)!

![image](https://github.com/user-attachments/assets/9f97b795-8f7e-4de0-abca-2945338721ca)

![image](https://github.com/user-attachments/assets/08b148c9-4c80-4cbc-83f1-c1ace1e61e0a)

![image](https://github.com/user-attachments/assets/d02a6717-8b47-4923-880d-1bc8e2574943)

![image](https://github.com/meemknight/ourCraft/assets/36445656/7e57cdc4-6f6c-4cc9-bce5-c8ff9131ab55)

![image](https://github.com/meemknight/ourCraft/assets/36445656/fd5ad17e-1bee-441d-8747-d4df4fdb850c)

![image](https://github.com/meemknight/ourCraft/assets/36445656/3f6c8976-8f63-4259-a1de-3305c4c52467)

## Building

The game renders with WebGPU (wgpu-native): Metal on macOS, Vulkan on Linux,
Direct3D 12 or Vulkan on Windows. It is built and played on macOS; the CMake
is kept portable, but Windows and Linux are not tested yet.

You need:

- CMake 3.16 or newer (CMake 4 works)
- A C++17 compiler: Xcode's command line tools on macOS (`xcode-select --install`),
  Visual Studio 2022 on Windows, gcc or clang on Linux
- git, and an internet connection for the first configure: CMake downloads
  GLFW 3.4, glfw3webgpu and prebuilt wgpu-native v24.0.3.1 into `build/_deps/`.
  Everything else is in `thirdparty/`.

From the repository's root folder:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 8
./build/ourCraft
```

Use `Release`: without a build type, CMake builds with no optimisation and the
game runs far slower. The first build compiles all of `thirdparty/` (assimp,
FastNoise, ImGui and others) and takes a few minutes; after that, only the
second command is needed after a change.

The build copies wgpu-native's library (`libwgpu_native.dylib`, `.so` or `.dll`)
next to the executable. The executable reads `resources/` from this folder by
its full path, so it can be started from anywhere, but it has to stay in this
checkout. Settings are saved in `playerSettings/` and worlds in
`resources/worlds/`.

On Windows, `cmake -S . -B build` makes a Visual Studio solution; build it with
`cmake --build build --config Release`, and the game is
`build\Release\ourCraft.exe`. On Linux, GLFW needs the X11 and Wayland
development packages (on Debian or Ubuntu: `libx11-dev libxrandr-dev
libxinerama-dev libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev
wayland-protocols`).

### Photo to PSX texture tool

`tools/photo2psx` turns photos into PlayStation-looking block textures, as a
texture pack. It is a Python script with its own requirements; the top of
`photo2psx.py` explains its use.

```bash
cd tools/photo2psx && python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
```

Features and todos:

- [ ] Rendering system
  - [ ] Shaders:
    - [x] Animated nice water 😻
  - [x] No visual artifacts on textures
  - [x] PBR pipeline
  - [x] Lights
    - [ ] Lights stored in cube maps
  - [ ] Sky Box reflection
  - [x] SSR
  - [x] HBAO / SSAO
  - [x] HDR, ACES tone mapping
  - [x] Bloom
  - [x] Automatic exposure
  - [x] Lens flare
  - [x] Color grading
  - [x] Fog -(todo improve)
  - [x] Underwater fog -(todo improve)
  - [x] God rays
  - [x] Fake Shadows for all light types (todo improve)
    - [x] Shadows (todo optimize)
    - [x] Cascaded shadow maps
  - [ ] Depth of field (maybe blur far stuff)
  - [ ] Use the same shader for all things in game
- [x] Chunk system

- [ ] Multi player
  - [x] Connection to server and handshake
  - [x] Server can validate moves
  - [x] Server knows player position to optimize chunk logic stuff
  - [x] Undo Stuff On client
  - [ ] Buffering
  - [x] Rubber banding
  - [x] Entities
