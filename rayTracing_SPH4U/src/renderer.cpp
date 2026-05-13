#include "../include/renderer.h"
#include "../include/objects.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT FrameCount = 2;
constexpr UINT DescriptorCount = 3;

void throwIfFailed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

UINT64 alignTo(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

D3D12_CPU_DESCRIPTOR_HANDLE offsetCpu(D3D12_CPU_DESCRIPTOR_HANDLE handle, UINT offset, UINT size) {
    handle.ptr += static_cast<SIZE_T>(offset) * size;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE offsetGpu(D3D12_GPU_DESCRIPTOR_HANDLE handle, UINT offset, UINT size) {
    handle.ptr += static_cast<UINT64>(offset) * size;
    return handle;
}

std::vector<uint8_t> readFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }

    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

std::filesystem::path findShaderPath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::vector<std::filesystem::path> roots = {
        std::filesystem::current_path(),
        std::filesystem::path(exePath).parent_path(),
        std::filesystem::path(exePath).parent_path().parent_path(),
        std::filesystem::path(exePath).parent_path().parent_path().parent_path()
    };

    for (const auto& root : roots) {
        const std::filesystem::path candidates[] = {
            root / L"shaders" / L"raytracing.hlsl",
            root / L"rayTracing_SPH4U" / L"shaders" / L"raytracing.hlsl"
        };

        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    throw std::runtime_error("Could not find shaders/raytracing.hlsl");
}

std::vector<std::filesystem::path> searchRoots() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::vector<std::filesystem::path> roots = {
        std::filesystem::current_path(),
        std::filesystem::path(exePath).parent_path()
    };

    std::filesystem::path root = std::filesystem::path(exePath).parent_path();
    for (int i = 0; i < 8 && root.has_parent_path(); ++i) {
        root = root.parent_path();
        roots.push_back(root);
    }

    return roots;
}

std::filesystem::path findDxcCompilerPath() {
    for (const auto& root : searchRoots()) {
        const std::filesystem::path candidates[] = {
            root / L"bin" / L"x64" / L"dxcompiler.dll",
            root / L"dxc" / L"bin" / L"x64" / L"dxcompiler.dll",
            root / L"dxcompiler.dll"
        };

        for (const auto& path : candidates) {
            if (std::filesystem::exists(path)) {
                return path;
            }
        }
    }

    return {};
}

ComPtr<IDxcBlob> compileRayTracingLibrary() {
    using DxcCreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

    std::filesystem::path dxcPath = findDxcCompilerPath();
    if (!dxcPath.empty()) {
        SetDllDirectoryW(dxcPath.parent_path().c_str());
    }

    HMODULE dxcModule = LoadLibraryW(dxcPath.empty() ? L"dxcompiler.dll" : dxcPath.c_str());
    if (!dxcModule) {
        throw std::runtime_error("dxcompiler.dll was not found. Install the DirectX Shader Compiler runtime or place dxcompiler.dll beside the executable.");
    }

    auto createDxcInstance = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxcModule, "DxcCreateInstance"));
    if (!createDxcInstance) {
        throw std::runtime_error("dxcompiler.dll does not export DxcCreateInstance");
    }

    const auto shaderBytes = readFileBytes(findShaderPath());
    if (shaderBytes.empty()) {
        throw std::runtime_error("raytracing.hlsl is empty or unreadable");
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includeHandler;
    throwIfFailed(createDxcInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "Failed to create DXC utils");
    throwIfFailed(createDxcInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "Failed to create DXC compiler");
    throwIfFailed(utils->CreateDefaultIncludeHandler(&includeHandler), "Failed to create DXC include handler");

    DxcBuffer source = {};
    source.Ptr = shaderBytes.data();
    source.Size = shaderBytes.size();
    source.Encoding = DXC_CP_UTF8;

    const wchar_t* args[] = {
        L"-T", L"lib_6_3",
        L"-Zi",
        L"-Qembed_debug",
        L"-O3"
    };

    ComPtr<IDxcResult> result;
    throwIfFailed(compiler->Compile(&source, args, _countof(args), includeHandler.Get(), IID_PPV_ARGS(&result)),
        "DXC failed to compile raytracing.hlsl");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        std::string message = "raytracing.hlsl compilation failed";
        if (errors && errors->GetStringLength() > 0) {
            message += ": ";
            message += errors->GetStringPointer();
        }
        throw std::runtime_error(message.c_str());
    }

    ComPtr<IDxcBlob> library;
    throwIfFailed(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&library), nullptr),
        "DXC did not produce a DXIL library");
    return library;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER uavBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    return barrier;
}
}

