#include "Render/MaterialShaderCache.h"
#include "Core/Hash/Hash.h"
#include "Core/Log/Log.h"
#include "Core/Time/Time.h"
#include "RHI/ShaderCompiler.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogMaterialShaders, Info);

namespace zx::render {

using namespace rhi;

MaterialShaderCache::~MaterialShaderCache()
{
    for (UniquePtr<Entry>& e : m_entries)
        if (e->job.IsValid())
            jobs::Wait(e->job);
}

void MaterialShaderCache::Compile(Entry& e)
{
    Stopwatch sw;
    const String source = e.code + "\n#include \"/Shaders/Forward/ForwardPBR.hlsl\"\n";
    const String name = String::Format("/Shaders/Generated/Material_{:016x}.hlsl", e.hash);
    auto compile = [&](StringView entry, bool alphaTest, bool depthOnly, ShaderBytecode& out, bool normals = false) -> bool {
        ShaderDefine defines[4] = {{"ZX_GRAPH_MATERIAL", "1"}};
        u32 count = 1;
        if (alphaTest)
            defines[count++] = {"ALPHA_TEST", "1"};
        if (depthOnly)
            defines[count++] = {"DEPTH_ONLY", "1"};
        if (normals)
            defines[count++] = {"ZX_NORMAL_OUTPUT", "1"};
        ShaderCompileDesc d;
        d.entryPoint = entry;
        d.stage = ShaderStage::Pixel;
        d.defines = Span<const ShaderDefine>(defines, count);
        Result<ShaderBytecode> r = ShaderCompiler::CompileSource(source.View(), name.View(), d);
        if (!r) {
            e.error = r.GetError().message;
            return false;
        }
        out = move(r.Value());
        return true;
    };
    const bool ok = e.alphaTested ? compile("PSMain", true, false, e.shaders.mainAlphaTest) && compile("PSDepth", true, true, e.shaders.depthAlphaTest) &&
                                        compile("PSMain", true, false, e.shaders.mainAlphaTestNormals, true)
                                  : compile("PSMain", false, false, e.shaders.main) && compile("PSMain", false, false, e.shaders.mainNormals, true);
    e.compileMs = sw.ElapsedMilliseconds();
    if (ok)
        ZX_LOG_INFO(LogMaterialShaders, "Material shader {:016x} compiled in {:.0f} ms", e.hash, e.compileMs);
    else
        ZX_LOG_ERROR(LogMaterialShaders, "Material shader {:016x} failed: {}", e.hash, e.error);
    e.state.store(ok ? State::Ready : State::Failed, std::memory_order_release);
}

u32 MaterialShaderCache::Request(u64 hash, StringView code, bool alphaTested)
{
    hash = HashCombine(hash, u64(alphaTested));
    for (usize i = 0; i < m_entries.Size(); ++i)
        if (m_entries[i]->hash == hash)
            return u32(i + 1);
    UniquePtr<Entry> e = MakeUnique<Entry>();
    e->hash = hash;
    e->code = code;
    e->alphaTested = alphaTested;
    e->state.store(State::Compiling, std::memory_order_release);
    Entry* raw = e.Get();
    m_entries.PushBack(move(e));
    if (jobs::IsInitialized())
        raw->job = jobs::Schedule([raw] { Compile(*raw); });
    else
        Compile(*raw);
    return u32(m_entries.Size());
}

u32 MaterialShaderCache::ReadyCount() const
{
    u32 n = 0;
    for (const UniquePtr<Entry>& e : m_entries)
        n += e->state.load(std::memory_order_acquire) == State::Ready ? 1u : 0u;
    return n;
}

MaterialShaderCache::State MaterialShaderCache::GetState(u32 id) const
{
    return id == 0 || id > m_entries.Size() ? State::Failed : m_entries[id - 1]->state.load(std::memory_order_acquire);
}

const MaterialShaderCache::Shaders* MaterialShaderCache::Get(u32 id) const
{
    return GetState(id) == State::Ready ? &m_entries[id - 1]->shaders : nullptr;
}

String MaterialShaderCache::GetError(u32 id) const { return GetState(id) == State::Failed && id ? m_entries[id - 1]->error : String(); }

f64 MaterialShaderCache::GetCompileMs(u32 id) const
{
    const State s = GetState(id);
    return (s == State::Ready || s == State::Failed) && id ? m_entries[id - 1]->compileMs : 0.0;
}

u32 MaterialShaderCache::PendingCount() const
{
    u32 n = 0;
    for (const UniquePtr<Entry>& e : m_entries)
        n += e->state.load(std::memory_order_acquire) == State::Compiling ? 1 : 0;
    return n;
}

} // namespace zx::render
