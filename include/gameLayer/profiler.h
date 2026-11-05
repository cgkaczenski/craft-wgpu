#pragma once
#include <profilerLib.h>
#include <unordered_map>
#include <deque>
#include <string>
#include <vector>
#include <cstring>


struct CharEquals
{
	bool operator()(const char * &__x, const char *&__y) const
	{
		return strcmp(__x, __y) == 0;
	}
};

// CPU time per frame and per named span, for the ImGui plots. (The GPU
// timer queries GL had are gone with OpenGL, milestone 19b.)
struct Profiler
{
	bool pause = 0;

	struct SavedData
	{
		float dataMs[16]; //todo magic number here
		float dataMsReal[16];
	};


	PL::Profiler mainProfiler;

	void startFrame();

	void endFrame();

	void startSubProfile(const char *c);
	void endSubProfile(const char *c);

	void setSubProfileManually(char *c, PL::ProfileRezults rezults);

	std::unordered_map<std::string, PL::Profiler> subProfiles;

	void displayPlot(const char *mainPlotName, float scale = 32);

	std::deque<SavedData> history;

};
