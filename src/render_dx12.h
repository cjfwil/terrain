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
    ID3D12Resource *constantBuffer = nullptr;

    ID3D12GraphicsCommandList *bundle = nullptr;
    const DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    UINT cbvSrvDescriptorSize = 0;
    UINT rtvDescriptorSize = 0;
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

struct d3d12_texture
{
    ID3D12Resource *texture = nullptr;
    wchar_t* filename;
    bool create(wchar_t* _filename=L"gravel.dds", bool mipmaps=true)
    {
        // Load BC7 DDS (with baked mipmaps) using DirectXTex

        filename = _filename;
        DirectX::ScratchImage image;
        DirectX::TexMetadata metadata;

        HRESULT hr = DirectX::LoadFromDDSFile(
            filename,
            DirectX::DDS_FLAGS_NONE,
            &metadata,
            image);
        if (FAILED(hr))
        {
            errhr("LoadFromDDSFile failed", hr);
            return false;
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
            IID_PPV_ARGS(&texture));
        if (FAILED(hr))
        {
            errhr("CreateCommittedResource (texture)", hr);
            return false;
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
            GetRequiredIntermediateSize(texture, 0, (UINT)subresources.size());

        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
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
            return false;
        }

        // Upload all mip levels

        UpdateSubresources(
            renderState.commandList,
            texture,
            textureUploadHeap,
            0, 0,
            (UINT)subresources.size(),
            subresources.data());

        // Transition to shader resource
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            texture,
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
        srvHandleCPU.ptr += renderState.cbvSrvDescriptorSize;

        renderState.device->CreateShaderResourceView(
            texture,
            &srvDesc,
            srvHandleCPU);

        CD3DX12_RESOURCE_BARRIER commandListResourceBarrierTransitionPixelShader = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        renderState.commandList->ResourceBarrier(1, &commandListResourceBarrierTransitionPixelShader);

        renderState.device->CreateShaderResourceView(texture, &srvDesc, srvHandleCPU);
        return true;
    }
};