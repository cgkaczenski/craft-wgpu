# wgpu2d: the 2D drawing library from space-game-wgpu (commit 71d8666),
# vendored here the way gl2d is (docs/roadmap.md, milestone 3). ourCraft's
# copy is free to diverge: device features and a 3D pass are coming.
#
# Needs the `webgpu` target from WebGPU-distribution, so it is added after
# FetchContent.

# The shaders are compiled into the library rather than read from disk, so
# nothing here depends on the game's RESOURCES_PATH. Each becomes a raw string
# literal; R"WGSL( is followed directly by the file so WGSL error line numbers
# match the file's.
function(wgpu2d_embed_wgsl SHADER_FILE VAR_NAME OUT_HEADER)
	set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_FILE}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SRC}")
	file(READ "${SRC}" CONTENTS)
	file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/render/${OUT_HEADER}"
"#pragma once
// Generated from thirdparty/wgpu2d/shaders/${SHADER_FILE} by CMake. Do not edit.
namespace render
{
\tinline const char *${VAR_NAME} = R\"WGSL(${CONTENTS})WGSL\";
}
")
endfunction()

wgpu2d_embed_wgsl(quad.wgsl quadShaderWGSL quadShaderSource.h)
wgpu2d_embed_wgsl(mipmap.wgsl mipmapShaderWGSL mipmapShaderSource.h)
wgpu2d_embed_wgsl(brightpass.wgsl brightpassShaderWGSL brightpassShaderSource.h)
wgpu2d_embed_wgsl(blur.wgsl blurShaderWGSL blurShaderSource.h)

add_library(wgpu2d STATIC
	"${CMAKE_CURRENT_SOURCE_DIR}/src/wgpuContext.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/layerEffect.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/font.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/menu.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/callout.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/webgpuImpl.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/src/gl2dCompat.cpp"
)
set_property(TARGET wgpu2d PROPERTY CXX_STANDARD 17)

target_include_directories(wgpu2d PUBLIC
	"${CMAKE_CURRENT_SOURCE_DIR}/include/"
	"${CMAKE_CURRENT_BINARY_DIR}/generated/")
target_link_libraries(wgpu2d PUBLIC webgpu glm PRIVATE stb_image stb_truetype)

# Code written against gl2d (the game, glui) links this instead of gl2d: its
# gl2d/gl2d.h makes `gl2d` another name for `wgpu2d`.
add_library(gl2dShim INTERFACE)
target_include_directories(gl2dShim INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/gl2dShim/")
target_link_libraries(gl2dShim INTERFACE wgpu2d stb_image stb_truetype)
