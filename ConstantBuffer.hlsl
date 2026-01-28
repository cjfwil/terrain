#ifndef SHADER_COMMON_INCLUDED
#define SHADER_COMMON_INCLUDED

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
    float debug_scaler;

    // Bindless tile info
    uint tileCount;
    uint visibleTilesWidth;
};

#endif
