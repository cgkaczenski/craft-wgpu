// Milestone G1b: the weather's sounds, made in code (the game has no rain or
// thunder recordings): a rain loop streamed while it rains, and thunder,
// several rumbles and a few close cracks. Replace them with recordings by
// loading files here instead; the API (audioEngine.h) stays.
#include <audioEngine.h>
#include <raudio.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <string>

namespace AudioEngine
{
	namespace
	{
		constexpr int sampleRate = 22050;
		constexpr float pi = 3.14159265f;

		struct Random
		{
			std::uint32_t state;
			// Small seeds give tiny first numbers; a few turns mix them in.
			explicit Random(std::uint32_t seed) : state(seed * 2654435761u + 1) { for (int i = 0; i < 8; i++) { next(); } }
			float next() // 0..1
			{
				state ^= state << 13; state ^= state >> 17; state ^= state << 5;
				return (state >> 8) * (1.f / 16777216.f);
			}
			float signedNext() { return next() * 2 - 1; }
		};

		// A one-pole low-pass at `hz`.
		float onePole(float hz) { return 1 - std::exp(-2 * pi * hz / sampleRate); }

		std::vector<std::int16_t> toPcm(const std::vector<float> &samples, float peak)
		{
			float loudest = 0.0001f;
			for (float s : samples) { loudest = std::max(loudest, std::abs(s)); }
			std::vector<std::int16_t> pcm(samples.size());
			for (size_t i = 0; i < samples.size(); i++)
			{
				pcm[i] = (std::int16_t)std::lround(std::tanh(samples[i] / loudest * peak * 1.2f) / 1.2f * 32767.f);
			}
			return pcm;
		}

		// Rain: a hiss (white noise, band-limited to about 400 Hz-3 kHz) and
		// many short drops, each a burst of noise dying in a few milliseconds.
		// `seconds` long, made seamless by fading its extra tail into its head.
		std::vector<std::int16_t> makeRain(float seconds)
		{
			const int length = (int)(seconds * sampleRate);
			const int overlap = sampleRate / 2;
			std::vector<float> out(length + overlap);
			Random random(1234);
			float low = 0, lower = 0;
			struct Drop { float amplitude, decay; } drops[48] = {};
			int dropCount = 0;
			const float aLow = onePole(3000), aLower = onePole(400);
			const float dropsPerSample = 260.f / sampleRate;
			float dropLow = 0;
			for (size_t i = 0; i < out.size(); i++)
			{
				const float white = random.signedNext();
				low += aLow * (white - low);
				lower += aLower * (low - lower);
				float sample = (low - lower) * 0.35f;

				if (random.next() < dropsPerSample && dropCount < 48)
				{
					const float r = random.next();
					drops[dropCount++] = { 0.05f + r * r * 0.5f,
						std::exp(-1.f / (sampleRate * (0.002f + random.next() * 0.006f))) };
				}
				float dropSum = 0;
				for (int d = 0; d < dropCount;)
				{
					dropSum += drops[d].amplitude * random.signedNext();
					drops[d].amplitude *= drops[d].decay;
					if (drops[d].amplitude < 0.002f) { drops[d] = drops[--dropCount]; }
					else { d++; }
				}
				dropLow += onePole(5000) * (dropSum - dropLow);
				out[i] = sample + dropLow;
			}
			for (int i = 0; i < overlap; i++)
			{
				const float t = (float)i / overlap;
				out[i] = out[i] * std::sin(t * pi / 2) + out[length + i] * std::cos(t * pi / 2);
			}
			out.resize(length);
			return toPcm(out, 0.7f);
		}

		// Thunder: brown noise low-passed to a rumble, shaped by a few swells
		// that rise fast and die over a second or two; a close strike starts
		// with a crack (bright noise, gone in a tenth of a second) and a
		// crackle after it.
		std::vector<std::int16_t> makeThunder(std::uint32_t seed, bool close)
		{
			Random random(seed);
			const float seconds = 5.5f + random.next() * 2.5f;
			const int length = (int)(seconds * sampleRate);
			std::vector<float> out(length);

			struct Swell { float start, amplitude, decay; } swells[5];
			const int swellCount = 3 + (int)(random.next() * 3);
			for (int s = 0; s < swellCount; s++)
			{
				swells[s] = { (close ? 0.05f : 0.2f) + random.next() * 1.8f, 0.4f + random.next() * 0.6f,
					0.7f + random.next() * 1.4f };
			}
			if (close) { swells[0] = { 0.02f, 1.f, 1.2f }; }

			float brown = 0, rumble = 0, rumbleLower = 0, crackHigh = 0;
			const float aRumble = onePole(close ? 220.f : 140.f), aLower = onePole(30);
			for (int i = 0; i < length; i++)
			{
				const float t = (float)i / sampleRate;
				const float white = random.signedNext();
				brown = brown * 0.996f + white * 0.06f;
				rumble += aRumble * (brown - rumble);
				rumbleLower += aLower * (rumble - rumbleLower);
				float envelope = 0;
				for (int s = 0; s < swellCount; s++)
				{
					const float d = t - swells[s].start;
					if (d > 0) { envelope += swells[s].amplitude * (1 - std::exp(-d / 0.06f)) * std::exp(-d / swells[s].decay); }
				}
				float sample = (rumble - rumbleLower) * envelope * 3.f;

				if (close)
				{
					crackHigh += onePole(6000) * (white - crackHigh);
					const float crack = (white - crackHigh * 0.5f) * std::exp(-t / 0.05f) * 1.6f;
					const float crackle = (random.next() < 0.02f ? random.signedNext() : 0.f) * std::exp(-t / 0.35f) * 2.f;
					sample += crack + crackle;
				}
				out[i] = sample;
			}
			// Fade the last half second, so no sound ends on a click.
			const int fade = sampleRate / 2;
			for (int i = 0; i < fade; i++) { out[length - 1 - i] *= (float)i / fade; }
			return toPcm(out, close ? 0.95f : 0.8f);
		}

