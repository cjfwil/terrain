cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;
    float4 cameraPos;
    float planetScaleRatio;
};

struct VSOut
{
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float2 uv       : TEXCOORD1;    
    float3 normalWS : TEXCOORD2;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

VSOut VSMain(float3 position : POSITION, float2 uv : TEXCOORD, float3 norm : NORMAL)
{
    VSOut o;
    
    float4 wp = mul(world, float4(position, 1.0f));

    float heightmapDim = 8192.0f;    
    float2 pUv = wp.xz + 0.5f;
    float2 terrainHeightmapUV = float2(1.0 - pUv.x, pUv.y) / heightmapDim;
    wp.y = g_texture.SampleLevel(g_sampler, terrainHeightmapUV, 0).r*(5000.0f*0.02f);
    
    float3 worldPos = wp.xyz;

    // camera-relative curvature (visual only)
    const float planetRadius = 600000.0f * planetScaleRatio;
    const float curvatureStrength = 1.0f;

    float3 rel = worldPos - cameraPos.xyz;    
    float dist2 = dot(rel.xz, rel.xz); // squared distance    
    float curvatureOffset = -(dist2 / (2.0f * planetRadius)) * curvatureStrength; // parabolic approx of a sphere

    worldPos.y += curvatureOffset;
    wp = float4(worldPos, 1.0f);
    
    float3 normalWS = mul((float3x3)world, norm);
    o.normalWS = normalize(normalWS);

    float4 viewPos = mul(view, wp);
    o.position = mul(projection, viewPos);

    o.worldPos = worldPos;
    o.uv = uv;
    return o;
}


// float4 PSMain(VSOut IN) : SV_Target
// {
//     const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f));
//     const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
//     const float ambient = 0.2f;

//     float4 albedo = g_texture.Sample(g_sampler, IN.uv);

//     float3 N = normalize(IN.normalWS);

//     float diff = saturate(dot(N, -lightDir));
//     float3 lit = (ambient + diff * 0.8f) * lightColor;

//     return float4(lit * albedo.rgb, albedo.a);
// }

float4 PSMain(VSOut IN) : SV_Target
{    
    float3 N = normalize(IN.normalWS);

    // Map from [-1,1] to [0,1]
    // float3 colour = N * 0.5f + 0.5f;
    float3 colour = float3(1.0f, 1.0f, 1.0f);

    return float4(colour, 1.0f);
}