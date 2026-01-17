#pragma once

#pragma warning(push, 0)
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_dx12.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3_image/SDL_image.h>
#pragma warning(pop)

#include "error.h"

struct vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 texCoords;
    DirectX::XMFLOAT3 normals;
};

// Simple free list based allocator
struct ImGuiDescriptorHeapAllocator
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
        for (UINT n = desc.NumDescriptors; n > 0; n--)
            FreeIndices.push_back((int)n - 1);
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
    ID3D12DescriptorHeap *imguiSrvHeap = nullptr;
    ImGuiDescriptorHeapAllocator imguiSrvAllocator;
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
    ID3D12Resource *depthBuffer = nullptr;
    ID3D12DescriptorHeap *dsvHeap = nullptr;
    ID3D12CommandAllocator *bundleAllocator = nullptr;
    ID3D12RootSignature *rootSignature = nullptr;
    ID3D12Fence *fence = nullptr;
    ID3D12PipelineState *pipelineState = nullptr;
    ID3D12GraphicsCommandList *commandList = nullptr;
    ID3DBlob *vertexShader = nullptr;
    ID3DBlob *pixelShader = nullptr;
    ID3D12Resource *texture = nullptr;
    ID3D12Resource *constantBuffer = nullptr;

    ID3D12GraphicsCommandList *bundle = nullptr;
    const DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // end of init
} renderState;

struct d3d12_index_buffer
{
    D3D12_INDEX_BUFFER_VIEW indexBufferView = {};
    ID3D12Resource *indexBuffer = nullptr;    
    bool create_and_upload(size_t indexBufferSize, void *indexBufferData)
    {
        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC indexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize);
        HRESULT hr = renderState.device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &indexBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&indexBuffer));
        if (FAILED(hr))
        {
            errhr("CreateCommittedResource failed", hr);
            return false;
        }

        Uint32 *gpuIndexData = nullptr;

        CD3DX12_RANGE readRangeIndexBuffer(0, 0); // we do not intend to read from this resource
        indexBuffer->Map(0, &readRangeIndexBuffer, (void **)&gpuIndexData);
        memcpy(gpuIndexData, indexBufferData, indexBufferSize);
        indexBuffer->Unmap(0, nullptr);

        indexBufferView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexBufferView.SizeInBytes = (UINT)indexBufferSize;
        indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        return true;
    }
};

struct d3d12_vertex_buffer
{

    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    ID3D12Resource *vertexBuffer = nullptr;

    bool create_and_upload(size_t vertexBufferSize, void *terrainPoints)
    {
        // FROM MICROSOFT:
        // Note: using upload heaps to transfer static data like vert buffers is not
        // recommended. Every time the GPU needs it, the upload heap will be marshalled
        // over. Please read up on Default Heap usage. An upload heap is used here for
        // code simplicity and because there are very few verts to actually transfer.
        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC vertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(vertexBufferSize);
        HRESULT hr = renderState.device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &vertexBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&vertexBuffer));
        UINT *vertexDataBegin = nullptr;
        CD3DX12_RANGE readRange(0, 0);
        hr = vertexBuffer->Map(0, &readRange, (void **)&vertexDataBegin);
        if (FAILED(hr))
        {
            errhr("Map failed (vertex buffer)", hr);
            return false;
        }
        // memcpy(vertexDataBegin, triangleVertices, terrainMeshBufferSize);
        memcpy(vertexDataBegin, terrainPoints, vertexBufferSize);
        vertexBuffer->Unmap(0, nullptr);

        vertexBufferView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
        vertexBufferView.StrideInBytes = sizeof(vertex);
        vertexBufferView.SizeInBytes = vertexBufferSize;
        return true;
    }
};