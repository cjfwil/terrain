#pragma comment(lib, "SDL3.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#pragma warning(push, 0)
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>

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
        errhr("Failed to get HWND",hr);
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

        programState.ticksElapsed++;
    }
    SDL_DestroyWindow(programState.window);
    SDL_Quit();
    return (0);
}