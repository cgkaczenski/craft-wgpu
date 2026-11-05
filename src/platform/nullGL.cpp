
#include <glad/glad.h>
#include "nullGL.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{

// GL object names. Code treats 0 as "failed to create", so hand out real ones.
// They start high because GL texture names end up in gl2d::Texture::id, which
// is wgpu2d's id on this path: wgpu2d skips an id it never issued, and its own
// ids count up from 1, so the two never collide.
GLuint nextName = 1u << 24;

void nullGLNewNames(GLsizei n, GLuint *names)
{
	for (GLsizei i = 0; i < n; i++) { names[i] = nextName++; }
}

}

#include "nullGLStubs.inl"

namespace
{

#pragma region overrides
// Stubs for the functions whose answers the game acts on. Everything else
// keeps its generated do-nothing stub.

const GLubyte *APIENTRY getString(GLenum name)
{
	// glad, the game and ImGui print or parse these; never return null.
	if (name == GL_VERSION) { return (const GLubyte *)"4.6 nullGL (WebGPU scaffold)"; }
	return (const GLubyte *)"nullGL";
}

const GLubyte *APIENTRY getStringi(GLenum, GLuint) { return (const GLubyte *)""; }

GLuint APIENTRY createShader(GLenum) { return nextName++; }
GLuint APIENTRY createProgram() { return nextName++; }

// Bindless handles: 0 would read as "no texture". A handle carries its
// texture's name in the low 32 bits, so BlocksLoader::gpuIds keep saying
// which texture each slot shows; wgpuWorld reads them back that way (the
// name of a texture made through gl2d is its wgpu2d id).
GLuint64 APIENTRY getTextureHandle(GLuint texture) { return (GLuint64(1) << 32) | texture; }
GLuint64 APIENTRY getTextureSamplerHandle(GLuint, GLuint) { return nextName++; }

// Every shader compiles and links, so shader.cpp doesn't print errors.
void APIENTRY getShaderiv(GLuint, GLenum pname, GLint *params)
{
	*params = (pname == GL_COMPILE_STATUS) ? GL_TRUE : 0;
}

void APIENTRY getProgramiv(GLuint, GLenum pname, GLint *params)
{
	*params = (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) ? GL_TRUE : 0;
}

GLenum APIENTRY checkFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }
GLenum APIENTRY checkNamedFramebufferStatus(GLuint, GLenum) { return GL_FRAMEBUFFER_COMPLETE; }

// GPU timer and occlusion queries: always finished, result 0, so nothing
// waits on them (profiler.cpp, Renderer's sun flare queries).
void APIENTRY getQueryObjectiv(GLuint, GLenum pname, GLint *params)
{
	*params = (pname == GL_QUERY_RESULT_AVAILABLE) ? 1 : 0;
}

void APIENTRY getQueryObjectuiv(GLuint, GLenum pname, GLuint *params)
{
	*params = (pname == GL_QUERY_RESULT_AVAILABLE) ? 1 : 0;
}

void APIENTRY getQueryObjecti64v(GLuint, GLenum, GLint64 *params) { *params = 0; }
void APIENTRY getQueryObjectui64v(GLuint, GLenum, GLuint64 *params) { *params = 0; }

// Queries return zeros instead of leaving the caller's memory as it was.
// GL_VIEWPORT and GL_COLOR_CLEAR_VALUE fill four values.
void APIENTRY getIntegerv(GLenum pname, GLint *data)
{
	int count = (pname == GL_VIEWPORT) ? 4 : 1;
	for (int i = 0; i < count; i++) { data[i] = 0; }
}

void APIENTRY getFloatv(GLenum pname, GLfloat *data)
{
	int count = (pname == GL_COLOR_CLEAR_VALUE || pname == GL_VIEWPORT) ? 4 : 1;
	for (int i = 0; i < count; i++) { data[i] = 0; }
}

// Mapped buffers point at scratch memory, so writes and reads through them are
// harmless (renderer.cpp reads the auto exposure value this way).
std::vector<unsigned char> mapScratch;

void *scratch(size_t size)
{
	if (mapScratch.size() < size) { mapScratch.resize(size); }
	return mapScratch.data();
}

void *APIENTRY mapBuffer(GLenum, GLenum) { return scratch(1 << 20); }
void *APIENTRY mapBufferRange(GLenum, GLintptr, GLsizeiptr length, GLbitfield) { return scratch(length); }
void *APIENTRY mapNamedBuffer(GLuint, GLenum) { return scratch(1 << 20); }
void *APIENTRY mapNamedBufferRange(GLuint, GLintptr, GLsizeiptr length, GLbitfield) { return scratch(length); }
GLboolean APIENTRY unmapBuffer(GLenum) { return GL_TRUE; }
GLboolean APIENTRY unmapNamedBuffer(GLuint) { return GL_TRUE; }

