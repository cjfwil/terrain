//TODO Do own constant buffer for sky only

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

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TexCoord;
};

VSOut VSMain(uint vertexID : SV_VertexID)
{
    VSOut o;

    // fullscreen triangle
    float2 pos[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    o.pos = float4(pos[vertexID], 0.0, 1.0);

    // UV from NDC
    o.uv = o.pos.xy * 0.5 + 0.5;

    return o;
}

// float4 PSMain(VSOut i) : SV_Target
// {
//     float3 dir = normalize(float3(i.uv * 2 - 1, 1));
//     // float3 sky = ComputeSky(dir); // your scattering function
//     float3 sky = dir;
//     return float4(sky, 1);
// }

float4 PSMain(VSOut i) : SV_Target
{
    float t = (float)timeElapsed * 0.05;

    // ---------------------------------------------------------
    // 1. Extract camera basis vectors from the view matrix
    // ---------------------------------------------------------
    float3 right   = normalize(view[0].xyz);
    float3 up      = normalize(view[1].xyz);
    float3 forward = normalize(view[2].xyz);

    // DirectX view matrix forward points *backwards*, so flip it
    // forward = -forward;

    // ---------------------------------------------------------
    // 2. Convert UV → NDC → world-space direction
    // ---------------------------------------------------------
    float2 ndc = i.uv * 2.0 - 1.0;

    float3 viewDir = normalize(
        forward +
        ndc.x * right +
        ndc.y * up
    );

    // ---------------------------------------------------------
    // 3. Sky gradient
    // ---------------------------------------------------------
    float h = saturate(viewDir.y); // 0 = horizon, 1 = zenith

    float3 zenithColor  = float3(0.05, 0.15, 0.35);
    float3 horizonColor = float3(0.65, 0.75, 0.9);

    float3 sky = lerp(horizonColor, zenithColor, h);

    // ---------------------------------------------------------
    // 4. Subtle time-based tint (optional)
    // ---------------------------------------------------------
    float3 timeTint = float3(
        0.02 * sin(t * 0.7),
        0.02 * sin(t * 1.1),
        0.02 * sin(t * 1.3)
    );

    sky += timeTint;


    // 3‑line dithering trick (blue-noise-ish)
    float2 noise = frac(sin(dot(i.uv, float2(12.9898, 78.233))) * 43758.5453);
    float dither = (noise.x + noise.y) * (1.0 / 255.0);
    sky += dither;

    return float4(sky, 1.0);
}