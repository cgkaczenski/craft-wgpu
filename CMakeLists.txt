#https://github.com/meemknight/cmakeSetup


cmake_minimum_required(VERSION 3.16)

# CMake 4 refuses projects that ask for cmake_minimum_required below 3.5, and
# most of thirdparty/ does. Treat them as 3.5 instead of editing each one.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

#cmake_policy(SET CMP0069 NEW)
#set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

#if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
#	add_compile_options(-ffast-math)
#elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
#	add_compile_options(/fp:fast)
#endif()


#make shure the runtime library is statically linked
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Release>:Release>")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(ourCraft)

# So clangd can resolve includes such as "rendering/frustumCulling.h"
# (headers live under include/gameLayer/, which is an include directory).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# AVX2 is x86-only. Apple Silicon and other ARM targets use NEON inside FastNoise.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|i386|i686|X86")
	if (MSVC)
		# Set the compiler flags for AVX
		#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:AVX")
		# Uncomment the following line for AVX2 instead of AVX
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:AVX2")
	else()
		# Set the compiler flags for AVX
		#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx")
		# Uncomment the following line for AVX2 instead of AVX
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
	endif()
endif()


set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Configure Assimp
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE) # Make sure zlib is enabled if you're linking against libzstd_static
set(ASSIMP_BUILD_NO_EXCEPTIONS ON CACHE BOOL "" FORCE) 
set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_OBJ_EXPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_MTL_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_MTL_EXPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTF_EXPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTFB_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_GLTFB_EXPORTER OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
add_definitions(-D_ITERATOR_DEBUG_LEVEL=0)
option(ASSIMP_BUILD_STATIC_LIB "If the library shall be built as static library." ON)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ZLIB "1" CACHE STRING "If zlib is used, static linking. Specify Zlib < 1.3")
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)



include(FetchContent)

# WebGPU (docs/roadmap.md): wgpu-native, prebuilt for Windows, Linux and
# macOS, with GLFW for the window. glfw3webgpu v1.3 calls glfwGetPlatform(),
# which needs GLFW 3.4.
FetchContent_Declare(glfw
	GIT_REPOSITORY https://github.com/glfw/glfw.git
	GIT_TAG 3.4
	GIT_SHALLOW TRUE
)

# Fetches precompiled wgpu-native (v24.0.3.1) and provides the 'webgpu'
# target, including the WebGPU-Cpp wrapper <webgpu/webgpu.hpp>.
set(WEBGPU_BACKEND "WGPU" CACHE STRING "" FORCE)
FetchContent_Declare(webgpu
	GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution.git
	GIT_TAG v0.3.0-gamma
	GIT_SHALLOW TRUE # older branches carry binaries; a full clone is huge
)

FetchContent_Declare(glfw3webgpu
	GIT_REPOSITORY https://github.com/eliemichel/glfw3webgpu.git
	GIT_TAG v1.3.0-alpha
	GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(glfw webgpu glfw3webgpu)

# The 2D layer (milestone 3): wgpu2d, with gl2d's API through a shim header
# so the game and glui compile unchanged.
add_subdirectory(thirdparty/wgpu2d)
set(GL2D_TARGET gl2dShim)

# glad stays until milestone 19d deletes the null GL scaffold it carries.
add_subdirectory(thirdparty/glad)
add_subdirectory(thirdparty/stb_image)
# PNG output for WGPU_SCREENSHOT_FRAME (wgpuWindow.cpp).
add_subdirectory(thirdparty/stb_image_write)
add_subdirectory(thirdparty/stb_truetype)
add_subdirectory(thirdparty/raudio)
add_subdirectory(thirdparty/glm)
add_subdirectory(thirdparty/imgui-docking)
add_subdirectory(thirdparty/FastNoiseSIMD)
add_subdirectory(thirdparty/FastNoise2)
add_subdirectory(thirdparty/enet-1.3.18)
add_subdirectory(thirdparty/glui)
add_subdirectory(thirdparty/assimp-5.2.4)
add_subdirectory(thirdparty/magic_enum-0.9.3)
add_subdirectory(thirdparty/safeSave)
add_subdirectory(thirdparty/profilerLib)
add_subdirectory(thirdparty/zstd-1.5.5/build/cmake)


# MY_SOURCES is a list of all the source files for my game 
file(GLOB_RECURSE MY_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE SHARED_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shared/*.cpp")


add_executable("${CMAKE_PROJECT_NAME}")

set_property(TARGET "${CMAKE_PROJECT_NAME}" PROPERTY CXX_STANDARD 17)

#set_target_properties("${CMAKE_PROJECT_NAME}" PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")


target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC RESOURCES_PATH="${CMAKE_CURRENT_SOURCE_DIR}/resources/") # This is useful to get an ASSETS_PATH in your IDE during development but you should comment this if you compile a release version and uncomment the next line
#target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC RESOURCES_PATH="./resources/") # Uncomment this line to setup the ASSETS_PATH macro to the final assets directory when you share the game


target_sources("${CMAKE_PROJECT_NAME}" PRIVATE ${MY_SOURCES} ${SHARED_SOURCES} )


if(MSVC) # If using the VS compiler...

	target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC _CRT_SECURE_NO_WARNINGS)

	set_target_properties("${CMAKE_PROJECT_NAME}" PROPERTIES LINK_FLAGS "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")


	#add_compile_options(-fsanitize=address)
	#add_link_options(-fsanitize=address)

endif()

if(WIN32)
	target_compile_definitions("${CMAKE_PROJECT_NAME}" PUBLIC _WIN32) 
  
endif()

target_include_directories("${CMAKE_PROJECT_NAME}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/shared/")
target_include_directories("${CMAKE_PROJECT_NAME}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/")
target_include_directories("${CMAKE_PROJECT_NAME}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/gameLayer/")
target_include_directories("${CMAKE_PROJECT_NAME}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include/platform/")
target_include_directories("${CMAKE_PROJECT_NAME}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/")


target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE glm glfw glad stb_image stb_image_write stb_truetype 
	${GL2D_TARGET} raudio imgui fastNoiseSIMD FastNoise2 enet glui
	magic_enum safeSave profilerLib
	libzstd_static assimp)

target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE webgpu glfw3webgpu)
if(APPLE)
	# Pins the CAMetalLayer colorspace to sRGB. Compiled as C++ with
	# -x objective-c++ rather than enable_language(OBJCXX), which would also
	# claim GLFW's .m files and compile its Objective-C as Objective-C++.
	set(WGPU_METAL_LAYER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/wgpuMetalLayer.mm")
	set_source_files_properties("${WGPU_METAL_LAYER_SRC}" PROPERTIES
		LANGUAGE CXX
		COMPILE_OPTIONS "-x;objective-c++")
	target_sources("${CMAKE_PROJECT_NAME}" PRIVATE "${WGPU_METAL_LAYER_SRC}")
	target_link_libraries("${CMAKE_PROJECT_NAME}" PRIVATE "-framework Cocoa" "-framework QuartzCore")
endif()
# Copies the wgpu-native .dylib/.so/.dll next to the executable.
target_copy_webgpu_binaries("${CMAKE_PROJECT_NAME}")


