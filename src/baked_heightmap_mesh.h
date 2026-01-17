#pragma once
#pragma warning(push, 0)

#include <SDL3/SDL.h>

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif
#pragma warning(pop)

#include "v3.h"
#include "render_dx12.h"

struct BakedHeightmeshConstants
{
    static constexpr Uint32 indicesPerQuad = 6;
    static constexpr Uint32 chunkDimVerts = 64;
    static constexpr Uint32 maxLod = 6; // no higher than 6 for 64x64, TODO: Calculate for different dimensions, formula := log_2(chunkDimVerts) - 1
};

struct quad_indices
{
    Uint32 indices[BakedHeightmeshConstants::indicesPerQuad];
};

struct
{
    size_t terrainMeshIndexBufferSize;
    quad_indices *terrainMeshIndexBuffer;
    size_t terrainPointsSize;
    vertex *terrainPoints;
    Uint32 terrainMeshIndexBufferNum;
    Uint32 chunkNumTotal;
    int terrainPointsNum;
    int terrainDimInQuads;
    Uint32 chunkDimQuads;
    Uint32 chunkNumDim;

    struct lod_range_baked_heightmap_mesh
    {
        Uint32 startIndex[BakedHeightmeshConstants::maxLod] = {};
        Uint32 numIndices[BakedHeightmeshConstants::maxLod] = {};
    };
    lod_range_baked_heightmap_mesh *lodRanges;

    int baked()
    {
        int img_w, img_h, img_channels;
        unsigned short *img_pixels = stbi_load_16("heightmap.png", &img_w, &img_h, &img_channels, 1);

        if (!img_pixels)
        {
            err("stbi_load_16 failed");
            return 1;
        }

        SDL_Log("Loaded 16-bit PNG: %dx%d, channels=%d", img_w, img_h, img_channels);

        // Allocate float heightmap
        terrainDimInQuads = img_w - 1;
        float *heightmap = (float *)SDL_malloc(img_w * img_h * sizeof(float));

        for (int y = 0; y < img_h; y++)
        {
            for (int x = 0; x < img_w; x++)
            {
                // 16-bit grayscale pixel
                unsigned short actualColour = img_pixels[y * img_w + (img_w - 1 - x)];

                float normalized = (float)actualColour / 65535.0f;

                // Apply height scale
                // TODO: calculate actual scale required automatically?
                const float swissAlps = 0.071f;
                const float peloponessus = 0.021f;
                float heightScale = ((float)terrainDimInQuads * peloponessus);
                float h = normalized * heightScale;

                heightmap[x + y * img_w] = h;
            }
        }
        stbi_image_free(img_pixels);

        terrainPointsNum = img_w * img_h;
        terrainPointsSize = sizeof(vertex) * terrainPointsNum;
        terrainPoints = (vertex *)SDL_malloc(terrainPointsSize);
        for (int x = 0; x < img_w; ++x)
        {
            for (int y = 0; y < img_h; ++y)
            {
                float _x = (float)x;
                float _y = heightmap[x + y * img_w];
                float _z = (float)y;

                vertex v = {};
                v.position.x = _x;
                v.position.y = _y;
                v.position.z = _z;

                float tile = (float)img_w;
                v.texCoords.x = ((float)x / (float)(img_w - 1)) * tile;
                v.texCoords.y = ((float)y / (float)(img_h - 1)) * tile;

                int xl = (x > 0) ? x - 1 : x;
                int xr = (x < img_w - 1) ? x + 1 : x;
                int yd = (y > 0) ? y - 1 : y;
                int yu = (y < img_h - 1) ? y + 1 : y;
                float hL = heightmap[xl + y * img_w];
                float hR = heightmap[xr + y * img_w];
                float hD = heightmap[x + yd * img_w];
                float hU = heightmap[x + yu * img_w];

                // Tangent vectors in X and Z directions
                v3 dx = {2.0f, hR - hL, 0.0f};
                v3 dz = {0.0f, hU - hD, 2.0f};

                v3 n = v3::normalised(v3::cross(dz, dx));

                v.normals.x = n.x;
                v.normals.y = n.y;
                v.normals.z = n.z;

                terrainPoints[x + y * img_w] = v;
            }
        }

        int quadNum = (img_w - 1) * (img_h - 1);
        terrainMeshIndexBufferSize = (size_t)(quadNum * sizeof(quad_indices) * 2); // TODO: calculate and alloc correct amount of space, we are doing double for now just because that is enough
        terrainMeshIndexBufferNum = BakedHeightmeshConstants::indicesPerQuad * quadNum;

        chunkNumDim = img_w / BakedHeightmeshConstants::chunkDimVerts;
        chunkNumTotal = chunkNumDim * chunkNumDim;
        chunkDimQuads = BakedHeightmeshConstants::chunkDimVerts - 1;
        Uint32 writeIndex = 0;

        lodRanges = (lod_range_baked_heightmap_mesh *)SDL_malloc((size_t)(chunkNumTotal * sizeof(lod_range_baked_heightmap_mesh)));
        terrainMeshIndexBuffer = (quad_indices *)SDL_malloc((size_t)(terrainMeshIndexBufferSize));
        for (Uint32 lod = 0; lod < BakedHeightmeshConstants::maxLod; ++lod)
        {
            Uint32 lodStep = 1U << lod; // 2 to the power of lod
            for (Uint32 cy = 0; cy < chunkNumDim; ++cy)
            {
                for (Uint32 cx = 0; cx < chunkNumDim; ++cx)
                {
                    Uint32 currentLodStart = writeIndex;
                    lodRanges[cx + cy * chunkNumDim].startIndex[lod] = currentLodStart;
                    for (Uint32 y = 0; y < chunkDimQuads; y += lodStep)
                    {
                        for (Uint32 x = 0; x < chunkDimQuads; x += lodStep)
                        {
                            Uint32 chunkSpaceX = cx * chunkDimQuads;
                            Uint32 chunkSpaceY = cy * chunkDimQuads;

                            Uint32 i = (chunkSpaceX + x) + (chunkSpaceY + y) * img_w;

                            quad_indices q = {};
                            q.indices[0] = i;
                            q.indices[1] = i + img_w * lodStep;
                            q.indices[2] = i + img_w * lodStep + lodStep;
                            q.indices[3] = i;
                            q.indices[4] = i + img_w * lodStep + lodStep;
                            q.indices[5] = i + lodStep;

                            terrainMeshIndexBuffer[writeIndex++] = q;
                        }
                    }
                    lodRanges[cx + cy * chunkNumDim].numIndices[lod] = writeIndex - currentLodStart;
                }
            }
        }
        return 0;
    }
} baked_heightmap_mesh;
