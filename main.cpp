#pragma comment(lib, "SDL3.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

#pragma warning(push, 0)
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#pragma warning(pop)
#pragma warning(disable : 5045) // disabling the spectre mitigation warning (not relevant because we are a game, no sensitive information should be in this program)

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
    int width = 1280;
    int height = 720;

    if (!SetExtendedMetadata())
        return 1;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
    {
        err("SDL_Init failed");
        return 1;
    }

    programState.window = SDL_CreateWindow(APP_WINDOW_TITLE, (int)width, (int)height, 0);
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
        errhr("CreateDescriptorHeap failed", hr);
        return 1;
    }
    UINT rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // create frame resources
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(rtvHeap->GetCPUDescriptorHandleForHeapStart());

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
        device->CreateRenderTargetView(renderTargets[n], nullptr, rtvHandle);
        rtvHandle.Offset(1, rtvDescriptorSize);
    }
    ID3D12CommandAllocator *commandAllocator = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator));
    if (FAILED(hr))
    {
        errhr("CreateCommandAllocator failed", hr);
        return 1;
    }

    // load assets
    CD3DX12_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init(0, nullptr, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ID3DBlob *signature;
    ID3DBlob *error;
    hr = D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
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

    // create pipelin state (including shaders)
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
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};

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

    // FROM MICROSOFT:
    // Command lists are created in the recording state, but there is nothing
    // to record yet. The main loop expects it to be closed, so close it now.
    hr = commandList->Close();
    if (FAILED(hr))
    {
        errhr("Failed to Close command list", hr);
        return 1;
    }

    // vertex
    struct vertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 colour;
    };

    const float aspectRatio = 16.0f / 9.0f;
    vertex triangleVertices[] = {
        {{0.0f, 0.25f * aspectRatio, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        {{0.25f, -0.25f * aspectRatio, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}},
        {{-0.25f, -0.25f * aspectRatio, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f}}};

    const UINT vertexBuffersize = sizeof(triangleVertices);

    // FROM MICROSOFT:
    // Note: using upload heaps to transfer static data like vert buffers is not
    // recommended. Every time the GPU needs it, the upload heap will be marshalled
    // over. Please read up on Default Heap usage. An upload heap is used here for
    // code simplicity and because there are very few verts to actually transfer.
    ID3D12Resource* vertexBuffer = nullptr;
    hr = device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(vertexBuffersize), D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&vertexBuffer));
    UINT* vertexDataBegin = nullptr;
    CD3DX12_RANGE readRange(0,0);
    hr = vertexBuffer->Map(0, &readRange, (void**)&vertexDataBegin);
    if (FAILED(hr)) {
        errhr("Map failed (vertex buffer)", hr);
        return 1;
    }


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
        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandlePerFrame(rtvHeap->GetCPUDescriptorHandleForHeapStart(), frameIndex, rtvDescriptorSize);

        // commands
        const float clearColour[4] = {0.0f, 0.2f, 0.4f, 1.0f};
        commandList->ClearRenderTargetView(rtvHandlePerFrame, clearColour, 0, nullptr);
        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(renderTargets[frameIndex], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        hr = commandList->Close();
        if (FAILED(hr))
        {
            errhr("Failed to close command list (frame rendering)", hr);
            return 1;
        }
        // end of populating command list

        // execute command list
        ID3D12CommandList *commandLists[] = {commandList};
        commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

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