#include "net.hpp"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int WindowWidth = 1920;
constexpr int WindowHeight = 1080;
constexpr float MouseSensitivity = 0.0015f;
constexpr float StepIntervalWalk = 0.37f;
constexpr float StepIntervalCrouch = 0.52f;
constexpr double FireIntervalSeconds = 0.24;
constexpr double EmptyIntervalSeconds = 0.2;
constexpr double ReloadFrameDuration = 0.13;
constexpr double KnifeAttackFrameDuration = 0.09;
constexpr double KnifeInspectFrameDuration = 0.13;
constexpr double KnifeEquipSlideDuration = 0.22;
constexpr float RecoilKickAmount = 18.0f;
constexpr int MaxShells = 2;
constexpr int MaxReserveAmmo = 32;
constexpr int AmmoPickupAmount = 6;

enum class WeaponSlot : uint8_t {
    Shotgun = 1,
    Knife = 3,
};

enum class WeaponAnimMode {
    Idle,
    ShotgunFire,
    ShotgunReload,
    KnifeAttack,
    KnifeInspect,
    KnifeEquip
};

struct RemotePlayer {
    arena::Vec3 position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool crouched = false;
    uint8_t teamId = 0;
    uint8_t health = 100;
    bool dead = false;
};

struct AmmoPack {
    Vector3 position{};
    bool active = true;
};

struct ClientState {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in serverAddress{};
    bool connected = false;
    uint32_t localPlayerId = 0;
    uint32_t inputSequence = 0;
    arena::Vec3 localPosition{0.0f, 0.0f, -6.0f};
    arena::Vec3 smoothedRenderPosition{0.0f, 0.0f, -6.0f};
    bool smoothedRenderPositionInitialized = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool localCrouched = false;
    std::map<uint32_t, RemotePlayer> players;

    bool assetsLoaded = false;
    bool audioReady = false;

    Texture2D wallTexture{};
    Texture2D floorTexture{};
    std::array<Texture2D, 8> shotgunFrames{};
    int shotgunFrameCount = 0;
    Texture2D knifeIdle{};
    std::array<Texture2D, 4> knifeAttackFrames{};
    int knifeAttackFrameCount = 0;
    std::array<Texture2D, 6> knifeInspectFrames{};
    int knifeInspectFrameCount = 0;

    Model floorModel{};
    Model ceilingModel{};
    Model wallModelX{};
    Model wallModelZ{};

    Sound shotgunFire{};
    Sound shotgunEmpty{};
    Sound ammoPickup{};
    std::array<Sound, 4> shotgunReloadSounds{};
    int shotgunReloadSoundCount = 0;
    Sound knifeEquip{};
    std::array<Sound, 4> footstepSounds{};
    int footstepSoundCount = 0;

    double nextFootstepAt = 0.0;
    WeaponAnimMode weaponAnimMode = WeaponAnimMode::Idle;
    double weaponAnimStartAt = 0.0;
    double nextWeaponActionAt = 0.0;
    std::array<bool, 4> reloadSoundPlayed{};
    int shellsInGun = MaxShells;
    int reserveAmmo = 12;
    WeaponSlot equippedWeapon = WeaponSlot::Knife;
    WeaponSlot lastEquippedWeapon = WeaponSlot::Shotgun;
    uint8_t localTeamId = 0;
    uint8_t localHealth = 100;
    bool localDead = false;
    float weaponBobPhase = 0.0f;
    float recoilOffset = 0.0f;
    bool jumpQueued = false;
    bool fireQueued = false;
    int wheelForwardTicks = 0;
    uint16_t team1Score = 0;
    uint16_t team2Score = 0;
    uint8_t hillOwnerTeam = 0;
    uint8_t hillCaptureTeam = 0;
    uint8_t hillContested = 0;
    float hillCaptureProgress = 0.0f;
    std::vector<AmmoPack> ammoPacks;
};

