<#
.SYNOPSIS
    Builds a ZeqouX3D release: Shipping binaries, the staged install folder, the installer
    (ZeqouX3D-<version>-Beta-Setup.exe, made by ZXSetup) and a portable zip, into Build/Release.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Tools/make_release.ps1 [-SkipBuild]
#>
param([switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$version = ([regex]::Match((Get-Content (Join-Path $root "CMakeLists.txt") -Raw), "VERSION (\d+\.\d+\.\d+) LANGUAGES")).Groups[1].Value
if (-not $version) { throw "Version not found in CMakeLists.txt" }

if (-not $SkipBuild) {
    & powershell -ExecutionPolicy Bypass -File (Join-Path $root "Tools/build.ps1") -Config Shipping
    if ($LASTEXITCODE -ne 0) { throw "Shipping build failed" }
}
$bin = Join-Path $root "Binaries/Win64/Shipping"
$out = Join-Path $root "Build/Release"
$stage = Join-Path $out "ZeqouX3D-$version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

function Copy-Into([string]$from, [string]$to) {
    $dest = Join-Path $stage $to
    New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
    Copy-Item -Recurse -Force (Join-Path $root $from) $dest
}

# Programs (the editor finds the engine by walking up to Engine/Content; packaging copies ZXGame.exe from its folder).
New-Item -ItemType Directory -Force (Join-Path $stage "Binaries/Win64") | Out-Null
foreach ($f in "ZXEditor.exe", "ZXGame.exe", "dxcompiler.dll", "dxil.dll") {
    Copy-Item (Join-Path $bin $f) (Join-Path $stage "Binaries/Win64/$f")
}
Copy-Into "Engine/Content" "Engine/Content"
Copy-Into "Engine/Shaders" "Engine/Shaders"
Copy-Into "Engine/Source/GameSDK" "Engine/Source/GameSDK"
Copy-Into "Tools/build_game.ps1" "Tools/build_game.ps1"
Copy-Into "LICENSE" "LICENSE"
Copy-Into "docs/ZeqouScript.md" "docs/ZeqouScript.md"
Copy-Into "docs/CHANGELOG.md" "docs/CHANGELOG.md"
# Source art of the branding is not needed at runtime.
$brandSource = Join-Path $stage "Engine/Content/Branding/Source"
if (Test-Path $brandSource) { Remove-Item -Recurse -Force $brandSource }

$readme = @"
ZeqouX3D $version (Beta)
========================

A 3D engine and editor written from scratch in C++ and Direct3D 12.

THIS IS A BETA - NOT A COMPLETE 3D ENGINE YET. Many features are missing or unfinished, expect bugs and
crashes, and projects made with this version may need changes to open in later versions. Keep backups.

Start:      Binaries\Win64\ZXEditor.exe  (New Project -> pick a template)
Scripting:  ZeqouScript works out of the box (docs\ZeqouScript.md). C++ game code needs Visual Studio 2022
            (Build Tools) with the C++ workload.
Requires:   Windows 10 / 11 64-bit, a DirectX 12 GPU (Intel UHD 620 or better), 8 GB RAM.
Logs:       Saved\Logs (attach them to bug reports)
Bugs:       https://github.com/Mishaadevv/x3d/issues
License:    PolyForm Strict 1.0.0 (LICENSE)
"@
Set-Content -Path (Join-Path $stage "README.txt") -Value $readme -Encoding UTF8

$setup = Join-Path $out "ZeqouX3D-$version-Beta-Setup.exe"
$zip = Join-Path $out "ZeqouX3D-$version-Beta-Portable.zip"
Remove-Item -Force -ErrorAction SilentlyContinue $setup, $zip
# The engine's command line splits at spaces: pass 8.3 paths.
$fso = New-Object -ComObject Scripting.FileSystemObject
$shortStage = $fso.GetFolder($stage).ShortPath
$shortSetup = Join-Path $fso.GetFolder($out).ShortPath (Split-Path -Leaf $setup)
$p = Start-Process -FilePath (Join-Path $bin "ZXSetup.exe") -ArgumentList "-makeinstaller=$shortStage", "-out=$shortSetup" -Wait -PassThru -NoNewWindow
if ($p.ExitCode -ne 0 -or -not (Test-Path $setup)) { throw "ZXSetup -makeinstaller failed ($($p.ExitCode)); see %TEMP%\ZeqouX3D-Setup\Logs\Setup.log" }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal

"{0}  {1:N1} MB" -f $setup, ((Get-Item $setup).Length / 1MB)
"{0}  {1:N1} MB" -f $zip, ((Get-Item $zip).Length / 1MB)
