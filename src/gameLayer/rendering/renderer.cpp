#include <rendering/debris.h>
#include <rendering/postModifiers.h>
#include "rendering/renderer.h"
#include "rendering/wgpuWorld.h"
#include <ctime>
#include <chrono>
#include "blocks.h"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/mat3x3.hpp>
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <blocksLoader.h>
#include <chunkSystem.h>
#include <gamePlayLogic.h>
#include <iostream>
#include <platformTools.h>
#include <gameplay/entityManagerClient.h>
#include <rendering/frustumCulling.h>
#include <rendering/model.h>
#include <lightSystem.h>
#include <rendering/renderSettings.h>
#include <vector>
#include <algorithm>
#include <biome.h>
#include <gameplay/precipitation.h>
#include <metrics.h>

#include <imgui-docking/imgui/imgui.h>

float vertexDataOriginal[] = {
	//front
	0.5, 0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	0.5, -0.5, 0.5,
	0.5, 0.5, 0.5,

	//back
	-0.5, -0.5, -0.5,
	-0.5, 0.5, -0.5,
	0.5, 0.5, -0.5,
	0.5, 0.5, -0.5,
	0.5, -0.5, -0.5,
	-0.5, -0.5, -0.5,

	//top
	-0.5, 0.5, -0.5,
	-0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, -0.5,
	-0.5, 0.5, -0.5,

	//bottom
	0.5, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, -0.5,
	-0.5, -0.5, -0.5,
	0.5, -0.5, -0.5,
	0.5, -0.5, 0.5,

	//left
	-0.5, -0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, 0.5, -0.5,
	-0.5, 0.5, -0.5,
	-0.5, -0.5, -0.5,
	-0.5, -0.5, 0.5,

	//right
	0.5, 0.5, -0.5,
	0.5, 0.5, 0.5,
	0.5, -0.5, 0.5,
	0.5, -0.5, 0.5,
	0.5, -0.5, -0.5,
	0.5, 0.5, -0.5,
};

float vertexUVOriginal[] = {
	//front
	1, 1,
	0, 1,
	0, 0,
	0, 0,
	1, 0,
	1, 1,

	//back
	0, 0,
	0, 1,
	1, 1,
	1, 1,
	1, 0,
	0, 0,

	//top
	1, 1,
	0, 1,
	0, 0,
	0, 0,
	1, 0,
	1, 1,

	//bottom
	1, 1,
	0, 1,
	0, 0,
	0, 0,
	1, 0,
	1, 1,

	//left
	1, 0,
	1, 1,
	0, 1,
	0, 1,
	0, 0,
	1, 0,

	//right
	1, 1,
	0, 1,
	0, 0,
	0, 0,
	1, 0,
	1, 1,
};

float vertexData[] = {
	//front
	0.5, 0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, -0.5, 0.5,
	0.5, -0.5, 0.5,

	//back
	-0.5, -0.5, -0.5,
	-0.5, 0.5, -0.5,
	0.5, 0.5, -0.5,
	0.5, -0.5, -0.5,

	//top
	-0.5, 0.5, -0.5,
	-0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, -0.5,

	//bottom
	0.5, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, -0.5,
	0.5, -0.5, -0.5,

	//left
	-0.5, -0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, 0.5, -0.5,
	-0.5, -0.5, -0.5,

	//right
	0.5, 0.5, -0.5,
	0.5, 0.5, 0.5,
	0.5, -0.5, 0.5,
	0.5, -0.5, -0.5,

	//grass
	0.5, 0.5, 0.5,
	-0.5, 0.5, -0.5,
	-0.5, -0.5, -0.5,
	0.5, -0.5, 0.5,

	-0.5, -0.5, -0.5,
	-0.5, 0.5, -0.5,
	0.5, 0.5, 0.5,
	0.5, -0.5, 0.5,

	-0.5, 0.5, 0.5,
	0.5, 0.5, -0.5,
	0.5, -0.5, -0.5,
	-0.5, -0.5, 0.5,

	-0.5, -0.5, 0.5,
	0.5, -0.5, -0.5,
	0.5, 0.5, -0.5,
	-0.5, 0.5, 0.5,
	
#pragma region leaves

	//moving leaves
	//front
	0.5, 0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, -0.5, 0.5,
	0.5, -0.5, 0.5,

	//back
	-0.5, -0.5, -0.5,
	-0.5, 0.5, -0.5,
	0.5, 0.5, -0.5,
	0.5, -0.5, -0.5,

	//top
	-0.5, 0.5, -0.5,
	-0.5, 0.5, 0.5,
	0.5, 0.5, 0.5,
	0.5, 0.5, -0.5,

	//bottom
	0.5, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, -0.5,
	0.5, -0.5, -0.5,

	//left
	-0.5, -0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, 0.5, -0.5,
	-0.5, -0.5, -0.5,

	//right
	0.5, 0.5, -0.5,
	0.5, 0.5, 0.5,
	0.5, -0.5, 0.5,
	0.5, -0.5, -0.5,
#pragma endregion

#pragma region torch and water

	//torch
	//front
	0.0625, 0.125, 0.0625,
	-0.0625, 0.125, 0.0625,
	-0.0625, -0.5, 0.0625,
	0.0625, -0.5, 0.0625,

	//back
	-0.0625, -0.5, -0.0625,
	-0.0625, 0.125, -0.0625,
	0.0625, 0.125, -0.0625,
	0.0625, -0.5, -0.0625,

	//top
	-0.0625, 0.125, -0.0625,
	-0.0625, 0.125, 0.0625,
	0.0625, 0.125, 0.0625,
	0.0625, 0.125, -0.0625,

	//bottom
	0.0625, -0.5, 0.0625,
	-0.0625, -0.5, 0.0625,
	-0.0625, -0.5, -0.0625,
	0.0625, -0.5, -0.0625,

	//left
	-0.0625, -0.5, 0.0625,
	-0.0625, 0.125, 0.0625,
	-0.0625, 0.125, -0.0625,
	-0.0625, -0.5, -0.0625,

	//right
	0.0625, 0.125, -0.0625,
	0.0625, 0.125, 0.0625,
	0.0625, -0.5, 0.0625,
	0.0625, -0.5, -0.0625,

	//water
	//front
	0.5, 0.375, 0.5,
	-0.5, 0.375, 0.5,
	-0.5, -0.5, 0.5,
	0.5, -0.5, 0.5,

	//back
	-0.5, -0.5, -0.5,
	-0.5, 0.375, -0.5,
	0.5, 0.375, -0.5,
	0.5, -0.5, -0.5,

	//top
	-0.5, 0.375, -0.5,
	-0.5, 0.375, 0.5,
	0.5, 0.375, 0.5,
	0.5, 0.375, -0.5,

	//bottom
	0.5, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, -0.5,
	0.5, -0.5, -0.5,

	//left
	-0.5, -0.5, 0.5,
	-0.5, 0.375, 0.5,
	-0.5, 0.375, -0.5,
	-0.5, -0.5, -0.5,

	//right
	0.5, 0.375, -0.5,
	0.5, 0.375, 0.5,
	0.5, -0.5, 0.5,
	0.5, -0.5, -0.5,

	//water taller down
	//front
	0.5, 0.375, 0.5,
	-0.5, 0.375, 0.5,
	-0.5, -0.625, 0.5,
	0.5, -0.625, 0.5,

	//back
	-0.5, -0.625, -0.5,
	-0.5, 0.375, -0.5,
	0.5, 0.375, -0.5,
	0.5, -0.625, -0.5,

	//left
	-0.5, -0.625, 0.5,
	-0.5, 0.375, 0.5,
	-0.5, 0.375, -0.5,
	-0.5, -0.625, -0.5,

	//right
	0.5, 0.375, -0.5,
	0.5, 0.375, 0.5,
	0.5, -0.625, 0.5,
	0.5, -0.625, -0.5,


	//water taller down and up
	//front
	0.5, 0.5, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, -0.625, 0.5,
	0.5, -0.625, 0.5,

	//back
	-0.5, -0.625, -0.5,
	-0.5, 0.5, -0.5,
	0.5, 0.5, -0.5,
	0.5, -0.625, -0.5,

	//left
	-0.5, -0.625, 0.5,
	-0.5, 0.5, 0.5,
	-0.5, 0.5, -0.5,
	-0.5, -0.625, -0.5,

	//right
	0.5, 0.5, -0.5,
	0.5, 0.5, 0.5,
	0.5, -0.625, 0.5,
	0.5, -0.625, -0.5,
#pragma endregion

#pragma region stairs

	//half bottom
		//front
		0.5, 0.0, 0.5,
		-0.5, 0.0, 0.5,
		-0.5, -0.5, 0.5,
		0.5, -0.5, 0.5,

		//back
		-0.5, -0.5, -0.5,
		-0.5, 0.0, -0.5,
		0.5, 0.0, -0.5,
		0.5, -0.5, -0.5,

		//left
		-0.5, -0.5, 0.5,
		-0.5, 0.0, 0.5,
		-0.5, 0.0, -0.5,
		-0.5, -0.5, -0.5,

		//right
		0.5, 0.0, -0.5,
		0.5, 0.0, 0.5,
		0.5, -0.5, 0.5,
		0.5, -0.5, -0.5,

	//top corner piece
	//front left side
		-0.5, 0.0, 0,
		-0.5, 0.5, 0,
		-0.5, 0.5, -0.5,
		-0.5, 0.0, -0.5,

	//front right side
		0.5, 0.5, -0.5,
		0.5, 0.5, 0,
		0.5, 0.0, 0,
		0.5, 0.0, -0.5,

	//front left side
		-0.5, 0.0, 0.5,
		-0.5, 0.5, 0.5,
		-0.5, 0.5, 0,
		-0.5, 0.0, 0,

		//front right side
		0.5, 0.5, 0,
		0.5, 0.5, 0.5,
		0.5, 0.0, 0.5,
		0.5, 0.0, 0,

	//side
		0, 0.5, 0.5,
		-0.5, 0.5, 0.5,
		-0.5, 0, 0.5,
		0, 0, 0.5,

		-0.5, 0, -0.5,
		-0.5, 0.5, -0.5,
		0, 0.5, -0.5,
		0, 0, -0.5,

	//side
		0.5, 0.5, 0.5,
		0, 0.5, 0.5,
		0, 0, 0.5,
		0.5, 0, 0.5,

		0, 0, -0.5,
		0, 0.5, -0.5,
		0.5, 0.5, -0.5,
		0.5, 0, -0.5,

	//top half
		//0
		-0.5, 0.5, -0.5,
		-0.5, 0.5, 0,
		0.5, 0.5, 0,
		0.5, 0.5, -0.5,

		//2
		-0.5, 0.5, 0,
		-0.5, 0.5, 0.5,
		0.5, 0.5, 0.5,
		0.5, 0.5, 0,

		//1
		-0.5, 0.5, -0.5,
		-0.5, 0.5, 0.5,
		0.0, 0.5, 0.5,
		0.0, 0.5, -0.5,


		//3
		0, 0.5, -0.5,
		0, 0.5, 0.5,
		0.5, 0.5, 0.5,
		0.5, 0.5, -0.5,

	//top half but bottom part
	//0
		-0.5, 0, -0.5,
		-0.5, 0, 0,
		0.5, 0, 0,
		0.5, 0, -0.5,

		//2
		-0.5, 0, 0,
		-0.5, 0, 0.5,
		0.5, 0, 0.5,
		0.5, 0, 0,

		//1
		-0.5, 0, -0.5,
		-0.5, 0, 0.5,
		0.0, 0, 0.5,
		0.0, 0, -0.5,


		//3
		0, 0, -0.5,
		0, 0, 0.5,
		0.5, 0, 0.5,
		0.5, 0, -0.5,

	//frontal middle top piece
		//front
		0.5, 0.5, 0,
		-0.5, 0.5, 0,
		-0.5, 0, 0,
		0.5, 0, 0,

		//back
		-0.5, 0, 0,
		-0.5, 0.5, 0,
		0.5, 0.5, 0,
		0.5, 0, 0,

		//right
		0, 0.5, -0.5,
		0, 0.5, 0.5,
		0, 0, 0.5,
		0, 0, -0.5,

		//left
		0, 0, 0.5,
		0, 0.5, 0.5,
		0, 0.5, -0.5,
		0, 0, -0.5,
#pragma endregion

#pragma region slabs

	//bottom slabs!!!
	//top
	-0.5, 0, -0.5,
	-0.5, 0, 0.5,
	0.5, 0, 0.5,
	0.5, 0, -0.5,

	//half top
	//front
			0.5, 0.5, 0.5,
			-0.5, 0.5, 0.5,
			-0.5, 0.0, 0.5,
			0.5, 0.0, 0.5,

			//back
			-0.5, 0.0, -0.5,
			-0.5, 0.5, -0.5,
			0.5, 0.5, -0.5,
			0.5, 0.0, -0.5,

			//left
			-0.5, 0.0, 0.5,
			-0.5, 0.5, 0.5,
			-0.5, 0.5, -0.5,
			-0.5, 0.0, -0.5,

			//right
			0.5, 0.5, -0.5,
			0.5, 0.5, 0.5,
			0.5, 0.0, 0.5,
			0.5, 0.0, -0.5,

		//bottom moddle part
		0.5, 0.0, 0.5,
		-0.5, 0.0, 0.5,
		-0.5, 0.0, -0.5,
		0.5, 0.0, -0.5,
#pragma endregion


	//walls inner part

		//front
		0.5, 0.5, 0,
		-0.5, 0.5, 0,
		-0.5, -0.5, 0,
		0.5, -0.5, 0,

		//back
		-0.5, -0.5, -0,
		-0.5, 0.5, -0,
		0.5, 0.5, -0,
		0.5, -0.5, -0,

		//left
		-0, -0.5, 0.5,
		-0, 0.5, 0.5,
		-0, 0.5, -0.5,
		-0, -0.5, -0.5,

		//right
		0, 0.5, -0.5,
		0, 0.5, 0.5,
		0, -0.5, 0.5,
		0, -0.5, -0.5,


	//walls bottom half
	//0
	0.5, -0.5, -0.5,
	0.5, -0.5, 0,
	-0.5, -0.5, 0,
	-0.5, -0.5, -0.5,

	//2
	0.5, -0.5, 0,
	0.5, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, 0,

	//1
	0.0, -0.5, -0.5,
	0.0, -0.5, 0.5,
	-0.5, -0.5, 0.5,
	-0.5, -0.5, -0.5,


	//3
	0.5, -0.5, -0.5,
	0.5, -0.5, 0.5,
	0, -0.5, 0.5,
	0, -0.5, -0.5,

	//walls side
		//front left
		0.0, 0.5, 0.5,
		-0.5, 0.5, 0.5,
		-0.5, -0.5, 0.5,
		0.0, -0.5, 0.5,


		//front right
		0.5, 0.5, 0.5,
		0.0, 0.5, 0.5,
		0.0, -0.5, 0.5,
		0.5, -0.5, 0.5,


		//back left
		-0.5, -0.5, -0.5,
		-0.5, 0.5, -0.5,
		0.0, 0.5, -0.5,
		0.0, -0.5, -0.5,
				
		//back right
		0, -0.5, -0.5,
		0, 0.5, -0.5,
		0.5, 0.5, -0.5,
		0.5, -0.5, -0.5,


		//left front
		-0.5, -0.5, 0.0,
		-0.5, 0.5, 0.0,
		-0.5, 0.5, -0.5,
		-0.5, -0.5, -0.5,

		//left back
		-0.5, -0.5, 0.5,
		-0.5, 0.5, 0.5,
		-0.5, 0.5, 0.0,
		-0.5, -0.5, 0.0,

		//right front
		0.5, 0.5, -0.5,
		0.5, 0.5, 0.0,
		0.5, -0.5, 0.0,
		0.5, -0.5, -0.5,

		//right back
		0.5, 0.5, 0.0,
		0.5, 0.5, 0.5,
		0.5, -0.5, 0.5,
		0.5, -0.5, 0.0,


	#pragma region LOD1
		//lod 1

			//front
			1.5, 1.5, 1.5,
			-0.5, 1.5, 1.5,
			-0.5, -0.5, 1.5,
			1.5, -0.5, 1.5,

			//back
			-0.5, -0.5, -0.5,
			-0.5, 1.5, -0.5,
			1.5, 1.5, -0.5,
			1.5, -0.5, -0.5,

			//top
			-0.5, 1.5, -0.5,
			-0.5, 1.5, 1.5,
			1.5, 1.5, 1.5,
			1.5, 1.5, -0.5,

			//bottom
			1.5, -0.5, 1.5,
			-0.5, -0.5, 1.5,
			-0.5, -0.5, -0.5,
			1.5, -0.5, -0.5,

			//left
			-0.5, -0.5, 1.5,
			-0.5, 1.5, 1.5,
			-0.5, 1.5, -0.5,
			-0.5, -0.5, -0.5,

			//right
			1.5, 1.5, -0.5,
			1.5, 1.5, 1.5,
			1.5, -0.5, 1.5,
			1.5, -0.5, -0.5,

		#pragma endregion



		//top for decals
		-0.5, 0.5, -0.5,
		-0.5, 0.5, 0.5,
		0.5, 0.5, 0.5,
		0.5, 0.5, -0.5,

		//top for decals
		-0.5, 0.5, -0.5,
		-0.5, 0.5, 0.5,
		0.5, 0.5, 0.5,
		0.5, 0.5, -0.5,

		//top for decals
		-0.5, 0.5, -0.5,
		-0.5, 0.5, 0.5,
		0.5, 0.5, 0.5,
		0.5, 0.5, -0.5,

		//top for decals
		-0.5, 0.5, -0.5,
		-0.5, 0.5, 0.5,
		0.5, 0.5, 0.5,
		0.5, 0.5, -0.5,

};