struct DxrRenderer::Impl {
    HWND hwnd = nullptr;
    UINT width = 0;
    UINT height = 0;
    UINT frameIndex = 0;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;
    std::wstring lastError = L"";

    ComPtr<IDXGIFactory6> factory;
    ComPtr<ID3D12Device5> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> srvUavHeap;
    ComPtr<ID3D12CommandAllocator> commandAllocators[FrameCount];
    ComPtr<ID3D12GraphicsCommandList4> commandList;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    ComPtr<ID3D12Resource> outputTexture;
    ComPtr<ID3D12Resource> sphereBuffer;
    ComPtr<ID3D12Resource> aabbBuffer;
    ComPtr<ID3D12Resource> blasScratch;
    ComPtr<ID3D12Resource> blasResult;
    ComPtr<ID3D12Resource> tlasScratch;
    ComPtr<ID3D12Resource> tlasResult;
    ComPtr<ID3D12Resource> instanceBuffer;
    ComPtr<ID3D12RootSignature> globalRootSignature;
    ComPtr<ID3D12StateObject> stateObject;
    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    ComPtr<ID3D12Resource> shaderTable;

    UINT rtvDescriptorSize = 0;
    UINT srvUavDescriptorSize = 0;
    UINT shaderRecordSize = 0;
    UINT64 rayGenOffset = 0;
    UINT64 missOffset = 0;
    UINT64 hitGroupOffset = 0;
    UINT sphereCount = 0;

    void initialize(HWND window, int renderWidth, int renderHeight) {
        hwnd = window;
        width = static_cast<UINT>(renderWidth);
        height = static_cast<UINT>(renderHeight);

        createDevice();
        createSwapChain();
        createDescriptors();
        createOutputTexture();
        createScene();
        createAccelerationStructures();
        createGlobalRootSignature();
        createStateObject();
        createShaderTable();
    }

    void createDevice() {
        throwIfFailed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "Failed to create DXGI factory");

