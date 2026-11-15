#include <rendering/waterWaves.h>
#include <rendering/wgpuWorld.h>
#include <glm/glm.hpp>
#include <chrono>
#include <cmath>

namespace waterWaves
{

namespace
{
	// chunkFaces.wgsl's tables, the waves that move the surface's corners
	// (its first waveVertexCount): each wave's vector (radians a block), its
	// number (the vector's length), its rate (radians a second) and its
	// amplitude (blocks). Keep them the same there and here.
	constexpr int count = 4;
	const glm::dvec2 waveVector[count] = {
		{0.2208932, 0.1043107}, {0.1902136, 0.3190680}, {0.5706409, -0.0184078}, {0.1533981, 0.8590292}};
	const double waveNumber[count] = {0.244284, 0.371464, 0.570938, 0.872618};
	const double waveRate[count] = {1.1596895, 1.4296701, 1.7732818, 2.1966605};
	const double waveAmplitude[count] = {0.049123, 0.050000, 0.050000, 0.050000};
	constexpr double waveMean = 0.4658;
	constexpr double waveDrag = 1.6;
	constexpr double waveTile = 1024.0;

	// The shader's waveStrength.
	double strength(float wind)
	{
		return glm::mix(0.85, 1.9, glm::clamp((wind - 0.35) / 0.65, 0.0, 1.0));
	}

	// The shader's waterWaves at one point (a corner of a block).
	double heightAtPoint(glm::dvec2 p, double time, double strength)
	{
		glm::dvec2 q = p - waveTile * glm::floor(p / waveTile);
		double height = 0;
		for (int i = 0; i < count; i++)
		{
			const double amplitude = waveAmplitude[i] * strength;
			const double x = glm::dot(waveVector[i], q) - waveRate[i] * time;
			const double crest = std::exp(std::sin(x) - 1.0);
			const double change = amplitude * crest * std::cos(x);
			height += amplitude * (crest - waveMean);
			q -= waveVector[i] / waveNumber[i] * change * waveDrag;
		}
		return height;
	}
}

float time()
{
	static const auto start = std::chrono::steady_clock::now();
	const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	return (float)std::fmod(seconds, wgpuWorld::waveTimePeriod);
}

float height(double x, double z, float time, float wind)
{
	// The block the point is over (a block's position is its middle), and
	// how far across it, 0..1.
	const double blockX = std::floor(x + 0.5), blockZ = std::floor(z + 0.5);
	const double fx = x - (blockX - 0.5), fz = z - (blockZ - 0.5);
	const double s = strength(wind);
	const double h00 = heightAtPoint({blockX - 0.5, blockZ - 0.5}, time, s);
	const double h10 = heightAtPoint({blockX + 0.5, blockZ - 0.5}, time, s);
	const double h01 = heightAtPoint({blockX - 0.5, blockZ + 0.5}, time, s);
	const double h11 = heightAtPoint({blockX + 0.5, blockZ + 0.5}, time, s);
	return (float)glm::mix(glm::mix(h00, h10, fx), glm::mix(h01, h11, fx), fz);
}

}