arena::Vec3 lerpVec3(arena::Vec3 a, arena::Vec3 b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

Vector3 toRaylib(arena::Vec3 value) {
    return {value.x, value.y, value.z};
}

Vector3 cameraForward(float yaw, float pitch) {
    return {
        static_cast<float>(std::sin(yaw)) * static_cast<float>(std::cos(pitch)),
        static_cast<float>(std::sin(pitch)),
        -static_cast<float>(std::cos(yaw)) * static_cast<float>(std::cos(pitch)),
    };
}

std::string resolveAssetPath(const std::string& relativePath) {
    static const std::array<const char*, 5> prefixes = {"", ".\\", "..\\", "..\\..\\", "..\\..\\..\\"};
    for (const char* prefix : prefixes) {
        const std::string candidate = std::string(prefix) + relativePath;
        if (FileExists(candidate.c_str())) {
            return candidate;
        }
    }
    throw std::runtime_error("Missing asset: " + relativePath);
}

Texture2D loadTextureAsset(const std::string& relativePath) {
    const std::string path = resolveAssetPath(relativePath);
    Texture2D texture = LoadTexture(path.c_str());
    if (texture.id == 0) {
        throw std::runtime_error("Failed to load texture: " + path);
    }
    SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(texture, TEXTURE_WRAP_REPEAT);
    return texture;
}

Sound loadSoundAsset(const std::string& relativePath) {
    const std::string path = resolveAssetPath(relativePath);
    Sound sound = LoadSound(path.c_str());
    if (sound.stream.buffer == nullptr) {
        throw std::runtime_error("Failed to load sound: " + path);
    }
    return sound;
}

void tileMeshUV(Mesh& mesh, float tileU, float tileV) {
    if (mesh.texcoords == nullptr || mesh.vertexCount <= 0) {
        return;
    }
    for (int i = 0; i < mesh.vertexCount; ++i) {
        mesh.texcoords[i * 2 + 0] *= tileU;
        mesh.texcoords[i * 2 + 1] *= tileV;
    }
    // GenMesh* uploads buffers on creation; push updated UVs back to GPU.
    UpdateMeshBuffer(mesh, 1, mesh.texcoords, mesh.vertexCount * 2 * static_cast<int>(sizeof(float)), 0);
}

void initAssets(ClientState& state) {
    state.wallTexture = loadTextureAsset("assets\\128x128\\128x128\\Tile\\Photoreal_Tile_03-512x512.png");
    state.floorTexture = loadTextureAsset("assets\\128x128\\128x128\\Concrete\\Photoreal_Concrete_03-512x512.png");

    Mesh floorMesh = GenMeshPlane(arena::RoomHalfSize * 2.0f, arena::RoomHalfSize * 2.0f, 32, 32);
    Mesh ceilingMesh = GenMeshPlane(arena::RoomHalfSize * 2.0f, arena::RoomHalfSize * 2.0f, 32, 32);
    Mesh wallMeshX = GenMeshCube(0.2f, arena::RoomHeight, arena::RoomHalfSize * 2.0f);
    Mesh wallMeshZ = GenMeshCube(arena::RoomHalfSize * 2.0f, arena::RoomHeight, 0.2f);

    // Tile textures across large surfaces so they repeat instead of stretching.
    tileMeshUV(floorMesh, 16.0f, 16.0f);
    tileMeshUV(ceilingMesh, 16.0f, 16.0f);
    tileMeshUV(wallMeshX, 16.0f, 16.0f);
    tileMeshUV(wallMeshZ, 16.0f, 16.0f);

    state.floorModel = LoadModelFromMesh(floorMesh);
    state.ceilingModel = LoadModelFromMesh(ceilingMesh);
    state.wallModelX = LoadModelFromMesh(wallMeshX);
    state.wallModelZ = LoadModelFromMesh(wallMeshZ);

    state.floorModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.floorTexture;
    state.ceilingModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.wallTexture;
    state.wallModelX.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.wallTexture;
    state.wallModelZ.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.wallTexture;

    static const std::array<const char*, 8> shotgunFrameFiles = {
        "assets\\weapons\\doubleshotgun\\idle.png",
        "assets\\weapons\\doubleshotgun\\reload1.png",
        "assets\\weapons\\doubleshotgun\\reload2.png",
        "assets\\weapons\\doubleshotgun\\reload3.png",
        "assets\\weapons\\doubleshotgun\\reload4.png",
        "assets\\weapons\\doubleshotgun\\reload5.png",
        "assets\\weapons\\doubleshotgun\\reload6.png",
        "assets\\weapons\\doubleshotgun\\reload7.png",
    };

    for (size_t i = 0; i < shotgunFrameFiles.size(); ++i) {
        state.shotgunFrames[i] = loadTextureAsset(shotgunFrameFiles[i]);
        SetTextureWrap(state.shotgunFrames[i], TEXTURE_WRAP_CLAMP);
    }
    state.shotgunFrameCount = static_cast<int>(shotgunFrameFiles.size());
    state.knifeIdle = loadTextureAsset("assets\\weapons\\karambit\\idle.png");
    SetTextureWrap(state.knifeIdle, TEXTURE_WRAP_CLAMP);
    state.knifeAttackFrames[0] = loadTextureAsset("assets\\weapons\\karambit\\attack1.png");
    state.knifeAttackFrames[1] = loadTextureAsset("assets\\weapons\\karambit\\attack2.png");
    state.knifeAttackFrames[2] = loadTextureAsset("assets\\weapons\\karambit\\attack3.png");
    state.knifeAttackFrames[3] = loadTextureAsset("assets\\weapons\\karambit\\attack4.png");
    for (Texture2D& frame : state.knifeAttackFrames) {
        SetTextureWrap(frame, TEXTURE_WRAP_CLAMP);
    }
    state.knifeAttackFrameCount = 4;
    state.knifeInspectFrames[0] = loadTextureAsset("assets\\weapons\\karambit\\inspect1.png");
    state.knifeInspectFrames[1] = loadTextureAsset("assets\\weapons\\karambit\\inspect2.png");
    state.knifeInspectFrames[2] = loadTextureAsset("assets\\weapons\\karambit\\inspect3.png");
    state.knifeInspectFrames[3] = loadTextureAsset("assets\\weapons\\karambit\\inspect4.png");
    state.knifeInspectFrames[4] = loadTextureAsset("assets\\weapons\\karambit\\inspect5.png");
    state.knifeInspectFrames[5] = loadTextureAsset("assets\\weapons\\karambit\\inspect6.png");
    for (Texture2D& frame : state.knifeInspectFrames) {
        SetTextureWrap(frame, TEXTURE_WRAP_CLAMP);
    }
    state.knifeInspectFrameCount = 6;

    InitAudioDevice();
    state.audioReady = true;
    state.shotgunFire = loadSoundAsset("assets\\weapons\\doubleshotgun\\fire.wav");
    state.shotgunEmpty = loadSoundAsset("assets\\weapons\\doubleshotgun\\empty.wav");
    state.ammoPickup = loadSoundAsset("assets\\sound\\ammo.wav");
    state.shotgunReloadSounds[0] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload1.wav");
    state.shotgunReloadSounds[1] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload2.wav");
    state.shotgunReloadSounds[2] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload3.wav");
    state.shotgunReloadSounds[3] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload4.wav");
    state.shotgunReloadSoundCount = 4;
    state.knifeEquip = loadSoundAsset("assets\\weapons\\karambit\\equip.wav");
    state.footstepSounds[0] = loadSoundAsset("assets\\sound\\boots1.wav");
    state.footstepSounds[1] = loadSoundAsset("assets\\sound\\boots2.wav");
    state.footstepSounds[2] = loadSoundAsset("assets\\sound\\boots3.wav");
    state.footstepSounds[3] = loadSoundAsset("assets\\sound\\boots4.wav");
    state.footstepSoundCount = 4;

    state.ammoPacks = {
        {{-12.0f, 0.35f, -10.0f}, true},
        {{14.0f, 0.35f, -8.0f}, true},
        {{-8.0f, 0.35f, 13.0f}, true},
        {{10.0f, 0.35f, 12.0f}, true},
        {{0.0f, 0.35f, 0.0f}, true},
    };

    state.assetsLoaded = true;
    state.weaponAnimMode = WeaponAnimMode::KnifeEquip;
    state.weaponAnimStartAt = arena::secondsNow();
    state.nextWeaponActionAt = state.weaponAnimStartAt + KnifeEquipSlideDuration;
}

void unloadAssets(ClientState& state) {
    if (!state.assetsLoaded) {
        return;
    }

    if (state.audioReady) {
        UnloadSound(state.shotgunFire);
        UnloadSound(state.shotgunEmpty);
        UnloadSound(state.ammoPickup);
        for (int i = 0; i < state.shotgunReloadSoundCount; ++i) {
            UnloadSound(state.shotgunReloadSounds[i]);
        }
        UnloadSound(state.knifeEquip);
        for (int i = 0; i < state.footstepSoundCount; ++i) {
            UnloadSound(state.footstepSounds[i]);
        }
        CloseAudioDevice();
        state.audioReady = false;
    }

    for (int i = 0; i < state.shotgunFrameCount; ++i) {
        UnloadTexture(state.shotgunFrames[i]);
    }
    if (state.knifeIdle.id != 0) {
        UnloadTexture(state.knifeIdle);
    }
    for (int i = 0; i < state.knifeAttackFrameCount; ++i) {
        UnloadTexture(state.knifeAttackFrames[i]);
    }
    for (int i = 0; i < state.knifeInspectFrameCount; ++i) {
        UnloadTexture(state.knifeInspectFrames[i]);
    }

    UnloadModel(state.floorModel);
    UnloadModel(state.ceilingModel);
    UnloadModel(state.wallModelX);
    UnloadModel(state.wallModelZ);

    UnloadTexture(state.wallTexture);
    UnloadTexture(state.floorTexture);
    state.assetsLoaded = false;
}

bool keyDown(int key) {
    return IsKeyDown(key);
}

void equipWeapon(ClientState& state, WeaponSlot slot, double now) {
    if (state.equippedWeapon == slot) {
        return;
    }
    state.lastEquippedWeapon = state.equippedWeapon;
    state.equippedWeapon = slot;
    state.recoilOffset = 0.0f;
    if (slot == WeaponSlot::Knife) {
        state.weaponAnimMode = WeaponAnimMode::KnifeEquip;
        state.weaponAnimStartAt = now;
        state.nextWeaponActionAt = now + KnifeEquipSlideDuration;
        if (state.audioReady) {
            PlaySound(state.knifeEquip);
        }
    } else {
        state.weaponAnimMode = WeaponAnimMode::Idle;
    }
}

void sendHello(ClientState& state) {
    arena::HelloPacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Hello);
    sendto(
        state.socket,
        reinterpret_cast<const char*>(&packet),
        sizeof(packet),
        0,
        reinterpret_cast<const sockaddr*>(&state.serverAddress),
        sizeof(state.serverAddress));
}

