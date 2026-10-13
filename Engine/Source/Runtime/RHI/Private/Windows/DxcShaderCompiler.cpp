#include "Core/FileSystem/FileSystem.h"
#include "Core/Hash/Hash.h"
#include "Core/Log/Log.h"
#include "Core/Serialization/Binary.h"
#include "Core/String/Path.h"
#include "Core/String/Utf.h"
#include "Core/Threading/Mutex.h"
#include "RHI/ShaderCompiler.h"

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <Windows.h>
#include <objbase.h>

#include <dxcapi.h>

#include <atomic>

ZX_DEFINE_LOG_CATEGORY_STATIC(LogShaders, Info);

namespace zx::rhi::ShaderCompiler {

namespace {

constexpr u32 CacheMagic = 0x4353585A; // "ZXSC"
constexpr u32 CacheVersion = 1;

template <typename T>
struct Com
{
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { Reset(); }
    void Reset()
    {
        if (p)
            p->Release();
        p = nullptr;
    }
    T** operator&()
    {
        Reset();
        return &p;
    }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct Dependency
{
    String path;
    u64 hash;
};

struct State
{
    HMODULE module = nullptr;
    DxcCreateInstanceProc create = nullptr;
    Mutex mutex; // DXC compiler objects are not thread-safe; one compile at a time per instance
    IDxcUtils* utils = nullptr;
    IDxcCompiler3* compiler = nullptr;
    std::atomic<u32> compiled{0};
    std::atomic<u32> cacheHits{0};
    std::atomic<u32> failures{0};
};

State* g_state = nullptr;

String NormalizeIncludePath(const wchar_t* name)
{
    String p = Utf16ToUtf8(name);
    p.ReplaceChar('\\', '/');
    while (p.StartsWith("./"))
        p.Erase(0, 2);
    return Path::Normalize(p);
}

/// Include handler resolving files through the VFS and recording dependencies for the cache.
class IncludeHandler final : public IDxcIncludeHandler
{
public:
    IncludeHandler(IDxcUtils* utils, Array<Dependency>& deps) : m_utils(utils), m_deps(deps) {}

    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR fileName, IDxcBlob** includeSource) override
    {
        *includeSource = nullptr;
        String path = NormalizeIncludePath(fileName);
        Array<u8> data;
        if (!fs::ReadFile(path, data)) {
            // DXC resolves includes relative to the including file first ("/Shaders/Tests/Common/X.hlsli").
            // Fall back to every suffix of the path under the shader root ("/Shaders/Common/X.hlsli").
            const Array<StringView> parts = SplitString(path, '/', true);
            bool found = false;
            for (usize first = 0; first < parts.Size() && !found; ++first) {
                String candidate("/Shaders");
                for (usize k = first; k < parts.Size(); ++k) {
                    candidate += '/';
                    candidate += parts[k];
                }
                if (candidate != path && fs::ReadFile(candidate, data)) {
                    path = candidate;
                    found = true;
                }
            }
            if (!found)
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        }
        bool seen = false;
        for (const Dependency& d : m_deps)
            seen |= d.path == path;
        if (!seen)
            m_deps.PushBack(Dependency{path, HashXX64(data.Data(), data.Size())});
        IDxcBlobEncoding* blob = nullptr;
        const HRESULT hr = m_utils->CreateBlob(data.Data(), u32(data.Size()), DXC_CP_UTF8, &blob);
        if (FAILED(hr))
            return hr;
        *includeSource = blob;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
    {
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }  // stack object
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

private:
    IDxcUtils* m_utils;
    Array<Dependency>& m_deps;
};

u64 CacheKey(StringView sourceName, const ShaderCompileDesc& d)
{
    XXHash64Stream h(0x5A58u);
    h.Update(sourceName);
    h.Update(d.entryPoint);
    h.UpdateValue(u32(d.stage));
    h.UpdateValue(d.shaderModel);
    h.UpdateValue(u8(d.optimize));
    h.UpdateValue(u8(d.debugInfo));
    for (const ShaderDefine& def : d.defines) {
        h.Update(def.name);
        h.Update("=");
        h.Update(def.value);
        h.Update(";");
    }
    h.UpdateValue(CacheVersion);
    return h.Digest();
}

String CachePath(u64 key) { return String::Format("/Saved/ShaderCache/{:016x}.zsc", key); }

bool TryLoadCache(u64 key, ShaderBytecode& out)
{
    if (!fs::IsMounted("/Saved"))
        return false;
    Array<u8> file;
    if (!fs::ReadFile(CachePath(key), file))
        return false;
    BinaryReader r(file.View());
    if (r.Read<u32>() != CacheMagic || r.Read<u32>() != CacheVersion)
        return false;
    const u64 depCount = r.ReadVarU64();
    for (u64 i = 0; i < depCount && !r.HasError(); ++i) {
        const StringView path = r.ReadStringView();
        const u64 hash = r.Read<u64>();
        Array<u8> current;
        if (!fs::ReadFile(path, current) || HashXX64(current.Data(), current.Size()) != hash)
            return false; // a source or include changed
    }
    out.hash = r.Read<u64>();
    r.ReadArray(out.code);
    return !r.HasError() && !out.code.Empty();
}

void StoreCache(u64 key, const Array<Dependency>& deps, const ShaderBytecode& bytecode)
{
    if (!fs::IsMounted("/Saved"))
        return;
    Array<u8> file;
    BinaryWriter w(file);
    w.Write(CacheMagic);
    w.Write(CacheVersion);
    w.WriteVarU64(deps.Size());
    for (const Dependency& d : deps) {
        w.WriteString(d.path);
        w.Write(d.hash);
    }
    w.Write(bytecode.hash);
    w.WriteArray(Span<const u8>(bytecode.code.Data(), bytecode.code.Size()));
    fs::WriteFile(CachePath(key), file.View());
}

Result<ShaderBytecode> CompileImpl(StringView source, StringView sourceName, const ShaderCompileDesc& desc,
                                   Array<Dependency>& deps)
{
    if (!g_state)
        return Error::Make(ErrorCode::Unsupported, "Shader compiler not initialized");
    ScopedLock<Mutex> guard(g_state->mutex);

    // Arguments (wide strings kept alive in `storage`).
    Array<Array<wchar_t>> storage;
    Array<LPCWSTR> args;
    auto arg = [&](StringView s) {
        storage.PushBack(Utf8ToUtf16(s));
        args.PushBack(storage.Back().Data());
    };
    arg(sourceName);
    arg("-E");
    arg(desc.entryPoint);
    const u32 sm = Max(desc.shaderModel, 60u);
    char profile[16];
    FormatToBuffer(profile, sizeof(profile), "{}_{}_{}", desc.stage == ShaderStage::Pixel ? "ps"
                                                           : desc.stage == ShaderStage::Compute ? "cs" : "vs",
                   sm / 10, sm % 10);
    arg("-T");
    arg(profile);
    arg("-HV");
    arg("2021");
    arg("-I");
    arg("/Shaders");
    arg(desc.optimize ? "-O3" : "-Od");
    if (desc.debugInfo) {
        arg("-Zi");
        arg("-Qembed_debug");
    }
    if (sm >= 62)
        arg("-enable-16bit-types");
    arg("-D");
    arg(String::Format("ZX_SHADER_MODEL={}", sm));
    for (const ShaderDefine& d : desc.defines) {
        arg("-D");
        arg(String::Format("{}={}", d.name, d.value));
    }

    DxcBuffer buffer;
    buffer.Ptr = source.Data();
    buffer.Size = source.Size();
    buffer.Encoding = DXC_CP_UTF8;

    IncludeHandler includes(g_state->utils, deps);
    Com<IDxcResult> result;
    HRESULT hr = g_state->compiler->Compile(&buffer, args.Data(), u32(args.Size()), &includes, IID_PPV_ARGS(&result));
    if (SUCCEEDED(hr))
        result->GetStatus(&hr);

    String messages;
    if (result) {
        Com<IDxcBlobUtf8> errors;
        result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        if (errors && errors->GetStringLength() > 0)
            messages = StringView(errors->GetStringPointer(), errors->GetStringLength());
    }
    if (FAILED(hr)) {
        g_state->failures.fetch_add(1);
        return Error::Make(ErrorCode::InvalidData, "Shader compilation failed: {} ({})\n{}", sourceName, desc.entryPoint,
                           messages);
    }
    if (!messages.Empty())
        ZX_LOG_WARN(LogShaders, "{} ({}):\n{}", sourceName, desc.entryPoint, messages);

    Com<IDxcBlob> object;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (!object || object->GetBufferSize() == 0)
        return Error::Make(ErrorCode::Internal, "DXC produced no bytecode for {}", sourceName);

    ShaderBytecode out;
    out.stage = desc.stage;
    out.entryPoint = desc.entryPoint;
    out.code.ResizeUninitialized(object->GetBufferSize());
    MemCopy(out.code.Data(), object->GetBufferPointer(), object->GetBufferSize());
    out.hash = HashXX64(out.code.Data(), out.code.Size());
    g_state->compiled.fetch_add(1);
    return out;
}

} // namespace

bool Initialize()
{
    if (g_state)
        return true;
    HMODULE module = ::LoadLibraryW(L"dxcompiler.dll");
    if (!module) {
        ZX_LOG_ERROR(LogShaders, "dxcompiler.dll not found next to the executable");
        return false;
    }
    auto create = reinterpret_cast<DxcCreateInstanceProc>(reinterpret_cast<void*>(::GetProcAddress(module, "DxcCreateInstance")));
    if (!create) {
        ::FreeLibrary(module);
        return false;
    }
    g_state = New<State>();
    g_state->module = module;
    g_state->create = create;
    if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&g_state->utils))) ||
        FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_state->compiler)))) {
        ZX_LOG_ERROR(LogShaders, "Failed to create DXC instances");
        Shutdown();
        return false;
    }
    ZX_LOG_INFO(LogShaders, "DXC shader compiler loaded");
    return true;
}

