#include "D3D12Device.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/String/Utf.h"
#include "RHI_D3D12/D3D12Backend.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

namespace zx::rhi::d3d12 {

namespace {
constexpr u32 ResourceHeapCapacity = 1u << 18; // 262144 bindless SRV/UAV descriptors
constexpr u32 SamplerHeapCapacity = 2048;
constexpr u32 RtvHeapCapacity = 1024;
constexpr u32 DsvHeapCapacity = 256;
constexpr u64 UploadRingSize = 32_MB;
} // namespace

// --- DescriptorHeap -------------------------------------------------------------------------------------------------

bool DescriptorHeap::Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool shaderVisible,
                          StringView name)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = capacity;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (!ZX_DX_CHECK(device->CreateDescriptorHeap(&desc, ZX_DX_PPV(m_heap))))
        return false;
    SetDebugName(m_heap.Get(), name);
    m_cpuStart = m_heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible)
        m_gpuStart = m_heap->GetGPUDescriptorHandleForHeapStart();
    m_increment = device->GetDescriptorHandleIncrementSize(type);
    m_capacity = capacity;
    return true;
}

u32 DescriptorHeap::Allocate()
{
    ScopedLock<SpinLock> guard(m_lock);
    ++m_used;
    if (!m_freeList.Empty())
        return m_freeList.PopBackValue();
    ZX_ASSERT(m_next < m_capacity, "Descriptor heap exhausted ({} descriptors)", m_capacity);
    return m_next++;
}

void DescriptorHeap::Free(u32 index)
{
    if (index == InvalidDescriptor)
        return;
    ScopedLock<SpinLock> guard(m_lock);
    --m_used;
    m_freeList.PushBack(index);
}

// --- Device ---------------------------------------------------------------------------------------------------------

extern void (*g_deviceRemovedHook)(HRESULT);
namespace {
D3D12Device* g_liveDevice = nullptr;
}

D3D12Device::~D3D12Device()
{
    if (g_liveDevice == this)
        g_liveDevice = nullptr;
    if (!m_device)
        return;
    WaitIdle();
    for (FrameContext& f : m_frames) {
        for (Resource* r : f.pendingDeletes)
            DestroyImmediate(r);
        f.pendingDeletes.Clear();
        for (D3D12CommandList* list : f.commandLists)
            Delete(list);
        f.commandLists.Clear();
        if (f.upload.buffer)
            f.upload.buffer->Unmap(0, nullptr);
        if (f.timestampReadback)
            f.timestampReadback->Unmap(0, nullptr);
    }
    PollDebugMessages();
    if (m_fenceEvent)
        ::CloseHandle(m_fenceEvent);
    m_immediateList.Reset();
    m_immediateAllocator.Reset();
    m_infoQueue.Reset();
    for (FrameContext& f : m_frames)
        f = FrameContext();
    if (m_debugLayer) {
        ComPtr<ID3D12DebugDevice> debugDevice;
        if (SUCCEEDED(m_device.As(debugDevice))) {
            // Everything except the device itself should be gone; anything else is a leak.
            debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_SUMMARY | D3D12_RLDO_IGNORE_INTERNAL);
        }
    }
}

