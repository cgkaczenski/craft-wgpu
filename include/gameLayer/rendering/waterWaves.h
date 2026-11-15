#pragma once

// The water's waves on the CPU: the same sum of waves chunkFaces.wgsl moves
// the water's surface by (its waterWaves and animatedCorner), so that what
// floats can be drawn where the surface is drawn. A sum of waves can simply
// be worked out again here; nothing is read back from the GPU.
//
// For drawing only. The physics floats things on the still surface
// (physics.h's applyBuoyancy): it runs on the server too, which has neither
// this clock nor the weather's wind.
namespace waterWaves
{
	// The waves' clock: seconds since the game started, wrapped every 1024
	// (every wave's rate is a whole number of turns in that time). Read it
	// once a frame and use it for both the water and what floats on it.
	float time();

	// How far the drawn surface is above the still one at a place, in
	// blocks: the long waves at the four corners of the water's block there,
	// and between them as the block's face is drawn. `wind` is the
	// renderer's (0.35 in clear weather, 1 in a storm): it raises the waves.
	float height(double x, double z, float time, float wind);
}
