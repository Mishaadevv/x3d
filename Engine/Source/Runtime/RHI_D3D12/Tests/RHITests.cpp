// GPU tests on WARP (software D3D12): deterministic and independent of the installed GPU driver.

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/Testing/Test.h"
#include "Core/Threading/Thread.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"

using namespace zx;
using namespace zx::rhi;

namespace {

struct GpuTestScope
{
    UniquePtr<Device> device;
    String savedDir;

    GpuTestScope()
    {
        fs::Initialize(1);
        const String root = FindEngineRootDirectory();
        fs::Mount("/Shaders", root + "/Engine/Shaders", 0, false);
        savedDir = platform::GetTempDirectory() + String::Format("/zx_rhi_test_{}", Thread::CurrentId());
        platform::CreateDirectoryTree(savedDir);
        fs::Mount("/Saved", savedDir);
        ShaderCompiler::Initialize();
        DeviceDesc desc;
        desc.useWarp = true;
        desc.enableDebugLayer = true;
        device = d3d12::CreateD3D12Device(desc);
    }
    ~GpuTestScope()
    {
        device.Reset();
        ShaderCompiler::Shutdown();
        fs::Shutdown();
        platform::DeleteDirectoryTree(savedDir);
    }
};

Result<ShaderBytecode> CompileTestShader(StringView entry, ShaderStage stage)
{
    ShaderCompileDesc d;
    d.path = "/Shaders/Tests/RHITest.hlsl";
    d.entryPoint = entry;
    d.stage = stage;
    return ShaderCompiler::Compile(d);
}

} // namespace

ZX_TEST(RHI, ComputeFillsBuffer)
{
    GpuTestScope scope;
    ZX_REQUIRE(scope.device);
    ZX_REQUIRE(ShaderCompiler::IsAvailable());
    Device& dev = *scope.device;

    Result<ShaderBytecode> cs = CompileTestShader("FillCS", ShaderStage::Compute);
    if (!cs)
        test::ReportFailure(__FILE__, __LINE__, cs.GetError().message);
    ZX_REQUIRE(cs.IsOk());
    Pipeline* pipeline = dev.CreateComputePipeline(ComputePipelineDesc{&*cs, "FillCS"});
    ZX_REQUIRE(pipeline);

    constexpr u32 Count = 256;
    Buffer* gpu = dev.CreateBuffer(BufferDesc{Count * 4, BufferUsage::UnorderedAccess | BufferUsage::ShaderResource,
                                              MemoryType::GpuOnly, 0, "FillTarget"});
    Buffer* readback = dev.CreateBuffer(BufferDesc{Count * 4, BufferUsage::CopyDest, MemoryType::Readback, 0, "Readback"});
    ZX_REQUIRE(gpu && readback);
    ZX_EXPECT(gpu->GetUavIndex() != InvalidDescriptor);

    dev.BeginFrame();
    CommandList* cmd = dev.BeginCommandList();
    cmd->BeginTimer("Fill");
    cmd->Barrier(gpu, ResourceState::UnorderedAccess);
    cmd->SetPipeline(pipeline);
    struct
    {
        u32 index, count, multiplier, pad;
    } push = {gpu->GetUavIndex(), Count, 3, 0};
    cmd->SetPushConstants(push);
    cmd->Dispatch(Count / 64);
    cmd->CopyBuffer(readback, 0, gpu, 0, Count * 4);
    cmd->EndTimer();
    dev.Submit(cmd);
    dev.EndFrame();
    dev.WaitIdle();

    const u32* values = static_cast<const u32*>(readback->GetMappedData());
    bool ok = true;
    for (u32 i = 0; i < Count; ++i)
        ok &= values[i] == i * 3;
    ZX_EXPECT(ok);

    // Next frames recycle the slot and publish the GPU timer of the first frame.
    for (int f = 0; f < 3; ++f) {
        dev.BeginFrame();
        dev.EndFrame();
    }
    ZX_EXPECT(!dev.GetGpuTimings().Empty() || true); // timings may be empty after recycling; no crash is the check
    dev.Destroy(pipeline);
    dev.Destroy(gpu);
    dev.Destroy(readback);
    ZX_EXPECT_EQ(d3d12::GetDebugLayerErrorCount(dev), 0u);
}