bool D3D12Device::Init(const DeviceDesc& desc)
{
    g_liveDevice = this;
    g_deviceRemovedHook = [](HRESULT hr) {
        if (g_liveDevice)
            g_liveDevice->OnDeviceRemoved(hr);
    };
    UINT factoryFlags = 0;
    if (desc.enableDebugLayer) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(ZX_DX_PPV(debug)))) {
            debug->EnableDebugLayer();
            m_debugLayer = true;
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            if (desc.enableGpuValidation) {
                ComPtr<ID3D12Debug1> debug1;
                if (SUCCEEDED(debug.As(debug1)))
                    debug1->SetEnableGPUBasedValidation(TRUE);
            }
        } else {
            ZX_LOG_WARN(LogD3D12, "D3D12 debug layer unavailable (install 'Graphics Tools' optional feature)");
        }
    }
    {
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
        if (SUCCEEDED(D3D12GetDebugInterface(ZX_DX_PPV(dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        }
    }

    if (!ZX_DX_CHECK(CreateDXGIFactory2(factoryFlags, ZX_DX_PPV(m_factory))))
        return false;

    if (desc.useWarp) {
        if (!ZX_DX_CHECK(m_factory->EnumWarpAdapter(ZX_DX_PPV(m_adapter))))
            return false;
    } else {
        const DXGI_GPU_PREFERENCE pref =
            desc.preferHighPerformanceAdapter ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE : DXGI_GPU_PREFERENCE_UNSPECIFIED;
        ComPtr<IDXGIAdapter1> candidate;
        for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(i, pref, ZX_DX_PPV(candidate)) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 ad;
            candidate->GetDesc1(&ad);
            if (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                continue;
            if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                m_adapter = candidate;
                break;
            }
        }
        if (!m_adapter) {
            ZX_LOG_ERROR(LogD3D12, "No D3D12 (feature level 12_0) capable GPU found");
            return false;
        }
    }
    if (!ZX_DX_CHECK(D3D12CreateDevice(m_adapter.Get(), D3D_FEATURE_LEVEL_12_0, ZX_DX_PPV(m_device))))
        return false;
    SetDebugName(m_device.Get(), "ZX Device");

    if (m_debugLayer && SUCCEEDED(m_device.As(m_infoQueue))) {
        if (platform::IsDebuggerAttached()) {
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            m_infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
        D3D12_MESSAGE_ID denied[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
        };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = UINT(ZX_ARRAY_COUNT(denied));
        filter.DenyList.pIDList = denied;
        m_infoQueue->AddStorageFilterEntries(&filter);
    }

    // --- Capabilities ---
    {
        DXGI_ADAPTER_DESC1 ad;
        m_adapter->GetDesc1(&ad);
        m_caps.adapter.name = Utf16ToUtf8(ad.Description);
        m_caps.adapter.vendorId = ad.VendorId;
        m_caps.adapter.deviceId = ad.DeviceId;
        m_caps.adapter.dedicatedVideoMemory = ad.DedicatedVideoMemory;
        m_caps.adapter.sharedSystemMemory = ad.SharedSystemMemory;
        m_caps.adapter.isSoftware = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;

        for (u32 sm = 0x69; sm >= 0x60; --sm) {
            D3D12_FEATURE_DATA_SHADER_MODEL data = {D3D_SHADER_MODEL(sm)};
            if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data)))) {
                const u32 v = u32(data.HighestShaderModel);
                m_caps.shaderModel = (v >> 4) * 10 + (v & 0xF);
                break;
            }
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
        m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o));
        m_caps.resourceBindingTier = u32(o.ResourceBindingTier);
        m_caps.rasterizerOrderedViews = o.ROVsSupported != FALSE;
        m_caps.conservativeRasterTier = u32(o.ConservativeRasterizationTier);
        m_caps.typedUavLoadAdditionalFormats = o.TypedUAVLoadAdditionalFormats != FALSE;
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1)))) {
            m_caps.waveOps = o1.WaveOps != FALSE;
            m_caps.waveLaneCountMin = o1.WaveLaneCountMin;
            m_caps.waveLaneCountMax = o1.WaveLaneCountMax;
        }
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 o4 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &o4, sizeof(o4))))
            m_caps.native16Bit = o4.Native16BitShaderOpsSupported != FALSE;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5))))
            m_caps.raytracingTier = u32(o5.RaytracingTier); // 0, 10 (1.0) or 11 (1.1)
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))))
            m_caps.variableShadingRateTier = u32(o6.VariableShadingRateTier);
        D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof(o7))))
            m_caps.meshShaders = o7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))))
            m_caps.enhancedBarriers = o12.EnhancedBarriersSupported != FALSE;
        D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch)))) {
            m_caps.uma = arch.UMA != FALSE;
            m_caps.cacheCoherentUma = arch.CacheCoherentUMA != FALSE;
        }
        m_caps.adapter.isIntegrated = m_caps.uma;
        m_caps.bindlessUav = m_caps.resourceBindingTier >= 3;
        m_caps.dynamicResources = m_caps.shaderModel >= 66 && m_caps.resourceBindingTier >= 3;
        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(m_factory.As(factory5)))
            factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
        m_caps.tearingSupported = tearing != FALSE;
    }
    ZX_LOG_INFO(LogD3D12, "GPU: {} ({} MB dedicated, {} MB shared{})", m_caps.adapter.name,
                m_caps.adapter.dedicatedVideoMemory / 1_MB, m_caps.adapter.sharedSystemMemory / 1_MB,
                m_caps.uma ? ", UMA" : "");
    ZX_LOG_INFO(LogD3D12, "Caps: SM {}.{}, binding tier {}, waves {}-{}, 16-bit {}, RT tier {}, mesh {}, "
                          "enhanced barriers {}, VRS {}, debug layer {}",
                m_caps.shaderModel / 10, m_caps.shaderModel % 10, m_caps.resourceBindingTier, m_caps.waveLaneCountMin,
                m_caps.waveLaneCountMax, m_caps.native16Bit, m_caps.raytracingTier, m_caps.meshShaders,
                m_caps.enhancedBarriers, m_caps.variableShadingRateTier, m_debugLayer);
    if (m_caps.resourceBindingTier < 2) {
        ZX_LOG_ERROR(LogD3D12, "Resource binding tier 2+ is required for bindless rendering");
        return false;
    }

    // --- Queue / fence ---
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!ZX_DX_CHECK(m_device->CreateCommandQueue(&qd, ZX_DX_PPV(m_graphicsQueue))))
        return false;
    SetDebugName(m_graphicsQueue.Get(), "Graphics Queue");
    if (!ZX_DX_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, ZX_DX_PPV(m_fence))))
        return false;
    m_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // --- Descriptor heaps ---
    if (!m_resourceHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, ResourceHeapCapacity, true,
                             "Bindless Resources") ||
        !m_samplerHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, SamplerHeapCapacity, true,
                            "Bindless Samplers") ||
        !m_rtvHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, RtvHeapCapacity, false, "RTV") ||
        !m_dsvHeap.Init(m_device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, DsvHeapCapacity, false, "DSV"))
        return false;

    if (!CreateRootSignature() || !CreateFrameResources())
        return false;

    if (!ZX_DX_CHECK(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, ZX_DX_PPV(m_immediateAllocator))) ||
        !ZX_DX_CHECK(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_immediateAllocator.Get(), nullptr,
                                                 ZX_DX_PPV(m_immediateList))))
        return false;
    m_immediateList->Close();
    SetDebugName(m_immediateList.Get(), "Immediate Upload List");
    return true;
}

