#pragma once

// Render Hardware Interface.
//
// Model:
//  - Resources are created and destroyed through Device. Destroy() is deferred until the GPU is done with it.
//  - Bindless: every SRV/UAV gets an index into one big descriptor heap (Buffer::srvIndex, Texture::srvIndex...).
//    Shaders fetch resources by index (see Engine/Shaders/Common/Bindless.hlsli). One global root signature:
//      b0       : 32 root constants (SetPushConstants)
//      b1, b2   : root constant buffers (SetConstantBuffer slots 0/1, usually per-pass / per-view data)
//      bindless : SRV/UAV tables over the whole heap, static samplers s0..s7
//  - Vertex data is fetched from buffers in the vertex shader (no input layouts).
//  - Frames: BeginFrame() waits for the GPU to finish frame N - FramesInFlight, then recycles its memory.
//
// Thread-safety: Device creation/destroy functions are thread-safe. A CommandList is recorded by one thread.

#include "Core/Containers/Function.h"
#include "Core/Math/Vector.h"
#include "Core/Memory/Pointers.h"
#include "RHI/RHITypes.h"

namespace zx::rhi {

inline constexpr u32 FramesInFlight = 2;

enum class ResourceType : u8
{
    Buffer,
    Texture,
    Pipeline
};

class Resource
{
public:
    virtual ~Resource() = default;
    ResourceType GetType() const { return m_type; }
    const String& GetDebugName() const { return m_debugName; }

protected:
    explicit Resource(ResourceType type) : m_type(type) {}
    ResourceType m_type;
    String m_debugName;
};

class Buffer : public Resource
{
public:
    const BufferDesc& GetDesc() const { return m_desc; }
    u64 GetSize() const { return m_desc.size; }
    /// GPU virtual address (for root constant buffers / index buffers).
    u64 GetGpuAddress() const { return m_gpuAddress; }
    /// Bindless index of the SRV (raw or structured), InvalidDescriptor if not a shader resource.
    u32 GetSrvIndex() const { return m_srvIndex; }
    u32 GetUavIndex() const { return m_uavIndex; }
    /// Persistently mapped pointer for Upload/Readback buffers (null for GpuOnly).
    void* GetMappedData() const { return m_mapped; }

protected:
    Buffer() : Resource(ResourceType::Buffer) {}
    BufferDesc m_desc;
    u64 m_gpuAddress = 0;
    u32 m_srvIndex = InvalidDescriptor;
    u32 m_uavIndex = InvalidDescriptor;
    void* m_mapped = nullptr;
};

class Texture : public Resource
{
public:
    const TextureDesc& GetDesc() const { return m_desc; }
    UVec2 GetSize() const { return UVec2(m_desc.width, m_desc.height); }
    Format GetFormat() const { return m_desc.format; }
    /// Bindless SRV index covering all mips/layers.
    u32 GetSrvIndex() const { return m_srvIndex; }
    /// Bindless UAV index for a mip (InvalidDescriptor if not created with UnorderedAccess).
    u32 GetUavIndex(u32 mip = 0) const { return mip < m_uavIndices.Size() ? m_uavIndices[mip] : InvalidDescriptor; }

protected:
    Texture() : Resource(ResourceType::Texture) {}
    TextureDesc m_desc;
    u32 m_srvIndex = InvalidDescriptor;
    SmallArray<u32, 1> m_uavIndices;
};

class Pipeline : public Resource
{
public:
    bool IsCompute() const { return m_isCompute; }

protected:
    Pipeline() : Resource(ResourceType::Pipeline) {}
    bool m_isCompute = false;
};

enum class LoadOp : u8
{
    Load,
    Clear,
    DontCare
};

enum class StoreOp : u8
{
    Store,
    DontCare
};

struct ColorAttachment
{
    Texture* texture = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    LinearColor clearColor = LinearColor(0, 0, 0, 1);
    u32 mip = 0;
    u32 slice = 0;
};

struct DepthAttachment
{
    Texture* texture = nullptr;
    LoadOp load = LoadOp::Clear;
    StoreOp store = StoreOp::Store;
    f32 clearDepth = 0.0f; // reversed-Z
    u8 clearStencil = 0;
    bool readOnly = false;
};

struct Viewport
{
    f32 x = 0, y = 0, width = 0, height = 0;
    f32 minDepth = 0.0f, maxDepth = 1.0f;
};

enum class IndexFormat : u8
{
    UInt16,
    UInt32
};

/// Transient GPU-visible memory valid for the current frame only.
struct TransientAllocation
{
    void* cpu = nullptr;
    u64 gpuAddress = 0;
    u64 size = 0;
};

class CommandList
{
public:
    virtual ~CommandList() = default;
    virtual QueueType GetQueueType() const = 0;