void Shutdown()
{
    if (!g_state)
        return;
    if (g_state->compiler)
        g_state->compiler->Release();
    if (g_state->utils)
        g_state->utils->Release();
    if (g_state->module)
        ::FreeLibrary(g_state->module);
    Delete(g_state);
    g_state = nullptr;
}

bool IsAvailable() { return g_state != nullptr; }

Result<ShaderBytecode> Compile(const ShaderCompileDesc& desc)
{
    String source;
    if (!fs::ReadTextFile(desc.path, source))
        return Error::Make(ErrorCode::NotFound, "Shader source not found: {}", desc.path);
    const String sourceName = Path::Normalize(desc.path);
    const u64 key = CacheKey(sourceName, desc);
    ShaderBytecode cached;
    if (desc.useCache && TryLoadCache(key, cached)) {
        cached.stage = desc.stage;
        cached.entryPoint = desc.entryPoint;
        if (g_state)
            g_state->cacheHits.fetch_add(1);
        return cached;
    }
    Array<Dependency> deps;
    deps.PushBack(Dependency{sourceName, HashXX64(source.Data(), source.Size())});
    Result<ShaderBytecode> result = CompileImpl(source, sourceName, desc, deps);
    if (result && desc.useCache)
        StoreCache(key, deps, *result);
    return result;
}

Result<ShaderBytecode> CompileSource(StringView source, StringView virtualName, const ShaderCompileDesc& desc)
{
    Array<Dependency> deps;
    return CompileImpl(source, virtualName, desc, deps);
}

Stats GetStats()
{
    Stats s;
    if (g_state) {
        s.compiled = g_state->compiled.load();
        s.cacheHits = g_state->cacheHits.load();
        s.failures = g_state->failures.load();
    }
    return s;
}

} // namespace zx::rhi::ShaderCompiler
