#include "ConstantBuffer.hlsl"

struct VSOut
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;    
    float3 normalWS : TEXCOORD2;        
    float2 water : TEXCOORD3;
};

Texture2D g_texture : register(t0);
Texture2D g_albedo : register(t2);
SamplerState g_sampler : register(s0);

float hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

VSOut VSMain(uint2 position : POSITION)
{    
    // int lodLevel = currentLodLevel*sampleLodLevelWithMipmaps;
    int lodLevel = 0;
    VSOut o;
    float4 wp = float4(position.x, 0.0f, position.y ,1.0f);
    
    float gridDimSize = terrainGridDimensionInVertices-1;    
    wp.xz -= (gridDimSize+2)/2.0f;
    wp.xz *= ringSampleStep;
    
    wp = mul(world, wp);

    // float heightmapDim = 8192.0f*2;
    float heightmapDim = 8192.0f / 2;
    float2 pUv = wp.xz + 0.5f + (ringOffset);
    float2 terrainHeightmapUV = float2(1.0 - pUv.x, pUv.y) / heightmapDim;
    float heightPointData = g_texture.SampleLevel(g_sampler, terrainHeightmapUV, lodLevel).r;    
    float artistScale = (5000.0f*0.015f)*debug_scaler;  //controlled by human hand, dependent on heightmap
    wp.y = heightPointData*artistScale;

    // --- Procedural micro-height detail ---
    // float detailFreq = 0.12; // how dense the bumps are
    // float detailAmp = 0.8 * debug_scaler; // height in world units    
    // float micro = hash21(wp.xz * detailFreq);
    // micro = micro * 2.0 - 1.0; // remap 0..1 → -1..1
    // wp.y += micro * detailAmp;

    float3 worldPos = wp.xyz;
    worldPos.x += ringOffset.x;
    worldPos.z += ringOffset.y;
    
    // --- QUICK NORMAL HACK: sample neighbors in heightmap space ---
    float texelWorld = ringSampleStep;                 // world units between samples
    float texelUV    = texelWorld / heightmapDim;      // UV offset that matches world spacing

    float hL = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2(-texelUV, 0), lodLevel).r * artistScale;
    float hR = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2( texelUV, 0), lodLevel).r * artistScale;
    float hD = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2(0, -texelUV), lodLevel).r * artistScale;
    float hU = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2(0,  texelUV), lodLevel).r * artistScale;

    
    float worldDelta = texelWorld * 2.0f; // distance between left and right sample in world units
    float3 dx = float3(worldDelta, hR - hL, 0.0f);
    float3 dz = float3(0.0f,      hU - hD, worldDelta);

    float3 n = normalize(cross(dz, dx));

    // --- Procedural micro-normal ---
    // float nFreq = 0.25;
    // float nAmp = 0.25;
    // float nx = hash21(worldPos.xz * nFreq + 10.0);
    // float nz = hash21(worldPos.xz * nFreq + 20.0);
    // float3 nDetail = normalize(float3(nx - 0.5, 1.0, nz - 0.5));
    // n = normalize(lerp(n, nDetail, nAmp));

    float seaTex = 0.2f / 100.0f;   // your actual water level in texture space
    float seaLevel = seaTex * artistScale;

    o.water.x = step(wp.y, seaLevel);
    o.water.y = heightPointData;

    // camera-relative curvature (visual only)
    const float planetRadius = 600000.0f * planetScaleRatio;
    const float curvatureStrength = 1.0f;

    float3 rel = worldPos - cameraPos.xyz;    
    float dist2 = dot(rel.xz, rel.xz); // squared distance    
    float curvatureOffset = -(dist2 / (2.0f * planetRadius)) * curvatureStrength; // parabolic approx of a sphere

    worldPos.y += curvatureOffset;
    wp = float4(worldPos, 1.0f);
    
    // float3 normalWS = mul((float3x3)world, norm);    
    // o.normalWS = normalize(normalWS);
    o.normalWS = n;

    float4 viewPos = mul(view, wp);
    o.position = mul(projection, viewPos);

    o.worldPos = worldPos;

    // float2 uv = float2(position) / (heightmapDim+1);
    o.uv = terrainHeightmapUV;
    
    return o;
}

float4 PSMain(VSOut IN) : SV_Target
{
    const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f));
    const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
    const float ambient = 0.2f;

    float4 sampleData = g_albedo.Sample(g_sampler, IN.uv);
    // float4 albedo = float4(0.8f,0.75f,0.5f,1.0f);
    // float3 wetlandColour = float3(0.8f, 0.75f, 0.5f) / 5.0f; 
    float wetness = clamp(IN.water.y*5, 0.25f, 1.0f);
    float blendStrength = 0.6f; // how strong the albedo layer is
    float3 landColor = lerp(float3(0.8f, 0.75f, 0.5f), sampleData.rgb, blendStrength);
    // --- Procedural albedo breakup ---
    // float cFreq = 0.35;
    // float cAmp = 0.15;

    // float noise = hash21(IN.worldPos.xz * cFreq);
    // landColor = lerp(landColor, landColor * (1.0 + cAmp), noise);

    float3 waterColor = lerp(float3(0.0f, 0.3f, 0.8f), landColor, 0.5f);
    
    float3 base = lerp(landColor, waterColor, IN.water.x);    
    float4 albedo = float4(base, 1.0f);


    float3 N = normalize(IN.normalWS);

    float diff = saturate(dot(N, -lightDir));
    float3 lit = (ambient + diff * 0.8f) * lightColor;

    return float4(lit * albedo.rgb, albedo.a);
}