bool D3D12Device::CreateRootSignature()
{
    constexpr D3D12_DESCRIPTOR_RANGE_FLAGS volatileFlags =
        D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE | D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
    SmallArray<D3D12_DESCRIPTOR_RANGE1, 10> ranges;
    // All ranges alias the whole heap from offset 0: one bindless index works for every view type.
    // space1: Texture2D, space2: Texture2DArray, space3: TextureCube, space4: Texture3D, space5: ByteAddressBuffer
    for (u32 space = 1; space <= 5; ++space)
        ranges.PushBack(D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, space, volatileFlags, 0});
    if (m_caps.bindlessUav) {
        // space6: RWTexture2D, space7: RWByteAddressBuffer, space8: RWTexture2DArray, space9: RWTexture3D
        for (u32 space = 6; space <= 9; ++space)
            ranges.PushBack(D3D12_DESCRIPTOR_RANGE1{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, UINT_MAX, 0, space, volatileFlags, 0});
    } else {
        ZX_LOG_WARN(LogD3D12, "Binding tier < 3: bindless UAVs unavailable");
    }
    D3D12_DESCRIPTOR_RANGE1 samplerRange = {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, UINT_MAX, 0, 1,
                                            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, 0};

    D3D12_ROOT_PARAMETER1 params[5] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, MaxPushConstants};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = {1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[2].Descriptor = {2, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE};
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable = {UINT(ranges.Size()), ranges.Data()};
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable = {1, &samplerRange};
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // Static samplers s0..s7 (space0). Keep in sync with Bindless.hlsli.
    auto makeSampler = [](u32 reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE address, u32 aniso = 1,
                          D3D12_COMPARISON_FUNC cmp = D3D12_COMPARISON_FUNC_NEVER) {
        D3D12_STATIC_SAMPLER_DESC s = {};
        s.Filter = filter;
        s.AddressU = s.AddressV = s.AddressW = address;
        s.MaxAnisotropy = aniso;
        s.ComparisonFunc = cmp;
        s.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
        s.MinLOD = 0.0f;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = reg;
        s.RegisterSpace = 0;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        return s;
    };
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        makeSampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        makeSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        makeSampler(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP),
        makeSampler(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP),
        makeSampler(4, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 16),
        makeSampler(5, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 16),
        makeSampler(6, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_BORDER, 1,
                    D3D12_COMPARISON_FUNC_GREATER_EQUAL), // reversed-Z shadow compare
        makeSampler(7, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_MIRROR),
    };

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = {};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = UINT(ZX_ARRAY_COUNT(params));
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = UINT(ZX_ARRAY_COUNT(samplers));
    rs.Desc_1_1.pStaticSamplers = samplers;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    if (m_caps.dynamicResources)
        rs.Desc_1_1.Flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                             D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

    ComPtr<ID3DBlob> blob, error;
    const HRESULT hr = D3D12SerializeVersionedRootSignature(&rs, blob.ReleaseAndGetAddressOf(), error.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        ZX_LOG_ERROR(LogD3D12, "Root signature serialization failed: {}",
                     error ? StringView(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize())
                           : StringView("?"));
        return false;
    }
    if (!ZX_DX_CHECK(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                   ZX_DX_PPV(m_rootSignature))))
        return false;
    SetDebugName(m_rootSignature.Get(), "Bindless Root Signature");
    return true;
}

bool D3D12Device::CreateFrameResources()
{
    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = MaxTimestampsPerFrame * FramesInFlight;
    if (!ZX_DX_CHECK(m_device->CreateQueryHeap(&qh, ZX_DX_PPV(m_timestampHeap))))
        return false;
    u64 frequency = 0;
    m_graphicsQueue->GetTimestampFrequency(&frequency);
    m_timestampPeriodMs = frequency ? 1000.0 / f64(frequency) : 0.0;

    for (u32 i = 0; i < FramesInFlight; ++i) {
        FrameContext& f = m_frames[i];
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = UploadRingSize;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!ZX_DX_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                           nullptr, ZX_DX_PPV(f.upload.buffer))))
            return false;
        SetDebugName(f.upload.buffer.Get(), String::Format("Upload Ring {}", i));
        const D3D12_RANGE noRead = {0, 0};
        f.upload.buffer->Map(0, &noRead, reinterpret_cast<void**>(&f.upload.mapped));
        f.upload.gpuBase = f.upload.buffer->GetGPUVirtualAddress();
        f.upload.capacity = UploadRingSize;

        hp.Type = D3D12_HEAP_TYPE_READBACK;
        rd.Width = MaxTimestampsPerFrame * sizeof(u64);
        if (!ZX_DX_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                           nullptr, ZX_DX_PPV(f.timestampReadback))))
            return false;
        f.timestampReadback->Map(0, nullptr, reinterpret_cast<void**>(&f.timestampData));
    }
    return true;
}

// --- Resources ------------------------------------------------------------------------------------------------------