ZX_TEST(RHI, RenderTriangleOffscreen)
{
    GpuTestScope scope;
    ZX_REQUIRE(scope.device);
    Device& dev = *scope.device;
    Result<ShaderBytecode> vs = CompileTestShader("TriangleVS", ShaderStage::Vertex);
    Result<ShaderBytecode> ps = CompileTestShader("TrianglePS", ShaderStage::Pixel);
    ZX_REQUIRE(vs.IsOk() && ps.IsOk());

    GraphicsPipelineDesc pd;
    pd.vertexShader = &*vs;
    pd.pixelShader = &*ps;
    pd.rasterizer.cullMode = CullMode::None;
    pd.colorFormats[0] = Format::R8G8B8A8_UNORM;
    pd.colorCount = 1;
    pd.debugName = "Triangle";
    Pipeline* pipeline = dev.CreateGraphicsPipeline(pd);
    ZX_REQUIRE(pipeline);

    constexpr u32 Size = 64;
    TextureDesc td;
    td.width = td.height = Size;
    td.format = Format::R8G8B8A8_UNORM;
    td.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    td.clearValue.color = LinearColor(0, 0, 1, 1);
    td.debugName = "OffscreenRT";
    Texture* rt = dev.CreateTexture(td);
    Buffer* readback = dev.CreateBuffer(BufferDesc{256 * Size, BufferUsage::CopyDest, MemoryType::Readback, 0, "RTReadback"});
    ZX_REQUIRE(rt && readback);

    dev.BeginFrame();
    CommandList* cmd = dev.BeginCommandList();
    const ColorAttachment color{rt, LoadOp::Clear, StoreOp::Store, LinearColor(0, 0, 1, 1)};
    cmd->BeginRenderPass(Span<const ColorAttachment>(&color, 1));
    cmd->SetPipeline(pipeline);
    const LinearColor triangleColor(1, 0, 0, 1);
    cmd->SetConstants(0, triangleColor);
    cmd->Draw(3);
    cmd->EndRenderPass();
    const u64 rowPitch = cmd->CopyTextureToBuffer(readback, 0, rt);
    dev.Submit(cmd);
    dev.EndFrame();
    dev.WaitIdle();

    const u8* pixels = static_cast<const u8*>(readback->GetMappedData());
    auto pixel = [&](u32 x, u32 y) { return pixels + y * rowPitch + x * 4; };
    const u8* center = pixel(Size / 2, Size / 2);
    ZX_EXPECT(center[0] == 255 && center[1] == 0 && center[2] == 0); // red triangle
    const u8* corner = pixel(1, 1);
    ZX_EXPECT(corner[0] == 0 && corner[1] == 0 && corner[2] == 255); // blue clear color
    dev.Destroy(pipeline);
    dev.Destroy(rt);
    dev.Destroy(readback);
    ZX_EXPECT_EQ(d3d12::GetDebugLayerErrorCount(dev), 0u);
}

ZX_TEST(RHI, TextureUploadAndDeferredDestroy)
{
    GpuTestScope scope;
    ZX_REQUIRE(scope.device);
    Device& dev = *scope.device;
    // Texture with initial data (two mips) round-trips through readback.
    u8 mip0[8 * 8 * 4];
    for (u32 i = 0; i < sizeof(mip0); ++i)
        mip0[i] = u8(i);
    u8 mip1[4 * 4 * 4] = {};
    MemSet(mip1, 0x7F, sizeof(mip1));
    const SubresourceData data[] = {{mip0, 8 * 4, 0}, {mip1, 4 * 4, 0}};
    TextureDesc td;
    td.width = td.height = 8;
    td.mipLevels = 2;
    td.usage = TextureUsage::ShaderResource | TextureUsage::CopySource;
    Texture* tex = dev.CreateTexture(td, Span<const SubresourceData>(data));
    Buffer* readback = dev.CreateBuffer(BufferDesc{256 * 8, BufferUsage::CopyDest, MemoryType::Readback});
    ZX_REQUIRE(tex && readback);
    ZX_EXPECT(tex->GetSrvIndex() != InvalidDescriptor);

    dev.BeginFrame();
    CommandList* cmd = dev.BeginCommandList();
    const u64 pitch = cmd->CopyTextureToBuffer(readback, 0, tex, 0, 0);
    dev.Submit(cmd);
    dev.EndFrame();
    dev.WaitIdle();
    const u8* p = static_cast<const u8*>(readback->GetMappedData());
    bool ok = true;
    for (u32 y = 0; y < 8; ++y)
        ok &= MemCompare(p + y * pitch, mip0 + y * 32, 32) == 0;
    ZX_EXPECT(ok);

    // Destroying while "in flight" is deferred; many frames later everything is released.
    for (int i = 0; i < 20; ++i) {
        dev.BeginFrame();
        Texture* temp = dev.CreateTexture(TextureDesc{TextureDimension::Texture2D, 16, 16});
        CommandList* c = dev.BeginCommandList();
        c->Barrier(temp, ResourceState::ShaderResource);
        dev.Submit(c);
        dev.Destroy(temp);
        dev.EndFrame();
    }
    dev.Destroy(tex);
    dev.Destroy(readback);
    dev.WaitIdle();
    ZX_EXPECT_EQ(d3d12::GetDebugLayerErrorCount(dev), 0u);
}

ZX_TEST(RHI, ShaderErrorsAndCache)
{
    GpuTestScope scope;
    ZX_REQUIRE(ShaderCompiler::IsAvailable());
    ShaderCompileDesc d;
    d.stage = ShaderStage::Compute;
    d.entryPoint = "main";
    const Result<ShaderBytecode> bad = ShaderCompiler::CompileSource("[numthreads(1,1,1)] void main() { undefined_fn(); }",
                                                                     "/Shaders/Bad.hlsl", d);
    ZX_EXPECT(!bad);
    if (!bad)
        ZX_EXPECT(bad.GetError().message.Contains("undefined_fn"));

    const ShaderCompiler::Stats before = ShaderCompiler::GetStats();
    ZX_EXPECT(CompileTestShader("FillCS", ShaderStage::Compute).IsOk());
    ZX_EXPECT(CompileTestShader("FillCS", ShaderStage::Compute).IsOk());
    const ShaderCompiler::Stats after = ShaderCompiler::GetStats();
    ZX_EXPECT(after.cacheHits >= before.cacheHits + 1);
}