        ComPtr<IDXGIAdapter1> adapter;
        for (UINT index = 0;
            factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
            ++index) {
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                continue;
            }

            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)))) {
                break;
            }
        }

        if (!device) {
            throw std::runtime_error("No Direct3D 12 feature level 12.1 adapter was found");
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
        throwIfFailed(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)),
            "Failed to query DXR feature support");
        if (options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
            throw std::runtime_error("This GPU/driver does not expose DirectX Raytracing support");
        }

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        throwIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueue)), "Failed to create command queue");

        for (UINT i = 0; i < FrameCount; ++i) {
            throwIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])),
                "Failed to create command allocator");
        }

        throwIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[0].Get(), nullptr,
            IID_PPV_ARGS(&commandList)), "Failed to create command list");
        commandList->Close();

        throwIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Failed to create fence");
        fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            throw std::runtime_error("Failed to create fence event");
        }
    }

    void createSwapChain() {
        DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
        swapChainDesc.BufferCount = FrameCount;
        swapChainDesc.Width = width;
        swapChainDesc.Height = height;
        swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapChainDesc.SampleDesc.Count = 1;

        ComPtr<IDXGISwapChain1> swapChain1;
        throwIfFailed(factory->CreateSwapChainForHwnd(commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1),
            "Failed to create swap chain");
        throwIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "Failed to configure window association");
        throwIfFailed(swapChain1.As(&swapChain), "Failed to query IDXGISwapChain3");
        frameIndex = swapChain->GetCurrentBackBufferIndex();
    }

    void createDescriptors() {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
        rtvDesc.NumDescriptors = FrameCount;
        rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        throwIfFailed(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&rtvHeap)), "Failed to create RTV heap");
        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (UINT i = 0; i < FrameCount; ++i) {
            throwIfFailed(swapChain->GetBuffer(i, IID_PPV_ARGS(&renderTargets[i])), "Failed to get swap-chain buffer");
            device->CreateRenderTargetView(renderTargets[i].Get(), nullptr,
                offsetCpu(rtvHeap->GetCPUDescriptorHandleForHeapStart(), i, rtvDescriptorSize));
        }

        D3D12_DESCRIPTOR_HEAP_DESC srvUavDesc = {};
        srvUavDesc.NumDescriptors = DescriptorCount;
        srvUavDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvUavDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        throwIfFailed(device->CreateDescriptorHeap(&srvUavDesc, IID_PPV_ARGS(&srvUavHeap)), "Failed to create SRV/UAV heap");
        srvUavDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    ComPtr<ID3D12Resource> createBuffer(UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState,
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = heapType;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;

        ComPtr<ID3D12Resource> resource;
        throwIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, initialState, nullptr,
            IID_PPV_ARGS(&resource)), "Failed to create buffer");
        return resource;
    }

    void createOutputTexture() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        throwIfFailed(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outputTexture)), "Failed to create output texture");

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, srvUavDescriptorSize));
    }

    void uploadToResource(ID3D12Resource* resource, const void* data, size_t size) {
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        throwIfFailed(resource->Map(0, &readRange, &mapped), "Failed to map upload resource");
        memcpy(mapped, data, size);
        resource->Unmap(0, nullptr);
    }

    void createScene() {
        const Sphere spheres[] = {
            {{ 0.0f, 0.0f,-3.0f}, 1.0f, {{0.7f,0.7f,0.7f}, DIFFUSE,    1.0f, 0.0f, {0.0f, 0.0f}}},
            {{ 2.0f, 0.0f,-4.0f}, 1.0f, {{0.9f,0.9f,0.9f}, METAL,      1.0f, 1.0f, {0.0f, 0.0f}}},
            {{-2.0f, 0.0f,-4.0f}, 1.0f, {{1.0f,1.0f,1.0f}, DIELECTRIC, 1.5f, 0.0f, {0.0f, 0.0f}}},
            {{ 0.0f,-1001.0f,-3.0f}, 1000.0f, {{0.2f,0.8f,0.2f}, DIFFUSE, 1.0f, 0.0f, {0.0f, 0.0f}}}
        };
        sphereCount = _countof(spheres);

        sphereBuffer = createBuffer(sizeof(spheres), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(sphereBuffer.Get(), spheres, sizeof(spheres));

        std::vector<D3D12_RAYTRACING_AABB> aabbs;
        aabbs.reserve(sphereCount);
        for (const Sphere& sphere : spheres) {
            aabbs.push_back({
                sphere.center.x - sphere.radius,
                sphere.center.y - sphere.radius,
                sphere.center.z - sphere.radius,
                sphere.center.x + sphere.radius,
                sphere.center.y + sphere.radius,
                sphere.center.z + sphere.radius
            });
        }

        aabbBuffer = createBuffer(sizeof(D3D12_RAYTRACING_AABB) * aabbs.size(),
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(aabbBuffer.Get(), aabbs.data(), sizeof(D3D12_RAYTRACING_AABB) * aabbs.size());

        D3D12_SHADER_RESOURCE_VIEW_DESC sphereSrvDesc = {};
        sphereSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sphereSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
        sphereSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sphereSrvDesc.Buffer.NumElements = sphereCount;
        sphereSrvDesc.Buffer.StructureByteStride = sizeof(Sphere);
        device->CreateShaderResourceView(sphereBuffer.Get(), &sphereSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 2, srvUavDescriptorSize));
    }

    void createAccelerationStructures() {
        commandAllocators[frameIndex]->Reset();
        commandList->Reset(commandAllocators[frameIndex].Get(), nullptr);

        D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
        geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        geometryDesc.AABBs.AABBCount = sphereCount;
        geometryDesc.AABBs.AABBs.StartAddress = aabbBuffer->GetGPUVirtualAddress();
        geometryDesc.AABBs.AABBs.StrideInBytes = sizeof(D3D12_RAYTRACING_AABB);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.NumDescs = 1;
        blasInputs.pGeometryDescs = &geometryDesc;
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasInfo);
        blasScratch = createBuffer(blasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        blasResult = createBuffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
        blasDesc.Inputs = blasInputs;
        blasDesc.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
        blasDesc.DestAccelerationStructureData = blasResult->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr);
        auto blasBarrier = uavBarrier(blasResult.Get());
        commandList->ResourceBarrier(1, &blasBarrier);

        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0][0] = 1.0f;
        instanceDesc.Transform[1][1] = 1.0f;
        instanceDesc.Transform[2][2] = 1.0f;
        instanceDesc.InstanceMask = 0xFF;
        instanceDesc.AccelerationStructure = blasResult->GetGPUVirtualAddress();

        instanceBuffer = createBuffer(sizeof(instanceDesc), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        uploadToResource(instanceBuffer.Get(), &instanceDesc, sizeof(instanceDesc));

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
        tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        tlasInputs.NumDescs = 1;
        tlasInputs.InstanceDescs = instanceBuffer->GetGPUVirtualAddress();
        tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasInfo = {};
        device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasInfo);
        tlasScratch = createBuffer(tlasInfo.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        tlasResult = createBuffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc = {};
        tlasDesc.Inputs = tlasInputs;
        tlasDesc.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
        tlasDesc.DestAccelerationStructureData = tlasResult->GetGPUVirtualAddress();
        commandList->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);
        auto tlasBarrier = uavBarrier(tlasResult.Get());
        commandList->ResourceBarrier(1, &tlasBarrier);

        commandList->Close();
        ID3D12CommandList* lists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);
        waitForGpu();

        D3D12_SHADER_RESOURCE_VIEW_DESC tlasSrvDesc = {};
        tlasSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tlasSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
        tlasSrvDesc.RaytracingAccelerationStructure.Location = tlasResult->GetGPUVirtualAddress();
        device->CreateShaderResourceView(nullptr, &tlasSrvDesc,
            offsetCpu(srvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, srvUavDescriptorSize));
    }

    void createGlobalRootSignature() {
        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[2].NumDescriptors = 1;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[4] = {};
        for (UINT i = 0; i < 3; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[i].DescriptorTable.NumDescriptorRanges = 1;
            params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        }
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[3].Constants.Num32BitValues = 4;
        params[3].Constants.ShaderRegister = 0;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = _countof(params);
        desc.pParameters = params;

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> errors;
        throwIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors),
            "Failed to serialize root signature");
        throwIfFailed(device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&globalRootSignature)), "Failed to create root signature");
    }

    void createStateObject() {
        ComPtr<IDxcBlob> dxilLibrary = compileRayTracingLibrary();

        const wchar_t* rayGen = L"RayGen";
        const wchar_t* miss = L"Miss";
        const wchar_t* closestHit = L"ClosestHit";
        const wchar_t* intersection = L"SphereIntersection";
        const wchar_t* hitGroup = L"SphereHitGroup";

        D3D12_EXPORT_DESC exports[] = {
            { rayGen, nullptr, D3D12_EXPORT_FLAG_NONE },
            { miss, nullptr, D3D12_EXPORT_FLAG_NONE },
            { closestHit, nullptr, D3D12_EXPORT_FLAG_NONE },
            { intersection, nullptr, D3D12_EXPORT_FLAG_NONE }
        };

        D3D12_DXIL_LIBRARY_DESC libraryDesc = {};
        libraryDesc.DXILLibrary.BytecodeLength = dxilLibrary->GetBufferSize();
        libraryDesc.DXILLibrary.pShaderBytecode = dxilLibrary->GetBufferPointer();
        libraryDesc.NumExports = _countof(exports);
        libraryDesc.pExports = exports;

        D3D12_STATE_SUBOBJECT librarySubobject = {};
        librarySubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
        librarySubobject.pDesc = &libraryDesc;

        D3D12_HIT_GROUP_DESC hitGroupDesc = {};
        hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        hitGroupDesc.HitGroupExport = hitGroup;
        hitGroupDesc.ClosestHitShaderImport = closestHit;
        hitGroupDesc.IntersectionShaderImport = intersection;

        D3D12_STATE_SUBOBJECT hitGroupSubobject = {};
        hitGroupSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
        hitGroupSubobject.pDesc = &hitGroupDesc;

        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
        shaderConfig.MaxPayloadSizeInBytes = 32;
        shaderConfig.MaxAttributeSizeInBytes = 16;

        D3D12_STATE_SUBOBJECT shaderConfigSubobject = {};
        shaderConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
        shaderConfigSubobject.pDesc = &shaderConfig;

        D3D12_GLOBAL_ROOT_SIGNATURE globalRoot = {};
        globalRoot.pGlobalRootSignature = globalRootSignature.Get();

        D3D12_STATE_SUBOBJECT rootSignatureSubobject = {};
        rootSignatureSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
        rootSignatureSubobject.pDesc = &globalRoot;

        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig = {};
        pipelineConfig.MaxTraceRecursionDepth = 6;

        D3D12_STATE_SUBOBJECT pipelineConfigSubobject = {};
        pipelineConfigSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
        pipelineConfigSubobject.pDesc = &pipelineConfig;

        D3D12_STATE_SUBOBJECT subobjects[] = {
            librarySubobject,
            hitGroupSubobject,
            shaderConfigSubobject,
            rootSignatureSubobject,
            pipelineConfigSubobject
        };

        D3D12_STATE_OBJECT_DESC stateObjectDesc = {};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = _countof(subobjects);
        stateObjectDesc.pSubobjects = subobjects;

        throwIfFailed(device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&stateObject)),
            "Failed to create DXR state object");
        throwIfFailed(stateObject.As(&stateObjectProps), "Failed to query DXR state object properties");
    }

    void createShaderTable() {
        shaderRecordSize = static_cast<UINT>(alignTo(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,
            D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT));

        rayGenOffset = 0;
        missOffset = alignTo(rayGenOffset + shaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        hitGroupOffset = alignTo(missOffset + shaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
        const UINT64 tableSize = alignTo(hitGroupOffset + shaderRecordSize, D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);

        shaderTable = createBuffer(tableSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

        uint8_t* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        throwIfFailed(shaderTable->Map(0, &readRange, reinterpret_cast<void**>(&mapped)), "Failed to map shader table");
        memcpy(mapped + rayGenOffset, stateObjectProps->GetShaderIdentifier(L"RayGen"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        memcpy(mapped + missOffset, stateObjectProps->GetShaderIdentifier(L"Miss"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        memcpy(mapped + hitGroupOffset, stateObjectProps->GetShaderIdentifier(L"SphereHitGroup"), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        shaderTable->Unmap(0, nullptr);
    }

    void render() {
        commandAllocators[frameIndex]->Reset();
        commandList->Reset(commandAllocators[frameIndex].Get(), nullptr);

        ID3D12DescriptorHeap* heaps[] = { srvUavHeap.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetComputeRootSignature(globalRootSignature.Get());
        commandList->SetComputeRootDescriptorTable(0, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 0, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(1, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 1, srvUavDescriptorSize));
        commandList->SetComputeRootDescriptorTable(2, offsetGpu(srvUavHeap->GetGPUDescriptorHandleForHeapStart(), 2, srvUavDescriptorSize));
        const UINT constants[] = { width, height, frameIndex, sphereCount };
        commandList->SetComputeRoot32BitConstants(3, _countof(constants), constants, 0);

        commandList->SetPipelineState1(stateObject.Get());

        D3D12_DISPATCH_RAYS_DESC dispatch = {};
        const D3D12_GPU_VIRTUAL_ADDRESS tableStart = shaderTable->GetGPUVirtualAddress();
        dispatch.RayGenerationShaderRecord.StartAddress = tableStart + rayGenOffset;
        dispatch.RayGenerationShaderRecord.SizeInBytes = shaderRecordSize;
        dispatch.MissShaderTable.StartAddress = tableStart + missOffset;
        dispatch.MissShaderTable.SizeInBytes = shaderRecordSize;
        dispatch.MissShaderTable.StrideInBytes = shaderRecordSize;
        dispatch.HitGroupTable.StartAddress = tableStart + hitGroupOffset;
        dispatch.HitGroupTable.SizeInBytes = shaderRecordSize;
        dispatch.HitGroupTable.StrideInBytes = shaderRecordSize;
        dispatch.Width = width;
        dispatch.Height = height;
        dispatch.Depth = 1;
        commandList->DispatchRays(&dispatch);

        auto outputUav = uavBarrier(outputTexture.Get());
        commandList->ResourceBarrier(1, &outputUav);

        D3D12_RESOURCE_BARRIER copyBarriers[] = {
            transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
        };
        commandList->ResourceBarrier(_countof(copyBarriers), copyBarriers);
        commandList->CopyResource(renderTargets[frameIndex].Get(), outputTexture.Get());

        D3D12_RESOURCE_BARRIER presentBarriers[] = {
            transition(outputTexture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(renderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)
        };
        commandList->ResourceBarrier(_countof(presentBarriers), presentBarriers);

        commandList->Close();
        ID3D12CommandList* lists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);

        throwIfFailed(swapChain->Present(1, 0), "Swap-chain present failed");
        moveToNextFrame();
    }

    void waitForGpu() {
        const UINT64 value = ++fenceValue;
        throwIfFailed(commandQueue->Signal(fence.Get(), value), "Failed to signal fence");
        if (fence->GetCompletedValue() < value) {
            throwIfFailed(fence->SetEventOnCompletion(value, fenceEvent), "Failed to set fence completion event");
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    void moveToNextFrame() {
        const UINT64 value = ++fenceValue;
        throwIfFailed(commandQueue->Signal(fence.Get(), value), "Failed to signal frame fence");
        frameIndex = swapChain->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < value) {
            throwIfFailed(fence->SetEventOnCompletion(value, fenceEvent), "Failed to set frame fence event");
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    void shutdown() {
        if (device && commandQueue && fence && fenceEvent) {
            waitForGpu();
        }
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
    }
};

DxrRenderer::DxrRenderer() : impl(new Impl()) {}

DxrRenderer::~DxrRenderer() {
    shutdown();
    delete impl;
    impl = nullptr;
}

bool DxrRenderer::initialize(HWND hwnd, int width, int height) {
    try {
        impl->initialize(hwnd, width, height);
        return true;
    }
    catch (const std::exception& e) {
        impl->lastError.assign(e.what(), e.what() + strlen(e.what()));
        return false;
    }
}

void DxrRenderer::render() {
    try {
        impl->render();
    }
    catch (const std::exception& e) {
        impl->lastError.assign(e.what(), e.what() + strlen(e.what()));
    }
}

void DxrRenderer::shutdown() {
    if (impl) {
        impl->shutdown();
    }
}

const wchar_t* DxrRenderer::lastError() const {
    if (!impl || impl->lastError.empty()) {
        return L"No error";
    }
    return impl->lastError.c_str();
}