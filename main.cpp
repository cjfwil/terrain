#pragma warning(disable : 5045) // disabling the spectre mitigation warning (not relevant because we are a game, no sensitive information should be in this program)
#pragma comment(lib, "SDL3.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

#pragma warning(push, 0)
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#pragma warning(pop)

#include "src/metadata.h"
#include "src/error.h"

static struct
{
    SDL_Window *window;
    Uint64 msElapsedSinceSDLInit;
    uint64_t ticksElapsed = 0;
    bool isRunning;
} programState;

int main(void)
{
    int width = 1920;
    int height = 1080;

    if (!SetExtendedMetadata())
        return 1;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        err("SDL_Init failed");
        return 1;
    }

    programState.window = SDL_CreateWindow(APP_WINDOW_TITLE, (int)width, (int)height, SDL_WINDOW_FULLSCREEN);
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

    IDXGIFactory6 *factory = nullptr;
    hr = CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        errhr("CreateDXGIFactory2 failed", hr);
        return 1;
    }

    IDXGIAdapter4 *hardwareAdapter = nullptr;

    hr = factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&hardwareAdapter));
    if (FAILED(hr))
    {
        errhr("EnumAdapterByGpuPreference failed", hr);
        return 1;
    }

    ID3D12Device *device = nullptr;
    hr = D3D12CreateDevice(hardwareAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
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

    ID3D12CommandQueue *commandQueue = nullptr;
    hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue));
    if (FAILED(hr))
    {
        errhr("CreateCommandQueue failed", hr);
        return 1;
    }

    static const int bufferCount = 3; // triple buffering
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = (UINT)width;
    swapChainDesc.Height = (UINT)height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = (UINT)bufferCount;
    swapChainDesc.Scaling = DXGI_SCALING_NONE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    SDL_PropertiesID props = SDL_GetWindowProperties(programState.window);
    HWND hwnd = nullptr;
    hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd)
    {
        errhr("Failed to get HWND", hr);
        return 1;
    }

    IDXGISwapChain1 *swapChain1 = nullptr;
    hr = factory->CreateSwapChainForHwnd(commandQueue, hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
    if (FAILED(hr))
    {
        errhr("CreateSwapChainForHwnd failed", hr);
        return 1;
    }

    IDXGISwapChain4 *swapChain = nullptr;
    hr = swapChain1->QueryInterface(IID_PPV_ARGS(&swapChain));
    if (FAILED(hr))
    {
        errhr("QueryInterface on swapChain1 failed", hr);
        return 1;
    }
    swapChain1->Release();
    UINT frameIndex = swapChain->GetCurrentBackBufferIndex();

    // create descriptor heaps
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = (UINT)bufferCount;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtvHeapDesc.NodeMask = 0;
    ID3D12DescriptorHeap *rtvHeap = nullptr;
    hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    if (FAILED(hr))
    {
        errhr("CreateDescriptorHeap failed (rtvHeap)", hr);
        return 1;
    }

    ID3D12DescriptorHeap *srvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap));
    if (FAILED(hr)) {
        errhr("CreateDescriptorHeap failed (srvHeap)", hr);
        return 1;
    }

    UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // create frame resources
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandleSetup(rtvHeap->GetCPUDescriptorHandleForHeapStart());

    ID3D12Resource *renderTargets[bufferCount] = {};
    // rtv for each buffer (double or triple buffering)
    for (UINT n = 0; n < bufferCount; n++)
    {
        hr = swapChain->GetBuffer(n, IID_PPV_ARGS(&renderTargets[n]));
        if (FAILED(hr))
        {
            errhr("GetBuffer failed", hr);
            return 1;
        }
        device->CreateRenderTargetView(renderTargets[n], nullptr, rtvHandleSetup);
        rtvHandleSetup.Offset(1, rtvDescriptorSize);
    }
    ID3D12CommandAllocator *commandAllocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr))
    {
        errhr("CreateCommandAllocator failed", hr);
        return 1;
    }

    // load assets
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
    hr = device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData));
    if (FAILED(hr))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
    CD3DX12_ROOT_PARAMETER1 rootParameters[1];
    rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    sampler.MipLODBias = 0;
    sampler.MaxAnisotropy = 0;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ID3DBlob *signature;
    ID3DBlob *error;
    hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error);
    if (FAILED(hr))
    {
        errhr("D3D12SerializeRootSignature failed", hr);
        return 1;
    }
    ID3D12RootSignature *rootSignature = nullptr;
    hr = device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
    if (FAILED(hr))
    {
        errhr("CreateRootSignature failed", hr);
        return 1;
    }

    // create pipeline state (including shaders)
    ID3DBlob *vertexShader = nullptr;
    ID3DBlob *pixelShader = nullptr;
