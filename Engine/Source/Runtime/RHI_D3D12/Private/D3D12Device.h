#pragma once

#include "Core/Containers/HashMap.h"
#include "Core/Threading/Mutex.h"
#include "D3D12Common.h"

namespace zx::rhi::d3d12 {

class D3D12Device;

/// Fixed-capacity descriptor heap with free-list index allocation. Thread-safe.
class DescriptorHeap
{
public:
    bool Init(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, u32 capacity, bool shaderVisible, StringView name);
    u32 Allocate();
    void Free(u32 index);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(u32 index) const { return {m_cpuStart.ptr + SIZE_T(index) * m_increment}; }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(u32 index) const { return {m_gpuStart.ptr + UINT64(index) * m_increment}; }
    ID3D12DescriptorHeap* Heap() const { return m_heap.Get(); }
    u32 Capacity() const { return m_capacity; }
    u32 Used() const { return m_used; }

private:
    ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_CPU_DESCRIPTOR_HANDLE m_cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_gpuStart = {};
    u32 m_increment = 0;
    u32 m_capacity = 0;
    u32 m_next = 0;
    u32 m_used = 0;
    Array<u32> m_freeList;
    SpinLock m_lock;
};

class D3D12Buffer final : public Buffer
{
public:
    friend class D3D12Device;
    friend class D3D12CommandList;
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
};

class D3D12Texture final : public Texture
{
public:
    friend class D3D12Device;
    friend class D3D12CommandList;
    friend class D3D12SwapChain;
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    u32 rtv = InvalidDescriptor;         // mip 0 / slice 0
    u32 dsv = InvalidDescriptor;
    u32 dsvReadOnly = InvalidDescriptor;
    /// Extra RTVs for other mips/slices, created on demand. Key = mip | slice << 16.
    HashMap<u32, u32> extraRtvs;
};

class D3D12Pipeline final : public Pipeline
{
public:
    friend class D3D12Device;
    friend class D3D12CommandList;
    ComPtr<ID3D12PipelineState> pso;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
};

/// Linear allocator over a persistently mapped upload buffer, reset each time its frame comes around.
struct UploadRing
{
    ComPtr<ID3D12Resource> buffer;
    u8* mapped = nullptr;
    u64 gpuBase = 0;
    u64 capacity = 0;
    u64 offset = 0;
    /// Oversized allocations get dedicated buffers released when the frame is recycled.
    Array<ComPtr<ID3D12Resource>> overflow;
};

struct TimerRecord
{
    const char* name;
    u32 beginQuery;
    u32 endQuery;
};

class D3D12CommandList;

struct FrameContext
{
    u64 fenceValue = 0;
    UploadRing upload;
    Array<D3D12CommandList*> commandLists; // pooled, reused when the frame recycles
    u32 commandListsUsed = 0;
    Array<Resource*> pendingDeletes;
    Array<TimerRecord> timers;
    u32 queryCount = 0;
    ComPtr<ID3D12Resource> timestampReadback;
    u64* timestampData = nullptr;
};

class D3D12CommandList final : public CommandList
{
public:
    D3D12CommandList(D3D12Device& device, QueueType type);
    ~D3D12CommandList() override = default;

    bool Init();
    void Begin(FrameContext& frame);
    void Close();
    ID3D12GraphicsCommandList* Native() const { return m_list.Get(); }

    QueueType GetQueueType() const override { return m_type; }
    void Barrier(Texture* texture, ResourceState state) override;
    void Barrier(Buffer* buffer, ResourceState state) override;
    void UavBarrier(Resource* resource) override;
    void BeginRenderPass(Span<const ColorAttachment> colors, const DepthAttachment* depth) override;
    void EndRenderPass() override;
    void SetViewport(const Viewport& viewport) override;
    void SetScissor(i32 x, i32 y, u32 width, u32 height) override;
    void SetPipeline(Pipeline* pipeline) override;
    void SetPushConstants(const void* data, u32 size, u32 offset) override;
    void SetConstantBuffer(u32 slot, u64 gpuAddress) override;
    void SetConstants(u32 slot, const void* data, u32 size) override;
    void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset) override;
    void Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance) override;
    void DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance) override;
    void Dispatch(u32 x, u32 y, u32 z) override;
    void DrawIndexedIndirect(Buffer* args, u64 offset, u32 maxCount) override;
    TransientAllocation AllocateTransient(u64 size, u64 alignment) override;
    void CopyBuffer(Buffer* dst, u64 dstOffset, Buffer* src, u64 srcOffset, u64 size) override;
    void UploadBuffer(Buffer* dst, u64 dstOffset, const void* data, u64 size) override;
    void UploadTexture(Texture* dst, u32 mip, u32 arraySlice, const SubresourceData& data) override;
    void UploadTextureRegion(Texture* dst, u32 mip, u32 arraySlice, u32 x, u32 y, u32 width, u32 height,
                             const SubresourceData& data) override;
    u64 CopyTextureToBuffer(Buffer* dst, u64 dstOffset, Texture* src, u32 mip, u32 arraySlice) override;
    void BeginEvent(StringView name) override;
    void EndEvent() override;
    void BeginTimer(const char* name) override;
    void EndTimer() override;

private:
    void Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES target);
    void FlushBarriers();

    D3D12Device& m_device;
    QueueType m_type;
    ComPtr<ID3D12CommandAllocator> m_allocator;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    FrameContext* m_frame = nullptr;
    SmallArray<D3D12_RESOURCE_BARRIER, 16> m_pendingBarriers;
    D3D12Pipeline* m_pipeline = nullptr;
    bool m_isComputeBound = false;
    SmallArray<Pair<const char*, u32>, 8> m_timerStack;
};

