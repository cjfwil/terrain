cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;
    float4 cameraPos;
    float2 ringOffset;
    double timeElapsed;
    float ringWorldSize;
    float ringSampleStep;    
    float planetScaleRatio;   
    int terrainGridDimensionInVertices;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;    
    float3 normalWS : TEXCOORD2;        
    float2 water : TEXCOORD3;
};

Texture2D g_texture : register(t0);
Texture2D g_albedo : register(t1);
SamplerState g_sampler : register(s0);

// VSOut VSMain(float3 position : POSITION, float2 uv : TEXCOORD, float3 norm : NORMAL)
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
    
    float heightmapDim = 8192.0f*2; 
    float2 pUv = wp.xz + 0.5f + ringOffset;
    float2 terrainHeightmapUV = float2(1.0 - pUv.x, pUv.y) / heightmapDim;
    float heightPointData = g_texture.SampleLevel(g_sampler, terrainHeightmapUV, lodLevel).r;    
    float artistScale = (5000.0f*0.015f);  //controlled by human hand, dependent on heightmap
    wp.y = heightPointData*artistScale;

    // // --- morph factor near inner edge ---
    // float2 local  = position.xz;
    // float2 center = float2(gridDimSize * 0.5f, gridDimSize * 0.5f);
    // float2 d      = abs(local - center);
    // float dist    = max(d.x, d.y);

    // float innerStart = (gridDimSize * 0.5f) - 4.0f;
    // float innerEnd   = (gridDimSize * 0.5f);
    // float morph = saturate((dist - innerStart) / (innerEnd - innerStart));
    // morph *= step(1.5f, ringSampleStep); // only outer rings

    // // --- parent LOD sample ---
    // float parentStep = ringSampleStep * 2.0f;
    // float2 worldXZ   = wp.xz + ringOffset;
    // float2 parentXZ  = floor(worldXZ / parentStep) * parentStep;

    // float2 parentPUv = parentXZ + 0.5f;
    // float2 parentUV  = float2(1.0 - parentPUv.x, parentPUv.y) / heightmapDim;

    // float parentHeightData = g_texture.SampleLevel(g_sampler, parentUV, lodLevel).r;
    // float lowHeight = parentHeightData * artistScale;

    // // final height
    // float finalHeight = lerp(highHeight, lowHeight, morph);
    // wp.y = finalHeight;

    
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

    float seaTex = 0.2f / 100.0f;   // your actual water level in texture space
    float seaLevel = seaTex * artistScale;

    o.water.x = step(wp.y, seaLevel);
    o.water.y = heightPointData;



    // lower centre of the grid so they dont overlay (major greedy DONT KEEP THIS !!!!!!!)
    // TODO: use actual ring meshes like a normal person
    // float2 local  = position.xz;
    // float2 center = float2(gridDimSize, gridDimSize) * 0.5f;    
    // float2 d = abs(local - center);
    // float dist = max(d.x, d.y);    
    // float innerHalf = (gridDimSize-1) * 0.25f; // central 32x32 region    
    // float inside = step(dist, innerHalf);    
    // float isOuter = step(1.5f, ringSampleStep); // 1 if ringSampleStep >= 2
    // float maxDrop = 50.0f;
    // worldPos.y -= maxDrop * inside * isOuter;


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

    float3 waterColor = lerp(float3(0.0f, 0.3f, 0.8f), landColor, 0.5f);
    
    float3 base = lerp(landColor, waterColor, IN.water.x);    
    float4 albedo = float4(base, 1.0f);


    float3 N = normalize(IN.normalWS);

    float diff = saturate(dot(N, -lightDir));
    float3 lit = (ambient + diff * 0.8f) * lightColor;

    return float4(lit * albedo.rgb, albedo.a);
}

// float4 PSMain(VSOut IN) : SV_Target
// {    
//     float3 N = normalize(IN.normalWS);

//     // Map from [-1,1] to [0,1]
//     float3 colour = N * 0.5f + 0.5f;
//     // float3 colour = float3(1.0f, 1.0f, 1.0f);

//     return float4(colour, 1.0f);
// }

// float4 PSMain(VSOut IN) : SV_Target
// {
//     float h = saturate(IN.height);

//     // Colour ramp: low = blue, mid = green, high = white
//     float3 low  = float3(0.0, 0.2, 0.0);
//     float3 mid  = float3(0.1, 0.6, 0.1);
//     float3 high = float3(1.0, 1.0, 1.0);

//     float3 colour;

//     if (h < 0.5)
//     {
//         float t = h / 0.5;
//         colour = lerp(low, mid, t);
//     }
//     else
//     {
//         float t = (h - 0.5) / 0.5;
//         colour = lerp(mid, high, t);
//     }

//     return float4(colour, 1.0f);
// }
