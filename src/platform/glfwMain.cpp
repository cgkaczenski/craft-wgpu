#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <stb_image/stb_image.h>
#include <stb_truetype/stb_truetype.h>
#include "gl2d/gl2d.h"
#include <iostream>
#include <ctime>
#include "platformTools.h"
#include "config.h"
#include <raudio.h>
#include "platformInput.h"
#include "otherPlatformFunctions.h"
#include "gameLayer.h"
#include <fstream>
#include <chrono>
#include <profilerLib/include/profilerLib.h>
#include <errorReporting.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

	#include "wgpuWindow.h"
	#include "wgpuImgui.h"
	#include "rendering/wgpuWorld.h"
	#include "nullGL.h"


#ifdef _WIN32
#define GPU_ENGINE 1
extern "C"
{
	__declspec(dllexport) unsigned long NvOptimusEnablement = GPU_ENGINE;
	__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = GPU_ENGINE;
}
#endif


#if REMOVE_IMGUI == 0
	#include "imgui.h"
	#include "backends/imgui_impl_glfw.h"
	#include "backends/imgui_impl_opengl3.h"
	#include "imguiThemes.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

#undef min
#undef max

#pragma region globals 
bool currentFullScreen = 0;
bool fullScreen = 0;

#pragma endregion


void keyCallback(GLFWwindow *window, int key, int scancode, int action, int mods)
{

	if ((action == GLFW_REPEAT || action == GLFW_PRESS) && key == GLFW_KEY_BACKSPACE)
	{
		platform::internal::addToTypedInput(8);
	}

	bool state = 0;

	if(action == GLFW_PRESS)
	{
		state = 1;
	}else if(action == GLFW_RELEASE)
	{
		state = 0;
	}else
	{
		return;
	}

	if(key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
	{
		int index = key - GLFW_KEY_A;
		platform::internal::setButtonState(platform::Button::A + index, state);
	}else if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
	{
		int index = key - GLFW_KEY_0;
		platform::internal::setButtonState(platform::Button::NR0 + index, state);
	}else
	{
	//special keys
		//GLFW_KEY_SPACE, GLFW_KEY_ENTER, GLFW_KEY_ESCAPE, GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT

		if (key == GLFW_KEY_SPACE)
		{
			platform::internal::setButtonState(platform::Button::Space, state);
		}
		else
		if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER)
		{
			platform::internal::setButtonState(platform::Button::Enter, state);
		}
		else
		if (key == GLFW_KEY_ESCAPE)
		{
			platform::internal::setButtonState(platform::Button::Escape, state);
		}
		else
		if (key == GLFW_KEY_UP)
		{
			platform::internal::setButtonState(platform::Button::Up, state);
		}
		else
		if (key == GLFW_KEY_DOWN)
		{
			platform::internal::setButtonState(platform::Button::Down, state);
		}
		else
		if (key == GLFW_KEY_LEFT)
		{
			platform::internal::setButtonState(platform::Button::Left, state);
		}
		else
		if (key == GLFW_KEY_RIGHT)
		{
			platform::internal::setButtonState(platform::Button::Right, state);
		}
		else
		if (key == GLFW_KEY_LEFT_CONTROL)
		{
			platform::internal::setButtonState(platform::Button::LeftCtrl, state);
		}
		else
		if (key == GLFW_KEY_LEFT_ALT)
		{
			platform::internal::setButtonState(platform::Button::LeftAlt, state);
		}
		else
		if (key == GLFW_KEY_LEFT_SHIFT)
		{
			platform::internal::setButtonState(platform::Button::LeftShift, state);

		}else 
		if (key == GLFW_KEY_SLASH)
		{
			platform::internal::setButtonState(platform::Button::SlashQuestionMark, state);
		}else
		if (key >= GLFW_KEY_F1 && key <= GLFW_KEY_F12)
		{
			int index = key - GLFW_KEY_F1;
			platform::internal::setButtonState(platform::Button::F1 + index, state);

		}
	}
	
};