float vertexUV[] = {
	//front
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//back
	0, 0,
	0, 1,
	1, 1,
	1, 0,

	//top
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//bottom
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//left
	1, 0,
	1, 1,
	0, 1,
	0, 0,

	//right
	1, 1,
	0, 1,
	0, 0,
	1, 0,

#pragma region other

	//grass
	//front
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	0, 0,
	0, 1,
	1, 1,
	1, 0,

	1, 1,
	0, 1,
	0, 0,
	1, 0,

	1, 0,
	0, 0,
	0, 1,
	1, 1,


	//leaves////////////////////////
	//front
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//back
	0, 0,
	0, 1,
	1, 1,
	1, 0,

	//top
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//bottom
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//left
	1, 0,
	1, 1,
	0, 1,
	0, 0,

	//right
	1, 1,
	0, 1,
	0, 0,
	1, 0,


	//torches ///////
	//front
	0.5625, 0.625,
	0.4375, 0.625,
	0.4375, 0,
	0.5625, 0,

	//back
	0.4375, 0,
	0.4375, 0.625,
	0.5625, 0.625,
	0.5625, 0,

	//top
	0.5625, 0.625,
	0.4375, 0.625,
	0.4375, 0.5,
	0.5625, 0.5,

	//bottom
	0.5625, 0.125,
	0.4375, 0.125,
	0.4375, 0,
	0.5625, 0,

	//left
	0.5625, 0,
	0.5625, 0.625,
	0.4375, 0.625,
	0.4375, 0,

	//right
	0.5625, 0.625,
	0.4375, 0.625,
	0.4375, 0,
	0.5625, 0,

	//water
	//front
	1, 0.875,
	0, 0.875,
	0, 0,
	1, 0,

	//back
	0, 0,
	0, 0.875,
	1, 0.875,
	1, 0,

	//top
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//bottom
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//left
	1, 0,
	1, 0.875,
	0, 0.875,
	0, 0,

	//right
	1, 0.875,
	0, 0.875,
	0, 0,
	1, 0,


	//water taller down,
	//front
	1, 0.875,
	0, 0.875,
	0, 0,
	1, 0,

	//back
	0, 0,
	0, 0.875,
	1, 0.875,
	1, 0,
	//left
	1, 0,
	1, 0.875,
	0, 0.875,
	0, 0,

	//right
	1, 0.875,
	0, 0.875,
	0, 0,
	1, 0,


	//
	//front
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//back
	0, 0,
	0, 1,
	1, 1,
	1, 0,

	//left
	1, 0,
	1, 1,
	0, 1,
	0, 0,

	//right
	1, 1,
	0, 1,
	0, 0,
	1, 0,
#pragma endregion

#pragma region stairs


	//half botom
		//front
		1, 0.5,
		0, 0.5,
		0, 0,
		1, 0,

		//back
		0, 0,
		0, 0.5,
		1, 0.5,
		1, 0,

		//left
		1, 0,
		1, 0.5,
		0, 0.5,
		0, 0,

		//right
		1, 0.5,
		0, 0.5,
		0, 0,
		1, 0,

	//top corner piece
	//front left side
		0.5, 0.5,
		0.5, 1,
		0, 1,
		0, 0.5,

	//front right side
		0.5, 1,
		0, 1,
		0, 0.5,
		0.5, 0.5,

		//top corner piece
	//front left side
	1, 0.5,
	1, 1,
	0.5, 1,
	0.5, 0.5,
	
	//front right side
	1, 1,
	0.5, 1,
	0.5, 0.5,
	1, 0.5,

	//side
	0.5, 1,
	0, 1,
	0, 0.5,
	0.5, 0.5,

	//side
	0, 0.5,
	0, 1,
	0.5, 1,
	0.5, 0.5,

	//side
	1, 1,
	0.5, 1,
	0.5, 0.5,
	1, 0.5,

	//side
	0.5, 0.5,
	0.5, 1,
	1, 1,
	1, 0.5,

	//top half stairt
	1, 1,
	0.5, 1,
	0.5, 0,
	1, 0,

	0.5, 1,
	0, 1,
	0, 0,
	0.5, 0,

	1, 0.5,
	0, 0.5,
	0, 0,
	1, 0,

	1, 1,
	0, 1,
	0, 0.5,
	1, 0.5,


	//top half stairt but bottom part
		1, 1,
		0.5, 1,
		0.5, 0,
		1, 0,

		0.5, 1,
		0, 1,
		0, 0,
		0.5, 0,

		1, 0.5,
		0, 0.5,
		0, 0,
		1, 0,

		1, 1,
		0, 1,
		0, 0.5,
		1, 0.5,

	//frontal middle top piece
		//front
		1, 1,
		0, 1,
		0, 0.5,
		1, 0.5,

		//back
		0, 0.5,
		0, 1,
		1, 1,
		1, 0.5,

		//right
		1, 1,
		0.5, 1,
		0.5, 0.5,
		1, 0.5,

		//left
		0.5, 0.5,
		0.5, 1,
		0, 1,
		0, 0.5,
#pragma endregion


	//bottom slabs!!!
	//top
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	//half top
	//front
		1, 1.0,
		0, 1.0,
		0, 0.5,
		1, 0.5,

		//back
		0, 0.5,
		0, 1.0,
		1, 1.0,
		1, 0.5,

		//left
		1, 0.5,
		1, 1.0,
		0, 1.0,
		0, 0.5,

		//right
		1, 1.0,
		0, 1.0,
		0, 0.5,
		1, 0.5,

	//bottom
	1, 1,
	0, 1,
	0, 0,
	1, 0,

	
	//walls inner part
		//front
		1, 1,
		0, 1,
		0, 0,
		1, 0,

		//back
		0, 0,
		0, 1,
		1, 1,
		1, 0,

		//left
		1, 0,
		1, 1,
		0, 1,
		0, 0,

		//right
		1, 1,
		0, 1,
		0, 0,
		1, 0,

	//bottom half walls

	1, 0,
	0.5, 0,
	0.5, 1,
	1, 1,

	0.5, 0,
	0, 0,
	0, 1,
	0.5, 1,

	1, 0,
	0, 0,
	0, 0.5,
	1, 0.5,

	1, 0.5,
	0, 0.5,
	0, 1,
	1, 1,


	//walls side
	//front left
	0.5, 1,
	0, 1,
	0, 0,
	0.5, 0,

	//front right
	1, 1,
	0.5, 1,
	0.5, 0,
	1, 0,

	//back left
	0, 0,
	0, 1,
	0.5, 1,
	0.5, 0,

	//back right
	0.5, 0,
	0.5, 1,
	1, 1,
	1, 0,

	//left front
	0.5, 0,
	0.5, 1,
	0, 1,
	0, 0,

	//left back
	1, 0,
	1, 1,
	0.5, 1,
	0.5, 0,


	//right front
	0.5, 1,
	0, 1,
	0, 0,
	0.5, 0,


	//right back
	1, 1,
	0.5, 1,
	0.5, 0,
	1, 0,

	#pragma region lods

		//front
		2, 1, //
		0, 1, //
		0, -1,// 
		2, -1,//

		//back
		0, -1,//
		0, 1, //
		2, 1, //
		2, -1,//

		//top
		2, 1, //
		0, 1, //
		0, -1,//
		2, -1,//

		//bottom
		2, 1, //
		0, 1, //
		0, -1,//
		2, -1,//

		//left
		2, -1,//
		2, 1, //
		0, 1, //
		0, -1,//

		//right
		2, 1, //
		0, 1, //
		0, -1,//
		2, -1,//



	#pragma endregion


	//top for decals
	1, 0,
	1, 1,
	0, 1,
	0, 0,

	0, 0,
	1, 0,
	1, 1,
	0, 1,

	0, 1,
	0, 0,
	1, 0,
	1, 1,

	1, 1,
	0, 1,
	0, 0,
	1, 0,

};


