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
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

VSOut VSMain(float3 position : POSITION, float2 uv : TEXCOORD)
{
    VSOut o;
    float4 wp = mul(world, float4(position, 1.0f)); // world-space position
    o.worldPos = wp.xyz;
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
    // hard-coded directional light (world space)
    const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f)); // direction TO light
    const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
    const float ambient = 0.2f;

    float4 albedo = g_texture.Sample(g_sampler, IN.uv);
    
    float3 N = SmoothNormal(IN.worldPos);

    // Lambert diffuse
    float diff = saturate(dot(N, -lightDir));
    float3 lit = albedo.rgb * (ambient + diff * 0.8f) * lightColor;

    return float4(lit, albedo.a);
}