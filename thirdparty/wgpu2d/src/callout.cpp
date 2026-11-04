// Callouts (hints roadmap H1): a speech bubble pointing at something, with
// key-caps in its text.
//
// All of it is rectangles, two triangles and renderText, so it batches with
// the rest of the HUD. Sizes are in font pixels times the scale, so a pixel
// font's bubble keeps the font's grid: every edge lands on a whole multiple
// of the scale.

#include <render/wgpu2d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace wgpu2d
{

namespace
{
	// Plain fills for renderTriangles, which wants a texture.
	Texture white()
	{
		static Texture t;
		if (t.id == 0) { t.create1PxSquare(); }
		return t;
	}

	enum class Icon { None, MouseLeft, MouseRight, MouseWheel };

	struct Run
	{
		std::string text;
		bool cap = false;
		bool lit = false;
		Icon icon = Icon::None;
		float width = 0.f;
		std::string label;   // words beside a mouse icon, in the same cap
	};

	struct Line
	{
		std::vector<Run> runs;
		float width = 0.f;
	};

	// One font pixel at this scale.
	float unit(const CalloutStyle &s) { return s.scale; }

	float lineHeight(const CalloutStyle &s) { return std::round((s.font->lineHeight() + 4.f) * unit(s)); }
	float capHeight(const CalloutStyle &s) { return std::round((s.font->lineHeight() + 2.f) * unit(s)); }

	std::vector<Line> layout(const char *markup, const CalloutStyle &s)
	{
		std::vector<Line> lines(1);
		const float u = unit(s);
		std::string plain;

		auto flushText = [&]()
		{
			if (plain.empty()) { return; }
			Run r;
			r.text = plain;
			r.width = measureText(*s.font, plain.c_str(), s.scale).x;
			lines.back().runs.push_back(r);
			lines.back().width += r.width;
			plain.clear();
		};

		for (const char *c = markup; c && *c; c++)
		{
			if (*c == '\n') { flushText(); lines.emplace_back(); continue; }
			if (*c == '[')
			{
				const char *close = std::strchr(c, ']');
				if (close)
				{
					flushText();
					Run r;
					r.cap = true;
					std::string inside(c + 1, close);
					if (!inside.empty() && inside[0] == '!') { r.lit = true; inside.erase(0, 1); }
					// A mouse icon, and optionally words after it in the same
					// cap: [mouse:wheel WHEEL].
					const size_t space = inside.find(' ');
					const std::string token = inside.substr(0, space);
					if (token == "mouse:left") { r.icon = Icon::MouseLeft; }
					else if (token == "mouse:right") { r.icon = Icon::MouseRight; }
					else if (token == "mouse:wheel") { r.icon = Icon::MouseWheel; }
					if (r.icon != Icon::None && space != std::string::npos) { r.label = inside.substr(space + 1); }
					r.text = inside;
					// A cap: two font pixels of air either side of its contents,
					// and one more either side of the frame, to the next run.
					float contents = r.icon != Icon::None ? 8.f * u
						: measureText(*s.font, inside.c_str(), s.scale).x;
					if (!r.label.empty()) { contents += 3.f * u + measureText(*s.font, r.label.c_str(), s.scale).x; }
					r.width = contents + 8.f * u;
					lines.back().runs.push_back(r);
					lines.back().width += r.width;
					c = close;
					continue;
				}
			}
			plain += *c;
		}
		flushText();
		return lines;
	}

	glm::vec2 contentSize(const std::vector<Line> &lines, const CalloutStyle &s)
	{
		float widest = 0.f;
		for (const Line &l : lines) { widest = std::max(widest, l.width); }
		return {widest, lines.size() * lineHeight(s)};
	}

	void outlinedRect(Renderer2D &r, glm::vec4 rect, float edge, Color4f frame, Color4f fill)
	{
		r.renderRectangle(rect, frame);
		r.renderRectangle({rect.x + edge, rect.y + edge, rect.z - 2.f * edge, rect.w - 2.f * edge}, fill);
	}

	// A mouse 8 x 12 font pixels: a body, the buttons split from it, and the
	// wheel between them. The part meant is filled in the lit colour; held,
	// the whole body is, and the part stands out in the cap's text colour.
	void drawMouse(Renderer2D &r, glm::vec2 centre, Icon icon, bool lit, const CalloutStyle &s)
	{
		const float u = unit(s);
		const glm::vec4 body = {std::round(centre.x - 4.f * u), std::round(centre.y - 6.f * u), 8.f * u, 12.f * u};
		const Color4f line = s.capText;
		const Color4f mark = lit ? s.capText : s.capLit;
		outlinedRect(r, body, u, line, lit ? s.capLit : s.capFill);

		r.renderRectangle({body.x, body.y + 5.f * u, body.z, u}, line);  // under the buttons
		if (icon == Icon::MouseLeft) { r.renderRectangle({body.x + u, body.y + u, 3.f * u, 4.f * u}, mark); }
		if (icon == Icon::MouseRight) { r.renderRectangle({body.x + 4.f * u, body.y + u, 3.f * u, 4.f * u}, mark); }
		// The split between the buttons, with the wheel in it.
		r.renderRectangle({body.x + 3.f * u, body.y, 2.f * u, 5.f * u}, line);
		r.renderRectangle({body.x + 3.5f * u, body.y + 1.f * u, u, 3.f * u},
			icon == Icon::MouseWheel ? mark : (lit ? s.capLit : s.capFill));
	}

	void drawLines(Renderer2D &r, const std::vector<Line> &lines, glm::vec2 topLeft, float width,
		float anchorX, const CalloutStyle &s, std::vector<glm::vec4> *capRects = nullptr)
	{
		const float u = unit(s);
		const float lh = lineHeight(s);
		const float ch = capHeight(s);
		for (size_t i = 0; i < lines.size(); i++)
		{
			const Line &l = lines[i];
			float x = std::round(topLeft.x + (width - l.width) * anchorX);
			const float mid = std::round(topLeft.y + lh * (i + 0.5f));
			for (const Run &run : l.runs)
			{
				if (!run.cap)
				{
					r.renderTextScaled({x, mid}, run.text.c_str(), *s.font, s.text, s.scale, {0.f, 0.5f});
				}
				else
				{
					const glm::vec4 cap = {x + u, std::round(mid - ch * 0.5f), run.width - 2.f * u, ch};
					if (capRects) { capRects->push_back(cap); }
					if (run.icon != Icon::None && !run.label.empty())
					{
						// Framed like a key, the mouse first and its words after.
						outlinedRect(r, cap, u, s.capFrame, run.lit ? s.capLit : s.capFill);
						drawMouse(r, {cap.x + 7.f * u, mid}, run.icon, run.lit, s);
						r.renderTextScaled({cap.x + 14.f * u, mid}, run.label.c_str(), *s.font,
							run.lit ? Color4f{s.fill.r, s.fill.g, s.fill.b, 1.f} : s.capText, s.scale, {0.f, 0.5f});
					}
					else if (run.icon != Icon::None)
					{
						drawMouse(r, {cap.x + cap.z * 0.5f, mid}, run.icon, run.lit, s);
					}
					else
					{
						outlinedRect(r, cap, u, s.capFrame, run.lit ? s.capLit : s.capFill);
						r.renderTextScaled({cap.x + cap.z * 0.5f, mid}, run.text.c_str(), *s.font,
							run.lit ? Color4f{s.fill.r, s.fill.g, s.fill.b, 1.f} : s.capText, s.scale, {0.5f, 0.5f});
					}
				}
				x += run.width;
			}
		}
	}

	void triangle(Renderer2D &r, glm::vec2 a, glm::vec2 b, glm::vec2 c, Color4f colour)
	{
		const glm::vec2 p[3] = {a, b, c};
		const glm::vec2 uv[3] = {};
		const Color4f col[3] = {colour, colour, colour};
		r.renderTriangles(p, uv, col, 3, white());
	}
}

glm::vec2 measureCallout(const char *markup, const CalloutStyle &style)
{
	if (!style.font || style.font->glyphs.empty()) { return {}; }
	return contentSize(layout(markup, style), style) + glm::vec2(2.f * style.padding * unit(style));
}

glm::vec2 measureMarkup(const char *markup, const CalloutStyle &style)
{
	if (!style.font || style.font->glyphs.empty()) { return {}; }
	return contentSize(layout(markup, style), style);
}

void renderMarkup(Renderer2D &renderer, glm::vec2 position, const char *markup,
	const CalloutStyle &style, glm::vec2 anchor)
{
	if (!style.font || style.font->glyphs.empty()) { return; }
	const std::vector<Line> lines = layout(markup, style);
	const glm::vec2 size = contentSize(lines, style);
	drawLines(renderer, lines, glm::round(position - anchor * size), size.x, anchor.x, style);
}

glm::vec4 drawCallout(Renderer2D &renderer, glm::vec2 target, glm::vec2 offset,
	const char *markup, const CalloutStyle &style, std::vector<glm::vec4> *capRects,
	const char *cornerLeft, const char *cornerRight)
{
	if (capRects) { capRects->clear(); }
	if (!style.font || style.font->glyphs.empty()) { return {}; }

	const float u = unit(style);
	const glm::vec2 screen = {(float)renderer.windowW, (float)renderer.windowH};
	const float margin = style.margin * u;
	const std::vector<Line> lines = layout(markup, style);
	glm::vec2 content = contentSize(lines, style);

	// The corners: smaller markup on a row of their own under the text, one
	// in the bottom-left and one in the bottom-right. The box widens until
	// the two have a gap between them.
	CalloutStyle cornerStyle = style;
	if (style.cornerScale > 0.f) { cornerStyle.scale = style.cornerScale; }
	std::vector<Line> leftLines, rightLines;
	glm::vec2 leftSize = {}, rightSize = {};
	if (cornerLeft && *cornerLeft) { leftLines = layout(cornerLeft, cornerStyle); leftSize = contentSize(leftLines, cornerStyle); }
	if (cornerRight && *cornerRight) { rightLines = layout(cornerRight, cornerStyle); rightSize = contentSize(rightLines, cornerStyle); }
	const float textHeight = content.y;
	const float rowGap = 2.f * u;
	if (!leftLines.empty() || !rightLines.empty())
	{
		content.x = std::max(content.x, leftSize.x + rightSize.x + 8.f * u);
		content.y += rowGap + std::max(leftSize.y, rightSize.y);
	}
	const glm::vec2 size = glm::round(content + glm::vec2(2.f * style.padding * u));

	// `offset` is the gap from the tip to the box's nearest edge, so the box's
	// centre is that, plus how far the box reaches along the same direction.
	// Measured to the centre instead, a wide box would cover its own tip.
	auto placeFrom = [&](glm::vec2 from, glm::vec2 dir, float gap)
	{
		const float reach = std::abs(dir.x) * size.x * 0.5f + std::abs(dir.y) * size.y * 0.5f;
		return from + dir * (gap + reach);
	};
	const float gap = glm::length(offset);
	const glm::vec2 offsetDir = gap > 0.f ? offset / gap : glm::vec2(0.f, -1.f);

	// Off screen, the tip waits at the edge on the line from the screen's
	// centre, and the box comes in from there.
	glm::vec2 tip = target;
	glm::vec2 centre = placeFrom(target, offsetDir, gap);
	const bool offScreen = target.x < 0.f || target.y < 0.f || target.x > screen.x || target.y > screen.y;
	if (offScreen)
	{
		const glm::vec2 mid = screen * 0.5f;
		const glm::vec2 toward = target - mid;
		const float length = glm::length(toward);
		const glm::vec2 dir = length > 0.f ? toward / length : glm::vec2(1.f, 0.f);
		const glm::vec2 half = mid - glm::vec2(margin);
		float reach = 1e9f;
		if (dir.x != 0.f) { reach = std::min(reach, half.x / std::abs(dir.x)); }
		if (dir.y != 0.f) { reach = std::min(reach, half.y / std::abs(dir.y)); }
		tip = mid + dir * reach;
		centre = placeFrom(tip, -dir, gap);
	}

	glm::vec4 box = {centre.x - size.x * 0.5f, centre.y - size.y * 0.5f, size.x, size.y};
	box.x = std::round(std::clamp(box.x, margin, std::max(margin, screen.x - margin - box.z)));
	box.y = std::round(std::clamp(box.y, margin, std::max(margin, screen.y - margin - box.w)));
	const glm::vec2 boxCentre = {box.x + box.z * 0.5f, box.y + box.w * 0.5f};
	const glm::vec2 half = {box.z * 0.5f, box.w * 0.5f};

	// The tail leaves the box where the line from its centre to the tip
	// crosses its edge, slid along that edge to stay clear of the corners.
	const glm::vec2 toTip = tip - boxCentre;
	const bool tipInside = std::abs(toTip.x) <= half.x && std::abs(toTip.y) <= half.y;
	glm::vec2 b1, b2, edgeOut;
	if (!tipInside)
	{
		const float sx = toTip.x != 0.f ? half.x / std::abs(toTip.x) : 1e9f;
		const float sy = toTip.y != 0.f ? half.y / std::abs(toTip.y) : 1e9f;
		const bool sideEdge = sx < sy;
		const glm::vec2 cross = boxCentre + toTip * std::min(sx, sy);
		const glm::vec2 along = sideEdge ? glm::vec2(0.f, 1.f) : glm::vec2(1.f, 0.f);
		edgeOut = sideEdge ? glm::vec2(toTip.x > 0.f ? 1.f : -1.f, 0.f) : glm::vec2(0.f, toTip.y > 0.f ? 1.f : -1.f);
		const float edgeHalf = sideEdge ? half.y : half.x;
		const float hw = std::max(u, std::min(style.tailWidth * 0.5f * u, edgeHalf - 3.f * u));
		const float centreAlong = sideEdge ? boxCentre.y : boxCentre.x;
		float at = sideEdge ? cross.y : cross.x;
		at = std::clamp(at, centreAlong - edgeHalf + hw + 2.f * u, centreAlong + edgeHalf - hw - 2.f * u);
		const glm::vec2 base = sideEdge ? glm::vec2(cross.x, at) : glm::vec2(at, cross.y);
		b1 = base - along * hw;
		b2 = base + along * hw;
	}

	// Frame colour first, the box and the tail; then the fill over both,
	// inset by one font pixel, with the tail's fill reaching into the box so
	// the frame between them is covered and the two read as one shape.
	renderer.renderRectangle(box, style.frame);
	if (!tipInside) { triangle(renderer, b1, b2, tip, style.frame); }
	renderer.renderRectangle({box.x + u, box.y + u, box.z - 2.f * u, box.w - 2.f * u}, style.fill);
	if (!tipInside)
	{
		const glm::vec2 base = (b1 + b2) * 0.5f;
		const glm::vec2 toward = tip - base;
		const float length = glm::length(toward);
		if (length > 3.f * u)
		{
			const glm::vec2 dir = toward / length;
			const glm::vec2 along = glm::normalize(b2 - b1);
			const glm::vec2 i1 = b1 + along * u - edgeOut * 2.f * u;
			const glm::vec2 i2 = b2 - along * u - edgeOut * 2.f * u;
			triangle(renderer, i1, i2, tip - dir * 2.5f * u, style.fill);
		}
	}

	drawLines(renderer, lines, {box.x + style.padding * u, box.y + style.padding * u}, content.x, 0.5f, style, capRects);
	const float rowTop = box.y + style.padding * u + textHeight + rowGap;
	if (!leftLines.empty())
	{
		drawLines(renderer, leftLines, glm::round(glm::vec2(box.x + style.padding * u, rowTop)),
			leftSize.x, 0.f, cornerStyle, capRects);
	}
	if (!rightLines.empty())
	{
		drawLines(renderer, rightLines, glm::round(glm::vec2(box.x + box.z - style.padding * u - rightSize.x, rowTop)),
			rightSize.x, 1.f, cornerStyle, capRects);
	}
	return box;
}

}