void Renderer::recreateBlocksTexturesBuffer(BlocksLoader &blocksLoader)
{
	// The slot table; the WebGPU world turns it into a texture array and a
	// slot -> layer table (milestone 7).
	wgpuWorld::setBlockTextures(blocksLoader.gpuIds, blocksLoader.spannedTextures);
}

void Renderer::renderAllBlocksUiTextures(BlocksLoader &blocksLoader, ModelsManager &modelsManager)
{
	// Each block's icon, drawn by wgpuWorld into a wgpu2d render target: an
	// orthographic camera from (3, 3, 3), the block's geometry range and its
	// textures per face. Not before create: the texture packs load (and call
	// this) before the block geometry exists, and create's caller draws them.
	if (!created) { return; }

	auto view = glm::lookAt(glm::vec3{3, 3, 3}, {0, 0, 0}, {0, 1, 0});
	const int TEXTURE_SIZE = 64;
	blocksLoader.blockUiTextures.resize(BlocksCount);

	auto cameraFor = [&](float distFromCamera, float yOffset)
	{
		return glm::ortho(-distFromCamera, distFromCamera, -distFromCamera + yOffset, distFromCamera + yOffset, 0.1f, 10.f) * view;
	};
	// setDistance's camera, for furniture.
	auto cameraForSize = [&](glm::vec3 size)
	{
		float s = std::max(size.x, std::max(size.y, size.z));
		s = glm::clamp(s, 0.4f, 2.f);
		return cameraFor(s - 0.1f * s, -0.1f);
	};

	std::vector<wgpuWorld::BlockIcon> icons;
	for (BlockType i = 1; i < BlocksCount; i++)
	{
		wgpuWorld::BlockIcon icon;
		bool furniture = false;
		if (isBlockMesh(i))
		{
			icon.firstVertex = 0;
			icon.vertexCount = 36;
			icon.viewProjection = cameraFor(0.78f, 0.f);
		}
		else
		{
			int index = -1;
			if (isStairsMesh(i)) { index = ModelsManager::stairsModel; }
			else if (isSlabMesh(i)) { index = ModelsManager::slabModel; }
			else if (isWallMesh(i)) { index = ModelsManager::wallModel; }
			else if (isDecorativeFurniture(i) || isFenceMesh(i)) { index = getDefaultBlockShapeForFurniture(i); furniture = true; }
			if (index < 0) { continue; }

			icon.oneTexture = true;
			icon.firstVertex = blockGeometryForEntities[index].startIndex / 8;
			icon.vertexCount = blockGeometryForEntities[index].componentCount;
			icon.viewProjection = furniture
				? cameraForSize(modelsManager.blockModels[index].getDimensions())
				: cameraFor(0.78f, 0.f);
		}

		for (int j = 0; j < 6; j++)
		{
			const unsigned slot = (unsigned)getGpuIdIndexForBlock(i, j);
			if (j < 4) { icon.slotsA[j] = slot; } else { icon.slotsB[j - 4] = slot; }
		}

		// Nearest filtering, as GL's icon textures. An icon from an
		// earlier call (a reload) is released first; GL's path leaks it.
		blocksLoader.blockUiTextures[i].cleanup();
		gl2d::FrameBuffer target;
		target.create(TEXTURE_SIZE, TEXTURE_SIZE, true);
		blocksLoader.blockUiTextures[i] = target.texture;
		icon.target = target.texture.id;
		icons.push_back(icon);
	}

	wgpuWorld::renderBlockIcons(icons, TEXTURE_SIZE);
}

void Renderer::create(ModelsManager &modelsManager)
{
	reloadShaders();

	recreateBlockGeometryData(modelsManager);

	// The entity geometry: the cube, then every block model, as position,
	// normal and uv per vertex.
	std::vector<float> data;
	data.reserve(2000);
	for (int face = 0; face < 6; face++)
	{


		glm::vec3 normal = {};
		glm::vec3 positions[3] = {};

		for (int i = 0; i < 3; i++)
		{	
			positions[i].x = vertexDataOriginal[i * 3 + face * 3 * 6 + 0];
			positions[i].y = vertexDataOriginal[i * 3 + face * 3 * 6 + 1];
			positions[i].z = vertexDataOriginal[i * 3 + face * 3 * 6 + 2];
		}

		glm::vec3 edge1 = positions[1] - positions[0];
		glm::vec3 edge2 = positions[2] - positions[0];
		normal = glm::normalize(glm::cross(edge1, edge2));

		for (int i = 0; i < 6; i++)
		{

			data.push_back(vertexDataOriginal[i * 3 + face * 3 * 6 + 0]);
			data.push_back(vertexDataOriginal[i * 3 + face * 3 * 6 + 1]);
			data.push_back(vertexDataOriginal[i * 3 + face * 3 * 6 + 2]);

			data.push_back(normal.x);
			data.push_back(normal.y);
			data.push_back(normal.z);

			data.push_back(vertexUVOriginal[i * 2 + face * 2 * 6 + 0]);
			data.push_back(vertexUVOriginal[i * 2 + face * 2 * 6 + 1]);

		}
	}


	auto addModelData = [&](BlockModel &blockModel, int index)
	{
		int startPos = data.size();
		int verticesCount = 0;

		for (int i = 0; i < blockModel.vertices.size() / (3 * 4); i++)
		{

			glm::vec3 a = glm::vec3(blockModel.vertices[i * 12 + 0], blockModel.vertices[i * 12 + 1], blockModel.vertices[i * 12 + 2]);
			glm::vec3 b = glm::vec3(blockModel.vertices[i * 12 + 3], blockModel.vertices[i * 12 + 4], blockModel.vertices[i * 12 + 5]);
			glm::vec3 c = glm::vec3(blockModel.vertices[i * 12 + 6], blockModel.vertices[i * 12 + 7], blockModel.vertices[i * 12 + 8]);
			glm::vec3 d = glm::vec3(blockModel.vertices[i * 12 + 9], blockModel.vertices[i * 12 + 10], blockModel.vertices[i * 12 + 11]);

			glm::vec3 edge1 = b - a;
			glm::vec3 edge2 = c - a;
			glm::vec3 normal = glm::normalize(glm::cross(edge1, edge2));
			
			glm::vec2 uvA = glm::vec2(blockModel.uvs[i * 8 + 0], blockModel.uvs[i * 8 + 1]);
			glm::vec2 uvB = glm::vec2(blockModel.uvs[i * 8 + 2], blockModel.uvs[i * 8 + 3]);
			glm::vec2 uvC = glm::vec2(blockModel.uvs[i * 8 + 4], blockModel.uvs[i * 8 + 5]);
			glm::vec2 uvD = glm::vec2(blockModel.uvs[i * 8 + 6], blockModel.uvs[i * 8 + 7]);

			auto addVertex = [&](glm::vec3 v, glm::vec2 uv) 
			{
				data.push_back(v.x);
				data.push_back(v.y);
				data.push_back(v.z);
				data.push_back(normal.x);
				data.push_back(normal.y);
				data.push_back(normal.z);
				data.push_back(uv.x);
				data.push_back(uv.y);
			};

			addVertex(a, uvA);
			addVertex(b, uvB);
			addVertex(c, uvC);

			addVertex(a, uvA);
			addVertex(c, uvC);
			addVertex(d, uvD);

			verticesCount += 6;
		}

		//for (int i = 0; i < blockModel.vertices.size()/3; i++)
		//{
		//	float x = blockModel.vertices[i * 3];
		//	float y = blockModel.vertices[i * 3 + 1];
		//	float z = blockModel.vertices[i * 3 + 2];
		//
		//	float uvX = blockModel.uvs[i * 2];
		//	float uvY = blockModel.uvs[i * 2 + 1];
		//	glm::vec3 normal = {0,1,0}; //todo
		//	//glm::vec3 edge1 = positions[1] - positions[0];
		//	//glm::vec3 edge2 = positions[2] - positions[0];
		//	//normal = glm::normalize(glm::cross(edge1, edge2));
		//
		//	data.push_back(x);
		//	data.push_back(y);
		//	data.push_back(z);
		//
		//	data.push_back(normal.x);
		//	data.push_back(normal.y);
		//	data.push_back(normal.z);
		//
		//	data.push_back(uvX);
		//	data.push_back(uvY);
		//}
		
		blockGeometryForEntities[index].startIndex = startPos;
		blockGeometryForEntities[index].componentCount = verticesCount;
	};

	for (int i = 0; i < ModelsManager::BLOCK_MODELS_COUNT; i++)
	{
		addModelData(modelsManager.blockModels[i], i);
	}

	// The cube (the first 36 vertices) for dropped block items, and all of
	// it for the inventory's block icons.
	wgpuWorld::setBlockCubeGeometry(data);

#pragma endregion

	created = true;
}

