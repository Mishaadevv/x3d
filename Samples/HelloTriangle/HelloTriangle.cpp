// HelloTriangle (M2 sample): window + D3D12 RHI + bindless textures + vertex pulling.
// Controls: LMB drag - orbit, wheel - zoom, F11 - fullscreen, V - toggle vsync, Esc - quit.
// Command line: -warp (software GPU), -debuglayer, -novsync, -autotest[=frames], -screenshot=<path>.

#include "Core/Console/Console.h"
#include "Core/CoreInit.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Image/Png.h"
#include "Core/Log/Log.h"
#include "Core/Math/Math.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Parse.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include "RHI/EngineProfile.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSample, Info);

using namespace zx;
using namespace zx::rhi;

namespace {

struct Vertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};
static_assert(sizeof(Vertex) == 32);

struct FrameData
{
    Mat4 viewProj;
    Vec3 lightDirection;
    f32 time;
    Vec3 cameraPosition;
    f32 ambient;
};

struct DrawData
{
    Mat4 model;
    u32 vertexBuffer;
    u32 texture;
    u32 pad0;
    u32 pad1;
};
static_assert(sizeof(DrawData) <= MaxPushConstants * 4);

/// Unit cube with outward normals and clockwise (front-facing) winding as seen from outside.
void BuildCube(Array<Vertex>& vertices, Array<u16>& indices)
{
    const Vec3 normals[6] = {Vec3::UnitX(), -Vec3::UnitX(), Vec3::UnitY(), -Vec3::UnitY(), Vec3::UnitZ(), -Vec3::UnitZ()};
    for (const Vec3& n : normals) {
        const Vec3 viewDir = -n; // direction a viewer looks when facing this side
        const Vec3 upHint = Abs(n.y) < 0.9f ? Vec3::Up() : Vec3::Forward();
        const Vec3 right = Normalize(Cross(upHint, viewDir));
        const Vec3 up = Cross(viewDir, right);
        const Vec3 c = n * 0.5f;
        const u16 base = u16(vertices.Size());
        vertices.PushBack({c - right * 0.5f - up * 0.5f, n, {0, 1}});
        vertices.PushBack({c - right * 0.5f + up * 0.5f, n, {0, 0}});
        vertices.PushBack({c + right * 0.5f + up * 0.5f, n, {1, 0}});
        vertices.PushBack({c + right * 0.5f - up * 0.5f, n, {1, 1}});
        indices.Append({base, u16(base + 1), u16(base + 2), base, u16(base + 2), u16(base + 3)});
    }
}

/// Procedural checker + gradient texture with a full mip chain (box filtered).
Texture* CreateCheckerTexture(Device& device)
{
    constexpr u32 Size = 256;
    const u32 mips = MipCount(Size, Size);
    Array<Array<u8>> levels;
    levels.Resize(mips);
    levels[0].ResizeUninitialized(Size * Size * 4);
    for (u32 y = 0; y < Size; ++y) {
        for (u32 x = 0; x < Size; ++x) {
            const bool check = ((x / 32) + (y / 32)) % 2 == 0;
            u8* p = &levels[0][(y * Size + x) * 4];
            const LinearColor a = LinearColor::FromHex("#E8A33D"); // warm orange
            const LinearColor b = LinearColor::FromHex("#2E3A59"); // deep blue
            const Color c = Color::FromLinear(Lerp(check ? a : b, LinearColor::White(), f32(y) / Size * 0.25f));
            p[0] = c.r;
            p[1] = c.g;
            p[2] = c.b;
            p[3] = 255;
        }
    }
    for (u32 m = 1; m < mips; ++m) {
        const u32 s = Size >> m, ps = Size >> (m - 1);
        levels[m].ResizeUninitialized(s * s * 4);
        for (u32 y = 0; y < s; ++y)
            for (u32 x = 0; x < s; ++x)
                for (u32 ch = 0; ch < 4; ++ch) {
                    // Average in linear space for correct mips of sRGB data.
                    f32 sum = 0.0f;
                    for (u32 dy = 0; dy < 2; ++dy)
                        for (u32 dx = 0; dx < 2; ++dx) {
                            const u8 v = levels[m - 1][((y * 2 + dy) * ps + (x * 2 + dx)) * 4 + ch];
                            sum += ch < 3 ? SRGB8ToLinear(v) : f32(v) / 255.0f;
                        }
                    sum *= 0.25f;
                    levels[m][(y * s + x) * 4 + ch] = u8((ch < 3 ? LinearToSRGB(sum) : sum) * 255.0f + 0.5f);
                }
    }
    Array<SubresourceData> data;
    for (u32 m = 0; m < mips; ++m)
        data.PushBack(SubresourceData{levels[m].Data(), u64(Size >> m) * 4, 0});
    TextureDesc td;
    td.width = td.height = Size;
    td.mipLevels = mips;
    td.format = Format::R8G8B8A8_SRGB;
    td.usage = TextureUsage::ShaderResource;
    td.debugName = "Checker";
    return device.CreateTexture(td, data.View());
}