#if defined(_DEBUG)
    // Enable better shader debugging with the graphics debugging tools.
    UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compileFlags = 0;
#endif

    hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr);
    if (FAILED(hr))
    {
        errhr("D3DCompile from file failed (vertex shader)", hr);
        return 1;
    }
    hr = D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr);
    if (FAILED(hr))
    {
        errhr("D3DCompile from file failed (pixel shader)", hr);
        return 1;
    }

    D3D12_INPUT_ELEMENT_DESC inputElementDesc[] =
        {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = {inputElementDesc, _countof(inputElementDesc)};
    psoDesc.pRootSignature = rootSignature;
    psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader);
    psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader);
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = FALSE;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    psoDesc.SampleDesc.Count = 1;
    ID3D12PipelineState *pipelineState = nullptr;
    hr = device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
    if (FAILED(hr))
    {
        errhr("CreateGraphicsPipelineState failed", hr);
        return 1;
    }

    ID3D12GraphicsCommandList *commandList = nullptr;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, nullptr, IID_PPV_ARGS(&commandList));
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

    const float aspectRatio = 16.0f / 9.0f;
    vertex triangleVertices[] = {
        {{0.0f, 0.25f * aspectRatio, 0.0f}, {0.5f, 0.0f}},
        {{0.25f, -0.25f * aspectRatio, 0.0f}, {1.0f, 1.0f}},
        {{-0.25f, -0.25f * aspectRatio, 0.0f}, {0.0f, 1.0f}}};

    const UINT vertexBufferSize = sizeof(triangleVertices);

    // FROM MICROSOFT:
    // Note: using upload heaps to transfer static data like vert buffers is not
    // recommended. Every time the GPU needs it, the upload heap will be marshalled
    // over. Please read up on Default Heap usage. An upload heap is used here for
    // code simplicity and because there are very few verts to actually transfer.
    ID3D12Resource *vertexBuffer = nullptr;
    hr = device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer));
    UINT *vertexDataBegin = nullptr;
    CD3DX12_RANGE readRange(0, 0);
    hr = vertexBuffer->Map(0, &readRange, (void **)&vertexDataBegin);
    if (FAILED(hr))
    {
        errhr("Map failed (vertex buffer)", hr);
        return 1;
    }
    memcpy(vertexDataBegin, triangleVertices, sizeof(triangleVertices));
    vertexBuffer->Unmap(0, nullptr);

    D3D12_VERTEX_BUFFER_VIEW vertexBufferView = {};
    vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexBufferView.StrideInBytes = sizeof(vertex);
    vertexBufferView.SizeInBytes = vertexBufferSize;

    const int texWidth = 16;
    const int texHeight = 16;
    unsigned int textureData[texWidth * texHeight] = {};    
    for (int i = 0; i < texWidth * texHeight; ++i)
    {
        int x = i % texWidth;
        int y = i / texHeight;
        if ((x+y) % 2 == 0)
            textureData[i] = 0xfffff000;
        else
            textureData[i] = 0xff000fff;
    }

    ID3D12Resource *textureUploadHeap = nullptr;
    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.Width = texWidth;
    textureDesc.Height = texHeight;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    ID3D12Resource *texture = nullptr;
    hr = device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture));
    if (FAILED(hr))
    {
        errhr("CreateCommittedResource (texture)", hr);
        return 1;
    }
    const UINT64 uploadBufferSize = GetRequiredIntermediateSize(texture, 0, 1);

    // gpu upload buffer
    hr = device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&textureUploadHeap));
    if (FAILED(hr))
    {
        errhr("CreateCommittedResource failed (gpu upload buffer)", hr);
        return 1;
    }

    D3D12_SUBRESOURCE_DATA textureDataDesc = {};
    textureDataDesc.pData = textureData;
    textureDataDesc.RowPitch = texWidth * sizeof(textureData[0]);
    textureDataDesc.SlicePitch = textureDataDesc.RowPitch * texHeight;

    UpdateSubresources(commandList, texture, textureUploadHeap, 0, 0, 1, &textureDataDesc);
    commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    // SRV for texture
    
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(texture, &srvDesc, srvHeap->GetCPUDescriptorHandleForHeapStart());

    commandList->Close();
    ID3D12CommandList *commandListsSetup[] = {commandList};
    commandQueue->ExecuteCommandLists(_countof(commandListsSetup), commandListsSetup);

    // create synchronisation objects
    ID3D12Fence *fence = nullptr;
    UINT64 fenceValue = 0;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fenceValue = 1;
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

    // main program
    programState.isRunning = true;
    while (programState.isRunning)
    {
        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent))
        {
            switch (sdlEvent.type)
            {
            case SDL_EVENT_QUIT:
                programState.isRunning = false;
                break;
            }
        }
        programState.msElapsedSinceSDLInit = SDL_GetTicks();

        // main loop main body
        // update here

        // render here
        // populate command list
        commandAllocator->Reset();
        commandList->Reset(commandAllocator, pipelineState);

        commandList->SetGraphicsRootSignature(rootSignature);

        ID3D12DescriptorHeap *heaps[] = {srvHeap};
        commandList->SetDescriptorHeaps(_countof(heaps), heaps);

        commandList->SetGraphicsRootDescriptorTable(0, srvHeap->GetGPUDescriptorHandleForHeapStart());
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissorRect);

        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandlePerFrame(rtvHeap->GetCPUDescriptorHandleForHeapStart(), frameIndex, rtvDescriptorSize);
        commandList->OMSetRenderTargets(1, &rtvHandlePerFrame, FALSE, nullptr);

        // commands
        const float clearColour[4] = {0.0f, 0.2f, 0.4f, 1.0f};
        commandList->ClearRenderTargetView(rtvHandlePerFrame, clearColour, 0, nullptr);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
        commandList->DrawInstanced(3, 1, 0, 0);

        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        hr = commandList->Close();
        if (FAILED(hr))
        {
            errhr("Failed to close command list (frame rendering)", hr);
            return 1;
        }
        // end of populating command list

        // execute command list
        ID3D12CommandList *commandListsPerFrame[] = {commandList};
        commandQueue->ExecuteCommandLists(_countof(commandListsPerFrame), commandListsPerFrame);

        hr = swapChain->Present(1, 0);
        if (FAILED(hr))
        {
            errhr("Present failed", hr);
            return 1;
        }

        // wait for prev frame (d3d11 style)
        //  TODO: switch to more d3d12 style frame buffering (no waiting)
        const UINT64 localFenceValue = fenceValue; // DO NOT ACCIDENTLY CHANGE THE FENCE VALUE HERE, it needs to be the same thats why we are creating a const copy
        hr = commandQueue->Signal(fence, localFenceValue);
        if (FAILED(hr))
        {
            errhr("Signal failed (command queue)", hr);
            return 1;
        }
        fenceValue++; // now we chan change it this is deliberate

        // waiting (d3d11 style)
        if (fence->GetCompletedValue() < localFenceValue)
        {
            hr = fence->SetEventOnCompletion(localFenceValue, fenceEvent);
            if (FAILED(hr))
            {
                errhr("SetEventOnCompletion failed", hr);
                return 1;
            }
            WaitForSingleObject(fenceEvent, INFINITE);
        }

        frameIndex = swapChain->GetCurrentBackBufferIndex();
        // end of waiting

        programState.ticksElapsed++;
    }
    SDL_DestroyWindow(programState.window);
    SDL_Quit();
    return (0);
}