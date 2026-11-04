// The WebGPU-Cpp wrapper (webgpu.hpp) is header-only but needs exactly one
// translation unit to define WEBGPU_CPP_IMPLEMENTATION so its method bodies
// are compiled once. That is this file's only job. Every other render source
// includes <webgpu/webgpu.hpp> without the define.
#define WEBGPU_CPP_IMPLEMENTATION
#include <webgpu/webgpu.hpp>
#undef WEBGPU_CPP_IMPLEMENTATION