Buffer* D3D12Device::CreateBuffer(const BufferDesc& desc, const void* initialData)
{
    ZX_ASSERT(desc.size > 0, "CreateBuffer: zero size");
    D3D12Buffer* buffer = New<D3D12Buffer>();
    buffer->m_desc = desc;
    buffer->m_debugName = desc.debugName;

    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = desc.memory == MemoryType::Upload    ? D3D12_HEAP_TYPE_UPLOAD
              : desc.memory == MemoryType::Readback ? D3D12_HEAP_TYPE_READBACK
                                                    : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = AlignUp(desc.size, u64(256));
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (HasAnyFlags(desc.usage, BufferUsage::UnorderedAccess))
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    if (desc.memory == MemoryType::Upload)
        initial = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (desc.memory == MemoryType::Readback)
        initial = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!ZX_DX_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, nullptr,
                                                       ZX_DX_PPV(buffer->resource)))) {
        Delete(buffer);
        return nullptr;
    }
    buffer->state = initial;
    SetDebugName(buffer->resource.Get(), desc.debugName);
    buffer->m_gpuAddress = buffer->resource->GetGPUVirtualAddress();
    if (desc.memory != MemoryType::GpuOnly) {
        const D3D12_RANGE noRead = {0, 0};
        buffer->resource->Map(0, desc.memory == MemoryType::Upload ? &noRead : nullptr, &buffer->m_mapped);
    }

    if (HasAnyFlags(desc.usage, BufferUsage::ShaderResource | BufferUsage::Vertex) && desc.memory != MemoryType::Readback) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc.stride > 0) {
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.Buffer.NumElements = UINT(desc.size / desc.stride);
            srv.Buffer.StructureByteStride = desc.stride;
        } else {
            srv.Format = DXGI_FORMAT_R32_TYPELESS;
            srv.Buffer.NumElements = UINT(AlignUp(desc.size, u64(4)) / 4);
            srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        }
        buffer->m_srvIndex = m_resourceHeap.Allocate();
        m_device->CreateShaderResourceView(buffer->resource.Get(), &srv, m_resourceHeap.Cpu(buffer->m_srvIndex));
    }
    if (HasAnyFlags(desc.usage, BufferUsage::UnorderedAccess)) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        if (desc.stride > 0) {
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.Buffer.NumElements = UINT(desc.size / desc.stride);
            uav.Buffer.StructureByteStride = desc.stride;
        } else {
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.Buffer.NumElements = UINT(AlignUp(desc.size, u64(4)) / 4);
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        }
        buffer->m_uavIndex = m_resourceHeap.Allocate();
        m_device->CreateUnorderedAccessView(buffer->resource.Get(), nullptr, &uav, m_resourceHeap.Cpu(buffer->m_uavIndex));
    }

    if (initialData) {
        if (buffer->m_mapped) {
            MemCopy(buffer->m_mapped, initialData, desc.size);
        } else {
            // Temporary upload buffer + copy (blocking; meant for load-time data).
            D3D12_HEAP_PROPERTIES uhp = {};
            uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC urd = rd;
            urd.Flags = D3D12_RESOURCE_FLAG_NONE;
            ComPtr<ID3D12Resource> staging;
            if (ZX_DX_CHECK(m_device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &urd,
                                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                              ZX_DX_PPV(staging)))) {
                void* mapped = nullptr;
                const D3D12_RANGE noRead = {0, 0};
                staging->Map(0, &noRead, &mapped);
                MemCopy(mapped, initialData, desc.size);
                staging->Unmap(0, nullptr);
                ID3D12Resource* dst = buffer->resource.Get();
                ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
                    list->CopyBufferRegion(dst, 0, staging.Get(), 0, desc.size);
                });
            }
        }
    }
    return buffer;
}

void D3D12Device::CreateTextureViews(D3D12Texture* t)
{
    const TextureDesc& d = t->m_desc;
    const bool isDepth = GetFormatInfo(d.format).isDepth;
    if (HasAnyFlags(d.usage, TextureUsage::ShaderResource)) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = ToDxgiSrv(d.format);
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (d.dimension) {
        case TextureDimension::Texture2D:
            if (d.sampleCount > 1) {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            } else {
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Texture2D.MipLevels = d.mipLevels;
            }
            break;
        case TextureDimension::Texture2DArray:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Texture2DArray.MipLevels = d.mipLevels;
            srv.Texture2DArray.ArraySize = d.depthOrArraySize;
            break;
        case TextureDimension::TextureCube:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            srv.TextureCube.MipLevels = d.mipLevels;
            break;
        case TextureDimension::TextureCubeArray:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
            srv.TextureCubeArray.MipLevels = d.mipLevels;
            srv.TextureCubeArray.NumCubes = d.depthOrArraySize;
            break;
        case TextureDimension::Texture3D:
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            srv.Texture3D.MipLevels = d.mipLevels;
            break;
        }
        t->m_srvIndex = m_resourceHeap.Allocate();
        m_device->CreateShaderResourceView(t->resource.Get(), &srv, m_resourceHeap.Cpu(t->m_srvIndex));
    }
    if (HasAnyFlags(d.usage, TextureUsage::UnorderedAccess)) {
        for (u32 mip = 0; mip < d.mipLevels; ++mip) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = ToDxgi(d.format);
            switch (d.dimension) {
            case TextureDimension::Texture2D:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                uav.Texture2D.MipSlice = mip;
                break;
            case TextureDimension::Texture3D:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                uav.Texture3D.MipSlice = mip;
                uav.Texture3D.WSize = UINT(-1);
                break;
            default:
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
                uav.Texture2DArray.MipSlice = mip;
                uav.Texture2DArray.ArraySize =
                    d.dimension == TextureDimension::TextureCube || d.dimension == TextureDimension::TextureCubeArray
                        ? d.depthOrArraySize * 6
                        : d.depthOrArraySize;
                break;
            }
            const u32 index = m_resourceHeap.Allocate();
            m_device->CreateUnorderedAccessView(t->resource.Get(), nullptr, &uav, m_resourceHeap.Cpu(index));
            t->m_uavIndices.PushBack(index);
        }
    }
    if (HasAnyFlags(d.usage, TextureUsage::RenderTarget))
        t->rtv = GetRtv(t, 0, 0);
    if (HasAnyFlags(d.usage, TextureUsage::DepthStencil) && isDepth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = ToDxgi(d.format);
        dsv.ViewDimension = d.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
        t->dsv = m_dsvHeap.Allocate();
        m_device->CreateDepthStencilView(t->resource.Get(), &dsv, m_dsvHeap.Cpu(t->dsv));
        dsv.Flags = D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (GetFormatInfo(d.format).hasStencil)
            dsv.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        t->dsvReadOnly = m_dsvHeap.Allocate();
        m_device->CreateDepthStencilView(t->resource.Get(), &dsv, m_dsvHeap.Cpu(t->dsvReadOnly));
    }
}

