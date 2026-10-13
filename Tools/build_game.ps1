<#
.SYNOPSIS
    Builds a project's C++ game module (Source/<Name>) into <Project>/Binaries/<Name>.dll with MSVC + CMake + Ninja.
    The editor runs this in the background (Compile, F7) and hot-reloads the DLL when it succeeds.
.EXAMPLE
    powershell -ExecutionPolicy Bypass -File Tools/build_game.ps1 -Project D:/Games/MyGame -Name MyGame
#>
param(
    [Parameter(Mandatory = $true)][string]$Project,
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Engine = "",
    [string]$Log = ""
)

$ErrorActionPreference = "Stop"
if (-not $Engine) { $Engine = Split-Path -Parent $PSScriptRoot }
if (-not $Log) { $Log = Join-Path $Project "Saved\Logs\GameBuild.log" }
New-Item -ItemType Directory -Force (Split-Path -Parent $Log) | Out-Null

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
    return $null
}

$vcvars = Find-VcVars
if (-not $vcvars) {
    Set-Content -Path $Log -Value "error: vcvars64.bat not found. Install Visual Studio 2022 (Build Tools) with the C++ workload." -Encoding UTF8
    exit 3
}

$source = Join-Path $Project "Source\$Name"
$build = Join-Path $Project "Intermediate\GameBuild"
$envCache = Join-Path $build "vcvars.env"
$output = (Join-Path $Project "Binaries") -replace '\\', '/'
$engineFwd = $Engine -replace '\\', '/'
if (-not (Test-Path (Join-Path $source "CMakeLists.txt"))) {
    Set-Content -Path $Log -Value "error: $source\CMakeLists.txt not found (no game module in this project)." -Encoding UTF8
    exit 2
}

# The batch file redirects its own output (quoting a redirect through cmd /c from PowerShell 5.1 is fragile).
$lines = @(
    "@echo off",
    "chcp 65001 >nul",
    "call :build > `"$Log`" 2>&1",
    "exit /b %errorlevel%",
    ":build",
    # vcvars64 takes seconds: its environment is cached per project (dropped after a failed build).
    "if exist `"$envCache`" goto :loadenv",
    "call `"$vcvars`" >nul 2>&1",
    "if not exist `"$build`" mkdir `"$build`"",
    "set > `"$envCache`"",
    "goto :envready",
    ":loadenv",
    "for /f `"usebackq delims=`" %%a in (`"$envCache`") do set `"%%a`"",
    ":envready",
    "cd /d `"$Project`"",
    "if not exist `"$build\build.ninja`" (cmake -S `"$source`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=Release `"-DZX_ENGINE_ROOT=$engineFwd`" `"-DZX_GAME_OUTPUT=$output`" || exit /b 1)",
    "cmake --build `"$build`" || exit /b 1",
    "exit /b 0"
)
$bat = Join-Path $env:TEMP "zx_build_game_$PID.bat"
[System.IO.File]::WriteAllText($bat, ($lines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding $false))
try {
    $start = Get-Date
    & cmd.exe /c $bat
    $code = $LASTEXITCODE
    $seconds = [math]::Round(((Get-Date) - $start).TotalSeconds, 1)
    $status = if ($code -eq 0) { "BUILD OK" } else { "BUILD FAILED (exit code $code)" }
    Add-Content -Path $Log -Value "$status in $seconds s"
    if ($code -ne 0) { Remove-Item $envCache -ErrorAction SilentlyContinue }
} finally {
    Remove-Item $bat -ErrorAction SilentlyContinue
}
exit $code
