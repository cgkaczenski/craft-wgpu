// gl2d compatibility (ourCraft, docs/roadmap.md milestone 3).
//
// ourCraft's game code and glui were written against gl2d. On the WebGPU path
// the gl2d/gl2d.h shim makes `gl2d` an alias for `wgpu2d`, and this file fills
// in what gl2d had and wgpu2d does not: gl2d's font baking and text layout,
// its 9-patch, its error hook and texture readback.
//
// The text and 9-patch code is ported from gl2d.cpp, layout quirks included,
// so menus measure and line up as they did on OpenGL. Everything still ends
// in renderRectangle, so it batches like any other sprite.

#include <render/wgpu2d.h>

#include <stb_image/stb_image.h>
#include <stb_truetype/stb_truetype.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace wgpu2d
{

#pragma region error hook

void defaultErrorFunc(const char *msg, void *)
{
	std::cerr << "gl2d error: " << msg << "\n";
}

static errorFuncType *errorFunc = defaultErrorFunc;

errorFuncType *setErrorFuncCallback(errorFuncType *newFunc)
{
	errorFuncType *old = errorFunc;
	errorFunc = newFunc ? newFunc : defaultErrorFunc;
	return old;
}

#pragma endregion

#pragma region font

void Font::createFromFile(const char *ttfFile, bool monospacedFont)
{
	cleanup();

	std::ifstream in(ttfFile, std::ios::binary);
	const std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (ttf.empty())
	{
		std::string message = std::string("error openning: ") + ttfFile;
		errorFunc(message.c_str(), nullptr);
		return;
	}

	// gl2d's bake: 64 px, a 2000x2000 atlas, padding 2, 2x2 oversampling.
	const int bakeSize = 64;
	const int atlasSize = 2000;

	stbtt_fontinfo info;
	stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0));

	int fontAscent = 0, fontDescent = 0, fontLineGap = 0;
	stbtt_GetFontVMetrics(&info, &fontAscent, &fontDescent, &fontLineGap);
	float scale = stbtt_ScaleForPixelHeight(&info, (float)bakeSize);

	std::vector<unsigned char> coverage((size_t)atlasSize * atlasSize, 0);
	stbtt_packedchar packed[charCount] = {};

	stbtt_pack_context pack;
	stbtt_PackBegin(&pack, coverage.data(), atlasSize, atlasSize, 0, 2, nullptr);
	stbtt_PackSetOversampling(&pack, 2, 2);
	// gl2d packed '~' - ' ' characters, one short of '~'; all 95 here.
	stbtt_PackFontRange(&pack, ttf.data(), 0, (float)bakeSize, firstChar, charCount, packed);
	stbtt_PackEnd(&pack);

	// gl2d's atlas puts coverage in the colour and makes alpha all or nothing,
	// so edges antialias by darkening rather than by transparency. Kept as it
	// was so text looks the same on both paths.
	std::vector<unsigned char> rgba(coverage.size() * 4);
	for (size_t i = 0; i < coverage.size(); i++)
	{
		rgba[i * 4 + 0] = coverage[i];
		rgba[i * 4 + 1] = coverage[i];
		rgba[i * 4 + 2] = coverage[i];
		rgba[i * 4 + 3] = coverage[i] > 1 ? 255 : 0;
	}
	texture.createFromBuffer((const char *)rgba.data(), atlasSize, atlasSize, false, false);

	// Each glyph's quad with the pen at (0, 0), as gl2d's fontGetGlyphQuad
	// asked stb for it. stb's t runs down the atlas; the atlas rows go up top
	// first, so gl2d's bottom-up v is 1 - t.
	glyphs.assign(charCount, Glyph{});
	for (int i = 0; i < charCount; i++)
	{
		float x = 0, y = 0;
		stbtt_aligned_quad q = {};
		stbtt_GetPackedQuad(packed, atlasSize, atlasSize, i, &x, &y, &q, 1);

		Glyph &g = glyphs[i];
		g.offset = {q.x0, q.y0};
		g.size = {q.x1 - q.x0, q.y1 - q.y0};
		g.advance = packed[i].xadvance;
		g.uv = {q.s0, 1.f - q.t0, q.s1, 1.f - q.t1};
	}

	pixelHeight = (float)bakeSize;
	ascent = fontAscent * scale;
	descent = fontDescent * scale;
	lineGap = fontLineGap * scale;

	monospaced = monospacedFont;
	max_height = (fontAscent - fontDescent + fontLineGap) * scale;
	spaceSize = monospaced ? max_height : packed[0].xadvance;
}

#pragma endregion

#pragma region text

static const Font::Glyph *glyphFor(const Font &font, char c)
{
	int i = c - Font::firstChar;
	if (i < 0 || i >= (int)font.glyphs.size()) { return nullptr; }
	return &font.glyphs[i];
}

