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

float3 FaceNormalFromDeriv(float3 p)
{
    float3 dx = ddx(p);
    float3 dy = ddy(p);
    float3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1e-6f) return float3(0.0f, 1.0f, 0.0f); // fallback
    return n / len;
}

float4 PSMain(VSOut IN) : SV_Target
{
    // hard-coded directional light (world space)
    const float3 lightDir = normalize(float3(0.5f, -1.0f, 0.2f)); // direction TO light
    const float3 lightColor = float3(1.0f, 0.98f, 0.9f);
    const float ambient = 0.2f;

    float4 albedo = g_texture.Sample(g_sampler, IN.uv);

    // flat (per-triangle) normal computed from interpolated world position
    float3 N = FaceNormalFromDeriv(IN.worldPos);

    // Lambert diffuse
    float diff = saturate(dot(N, -lightDir)); // -lightDir so positive when normal faces light
    float3 lit = albedo.rgb * (ambient + diff * 0.8f) * lightColor;

    return float4(lit, albedo.a);
}
