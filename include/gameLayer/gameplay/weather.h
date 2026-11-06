#pragma once
#include <cstdint>
#include <cstddef>
#include <random>

// Milestone G1 (docs/gameplay-roadmap.md): the world clock and the weather.
//
// The server owns both (getServerWorldClock), advances them, saves them with
// the world (clock.bin) and sends them to every client (headerUpdateWorldClock)
// when a player joins, once a second and whenever a command changes them.
// Clients keep the last copy (getClientWorldClock), advance it between
// packets so the sun moves smoothly, and only present it.
//
// The weather is one state for the whole world, as in Minecraft. A weather
// changes how the time of day looks (WeatherLook); while one weather turns
// into the next, the two looks are blended.

enum WeatherType : unsigned char
{
	weatherClear = 0,
	weatherCloudy,
	weatherRain,
	weatherStorm,

	WEATHER_COUNT
};

const char *getWeatherName(int weather);

// What a weather does to the look of the time of day: multipliers and
// amounts on top of the day/night values Renderer::renderFromBakedData
// already computes. Clear is the identity.
struct WeatherLook
{
	float sunStrength = 1;     // the sun's (or moon's) light color times this
	float ambientStrength = 1; // the ambient light color times this
	float skyLightDrop = 0;    // sky light levels taken off the time of day's (of 15)
	float overcast = 0;        // the sky toward grey, and the sun, moon and flare hidden; 0..1
	float skyDarken = 0;       // how much darker the grey sky is; 0..1
	float rain = 0;            // the near fog's rainStrength (GL's computeFog term); 0..1
};

WeatherLook getWeatherLook(int weather);
WeatherLook mixWeatherLooks(const WeatherLook &a, const WeatherLook &b, float t);

// Sent as is in Packet_UpdateWorldClock and saved as is in clock.bin, so plain
// data only.
struct WorldClock
{
	// The world's age in seconds, by the day cycle. dayTime() is where in the
	// day that is: 0 sunrise, 0.25 noon, 0.5 sunset, 0.75 midnight.
	double worldSeconds = 0;
	float dayLengthSeconds = 20 * 60;
	unsigned char dayCycle = 1; // 0 stops the clock (/time stop)

	unsigned char weather = weatherClear;
	unsigned char nextWeather = weatherClear;
	unsigned char padding = 0;
	float transition = 0;          // 0 all weather, 1 all nextWeather
	float transitionSeconds = 40;  // how long the current change takes
	float secondsUntilChange = 600; // the server picks the next weather then

	float dayTime() const;
	void setDayTime(float dayTime); // keeps the day count, moves within the day
	WeatherLook look() const;

	// The clock and a running transition, for clients between packets and
	// for the server.
	void advance(float seconds);

	// Start turning into `weather` over `seconds` (0: at once).
	void startWeather(int weather, float seconds);
};

// clock.bin and the packet carry this layout (WorldSaver::saveWorldClock).
static_assert(sizeof(WorldClock) == 32, "clock.bin's layout");
static_assert(offsetof(WorldClock, weather) == 13 && offsetof(WorldClock, transition) == 16
	&& offsetof(WorldClock, secondsUntilChange) == 24, "clock.bin's layout");

// The server's part: advance, and pick the next weather when it is time.
void serverUpdateWorldClock(WorldClock &clock, float seconds, std::minstd_rand &rng);

// A new world's clock: the morning, clear.
WorldClock makeNewWorldClock();

// Only touched from the server's thread (enetServerFunction and the
// commands it runs). serverWorldClockChanged asks for a broadcast now.
WorldClock &getServerWorldClock();
bool &serverWorldClockChanged();

// Only touched from the client's game thread.
WorldClock &getClientWorldClock();
