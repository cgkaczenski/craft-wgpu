#pragma once

#include <string>
#include <glm/vec3.hpp>
#include <gl2d/gl2d.h>

// The sky's inputs: the day sky's colors, which wgpuWorld bakes into its
// three skies (and re-bakes when they change), and the sun and moon
// textures from the sky texture pack folder.
struct SkyBoxLoaderAndDrawer
{
	struct SkyConfigs
	{
		glm::vec3 a1 = glm::vec3{9, 47, 116} / 255.f;
		glm::vec3 a2 = glm::vec3{33,38,44} / 255.f;
		float g = 0.4;
	};

	SkyConfigs skyConfig;

	void loadAllTextures(std::string path);

	void clearOnlyTextures();

	gl2d::Texture sunTexture;
	gl2d::Texture moonTexture;
};


glm::vec3 calculateSunPosition(float dayTime);
