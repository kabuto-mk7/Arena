param(
    [string]$Platform = "win32",
    [string]$Config = "Release",
    [string]$BuildDir = "build",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"

function Find-CMake {
    $cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmakeCmd) {
        return $cmakeCmd.Source
    }

    $vsCmake = Get-ChildItem -Path "C:\Program Files\Microsoft Visual Studio" -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake\.exe$" } |
        Select-Object -First 1

    if ($vsCmake) {
        return $vsCmake.FullName
    }

    throw "Could not find cmake.exe. Install CMake or Visual Studio C++ tools."
}

$cmake = Find-CMake
Write-Host "Using CMake: $cmake"

& $cmake -S . -B $BuildDir -A $Platform
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $BuildDir --config $Config --clean-first
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$timestamp = Get-Date -Format "yyyyMMdd-HHmm"
$packageName = "arena-playtest-$timestamp"
$packageDir = Join-Path $OutDir $packageName
$binDir = Join-Path $BuildDir $Config

if (-not (Test-Path $binDir)) {
    throw "Build output folder not found: $binDir"
}

if (Test-Path $packageDir) {
    Remove-Item -Recurse -Force $packageDir
}
New-Item -ItemType Directory -Path $packageDir | Out-Null

Copy-Item (Join-Path $binDir "arena_client.exe") $packageDir -Force
Copy-Item (Join-Path $binDir "arena_server.exe") $packageDir -Force

# Include runtime DLLs produced by CMake dependencies (for example assimp on some setups).
$dlls = Get-ChildItem -Path $binDir -Filter *.dll -File -ErrorAction SilentlyContinue
foreach ($dll in $dlls) {
    Copy-Item $dll.FullName $packageDir -Force
}

Copy-Item "assets" (Join-Path $packageDir "assets") -Recurse -Force

$runServer = @"
@echo off
cd /d "%~dp0"
arena_server.exe
pause
"@
Set-Content -Path (Join-Path $packageDir "run-server.bat") -Value $runServer -Encoding ASCII

$runClient = @"
@echo off
cd /d "%~dp0"
if "%~1"=="" (
  arena_client.exe
) else (
  arena_client.exe %*
)
pause
"@
Set-Content -Path (Join-Path $packageDir "run-client.bat") -Value $runClient -Encoding ASCII

$notes = @"
Arena playtest package

1) Host:
   - Run run-server.bat
   - Open UDP port 40000 on firewall/router (or use LAN/VPN).

2) Players:
   - Run: run-client.bat <HOST_IP> 40000
   - Example: run-client.bat 192.168.1.20 40000

"@
Set-Content -Path (Join-Path $packageDir "PLAYTEST-README.txt") -Value $notes -Encoding ASCII

$zipPath = Join-Path $OutDir ($packageName + ".zip")
if (Test-Path $zipPath) {
    Remove-Item -Force $zipPath
}
Compress-Archive -Path (Join-Path $packageDir "*") -DestinationPath $zipPath

Write-Host "Packaged playtest build:"
Write-Host "  Folder: $packageDir"
Write-Host "  Zip:    $zipPath"
