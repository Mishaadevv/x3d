#include "Render/RenderGraph.h"
#include "Core/Hash/Hash.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Name.h"

namespace zx::render {

using namespace rhi;

namespace {

u64 TextureKey(const TextureDesc& d)
{
    XXHash64Stream h;
    h.UpdateValue(u32(d.dimension));
    h.UpdateValue(d.width);
    h.UpdateValue(d.height);
    h.UpdateValue(d.depthOrArraySize);
    h.UpdateValue(d.mipLevels);
    h.UpdateValue(d.sampleCount);
    h.UpdateValue(u32(d.format));
    h.UpdateValue(u32(d.usage));
    h.UpdateValue(d.clearValue.color.r);
    h.UpdateValue(d.clearValue.color.g);
    h.UpdateValue(d.clearValue.color.b);
    h.UpdateValue(d.clearValue.color.a);
    h.UpdateValue(d.clearValue.depth);
    return h.Digest();
}

u64 BufferKey(const BufferDesc& d)
{
    XXHash64Stream h;
    h.UpdateValue(d.size);
    h.UpdateValue(u32(d.usage));
    h.UpdateValue(u32(d.memory));
    h.UpdateValue(d.stride);
    return h.Digest();
}

} // namespace

// --- Pool -----------------------------------------------------------------------------------------------------------

TransientResourcePool::~TransientResourcePool() { Clear(); }

Texture* TransientResourcePool::AcquireTexture(const TextureDesc& desc, u64 frame)
{
    const u64 key = TextureKey(desc);
    for (usize i = 0; i < m_freeTextures.Size(); ++i) {
        if (m_freeTextures[i].key == key) {
            Texture* t = m_freeTextures[i].texture;
            m_freeTextures.RemoveAtSwap(i);
            return t;
        }
    }
    ZX_UNUSED(frame);
    ++m_texturesCreated;
    return m_device.CreateTexture(desc);
}

Buffer* TransientResourcePool::AcquireBuffer(const BufferDesc& desc, u64 frame)
{
    const u64 key = BufferKey(desc);
    for (usize i = 0; i < m_freeBuffers.Size(); ++i) {
        if (m_freeBuffers[i].key == key) {
            Buffer* b = m_freeBuffers[i].buffer;
            m_freeBuffers.RemoveAtSwap(i);
            return b;
        }
    }
    ZX_UNUSED(frame);
    return m_device.CreateBuffer(desc);
}

void TransientResourcePool::ReleaseTexture(Texture* texture, u64 frame)
{
    m_freeTextures.PushBack(FreeTexture{TextureKey(texture->GetDesc()), texture, frame});
}

void TransientResourcePool::ReleaseBuffer(Buffer* buffer, u64 frame)
{
    m_freeBuffers.PushBack(FreeBuffer{BufferKey(buffer->GetDesc()), buffer, frame});
}

void TransientResourcePool::Trim(u64 frame, u64 maxIdleFrames)
{
    m_freeTextures.RemoveIf([&](const FreeTexture& f) {
        if (frame - f.lastUsedFrame <= maxIdleFrames)
            return false;
        m_device.Destroy(f.texture);
        return true;
    });
    m_freeBuffers.RemoveIf([&](const FreeBuffer& f) {
        if (frame - f.lastUsedFrame <= maxIdleFrames)
            return false;
        m_device.Destroy(f.buffer);
        return true;
    });
}

void TransientResourcePool::Clear()
{
    for (const FreeTexture& f : m_freeTextures)
        m_device.Destroy(f.texture);
    for (const FreeBuffer& f : m_freeBuffers)
        m_device.Destroy(f.buffer);
    m_freeTextures.Clear();
    m_freeBuffers.Clear();
}

// --- Builder / context ----------------------------------------------------------------------------------------------

void PassBuilder::ReadTexture(RGTexture t, ResourceState s) { m_graph.AddAccess(m_pass, t.index, true, false, s); }
void PassBuilder::WriteTexture(RGTexture t, ResourceState s) { m_graph.AddAccess(m_pass, t.index, true, true, s); }
void PassBuilder::ReadBuffer(RGBuffer b, ResourceState s) { m_graph.AddAccess(m_pass, b.index, false, false, s); }
void PassBuilder::WriteBuffer(RGBuffer b, ResourceState s) { m_graph.AddAccess(m_pass, b.index, false, true, s); }

void PassBuilder::RenderTarget(RGTexture t, LoadOp load, LinearColor clearColor)
{
    // Loading previous contents is also a read.
    if (load == LoadOp::Load)
        m_graph.AddAccess(m_pass, t.index, true, false, ResourceState::RenderTarget);
    m_graph.AddAccess(m_pass, t.index, true, true, ResourceState::RenderTarget);
    m_graph.m_passes[m_pass].colors.PushBack(RenderGraph::Attachment{t, load, clearColor, 0.0f, false});
}

void PassBuilder::DepthStencil(RGTexture t, LoadOp load, f32 clearDepth, bool readOnly)
{
    if (load == LoadOp::Load || readOnly)
        m_graph.AddAccess(m_pass, t.index, true, false, readOnly ? ResourceState::DepthRead : ResourceState::DepthWrite);
    if (!readOnly)
        m_graph.AddAccess(m_pass, t.index, true, true, ResourceState::DepthWrite);
    m_graph.m_passes[m_pass].depth = RenderGraph::Attachment{t, load, LinearColor(), clearDepth, readOnly};
}

void PassBuilder::SideEffects() { m_graph.m_passes[m_pass].sideEffects = true; }

Texture* PassContext::Texture(RGTexture h) const
{
    ZX_ASSERT(h.index < m_graph.m_textures.Size());
    return m_graph.m_textures[h.index].physical;
}

Buffer* PassContext::Buffer(RGBuffer h) const
{
    ZX_ASSERT(h.index < m_graph.m_buffers.Size());
    return m_graph.m_buffers[h.index].physical;
}

// --- Graph ----------------------------------------------------------------------------------------------------------

RenderGraph::RenderGraph(Device& device, TransientResourcePool& pool) : m_device(device), m_pool(pool) {}

RenderGraph::~RenderGraph()
{
    ZX_ASSERT(m_executed || m_passes.Empty(), "RenderGraph destroyed without Execute()");
}

RGTexture RenderGraph::CreateTexture(const TextureDesc& desc)
{
    TextureEntry e;
    e.desc = desc;
    e.name = desc.debugName;
    e.desc.debugName = StringView(); // the name string is owned by the entry
    m_textures.PushBack(move(e));
    return RGTexture{u32(m_textures.Size() - 1)};
}

RGBuffer RenderGraph::CreateBuffer(const BufferDesc& desc)
{
    BufferEntry e;
    e.desc = desc;
    e.name = desc.debugName;
    e.desc.debugName = StringView();
    m_buffers.PushBack(move(e));
    return RGBuffer{u32(m_buffers.Size() - 1)};
}

RGTexture RenderGraph::ImportTexture(rhi::Texture* texture, ResourceState finalState)
{
    TextureEntry e;
    e.desc = texture->GetDesc();
    e.name = texture->GetDebugName();
    e.physical = texture;
    e.imported = true;
    e.finalState = finalState;
    m_textures.PushBack(move(e));
    return RGTexture{u32(m_textures.Size() - 1)};
}

RGBuffer RenderGraph::ImportBuffer(rhi::Buffer* buffer, ResourceState finalState)
{
    BufferEntry e;
    e.desc = buffer->GetDesc();
    e.name = buffer->GetDebugName();
    e.physical = buffer;
    e.imported = true;
    e.finalState = finalState;
    m_buffers.PushBack(move(e));
    return RGBuffer{u32(m_buffers.Size() - 1)};
}

const TextureDesc& RenderGraph::GetDesc(RGTexture t) const { return m_textures[t.index].desc; }

void RenderGraph::AddPass(StringView name, PassType type, SetupFn setup, ExecuteFn execute)
{
    ZX_ASSERT(!m_executed, "AddPass after Execute");
    Pass p;
    p.name = name;
    p.type = type;
    p.execute = move(execute);
    m_passes.PushBack(move(p));
    PassBuilder builder(*this, u32(m_passes.Size() - 1));
    setup(builder);
}

void RenderGraph::AddAccess(u32 pass, u32 resource, bool isTexture, bool write, ResourceState state)
{
    ZX_ASSERT(resource < (isTexture ? m_textures.Size() : m_buffers.Size()), "Invalid render graph resource handle");
    m_passes[pass].accesses.PushBack(Access{resource, isTexture, write, state});
}

void RenderGraph::Cull()
{
    // Reference counting from outputs backwards: a pass is needed if it has side effects or writes a resource
    // that is imported or read by a needed later pass.
    const usize n = m_passes.Size();
    Array<u8> textureNeeded, bufferNeeded;
    textureNeeded.ResizeZeroed(m_textures.Size());
    bufferNeeded.ResizeZeroed(m_buffers.Size());
    for (usize i = 0; i < m_textures.Size(); ++i)
        textureNeeded[i] = m_textures[i].imported ? 1 : 0;
    for (usize i = 0; i < m_buffers.Size(); ++i)
        bufferNeeded[i] = m_buffers[i].imported ? 1 : 0;
    for (usize pi = n; pi > 0; --pi) {
        Pass& p = m_passes[pi - 1];
        bool needed = p.sideEffects;
        for (const Access& a : p.accesses)
            if (a.write && (a.isTexture ? textureNeeded[a.resource] : bufferNeeded[a.resource]))
                needed = true;
        p.culled = !needed;
        if (needed)
            for (const Access& a : p.accesses)
                if (!a.write) {
                    if (a.isTexture)
                        textureNeeded[a.resource] = 1;
                    else
                        bufferNeeded[a.resource] = 1;
                }
    }
    m_stats.passesTotal = u32(n);
    m_stats.passesCulled = 0;
    for (const Pass& p : m_passes)
        m_stats.passesCulled += p.culled ? 1 : 0;
}

void RenderGraph::ComputeLifetimes()
{
    for (u32 pi = 0; pi < m_passes.Size(); ++pi) {
        if (m_passes[pi].culled)
            continue;
        for (const Access& a : m_passes[pi].accesses) {
            if (a.isTexture) {
                TextureEntry& t = m_textures[a.resource];
                t.firstPass = Min(t.firstPass, pi);
                t.lastPass = Max(t.lastPass, pi);
            } else {
                BufferEntry& b = m_buffers[a.resource];
                b.firstPass = Min(b.firstPass, pi);
                b.lastPass = Max(b.lastPass, pi);
            }
        }
    }
}

void RenderGraph::Execute(CommandList& cmd)
{
    ZX_PROFILE_SCOPE("RenderGraph::Execute");
    ZX_ASSERT(!m_executed, "RenderGraph executed twice");
    m_executed = true;
    Cull();
    ComputeLifetimes();
    const u64 frame = m_device.GetFrameIndex();

    // Allocation order: at each pass, first release transients whose lifetime ended before it, then acquire
    // transients starting at it. Releasing to the pool makes the physical resource available to later
    // transients with the same description (aliasing within the frame).
    Array<Texture*> distinct;
    m_stats.transientTextures = 0;
    for (u32 pi = 0; pi < m_passes.Size(); ++pi) {
        Pass& pass = m_passes[pi];
        if (pass.culled)
            continue;
        for (TextureEntry& t : m_textures) {
            if (!t.imported && t.physical && t.lastPass < pi) {
                m_pool.ReleaseTexture(t.physical, frame);
                t.physical = nullptr; // still remembered for stats; no longer accessible
            }
        }
        for (BufferEntry& b : m_buffers) {
            if (!b.imported && b.physical && b.lastPass < pi) {
                m_pool.ReleaseBuffer(b.physical, frame);
                b.physical = nullptr;
            }
        }
        for (TextureEntry& t : m_textures) {
            if (!t.imported && !t.physical && t.firstPass == pi) {
                TextureDesc desc = t.desc;
                desc.debugName = t.name;
                t.physical = m_pool.AcquireTexture(desc, frame);
                ++m_stats.transientTextures;
                if (!distinct.Contains(t.physical))
                    distinct.PushBack(t.physical);
            }
        }
        for (BufferEntry& b : m_buffers) {
            if (!b.imported && !b.physical && b.firstPass == pi) {
                BufferDesc desc = b.desc;
                desc.debugName = b.name;
                b.physical = m_pool.AcquireBuffer(desc, frame);
            }
        }

        // Barriers for every access of this pass (the command list tracks current states).
        for (const Access& a : pass.accesses) {
            if (a.isTexture)
                cmd.Barrier(m_textures[a.resource].physical, a.state);
            else
                cmd.Barrier(m_buffers[a.resource].physical, a.state);
        }

        // Interned names stay valid until shutdown, as GPU timing results are read frames later.
        cmd.BeginTimer(Name(pass.name).CStr());
        const bool renderPass = pass.type == PassType::Graphics && (!pass.colors.Empty() || pass.depth.HasValue());
        if (renderPass) {
            SmallArray<ColorAttachment, MaxRenderTargets> colors;
            for (const Attachment& c : pass.colors)
                colors.PushBack(ColorAttachment{m_textures[c.texture.index].physical, c.load, StoreOp::Store, c.clearColor});
            DepthAttachment depth;
            if (pass.depth) {
                depth.texture = m_textures[pass.depth->texture.index].physical;
                depth.load = pass.depth->load;
                depth.clearDepth = pass.depth->clearDepth;
                depth.readOnly = pass.depth->readOnly;
            }
            cmd.BeginRenderPass(Span<const ColorAttachment>(colors.Data(), colors.Size()), pass.depth ? &depth : nullptr);
        }
        PassContext ctx(cmd, *this);
        if (pass.execute)
            pass.execute(ctx);
        if (renderPass)
            cmd.EndRenderPass();
        cmd.EndTimer();
    }
    m_stats.physicalTransientTextures = u32(distinct.Size());

    // Release remaining transients and move imported resources to their final states.
    for (TextureEntry& t : m_textures) {
        if (t.imported) {
            if (t.firstPass != InvalidIndex)
                cmd.Barrier(t.physical, t.finalState);
        } else if (t.physical) {
            m_pool.ReleaseTexture(t.physical, frame);
            t.physical = nullptr;
        }
    }
    for (BufferEntry& b : m_buffers) {
        if (b.imported) {
            if (b.firstPass != InvalidIndex)
                cmd.Barrier(b.physical, b.finalState);
        } else if (b.physical) {
            m_pool.ReleaseBuffer(b.physical, frame);
            b.physical = nullptr;
        }
    }
    m_pool.Trim(frame);
}

String RenderGraph::Dump() const
{
    String s;
    for (u32 i = 0; i < m_passes.Size(); ++i) {
        const Pass& p = m_passes[i];
        s.Appendf("[{}] {}{}\n", i, p.name, p.culled ? " (culled)" : "");
        for (const Access& a : p.accesses)
            s.Appendf("    {} {} '{}'\n", a.write ? "write" : "read ", a.isTexture ? "texture" : "buffer",
                      a.isTexture ? m_textures[a.resource].name.View() : m_buffers[a.resource].name.View());
    }
    for (const TextureEntry& t : m_textures)
        if (t.firstPass != InvalidIndex)
            s.Appendf("texture '{}' {}x{} passes {}..{}{}\n", t.name, t.desc.width, t.desc.height, t.firstPass,
                      t.lastPass, t.imported ? " (imported)" : "");
    return s;
}

} // namespace zx::render
