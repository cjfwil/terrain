cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;
    float4 cameraPos;
    float2 ringOffset;
    float ringWorldSize;
    float ringSampleStep;    
    float planetScaleRatio;    
};

struct VSOut
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;    
    float3 normalWS : TEXCOORD2;    
    float water : TEXCOORD3;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

VSOut VSMain(float3 position : POSITION, float2 uv : TEXCOORD, float3 norm : NORMAL)
{
    VSOut o;
    
    float gridDimSize = 128.0f;
    float4 wp = mul(world, float4(position-gridDimSize/2.0f, 1.0f));        
    // wp.xz *= ringWorldSize / gridDimSize;
    wp.xz *= ringSampleStep;

    float heightmapDim = 8192.0f;    
    float2 pUv = wp.xz + 0.5f + ringOffset;
    float2 terrainHeightmapUV = float2(1.0 - pUv.x, pUv.y) / heightmapDim;
    float heightPointData = g_texture.SampleLevel(g_sampler, terrainHeightmapUV, 0).r;
    float artistScale = (5000.0f*0.02f); //controlled by human hand, dependent on heightmap
    // float artistScale = (5000.0f*0.2f); 
    wp.y = heightPointData*artistScale;
    
    float3 worldPos = wp.xyz;
    worldPos.x += ringOffset.x;
    worldPos.z += ringOffset.y;
    
    // --- QUICK NORMAL HACK: sample neighbors in heightmap space ---
    float texelWorld = ringSampleStep;                 // world units between samples
    float texelUV    = texelWorld / heightmapDim;      // UV offset that matches world spacing

    float hL = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2(-texelUV, 0), 0).r * artistScale;
    float hR = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2( texelUV, 0), 0).r * artistScale;
    float hD = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2(0, -texelUV), 0).r * artistScale;
    float hU = g_texture.SampleLevel(g_sampler, terrainHeightmapUV + float2(0,  texelUV), 0).r * artistScale;

    
    float worldDelta = texelWorld * 2.0f; // distance between left and right sample in world units
    float3 dx = float3(worldDelta, hR - hL, 0.0f);
    float3 dz = float3(0.0f,      hU - hD, worldDelta);

    float3 n = normalize(cross(dz, dx));

    float seaTex = 1.9f / 100.0f;   // your actual water level in texture space
    float seaLevel = seaTex * artistScale;

    o.water = step(wp.y, seaLevel);



    // lower centre of the grid so they dont overlay (major greedy DONT KEEP THIS !!!!!!!)
    // TODO: use actual ring meshes like a normal person
    float2 local  = position.xz;
    float2 center = float2(gridDimSize, gridDimSize) * 0.5f;    
    float2 d = abs(local - center);
    float dist = max(d.x, d.y);    
    float innerHalf = (gridDimSize-1) * 0.25f; // central 32x32 region    
    float inside = step(dist, innerHalf);    
    float isOuter = step(1.5f, ringSampleStep); // 1 if ringSampleStep >= 2
    float maxDrop = 50.0f;
    worldPos.y -= maxDrop * inside * isOuter;


    // camera-relative curvature (visual only)
    const float planetRadius = 600000.0f * planetScaleRatio;
    const float curvatureStrength = 1.0f;

    float3 rel = worldPos - cameraPos.xyz;    
    float dist2 = dot(rel.xz, rel.xz); // squared distance    
    float curvatureOffset = -(dist2 / (2.0f * planetRadius)) * curvatureStrength; // parabolic approx of a sphere

    worldPos.y += curvatureOffset;
    wp = float4(worldPos, 1.0f);
    
    float3 normalWS = mul((float3x3)world, norm);    
    // o.normalWS = normalize(normalWS);
    o.normalWS = n;

    float4 viewPos = mul(view, wp);
    o.position = mul(projection, viewPos);

    o.worldPos = worldPos;
    o.uv = uv;
    
    return o;
}


float4 PSMain(VSOut IN) : SV_Target
{
    const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f));
    const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
    const float ambient = 0.2f;

    // float4 albedo = g_texture.Sample(g_sampler, IN.uv);
    // float4 albedo = float4(0.8f,0.75f,0.5f,1.0f);
    float3 landColor  = float3(0.8f, 0.75f, 0.5f);
    float3 waterColor = float3(0.0f, 0.3f, 0.8f);

    float3 base = lerp(landColor, waterColor, IN.water);
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
