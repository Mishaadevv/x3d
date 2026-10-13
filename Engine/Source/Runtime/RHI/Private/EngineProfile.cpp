#include "RHI/EngineProfile.h"
#include "Core/Console/Console.h"
#include "Core/HAL/PlatformMisc.h"
#include "Core/Log/Log.h"

ZX_DEFINE_LOG_CATEGORY_STATIC(LogProfile, Info);

namespace zx {

namespace {
CVar<String> CVarEngineProfile("engine.Profile", "auto", "Engine mode: auto, lite (weak devices) or full",
                               CVarFlags::Archive);
EngineProfile g_profile = EngineProfile::Lite;
} // namespace

const char* EngineProfileName(EngineProfile profile) { return profile == EngineProfile::Lite ? "Lite" : "Full"; }

EngineProfile SelectEngineProfile(const rhi::DeviceCaps& caps)
{
    EngineProfile profile;
    const char* reason;
    const StringView setting = CVarEngineProfile.Get().View();
    if (CommandLine::HasFlag("lite")) {
        profile = EngineProfile::Lite;
        reason = "command line";
    } else if (CommandLine::HasFlag("full")) {
        profile = EngineProfile::Full;
        reason = "command line";
    } else if (setting.EqualsIgnoreCase("lite") || setting.EqualsIgnoreCase("full")) {
        profile = setting.EqualsIgnoreCase("lite") ? EngineProfile::Lite : EngineProfile::Full;
        reason = "engine.Profile";
    } else {
        const u64 ram = platform::GetMemoryInfo().totalPhysical;
        const bool weak = caps.uma || caps.adapter.isSoftware || caps.adapter.dedicatedVideoMemory < 3_GB ||
                          ram < 12_GB;
        profile = weak ? EngineProfile::Lite : EngineProfile::Full;
        reason = "hardware";
    }
    g_profile = profile;
    ZX_LOG_INFO(LogProfile, "Engine profile: {} (by {})", EngineProfileName(profile), reason);
    return profile;
}

EngineProfile GetEngineProfile() { return g_profile; }
void SetEngineProfile(EngineProfile profile) { g_profile = profile; }

} // namespace zx
