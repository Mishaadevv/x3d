# Downloads sample content (Khronos glTF Sample Assets) into Samples/Content/<Model>/.
# Content is not stored in git; the ZeqouX Hub launcher will take over this job later.
#
#   powershell -ExecutionPolicy Bypass -File Tools/fetch_samples.ps1 [-Models Sponza,DamagedHelmet]

param(
    [string[]]$Models = @("Sponza", "DamagedHelmet", "NormalTangentMirrorTest")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Split-Path -Parent $PSScriptRoot
$api = "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models"
$raw = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"

foreach ($model in $Models) {
    $dest = Join-Path $root "Samples/Content/$model"
    New-Item -ItemType Directory -Force $dest | Out-Null
    $files = Invoke-RestMethod -Uri "$api/$model/glTF" -Headers @{ "User-Agent" = "ZeqouX3D" }
    $total = 0
    foreach ($f in $files) {
        if ($f.type -ne "file") { continue }
        $out = Join-Path $dest $f.name
        if ((Test-Path $out) -and ((Get-Item $out).Length -eq $f.size)) { continue }
        Invoke-WebRequest -Uri "$raw/$model/glTF/$($f.name)" -OutFile $out -UseBasicParsing
        $total += $f.size
    }
    try {
        Invoke-WebRequest -Uri "$raw/$model/LICENSE.md" -OutFile (Join-Path $dest "LICENSE.md") -UseBasicParsing
    } catch { }
    Write-Host ("{0}: {1} files, downloaded {2:N1} MB" -f $model, $files.Count, ($total / 1MB))
}
