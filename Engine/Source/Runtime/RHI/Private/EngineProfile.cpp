#include "RHI/EngineProfile.h"
#include "Core/Console/Console.h"
#include "Core/FileSystem/FileSystem.h"
#include "Core/HAL/PlatformFile.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Log/Log.h"
#include "Core/String/Path.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogProfile, Info);

namespace zx {

namespace {
CVar<String> CVarEngineProfile("engine.Profile", "auto", "Engine mode: auto, lite (weak devices) or full",
                               CVarFlags::Archive);
EngineProfile g_profile = EngineProfile::Lite;
bool g_forced = false;

String PreferenceFile(StringView file)
{
    return file.Empty() ? platform::GetUserLocalAppDataDirectory() + "/ZeqouX3D/EngineProfile.txt" : String(file);
}
} // namespace

const char* EngineProfileName(EngineProfile profile) { return profile == EngineProfile::Lite ? "Lite" : "Full"; }

EngineProfile RecommendedEngineProfile(const rhi::DeviceCaps& caps)
{
    const u64 ram = platform::GetMemoryInfo().totalPhysical;
    const bool weak = caps.uma || caps.adapter.isSoftware || caps.adapter.dedicatedVideoMemory < 3_GB || ram < 12_GB;
    return weak ? EngineProfile::Lite : EngineProfile::Full;
}

EngineProfile SelectEngineProfile(const rhi::DeviceCaps& caps)
{
    EngineProfile profile;
    const char* reason;
    const StringView setting = CVarEngineProfile.Get().View();
    const String saved = LoadEngineProfilePreference();
    g_forced = true;
    if (CommandLine::HasFlag("lite")) {
        profile = EngineProfile::Lite;
        reason = "command line";
    } else if (CommandLine::HasFlag("full")) {
        profile = EngineProfile::Full;
        reason = "command line";
    } else if (setting.EqualsIgnoreCase("lite") || setting.EqualsIgnoreCase("full")) {
        profile = setting.EqualsIgnoreCase("lite") ? EngineProfile::Lite : EngineProfile::Full;
        reason = "engine.Profile";
    } else if (saved == "lite" || saved == "full") {
        profile = saved == "lite" ? EngineProfile::Lite : EngineProfile::Full;
        reason = "user choice";
        g_forced = false;
    } else {
        profile = RecommendedEngineProfile(caps);
        reason = "hardware";
        g_forced = false;
    }
    g_profile = profile;
    ZX_LOG_INFO(LogProfile, "Engine profile: {} (by {})", EngineProfileName(profile), reason);
    return profile;
}

bool IsEngineProfileForced() { return g_forced; }

bool SaveEngineProfilePreference(StringView value, StringView file)
{
    const String path = PreferenceFile(file);
    platform::CreateDirectoryTree(Path::Parent(path));
    return fs::WriteTextFile(path, value);
}

String LoadEngineProfilePreference(StringView file)
{
    String text;
    if (!fs::ReadTextFile(PreferenceFile(file), text))
        return "auto";
    const StringView v = text.View().Trim();
    return v.EqualsIgnoreCase("lite") ? String("lite") : v.EqualsIgnoreCase("full") ? String("full") : String("auto");
}

EngineProfile GetEngineProfile() { return g_profile; }
void SetEngineProfile(EngineProfile profile) { g_profile = profile; }

} // namespace zx
