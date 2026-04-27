param(
    [string]$BlenderExe = "C:\Program Files\Blender Foundation\Blender\blender.exe",
    [string]$AssetsDir = "assets"
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $BlenderExe)) {
    throw "Blender not found at: $BlenderExe"
}

$scriptPath = Join-Path $PSScriptRoot "convert_mixamo_to_glb.py"
if (!(Test-Path $scriptPath)) {
    throw "Missing script: $scriptPath"
}

$assetsAbs = Resolve-Path $AssetsDir
& $BlenderExe --background --python $scriptPath -- --assets-dir $assetsAbs

