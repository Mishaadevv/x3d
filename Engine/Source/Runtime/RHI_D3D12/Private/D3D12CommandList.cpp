#include "D3D12Device.h"
#include "Core/String/Utf.h"

namespace zx::rhi::d3d12 {

D3D12CommandList::D3D12CommandList(D3D12Device& device, QueueType type) : m_device(device), m_type(type) {}

bool D3D12CommandList::Init()
{
    ID3D12Device* dev = m_device.Native();
    if (!ZX_DX_CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, ZX_DX_PPV(m_allocator))) ||
        !ZX_DX_CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_allocator.Get(), nullptr,
                                            ZX_DX_PPV(m_list))))
        return false;
    m_list->Close();
    SetDebugName(m_list.Get(), "Graphics Command List");
    return true;
}

void D3D12CommandList::Begin(FrameContext& frame)
{
    m_frame = &frame;
    m_allocator->Reset();
    m_list->Reset(m_allocator.Get(), nullptr);
    ID3D12DescriptorHeap* heaps[] = {m_device.ResourceHeap().Heap(), m_device.SamplerHeap().Heap()};
    m_list->SetDescriptorHeaps(2, heaps);
    m_list->SetGraphicsRootSignature(m_device.RootSignature());
    m_list->SetComputeRootSignature(m_device.RootSignature());
    m_list->SetGraphicsRootDescriptorTable(3, m_device.ResourceHeap().Gpu(0));
    m_list->SetGraphicsRootDescriptorTable(4, m_device.SamplerHeap().Gpu(0));
    m_list->SetComputeRootDescriptorTable(3, m_device.ResourceHeap().Gpu(0));
    m_list->SetComputeRootDescriptorTable(4, m_device.SamplerHeap().Gpu(0));
    m_pipeline = nullptr;
    m_isComputeBound = false;
    m_pendingBarriers.Clear();
    m_timerStack.Clear();
}

void D3D12CommandList::Close()
{
    FlushBarriers();
    ZX_ASSERT(m_timerStack.Empty(), "Unbalanced BeginTimer/EndTimer");
    m_list->Close();
    m_frame = nullptr;
}

// --- Barriers -------------------------------------------------------------------------------------------------------

void D3D12CommandList::Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES& current, D3D12_RESOURCE_STATES target)
{
    if (current == target)
        return;
    // Read-only states can be combined; skip if the target is already included.
    if (target != D3D12_RESOURCE_STATE_COMMON && (current & target) == target &&
        !(target & (D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET |
                    D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_COPY_DEST)))
        return;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = current;
    b.Transition.StateAfter = target;
    m_pendingBarriers.PushBack(b);
    current = target;
}

void D3D12CommandList::FlushBarriers()
{
    if (m_pendingBarriers.Empty())
        return;
    m_list->ResourceBarrier(UINT(m_pendingBarriers.Size()), m_pendingBarriers.Data());
    m_pendingBarriers.Clear();
}

void D3D12CommandList::Barrier(Texture* texture, ResourceState state)
{
    auto* t = static_cast<D3D12Texture*>(texture);
    Transition(t->resource.Get(), t->state, ToD3D12(state));
}

void D3D12CommandList::Barrier(Buffer* buffer, ResourceState state)
{
    auto* b = static_cast<D3D12Buffer*>(buffer);
    if (b->GetDesc().memory != MemoryType::GpuOnly)
        return; // upload/readback heaps have fixed states
    Transition(b->resource.Get(), b->state, ToD3D12(state));
}

void D3D12CommandList::UavBarrier(Resource* resource)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    if (resource) {
        b.UAV.pResource = resource->GetType() == ResourceType::Texture ? static_cast<D3D12Texture*>(resource)->resource.Get()
                                                                      : static_cast<D3D12Buffer*>(resource)->resource.Get();
    }
    m_pendingBarriers.PushBack(b);
}

// --- Rendering ------------------------------------------------------------------------------------------------------