void Renderer::recreateBlockGeometryData(ModelsManager &modelsManager)
{

	auto noRotation = [&](int i)
	{
		return 
			i == ModelsManager::fence ||
			i == ModelsManager::fenceFront ||
			i == ModelsManager::fenceBack ||
			i == ModelsManager::fenceLeft ||
			i == ModelsManager::fenceRight
			;
	};

	static std::vector<float> newVertexData;
	newVertexData.clear();
	int newVertexDataSize = sizeof(vertexData) / sizeof(vertexData[0]);
	int newVertexUVSize = sizeof(vertexUV) / sizeof(vertexUV[0]);
	for (int i = 0; i < ModelsManager::BLOCK_MODELS_COUNT; i++)
	{

		if (noRotation(i))
		{
			newVertexDataSize += modelsManager.blockModels[i].vertices.size();
			newVertexUVSize += modelsManager.blockModels[i].uvs.size();
		}
		else
		{
			newVertexDataSize += modelsManager.blockModels[i].vertices.size() * 4;
			newVertexUVSize += modelsManager.blockModels[i].uvs.size() * 4;
		}

	}

	newVertexData.resize(newVertexDataSize);

	static std::vector<float> newUVData;
	newUVData.clear();
	newUVData.resize(newVertexUVSize);


	memcpy(newVertexData.data(), vertexData, sizeof(vertexData));
	memcpy(newUVData.data(), vertexUV, sizeof(vertexUV));

	int currentVertexIndex = sizeof(vertexData) / 4;
	int currentUvIndex = sizeof(vertexUV) / 4;

	//special rotate rotates the block a few degrees each time instead of 90. for crates
	auto addGeometry = [&](BlockGeometryIndex &b, BlockModel &model, bool specialRotate, bool noRotate)
	{
		b.startIndex = currentVertexIndex / (sizeof(float) * 3);
		b.componentCount = model.vertices.size() / (3 * 4);
		memcpy(newVertexData.data() + currentVertexIndex, model.vertices.data(),
			model.vertices.size() * sizeof(float));

		float rotationAngle = glm::radians(20.0f);
		glm::mat2 rotationMatrix = glm::mat2(
			glm::cos(rotationAngle), -glm::sin(rotationAngle),
			glm::sin(rotationAngle), glm::cos(rotationAngle)
		);

		size_t vSize = model.vertices.size();
		if(!noRotate)
		for (int i = 0; i < vSize / 3; i++)
		{
			glm::vec2 angle = {};
			angle.x = model.vertices[i * 3 + 0];
			float y = model.vertices[i * 3 + 1];
			angle.y = model.vertices[i * 3 + 2];

			if (specialRotate)
			{
				angle = rotationMatrix * angle;
			}
			else
			{
				angle = glm::vec2(angle.y, -angle.x); //rotate 90
			}
			newVertexData[currentVertexIndex + model.vertices.size() + i * 3 + 0] = angle.x;
			newVertexData[currentVertexIndex + model.vertices.size() + i * 3 + 1] = y;
			newVertexData[currentVertexIndex + model.vertices.size() + i * 3 + 2] = angle.y;

			if (specialRotate)
			{
				angle = rotationMatrix * angle;
			}
			else
			{
				angle = glm::vec2(angle.y, -angle.x); //rotate 90
			}
			newVertexData[currentVertexIndex + model.vertices.size()*2 + i * 3 + 0] = angle.x;
			newVertexData[currentVertexIndex + model.vertices.size()*2 + i * 3 + 1] = y;
			newVertexData[currentVertexIndex + model.vertices.size()*2 + i * 3 + 2] = angle.y;

			if (specialRotate)
			{
				angle = rotationMatrix * angle;
			}
			else
			{
				angle = glm::vec2(angle.y, -angle.x); //rotate 90
			}
			newVertexData[currentVertexIndex + model.vertices.size()*3 + i * 3 + 0] = angle.x;
			newVertexData[currentVertexIndex + model.vertices.size()*3 + i * 3 + 1] = y;
			newVertexData[currentVertexIndex + model.vertices.size()*3 + i * 3 + 2] = angle.y;
		}

		if (noRotate)
		{
			currentVertexIndex += model.vertices.size();
		}
		else
		{
			currentVertexIndex += model.vertices.size() * 4;
		}

		memcpy(newUVData.data() + currentUvIndex, model.uvs.data(),
			model.uvs.size() * sizeof(float));

		if (!noRotate)
		{
			memcpy(newUVData.data() + currentUvIndex + model.uvs.size(), model.uvs.data(),
				model.uvs.size() * sizeof(float));
			memcpy(newUVData.data() + currentUvIndex + model.uvs.size() * 2, model.uvs.data(),
				model.uvs.size() * sizeof(float));
			memcpy(newUVData.data() + currentUvIndex + model.uvs.size() * 3, model.uvs.data(),
				model.uvs.size() * sizeof(float));
		}


		if (noRotate)
		{
			currentUvIndex += model.uvs.size();
		}
		else
		{
			currentUvIndex += model.uvs.size() * 4;
		}

	};

	for (int i = 0; i < ModelsManager::BLOCK_MODELS_COUNT; i++)
	{
		addGeometry(blockGeometry[i], modelsManager.blockModels[i], 
			i == ModelsManager::crateModel, noRotation(i));
	}

	wgpuWorld::setBlockGeometry(newVertexData, newUVData);
}

void Renderer::reloadShaders()
{
	// Milestone 20: the world's WGSL.
	wgpuWorld::reloadShaders();
}

// Milestone 13: one sun shadow cascade's matrix, as renderShadow's
// renderOneFrustum builds it, kept in double throughout (GL stores a float
// mat4 of world coordinates). OpenGL clip space, world coordinates;
// wgpuWorld makes it camera-relative and 0..1 deep. The corners of the
// camera frustum from GL depth -1 to zEnd, turned so the sun is +z, grown to
// a cube as long as their diagonal (the box keeps its size as the camera
// turns), then an orthographic box 500 blocks deeper toward and away from
// the sun, so blockers outside the view still cast.
//
// One difference from GL: GL starts the running maximum at
// -numeric_limits<double>::min(), the smallest positive double negated
// (about -2e-308, not the most negative), so its box always reaches back to
// 0 on each axis: around the world origin that changes little, but a few
// hundred blocks out it spreads the first cascade's 2048 texels over
// hundreds of blocks. Here the maximum starts at lowest().
static glm::dmat4 sunCascadeMatrix(Camera &c, glm::vec3 sunPos, double zEnd)
{
	const double zStart = -1;
	const double depthMargin = 500; // GL's farPlane

	const glm::dmat4 inverseViewProjection = glm::inverse(c.getViewProjectionWithPositionMatrixDouble());
	const glm::dquat lightRotation = glm::conjugate(glm::dquat(glm::rotation(glm::vec3(0, 0, 1), sunPos)));

	glm::dvec3 boxMin(std::numeric_limits<double>::max());
	glm::dvec3 boxMax(std::numeric_limits<double>::lowest());
	for (int x = -1; x <= 1; x += 2)
		for (int y = -1; y <= 1; y += 2)
			for (int z = -1; z <= 1; z += 2)
			{
				glm::dvec4 corner = inverseViewProjection * glm::dvec4(x, y, z == -1 ? zStart : zEnd, 1.0);
				corner /= corner.w;
				const glm::dvec3 turned = lightRotation * glm::dvec3(corner);
				boxMin = glm::min(boxMin, turned);
				boxMax = glm::max(boxMax, turned);
			}

	// Each axis as long as the diagonal, about its own center.
	const double diagonal = glm::distance(boxMax, boxMin);
	const glm::dvec3 center = (boxMin + boxMax) * 0.5;
	boxMin = center - diagonal * 0.5;
	boxMax = center + diagonal * 0.5;

	const glm::dmat4 lightProjection = glm::ortho(boxMin.x, boxMax.x, boxMin.y, boxMax.y,
		-(boxMax.z + depthMargin), -(boxMin.z - depthMargin));
	return lightProjection * glm::mat4_cast(lightRotation);
}

// Milestone 14: SkyBoxLoaderAndDrawer::drawBefore's choice of two skies and
// their mix, as one weight per sky (day, twilight, night).
static glm::vec3 skyWeightsFor(float timeOfDay)
{
	if (timeOfDay > 1.f) { timeOfDay -= (int)timeOfDay; }
	enum { Day, Twilight, Night };
	const float bias = 2;
	int first = Twilight, second = Day;
	float mix = 0;
	if (timeOfDay <= 0.25f)
	{
		first = Twilight; second = Day;
		mix = std::powf(timeOfDay / 0.25f, 1.f / bias);
	}
	else if (timeOfDay <= 0.50f)
	{
		first = Day; second = Twilight;
		mix = std::powf((timeOfDay - 0.25f) / 0.25f, bias);
	}
	else if (timeOfDay <= 0.75f)
	{
		first = Twilight; second = Night;
		mix = std::powf((timeOfDay - 0.50f) / 0.25f, 1.f / bias);
	}
	else
	{
		first = Night; second = Twilight;
		mix = std::powf((timeOfDay - 0.75f) / 0.25f, bias);
	}
	glm::vec3 weights(0.f);
	weights[first] += 1.f - mix;
	weights[second] += mix;
	return weights;
}

