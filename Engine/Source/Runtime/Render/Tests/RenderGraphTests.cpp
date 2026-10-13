#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Testing/Test.h"
#include "Render/RenderGraph.h"
#include "RHI_D3D12/D3D12Backend.h"

using namespace zx;
using namespace zx::rhi;
using namespace zx::render;

namespace {
TextureDesc RTDesc(StringView name)
{
    TextureDesc d;
    d.width = d.height = 16;
    d.format = Format::R8G8B8A8_UNORM;
    d.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    d.debugName = name;
    return d;
}
} // namespace

ZX_TEST(RenderGraph, CullingAliasingAndExecution)
{
    UniquePtr<Device> device = d3d12::CreateD3D12Device(DeviceDesc{.enableDebugLayer = true, .useWarp = true});
    ZX_REQUIRE(device);
    Buffer* readback = device->CreateBuffer(BufferDesc{256 * 16, BufferUsage::CopyDest, MemoryType::Readback, 0, "RB"});
    {
        TransientResourcePool pool(*device);
        for (int frame = 0; frame < 3; ++frame) {
            device->BeginFrame();
            CommandList* cmd = device->BeginCommandList();
            RenderGraph graph(*device, pool);
            const RGBuffer out = graph.ImportBuffer(readback, ResourceState::CopyDest);
            const RGTexture a = graph.CreateTexture(RTDesc("A"));
            const RGTexture b = graph.CreateTexture(RTDesc("B")); // same desc, disjoint lifetime -> aliases A
            const RGTexture unused = graph.CreateTexture(RTDesc("Unused"));
            graph.AddPass("ClearA", PassType::Graphics, [&](PassBuilder& p) { p.RenderTarget(a, LoadOp::Clear, LinearColor(0, 1, 0, 1)); },
                          nullptr);
            graph.AddPass("CopyA", PassType::Copy,
                          [&](PassBuilder& p) {
                              p.ReadTexture(a, ResourceState::CopySource);
                              p.WriteBuffer(out, ResourceState::CopyDest);
                          },
                          [&](PassContext& ctx) { ctx.cmd.CopyTextureToBuffer(ctx.Buffer(out), 0, ctx.Texture(a)); });
            graph.AddPass("ClearB", PassType::Graphics, [&](PassBuilder& p) { p.RenderTarget(b, LoadOp::Clear, LinearColor(1, 0, 0, 1)); },
                          nullptr);
            graph.AddPass("CopyBDebug", PassType::Copy, [&](PassBuilder& p) {
                p.ReadTexture(b, ResourceState::CopySource);
                p.SideEffects();
            }, nullptr);
            graph.AddPass("WriteUnused", PassType::Graphics, [&](PassBuilder& p) { p.RenderTarget(unused, LoadOp::Clear); },
                          nullptr);
            graph.Execute(*cmd);
            device->Submit(cmd);
            device->EndFrame();
            ZX_EXPECT_EQ(graph.GetStats().passesTotal, 5u);
            ZX_EXPECT_EQ(graph.GetStats().passesCulled, 1u); // WriteUnused
            ZX_EXPECT_EQ(graph.GetStats().transientTextures, 2u);
            ZX_EXPECT_EQ(graph.GetStats().physicalTransientTextures, 1u); // A and B share memory
            if (frame == 0)
                ZX_EXPECT(graph.Dump().Contains("WriteUnused (culled)"));
        }
        ZX_EXPECT_EQ(pool.TexturesAllocated(), 1u); // reused across frames
        device->WaitIdle();
        const u8* px = static_cast<const u8*>(readback->GetMappedData());
        ZX_EXPECT(px[0] == 0 && px[1] == 255 && px[2] == 0); // green from pass ClearA
    }
    device->Destroy(readback);
    device->WaitIdle();
    ZX_EXPECT_EQ(d3d12::GetDebugLayerErrorCount(*device), 0u);
}