glm::vec2 Renderer2D::getTextSize(const char *text, const Font &font,
	const float sizePixels, const float spacing, const float line_space)
{
	if (font.texture.id == 0)
	{
		errorFunc("Missing font", nullptr);
		return {};
	}

	float size = sizePixels / 64.f;

	const int text_length = (int)strlen(text);
	if (text_length == 0) { return {}; }

	Rect rectangle = {};
	float linePositionY = 0;
	float maxPos = 0;
	float maxPosY = 0;
	int lineCount = 1;

	for (int i = 0; i < text_length; i++)
	{
		if (text[i] == '\n')
		{
			rectangle.x = 0;
			linePositionY += (font.max_height + line_space) * size;
			maxPosY = 0;
			lineCount++;
		}
		else if (text[i] == '\t')
		{
			rectangle.x += (font.max_height + spacing) * size * 3;
		}
		else if (text[i] == ' ')
		{
			rectangle.x += (font.spaceSize + spacing) * size;
		}
		else if (const Font::Glyph *g = glyphFor(font, text[i]))
		{
			rectangle.z = g->size.x * size;
			rectangle.w = g->size.y * size;
			rectangle.y = linePositionY + g->offset.y * size;

			if (font.monospaced)
			{
				rectangle.x += font.max_height + spacing * size;
			}
			else
			{
				rectangle.x += rectangle.z + spacing * size;
			}

			maxPosY = std::max(maxPosY, rectangle.y);
			maxPos = std::max(maxPos, rectangle.x);
		}
	}

	maxPos = std::max(maxPos, rectangle.x);

	float paddY = (lineCount * font.max_height + (lineCount - 1) * line_space) * size;
	return glm::vec2{maxPos, paddY};
}

void Renderer2D::renderText(glm::vec2 position, const char *text, const Font &font,
	const Color4f color, const float sizePixels, const float spacing, const float line_space,
	bool showInCenter, const Color4f ShadowColor, const Color4f LightColor)
{
	float size = sizePixels / 64.f;

	if (font.texture.id == 0)
	{
		errorFunc("Missing font", nullptr);
		return;
	}

	if (showInCenter)
	{
		glm::vec2 textSize = getTextSize(text, font, sizePixels, spacing, line_space);

		position.x -= textSize.x / 2.f;
		position.y -= textSize.y / 2.f;
		position.y += (font.max_height * size / 2.f) * 1.5f;
	}

	const int text_length = (int)strlen(text);
	Rect rectangle = {};
	rectangle.x = position.x;
	float linePositionY = position.y;

	for (int i = 0; i < text_length; i++)
	{
		if (text[i] == '\n')
		{
			rectangle.x = position.x;
			linePositionY += (font.max_height + line_space) * size;
		}
		else if (text[i] == '\t')
		{
			rectangle.x += (font.max_height + spacing) * size * 3;
		}
		else if (text[i] == ' ')
		{
			rectangle.x += (font.spaceSize + spacing) * size;
		}
		else if (const Font::Glyph *g = glyphFor(font, text[i]))
		{
			rectangle.z = g->size.x * size;
			rectangle.w = g->size.y * size;
			rectangle.y = linePositionY + g->offset.y * size;

			if (ShadowColor.w)
			{
				glm::vec2 offset = glm::vec2{-5, 3} * size;
				renderRectangle({rectangle.x + offset.x, rectangle.y + offset.y, rectangle.z, rectangle.w},
					font.texture, ShadowColor, glm::vec2{0, 0}, 0, g->uv);
			}

			renderRectangle(rectangle, font.texture, color, glm::vec2{0, 0}, 0, g->uv);

			if (LightColor.w)
			{
				glm::vec2 offset = glm::vec2{-2, 1} * size;
				renderRectangle({rectangle.x + offset.x, rectangle.y + offset.y, rectangle.z, rectangle.w},
					font.texture, LightColor, glm::vec2{0, 0}, 0, g->uv);
			}

			if (font.monospaced)
			{
				rectangle.x += font.max_height + spacing * size;
			}
			else
			{
				rectangle.x += rectangle.z + spacing * size;
			}
		}
	}
}

#pragma endregion

#pragma region 9 patch

