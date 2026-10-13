#pragma once

// Render graph (frame graph).
//
//   RenderGraph graph(device, pool);
//   RGTexture backBuffer = graph.ImportTexture(swapChain->GetCurrentBackBuffer(), ResourceState::Present);
//   RGTexture hdr = graph.CreateTexture(desc);                       // transient: allocated from the pool
//   graph.AddPass("Scene", PassType::Graphics,
//       [&](PassBuilder& b) { b.RenderTarget(hdr, LoadOp::Clear); b.DepthStencil(depth); },
//       [&](PassContext& ctx) { ctx.cmd.Draw(...); });
//   graph.AddPass("Tonemap", PassType::Graphics,
//       [&](PassBuilder& b) { b.ReadTexture(hdr); b.RenderTarget(backBuffer); },
//       [&](PassContext& ctx) { ... ctx.Texture(hdr)->GetSrvIndex() ... });
//   graph.Execute(*cmd);  // culls unused passes, allocates/aliases transients, inserts barriers
//
// Passes are culled unless they (transitively) write an imported resource or are marked with SideEffects().
// Transient textures/buffers with identical descriptions and non-overlapping lifetimes share one physical
// resource within a frame; the pool keeps them across frames and frees unused ones after a few frames.

#include "Core/Containers/Function.h"
#include "Core/Containers/HashMap.h"
#include "Core/Containers/Optional.h"
#include "RHI/RHI.h"

namespace zx::render {

struct RGTexture
{
    u32 index = InvalidIndex;
    bool IsValid() const { return index != InvalidIndex; }
    friend bool operator==(RGTexture, RGTexture) = default;
};

struct RGBuffer
{
    u32 index = InvalidIndex;
    bool IsValid() const { return index != InvalidIndex; }
    friend bool operator==(RGBuffer, RGBuffer) = default;
};

enum class PassType : u8
{
    Graphics,
    Compute,
    Copy
};

/// Keeps physical transient resources alive between frames. One pool per renderer.
class TransientResourcePool
{
public:
    explicit TransientResourcePool(rhi::Device& device) : m_device(device) {}
    ~TransientResourcePool();
    ZX_NON_COPYABLE(TransientResourcePool)

    rhi::Texture* AcquireTexture(const rhi::TextureDesc& desc, u64 frame);
    rhi::Buffer* AcquireBuffer(const rhi::BufferDesc& desc, u64 frame);
    void ReleaseTexture(rhi::Texture* texture, u64 frame);
    void ReleaseBuffer(rhi::Buffer* buffer, u64 frame);
    /// Destroys resources unused for `maxIdleFrames`.
    void Trim(u64 frame, u64 maxIdleFrames = 4);
    void Clear();

    u32 TexturesAllocated() const { return m_texturesCreated; }
    usize FreeTextureCount() const { return m_freeTextures.Size(); }

private:
    struct FreeTexture
    {
        u64 key;
        rhi::Texture* texture;
        u64 lastUsedFrame;
    };
    struct FreeBuffer
    {
        u64 key;
        rhi::Buffer* buffer;
        u64 lastUsedFrame;
    };
    rhi::Device& m_device;
    Array<FreeTexture> m_freeTextures;
    Array<FreeBuffer> m_freeBuffers;
    u32 m_texturesCreated = 0;
};

class RenderGraph;

class PassBuilder
{
public:
    void ReadTexture(RGTexture texture, rhi::ResourceState state = rhi::ResourceState::ShaderResource);
    void WriteTexture(RGTexture texture, rhi::ResourceState state = rhi::ResourceState::UnorderedAccess);
    void ReadBuffer(RGBuffer buffer, rhi::ResourceState state = rhi::ResourceState::ShaderResource);
    void WriteBuffer(RGBuffer buffer, rhi::ResourceState state = rhi::ResourceState::UnorderedAccess);
    /// Color attachment (graphics passes). The graph begins/ends the render pass around the execute callback.
    void RenderTarget(RGTexture texture, rhi::LoadOp load = rhi::LoadOp::Load,
                      LinearColor clearColor = LinearColor(0, 0, 0, 1));
    void DepthStencil(RGTexture texture, rhi::LoadOp load = rhi::LoadOp::Load, f32 clearDepth = 0.0f,
                      bool readOnly = false);
    /// Never cull this pass (e.g. writes to external state, readbacks, debug output).
    void SideEffects();

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, u32 pass) : m_graph(graph), m_pass(pass) {}
    RenderGraph& m_graph;
    u32 m_pass;
};

