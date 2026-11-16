#include "gameplay/mapEngine.h"
#include "gamePlayLogic.h"
#include "chunkSystem.h"
#include <platform/platformInput.h>
#include <rendering/crt.h>
#include <cstdio>
#include <cmath>

extern char *blockNames[];

void MapEngine::close()
{
	for (auto &it : mapChunks)
	{
		it.second.cleanup();
	}

	mapChunks = {};

	whitePixel.cleanup();
	whitePixel = {};
}

void MapEngine::open(ProgramData &programData, glm::ivec2 playerPos, 
	ChunkSystem &chunkSystem)
{
	centerPos = playerPos;

	camera.follow(centerPos, 10000, 0, 0, programData.ui.renderer2d.windowW,
		programData.ui.renderer2d.windowH);
	camera.zoom = 3;


}

void createMapChunkFromChunk(ChunkData &chunkData, MapEngine::MapChunk &chunk, ProgramData &programData)
{

	gl2d::Texture t;
	
	unsigned char textureData[CHUNK_SIZE * CHUNK_SIZE * 4] = {};

	for (int textureY = 0; textureY < CHUNK_SIZE; textureY++)
	{
		for (int textureX = 0; textureX < CHUNK_SIZE; textureX++)
		{

			unsigned char colorR = 0;
			unsigned char colorG = 0;
			unsigned char colorB = 0;
			unsigned char colorA = 255;

			bool bordering = chunkData.isBorder(textureX, textureY);
			bool center = chunkData.x == chunkData.regionCenterX && chunkData.z == chunkData.regionCenterZ;

			for (int y = CHUNK_HEIGHT - 1; y >= 0; y--)
			{
				auto &b = chunkData.unsafeGet(textureX, y, textureY);
				if (b.getType())
				{
					auto color = programData.blocksLoader.blocksColors[b.getType()];

					if (center)
					{
						color = glm::mix(color, glm::vec3{0.2, 0.2, 0.9}, 0.5f);
					}

					if (bordering)
					{
						color *= glm::vec3{0.9, 0.2, 0.2};
					}

					colorR = color.r * 255;
					colorG = color.g * 255;
					colorB = color.b * 255;
					break;
				}
			}

			textureData[(textureX + textureY * CHUNK_SIZE) * 4 + 0] = colorR;
			textureData[(textureX + textureY * CHUNK_SIZE) * 4 + 1] = colorG;
			textureData[(textureX + textureY * CHUNK_SIZE) * 4 + 2] = colorB;
			textureData[(textureX + textureY * CHUNK_SIZE) * 4 + 3] = colorA;

		}
	}

	t.createFromBuffer((char *)textureData, CHUNK_SIZE, CHUNK_SIZE, true, true);

	chunk.t = t;
}

