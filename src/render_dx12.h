#pragma once

#pragma warning(push, 0)
#include <directx/d3dx12.h>
#include <dxgi1_6.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <dxcapi.h>

#include <imgui.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_dx12.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_thread.h>
#include <SDL3_image/SDL_image.h>
#pragma warning(pop)

#include "error.h"

struct dxc_context
{
    IDxcUtils *utils = nullptr;
    IDxcCompiler3 *compiler = nullptr;
    IDxcIncludeHandler *includeHandler = nullptr;

    bool init()
    {
        if (utils)
            return true; // already inited

        HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
        if (FAILED(hr))
            return false;

        hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
        if (FAILED(hr))
            return false;

        hr = utils->CreateDefaultIncludeHandler(&includeHandler);
        if (FAILED(hr))
            return false;

        return true;
    }
};

static struct
{
    DirectX::XMFLOAT4X4 world;
    DirectX::XMFLOAT4X4 view; // 4 x 4 matrix has 16 entries. 4 bytes per entry -> 64 bytes total
    DirectX::XMFLOAT4X4 projection;
    DirectX::XMVECTOR cameraPos;
    DirectX::XMFLOAT2 ringOffset;
    double timeElapsed;
    float ringWorldSize;
    float ringSampleStep;
    float planetScaleRatio = 1.0f / 75.0f;
    int terrainGridDimensionInVertices;
    float debug_scaler = 1.0f;
    
    unsigned int tileCount;
    unsigned int visibleTileWidth;
} constantBufferData;

struct vertex
{
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT2 texCoords;
    DirectX::XMFLOAT3 normals;
};

struct vertex_optimised
{
    uint16_t x;
    uint16_t y;
};

// TODO ultra optimised heightmap for grids that are set to 129x129 vertices?
//  struct vertex_ultra_optimised_heightmap {
//      uint8_t x;
//      uint8_t y;
//  };

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
    dxc_context dxc;

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
    ID3D12GraphicsCommandList *commandList = nullptr;

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

    // stride == size of 1 vertex
    bool create_and_upload(size_t vertexBufferSize, void *terrainPoints, UINT stride)
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
        vertexBufferView.StrideInBytes = stride;
        vertexBufferView.SizeInBytes = vertexBufferSize;
        return true;
    }
};

struct d3d12_texture_2d
{
    ID3D12Resource *texture = nullptr;
    wchar_t *filename;
    UINT descriptorIndex = 0; // which SRV slot in the heap

    bool create(wchar_t *_filename = L"gravel.dds", bool mipmaps = true, UINT srvIndex = 0)
    {
        filename = _filename;
        descriptorIndex = srvIndex;
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

        // If mipmaps are disabled, override metadata
        if (!mipmaps)
        {
            metadata.mipLevels = 1;
        }

        // Create GPU texture resource
        D3D12_RESOURCE_DESC textureDesc = {};
        textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        textureDesc.Alignment = 0;
        textureDesc.Width = (UINT)metadata.width;
        textureDesc.Height = (UINT)metadata.height;
        textureDesc.DepthOrArraySize = (UINT16)metadata.arraySize;
        textureDesc.MipLevels = (UINT16)metadata.mipLevels;
        textureDesc.Format = metadata.format;
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

        // Prepare subresources
        // TODO: remove std
        std::vector<D3D12_SUBRESOURCE_DATA> subresources;

        const DirectX::Image *imgs = image.GetImages();

        if (mipmaps)
        {
            // Use all mip levels
            subresources.reserve(image.GetImageCount());
            for (size_t i = 0; i < image.GetImageCount(); ++i)
            {
                D3D12_SUBRESOURCE_DATA s = {};
                s.pData = imgs[i].pixels;
                s.RowPitch = imgs[i].rowPitch;
                s.SlicePitch = imgs[i].slicePitch;
                subresources.push_back(s);
            }
        }
        else
        {
            // Only use mip 0
            subresources.reserve(1);
            D3D12_SUBRESOURCE_DATA s = {};
            s.pData = imgs[0].pixels;
            s.RowPitch = imgs[0].rowPitch;
            s.SlicePitch = imgs[0].slicePitch;
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

        // Upload only the selected mip levels
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
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderState.commandList->ResourceBarrier(1, &barrier);

        // Create SRV
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = metadata.format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = mipmaps ? (UINT)metadata.mipLevels : 1;

        D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU =
            renderState.srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandleCPU.ptr += renderState.cbvSrvDescriptorSize * descriptorIndex;

        renderState.device->CreateShaderResourceView(
            texture,
            &srvDesc,
            srvHandleCPU);

        // TODO: Need proper allocator that releases and stuff after pending
        // NOTE: CURRENT MEMORY LEAK BUT DONT UNCOMMENT THIS LINE IT CRASHES
        // textureUploadHeap->Release();
        return true;
    }
};

struct d3d12_bindless_texture
{
    ID3D12Resource* texture;
    ID3D12Resource* uploadHeap;
    UINT descriptorIndex;
    UINT mipLevels;
    DXGI_FORMAT format;
    UINT width;
    UINT height;

