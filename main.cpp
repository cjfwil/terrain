#pragma warning(disable : 5045) // disabling the spectre mitigation warning (not relevant because we are a game, no sensitive information should be in this program)
#pragma comment(lib, "SDL3.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "imgui.lib")
#pragma comment(lib, "DirectXTex.lib")
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
#pragma warning(pop)

#include "src/metadata.h"
#include "src/error.h"

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

// Simple free list based allocator
struct DescriptorHeapAllocator
{
    ID3D12DescriptorHeap *Heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
    UINT HeapHandleIncrement;
    ImVector<int> FreeIndices;

    void Create(ID3D12Device *device, ID3D12DescriptorHeap *heap)
    {
        IM_ASSERT(Heap == nullptr && FreeIndices.empty());
        Heap = heap;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        HeapType = desc.Type;
        HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
        HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
        HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
        FreeIndices.reserve((int)desc.NumDescriptors);
        for (int n = desc.NumDescriptors; n > 0; n--)
            FreeIndices.push_back(n - 1);
    }
    void Destroy()
    {
        Heap = nullptr;
        FreeIndices.clear();
    }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu_desc_handle)
    {
        IM_ASSERT(FreeIndices.Size > 0);
        int idx = FreeIndices.back();
        FreeIndices.pop_back();
        out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
        out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
    {
        int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
        int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
        IM_ASSERT(cpu_idx == gpu_idx);
        FreeIndices.push_back(cpu_idx);
    }
};

static struct
{
    SDL_Window *window;
    Uint64 msElapsedSinceSDLInit;
    uint64_t ticksElapsed = 0;
    bool isRunning;
} programState;

static struct
{
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 view; // 4 x 4 matrix has 16 entries. 4 bytes per entry -> 64 bytes total
    DirectX::XMFLOAT4X4 projection;
} constantBufferData;

