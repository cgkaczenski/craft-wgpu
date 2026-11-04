#pragma once

// On the WebGPU path this header stands in for thirdparty/gl2d's: everything
// that includes <gl2d/gl2d.h> -- the game and glui -- gets wgpu2d under
// gl2d's name. The API it relies on that wgpu2d did not have is in the
// "gl2d compatibility" sections of render/wgpu2d.h (docs/roadmap.md,
// milestone 3).
//
// The OpenGL path never sees this: it links the real gl2d.

#include <render/wgpu2d.h>

// What gl2d.h included, because code that included gl2d.h relies on getting
// these through it (GLuint, stbi_*, std::minstd_rand). glad goes once the null
// GL scaffold does.
#include <glad/glad.h>
#include <random>
#include <stb_image/stb_image.h>
#include <stb_truetype/stb_truetype.h>
#include <vector>

namespace gl2d = wgpu2d;

#define GL2D_DefaultTextureCoords WGPU2D_DefaultTextureCoords
