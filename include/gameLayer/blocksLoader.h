#pragma once
#include <gl2d/gl2d.h>
#include <unordered_map>
#include <vector>

struct BlocksLoader
{

	// Texture slots (4 per block: albedo, normal, material, parallax), each
	// the wgpu2d id of the texture shown there (GL kept bindless handles
	// here); texturesIds the same ids, as loaded.
	std::vector<std::uint64_t> gpuIds;
	std::vector<unsigned int> texturesIds;

	// Textures that span several blocks, so a wall or floor does not repeat
	// every block: <name>_span.txt beside <name>.png holds N, and the picture
	// covers N x N blocks. Keyed by the albedo's id: its N*N slices, each a
	// texture of its own, bottom row first and left to right. wgpuWorld puts
	// them in consecutive layers and each block face picks its slice by the
	// block's position.
	std::unordered_map<unsigned int, std::vector<unsigned int>> spannedTextures;

	//load all blocks loadallblocks
	void loadAllTextures(std::string path, bool reportErrors);
	void loadTextureSpan(unsigned int albedo, const std::string &spanPath);
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

