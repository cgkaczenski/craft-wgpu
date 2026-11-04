// Menus (gameplay roadmap U2): an immediate-mode menu over renderText.
//
// Every row is laid out, hit-tested and drawn in the same call, so the
// rectangle the pointer is tested against is the rectangle on screen -- they
// cannot drift apart, because there is only one of them. That is most of
// what immediate mode buys.

#include <render/wgpu2d.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace wgpu2d
{

namespace
{
	bool inside(glm::vec4 rect, glm::vec2 p)
	{
		return p.x >= rect.x && p.x < rect.x + rect.z && p.y >= rect.y && p.y < rect.y + rect.w;
	}

	void text(Menu &m, glm::vec2 at, const char *s, Color4f colour, float scale, glm::vec2 anchor)
	{
		if (m.style.shadow.a > 0.f)
		{
			m.renderer->renderTextScaled(at + glm::vec2(scale), s, *m.style.font,
				{m.style.shadow.r, m.style.shadow.g, m.style.shadow.b, m.style.shadow.a * colour.a}, scale, anchor);
		}
		m.renderer->renderTextScaled(at, s, *m.style.font, colour, scale, anchor);
	}

	// A selectable row's shared part: its rectangle, the selection by
	// hovering, the highlight. Returns the rectangle; `pressed` is confirm
	// while selected or a press on it.
	struct Row
	{
		glm::vec4 rect;
		int index;
		bool selected;
		bool hovered;
		bool pressed;
	};

	Row beginRow(Menu &m)
	{
		const float h = std::round(m.style.font->lineHeight() * m.style.scale * m.style.rowHeight);
		Row r;
		r.rect = {std::round(m.topCentre.x - m.style.width * 0.5f), m.y, m.style.width, h};
		r.index = m.row++;
		r.hovered = m.input.pointerActive && inside(r.rect, m.input.pointer);
		if (r.hovered && (m.input.pointerMoved || m.input.pointerPressed)) { m.selected = r.index; }
		r.selected = m.selected == r.index;
		r.pressed = (r.selected && m.input.confirm) || (r.hovered && m.input.pointerPressed);
		if (r.selected) { m.renderer->renderRectangle(r.rect, m.style.highlight); }
		m.y += h;
		return r;
	}

	bool pointerOver(const MenuInput &in, glm::vec4 rect) { return in.pointerActive && inside(rect, in.pointer); }

	Color4f rowColour(const Menu &m, const Row &r) { return r.selected ? m.style.selected : m.style.text; }

	float pad(const Menu &m) { return std::round(m.style.font->lineHeight() * m.style.scale * m.style.padding); }

	float middle(const Row &r) { return r.rect.y + r.rect.w * 0.5f; }
}

void Menu::begin(Renderer2D &r, const MenuInput &in, glm::vec2 top, const MenuStyle &s)
{
	renderer = &r;
	input = in;
	style = s;
	topCentre = top;
	y = top.y;
	row = 0;

	if (rowCount > 0)
	{
		if (input.up) { selected = (selected - 1 + rowCount) % rowCount; }
		if (input.down) { selected = (selected + 1) % rowCount; }
		selected = std::clamp(selected, 0, rowCount - 1);
	}
	if (!input.pointerHeld) { dragging = -1; }

	// Last frame's height: the rows are not known yet, and a menu's size
	// changes rarely enough that a frame late is never seen.
	if (style.panel.a > 0.f && lastHeight > 0.f)
	{
		const float margin = std::round(style.font->lineHeight() * style.scale);
		renderer->renderRectangle({std::round(top.x - style.width * 0.5f) - margin, top.y - margin,
			style.width + 2.f * margin, lastHeight + 2.f * margin}, style.panel);
	}
}

void Menu::title(const char *s)
{
	// One line of its own size: `rowHeight` is spacing for rows to be picked
	// out of, and at a heading's scale it would leave a hole under it.
	const float h = std::round(style.font->lineHeight() * style.titleScale);
	text(*this, {topCentre.x, y + h * 0.5f}, s, style.title, style.titleScale, {0.5f, 0.5f});
	y += h;
}

void Menu::space(float lines)
{
	y += std::round(style.font->lineHeight() * style.scale * lines);
}

bool Menu::button(const char *label)
{
	const Row r = beginRow(*this);
	text(*this, {topCentre.x, middle(r)}, label, rowColour(*this, r), style.scale, {0.5f, 0.5f});
	return r.pressed;
}

void Menu::inactive(const char *label, const char *detail)
{
	// A row's height and place, but no index: up and down never land on it,
	// and hovering selects nothing.
	const float h = std::round(style.font->lineHeight() * style.scale * style.rowHeight);
	const glm::vec4 rect = {std::round(topCentre.x - style.width * 0.5f), y, style.width, h};
	const float mid = rect.y + h * 0.5f;
	text(*this, {topCentre.x, mid}, label, style.inactive, style.scale, {0.5f, 0.5f});
	if (detail)
	{
		const float p = std::round(style.font->lineHeight() * style.scale * style.padding);
		text(*this, {rect.x + rect.z - p, mid}, detail, style.inactive, style.scale, {1.f, 0.5f});
	}
	y += h;
}

bool Menu::choice(const char *label, int &index, const char *const *options, int count)
{
	const Row r = beginRow(*this);
	const int before = index;
	if (count > 0)
	{
		if (r.selected && input.left) { index--; }
		if (r.selected && input.right) { index++; }
		if (r.pressed) { index++; }
		index = ((index % count) + count) % count;
	}

	const Color4f c = rowColour(*this, r);
	text(*this, {r.rect.x + pad(*this), middle(r)}, label, c, style.scale, {0.f, 0.5f});
	if (count > 0)
	{
		char value[64];
		std::snprintf(value, sizeof(value), r.selected ? "< %s >" : "%s", options[index]);
		text(*this, {r.rect.x + r.rect.z - pad(*this), middle(r)}, value, c, style.scale, {1.f, 0.5f});
	}
	return index != before;
}

bool Menu::toggle(const char *label, bool &value)
{
	static const char *const offOn[] = {"OFF", "ON"};
	int index = value ? 1 : 0;
	const bool changed = choice(label, index, offOn, 2);
	value = index == 1;
	return changed;
}

bool Menu::slider(const char *label, float &value, float min, float max, float step)
{
	const Row r = beginRow(*this);
	const float before = value;

	// The bar: the right 40% of the row inside its padding, a line's
	// thickness at a third of the row's height.
	const float p = pad(*this);
	const float barW = std::round(r.rect.z * 0.4f);
	const float barH = std::round(std::max(style.scale * 3.f, r.rect.w * 0.3f));
	const glm::vec4 bar = {r.rect.x + r.rect.z - p - barW, std::round(middle(r) - barH * 0.5f), barW, barH};

	if (r.selected && input.left) { value -= step; }
	if (r.selected && input.right) { value += step; }
	// A drag starts on the bar, or the row's full height over it -- not on
	// the label, which would jump the value to the bottom.
	const glm::vec4 grab = {bar.x - p, r.rect.y, bar.z + 2.f * p, r.rect.w};
	if (input.pointerActive && input.pointerPressed && inside(grab, input.pointer)) { dragging = r.index; }
	if (dragging == r.index && input.pointerHeld && bar.z > 0.f)
	{
		const float t = std::clamp((input.pointer.x - bar.x) / bar.z, 0.f, 1.f);
		value = min + t * (max - min);
		if (step > 0.f) { value = min + std::round((value - min) / step) * step; }
	}
	value = std::clamp(value, min, max);

	const Color4f c = rowColour(*this, r);
	text(*this, {r.rect.x + p, middle(r)}, label, c, style.scale, {0.f, 0.5f});
	const float t = max > min ? (value - min) / (max - min) : 0.f;
	renderer->renderRectangle(bar, style.track);
	renderer->renderRectangle({bar.x, bar.y, std::round(bar.z * t), bar.w},
		r.selected ? style.fill : Color4f{style.fill.r, style.fill.g, style.fill.b, style.fill.a * 0.7f});
	return value != before;
}

int Menu::fields(const char *label, const char *const *values, int count, const Color4f *colours)
{
	const Row r = beginRow(*this);
	const float p = pad(*this);
	const Color4f c = rowColour(*this, r);
	text(*this, {r.rect.x + p, middle(r)}, label, c, style.scale, {0.f, 0.5f});
	if (count <= 0) { return -1; }

	// The cells take the right 60% of the row, split evenly.
	const float cellsW = std::round(r.rect.z * 0.6f) - p;
	const float cellW = std::floor(cellsW / count);
	const float cellsX = r.rect.x + r.rect.z - p - cellW * count;

	if (r.selected && input.left) { field--; }
	if (r.selected && input.right) { field++; }
	field = std::clamp(field, 0, count - 1);

	int pressedCell = -1;
	for (int i = 0; i < count; i++)
	{
		const glm::vec4 cell = {cellsX + cellW * i, r.rect.y, cellW, r.rect.w};
		const bool over = pointerOver(input, cell);
		if (over && (input.pointerMoved || input.pointerPressed)) { field = i; }
		const bool chosen = r.selected && field == i;
		if (chosen)
		{
			renderer->renderRectangle({cell.x + 1.f, cell.y + 1.f, cell.z - 2.f, cell.w - 2.f}, style.highlight);
			if (input.confirm || (over && input.pointerPressed)) { pressedCell = i; }
		}
		const Color4f tc = colours && colours[i].a > 0.f ? colours[i] : (chosen ? style.selected : style.text);
		text(*this, {cell.x + cell.z * 0.5f, middle(r)}, values[i], tc, style.scale, {0.5f, 0.5f});
	}
	return pressedCell;
}

void Menu::note(const char *s, Color4f colour)
{
	const float h = std::round(style.font->lineHeight() * style.scale * style.rowHeight);
	text(*this, {topCentre.x, y + h * 0.5f}, s, colour, style.scale, {0.5f, 0.5f});
	y += h;
}

void Menu::end()
{
	rowCount = row;
	lastHeight = y - topCentre.y;
	if (rowCount > 0) { selected = std::clamp(selected, 0, rowCount - 1); }
	renderer = nullptr;
}

// ---- GridView (inventory roadmap I2) -------------------------------------

glm::vec4 GridView::bounds() const
{
	return {topLeft.x, topLeft.y, columns * cell + (columns - 1) * gap, rows * cell + (rows - 1) * gap};
}

glm::vec4 GridView::square(glm::ivec2 c) const
{
	return {topLeft.x + c.x * (cell + gap), topLeft.y + c.y * (cell + gap), cell, cell};
}

bool GridView::squareAt(glm::vec2 p, glm::ivec2 &out) const
{
	const glm::vec2 local = p - topLeft;
	if (local.x < 0.f || local.y < 0.f) { return false; }
	const glm::ivec2 c = {(int)(local.x / (cell + gap)), (int)(local.y / (cell + gap))};
	if (c.x >= columns || c.y >= rows) { return false; }
	out = c;
	return true;
}

glm::ivec2 GridView::nearestSquare(glm::vec2 p) const
{
	const glm::vec2 local = (p - topLeft) / (cell + gap);
	return {std::clamp((int)std::floor(local.x), 0, columns - 1), std::clamp((int)std::floor(local.y), 0, rows - 1)};
}

void GridView::draw(Renderer2D &r, Color4f fill, Color4f line, float lineWidth) const
{
	for (int y = 0; y < rows; y++)
	{
		for (int x = 0; x < columns; x++)
		{
			const glm::vec4 s = square({x, y});
			r.renderRectangle({s.x - lineWidth, s.y - lineWidth, s.z + 2.f * lineWidth, s.w + 2.f * lineWidth}, line);
			r.renderRectangle(s, fill);
		}
	}
}

void GridView::fillCells(Renderer2D &r, const glm::ivec2 *cells, size_t count, glm::ivec2 at, Color4f colour) const
{
	auto has = [&](glm::ivec2 c)
	{
		for (size_t i = 0; i < count; i++) { if (cells[i] == c) { return true; } }
		return false;
	};
	for (size_t i = 0; i < count; i++)
	{
		const glm::vec4 s = square(at + cells[i]);
		r.renderRectangle(s, colour);
		// Bridge the gap to a neighbour right and below, and the corner when
		// all four meet, so a piece is one shape rather than tiles.
		const bool right = has(cells[i] + glm::ivec2(1, 0));
		const bool below = has(cells[i] + glm::ivec2(0, 1));
		if (right) { r.renderRectangle({s.x + s.z, s.y, gap, s.w}, colour); }
		if (below) { r.renderRectangle({s.x, s.y + s.w, s.z, gap}, colour); }
		if (right && below && has(cells[i] + glm::ivec2(1, 1))) { r.renderRectangle({s.x + s.z, s.y + s.w, gap, gap}, colour); }
	}
}

}