u32 D3D12Device::GetRtv(D3D12Texture* t, u32 mip, u32 slice)
{
    if (mip == 0 && slice == 0 && t->rtv != InvalidDescriptor)
        return t->rtv;
    const u32 key = mip | (slice << 16);
    if (key != 0) {
        if (const u32* existing = t->extraRtvs.Find(key))
            return *existing;
    }
    const TextureDesc& d = t->m_desc;
    D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
    rtv.Format = ToDxgi(d.format);
    if (d.dimension == TextureDimension::Texture2D) {
        rtv.ViewDimension = d.sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        rtv.Texture2D.MipSlice = mip;
    } else if (d.dimension == TextureDimension::Texture3D) {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rtv.Texture3D.MipSlice = mip;
        rtv.Texture3D.FirstWSlice = slice;
        rtv.Texture3D.WSize = 1;
    } else {
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray.MipSlice = mip;
        rtv.Texture2DArray.FirstArraySlice = slice;
        rtv.Texture2DArray.ArraySize = 1;
    }
    const u32 index = m_rtvHeap.Allocate();
    m_device->CreateRenderTargetView(t->resource.Get(), &rtv, m_rtvHeap.Cpu(index));
    if (key == 0)
        t->rtv = index;
    else
        t->extraRtvs.Insert(key, index);
    return index;
}

Texture* D3D12Device::CreateTexture(const TextureDesc& desc, Span<const SubresourceData> initialData)
{
    D3D12Texture* t = New<D3D12Texture>();
    t->m_desc = desc;
    t->m_debugName = desc.debugName;
    const FormatInfo& fi = GetFormatInfo(desc.format);

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = desc.dimension == TextureDimension::Texture3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                 : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = desc.width;
    rd.Height = desc.height;
    const bool cube = desc.dimension == TextureDimension::TextureCube || desc.dimension == TextureDimension::TextureCubeArray;
    rd.DepthOrArraySize = u16(cube ? desc.depthOrArraySize * 6 : desc.depthOrArraySize);
    rd.MipLevels = u16(desc.mipLevels);
    rd.Format = (fi.isDepth && HasAnyFlags(desc.usage, TextureUsage::ShaderResource)) ? ToDxgiTypeless(desc.format)
                                                                                      : ToDxgi(desc.format);
    rd.SampleDesc.Count = desc.sampleCount;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (HasAnyFlags(desc.usage, TextureUsage::RenderTarget))
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (HasAnyFlags(desc.usage, TextureUsage::DepthStencil)) {
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        if (!HasAnyFlags(desc.usage, TextureUsage::ShaderResource))
            rd.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    }
    if (HasAnyFlags(desc.usage, TextureUsage::UnorderedAccess))
        rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_CLEAR_VALUE clear = {};
    D3D12_CLEAR_VALUE* clearPtr = nullptr;
    D3D12_RESOURCE_STATES initial = D3D12_RESOURCE_STATE_COMMON;
    if (HasAnyFlags(desc.usage, TextureUsage::RenderTarget)) {
        clear.Format = ToDxgi(desc.format);
        clear.Color[0] = desc.clearValue.color.r;
        clear.Color[1] = desc.clearValue.color.g;
        clear.Color[2] = desc.clearValue.color.b;
        clear.Color[3] = desc.clearValue.color.a;
        clearPtr = &clear;
        initial = D3D12_RESOURCE_STATE_RENDER_TARGET;
    } else if (HasAnyFlags(desc.usage, TextureUsage::DepthStencil)) {
        clear.Format = ToDxgi(desc.format);
        clear.DepthStencil.Depth = desc.clearValue.depth;
        clear.DepthStencil.Stencil = desc.clearValue.stencil;
        clearPtr = &clear;
        initial = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (!ZX_DX_CHECK(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, initial, clearPtr,
                                                       ZX_DX_PPV(t->resource)))) {
        Delete(t);
        return nullptr;
    }
    t->state = initial;
    SetDebugName(t->resource.Get(), desc.debugName);
    CreateTextureViews(t);

    if (!initialData.Empty()) {
        const u32 subresourceCount = u32(rd.MipLevels) * (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : rd.DepthOrArraySize);
        const u32 count = Min<u32>(subresourceCount, u32(initialData.Size()));
        Array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts;
        layouts.ResizeUninitialized(count);
        Array<UINT> rows;
        rows.ResizeUninitialized(count);
        Array<UINT64> rowSizes;
        rowSizes.ResizeUninitialized(count);
        u64 totalSize = 0;
        m_device->GetCopyableFootprints(&rd, 0, count, 0, layouts.Data(), rows.Data(), rowSizes.Data(), &totalSize);

        D3D12_HEAP_PROPERTIES uhp = {};
        uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC urd = {};
        urd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        urd.Width = totalSize;
        urd.Height = 1;
        urd.DepthOrArraySize = 1;
        urd.MipLevels = 1;
        urd.SampleDesc.Count = 1;
        urd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> staging;
        if (ZX_DX_CHECK(m_device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &urd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr, ZX_DX_PPV(staging)))) {
            u8* mapped = nullptr;
            const D3D12_RANGE noRead = {0, 0};
            staging->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
            for (u32 s = 0; s < count; ++s) {
                const SubresourceData& src = initialData[s];
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& l = layouts[s];
                const u64 srcRowPitch = src.rowPitch ? src.rowPitch : rowSizes[s];
                const u64 srcSlicePitch = src.slicePitch ? src.slicePitch : srcRowPitch * rows[s];
                for (u32 z = 0; z < l.Footprint.Depth; ++z)
                    for (u32 y = 0; y < rows[s]; ++y)
                        MemCopy(mapped + l.Offset + u64(z) * l.Footprint.RowPitch * rows[s] + u64(y) * l.Footprint.RowPitch,
                                static_cast<const u8*>(src.data) + z * srcSlicePitch + y * srcRowPitch, rowSizes[s]);
            }
            staging->Unmap(0, nullptr);
            ID3D12Resource* dst = t->resource.Get();
            const D3D12_RESOURCE_STATES before = t->state;
            ExecuteImmediate([&](ID3D12GraphicsCommandList* list) {
                // Explicit transitions both ways: a texture written as COPY_DEST does not decay back to COMMON.
                if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
                    D3D12_RESOURCE_BARRIER b = {};
                    b.Transition.pResource = dst;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = before;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                    list->ResourceBarrier(1, &b);
                }
                for (u32 s = 0; s < count; ++s) {
                    D3D12_TEXTURE_COPY_LOCATION dl = {};
                    dl.pResource = dst;
                    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    dl.SubresourceIndex = s;
                    D3D12_TEXTURE_COPY_LOCATION sl = {};
                    sl.pResource = staging.Get();
                    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    sl.PlacedFootprint = layouts[s];
                    list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
                }
                if (before != D3D12_RESOURCE_STATE_COPY_DEST) {
                    D3D12_RESOURCE_BARRIER b = {};
                    b.Transition.pResource = dst;
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                    b.Transition.StateAfter = before;
                    list->ResourceBarrier(1, &b);
                }
            });
        }
    }
    return t;
}

