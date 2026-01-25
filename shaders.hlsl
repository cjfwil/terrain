#include "ConstantBuffer.hlsl"

struct VSOut
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uvLocal : TEXCOORD1;
    float3 normalWS : TEXCOORD2;
    float2 water : TEXCOORD3;
    int sliceIndex : TEXCOORD4;
};

Texture2DArray<float> g_heightArray : register(t0);
Texture2DArray<float4> g_albedoArray : register(t1);

SamplerState g_sampler : register(s0);

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

VSOut VSMain(uint2 position: POSITION)
{
    int lodLevel = 0;
    VSOut o;
    float4 wp = float4(position.x, 0.0f, position.y, 1.0f);

    float gridDimSize = terrainGridDimensionInVertices - 1;
    wp.xz -= (gridDimSize + 2) / 2.0f;
    wp.xz *= ringSampleStep;

    wp = mul(world, wp);

    float tileDim = 4096.0f;
    float tilesPerRow = 3.0f;
    float tilesPerCol = 3.0f;
    float2 virtualDim = float2(tileDim * tilesPerRow, tileDim * tilesPerCol);

    float2 pUv = wp.xz + 0.5f + ringOffset;
    float2 uvGlobal = float2(1.0 - pUv.x, pUv.y) / virtualDim;

    // --- Tile selection in 2D (2x2 grid) ---
    float2 tilePos = float2(uvGlobal.x * tilesPerRow,
                            uvGlobal.y * tilesPerCol);

    int ix = (int)floor(tilePos.x);
    int iy = (int)floor(tilePos.y);

    ix = clamp(ix, 0, (int)tilesPerRow - 1);
    iy = clamp(iy, 0, (int)tilesPerCol - 1);

    int sliceIndex = iy * (int)tilesPerRow + ix; // 0..3

    // Local UV inside that tile
    float2 uvLocal;
    uvLocal.x = frac(tilePos.x);
    uvLocal.y = frac(tilePos.y);

    // Sample height from array
    // float heightPointData = g_heightArray.SampleLevel(g_sampler, float3(uvLocal, sliceIndex), lodLevel).r;
    // Convert UV to integer texel coordinates
    int2 pixel = int2(uvLocal * tileDim);

    // Center height
    float heightPointData =
        g_heightArray.Load(int4(pixel.x, pixel.y, sliceIndex, 0)).r;

    float artistScale = (5000.0f * 0.03f) * debug_scaler;
    wp.y = heightPointData * artistScale;

    float3 worldPos = wp.xyz;
    worldPos.x += ringOffset.x;
    worldPos.z += ringOffset.y;

    float texelWorld = ringSampleStep;
    
    int2 leftPixel = pixel + int2(-1, 0);
    int2 rightPixel = pixel + int2(1, 0);
    int2 downPixel = pixel + int2(0, -1);
    int2 upPixel = pixel + int2(0, 1);
    float hL = g_heightArray.Load(int4(leftPixel.x, leftPixel.y, sliceIndex, 0)).r * artistScale;
    float hR = g_heightArray.Load(int4(rightPixel.x, rightPixel.y, sliceIndex, 0)).r * artistScale;
    float hD = g_heightArray.Load(int4(downPixel.x, downPixel.y, sliceIndex, 0)).r * artistScale;
    float hU = g_heightArray.Load(int4(upPixel.x, upPixel.y, sliceIndex, 0)).r * artistScale;

    
    // Reconstruct normal
    float worldDelta = texelWorld * 2.0f; // World‑space delta for normal reconstruction
    float3 dx = float3(worldDelta, hR - hL, 0.0f);
    float3 dz = float3(0.0f, hU - hD, worldDelta);
    float3 n = normalize(cross(dz, dx));

    float seaTex = 0.2f / 100.0f;
    float seaLevel = seaTex * artistScale;

    o.water.x = step(wp.y, seaLevel);
    o.water.y = heightPointData;

    const float planetRadius = 600000.0f * planetScaleRatio;
    const float curvatureStrength = 1.0f;

    float3 rel = worldPos - cameraPos.xyz;
    float dist2 = dot(rel.xz, rel.xz);
    float curvatureOffset = -(dist2 / (2.0f * planetRadius)) * curvatureStrength;

    worldPos.y += curvatureOffset;
    wp = float4(worldPos, 1.0f);

    o.normalWS = n;

    float4 viewPos = mul(view, wp);
    o.position = mul(projection, viewPos);

    o.worldPos = worldPos;
    o.uvLocal = uvLocal;
    o.sliceIndex = sliceIndex;

    return o;
}

float4 PSMain(VSOut IN) : SV_Target
{
    const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f));
    const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
    const float ambient = 0.2f;

    int sliceIndex = IN.sliceIndex;

    float4 sampleData =
        g_albedoArray.Sample(g_sampler, float3(IN.uvLocal, sliceIndex));

    float wetness = clamp(IN.water.y, 0.0f, 1.0f);
    float blendStrength = 0.6f;
    float3 landColor = lerp(float3(0.8f, 0.75f, 0.5f), sampleData.rgb, blendStrength);
    // float3 waterColor = lerp(float3(0.0f, 0.3f, 0.8f), landColor, 0.5f);

    float3 base = landColor;
    float4 albedo = float4(base, 1.0f);

    float3 N = normalize(IN.normalWS);
    float diff = saturate(dot(N, -lightDir));
    float3 lit = (ambient + diff * 0.8f) * lightColor;

    return float4(lit * albedo.rgb, albedo.a);
}
