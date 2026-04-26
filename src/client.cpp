#include "net.hpp"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr int WindowWidth = 1920;
constexpr int WindowHeight = 1080;
constexpr float DefaultMouseSensitivity = 0.0009f;
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

enum class ScreenMode {
    MainMenu,
    Settings,
    InGame
};

struct RemotePlayer {
    arena::Vec3 position{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool crouched = false;
    uint8_t teamId = 0;
    uint8_t health = 100;
    bool dead = false;
    uint16_t hitConfirmCount = 0;
    uint8_t lastDamageDealt = 0;
    uint32_t lastHitTargetId = 0;
};

struct AmmoPack {
    Vector3 position{};
    bool active = true;
};

struct DamagePopup {
    Vector3 worldPos{};
    int damage = 0;
    float age = 0.0f;
    float lifetime = 0.85f;
};

struct ClientState {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in serverAddress{};
    bool connected = false;
    ScreenMode screenMode = ScreenMode::MainMenu;
    int menuIndex = 0;
    int settingsIndex = 0;
    float mouseSensitivity = DefaultMouseSensitivity;
    bool hitSoundEnabled = true;
    float hitSoundVolume = 0.65f;
    double mainMenuOpenedAt = 0.0;
    uint32_t localPlayerId = 0;
    uint32_t inputSequence = 0;
    arena::Vec3 localPosition{0.0f, 0.0f, -6.0f};
    arena::Vec3 smoothedRenderPosition{0.0f, 0.0f, -6.0f};
    bool smoothedRenderPositionInitialized = false;
    arena::Vec3 lastSpeedSamplePosition{0.0f, 0.0f, -6.0f};
    bool speedSampleInitialized = false;
    float currentSpeed = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool localCrouched = false;
    std::map<uint32_t, RemotePlayer> players;

    bool assetsLoaded = false;
    bool audioReady = false;