// Texture sizes. gl2d's Texture::GetSize() asks GL for the width and height of
// the bound texture, and UI layout and atlas math use the answer. Remember the
// size given at upload, per texture, and the 2D texture bound on each unit.
struct Size { GLint w = 0, h = 0; };
std::unordered_map<GLuint, Size> textureSizes;
GLuint bound2D[32] = {};
GLuint activeUnit = 0;

void APIENTRY activeTexture(GLenum unit)
{
	activeUnit = (unit - GL_TEXTURE0) % 32;
}

void APIENTRY bindTexture(GLenum target, GLuint texture)
{
	if (target == GL_TEXTURE_2D) { bound2D[activeUnit] = texture; }
}

void APIENTRY bindTextureUnit(GLuint unit, GLuint texture)
{
	bound2D[unit % 32] = texture;
}

void APIENTRY texImage2D(GLenum target, GLint level, GLint, GLsizei width, GLsizei height,
	GLint, GLenum, GLenum, const void *)
{
	if (target == GL_TEXTURE_2D && level == 0) { textureSizes[bound2D[activeUnit]] = {width, height}; }
}

void APIENTRY texStorage2D(GLenum target, GLsizei, GLenum, GLsizei width, GLsizei height)
{
	if (target == GL_TEXTURE_2D) { textureSizes[bound2D[activeUnit]] = {width, height}; }
}

void APIENTRY textureStorage2D(GLuint texture, GLsizei, GLenum, GLsizei width, GLsizei height)
{
	textureSizes[texture] = {width, height};
}

void APIENTRY getTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint *params)
{
	*params = 0;
	if (target != GL_TEXTURE_2D) { return; }

	auto found = textureSizes.find(bound2D[activeUnit]);
	if (found == textureSizes.end()) { return; }

	if (pname == GL_TEXTURE_WIDTH) { *params = std::max(1, found->second.w >> level); }
	if (pname == GL_TEXTURE_HEIGHT) { *params = std::max(1, found->second.h >> level); }
}

#pragma endregion

}

namespace nullGL
{

void install()
{
	installGeneratedStubs();

	glad_glGetString = getString;
	glad_glGetStringi = getStringi;
	glad_glCreateShader = createShader;
	glad_glCreateProgram = createProgram;
	glad_glGetTextureHandleARB = getTextureHandle;
	glad_glGetTextureSamplerHandleARB = getTextureSamplerHandle;
	glad_glGetShaderiv = getShaderiv;
	glad_glGetProgramiv = getProgramiv;
	glad_glCheckFramebufferStatus = checkFramebufferStatus;
	glad_glCheckNamedFramebufferStatus = checkNamedFramebufferStatus;
	glad_glGetQueryObjectiv = getQueryObjectiv;
	glad_glGetQueryObjectuiv = getQueryObjectuiv;
	glad_glGetQueryObjecti64v = getQueryObjecti64v;
	glad_glGetQueryObjectui64v = getQueryObjectui64v;
	glad_glGetIntegerv = getIntegerv;
	glad_glGetFloatv = getFloatv;
	glad_glMapBuffer = mapBuffer;
	glad_glMapBufferRange = mapBufferRange;
	glad_glMapNamedBuffer = mapNamedBuffer;
	glad_glMapNamedBufferRange = mapNamedBufferRange;
	glad_glUnmapBuffer = unmapBuffer;
	glad_glUnmapNamedBuffer = unmapNamedBuffer;
	glad_glActiveTexture = activeTexture;
	glad_glBindTexture = bindTexture;
	glad_glBindTextureUnit = bindTextureUnit;
	glad_glTexImage2D = texImage2D;
	glad_glTexStorage2D = texStorage2D;
	glad_glTextureStorage2D = textureStorage2D;
	glad_glGetTexLevelParameteriv = getTexLevelParameteriv;

	// What glad would have detected on a real 4.6 driver with bindless.
	GLAD_GL_VERSION_1_0 = GLAD_GL_VERSION_1_1 = GLAD_GL_VERSION_1_2 = GLAD_GL_VERSION_1_3 = 1;
	GLAD_GL_VERSION_1_4 = GLAD_GL_VERSION_1_5 = GLAD_GL_VERSION_2_0 = GLAD_GL_VERSION_2_1 = 1;
	GLAD_GL_VERSION_3_0 = GLAD_GL_VERSION_3_1 = GLAD_GL_VERSION_3_2 = GLAD_GL_VERSION_3_3 = 1;
	GLAD_GL_VERSION_4_0 = GLAD_GL_VERSION_4_1 = GLAD_GL_VERSION_4_2 = GLAD_GL_VERSION_4_3 = 1;
	GLAD_GL_VERSION_4_4 = GLAD_GL_VERSION_4_5 = GLAD_GL_VERSION_4_6 = 1;
	GLAD_GL_ARB_bindless_texture = 1;
}

}

