#pragma warning(disable : 5045) // disabling the spectre mitigation warning (not relevant because we are a game, no sensitive information should be in this program)
#pragma comment(lib, "SDL3.lib")
#pragma comment(lib, "SDL3_image.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "imgui.lib")
#if defined(_DEBUG)
#pragma comment(lib, "DirectXTex.lib")
#else
#pragma comment(lib, "DirectXTex_release.lib")
#endif
#pragma comment(lib, "ole32.lib")

#pragma warning(push, 0)
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_dx12.h>

#include <DirectXTex.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3_image/SDL_image.h>

#pragma warning(pop)

#include "src/metadata.h"
#include "src/error.h"
#include "src/profiling.h"
#include "src/v3.h"
#include "src/baked_heightmap_mesh.h"

#include "src/render_dx12.h"

#define PI 3.1415926535897932384626433832795f
#define PI_OVER_2 1.5707963267948966192313216916398f

struct clipmap_mesh_data
{
    uint32_t *indexData;
    size_t indexBufferDataSize;
    uint32_t indexCount;
};

clipmap_mesh_data GenerateClipmapMeshData(int N, bool fillHole)
{
    clipmap_mesh_data result = {};
    // const int N = terrainGridDimensionInVertices;
    int quadsPerRow = N - 1;
    int quadsTotal = quadsPerRow * quadsPerRow;
    int indicesPerQuad = 6;
    uint32_t indexCount = quadsTotal * indicesPerQuad;
    size_t indexBufferDataSize = indexCount * sizeof(uint32_t);

    size_t maxIndexCount = (N - 1) * (N - 1) * 6;
    indexBufferDataSize = maxIndexCount * sizeof(uint32_t);
    uint32_t *indices = (uint32_t *)SDL_malloc(indexBufferDataSize);

    int idx = 0;

    int innerSize = N / 2 - 1; // example: hole is N/4 wide
    if (fillHole)
        innerSize = 0;
    int innerHalf = innerSize / 2;

    int cx = N / 2; // grid center
    int cy = N / 2;

    for (int y = 0; y < N - 1; ++y)
    {
        for (int x = 0; x < N - 1; ++x)
        {
            // Compute quad center in grid space
            int qx = x + 0.5f;
            int qy = y + 0.5f;

            // Check if quad is inside the hollow center
            bool insideX = abs(qx - cx) < innerHalf;
            bool insideY = abs(qy - cy) < innerHalf;

            if (insideX && insideY)
                continue; // skip this quad entirely

            uint32_t v0 = x + y * N;
            uint32_t v1 = (x + 1) + y * N;
            uint32_t v2 = x + (y + 1) * N;
            uint32_t v3 = (x + 1) + (y + 1) * N;

            // tri 1
            indices[idx++] = v0;
            indices[idx++] = v2;
            indices[idx++] = v1;

            // tri 2
            indices[idx++] = v1;
            indices[idx++] = v2;
            indices[idx++] = v3;
        }
    }
    result.indexData = indices;
    result.indexBufferDataSize = idx * sizeof(uint32_t);
    result.indexCount = idx;
    return result;
}

inline float randf()
{
    return (float)rand() / (float)RAND_MAX;
}

void PrintMatrix(const DirectX::XMFLOAT4X4 &matrix)
{
    const float *m = &matrix._11;
    SDL_Log("[\n");
    SDL_Log("  %.3f  %.3f  %.3f  %.3f\n", m[0], m[1], m[2], m[3]);
    SDL_Log("  %.3f  %.3f  %.3f  %.3f\n", m[4], m[5], m[6], m[7]);
    SDL_Log("  %.3f  %.3f  %.3f  %.3f\n", m[8], m[9], m[10], m[11]);
    SDL_Log("  %.3f  %.3f  %.3f  %.3f\n", m[12], m[13], m[14], m[15]);
    SDL_Log("]\n\n");
}

static struct
{
    SDL_Window *window;
    Uint64 msElapsedSinceSDLInit;
    uint64_t ticksElapsed = 0;
    double timeElapsed = 0;
    bool fullscreen;
    bool isRunning;
} programState;