Texture* CreateDepth(Device& device, UVec2 size)
{
    TextureDesc td;
    td.width = size.x;
    td.height = size.y;
    td.format = Format::D32_FLOAT;
    td.usage = TextureUsage::DepthStencil;
    td.clearValue.depth = 0.0f;
    td.debugName = "Depth";
    return device.CreateTexture(td);
}

int Run()
{
    WindowDesc wd;
    wd.title = "ZeqouX3D - HelloTriangle";
    wd.width = 1280;
    wd.height = 720;
    UniquePtr<Window> window(Window::Create(wd));
    if (!window)
        return 1;

    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG || CommandLine::HasFlag("debuglayer");
    dd.useWarp = CommandLine::HasFlag("warp");
    UniquePtr<Device> device = d3d12::CreateD3D12Device(dd);
    if (!device) {
        platform::ShowErrorMessageBox("ZeqouX3D", "Failed to create a Direct3D 12 device.");
        return 1;
    }
    SelectEngineProfile(device->GetCaps());
    UVec2 size = window->GetClientSize();
    UniquePtr<SwapChain> swapChain = device->CreateSwapChain(
        SwapChainDesc{window->GetNativeHandle(), size.x, size.y, 3, Format::R8G8B8A8_UNORM, !CommandLine::HasFlag("novsync")});
    if (!swapChain)
        return 1;

    ShaderCompileDesc sd;
    sd.path = "/Shaders/Samples/HelloCube.hlsl";
    sd.entryPoint = "VSMain";
    sd.stage = ShaderStage::Vertex;
    Result<ShaderBytecode> vs = ShaderCompiler::Compile(sd);
    sd.entryPoint = "PSMain";
    sd.stage = ShaderStage::Pixel;
    Result<ShaderBytecode> ps = ShaderCompiler::Compile(sd);
    if (!vs || !ps) {
        ZX_LOG_ERROR(LogSample, "{}", !vs ? vs.GetError().message : ps.GetError().message);
        return 1;
    }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &*vs;
    pd.pixelShader = &*ps;
    pd.colorFormats[0] = swapChain->GetFormat();
    pd.colorCount = 1;
    pd.depthFormat = Format::D32_FLOAT;
    pd.debugName = "HelloCube";
    Pipeline* pipeline = device->CreateGraphicsPipeline(pd);
    if (!pipeline)
        return 1;

    Array<Vertex> vertices;
    Array<u16> indices;
    BuildCube(vertices, indices);
    Buffer* vertexBuffer = device->CreateBuffer(
        BufferDesc{vertices.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, "CubeVB"}, vertices.Data());
    Buffer* indexBuffer =
        device->CreateBuffer(BufferDesc{indices.SizeBytes(), BufferUsage::Index, MemoryType::GpuOnly, 0, "CubeIB"},
                             indices.Data());
    Texture* checker = CreateCheckerTexture(*device);
    Texture* depth = CreateDepth(*device, swapChain->GetSize());

    StringView autotestValue;
    const bool autotest = CommandLine::HasFlag("autotest") || CommandLine::GetValue("autotest", autotestValue);
    const u64 autotestFrames = autotestValue.Empty() ? 120 : u64(Max<i64>(1, ParseInt(autotestValue).ValueOr(120)));

    // -screenshot=<path>: capture the back buffer of frame `screenshotFrame` to a PNG (visual checks / golden images).
    StringView screenshotPath;
    const bool takeScreenshot = CommandLine::GetValue("screenshot", screenshotPath);
    const u64 screenshotFrame = autotest ? autotestFrames - 1 : 60;
    Buffer* screenshotBuffer = nullptr;
    u64 screenshotPitch = 0;
    UVec2 screenshotSize;

    InputState input;
    Array<WindowEvent> events;
    FrameTimer timer;
    f32 yaw = 0.6f, pitch = 0.45f, distance = 7.0f;
    f64 titleTimer = 0.0;
    u64 frames = 0;
    f64 cpuMsSum = 0.0, gpuMsSum = 0.0;
    u64 statFrames = 0;

    while (!window->ShouldClose() && !device->IsDeviceLost()) {
        if (!platform::PumpMessages())
            break;
        profiler::BeginFrame();
        const f32 dt = timer.Tick();
        events.Clear();
        platform::UpdateInput(*window, input, events);
        bool resized = false;
        for (const WindowEvent& e : events) {
            if (e.type == WindowEventType::Resize)
                resized = true;
            if (e.type == WindowEventType::KeyDown && !e.repeat) {
                if (e.key == Key::F11)
                    window->SetMode(window->GetMode() == WindowMode::Windowed ? WindowMode::BorderlessFullscreen
                                                                             : WindowMode::Windowed);
                if (e.key == Key::V)
                    swapChain->SetVSync(!swapChain->GetVSync());
                if (e.key == Key::Escape)
                    window->RequestClose();
            }
        }
        if (input.IsMouseDown(MouseButton::Left)) {
            yaw += input.MouseDelta().x * 0.005f;
            pitch = Clamp(pitch + input.MouseDelta().y * 0.005f, -1.4f, 1.4f);
        }
        distance = Clamp(distance * Pow(0.9f, input.MouseWheel()), 2.0f, 30.0f);

        const UVec2 clientSize = window->GetClientSize();
        if (clientSize.x == 0 || clientSize.y == 0 || window->IsMinimized()) {
            Thread::SleepMs(16);
            continue;
        }
        if (resized || clientSize != swapChain->GetSize()) {
            swapChain->Resize(clientSize.x, clientSize.y);
            device->Destroy(depth);
            depth = CreateDepth(*device, swapChain->GetSize());
        }

        const UVec2 sz = swapChain->GetSize();
        const Vec3 eye = Vec3(Sin(yaw) * Cos(pitch), Sin(pitch), -Cos(yaw) * Cos(pitch)) * distance;
        FrameData frame;
        frame.viewProj = Mat4::PerspectiveInfiniteReversedZ(DegToRad(60.0f), f32(sz.x) / f32(sz.y), 0.05f) *
                         Mat4::LookAt(eye, Vec3::Zero());
        frame.lightDirection = Normalize(Vec3(-0.4f, -1.0f, 0.6f));
        frame.time = f32(timer.TotalSeconds());
        frame.cameraPosition = eye;
        frame.ambient = 0.12f;

        device->BeginFrame();
        CommandList* cmd = device->BeginCommandList();
        cmd->BeginTimer("Frame");
        Texture* backBuffer = swapChain->GetCurrentBackBuffer();
        const ColorAttachment color{backBuffer, LoadOp::Clear, StoreOp::Store, LinearColor(0.02f, 0.025f, 0.035f, 1)};
        const DepthAttachment depthAttachment{depth, LoadOp::Clear, StoreOp::DontCare, 0.0f};
        cmd->BeginRenderPass(Span<const ColorAttachment>(&color, 1), &depthAttachment);
        cmd->SetPipeline(pipeline);
        cmd->SetConstants(0, frame);
        cmd->SetIndexBuffer(indexBuffer, IndexFormat::UInt16);
        cmd->Barrier(checker, ResourceState::ShaderResource);
        cmd->Barrier(vertexBuffer, ResourceState::ShaderResource);
        for (i32 z = -1; z <= 1; ++z) {
            for (i32 x = -1; x <= 1; ++x) {
                const f32 phase = frame.time * 0.7f + f32(x * 3 + z) * 0.4f;
                DrawData draw;
                draw.model = Mat4::TRS(Vec3(f32(x) * 2.0f, Sin(phase) * 0.3f, f32(z) * 2.0f),
                                       Quat::FromEuler(phase * 0.5f, phase, 0.0f), Vec3(x == 0 && z == 0 ? 1.4f : 1.0f));
                draw.vertexBuffer = vertexBuffer->GetSrvIndex();
                draw.texture = checker->GetSrvIndex();
                draw.pad0 = draw.pad1 = 0;
                cmd->SetPushConstants(draw);
                cmd->DrawIndexed(u32(indices.Size()));
            }
        }
        cmd->EndRenderPass();
        if (takeScreenshot && frames == screenshotFrame && !screenshotBuffer) {
            screenshotSize = sz;
            screenshotBuffer = device->CreateBuffer(
                BufferDesc{AlignUp(u64(sz.x) * 4, u64(256)) * sz.y, BufferUsage::CopyDest, MemoryType::Readback, 0, "Screenshot"});
            screenshotPitch = cmd->CopyTextureToBuffer(screenshotBuffer, 0, backBuffer);
        }
        cmd->Barrier(backBuffer, ResourceState::Present);
        cmd->EndTimer();
        device->Submit(cmd);
        swapChain->Present();
        device->EndFrame();

        if (frames > 10) { // skip warm-up (shader/PSO creation, first uploads)
            cpuMsSum += f64(timer.UnscaledDeltaSeconds()) * 1000.0;
            for (const GpuTiming& t : device->GetGpuTimings())
                gpuMsSum += t.milliseconds;
            ++statFrames;
        }
        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0.0;
            f64 gpuMs = 0.0;
            for (const GpuTiming& t : device->GetGpuTimings())
                if (StringView(t.name) == "Frame")
                    gpuMs += t.milliseconds;
            window->SetTitle(String::Format("ZeqouX3D - HelloTriangle | {} | {}x{} | CPU {:.2f} ms | GPU {:.2f} ms | VSync {}",
                                            device->GetCaps().adapter.name, sz.x, sz.y,
                                            timer.SmoothedDeltaSeconds() * 1000.0f, gpuMs,
                                            swapChain->GetVSync() ? "on" : "off"));
        }
        ++frames;
        if (autotest) {
            // Exercise swapchain recreation paths under the debug layer.
            if (frames == autotestFrames / 4)
                window->SetMode(WindowMode::BorderlessFullscreen);
            else if (frames == autotestFrames / 2)
                window->SetMode(WindowMode::Windowed);
            else if (frames == autotestFrames * 3 / 4 - 10)
                window->SetClientSize(800, 600);
            if (frames >= autotestFrames)
                window->RequestClose();
        }
    }

    device->WaitIdle();
    if (screenshotBuffer) {
        const bool ok = png::WriteFile(screenshotPath, static_cast<const u8*>(screenshotBuffer->GetMappedData()),
                                       screenshotSize.x, screenshotSize.y, 4, usize(screenshotPitch));
        ZX_LOG_INFO(LogSample, "Screenshot {} -> {}", ok ? "saved" : "FAILED", screenshotPath);
        device->Destroy(screenshotBuffer);
    }
    const u32 debugErrors = d3d12::GetDebugLayerErrorCount(*device);
    device->Destroy(pipeline);
    device->Destroy(vertexBuffer);
    device->Destroy(indexBuffer);
    device->Destroy(checker);
    device->Destroy(depth);
    swapChain.Reset();
    device.Reset();
    ZX_LOG_INFO(LogSample, "Exited after {} frames, debug layer errors: {}", frames, debugErrors);
    if (statFrames > 0)
        ZX_LOG_INFO(LogSample, "Average frame: CPU {:.3f} ms, GPU {:.3f} ms", cpuMsSum / f64(statFrames),
                    gpuMsSum / f64(statFrames));
    return debugErrors == 0 ? 0 : 3;
}

} // namespace

int main()
{
    if (!CoreInit(CoreInitParams{.appName = "HelloTriangle"}))
        return 1;
    platform::InitializePlatform();
    ShaderCompiler::Initialize();
    const int result = Run();
    ShaderCompiler::Shutdown();
    platform::ShutdownPlatform();
    const u64 leaks = CoreShutdown();
    return result != 0 ? result : (leaks == 0 ? 0 : 2);
}
