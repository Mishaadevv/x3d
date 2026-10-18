#pragma once

// Windows integration of the installer: the per-user "Apps & features" entry, shortcuts, message boxes.

#include "Core/String/String.h"

namespace zx::setup {

/// %LOCALAPPDATA%/Programs/ZeqouX3D (per-user: no administrator rights needed, the engine can write its Saved folder).
String DefaultInstallDir();
/// The folder of an existing installation (from the registry), or empty.
String FindInstalledDir();
String FindInstalledVersion();

/// The "Apps & features" entry (HKCU) with Uninstall.exe as the uninstaller.
bool RegisterInstall(StringView dir, StringView version, u64 sizeBytes);
void UnregisterInstall();

String DesktopDir();
String StartMenuDir(); // ...\Start Menu\Programs
bool CreateShortcut(StringView link, StringView target, StringView workingDir, StringView description);

/// Free bytes on the volume of `dir` (or of its first existing parent).
u64 FreeDiskSpace(StringView dir);
/// Lets DLLs (dxcompiler) load from `dir` too.
void AddDllSearchDirectory(StringView dir);

bool AskYesNo(StringView title, StringView text);
void ShowInfo(StringView title, StringView text);

/// After this process exits: deletes `file`, then removes `dir` if it is empty (a running exe can't delete itself).
void DeleteAfterExit(StringView file, StringView dir);
/// Starts a program (not waiting).
bool Launch(StringView exe, StringView workingDir);

} // namespace zx::setup