void mouseCallback(GLFWwindow *window, int key, int action, int mods)
{
	bool state = 0;

	if (action == GLFW_PRESS)
	{
		state = 1;
	}
	else if (action == GLFW_RELEASE)
	{
		state = 0;
	}
	else
	{
		return;
	}

	if(key == GLFW_MOUSE_BUTTON_LEFT)
	{
		platform::internal::setLeftMouseState(state);
	}else
	if (key == GLFW_MOUSE_BUTTON_RIGHT)
	{
		platform::internal::setRightMouseState(state);
	}
	

}

bool windowFocus = 1;

void windowFocusCallback(GLFWwindow *window, int focused)
{
	if (focused)
	{
		windowFocus = 1;
	}
	else
	{
		windowFocus = 0;
		//if you not capture the release event when the window loses focus,
		//the buttons will stay pressed
		platform::internal::resetInputsToZero();
	}
}

void windowSizeCallback(GLFWwindow *window, int x, int y)
{
	platform::internal::resetInputsToZero();
}

int mouseMovedFlag = 0;

void cursorPositionCallback(GLFWwindow *window, double xpos, double ypos)
{
	mouseMovedFlag = 1;
}

void characterCallback(GLFWwindow *window, unsigned int codepoint)
{
	if (codepoint < 127)
	{
		platform::internal::addToTypedInput(codepoint);
	}
}

void scrollCallback(GLFWwindow *window, double xoffset, double yoffset);

// OURCRAFT_INPUT="frame:action;frame:action..." plays input on given frames, so
// a WGPU_SCREENSHOT_FRAME capture can reach a menu with no one at the keyboard.
// Actions: click@x,y (window points; the press lands on that frame, the
// release on the next), move@x,y (the mouse moves there, no button: turns the
// camera in game; the game re-centres the cursor at half the framebuffer
// size each frame, so the offset from there is the turn), key@E, key@Space,
// key@LShift or key@F9 (pressed, then released the same way), hold@W,N (held
// for N frames: walking; hold@Mouse,N holds the left button) and quit (closes
// the window, so shutdown runs). Feeds the callbacks below; the mouse position is overridden from
// the first click on, without moving the real cursor.
static glm::ivec2 scriptedMouse = {0, 0};
static bool scriptedMouseActive = false; // set by the first click@ or move@ (coordinates may be negative)

// A key named in a script: one letter or digit, Space or LShift. -1 if unknown.
static int scriptedKey(const char *name)
{
	const std::string n = name;
	if (n.size() == 1 && std::isalpha((unsigned char)n[0])) { return GLFW_KEY_A + (std::toupper((unsigned char)n[0]) - 'A'); }
	if (n.size() == 1 && std::isdigit((unsigned char)n[0])) { return GLFW_KEY_0 + (n[0] - '0'); }
	if (n == "Space") { return GLFW_KEY_SPACE; }
	if (n == "LShift") { return GLFW_KEY_LEFT_SHIFT; }
	return -1;
}

