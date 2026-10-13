<#
.SYNOPSIS
    Builds ZeqouX3D with MSVC + CMake + Ninja (sets up vcvars64 automatically).
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Tools/build.ps1 -Config Development
    powershell -ExecutionPolicy Bypass -File Tools/build.ps1 -Config Debug -Target ZXTests -Test
    powershell -ExecutionPolicy Bypass -File Tools/build.ps1 -All
#>
param(
    [ValidateSet("Debug", "Development", "Shipping")]
    [string]$Config = "Development",
    [string]$Target = "",
    [switch]$All,       # build all three configurations
    [switch]$Clean,     # wipe Build/ before configuring
    [switch]$Test       # run ZXTests after building
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

function Find-VcVars {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $path = & $vswhere -latest -products * -version "[17.0,18.0)" `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($path) {
            $bat = Join-Path $path "VC\Auxiliary\Build\vcvars64.bat"
            if (Test-Path $bat) { return $bat }
        }
    }
    $fallback = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    if (Test-Path $fallback) { return $fallback }
    throw "vcvars64.bat not found. Install Visual Studio 2022 Build Tools with the C++ workload."
}

$presetFor = @{ Debug = "win64-debug"; Development = "win64-dev"; Shipping = "win64-shipping" }
$configs = if ($All) { @("Debug", "Development", "Shipping") } else { @($Config) }

if ($Clean -and (Test-Path "$Root\Build")) { Remove-Item -Recurse -Force "$Root\Build" }

$vcvars = Find-VcVars
$targetArg = if ($Target) { "--target $Target" } else { "" }

# A temporary batch file is the most robust way to run commands inside the vcvars environment.
$lines = @(
    "@echo off",
    "chcp 65001 >nul",
    "call `"$vcvars`" >nul 2>&1",
    "cd /d `"$Root`"",
    "if not exist `"Build\win64\build.ninja`" (cmake --preset win64 || exit /b 1)"
)
foreach ($c in $configs) {
    $lines += "cmake --build --preset $($presetFor[$c]) $targetArg || exit /b 1"
}
$bat = Join-Path $env:TEMP "zx_build_$PID.bat"
Set-Content -Path $bat -Value ($lines -join "`r`n") -Encoding ASCII
try {
    & cmd.exe /c $bat
    $code = $LASTEXITCODE
} finally {
    Remove-Item $bat -ErrorAction SilentlyContinue
}
if ($code -ne 0) { Write-Host "BUILD FAILED" -ForegroundColor Red; exit $code }
Write-Host "BUILD OK ($($configs -join ', '))" -ForegroundColor Green

if ($Test) {
    foreach ($c in $configs) {
        & "$Root\Binaries\Win64\$c\ZXTests.exe"
        if ($LASTEXITCODE -ne 0) { Write-Host "TESTS FAILED ($c)" -ForegroundColor Red; exit $LASTEXITCODE }
    }
}
