@echo off
setlocal

if not exist build mkdir build

cl /nologo /std:c++17 /EHsc /W4 /DWIN32_LEAN_AND_MEAN /Fo:build\ /Fe:build\arena_server.exe src\server.cpp ws2_32.lib
if errorlevel 1 exit /b %errorlevel%

echo Built build\arena_server.exe
echo The raylib client is built through CMake so raylib can be fetched and linked.