class PassContext
{
public:
    rhi::CommandList& cmd;
    rhi::Texture* Texture(RGTexture handle) const;
    rhi::Buffer* Buffer(RGBuffer handle) const;

private:
    friend class RenderGraph;
    PassContext(rhi::CommandList& c, const RenderGraph& g) : cmd(c), m_graph(g) {}
    const RenderGraph& m_graph;
};

class RenderGraph
{
public:
    RenderGraph(rhi::Device& device, TransientResourcePool& pool);
    ~RenderGraph();
    ZX_NON_COPYABLE(RenderGraph)

    RGTexture CreateTexture(const rhi::TextureDesc& desc);
    RGBuffer CreateBuffer(const rhi::BufferDesc& desc);
    /// External resource; it is left in `finalState` after execution.
    RGTexture ImportTexture(rhi::Texture* texture, rhi::ResourceState finalState);
    RGBuffer ImportBuffer(rhi::Buffer* buffer, rhi::ResourceState finalState);

    using SetupFn = Function<void(PassBuilder&)>;
    using ExecuteFn = Function<void(PassContext&)>;
    void AddPass(StringView name, PassType type, SetupFn setup, ExecuteFn execute);

    /// Compiles and records all passes into `cmd`. The graph is single-use.
    void Execute(rhi::CommandList& cmd);

    const rhi::TextureDesc& GetDesc(RGTexture texture) const;

    struct Stats
    {
        u32 passesTotal = 0;
        u32 passesCulled = 0;
        u32 transientTextures = 0;       // logical
        u32 physicalTransientTextures = 0; // after aliasing within the frame
    };
    const Stats& GetStats() const { return m_stats; }
    /// Human-readable dump of passes (culled marked) and resource lifetimes.
    String Dump() const;

private:
    friend class PassBuilder;
    friend class PassContext;

    struct Access
    {
        u32 resource;
        bool isTexture;
        bool write;
        rhi::ResourceState state;
    };
    struct Attachment
    {
        RGTexture texture;
        rhi::LoadOp load;
        LinearColor clearColor;
        f32 clearDepth;
        bool readOnly;
    };
    struct Pass
    {
        String name;
        PassType type;
        ExecuteFn execute;
        SmallArray<Access, 8> accesses;
        SmallArray<Attachment, 4> colors;
        Optional<Attachment> depth;
        bool sideEffects = false;
        bool culled = false;
        u32 refCount = 0;
    };
    struct TextureEntry
    {
        rhi::TextureDesc desc;
        String name;
        rhi::Texture* physical = nullptr;
        bool imported = false;
        rhi::ResourceState finalState = rhi::ResourceState::Common;
        u32 firstPass = InvalidIndex, lastPass = 0;
        u32 producerCount = 0;
    };
    struct BufferEntry
    {
        rhi::BufferDesc desc;
        String name;
        rhi::Buffer* physical = nullptr;
        bool imported = false;
        rhi::ResourceState finalState = rhi::ResourceState::Common;
        u32 firstPass = InvalidIndex, lastPass = 0;
    };

    void AddAccess(u32 pass, u32 resource, bool isTexture, bool write, rhi::ResourceState state);
    void Cull();
    void ComputeLifetimes();

    rhi::Device& m_device;
    TransientResourcePool& m_pool;
    Array<Pass> m_passes;
    Array<TextureEntry> m_textures;
    Array<BufferEntry> m_buffers;
    Stats m_stats;
    bool m_executed = false;
};

} // namespace zx::render
