#include <render/wgpu2d.h>

namespace wgpu2d
{

void LayerEffect::flush(Renderer2D &renderer, int width, int height,
	const LayerTransform &transform)
{
	// No window, or nothing to transform: straight to the screen. Both cases
	// still have to flush, because the caller has recorded quads either way.
	if (width <= 0 || height <= 0 || transform.isIdentity())
	{
		renderer.flush();
		return;
	}

	if (target.fbo == 0)
	{
		target.create((unsigned)width, (unsigned)height);
		if (target.fbo == 0) { renderer.flush(); return; } // creation failed; draw normally
	}
	target.resize((unsigned)width, (unsigned)height);

	// Last frame's contents must not linger: the target is transparent
	// everywhere this layer does not cover, and that is what lets whatever was
	// drawn earlier show through it.
	target.clear();
	renderer.flushFBO(target);

	// One quad the size of the screen, sampling the composed layer. The
	// rotation is about the quad's centre, which is the screen's centre.
	//
	// Under the default camera, not the caller's: the caller may be sitting in
	// a world camera that is offset and zoomed, and the target has to land on
	// the screen 1:1 the way its contents were laid out.
	//
	// Premultiplied, not Alpha: the target was cleared transparent and drawn
	// into, so every pixel it holds is already scaled by its own coverage.
	// Alpha would scale by coverage a second time and the layer would come out
	// dark -- and only while the effect is active, which is the worst kind of
	// bug to notice. See outline 12.
	const BlendMode previousBlend = renderer.currentBlendMode;
	renderer.setBlendMode(BlendMode::Premultiplied);
	renderer.pushCamera();
	const Rect quad = {
		transform.offsetPixels.x, transform.offsetPixels.y, (float)width, (float)height };
	renderer.renderRectangle(quad, target.texture, Colors_White, {}, transform.rotationDegrees);
	renderer.popCamera();
	renderer.setBlendMode(previousBlend);
}

void LayerEffect::cleanup()
{
	target.cleanup();
}

}