    bool loadFromDDS(const wchar_t* filename, UINT srvIndex, bool useMips)
    {
        texture = nullptr;
        uploadHeap = nullptr;
        descriptorIndex = srvIndex;

        DirectX::ScratchImage image;
        DirectX::TexMetadata metadata;

        HRESULT hr = DirectX::LoadFromDDSFile(
            filename,
            DirectX::DDS_FLAGS_NONE,
            &metadata,
            image);

        if (FAILED(hr))
        {
            errhr(L"LoadFromDDSFile failed", hr);
            return false;
        }

        width = (UINT)metadata.width;
        height = (UINT)metadata.height;
        mipLevels = useMips ? (UINT)metadata.mipLevels : 1;
        format = metadata.format;

        // --- Create GPU texture ---
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = (UINT16)mipLevels;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES heapPropsDefault(D3D12_HEAP_TYPE_DEFAULT);

        hr = renderState.device->CreateCommittedResource(
            &heapPropsDefault,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture));

        if (FAILED(hr))
        {
            errhr(L"CreateCommittedResource (texture)", hr);
            return false;
        }

        // --- Prepare subresources (no std::vector) ---
        const DirectX::Image* imgs = image.GetImages();

        D3D12_SUBRESOURCE_DATA subresources[32]; // supports up to 32 mips
        UINT count = mipLevels;

        for (UINT i = 0; i < count; i++)
        {
            subresources[i].pData = imgs[i].pixels;
            subresources[i].RowPitch = imgs[i].rowPitch;
            subresources[i].SlicePitch = imgs[i].slicePitch;
        }

        // --- Create upload heap ---
        UINT64 uploadSize = GetRequiredIntermediateSize(texture, 0, count);

        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadSize);

        hr = renderState.device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadHeap));

        if (FAILED(hr))
        {
            errhr(L"CreateCommittedResource (upload buffer)", hr);
            return false;
        }

        // --- Upload ---
        UpdateSubresources(
            renderState.commandList,
            texture,
            uploadHeap,
            0, 0,
            count,
            subresources);

        // --- Transition to SRV state ---
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            texture,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        renderState.commandList->ResourceBarrier(1, &barrier);

        // --- Create SRV ---
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = mipLevels;

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
            renderState.srvHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += descriptorIndex * renderState.cbvSrvDescriptorSize;

        renderState.device->CreateShaderResourceView(texture, &srvDesc, cpuHandle);

        return true;
    }
};


struct d3d12_texture_array
{
    ID3D12Resource *texture = nullptr; // The Texture2DArray resource
    UINT descriptorIndex = 0;          // SRV slot in the descriptor heap

    UINT width = 0;
    UINT height = 0;
    UINT slices = 0;
    UINT mipLevels = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;