void Renderer::renderFromBakedData(ChunkSystem &chunkSystem, Camera &c,
	ProgramData &programData, BlocksLoader &blocksLoader, ClientEntityManager &entityManager, 
	ModelsManager &modelsManager, 
	AdaptiveExposure &adaptiveExposure,
	bool showLightLevels, glm::dvec3 pointPos, bool underWater, int screenX, int screenY,
	float deltaTime, float dayTime, 
	std::uint64_t currentSkinTextureId, bool &playerClicked, float playerRunning,
	BoneTransform &playerHand, int currentHeldItemIndex,
	bool showHand, std::unordered_map<std::uint64_t, PlayerConnectionData> &playersConnectionData
	)
{
	shadingSettings.exposure = adaptiveExposure.currentExposure
		+ getShadingSettings().exposure;

	if (dayTime > 1.f)
		{ dayTime -= (int)dayTime; }
	glm::vec3 mainLightPosition = sunPos;
	if (dayTime > 0.50)
	{
		mainLightPosition = -sunPos;
	}

	glm::vec3 posFloat = {};
	glm::ivec3 posInt = {};
	glm::ivec3 blockPosition = from3DPointToBlock(c.position);
	c.decomposePosition(posFloat, posInt);

	auto viewMatrix = c.getViewMatrix();
	auto projectionMatrix = c.getProjectionMatrix();
	auto vp = projectionMatrix * viewMatrix;

	float sunTwilight = 0;
	{
		if (dayTime <= 0.25)
		{
			sunTwilight = std::powf(1 - (dayTime / 0.25f), 3.f);
		}
		else if (dayTime <= 0.50)
		{
			sunTwilight = std::powf(((dayTime-0.25f) / 0.25f), 3.f);

		}
		else if (dayTime <= 0.75)
		{
			sunTwilight = 1;
		}
		else if (dayTime <= 1.f)
		{
			sunTwilight = 1;
		}


	}

	auto doDayLightCalculations = [dayTime](auto dayValue, auto nightValue, auto twilightValue)
	{

		auto firstLight = dayValue;
		auto secondLight = dayValue;
		float mix = 0;

		float bias = 2;

		if (dayTime <= 0.25)
		{
			firstLight = twilightValue;
			secondLight = dayValue;
			mix = std::powf(dayTime / 0.25f, 1.f / bias);
		}
		else if (dayTime <= 0.50)
		{
			firstLight = dayValue;
			secondLight = twilightValue;
			mix = std::powf((dayTime - 0.25f) / 0.25f, bias);
		}
		else if (dayTime <= 0.75)
		{
			firstLight = twilightValue;
			secondLight = nightValue;
			mix = std::powf((dayTime - 0.50f) / 0.25f, 1.f / bias);
		}
		else if (dayTime <= 1.f)
		{
			firstLight = nightValue;
			secondLight = twilightValue;
			mix = std::powf((dayTime - 0.75f) / 0.25f, bias);
		}

		return glm::mix(firstLight, secondLight, mix);
		//return (firstLight * (1.f - mix)) + (secondLight * mix);
	};


	//determine sky light intensity.
	int skyLightIntensity = 15;
	{
		float daySkyLight = 15;
		float nightSkyLight = 3;
		float twilightLight = 9;

		// Milestone G1: clouds and rain take sky light levels off.
		skyLightIntensity = std::roundf(doDayLightCalculations(daySkyLight, nightSkyLight, twilightLight)
			- weather.skyLightDrop);
		skyLightIntensity = std::clamp(skyLightIntensity, 0, 15);
		
		//std::cout << skyLightIntensity << "\n";
	}

	glm::vec3 sunLightColor = {1,1,1};
	{

		glm::vec3 daySkyLight(1.5f);
		glm::vec3 nightSkyLight = (glm::vec3(47, 135, 244) / 255.f) * 0.2f;
		glm::vec3 twilightLight = (glm::vec3(255, 159, 107) / 255.f) * 0.3f;
		
		sunLightColor = doDayLightCalculations(daySkyLight, nightSkyLight, twilightLight)
			* weather.sunStrength;
	}

	glm::vec3 ambientColor = {1,1,1};
	{
		glm::vec3 daySkyLight(1.0f);
		glm::vec3 nightSkyLight = (glm::vec3(47, 135, 244) / 255.f) * 0.6f;
		glm::vec3 twilightLight = (glm::vec3(255, 159, 107) / 255.f) * 0.7f;

		ambientColor = doDayLightCalculations(daySkyLight, nightSkyLight, twilightLight)
			* weather.ambientStrength;
	}

	
#pragma region frustum culling and sorting

	//chunk vector copy has only valid non culled chunks!
	std::vector<Chunk*> chunkVectorCopy;
	chunkVectorCopy.reserve(chunkSystem.loadedChunks.size());

	FrustumVolume cameraFrustum(c.getViewProjectionWithPositionMatrixDouble());

	for (auto c : chunkSystem.loadedChunks)
	{
		if (c)
		{
			bool culled = 0;
			c->setCulled(0);

			if (c->isDontDrawYet())
			{
				culled = 1;
			}else
			if (frustumCulling)
			{
				AABBVolume chunkAABB;

				chunkAABB.minVertex = {c->data.x * CHUNK_SIZE, 0, c->data.z * CHUNK_SIZE};
				chunkAABB.maxVertex = {(c->data.x + 1) * CHUNK_SIZE, CHUNK_HEIGHT + 1, (c->data.z + 1) * CHUNK_SIZE};

				if (!CheckFrustumVsAABB(cameraFrustum, chunkAABB))
				{
					c->setCulled(1);
					culled = 1;
				}
			};

			if (!culled)
			{
				chunkVectorCopy.push_back(c);
			}
		}
	}

	//sort chunks
	{
		std::sort(chunkVectorCopy.begin(), chunkVectorCopy.end(),
			[x = divideChunk(blockPosition.x), z = divideChunk(blockPosition.z)](Chunk *b, Chunk *a)
		{
			if (a == nullptr) { return false; }
			if (b == nullptr) { return true; }

			int ax = a->data.x - x;
			int az = a->data.z - z;

			int bx = b->data.x - x;
			int bz = b->data.z - z;

			unsigned long reza = ax * ax + az * az;
			unsigned long rezb = bx * bx + bz * bz;

			return reza < rezb;
		}
		);
	}

#pragma endregion

	// The world: the chunks that survived the frustum culling above, the
	// dropped block items, and how to light them.
	// Dropped block items: a 0.4 cube 0.2 above the item (as GL's block entity
	// shader placed it), its faces' textures, its light.
	static std::vector<wgpuWorld::BlockCube> blockCubes;
	blockCubes.clear();
	for (auto &e : entityManager.droppedItems)
	{
		const auto type = e.second.entityBuffered.type;
		if (!isBlock(type)) { continue; }

		wgpuWorld::BlockCube cube;
		glm::vec3 entityFloat = {};
		glm::ivec3 entityInt = {};
		decomposePosition(e.second.getRubberBandPosition(), entityFloat, entityInt);
		entityFloat += glm::vec3(0, 0.2, 0);

		// The light where it lies, as baked; entities.wgsl lights the cube as
		// a block face is lit, time of day and weather included.
		uint32_t light = 0xF0;
		if (auto b = chunkSystem.getBlockSafe(e.second.getRubberBandPosition() + glm::dvec3(0, 0.2, 0));
			b && !dontUpdateLightSystem)
		{
			light = (uint32_t(b->getSkyLight() & 0xF) << 4) | uint32_t(b->getLight() & 0xF);
		}

		cube.positionInt = glm::ivec4(entityInt, 0);
		cube.positionFloat = glm::vec4(entityFloat, (float)light);
		cube.slotsA = glm::uvec4(getGpuIdIndexForBlock(type, 0), getGpuIdIndexForBlock(type, 1),
			getGpuIdIndexForBlock(type, 2), getGpuIdIndexForBlock(type, 3));
		cube.slotsB = glm::uvec4(getGpuIdIndexForBlock(type, 4), getGpuIdIndexForBlock(type, 5), 0, 0);
		blockCubes.push_back(cube);
	}

	// Milestone G4: the debris of strikes, as small turned cubes: a quarter
	// of the block's texture, or a flat colour (entities.wgsl's Cube flags).
	for (const debris::Piece &piece : debris::pieces())
	{
		const float size = debris::currentSize(piece);
		if (size <= 0.002f) { continue; }

		wgpuWorld::BlockCube cube;
		glm::vec3 pieceFloat = {};
		glm::ivec3 pieceInt = {};
		decomposePosition(piece.position, pieceFloat, pieceInt);

		uint32_t light = 0xF0;
		if (auto b = chunkSystem.getBlockSafe(piece.position); b && !dontUpdateLightSystem)
		{
			light = (uint32_t(b->getSkyLight() & 0xF) << 4) | uint32_t(b->getLight() & 0xF);
		}

		cube.positionInt = glm::ivec4(pieceInt, std::max(1, (int)std::round(size * 1000.f)));
		cube.positionFloat = glm::vec4(pieceFloat, (float)light);
		auto angle = [](float radians)
		{
			const float turns = radians / 6.2831853f;
			return uint32_t((turns - std::floor(turns)) * 65535.f) & 0xFFFFu;
		};
		const uint32_t turn = angle(piece.spin.x) | (angle(piece.spin.y) << 16);
		if (piece.blockType)
		{
			const auto type = piece.blockType;
			cube.slotsA = glm::uvec4(getGpuIdIndexForBlock(type, 0), getGpuIdIndexForBlock(type, 1),
				getGpuIdIndexForBlock(type, 2), getGpuIdIndexForBlock(type, 3));
			cube.slotsB = glm::uvec4(getGpuIdIndexForBlock(type, 4), getGpuIdIndexForBlock(type, 5),
				std::max(turn, 1u), 1u | (uint32_t(piece.quarter & 15u) << 1));
		}
		else
		{
			const glm::uvec3 c = glm::uvec3(glm::clamp(piece.colour, glm::vec3(0.f), glm::vec3(1.f)) * 255.f + 0.5f);
			cube.slotsA = glm::uvec4(c.r | (c.g << 8) | (c.b << 16) | (255u << 24), 0, 0, 0);
			cube.slotsB = glm::uvec4(0, 0, std::max(turn, 1u), 32u);
		}
		blockCubes.push_back(cube);
	}

	wgpuWorld::Lighting lighting;
	lighting.sunDirection = mainLightPosition;
	lighting.sunLightColor = sunLightColor;
	lighting.ambientColor = ambientColor;
	lighting.skyLightIntensity = skyLightIntensity;
	lighting.baseAmbientExtra = adaptiveExposure.bonusAmbient;
	lighting.exposure = adaptiveExposure.currentExposure;
	lighting.useLights = getShadingSettings().useLights != 0;
	lighting.maxLights = getShadingSettings().maxLights;
	lighting.aoTexture = programData.aoTexture.id;
	// Milestone 13: the sun's shadow cascades, at GL's depths (passed as
	// floats there, so the same rounding here). OURCRAFT_SHADOWS=0|1|2
	// overrides the setting for test runs.
	lighting.shadows = getShadingSettings().shadows;
	if (const char *env = std::getenv("OURCRAFT_SHADOWS")) { lighting.shadows = glm::clamp(std::atoi(env), 0, 2); }
	if (lighting.shadows)
	{
		const float cascadeEnds[3] = { 0.9985f, 0.9995f, 0.99994f };
		for (int i = 0; i < 3; i++)
		{
			lighting.shadowCascades[i] = sunCascadeMatrix(c, mainLightPosition, cascadeEnds[i]);
		}
	}
	// Milestone 14: the sky, the sun and moon, and the fog, from what
	// drawBefore, renderSun and the fog uniforms use.
	lighting.skyWeights = skyWeightsFor(dayTime);
	lighting.daySkyColor = programData.skyBoxLoaderAndDrawer.skyConfig.a1;
	lighting.daySkyHorizon = programData.skyBoxLoaderAndDrawer.skyConfig.a2;
	lighting.daySkyG = programData.skyBoxLoaderAndDrawer.skyConfig.g;
	lighting.sunPosition = sunPos;
	lighting.sunTwilight = sunTwilight;
	lighting.sunTexture = programData.skyBoxLoaderAndDrawer.sunTexture.id;
	lighting.moonTexture = programData.skyBoxLoaderAndDrawer.moonTexture.id;
	lighting.fogDistance = (chunkSystem.squareSize / 2.f) * CHUNK_SIZE - CHUNK_SIZE;
	lighting.fogGradient = getShadingSettings().fogGradient;
	lighting.underWater = underWater;
	// Milestone G1: the weather's grey sky (which also hides the sun and the
	// moon) and the rain's thicker near fog.
	lighting.overcast = weather.overcast;
	lighting.skyDarken = weather.skyDarken;
	lighting.rain = weather.rain;
	// The snow weather's whiteout. Its colour is about what snow in the open
	// looks like in this light (computeAmbient's sky term on a face under the
	// open sky, a little of the sun), a touch blue; the sky goes to it too, so
	// the world closes into one colour.
	lighting.snow = weather.snow;
	{
		auto toLinear = [](float c) { return c < 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
		const float sky = 0.62f * std::min(toLinear(skyLightIntensity / 15.f * 0.8f + 0.2f), 1.f);
		lighting.snowFogColour = (ambientColor * sky + sunLightColor * 0.15f) * glm::vec3(0.86f, 0.89f, 0.94f);
	}
	// The PSX look: always, or while it snows. OURCRAFT_PSX=0|1|2 overrides
	// the setting, for scripted screenshots.
	{
		static const int environmentPsx = []()
		{
			const char *env = std::getenv("OURCRAFT_PSX");
			return env ? std::atoi(env) : -1;
		}();
		const int psxLook = environmentPsx >= 0 ? environmentPsx : getShadingSettings().psxLook;
		lighting.psx = psxLook == 2 || (psxLook == 1 && weather.snow >= 0.5f);
	}
	// How far indoors the camera is, from the sky light where it stands:
	// 15 is under the open sky (it comes down undimmed through air, leaves
	// and glass), and it drops a level a block sideways from there, so in
	// a room it is 12 or less a few blocks past the door. Eased, so walking
	// through a door doesn't pop.
	{
		static float indoors = 0;
		float target = 0;
		if (auto b = chunkSystem.getBlockSafe(blockPosition.x, blockPosition.y, blockPosition.z))
		{
			target = std::clamp((15 - b->getSkyLight()) / 3.f, 0.f, 1.f);
		}
		const float step = std::min(deltaTime * 2.5f, 1.f);
		indoors += (target - indoors) * step;
		lighting.indoors = indoors;
	}
	// Milestone 15: the tone mapper's and FXAA's settings, as GL sets them.
	lighting.tonemapper = getShadingSettings().tonemapper;
	// Milestone G2: the screen effects on top (the weather's overcast holds
	// one, gamePlayLogic the rest).
	lighting.screenEffects = postModifiers::current();
	lighting.saturation = getShadingSettings().toneMapSaturation * lighting.screenEffects.saturation;
	lighting.vibrance = getShadingSettings().toneMapVibrance;
	lighting.gamma = getShadingSettings().toneMapGamma;
	lighting.shadowBoost = getShadingSettings().toneMapShadowBoost;
	lighting.highlightBoost = getShadingSettings().toneMapHighlightBoost;
	lighting.vignette = getShadingSettings().vignette;
	lighting.lift = getShadingSettings().toneMapLift * glm::vec3(0.2f) - glm::vec3(0.1f);
	lighting.gain = getShadingSettings().toneMapGain * glm::vec3(0.4f) + glm::vec3(0.8f);
	lighting.fxaa = getShadingSettings().FXAA;
	// Milestone 16: bloom's uniforms, by the expressions of the bloom
	// filter and apply passes below (GL's run after this frame's exposure
	// update; these use the last frame's luminosity, a frame apart), and the
	// dirt textures (the lens's for bloom, the hit's and water's for G3's
	// splats).
	lighting.bloom = getShadingSettings().bloom && getShadingSettings().bloomMultiplier > 0;
	{
		float threshold = getShadingSettings().bloomTresshold / 100.f;
		threshold *= linearRemap(pow(adaptiveExposure.getLuminosityOrDefaultValueIfDisabeled(), 2.2), 0, 1, 0.8, 1.8);
		float multiplier = getShadingSettings().bloomMultiplier * (2.f / 5.f);
		multiplier *= linearRemap(pow(adaptiveExposure.getLuminosityOrDefaultValueIfDisabeled(), 2.0), 0, 1, 1.2, 0.6);
		lighting.bloomExposure = shadingSettings.exposure;
		lighting.bloomThreshold = underWater ? threshold / 2 : threshold;
		lighting.bloomMultiplier = multiplier / 10000.f;
	}
	lighting.lensDirtTexture = programData.lensDirtTexture.id;
	lighting.lensDirtGlare = sunGlare;
	lighting.hitDirtTexture = programData.hitDirtTexture.id;
	lighting.waterDirtTexture = programData.waterDirtTexture.id;
	// Milestone 17: the AO and the reflections.
	lighting.ssao = programData.renderer.ssao;
	lighting.aoViewDistance = (chunkSystem.squareSize / 2.f) * 16.f;
	lighting.ssr = getShadingSettings().SSR != 0;
	// Milestone 18: under water.
	lighting.underWaterColor = shadingSettings.underWaterColor;
	lighting.underwaterDarkenStrength = shadingSettings.underwaterDarkenStrength;
	lighting.underwaterDarkenDistance = shadingSettings.underwaterDarkenDistance;
	lighting.fogGradientUnderWater = shadingSettings.fogGradientUnderWater;
	// 18-parity: the animation's clock in wall seconds, what timeGrass is
	// on Windows (std::clock is CPU time elsewhere).
	{
		static const auto animationStart = std::chrono::steady_clock::now();
		lighting.animationTime = std::chrono::duration<float>(std::chrono::steady_clock::now() - animationStart).count();
	}
	lighting.waterMove = waterTimer;
	lighting.dudvTexture = programData.dudv.id;
	lighting.dudvNormalTexture = programData.dudvNormal.id;
	lighting.causticsTexture = programData.causticsTexture.id;
	wgpuWorld::render(c, chunkVectorCopy, blockCubes, shadingSettings.waterColor,
		getShadingSettings().waterType != 0, zprepass, lighting);

	// GL's u_waterMove: how far the water's textures have scrolled, 0..20.
	waterTimer += deltaTime * 0.09;
	if (waterTimer > 20)
	{
		waterTimer -= 20;
	}

	renderEntities(deltaTime, c, modelsManager, blocksLoader,
		entityManager, vp, c.getProjectionMatrix(), viewMatrix, posFloat, posInt,
		shadingSettings.exposure, chunkSystem, skyLightIntensity,
		currentSkinTextureId, playerClicked, playerRunning, playerHand, currentHeldItemIndex,
		showHand, playersConnectionData);
	// Milestone G1b: the rain and snow around the camera, one sheet per block
	// column, from where the column's highest blocking block stops it. The
	// chunk's biome picks the kind: nothing in the desert, snow in the snow
	// biome and high up (and everywhere else in the snow weather), rain
	// elsewhere. Not under water.
	{
		wgpuWorld::Precipitation precipitation;
		// Rain, or the snow weather's snow (which falls where rain would).
		precipitation.intensity = underWater ? 0.f : std::max(weather.rain, weather.snow);
		if (precipitation.intensity > 0)
		{
			constexpr int radius = 16;
			precipitation.size = radius * 2 + 1;
			precipitation.corner = {blockPosition.x - radius, blockPosition.z - radius};
			precipitation.brightness = std::max(skyLightIntensity / 15.f, 0.08f) * 0.9f;
			precipitation.wind = 0.2f * weather.rain * weather.rain;
			precipitation.columns.assign(precipitation.size * precipitation.size, glm::vec2(0));

			static const int environmentPreview = []()
			{
				const char *env = std::getenv("OURCRAFT_PRECIPITATION");
				if (!env) { return 0; }
				if (std::string(env) == "rain") { return 1; }
				if (std::string(env) == "snow") { return 2; }
				return 0;
			}();
			const int preview = precipitationPreview ? precipitationPreview : environmentPreview;

			for (int j = 0; j < precipitation.size; j++)
				for (int i = 0; i < precipitation.size; i++)
				{
					const int x = precipitation.corner.x + i;
					const int z = precipitation.corner.y + j;
					Chunk *chunk = chunkSystem.getChunkSafeFromBlockPos(x, z);
					if (!chunk) { continue; } // not loaded: none

					const int lx = modBlockToChunk(x);
					const int lz = modBlockToChunk(z);
					int top = 0;
					for (int y = CHUNK_HEIGHT - 1; y >= 0; y--)
					{
						if (blockStopsRain(chunk->data.unsafeGet(lx, y, lz).getType())) { top = y + 1; break; }
					}

					float kind = (float)precipitationForColumn(chunk->data.biome, top, weather.rain, weather.snow);
					if (preview) { kind = (float)preview; }
					precipitation.columns[i + j * precipitation.size] = glm::vec2((float)top, kind);
				}
		}
		wgpuWorld::setPrecipitation(precipitation);
	}

	// The world's transparent passes come after the entities.
	wgpuWorld::renderTransparent();

#pragma region automatic exposure
	// The WebGPU world's reading of a frame's average color (it arrives a
	// few frames late), as GL's luminosity.
	{
		glm::vec3 average;
		if (wgpuWorld::takeExposureColor(average))
		{
			glm::vec3 screenColor = glm::pow(average, glm::vec3(1.f / 2.2f));
			averageLuminosity = glm::dot(screenColor, glm::vec3(0.2126, 0.7152, 0.0722));
		}
		adaptiveExposure.update(deltaTime, averageLuminosity);
	}
#pragma endregion

#pragma region lens flare


	{
		auto &renderer2d = programData.ui.renderer2d;
		renderer2d.pushCamera();
		

		//sunPos

		glm::vec2 screenCoords = {};
		glm::vec4 sun3Dpos = glm::vec4(sunPos, 1);
		glm::vec4 sunProjectedCoords = c.getProjectionMatrix() * c.getViewMatrix() * sun3Dpos;

		sunGlare = 0;
		if (sunProjectedCoords.w > 0)
		{
			sunProjectedCoords.x /= sunProjectedCoords.w;
			sunProjectedCoords.y /= sunProjectedCoords.w;
			sunProjectedCoords.x += 1;
			sunProjectedCoords.x /= 2;
			sunProjectedCoords.y += 1;
			sunProjectedCoords.y /= 2;
			sunProjectedCoords.y = 1.f - sunProjectedCoords.y;


			float sunSize = 0;
			float maxSize = 0;
			{
				auto c2 = c;
				c2.viewDirection = glm::vec3(0, 0, -1);

				glm::vec4 sunEdge3Dpos1 = glm::vec4(glm::vec3(0, 0, -4), 1);
				glm::vec4 sunEdge3Dpos2 = glm::vec4(glm::vec3(1, 0, -4), 1);
				glm::vec4 sunEdgeProjectedCoords = c.getProjectionMatrix() * c2.getViewMatrix() * sunEdge3Dpos1;
				glm::vec4 sunEdgeProjectedCoords2 = c.getProjectionMatrix() * c2.getViewMatrix() * sunEdge3Dpos2;
				sunEdgeProjectedCoords.x /= sunEdgeProjectedCoords.w;
				sunEdgeProjectedCoords.x += 1;
				sunEdgeProjectedCoords.x /= 2;

				sunEdgeProjectedCoords2.x /= sunEdgeProjectedCoords2.w;
				sunEdgeProjectedCoords2.x += 1;
				sunEdgeProjectedCoords2.x /= 2;

				sunSize = std::abs(sunEdgeProjectedCoords.x - sunEdgeProjectedCoords2.x) * 1.7;
				maxSize = std::abs(sunEdgeProjectedCoords.x - sunEdgeProjectedCoords2.x);

				//std::cout << queryDataForSunFlare << "\n";
				//std::cout << "MAX: " << int(maxSize * screenX) * int(maxSize * screenX) * 4 << " ";
				maxSize = int(maxSize * screenX) * int(maxSize * screenX) * 4;
			}
			// How many of the sun's pixels the world left visible.
			const int queryDataForSunFlare = wgpuWorld::sunVisibleSamples();
			float sunOcclusionFactor = glm::clamp(queryDataForSunFlare / maxSize, 0.f, 1.f);
			sunOcclusionFactor *= sunOcclusionFactor;

			glm::vec2 center = {0.5,0.5};
			glm::vec2 sunPos = sunProjectedCoords;
			glm::vec2 vectorTwardsCenter = (center - sunPos) * 2.f;

			float globalBrightness = (1 - (glm::length(center - sunPos) / 0.6f)) * sunOcclusionFactor;
			globalBrightness *= 1.f - weather.overcast; // G1: no flare through clouds
			// The lens dirt reaches a little wider than the flare.
			sunGlare = glm::clamp(1.f - glm::length(center - sunPos) / 0.9f, 0.f, 1.f) * sunOcclusionFactor
				* (1.f - weather.overcast);

			if (globalBrightness > 0)
			{

				glm::vec2 flarePos = sunPos;
				for (int i = 0; i < programData.lensFlare.size(); i++)
				{
					float size = sunSize * screenX;
					//float size = 800;
					flarePos += vectorTwardsCenter / (float)programData.lensFlare.size();

					float textureSizeX = programData.lensFlare[i].GetSize().x;
					float textureSizeY = programData.lensFlare[i].GetSize().y;
					float sizeX = size * textureSizeX / programData.maxFlareSize;
					float sizeY = size * textureSizeY / programData.maxFlareSize;

					float distanceToCenter = glm::distance(flarePos, center);
					float alpha = 1.f - (distanceToCenter / glm::length(vectorTwardsCenter));
					alpha = glm::clamp(alpha, 0.3f, 1.f) * 0.70;
					alpha *= globalBrightness;
					
					renderer2d.renderRectangle({
						flarePos.x * screenX - sizeX / 2.f, flarePos.y * screenY - sizeY / 2.f,
						sizeX,sizeY}, programData.lensFlare[i],
						{1.3,1.3,1.3,alpha});

				}

			};



		}
	


		renderer2d.popCamera();
	}


#pragma endregion
}

//draw decal
void Renderer::renderDecal(glm::ivec3 position, Camera &c, Block b, ProgramData &programData,
	float crack)
{
	int decalPosition = 0;
	if (crack <= 0) { return; }
	if (crack > 1) { crack = 1; }
	else
	{
		decalPosition = crack * 10;
		if (decalPosition > 10) { decalPosition = 10; }
	}


	//setup geometry
	static std::vector<int> currentVector;
	currentVector.clear();
	
	if (b.isGrassMesh())
	{
		for (int i = 6; i <= 9; i++)
		{
			currentVector.push_back(mergeShorts(i, getGpuIdIndexForBlock(b.getType(), 0)));

			pushFlagsLightAndPosition(currentVector, position, 0, 0, 15, 15, 0);

		}
	}
	else if (b.getType() == BlockTypes::torch)
	{
		for (int i = 0; i < 6; i++)
		{
			currentVector.push_back(mergeShorts(i + 16, getGpuIdIndexForBlock(b.getType(), i)));

			pushFlagsLightAndPosition(currentVector, position, 0, 0, 15, 15, 0);
		}
	}
	else
	{
		bool isAnimated = b.isAnimatedBlock();

		for (int i = 0; i < 6; i++)
		{
			//todo face type or remove
			currentVector.push_back(mergeShorts(i + isAnimated * 10, getGpuIdIndexForBlock(b.getType(), i)));

			pushFlagsLightAndPosition(currentVector, position, 0, 0,
				15, 15, 0);
		}
	}
	
	wgpuWorld::renderDecal(currentVector, decalPosition, programData.crackTexture.id);
}

void Renderer::renderEntities(
	float deltaTime,
	Camera &c,
	ModelsManager &modelsManager,
	BlocksLoader &blocksLoader, ClientEntityManager &entityManager,
	glm::mat4 &vp, const glm::mat4 &projection,
	glm::mat4 &viewMatrix,
	glm::vec3 posFloat,
	glm::ivec3 posInt,
	float exposure, ChunkSystem &chunkSystem, int 
	skyLightIntensity, std::uint64_t currentSkinTextureId,
	bool &playerClicked, float playerRunning, BoneTransform &playerHand, int currentHeldItemIndex,
	bool showHand, std::unordered_map<std::uint64_t, PlayerConnectionData> &playersConnectionData
	)
{
	struct PerEntityData
	{
		glm::ivec3 entityPositionInt = {};
		glm::vec3 entityPositionFloat = {};
		glm::vec3 color = {1,1,1};
		// The light where it stands, as baked (the shader takes off what the
		// time of day does): sky light << 4 | block light.
		uint32_t light = 0xF0;
		std::uint64_t textureId0;
		std::uint64_t textureId1;
		std::uint64_t textureId2;
		std::uint64_t textureId3;
	};

	static std::vector<glm::mat4> skinningMatrix;
	skinningMatrix.clear();

	static std::vector<PerEntityData> entityData;
	entityData.clear();

	// The entity pass's input: bone matrices, one record per model and one per
	// item, textures as wgpu2d ids (a texture slot is its texture's id, 0 for
	// none).
	static std::vector<glm::mat4> wgpuBones;
	static std::vector<wgpuWorld::ModelInstance> wgpuModels;
	static std::vector<wgpuWorld::ItemInstance> wgpuItems;
	wgpuBones.clear();
	wgpuModels.clear();
	wgpuItems.clear();

	auto textureIdOf = [](std::uint64_t slot) -> uint32_t { return (uint32_t)slot; };

	auto addModel = [&](Model &model, const glm::mat4 *matrices, size_t count, const PerEntityData &data,
		bool firstPerson = false)
	{
		wgpuWorld::ModelInstance instance;
		instance.firstPerson = firstPerson;
		instance.mesh = model.mesh;
		instance.boneBase = (uint32_t)wgpuBones.size();
		wgpuBones.insert(wgpuBones.end(), matrices, matrices + count);
		instance.positionInt = data.entityPositionInt;
		instance.positionFloat = data.entityPositionFloat;
		instance.color = data.color;
		instance.light = data.light;
		instance.textures[0] = textureIdOf(data.textureId0);
		instance.textures[1] = textureIdOf(data.textureId1);
		instance.textures[2] = textureIdOf(data.textureId2);
		instance.textures[3] = textureIdOf(data.textureId3);
		wgpuModels.push_back(instance);
	};

	// The light of the block at `blockPos`, for a model or item in it. Open
	// sky where the chunk is not loaded.
	auto lightAt = [&](glm::ivec3 blockPos) -> uint32_t
	{
		if (dontUpdateLightSystem) { return 0xF0; }
		auto block = chunkSystem.getBlockSafe(blockPos.x, blockPos.y, blockPos.z);
		if (!block) { return 0xF0; }
		return (uint32_t(block->getSkyLight() & 0xF) << 4) | uint32_t(block->getLight() & 0xF);
	};

	glm::mat4 handItemMatrix = glm::mat4{1.f};

	// The held item's type (0 for none or a block). OURCRAFT_HELD_ITEM="name"
	// (as the item's name, e.g. "silver sword") shows that item instead, for
	// scripted screenshots; only the drawing, not the inventory.
	int heldItemType = 0;
	{
		static const int environmentHeld = []()
		{
			const char *env = std::getenv("OURCRAFT_HELD_ITEM");
			if (!env) { return 0; }
			for (int i = ItemsStartPoint; i < lastItem; i++)
			{
				if (Item(i).getItemName() == env) { return i; }
			}
			std::cout << "OURCRAFT_HELD_ITEM: no item named " << env << "\n";
			return 0;
		}();
		auto handItem = entityManager.localPlayer.inventory.getItemFromIndex(currentHeldItemIndex, nullptr);
		if (handItem && !handItem->isBlock() && handItem->type) { heldItemType = handItem->type; }
		if (environmentHeld) { heldItemType = environmentHeld; }
	}
	// Pixel items that take the hand's pose: held, the hand isn't drawn and
	// the item is where the hand would be, through its idle, running and
	// swing poses. The training sword and the silver sword for now.
	const bool itemTakesHandPose = heldItemType == ItemTypes::trainingSword
		|| heldItemType == ItemTypes::silverSword;

	auto renderHand = [&]()
	{
		auto &model = modelsManager.rightHand;

		skinningMatrix.clear();
		entityData.clear();

		skinningMatrix.reserve(model.transforms.size());
		entityData.reserve(model.transforms.size());

		{
			BoneTransform handIdle;
			handIdle.position = glm::vec3{0.2,-2.0,-0.5};
			handIdle.rotation = glm::vec3{glm::radians(120.f),0.f,0.f};
			const BoneTransform handRest = handIdle; // before the running bob

			if (playerRunning)
			{
				static Oscilator handOscilator(0.3);
				BoneTransform handIdle2;
				handIdle2.position = glm::vec3{0.1,-2.1,-0.4};
				handIdle2.rotation = glm::vec3{glm::radians(123.f),glm::radians(0.f) ,
				glm::radians(2.f)};

				handOscilator.update(deltaTime);

				if (handOscilator.currentFaze)
				{
					handIdle = handIdle2;
				}
			}

			BoneTransform handHit;
			handHit.position = glm::vec3{0.1,-2.1,-0.9};
			handHit.rotation = glm::vec3{glm::radians(90.f),glm::radians(25.f),glm::radians(5.f)};

			float hitSpeed = 6 * deltaTime;
			float returnSpeed = 1 * deltaTime;

			static bool hitReturn = 0;

			if (playerClicked)
			{
				if (hitReturn)
				{
					if (playerHand.goTowards(handIdle, hitSpeed))
					{
						hitReturn = false;
						playerClicked = false;
					}
				}
				else
				{
					if (playerHand.goTowards(handHit, hitSpeed))
					{
						hitReturn = true;
						playerClicked = false;
					}
				}


			}
			else
			{
				hitReturn = 0;
				playerHand.goTowards(handIdle, returnSpeed);
			}


			auto transform = model.transforms[0]; //loaded from glb file
			auto poseMatrix = playerHand.getPoseMatrix();

			glm::vec3 lookDirection = c.viewDirection;
			glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
			glm::vec3 right = glm::normalize(glm::cross(up, lookDirection));
			up = glm::normalize(glm::cross(lookDirection, right));
			auto rotMatrix = glm::mat4(glm::vec4(right, 0.0f), glm::vec4(up, 0.0f), glm::vec4(-lookDirection, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			//rotMatrix[0][0] *= -1.0f;

			skinningMatrix.push_back(rotMatrix * glm::scale(glm::vec3{-1,1,1}) * transform * poseMatrix);

			//hand item matrix calculate
			handItemMatrix = skinningMatrix.back() * glm::translate(glm::vec3{0.1,-0.55,-0.4})
				* glm::rotate(glm::radians(180.f), glm::vec3(0, 0, 1))
				* glm::rotate(glm::radians(-45.f), glm::vec3(1, 0, 0))
				* glm::rotate(glm::radians(90.f), glm::vec3(0, 1, 0))
				* glm::scale(glm::vec3(0.45f));
			//handItemMatrix = glm::translate(glm::vec3{0,0,2});

			// An item that takes the hand's pose: two poses of its own, after
			// Minecraft's first-person held item. At rest it is at the
			// screen's right edge, its flat turned sideways so the picture
			// runs into the screen (seen at an angle from the middle, the
			// near pixels big), the tip leaning back and off the edge. At
			// the end of the swing it is in at the bottom middle, tipped
			// forward toward the crosshair, nearly edge on. The swing goes
			// from one to the other and back as the hand's does, with the
			// hand's walking bob on top.
			if (itemTakesHandPose)
			{
				// Each pose: where the grip is on the screen (-1..1 each way,
				// so it keeps its place whatever the window's shape) and how
				// far in front of the camera; then the blade's lean (degrees,
				// toward the left), tip (forward when negative) and twist about
				// itself (-90: the flat toward the screen's middle).
				// The numbers are Minecraft's handheld transform at 70 degrees
				// and 16:9: at rest, and 0.4 of the way through its swing.
				struct ItemPose { glm::vec2 screen; float depth; float lean, tip, twist; };
				const ItemPose atRest = {{0.68f, -1.1f}, 0.74f, 0.f, 20.f, -90.f};
				const ItemPose swung = {{0.25f, -1.06f}, 0.9f, 44.f, -39.f, -64.f};

				// How far through the swing the hand is, 0..1, along the way
				// from its rest to its hit position.
				const glm::vec3 swingWay = handHit.position - handRest.position;
				float swing = glm::dot(playerHand.position - handRest.position, swingWay)
					/ glm::dot(swingWay, swingWay);
				swing = glm::smoothstep(0.f, 1.f, glm::clamp(swing, 0.f, 1.f));

				const float tanHalf = std::tan(c.fovRadians / 2.f);
				auto place = [&](const ItemPose &pose)
				{
					return glm::vec3(pose.screen.x * tanHalf * c.aspectRatio * pose.depth,
						pose.screen.y * tanHalf * pose.depth, -pose.depth);
				};
				auto turn = [](const ItemPose &pose)
				{
					return glm::quat_cast(glm::mat3(glm::rotate(glm::radians(pose.lean), glm::vec3(0, 0, 1))
						* glm::rotate(glm::radians(pose.tip), glm::vec3(1, 0, 0))
						* glm::rotate(glm::radians(pose.twist), glm::vec3(0, 1, 0))));
				};
				const glm::vec3 position = glm::mix(place(atRest), place(swung), swing);
				const glm::quat rotation = glm::slerp(turn(atRest), turn(swung), swing);

				// The pixel item's square is -1..1, but a 16 pixel picture is
				// padded to 28 when loaded (loadFromFileAndAddPadding), so it
				// fills only the middle 16/28 of it: a sword's pommel at the
				// picture's bottom left and its tip at the top right. The
				// grip, on the handle, goes to the origin, the blade is turned
				// to point up with its flat toward +z, and the picture is
				// made 0.68 across, Minecraft's size in the hand.
				const float picture = 16.f / 28.f;
				const glm::vec3 grip = {-0.55f * picture, -0.55f * picture, 0.f};
				const glm::mat4 bladeUp = glm::rotate(glm::radians(45.f), glm::vec3(0, 0, 1))
					* glm::scale(glm::vec3(0.34f / picture)) * glm::translate(-grip);

				// The walking bob: the hand's change from its rest while it
				// walks, eased as the hand eases, into the camera's frame.
				static BoneTransform bob = handRest;
				bob.goTowards(handIdle, returnSpeed);
				BoneTransform rest = handRest;
				const glm::mat4 handFrame = glm::scale(glm::vec3{-1,1,1}) * transform;
				const glm::mat4 follow = handFrame * bob.getPoseMatrix() * glm::inverse(rest.getPoseMatrix())
					* glm::inverse(handFrame);

				// The poses are at Minecraft's size, which reaches over a
				// block out: the item draws against the world's depth, so a
				// wall beside the player would cut it off. Shrunk toward the
				// eye it looks the same and stays inside the player's reach.
				const float towardEye = 0.25f;

				// The camera's frame is rotMatrix with its x turned around
				// (rotMatrix's runs left; hence the hand's mirror).
				handItemMatrix = rotMatrix * glm::scale(glm::vec3(towardEye)) * follow * glm::scale(glm::vec3{-1,1,1})
					* glm::translate(position) * glm::mat4_cast(rotation) * bladeUp;
			}

			PerEntityData data = {};
			//data.textureId1 = currentSkinTextureId;
			//data.textureId0 = currentSkinTextureId;
			//data.textureId2 = currentSkinTextureId;
			//data.textureId3 = currentSkinTextureId;

			data.textureId0 = modelsManager.temporaryPlayerHandTextureId;
			data.textureId1 = modelsManager.temporaryPlayerHandTextureId;
			data.textureId2 = modelsManager.temporaryPlayerHandTextureId;
			data.textureId3 = modelsManager.temporaryPlayerHandTextureId;


			glm::dvec3 position = glm::dvec3(posInt) + glm::dvec3(posFloat);

			decomposePosition(position, data.entityPositionFloat, data.entityPositionInt);
			data.light = lightAt(from3DPointToBlock(position));
			entityData.push_back(data);
		}


		if (showHand && !itemTakesHandPose)
		{
			addModel(model, skinningMatrix.data(), skinningMatrix.size(), entityData.back(), true);
		};

	};

	//render hand draw hand renderHand drawHand
	renderHand();

	std::minstd_rand rng{std::random_device()()};

	auto renderAllEntitiesOfOneType = [&](Model &model, auto &container, bool isPlayers = 0)
	{
		skinningMatrix.clear();
		entityData.clear();

		skinningMatrix.reserve(model.transforms.size() * container.size());
		entityData.reserve(container.size());

		for (auto &e : container)
		{
			PerEntityData data = {};

			glm::mat4 rotMatrix(1.f);

			if constexpr (hasPositionBasedID<decltype(e.second.entityBuffered)>)
			{
				std::uint64_t entityID = e.first;
				auto blockPos = fromEntityIDToBlockPos(entityID);

				Block *b = chunkSystem.getBlockSafe(blockPos.x, blockPos.y, blockPos.z);

				if (b)
				{
					int rotation = b->getRotationFor365RotationTypeBlocks();
					rotMatrix = glm::rotate(glm::radians(90.f * rotation), glm::vec3(0, 1, 0));
				}

			}
			else
			{
				rotMatrix = e.second.getBodyRotationMatrix();
			}


			if constexpr (hasCanBeKilled<decltype(e.second.entityBuffered)>)
			{
				if (e.second.wasKilled)
				{
					data.color = {0.6,0.2,0.2};
					rotMatrix = rotMatrix * glm::rotate(PI / 2.f, glm::vec3{0,0,1});

					if (e.second.wasKilledTimer <= 0)
					{
						continue;
					}
				}
			}

			for (int i = 0; i < model.transforms.size(); i++)
			{
				skinningMatrix.push_back(rotMatrix * model.transforms[i]);
			}

			//todo set kill animation or something

			e.second.setEntityMatrixFull(skinningMatrix.data() +
				(skinningMatrix.size() - model.transforms.size()), model, 
				deltaTime, rng, rotMatrix);

			if (isPlayers)
			{

				auto found = playersConnectionData.find(e.first);

				if (found != playersConnectionData.end())
				{
					//todo armour here!
					data.textureId0 = found->second.skinTextureId;
					data.textureId1 = modelsManager.gpuIds[ModelsManager::HelmetTestTexture];
					data.textureId2 = found->second.skinTextureId;
					data.textureId3 = found->second.skinTextureId;
				}
				else
				{
					data.textureId0 = modelsManager.gpuIds[e.second.getTextureIndex()];
					data.textureId1 = data.textureId0;
					data.textureId2 = data.textureId0;
					data.textureId3 = data.textureId0;
				}

			}
			else
			{
				data.textureId0 = modelsManager.gpuIds[e.second.getTextureIndex()];
				data.textureId1 = data.textureId0;
				data.textureId2 = data.textureId0;
				data.textureId3 = data.textureId0;
			}

			//if constexpr (hasSkinTextureId<decltype(e.second)>)
			//{
			//	if (e.second.skinTextureId)
			//	{
			//		data.textureId = e.second.skinTextureId;
			//	}
			//	else
			//	{
			//		data.textureId = modelsManager.gpuIds[e.second.getTextureIndex()];
			//	}
			//
			//}
			//else
			//{
			//	data.textureId = modelsManager.gpuIds[e.second.getTextureIndex()];
			//}

			glm::ivec3 blockPos = {};

			if constexpr (hasPositionBasedID<decltype(e.second.entityBuffered)>)
			{
				std::uint64_t entityID = e.first;
				blockPos = fromEntityIDToBlockPos(entityID);

				data.entityPositionFloat = {0,-0.5,0};
				data.entityPositionInt = blockPos;
			}
			else
			{
				decomposePosition(e.second.getRubberBandPosition(), data.entityPositionFloat, data.entityPositionInt);
				blockPos = {from3DPointToBlock(e.second.getRubberBandPosition())};
			}

			data.light = lightAt(blockPos);

			entityData.push_back(data);

			addModel(model, skinningMatrix.data() + (skinningMatrix.size() - model.transforms.size()),
				model.transforms.size(), data);
		}
	};

	renderAllEntitiesOfOneType(modelsManager.human, entityManager.players, true);
	renderAllEntitiesOfOneType(modelsManager.human, entityManager.zombies);
	renderAllEntitiesOfOneType(modelsManager.pig, entityManager.pigs);
	renderAllEntitiesOfOneType(modelsManager.cat, entityManager.cats);
	renderAllEntitiesOfOneType(modelsManager.goblin, entityManager.goblins);
	renderAllEntitiesOfOneType(modelsManager.trainingDummy, entityManager.trainingDummy);
	renderAllEntitiesOfOneType(modelsManager.scareCrow, entityManager.scareCrows);

	static gl2d::Texture tempWhite;
	if (!tempWhite.id)
	{
		tempWhite.create1PxSquare();
	}

#pragma region item entities

	{

		//not for blocks
		auto renderOneItem = [&](auto type, glm::dvec3 pos, uint32_t light, glm::mat4 *mat = 0)
		{
			std::uint64_t texture;

			unsigned int mesh = 0;
			bool isA3DModel = 0;

			if (type >= ItemsStartPoint)
			{


				BlocksLoader::FullItemGeometryData geometry = blocksLoader.itemsGeometry[type - ItemsStartPoint + 1];

				//this means this is a model
				if (geometry.model3D.count)
				{
					texture = geometry.model3D.gpuId;
					if (texture == 0)
					{
						texture = tempWhite.id;
					}

					isA3DModel = true;
					mesh = geometry.model3D.mesh;
				}
				else
				{
					texture = blocksLoader.gpuIdsItems
						[type - ItemsStartPoint];

					isA3DModel = false;
					mesh = geometry.model2D.mesh;
				}


			}
			else
			{
				return;
				//texture = blocksLoader.gpuIds
				//	[getGpuIdIndexForBlock(type, 0)];
			}

			{
				// One item: its mesh, texture, placement and brightness.
				wgpuWorld::ItemInstance item;
				item.mesh = mesh;
				item.texture = textureIdOf(texture);
				if (mat)
				{
					decomposePosition(c.position, item.positionFloat, item.positionInt);
					item.model = *mat;
				}
				else
				{
					decomposePosition(pos, item.positionFloat, item.positionInt);
					item.positionFloat += glm::vec3(0, 0.4, 0);
					item.model = glm::scale(glm::vec3{isA3DModel ? 1.f : 0.4f});
				}
				item.light = light;
				item.firstPerson = mat != nullptr; // only the held item comes with a matrix
				wgpuItems.push_back(item);
			}

		
		};

		//todo instance rendering
		for (auto &e : entityManager.droppedItems)
		{
			//continue;
			if (isBlock(e.second.entityBuffered.type)) { continue; }
			if (!e.second.entityBuffered.type) { continue; }

			renderOneItem(e.second.entityBuffered.type, e.second.getRubberBandPosition(),
				lightAt(from3DPointToBlock(e.second.getRubberBandPosition() + glm::dvec3(0, 0.2, 0))));

		}


		//hand item
		// Items only: a held block draws nothing, as in GL.
		if (heldItemType)
		{
			renderOneItem(heldItemType, {}, lightAt(from3DPointToBlock(glm::dvec3(posInt) + glm::dvec3(posFloat))),
				&handItemMatrix);
		}


	}


#pragma endregion

	wgpuWorld::renderEntities(wgpuBones, wgpuModels, wgpuItems);
}

float cubePositions[] = {
		-0.5f, +0.5f, +0.5f, // 0
		+0.5f, +0.5f, +0.5f, // 1
		+0.5f, +0.5f, -0.5f, // 2
		-0.5f, +0.5f, -0.5f, // 3
		-0.5f, +0.5f, -0.5f, // 4
		+0.5f, +0.5f, -0.5f, // 5
		+0.5f, -0.5f, -0.5f, // 6
		-0.5f, -0.5f, -0.5f, // 7
		+0.5f, +0.5f, -0.5f, // 8
		+0.5f, +0.5f, +0.5f, // 9
		+0.5f, -0.5f, +0.5f, // 10
		+0.5f, -0.5f, -0.5f, // 11
		-0.5f, +0.5f, +0.5f, // 12
		-0.5f, +0.5f, -0.5f, // 13
		-0.5f, -0.5f, -0.5f, // 14
		-0.5f, -0.5f, +0.5f, // 15
		+0.5f, +0.5f, +0.5f, // 16
		-0.5f, +0.5f, +0.5f, // 17
		-0.5f, -0.5f, +0.5f, // 18
		+0.5f, -0.5f, +0.5f, // 19
		+0.5f, -0.5f, -0.5f, // 20
		-0.5f, -0.5f, -0.5f, // 21
		-0.5f, -0.5f, +0.5f, // 22
		+0.5f, -0.5f, +0.5f, // 23
};

unsigned int cubeIndicesData[] = {
	0,   1,  2,  0,  2,  3, // Top
	4,   5,  6,  4,  6,  7, // Back
	8,   9, 10,  8, 10, 11, // Right
	12, 13, 14, 12, 14, 15, // Left
	16, 17, 18, 16, 18, 19, // Front
	20, 22, 21, 20, 23, 22, // Bottom
};

void GyzmosRenderer::render(Camera &c, glm::ivec3 posInt, glm::vec3 posFloat)
{
	// WebGPU has no wireframe polygon mode: each cube triangle's three edges
	// become line segments, which is what glPolygonMode(GL_LINE) draws
	// (face diagonals included). Points are camera-relative, integer part
	// first, as the cube shader does.
	{
		static std::vector<glm::vec3> points;
		points.clear();
		for (auto &cube : cubes)
		{
			const glm::vec3 base = glm::vec3(glm::ivec3(cube.x, cube.y, cube.z) - posInt) - posFloat
				+ glm::vec3(cube.posFloatX, cube.posFloatY, cube.posFloatZ);
			const glm::vec3 size = {cube.sizeX, cube.sizeY, cube.sizeZ};
			auto corner = [&](unsigned int index)
			{
				return base + size * glm::vec3(cubePositions[index * 3], cubePositions[index * 3 + 1],
					cubePositions[index * 3 + 2]);
			};
			for (int t = 0; t < 36; t += 3)
			{
				for (int e = 0; e < 3; e++)
				{
					points.push_back(corner(cubeIndicesData[t + e]));
					points.push_back(corner(cubeIndicesData[t + (e + 1) % 3]));
				}
			}
		}
		for (auto &line : lines)
		{
			points.push_back(glm::vec3(glm::dvec3(line.a) - c.position));
			points.push_back(glm::vec3(glm::dvec3(line.b) - c.position));
		}
		wgpuWorld::renderLines(points);
	}

	cubes.clear();
	lines.clear();
}

void AdaptiveExposure::update(float deltaTime, float newLuminosity)
{

	auto moveTowards = [&](float &current, float to, float speed)
	{
		float diff = to - current;

		if (diff < 0)
		{
			current -= deltaTime * speed;

			if (current < to)
			{
				current = to;
			}
		}
		else if (diff > 0)
		{
			current += deltaTime * speed;

			if (current > to)
			{
				current = to;
			}
		}
	};

	moveTowards(currentLuminosity, newLuminosity, 0.3);

	float newValue = linearRemap(currentLuminosity, 0, 1, maxExposure, minExposure);
	moveTowards(currentExposure, newValue, 0.1);


	float newValuebonusAmbient = linearRemap(currentLuminosity, 0, 0.3, 0.35, 0);
	moveTowards(bonusAmbient, newValuebonusAmbient, 0.1);

	//if (currentLuminosity > 0.50)
	//{
	//	currentExposure -= deltaTime * 0.2;
	//}
	//
	//if (currentLuminosity < 0.4)
	//{
	//	currentExposure += deltaTime * 0.2;
	//}

	currentExposure = glm::clamp(currentExposure, minExposure, maxExposure);
	bonusAmbient = glm::clamp(bonusAmbient, 0.f, 0.35f);



}

float AdaptiveExposure::getLuminosityOrDefaultValueIfDisabeled()
{
	return currentLuminosity;
}