static struct
{
    ID3D12DescriptorHeap *imguiSrvHeap = nullptr;
    DescriptorHeapAllocator imguiSrvAllocator;
    // init
    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter4 *hardwareAdapter = nullptr;
    ID3D12Device *device = nullptr;
    IDXGISwapChain1 *swapChain1 = nullptr; // only for initialisation
    IDXGISwapChain4 *swapChain = nullptr;
    ID3D12CommandQueue *commandQueue = nullptr;
    ID3D12DescriptorHeap *rtvHeap = nullptr;
    ID3D12DescriptorHeap *srvHeap = nullptr;
    static const int frameCount = 3;
    ID3D12CommandAllocator *commandAllocators[frameCount] = {};
    ID3D12Resource *renderTargets[frameCount] = {};
    ID3D12CommandAllocator *bundleAllocator = nullptr;
    ID3D12RootSignature *rootSignature = nullptr;
    ID3D12Fence *fence = nullptr;
    ID3D12PipelineState *pipelineState = nullptr;
    ID3D12GraphicsCommandList *commandList = nullptr;
    ID3DBlob *vertexShader = nullptr;
    ID3DBlob *pixelShader = nullptr;
    ID3D12Resource *texture = nullptr;
    ID3D12Resource *constantBuffer = nullptr;
    ID3D12Resource *vertexBuffer = nullptr;
    ID3D12GraphicsCommandList *bundle = nullptr;
    const DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // end of init
} renderState;

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

    programState.window = SDL_CreateWindow(APP_WINDOW_TITLE, (int)width, (int)height, SDL_WINDOW_BORDERLESS);
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
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 0;
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
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_RASTERIZER_DESC rasterizerDesc = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // Disable culling

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputElementDesc, _countof(inputElementDesc)};
    psoDesc.pRootSignature = renderState.rootSignature;
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(renderState.vertexShader);
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(renderState.pixelShader);
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
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

    // // FROM MICROSOFT:
    // // Command lists are created in the recording state, but there is nothing
    // // to record yet. The main loop expects it to be closed, so close it now.
    // hr = commandList->Close();
    // if (FAILED(hr))
    // {
    //     errhr("Failed to Close command list", hr);
    //     return 1;
    // }

    // vertex
    struct vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT2 texCoords;
    };

    const float aspectRatio = (float)width / (float)height;
    vertex triangleVertices[] = {
        {{0.0f, 0.9f, 0.0f}, {0.5f, 0.0f}},
        {{0.9f, -0.9f, 0.0f}, {1.0f, 1.0f}},
        {{-0.9f, -0.9f, 0.0f}, {0.0f, 1.0f}}};

    const UINT vertexBufferSize = sizeof(triangleVertices);

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

    // FROM MICROSOFT:
    // Note: using upload heaps to transfer static data like vert buffers is not
    // recommended. Every time the GPU needs it, the upload heap will be marshalled
    // over. Please read up on Default Heap usage. An upload heap is used here for
    // code simplicity and because there are very few verts to actually transfer.
    CD3DX12_RESOURCE_DESC vertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
    hr = renderState.device->CreateCommittedResource(
        &heapPropsUpload,
        D3D12_HEAP_FLAG_NONE,
        &vertexBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&renderState.vertexBuffer));
    UINT *vertexDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    hr = renderState.vertexBuffer->Map(0, &readRange, (void **)&vertexDataBegin);
    if (FAILED(hr))
    {
        errhr("Map failed (vertex buffer)", hr);
        return 1;
    }
    memcpy(vertexDataBegin, triangleVertices, sizeof(triangleVertices));
    renderState.vertexBuffer->Unmap(0, nullptr);

    D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
    vertexBufferView.BufferLocation = renderState.vertexBuffer->GetGPUVirtualAddress();
    vertexBufferView.StrideInBytes = sizeof(vertex);
    vertexBufferView.SizeInBytes = vertexBufferSize;

    // CREATE BUNDLE
    hr = renderState.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, renderState.bundleAllocator, renderState.pipelineState, IID_PPV_ARGS(&renderState.bundle));
    if (FAILED(hr))
    {
        errhr("CreateCommandList failed", hr);
        return 1;
    }
    renderState.bundle->SetGraphicsRootSignature(renderState.rootSignature);
    renderState.bundle->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    renderState.bundle->IASetVertexBuffers(0, 1, &vertexBufferView);
    renderState.bundle->DrawInstanced(3, 1, 0, 0);
    renderState.bundle->Close();

    // ------------------------------------------------------------
    // Load BC7 DDS (with baked mipmaps) using DirectXTex
    // ------------------------------------------------------------
    DirectX::ScratchImage image;
    DirectX::TexMetadata metadata;

    hr = DirectX::LoadFromDDSFile(
        L"ground_texture_0.dds",
        DirectX::DDS_FLAGS_NONE,
        &metadata,
        image);
    if (FAILED(hr))
    {
        errhr("LoadFromDDSFile failed", hr);
        return 1;
    }

    // ------------------------------------------------------------
    // Create GPU texture resource
    // ------------------------------------------------------------
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

    // ------------------------------------------------------------
    // Prepare subresources (DirectXTex gives correct BC7 pitches)
    // ------------------------------------------------------------
    std::vector<D3D12_SUBRESOURCE_DATA> subresources;
    subresources.reserve(image.GetImageCount());

    const DirectX::Image *imgs = image.GetImages();
    for (size_t i = 0; i < image.GetImageCount(); ++i)
    {
        D3D12_SUBRESOURCE_DATA s = {};
        s.pData = imgs[i].pixels;
        s.RowPitch = imgs[i].rowPitch;
        s.SlicePitch = imgs[i].slicePitch;
        subresources.push_back(s);
    }

    // ------------------------------------------------------------
    // Create upload heap
    // ------------------------------------------------------------
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

    // ------------------------------------------------------------
    // Upload all mip levels
    // ------------------------------------------------------------
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

    // ------------------------------------------------------------
    // Create SRV
    // ------------------------------------------------------------
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

    // // generate texture
    // const int texWidth = 16;
    // const int texHeight = 16;
    // unsigned int textureData[texWidth * texHeight] = {};
    // for (int i = 0; i < texWidth * texHeight; ++i)
    // {
    //     int x = i % texWidth;
    //     int y = i / texHeight;
    //     if ((x + y) % 2 == 0)
    //         textureData[i] = 0xff444411;
    //     else
    //         textureData[i] = 0xff44cc11;
    // }

    // ID3D12Resource *textureUploadHeap = nullptr;
    // D3D12_RESOURCE_DESC textureDesc = {};
    // textureDesc.MipLevels = 1;
    // textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    // textureDesc.Width = texWidth;
    // textureDesc.Height = texHeight;
    // textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    // textureDesc.DepthOrArraySize = 1;
    // textureDesc.SampleDesc.Count = 1;
    // textureDesc.SampleDesc.Quality = 0;
    // textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    // CD3DX12_HEAP_PROPERTIES heapPropsDefault(D3D12_HEAP_TYPE_DEFAULT);
    // hr = renderState.device->CreateCommittedResource(
    //     &heapPropsDefault,
    //     D3D12_HEAP_FLAG_NONE,
    //     &textureDesc,
    //     D3D12_RESOURCE_STATE_COPY_DEST,
    //     nullptr,
    //     IID_PPV_ARGS(&renderState.texture));
    // if (FAILED(hr))
    // {
    //     errhr("CreateCommittedResource (texture)", hr);
    //     return 1;
    // }
    // const UINT64 uploadBufferSize = GetRequiredIntermediateSize(renderState.texture, 0, 1);

    // // gpu upload buffer
    // CD3DX12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    // hr = renderState.device->CreateCommittedResource(
    //     &heapPropsUpload,
    //     D3D12_HEAP_FLAG_NONE,
    //     &uploadBufferDesc,
    //     D3D12_RESOURCE_STATE_GENERIC_READ,
    //     nullptr,
    //     IID_PPV_ARGS(&textureUploadHeap));
    // if (FAILED(hr))
    // {
    //     errhr("CreateCommittedResource failed (gpu upload buffer)", hr);
    //     return 1;
    // }

    // D3D12_SUBRESOURCE_DATA textureDataDesc = {};
    // textureDataDesc.pData = textureData;
    // textureDataDesc.RowPitch = texWidth * sizeof(textureData[0]);
    // textureDataDesc.SlicePitch = textureDataDesc.RowPitch * texHeight;

    // UpdateSubresources(renderState.commandList, renderState.texture, textureUploadHeap, 0, 0, 1, &textureDataDesc);

    CD3DX12_RESOURCE_BARRIER commandListResourceBarrierTransitionPixelShader = CD3DX12_RESOURCE_BARRIER::Transition(renderState.texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderState.commandList->ResourceBarrier(1, &commandListResourceBarrierTransitionPixelShader);

    // SRV for texture

    // D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    // srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    // srvDesc.Format = textureDesc.Format;
    // srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    // srvDesc.Texture2D.MipLevels = 1;
    // D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU = renderState.srvHeap->GetCPUDescriptorHandleForHeapStart();
    // srvHandleCPU.ptr += cbvSrvDescriptorSize;
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

    uint64_t lastCounter = SDL_GetPerformanceCounter();
    float deltaTime = 0.0f;
    // main program
    programState.isRunning = true;
    while (programState.isRunning)
    {
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent))
        {
            if (enableImgui)
                ImGui_ImplSDL3_ProcessEvent(&sdlEvent);
            switch (sdlEvent.type)
            {
            case SDL_EVENT_QUIT:
                programState.isRunning = false;
                break;
            }
        }
        programState.msElapsedSinceSDLInit = SDL_GetTicks();

        if (enableImgui)
        {
            ImGui_ImplDX12_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            static bool show_demo_window = false;
            if (show_demo_window)
                ImGui::ShowDemoWindow(&show_demo_window);
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                        1000.0f / ImGui::GetIO().Framerate,
                        ImGui::GetIO().Framerate);

            // basic profiling
            static float frameRateHistory[256] = {};
            uint64_t frameHistoryIndex = programState.ticksElapsed % 256;
            frameRateHistory[frameHistoryIndex] = ImGui::GetIO().Framerate;
            ImGui::PlotLines("Frametime", frameRateHistory, IM_ARRAYSIZE(frameRateHistory), (int)frameHistoryIndex);
            ImGui::Checkbox("VSync", &vsync);
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

        // main matrix update
        // main view and projection matrices setup
        DirectX::XMMATRIX world = DirectX::XMMatrixIdentity();

        float radius = 4.0f;
        float angle = movingPoint * DirectX::XM_2PI;
        DirectX::XMVECTOR eye = DirectX::XMVectorSet(radius * cosf(angle), 0.0f, radius * sinf(angle), 0.0f);
        DirectX::XMVECTOR at = DirectX::XMVectorZero();
        DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, at, up);
        // view = DirectX::XMMatrixIdentity();

        float fov = DirectX::XMConvertToRadians(60.0f);
        float nearZ = 0.1f;
        float farZ = 100.0f;
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

        // commands
        const float clearColour[4] = {0.0f, 0.2f, 0.4f, 1.0f};
        renderState.commandList->ClearRenderTargetView(rtvHandlePerFrame, clearColour, 0, nullptr);

        // non bundle rendering
        renderState.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        renderState.commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        renderState.commandList->DrawInstanced(3, 1, 0, 0);

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

        hr = renderState.swapChain->Present((vsync) ? 1 : 0, (vsync) ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        if (FAILED(hr))
        {
            errhr("Present failed", hr);
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

        // ImGui_ImplDX12_NewFrame();
        // ImGui_ImplSDL3_NewFrame();

        programState.ticksElapsed++;
    }
    SDL_DestroyWindow(programState.window);
    SDL_Quit();

    // cleanup
    if (imguiInitialised)
    {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }

    if (fenceEvent)
        CloseHandle(fenceEvent);
    if (renderState.fence)
        renderState.fence->Release();
    if (renderState.texture)
        renderState.texture->Release();
    if (textureUploadHeap)
        textureUploadHeap->Release();
    if (renderState.vertexBuffer)
        renderState.vertexBuffer->Release();
    if (renderState.constantBuffer)
        renderState.constantBuffer->Release();
    if (renderState.bundle)
        renderState.bundle->Release();
    if (renderState.commandList)
        renderState.commandList->Release();
    if (renderState.pipelineState)
        renderState.pipelineState->Release();
    if (renderState.rootSignature)
        renderState.rootSignature->Release();
    if (renderState.srvHeap)
        renderState.srvHeap->Release();
    if (renderState.rtvHeap)
        renderState.rtvHeap->Release();
    for (UINT i = 0; i < renderState.frameCount; ++i)
    {
        if (renderState.renderTargets[i])
            renderState.renderTargets[i]->Release();
        if (renderState.commandAllocators[i])
            renderState.commandAllocators[i]->Release();
    }
    if (renderState.swapChain)
        renderState.swapChain->Release();
    if (renderState.commandQueue)
        renderState.commandQueue->Release();
    if (renderState.bundleAllocator)
        renderState.bundleAllocator->Release();
    if (renderState.device)
        renderState.device->Release();
    if (renderState.hardwareAdapter)
        renderState.hardwareAdapter->Release();
    if (renderState.factory)
        renderState.factory->Release();
    if (renderState.vertexShader)
        renderState.vertexShader->Release();
    if (renderState.pixelShader)
        renderState.pixelShader->Release();
    if (signature)
        signature->Release();
    return (0);
}