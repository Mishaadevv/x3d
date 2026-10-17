#pragma once

// Pixel shaders of material graphs: the generated code is compiled together with Forward/ForwardPBR.hlsl into the
// variants the scene renderer needs (main, alpha-tested main, alpha-tested depth). Compilation runs on the job system;
// until a shader is ready the renderer draws the material with the standard pipeline (a neutral grey row).

#include "Core/Jobs/JobSystem.h"
#include "Core/Memory/Pointers.h"
#include "RHI/RHITypes.h"

#include <atomic>

namespace zx::render {

class MaterialShaderCache
{
public:
    enum class State : u8
    {
        Pending,
        Compiling,
        Ready,
        Failed,
    };
    struct Shaders
    {
        rhi::ShaderBytecode main;
        rhi::ShaderBytecode mainAlphaTest;
        rhi::ShaderBytecode depthAlphaTest;
        rhi::ShaderBytecode mainNormals; // + normal/roughness target (screen-space reflections)
        rhi::ShaderBytecode mainAlphaTestNormals;
        rhi::ShaderBytecode gbuffer; // deferred G-buffer pass (PSGBuffer)
        rhi::ShaderBytecode gbufferAlphaTest;
    };

    MaterialShaderCache() = default;
    /// Number of shaders compiled successfully so far.
    u32 ReadyCount() const;
    ~MaterialShaderCache();
    ZX_NON_COPYABLE(MaterialShaderCache)

    /// Shader id (>= 1) for the code; the same hash returns the same id. Starts compiling on first request. Only the
    /// variants the blend mode needs are built: `main` (opaque/translucent) or the two alpha-tested ones (masked).
    u32 Request(u64 hash, StringView code, bool alphaTested);
    State GetState(u32 id) const;
    /// Ready shaders, or null.
    const Shaders* Get(u32 id) const;
    String GetError(u32 id) const;
    /// Milliseconds the last finished compilation of `id` took (0 if not finished).
    f64 GetCompileMs(u32 id) const;
    u32 PendingCount() const;

private:
    struct Entry
    {
        u64 hash = 0;
        String code;
        bool alphaTested = false;
        std::atomic<State> state{State::Pending};
        Shaders shaders;
        String error;
        f64 compileMs = 0.0;
        JobHandle job;
    };
    static void Compile(Entry& e);

    Array<UniquePtr<Entry>> m_entries; // index = id - 1
};

} // namespace zx::render
