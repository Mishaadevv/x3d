#pragma once

// HLSL compilation through DXC (dxcompiler.dll from the Windows SDK, copied next to the executable).
// Includes are resolved through the virtual file system (engine shaders live under "/Shaders").
// Results are cached in /Saved/ShaderCache keyed by source + includes + options.

#include "Core/Result.h"
#include "RHI/RHITypes.h"

namespace zx::rhi {

struct ShaderDefine
{
    StringView name;
    StringView value = "1";
};

struct ShaderCompileDesc
{
    /// Virtual or physical path to the .hlsl file.
    StringView path;
    StringView entryPoint = "main";
    ShaderStage stage = ShaderStage::Vertex;
    Span<const ShaderDefine> defines;
    /// Target shader model *10 (60 = 6.0, 66 = 6.6).
    u32 shaderModel = 60;
    bool optimize = true;
    bool debugInfo = false;
    bool useCache = true;
};

namespace ShaderCompiler {

/// Loads dxcompiler.dll. Returns false if unavailable.
bool Initialize();
void Shutdown();
bool IsAvailable();

Result<ShaderBytecode> Compile(const ShaderCompileDesc& desc);

/// Compiles from an in-memory string (no caching). `virtualName` is used for error messages / includes.
Result<ShaderBytecode> CompileSource(StringView source, StringView virtualName, const ShaderCompileDesc& desc);

struct Stats
{
    u32 compiled = 0;
    u32 cacheHits = 0;
    u32 failures = 0;
};
Stats GetStats();

} // namespace ShaderCompiler
} // namespace zx::rhi
