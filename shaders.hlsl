cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;
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
    float4 wp = mul(world, float4(position, 1.0f)); // world-space position
    o.worldPos = wp.xyz;

    float3 normalWS = mul((float3x3)world, norm);
    o.normalWS = normalize(normalWS);

    float4 viewPos = mul(view, wp);
    o.position = mul(projection, viewPos);
    o.uv = uv;
    return o;
}

float3 SmoothNormal(float3 worldPos)
{
    float3 dx = ddx(worldPos);
    float3 dy = ddy(worldPos);

    float3 n = cross(dx, dy);
    
    if (n.y < 0) n = -n;

    return normalize(n);
}


float4 PSMain(VSOut IN) : SV_Target
{
    const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f));
    const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
    const float ambient = 0.2f;

    float4 albedo = g_texture.Sample(g_sampler, IN.uv);

    float3 N = normalize(IN.normalWS);

    float diff = saturate(dot(N, -lightDir));
    float3 lit = (ambient + diff * 0.8f) * lightColor;

    return float4(lit * albedo.rgb, albedo.a);
}

// float4 PSMain(VSOut IN) : SV_Target
// {
//     // Use the world‑space normal passed from the vertex shader
//     float3 N = normalize(IN.normalWS);

//     // Map from [-1,1] to [0,1]
//     float3 color = N * 0.5f + 0.5f;

//     return float4(color, 1.0f);
// }