static void playScriptedInput(GLFWwindow *window)
{
	static const char *script = std::getenv("OURCRAFT_INPUT");
	if (!script) { return; }

	static long long frameIndex = 0;
	static std::vector<std::pair<int, int>> releases; // frame, key (-1 = left mouse)
	++frameIndex;

	for (auto it = releases.begin(); it != releases.end();)
	{
		if (it->first != frameIndex) { ++it; continue; }
		if (it->second < 0) { mouseCallback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0); }
		else { keyCallback(window, it->second, 0, GLFW_RELEASE, 0); }
		it = releases.erase(it);
	}

	std::string s = script;
	size_t start = 0;
	while (start < s.size())
	{
		size_t end = s.find(';', start);
		if (end == std::string::npos) { end = s.size(); }
		std::string step = s.substr(start, end - start);
		start = end + 1;

		int frame = 0, x = 0, y = 0, function = 0;
		int quitFrame = 0;
		char quitWord[5] = {};
		if (std::sscanf(step.c_str(), "%d:%4s", &quitFrame, quitWord) == 2 && std::string(quitWord) == "quit"
			&& quitFrame == frameIndex)
		{
			glfwSetWindowShouldClose(window, GLFW_TRUE);
			std::cout << "input: frame " << quitFrame << " quit\n";
		}
		else if (std::sscanf(step.c_str(), "%d:key@F%d", &frame, &function) == 2 && frame == frameIndex
			&& function >= 1 && function <= 12)
		{
			int key = GLFW_KEY_F1 + function - 1;
			keyCallback(window, key, 0, GLFW_PRESS, 0);
			releases.push_back({frame + 1, key});
			std::cout << "input: frame " << frame << " key F" << function << "\n";
		}
		else if (std::sscanf(step.c_str(), "%d:click@%d,%d", &frame, &x, &y) == 3 && frame == frameIndex)
		{
			scriptedMouse = {x, y};
			scriptedMouseActive = true;
			mouseCallback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
			releases.push_back({frame + 1, -1});
			std::cout << "input: frame " << frame << " click " << x << "," << y << "\n";
		}
		else if (std::sscanf(step.c_str(), "%d:move@%d,%d", &frame, &x, &y) == 3 && frame == frameIndex)
		{
			scriptedMouse = {x, y};
			scriptedMouseActive = true;
			std::cout << "input: frame " << frame << " move " << x << "," << y << "\n";
		}
		else if (char name[16] = {}; std::sscanf(step.c_str(), "%d:key@%15[A-Za-z0-9]", &frame, name) == 2
			&& frame == frameIndex && scriptedKey(name) >= 0)
		{
			int key = scriptedKey(name);
			keyCallback(window, key, 0, GLFW_PRESS, 0);
			releases.push_back({frame + 1, key});
			std::cout << "input: frame " << frame << " key " << name << "\n";
		}
		else if (int frames = 0; std::sscanf(step.c_str(), "%d:hold@%15[A-Za-z0-9],%d", &frame, name, &frames) == 3
			&& frame == frameIndex && std::string(name) == "Mouse" && frames > 0)
		{
			// The left button held at the scripted mouse: breaking a block.
			mouseCallback(window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
			releases.push_back({frame + frames, -1});
			std::cout << "input: frame " << frame << " hold Mouse for " << frames << " frames\n";
		}
		else if (int frames = 0; std::sscanf(step.c_str(), "%d:hold@%15[A-Za-z0-9],%d", &frame, name, &frames) == 3
			&& frame == frameIndex && scriptedKey(name) >= 0 && frames > 0)
		{
			// Like key@, but released `frames` frames later: walks the player.
			int key = scriptedKey(name);
			keyCallback(window, key, 0, GLFW_PRESS, 0);
			releases.push_back({frame + frames, key});
			std::cout << "input: frame " << frame << " hold " << name << " for " << frames << " frames\n";
		}
	}
}



#pragma region platform functions

GLFWwindow *wind = 0;

namespace platform
{

	void setRelMousePosition(int x, int y)
	{
		// A scripted mouse (OURCRAFT_INPUT) follows the warp like a real one.
		if (scriptedMouseActive) { scriptedMouse = {x, y}; }
		glfwSetCursorPos(wind, x, y);
	}

	bool isFullScreen()
	{
		return fullScreen;
	}

	void setFullScreen(bool f)
	{
		fullScreen = f;
	}

	glm::ivec2 getRelMousePosition()
	{
		if (scriptedMouseActive) { return scriptedMouse; }
		double x = 0, y = 0;
		glfwGetCursorPos(wind, &x, &y);
		return { x, y };
	}

	glm::ivec2 getWindowSize()
	{
		int x = 0; int y = 0;
		glfwGetWindowSize(wind, &x, &y);
		return { x, y };
	}