    // --- Barriers (tracked: only the target state is needed) ---
    virtual void Barrier(Texture* texture, ResourceState state) = 0;
    virtual void Barrier(Buffer* buffer, ResourceState state) = 0;
    /// Waits for prior UAV writes to finish before subsequent accesses.
    virtual void UavBarrier(Resource* resource = nullptr) = 0;

    // --- Rendering ---
    virtual void BeginRenderPass(Span<const ColorAttachment> colors, const DepthAttachment* depth = nullptr) = 0;
    virtual void EndRenderPass() = 0;
    virtual void SetViewport(const Viewport& viewport) = 0;
    virtual void SetScissor(i32 x, i32 y, u32 width, u32 height) = 0;
    virtual void SetPipeline(Pipeline* pipeline) = 0;
    /// Writes 32-bit root constants (b0). size and offset in bytes, multiples of 4, <= 128 total.
    virtual void SetPushConstants(const void* data, u32 size, u32 offset = 0) = 0;
    template <typename T>
    void SetPushConstants(const T& value)
    {
        static_assert(sizeof(T) % 4 == 0 && sizeof(T) <= MaxPushConstants * 4);
        SetPushConstants(&value, u32(sizeof(T)), 0);
    }
    /// Binds a constant buffer GPU address to slot 0 (b1) or 1 (b2).
    virtual void SetConstantBuffer(u32 slot, u64 gpuAddress) = 0;
    /// Allocates per-frame constant memory, copies `data` into it and binds it to `slot`.
    virtual void SetConstants(u32 slot, const void* data, u32 size) = 0;
    template <typename T>
    void SetConstants(u32 slot, const T& value)
    {
        SetConstants(slot, &value, u32(sizeof(T)));
    }
    virtual void SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset = 0) = 0;
    virtual void Draw(u32 vertexCount, u32 instanceCount = 1, u32 firstVertex = 0, u32 firstInstance = 0) = 0;
    virtual void DrawIndexed(u32 indexCount, u32 instanceCount = 1, u32 firstIndex = 0, i32 vertexOffset = 0,
                             u32 firstInstance = 0) = 0;
    virtual void Dispatch(u32 groupsX, u32 groupsY = 1, u32 groupsZ = 1) = 0;
    /// `maxCount` IndirectDrawArgs from `args` (IndirectArgument state) with the bound pipeline and index buffer;
    /// each writes its drawId to push constant dword IndirectDrawIdConstant first (GPU-driven rendering).
    virtual void DrawIndexedIndirect(Buffer* args, u64 offset, u32 maxCount) = 0;

    // --- Transfers ---
    /// Per-frame upload memory (aligned to 256 bytes).
    virtual TransientAllocation AllocateTransient(u64 size, u64 alignment = 256) = 0;
    virtual void CopyBuffer(Buffer* dst, u64 dstOffset, Buffer* src, u64 srcOffset, u64 size) = 0;
    /// Uploads data through transient memory. Transitions the resource as needed.
    virtual void UploadBuffer(Buffer* dst, u64 dstOffset, const void* data, u64 size) = 0;
    virtual void UploadTexture(Texture* dst, u32 mip, u32 arraySlice, const SubresourceData& data) = 0;
    /// Uploads a rectangle of one subresource (uncompressed formats only). `data.rowPitch` = source row stride.
    virtual void UploadTextureRegion(Texture* dst, u32 mip, u32 arraySlice, u32 x, u32 y, u32 width, u32 height,
                                     const SubresourceData& data) = 0;
    /// Copies one subresource into a buffer (rows aligned to 256 bytes). Returns the row pitch in bytes.
    virtual u64 CopyTextureToBuffer(Buffer* dst, u64 dstOffset, Texture* src, u32 mip = 0, u32 arraySlice = 0) = 0;

    // --- Debug / profiling ---
    virtual void BeginEvent(StringView name) = 0;
    virtual void EndEvent() = 0;
    /// Measures GPU time between Begin/EndTimer with the same name; results via Device::GetGpuTimings().
    virtual void BeginTimer(const char* name) = 0;
    virtual void EndTimer() = 0;
};

