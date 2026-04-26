param(
    [string]$Config = "Debug",
    [string]$Platform = "win32"
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

if ($Platform -ieq "Win32") {
    $Platform = "win32"
}

& $cmake -S . -B build -A $Platform
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build build --config $Config
exit $LASTEXITCODE
