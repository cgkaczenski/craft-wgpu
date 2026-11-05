// macOS only (added to the build by CMakeLists.txt on APPLE).

#include "wgpuWindow.h"

#if defined(__APPLE__)

#define GLFW_EXPOSE_NATIVE_COCOA // GLFW_INCLUDE_NONE comes from CMake
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

namespace wgpuWindow
{

// A CAMetalLayer with no colorspace (the default) shows its pixels in the
// display's own colorspace. On a P3 display that oversaturates colors that
// were authored as sRGB. Pinning the layer to sRGB makes macOS convert them,
// which is what the OpenGL path got for free.
void pinMetalLayerToSRGB(GLFWwindow *window)
{
	NSWindow *nsWindow = glfwGetCocoaWindow(window);
	if (!nsWindow) { return; }

	CALayer *layer = nsWindow.contentView.layer;
	if (![layer isKindOfClass:[CAMetalLayer class]]) { return; }

	CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	((CAMetalLayer *)layer).colorspace = srgb;
	CGColorSpaceRelease(srgb);
}

}

#endif
