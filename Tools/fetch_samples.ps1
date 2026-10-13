# Downloads sample content into Samples/Content: Khronos glTF Sample Assets (Samples/Content/<Model>/) and HDR sky
# panoramas from Poly Haven (CC0, 1k OpenEXR, Samples/Content/HDRI/). Content is not stored in git; the ZeqouX Hub
# launcher will take over this job later.
#
#   powershell -ExecutionPolicy Bypass -File Tools/fetch_samples.ps1 [-Models Sponza,DamagedHelmet] [-Hdris name1,name2]

param(
    [string[]]$Models = @("Sponza", "DamagedHelmet", "NormalTangentMirrorTest"),
    [string[]]$Hdris = @("kloofendal_48d_partly_cloudy_puresky")
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

# HDRI panoramas (HDRI Sky component): https://polyhaven.com/hdris, CC0.
if ($Hdris.Count -gt 0) {
    $dest = Join-Path $root "Samples/Content/HDRI"
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($hdri in $Hdris) {
        $out = Join-Path $dest "$($hdri)_1k.exr"
        if (Test-Path $out) { continue }
        Invoke-WebRequest -Uri "https://dl.polyhaven.org/file/ph-assets/HDRIs/exr/1k/$($hdri)_1k.exr" -OutFile $out -UseBasicParsing
        Write-Host ("HDRI {0}: {1:N1} MB" -f $hdri, ((Get-Item $out).Length / 1MB))
    }
    Set-Content -Path (Join-Path $dest "LICENSE.md") -Encoding utf8 -Value "HDR panoramas from Poly Haven (https://polyhaven.com), CC0 1.0 (public domain)."
}