void updateLook(ClientState& state) {
    const Vector2 mouseDelta = GetMouseDelta();
    state.yaw += mouseDelta.x * MouseSensitivity;
    state.pitch -= mouseDelta.y * MouseSensitivity;

    constexpr float keyboardLookSpeed = 0.035f;
    if (keyDown(KEY_LEFT)) state.yaw -= keyboardLookSpeed;
    if (keyDown(KEY_RIGHT) || keyDown(KEY_E)) state.yaw += keyboardLookSpeed;
    if (keyDown(KEY_UP)) state.pitch += keyboardLookSpeed;
    if (keyDown(KEY_DOWN)) state.pitch -= keyboardLookSpeed;

    state.pitch = arena::clamp(state.pitch, -arena::MaxLookPitch, arena::MaxLookPitch);
}

void sendInput(ClientState& state) {
    float moveX = 0.0f;
    float moveZ = 0.0f;

    if (keyDown(KEY_A)) moveX -= 1.0f;
    if (keyDown(KEY_D)) moveX += 1.0f;
    if (keyDown(KEY_W)) moveZ += 1.0f;
    if (keyDown(KEY_S)) moveZ -= 1.0f;
    if (state.wheelForwardTicks > 0) {
        moveZ += 1.0f;
        state.wheelForwardTicks--;
    }
    moveZ = std::clamp(moveZ, -1.0f, 1.0f);

    updateLook(state);

    arena::InputPacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Input);
    packet.sequence = state.inputSequence++;
    packet.moveX = moveX;
    packet.moveZ = moveZ;
    packet.yaw = state.yaw;
    packet.pitch = state.pitch;
    packet.jumpPressed = state.jumpQueued ? 1 : 0;
    packet.firePressed = state.fireQueued ? 1 : 0;
    packet.crouchHeld = (keyDown(KEY_LEFT_SHIFT) || keyDown(KEY_RIGHT_SHIFT)) ? 1 : 0;
    packet.weaponSlot = static_cast<uint8_t>(state.equippedWeapon);
    state.jumpQueued = false;
    state.fireQueued = false;

    sendto(
        state.socket,
        reinterpret_cast<const char*>(&packet),
        sizeof(packet),
        0,
        reinterpret_cast<const sockaddr*>(&state.serverAddress),
        sizeof(state.serverAddress));
}

