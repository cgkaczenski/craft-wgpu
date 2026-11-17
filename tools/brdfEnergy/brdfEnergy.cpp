// Roadmap L0's energy check: how much of the light that reaches a surface
// each lobe of the game's BRDF sends back, summed over every direction it
// could be seen from. A surface cannot send back more than arrives, so a
// number above 1 means the model makes light.
//
//   cmake --build build --target brdfEnergy && build/brdfEnergy
//
// The functions are lighting.wgsl's, same names, line for line: change one
// there and change it here. To try a new diffuse model, write it below and
// add it to `diffuseModels`.
//
// What is summed. For a light from direction L, a BRDF f and a white
// surface, the share sent back is
//
//     integral over the hemisphere of  f(L, V) * (N.V)  dV
//
// (lighting.wgsl multiplies by N.L and the light's colour afterwards; those
// are the light that arrives, so they are not part of the share.) The
// hemisphere is cut into cells by even steps of the angle from N and of the
// angle around it; a cell that far from N covers sin(angle) times its two
// steps of the sphere. (Even steps of the cosine, cells of equal size,
// would be simpler, but they are too coarse next to N for a sharp highlight
// from a light straight overhead.)

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
	const double PI = 3.14159265358979323846;

	struct Vec3 { double x, y, z; };
	Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
	double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	Vec3 normalize(Vec3 a) { double l = std::sqrt(dot(a, a)); return {a.x / l, a.y / l, a.z / l}; }

	// ---- lighting.wgsl -----------------------------------------------------

	double geometrySchlickGGX(double NdotV, double roughness)
	{
		double k = roughness * roughness / 2.0;
		return NdotV / std::max(NdotV * (1.0 - k) + k, 0.0000001);
	}

	double geometrySmith(Vec3 N, Vec3 V, Vec3 L, double roughness)
	{
		double NdotV = std::max(dot(N, V), 0.0);
		double NdotL = std::max(dot(N, L), 0.0);
		return geometrySchlickGGX(NdotL, roughness) * geometrySchlickGGX(NdotV, roughness);
	}

	double distributionGGX(Vec3 N, Vec3 H, double roughness)
	{
		double a = roughness * roughness;
		double a2 = a * a;
		double NdotH = std::max(dot(N, H), 0.0);
		double denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
		denom = PI * denom * denom;
		return a2 / std::max(denom, 0.0000001);
	}

	// With F = 1: a mirror-coated surface, which should send back everything
	// but what its roughness loses between its own bumps.
	double fSpecular(Vec3 N, Vec3 V, Vec3 L, double roughness)
	{
		Vec3 H = normalize(L + V);
		double dotNVclamped = std::clamp(dot(N, V), 0.0, 0.99);
		double NDF = distributionGGX(N, H, roughness);
		double G = geometrySmith(N, V, L, roughness);
		double denominator = 4.0 * dotNVclamped * std::max(dot(N, L), 0.0);
		return NDF * G / std::max(denominator, 0.001);
	}

	// The game's diffuse, for a white surface.
	double diffuseOrenNayar2(double roughness, Vec3 L, Vec3 V, Vec3 N)
	{
		double a = roughness;
		double A = 1.0 / (PI + (PI / 2.0 - 2.0 / 3.0) * a);
		double B = PI / (PI + (PI / 2.0 - 2.0 / 3.0) * a);
		double s = dot(L, N) - dot(N, L) * dot(N, V);
		double t = 1.0;
		if (s > 0.0) { t = std::max(dot(N, L), dot(N, V)); }
		return A + B * s / t;
	}

	// ---- For comparison ----------------------------------------------------

	double lambert(double, Vec3, Vec3, Vec3) { return 1.0 / PI; }

	// Oren-Nayar as Fujii published it ("A tiny improvement of Oren-Nayar
	// reflectance model"), which the game's is two terms away from: L.V
	// where the game has L.N, and the roughness where it has PI.
	double orenNayarFujii(double roughness, Vec3 L, Vec3 V, Vec3 N)
	{
		double a = roughness;
		double A = 1.0 / (PI + (PI / 2.0 - 2.0 / 3.0) * a);
		double B = a / (PI + (PI / 2.0 - 2.0 / 3.0) * a);
		double s = dot(L, V) - dot(N, L) * dot(N, V);
		double t = 1.0;
		if (s > 0.0) { t = std::max(dot(N, L), dot(N, V)); }
		return A + B * s / t;
	}

	typedef double (*Diffuse)(double roughness, Vec3 L, Vec3 V, Vec3 N);
	struct Model { const char *name; Diffuse f; };
	const Model diffuseModels[] = {
		{"the game's (diffuseOrenNayar2)", diffuseOrenNayar2},
		{"Lambert", lambert},
		{"Oren-Nayar, as Fujii published it", orenNayarFujii},
	};

	// ---- The sum -----------------------------------------------------------

	const Vec3 N = {0, 1, 0};

	// A direction `degrees` up from the surface, toward +x.
	Vec3 elevated(double degrees)
	{
		double e = degrees * PI / 180.0;
		return {std::cos(e), std::sin(e), 0};
	}

	template <class F>
	double shareSentBack(F f, Vec3 L, int steps)
	{
		double sum = 0;
		for (int i = 0; i < steps; i++)
		{
			double fromNormal = (i + 0.5) / steps * (PI / 2.0);
			double cosine = std::cos(fromNormal);
			double sine = std::sin(fromNormal);
			for (int j = 0; j < 2 * steps; j++)
			{
				double around = (j + 0.5) / (2 * steps) * 2.0 * PI;
				Vec3 V = {sine * std::cos(around), cosine, sine * std::sin(around)};
				sum += f(L, V) * cosine * sine;
			}
		}
		return sum * (PI / 2.0 / steps) * (2.0 * PI / (2 * steps));
	}

	// 0.07 is a puddle's, 0.09 the material map's smoothest, 0.11 still water's.
	const double roughnesses[] = {0.07, 0.09, 0.11, 0.3, 0.5, 0.7, 0.9, 0.99};
	const double elevations[] = {90, 60, 45, 30, 15, 5};

	void header(const char *first)
	{
		std::printf("  %-10s", first);
		for (double e : elevations) { std::printf(" %6.0f", e); }
		std::printf("\n");
	}
}