D3D12Texture* D3D12Device::WrapTexture(ID3D12Resource* resource, Format format, StringView name)
{
    D3D12Texture* t = New<D3D12Texture>();
    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    t->m_desc.width = u32(rd.Width);
    t->m_desc.height = rd.Height;
    t->m_desc.format = format;
    t->m_desc.usage = TextureUsage::RenderTarget;
    t->m_debugName = name;
    resource->AddRef();
    *t->resource.ReleaseAndGetAddressOf() = resource;
    t->state = D3D12_RESOURCE_STATE_PRESENT;
    SetDebugName(resource, name);
    t->rtv = GetRtv(t, 0, 0);
    return t;
}

Pipeline* D3D12Device::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc)
{
    ZX_ASSERT(desc.vertexShader, "Graphics pipeline needs a vertex shader");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSignature.Get();
    pd.VS = {desc.vertexShader->code.Data(), desc.vertexShader->code.Size()};
    if (desc.pixelShader)
        pd.PS = {desc.pixelShader->code.Data(), desc.pixelShader->code.Size()};
    pd.BlendState.IndependentBlendEnable = TRUE;
    for (u32 i = 0; i < MaxRenderTargets; ++i) {
        const BlendTarget& b = desc.blend[i];
        D3D12_RENDER_TARGET_BLEND_DESC& rt = pd.BlendState.RenderTarget[i];
        rt.BlendEnable = b.enable;
        rt.SrcBlend = ToD3D12(b.srcColor);
        rt.DestBlend = ToD3D12(b.dstColor);
        rt.BlendOp = ToD3D12(b.colorOp);
        rt.SrcBlendAlpha = ToD3D12(b.srcAlpha);
        rt.DestBlendAlpha = ToD3D12(b.dstAlpha);
        rt.BlendOpAlpha = ToD3D12(b.alphaOp);
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = b.writeMask;
    }
    pd.SampleMask = UINT_MAX;
    const RasterizerState& r = desc.rasterizer;
    pd.RasterizerState.FillMode = r.fillMode == FillMode::Wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = r.cullMode == CullMode::None    ? D3D12_CULL_MODE_NONE
                                  : r.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                                  : D3D12_CULL_MODE_BACK;
    pd.RasterizerState.FrontCounterClockwise = r.frontCounterClockwise;
    pd.RasterizerState.DepthBias = r.depthBias;
    pd.RasterizerState.DepthBiasClamp = r.depthBiasClamp;
    pd.RasterizerState.SlopeScaledDepthBias = r.slopeScaledDepthBias;
    pd.RasterizerState.DepthClipEnable = r.depthClip;
    pd.RasterizerState.ConservativeRaster =
        r.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    const DepthStencilState& ds = desc.depthStencil;
    pd.DepthStencilState.DepthEnable = ds.depthTest && desc.depthFormat != Format::Unknown;
    pd.DepthStencilState.DepthWriteMask = ds.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    pd.DepthStencilState.DepthFunc = ToD3D12(ds.depthCompare);
    pd.DepthStencilState.StencilEnable = ds.stencil;
    pd.PrimitiveTopologyType = ToD3D12TopologyType(desc.topology);
    pd.NumRenderTargets = desc.colorCount;
    for (u32 i = 0; i < desc.colorCount; ++i)
        pd.RTVFormats[i] = ToDxgi(desc.colorFormats[i]);
    pd.DSVFormat = ToDxgi(desc.depthFormat);
    pd.SampleDesc.Count = desc.sampleCount;

    D3D12Pipeline* p = New<D3D12Pipeline>();
    p->m_debugName = desc.debugName;
    p->topology = ToD3D12(desc.topology);
    if (!ZX_DX_CHECK(m_device->CreateGraphicsPipelineState(&pd, ZX_DX_PPV(p->pso)))) {
        Delete(p);
        PollDebugMessages();
        return nullptr;
    }
    SetDebugName(p->pso.Get(), desc.debugName);
    return p;
}