void pumpNetwork(ClientState& state) {
    char buffer[1500]{};
    sockaddr_in from{};
    int fromLength = sizeof(from);

    while (true) {
        const int received = recvfrom(
            state.socket,
            buffer,
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLength);

        if (received == SOCKET_ERROR) {
            if (WSAGetLastError() == WSAEWOULDBLOCK) {
                break;
            }
            break;
        }

        if (received == sizeof(arena::WelcomePacket) && arena::hasValidHeader(buffer, received, arena::PacketType::Welcome)) {
            arena::WelcomePacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            state.localPlayerId = packet.playerId;
            state.connected = true;
        } else if (received >= static_cast<int>(sizeof(arena::PacketHeader) + sizeof(uint32_t) * 2) &&
                   arena::hasValidHeader(buffer, received, arena::PacketType::Snapshot)) {
            arena::SnapshotPacket packet{};
            std::memcpy(&packet, buffer, std::min<int>(received, static_cast<int>(sizeof(packet))));

            std::map<uint32_t, RemotePlayer> nextPlayers;
            const uint32_t count = std::min<uint32_t>(packet.playerCount, arena::MaxPlayers);
            for (uint32_t i = 0; i < count; ++i) {
                RemotePlayer player{};
                player.position = {packet.players[i].x, packet.players[i].y, packet.players[i].z};
                player.yaw = packet.players[i].yaw;
                player.pitch = packet.players[i].pitch;
                player.crouched = packet.players[i].crouched != 0;
                player.teamId = packet.players[i].teamId;
                player.health = packet.players[i].health;
                player.dead = packet.players[i].dead != 0;
                nextPlayers[packet.players[i].playerId] = player;
            }
            state.players = std::move(nextPlayers);
            state.team1Score = packet.team1Score;
            state.team2Score = packet.team2Score;
            state.hillOwnerTeam = packet.hillOwnerTeam;
            state.hillCaptureTeam = packet.hillCaptureTeam;
            state.hillContested = packet.hillContested;
            state.hillCaptureProgress = packet.hillCaptureProgress;
        }
    }
}

void syncLocalPosition(ClientState& state) {
    const auto it = state.players.find(state.localPlayerId);
    if (it != state.players.end()) {
        state.localPosition = it->second.position;
        state.localCrouched = it->second.crouched;
        state.localTeamId = it->second.teamId;
        state.localHealth = it->second.health;
        state.localDead = it->second.dead;
        if (!state.smoothedRenderPositionInitialized) {
            state.smoothedRenderPosition = state.localPosition;
            state.smoothedRenderPositionInitialized = true;
        }
    }
}

void updateSmoothedRenderPosition(ClientState& state, float dt) {
    if (!state.smoothedRenderPositionInitialized) {
        state.smoothedRenderPosition = state.localPosition;
        state.smoothedRenderPositionInitialized = true;
        return;
    }
    // Exponential smoothing keeps camera motion stable between snapshot updates.
    const float follow = 1.0f - std::exp(-dt * 20.0f);
    state.smoothedRenderPosition = lerpVec3(state.smoothedRenderPosition, state.localPosition, follow);
}