void MapEngine::update(ProgramData &programData, float deltaTime,
	glm::ivec2 chunkPos, ChunkSystem &chunkSystem, glm::dvec3 playerPosition, glm::vec3 viewDirection)
{
	time += deltaTime;

	camera.zoom += platform::getScroll() * 0.2;

	camera.zoom = glm::clamp(camera.zoom, 0.6f, 10.f);


	//programData.blocksLoader.
	auto &renderer = programData.ui.renderer2d;
	
	camera.follow(centerPos, deltaTime * 10, 0.01, 10, programData.ui.renderer2d.windowW,
		programData.ui.renderer2d.windowH);


	renderer.pushCamera(camera);
	{
		
		auto cameraBox = renderer.getViewRect();
		glm::vec2 mousePosInViewRect = glm::vec2(cameraBox);
		mousePosInViewRect += glm::vec2(cameraBox.z, cameraBox.w) * glm::vec2(crt::mouseOnPicture()) /
			glm::vec2(renderer.windowW, renderer.windowH);

		mouseHoveredId = UINT64_MAX;

		// The block column under the cursor (the map's x is world x, its y
		// world z, one unit a block), for the label and for /tp.
		hoveredBlock = glm::ivec2(glm::floor(mousePosInViewRect));
		hoveredTop = -1;
		hoveredType = 0;
		if (Chunk *c = chunkSystem.getChunkSafeFromChunkPos(divideChunk(hoveredBlock.x),
			divideChunk(hoveredBlock.y)))
		{
			const int x = modBlockToChunk(hoveredBlock.x);
			const int z = modBlockToChunk(hoveredBlock.y);
			for (int y = CHUNK_HEIGHT - 1; y >= 0; y--)
			{
				auto &b = c->data.unsafeGet(x, y, z);
				if (b.getType())
				{
					hoveredTop = y;
					hoveredType = b.getType();
					break;
				}
			}
		}

		//first pass
		for (int z = 0; z < chunkSystem.squareSize; z++)
		{
			for (int x = 0; x < chunkSystem.squareSize; x++)
			{

				auto c = chunkSystem.getChunksInMatrixSpaceUnsafe(x, z);

				if (c)
				{
					int chunkX = c->data.x;
					int chunkZ = c->data.z;

					glm::vec4 pos = glm::vec4{x,z,1,1};
					pos.x += chunkSystem.cornerPos.x;
					pos.y += chunkSystem.cornerPos.y;
					pos *= (float)CHUNK_SIZE;

					if (glui::aabb(pos, mousePosInViewRect))
					{
						mouseHovered = glm::ivec2(pos);
						mouseHoveredId = getRegionId(c->data.regionCenterX, c->data.regionCenterZ); //todo
						//color = {0.5,0.5,0.5,0.5};
					}
				}
			}
		}

		for (int z = 0; z < chunkSystem.squareSize; z++)
		{
			for (int x = 0; x < chunkSystem.squareSize; x++)
			{

				auto c = chunkSystem.getChunksInMatrixSpaceUnsafe(x, z);

				if (c)
				{
					int chunkX = c->data.x;
					int chunkZ = c->data.z;

					auto found = mapChunks.find({chunkX, chunkZ});

					if (found == mapChunks.end())
					{
						MapChunk mapChunk;
						createMapChunkFromChunk(c->data, mapChunk, programData);

						mapChunks.insert({{chunkX, chunkZ}, mapChunk});

						found = mapChunks.find({chunkX, chunkZ});
					}

					glm::vec4 pos = glm::vec4{x,z,1,1};
					pos.x += chunkSystem.cornerPos.x;
					pos.y += chunkSystem.cornerPos.y;
					pos *= (float)CHUNK_SIZE;

					glm::vec4 color = Colors_White;

					//renderer.renderRectangle(pos * (float)CHUNK_SIZE,
					//	color);
					
					renderer.renderRectangle(pos, found->second.t, 
						color,
						{}, 0, {0,0,1,1});


					if (mouseHoveredId == getRegionId(c->data.regionCenterX, c->data.regionCenterZ))
					{
						renderer.renderRectangle(pos, glm::vec4{232, 207, 149, 125} / 255.f);

					}

				}
				

			}
		}

		//renderer.renderRectangle({mousePosInViewRect, 5, 5}, Colors_Red);

	}
	auto viewRect = renderer.getViewRect();
	renderer.popCamera();

	// The player: an arrow pointing where they face, the same size at any
	// zoom, outlined in black over a pulsing ring so it shows on grass, sand
	// and water alike. Block n spans n to n + 1 on the map, centred on n + 0.5.
	{
		if (!whitePixel.id)
		{
			const unsigned char white[4] = {255, 255, 255, 255};
			whitePixel.createFromBuffer((const char *)white, 1, 1, true, false);
		}

		const glm::vec2 mapPos = glm::vec2(playerPosition.x + 0.5, playerPosition.z + 0.5);
		const glm::vec2 screenPos = (mapPos - glm::vec2(viewRect.x, viewRect.y))
			* glm::vec2(renderer.windowW, renderer.windowH) / glm::vec2(viewRect.z, viewRect.w);

		glm::vec2 forward = {viewDirection.x, viewDirection.z};
		forward = glm::length(forward) > 0.001f ? glm::normalize(forward) : glm::vec2(0, -1);
		const glm::vec2 right = {-forward.y, forward.x};

		// Sized for a 720 pixel tall window and grown with it (a full screen
		// Retina framebuffer is two to three times that).
		const float scale = 1.25f * glm::max(1.f, renderer.windowH / 720.f);

		const float pulse = 0.5f + 0.5f * std::sin(time * 5.f);
		renderer.renderCircleOutline(screenPos, {0, 0, 0, 0.6f}, (22 + pulse * 8) * scale, 6 * scale, 32);
		renderer.renderCircleOutline(screenPos, {1, 0.15f, 0.1f, 1}, (22 + pulse * 8) * scale, 3 * scale, 32);

		// A chevron: tip, right wing, notch, left wing; two triangles.
		auto arrow = [&](float size, glm::vec4 color)
		{
			// The tip reaches well past the wings, so the way it points reads at a glance.
			const glm::vec2 tip = screenPos + forward * 20.f * size;
			const glm::vec2 rightWing = screenPos + (-forward * 10.f + right * 12.f) * size;
			const glm::vec2 notch = screenPos - forward * 3.f * size;
			const glm::vec2 leftWing = screenPos + (-forward * 10.f - right * 12.f) * size;
			const glm::vec2 positions[6] = {tip, rightWing, notch, tip, notch, leftWing};
			const glm::vec2 uvs[6] = {};
			glm::vec4 colors[6];
			for (auto &c : colors) { c = color; }
			renderer.renderTriangles(positions, uvs, colors, 6, whitePixel);
		};
		arrow(1.45f * scale, {0, 0, 0, 1});
		arrow(scale, {1, 1, 1, 1});
	}

	// The label: what /tp needs to get there. "x, z" lands on top of the
	// column; the top block's height and name tell what is there.
	{
		char text[128] = {};
		if (hoveredTop >= 0)
		{
			std::snprintf(text, sizeof(text), "%d, %d   (top y %d, %s)", hoveredBlock.x, hoveredBlock.y,
				hoveredTop, blockNames[hoveredType]);
		}
		else
		{
			std::snprintf(text, sizeof(text), "%d, %d", hoveredBlock.x, hoveredBlock.y);
		}

		const float size = 40;
		const glm::vec2 mouse = glm::vec2(crt::mouseOnPicture());
		const glm::vec2 textSize = renderer.getTextSize(text, programData.ui.font, size, 4, 0);
		glm::vec2 corner = mouse + glm::vec2(28, -textSize.y - 16);
		if (corner.x + textSize.x + 16 > renderer.windowW) { corner.x = mouse.x - 28 - textSize.x - 16; }
		corner.y = glm::max(corner.y, 0.f);

		renderer.renderRectangle({corner, textSize.x + 16, textSize.y + 12}, {0.1, 0.1, 0.1, 0.75});
		renderer.renderText(corner + glm::vec2(8, 6 + textSize.y), text, programData.ui.font, Colors_White,
			size, 4, 0, false);
	}

}

void MapEngine::MapChunk::cleanup()
{
	t.cleanup();

}
