// SceneDemo (M3 sample): render graph, HDR scene color + tonemapping, 1000 cubes with BC-compressed procedural
// textures and a texture loaded from a JPEG file, debug draw, fly camera, debug views of graph resources.
//
// Controls: RMB + mouse look, WASD/QE move, Shift fast, wheel speed; F1 help in log; F2 cycle view
// (final / HDR raw / depth / luminance); F3 debug draw; F4 dump render graph; R pause rotation; F11 fullscreen.
// Command line: -warp, -debuglayer, -novsync, -lite/-full, -autotest[=frames], -screenshot=<path>.

#include "Core/Console/Console.h"
#include "Core/CoreInit.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Image/Png.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Log/Log.h"
#include "Core/Math/Math.h"
#include "Core/Profiler/Profiler.h"
#include "Core/String/Parse.h"
#include "Core/Threading/Thread.h"
#include "Core/Time/Time.h"
#include "Engine/FlyCamera.h"
#include "Platform/Platform.h"
#include "Platform/Window.h"
#include "Render/DebugDraw.h"
#include "Render/RenderGraph.h"
#include "Render/TextureLoader.h"
#include "RHI/EngineProfile.h"
#include "RHI/ShaderCompiler.h"
#include "RHI_D3D12/D3D12Backend.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogSceneDemo, Info);

using namespace zx;
using namespace zx::rhi;
using namespace zx::render;