    // Create the empty array resource + SRV
    bool create(UINT _width,
                UINT _height,
                UINT _slices,
                DXGI_FORMAT _format,
                UINT _mipLevels,
                UINT srvIndex)
    {
        width = _width;
        height = _height;
        slices = _slices;
        mipLevels = _mipLevels;
        format = _format;
        descriptorIndex = srvIndex;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = (UINT16)slices;
        desc.MipLevels = (UINT16)mipLevels;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        CD3DX12_HEAP_PROPERTIES heapPropsDefault(D3D12_HEAP_TYPE_DEFAULT);

        HRESULT hr = renderState.device->CreateCommittedResource(
            &heapPropsDefault,
            D3D12_HEAP_FLAG_NONE,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&texture));

        if (FAILED(hr))
        {
            errhr("CreateCommittedResource (Texture2DArray)", hr);
            return false;
        }

        // Create SRV for the entire array
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Texture2DArray.MipLevels = mipLevels;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = slices;

        D3D12_CPU_DESCRIPTOR_HANDLE srvHandleCPU =
            renderState.srvHeap->GetCPUDescriptorHandleForHeapStart();
        srvHandleCPU.ptr += renderState.cbvSrvDescriptorSize * descriptorIndex;

        renderState.device->CreateShaderResourceView(
            texture,
            &srvDesc,
            srvHandleCPU);

        return true;
    }

    bool uploadSliceFromDDS(const wchar_t *filename,
                            UINT slice,
                            bool mipmaps)
    {
        if (!texture || slice >= slices)
            return false;

        DirectX::ScratchImage image;
        DirectX::TexMetadata metadata;

        HRESULT hr = DirectX::LoadFromDDSFile(
            filename,
            DirectX::DDS_FLAGS_NONE,
            &metadata,
            image);

        if (FAILED(hr))
        {
            errhr(L"LoadFromDDSFile failed", hr);
            return false;
        }

        if (!mipmaps && metadata.mipLevels > 1)
        {
            err("Mipmaps in file but mipmaps=false specified. Remove the mipmaps from the file or use mipmaps");
            return false;
        }

        if (metadata.width != width || metadata.height != height)
        {
            err("DDS file does not match array texture dimensions");
            return false;
        }

        if (metadata.format != format)
        {
            err("DDS file does not match array texture format");
            return false;
        }

        // Determine how many mips to upload
        UINT srcMipLevels = mipmaps ? (UINT)metadata.mipLevels : 1;
        if (srcMipLevels > mipLevels)
            srcMipLevels = mipLevels;

        const DirectX::Image *imgs = image.GetImages();

        D3D12_SUBRESOURCE_DATA subresources[32]; // supports up to 32 mips
        UINT subresourceCount = srcMipLevels;

        for (UINT mip = 0; mip < srcMipLevels; ++mip)
        {
            const DirectX::Image &img = imgs[mip];
            subresources[mip].pData = img.pixels;
            subresources[mip].RowPitch = img.rowPitch;
            subresources[mip].SlicePitch = img.slicePitch;
        }

        UINT firstSubresource =
            D3D12CalcSubresource(0, slice, 0, mipLevels, slices);
        UINT64 uploadBufferSize =
            GetRequiredIntermediateSize(texture,
                                        firstSubresource,
                                        subresourceCount);

        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        auto uploadDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);

        ID3D12Resource *uploadHeap = nullptr;
        hr = renderState.device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&uploadHeap));

        if (FAILED(hr))
        {
            errhr(L"CreateCommittedResource (upload buffer)", hr);
            return false;
        }

        UpdateSubresources(
            renderState.commandList,
            texture,
            uploadHeap,
            0,
            firstSubresource,
            subresourceCount,
            subresources);

        return true;
    }
};

struct d3d12_shader_pair_old_model
{
    ID3DBlob *vertexShader = nullptr;
    ID3DBlob *pixelShader = nullptr;

    bool create(LPCWSTR filename)
    {
#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        ID3DBlob *vsError = nullptr;
        HRESULT hr = D3DCompileFromFile(filename, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &vsError);
        if (FAILED(hr))
        {
            errhr("D3DCompile from file failed (vertex shader)", hr);
            if (vsError)
            {
                SDL_Log((char *)vsError->GetBufferPointer());
                vsError->Release();
            }
            return false;
        }
        hr = D3DCompileFromFile(filename, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr);
        if (FAILED(hr))
        {
            errhr("D3DCompile from file failed (pixel shader)", hr);
            return false;
        }
        return true;
    }
};

