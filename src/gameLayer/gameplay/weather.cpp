#include <gameplay/weather.h>
#include <algorithm>
#include <cmath>

const char *getWeatherName(int weather)
{
	switch (weather)
	{
	case weatherClear: return "clear";
	case weatherCloudy: return "cloudy";
	case weatherRain: return "rain";
	case weatherStorm: return "storm";
	case weatherSnow: return "snow";
	}
	return "unknown";
}

WeatherLook getWeatherLook(int weather)
{
	WeatherLook l;
	switch (weather)
	{
	case weatherCloudy:
		l.sunStrength = 0.55f;
		l.ambientStrength = 0.85f;
		l.skyLightDrop = 1;
		l.overcast = 0.6f;
		l.skyDarken = 0.1f;
		l.rain = 0;
		break;
	case weatherRain:
		l.sunStrength = 0.25f;
		l.ambientStrength = 0.7f;
		l.skyLightDrop = 3;
		l.overcast = 0.9f;
		l.skyDarken = 0.35f;
		l.rain = 0.7f;
		l.fog = 1;
		l.fogEnd = 90;
		l.fogShade = 0.6f;
		break;
	case weatherStorm:
		l.sunStrength = 0.1f;
		l.ambientStrength = 0.55f;
		l.skyLightDrop = 5;
		l.overcast = 1;
		l.skyDarken = 0.6f;
		l.rain = 1;
		l.fog = 1;
		l.fogEnd = 60;
		l.fogShade = 0.42f;
		break;
	case weatherSnow:
		// A bright, flat white-grey: the sun gone, the light all ambient,
		// the sky a pale grey that the fog closes into.
		l.sunStrength = 0.2f;
		l.ambientStrength = 0.9f;
		l.skyLightDrop = 2;
		l.overcast = 1;
		l.skyDarken = 0;
		l.rain = 0;
		l.snow = 1;
		l.fog = 1;
		l.fogEnd = 40;
		l.fogShade = 1;
		break;
	default: break; // clear: the identity
	}
	return l;
}

WeatherLook mixWeatherLooks(const WeatherLook &a, const WeatherLook &b, float t)
{
	t = std::clamp(t, 0.f, 1.f);
	auto mix = [t](float x, float y) { return x + (y - x) * t; };
	WeatherLook l;
	l.sunStrength = mix(a.sunStrength, b.sunStrength);
	l.ambientStrength = mix(a.ambientStrength, b.ambientStrength);
	l.skyLightDrop = mix(a.skyLightDrop, b.skyLightDrop);
	l.overcast = mix(a.overcast, b.overcast);
	l.skyDarken = mix(a.skyDarken, b.skyDarken);
	l.rain = mix(a.rain, b.rain);
	l.snow = mix(a.snow, b.snow);
	l.fog = mix(a.fog, b.fog);
	// The fog's distance and colour are those of the weather that has it,
	// by how much each has: from clear into rain the fog thickens at rain's
	// distance, rather than sweeping in from wherever clear's would be.
	const float wa = a.fog * (1 - t), wb = b.fog * t;
	if (wa + wb > 0)
	{
		l.fogEnd = (a.fogEnd * wa + b.fogEnd * wb) / (wa + wb);
		l.fogShade = (a.fogShade * wa + b.fogShade * wb) / (wa + wb);
	}
	return l;
}

float WorldClock::dayTime() const
{
	const double length = std::max(dayLengthSeconds, 1.f);
	double t = std::fmod(worldSeconds / length, 1.0);
	if (t < 0) { t += 1; }
	return (float)t;
}

// Time only runs forward (so a schedule never sees a day twice): an earlier
// time of day than now is the next day's.
void WorldClock::setDayTime(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	if (t >= 1.f) { t = 0; }
	const double length = std::max(dayLengthSeconds, 1.f);
	double days = std::floor(worldSeconds / length);
	if (t < dayTime()) { days += 1; }
	worldSeconds = (days + t) * length;
}