namespace {

struct Vertex
{
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
};

struct FrameData
{
    Mat4 viewProj;
    Vec3 lightDirection;
    f32 lightIntensity;
    Vec3 cameraPosition;
    f32 ambient;
    Vec3 skyColor;
    f32 pad;
};

struct DrawData
{
    Mat4 model;
    u32 vertexBuffer;
    u32 texture;
    u32 tint;
    u32 pad;
};

struct CompositeData
{
    u32 sceneColor;
    u32 depth;
    u32 viewMode;
    f32 exposure;
};

struct Cube
{
    Vec3 position;
    Vec3 rotationAxis;
    f32 rotationSpeed;
    f32 scale;
    u32 texture;
    u32 tint;
};

void BuildCube(Array<Vertex>& vertices, Array<u16>& indices)
{
    const Vec3 normals[6] = {Vec3::UnitX(), -Vec3::UnitX(), Vec3::UnitY(), -Vec3::UnitY(), Vec3::UnitZ(), -Vec3::UnitZ()};
    for (const Vec3& n : normals) {
        const Vec3 viewDir = -n;
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

/// Procedural 256x256 texture variant `kind` using Core noise functions.
Image MakeProceduralTexture(u32 kind)
{
    constexpr u32 Size = 256;
    Image img = Image::Create(Size, Size, ImageFormat::RGBA8);
    Random rng(kind * 7919 + 13);
    const LinearColor a = LinearColor::FromHSV(rng.NextFloat(), 0.55f, 0.85f);
    const LinearColor b = LinearColor::FromHSV(rng.NextFloat(), 0.45f, 0.35f);
    jobs::ParallelFor(Size, 16, [&](u32 y0, u32 y1) {
        for (u32 y = y0; y < y1; ++y)
            for (u32 x = 0; x < Size; ++x) {
                const Vec2 p(f32(x) / Size, f32(y) / Size);
                f32 t;
                switch (kind % 6) {
                case 0: t = noise::Fbm(p * 8.0f, 5, 2.0f, 0.5f, kind) * 0.5f + 0.5f; break;           // clouds
                case 1: t = Saturate(noise::Worley(p * 10.0f, kind) * 1.2f); break;                        // cells
                case 2: t = Sin((p.x + noise::Fbm(p * 4.0f, 4, 2.0f, 0.5f, kind) * 0.3f) * 40.0f) * 0.5f + 0.5f; break; // marble
                case 3: t = ((x / 32 + y / 32) % 2) ? 0.85f : 0.15f; break;                              // checker
                case 4: t = noise::Ridged(p * 6.0f, 5, 2.0f, 0.5f, kind); break;                          // ridges
                default: t = Frac(p.y * 8.0f + noise::Simplex(p * 3.0f, kind) * 0.5f) > 0.5f ? 0.8f : 0.25f; break; // stripes
                }
                const Color c = Color::FromLinear(Lerp(b, a, Saturate(t)));
                u8* px = img.Row(y) + usize(x) * 4;
                px[0] = c.r;
                px[1] = c.g;
                px[2] = c.b;
                px[3] = 255;
            }
    });
    return img;
}

Texture* CreateRT(Device& device, UVec2 size, Format format, TextureUsage usage, StringView name)
{
    TextureDesc d;
    d.width = size.x;
    d.height = size.y;
    d.format = format;
    d.usage = usage;
    d.debugName = name;
    return device.CreateTexture(d);
}

Result<ShaderBytecode> Compile(StringView path, StringView entry, ShaderStage stage)
{
    ShaderCompileDesc sd;
    sd.path = path;
    sd.entryPoint = entry;
    sd.stage = stage;
    return ShaderCompiler::Compile(sd);
}

int Run()
{
    WindowDesc wd;
    wd.title = "ZeqouX3D - SceneDemo";
    UniquePtr<Window> window(Window::Create(wd));
    if (!window)
        return 1;
    DeviceDesc dd;
    dd.enableDebugLayer = ZX_BUILD_DEBUG || CommandLine::HasFlag("debuglayer");
    dd.useWarp = CommandLine::HasFlag("warp");
    UniquePtr<Device> device = d3d12::CreateD3D12Device(dd);
    if (!device)
        return 1;
    const EngineProfile profile = SelectEngineProfile(device->GetCaps());
    UVec2 size = window->GetClientSize();
    UniquePtr<SwapChain> swapChain = device->CreateSwapChain(
        SwapChainDesc{window->GetNativeHandle(), size.x, size.y, 3, Format::R8G8B8A8_UNORM, !CommandLine::HasFlag("novsync")});

    // --- Pipelines ---
    const Format hdrFormat = Format::R16G16B16A16_FLOAT;
    const Format depthFormat = Format::D32_FLOAT;
    Result<ShaderBytecode> sceneVS = Compile("/Shaders/Samples/SceneDemo.hlsl", "VSMain", ShaderStage::Vertex);
    Result<ShaderBytecode> scenePS = Compile("/Shaders/Samples/SceneDemo.hlsl", "PSMain", ShaderStage::Pixel);
    Result<ShaderBytecode> fullscreenVS = Compile("/Shaders/Post/Composite.hlsl", "FullscreenVS", ShaderStage::Vertex);
    Result<ShaderBytecode> compositePS = Compile("/Shaders/Post/Composite.hlsl", "CompositePS", ShaderStage::Pixel);
    for (const Result<ShaderBytecode>* r : {&sceneVS, &scenePS, &fullscreenVS, &compositePS})
        if (!*r) {
            ZX_LOG_ERROR(LogSceneDemo, "{}", r->GetError().message);
            return 1;
        }
    GraphicsPipelineDesc pd;
    pd.vertexShader = &*sceneVS;
    pd.pixelShader = &*scenePS;
    pd.colorFormats[0] = hdrFormat;
    pd.colorCount = 1;
    pd.depthFormat = depthFormat;
    pd.debugName = "SceneOpaque";
    Pipeline* scenePipeline = device->CreateGraphicsPipeline(pd);
    GraphicsPipelineDesc cd;
    cd.vertexShader = &*fullscreenVS;
    cd.pixelShader = &*compositePS;
    cd.rasterizer.cullMode = CullMode::None;
    cd.depthStencil.depthTest = false;
    cd.colorFormats[0] = swapChain->GetFormat();
    cd.colorCount = 1;
    cd.debugName = "Composite";
    Pipeline* compositePipeline = device->CreateGraphicsPipeline(cd);
    DebugDraw debugDraw(*device);
    if (!scenePipeline || !compositePipeline || !debugDraw.Init(hdrFormat, depthFormat))
        return 1;

    // --- Geometry & textures ---
    Array<Vertex> vertices;
    Array<u16> indices;
    BuildCube(vertices, indices);
    Buffer* vb = device->CreateBuffer(BufferDesc{vertices.SizeBytes(), BufferUsage::ShaderResource, MemoryType::GpuOnly, 0, "CubeVB"},
                                      vertices.Data());
    Buffer* ib = device->CreateBuffer(BufferDesc{indices.SizeBytes(), BufferUsage::Index, MemoryType::GpuOnly, 0, "CubeIB"},
                                      indices.Data());

    Stopwatch loadTimer;
    Array<Texture*> textures;
    constexpr u32 ProceduralCount = 12;
    Array<Image> images;
    images.Resize(ProceduralCount);
    for (u32 i = 0; i < ProceduralCount; ++i)
        images[i] = MakeProceduralTexture(i);
    for (u32 i = 0; i < ProceduralCount; ++i) {
        Result<TextureData> data = PrepareTexture(images[i], TextureLoadOptions{});
        if (data)
            textures.PushBack(CreateTexture(*device, *data, String::Format("Procedural{}", i)));
    }
    // A texture decoded from a real JPEG file (own decoder) via the VFS (physical path).
    Result<Texture*> fileTexture = LoadTexture(*device, FindEngineRootDirectory() + "/Tests/Data/Images/jpeg444.jpg");
    if (fileTexture)
        textures.PushBack(*fileTexture);
    else
        ZX_LOG_WARN(LogSceneDemo, "{}", fileTexture.GetError().message);
    ZX_LOG_INFO(LogSceneDemo, "Prepared {} textures (mips + BC1) in {:.1f} ms", textures.Size(), loadTimer.ElapsedMilliseconds());

    Array<Cube> cubes;
    Random rng(2026);
    constexpr i32 GridSize = 10;
    for (i32 z = 0; z < GridSize; ++z)
        for (i32 y = 0; y < GridSize; ++y)
            for (i32 x = 0; x < GridSize; ++x) {
                Cube c;
                c.position = Vec3(f32(x - GridSize / 2) * 3.0f, f32(y) * 3.0f + 1.0f, f32(z - GridSize / 2) * 3.0f);
                c.rotationAxis = rng.OnUnitSphere();
                c.rotationSpeed = rng.Range(-1.0f, 1.0f);
                c.scale = rng.Range(0.7f, 1.6f);
                c.texture = u32(rng.NextBounded(u32(textures.Size())));
                c.tint = Color::FromLinear(LinearColor::FromHSV(rng.NextFloat(), 0.15f, 1.0f)).ToPackedABGR();
                cubes.PushBack(c);
            }

    TransientResourcePool pool(*device);
    FlyCamera camera;
    camera.position = Vec3(0, 18, -42);
    camera.LookAt(Vec3(0, 12, 0));

    StringView autotestValue, screenshotPath;
    const bool autotest = CommandLine::HasFlag("autotest") || CommandLine::GetValue("autotest", autotestValue);
    const u64 autotestFrames = autotestValue.Empty() ? 120 : u64(Max<i64>(1, ParseInt(autotestValue).ValueOr(120)));
    const bool takeScreenshot = CommandLine::GetValue("screenshot", screenshotPath);
    Buffer* screenshotBuffer = nullptr;
    u64 screenshotPitch = 0;
    UVec2 screenshotSize;

    InputState input;
    Array<WindowEvent> events;
    FrameTimer timer;
    u32 viewMode = 0;
    bool showDebug = true, rotate = true, dumpGraph = false;
    f64 animTime = 0.0, titleTimer = 0.0, cpuSum = 0.0, gpuSum = 0.0, workSum = 0.0;
    Stopwatch workTimer;
    f64 workMs = 0.0;
    u64 frames = 0, statFrames = 0;
    // Lite profile: render the scene at reduced resolution (TZ 1.2/1.3); composite upscales.
    const f32 renderScale = profile == EngineProfile::Lite ? 0.75f : 1.0f;

    while (!window->ShouldClose() && !device->IsDeviceLost()) {
        if (!platform::PumpMessages())
            break;
        profiler::BeginFrame();
        workTimer.Restart();
        const f32 dt = timer.Tick();
        events.Clear();
        platform::UpdateInput(*window, input, events);
        for (const WindowEvent& e : events) {
            if (e.type != WindowEventType::KeyDown || e.repeat)
                continue;
            switch (e.key) {
            case Key::F2: viewMode = (viewMode + 1) % 4; ZX_LOG_INFO(LogSceneDemo, "View mode {}", viewMode); break;
            case Key::F3: showDebug = !showDebug; break;
            case Key::F4: dumpGraph = true; break;
            case Key::R: rotate = !rotate; break;
            case Key::F11:
                window->SetMode(window->GetMode() == WindowMode::Windowed ? WindowMode::BorderlessFullscreen : WindowMode::Windowed);
                break;
            case Key::Escape: window->RequestClose(); break;
            default: break;
            }
        }
        window->SetCursorLocked(camera.Update(input, dt));
        if (rotate)
            animTime += dt;

        const UVec2 client = window->GetClientSize();
        if (client.x == 0 || client.y == 0 || window->IsMinimized()) {
            Thread::SleepMs(16);
            continue;
        }
        if (client != swapChain->GetSize())
            swapChain->Resize(client.x, client.y);
        const UVec2 outSize = swapChain->GetSize();
        const UVec2 renderSize(Max(1u, u32(f32(outSize.x) * renderScale)), Max(1u, u32(f32(outSize.y) * renderScale)));

        FrameData frame;
        frame.viewProj = camera.ProjectionMatrix(f32(renderSize.x) / f32(renderSize.y)) * camera.ViewMatrix();
        frame.lightDirection = Normalize(Vec3(-0.35f, -1.0f, 0.45f));
        frame.lightIntensity = 3.0f;
        frame.cameraPosition = camera.position;
        frame.ambient = 0.6f;
        frame.skyColor = Vec3(0.45f, 0.6f, 0.9f);
        frame.pad = 0;

        if (showDebug) {
            debugDraw.Grid(Vec3::Zero(), 60.0f, 30, Color(70, 75, 85));
            debugDraw.Axes(Mat4::Identity(), 3.0f);
            const Cube& c0 = cubes[555];
            debugDraw.Box(AABB::FromCenterExtents(c0.position, Vec3(c0.scale)), Color(255, 220, 60));
            debugDraw.Sphere(Vec3(0, 32, 0), 2.0f, Color(255, 120, 60));
            debugDraw.Arrow(Vec3(0, 34, 0), Vec3(0, 34, 0) + frame.lightDirection * 4.0f, Color(255, 255, 120));
        }

        workMs = workTimer.ElapsedMilliseconds(); // exclude the frame-fence wait and Present from CPU work
        device->BeginFrame();
        workTimer.Restart();
        CommandList* cmd = device->BeginCommandList();
        cmd->BeginTimer("Frame");
        Texture* backBuffer = swapChain->GetCurrentBackBuffer();
        RenderGraph graph(*device, pool);
        const RGTexture output = graph.ImportTexture(backBuffer, takeScreenshot && frames == (autotest ? autotestFrames - 1 : 60)
                                                                     ? ResourceState::CopySource
                                                                     : ResourceState::Present);
        TextureDesc hdrDesc;
        hdrDesc.width = renderSize.x;
        hdrDesc.height = renderSize.y;
        hdrDesc.format = hdrFormat;
        hdrDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
        hdrDesc.debugName = "SceneColor";
        const RGTexture sceneColor = graph.CreateTexture(hdrDesc);
        TextureDesc depthDesc = hdrDesc;
        depthDesc.format = depthFormat;
        depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
        depthDesc.debugName = "SceneDepth";
        const RGTexture sceneDepth = graph.CreateTexture(depthDesc);

        graph.AddPass("Opaque", PassType::Graphics,
                      [&](PassBuilder& b) {
                          b.RenderTarget(sceneColor, LoadOp::Clear, LinearColor(frame.skyColor * 0.6f));
                          b.DepthStencil(sceneDepth, LoadOp::Clear, 0.0f);
                      },
                      [&](PassContext& ctx) {
                          ctx.cmd.SetPipeline(scenePipeline);
                          ctx.cmd.SetConstants(0, frame);
                          ctx.cmd.SetIndexBuffer(ib, IndexFormat::UInt16);
                          for (const Cube& c : cubes) {
                              DrawData draw;
                              draw.model = Mat4::TRS(c.position, Quat::FromAxisAngle(c.rotationAxis, f32(animTime) * c.rotationSpeed),
                                                     Vec3(c.scale));
                              draw.vertexBuffer = vb->GetSrvIndex();
                              draw.texture = textures[c.texture]->GetSrvIndex();
                              draw.tint = c.tint;
                              draw.pad = 0;
                              ctx.cmd.SetPushConstants(draw);
                              ctx.cmd.DrawIndexed(u32(indices.Size()));
                          }
                      });
        graph.AddPass("DebugDraw", PassType::Graphics,
                      [&](PassBuilder& b) {
                          b.RenderTarget(sceneColor, LoadOp::Load);
                          b.DepthStencil(sceneDepth, LoadOp::Load, 0.0f, true);
                      },
                      [&](PassContext& ctx) { debugDraw.Render(ctx.cmd, frame.viewProj); });
        graph.AddPass("Composite", PassType::Graphics,
                      [&](PassBuilder& b) {
                          b.ReadTexture(sceneColor);
                          b.ReadTexture(sceneDepth);
                          b.RenderTarget(output, LoadOp::DontCare);
                      },
                      [&](PassContext& ctx) {
                          ctx.cmd.SetPipeline(compositePipeline);
                          const CompositeData data{ctx.Texture(sceneColor)->GetSrvIndex(), ctx.Texture(sceneDepth)->GetSrvIndex(),
                                                   viewMode, 1.0f};
                          ctx.cmd.SetPushConstants(data);
                          ctx.cmd.Draw(3);
                      });
        graph.Execute(*cmd);
        if (dumpGraph) {
            ZX_LOG_INFO(LogSceneDemo, "Render graph:\n{}", graph.Dump());
            dumpGraph = false;
        }
        if (takeScreenshot && !screenshotBuffer && frames == (autotest ? autotestFrames - 1 : 60)) {
            screenshotSize = outSize;
            screenshotBuffer = device->CreateBuffer(BufferDesc{AlignUp(u64(outSize.x) * 4, u64(256)) * outSize.y,
                                                               BufferUsage::CopyDest, MemoryType::Readback, 0, "Screenshot"});
            screenshotPitch = cmd->CopyTextureToBuffer(screenshotBuffer, 0, backBuffer);
            cmd->Barrier(backBuffer, ResourceState::Present);
        }
        cmd->EndTimer();
        device->Submit(cmd);
        workMs += workTimer.ElapsedMilliseconds();
        swapChain->Present();
        device->EndFrame();

        if (frames > 10) {
            cpuSum += f64(timer.UnscaledDeltaSeconds()) * 1000.0;
            workSum += workMs;
            for (const GpuTiming& t : device->GetGpuTimings())
                if (StringView(t.name) == "Frame")
                    gpuSum += t.milliseconds;
            ++statFrames;
        }
        titleTimer += dt;
        if (titleTimer > 0.5) {
            titleTimer = 0.0;
            f64 gpuMs = 0.0;
            for (const GpuTiming& t : device->GetGpuTimings())
                if (StringView(t.name) == "Frame")
                    gpuMs += t.milliseconds;
            window->SetTitle(String::Format("ZeqouX3D - SceneDemo [{}] | {} cubes | {}x{} (render {}x{}) | CPU {:.2f} ms | GPU {:.2f} ms | view {}",
                                            EngineProfileName(profile), cubes.Size(), outSize.x, outSize.y, renderSize.x,
                                            renderSize.y, timer.SmoothedDeltaSeconds() * 1000.0f, gpuMs, viewMode));
        }
        ++frames;
        if (autotest) {
            if (frames == autotestFrames / 3)
                viewMode = 2; // exercise debug views
            else if (frames == autotestFrames / 3 + 10)
                viewMode = 0;
            if (frames >= autotestFrames)
                window->RequestClose();
        }
    }

    device->WaitIdle();
    if (screenshotBuffer) {
        const bool ok = png::WriteFile(screenshotPath, static_cast<const u8*>(screenshotBuffer->GetMappedData()), screenshotSize.x,
                                       screenshotSize.y, 4, usize(screenshotPitch));
        ZX_LOG_INFO(LogSceneDemo, "Screenshot {} -> {}", ok ? "saved" : "FAILED", screenshotPath);
        device->Destroy(screenshotBuffer);
    }
    const u32 debugErrors = d3d12::GetDebugLayerErrorCount(*device);
    ZX_LOG_INFO(LogSceneDemo, "Exited after {} frames, debug layer errors: {}", frames, debugErrors);
    if (statFrames)
        ZX_LOG_INFO(LogSceneDemo, "Average frame: {:.3f} ms (CPU work {:.3f} ms), GPU {:.3f} ms", cpuSum / f64(statFrames),
                    workSum / f64(statFrames), gpuSum / f64(statFrames));
    for (Texture* t : textures)
        device->Destroy(t);
    device->Destroy(vb);
    device->Destroy(ib);
    device->Destroy(scenePipeline);
    device->Destroy(compositePipeline);
    pool.Clear();
    return debugErrors == 0 ? 0 : 3;
}

} // namespace

int main()
{
    if (!CoreInit(CoreInitParams{.appName = "SceneDemo"}))
        return 1;
    platform::InitializePlatform();
    ShaderCompiler::Initialize();
    int result = 0;
    {
        result = Run();
    }
    ShaderCompiler::Shutdown();
    platform::ShutdownPlatform();
    const u64 leaks = CoreShutdown();
    return result != 0 ? result : (leaks == 0 ? 0 : 2);
}