// The nine pieces of a 9-patch, given the border widths in the rectangle and
// the outer and inner texture rectangles (gl2d's coordinate convention).
static void renderNinePieces(Renderer2D &r, const Rect position, const Color4f color,
	const Texture texture, const glm::vec4 tex, const glm::vec4 inner,
	float topBorder, float bottomBorder, float leftBorder, float rightBorder)
{
	auto piece = [&](Rect at, glm::vec4 uv) { r.renderRectangle(at, texture, color, glm::vec2{0, 0}, 0, uv); };

	float innerW = position.z - leftBorder - rightBorder;
	float innerH = position.w - topBorder - bottomBorder;
	float rightX = position.x + position.z - rightBorder;
	float bottomY = position.y + position.w - bottomBorder;

	piece({position.x + leftBorder, position.y + topBorder, innerW, innerH}, inner);

	piece({position.x + leftBorder, position.y, innerW, topBorder}, {inner.x, tex.y, inner.z, inner.y});
	piece({position.x + leftBorder, bottomY, innerW, bottomBorder}, {inner.x, inner.w, inner.z, tex.w});
	piece({position.x, position.y + topBorder, leftBorder, innerH}, {tex.x, inner.y, inner.x, inner.w});
	piece({rightX, position.y + topBorder, rightBorder, innerH}, {inner.z, inner.y, tex.z, inner.w});

	piece({position.x, position.y, leftBorder, topBorder}, {tex.x, tex.y, inner.x, inner.y});
	piece({rightX, position.y, rightBorder, topBorder}, {inner.z, tex.y, tex.z, inner.y});
	piece({position.x, bottomY, leftBorder, bottomBorder}, {tex.x, inner.w, inner.x, tex.w});
	piece({rightX, bottomY, rightBorder, bottomBorder}, {inner.z, inner.w, tex.z, tex.w});
}

// gl2d accepted origin and rotation here and ignored both; so does this.
void Renderer2D::render9Patch(const Rect position, const int borderSize, const Color4f color,
	const glm::vec2, const float, const Texture texture,
	const glm::vec4 textureCoords, const glm::vec4 inner_texture_coords)
{
	float b = (float)borderSize;
	renderNinePieces(*this, position, color, texture, textureCoords, inner_texture_coords, b, b, b, b);
}

void Renderer2D::render9Patch2(const Rect position, const Color4f color, const glm::vec2,
	const float, const Texture texture, const glm::vec4 textureCoords,
	const glm::vec4 inner_texture_coords)
{
	float textureSpaceW = textureCoords.z - textureCoords.x;
	float textureSpaceH = textureCoords.y - textureCoords.w;

	float topBorder = (textureCoords.y - inner_texture_coords.y) / textureSpaceH * position.w;
	float bottomBorder = (inner_texture_coords.w - textureCoords.w) / textureSpaceH * position.w;
	float leftBorder = (inner_texture_coords.x - textureCoords.x) / textureSpaceW * position.z;
	float rightBorder = (textureCoords.z - inner_texture_coords.z) / textureSpaceW * position.z;

	float newAspectRatio = position.z / position.w;
	if (newAspectRatio < 1.f)
	{
		topBorder *= newAspectRatio;
		bottomBorder *= newAspectRatio;
	}
	else
	{
		leftBorder /= newAspectRatio;
		rightBorder /= newAspectRatio;
	}

	renderNinePieces(*this, position, color, texture, textureCoords, inner_texture_coords,
		topBorder, bottomBorder, leftBorder, rightBorder);
}

#pragma endregion

#pragma region camera

void Camera::follow(glm::vec2 pos, float speed, float min, float max, float w, float h)
{
	pos.x -= w / 2.f;
	pos.y -= h / 2.f;

	glm::vec2 delta = pos - position;
	float len = glm::length(delta);
	if (len <= min) { return; }

	delta = glm::normalize(delta);

	if (len < min * 2) { speed /= 4.f; }
	else if (len < min * 4) { speed /= 2.f; }

	if (len > max)
	{
		position = pos - (max * delta);
	}
	else
	{
		position += delta * speed;
	}
}

#pragma endregion

#pragma region texture

void Texture::createFromFileData(const unsigned char *image_file_data, const size_t image_file_size,
	bool pixelated, bool useMipMaps)
{
	// Rows top first, as createFromBuffer wants. gl2d flipped on load for
	// OpenGL's bottom-up rows; wgpu2d's coordinate conversion makes that
	// unnecessary.
	stbi_set_flip_vertically_on_load(false);

	int width = 0, height = 0, channels = 0;
	unsigned char *pixels = stbi_load_from_memory(image_file_data, (int)image_file_size,
		&width, &height, &channels, 4);
	if (!pixels)
	{
		errorFunc("error decoding image from memory", nullptr);
		return;
	}

	createFromBuffer((const char *)pixels, width, height, pixelated, useMipMaps);
	stbi_image_free(pixels);
}

#pragma endregion

#pragma region texture readback

std::vector<unsigned char> Texture::readTextureData(int mipLevel, glm::ivec2 *outSize)
{
	glm::ivec2 size = GetSize();
	size.x = std::max(1, size.x >> mipLevel);
	size.y = std::max(1, size.y >> mipLevel);
	if (id == 0) { size = {}; }

	if (outSize) { *outSize = size; }
	return std::vector<unsigned char>((size_t)size.x * size.y * 4, 0);
}

#pragma endregion

}
