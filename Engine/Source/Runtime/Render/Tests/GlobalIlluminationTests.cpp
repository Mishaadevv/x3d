// Dynamic GI: every compute shader of the distance field and the probes compiles, and the constant layouts the C++
// side writes match what the shaders were written against.

#include "Core/CoreInit.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/Log/Log.h"
#include "Core/Testing/Test.h"
#include "Render/GlobalIllumination.h"
#include "RHI/ShaderCompiler.h"

using namespace zx;
using namespace zx::render;

ZX_DEFINE_LOG_CATEGORY_STATIC(LogGITest, Info);

ZX_TEST(GI, ShadersCompile)
{
    fs::Initialize(1);
    fs::Mount("/Shaders", FindEngineRootDirectory() + "/Engine/Shaders", 0, false);
    const String savedDir = platform::GetTempDirectory() + "/zx_gi_test";
    platform::CreateDirectoryTree(savedDir);
    fs::Mount("/Saved", savedDir);
    rhi::ShaderCompiler::Initialize();
    if (rhi::ShaderCompiler::IsAvailable()) {
        struct Entry
        {
            const char* path;
            const char* entry;
        } entries[] = {{"/Shaders/GI/GlobalSDF.hlsl", "ClearCS"},       {"/Shaders/GI/GlobalSDF.hlsl", "VoxelizeCS"},
                       {"/Shaders/GI/GlobalSDF.hlsl", "JumpFloodCS"},   {"/Shaders/GI/GlobalSDF.hlsl", "FinalizeCS"},
                       {"/Shaders/GI/DDGI.hlsl", "TraceCS"},            {"/Shaders/GI/DDGI.hlsl", "UpdateIrradianceCS"},
                       {"/Shaders/GI/DDGI.hlsl", "UpdateDistanceCS"},   {"/Shaders/GI/DDGI.hlsl", "AmbientCubeCS"},
                       {"/Shaders/GI/SSGI.hlsl", "TraceCS"},            {"/Shaders/GI/SSGI.hlsl", "TemporalCS"}};
        for (const Entry& e : entries) {
            rhi::ShaderCompileDesc d;
            d.path = e.path;
            d.entryPoint = e.entry;
            d.stage = rhi::ShaderStage::Compute;
            const Result<rhi::ShaderBytecode> r = rhi::ShaderCompiler::Compile(d);
            if (!r)
                ZX_LOG_WARN(LogGITest, "{}", r.GetError().message);
            ZX_EXPECT(r.IsOk());
        }
    }
    rhi::ShaderCompiler::Shutdown();
    fs::Shutdown();
    platform::DeleteDirectoryTree(savedDir);
}

ZX_TEST(GI, VolumeConstantsMatchTheShaderLayout)
{
    // DDGIVolume (Common/DDGI.hlsli) is 80 bytes and ends the forward view constants.
    ZX_EXPECT_EQ(sizeof(GIVolumeConstants), 80u);
    ZX_EXPECT_EQ(offsetof(GIVolumeConstants, spacing), 16u);
    ZX_EXPECT_EQ(offsetof(GIVolumeConstants, counts), 32u);
    ZX_EXPECT_EQ(offsetof(GIVolumeConstants, normalBias), 48u);
    ZX_EXPECT_EQ(offsetof(GIVolumeConstants, invIrradianceSize), 56u);
    ZX_EXPECT_EQ(offsetof(GIVolumeConstants, maxDistance), 72u);
    // Off by default: the open sky lights everything until the probes have data.
    SceneLighting lighting;
    ZX_EXPECT_EQ(lighting.gi.irradiance, rhi::InvalidDescriptor);
}

ZX_TEST(GI, BakedProbesRoundTrip)
{
    fs::Initialize(1);
    const String dir = platform::GetTempDirectory() + "/zx_gi_bake_test";
    platform::CreateDirectoryTree(dir);
    BakedGI b;
    b.originIndex = IVec3(-3, 0, 5);
    b.counts = UVec3(4, 2, 3);
    b.spacing = 1.5f;
    b.maxDistance = 30.0f;
    b.exposure = 0.25f;
    const u64 probes = 4 * 2 * 3;
    b.irradiance.Resize(probes * 8 * 8 * 8, 7);
    b.distance.Resize(probes * 16 * 16 * 4, 9);
    b.probeData.Resize(probes * 8, 11);
    b.lightmapWidth = 16;
    b.lightmapHeight = 8;
    b.lightmap.Resize(16 * 8 * 8, 13);
    ZX_EXPECT(b.IsValid());
    const String path = dir + "/Scene.zgi";
    ZX_REQUIRE(SaveBakedGI(path, b));
    BakedGI back;
    ZX_REQUIRE(LoadBakedGI(path, back));
    ZX_EXPECT(back.originIndex == b.originIndex);
    ZX_EXPECT(back.counts == b.counts);
    ZX_EXPECT_NEAR(back.spacing, 1.5f, 1e-6f);
    ZX_EXPECT_NEAR(back.exposure, 0.25f, 1e-6f);
    ZX_EXPECT(back.irradiance.Size() == b.irradiance.Size() && back.irradiance[17] == 7);
    ZX_EXPECT(back.distance.Size() == b.distance.Size() && back.probeData.Size() == b.probeData.Size());
    ZX_EXPECT(back.lightmapWidth == 16 && back.lightmapHeight == 8 && back.lightmap.Size() == b.lightmap.Size() && back.lightmap[5] == 13);
    // A truncated or foreign file is refused.
    b.probeData.PopBack();
    ZX_EXPECT(!b.IsValid());
    ZX_EXPECT(!SaveBakedGI(path + "2", b));
    ZX_EXPECT(!LoadBakedGI(dir + "/missing.zgi", back));
    fs::Shutdown();
    platform::DeleteDirectoryTree(dir);
}