void D3D12CommandList::BeginRenderPass(Span<const ColorAttachment> colors, const DepthAttachment* depth)
{
    ZX_ASSERT(colors.Size() <= MaxRenderTargets);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[MaxRenderTargets];
    UVec2 size(0, 0);
    for (usize i = 0; i < colors.Size(); ++i) {
        auto* t = static_cast<D3D12Texture*>(colors[i].texture);
        Transition(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rtvs[i] = m_device.RtvHeap().Cpu(m_device.GetRtv(t, colors[i].mip, colors[i].slice));
        if (i == 0)
            size = UVec2(Max(1u, t->GetDesc().width >> colors[i].mip), Max(1u, t->GetDesc().height >> colors[i].mip));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = {};
    if (depth && depth->texture) {
        auto* t = static_cast<D3D12Texture*>(depth->texture);
        Transition(t->resource.Get(), t->state,
                   depth->readOnly ? D3D12_RESOURCE_STATE_DEPTH_READ : D3D12_RESOURCE_STATE_DEPTH_WRITE);
        dsv = m_device.DsvHeap().Cpu(depth->readOnly ? t->dsvReadOnly : t->dsv);
        if (colors.Empty())
            size = t->GetSize();
    }
    FlushBarriers();
    m_list->OMSetRenderTargets(UINT(colors.Size()), colors.Empty() ? nullptr : rtvs, FALSE,
                               (depth && depth->texture) ? &dsv : nullptr);
    for (usize i = 0; i < colors.Size(); ++i) {
        if (colors[i].load == LoadOp::Clear) {
            const LinearColor& c = colors[i].clearColor;
            const f32 rgba[4] = {c.r, c.g, c.b, c.a};
            m_list->ClearRenderTargetView(rtvs[i], rgba, 0, nullptr);
        }
    }
    if (depth && depth->texture && depth->load == LoadOp::Clear && !depth->readOnly) {
        D3D12_CLEAR_FLAGS flags = D3D12_CLEAR_FLAG_DEPTH;
        if (GetFormatInfo(depth->texture->GetFormat()).hasStencil)
            flags |= D3D12_CLEAR_FLAG_STENCIL;
        m_list->ClearDepthStencilView(dsv, flags, depth->clearDepth, depth->clearStencil, 0, nullptr);
    }
    SetViewport(Viewport{0, 0, f32(size.x), f32(size.y), 0.0f, 1.0f});
    SetScissor(0, 0, size.x, size.y);
}

void D3D12CommandList::EndRenderPass() {}

void D3D12CommandList::SetViewport(const Viewport& v)
{
    const D3D12_VIEWPORT vp = {v.x, v.y, v.width, v.height, v.minDepth, v.maxDepth};
    m_list->RSSetViewports(1, &vp);
}

void D3D12CommandList::SetScissor(i32 x, i32 y, u32 width, u32 height)
{
    const D3D12_RECT r = {x, y, LONG(x + i32(width)), LONG(y + i32(height))};
    m_list->RSSetScissorRects(1, &r);
}

void D3D12CommandList::SetPipeline(Pipeline* pipeline)
{
    auto* p = static_cast<D3D12Pipeline*>(pipeline);
    if (p == m_pipeline)
        return;
    m_pipeline = p;
    m_isComputeBound = p->IsCompute();
    m_list->SetPipelineState(p->pso.Get());
    if (!p->IsCompute())
        m_list->IASetPrimitiveTopology(p->topology);
}

void D3D12CommandList::SetPushConstants(const void* data, u32 size, u32 offset)
{
    ZX_ASSERT(size % 4 == 0 && offset % 4 == 0 && offset + size <= MaxPushConstants * 4, "Invalid push constant range");
    // The same root signature is bound for graphics and compute; set both so bindings made before SetPipeline (or
    // carried across a compute <-> graphics switch) are always valid.
    m_list->SetGraphicsRoot32BitConstants(0, size / 4, data, offset / 4);
    m_list->SetComputeRoot32BitConstants(0, size / 4, data, offset / 4);
}

void D3D12CommandList::SetConstantBuffer(u32 slot, u64 gpuAddress)
{
    ZX_ASSERT(slot < 2, "Constant buffer slot must be 0 or 1");
    m_list->SetGraphicsRootConstantBufferView(1 + slot, gpuAddress);
    m_list->SetComputeRootConstantBufferView(1 + slot, gpuAddress);
}

void D3D12CommandList::SetConstants(u32 slot, const void* data, u32 size)
{
    const TransientAllocation a = AllocateTransient(size, 256);
    MemCopy(a.cpu, data, size);
    SetConstantBuffer(slot, a.gpuAddress);
}

void D3D12CommandList::SetIndexBuffer(Buffer* buffer, IndexFormat format, u64 offset)
{
    auto* b = static_cast<D3D12Buffer*>(buffer);
    Barrier(buffer, ResourceState::IndexBuffer);
    D3D12_INDEX_BUFFER_VIEW view = {};
    view.BufferLocation = b->GetGpuAddress() + offset;
    view.SizeInBytes = UINT(b->GetSize() - offset);
    view.Format = format == IndexFormat::UInt16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    m_list->IASetIndexBuffer(&view);
}

void D3D12CommandList::Draw(u32 vertexCount, u32 instanceCount, u32 firstVertex, u32 firstInstance)
{
    FlushBarriers();
    m_list->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D12CommandList::DrawIndexed(u32 indexCount, u32 instanceCount, u32 firstIndex, i32 vertexOffset, u32 firstInstance)
{
    FlushBarriers();
    m_list->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void D3D12CommandList::Dispatch(u32 x, u32 y, u32 z)
{
    FlushBarriers();
    m_list->Dispatch(x, y, z);
}

// --- Transfers ------------------------------------------------------------------------------------------------------

TransientAllocation D3D12CommandList::AllocateTransient(u64 size, u64 alignment)
{
    ZX_ASSERT(m_frame, "Command list not recording");
    UploadRing& ring = m_frame->upload;
    const u64 offset = AlignUp(ring.offset, alignment);
    if (offset + size <= ring.capacity) {
        ring.offset = offset + size;
        return TransientAllocation{ring.mapped + offset, ring.gpuBase + offset, size};
    }
    // Overflow: dedicated upload buffer for this frame.
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = AlignUp(size, u64(65536));
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> extra;
    ZX_VERIFY(ZX_DX_CHECK(m_device.Native()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                                    ZX_DX_PPV(extra))));
    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};
    extra->Map(0, &noRead, &mapped);
    TransientAllocation a{mapped, extra->GetGPUVirtualAddress(), size};
    ring.overflow.PushBack(move(extra));
    return a;
}

void D3D12CommandList::CopyBuffer(Buffer* dst, u64 dstOffset, Buffer* src, u64 srcOffset, u64 size)
{
    Barrier(dst, ResourceState::CopyDest);
    Barrier(src, ResourceState::CopySource);
    FlushBarriers();
    m_list->CopyBufferRegion(static_cast<D3D12Buffer*>(dst)->resource.Get(), dstOffset,
                             static_cast<D3D12Buffer*>(src)->resource.Get(), srcOffset, size);
}

void D3D12CommandList::UploadBuffer(Buffer* dst, u64 dstOffset, const void* data, u64 size)
{
    auto* b = static_cast<D3D12Buffer*>(dst);
    if (b->GetMappedData()) {
        MemCopy(static_cast<u8*>(b->GetMappedData()) + dstOffset, data, size);
        return;
    }
    const TransientAllocation a = AllocateTransient(size, 16);
    MemCopy(a.cpu, data, size);
    // Locate the upload resource that owns the allocation (ring or overflow).
    UploadRing& ring = m_frame->upload;
    ID3D12Resource* srcResource = ring.buffer.Get();
    u64 srcOffset = a.gpuAddress - ring.gpuBase;
    if (a.gpuAddress < ring.gpuBase || a.gpuAddress >= ring.gpuBase + ring.capacity) {
        srcResource = ring.overflow.Back().Get();
        srcOffset = 0;
    }
    Barrier(dst, ResourceState::CopyDest);
    FlushBarriers();
    m_list->CopyBufferRegion(b->resource.Get(), dstOffset, srcResource, srcOffset, size);
}

void D3D12CommandList::UploadTexture(Texture* dst, u32 mip, u32 arraySlice, const SubresourceData& data)
{
    auto* t = static_cast<D3D12Texture*>(dst);
    const D3D12_RESOURCE_DESC rd = t->resource->GetDesc();
    const u32 subresource = mip + arraySlice * rd.MipLevels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    m_device.Native()->GetCopyableFootprints(&rd, subresource, 1, 0, &layout, &rows, &rowSize, &total);
    const TransientAllocation a = AllocateTransient(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const u64 srcRowPitch = data.rowPitch ? data.rowPitch : rowSize;
    const u64 srcSlicePitch = data.slicePitch ? data.slicePitch : srcRowPitch * rows;
    for (u32 z = 0; z < layout.Footprint.Depth; ++z)
        for (u32 y = 0; y < rows; ++y)
            MemCopy(static_cast<u8*>(a.cpu) + u64(z) * layout.Footprint.RowPitch * rows + u64(y) * layout.Footprint.RowPitch,
                    static_cast<const u8*>(data.data) + z * srcSlicePitch + y * srcRowPitch, rowSize);
    UploadRing& ring = m_frame->upload;
    ID3D12Resource* srcResource = ring.buffer.Get();
    u64 srcOffset = a.gpuAddress - ring.gpuBase;
    if (a.gpuAddress < ring.gpuBase || a.gpuAddress >= ring.gpuBase + ring.capacity) {
        srcResource = ring.overflow.Back().Get();
        srcOffset = 0;
    }
    layout.Offset = srcOffset;
    Transition(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_COPY_DEST);
    FlushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl = {};
    dl.pResource = t->resource.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION sl = {};
    sl.pResource = srcResource;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = layout;
    m_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12CommandList::UploadTextureRegion(Texture* dst, u32 mip, u32 arraySlice, u32 x, u32 y, u32 width, u32 height,
                                           const SubresourceData& data)
{
    auto* t = static_cast<D3D12Texture*>(dst);
    const FormatInfo& fi = GetFormatInfo(t->GetFormat());
    ZX_ASSERT(!fi.isCompressed, "UploadTextureRegion: compressed formats are not supported");
    if (width == 0 || height == 0)
        return;
    const D3D12_RESOURCE_DESC rd = t->resource->GetDesc();
    const u32 subresource = mip + arraySlice * rd.MipLevels;
    const u64 rowSize = u64(width) * fi.bytesPerBlock;
    const u64 pitch = AlignUp(rowSize, u64(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
    const TransientAllocation a = AllocateTransient(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    const u64 srcRowPitch = data.rowPitch ? data.rowPitch : rowSize;
    for (u32 row = 0; row < height; ++row)
        MemCopy(static_cast<u8*>(a.cpu) + row * pitch, static_cast<const u8*>(data.data) + row * srcRowPitch, rowSize);
    UploadRing& ring = m_frame->upload;
    ID3D12Resource* srcResource = ring.buffer.Get();
    u64 srcOffset = a.gpuAddress - ring.gpuBase;
    if (a.gpuAddress < ring.gpuBase || a.gpuAddress >= ring.gpuBase + ring.capacity) {
        srcResource = ring.overflow.Back().Get();
        srcOffset = 0;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    layout.Offset = srcOffset;
    layout.Footprint.Format = rd.Format;
    layout.Footprint.Width = width;
    layout.Footprint.Height = height;
    layout.Footprint.Depth = 1;
    layout.Footprint.RowPitch = UINT(pitch);
    Transition(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_COPY_DEST);
    FlushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl = {};
    dl.pResource = t->resource.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION sl = {};
    sl.pResource = srcResource;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint = layout;
    m_list->CopyTextureRegion(&dl, x, y, 0, &sl, nullptr);
}

u64 D3D12CommandList::CopyTextureToBuffer(Buffer* dst, u64 dstOffset, Texture* src, u32 mip, u32 arraySlice)
{
    auto* t = static_cast<D3D12Texture*>(src);
    auto* b = static_cast<D3D12Buffer*>(dst);
    const D3D12_RESOURCE_DESC rd = t->resource->GetDesc();
    const u32 subresource = mip + arraySlice * rd.MipLevels;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    m_device.Native()->GetCopyableFootprints(&rd, subresource, 1, dstOffset, &layout, &rows, &rowSize, &total);
    ZX_ASSERT(dstOffset + total <= b->GetSize(), "CopyTextureToBuffer: destination too small ({} needed)", total);
    Transition(t->resource.Get(), t->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(dst, ResourceState::CopyDest);
    FlushBarriers();
    D3D12_TEXTURE_COPY_LOCATION dl = {};
    dl.pResource = b->resource.Get();
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = layout;
    D3D12_TEXTURE_COPY_LOCATION sl = {};
    sl.pResource = t->resource.Get();
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = subresource;
    m_list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    return layout.Footprint.RowPitch;
}

// --- Debug / timing -------------------------------------------------------------------------------------------------

void D3D12CommandList::BeginEvent(StringView name)
{
    // PIX-compatible marker without WinPixEventRuntime: metadata 1 = UTF-16 string.
    const Array<wchar_t> w = Utf8ToUtf16(name);
    m_list->BeginEvent(1, w.Data(), UINT((w.Size() + 1) * sizeof(wchar_t)));
}

void D3D12CommandList::EndEvent() { m_list->EndEvent(); }

void D3D12CommandList::BeginTimer(const char* name)
{
    FrameContext& f = *m_frame;
    if (f.queryCount + 2 > D3D12Device::MaxTimestampsPerFrame) {
        m_timerStack.PushBack({name, InvalidIndex});
        return;
    }
    const u32 base = u32(m_device.GetFrameIndex() % FramesInFlight) * D3D12Device::MaxTimestampsPerFrame;
    const u32 index = f.queryCount++;
    m_list->EndQuery(m_device.TimestampHeap(), D3D12_QUERY_TYPE_TIMESTAMP, base + index);
    m_timerStack.PushBack({name, index});
    BeginEvent(name);
}

void D3D12CommandList::EndTimer()
{
    ZX_ASSERT(!m_timerStack.Empty(), "EndTimer without BeginTimer");
    const Pair<const char*, u32> top = m_timerStack.PopBackValue();
    if (top.second == InvalidIndex)
        return;
    EndEvent();
    FrameContext& f = *m_frame;
    const u32 base = u32(m_device.GetFrameIndex() % FramesInFlight) * D3D12Device::MaxTimestampsPerFrame;
    const u32 index = f.queryCount++;
    m_list->EndQuery(m_device.TimestampHeap(), D3D12_QUERY_TYPE_TIMESTAMP, base + index);
    f.timers.PushBack(TimerRecord{top.first, top.second, index});
}

} // namespace zx::rhi::d3d12
