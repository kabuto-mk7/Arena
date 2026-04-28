# Arena FPS Networking Demo

A tiny C++ multiplayer FPS networking skeleton.

It is intentionally plain:

- Authoritative UDP server.
- C++ raylib client with a real perspective 3D view.
- WASD movement, Space jump, Shift crouch, and mouse look.
- Server broadcasts player transforms to all clients.
- Large bright test room with server-side wall, floor, and ceiling collision.
- Remote players rendered as simple capsule avatars.
- No engine, assets, prediction, lag compensation, shooting, or gameplay rules yet.

## Build

This project is Windows-native. Networking uses Winsock, and rendering uses raylib.

The first CMake configure will download raylib automatically through `FetchContent`, so you need internet access for that configure step.

From a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build
cmake --build build --config Debug
```

Or use the helper:

```powershell
.\build.ps1
```

`build.ps1` also works when `cmake` is not on your PATH by locating the Visual Studio bundled CMake automatically.

## Share A Playtest Build

Use the release packager to build and bundle a shareable Windows playtest zip:

```powershell
.\release.ps1
```

If your normal local build is `Win32` (same as `build.ps1` default), keep that default.
If you want 64-bit explicitly, run:

```powershell
.\release.ps1 -Platform x64
```

This creates:

- `dist\arena-playtest-<timestamp>\` with:
  - `arena_server.exe`
  - `arena_client.exe`
  - dependency `.dll` files (if any)
  - `assets\`
  - `run-server.bat`
  - `run-client.bat`
  - `PLAYTEST-README.txt`
- `dist\arena-playtest-<timestamp>.zip`

Friends can unzip and run:

- host: `run-server.bat`
- client: `run-client.bat <HOST_IP> 40000`

The old zero-dependency MSVC batch file is still useful for the server, but the raylib client now expects the CMake build.

```bat
build-msvc.bat
```

## Run

Start the server:

```powershell
.\build\Debug\arena_server.exe
```

If you built only the server with `build-msvc.bat`, the server executable is directly under `build\` instead:

```powershell
.\build\arena_server.exe
```

Start one or more clients in separate terminals:

```powershell
.\build\Debug\arena_client.exe
```

By default both use `127.0.0.1:40000`. To connect to another host:

```powershell
.\build\Debug\arena_client.exe 192.168.1.20 40000
```

## Controls

- `WASD`: move
- `Space`: jump
- `Shift`: crouch
- `Mouse`: look around
- `Left/Right` or `Q/E`: turn fallback
- `Up/Down`: look fallback
- `Left Click`: recapture mouse if the window loses it
- `Esc`: quit

## Architecture

- [src/net.hpp](src/net.hpp): shared packets, socket helpers, vector math.
- [src/server.cpp](src/server.cpp): authoritative simulation and snapshots.
- [src/client.cpp](src/client.cpp): raylib input, 3D rendering, and snapshot display.

The client sends input intents. The server owns player positions and sends snapshots. This is the basic shape you would later extend with prediction, reconciliation, entity replication, gameplay commands, and snapshot compression.
