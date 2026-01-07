cbuffer SceneConstantBuffer : register(b0)
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;    
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

PSInput VSMain(float3 position : POSITION, float2 uv : TEXCOORD)
{
    PSInput result;

    float4 newPosition = float4(position, 1.0f);    
    newPosition = mul(world, newPosition);
    newPosition = mul(view, newPosition);
    newPosition = mul(projection, newPosition);
 

    result.position = newPosition;    
    result.uv = uv;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return g_texture.Sample(g_sampler, input.uv);
}