    Texture2D wallTexture{};
    Texture2D floorTexture{};
    Texture2D menuBackgroundTexture{};
    bool menuBackgroundLoaded = false;
    Texture2D logoTexture{};
    bool logoLoaded = false;
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
    Sound hitSound{};
    bool hitSoundLoaded = false;
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
    uint16_t localHitConfirmCount = 0;
    bool localHitConfirmInitialized = false;
    bool localDead = false;
    float weaponBobPhase = 0.0f;
    float recoilOffset = 0.0f;
    bool jumpQueued = false;
    bool fireQueued = false;
    bool dashQueued = false;
    int8_t dashMoveX = 0;
    int8_t dashMoveZ = 0;
    double lastWPressAt = -10.0;
    double lastAPressAt = -10.0;
    double lastSPressAt = -10.0;
    double lastDPressAt = -10.0;
    int wheelForwardTicks = 0;
    uint16_t team1TimeLeftSeconds = 180;
    uint16_t team2TimeLeftSeconds = 180;
    uint8_t hillOwnerTeam = 0;
    uint8_t hillCaptureTeam = 0;
    uint8_t hillContested = 0;
    uint8_t hillOvertime = 0;
    uint8_t hillWinnerTeam = 0;
    float hillCaptureProgress = 0.0f;
    std::vector<AmmoPack> ammoPacks;
    std::vector<DamagePopup> damagePopups;
};

arena::Vec3 lerpVec3(arena::Vec3 a, arena::Vec3 b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

float easeOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

std::string formatClock(uint16_t secondsLeft) {
    const int total = static_cast<int>(secondsLeft);
    const int minutes = total / 60;
    const int seconds = total % 60;
    char buffer[16]{};
    std::snprintf(buffer, sizeof(buffer), "%d:%02d", minutes, seconds);
    return std::string(buffer);
}

void drawTextureCover(const Texture2D& texture, int screenWidth, int screenHeight, Color tint) {
    if (texture.id == 0 || texture.width <= 0 || texture.height <= 0) {
        return;
    }
    const float sx = static_cast<float>(screenWidth) / static_cast<float>(texture.width);
    const float sy = static_cast<float>(screenHeight) / static_cast<float>(texture.height);
    const float scale = std::max(sx, sy);
    const float drawW = static_cast<float>(texture.width) * scale;
    const float drawH = static_cast<float>(texture.height) * scale;
    const float x = (static_cast<float>(screenWidth) - drawW) * 0.5f;
    const float y = (static_cast<float>(screenHeight) - drawH) * 0.5f;
    DrawTextureEx(texture, {x, y}, 0.0f, scale, tint);
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
    try {
        state.menuBackgroundTexture = loadTextureAsset("assets\\bg.png");
        SetTextureWrap(state.menuBackgroundTexture, TEXTURE_WRAP_CLAMP);
        state.menuBackgroundLoaded = state.menuBackgroundTexture.id != 0;
    } catch (...) {
        state.menuBackgroundLoaded = false;
    }
    try {
        state.logoTexture = loadTextureAsset("assets\\logo.png");
        SetTextureWrap(state.logoTexture, TEXTURE_WRAP_CLAMP);
        state.logoLoaded = state.logoTexture.id != 0;
    } catch (...) {
        state.logoLoaded = false;
    }

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
    try {
        state.hitSound = loadSoundAsset("assets\\sound\\hit.mp3");
        state.hitSoundLoaded = state.hitSound.stream.buffer != nullptr;
    } catch (...) {
        try {
            state.hitSound = loadSoundAsset("assets\\sound\\hit.wav");
            state.hitSoundLoaded = state.hitSound.stream.buffer != nullptr;
        } catch (...) {
            state.hitSoundLoaded = false;
        }
    }
    if (state.hitSoundLoaded) {
        SetSoundVolume(state.hitSound, state.hitSoundVolume);
    }
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
        if (state.hitSoundLoaded) {
            UnloadSound(state.hitSound);
            state.hitSoundLoaded = false;
        }
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
    if (state.menuBackgroundLoaded && state.menuBackgroundTexture.id != 0) {
        UnloadTexture(state.menuBackgroundTexture);
    }
    if (state.logoLoaded && state.logoTexture.id != 0) {
        UnloadTexture(state.logoTexture);
    }
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
    state.yaw += mouseDelta.x * state.mouseSensitivity;
    state.pitch -= mouseDelta.y * state.mouseSensitivity;

    constexpr float keyboardLookSpeed = 0.035f;
    if (keyDown(KEY_LEFT)) state.yaw -= keyboardLookSpeed;
    if (keyDown(KEY_RIGHT) || keyDown(KEY_E)) state.yaw += keyboardLookSpeed;
    if (keyDown(KEY_UP)) state.pitch += keyboardLookSpeed;
    if (keyDown(KEY_DOWN)) state.pitch -= keyboardLookSpeed;

    state.pitch = arena::clamp(state.pitch, -arena::MaxLookPitch, arena::MaxLookPitch);
}

bool drawMenuButton(const Rectangle& rect, const char* text, bool selected) {
    const Vector2 mouse = GetMousePosition();
    const bool hovered = CheckCollisionPointRec(mouse, rect);
    const bool active = selected || hovered;
    const int fontSize = active ? 48 : 44;
    const int tw = MeasureText(text, fontSize);
    const int tx = static_cast<int>(rect.x + (rect.width - tw) * 0.5f);
    const int ty = static_cast<int>(rect.y + (rect.height - fontSize) * 0.5f - 2.0f);
    DrawText(text, tx + 4, ty + 4, fontSize, Color{0, 0, 0, 180});
    if (active) {
        DrawText(">", static_cast<int>(rect.x + 8), ty, fontSize, Color{255, 206, 146, 255});
        DrawText(text, tx, ty, fontSize, Color{255, 228, 188, 255});
    } else {
        DrawText(text, tx, ty, fontSize, Color{220, 226, 232, 242});
    }
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

void drawMainMenu(ClientState& state) {
    BeginDrawing();
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    if (state.menuBackgroundLoaded && state.menuBackgroundTexture.id != 0) {
        drawTextureCover(state.menuBackgroundTexture, sw, sh, Color{255, 255, 255, 255});
    } else {
        ClearBackground(Color{16, 18, 24, 255});
    }

    const float menuTime = static_cast<float>(GetTime() - state.mainMenuOpenedAt);
    const float intro = easeOutCubic(menuTime / 1.1f);

    const float leftSpaceW = sw * 0.35f;
    const float columnW = std::min(500.0f, leftSpaceW * 0.82f);
    const float columnX = (leftSpaceW - columnW) * 0.5f;
    const float baseY = sh * 0.56f;

    if (state.logoLoaded && state.logoTexture.id != 0) {
        const float floatY = std::sin(menuTime * 1.9f) * 7.0f;
        const float pulse = 1.0f + std::sin(menuTime * 2.6f) * 0.02f;
        const float targetLogoWidth = columnW * 0.92f;
        const float fitScale = targetLogoWidth / static_cast<float>(state.logoTexture.width);
        const float scale = (fitScale * (0.90f + intro * 0.10f)) * pulse;
        const float drawW = static_cast<float>(state.logoTexture.width) * scale;
        const float drawH = static_cast<float>(state.logoTexture.height) * scale;
        const float x = columnX + (columnW - drawW) * 0.5f;
        const float y = -drawH * (1.0f - intro) + sh * 0.11f + floatY;
        const unsigned char alpha = static_cast<unsigned char>(std::clamp(intro, 0.0f, 1.0f) * 255.0f);
        DrawTextureEx(state.logoTexture, {x + 6.0f, y + 6.0f}, std::sin(menuTime * 1.2f) * 1.2f, scale, Color{0, 0, 0, static_cast<unsigned char>(alpha * 0.62f)});
        DrawTextureEx(state.logoTexture, {x, y}, std::sin(menuTime * 1.2f) * 1.6f, scale, Color{255, 255, 255, alpha});
    }

    Rectangle buttons[3] = {
        {columnX, baseY, columnW, 68.0f},
        {columnX, baseY + 92.0f, columnW, 68.0f},
        {columnX, baseY + 184.0f, columnW, 68.0f}
    };

    if (IsKeyPressed(KEY_UP)) state.menuIndex = (state.menuIndex + 2) % 3;
    if (IsKeyPressed(KEY_DOWN)) state.menuIndex = (state.menuIndex + 1) % 3;

    bool joinClicked = drawMenuButton(buttons[0], "Join Game", state.menuIndex == 0);
    bool settingsClicked = drawMenuButton(buttons[1], "Settings", state.menuIndex == 1);
    bool quitClicked = drawMenuButton(buttons[2], "Quit Game", state.menuIndex == 2);

    const bool activate = IsKeyPressed(KEY_ENTER);
    if (joinClicked || (activate && state.menuIndex == 0)) {
        state.screenMode = ScreenMode::InGame;
        state.connected = false;
        state.localPlayerId = 0;
        state.players.clear();
        DisableCursor();
    } else if (settingsClicked || (activate && state.menuIndex == 1)) {
        state.screenMode = ScreenMode::Settings;
    } else if (quitClicked || (activate && state.menuIndex == 2)) {
        CloseWindow();
    }

    const char* hint = "Use Up/Down + Enter, or mouse";
    const int hintSize = 24;
    const int hx = static_cast<int>(columnX + (columnW - MeasureText(hint, hintSize)) * 0.5f);
    const int hy = static_cast<int>(baseY + 292.0f);
    DrawText(hint, hx + 3, hy + 3, hintSize, Color{0, 0, 0, 175});
    DrawText(hint, hx, hy, hintSize, Color{204, 210, 220, 242});
    EndDrawing();
}

void drawSettingsMenu(ClientState& state) {
    BeginDrawing();
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    if (state.menuBackgroundLoaded && state.menuBackgroundTexture.id != 0) {
        drawTextureCover(state.menuBackgroundTexture, sw, sh, Color{255, 255, 255, 255});
        DrawRectangle(0, 0, static_cast<int>(sw * 0.34f), sh, Color{9, 14, 22, 200});
        DrawRectangleGradientH(static_cast<int>(sw * 0.32f), 0, static_cast<int>(sw * 0.14f), sh, Color{9, 14, 22, 200}, Color{9, 14, 22, 0});
    } else {
        ClearBackground(Color{16, 18, 24, 255});
    }

    const float columnX = std::max(48.0f, sw * 0.08f);
    const float columnW = std::min(460.0f, sw * 0.26f);
    DrawText("Settings", static_cast<int>(columnX + (columnW - MeasureText("Settings", 72)) * 0.5f), 120, 72, Color{228, 236, 255, 255});

    if (IsKeyPressed(KEY_UP)) state.settingsIndex = (state.settingsIndex + 2) % 3;
    if (IsKeyPressed(KEY_DOWN)) state.settingsIndex = (state.settingsIndex + 1) % 3;

    const bool leftAdjust = IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A);
    const bool rightAdjust = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D);
    const bool toggle = IsKeyPressed(KEY_ENTER);

    if (state.settingsIndex == 0) {
        if (leftAdjust) state.mouseSensitivity = std::max(0.0003f, state.mouseSensitivity - 0.0001f);
        if (rightAdjust) state.mouseSensitivity = std::min(0.006f, state.mouseSensitivity + 0.0001f);
    } else if (state.settingsIndex == 1) {
        if (leftAdjust || rightAdjust || toggle) state.hitSoundEnabled = !state.hitSoundEnabled;
    } else if (state.settingsIndex == 2) {
        if (leftAdjust) state.hitSoundVolume = std::max(0.0f, state.hitSoundVolume - 0.05f);
        if (rightAdjust) state.hitSoundVolume = std::min(1.0f, state.hitSoundVolume + 0.05f);
        if (state.hitSoundLoaded) {
            SetSoundVolume(state.hitSound, state.hitSoundVolume);
        }
    }

    auto drawSettingRow = [&](int index, float y, const char* label, const char* value) {
        const bool selected = state.settingsIndex == index;
        const Color labelColor = selected ? Color{245, 230, 196, 255} : RAYWHITE;
        const Color valueColor = selected ? Color{255, 210, 145, 255} : Color{224, 214, 195, 255};
        DrawText(label, static_cast<int>(columnX + 24.0f), static_cast<int>(y), 32, labelColor);
        const int vw = MeasureText(value, 32);
        DrawText(value, static_cast<int>(columnX + columnW - vw - 24.0f), static_cast<int>(y), 32, valueColor);
    };

    drawSettingRow(0, 272.0f, "Mouse Sensitivity", TextFormat("%.4f", state.mouseSensitivity));
    drawSettingRow(1, 336.0f, "Hit Sound", state.hitSoundEnabled ? "On" : "Off");
    drawSettingRow(2, 400.0f, "Hit Volume", TextFormat("%d%%", static_cast<int>(std::round(state.hitSoundVolume * 100.0f))));

    DrawText("Up/Down to select | Left/Right to adjust", static_cast<int>(columnX + (columnW - MeasureText("Up/Down to select | Left/Right to adjust", 24)) * 0.5f), 470, 24, Color{172, 178, 192, 255});
    DrawText("Press Esc to return", static_cast<int>(columnX + (columnW - MeasureText("Press Esc to return", 24)) * 0.5f), 504, 24, Color{172, 178, 192, 255});

    if (IsKeyPressed(KEY_ESCAPE)) {
        state.screenMode = ScreenMode::MainMenu;
        state.mainMenuOpenedAt = GetTime();
    }

    EndDrawing();
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
    const bool holdJump = keyDown(KEY_SPACE);
    packet.jumpPressed = (state.jumpQueued || holdJump) ? 1 : 0;
    packet.firePressed = state.fireQueued ? 1 : 0;
    packet.dashPressed = state.dashQueued ? 1 : 0;
    packet.dashMoveX = state.dashMoveX;
    packet.dashMoveZ = state.dashMoveZ;
    packet.crouchHeld = (keyDown(KEY_LEFT_SHIFT) || keyDown(KEY_RIGHT_SHIFT)) ? 1 : 0;
    packet.weaponSlot = static_cast<uint8_t>(state.equippedWeapon);
    state.jumpQueued = false;
    state.fireQueued = false;
    state.dashQueued = false;
    state.dashMoveX = 0;
    state.dashMoveZ = 0;

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
                player.hitConfirmCount = packet.players[i].hitConfirmCount;
                player.lastDamageDealt = packet.players[i].lastDamageDealt;
                player.lastHitTargetId = packet.players[i].lastHitTargetId;
                nextPlayers[packet.players[i].playerId] = player;
            }
            state.players = std::move(nextPlayers);
            state.team1TimeLeftSeconds = packet.team1TimeLeftSeconds;
            state.team2TimeLeftSeconds = packet.team2TimeLeftSeconds;
            state.hillOwnerTeam = packet.hillOwnerTeam;
            state.hillCaptureTeam = packet.hillCaptureTeam;
            state.hillContested = packet.hillContested;
            state.hillOvertime = packet.hillOvertime;
            state.hillWinnerTeam = packet.hillWinnerTeam;
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
        if (state.localHitConfirmInitialized && it->second.hitConfirmCount != state.localHitConfirmCount &&
            state.hitSoundEnabled && state.hitSoundLoaded && state.audioReady) {
            PlaySound(state.hitSound);
        }
        if (state.localHitConfirmInitialized && it->second.hitConfirmCount != state.localHitConfirmCount) {
            const auto targetIt = state.players.find(it->second.lastHitTargetId);
            if (targetIt != state.players.end()) {
                const float targetHeight = targetIt->second.crouched ? arena::CrouchHeight : arena::PlayerHeight;
                DamagePopup popup{};
                popup.damage = static_cast<int>(it->second.lastDamageDealt);
                popup.worldPos = {targetIt->second.position.x, targetIt->second.position.y + targetHeight + 0.28f, targetIt->second.position.z};
                state.damagePopups.push_back(popup);
            }
        }
        state.localHitConfirmCount = it->second.hitConfirmCount;
        state.localHitConfirmInitialized = true;
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

void updateSpeedCounter(ClientState& state, float dt) {
    if (!state.speedSampleInitialized) {
        state.lastSpeedSamplePosition = state.localPosition;
        state.speedSampleInitialized = true;
        state.currentSpeed = 0.0f;
        return;
    }
    if (dt <= 0.00001f) {
        return;
    }

    const float dx = state.localPosition.x - state.lastSpeedSamplePosition.x;
    const float dz = state.localPosition.z - state.lastSpeedSamplePosition.z;
    const float instSpeed = std::sqrt(dx * dx + dz * dz) / dt;
    state.currentSpeed += (instSpeed - state.currentSpeed) * std::clamp(dt * 18.0f, 0.0f, 1.0f);
    state.lastSpeedSamplePosition = state.localPosition;
}

void updateDamagePopups(ClientState& state, float dt) {
    for (DamagePopup& popup : state.damagePopups) {
        popup.age += dt;
        popup.worldPos.y += dt * 1.25f;
    }
    state.damagePopups.erase(
        std::remove_if(state.damagePopups.begin(), state.damagePopups.end(), [](const DamagePopup& popup) {
            return popup.age >= popup.lifetime;
        }),
        state.damagePopups.end());
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
    for (const DamagePopup& popup : state.damagePopups) {
        const float t = std::clamp(popup.age / popup.lifetime, 0.0f, 1.0f);
        const unsigned char alpha = static_cast<unsigned char>((1.0f - t) * 255.0f);
        const Vector2 p = GetWorldToScreen(popup.worldPos, camera);
        if (p.x < -100.0f || p.y < -100.0f || p.x > static_cast<float>(GetScreenWidth()) + 100.0f || p.y > static_cast<float>(GetScreenHeight()) + 100.0f) {
            continue;
        }
        const char* txt = TextFormat("%d", popup.damage);
        const int fs = 28;
        const int tw = MeasureText(txt, fs);
        DrawText(txt, static_cast<int>(p.x - tw * 0.5f + 2.0f), static_cast<int>(p.y + 2.0f), fs, Color{0, 0, 0, static_cast<unsigned char>(alpha * 0.7f)});
        DrawText(txt, static_cast<int>(p.x - tw * 0.5f), static_cast<int>(p.y), fs, Color{255, 205, 125, alpha});
    }
    drawViewmodel(state, now);

    const int cx = GetScreenWidth() / 2;
    const int cy = GetScreenHeight() / 2;
    DrawLine(cx - 8, cy, cx + 8, cy, RED);
    DrawLine(cx, cy - 8, cx, cy + 8, RED);
    DrawText(TextFormat("%.1f u/s", state.currentSpeed), cx + 14, cy + 14, 20, Color{225, 232, 245, 255});

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
    const std::string t1Clock = formatClock(state.team1TimeLeftSeconds);
    const std::string t2Clock = formatClock(state.team2TimeLeftSeconds);
    const std::string teamText = "Team " + std::to_string(state.localTeamId == 0 ? 1 : state.localTeamId) +
        " | HP: " + std::to_string(state.localHealth) + " | KOTH clock T1: " + t1Clock + "  T2: " + t2Clock;
    DrawText(teamText.c_str(), 16, 94, 20, RAYWHITE);
    std::string hillText = "Hill: Neutral";
    if (state.hillWinnerTeam != 0) {
        hillText = "Round Over: Team " + std::to_string(state.hillWinnerTeam) + " wins";
    } else if (state.hillOvertime != 0) {
        hillText = "Hill: OVERTIME";
    } else if (state.hillContested != 0) {
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
    DrawText("WASD move | Mouse look | Space/wheel-down jump | wheel-up forward | A/D double-tap air dash | Shift crouch | 1 shotgun | 3 knife | LMB attack/fire | F inspect | Esc menu", 16, GetScreenHeight() - 32, 18, LIGHTGRAY);
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
        InitWindow(WindowWidth, WindowHeight, "KOTH");
        SetExitKey(KEY_NULL);
        SetWindowPosition(0, 0);
        EnableCursor();
        state.mainMenuOpenedAt = GetTime();
        initAssets(state);

        double nextHelloAt = 0.0;
        double nextInputAt = 0.0;
        std::cout << "Connecting to " << host << ":" << port << "\n";

        while (!WindowShouldClose()) {
            if (state.screenMode == ScreenMode::MainMenu) {
                if (IsCursorHidden()) {
                    EnableCursor();
                }
                drawMainMenu(state);
                continue;
            }

            if (state.screenMode == ScreenMode::Settings) {
                if (IsCursorHidden()) {
                    EnableCursor();
                }
                drawSettingsMenu(state);
                continue;
            }

            if (IsWindowFocused() && !IsCursorHidden()) {
                DisableCursor();
            }

            const float wheelMove = GetMouseWheelMove();
            const double inputNow = arena::secondsNow();
            const bool airborne = state.localPosition.y > 0.05f;
            constexpr double dashTapWindow = 0.25;
            auto tryQueueDash = [&](double& lastAt, int8_t mx, int8_t mz) {
                if (airborne && (inputNow - lastAt) <= dashTapWindow) {
                    state.dashQueued = true;
                    state.dashMoveX = mx;
                    state.dashMoveZ = mz;
                }
                lastAt = inputNow;
            };

            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                state.fireQueued = true;
            }
            if (IsKeyPressed(KEY_SPACE)) {
                state.jumpQueued = true;
            }
            if (IsKeyPressed(KEY_A)) {
                tryQueueDash(state.lastAPressAt, -1, 0);
            }
            if (IsKeyPressed(KEY_D)) {
                tryQueueDash(state.lastDPressAt, 1, 0);
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
            if (IsKeyPressed(KEY_ESCAPE)) {
                state.screenMode = ScreenMode::MainMenu;
                state.mainMenuOpenedAt = GetTime();
                EnableCursor();
                continue;
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
            updateSpeedCounter(state, dt);
            updateDamagePopups(state, dt);
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