int main()
{
	std::printf("The share of a light sent back, by the light's height above the surface in\n"
		"degrees (across) and the roughness (down). Above 1: more than arrived.\n");

	for (const Model &model : diffuseModels)
	{
		std::printf("\nDiffuse: %s\n", model.name);
		header("roughness");
		for (double roughness : roughnesses)
		{
			std::printf("  %-10.2f", roughness);
			for (double e : elevations)
			{
				Vec3 L = elevated(e);
				std::printf(" %6.3f", shareSentBack([&](Vec3 L, Vec3 V) { return model.f(roughness, L, V, N); }, L, 400));
			}
			std::printf("\n");
		}
	}

	// A sharp highlight needs finer cells: at roughness 0.09 it is under a
	// degree wide. Where the share falls short of 1 at low roughness, it is
	// distributionGGX's floor under its denominator cutting the highlight's
	// peak off; at high roughness, light lost between the surface's bumps
	// (real surfaces get it back after a second bounce; this model doesn't).
	std::printf("\nSpecular (GGX, Smith, the reflectance set to 1)\n");
	header("roughness");
	for (double roughness : roughnesses)
	{
		std::printf("  %-10.2f", roughness);
		for (double e : elevations)
		{
			Vec3 L = elevated(e);
			std::printf(" %6.3f", shareSentBack([&](Vec3 L, Vec3 V) { return fSpecular(N, V, L, roughness); }, L, 3000));
		}
		std::printf("\n");
	}

	// The same surface by where it is seen from: what makes far ground
	// bright or dull.
	std::printf("\nDiffuse brightness against Lambert, sun straight overhead, roughness 0.9, by\n"
		"the eye's height above the surface in degrees:\n");
	const double eyes[] = {90, 60, 45, 30, 15, 5, 1};
	std::printf("  %-34s", "eye");
	for (double e : eyes) { std::printf(" %6.0f", e); }
	std::printf("\n");
	for (const Model &model : diffuseModels)
	{
		std::printf("  %-34s", model.name);
		for (double e : eyes) { std::printf(" %6.2f", model.f(0.9, elevated(90), elevated(e), N) * PI); }
		std::printf("\n");
	}

	// Light runs the same both ways: a real surface is as bright with the
	// light and the eye swapped.
	std::printf("\nSwapping the light and the eye (light 60 degrees up, eye 10 degrees up and a\n"
		"quarter turn round), roughness 0.9: the BRDF each way.\n");
	{
		Vec3 a = elevated(60);
		double e = 10.0 * PI / 180.0;
		Vec3 b = {0, std::sin(e), std::cos(e)};
		for (const Model &model : diffuseModels)
		{
			std::printf("  %-34s %6.3f %6.3f\n", model.name, model.f(0.9, a, b, N), model.f(0.9, b, a, N));
		}
		std::printf("  %-34s %6.3f %6.3f\n", "specular", fSpecular(N, b, a, 0.9), fSpecular(N, a, b, 0.9));
	}
	return 0;
}