void drawRoom(const ClientState& state) {
    constexpr float half = arena::RoomHalfSize;
    constexpr float height = arena::RoomHeight;
    constexpr float thickness = 0.2f;

    DrawModel(state.floorModel, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
    DrawModel(state.ceilingModel, {0.0f, height, 0.0f}, 1.0f, WHITE);

    DrawModel(state.wallModelX, {-half - thickness * 0.5f, height * 0.5f, 0.0f}, 1.0f, WHITE);
    DrawModel(state.wallModelX, {half + thickness * 0.5f, height * 0.5f, 0.0f}, 1.0f, WHITE);
    DrawModel(state.wallModelZ, {0.0f, height * 0.5f, -half - thickness * 0.5f}, 1.0f, WHITE);
    DrawModel(state.wallModelZ, {0.0f, height * 0.5f, half + thickness * 0.5f}, 1.0f, WHITE);

    DrawGrid(32, 5.0f);

    constexpr float hillRadius = 8.0f;
    DrawCylinderWires({0.0f, 0.05f, 0.0f}, hillRadius, hillRadius, 0.08f, 32, Color{245, 245, 210, 255});
    DrawCircle3D({0.0f, 0.06f, 0.0f}, hillRadius, {1.0f, 0.0f, 0.0f}, 90.0f, Color{245, 245, 210, 30});
    DrawSphere({0.0f, 0.45f, 0.0f}, 0.28f, Color{255, 240, 180, 220});

    for (const AmmoPack& pack : state.ammoPacks) {
        if (!pack.active) {
            continue;
        }
        DrawCube(pack.position, 0.7f, 0.35f, 0.7f, Color{205, 180, 72, 255});
        DrawCubeWires(pack.position, 0.7f, 0.35f, 0.7f, Color{255, 240, 170, 255});
    }
}

void drawPlayerCapsule(const RemotePlayer& player) {
    if (player.dead) {
        return;
    }
    const bool team1 = player.teamId == 1;
    const Color body = team1 ? Color{110, 170, 245, 255} : Color{245, 120, 120, 255};
    const Color outline = team1 ? Color{78, 124, 190, 255} : Color{185, 78, 78, 255};
    const Vector3 feet = toRaylib(player.position);
    const float playerHeight = player.crouched ? arena::CrouchHeight : arena::PlayerHeight;
    const float eyeHeight = player.crouched ? arena::CrouchEyeHeight : arena::StandEyeHeight;
    const Vector3 bodyCenter{feet.x, feet.y + playerHeight * 0.5f, feet.z};
    const float cylinderHeight = playerHeight - arena::PlayerRadius * 2.0f;

    DrawCylinder(bodyCenter, arena::PlayerRadius, arena::PlayerRadius, cylinderHeight, 18, body);
    DrawCylinderWires(bodyCenter, arena::PlayerRadius, arena::PlayerRadius, cylinderHeight, 18, outline);
    DrawSphere({feet.x, feet.y + arena::PlayerRadius, feet.z}, arena::PlayerRadius, body);
    DrawSphereWires({feet.x, feet.y + arena::PlayerRadius, feet.z}, arena::PlayerRadius, 12, 8, outline);
    DrawSphere({feet.x, feet.y + playerHeight - arena::PlayerRadius, feet.z}, arena::PlayerRadius, body);
    DrawSphereWires({feet.x, feet.y + playerHeight - arena::PlayerRadius, feet.z}, arena::PlayerRadius, 12, 8, outline);

    const Vector3 eye{feet.x, feet.y + eyeHeight, feet.z};
    const Vector3 facing = cameraForward(player.yaw, player.pitch);
    DrawLine3D(eye, Vector3Add(eye, Vector3Scale(facing, 1.2f)), outline);
}

void updateViewmodelAndFootsteps(ClientState& state, double now, float dt) {
    if (state.localDead) {
        return;
    }

    const bool moving = keyDown(KEY_W) || keyDown(KEY_A) || keyDown(KEY_S) || keyDown(KEY_D);
    const bool grounded = state.localPosition.y <= 0.05f;
    const bool knifeEquipped = state.equippedWeapon == WeaponSlot::Knife;

    if (knifeEquipped) {
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && now >= state.nextWeaponActionAt) {
            state.weaponAnimMode = WeaponAnimMode::KnifeAttack;
            state.weaponAnimStartAt = now;
            state.nextWeaponActionAt = now + static_cast<double>(state.knifeAttackFrameCount) * KnifeAttackFrameDuration;
        } else if (IsKeyPressed(KEY_F) && state.weaponAnimMode == WeaponAnimMode::Idle && now >= state.nextWeaponActionAt) {
            state.weaponAnimMode = WeaponAnimMode::KnifeInspect;
            state.weaponAnimStartAt = now;
            state.nextWeaponActionAt = now + static_cast<double>(state.knifeInspectFrameCount) * KnifeInspectFrameDuration;
        }
    } else {
        if (state.weaponAnimMode == WeaponAnimMode::Idle && state.shellsInGun == 0 && state.reserveAmmo > 0) {
            state.weaponAnimMode = WeaponAnimMode::ShotgunReload;
            state.weaponAnimStartAt = now;
            state.reloadSoundPlayed = {false, false, false, false};
        }

        if (IsKeyPressed(KEY_R) && state.weaponAnimMode != WeaponAnimMode::ShotgunReload && state.shellsInGun < MaxShells && state.reserveAmmo > 0) {
            state.weaponAnimMode = WeaponAnimMode::ShotgunReload;
            state.weaponAnimStartAt = now;
            state.reloadSoundPlayed = {false, false, false, false};
        }

        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && state.weaponAnimMode != WeaponAnimMode::ShotgunReload && now >= state.nextWeaponActionAt) {
            if (state.shellsInGun > 0) {
                state.shellsInGun--;
                state.weaponAnimMode = WeaponAnimMode::ShotgunFire;
                state.weaponAnimStartAt = now;
                state.nextWeaponActionAt = now + FireIntervalSeconds;
                state.recoilOffset += RecoilKickAmount;
                if (state.audioReady) {
                    PlaySound(state.shotgunFire);
                }
            } else {
                if (state.reserveAmmo <= 0) {
                    state.nextWeaponActionAt = now + EmptyIntervalSeconds;
                    if (state.audioReady) {
                        PlaySound(state.shotgunEmpty);
                    }
                }
            }
        }
    }

    if (moving && grounded) {
        const float cadence = state.localCrouched
            ? (knifeEquipped ? StepIntervalCrouch * 0.9f : StepIntervalCrouch)
            : (knifeEquipped ? StepIntervalWalk * 0.85f : StepIntervalWalk);
        state.weaponBobPhase += dt * (state.localCrouched ? (knifeEquipped ? 7.0f : 6.0f) : (knifeEquipped ? 11.0f : 9.0f));
        if (now >= state.nextFootstepAt && state.audioReady && state.footstepSoundCount > 0) {
            const int index = GetRandomValue(0, state.footstepSoundCount - 1);
            PlaySound(state.footstepSounds[index]);
            state.nextFootstepAt = now + cadence;
        }
    } else {
        state.weaponBobPhase += dt * 2.0f;
    }

    state.recoilOffset = arena::clamp(state.recoilOffset - dt * 85.0f, 0.0f, RecoilKickAmount * 2.0f);

    // Ammo pickup sweep.
    for (AmmoPack& pack : state.ammoPacks) {
        if (!pack.active) {
            continue;
        }
        const float dx = pack.position.x - state.localPosition.x;
        const float dz = pack.position.z - state.localPosition.z;
        const float distSq = dx * dx + dz * dz;
        if (distSq <= 1.6f * 1.6f) {
            pack.active = false;
            state.reserveAmmo = std::min(MaxReserveAmmo, state.reserveAmmo + AmmoPickupAmount);
            if (state.audioReady) {
                PlaySound(state.ammoPickup);
            }
        }
    }
}