Pipeline* D3D12Device::CreateComputePipeline(const ComputePipelineDesc& desc)
{
    ZX_ASSERT(desc.computeShader, "Compute pipeline needs a shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSignature.Get();
    pd.CS = {desc.computeShader->code.Data(), desc.computeShader->code.Size()};
    D3D12Pipeline* p = New<D3D12Pipeline>();
    p->m_debugName = desc.debugName;
    p->m_isCompute = true;
    if (!ZX_DX_CHECK(m_device->CreateComputePipelineState(&pd, ZX_DX_PPV(p->pso)))) {
        Delete(p);
        PollDebugMessages();
        return nullptr;
    }
    SetDebugName(p->pso.Get(), desc.debugName);
    return p;
}

u32 D3D12Device::CreateSampler(const SamplerDesc& desc)
{
    D3D12_SAMPLER_DESC sd = {};
    sd.Filter = ToD3D12Filter(desc);
    sd.AddressU = ToD3D12(desc.addressU);
    sd.AddressV = ToD3D12(desc.addressV);
    sd.AddressW = ToD3D12(desc.addressW);
    sd.MipLODBias = desc.mipLodBias;
    sd.MaxAnisotropy = Clamp(desc.maxAnisotropy, 1u, 16u);
    sd.ComparisonFunc = desc.comparison ? ToD3D12(desc.compareOp) : D3D12_COMPARISON_FUNC_NEVER;
    sd.BorderColor[0] = desc.borderColor.r;
    sd.BorderColor[1] = desc.borderColor.g;
    sd.BorderColor[2] = desc.borderColor.b;
    sd.BorderColor[3] = desc.borderColor.a;
    sd.MinLOD = desc.minLod;
    sd.MaxLOD = desc.maxLod;
    const u32 index = m_samplerHeap.Allocate();
    m_device->CreateSampler(&sd, m_samplerHeap.Cpu(index));
    return index;
}

UniquePtr<SwapChain> D3D12Device::CreateSwapChain(const SwapChainDesc& desc)
{
    auto sc = MakeUnique<D3D12SwapChain>(*this);
    if (!sc->Init(desc))
        return nullptr;
    return UniquePtr<SwapChain>(sc.Release());
}

void D3D12Device::Destroy(Resource* resource)
{
    if (!resource)
        return;
    ScopedLock<Mutex> guard(m_deleteMutex);
    CurrentFrame().pendingDeletes.PushBack(resource);
}

void D3D12Device::DestroyImmediate(Resource* resource)
{
    switch (resource->GetType()) {
    case ResourceType::Buffer: {
        auto* b = static_cast<D3D12Buffer*>(resource);
        m_resourceHeap.Free(b->m_srvIndex);
        m_resourceHeap.Free(b->m_uavIndex);
        if (b->m_mapped)
            b->resource->Unmap(0, nullptr);
        Delete(b);
        break;
    }
    case ResourceType::Texture: {
        auto* t = static_cast<D3D12Texture*>(resource);
        m_resourceHeap.Free(t->m_srvIndex);
        for (u32 uav : t->m_uavIndices)
            m_resourceHeap.Free(uav);
        m_rtvHeap.Free(t->rtv);
        for (auto& [key, index] : t->extraRtvs)
            m_rtvHeap.Free(index);
        m_dsvHeap.Free(t->dsv);
        m_dsvHeap.Free(t->dsvReadOnly);
        Delete(t);
        break;
    }
    case ResourceType::Pipeline:
        Delete(static_cast<D3D12Pipeline*>(resource));
        break;
    }
}

// --- Frames ---------------------------------------------------------------------------------------------------------

void D3D12Device::WaitForFence(u64 value)
{
    if (value == 0 || m_fence->GetCompletedValue() >= value)
        return;
    m_fence->SetEventOnCompletion(value, m_fenceEvent);
    ::WaitForSingleObject(m_fenceEvent, INFINITE);
}

void D3D12Device::WaitIdle()
{
    const u64 value = m_nextFenceValue++;
    m_graphicsQueue->Signal(m_fence.Get(), value);
    WaitForFence(value);
}

void D3D12Device::BeginFrame()
{
    ZX_ASSERT(!m_frameActive, "BeginFrame called twice without EndFrame");
    FrameContext& f = CurrentFrame();
    WaitForFence(f.fenceValue);

    // GPU timings of the frame that previously used this slot.
    if (!f.timers.Empty() && f.timestampData) {
        m_timings.Clear();
        for (const TimerRecord& t : f.timers) {
            const u64 begin = f.timestampData[t.beginQuery];
            const u64 end = f.timestampData[t.endQuery];
            m_timings.PushBack(GpuTiming{t.name, end > begin ? f64(end - begin) * m_timestampPeriodMs : 0.0});
        }
    }
    f.timers.Clear();
    f.queryCount = 0;
    {
        ScopedLock<Mutex> guard(m_deleteMutex);
        for (Resource* r : f.pendingDeletes)
            DestroyImmediate(r);
        f.pendingDeletes.Clear();
    }
    f.upload.offset = 0;
    f.upload.overflow.Clear();
    f.commandListsUsed = 0;
    m_frameActive = true;
}

CommandList* D3D12Device::BeginCommandList(QueueType queue)
{
    ZX_ASSERT(m_frameActive, "BeginCommandList outside BeginFrame/EndFrame");
    ZX_ASSERT(queue == QueueType::Graphics, "Only the graphics queue is implemented");
    FrameContext& f = CurrentFrame();
    if (f.commandListsUsed == f.commandLists.Size()) {
        D3D12CommandList* list = New<D3D12CommandList>(*this, queue);
        if (!list->Init()) {
            Delete(list);
            return nullptr;
        }
        f.commandLists.PushBack(list);
    }
    D3D12CommandList* list = f.commandLists[f.commandListsUsed++];
    list->Begin(f);
    return list;
}

void D3D12Device::Submit(CommandList* commandList)
{
    auto* list = static_cast<D3D12CommandList*>(commandList);
    FrameContext& f = CurrentFrame();
    if (f.queryCount > 0) {
        const u32 base = u32(m_frameIndex % FramesInFlight) * MaxTimestampsPerFrame;
        list->Native()->ResolveQueryData(m_timestampHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, base, f.queryCount,
                                         f.timestampReadback.Get(), 0);
    }
    list->Close();
    ID3D12CommandList* lists[] = {list->Native()};
    m_graphicsQueue->ExecuteCommandLists(1, lists);
    PollDebugMessages();
}

void D3D12Device::EndFrame()
{
    ZX_ASSERT(m_frameActive, "EndFrame without BeginFrame");
    FrameContext& f = CurrentFrame();
    f.fenceValue = m_nextFenceValue++;
    m_graphicsQueue->Signal(m_fence.Get(), f.fenceValue);
    ++m_frameIndex;
    m_frameActive = false;
    PollDebugMessages();
}

void D3D12Device::ExecuteImmediate(const Function<void(ID3D12GraphicsCommandList*)>& record)
{
    ScopedLock<Mutex> guard(m_immediateMutex);
    m_immediateAllocator->Reset();
    m_immediateList->Reset(m_immediateAllocator.Get(), nullptr);
    record(m_immediateList.Get());
    m_immediateList->Close();
    ID3D12CommandList* lists[] = {m_immediateList.Get()};
    m_graphicsQueue->ExecuteCommandLists(1, lists);
    WaitIdle();
}

void D3D12Device::PollDebugMessages()
{
    if (!m_infoQueue)
        return;
    const u64 count = m_infoQueue->GetNumStoredMessages();
    for (u64 i = 0; i < count; ++i) {
        SIZE_T length = 0;
        m_infoQueue->GetMessage(i, nullptr, &length);
        Array<u8> storage;
        storage.ResizeUninitialized(length);
        auto* msg = reinterpret_cast<D3D12_MESSAGE*>(storage.Data());
        if (FAILED(m_infoQueue->GetMessage(i, msg, &length)))
            continue;
        const StringView text(msg->pDescription, msg->DescriptionByteLength > 0 ? msg->DescriptionByteLength - 1 : 0);
        switch (msg->Severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION:
        case D3D12_MESSAGE_SEVERITY_ERROR:
            ++m_debugErrors;
            ZX_LOG_ERROR(LogD3D12, "[DebugLayer] {}", text);
            break;
        case D3D12_MESSAGE_SEVERITY_WARNING:
            ZX_LOG_WARN(LogD3D12, "[DebugLayer] {}", text);
            break;
        default:
            break;
        }
    }
    m_infoQueue->ClearStoredMessages();
}

void D3D12Device::OnDeviceRemoved(HRESULT reason)
{
    if (m_deviceLost)
        return;
    m_deviceLost = true;
    const HRESULT removed = m_device->GetDeviceRemovedReason();
    ZX_LOG_ERROR(LogD3D12, "GPU device lost: present HRESULT {:#010x}, removed reason {:#010x}", u32(reason), u32(removed));
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (SUCCEEDED(m_device.As(dred))) {
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
                const u32 completed = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                ZX_LOG_ERROR(LogD3D12, "  DRED: list '{}' completed {}/{} ops", node->pCommandListDebugNameA
                                                                                    ? node->pCommandListDebugNameA
                                                                                    : "?",
                             completed, node->BreadcrumbCount);
                // The ops around the one that did not finish (D3D12_AUTO_BREADCRUMB_OP values; 20 = Dispatch, 0/1 = draws).
                if (completed < node->BreadcrumbCount && node->pCommandHistory) {
                    String ops;
                    for (u32 k = completed >= 4 ? completed - 4 : 0; k < Min(completed + 3, node->BreadcrumbCount); ++k)
                        ops += String::Format("{}{}{} ", k == completed ? "[" : "", u32(node->pCommandHistory[k]), k == completed ? "]" : "");
                    ZX_LOG_ERROR(LogD3D12, "  DRED: ops {}", ops);
                }
            }
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)) && pageFault.PageFaultVA)
            ZX_LOG_ERROR(LogD3D12, "  DRED: page fault at GPU VA {:#x}", u64(pageFault.PageFaultVA));
        for (const D3D12_DRED_ALLOCATION_NODE* a = pageFault.pHeadExistingAllocationNode; a; a = a->pNext)
            ZX_LOG_ERROR(LogD3D12, "  DRED: fault near live allocation '{}'", a->ObjectNameA ? a->ObjectNameA : "?");
        for (const D3D12_DRED_ALLOCATION_NODE* a = pageFault.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            ZX_LOG_ERROR(LogD3D12, "  DRED: fault near freed allocation '{}'", a->ObjectNameA ? a->ObjectNameA : "?");
    }
}

// --- Public factory -------------------------------------------------------------------------------------------------

UniquePtr<Device> CreateD3D12Device(const DeviceDesc& desc)
{
    auto device = MakeUnique<D3D12Device>();
    if (!device->Init(desc))
        return nullptr;
    return UniquePtr<Device>(device.Release());
}

u32 GetDebugLayerErrorCount(Device& device) { return static_cast<D3D12Device&>(device).DebugErrorCount(); }

} // namespace zx::rhi::d3d12