int main(void)
{
    // todo game state struct:
    // static v3 cameraPos = {4096.0f, 120.0f, 4096.0f};
    static v3 cameraPos = {0.0f, 80.0f, 0.0f};
    static float cameraYaw = 2.45f;
    static float cameraPitch = 0.0f;
    float fov = DirectX::XMConvertToRadians(60.0f);

    if (!SetExtendedMetadata())
        return 1;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        err("SDL_Init failed");
        return 1;
    }

    int width = 1920;
    int height = 1080;

    SDL_WindowFlags sdlWindowFlags = SDL_WINDOW_FULLSCREEN;
    programState.window = SDL_CreateWindow(APP_WINDOW_TITLE, (int)width, (int)height, sdlWindowFlags);
    if (!programState.window)
    {
        err("SDL_CreateWindow failed");
        return 1;
    }

    // init d3d12 pipeline

    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ID3D12Debug *debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    HRESULT hr = S_OK;

    hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&renderState.factory));
    if (FAILED(hr))
    {
        errhr("CreateDXGIFactory2 failed", hr);
        return 1;
    }

    hr = renderState.factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&renderState.hardwareAdapter));
    if (FAILED(hr))
    {
        errhr("EnumAdapterByGpuPreference failed", hr);
        return 1;
    }

    hr = D3D12CreateDevice(renderState.hardwareAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&renderState.device));
    if (FAILED(hr))
    {
        errhr("D3D12CreateDevice failed", hr);
        return 1;
    }

    // command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    hr = renderState.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&renderState.commandQueue));
    if (FAILED(hr))
    {
        errhr("CreateCommandQueue failed", hr);
        return 1;
    }

    static bool vsync = true;
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = (UINT)width;
    swapChainDesc.Height = (UINT)height;
    swapChainDesc.Format = renderState.rtvFormat;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = (UINT)renderState.frameCount;
    swapChainDesc.Scaling = DXGI_SCALING_NONE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    SDL_PropertiesID props = SDL_GetWindowProperties(programState.window);
    HWND hwnd = nullptr;
    hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd)
    {
        errhr("Failed to get HWND", hr);
        return 1;
    }

    hr = renderState.factory->CreateSwapChainForHwnd(renderState.commandQueue, hwnd, &swapChainDesc, nullptr, nullptr, &renderState.swapChain1);
    if (FAILED(hr))
    {
        errhr("CreateSwapChainForHwnd failed", hr);
        return 1;
    }

    hr = renderState.swapChain1->QueryInterface(IID_PPV_ARGS(&renderState.swapChain));
    if (FAILED(hr))
    {
        errhr("QueryInterface on swapChain1 failed", hr);
        return 1;
    }
    renderState.swapChain1->Release();
    UINT frameIndex = renderState.swapChain->GetCurrentBackBufferIndex();

    // create descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = (UINT)renderState.frameCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;

    hr = renderState.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&renderState.rtvHeap));
    if (FAILED(hr))
    {
        errhr("CreateDescriptorHeap failed (rtvHeap)", hr);
        return 1;
    }

    const int maxSRVDescriptors = 1024;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = maxSRVDescriptors; // CBV + SRV
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = renderState.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&renderState.srvHeap));
    if (FAILED(hr))
    {
        errhr("CreateDescriptorHeap failed (srvHeap)", hr);
        return 1;
    }

    // create imgui srv descriptor heap
    //  Create ImGui SRV heap
    D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc = {};
    imguiHeapDesc.NumDescriptors = 64; // plenty for font + user textures
    imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = renderState.device->CreateDescriptorHeap(&imguiHeapDesc, IID_PPV_ARGS(&renderState.imguiSrvHeap));
    if (FAILED(hr))
    {
        errhr("CreateDescriptorHeap failed (imguiSrvHeap)", hr);
        return 1;
    }

    // Initialize ImGui allocator
    renderState.imguiSrvAllocator.Create(renderState.device, renderState.imguiSrvHeap);

    renderState.cbvSrvDescriptorSize = renderState.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    renderState.rtvDescriptorSize = renderState.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // create frame resources
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandleSetup(renderState.rtvHeap->GetCPUDescriptorHandleForHeapStart());

    // rtv for each buffer (double or triple buffering)
    for (UINT n = 0; n < renderState.frameCount; n++)
    {
        hr = renderState.swapChain->GetBuffer(n, IID_PPV_ARGS(&renderState.renderTargets[n]));
        if (FAILED(hr))
        {
            errhr("GetBuffer failed", hr);
            return 1;
        }
        renderState.device->CreateRenderTargetView(renderState.renderTargets[n], nullptr, rtvHandleSetup);
        rtvHandleSetup.Offset(1, renderState.rtvDescriptorSize);

        hr = renderState.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&renderState.commandAllocators[n]));
        if (FAILED(hr))
        {
            errhr("CreateCommandAllocator failed", hr);
            return 1;
        }
    }

    // depth buffer view
    // ------------------------------
    // Create depth buffer + DSV heap
    // ------------------------------
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

    hr = renderState.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&renderState.dsvHeap));
    if (FAILED(hr))
    {
        errhr("CreateDescriptorHeap failed (dsvHeap)", hr);
        return 1;
    }

    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Alignment = 0;
    depthDesc.Width = (UINT64)width;
    depthDesc.Height = (UINT64)height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = DXGI_FORMAT_D32_FLOAT;
    depthClear.DepthStencil.Depth = 1.0f;
    depthClear.DepthStencil.Stencil = 0;

    CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);

    hr = renderState.device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthClear,
        IID_PPV_ARGS(&renderState.depthBuffer));
    if (FAILED(hr))
    {
        errhr("CreateCommittedResource failed (depthBuffer)", hr);
        return 1;
    }

    renderState.device->CreateDepthStencilView(
        renderState.depthBuffer,
        nullptr,
        renderState.dsvHeap->GetCPUDescriptorHandleForHeapStart());

    hr = renderState.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&renderState.bundleAllocator));
    if (FAILED(hr))
    {
        errhr("CreateCommandAllocator failed", hr);
        return 1;
    }

    // load assets
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    hr = renderState.device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData));
    if (FAILED(hr))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    // bindless root signature:
    CD3DX12_DESCRIPTOR_RANGE1 srvRange;
    srvRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                  maxSRVDescriptors,
                  0,                                                 // base shader register t0
                  1,                                                 // register space = space1
                  D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE); // volatile is fine for bindless

    // Root parameters
    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    rootParameters[0].InitAsConstantBufferView(0); // const buffer
    rootParameters[1].InitAsDescriptorTable(1, &srvRange, D3D12_SHADER_VISIBILITY_ALL);

    // TODO: abstract out into reusable
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    // sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; // TODO: make different filter for heightmaps versus albedo
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 16;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_MIRROR; // these are only specifically for the world terrain (wrap only going sideways)
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    sampler.MipLODBias = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, rootSignatureFlags);
    ID3DBlob *signature;
    ID3DBlob *error;
    hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error);
    if (FAILED(hr))
    {
        errhr("D3D12SerializeRootSignature failed", hr);
        return 1;
    }

    hr = renderState.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&renderState.rootSignature));
    if (FAILED(hr))
    {
        errhr("CreateRootSignature failed", hr);
        return 1;
    }

    // create pipeline state (including shaders)
    // create shaders here
    d3d12_shader_pair terrainShaderPair;
    if (!terrainShaderPair.create(L"shaders.hlsl"))
    {
        err("Failed to create shader pair.");
        return 1;
    }

    d3d12_shader_pair skyShader;
    if (!skyShader.create(L"sky.hlsl"))
    {
        err("Failed to create sky shader pair");
        return 1;
    }

    // old mesh stuff
    // D3D12_INPUT_ELEMENT_DESC inputElementDesc[] =
    //     {
    //         {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    //         {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    //         {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    // heightmap mesh stuff
    D3D12_INPUT_ELEMENT_DESC inputElementDesc[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R16G16_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            // {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            // {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // rasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    // rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // Disable culling (backface culling)

    d3d12_pipeline_state terrainPSO;
    D3D12_INPUT_LAYOUT_DESC inputLayout = {inputElementDesc, _countof(inputElementDesc)};
    if (!terrainPSO.create(inputLayout, &terrainShaderPair, rasterizerDesc, true))
    {
        err("Failed to create terrain pipleline state");
        return 1;
    }

    d3d12_pipeline_state skyPSO;
    if (!skyPSO.create({nullptr, 0}, &skyShader, CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT), false))
    {
        err("Failed to create sky pipleline state");
        return 1;
    }

    hr = renderState.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, renderState.commandAllocators[frameIndex], nullptr, IID_PPV_ARGS(&renderState.commandList));
    if (FAILED(hr))
    {
        errhr("CreateCommandList failed", hr);
        return 1;
    }

    const float aspectRatio = (float)width / (float)height;

    int bakedResult = 0;
    // bakedResult = baked_heightmap_mesh.baked(); //comment this out to disable the baked mesh
    if (bakedResult != 0)
    {
        err("Baked Mesh failed");
        return 1;
    }

    const int terrainGridDimensionInVertices = 256 + 1;
    float terrainGridDimensionInWorldUnits = terrainGridDimensionInVertices - 1;
    constantBufferData.terrainGridDimensionInVertices = terrainGridDimensionInVertices;
    d3d12_vertex_buffer terrainGridVB;
    // float baseGridSize = terrainGridDimensionInVertices; // world units
    const int terrainGridVertexCount = terrainGridDimensionInVertices * terrainGridDimensionInVertices;
    const size_t terrainGridVertexDataSize = terrainGridVertexCount * sizeof(vertex_optimised);
    vertex_optimised *terrainGridVertexData = (vertex_optimised *)SDL_malloc(terrainGridVertexDataSize);
    for (int y = 0; y < terrainGridDimensionInVertices; ++y)
    {
        for (int x = 0; x < terrainGridDimensionInVertices; ++x)
        {
            vertex_optimised v = {};
            v.x = x;
            v.y = y;

            terrainGridVertexData[x + y * terrainGridDimensionInVertices] = v;
        }
    }
    if (!terrainGridVB.create_and_upload(terrainGridVertexDataSize, terrainGridVertexData, sizeof(vertex_optimised)))
    {
        err("Failed to create or upload terrain grid mesh");
        return 1;
    }

    d3d12_index_buffer terrainGridCentreIB;
    clipmap_mesh_data clipmapCentre = GenerateClipmapMeshData(terrainGridDimensionInVertices, true);
    if (!terrainGridCentreIB.create_and_upload(clipmapCentre.indexBufferDataSize, clipmapCentre.indexData))
    {
        err("Failed to create or upload terrain grid index buffer (centre piece)");
        return 1;
    }

    d3d12_index_buffer terrainGridRingIB;
    clipmap_mesh_data clipmapRing = GenerateClipmapMeshData(terrainGridDimensionInVertices, false);
    if (!terrainGridRingIB.create_and_upload(clipmapRing.indexBufferDataSize, clipmapRing.indexData))
    {
        err("Failed to create or upload terrain grid index buffer (ring piece)");
        return 1;
    }

    int maxClipmapRings = 8; // for terrain
    int activeClipmapRings = 5;
    // TODO: make reusuable constant buffer stuff

    // create constant buffer
    const UINT constantBufferSize = 256U;
    static UINT *CbvDataBegin = nullptr;

    const UINT TotalCBSize = constantBufferSize * maxClipmapRings;

    CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(TotalCBSize);
    hr = renderState.device->CreateCommittedResource(
        &heapPropsUpload,
        D3D12_HEAP_FLAG_NONE,
        &constantBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&renderState.constantBuffer));
    if (FAILED(hr))
    {
        errhr("CreateCommittedResource failed", hr);
        return 1;
    }

    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = renderState.constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = constantBufferSize;
    renderState.device->CreateConstantBufferView(&cbvDesc, renderState.srvHeap->GetCPUDescriptorHandleForHeapStart());

    CD3DX12_RANGE readRangeCBV(0, 0);
    renderState.constantBuffer->Map(0, &readRangeCBV, reinterpret_cast<void **>(&CbvDataBegin));
    memcpy(CbvDataBegin, &constantBufferData, sizeof(constantBufferData));

    // CREATE BUNDLE
    hr = renderState.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, renderState.bundleAllocator, terrainPSO.pipelineState, IID_PPV_ARGS(&renderState.bundle));
    if (FAILED(hr))
    {
        errhr("CreateCommandList failed", hr);
        return 1;
    }

    renderState.bundle->SetGraphicsRootSignature(renderState.rootSignature);
    renderState.bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderState.bundle->IASetVertexBuffers(0, 1, &baked_heightmap_mesh.terrainMeshVertexBuffer.vertexBufferView);
    renderState.bundle->IASetIndexBuffer(&baked_heightmap_mesh.terrainMeshIndexBuffer.indexBufferView);
    renderState.bundle->DrawIndexedInstanced(baked_heightmap_mesh.terrainMeshIndexBufferNum, 1, 0, 0, 0);

    // renderState.bundle->DrawInstanced(terrainMeshSizeInVertices, 1, 0, 0);
    renderState.bundle->Close();

    // beginning of texture
    // d3d12_texture gravelTexture;
    // if (!gravelTexture.create(L"gravel.dds"))
    // {
    //     err("Create Texture (gravelTexture) failed.");
    //     return 1;
    // }

    // 3×3 = 9 tiles
    const uint32_t tileNum = 9;

    // Arrays of wchar_t filenames
    static wchar_t heightmapFilenames[9][256];
    static wchar_t albedoFilenames[9][256];

    uint32_t startingSegmentX = 0;
    uint32_t startingSegmentY = 6;

    uint32_t endingSegmentX = startingSegmentX +3;
    uint32_t endingSegmentY = startingSegmentY +3;

    // Fill in row-major order (0,0) → (2,0) → (0,1) → ... → (2,2)
    uint32_t index = 0;
    for (uint32_t y = startingSegmentY; y < endingSegmentY; ++y)
    {
        for (uint32_t x = startingSegmentX; x < endingSegmentX; ++x)
        {
            swprintf(heightmapFilenames[index], 256, L"data\\height\\chunk_height_%u_%u.dds", x, y);
            swprintf(albedoFilenames[index], 256, L"data\\albedo\\chunk_albedo_%u_%u.dds", x, y);
            index++;
        }
    }

    // Create the array textures
    // d3d12_texture_array heightArray;
    // d3d12_texture_array albedoArray;

    // // TODO: TEST WITH UNCOMPRESSED FORMAT
    // //  Create empty array resources (one SRV each)
    // if (!heightArray.create(4096, 4096, tileNum, DXGI_FORMAT_BC4_UNORM, 1, 0))
    // {
    //     err("Failed to create height array");
    //     return 1;
    // }

    // if (!albedoArray.create(
    //         4096,
    //         4096,
    //         tileNum,
    //         DXGI_FORMAT_BC1_UNORM,
    //         13, // mipLevels TODO: automatically do this.
    //         1   // SRV index in heap
    //         ))
    // {
    //     err("Failed to create albedo array");
    //     return 1;
    // }

    // // Upload each tile into its slice
    // for (uint32_t i = 0; i < tileNum; i++)
    // {
    //     if (!heightArray.uploadSliceFromDDS(heightmapFilenames[i], i, false))
    //     {
    //         err("Failed to upload height slice");
    //         return 1;
    //     }

    //     if (!albedoArray.uploadSliceFromDDS(albedoFilenames[i], i, true))
    //     {
    //         err("Failed to upload albedo slice");
    //         return 1;
    //     }
    // }

    d3d12_bindless_texture heightTiles[tileNum];
    d3d12_bindless_texture albedoTiles[tileNum];

    for (UINT i = 0; i < tileNum; i++)
    {
        heightTiles[i].loadFromDDS(heightmapFilenames[i], i, false);
        albedoTiles[i].loadFromDDS(albedoFilenames[i], tileNum + i, true);
    }

    constantBufferData.tileCount = tileNum;
    // end of texture

    renderState.commandList->Close();
    ID3D12CommandList *commandListsSetup[] = {renderState.commandList};
    renderState.commandQueue->ExecuteCommandLists(_countof(commandListsSetup), commandListsSetup);

    // create synchronisation objects

    UINT64 fenceValues[renderState.frameCount] = {};
    hr = renderState.device->CreateFence(fenceValues[frameIndex], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&renderState.fence));
    fenceValues[frameIndex]++;
    if (FAILED(hr))
    {
        errhr("CreateFence failed", hr);
        return 1;
    }

    HANDLE fenceEvent = nullptr;
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fenceEvent == nullptr)
    {
        err("CreateEvent failed (fenceEvent)");
        return 1;
    }

    static CD3DX12_VIEWPORT viewport = {};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    static CD3DX12_RECT scissorRect = {};
    scissorRect.left = 0;
    scissorRect.top = 0;
    scissorRect.right = static_cast<LONG>(width);
    scissorRect.bottom = static_cast<LONG>(height);

    static bool enableImgui = true;
    bool imguiInitialised = false;
    if (enableImgui)
    {
        // imgui setup
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
        ImGui::StyleColorsDark();

        ImGui_ImplSDL3_InitForD3D(programState.window);

        // renderState.imguiSrvAllocator.Create(renderState.device, renderState.srvHeap);
        ImGui_ImplDX12_InitInfo init_info = {};
        init_info.Device = renderState.device;
        init_info.CommandQueue = renderState.commandQueue;
        init_info.NumFramesInFlight = renderState.frameCount;
        init_info.RTVFormat = renderState.rtvFormat;
        init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        init_info.SrvDescriptorHeap = renderState.imguiSrvHeap;
        init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_handle)
        { return renderState.imguiSrvAllocator.Alloc(out_cpu_handle, out_gpu_handle); };
        init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo *, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return renderState.imguiSrvAllocator.Free(cpu_handle, gpu_handle); };
        ImGui_ImplDX12_Init(&init_info);

        imguiInitialised = true;
    }

    // enable mouse look
    static bool mouseLookEnabled = true;
    SDL_SetWindowRelativeMouseMode(programState.window, true);
    float mouseXrel = 0.0f;
    float mouseYrel = 0.0f;

    float inputMotionXAxis = 0.0f;
    float inputMotionYAxis = 0.0f;

    // gamepad look
    SDL_Gamepad *gamepad = nullptr;

    // input
    static struct
    {
        bool w;
        bool a;
        bool s;
        bool d;
    } inputState;

    uint64_t lastCounter = SDL_GetPerformanceCounter();
    float deltaTime = 0.0f;
    // main program

    QueryPerformanceFrequency(&qpc_freq);

    programState.isRunning = true;
    while (programState.isRunning)
    {
        profiling.update_ms = qpc_ms(profiling.t0, profiling.t1);
        profiling.render_ms = qpc_ms(profiling.t1, profiling.t2);
        profiling.present_ms = qpc_ms(profiling.t2, profiling.t3);
        profiling.frame_ms = qpc_ms(profiling.t0, profiling.t3);
        QueryPerformanceCounter(&profiling.t0);

        mouseXrel = 0.0f;
        mouseYrel = 0.0f;
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent))
        {
            if (enableImgui)
                ImGui_ImplSDL3_ProcessEvent(&sdlEvent);
            switch (sdlEvent.type)
            {
            case SDL_EVENT_QUIT:
            {
                programState.isRunning = false;
            }
            break;
            case SDL_EVENT_GAMEPAD_ADDED:
            {
                if (gamepad == nullptr)
                {
                    gamepad = SDL_OpenGamepad(sdlEvent.gdevice.which);
                    if (!gamepad)
                    {
                        err("Failed to open gamepad");
                    }
                }
            }
            break;
            case SDL_EVENT_GAMEPAD_REMOVED:
            {
                if (gamepad && (SDL_GetGamepadID(gamepad) == sdlEvent.gdevice.which))
                {
                    SDL_CloseGamepad(gamepad);
                    gamepad = nullptr;
                }
            }
            break;
            case SDL_EVENT_MOUSE_MOTION:
            {
                if (mouseLookEnabled)
                {
                    mouseXrel = sdlEvent.motion.xrel;
                    mouseYrel = sdlEvent.motion.yrel;
                }
            }
            break;
            case SDL_EVENT_KEY_DOWN:
            {
                SDL_Keycode sym = sdlEvent.key.key;
                if (sym == SDLK_W)
                    inputState.w = true;
                if (sym == SDLK_A)
                    inputState.a = true;
                if (sym == SDLK_S)
                    inputState.s = true;
                if (sym == SDLK_D)
                    inputState.d = true;
                if (sym == SDLK_F1)
                {
                    mouseLookEnabled = !mouseLookEnabled;
                    SDL_SetWindowRelativeMouseMode(programState.window, mouseLookEnabled);
                }
            }
            break;
            case SDL_EVENT_KEY_UP:
            {
                SDL_Keycode sym = sdlEvent.key.key;
                if (sym == SDLK_W)
                    inputState.w = false;
                if (sym == SDLK_A)
                    inputState.a = false;
                if (sym == SDLK_S)
                    inputState.s = false;
                if (sym == SDLK_D)
                    inputState.d = false;
            }
            break;
            }
        }
        programState.msElapsedSinceSDLInit = SDL_GetTicks();

        static float debugBoostSpeed = 2.7778f; // same speed as average 16th century merchant vessel
        static int debugDrawOnlyChunk = 0;

        if (enableImgui)
        {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            static bool show_demo_window = false;
            if (show_demo_window)
                ImGui::ShowDemoWindow(&show_demo_window);

            if (baked_heightmap_mesh.created)
                baked_heightmap_mesh.imgui_show_options();
            ImGui::Text("Application average %.3f ms/frame (%.2f FPS)",
                        1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);

            ImGui::Text("Terrain Dimension vertices: %d", constantBufferData.terrainGridDimensionInVertices);

            static int planetScaleRatioDenom = 50;
            ImGui::SliderInt("Planet Scale 1:X", &planetScaleRatioDenom, 1, 100);
            constantBufferData.planetScaleRatio = 1.0f / (float)planetScaleRatioDenom;

            ImGui::SliderInt("Clipmaps", &activeClipmapRings, 1, maxClipmapRings);

            ImGui::SliderFloat("Debug Speed Boost", &debugBoostSpeed, 1.0f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Debug Scaler", &constantBufferData.debug_scaler, 0.25f, 4.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
            // basic profiling

            uint64_t frameHistoryIndex = programState.ticksElapsed % 256;
            profiling.frameRateHistory[frameHistoryIndex] = ImGui::GetIO().Framerate;
            ImGui::PlotLines("Frametime", profiling.frameRateHistory, IM_ARRAYSIZE(profiling.frameRateHistory), (int)frameHistoryIndex);

            // camera info
            ImGui::Text("Camera Pos: %.2f, %.2f, %.2f", cameraPos.x, cameraPos.y, cameraPos.z);
            ImGui::Text("Camera Yaw: %.2f. Pitch %.2f", cameraYaw, cameraPitch);

            // options
            ImGui::Checkbox("VSync", &vsync);

            // FPS values
            profiling.update_fps();
            ImGui::Text("1%% Low:  %.2f FPS, %.3f ms", profiling.fps_1pct_low, 1000.0f / profiling.fps_1pct_low);
            ImGui::Text("0.1%% Low:%.2f FPS, %.3f ms", profiling.fps_01pct_low, 1000.0f / profiling.fps_01pct_low);
            ImGui::Text("Peak:    %.2f FPS, %.3f ms", profiling.fps_peak, 1000.0f / profiling.fps_peak);
            ImGui::Text("Min:     %.2f FPS, %.3f ms", profiling.fps_min, 1000.0f / profiling.fps_min);
            ImGui::Text("Max:     %.2f FPS, %.3f ms", profiling.fps_max, 1000.0f / profiling.fps_max);

            ImGui::Text("Update:  %.3f ms", profiling.update_ms);
            ImGui::Text("Render:  %.3f ms", profiling.render_ms);
            ImGui::Text("Present: %.3f ms", profiling.present_ms);
            ImGui::Text("Frame:   %.3f ms", profiling.frame_ms);

            if (gamepad)
            {
                ImGui::Text("Gamepad Connected");
                ImGui::Text("Left Stick X: %.3f", inputMotionXAxis);
                ImGui::Text("Left Stick Y: %.3f", inputMotionYAxis);
            }
            if (ImGui::Button("Quit"))
            {
                programState.isRunning = false;
            }
        }

        // main loop main body
        uint64_t currentCounter = SDL_GetPerformanceCounter();
        deltaTime = ((float)(currentCounter - lastCounter)) / (float)SDL_GetPerformanceFrequency();
        if (deltaTime > 0.1f)
            deltaTime = 0.1f;

        lastCounter = currentCounter;

        // update here
        const float translationSpeed = 0.2f * deltaTime;
        const float offsetBounds = 1.0f;
        static float movingPoint = 0;

        movingPoint += translationSpeed;
        if (movingPoint > offsetBounds)
        {
            movingPoint = -offsetBounds;
        }

        float epsilon = 0.0001f;

        // static bool enableMouseLook = true;
        SDL_UpdateGamepads();

        // pitch calculations
        // input from the controller
        float lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        float ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
        float rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
        float ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f;
        float squareDeadzone = 0.005f;

        if (-squareDeadzone < lx && lx < squareDeadzone)
        {
            lx = 0.0f;
        }
        if (-squareDeadzone < ly && ly < squareDeadzone)
        {
            ly = 0.0f;
        }
        if (-squareDeadzone < rx && rx < squareDeadzone)
        {
            rx = 0.0f;
        }
        if (-squareDeadzone < ry && ry < squareDeadzone)
        {
            ry = 0.0f;
        }

        inputMotionXAxis = 0;
        inputMotionYAxis = 0;
        if (gamepad)
        {
            inputMotionXAxis = -lx;
            inputMotionYAxis = -ly;
        }
        if (inputState.w)
            inputMotionYAxis = 1.0f;
        if (inputState.s)
            inputMotionYAxis = -1.0f;
        if (inputState.a)
            inputMotionXAxis = 1.0f;
        if (inputState.d)
            inputMotionXAxis = -1.0f;

        cameraPitch -= mouseYrel * deltaTime * 0.05f; // input from the mouse
        cameraPitch -= ry * deltaTime * 3.5f;

        // what should happen when both mouse and joystick input at once?

        cameraPitch = SDL_clamp(cameraPitch, -PI_OVER_2 + epsilon, PI_OVER_2 - epsilon);

        cameraYaw -= mouseXrel * deltaTime * 0.05f;
        cameraYaw -= rx * deltaTime * 3.5f;

        v3 worldUp = {0.0f, 1.0f, 0.0f};

        v3 cameraForward = {};
        cameraForward.x = cosf(cameraPitch) * cosf(cameraYaw);
        cameraForward.y = sinf(cameraPitch);
        cameraForward.z = cosf(cameraPitch) * sinf(cameraYaw);

        v3 cameraRight = v3::normalised(v3::cross(cameraForward, worldUp));
        v3 cameraUp = v3::cross(cameraRight, cameraForward);

        static float forwardSpeed = 0.0f;

        forwardSpeed = inputMotionYAxis * deltaTime * debugBoostSpeed;
        static float strafeSpeed = 0.0f;
        strafeSpeed = inputMotionXAxis * deltaTime * debugBoostSpeed;

        cameraPos = cameraPos + (cameraForward * forwardSpeed);
        cameraPos = cameraPos + (cameraRight * strafeSpeed);

        QueryPerformanceCounter(&profiling.t1);

        // main matrix update
        v3 atPos = cameraPos + cameraForward;
        // main view and projection matrices setup
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();

        // DirectX::XMVECTOR eye = DirectX::XMVectorSet(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        DirectX::XMVECTOR eye = DirectX::XMVectorSet(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        constantBufferData.cameraPos = eye;
        DirectX::XMVECTOR at = DirectX::XMVectorSet(atPos.x, atPos.y, atPos.z, 0.0f);
        DirectX::XMVECTOR up = DirectX::XMVectorSet(cameraUp.x, cameraUp.y, cameraUp.z, 0.0f);
        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, at, up);
        // view = DirectX::XMMatrixIdentity();

        float nearZ = 0.1f;
        float farZ = 9999999.0f; // TODO: change to infinite far plane???
        DirectX::XMMATRIX projection = DirectX::XMMatrixPerspectiveFovLH(fov, aspectRatio, nearZ, farZ);
        // DirectX::XMMATRIX projection = DirectX::XMMatrixOrthographicLH(2.0f, 2.0f, 0.1f, 100.0f);

        // projection = DirectX::XMMatrixIdentity();
        DirectX::XMStoreFloat4x4(&constantBufferData.world, world);
        DirectX::XMStoreFloat4x4(&constantBufferData.view, view);
        DirectX::XMStoreFloat4x4(&constantBufferData.projection, projection);

        memcpy(CbvDataBegin, &constantBufferData, sizeof(constantBufferData));

        if (enableImgui)
            ImGui::Render();
        // render here
        // populate command list
        hr = renderState.commandAllocators[frameIndex]->Reset();
        if (FAILED(hr))
        {
            errhr("Reset failed (command allocators)", hr);
            return 1;
        }
        hr = renderState.commandList->Reset(renderState.commandAllocators[frameIndex], terrainPSO.pipelineState);
        if (FAILED(hr))
        {
            errhr("Reset failed (command list)", hr);
            return 1;
        }

        renderState.commandList->SetGraphicsRootSignature(renderState.rootSignature);

        ID3D12DescriptorHeap *heaps[] = {renderState.srvHeap};
        renderState.commandList->SetDescriptorHeaps(_countof(heaps), heaps);

        // renderState.commandList->SetGraphicsRootDescriptorTable(0, renderState.srvHeap->GetGPUDescriptorHandleForHeapStart()); // CBV
        renderState.commandList->SetGraphicsRootConstantBufferView(
            0, // root parameter index
            renderState.constantBuffer->GetGPUVirtualAddress());

        // D3D12_GPU_DESCRIPTOR_HANDLE srvHandleGPU = renderState.srvHeap->GetGPUDescriptorHandleForHeapStart();
        // srvHandleGPU.ptr += renderState.cbvSrvDescriptorSize;
        // renderState.commandList->SetGraphicsRootDescriptorTable(1, srvHandleGPU);
        D3D12_GPU_DESCRIPTOR_HANDLE srvStart =
            renderState.srvHeap->GetGPUDescriptorHandleForHeapStart();
        renderState.commandList->SetGraphicsRootDescriptorTable(1, srvStart);

        renderState.commandList->RSSetViewports(1, &viewport);
        renderState.commandList->RSSetScissorRects(1, &scissorRect);

        CD3DX12_RESOURCE_BARRIER commandListResourceBarrierTransitionRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(renderState.renderTargets[frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderState.commandList->ResourceBarrier(1, &commandListResourceBarrierTransitionRenderTarget);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandlePerFrame(renderState.rtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)frameIndex, renderState.rtvDescriptorSize);
        renderState.commandList->OMSetRenderTargets(1, &rtvHandlePerFrame, FALSE, nullptr);

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(renderState.dsvHeap->GetCPUDescriptorHandleForHeapStart());
        renderState.commandList->OMSetRenderTargets(1, &rtvHandlePerFrame, FALSE, &dsvHandle);

        // commands
        const float clearColour[4] = {0.0f, 0.2f, 0.4f, 1.0f};
        renderState.commandList->ClearRenderTargetView(rtvHandlePerFrame, clearColour, 0, nullptr);
        renderState.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // non bundle rendering

        // sky render
        renderState.commandList->SetPipelineState(skyPSO.pipelineState);
        renderState.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderState.commandList->IASetVertexBuffers(0, 0, nullptr);
        renderState.commandList->IASetIndexBuffer(nullptr);
        renderState.commandList->DrawInstanced(3, 1, 0, 0);

        // terrain render
        renderState.commandList->SetPipelineState(terrainPSO.pipelineState);
        if (baked_heightmap_mesh.created)
            baked_heightmap_mesh.draw(cameraPos);

        renderState.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderState.commandList->IASetVertexBuffers(0, 1, &terrainGridVB.vertexBufferView);

        // renderState.commandList->SetGraphicsRootConstantBufferView(0, renderState.constantBuffer->GetGPUVirtualAddress());

        // TODO:
        //  draw max detail mesh here?

        // NOTE: start at 1 for clipmap rings only

        float h = cameraPos.y;
        float halfFov = fov * 0.5f;

        float bottomRayAngle = cameraPitch - halfFov;
        if (bottomRayAngle > -0.001f)
            bottomRayAngle = -0.001f;

        float L = h / tanf(-bottomRayAngle);

        float forwardDistance = (L > 0.0f) ? L : 0.0f;

        v3 cmFwd2D = cameraForward;
        cmFwd2D.y = 0;                     // for ground level
        cmFwd2D = v3::normalised(cmFwd2D); // TODO: note debug scaler here

        v3 clipmapCentreLocation = cameraPos + cmFwd2D * forwardDistance;
        // v3 clipmapCentreLocation = cameraPos + cmFwd2D * (terrainGridDimensionInWorldUnits / 2);

        for (int i = 0; i < activeClipmapRings; ++i)
        {
            float lodScale = (float)(1 << i);
            // constantBufferData.ringWorldSize = baseGridSize * lodScale;
            constantBufferData.ringSampleStep = lodScale;

            // Snap camera position to sample grid to avoid jitter
            float snappedX = floor(clipmapCentreLocation.x / lodScale) * lodScale;
            float snappedZ = floor(clipmapCentreLocation.z / lodScale) * lodScale;

            constantBufferData.ringOffset.x = snappedX;
            constantBufferData.ringOffset.y = snappedZ;

            // update constant buffer

            DirectX::XMStoreFloat4x4(&constantBufferData.world, DirectX::XMMatrixIdentity());

            UINT cbOffset = i * constantBufferSize;

            memcpy(reinterpret_cast<byte *>(CbvDataBegin) + cbOffset, &constantBufferData, sizeof(constantBufferData));
            renderState.commandList->SetGraphicsRootConstantBufferView(0, renderState.constantBuffer->GetGPUVirtualAddress() + cbOffset);

            UINT localIndexCount = clipmapRing.indexCount;
            if (i == 0)
            {
                renderState.commandList->IASetIndexBuffer(&terrainGridCentreIB.indexBufferView);
                localIndexCount = clipmapCentre.indexCount;
            }
            else
            {
                renderState.commandList->IASetIndexBuffer(&terrainGridRingIB.indexBufferView);
                localIndexCount = clipmapRing.indexCount;
            }
            renderState.commandList->DrawIndexedInstanced(localIndexCount, 1, 0, 0, 0);
        }

        // bundle rendering
        // renderState.commandList->ExecuteBundle(renderState.bundle);

        if (enableImgui)
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), renderState.commandList);

        // CD3DX12_RESOURCE_BARRIER commandListResourceBarrierTransitionPixelShader = CD3DX12_RESOURCE_BARRIER::Transition(renderState.renderTargets[frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        renderState.commandList->ResourceBarrier(1, &commandListResourceBarrierTransitionRenderTarget);
        hr = renderState.commandList->Close();
        if (FAILED(hr))
        {
            errhr("Failed to close command list (frame rendering)", hr);
            return 1;
        }
        // end of populating command list

        // execute command list
        ID3D12CommandList *commandListsPerFrame[] = {renderState.commandList};
        renderState.commandQueue->ExecuteCommandLists(_countof(commandListsPerFrame), commandListsPerFrame);

        QueryPerformanceCounter(&profiling.t2);

        hr = renderState.swapChain->Present((vsync) ? 1 : 0, (vsync) ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        if (FAILED(hr))
        {
            errhr("Present failed", hr);
            hr = renderState.device->GetDeviceRemovedReason();
            errhr("GetDeviceRemovedReason: ", hr);
            return 1;
        }

        // move to next frame (framebuffering)
        const UINT64 currentFenceValue = fenceValues[frameIndex];
        hr = renderState.commandQueue->Signal(renderState.fence, currentFenceValue);
        if (FAILED(hr))
        {
            errhr("Signal failed", hr);
            return 1;
        }
        frameIndex = renderState.swapChain->GetCurrentBackBufferIndex();

        if (renderState.fence->GetCompletedValue() < fenceValues[frameIndex])
        {
            hr = renderState.fence->SetEventOnCompletion(fenceValues[frameIndex], fenceEvent);
            if (FAILED(hr))
            {
                errhr("SetEventOnCompletion failed", hr);
                return 1;
            }
            WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
        }
        fenceValues[frameIndex] = currentFenceValue + 1;
        // end of moving to next frame

        programState.timeElapsed += deltaTime;
        programState.ticksElapsed++;

        constantBufferData.timeElapsed = programState.timeElapsed;

        QueryPerformanceCounter(&profiling.t3);
    }
    // // SDL_SetWindowBordered(programState.window, true);
    // SDL_SyncWindow(programState.window);
    // SDL_DestroyWindow(programState.window);
    // SDL_Quit();

    // // cleanup
    // if (imguiInitialised)
    // {
    //     ImGui_ImplDX12_Shutdown();
    //     ImGui_ImplSDL3_Shutdown();
    //     ImGui::DestroyContext();
    // }

    // if (fenceEvent)
    //     CloseHandle(fenceEvent);
    // if (renderState.fence)
    //     renderState.fence->Release();
    // if (renderState.texture)
    //     renderState.texture->Release();
    // if (textureUploadHeap)
    //     textureUploadHeap->Release();
    // if (renderState.vertexBuffer)
    //     renderState.vertexBuffer->Release();
    // if (renderState.constantBuffer)
    //     renderState.constantBuffer->Release();
    // if (renderState.bundle)
    //     renderState.bundle->Release();
    // if (renderState.commandList)
    //     renderState.commandList->Release();
    // if (terrainPSO.pipelineState)
    //     terrainPSO.pipelineState->Release();
    // if (renderState.rootSignature)
    //     renderState.rootSignature->Release();
    // if (renderState.srvHeap)
    //     renderState.srvHeap->Release();
    // if (renderState.rtvHeap)
    //     renderState.rtvHeap->Release();
    // for (UINT i = 0; i < renderState.frameCount; ++i)
    // {
    //     if (renderState.renderTargets[i])
    //         renderState.renderTargets[i]->Release();
    //     if (renderState.commandAllocators[i])
    //         renderState.commandAllocators[i]->Release();
    // }
    // if (renderState.swapChain)
    //     renderState.swapChain->Release();
    // if (renderState.commandQueue)
    //     renderState.commandQueue->Release();
    // if (renderState.bundleAllocator)
    //     renderState.bundleAllocator->Release();
    // if (renderState.device)
    //     renderState.device->Release();
    // if (renderState.hardwareAdapter)
    //     renderState.hardwareAdapter->Release();
    // if (renderState.factory)
    //     renderState.factory->Release();
    // if (terrainShaderPair.vertexShader)
    //     terrainShaderPair.vertexShader->Release();
    // if (terrainShaderPair.pixelShader)
    //     terrainShaderPair.pixelShader->Release();
    // if (signature)
    //     signature->Release();
    return (0);
}