		// OURCRAFT_EXPORT_WEATHER_SOUNDS=<folder> writes each sound there as a
		// WAV when it is made, to listen to them outside the game.
		void exportPcm(std::vector<std::int16_t> &pcm, const std::string &name)
		{
			const char *folder = std::getenv("OURCRAFT_EXPORT_WEATHER_SOUNDS");
			if (!folder) { return; }
			Wave wave = {};
			wave.sampleCount = (unsigned int)pcm.size();
			wave.sampleRate = sampleRate;
			wave.sampleSize = 16;
			wave.channels = 1;
			wave.data = pcm.data();
			ExportWave(wave, (std::string(folder) + "/" + name + ".wav").c_str());
		}

		Sound soundFromPcm(std::vector<std::int16_t> &pcm)
		{
			Wave wave = {};
			wave.sampleCount = (unsigned int)pcm.size();
			wave.sampleRate = sampleRate;
			wave.sampleSize = 16;
			wave.channels = 1;
			wave.data = pcm.data();
			return LoadSoundFromWave(wave); // copies the samples
		}

		// The rain, streamed from its loop. A stream's sub-buffers are this
		// many frames (set before it is made), and each refill writes exactly
		// one.
		constexpr int rainChunkFrames = 8192;
		bool made = false;
		std::vector<std::int16_t> rainLoop;
		size_t rainCursor = 0;
		AudioStream rainStream = {};
		bool rainPlaying = false;
		float rainTarget = 0, rainLevel = 0;
		int rainHeartbeat = 0; // frames left before an unrefreshed level counts as 0

		std::vector<Sound> farThunder, closeThunder;

		void make()
		{
			if (made) { return; }
			made = true;

			rainLoop = makeRain(9.f);
			exportPcm(rainLoop, "rain");
			SetAudioStreamBufferSizeDefault(rainChunkFrames);
			rainStream = InitAudioStream(sampleRate, 16, 1);
			SetAudioStreamBufferSizeDefault(4096);

			for (std::uint32_t seed : { 11u, 23u, 37u, 41u })
			{
				auto pcm = makeThunder(seed, false);
				exportPcm(pcm, "thunderFar" + std::to_string(seed));
				Sound sound = soundFromPcm(pcm);
				if (sound.stream.buffer) { farThunder.push_back(sound); }
			}
			for (std::uint32_t seed : { 5u, 17u, 29u })
			{
				auto pcm = makeThunder(seed, true);
				exportPcm(pcm, "thunderClose" + std::to_string(seed));
				Sound sound = soundFromPcm(pcm);
				if (sound.stream.buffer) { closeThunder.push_back(sound); }
			}
		}

		float soundsLevel()
		{
			return getMasterVolume() * getMasterVolume() * getSoundsVolume() * getSoundsVolume();
		}
	}

	void setRainSound(float level)
	{
		rainTarget = std::clamp(level, 0.f, 1.f);
		rainHeartbeat = 30;
	}

	void playThunder(float level, bool close)
	{
		make();
		auto &sounds = close && !closeThunder.empty() ? closeThunder : farThunder;
		if (sounds.empty()) { return; }
		Sound &sound = sounds[std::rand() % sounds.size()];
		SetSoundVolume(sound, std::clamp(level, 0.f, 1.f) * soundsLevel());
		SetSoundPitch(sound, 0.85f + (std::rand() % 100) / 100.f * 0.3f);
		PlaySound(sound);
	}

	// From AudioEngine::update, every frame.
	void updateWeatherSounds()
	{
		if (rainHeartbeat > 0) { rainHeartbeat--; }
		else { rainTarget = 0; }
		if (!made && rainTarget <= 0) { return; }
		make();
		if (!rainStream.buffer || rainLoop.empty()) { return; }

		// About a second to swell or fade.
		rainLevel += (rainTarget - rainLevel) * 0.03f;
		if (rainTarget <= 0 && rainLevel < 0.002f) { rainLevel = 0; }

		if (rainLevel > 0)
		{
			while (IsAudioStreamProcessed(rainStream))
			{
				std::int16_t chunk[rainChunkFrames];
				for (int i = 0; i < rainChunkFrames; i++)
				{
					chunk[i] = rainLoop[rainCursor];
					rainCursor = (rainCursor + 1) % rainLoop.size();
				}
				UpdateAudioStream(rainStream, chunk, rainChunkFrames);
			}
			SetAudioStreamVolume(rainStream, rainLevel * soundsLevel());
			if (!rainPlaying) { PlayAudioStream(rainStream); rainPlaying = true; }
		}
		else if (rainPlaying)
		{
			StopAudioStream(rainStream);
			rainPlaying = false;
		}
	}
}
