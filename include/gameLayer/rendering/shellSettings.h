#pragma once
#include <glm/vec3.hpp>

// Shells (roadmap S0; chunkFaces.wgsl's vs_shell): Acerola's shell-texturing
// demo on the grass tops near the camera, with the demo's parameters under
// their own names. They are drawn over the grass blades' cells, so only with
// the blades on.
struct ShellSettings
{
	int count = 0;                     // layers, the block's own top being layer 0; below 2: off
	float length = 0.15f;              // how far the last layer stands off the top, in blocks
	float distanceAttenuation = 1;     // an exponent on a layer's height: where between the top and `length` it lies
	float density = 16;                // strands across a block
	float noiseMin = 0, noiseMax = 1;  // the range of a strand's height, of `length`
	float thickness = 2;               // a strand's width at its root, in strand cells
	float curvature = 1;               // an exponent on the height: how much of the push the lower layers take
	float displacementStrength = 0.1f; // how far the last layer is pushed sideways, in blocks
	glm::vec3 color = {0.22f, 0.5f, 0.12f}; // sRGB
	float occlusionAttenuation = 2;    // an exponent on the height: how dark the roots are
	float occlusionBias = 0.1f;        // added to that, so the roots are not black
	float reach = 13;                  // blocks from the camera
	int view = 0;                      // 1: each pixel coloured by its layer, unlit
};
