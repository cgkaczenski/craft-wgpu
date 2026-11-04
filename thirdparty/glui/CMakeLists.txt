cmake_minimum_required(VERSION 3.1)
project(glui)

# ourCraft's WebGPU path sets GL2D_TARGET to wgpu2d's gl2d shim; standalone,
# glui draws with gl2d.
if(NOT GL2D_TARGET)
	set(GL2D_TARGET gl2d)
endif()

add_library(glui)
target_sources(glui PRIVATE "src/glui.cpp")
target_include_directories(glui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(glui PUBLIC glm glad stb_image stb_truetype ${GL2D_TARGET})
target_compile_features(glui PUBLIC cxx_std_17)