WeatherLook WorldClock::look() const
{
	if (weather == nextWeather) { return getWeatherLook(weather); }
	// Smoothstep, so a change eases in and out instead of starting and
	// stopping at full speed.
	const float t = std::clamp(transition, 0.f, 1.f);
	return mixWeatherLooks(getWeatherLook(weather), getWeatherLook(nextWeather), t * t * (3 - 2 * t));
}

void WorldClock::advance(float seconds)
{
	if (seconds <= 0) { return; }
	if (dayCycle) { worldSeconds += seconds; }

	if (weather != nextWeather)
	{
		transition += seconds / std::max(transitionSeconds, 0.001f);
		if (transition >= 1)
		{
			weather = nextWeather;
			transition = 0;
		}
	}
}

void WorldClock::startWeather(int newWeather, float seconds)
{
	if (newWeather < 0 || newWeather >= WEATHER_COUNT) { return; }

	// A change already running settles on whichever weather it is closer to;
	// the blend can only run between two weathers.
	if (weather != nextWeather && transition >= 0.5f) { weather = nextWeather; }
	transition = 0;

	if (seconds <= 0 || newWeather == weather)
	{
		weather = (unsigned char)newWeather;
		nextWeather = (unsigned char)newWeather;
		return;
	}

	nextWeather = (unsigned char)newWeather;
	transitionSeconds = seconds;
}

namespace
{
	// Which weather follows which, as weights (a Markov chain), and how long
	// each lasts. Storms only come out of rain or clouds, and end in rain.
	// Snow is rarer, mostly out of clouds, and clears through clouds.
	const float nextWeatherWeights[WEATHER_COUNT][WEATHER_COUNT] =
	{
		//clear cloudy rain storm snow
		{5,     4,     1,   0,    0.5f}, // after clear
		{4,     2,     3,   1,    1.5f}, // after cloudy
		{1,     4,     2,   2,    0.5f}, // after rain
		{0,     2,     5,   0,    0},    // after a storm
		{2,     5,     0,   0,    1},    // after snow
	};

	const float weatherMinutes[WEATHER_COUNT][2] =
	{
		{8, 20}, // clear
		{4, 10}, // cloudy
		{3, 8},  // rain
		{2, 5},  // storm
		{4, 10}, // snow
	};

	float randomRange(std::minstd_rand &rng, float a, float b)
	{
		return std::uniform_real_distribution<float>(a, b)(rng);
	}
}

void serverUpdateWorldClock(WorldClock &clock, float seconds, std::minstd_rand &rng)
{
	clock.advance(seconds);

	clock.secondsUntilChange -= seconds;
	if (clock.secondsUntilChange > 0 || clock.weather != clock.nextWeather) { return; }

	const int from = std::clamp<int>(clock.weather, 0, WEATHER_COUNT - 1);
	std::discrete_distribution<int> pick(std::begin(nextWeatherWeights[from]), std::end(nextWeatherWeights[from]));
	const int next = pick(rng);

	clock.secondsUntilChange = randomRange(rng, weatherMinutes[next][0], weatherMinutes[next][1]) * 60;
	if (next != clock.weather)
	{
		clock.startWeather(next, randomRange(rng, 30, 60));
	}
	serverWorldClockChanged() = true;
}

WorldClock makeNewWorldClock()
{
	WorldClock clock;
	clock.worldSeconds = 0.1 * clock.dayLengthSeconds;
	return clock;
}

WorldClock &getServerWorldClock()
{
	static WorldClock clock = makeNewWorldClock();
	return clock;
}

bool &serverWorldClockChanged()
{
	static bool changed = false;
	return changed;
}

WorldClock &getClientWorldClock()
{
	static WorldClock clock = makeNewWorldClock();
	return clock;
}

float getStormAmount(const WorldClock &clock)
{
	const float t = std::clamp(clock.transition, 0.f, 1.f);
	if (clock.weather == weatherStorm) { return clock.nextWeather == weatherStorm ? 1.f : 1.f - t; }
	if (clock.nextWeather == weatherStorm) { return t; }
	return 0;
}

std::vector<glm::ivec2> &getClientLightningStrikes()
{
	static std::vector<glm::ivec2> strikes;
	return strikes;
}
