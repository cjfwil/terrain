#pragma warning(disable : 5045) // disabling the spectre mitigation warning (not relevant because we are a game, no sensitive information should be in this program)
#pragma comment(lib, "SDL3.lib")
#pragma comment(lib, "SDL3_image.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
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

// float x and float y are in world space
float sampleHeightmap(float *heightmap, int dim, float _worldSpacePosX, float _worldSpacePosY, float quadSize, float terrainDimQuads)
{
    // convert to [0, 1]
    float x = _worldSpacePosX / (float)dim;
    float y = _worldSpacePosY / (float)dim;

    x = (float)SDL_clamp((float)x, 0.0, 1.0f);
    y = (float)SDL_clamp((float)y, 0.0, 1.0f);

    x *= (float)terrainDimQuads;
    y *= (float)terrainDimQuads;

    int x0 = (int)(x / quadSize);
    int y0 = (int)(y / quadSize);
    int x1 = (x0 + 1 < dim) ? x0 + 1 : x0;
    int y1 = (y0 + 1 < dim) ? y0 + 1 : y0;

    float tx = x - (float)x0;
    float ty = y - (float)y0;

    // Fetch 4 neighbors
    float h00 = heightmap[y0 * dim + x0];
    float h10 = heightmap[y0 * dim + x1];
    float h01 = heightmap[y1 * dim + x0];
    float h11 = heightmap[y1 * dim + x1];

    // Bilinear interpolation
    float hx0 = h00 + (h10 - h00) * tx;
    float hx1 = h01 + (h11 - h01) * ty;

    return hx0 + (hx1 - hx0) * ty;
}

static struct
{
    SDL_Window *window;
    Uint64 msElapsedSinceSDLInit;
    uint64_t ticksElapsed = 0;
    bool fullscreen;
    bool isRunning;
} programState;

static struct
{
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 view; // 4 x 4 matrix has 16 entries. 4 bytes per entry -> 64 bytes total
    DirectX::XMFLOAT4X4 projection;
    DirectX::XMVECTOR cameraPos;
    float planetScaleRatio = 1.0f / 75.0f;
} constantBufferData;

