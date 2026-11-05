#pragma once
#include <gl2d/gl2d.h>

struct BlocksLoader
{

	// Texture slots (4 per block: albedo, normal, material, parallax), each
	// the wgpu2d id of the texture shown there (GL kept bindless handles
	// here); texturesIds the same ids, as loaded.
	std::vector<std::uint64_t> gpuIds;
	std::vector<unsigned int> texturesIds;

	//load all blocks loadallblocks
	void loadAllTextures(std::string path, bool reportErrors);
	void setupAllColors();
	void clearAllTextures();

	void loadAllItemsGeometry();

	gl2d::Texture spawnEgg;
	gl2d::Texture spawnEggOverlay;

	std::vector<std::uint64_t> gpuIdsItems;
	std::vector<unsigned int> texturesIdsItems;
	

	struct ItemGeometry2D
	{
		unsigned int mesh = 0; // its wgpuWorld mesh id (wgpuWorld::newMeshId)
		unsigned int count = 0;

		void clear();
	};

	struct ItemGeometry3D
	{
		unsigned int mesh = 0; // its wgpuWorld mesh id (wgpuWorld::newMeshId)
		unsigned int count = 0;

		std::uint64_t gpuId = 0; // texturesId's id, as a texture slot
		gl2d::Texture texturesId;

		void clear();
	};


	struct FullItemGeometryData
	{
		ItemGeometry2D model2D;

		ItemGeometry3D model3D;

	};


	std::vector<FullItemGeometryData> itemsGeometry;

	gl2d::Texture backgroundTexture;

	std::vector<gl2d::Texture> blockUiTextures;

	std::vector<glm::vec3> blocksColors;

	void clearItemsGeometry();

};

bool loadFromFileAndAddPadding(gl2d::Texture &t, const char *path);


uint16_t getGpuIdIndexForBlock(short type, int face);

#define SNOW_GRASS_TEXTURE_INDEX 82
#define YELLOW_GRASS_TEXTURE_INDEX 97
#define BRICKS_VARIATION_TEXTURE_INDEX 99
#define BLUE_BRICKS_VARIATION_TEXTURE_INDEX 100