	void showMouse(bool show)
	{
		static bool lastValue = true;
		if(show)
		{
			if (!lastValue)
			{
			
				glfwSetInputMode(wind, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
				if (glfwRawMouseMotionSupported())
				{
					glfwSetInputMode(wind, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
				}

			}
			lastValue = true;
		}else
		{
			if (lastValue)
			{
				auto size = getWindowSize();
				glfwSetCursorPos(wind, size.x / 2, size.y / 2);
				//glfwSetInputMode(wind, GLFW_CURSOR, GLFW_CURSOR_HIDDEN);
				glfwSetInputMode(wind, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
				if (glfwRawMouseMotionSupported())
				{
					glfwSetInputMode(wind, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
				}

			}

			lastValue = false;
		}
	}

	bool isFocused()
	{
		return windowFocus;
	}

	bool mouseMoved()
	{
		return mouseMovedFlag;
	}

	bool writeEntireFile(const char *name, void *buffer, size_t size)
	{
		std::ofstream f(name, std::ios::binary);

		if(!f.is_open())
		{
			return 0;
		}

		f.write((char*)buffer, size);

		f.close();

		return 1;
	}


	bool readEntireFile(const char *name, void *buffer, size_t size)
	{
		std::ifstream f(name, std::ios::binary);

		if (!f.is_open())
		{
			return 0;
		}

		f.read((char *)buffer, size);

		f.close();

		return 1;
	}


};
#pragma endregion


#ifdef __APPLE__
static void APIENTRY appleCreateBuffers(GLsizei n, GLuint *buffers)
{
	glGenBuffers(n, buffers);
}

static void APIENTRY appleCreateVertexArrays(GLsizei n, GLuint *arrays)
{
	glGenVertexArrays(n, arrays);
}

static void APIENTRY appleBufferStorage(GLenum target, GLsizeiptr size, const void *data, GLbitfield)
{
	glBufferData(target, size, data, GL_STATIC_DRAW);
}

static void APIENTRY appleNamedBufferData(GLuint buffer, GLsizeiptr size, const void *data, GLenum usage)
{
	GLint previous = 0;
	glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, size, data, usage);
	glBindBuffer(GL_ARRAY_BUFFER, (GLuint)previous);
}

static GLuint64 APIENTRY appleGetTextureHandle(GLuint texture)
{
	return texture;
}

static void APIENTRY appleMakeTextureHandleResident(GLuint64)
{
}

static void APIENTRY appleShaderStorageBlockBinding(GLuint, GLuint, GLuint)
{
}

static GLuint APIENTRY appleGetProgramResourceIndex(GLuint, GLenum, const GLchar *)
{
	return GL_INVALID_INDEX;
}

static void APIENTRY appleMultiDrawElementsIndirect(GLenum, GLenum, const void *, GLsizei, GLsizei)
{
}

static void APIENTRY appleUniformHandleui64(GLint, GLuint64)
{
}

static void APIENTRY appleUniformHandleui64v(GLint, GLsizei, const GLuint64 *)
{
}

static void APIENTRY applePushDebugGroup(GLenum, GLuint, GLsizei, const GLchar *)
{
}

static void APIENTRY applePopDebugGroup()
{
}

static void APIENTRY appleBlendFunci(GLuint, GLenum src, GLenum dst)
{
	glBlendFunc(src, dst);
}

static void installAppleGLFallbacks()
{
	if (!glad_glCreateBuffers) { glad_glCreateBuffers = appleCreateBuffers; }
	if (!glad_glCreateVertexArrays) { glad_glCreateVertexArrays = appleCreateVertexArrays; }
	if (!glad_glBufferStorage) { glad_glBufferStorage = appleBufferStorage; }
	if (!glad_glNamedBufferData) { glad_glNamedBufferData = appleNamedBufferData; }
	if (!glad_glGetTextureHandleARB) { glad_glGetTextureHandleARB = appleGetTextureHandle; }
	if (!glad_glMakeTextureHandleResidentARB) { glad_glMakeTextureHandleResidentARB = appleMakeTextureHandleResident; }
	if (!glad_glShaderStorageBlockBinding) { glad_glShaderStorageBlockBinding = appleShaderStorageBlockBinding; }
	if (!glad_glGetProgramResourceIndex) { glad_glGetProgramResourceIndex = appleGetProgramResourceIndex; }
	if (!glad_glMultiDrawElementsIndirect) { glad_glMultiDrawElementsIndirect = appleMultiDrawElementsIndirect; }
	if (!glad_glUniformHandleui64ARB) { glad_glUniformHandleui64ARB = appleUniformHandleui64; }
	if (!glad_glUniformHandleui64vARB) { glad_glUniformHandleui64vARB = appleUniformHandleui64v; }
	if (!glad_glPushDebugGroup) { glad_glPushDebugGroup = applePushDebugGroup; }
	if (!glad_glPopDebugGroup) { glad_glPopDebugGroup = applePopDebugGroup; }
	if (!glad_glBlendFunci) { glad_glBlendFunci = appleBlendFunci; }
}
#endif

bool HasExtension(const char *name)
{
	GLint count = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &count);

	for (GLint i = 0; i < count; i++)
	{
		const char *ext = (const char *)glGetStringi(GL_EXTENSIONS, i);
		if (strcmp(ext, name) == 0)
			return true;
	}

	return false;
}


int main()
{

#ifdef _WIN32
	timeBeginPeriod(1);
#ifdef _MSC_VER 
#if INTERNAL_BUILD
	AllocConsole();
	(void)freopen("conin$", "r", stdin);
	(void)freopen("conout$", "w", stdout);
	(void)freopen("conout$", "w", stderr);
	std::cout.sync_with_stdio();
#endif
#endif
#endif

//#ifdef _WIN32
//	timeBeginPeriod(1);
//	atexit([]() { timeEndPeriod(1); });
//#endif


#pragma region window and opengl

	glfwSetErrorCallback([](int error, const char *description)
	{
		std::cout << "GLFW error " << error << ": " << description << "\n";
	});

	permaAssertComment(glfwInit(), "err initializing glfw");
	//glfwWindowHint(GLFW_SAMPLES, 4);
	// No OpenGL context: WebGPU owns the window's drawable (a CAMetalLayer on macOS).
	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

	int w = 500;
	int h = 500;
	wind = glfwCreateWindow(w, h, "geam", nullptr, nullptr);
	if (!wind)
	{
		std::cout << "Failed to create a window.\n";
		return 1;
	}
#ifdef __APPLE__
	if (GLFWmonitor *monitor = glfwGetPrimaryMonitor())
	{
		int mx = 0, my = 0;
		glfwGetMonitorPos(monitor, &mx, &my);
		glfwSetWindowPos(wind, mx + 80, my + 80);
	}
#endif

	glfwSetKeyCallback(wind, keyCallback);
	glfwSetMouseButtonCallback(wind, mouseCallback);
	glfwSetWindowFocusCallback(wind, windowFocusCallback);
	glfwSetWindowSizeCallback(wind, windowSizeCallback);
	glfwSetCursorPosCallback(wind, cursorPositionCallback);
	glfwSetCharCallback(wind, characterCallback);
	glfwSetScrollCallback(wind, scrollCallback);

	// WebGPU owns the window. gl2d and glui draw through wgpu2d and ImGui
	// through wgpuImgui; the 3D renderer still calls OpenGL, and with no
	// context those calls go to nullGL's do-nothing stubs until later
	// milestones port it.
	if (!wgpuWindow::init(wind))
	{
		std::cout << "WebGPU initialization failed.\n";
		wgpuWindow::shutdown();
		glfwTerminate();
		return 1;
	}
	nullGL::install();

	//enableReportGlErrors();

#pragma endregion

#pragma region gl2d
	gl2d::init();
#pragma endregion

#pragma region imgui
	#if REMOVE_IMGUI == 0
		ImGui::CreateContext();
		//ImGui::StyleColorsDark();
		imguiThemes::embraceTheDarkness();

		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;           // Enable Docking
		//io.ConfigViewportsNoAutoMerge = true;
		//io.ConfigViewportsNoTaskBarIcon = true;
	
		ImGuiStyle& style = ImGui::GetStyle();
		// The same see-through style as with viewports, so the dock space
		// covering the window does not hide the game behind it.
		const bool seeThrough = true;
		if (seeThrough)
		{
			//style.WindowRounding = 0.0f;
			style.Colors[ImGuiCol_WindowBg].w = 0.f; //always
			style.Colors[ImGuiCol_DockingEmptyBg].w = 0.f; //always
		}
	
		// Input and window size; wgpuImgui draws.
		ImGui_ImplGlfw_InitForOther(wind, true);

		//ImGuiIO &io = ImGui::GetIO();
		//io.Fonts->AddFontFromFileTTF("path/to/your/font.ttf", 24.0f);

		ImFontConfig fontConfig;
		ImFont *font = io.Fonts->AddFontDefault(&fontConfig);
		font->Scale = 1.5f; // Scale factor to make the default font larger
		io.FontDefault = font;

		// After the font setup: it builds the atlas and uploads it.
		permaAssertComment(wgpuImgui::init(), "err initializing the WebGPU ImGui backend");
	#endif
#pragma endregion

#pragma region audio
	//InitAudioDevice();
	//
	////Music m = LoadMusicStream(RESOURCES_PATH "target.ogg");
	//Music m = {};
	//UpdateMusicStream(m);
	////StopMusicStream(m);
	//PlayMusicStream(m);

#pragma endregion

#pragma region initGame
	if (!initGame())
	{
		return 0;
	}
#pragma endregion


	//long lastTime = clock();
	
	auto stop = std::chrono::high_resolution_clock::now();

	while (!glfwWindowShouldClose(wind))
	{
		//UpdateMusicStream(m);
		//PlayMusicStream(m);

	#pragma region deltaTime

		//long newTime = clock();
		//float deltaTime = (float)(newTime - lastTime) / CLOCKS_PER_SEC;
		//lastTime = clock();
		auto start = std::chrono::high_resolution_clock::now();

		float deltaTime = (std::chrono::duration_cast<std::chrono::microseconds>(start - stop)).count() / 1000000.0f;
		stop = std::chrono::high_resolution_clock::now();

		float augmentedDeltaTime = deltaTime;
		if (augmentedDeltaTime > 1.f / 6) { augmentedDeltaTime = 1.f / 6; }
	
	#pragma endregion

	#pragma region imgui
		#if REMOVE_IMGUI == 0
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
		#endif
	#pragma endregion

	#pragma region game logic

		// The frame must be open before gameLogic: wgpu2d's flush records into it.
		wgpuWindow::beginFrame();

		playScriptedInput(wind);

		if (!gameLogic(augmentedDeltaTime))
		{
			closeGame();
			wgpuWorld::shutdown();
			wgpuImgui::shutdown();
			wgpuWindow::shutdown();
			return 0;
		}

	#pragma endregion


	#pragma region fullscreen 

		if (platform::isFocused() && currentFullScreen != fullScreen)
		{
			static int lastW = w;
			static int lastH = w;
			static int lastPosX = 0;
			static int lastPosY = 0;

			if (fullScreen)
			{
				lastW = w;
				lastH = h;

				//glfwWindowHint(GLFW_DECORATED, NULL); // Remove the border and titlebar..  
				glfwGetWindowPos(wind, &lastPosX, &lastPosY);


				//auto monitor = glfwGetPrimaryMonitor();
				auto monitor = getCurrentMonitor(wind);


				const GLFWvidmode* mode = glfwGetVideoMode(monitor);

				// switch to full screen
				glfwSetWindowMonitor(wind, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);

				currentFullScreen = 1;

			}
			else
			{
				//glfwWindowHint(GLFW_DECORATED, GLFW_TRUE); // 
				glfwSetWindowMonitor(wind, nullptr, lastPosX, lastPosY, lastW, lastH, 0);

				currentFullScreen = 0;
			}

		}

	#pragma endregion

	#pragma region reset flags

		mouseMovedFlag = 0;
		platform::internal::updateAllButtons(deltaTime);
		platform::internal::resetTypedInput();

	#pragma endregion

	#pragma region window stuff


		PL::Profiler pl;
		pl.start();

		#pragma region imgui
				#if REMOVE_IMGUI == 0
				ImGui::Render();
				wgpuImgui::renderDrawData(); // on top of the game, same pass
			#endif
		#pragma endregion

		pl.end();
		//if (pl.rezult.timeSeconds * 1000 > (17))
		//{
		//	std::cout << "IMGUI PROBLEM! " << pl.rezult.timeSeconds * 1000 << "ms!!!!\n";
		//}

		pl.start();
		wgpuWindow::endFrame();
		pl.end();

		//if (pl.rezult.timeSeconds * 1000 > (17))
		//{
		//	std::cout << "SWAP BUFFERS PROBLEM! " << pl.rezult.timeSeconds * 1000 << "ms!!!!\n";
		//}
		

		glfwPollEvents();

	#pragma endregion

	}

	closeGame();
	wgpuWorld::shutdown();
	wgpuImgui::shutdown();
	wgpuWindow::shutdown();

	//if you want the console to stay after closing the window
	//std::cin.clear();
	//std::cin.get();
}
