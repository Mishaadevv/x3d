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
                       {"/Shaders/GI/DDGI.hlsl", "UpdateDistanceCS"},   {"/Shaders/GI/DDGI.hlsl", "AmbientCubeCS"}};
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
