// Text (gameplay roadmap U1): a bitmap font, baked once with stb_truetype's
// pack API, drawn as one quad per character through the existing batch.
//
// Nothing here touches WebGPU. The atlas becomes a GPU texture through
// Texture::createFromBufferTopFirst like any other image, and the glyphs are drawn
// with renderRectangle like any other sprite; consecutive glyphs share that
// texture, so a string -- or a whole HUD's worth of strings -- is one draw run.

#include <render/wgpu2d.h>

#include <stb_truetype/stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>

namespace wgpu2d
{

bool Font::createFromFile(const char *ttfFile, float bakePixelHeight, bool pixelated)
{
	cleanup();

	std::ifstream in(ttfFile, std::ios::binary);
	const std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	if (ttf.empty())
	{
		std::cerr << "wgpu2d: error opening font: " << ttfFile << "\n";
		return false;
	}

	stbtt_fontinfo info;
	if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)))
	{
		std::cerr << "wgpu2d: not a font stb_truetype can read: " << ttfFile << "\n";
		return false;
	}

	// The atlas is one channel of coverage. Start small and double until the
	// glyphs fit: ASCII at a pixel font's size wants about 128 x 64, a large
	// smooth font a good deal more. Padding 1 keeps neighbours out of each
	// other's texels, which nearest sampling needs as much as linear does
	// once a scaled quad's edge lands between texels.
	stbtt_packedchar packed[charCount];
	std::vector<unsigned char> coverage;
	int w = 128, h = 64;
	for (;;)
	{
		coverage.assign((size_t)w * h, 0);
		stbtt_pack_context pack;
		if (!stbtt_PackBegin(&pack, coverage.data(), w, h, 0, 1, nullptr))
		{
			std::cerr << "wgpu2d: stbtt_PackBegin failed for " << ttfFile << "\n";
			return false;
		}
		// 1 x 1: no oversampling. Oversampling buys subpixel placement under
		// linear filtering, which is the opposite of what a pixel font wants.
		stbtt_PackSetOversampling(&pack, 1, 1);
		const int fitted = stbtt_PackFontRange(&pack, ttf.data(), 0, bakePixelHeight,
			firstChar, charCount, packed);
		stbtt_PackEnd(&pack);
		if (fitted) { break; }

		if (w >= 4096 && h >= 4096)
		{
			std::cerr << "wgpu2d: " << ttfFile << " at " << bakePixelHeight << " px does not fit a 4096 atlas\n";
			return false;
		}
		if (h < w) { h *= 2; } else { w *= 2; }
	}

	// Coverage into the alpha of white RGBA: the sprite shader multiplies by
	// the vertex colour, so the colour is the tint and the alpha the shape.
	// A single-channel texture would be a quarter of the memory, but the
	// renderer only makes RGBA8 and the atlas is a few kilobytes.
	std::vector<unsigned char> rgba((size_t)w * h * 4);
	for (size_t i = 0; i < coverage.size(); i++)
	{
		rgba[i * 4 + 0] = 255;
		rgba[i * 4 + 1] = 255;
		rgba[i * 4 + 2] = 255;
		rgba[i * 4 + 3] = coverage[i];
	}
	// No mipmaps: text is drawn at its size or larger, and a mip chain would
	// only ever be sampled when it is shrunk -- where a bitmap font is
	// illegible anyway.
	texture.createFromBufferTopFirst((const char *)rgba.data(), w, h, pixelated, false);
	if (texture.id == 0)
	{
		std::cerr << "wgpu2d: could not create the atlas for " << ttfFile << "\n";
		return false;
	}

	// The vertical metrics are in font units; the same scale the pack used
	// for a positive size turns them into pixels.
	const float scale = stbtt_ScaleForPixelHeight(&info, bakePixelHeight);
	int a = 0, d = 0, gap = 0;
	stbtt_GetFontVMetrics(&info, &a, &d, &gap);
	pixelHeight = bakePixelHeight;
	ascent = std::round(a * scale);
	descent = std::round(d * scale);
	lineGap = std::round(gap * scale);

	// stbtt_packedchar holds texel corners and offsets from the pen; turn
	// them into the size and gl2d-convention coordinates renderRectangle
	// takes. Texel rows run top-down, gl2d's v bottom-up, hence 1 - y.
	glyphs.resize(charCount);
	for (int i = 0; i < charCount; i++)
	{
		const stbtt_packedchar &p = packed[i];
		Glyph &g = glyphs[i];
		g.offset = {p.xoff, p.yoff};
		g.size = {p.xoff2 - p.xoff, p.yoff2 - p.yoff};
		g.advance = p.xadvance;
		g.uv = {(float)p.x0 / w, 1.f - (float)p.y0 / h, (float)p.x1 / w, 1.f - (float)p.y1 / h};
	}

	std::cout << "wgpu2d: font " << ttfFile << " at " << bakePixelHeight << " px, atlas "
		<< w << "x" << h << ", ascent " << ascent << " descent " << descent
		<< " line " << lineHeight() << "\n" << std::flush;
	return true;
}

