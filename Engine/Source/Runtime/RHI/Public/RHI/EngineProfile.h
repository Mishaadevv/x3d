#pragma once

// Engine modes (TZ 1.3): Lite for weak devices (iGPU, little RAM), Full for strong devices.
// The profile is a set of defaults (scalability presets), never a separate code path without fallbacks.

#include "RHI/RHI.h"

namespace zx {

enum class EngineProfile : u8
{
    Lite,
    Full
};

const char* EngineProfileName(EngineProfile profile);

/// Chooses the profile: command line (-lite / -full) > CVar engine.Profile ("lite"/"full"/"auto") > the user's saved
/// choice (SaveEngineProfilePreference) > hardware.
/// Hardware rule: UMA/integrated GPU, < 3 GB VRAM or < 12 GB RAM -> Lite.
EngineProfile SelectEngineProfile(const rhi::DeviceCaps& caps);
/// What the hardware rule picks for this device.
EngineProfile RecommendedEngineProfile(const rhi::DeviceCaps& caps);
/// True when the active profile was forced by -lite / -full or engine.Profile (a saved choice can't override it).
bool IsEngineProfileForced();

/// The user's choice for the next start, per user (%LOCALAPPDATA%/ZeqouX3D/EngineProfile.txt): "lite", "full" or
/// "auto" (= hardware). `file` overrides the location (tests).
bool SaveEngineProfilePreference(StringView value, StringView file = {});
String LoadEngineProfilePreference(StringView file = {});

/// Currently active profile (set by SelectEngineProfile; Lite until then).
EngineProfile GetEngineProfile();
void SetEngineProfile(EngineProfile profile);

} // namespace zx