int main(void)
{
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

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 2; // CBV + SRV
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

    UINT cbvSrvDescriptorSize = renderState.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT rtvDescriptorSize = renderState.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

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
        rtvHandleSetup.Offset(1, rtvDescriptorSize);

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

    CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
    CD3DX12_ROOT_PARAMETER1 rootParameters[2];
    rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_VERTEX); // crv
    rootParameters[1].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_PIXEL);  // srv

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 16;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

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
#if defined(_DEBUG)
    // Enable better shader debugging with the graphics debugging tools.
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = 0;
#endif

    hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &renderState.vertexShader, nullptr);
    if (FAILED(hr))
    {
        errhr("D3DCompile from file failed (vertex shader)", hr);
        return 1;
    }
    hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &renderState.pixelShader, nullptr);
    if (FAILED(hr))
    {
        errhr("D3DCompile from file failed (pixel shader)", hr);
        return 1;
    }

    D3D12_INPUT_ELEMENT_DESC inputElementDesc[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // rasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
    // rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // Disable culling (backface culling)

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputElementDesc, _countof(inputElementDesc)};
    psoDesc.pRootSignature = renderState.rootSignature;
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(renderState.vertexShader);
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(renderState.pixelShader);
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = renderState.rtvFormat;
    psoDesc.SampleDesc.Count = 1;

    hr = renderState.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&renderState.pipelineState));
    if (FAILED(hr))
    {
        errhr("CreateGraphicsPipelineState failed", hr);
        return 1;
    }

    hr = renderState.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, renderState.commandAllocators[frameIndex], nullptr, IID_PPV_ARGS(&renderState.commandList));
    if (FAILED(hr))
    {
        errhr("CreateCommandList failed", hr);
        return 1;
    }

    const float aspectRatio = (float)width / (float)height;

    int bakedResult = baked_heightmap_mesh.baked();
    if (bakedResult != 0)
    {
        err("Baked Mesh failed");
        return 1;
    }

    d3d12_index_buffer terrainMeshIndexBuffer = {};
    if (!terrainMeshIndexBuffer.create_and_upload(baked_heightmap_mesh.terrainMeshIndexBufferSize, baked_heightmap_mesh.terrainMeshIndexBuffer))
    {
        err("Terrain Index Buffer Create and upload failed");
        return 1;
    }

    // create constant buffer
    const UINT constantBufferSize = 256U;
    static UINT *CbvDataBegin = nullptr;

    CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);
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

    d3d12_vertex_buffer terrainMeshVertexBuffer;
    if (!terrainMeshVertexBuffer.create_and_upload(baked_heightmap_mesh.terrainPointsSize, baked_heightmap_mesh.terrainPoints))
    {
        err("Terrain Mesh Vertex Buffer create and upload failed");
        return 1;
    }

    // CREATE BUNDLE
    hr = renderState.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, renderState.bundleAllocator, renderState.pipelineState, IID_PPV_ARGS(&renderState.bundle));
    if (FAILED(hr))
    {
        errhr("CreateCommandList failed", hr);
        return 1;
    }

    renderState.bundle->SetGraphicsRootSignature(renderState.rootSignature);
    renderState.bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderState.bundle->IASetVertexBuffers(0, 1, &terrainMeshVertexBuffer.vertexBufferView);
    renderState.bundle->IASetIndexBuffer(&terrainMeshIndexBuffer.indexBufferView);
    renderState.bundle->DrawIndexedInstanced(baked_heightmap_mesh.terrainMeshIndexBufferNum, 1, 0, 0, 0);

    // renderState.bundle->DrawInstanced(terrainMeshSizeInVertices, 1, 0, 0);
    renderState.bundle->Close();

    // beginning of texture
    // Load BC7 DDS (with baked mipmaps) using DirectXTex

    DirectX::ScratchImage image;
    DirectX::TexMetadata metadata;

    hr = DirectX::LoadFromDDSFile(
        L"gravel.dds",
        DirectX::DDS_FLAGS_NONE,
        &metadata,
        image);
    if (FAILED(hr))
    {
        errhr("LoadFromDDSFile failed", hr);
        return 1;
    }

    // Create GPU texture resource

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Alignment = 0;
    textureDesc.Width = (UINT)metadata.width;
    textureDesc.Height = (UINT)metadata.height;
    textureDesc.DepthOrArraySize = (UINT16)metadata.arraySize;
    textureDesc.MipLevels = (UINT16)metadata.mipLevels;
    textureDesc.Format = metadata.format; // DXGI_FORMAT_BC7_UNORM or BC7_UNORM_SRGB
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    CD3DX12_HEAP_PROPERTIES heapPropsDefault(D3D12_HEAP_TYPE_DEFAULT);

    hr = renderState.device->CreateCommittedResource(
        &heapPropsDefault,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&renderState.texture));
    if (FAILED(hr))
    {
        errhr("CreateCommittedResource (texture)", hr);
        return 1;
    }

    // Prepare subresources (DirectXTex gives correct BC7 pitches)

    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    subresources.reserve(image.GetImageCount());

    const DirectX::Image *imgs = image.GetImages();
    for (size_t i = 0; i < image.GetImageCount(); ++i)
    {
        D3D12_SUBRESOURCE_DATA s = {};
        s.pData = imgs[i].pixels;
        s.RowPitch = (LONG_PTR)imgs[i].rowPitch;
        s.SlicePitch = (LONG_PTR)imgs[i].slicePitch;
        subresources.push_back(s);
    }

    // Create upload heap

    UINT64 uploadBufferSize =
        GetRequiredIntermediateSize(renderState.texture, 0, (UINT)subresources.size());

    // CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

    ID3D12Resource *textureUploadHeap = nullptr;
    hr = renderState.device->CreateCommittedResource(
        &heapPropsUpload,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&textureUploadHeap));
    if (FAILED(hr))
    {
        errhr("CreateCommittedResource (upload buffer)", hr);
        return 1;
    }

    // Upload all mip levels

    UpdateSubresources(
        renderState.commandList,
        renderState.texture,
        textureUploadHeap,
        0, 0,
        (UINT)subresources.size(),
        subresources.data());

    // Transition to shader resource
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        renderState.texture,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderState.commandList->ResourceBarrier(1, &barrier);

    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = metadata.format; // must match BC7 format
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = (UINT)metadata.mipLevels;

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU =
        renderState.srvHeap->GetCPUDescriptorHandleForHeapStart();
    srvHandleCPU.ptr += cbvSrvDescriptorSize;

    renderState.device->CreateShaderResourceView(
        renderState.texture,
        &srvDesc,
        srvHandleCPU);

    CD3DX12_RESOURCE_BARRIER commandListResourceBarrierTransitionPixelShader = CD3DX12_RESOURCE_BARRIER::Transition(renderState.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderState.commandList->ResourceBarrier(1, &commandListResourceBarrierTransitionPixelShader);

    renderState.device->CreateShaderResourceView(renderState.texture, &srvDesc, srvHandleCPU);
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

        static int newBaseDist = 141;
        static int drawDist[BakedConstants::maxLod] = {150, 300, 600, 1200, 2400, 4800};
        static bool renderBeyondMaxRange = false;

        // 0.091f is a nice value for 1080p with good performance on a 2k heightmap, but more for flying up into the atmosphere, doesnt have an effect when on top of mountains
        static float heightbasedLODModScaler = 0.091f; // dont put this to zero or lower TODO: calculate appropriate min and max
        static bool enableHeightLODMod = false;

        if (enableImgui)
        {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            static bool show_demo_window = false;
            if (show_demo_window)
                ImGui::ShowDemoWindow(&show_demo_window);
            ImGui::Text("Application average %.3f ms/frame (%.2f FPS)",
                        1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);

            // basic profiling
            
            ImGui::SliderInt("LodDist", &newBaseDist, BakedConstants::chunkDimVerts, 512);
            static int planetScaleRatioDenom = 50;
            ImGui::SliderInt("Planet Scale 1:X", &planetScaleRatioDenom, 1, 100);
            constantBufferData.planetScaleRatio = 1.0f / (float)planetScaleRatioDenom;

            ImGui::SliderFloat("Debug Speed Boost", &debugBoostSpeed, 1.0f, 1000.0f, "%.3f", ImGuiSliderFlags_Logarithmic);

            for (int i = 0; i < BakedConstants::maxLod; ++i)
            {
                ImGui::Text("LOD%d: %d", i, drawDist[i]);
            }

            uint64_t frameHistoryIndex = programState.ticksElapsed % 256;
            profiling.frameRateHistory[frameHistoryIndex] = ImGui::GetIO().Framerate;
            ImGui::PlotLines("Frametime", profiling.frameRateHistory, IM_ARRAYSIZE(profiling.frameRateHistory), (int)frameHistoryIndex);

            // options
            ImGui::Checkbox("VSync", &vsync);
            ImGui::Checkbox("Render beyond Max range", &renderBeyondMaxRange);
            ImGui::Checkbox("Boost terrain detail when camera is higher", &enableHeightLODMod);
            if (enableHeightLODMod)
            {
                ImGui::SliderFloat("Scaler", &heightbasedLODModScaler, 0.01f, 0.1f, "%.3f", ImGuiSliderFlags_Logarithmic);
            }

            // FPS values
            profiling.update_fps();
            ImGui::Text("1%% Low:  %.2f FPS", profiling.fps_1pct_low);
            ImGui::Text("0.1%% Low:%.2f FPS", profiling.fps_01pct_low);
            ImGui::Text("Peak:    %.2f FPS", profiling.fps_peak);
            ImGui::Text("Min:     %.2f FPS", profiling.fps_min);
            ImGui::Text("Max:     %.2f FPS", profiling.fps_max);

            ImGui::Text("Update:  %.3f ms", profiling.update_ms);
            ImGui::Text("Render:  %.3f ms", profiling.render_ms);
            ImGui::Text("Present: %.3f ms", profiling.present_ms);
            ImGui::Text("Frame:   %.3f ms", profiling.frame_ms);

            ImGui::Text("Vertices:%d", baked_heightmap_mesh.terrainPointsNum);
            ImGui::Text("Indices:%d", baked_heightmap_mesh.terrainMeshIndexBufferNum);

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

        static float cameraYaw = PI * 2.26f / 3.0f;
        static float cameraPitch = 0.0f;
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

        static v3 cameraPos = {(float)baked_heightmap_mesh.terrainDimInQuads, 100.0f, 0.0f};
        cameraPos = cameraPos + (cameraForward * forwardSpeed);
        cameraPos = cameraPos + (cameraRight * strafeSpeed);

        QueryPerformanceCounter(&profiling.t1);

        // main matrix update
        v3 atPos = cameraPos + cameraForward;
        // main view and projection matrices setup
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();

        DirectX::XMVECTOR eye = DirectX::XMVectorSet(cameraPos.x, cameraPos.y, cameraPos.z, 0.0f);
        constantBufferData.cameraPos = eye;
        DirectX::XMVECTOR at = DirectX::XMVectorSet(atPos.x, atPos.y, atPos.z, 0.0f);
        DirectX::XMVECTOR up = DirectX::XMVectorSet(cameraUp.x, cameraUp.y, cameraUp.z, 0.0f);
        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, at, up);
        // view = DirectX::XMMatrixIdentity();

        float fov = DirectX::XMConvertToRadians(60.0f);
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
        hr = renderState.commandList->Reset(renderState.commandAllocators[frameIndex], renderState.pipelineState);
        if (FAILED(hr))
        {
            errhr("Reset failed (command list)", hr);
            return 1;
        }

        renderState.commandList->SetGraphicsRootSignature(renderState.rootSignature);

        ID3D12DescriptorHeap *heaps[] = {renderState.srvHeap};
        renderState.commandList->SetDescriptorHeaps(_countof(heaps), heaps);

        renderState.commandList->SetGraphicsRootDescriptorTable(0, renderState.srvHeap->GetGPUDescriptorHandleForHeapStart()); // CBV

        D3D12_GPU_DESCRIPTOR_HANDLE srvHandleGPU = renderState.srvHeap->GetGPUDescriptorHandleForHeapStart();
        srvHandleGPU.ptr += cbvSrvDescriptorSize;
        renderState.commandList->SetGraphicsRootDescriptorTable(1, srvHandleGPU);
        renderState.commandList->RSSetViewports(1, &viewport);
        renderState.commandList->RSSetScissorRects(1, &scissorRect);

        CD3DX12_RESOURCE_BARRIER commandListResourceBarrierTransitionRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(renderState.renderTargets[frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderState.commandList->ResourceBarrier(1, &commandListResourceBarrierTransitionRenderTarget);

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandlePerFrame(renderState.rtvHeap->GetCPUDescriptorHandleForHeapStart(), (INT)frameIndex, rtvDescriptorSize);
        renderState.commandList->OMSetRenderTargets(1, &rtvHandlePerFrame, FALSE, nullptr);

        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(renderState.dsvHeap->GetCPUDescriptorHandleForHeapStart());
        renderState.commandList->OMSetRenderTargets(1, &rtvHandlePerFrame, FALSE, &dsvHandle);

        // commands
        const float clearColour[4] = {0.0f, 0.2f, 0.4f, 1.0f};
        renderState.commandList->ClearRenderTargetView(rtvHandlePerFrame, clearColour, 0, nullptr);
        renderState.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // non bundle rendering
        renderState.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderState.commandList->IASetVertexBuffers(0, 1, &terrainMeshVertexBuffer.vertexBufferView);
        renderState.commandList->IASetIndexBuffer(&terrainMeshIndexBuffer.indexBufferView);
        // renderState.commandList->DrawIndexedInstanced(baked_heightmap_mesh.terrainMeshIndexBufferNum, 1, 0, 0, 0);

        // debugDrawOnlyChunk = programState.ticksElapsed % (chunkNumTotal);

        // for live tweaking of LOD distance
        int baseDist = 0;
        if (newBaseDist != baseDist || 1)
        {
            baseDist = newBaseDist;
            // extra lod when camera is high. TODO: should we even be doing this?
            // TODO: figure this out because this doesnt feel right
            float heightbasedLODMod = 1.0f;
            if (enableHeightLODMod && cameraPos.y > 0)
            {
                heightbasedLODMod = SDL_clamp(heightbasedLODModScaler * sqrtf(cameraPos.y), 1.0f, 8.0f);
            }
            for (int lod = 0; lod < BakedConstants::maxLod; ++lod)
            {
                drawDist[lod] = baseDist * (1 << lod) * heightbasedLODMod;
            }
        }

        for (UINT i = 0; i < baked_heightmap_mesh.chunkNumTotal; ++i)
        {

            UINT cx = (i % baked_heightmap_mesh.chunkNumDim) * baked_heightmap_mesh.chunkDimQuads;
            UINT cy = (i / baked_heightmap_mesh.chunkNumDim) * baked_heightmap_mesh.chunkDimQuads;
            v3 pointEye = cameraPos;
            int distCx = (pointEye.x - (int)cx);
            int distCy = (pointEye.y - 0);
            int distCz = (pointEye.z - (int)cy);
            int squaredDist = distCx * distCx + distCy * distCy + distCz * distCz;

            int desiredLod = (renderBeyondMaxRange) ? BakedConstants::maxLod - 1 : -1; // -1 == cull

            // TODO: somehow figure out how to cull when a chunk is well below the horizon
            // mountains sticking up above horizon
            for (int j = 0; j < BakedConstants::maxLod; ++j)
            {
                if (squaredDist < drawDist[j] * drawDist[j])
                {
                    desiredLod = j;
                    break;
                }
            }
            if (desiredLod >= 0)
            {
                UINT currentStartingIndex = baked_heightmap_mesh.lodRanges[i].startIndex[desiredLod] * 6U;
                UINT numIndicesToDraw = baked_heightmap_mesh.lodRanges[i].numIndices[desiredLod] * 6U;
                renderState.commandList->DrawIndexedInstanced(numIndicesToDraw, 1, currentStartingIndex, 0, 0);
            }
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

        programState.ticksElapsed++;

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
    // if (renderState.pipelineState)
    //     renderState.pipelineState->Release();
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
    // if (renderState.vertexShader)
    //     renderState.vertexShader->Release();
    // if (renderState.pixelShader)
    //     renderState.pixelShader->Release();
    // if (signature)
    //     signature->Release();
    return (0);
}