void Font::cleanup()
{
	texture.cleanup();
	glyphs.clear();
	pixelHeight = ascent = descent = lineGap = 0.f;
}

namespace
{
	const Font::Glyph *glyphFor(const Font &font, char c)
	{
		const int i = (unsigned char)c - Font::firstChar;
		if (i < 0 || i >= (int)font.glyphs.size()) { return nullptr; }
		return &font.glyphs[i];
	}

	// Width of the line starting at `text`, up to '\n' or the end, unscaled.
	// A character outside the atlas advances like a space, so a stray byte
	// leaves a gap rather than shifting everything after it.
	float lineWidth(const Font &font, const char *text)
	{
		float width = 0.f;
		for (const char *c = text; *c && *c != '\n'; c++)
		{
			const Font::Glyph *g = glyphFor(font, *c);
			width += g ? g->advance : font.glyphs[0].advance;
		}
		return width;
	}
}

glm::vec2 measureText(const Font &font, const char *text, float scale)
{
	if (!text || font.glyphs.empty()) { return {}; }

	float widest = 0.f;
	int lines = 1;
	for (const char *line = text; ; )
	{
		widest = std::max(widest, lineWidth(font, line));
		const char *end = line;
		while (*end && *end != '\n') { end++; }
		if (!*end) { break; }
		line = end + 1;
		lines++;
	}
	return glm::vec2{widest, lines * font.lineHeight()} * scale;
}

void Renderer2D::renderTextScaled(glm::vec2 position, const char *text, const Font &font,
	const Color4f color, float scale, glm::vec2 anchor)
{
	if (!text || font.glyphs.empty() || font.texture.id == 0) { return; }

	const glm::vec2 box = measureText(font, text, scale);
	const float top = position.y - anchor.y * box.y;
	const float lineStep = font.lineHeight() * scale;

	int lineIndex = 0;
	for (const char *line = text; ; lineIndex++)
	{
		// Each line anchored by its own width -- which lines up the block's
		// left edges, centres or right edges -- then put on whole units.
		const float width = lineWidth(font, line) * scale;
		glm::vec2 pen = {
			std::round(position.x - anchor.x * width),
			std::round(top + lineIndex * lineStep + font.ascent * scale)};

		const char *c = line;
		for (; *c && *c != '\n'; c++)
		{
			const Font::Glyph *g = glyphFor(font, *c);
			if (!g) { pen.x += font.glyphs[0].advance * scale; continue; }
			if (g->size.x > 0.f && g->size.y > 0.f)
			{
				renderRectangle({pen + g->offset * scale, g->size * scale}, font.texture,
					color, {}, 0.f, g->uv);
			}
			pen.x += g->advance * scale;
		}
		if (!*c) { break; }
		line = c + 1;
	}
}

}