int weaponFrameIndex(const ClientState& state, double now) {
    if (state.equippedWeapon == WeaponSlot::Knife) {
        if (state.knifeIdle.id == 0) {
            return -1;
        }
        if (state.weaponAnimMode == WeaponAnimMode::KnifeAttack) {
            const int step = static_cast<int>((now - state.weaponAnimStartAt) / KnifeAttackFrameDuration);
            return std::clamp(step, 0, state.knifeAttackFrameCount - 1);
        }
        if (state.weaponAnimMode == WeaponAnimMode::KnifeInspect) {
            const int step = static_cast<int>((now - state.weaponAnimStartAt) / KnifeInspectFrameDuration);
            return std::clamp(step, 0, state.knifeInspectFrameCount - 1);
        }
        if (state.weaponAnimMode == WeaponAnimMode::KnifeEquip) {
            return -1;
        }
        return -1;
    }

    if (state.shotgunFrameCount == 0) {
        return -1;
    }
    constexpr int idleFrame = 0;
    if (state.weaponAnimMode == WeaponAnimMode::Idle || state.weaponAnimMode == WeaponAnimMode::ShotgunFire) {
        return idleFrame;
    }
    static const std::array<int, 7> reloadSequence = {1, 2, 3, 4, 5, 6, 7};
    const int step = static_cast<int>((now - state.weaponAnimStartAt) / ReloadFrameDuration);
    return (step >= 0 && step < static_cast<int>(reloadSequence.size())) ? reloadSequence[step] : idleFrame;
}