class D3D12SwapChain final : public SwapChain
{
public:
    D3D12SwapChain(D3D12Device& device) : m_device(device) {}
    ~D3D12SwapChain() override;
    bool Init(const SwapChainDesc& desc);

    Texture* GetCurrentBackBuffer() override;
    UVec2 GetSize() const override { return UVec2(m_width, m_height); }
    Format GetFormat() const override { return m_format; }
    void Resize(u32 width, u32 height) override;
    void Present() override;
    void SetVSync(bool enabled) override { m_vsync = enabled; }
    bool GetVSync() const override { return m_vsync; }

private:
    void CreateBackBuffers();
    void ReleaseBackBuffers();

    D3D12Device& m_device;
    ComPtr<IDXGISwapChain3> m_swapChain;
    HWND m_hwnd = nullptr;
    Array<D3D12Texture*> m_backBuffers;
    u32 m_width = 0;
    u32 m_height = 0;
    u32 m_bufferCount = 3;
    Format m_format = Format::R8G8B8A8_UNORM;
    bool m_vsync = true;
    bool m_tearing = false;
};

class D3D12Device final : public Device
{
public:
    D3D12Device() = default;
    ~D3D12Device() override;
    bool Init(const DeviceDesc& desc);

    const DeviceCaps& GetCaps() const override { return m_caps; }
    StringView GetBackendName() const override { return "D3D12"; }
    Buffer* CreateBuffer(const BufferDesc& desc, const void* initialData) override;
    Texture* CreateTexture(const TextureDesc& desc, Span<const SubresourceData> initialData) override;
    Pipeline* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    Pipeline* CreateComputePipeline(const ComputePipelineDesc& desc) override;
    UniquePtr<SwapChain> CreateSwapChain(const SwapChainDesc& desc) override;
    u32 CreateSampler(const SamplerDesc& desc) override;
    void Destroy(Resource* resource) override;
    void BeginFrame() override;
    CommandList* BeginCommandList(QueueType queue) override;
    void Submit(CommandList* list) override;
    void EndFrame() override;
    void WaitIdle() override;
    u64 GetFrameIndex() const override { return m_frameIndex; }
    Span<const GpuTiming> GetGpuTimings() const override { return m_timings.View(); }
    bool IsDeviceLost() const override { return m_deviceLost; }

    // --- Backend internals ---
    ID3D12Device* Native() const { return m_device.Get(); }
    IDXGIFactory4* Factory() const { return m_factory.Get(); }
    ID3D12CommandQueue* GraphicsQueue() const { return m_graphicsQueue.Get(); }
    ID3D12RootSignature* RootSignature() const { return m_rootSignature.Get(); }
    ID3D12CommandSignature* DrawIndirectSignature() const { return m_drawIndirectSignature.Get(); }
    DescriptorHeap& ResourceHeap() { return m_resourceHeap; }
    DescriptorHeap& SamplerHeap() { return m_samplerHeap; }
    DescriptorHeap& RtvHeap() { return m_rtvHeap; }
    DescriptorHeap& DsvHeap() { return m_dsvHeap; }
    ID3D12QueryHeap* TimestampHeap() const { return m_timestampHeap.Get(); }
    FrameContext& CurrentFrame() { return m_frames[m_frameIndex % FramesInFlight]; }
    u32 GetRtv(D3D12Texture* texture, u32 mip, u32 slice);
    void OnDeviceRemoved(HRESULT reason);
    /// Wraps an existing resource (swapchain back buffer) into a texture object.
    D3D12Texture* WrapTexture(ID3D12Resource* resource, Format format, StringView name);
    void DestroyImmediate(Resource* resource);
    void PollDebugMessages();
    u32 DebugErrorCount() const { return m_debugErrors; }

    static constexpr u32 MaxTimestampsPerFrame = 256;

private:
    bool CreateRootSignature();
    bool CreateFrameResources();
    void WaitForFence(u64 value);
    /// Records commands with `record` on a temporary list, executes it and waits (used for initial uploads).
    void ExecuteImmediate(const Function<void(ID3D12GraphicsCommandList*)>& record);
    void CreateTextureViews(D3D12Texture* texture);

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter1> m_adapter;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12InfoQueue> m_infoQueue;
    ComPtr<ID3D12CommandQueue> m_graphicsQueue;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    u64 m_nextFenceValue = 1;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12CommandSignature> m_drawIndirectSignature; // root constant (draw id) + DrawIndexed
    DescriptorHeap m_resourceHeap;
    DescriptorHeap m_samplerHeap;
    DescriptorHeap m_rtvHeap;
    DescriptorHeap m_dsvHeap;
    ComPtr<ID3D12QueryHeap> m_timestampHeap;
    f64 m_timestampPeriodMs = 0.0;
    FrameContext m_frames[FramesInFlight];
    u64 m_frameIndex = 0;
    bool m_frameActive = false;
    Mutex m_deleteMutex;
    Array<GpuTiming> m_timings;
    DeviceCaps m_caps;
    bool m_deviceLost = false;
    bool m_debugLayer = false;
    u32 m_debugErrors = 0;
    Mutex m_immediateMutex;
    ComPtr<ID3D12CommandAllocator> m_immediateAllocator;
    ComPtr<ID3D12GraphicsCommandList> m_immediateList;
};

} // namespace zx::rhi::d3d12