struct SwapChainDesc
{
    void* windowHandle = nullptr; // HWND
    u32 width = 0;
    u32 height = 0;
    u32 bufferCount = 3;
    Format format = Format::R8G8B8A8_UNORM; // the render pipeline writes sRGB-encoded values itself
    bool vsync = true;
};

class SwapChain
{
public:
    virtual ~SwapChain() = default;
    virtual Texture* GetCurrentBackBuffer() = 0;
    virtual UVec2 GetSize() const = 0;
    virtual Format GetFormat() const = 0;
    /// Recreates buffers. Waits for the GPU. width/height 0 = query from window.
    virtual void Resize(u32 width, u32 height) = 0;
    virtual void Present() = 0;
    virtual void SetVSync(bool enabled) = 0;
    virtual bool GetVSync() const = 0;
};

struct GpuTiming
{
    const char* name;
    f64 milliseconds;
};

struct AdapterInfo
{
    String name;
    u32 vendorId = 0;
    u32 deviceId = 0;
    u64 dedicatedVideoMemory = 0;
    u64 sharedSystemMemory = 0;
    bool isIntegrated = false; // UMA
    bool isSoftware = false;
};

struct DeviceCaps
{
    AdapterInfo adapter;
    u32 shaderModel = 60;          // e.g. 66 for SM 6.6
    u32 resourceBindingTier = 1;
    bool bindlessUav = false;      // unbounded UAV arrays (binding tier 3)
    bool dynamicResources = false; // SM 6.6 ResourceDescriptorHeap
    bool waveOps = false;
    u32 waveLaneCountMin = 0;
    u32 waveLaneCountMax = 0;
    bool native16Bit = false;
    bool typedUavLoadAdditionalFormats = false;
    bool rasterizerOrderedViews = false;
    u32 conservativeRasterTier = 0;
    u32 variableShadingRateTier = 0;
    bool meshShaders = false;
    u32 raytracingTier = 0; // 10 = 1.0, 11 = 1.1
    bool enhancedBarriers = false;
    bool tearingSupported = false;
    bool uma = false;
    bool cacheCoherentUma = false;
    u32 maxTextureSize = 16384;
};

struct DeviceDesc
{
    bool enableDebugLayer = false;
    bool enableGpuValidation = false;
    /// Prefer the discrete GPU (high performance) when several are available.
    bool preferHighPerformanceAdapter = true;
    /// Use WARP (software rasterizer) - for tests on machines without a GPU.
    bool useWarp = false;
};

class Device
{
public:
    virtual ~Device() = default;

    virtual const DeviceCaps& GetCaps() const = 0;
    virtual StringView GetBackendName() const = 0;

    virtual Buffer* CreateBuffer(const BufferDesc& desc, const void* initialData = nullptr) = 0;
    /// `initialData`: one entry per subresource (mip-major within each array slice), or empty.
    virtual Texture* CreateTexture(const TextureDesc& desc, Span<const SubresourceData> initialData = {}) = 0;
    virtual Pipeline* CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual Pipeline* CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual UniquePtr<SwapChain> CreateSwapChain(const SwapChainDesc& desc) = 0;
    /// Bindless sampler index for a custom sampler (static samplers s0..s7 are always available).
    virtual u32 CreateSampler(const SamplerDesc& desc) = 0;

    /// Deferred destruction (safe to call while the GPU may still use the resource). Null is ignored.
    virtual void Destroy(Resource* resource) = 0;

    /// Waits for frame N - FramesInFlight and recycles its transient memory/command allocators.
    virtual void BeginFrame() = 0;
    virtual CommandList* BeginCommandList(QueueType queue = QueueType::Graphics) = 0;
    virtual void Submit(CommandList* list) = 0;
    /// Signals the end of the frame on the GPU timeline.
    virtual void EndFrame() = 0;
    virtual void WaitIdle() = 0;
    virtual u64 GetFrameIndex() const = 0;

    /// GPU timings of the most recently completed frame.
    virtual Span<const GpuTiming> GetGpuTimings() const = 0;

    /// Device-removed / lost state (after a TDR). The app should shut down the renderer.
    virtual bool IsDeviceLost() const = 0;
};

} // namespace zx::rhi