void updateWeaponAnimationState(ClientState& state, double now) {
    if (state.weaponAnimMode == WeaponAnimMode::Idle) {
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::ShotgunFire) {
        constexpr double totalDuration = 0.08;
        if (now - state.weaponAnimStartAt >= totalDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::ShotgunReload) {
        static const std::array<int, 7> reloadSequence = {1, 2, 3, 4, 5, 6, 7};
        const double elapsed = now - state.weaponAnimStartAt;
        const int step = static_cast<int>(elapsed / ReloadFrameDuration);

        if (step >= 0 && step < static_cast<int>(reloadSequence.size()) && state.audioReady) {
            if (step < 4 && step < state.shotgunReloadSoundCount && !state.reloadSoundPlayed[step]) {
                PlaySound(state.shotgunReloadSounds[step]);
                state.reloadSoundPlayed[step] = true;
            }
        }

        const double totalDuration = static_cast<double>(reloadSequence.size()) * ReloadFrameDuration;
        if (elapsed >= totalDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
            state.reloadSoundPlayed = {false, false, false, false};
            const int needed = MaxShells - state.shellsInGun;
            const int toLoad = std::min(needed, state.reserveAmmo);
            state.shellsInGun += toLoad;
            state.reserveAmmo -= toLoad;
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::KnifeAttack) {
        const double totalDuration = static_cast<double>(state.knifeAttackFrameCount) * KnifeAttackFrameDuration;
        if (now - state.weaponAnimStartAt >= totalDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::KnifeInspect) {
        const double totalDuration = static_cast<double>(state.knifeInspectFrameCount) * KnifeInspectFrameDuration;
        if (now - state.weaponAnimStartAt >= totalDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::KnifeEquip) {
        if (now - state.weaponAnimStartAt >= KnifeEquipSlideDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
        }
    }
}

void drawViewmodel(const ClientState& state, double now) {
    if (state.localDead) {
        return;
    }

    const int frameIndex = weaponFrameIndex(state, now);
    const Texture2D* frame = nullptr;
    if (state.equippedWeapon == WeaponSlot::Knife) {
        if (state.weaponAnimMode == WeaponAnimMode::KnifeAttack && frameIndex >= 0 && frameIndex < state.knifeAttackFrameCount) {
            frame = &state.knifeAttackFrames[frameIndex];
        } else if (state.weaponAnimMode == WeaponAnimMode::KnifeInspect &&
                   frameIndex >= 0 && frameIndex < state.knifeInspectFrameCount) {
            frame = &state.knifeInspectFrames[frameIndex];
        } else {
            frame = &state.knifeIdle;
        }
    } else {
        if (frameIndex < 0 || frameIndex >= state.shotgunFrameCount) {
            return;
        }
        frame = &state.shotgunFrames[frameIndex];
    }

    if (frame == nullptr) {
        return;
    }

    const Texture2D& frameTex = *frame;
    if (frameTex.id == 0) {
        return;
    }

    const float screenWidth = static_cast<float>(GetScreenWidth());
    const float screenHeight = static_cast<float>(GetScreenHeight());
    const bool knifeEquipped = state.equippedWeapon == WeaponSlot::Knife;
    const float targetWidth = screenWidth * (knifeEquipped ? 0.47f : 0.50f);
    const float scale = targetWidth / static_cast<float>(frameTex.width);
    const float drawWidth = static_cast<float>(frameTex.width) * scale;
    const float drawHeight = static_cast<float>(frameTex.height) * scale;
    const float bobY = std::sin(state.weaponBobPhase) * (knifeEquipped ? 6.0f : 8.0f);
    const float recoilY = knifeEquipped ? 0.0f : state.recoilOffset;
    float equipSlideY = 0.0f;
    if (knifeEquipped && state.weaponAnimMode == WeaponAnimMode::KnifeEquip) {
        const float t = std::clamp(static_cast<float>((now - state.weaponAnimStartAt) / KnifeEquipSlideDuration), 0.0f, 1.0f);
        equipSlideY = (1.0f - t) * (drawHeight + 80.0f);
    }

    const float baseX = (screenWidth - drawWidth) * 0.5f;
    const float baseY = knifeEquipped ? (screenHeight - drawHeight + 70.0f) : (screenHeight - drawHeight + 92.0f);
    const Vector2 pos{
        baseX,
        baseY + bobY + recoilY + equipSlideY
    };

    DrawTextureEx(frameTex, pos, 0.0f, scale, WHITE);
}

void render(ClientState& state, double now) {
    const float localEyeHeight = state.localCrouched ? arena::CrouchEyeHeight : arena::StandEyeHeight;
    const Vector3 eye{state.smoothedRenderPosition.x, state.smoothedRenderPosition.y + localEyeHeight, state.smoothedRenderPosition.z};
    const Vector3 forward = cameraForward(state.yaw, state.pitch);

    Camera3D camera{};
    camera.position = eye;
    camera.target = Vector3Add(eye, forward);
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = 75.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    BeginDrawing();
    ClearBackground(Color{18, 18, 20, 255});

    BeginMode3D(camera);
    drawRoom(state);

    for (const auto& [id, player] : state.players) {
        if (id == state.localPlayerId) {
            continue;
        }
        drawPlayerCapsule(player);
    }

    EndMode3D();
    drawViewmodel(state, now);

    const int cx = GetScreenWidth() / 2;
    const int cy = GetScreenHeight() / 2;
    DrawLine(cx - 8, cy, cx + 8, cy, RED);
    DrawLine(cx, cy - 8, cx, cy + 8, RED);

    const std::string status = state.connected
        ? "Connected as player " + std::to_string(state.localPlayerId) + " | players: " + std::to_string(state.players.size())
        : "Connecting to server...";
    DrawText(status.c_str(), 16, 16, 20, RAYWHITE);
    const std::string weaponLabel = (state.equippedWeapon == WeaponSlot::Knife) ? "Karambit" : "Shotgun";
    const std::string ammoText = (state.equippedWeapon == WeaponSlot::Shotgun)
        ? ("Shotgun: " + std::to_string(state.shellsInGun) + "/" + std::to_string(MaxShells) + " | Reserve: " + std::to_string(state.reserveAmmo))
        : "Karambit ready";
    DrawText(("Weapon: " + weaponLabel).c_str(), 16, 42, 20, RAYWHITE);
    DrawText(ammoText.c_str(), 16, 68, 20, RAYWHITE);
    const std::string teamText = "Team " + std::to_string(state.localTeamId == 0 ? 1 : state.localTeamId) +
        " | HP: " + std::to_string(state.localHealth) +
        " | KOTH score T1: " + std::to_string(state.team1Score) + "  T2: " + std::to_string(state.team2Score);
    DrawText(teamText.c_str(), 16, 94, 20, RAYWHITE);
    std::string hillText = "Hill: Neutral";
    if (state.hillContested != 0) {
        hillText = "Hill: Contested";
    }
    if (state.hillOwnerTeam == 1) hillText = "Hill: Owned by Team 1";
    if (state.hillOwnerTeam == 2) hillText = "Hill: Owned by Team 2";
    if (state.hillContested == 0 && state.hillOwnerTeam == 0 && state.hillCaptureTeam != 0) {
        hillText = "Hill: Team " + std::to_string(state.hillCaptureTeam) + " capturing " +
            std::to_string(static_cast<int>(state.hillCaptureProgress * 100.0f)) + "%";
    }
    DrawText(hillText.c_str(), 16, 120, 20, Color{245, 235, 170, 255});
    if (state.localDead) {
        DrawText("You are dead - respawning...", GetScreenWidth() / 2 - 190, GetScreenHeight() / 2 - 70, 30, RED);
    }
    DrawText("WASD move | Mouse look | Space/wheel-down jump | wheel-up forward | Shift crouch | 1 shotgun | 3 knife | LMB attack/fire | F inspect | Esc quit", 16, GetScreenHeight() - 32, 18, LIGHTGRAY);
    DrawFPS(GetScreenWidth() - 95, 12);

    EndDrawing();
}

} // namespace

int main(int argc, char** argv) {
    try {
        const char* host = argc >= 2 ? argv[1] : "127.0.0.1";
        const uint16_t port = argc >= 3 ? static_cast<uint16_t>(std::stoi(argv[2])) : arena::DefaultPort;

        arena::requireWinsock();

        ClientState state{};
        state.serverAddress = arena::makeAddress(host, port);
        state.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (state.socket == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }
        arena::makeNonBlocking(state.socket);

        SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_UNDECORATED);
        InitWindow(WindowWidth, WindowHeight, "Arena FPS Networking Demo");
        SetWindowPosition(0, 0);
        DisableCursor();
        initAssets(state);

        double nextHelloAt = 0.0;
        double nextInputAt = 0.0;
        std::cout << "Connecting to " << host << ":" << port << "\n";

        while (!WindowShouldClose()) {
            if (IsWindowFocused() && !IsCursorHidden()) {
                DisableCursor();
            }

            const float wheelMove = GetMouseWheelMove();
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                state.fireQueued = true;
            }
            if (IsKeyPressed(KEY_SPACE)) {
                state.jumpQueued = true;
            }
            if (wheelMove < 0.0f) {
                state.jumpQueued = true;
            }
            if (wheelMove > 0.0f) {
                state.wheelForwardTicks = std::max(state.wheelForwardTicks, 2);
            }
            if (IsKeyPressed(KEY_ONE)) {
                equipWeapon(state, WeaponSlot::Shotgun, arena::secondsNow());
            }
            if (IsKeyPressed(KEY_THREE)) {
                equipWeapon(state, WeaponSlot::Knife, arena::secondsNow());
            }
            if (IsKeyPressed(KEY_Q)) {
                WeaponSlot target = state.lastEquippedWeapon;
                if (target == state.equippedWeapon) {
                    target = (state.equippedWeapon == WeaponSlot::Knife) ? WeaponSlot::Shotgun : WeaponSlot::Knife;
                }
                equipWeapon(state, target, arena::secondsNow());
            }

            const double now = arena::secondsNow();
            const float dt = GetFrameTime();
            if (!state.connected && now >= nextHelloAt) {
                sendHello(state);
                nextHelloAt = now + 0.5;
            }

            if (now >= nextInputAt) {
                sendInput(state);
                nextInputAt = now + arena::TickSeconds;
            } else {
                updateLook(state);
            }

            pumpNetwork(state);
            syncLocalPosition(state);
            updateSmoothedRenderPosition(state, dt);
            updateViewmodelAndFootsteps(state, now, dt);
            updateWeaponAnimationState(state, now);
            render(state, now);
        }

        unloadAssets(state);
        EnableCursor();
        CloseWindow();
        closesocket(state.socket);
        WSACleanup();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Client error: " << error.what() << "\n";
        if (IsWindowReady()) {
            CloseWindow();
        }
        return 1;
    }
}
