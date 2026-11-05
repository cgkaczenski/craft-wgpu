#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/transform.hpp>

#include "rendering/skyBoxRenderer.h"

void SkyBoxLoaderAndDrawer::loadAllTextures(std::string path)
{
	if (!sunTexture.id)
	{
		sunTexture.loadFromFile((path + "sun.png").c_str(), false, false);
	}

	if (!moonTexture.id)
	{
		moonTexture.loadFromFile((path + "moon.png").c_str(), false, false);
	}
}

void SkyBoxLoaderAndDrawer::clearOnlyTextures()
{
	sunTexture.cleanup(); sunTexture = {};
	moonTexture.cleanup(); moonTexture = {};
}

glm::vec3 calculateSunPosition(float dayTime)
{
	if (dayTime > 1.f) {dayTime -= (int)dayTime;}

	//0   -> sunrise
	//0.25 -> noon
	//0.75 -> noon
	//1    -> sunrise

	float angle = dayTime * 2 * 3.1415926;

	glm::vec4 finalAngle = {glm::normalize(glm::vec3(1,0,0.1)), 1.f};
	finalAngle = glm::rotate(angle, glm::vec3{0,0,1}) * finalAngle;

	return finalAngle;
}