struct d3d12_shader_pair
{
    ID3DBlob *vertexShader = nullptr;
    ID3DBlob *pixelShader = nullptr;

    bool compileShaderDXC(LPCWSTR filename, LPCWSTR entryPoint, LPCWSTR target, ID3DBlob **outBlob)
    {
        if (!renderState.dxc.init())
        {
            err("DXC init failed");
            return false;
        }

        IDxcBlobEncoding *source = nullptr;
        HRESULT hr = renderState.dxc.utils->LoadFile(filename, nullptr, &source);
        if (FAILED(hr))
        {
            errhr("DXC LoadFile failed", hr);
            return false;
        }

        DxcBuffer srcBuffer{};
        srcBuffer.Ptr = source->GetBufferPointer();
        srcBuffer.Size = source->GetBufferSize();
        srcBuffer.Encoding = DXC_CP_ACP;

        // TODO: take out std
        std::vector<LPCWSTR> args;
        args.push_back(filename);
        args.push_back(L"-E");
        args.push_back(entryPoint);
        args.push_back(L"-T");
        args.push_back(target);

#if defined(_DEBUG)
        args.push_back(L"-Zi"); // debug info
        args.push_back(L"-Qembed_debug");
        args.push_back(L"-Od"); // no optimization
#else
        args.push_back(L"-O3");
#endif

        IDxcResult *result = nullptr;
        hr = renderState.dxc.compiler->Compile(
            &srcBuffer,
            args.data(),
            (UINT)args.size(),
            renderState.dxc.includeHandler,
            IID_PPV_ARGS(&result));

        if (FAILED(hr) || result == nullptr)
        {
            errhr("DXC Compile failed", hr);
            source->Release();
            return false;
        }

        IDxcBlobUtf8 *errors = nullptr;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0)
        {
            SDL_Log("%s", errors->GetStringPointer());
        }
        if (errors)
            errors->Release();

        HRESULT status = S_OK;
        result->GetStatus(&status);
        if (FAILED(status))
        {
            errhr("DXC compilation failed (status)", status);
            result->Release();
            source->Release();
            return false;
        }

        IDxcBlob *shaderBlob = nullptr;
        hr = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
        result->Release();
        source->Release();

        if (FAILED(hr) || shaderBlob == nullptr)
        {
            errhr("DXC GetOutput(DXC_OUT_OBJECT) failed", hr);
            return false;
        }
        *outBlob = reinterpret_cast<ID3DBlob *>(shaderBlob);
        return true;
    }

    bool create(LPCWSTR filename)
    {
        if (!compileShaderDXC(filename, L"VSMain", L"vs_6_0", &vertexShader))
        {
            err("Failed to compile vertex shader with DXC");
            return false;
        }

        if (!compileShaderDXC(filename, L"PSMain", L"ps_6_0", &pixelShader))
        {
            err("Failed to compile pixel shader with DXC");
            return false;
        }

        return true;
    }
};

struct d3d12_pipeline_state
{
    ID3D12PipelineState *pipelineState = nullptr;

    bool create(D3D12_INPUT_LAYOUT_DESC inputLayout, d3d12_shader_pair *shaderPair, D3D12_RASTERIZER_DESC rasterizerDesc, bool depthEnable)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = inputLayout;
        psoDesc.pRootSignature = renderState.rootSignature;
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(shaderPair->vertexShader);
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(shaderPair->pixelShader);
        psoDesc.RasterizerState = rasterizerDesc;
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        // psoDesc.DepthStencilState.StencilEnable = FALSE; // TODO: what does this do
        if (depthEnable)
        {
            psoDesc.DepthStencilState.DepthEnable = TRUE;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        }
        else
        {
            psoDesc.DepthStencilState.DepthEnable = FALSE;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        }
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = renderState.rtvFormat;
        psoDesc.SampleDesc.Count = 1;

        HRESULT hr = renderState.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
        if (FAILED(hr))
        {
            errhr("CreateGraphicsPipelineState failed", hr);
            return false;
        }
        return true;
    }
};