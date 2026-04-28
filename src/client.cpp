#include "net.hpp"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/material.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int WindowWidth = 1280;
constexpr int WindowHeight = 720;
#if defined(NDEBUG)
constexpr bool ShowBottomDebugOverlay = false;
#else
constexpr bool ShowBottomDebugOverlay = true;
#endif
constexpr float DefaultMouseSensitivity = 0.0005f;
constexpr float StepIntervalWalk = 0.37f;
constexpr float StepIntervalCrouch = 0.52f;
constexpr double FireIntervalSeconds = 0.24;
constexpr double EmptyIntervalSeconds = 0.2;
constexpr double ReloadFrameDuration = 0.13;
constexpr double KnifeAttackFrameDuration = 0.09;
constexpr double KnifeInspectFrameDuration = 0.13;
constexpr double KnifeEquipSlideDuration = 0.22;
constexpr double LightningGunStartupFrameDuration = 0.05;
constexpr double LightningGunWinddownFrameDuration = 0.05;
constexpr float LightningLoopFadeOutSeconds = 0.14f;
constexpr float RecoilKickAmount = 18.0f;
constexpr float WorldUnitsPerTextureTile = 10.0f;
constexpr int MaxShells = 2;
constexpr int MaxReserveAmmo = 32;
constexpr int AmmoPickupAmount = 6;
constexpr float SfxVolumeMaster = 1.0f;
constexpr float SfxVolumeShotgunFire = 0.2f;
constexpr float SfxVolumeShotgunEmpty = 0.2f;
constexpr float SfxVolumeShotgunReload = 0.2f;
constexpr float SfxVolumeAmmoPickup = 0.7f;
constexpr float SfxVolumeLightningStart = 0.2f;
constexpr float SfxVolumeLightningLoop = 0.2f;
constexpr float SfxVolumeKnifeEquip = 0.2f;
constexpr float SfxVolumeHitBase = 0.3f;
constexpr float SfxVolumeFootsteps = 0.70f;
constexpr float SfxVolumeAnnouncer = 1.00f;

enum class WeaponSlot : uint8_t {
    Shotgun = 1,
    LightningGun = 2,
    Knife = 3,
};

enum class WeaponAnimMode {
    Idle,
    ShotgunEquip,
    ShotgunFire,
    ShotgunReload,
    LightningGunEquip,
    LightningGunStartup,
    LightningGunFiring,
    LightningGunWinddown,
    KnifeAttack,
    KnifeInspect,
    KnifeEquip
};

enum class ScreenMode {
    MainMenu,
    Settings,
    InGame
};

enum class DisplayMode {
    Windowed = 0,
    Borderless = 1,
    Fullscreen = 2
};

struct RemotePlayer {
    std::string name = "Player";
    arena::Vec3 position{};
    arena::Vec3 velocity{};
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool crouched = false;
    uint8_t teamId = 0;
    uint8_t health = 100;
    bool dead = false;
    uint8_t weaponSlot = 1;
    bool firing = false;
    bool lgBeamActive = false;
    arena::Vec3 lgBeamEnd{};
    uint16_t hitConfirmCount = 0;
    uint8_t lastDamageDealt = 0;
    uint32_t lastHitTargetId = 0;
    uint16_t pingMs = 0;
    uint16_t kills = 0;
    uint16_t deaths = 0;
    uint32_t damageDealt = 0;
};

enum class EnemyAnimClip {
    Idle = 0,
    WalkForward,
    RunForward,
    RunBackward,
    StrafeLeft,
    StrafeRight,
    Jump,
    HitReact,
    Count
};

struct EnemyAnimState {
    arena::Vec3 lastPosition{};
    bool hasLastPosition = false;
    float estimatedSpeed = 0.0f;
    float smoothedSpeed = 0.0f;
    arena::Vec3 smoothedVelocity{};
    uint8_t lastHealth = 100;
    bool hasLastHealth = false;
    float hitReactTimeRemaining = 0.0f;
    bool hitReactRestartRequested = false;
    bool wasAirborne = false;
    EnemyAnimClip activeClip = EnemyAnimClip::Idle;
    EnemyAnimClip pendingClip = EnemyAnimClip::Idle;
    float pendingClipTime = 0.0f;
    float frame = 0.0f;
    float animUpdateAccumulator = 0.0f;
    int lastAppliedAnimFrame = -1;
};

struct EnemyClipAsset {
    Model model{};
    bool modelLoaded = false;
    std::string sourcePath{};
    float modelScale = 1.0f;
    float modelMinY = 0.0f;
    float modelCenterX = 0.0f;
    float modelCenterZ = 0.0f;
    ModelAnimation* anims = nullptr;
    int animCount = 0;
    bool animValidForModel = false;
    float animFps = 30.0f;
    std::vector<Texture2D> ownedTextures;
    std::vector<unsigned char> alphaMeshes;
};

struct FbxModelLoadResult {
    Model model{};
    ModelAnimation* anims = nullptr;
    int animCount = 0;
    float animFps = 30.0f;
    std::vector<Texture2D> ownedTextures;
    std::vector<unsigned char> alphaMeshes;
};

void resetEnemyTune(class ClientState& state);
void sanitizeEnemyTune(class ClientState& state);

void forceVisibleMaterial(Model& model) {
    for (int i = 0; i < model.materialCount; ++i) {
        // Keep imported albedo textures; only ensure visible tint defaults.
        if (model.materials[i].maps[MATERIAL_MAP_ALBEDO].texture.id == 0) {
            model.materials[i].maps[MATERIAL_MAP_ALBEDO].color = Color{255, 120, 120, 255};
        } else {
            model.materials[i].maps[MATERIAL_MAP_ALBEDO].color = WHITE;
        }
        model.materials[i].maps[MATERIAL_MAP_EMISSION].color = Color{0, 0, 0, 255};
    }
}

void unloadEnemyClipAsset(EnemyClipAsset& clip) {
    if (clip.anims != nullptr) {
        UnloadModelAnimations(clip.anims, clip.animCount);
    }
    if (clip.modelLoaded) {
        UnloadModel(clip.model);
    }
    for (Texture2D& texture : clip.ownedTextures) {
        if (texture.id != 0) {
            UnloadTexture(texture);
        }
    }
    clip = EnemyClipAsset{};
}

void drawModelEuler(Model& model, Vector3 position, Vector3 rotationDeg, Vector3 scale, Color tint,
    const std::vector<unsigned char>* alphaMeshes = nullptr) {
    rlPushMatrix();
    rlTranslatef(position.x, position.y, position.z);
    rlRotatef(rotationDeg.y, 0.0f, 1.0f, 0.0f);
    rlRotatef(rotationDeg.x, 1.0f, 0.0f, 0.0f);
    rlRotatef(rotationDeg.z, 0.0f, 0.0f, 1.0f);
    rlScalef(scale.x, scale.y, scale.z);
    // Use DrawModel on identity-local origin so raylib handles full model/material/shader path.
    rlDisableBackfaceCulling();
    if (alphaMeshes == nullptr || alphaMeshes->empty()) {
        DrawModel(model, Vector3{0.0f, 0.0f, 0.0f}, 1.0f, tint);
    } else {
        auto meshUsesAlphaPass = [&](int meshIndex) {
            return meshIndex >= 0 && meshIndex < static_cast<int>(alphaMeshes->size()) &&
                (*alphaMeshes)[static_cast<size_t>(meshIndex)] != 0;
        };
        auto drawMeshIndex = [&](int meshIndex) {
            const int rawMaterialIndex = (model.meshMaterial != nullptr)
                ? model.meshMaterial[meshIndex]
                : 0;
            const int materialIndex = std::clamp(rawMaterialIndex, 0, std::max(0, model.materialCount - 1));
            Material& material = model.materials[materialIndex];
            Color color = material.maps[MATERIAL_MAP_ALBEDO].color;
            Color colorTint = WHITE;
            colorTint.r = static_cast<unsigned char>((static_cast<int>(color.r) * static_cast<int>(tint.r)) / 255);
            colorTint.g = static_cast<unsigned char>((static_cast<int>(color.g) * static_cast<int>(tint.g)) / 255);
            colorTint.b = static_cast<unsigned char>((static_cast<int>(color.b) * static_cast<int>(tint.b)) / 255);
            colorTint.a = static_cast<unsigned char>((static_cast<int>(color.a) * static_cast<int>(tint.a)) / 255);
            material.maps[MATERIAL_MAP_ALBEDO].color = colorTint;
            DrawMesh(model.meshes[meshIndex], material, model.transform);
            material.maps[MATERIAL_MAP_ALBEDO].color = color;
        };

        bool hasAlphaMeshes = false;
        for (int i = 0; i < model.meshCount; ++i) {
            if (meshUsesAlphaPass(i)) {
                hasAlphaMeshes = true;
                continue;
            }
            drawMeshIndex(i);
        }
        if (hasAlphaMeshes) {
            rlDrawRenderBatchActive();
            rlSetBlendMode(RL_BLEND_ALPHA);
            rlDisableDepthTest();
            rlDisableDepthMask();
            for (int i = 0; i < model.meshCount; ++i) {
                if (meshUsesAlphaPass(i)) {
                    drawMeshIndex(i);
                }
            }
            rlDrawRenderBatchActive();
            rlEnableDepthMask();
            rlEnableDepthTest();
        }
    }
    rlEnableBackfaceCulling();
    rlPopMatrix();
}

BoundingBox getModelBounds(const Model& model) {
    if (model.meshCount <= 0 || model.meshes == nullptr) {
        return BoundingBox{Vector3{0.0f, 0.0f, 0.0f}, Vector3{0.0f, 0.0f, 0.0f}};
    }
    BoundingBox merged = GetMeshBoundingBox(model.meshes[0]);
    for (int i = 1; i < model.meshCount; ++i) {
        const BoundingBox b = GetMeshBoundingBox(model.meshes[i]);
        merged.min.x = std::min(merged.min.x, b.min.x);
        merged.min.y = std::min(merged.min.y, b.min.y);
        merged.min.z = std::min(merged.min.z, b.min.z);
        merged.max.x = std::max(merged.max.x, b.max.x);
        merged.max.y = std::max(merged.max.y, b.max.y);
        merged.max.z = std::max(merged.max.z, b.max.z);
    }
    return merged;
}

float computeEnemyModelScale(const BoundingBox& bounds) {
    const float sx = std::max(0.000001f, bounds.max.x - bounds.min.x);
    const float sy = std::max(0.000001f, bounds.max.y - bounds.min.y);
    const float sz = std::max(0.000001f, bounds.max.z - bounds.min.z);
    const float dominant = std::max(sx, std::max(sy, sz));
    // Use dominant axis so prone/leaning source poses don't explode scale from tiny Y-height.
    return arena::PlayerHeight / dominant;
}

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

struct KillFeedEntry {
    std::string text{};
    float age = 0.0f;
    float lifetime = 2.0f;
};

struct ResolutionPreset {
    int width = 1280;
    int height = 720;
};

constexpr std::array<ResolutionPreset, 6> ResolutionPresets{{
    {1280, 720},
    {1366, 768},
    {1600, 900},
    {1920, 1080},
    {2560, 1440},
    {3840, 2160}
}};

struct ClientState {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in serverAddress{};
    bool connected = false;
    ScreenMode screenMode = ScreenMode::MainMenu;
    int menuIndex = 0;
    int settingsIndex = 0;
    int resolutionIndex = 0;
    DisplayMode displayMode = DisplayMode::Windowed;
    float mouseSensitivity = DefaultMouseSensitivity;
    bool hitSoundEnabled = true;
    float hitSoundVolume = SfxVolumeHitBase;
    double mainMenuOpenedAt = 0.0;
    uint32_t localPlayerId = 0;
    std::string localPlayerName = "Player";
    std::string desiredJoinName = "Player";
    bool joinNameActive = false;
    uint32_t inputSequence = 0;
    uint32_t latestServerTick = 0;
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
    Font uiFont{};
    bool uiFontLoaded = false;
    std::array<Texture2D, 8> shotgunFrames{};
    int shotgunFrameCount = 0;
    Texture2D lightningGunIdle{};
    std::array<Texture2D, 3> lightningGunStartupFrames{};
    int lightningGunStartupFrameCount = 0;
    Texture2D lightningGunFiring{};
    Texture2D knifeIdle{};
    std::array<Texture2D, 4> knifeAttackFrames{};
    int knifeAttackFrameCount = 0;
    std::array<Texture2D, 6> knifeInspectFrames{};
    int knifeInspectFrameCount = 0;

    Model floorModel{};
    Model ceilingModel{};
    Model wallModelX{};
    Model wallModelZ{};
    Model mapModel{};
    bool mapModelLoaded = false;
    float mapModelScale = 3.879997f;
    Vector3 mapModelOffset{0.0f, -4.0f, 0.0f};
    Vector3 mapModelRotationDeg{270.0f, 0.0f, 0.0f};
    std::array<EnemyClipAsset, static_cast<size_t>(EnemyAnimClip::Count)> enemyClipAssets{};
    bool enemyAnimSetReady = false;
    std::string enemyAnimStatus = "enemy anim: not initialized";
    std::string enemyModelDebug = "enemy model debug: n/a";
    float enemyTuneScale = 1.2f;
    float enemyTuneOffsetX = 0.0f;
    float enemyTuneOffsetY = -0.3f;
    float enemyTuneOffsetZ = 1.2f;
    float enemyTuneRotX = 270.0f;
    float enemyTuneRotY = 180.0f;
    float enemyTuneRotZ = 0.0f;

    Sound shotgunFire{};
    Sound shotgunEmpty{};
    Sound ammoPickup{};
    std::array<Sound, 4> shotgunReloadSounds{};
    int shotgunReloadSoundCount = 0;
    Sound lightningFireStart{};
    std::array<Sound, 4> lightningFireStartAliases{};
    int lightningFireStartAliasCount = 0;
    int lightningFireStartAliasNext = 0;
    Music lightningFireLoop{};
    bool lightningAudioLoaded = false;
    bool lightningLoopPlaying = false;
    bool lightningLoopFadingOut = false;
    float lightningLoopVolume = SfxVolumeMaster * SfxVolumeLightningLoop;
    Sound knifeEquip{};
    Sound hitSound{};
    bool hitSoundLoaded = false;
    std::array<Sound, 8> hitSoundAliases{};
    int hitSoundAliasCount = 0;
    int hitSoundAliasNext = 0;
    std::array<Sound, 4> footstepSounds{};
    int footstepSoundCount = 0;
    std::array<Sound, 16> announcerSounds{};
    std::array<uint8_t, 16> announcerSoundLoaded{};
    Sound announcerWeCaptured{};
    Sound announcerWeLost{};
    bool announcerWeCapturedLoaded = false;
    bool announcerWeLostLoaded = false;
    uint32_t lastAnnouncerSeqHeard = 0;
    std::string serverAnnouncementText{};
    double serverAnnouncementUntil = 0.0;

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
    uint16_t localKillCount = 0;
    bool localKillCountInitialized = false;
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
    uint8_t team1RoundPoints = 0;
    uint8_t team2RoundPoints = 0;
    uint8_t matchWinnerTeam = 0;
    uint16_t matchResetSecondsLeft = 0;
    float hillCaptureProgress = 0.0f;
    float smoothedPingMs = 0.0f;
    bool smoothedPingInitialized = false;
    std::vector<AmmoPack> ammoPacks;
    std::vector<DamagePopup> damagePopups;
    std::vector<KillFeedEntry> killFeed;
    std::map<uint32_t, uint8_t> enemyPrevFiring;
    std::map<uint32_t, double> enemyNextFireSoundAt;
    std::map<uint32_t, EnemyAnimState> enemyAnimStateById;
    float damageFlash = 0.0f;
    bool mapLoaded = false;
    uint16_t mapWidth = 0;
    uint16_t mapHeight = 0;
    float mapCellSize = 4.0f;
    float mapOriginX = 0.0f;
    float mapOriginZ = 0.0f;
    std::array<char, arena::MapMaxWidth * arena::MapMaxHeight> mapCells{};
};

Font* gUiFont = nullptr;

int UiMeasureText(const char* text, int fontSize) {
    if (gUiFont != nullptr && gUiFont->texture.id != 0) {
        const Vector2 size = MeasureTextEx(*gUiFont, text, static_cast<float>(fontSize), 1.0f);
        return static_cast<int>(std::round(size.x));
    }
    return ::MeasureText(text, fontSize);
}

void UiDrawText(const char* text, int posX, int posY, int fontSize, Color color) {
    if (gUiFont != nullptr && gUiFont->texture.id != 0) {
        DrawTextEx(*gUiFont, text, Vector2{static_cast<float>(posX), static_cast<float>(posY)}, static_cast<float>(fontSize), 1.0f, color);
        return;
    }
    ::DrawText(text, posX, posY, fontSize, color);
}

#define MeasureText UiMeasureText
#define DrawText UiDrawText

size_t announcerIndex(arena::AnnouncerEvent event) {
    return static_cast<size_t>(event);
}

void playAnnouncer(ClientState& state, arena::AnnouncerEvent event) {
    const size_t idx = announcerIndex(event);
    if (!state.audioReady || idx >= state.announcerSounds.size() || state.announcerSoundLoaded[idx] == 0) {
        return;
    }
    SetSoundVolume(state.announcerSounds[idx], SfxVolumeMaster * SfxVolumeAnnouncer);
    PlaySound(state.announcerSounds[idx]);
}

const char* announcerEventLabel(arena::AnnouncerEvent event) {
    switch (event) {
    case arena::AnnouncerEvent::DoubleKill: return "DOUBLE KILL";
    case arena::AnnouncerEvent::TripleKill: return "TRIPLE KILL";
    case arena::AnnouncerEvent::QuadKill: return "QUAD KILL";
    case arena::AnnouncerEvent::PentaKill: return "PENTA KILL";
    case arena::AnnouncerEvent::Godlike: return "GODLIKE";
    default: return "";
    }
}

const char* displayModeName(DisplayMode mode) {
    switch (mode) {
    case DisplayMode::Windowed: return "Windowed";
    case DisplayMode::Borderless: return "Borderless";
    case DisplayMode::Fullscreen: return "Fullscreen";
    default: return "Windowed";
    }
}

void applyDisplayMode(ClientState& state, DisplayMode mode) {
    const ResolutionPreset preset = ResolutionPresets[static_cast<size_t>(state.resolutionIndex)];
    const int monitor = GetCurrentMonitor();
    const int monitorWidth = GetMonitorWidth(monitor);
    const int monitorHeight = GetMonitorHeight(monitor);

    ClearWindowState(FLAG_FULLSCREEN_MODE);
    ClearWindowState(FLAG_BORDERLESS_WINDOWED_MODE);

    if (mode == DisplayMode::Fullscreen) {
        SetWindowSize(monitorWidth, monitorHeight);
        SetWindowPosition(0, 0);
        SetWindowState(FLAG_FULLSCREEN_MODE);
    } else if (mode == DisplayMode::Borderless) {
        SetWindowSize(monitorWidth, monitorHeight);
        SetWindowPosition(0, 0);
        SetWindowState(FLAG_BORDERLESS_WINDOWED_MODE);
    } else {
        SetWindowSize(preset.width, preset.height);
        SetWindowPosition(std::max(0, (monitorWidth - preset.width) / 2), std::max(0, (monitorHeight - preset.height) / 2));
    }

    state.displayMode = mode;
}

void resetEnemyTune(ClientState& state) {
    state.enemyTuneScale = 1.2f;
    state.enemyTuneOffsetX = 0.0f;
    state.enemyTuneOffsetY = -0.3f;
    state.enemyTuneOffsetZ = 1.2f;
    state.enemyTuneRotX = 270.0f;
    state.enemyTuneRotY = 180.0f;
    state.enemyTuneRotZ = 0.0f;
}

void sanitizeEnemyTune(ClientState& state) {
    // Temporary hard lock while tracking an out-of-band overwrite of tune fields.
    // Keeps FBX verification stable and prevents disappearing enemy models.
    resetEnemyTune(state);
}

arena::Vec3 lerpVec3(arena::Vec3 a, arena::Vec3 b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t
    };
}

Sound& nextAliasOrBase(Sound& base, std::array<Sound, 8>& aliases, int aliasCount, int& nextIndex) {
    if (aliasCount <= 0) {
        return base;
    }
    Sound& chosen = aliases[nextIndex];
    nextIndex = (nextIndex + 1) % aliasCount;
    return chosen;
}

Sound& nextAliasOrBase(Sound& base, std::array<Sound, 4>& aliases, int aliasCount, int& nextIndex) {
    if (aliasCount <= 0) {
        return base;
    }
    Sound& chosen = aliases[nextIndex];
    nextIndex = (nextIndex + 1) % aliasCount;
    return chosen;
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

std::string trimForJoinName(const std::string& raw) {
    size_t start = 0;
    while (start < raw.size() && std::isspace(static_cast<unsigned char>(raw[start])) != 0) {
        start++;
    }
    size_t end = raw.size();
    while (end > start && std::isspace(static_cast<unsigned char>(raw[end - 1])) != 0) {
        end--;
    }
    std::string out = raw.substr(start, end - start);
    if (out.empty()) {
        out = "Player";
    }
    if (out.size() >= static_cast<size_t>(arena::MaxPlayerNameChars)) {
        out.resize(arena::MaxPlayerNameChars - 1);
    }
    return out;
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

float firstAnimFpsOrDefault(const ModelAnimation* anim, int count) {
    if (anim == nullptr || count <= 0 || anim[0].frameCount <= 1) {
        return 30.0f;
    }
    return 30.0f;
}

std::string resolveAssetPathIfExists(const std::string& relativePath) {
    static const std::array<const char*, 5> prefixes = {"", ".\\", "..\\", "..\\..\\", "..\\..\\..\\"};
    for (const char* prefix : prefixes) {
        const std::string candidate = std::string(prefix) + relativePath;
        if (FileExists(candidate.c_str())) {
            return candidate;
        }
    }
    return {};
}

std::string fileBaseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos) ? path : path.substr(slash + 1);
}

std::string fileDirectory(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return (slash == std::string::npos) ? std::string{} : path.substr(0, slash);
}

std::string fileStem(const std::string& path) {
    std::string base = fileBaseName(path);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) {
        base.resize(dot);
    }
    return base;
}

std::string joinAssetPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char back = a.back();
    if (back == '\\' || back == '/') return a + b;
    return a + "\\" + b;
}

std::string toLowerAscii(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

bool containsNoCase(const std::string& haystack, const char* needle) {
    return toLowerAscii(haystack).find(needle) != std::string::npos;
}

void copyFixedName(char* dst, size_t dstSize, const std::string& src) {
    if (dst == nullptr || dstSize == 0) {
        return;
    }
    std::memset(dst, 0, dstSize);
    std::strncpy(dst, src.empty() ? "fbx" : src.c_str(), dstSize - 1);
}

Vector3 aiToVector3(const aiVector3D& v) {
    return Vector3{v.x, v.y, v.z};
}

Quaternion aiToQuaternion(const aiQuaternion& q) {
    return Quaternion{q.x, q.y, q.z, q.w};
}

Transform identityTransform() {
    return Transform{
        Vector3{0.0f, 0.0f, 0.0f},
        Quaternion{0.0f, 0.0f, 0.0f, 1.0f},
        Vector3{1.0f, 1.0f, 1.0f}
    };
}

Transform aiMatrixToTransform(const aiMatrix4x4& matrix) {
    aiVector3D scaling{};
    aiQuaternion rotation{};
    aiVector3D translation{};
    matrix.Decompose(scaling, rotation, translation);
    Transform transform{};
    transform.translation = aiToVector3(translation);
    transform.rotation = QuaternionNormalize(aiToQuaternion(rotation));
    transform.scale = aiToVector3(scaling);
    return transform;
}

Transform composeTransform(const Transform& parent, const Transform& local) {
    Transform out{};
    out.rotation = QuaternionMultiply(parent.rotation, local.rotation);
    out.translation = Vector3RotateByQuaternion(local.translation, parent.rotation);
    out.translation = Vector3Add(out.translation, parent.translation);
    out.scale = Vector3Multiply(parent.scale, local.scale);
    return out;
}

aiVector3D sampleVectorKeys(const aiVectorKey* keys, unsigned int count, double time, const aiVector3D& fallback) {
    if (keys == nullptr || count == 0) {
        return fallback;
    }
    if (count == 1 || time <= keys[0].mTime) {
        return keys[0].mValue;
    }
    for (unsigned int i = 0; i + 1 < count; ++i) {
        if (time <= keys[i + 1].mTime) {
            const double span = keys[i + 1].mTime - keys[i].mTime;
            const float t = (span > 0.000001) ? static_cast<float>((time - keys[i].mTime) / span) : 0.0f;
            const aiVector3D& a = keys[i].mValue;
            const aiVector3D& b = keys[i + 1].mValue;
            return aiVector3D{
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t
            };
        }
    }
    return keys[count - 1].mValue;
}

aiQuaternion sampleQuatKeys(const aiQuatKey* keys, unsigned int count, double time, const aiQuaternion& fallback) {
    if (keys == nullptr || count == 0) {
        return fallback;
    }
    if (count == 1 || time <= keys[0].mTime) {
        return keys[0].mValue;
    }
    for (unsigned int i = 0; i + 1 < count; ++i) {
        if (time <= keys[i + 1].mTime) {
            const double span = keys[i + 1].mTime - keys[i].mTime;
            const float t = (span > 0.000001) ? static_cast<float>((time - keys[i].mTime) / span) : 0.0f;
            const Quaternion a = QuaternionNormalize(aiToQuaternion(keys[i].mValue));
            const Quaternion b = QuaternionNormalize(aiToQuaternion(keys[i + 1].mValue));
            const Quaternion q = QuaternionNormalize(QuaternionSlerp(a, b, t));
            aiQuaternion out{};
            out.x = q.x;
            out.y = q.y;
            out.z = q.z;
            out.w = q.w;
            return out;
        }
    }
    return keys[count - 1].mValue;
}

struct VertexInfluence {
    int boneIndex = 0;
    float weight = 0.0f;
};

void addVertexInfluence(std::array<VertexInfluence, 4>& influences, int boneIndex, float weight) {
    if (weight <= 0.0f || boneIndex < 0) {
        return;
    }
    for (VertexInfluence& influence : influences) {
        if (influence.weight > 0.0f && influence.boneIndex == boneIndex) {
            influence.weight += weight;
            return;
        }
    }
    int slot = -1;
    float smallest = std::numeric_limits<float>::max();
    for (int i = 0; i < 4; ++i) {
        if (influences[i].weight <= 0.0f) {
            slot = i;
            break;
        }
        if (influences[i].weight < smallest) {
            smallest = influences[i].weight;
            slot = i;
        }
    }
    if (slot >= 0 && weight > influences[slot].weight) {
        influences[slot] = VertexInfluence{boneIndex, weight};
    }
}

void normalizeVertexInfluences(std::array<VertexInfluence, 4>& influences, int fallbackBone) {
    float total = 0.0f;
    for (const VertexInfluence& influence : influences) {
        total += influence.weight;
    }
    if (total <= 0.000001f) {
        influences = {};
        influences[0] = VertexInfluence{std::max(0, fallbackBone), 1.0f};
        return;
    }
    for (VertexInfluence& influence : influences) {
        influence.weight /= total;
    }
}

FbxModelLoadResult loadModelAssimpFbx(const std::string& path) {
    FbxModelLoadResult result{};
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);
    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_MATERIALS, true);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_TEXTURES, true);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_ANIMATIONS, true);
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_READ_WEIGHTS, true);

    const aiScene* scene = importer.ReadFile(
        path.c_str(),
        aiProcess_Triangulate |
            aiProcess_JoinIdenticalVertices |
            aiProcess_GenSmoothNormals |
            aiProcess_CalcTangentSpace |
            aiProcess_ImproveCacheLocality |
            aiProcess_LimitBoneWeights |
            aiProcess_SortByPType |
            aiProcess_GlobalScale
    );

    if (scene == nullptr || scene->mRootNode == nullptr || scene->mNumMeshes == 0 || scene->mMeshes == nullptr) {
        throw std::runtime_error("Assimp failed to load FBX scene: " + path);
    }

    const std::string modelDir = fileDirectory(path);
    const std::string modelStem = fileStem(path);
    auto getExtWithDot = [](const std::string& p) {
        const size_t dot = p.find_last_of('.');
        return (dot == std::string::npos) ? std::string{} : p.substr(dot);
    };

    auto loadEmbeddedTexture = [&](const aiTexture* embedded, const std::string& requestedPath) -> Texture2D {
        if (embedded == nullptr) {
            return Texture2D{};
        }
        if (embedded->mHeight == 0) {
            std::string ext = getExtWithDot(requestedPath);
            if (ext.empty()) {
                const std::string hint = embedded->achFormatHint;
                ext = hint.empty() ? ".png" : "." + hint;
            }
            Image image = LoadImageFromMemory(
                ext.c_str(),
                reinterpret_cast<const unsigned char*>(embedded->pcData),
                static_cast<int>(embedded->mWidth));
            if (image.data == nullptr) {
                return Texture2D{};
            }
            Texture2D tex = LoadTextureFromImage(image);
            UnloadImage(image);
            if (tex.id != 0) {
                SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
                SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
            }
            return tex;
        }

        const int width = static_cast<int>(embedded->mWidth);
        const int height = static_cast<int>(embedded->mHeight);
        if (width <= 0 || height <= 0) {
            return Texture2D{};
        }
        const size_t byteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        void* pixels = MemAlloc(byteCount);
        if (pixels == nullptr) {
            return Texture2D{};
        }
        std::memcpy(pixels, embedded->pcData, byteCount);
        Image image{};
        image.data = pixels;
        image.width = width;
        image.height = height;
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D tex = LoadTextureFromImage(image);
        UnloadImage(image);
        if (tex.id != 0) {
            SetTextureFilter(tex, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(tex, TEXTURE_WRAP_CLAMP);
        }
        return tex;
    };

    auto findEmbeddedTexture = [&](const std::string& texPath) -> const aiTexture* {
        if (scene->mTextures == nullptr || scene->mNumTextures == 0) {
            return nullptr;
        }
        if (!texPath.empty() && texPath[0] == '*') {
            const int embeddedIndex = std::atoi(texPath.c_str() + 1);
            if (embeddedIndex >= 0 && static_cast<unsigned int>(embeddedIndex) < scene->mNumTextures) {
                return scene->mTextures[embeddedIndex];
            }
        }
        const std::string wantedBase = fileBaseName(texPath);
        for (unsigned int i = 0; i < scene->mNumTextures; ++i) {
            const aiTexture* embedded = scene->mTextures[i];
            if (embedded == nullptr) {
                continue;
            }
            const std::string embeddedName = embedded->mFilename.C_Str();
            if (!embeddedName.empty() && (embeddedName == texPath || fileBaseName(embeddedName) == wantedBase)) {
                return embedded;
            }
        }
        return nullptr;
    };

    result.model.materialCount = std::max(1, static_cast<int>(scene->mNumMaterials));
    result.model.materials = static_cast<Material*>(MemAlloc(static_cast<size_t>(result.model.materialCount) * sizeof(Material)));
    if (result.model.materials == nullptr) {
        throw std::runtime_error("Out of memory while allocating FBX materials: " + path);
    }
    for (int i = 0; i < result.model.materialCount; ++i) {
        result.model.materials[i] = LoadMaterialDefault();
        result.model.materials[i].maps[MATERIAL_MAP_ALBEDO].color = Color{255, 120, 120, 255};
    }

    auto assignTextureByPath = [&](const std::string& texPath, Material& outMaterial) {
        if (texPath.empty()) {
            return false;
        }
        if (const aiTexture* embedded = findEmbeddedTexture(texPath)) {
            Texture2D texture = loadEmbeddedTexture(embedded, texPath);
            if (texture.id != 0) {
                result.ownedTextures.push_back(texture);
                outMaterial.maps[MATERIAL_MAP_ALBEDO].texture = texture;
                outMaterial.maps[MATERIAL_MAP_ALBEDO].color = WHITE;
                return true;
            }
        }

        const std::string base = fileBaseName(texPath);
        const std::string fbmDir = joinAssetPath(modelDir, modelStem + ".fbm");
        const std::array<std::string, 5> candidates = {
            joinAssetPath(modelDir, texPath),
            joinAssetPath(modelDir, base),
            joinAssetPath(fbmDir, base),
            joinAssetPath("assets\\Walk Forward.fbm", base),
            joinAssetPath("assets", base)
        };
        for (const std::string& candidate : candidates) {
            if (candidate.empty() || !FileExists(candidate.c_str())) {
                continue;
            }
            Texture2D texture = LoadTexture(candidate.c_str());
            if (texture.id == 0) {
                continue;
            }
            SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
            SetTextureWrap(texture, TEXTURE_WRAP_CLAMP);
            result.ownedTextures.push_back(texture);
            outMaterial.maps[MATERIAL_MAP_ALBEDO].texture = texture;
            outMaterial.maps[MATERIAL_MAP_ALBEDO].color = WHITE;
            return true;
        }
        return false;
    };

    auto fallbackTextureNameFor = [](const std::string& rawName) -> const char* {
        if (containsNoCase(rawName, "hair")) return "hair_DM.png";
        if (containsNoCase(rawName, "eye")) return "AliceW_Eye_DM.png";
        if (containsNoCase(rawName, "skin") || containsNoCase(rawName, "body") ||
            containsNoCase(rawName, "head") || containsNoCase(rawName, "arm") ||
            containsNoCase(rawName, "hand")) {
            return "AliceW_Skin_DM.png";
        }
        if (containsNoCase(rawName, "skirt") || containsNoCase(rawName, "apron")) return "AliceW_Skirt_DM.png";
        if (containsNoCase(rawName, "cloth") || containsNoCase(rawName, "sleeve") ||
            containsNoCase(rawName, "ribbon") || containsNoCase(rawName, "bow")) {
            return "AliceW_Cloth_DM.png";
        }
        return nullptr;
    };

    auto assignFallbackTextureByName = [&](const std::string& rawName, Material& outMaterial) {
        if (outMaterial.maps[MATERIAL_MAP_ALBEDO].texture.id != 0) {
            return true;
        }
        const char* fallbackTexture = fallbackTextureNameFor(rawName);
        return fallbackTexture != nullptr && assignTextureByPath(fallbackTexture, outMaterial);
    };

    auto loadTextureFromMaterial = [&](const aiMaterial* material, aiTextureType type, Material& outMaterial) {
        if (material == nullptr || material->GetTextureCount(type) == 0) {
            return false;
        }
        for (unsigned int i = 0; i < material->GetTextureCount(type); ++i) {
            aiString texRef{};
            if (material->GetTexture(type, i, &texRef) != AI_SUCCESS) {
                continue;
            }
            if (assignTextureByPath(texRef.C_Str(), outMaterial)) {
                return true;
            }
        }
        return false;
    };

    for (unsigned int i = 0; i < scene->mNumMaterials; ++i) {
        Material& outMaterial = result.model.materials[i];
        const aiMaterial* material = scene->mMaterials[i];
        aiString materialName{};
        if (material != nullptr) {
            material->Get(AI_MATKEY_NAME, materialName);
        }
        aiColor4D diffuse{};
        if (material != nullptr && aiGetMaterialColor(material, AI_MATKEY_COLOR_DIFFUSE, &diffuse) == AI_SUCCESS) {
            outMaterial.maps[MATERIAL_MAP_ALBEDO].color = Color{
                static_cast<unsigned char>(std::clamp(diffuse.r, 0.0f, 1.0f) * 255.0f),
                static_cast<unsigned char>(std::clamp(diffuse.g, 0.0f, 1.0f) * 255.0f),
                static_cast<unsigned char>(std::clamp(diffuse.b, 0.0f, 1.0f) * 255.0f),
                static_cast<unsigned char>(std::clamp(diffuse.a, 0.0f, 1.0f) * 255.0f)
            };
        }
        const std::array<aiTextureType, 5> textureTypes = {
            aiTextureType_BASE_COLOR,
            aiTextureType_DIFFUSE,
            aiTextureType_UNKNOWN,
            aiTextureType_AMBIENT,
            aiTextureType_EMISSIVE
        };
        bool loadedTexture = false;
        for (aiTextureType type : textureTypes) {
            if (loadTextureFromMaterial(material, type, outMaterial)) {
                loadedTexture = true;
                break;
            }
        }
        if (!loadedTexture) {
            assignFallbackTextureByName(materialName.C_Str(), outMaterial);
        }
    }

    std::vector<aiMatrix4x4> meshTransforms(scene->mNumMeshes);
    std::vector<bool> meshTransformSet(scene->mNumMeshes, false);
    std::map<std::string, const aiNode*> nodeByName;
    aiMatrix4x4 identity;
    auto walkNode = [&](const aiNode* root, auto&& self, const aiMatrix4x4& parent) -> void {
        if (root == nullptr) {
            return;
        }
        const aiMatrix4x4 world = parent * root->mTransformation;
        nodeByName[root->mName.C_Str()] = root;
        for (unsigned int i = 0; i < root->mNumMeshes; ++i) {
            const unsigned int meshIndex = root->mMeshes[i];
            if (meshIndex < meshTransforms.size()) {
                meshTransforms[meshIndex] = world;
                meshTransformSet[meshIndex] = true;
            }
        }
        for (unsigned int i = 0; i < root->mNumChildren; ++i) {
            self(root->mChildren[i], self, world);
        }
    };
    walkNode(scene->mRootNode, walkNode, identity);

    std::map<std::string, bool> weightedBoneNames;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (mesh == nullptr) {
            continue;
        }
        for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
            const aiBone* bone = mesh->mBones[b];
            if (bone != nullptr) {
                weightedBoneNames[bone->mName.C_Str()] = true;
            }
        }
    }

    std::map<std::string, bool> requiredNodeNames;
    for (const auto& [boneName, ignored] : weightedBoneNames) {
        (void)ignored;
        auto found = nodeByName.find(boneName);
        if (found == nodeByName.end()) {
            continue;
        }
        const aiNode* node = found->second;
        while (node != nullptr) {
            requiredNodeNames[node->mName.C_Str()] = true;
            node = node->mParent;
        }
    }

    std::vector<const aiNode*> boneNodes;
    std::map<std::string, int> boneIndexByName;
    auto collectBoneNodes = [&](const aiNode* root, auto&& self) -> void {
        if (root == nullptr) {
            return;
        }
        const std::string name = root->mName.C_Str();
        if (requiredNodeNames.find(name) != requiredNodeNames.end()) {
            const int index = static_cast<int>(boneNodes.size());
            boneNodes.push_back(root);
            boneIndexByName[name] = index;
        }
        for (unsigned int i = 0; i < root->mNumChildren; ++i) {
            self(root->mChildren[i], self);
        }
    };
    collectBoneNodes(scene->mRootNode, collectBoneNodes);

    const int realBoneCount = static_cast<int>(boneNodes.size());
    const int noBoneIndex = (realBoneCount > 0) ? realBoneCount : -1;
    if (realBoneCount + ((noBoneIndex >= 0) ? 1 : 0) > 255) {
        throw std::runtime_error("FBX skeleton has more than 255 bones, raylib skinning cannot address it: " + path);
    }
    if (realBoneCount > 0) {
        result.model.boneCount = realBoneCount + 1;
        result.model.bones = static_cast<BoneInfo*>(MemAlloc(static_cast<size_t>(result.model.boneCount) * sizeof(BoneInfo)));
        result.model.bindPose = static_cast<Transform*>(MemAlloc(static_cast<size_t>(result.model.boneCount) * sizeof(Transform)));
        if (result.model.bones == nullptr || result.model.bindPose == nullptr) {
            throw std::runtime_error("Out of memory while allocating FBX bones: " + path);
        }
        std::vector<Transform> localBind(result.model.boneCount, identityTransform());
        for (int i = 0; i < realBoneCount; ++i) {
            const aiNode* node = boneNodes[static_cast<size_t>(i)];
            copyFixedName(result.model.bones[i].name, sizeof(result.model.bones[i].name), node->mName.C_Str());
            int parentIndex = -1;
            const aiNode* parent = node->mParent;
            while (parent != nullptr) {
                auto parentFound = boneIndexByName.find(parent->mName.C_Str());
                if (parentFound != boneIndexByName.end()) {
                    parentIndex = parentFound->second;
                    break;
                }
                parent = parent->mParent;
            }
            result.model.bones[i].parent = parentIndex;
            localBind[i] = aiMatrixToTransform(node->mTransformation);
        }
        copyFixedName(result.model.bones[noBoneIndex].name, sizeof(result.model.bones[noBoneIndex].name), "NO_BONE");
        result.model.bones[noBoneIndex].parent = -1;
        localBind[noBoneIndex] = identityTransform();
        for (int i = 0; i < result.model.boneCount; ++i) {
            const int parent = result.model.bones[i].parent;
            result.model.bindPose[i] = (parent >= 0) ? composeTransform(result.model.bindPose[parent], localBind[i]) : localBind[i];
        }
    }

    struct SourceMesh {
        const aiMesh* mesh = nullptr;
        aiMatrix4x4 world{};
        size_t triVerts = 0;
    };

    std::vector<SourceMesh> sourceMeshes;
    for (unsigned int m = 0; m < scene->mNumMeshes; ++m) {
        const aiMesh* mesh = scene->mMeshes[m];
        if (mesh == nullptr) {
            continue;
        }
        size_t triVerts = 0;
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            if (mesh->mFaces[f].mNumIndices == 3) {
                triVerts += 3;
            }
        }
        if (triVerts > 0) {
            sourceMeshes.push_back(SourceMesh{mesh, meshTransformSet[m] ? meshTransforms[m] : identity, triVerts});
        }
    }
    if (sourceMeshes.empty()) {
        throw std::runtime_error("Assimp FBX has no triangulated faces: " + path);
    }

    result.model.meshCount = static_cast<int>(sourceMeshes.size());
    result.model.meshes = static_cast<Mesh*>(MemAlloc(static_cast<size_t>(result.model.meshCount) * sizeof(Mesh)));
    result.model.meshMaterial = static_cast<int*>(MemAlloc(static_cast<size_t>(result.model.meshCount) * sizeof(int)));
    result.alphaMeshes.assign(static_cast<size_t>(result.model.meshCount), 0);
    if (result.model.meshes == nullptr || result.model.meshMaterial == nullptr) {
        throw std::runtime_error("Out of memory while allocating FBX meshes: " + path);
    }
    std::memset(result.model.meshes, 0, static_cast<size_t>(result.model.meshCount) * sizeof(Mesh));
    std::memset(result.model.meshMaterial, 0, static_cast<size_t>(result.model.meshCount) * sizeof(int));

    for (int meshIndex = 0; meshIndex < result.model.meshCount; ++meshIndex) {
        const SourceMesh& source = sourceMeshes[static_cast<size_t>(meshIndex)];
        const aiMesh* mesh = source.mesh;
        Mesh& outMesh = result.model.meshes[meshIndex];
        outMesh.vertexCount = static_cast<int>(source.triVerts);
        outMesh.triangleCount = outMesh.vertexCount / 3;
        outMesh.vertices = static_cast<float*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 3 * sizeof(float)));
        outMesh.normals = static_cast<float*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 3 * sizeof(float)));
        outMesh.texcoords = static_cast<float*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 2 * sizeof(float)));
        if (outMesh.vertices == nullptr || outMesh.normals == nullptr || outMesh.texcoords == nullptr) {
            throw std::runtime_error("Out of memory while converting FBX mesh: " + path);
        }
        if (result.model.boneCount > 0) {
            outMesh.boneIds = static_cast<unsigned char*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 4 * sizeof(unsigned char)));
            outMesh.boneWeights = static_cast<float*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 4 * sizeof(float)));
            outMesh.animVertices = static_cast<float*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 3 * sizeof(float)));
            outMesh.animNormals = static_cast<float*>(MemAlloc(static_cast<size_t>(outMesh.vertexCount) * 3 * sizeof(float)));
            outMesh.boneMatrices = static_cast<Matrix*>(MemAlloc(static_cast<size_t>(result.model.boneCount) * sizeof(Matrix)));
            if (outMesh.boneIds == nullptr || outMesh.boneWeights == nullptr ||
                outMesh.animVertices == nullptr || outMesh.animNormals == nullptr || outMesh.boneMatrices == nullptr) {
                throw std::runtime_error("Out of memory while allocating FBX skin data: " + path);
            }
            std::memset(outMesh.boneIds, 0, static_cast<size_t>(outMesh.vertexCount) * 4 * sizeof(unsigned char));
            std::memset(outMesh.boneWeights, 0, static_cast<size_t>(outMesh.vertexCount) * 4 * sizeof(float));
            outMesh.boneCount = result.model.boneCount;
            for (int b = 0; b < result.model.boneCount; ++b) {
                outMesh.boneMatrices[b] = MatrixIdentity();
            }
        }

        result.model.meshMaterial[meshIndex] = (mesh->mMaterialIndex < static_cast<unsigned int>(result.model.materialCount))
            ? static_cast<int>(mesh->mMaterialIndex)
            : 0;
        Material& meshMaterial = result.model.materials[result.model.meshMaterial[meshIndex]];
        assignFallbackTextureByName(mesh->mName.C_Str(), meshMaterial);
        aiString meshMaterialName{};
        if (mesh->mMaterialIndex < scene->mNumMaterials && scene->mMaterials[mesh->mMaterialIndex] != nullptr) {
            scene->mMaterials[mesh->mMaterialIndex]->Get(AI_MATKEY_NAME, meshMaterialName);
        }
        result.alphaMeshes[static_cast<size_t>(meshIndex)] =
            (containsNoCase(mesh->mName.C_Str(), "hair") ||
             containsNoCase(meshMaterialName.C_Str(), "hair")) ? 1 : 0;

        std::vector<std::array<VertexInfluence, 4>> sourceInfluences(mesh->mNumVertices);
        if (result.model.boneCount > 0) {
            for (unsigned int b = 0; b < mesh->mNumBones; ++b) {
                const aiBone* bone = mesh->mBones[b];
                if (bone == nullptr) {
                    continue;
                }
                auto found = boneIndexByName.find(bone->mName.C_Str());
                const int boneIndex = (found != boneIndexByName.end()) ? found->second : noBoneIndex;
                for (unsigned int w = 0; w < bone->mNumWeights; ++w) {
                    const aiVertexWeight& weight = bone->mWeights[w];
                    if (weight.mVertexId < sourceInfluences.size()) {
                        addVertexInfluence(sourceInfluences[weight.mVertexId], boneIndex, weight.mWeight);
                    }
                }
            }
            for (std::array<VertexInfluence, 4>& influences : sourceInfluences) {
                normalizeVertexInfluences(influences, noBoneIndex);
            }
        }

        aiMatrix3x3 normalMatrix(source.world);
        normalMatrix.Inverse().Transpose();
        int outVertex = 0;
        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) {
                continue;
            }
            for (unsigned int i = 0; i < 3; ++i) {
                const unsigned int idx = face.mIndices[i];
                const aiVector3D wp = source.world * mesh->mVertices[idx];
                outMesh.vertices[outVertex * 3 + 0] = wp.x;
                outMesh.vertices[outVertex * 3 + 1] = wp.y;
                outMesh.vertices[outVertex * 3 + 2] = wp.z;

                aiVector3D wn{0.0f, 1.0f, 0.0f};
                if (mesh->HasNormals()) {
                    wn = normalMatrix * mesh->mNormals[idx];
                    if (wn.SquareLength() > 0.000001f) {
                        wn.Normalize();
                    }
                }
                outMesh.normals[outVertex * 3 + 0] = wn.x;
                outMesh.normals[outVertex * 3 + 1] = wn.y;
                outMesh.normals[outVertex * 3 + 2] = wn.z;

                if (mesh->HasTextureCoords(0)) {
                    const aiVector3D& uv = mesh->mTextureCoords[0][idx];
                    outMesh.texcoords[outVertex * 2 + 0] = uv.x;
                    outMesh.texcoords[outVertex * 2 + 1] = 1.0f - uv.y;
                } else {
                    outMesh.texcoords[outVertex * 2 + 0] = 0.0f;
                    outMesh.texcoords[outVertex * 2 + 1] = 0.0f;
                }

                if (result.model.boneCount > 0) {
                    const std::array<VertexInfluence, 4>& influences = sourceInfluences[idx];
                    for (int b = 0; b < 4; ++b) {
                        outMesh.boneIds[outVertex * 4 + b] = static_cast<unsigned char>(influences[b].boneIndex);
                        outMesh.boneWeights[outVertex * 4 + b] = influences[b].weight;
                    }
                }
                outVertex++;
            }
        }

        if (result.model.boneCount > 0) {
            std::memcpy(outMesh.animVertices, outMesh.vertices, static_cast<size_t>(outMesh.vertexCount) * 3 * sizeof(float));
            std::memcpy(outMesh.animNormals, outMesh.normals, static_cast<size_t>(outMesh.vertexCount) * 3 * sizeof(float));
        }
        UploadMesh(&outMesh, result.model.boneCount > 0);
    }

    result.model.transform = MatrixIdentity();

    if (result.model.boneCount > 0 && scene->mNumAnimations > 0) {
        result.animCount = static_cast<int>(scene->mNumAnimations);
        result.anims = static_cast<ModelAnimation*>(MemAlloc(static_cast<size_t>(result.animCount) * sizeof(ModelAnimation)));
        if (result.anims == nullptr) {
            throw std::runtime_error("Out of memory while allocating FBX animations: " + path);
        }
        std::memset(result.anims, 0, static_cast<size_t>(result.animCount) * sizeof(ModelAnimation));

        std::vector<Transform> localBind(result.model.boneCount, identityTransform());
        for (int i = 0; i < realBoneCount; ++i) {
            localBind[i] = aiMatrixToTransform(boneNodes[static_cast<size_t>(i)]->mTransformation);
        }

        for (int a = 0; a < result.animCount; ++a) {
            const aiAnimation* sourceAnim = scene->mAnimations[a];
            ModelAnimation& anim = result.anims[a];
            anim.boneCount = result.model.boneCount;
            anim.bones = static_cast<BoneInfo*>(MemAlloc(static_cast<size_t>(anim.boneCount) * sizeof(BoneInfo)));
            if (anim.bones == nullptr) {
                throw std::runtime_error("Out of memory while copying FBX animation bones: " + path);
            }
            std::memcpy(anim.bones, result.model.bones, static_cast<size_t>(anim.boneCount) * sizeof(BoneInfo));
            copyFixedName(anim.name, sizeof(anim.name), (sourceAnim != nullptr) ? sourceAnim->mName.C_Str() : modelStem);

            std::map<std::string, const aiNodeAnim*> channelsByName;
            double maxKeyTime = 0.0;
            if (sourceAnim != nullptr) {
                for (unsigned int c = 0; c < sourceAnim->mNumChannels; ++c) {
                    const aiNodeAnim* channel = sourceAnim->mChannels[c];
                    if (channel == nullptr) {
                        continue;
                    }
                    channelsByName[channel->mNodeName.C_Str()] = channel;
                    if (channel->mNumPositionKeys > 0) maxKeyTime = std::max(maxKeyTime, channel->mPositionKeys[channel->mNumPositionKeys - 1].mTime);
                    if (channel->mNumRotationKeys > 0) maxKeyTime = std::max(maxKeyTime, channel->mRotationKeys[channel->mNumRotationKeys - 1].mTime);
                    if (channel->mNumScalingKeys > 0) maxKeyTime = std::max(maxKeyTime, channel->mScalingKeys[channel->mNumScalingKeys - 1].mTime);
                }
            }

            const double ticksPerSecond = (sourceAnim != nullptr && sourceAnim->mTicksPerSecond > 0.0) ? sourceAnim->mTicksPerSecond : 30.0;
            const double durationTicks = std::max((sourceAnim != nullptr) ? sourceAnim->mDuration : 0.0, maxKeyTime);
            const double durationSeconds = (durationTicks > 0.0) ? (durationTicks / ticksPerSecond) : (1.0 / result.animFps);
            anim.frameCount = std::max(2, static_cast<int>(std::floor(durationSeconds * result.animFps + 0.5)) + 1);
            anim.framePoses = static_cast<Transform**>(MemAlloc(static_cast<size_t>(anim.frameCount) * sizeof(Transform*)));
            if (anim.framePoses == nullptr) {
                throw std::runtime_error("Out of memory while allocating FBX animation poses: " + path);
            }

            for (int frame = 0; frame < anim.frameCount; ++frame) {
                anim.framePoses[frame] = static_cast<Transform*>(MemAlloc(static_cast<size_t>(anim.boneCount) * sizeof(Transform)));
                if (anim.framePoses[frame] == nullptr) {
                    throw std::runtime_error("Out of memory while allocating FBX animation frame: " + path);
                }
                const double time = (durationTicks > 0.0)
                    ? std::min(durationTicks, (static_cast<double>(frame) / result.animFps) * ticksPerSecond)
                    : 0.0;
                std::vector<Transform> localPose = localBind;

                for (int b = 0; b < realBoneCount; ++b) {
                    const aiNode* node = boneNodes[static_cast<size_t>(b)];
                    auto channelFound = channelsByName.find(node->mName.C_Str());
                    if (channelFound == channelsByName.end()) {
                        continue;
                    }
                    const aiNodeAnim* channel = channelFound->second;
                    aiVector3D defaultScale{localBind[b].scale.x, localBind[b].scale.y, localBind[b].scale.z};
                    aiQuaternion defaultRot{};
                    defaultRot.x = localBind[b].rotation.x;
                    defaultRot.y = localBind[b].rotation.y;
                    defaultRot.z = localBind[b].rotation.z;
                    defaultRot.w = localBind[b].rotation.w;
                    aiVector3D defaultPos{localBind[b].translation.x, localBind[b].translation.y, localBind[b].translation.z};

                    const aiVector3D pos = sampleVectorKeys(channel->mPositionKeys, channel->mNumPositionKeys, time, defaultPos);
                    const aiQuaternion rot = sampleQuatKeys(channel->mRotationKeys, channel->mNumRotationKeys, time, defaultRot);
                    const aiVector3D scale = sampleVectorKeys(channel->mScalingKeys, channel->mNumScalingKeys, time, defaultScale);
                    localPose[b].translation = aiToVector3(pos);
                    localPose[b].rotation = QuaternionNormalize(aiToQuaternion(rot));
                    localPose[b].scale = aiToVector3(scale);
                }

                if (noBoneIndex >= 0) {
                    localPose[noBoneIndex] = identityTransform();
                }
                for (int b = 0; b < anim.boneCount; ++b) {
                    const int parent = anim.bones[b].parent;
                    anim.framePoses[frame][b] = (parent >= 0)
                        ? composeTransform(anim.framePoses[frame][parent], localPose[b])
                        : localPose[b];
                }
            }
        }
    }

    return result;
}

std::string resolvePreferredAnimAsset(const char* stemNoExt) {
    const std::array<const char*, 1> extensions = {".fbx"};
    for (const char* ext : extensions) {
        const std::string rel = std::string("assets\\") + stemNoExt + ext;
        const std::string found = resolveAssetPathIfExists(rel);
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

void initEnemyModelAssets(ClientState& state) {
    state.enemyAnimSetReady = false;
    state.enemyAnimStatus = "enemy anim: loading";
    for (EnemyClipAsset& clip : state.enemyClipAssets) {
        unloadEnemyClipAsset(clip);
    }

    try {
        int loadedCount = 0;
        auto tryLoad = [&](EnemyAnimClip clipType, const char* stemNoExt) {
            const std::string path = resolvePreferredAnimAsset(stemNoExt);
            if (path.empty()) {
                return;
            }
            EnemyClipAsset& clip = state.enemyClipAssets[static_cast<size_t>(clipType)];
            FbxModelLoadResult loaded = loadModelAssimpFbx(path);
            clip.model = loaded.model;
            clip.anims = loaded.anims;
            clip.animCount = loaded.animCount;
            clip.animFps = loaded.animFps;
            clip.ownedTextures = std::move(loaded.ownedTextures);
            clip.alphaMeshes = std::move(loaded.alphaMeshes);
            clip.modelLoaded = clip.model.meshCount > 0;
            if (!clip.modelLoaded) {
                return;
            }
            clip.sourcePath = path;
            forceVisibleMaterial(clip.model);
            BoundingBox bounds = getModelBounds(clip.model);
            clip.modelMinY = bounds.min.y;
            clip.modelCenterX = (bounds.min.x + bounds.max.x) * 0.5f;
            clip.modelCenterZ = (bounds.min.z + bounds.max.z) * 0.5f;
            clip.modelScale = computeEnemyModelScale(bounds);

            const bool hasAnims = clip.anims != nullptr && clip.animCount > 0;
            clip.animValidForModel = hasAnims && IsModelAnimationValid(clip.model, clip.anims[0]);
            if (clip.animValidForModel) {
                loadedCount++;
            }
        };
        auto tryLoadFirst = [&](EnemyAnimClip clipType, std::initializer_list<const char*> stems) {
            for (const char* stem : stems) {
                tryLoad(clipType, stem);
                EnemyClipAsset& clip = state.enemyClipAssets[static_cast<size_t>(clipType)];
                if (clip.modelLoaded && clip.animValidForModel) {
                    return;
                }
                if (clip.modelLoaded) {
                    unloadEnemyClipAsset(clip);
                }
            }
        };

        // FBX-only runtime assets in /assets, mapped directly to gameplay actions.
        tryLoadFirst(EnemyAnimClip::Idle, {"Rifle Idle"});
        tryLoadFirst(EnemyAnimClip::WalkForward, {"Walk Forward"});
        tryLoadFirst(EnemyAnimClip::RunForward, {"Run Forward"});
        tryLoadFirst(EnemyAnimClip::RunBackward, {"Run Backwards"});
        tryLoadFirst(EnemyAnimClip::StrafeLeft, {"Strafe Left"});
        tryLoadFirst(EnemyAnimClip::StrafeRight, {"Strafe Right"});
        tryLoadFirst(EnemyAnimClip::Jump, {"Rifle Jump"});
        tryLoadFirst(EnemyAnimClip::HitReact, {"Hit Reaction"});

        // Only idle is mandatory. Missing clips gracefully fall back at runtime.
        const EnemyClipAsset& idle = state.enemyClipAssets[static_cast<size_t>(EnemyAnimClip::Idle)];
        const bool hasIdle = idle.modelLoaded && idle.animValidForModel;
        state.enemyAnimSetReady = hasIdle;
        state.enemyAnimStatus = state.enemyAnimSetReady
            ? ("enemy anim: FBX model+clips ready (" + std::to_string(loadedCount) + "/8) idle=" + idle.sourcePath)
            : "enemy anim: FBX idle clip unavailable";
    } catch (...) {
        state.enemyAnimSetReady = false;
        state.enemyAnimStatus = "enemy anim: init threw exception";
    }
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

Music loadMusicAsset(const std::string& relativePath) {
    const std::string path = resolveAssetPath(relativePath);
    Music music = LoadMusicStream(path.c_str());
    if (music.ctxData == nullptr) {
        throw std::runtime_error("Failed to load music: " + path);
    }
    return music;
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
    try {
        const std::string fontPath = resolveAssetPath("assets\\fonts\\SilentHunterIII Font.ttf");
        state.uiFont = LoadFontEx(fontPath.c_str(), 96, nullptr, 0);
        state.uiFontLoaded = state.uiFont.texture.id != 0;
        if (state.uiFontLoaded) {
            SetTextureFilter(state.uiFont.texture, TEXTURE_FILTER_BILINEAR);
            gUiFont = &state.uiFont;
        }
    } catch (...) {
        state.uiFontLoaded = false;
        gUiFont = nullptr;
    }

    Mesh floorMesh = GenMeshPlane(arena::RoomHalfSize * 2.0f, arena::RoomHalfSize * 2.0f, 32, 32);
    Mesh ceilingMesh = GenMeshPlane(arena::RoomHalfSize * 2.0f, arena::RoomHalfSize * 2.0f, 32, 32);
    Mesh wallMeshX = GenMeshCube(0.2f, arena::RoomHeight, arena::RoomHalfSize * 2.0f);
    Mesh wallMeshZ = GenMeshCube(arena::RoomHalfSize * 2.0f, arena::RoomHeight, 0.2f);

    // Tile using world-size-based UV scales so 128x128-style texels stay square on all surfaces.
    const float roomSpan = arena::RoomHalfSize * 2.0f;
    const float floorTiles = roomSpan / WorldUnitsPerTextureTile;
    const float wallTilesU = roomSpan / WorldUnitsPerTextureTile;
    const float wallTilesV = arena::RoomHeight / WorldUnitsPerTextureTile;
    tileMeshUV(floorMesh, floorTiles, floorTiles);
    tileMeshUV(ceilingMesh, floorTiles, floorTiles);
    tileMeshUV(wallMeshX, wallTilesU, wallTilesV);
    tileMeshUV(wallMeshZ, wallTilesU, wallTilesV);

    state.floorModel = LoadModelFromMesh(floorMesh);
    state.ceilingModel = LoadModelFromMesh(ceilingMesh);
    state.wallModelX = LoadModelFromMesh(wallMeshX);
    state.wallModelZ = LoadModelFromMesh(wallMeshZ);

    state.floorModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.floorTexture;
    state.ceilingModel.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.wallTexture;
    state.wallModelX.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.wallTexture;
    state.wallModelZ.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = state.wallTexture;
    initEnemyModelAssets(state);

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
    state.lightningGunIdle = loadTextureAsset("assets\\weapons\\LG\\lgidle.png");
    state.lightningGunStartupFrames[0] = loadTextureAsset("assets\\weapons\\LG\\lgfire1.png");
    state.lightningGunStartupFrames[1] = loadTextureAsset("assets\\weapons\\LG\\lgfire2.png");
    state.lightningGunStartupFrames[2] = loadTextureAsset("assets\\weapons\\LG\\lgfire3.png");
    state.lightningGunStartupFrameCount = 3;
    state.lightningGunFiring = loadTextureAsset("assets\\weapons\\LG\\lgfiring.png");
    SetTextureWrap(state.lightningGunIdle, TEXTURE_WRAP_CLAMP);
    SetTextureWrap(state.lightningGunFiring, TEXTURE_WRAP_CLAMP);
    for (Texture2D& frame : state.lightningGunStartupFrames) {
        SetTextureWrap(frame, TEXTURE_WRAP_CLAMP);
    }
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
    SetSoundVolume(state.shotgunFire, SfxVolumeMaster * SfxVolumeShotgunFire);
    state.shotgunEmpty = loadSoundAsset("assets\\weapons\\doubleshotgun\\empty.wav");
    SetSoundVolume(state.shotgunEmpty, SfxVolumeMaster * SfxVolumeShotgunEmpty);
    state.ammoPickup = loadSoundAsset("assets\\sound\\ammo.wav");
    SetSoundVolume(state.ammoPickup, SfxVolumeMaster * SfxVolumeAmmoPickup);
    state.shotgunReloadSounds[0] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload1.wav");
    state.shotgunReloadSounds[1] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload2.wav");
    state.shotgunReloadSounds[2] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload3.wav");
    state.shotgunReloadSounds[3] = loadSoundAsset("assets\\weapons\\doubleshotgun\\reload4.wav");
    for (Sound& reloadSound : state.shotgunReloadSounds) {
        SetSoundVolume(reloadSound, SfxVolumeMaster * SfxVolumeShotgunReload);
    }
    state.shotgunReloadSoundCount = 4;
    try {
        state.lightningFireStart = loadSoundAsset("assets\\weapons\\LG\\fire.ogg");
        state.lightningFireLoop = loadMusicAsset("assets\\weapons\\LG\\loop.ogg");
        state.lightningFireLoop.looping = true;
        state.lightningFireStartAliasCount = static_cast<int>(state.lightningFireStartAliases.size());
        for (int i = 0; i < state.lightningFireStartAliasCount; ++i) {
            state.lightningFireStartAliases[i] = LoadSoundAlias(state.lightningFireStart);
            SetSoundVolume(state.lightningFireStartAliases[i], SfxVolumeMaster * SfxVolumeLightningStart);
        }
        SetSoundVolume(state.lightningFireStart, SfxVolumeMaster * SfxVolumeLightningStart);
        state.lightningLoopVolume = SfxVolumeMaster * SfxVolumeLightningLoop;
        SetMusicVolume(state.lightningFireLoop, state.lightningLoopVolume);
        state.lightningAudioLoaded = true;
    } catch (...) {
        state.lightningAudioLoaded = false;
    }
    state.knifeEquip = loadSoundAsset("assets\\weapons\\karambit\\equip.wav");
    SetSoundVolume(state.knifeEquip, SfxVolumeMaster * SfxVolumeKnifeEquip);
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
        SetSoundVolume(state.hitSound, SfxVolumeMaster * state.hitSoundVolume);
        state.hitSoundAliasCount = static_cast<int>(state.hitSoundAliases.size());
        for (int i = 0; i < state.hitSoundAliasCount; ++i) {
            state.hitSoundAliases[i] = LoadSoundAlias(state.hitSound);
            SetSoundVolume(state.hitSoundAliases[i], SfxVolumeMaster * state.hitSoundVolume);
        }
    }
    state.footstepSounds[0] = loadSoundAsset("assets\\sound\\boots1.wav");
    state.footstepSounds[1] = loadSoundAsset("assets\\sound\\boots2.wav");
    state.footstepSounds[2] = loadSoundAsset("assets\\sound\\boots3.wav");
    state.footstepSounds[3] = loadSoundAsset("assets\\sound\\boots4.wav");
    for (Sound& footstepSound : state.footstepSounds) {
        SetSoundVolume(footstepSound, SfxVolumeMaster * SfxVolumeFootsteps);
    }
    state.footstepSoundCount = 4;
    state.announcerSoundLoaded.fill(0);
    auto loadAnnouncer = [&](arena::AnnouncerEvent event, const char* path) {
        try {
            const size_t idx = announcerIndex(event);
            state.announcerSounds[idx] = loadSoundAsset(path);
            SetSoundVolume(state.announcerSounds[idx], SfxVolumeMaster * SfxVolumeAnnouncer);
            state.announcerSoundLoaded[idx] = (state.announcerSounds[idx].stream.buffer != nullptr) ? 1 : 0;
        } catch (...) {
            // Optional content: skip missing announcer clips.
        }
    };
    loadAnnouncer(arena::AnnouncerEvent::DoubleKill, "assets\\sound\\announcer\\[Half-Life VOX]Doubl...... kill_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::TripleKill, "assets\\sound\\announcer\\[Half-Life VOX]Tripl......kill!_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::QuadKill, "assets\\sound\\announcer\\[Half-Life VOX]Quadr......kill!_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::PentaKill, "assets\\sound\\announcer\\[Half-Life VOX]Penta......kill!_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::Godlike, "assets\\sound\\announcer\\[Half-Life VOX]Godlike_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::Overtime, "assets\\sound\\announcer\\[Half-Life VOX]Overt......me..._8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::Victory, "assets\\sound\\announcer\\[Half-Life VOX]Victory..._8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::Defeat, "assets\\sound\\announcer\\[Half-Life VOX]Defeat..._8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::CountOne, "assets\\sound\\announcer\\[Half-Life VOX]One_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::CountTwo, "assets\\sound\\announcer\\[Half-Life VOX]Two_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::CountThree, "assets\\sound\\announcer\\[Half-Life VOX]Three_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::CountFour, "assets\\sound\\announcer\\[Half-Life VOX]Four_8000hz.mp3");
    loadAnnouncer(arena::AnnouncerEvent::CountFive, "assets\\sound\\announcer\\[Half-Life VOX]Five_8000hz.mp3");
    try {
        state.announcerWeCaptured = loadSoundAsset("assets\\sound\\announcer\\[Half-Life VOX]We ha......oint._8000hz.mp3");
        SetSoundVolume(state.announcerWeCaptured, SfxVolumeMaster * SfxVolumeAnnouncer);
        state.announcerWeCapturedLoaded = state.announcerWeCaptured.stream.buffer != nullptr;
    } catch (...) {
        state.announcerWeCapturedLoaded = false;
    }
    try {
        state.announcerWeLost = loadSoundAsset("assets\\sound\\announcer\\[Half-Life VOX]We ha......oint.(1)_8000hz.mp3");
        SetSoundVolume(state.announcerWeLost, SfxVolumeMaster * SfxVolumeAnnouncer);
        state.announcerWeLostLoaded = state.announcerWeLost.stream.buffer != nullptr;
    } catch (...) {
        state.announcerWeLostLoaded = false;
    }

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
        if (state.lightningAudioLoaded) {
            StopMusicStream(state.lightningFireLoop);
            UnloadMusicStream(state.lightningFireLoop);
            for (int i = 0; i < state.lightningFireStartAliasCount; ++i) {
                UnloadSoundAlias(state.lightningFireStartAliases[i]);
            }
            state.lightningFireStartAliasCount = 0;
            UnloadSound(state.lightningFireStart);
            state.lightningAudioLoaded = false;
            state.lightningLoopPlaying = false;
            state.lightningLoopFadingOut = false;
            state.lightningLoopVolume = SfxVolumeMaster * SfxVolumeLightningLoop;
        }
        UnloadSound(state.knifeEquip);
        if (state.hitSoundLoaded) {
            for (int i = 0; i < state.hitSoundAliasCount; ++i) {
                UnloadSoundAlias(state.hitSoundAliases[i]);
            }
            state.hitSoundAliasCount = 0;
            UnloadSound(state.hitSound);
            state.hitSoundLoaded = false;
        }
        for (int i = 0; i < state.footstepSoundCount; ++i) {
            UnloadSound(state.footstepSounds[i]);
        }
        for (size_t i = 0; i < state.announcerSounds.size(); ++i) {
            if (state.announcerSoundLoaded[i] != 0) {
                UnloadSound(state.announcerSounds[i]);
                state.announcerSoundLoaded[i] = 0;
            }
        }
        if (state.announcerWeCapturedLoaded) {
            UnloadSound(state.announcerWeCaptured);
            state.announcerWeCapturedLoaded = false;
        }
        if (state.announcerWeLostLoaded) {
            UnloadSound(state.announcerWeLost);
            state.announcerWeLostLoaded = false;
        }
        CloseAudioDevice();
        state.audioReady = false;
    }

    for (int i = 0; i < state.shotgunFrameCount; ++i) {
        UnloadTexture(state.shotgunFrames[i]);
    }
    if (state.lightningGunIdle.id != 0) {
        UnloadTexture(state.lightningGunIdle);
    }
    if (state.lightningGunFiring.id != 0) {
        UnloadTexture(state.lightningGunFiring);
    }
    for (int i = 0; i < state.lightningGunStartupFrameCount; ++i) {
        UnloadTexture(state.lightningGunStartupFrames[i]);
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
    for (EnemyClipAsset& clip : state.enemyClipAssets) {
        unloadEnemyClipAsset(clip);
    }
    state.enemyAnimSetReady = false;

    UnloadTexture(state.wallTexture);
    UnloadTexture(state.floorTexture);
    if (state.menuBackgroundLoaded && state.menuBackgroundTexture.id != 0) {
        UnloadTexture(state.menuBackgroundTexture);
    }
    if (state.logoLoaded && state.logoTexture.id != 0) {
        UnloadTexture(state.logoTexture);
    }
    if (state.uiFontLoaded && state.uiFont.texture.id != 0) {
        gUiFont = nullptr;
        UnloadFont(state.uiFont);
        state.uiFontLoaded = false;
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
    } else if (slot == WeaponSlot::LightningGun) {
        state.weaponAnimMode = WeaponAnimMode::LightningGunEquip;
        state.weaponAnimStartAt = now;
        state.nextWeaponActionAt = now + KnifeEquipSlideDuration;
    } else {
        state.weaponAnimMode = WeaponAnimMode::ShotgunEquip;
        state.weaponAnimStartAt = now;
        state.nextWeaponActionAt = now + KnifeEquipSlideDuration;
    }
}

void updateLightningAudio(ClientState& state) {
    if (!state.audioReady || !state.lightningAudioLoaded) {
        return;
    }
    const bool lgFiring = state.equippedWeapon == WeaponSlot::LightningGun && state.weaponAnimMode == WeaponAnimMode::LightningGunFiring;
    if (lgFiring) {
        if (!state.lightningLoopPlaying) {
            state.lightningLoopVolume = SfxVolumeMaster * SfxVolumeLightningLoop;
            SetMusicVolume(state.lightningFireLoop, state.lightningLoopVolume);
            PlayMusicStream(state.lightningFireLoop);
            state.lightningLoopPlaying = true;
        }
        state.lightningLoopFadingOut = false;
        UpdateMusicStream(state.lightningFireLoop);
    } else if (state.lightningLoopPlaying) {
        state.lightningLoopFadingOut = true;
        UpdateMusicStream(state.lightningFireLoop);
        const float dt = std::max(GetFrameTime(), 0.00001f);
        state.lightningLoopVolume = std::max(0.0f, state.lightningLoopVolume - dt / LightningLoopFadeOutSeconds);
        SetMusicVolume(state.lightningFireLoop, state.lightningLoopVolume);
        if (state.lightningLoopVolume <= 0.0f) {
            StopMusicStream(state.lightningFireLoop);
            state.lightningLoopPlaying = false;
            state.lightningLoopFadingOut = false;
            state.lightningLoopVolume = SfxVolumeMaster * SfxVolumeLightningLoop;
            SetMusicVolume(state.lightningFireLoop, state.lightningLoopVolume);
        }
    }
}

void sendHello(ClientState& state) {
    arena::HelloPacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Hello);
    std::memset(packet.desiredName, 0, sizeof(packet.desiredName));
    const std::string cleaned = trimForJoinName(state.desiredJoinName);
    std::memcpy(packet.desiredName, cleaned.c_str(), std::min(cleaned.size(), sizeof(packet.desiredName) - 1));
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

    Rectangle nameField{columnX, baseY - 96.0f, columnW, 58.0f};
    const Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        state.joinNameActive = CheckCollisionPointRec(mouse, nameField);
    }
    if (state.joinNameActive) {
        int codepoint = GetCharPressed();
        while (codepoint > 0) {
            const bool printableAscii = codepoint >= 32 && codepoint <= 126;
            if (printableAscii && state.desiredJoinName.size() < static_cast<size_t>(arena::MaxPlayerNameChars - 1)) {
                state.desiredJoinName.push_back(static_cast<char>(codepoint));
            }
            codepoint = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !state.desiredJoinName.empty()) {
            state.desiredJoinName.pop_back();
        }
    }

    DrawRectangleRounded(nameField, 0.18f, 8, Color{15, 20, 30, 195});
    DrawRectangleRoundedLinesEx(nameField, 0.18f, 8, 2.0f, state.joinNameActive ? Color{255, 219, 160, 255} : Color{130, 146, 170, 220});
    DrawText("Name", static_cast<int>(nameField.x + 14.0f), static_cast<int>(nameField.y - 30.0f), 24, Color{214, 220, 232, 240});
    std::string shownName = state.desiredJoinName.empty() ? "Player" : state.desiredJoinName;
    if (state.joinNameActive && (static_cast<int>(GetTime() * 2.0) % 2 == 0) && shownName.size() < static_cast<size_t>(arena::MaxPlayerNameChars - 1)) {
        shownName.push_back('_');
    }
    DrawText(shownName.c_str(), static_cast<int>(nameField.x + 14.0f), static_cast<int>(nameField.y + 14.0f), 28, Color{245, 236, 215, 255});

    if (IsKeyPressed(KEY_UP)) state.menuIndex = (state.menuIndex + 2) % 3;
    if (IsKeyPressed(KEY_DOWN)) state.menuIndex = (state.menuIndex + 1) % 3;

    bool joinClicked = drawMenuButton(buttons[0], "Join Game", state.menuIndex == 0);
    bool settingsClicked = drawMenuButton(buttons[1], "Settings", state.menuIndex == 1);
    bool quitClicked = drawMenuButton(buttons[2], "Quit Game", state.menuIndex == 2);

    const bool activate = IsKeyPressed(KEY_ENTER) && !keyDown(KEY_LEFT_ALT) && !keyDown(KEY_RIGHT_ALT);
    if (joinClicked || (activate && state.menuIndex == 0)) {
        state.desiredJoinName = trimForJoinName(state.desiredJoinName);
        state.screenMode = ScreenMode::InGame;
        state.connected = false;
        state.localPlayerId = 0;
        state.localPlayerName = state.desiredJoinName;
        state.players.clear();
        state.latestServerTick = 0;
        DisableCursor();
    } else if (settingsClicked || (activate && state.menuIndex == 1)) {
        state.screenMode = ScreenMode::Settings;
    } else if (quitClicked || (activate && state.menuIndex == 2)) {
        CloseWindow();
    }

    const char* hint = "Type name, then join. Up/Down + Enter or mouse";
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

    constexpr int SettingsRowCount = 5;
    if (IsKeyPressed(KEY_UP)) state.settingsIndex = (state.settingsIndex + SettingsRowCount - 1) % SettingsRowCount;
    if (IsKeyPressed(KEY_DOWN)) state.settingsIndex = (state.settingsIndex + 1) % SettingsRowCount;

    const bool leftAdjust = IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A);
    const bool rightAdjust = IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D);
    const bool toggle = IsKeyPressed(KEY_ENTER) && !keyDown(KEY_LEFT_ALT) && !keyDown(KEY_RIGHT_ALT);

    if (state.settingsIndex == 0) {
        if (leftAdjust) {
            state.resolutionIndex = (state.resolutionIndex + static_cast<int>(ResolutionPresets.size()) - 1) % static_cast<int>(ResolutionPresets.size());
            if (state.displayMode == DisplayMode::Windowed) {
                applyDisplayMode(state, DisplayMode::Windowed);
            }
        }
        if (rightAdjust) {
            state.resolutionIndex = (state.resolutionIndex + 1) % static_cast<int>(ResolutionPresets.size());
            if (state.displayMode == DisplayMode::Windowed) {
                applyDisplayMode(state, DisplayMode::Windowed);
            }
        }
    } else if (state.settingsIndex == 1) {
        if (leftAdjust) {
            const int mode = (static_cast<int>(state.displayMode) + 2) % 3;
            applyDisplayMode(state, static_cast<DisplayMode>(mode));
        }
        if (rightAdjust || toggle) {
            const int mode = (static_cast<int>(state.displayMode) + 1) % 3;
            applyDisplayMode(state, static_cast<DisplayMode>(mode));
        }
    } else if (state.settingsIndex == 2) {
        if (leftAdjust) state.mouseSensitivity = std::max(0.0003f, state.mouseSensitivity - 0.0001f);
        if (rightAdjust) state.mouseSensitivity = std::min(0.006f, state.mouseSensitivity + 0.0001f);
    } else if (state.settingsIndex == 3) {
        if (leftAdjust || rightAdjust || toggle) state.hitSoundEnabled = !state.hitSoundEnabled;
    } else if (state.settingsIndex == 4) {
        if (leftAdjust) state.hitSoundVolume = std::max(0.0f, state.hitSoundVolume - 0.05f);
        if (rightAdjust) state.hitSoundVolume = std::min(1.0f, state.hitSoundVolume + 0.05f);
        if (state.hitSoundLoaded) {
            SetSoundVolume(state.hitSound, SfxVolumeMaster * state.hitSoundVolume);
            for (int i = 0; i < state.hitSoundAliasCount; ++i) {
                SetSoundVolume(state.hitSoundAliases[i], SfxVolumeMaster * state.hitSoundVolume);
            }
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

    const ResolutionPreset selectedRes = ResolutionPresets[static_cast<size_t>(state.resolutionIndex)];
    drawSettingRow(0, 240.0f, "Resolution", TextFormat("%dx%d", selectedRes.width, selectedRes.height));
    drawSettingRow(1, 304.0f, "Display Mode", displayModeName(state.displayMode));
    drawSettingRow(2, 368.0f, "Mouse Sensitivity", TextFormat("%.4f", state.mouseSensitivity));
    drawSettingRow(3, 432.0f, "Hit Sound", state.hitSoundEnabled ? "On" : "Off");
    drawSettingRow(4, 496.0f, "Hit Volume", TextFormat("%d%%", static_cast<int>(std::round(state.hitSoundVolume * 100.0f))));

    DrawText("Up/Down to select | Left/Right to adjust", static_cast<int>(columnX + (columnW - MeasureText("Up/Down to select | Left/Right to adjust", 24)) * 0.5f), 570, 24, Color{172, 178, 192, 255});
    DrawText("Alt+Enter toggles Borderless/Fullscreen", static_cast<int>(columnX + (columnW - MeasureText("Alt+Enter toggles Borderless/Fullscreen", 24)) * 0.5f), 604, 24, Color{172, 178, 192, 255});
    DrawText("Press Esc to return", static_cast<int>(columnX + (columnW - MeasureText("Press Esc to return", 24)) * 0.5f), 638, 24, Color{172, 178, 192, 255});

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
    packet.fireHeld = IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 1 : 0;
    packet.dashPressed = state.dashQueued ? 1 : 0;
    packet.dashMoveX = state.dashMoveX;
    packet.dashMoveZ = state.dashMoveZ;
    packet.crouchHeld = (keyDown(KEY_LEFT_SHIFT) || keyDown(KEY_RIGHT_SHIFT)) ? 1 : 0;
    packet.weaponSlot = static_cast<uint8_t>(state.equippedWeapon);
    packet.lastReceivedServerTick = state.latestServerTick;
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

void cleanupMissingEnemyAnimState(ClientState& state);

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
            state.localPlayerName = std::string(packet.assignedName);
            state.connected = true;
        } else if (received == sizeof(arena::MapDataPacket) && arena::hasValidHeader(buffer, received, arena::PacketType::MapData)) {
            arena::MapDataPacket packet{};
            std::memcpy(&packet, buffer, sizeof(packet));
            state.mapWidth = std::min<uint16_t>(packet.width, arena::MapMaxWidth);
            state.mapHeight = std::min<uint16_t>(packet.height, arena::MapMaxHeight);
            state.mapCellSize = packet.cellSize;
            state.mapOriginX = packet.originX;
            state.mapOriginZ = packet.originZ;
            std::memcpy(state.mapCells.data(), packet.cells, sizeof(packet.cells));
            state.mapLoaded = true;
        } else if (received >= static_cast<int>(sizeof(arena::PacketHeader) + sizeof(uint32_t) * 2) &&
                   arena::hasValidHeader(buffer, received, arena::PacketType::Snapshot)) {
            arena::SnapshotPacket packet{};
            std::memcpy(&packet, buffer, std::min<int>(received, static_cast<int>(sizeof(packet))));
            state.latestServerTick = packet.serverTick;

            std::map<uint32_t, RemotePlayer> nextPlayers;
            const uint32_t count = std::min<uint32_t>(packet.playerCount, arena::MaxPlayers);
            const double now = arena::secondsNow();
            for (uint32_t i = 0; i < count; ++i) {
                RemotePlayer player{};
                player.name = std::string(packet.players[i].name);
                player.position = {packet.players[i].x, packet.players[i].y, packet.players[i].z};
                player.velocity = {packet.players[i].vx, packet.players[i].vy, packet.players[i].vz};
                player.yaw = packet.players[i].yaw;
                player.pitch = packet.players[i].pitch;
                player.crouched = packet.players[i].crouched != 0;
                player.teamId = packet.players[i].teamId;
                player.health = packet.players[i].health;
                player.dead = packet.players[i].dead != 0;
                player.weaponSlot = packet.players[i].weaponSlot;
                player.firing = packet.players[i].firing != 0;
                player.lgBeamActive = packet.players[i].lgBeamActive != 0;
                player.lgBeamEnd = {packet.players[i].lgBeamEndX, packet.players[i].lgBeamEndY, packet.players[i].lgBeamEndZ};
                player.hitConfirmCount = packet.players[i].hitConfirmCount;
                player.lastDamageDealt = packet.players[i].lastDamageDealt;
                player.lastHitTargetId = packet.players[i].lastHitTargetId;
                player.pingMs = packet.players[i].pingMs;
                player.kills = packet.players[i].kills;
                player.deaths = packet.players[i].deaths;
                player.damageDealt = packet.players[i].damageDealt;
                if (packet.players[i].playerId != state.localPlayerId && player.firing && state.audioReady) {
                    const uint8_t prevFiring = state.enemyPrevFiring[packet.players[i].playerId];
                    const bool risingEdge = prevFiring == 0;
                    if (player.weaponSlot == static_cast<uint8_t>(WeaponSlot::Shotgun)) {
                        if (risingEdge) {
                            PlaySound(state.shotgunFire);
                        }
                    } else if (player.weaponSlot == static_cast<uint8_t>(WeaponSlot::LightningGun) && state.lightningAudioLoaded) {
                        double& nextAt = state.enemyNextFireSoundAt[packet.players[i].playerId];
                        if (risingEdge || now >= nextAt) {
                            Sound& s = nextAliasOrBase(
                                state.lightningFireStart,
                                state.lightningFireStartAliases,
                                state.lightningFireStartAliasCount,
                                state.lightningFireStartAliasNext);
                            PlaySound(s);
                            nextAt = now + 0.11;
                        }
                    }
                }
                state.enemyPrevFiring[packet.players[i].playerId] = player.firing ? 1 : 0;
                nextPlayers[packet.players[i].playerId] = player;
            }
            state.players = std::move(nextPlayers);
            for (auto itF = state.enemyPrevFiring.begin(); itF != state.enemyPrevFiring.end();) {
                if (state.players.find(itF->first) == state.players.end()) {
                    itF = state.enemyPrevFiring.erase(itF);
                } else {
                    ++itF;
                }
            }
            for (auto itS = state.enemyNextFireSoundAt.begin(); itS != state.enemyNextFireSoundAt.end();) {
                if (state.players.find(itS->first) == state.players.end()) {
                    itS = state.enemyNextFireSoundAt.erase(itS);
                } else {
                    ++itS;
                }
            }
            cleanupMissingEnemyAnimState(state);
            const uint8_t previousHillOwnerTeam = state.hillOwnerTeam;
            state.team1TimeLeftSeconds = packet.team1TimeLeftSeconds;
            state.team2TimeLeftSeconds = packet.team2TimeLeftSeconds;
            state.hillOwnerTeam = packet.hillOwnerTeam;
            state.hillCaptureTeam = packet.hillCaptureTeam;
            state.hillContested = packet.hillContested;
            state.hillOvertime = packet.hillOvertime;
            state.hillWinnerTeam = packet.hillWinnerTeam;
            state.team1RoundPoints = packet.team1RoundPoints;
            state.team2RoundPoints = packet.team2RoundPoints;
            state.matchWinnerTeam = packet.matchWinnerTeam;
            state.matchResetSecondsLeft = packet.matchResetSecondsLeft;
            state.hillCaptureProgress = packet.hillCaptureProgress;

            if (packet.announcerSeq != state.lastAnnouncerSeqHeard) {
                state.lastAnnouncerSeqHeard = packet.announcerSeq;
                const arena::AnnouncerEvent event = static_cast<arena::AnnouncerEvent>(packet.announcerEvent);
                playAnnouncer(state, event);
                const char* eventLabel = announcerEventLabel(event);
                if (eventLabel[0] != '\0' && packet.announcerActorPlayerId != 0) {
                    std::string actorName = "Player " + std::to_string(packet.announcerActorPlayerId);
                    const auto actorIt = state.players.find(packet.announcerActorPlayerId);
                    if (actorIt != state.players.end() && !actorIt->second.name.empty()) {
                        actorName = actorIt->second.name;
                    }
                    state.serverAnnouncementText = actorName + " " + eventLabel;
                    state.serverAnnouncementUntil = arena::secondsNow() + 2.6;
                }
            }

            uint8_t localTeam = state.localTeamId;
            const auto localIt = state.players.find(state.localPlayerId);
            if (localIt != state.players.end()) {
                localTeam = localIt->second.teamId;
            }
            if (localTeam != 0 && previousHillOwnerTeam != state.hillOwnerTeam) {
                if (state.hillOwnerTeam == localTeam) {
                    if (state.audioReady && state.announcerWeCapturedLoaded) {
                        PlaySound(state.announcerWeCaptured);
                    }
                } else if (previousHillOwnerTeam == localTeam && state.hillOwnerTeam != 0) {
                    if (state.audioReady && state.announcerWeLostLoaded) {
                        PlaySound(state.announcerWeLost);
                    }
                }
            }
        }
    }
}

void syncLocalPosition(ClientState& state) {
    const auto it = state.players.find(state.localPlayerId);
    if (it != state.players.end()) {
        const uint8_t previousHealth = state.localHealth;
        state.localPosition = it->second.position;
        state.localCrouched = it->second.crouched;
        state.localTeamId = it->second.teamId;
        state.localHealth = it->second.health;
        state.localDead = it->second.dead;
        const float latestPingMs = static_cast<float>(it->second.pingMs);
        if (!state.smoothedPingInitialized) {
            state.smoothedPingMs = latestPingMs;
            state.smoothedPingInitialized = true;
        } else {
            state.smoothedPingMs += (latestPingMs - state.smoothedPingMs) * 0.18f;
        }
        if (state.localHealth < previousHealth) {
            state.damageFlash = 1.0f;
        }
        if (state.localHitConfirmInitialized && it->second.hitConfirmCount != state.localHitConfirmCount &&
            state.hitSoundEnabled && state.hitSoundLoaded && state.audioReady) {
            const uint16_t delta = static_cast<uint16_t>(it->second.hitConfirmCount - state.localHitConfirmCount);
            const int plays = std::min<int>(delta == 0 ? 0 : delta, 8);
            for (int i = 0; i < plays; ++i) {
                Sound& s = nextAliasOrBase(state.hitSound, state.hitSoundAliases, state.hitSoundAliasCount, state.hitSoundAliasNext);
                PlaySound(s);
            }
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
        if (state.localKillCountInitialized && it->second.kills != state.localKillCount) {
            const uint16_t delta = static_cast<uint16_t>(it->second.kills - state.localKillCount);
            int toAdd = std::min<int>(delta == 0 ? 0 : delta, 6);
            while (toAdd-- > 0) {
                KillFeedEntry entry{};
                entry.lifetime = 2.2f;
                const auto targetIt = state.players.find(it->second.lastHitTargetId);
                if (targetIt != state.players.end() && !targetIt->second.name.empty()) {
                    entry.text = "ELIMINATED " + targetIt->second.name;
                } else {
                    entry.text = "ELIMINATED ENEMY";
                }
                state.killFeed.push_back(entry);
            }
        }
        state.localHitConfirmCount = it->second.hitConfirmCount;
        state.localHitConfirmInitialized = true;
        state.localKillCount = it->second.kills;
        state.localKillCountInitialized = true;
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

void updateKillFeed(ClientState& state, float dt) {
    for (KillFeedEntry& entry : state.killFeed) {
        entry.age += dt;
    }
    state.killFeed.erase(
        std::remove_if(state.killFeed.begin(), state.killFeed.end(), [](const KillFeedEntry& entry) {
            return entry.age >= entry.lifetime;
        }),
        state.killFeed.end());
}

void drawTriangleBothSides(Vector3 a, Vector3 b, Vector3 c, Color color) {
    DrawTriangle3D(a, b, c, color);
    DrawTriangle3D(a, c, b, color);
}

void drawRampWedgeX(Vector3 center, float width, float depth, float height, Color fill, Color wire) {
    const float halfW = width * 0.5f;
    const float halfD = depth * 0.5f;

    const Vector3 a{center.x - halfW, center.y, center.z - halfD}; // low-left-back
    const Vector3 b{center.x + halfW, center.y, center.z - halfD}; // high-side base back
    const Vector3 c{center.x + halfW, center.y, center.z + halfD}; // high-side base front
    const Vector3 d{center.x - halfW, center.y, center.z + halfD}; // low-left-front
    const Vector3 e{center.x + halfW, center.y + height, center.z - halfD}; // high-side top back
    const Vector3 f{center.x + halfW, center.y + height, center.z + halfD}; // high-side top front

    // Bottom.
    drawTriangleBothSides(a, b, c, fill);
    drawTriangleBothSides(a, c, d, fill);

    // Sloped top.
    drawTriangleBothSides(a, e, f, fill);
    drawTriangleBothSides(a, f, d, fill);

    // Side caps.
    drawTriangleBothSides(a, b, e, fill);
    drawTriangleBothSides(d, c, f, fill);

    // Vertical high wall.
    drawTriangleBothSides(b, c, f, fill);
    drawTriangleBothSides(b, f, e, fill);

    // Wire edges.
    DrawLine3D(a, b, wire);
    DrawLine3D(b, c, wire);
    DrawLine3D(c, d, wire);
    DrawLine3D(d, a, wire);
    DrawLine3D(b, e, wire);
    DrawLine3D(c, f, wire);
    DrawLine3D(e, f, wire);
    DrawLine3D(a, e, wire);
    DrawLine3D(d, f, wire);
}

void drawKothTestMapGeometry(const ClientState& state) {
    const Color block{102, 108, 116, 255};
    const Color ramp{118, 104, 90, 255};
    const Color wire{168, 176, 190, 220};
    if (!state.mapLoaded || state.mapWidth == 0 || state.mapHeight == 0) {
        return;
    }

    for (uint16_t z = 0; z < state.mapHeight; ++z) {
        for (uint16_t x = 0; x < state.mapWidth; ++x) {
            const char symbol = state.mapCells[z * arena::MapMaxWidth + x];
            float h = 0.0f;
            Color c = block;
            if (symbol == 'B') {
                h = 5.0f;
            } else if (symbol == 'R') {
                h = 2.0f;
                c = ramp;
            }
            if (h <= 0.0f) {
                continue;
            }
            const float cx = state.mapOriginX + static_cast<float>(x) * state.mapCellSize;
            const float cz = state.mapOriginZ + static_cast<float>(z) * state.mapCellSize;
            if (symbol == 'R') {
                // Match server rampSurfaceYAt(): low at min-X, high at max-X.
                drawRampWedgeX({cx, 0.0f, cz}, state.mapCellSize * 0.95f, state.mapCellSize * 0.95f, h, c, wire);
            } else {
                DrawCube({cx, h * 0.5f, cz}, state.mapCellSize * 0.95f, h, state.mapCellSize * 0.95f, c);
                DrawCubeWires({cx, h * 0.5f, cz}, state.mapCellSize * 0.95f, h, state.mapCellSize * 0.95f, wire);
            }
        }
    }
}

void drawRoom(ClientState& state) {
    constexpr float half = arena::RoomHalfSize;
    constexpr float height = arena::RoomHeight;
    constexpr float thickness = 0.2f;

    DrawModel(state.floorModel, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
    DrawModel(state.ceilingModel, {0.0f, height, 0.0f}, 1.0f, WHITE);

    DrawModel(state.wallModelX, {-half - thickness * 0.5f, height * 0.5f, 0.0f}, 1.0f, WHITE);
    DrawModel(state.wallModelX, {half + thickness * 0.5f, height * 0.5f, 0.0f}, 1.0f, WHITE);
    DrawModel(state.wallModelZ, {0.0f, height * 0.5f, -half - thickness * 0.5f}, 1.0f, WHITE);
    DrawModel(state.wallModelZ, {0.0f, height * 0.5f, half + thickness * 0.5f}, 1.0f, WHITE);

    drawKothTestMapGeometry(state);

    DrawGrid(32, 5.0f);

    constexpr float hillRadius = 8.0f;
    DrawCircle3D({0.0f, 0.08f, 0.0f}, hillRadius, {1.0f, 0.0f, 0.0f}, 90.0f, Color{255, 238, 165, 60});
    DrawCircle3D({0.0f, 0.09f, 0.0f}, hillRadius - 0.45f, {1.0f, 0.0f, 0.0f}, 90.0f, Color{255, 240, 170, 28});
    DrawCylinderWires({0.0f, 0.10f, 0.0f}, hillRadius, hillRadius, 0.12f, 48, Color{255, 240, 170, 255});
    DrawCylinderWires({0.0f, 0.10f, 0.0f}, hillRadius - 0.45f, hillRadius - 0.45f, 0.10f, 48, Color{255, 246, 210, 190});
    for (int i = 0; i < 12; ++i) {
        const float a = (static_cast<float>(i) / 12.0f) * 2.0f * PI;
        const float sx = std::cos(a) * (hillRadius - 0.3f);
        const float sz = std::sin(a) * (hillRadius - 0.3f);
        const float ex = std::cos(a) * (hillRadius + 0.8f);
        const float ez = std::sin(a) * (hillRadius + 0.8f);
        DrawLine3D({sx, 0.11f, sz}, {ex, 0.11f, ez}, Color{255, 236, 150, 220});
    }
    DrawSphere({0.0f, 0.60f, 0.0f}, 0.30f, Color{255, 240, 180, 220});

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

EnemyAnimClip pickEnemyAnim(const RemotePlayer& player, EnemyAnimState& animState, float dt) {
    const bool airborne = player.position.y > (animState.wasAirborne ? 0.04f : 0.10f);
    if (player.dead) {
        return EnemyAnimClip::HitReact;
    }
    if (animState.hitReactTimeRemaining > 0.0f) {
        return EnemyAnimClip::HitReact;
    }
    if (airborne) {
        return EnemyAnimClip::Jump; // Full jump clip used as airborne pose/sequence.
    }
    if (animState.wasAirborne && !airborne) {
        animState.frame = 0.0f;
    }
    const float sinYaw = std::sin(player.yaw);
    const float cosYaw = std::cos(player.yaw);
    const arena::Vec3 forward{sinYaw, 0.0f, -cosYaw};
    const arena::Vec3 right{cosYaw, 0.0f, sinYaw};
    arena::Vec3 velocity = player.velocity;
    const float netVelSq = velocity.x * velocity.x + velocity.z * velocity.z;
    if (netVelSq < 0.000001f && animState.hasLastPosition && dt > 0.0001f) {
        velocity = (player.position - animState.lastPosition) * (1.0f / dt);
    }
    const float rawSpeed = std::sqrt(velocity.x * velocity.x + velocity.z * velocity.z);
    const float smoothT = animState.hasLastPosition ? std::clamp(dt * 12.0f, 0.0f, 1.0f) : 1.0f;
    animState.smoothedVelocity.x += (velocity.x - animState.smoothedVelocity.x) * smoothT;
    animState.smoothedVelocity.y = 0.0f;
    animState.smoothedVelocity.z += (velocity.z - animState.smoothedVelocity.z) * smoothT;
    animState.smoothedSpeed = std::sqrt(
        animState.smoothedVelocity.x * animState.smoothedVelocity.x +
        animState.smoothedVelocity.z * animState.smoothedVelocity.z);
    animState.estimatedSpeed = animState.smoothedSpeed;

    const bool activeIdle = animState.activeClip == EnemyAnimClip::Idle;
    const float idleThreshold = activeIdle ? 0.35f : 0.18f;
    if (animState.smoothedSpeed < idleThreshold) {
        return EnemyAnimClip::Idle;
    }

    if (rawSpeed < 0.08f) {
        switch (animState.activeClip) {
        case EnemyAnimClip::WalkForward:
        case EnemyAnimClip::RunForward:
        case EnemyAnimClip::RunBackward:
        case EnemyAnimClip::StrafeLeft:
        case EnemyAnimClip::StrafeRight:
            return animState.activeClip;
        default:
            return EnemyAnimClip::WalkForward;
        }
    }

    const float moveForward = arena::dot(animState.smoothedVelocity, forward);
    const float moveRight = arena::dot(animState.smoothedVelocity, right);
    const float absForward = std::abs(moveForward);
    const float absRight = std::abs(moveRight);

    // Deterministic directional mapping: dominant lateral motion is strafe,
    // dominant negative forward motion is run backward, otherwise move forward.
    if (absRight > absForward * 0.75f) {
        return moveRight > 0.0f ? EnemyAnimClip::StrafeRight : EnemyAnimClip::StrafeLeft;
    }
    if (moveForward < -0.05f) {
        return EnemyAnimClip::RunBackward;
    }
    const bool activeRun = animState.activeClip == EnemyAnimClip::RunForward;
    if (animState.smoothedSpeed > (activeRun ? 3.8f : 4.6f)) {
        return EnemyAnimClip::RunForward;
    }
    return EnemyAnimClip::WalkForward;
}

void drawAnimatedRemotePlayer(ClientState& state, uint32_t playerId, const RemotePlayer& player, float dt) {
    sanitizeEnemyTune(state);
    EnemyAnimState& animState = state.enemyAnimStateById[playerId];
    const bool airborne = player.position.y > 0.08f;
    animState.hitReactTimeRemaining = std::max(0.0f, animState.hitReactTimeRemaining - std::max(dt, 0.0f));
    if (animState.hasLastHealth && !player.dead && player.health < animState.lastHealth) {
        animState.hitReactTimeRemaining = 0.20f;
        animState.hitReactRestartRequested = true;
    }
    animState.lastHealth = player.health;
    animState.hasLastHealth = true;
    auto finishAnimSample = [&]() {
        animState.lastPosition = player.position;
        animState.hasLastPosition = true;
        animState.wasAirborne = airborne;
    };

    const EnemyAnimClip desiredClip = pickEnemyAnim(player, animState, dt);
    EnemyClipAsset* renderClip = nullptr;
    EnemyAnimClip renderClipType = desiredClip;
    auto clipLooksDrawable = [](const EnemyClipAsset& c) {
        return c.modelLoaded && c.model.meshCount > 0 && c.model.meshes != nullptr;
    };
    auto clipHasPlayableAnim = [](const EnemyClipAsset& c) {
        return c.modelLoaded && c.animValidForModel && c.anims != nullptr && c.animCount > 0;
    };

    auto clipPlayable = [&](EnemyAnimClip clipType) {
        const EnemyClipAsset& clip = state.enemyClipAssets[static_cast<size_t>(clipType)];
        return clipLooksDrawable(clip) && clipHasPlayableAnim(clip);
    };

    auto usePlayableClip = [&](EnemyAnimClip clipType) {
        auto tryClip = [&](EnemyAnimClip candidate) {
            if (!clipPlayable(candidate)) {
                return false;
            }
            renderClipType = candidate;
            renderClip = &state.enemyClipAssets[static_cast<size_t>(candidate)];
            return true;
        };

        switch (clipType) {
        case EnemyAnimClip::StrafeLeft:
            if (tryClip(EnemyAnimClip::StrafeLeft) || tryClip(EnemyAnimClip::WalkForward) || tryClip(EnemyAnimClip::RunForward)) return;
            break;
        case EnemyAnimClip::StrafeRight:
            if (tryClip(EnemyAnimClip::StrafeRight) || tryClip(EnemyAnimClip::WalkForward) || tryClip(EnemyAnimClip::RunForward)) return;
            break;
        case EnemyAnimClip::RunBackward:
            if (tryClip(EnemyAnimClip::RunBackward) || tryClip(EnemyAnimClip::WalkForward) || tryClip(EnemyAnimClip::RunForward)) return;
            break;
        case EnemyAnimClip::RunForward:
            if (tryClip(EnemyAnimClip::RunForward) || tryClip(EnemyAnimClip::WalkForward)) return;
            break;
        case EnemyAnimClip::WalkForward:
            if (tryClip(EnemyAnimClip::WalkForward) || tryClip(EnemyAnimClip::RunForward)) return;
            break;
        default:
            if (tryClip(clipType)) return;
            break;
        }

        renderClipType = EnemyAnimClip::Idle;
        renderClip = &state.enemyClipAssets[static_cast<size_t>(EnemyAnimClip::Idle)];
    };
    usePlayableClip(desiredClip);
    if (animState.hitReactRestartRequested && renderClipType == EnemyAnimClip::HitReact) {
        animState.activeClip = EnemyAnimClip::HitReact;
        animState.pendingClip = EnemyAnimClip::HitReact;
        animState.pendingClipTime = 0.0f;
        animState.frame = 0.0f;
        animState.hitReactRestartRequested = false;
    }

    if (renderClipType != animState.activeClip) {
        if (renderClipType != animState.pendingClip) {
            animState.pendingClip = renderClipType;
            animState.pendingClipTime = 0.0f;
        }
        animState.pendingClipTime += std::max(dt, 0.0f);
        const bool immediateSwitch = renderClipType == EnemyAnimClip::Jump ||
            renderClipType == EnemyAnimClip::HitReact ||
            animState.activeClip == EnemyAnimClip::Jump ||
            animState.activeClip == EnemyAnimClip::HitReact;
        if (immediateSwitch || animState.pendingClipTime >= 0.12f) {
            animState.activeClip = renderClipType;
            animState.pendingClip = renderClipType;
            animState.pendingClipTime = 0.0f;
            animState.frame = 0.0f;
            animState.animUpdateAccumulator = 0.0f;
            animState.lastAppliedAnimFrame = -1;
        } else {
            usePlayableClip(animState.activeClip);
            if (renderClipType != animState.activeClip) {
                animState.activeClip = renderClipType;
                animState.pendingClip = renderClipType;
                animState.pendingClipTime = 0.0f;
                animState.frame = 0.0f;
                animState.animUpdateAccumulator = 0.0f;
                animState.lastAppliedAnimFrame = -1;
            }
        }
    } else {
        animState.pendingClip = renderClipType;
        animState.pendingClipTime = 0.0f;
    }
    if (renderClipType != EnemyAnimClip::HitReact) {
        animState.hitReactRestartRequested = false;
    }
    if (!clipLooksDrawable(*renderClip)) {
        state.enemyModelDebug = "enemy model debug: idle model not loaded";
        finishAnimSample();
        return;
    }
    constexpr bool kEnableRemoteEnemyAnimation = true;
    if (kEnableRemoteEnemyAnimation && renderClip->anims != nullptr && renderClip->animCount > 0 && renderClip->animValidForModel) {
        ModelAnimation& anim = renderClip->anims[0];
        const float fps = (renderClip->animFps > 0.0f) ? renderClip->animFps : firstAnimFpsOrDefault(renderClip->anims, renderClip->animCount);
        if (anim.frameCount > 0) {
            constexpr float kRemoteAnimTickHz = 24.0f;
            constexpr float kRemoteAnimStep = 1.0f / kRemoteAnimTickHz;
            animState.animUpdateAccumulator += std::max(0.0f, dt);
            bool advanced = false;
            while (animState.animUpdateAccumulator >= kRemoteAnimStep) {
                animState.frame += fps * kRemoteAnimStep;
                while (animState.frame >= static_cast<float>(anim.frameCount)) {
                    animState.frame -= static_cast<float>(anim.frameCount);
                }
                animState.animUpdateAccumulator -= kRemoteAnimStep;
                advanced = true;
            }
            const int frame = std::clamp(static_cast<int>(animState.frame), 0, anim.frameCount - 1);
            if (advanced && frame != animState.lastAppliedAnimFrame) {
                UpdateModelAnimation(renderClip->model, anim, frame);
                animState.lastAppliedAnimFrame = frame;
            }
        }
    }

    const float scaleY = renderClip->modelScale;
    const float scaleXZ = scaleY;

    // Visibility-first transform for reliable in-game validation.
    // Anchor feet to player position and rotate only around yaw.
    const float totalYawDeg = (-player.yaw * RAD2DEG) + 180.0f;
    Vector3 drawPos{
        player.position.x,
        player.position.y - (renderClip->modelMinY * scaleY),
        player.position.z
    };
    if (!std::isfinite(drawPos.x) || !std::isfinite(drawPos.y) || !std::isfinite(drawPos.z) || !std::isfinite(renderClip->modelScale)) {
        state.enemyModelDebug = "enemy model debug: non-finite transform";
        finishAnimSample();
        return;
    }
    if (ShowBottomDebugOverlay) {
        const BoundingBox dbgBounds = getModelBounds(renderClip->model);
        const float dbgSizeX = dbgBounds.max.x - dbgBounds.min.x;
        const float dbgSizeY = dbgBounds.max.y - dbgBounds.min.y;
        const float dbgSizeZ = dbgBounds.max.z - dbgBounds.min.z;
        state.enemyModelDebug = "enemy model debug: mesh=" + std::to_string(renderClip->model.meshCount) +
            " vtx=" + std::to_string((renderClip->model.meshCount > 0) ? renderClip->model.meshes[0].vertexCount : 0) +
            " tri=" + std::to_string((renderClip->model.meshCount > 0) ? renderClip->model.meshes[0].triangleCount : 0) +
            " sx=" + std::to_string(dbgSizeX) +
            " sy=" + std::to_string(dbgSizeY) +
            " sz=" + std::to_string(dbgSizeZ) +
            " sXZ=" + std::to_string(scaleXZ) +
            " sY=" + std::to_string(scaleY) +
            " minY=" + std::to_string(renderClip->modelMinY) +
            " drawY=" + std::to_string(drawPos.y) +
            " yaw=" + std::to_string(totalYawDeg) +
            " frame=" + std::to_string(animState.frame) +
            " anim=" + std::string(kEnableRemoteEnemyAnimation ? "on" : "off") +
            " clip=" + std::to_string(static_cast<int>(renderClipType)) +
            " src=" + renderClip->sourcePath;
    }
    const Vector3 eulerDeg{0.0f, totalYawDeg, 0.0f};
    drawModelEuler(
        renderClip->model,
        drawPos,
        eulerDeg,
        Vector3{scaleXZ, scaleY, scaleXZ},
        WHITE,
        &renderClip->alphaMeshes);
    finishAnimSample();
}

void drawRemoteWeaponProxy(const RemotePlayer& player) {
    if (player.dead) {
        return;
    }

    const float eyeHeight = player.crouched ? arena::CrouchEyeHeight : arena::StandEyeHeight;
    const Vector3 eye{player.position.x, player.position.y + eyeHeight, player.position.z};
    const Vector3 forward = cameraForward(player.yaw, player.pitch);
    const Vector3 right{
        static_cast<float>(std::cos(player.yaw)),
        0.0f,
        static_cast<float>(std::sin(player.yaw))
    };
    const float shoulderDrop = player.crouched ? 0.18f : 0.32f;
    const Vector3 grip = Vector3Add(
        Vector3Add(eye, Vector3Scale(right, 0.24f)),
        Vector3Add(Vector3{0.0f, -shoulderDrop, 0.0f}, Vector3Scale(forward, 0.24f)));

    if (player.weaponSlot == static_cast<uint8_t>(WeaponSlot::Knife)) {
        const Vector3 handleEnd = Vector3Add(grip, Vector3Scale(forward, 0.18f));
        const Vector3 bladeEnd = Vector3Add(handleEnd, Vector3Scale(forward, 0.46f));
        DrawCylinderEx(grip, handleEnd, 0.045f, 0.045f, 8, Color{62, 66, 78, 255});
        DrawCylinderEx(handleEnd, bladeEnd, 0.024f, 0.012f, 8, Color{205, 215, 225, 255});
        if (player.firing) {
            DrawLine3D(handleEnd, Vector3Add(bladeEnd, Vector3{0.0f, 0.06f, 0.0f}), Color{255, 195, 165, 230});
        }
        return;
    }

    if (player.weaponSlot == static_cast<uint8_t>(WeaponSlot::LightningGun)) {
        const Vector3 bodyEnd = Vector3Add(grip, Vector3Scale(forward, 0.56f));
        const Vector3 muzzle = Vector3Add(bodyEnd, Vector3Scale(forward, 0.30f));
        DrawCylinderEx(grip, bodyEnd, 0.062f, 0.058f, 10, Color{78, 90, 98, 255});
        DrawCylinderEx(bodyEnd, muzzle, 0.040f, 0.032f, 10, Color{95, 112, 122, 255});
        DrawSphere(muzzle, 0.045f, Color{108, 238, 238, 230});
        if (player.firing) {
            DrawSphere(muzzle, 0.07f, Color{180, 255, 250, 200});
        }
        return;
    }

    const Vector3 bodyEnd = Vector3Add(grip, Vector3Scale(forward, 0.62f));
    const Vector3 muzzle = Vector3Add(bodyEnd, Vector3Scale(forward, 0.32f));
    DrawCylinderEx(grip, bodyEnd, 0.068f, 0.062f, 10, Color{80, 78, 74, 255});
    DrawCylinderEx(bodyEnd, muzzle, 0.046f, 0.038f, 10, Color{108, 102, 95, 255});
    if (player.firing) {
        DrawSphere(muzzle, 0.10f, Color{255, 210, 135, 225});
        DrawLine3D(muzzle, Vector3Add(muzzle, Vector3Scale(forward, 1.2f)), Color{255, 215, 140, 210});
    }
}

void drawRemoteServerLightningBeams(const ClientState& state) {
    for (const auto& [id, player] : state.players) {
        if (id == state.localPlayerId || player.dead) {
            continue;
        }
        if (player.weaponSlot != static_cast<uint8_t>(WeaponSlot::LightningGun) || !player.lgBeamActive) {
            continue;
        }
        const float eyeHeight = player.crouched ? arena::CrouchEyeHeight : arena::StandEyeHeight;
        const Vector3 start{player.position.x, player.position.y + eyeHeight, player.position.z};
        const Vector3 end{player.lgBeamEnd.x, player.lgBeamEnd.y, player.lgBeamEnd.z};
        DrawLine3D(start, end, Color{132, 255, 246, 220});
        DrawSphere(start, 0.08f, Color{170, 255, 248, 220});
        DrawSphere(end, 0.09f, Color{190, 255, 250, 200});
    }
}

void cleanupMissingEnemyAnimState(ClientState& state) {
    for (auto it = state.enemyAnimStateById.begin(); it != state.enemyAnimStateById.end();) {
        if (state.players.find(it->first) == state.players.end()) {
            it = state.enemyAnimStateById.erase(it);
        } else {
            ++it;
        }
    }
}

void drawIdleEnemyModelPreview(ClientState& state, float dt) {
    const size_t idleIndex = static_cast<size_t>(EnemyAnimClip::Idle);
    EnemyClipAsset& idle = state.enemyClipAssets[idleIndex];
    if (!idle.modelLoaded) {
        return;
    }

    // Animate idle only when raylib validates this animation/model pair.
    static float previewFrame = 0.0f;
    if (idle.animValidForModel) {
        ModelAnimation& anim = idle.anims[0];
        const float fps = (idle.animFps > 0.0f) ? idle.animFps : firstAnimFpsOrDefault(idle.anims, idle.animCount);
        previewFrame += fps * dt;
        if (anim.frameCount > 0) {
            while (previewFrame >= static_cast<float>(anim.frameCount)) {
                previewFrame -= static_cast<float>(anim.frameCount);
            }
            const int frame = std::clamp(static_cast<int>(previewFrame), 0, anim.frameCount - 1);
            UpdateModelAnimation(idle.model, anim, frame);
        }
    }

    const Vector3 drawPos{
        0.0f,
        0.0f - idle.modelMinY * idle.modelScale,
        0.0f
    };
    drawModelEuler(
        idle.model,
        drawPos,
        Vector3{0.0f, 180.0f, 0.0f},
        Vector3{idle.modelScale, idle.modelScale, idle.modelScale},
        WHITE,
        &idle.alphaMeshes);
}

void updateViewmodelAndFootsteps(ClientState& state, double now, float dt) {
    if (state.localDead) {
        return;
    }

    const bool moving = keyDown(KEY_W) || keyDown(KEY_A) || keyDown(KEY_S) || keyDown(KEY_D);
    const bool grounded = state.localPosition.y <= 0.05f;
    const bool knifeEquipped = state.equippedWeapon == WeaponSlot::Knife;
    const bool lgEquipped = state.equippedWeapon == WeaponSlot::LightningGun;

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
    } else if (lgEquipped) {
        const bool triggerHeld = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
        if (triggerHeld) {
            if (state.weaponAnimMode == WeaponAnimMode::Idle || state.weaponAnimMode == WeaponAnimMode::LightningGunWinddown) {
                state.weaponAnimMode = WeaponAnimMode::LightningGunStartup;
                state.weaponAnimStartAt = now;
                if (state.audioReady && state.lightningAudioLoaded) {
                    Sound& s = nextAliasOrBase(
                        state.lightningFireStart,
                        state.lightningFireStartAliases,
                        state.lightningFireStartAliasCount,
                        state.lightningFireStartAliasNext);
                    PlaySound(s);
                }
            }
        } else {
            if (state.weaponAnimMode == WeaponAnimMode::LightningGunFiring || state.weaponAnimMode == WeaponAnimMode::LightningGunStartup) {
                state.weaponAnimMode = WeaponAnimMode::LightningGunWinddown;
                state.weaponAnimStartAt = now;
            }
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
    if (state.equippedWeapon == WeaponSlot::LightningGun) {
        return -1;
    }

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
    if (state.weaponAnimMode == WeaponAnimMode::Idle || state.weaponAnimMode == WeaponAnimMode::ShotgunFire || state.weaponAnimMode == WeaponAnimMode::ShotgunEquip) {
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

    if (state.weaponAnimMode == WeaponAnimMode::ShotgunEquip || state.weaponAnimMode == WeaponAnimMode::LightningGunEquip) {
        if (now - state.weaponAnimStartAt >= KnifeEquipSlideDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::LightningGunStartup) {
        const double totalDuration = static_cast<double>(state.lightningGunStartupFrameCount) * LightningGunStartupFrameDuration;
        if (now - state.weaponAnimStartAt >= totalDuration) {
            if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
                state.weaponAnimMode = WeaponAnimMode::LightningGunFiring;
            } else {
                state.weaponAnimMode = WeaponAnimMode::LightningGunWinddown;
                state.weaponAnimStartAt = now;
            }
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::LightningGunFiring) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            state.weaponAnimMode = WeaponAnimMode::LightningGunWinddown;
            state.weaponAnimStartAt = now;
        }
        return;
    }

    if (state.weaponAnimMode == WeaponAnimMode::LightningGunWinddown) {
        const double totalDuration = static_cast<double>(state.lightningGunStartupFrameCount) * LightningGunWinddownFrameDuration;
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            state.weaponAnimMode = WeaponAnimMode::LightningGunStartup;
            state.weaponAnimStartAt = now;
        } else if (now - state.weaponAnimStartAt >= totalDuration) {
            state.weaponAnimMode = WeaponAnimMode::Idle;
        }
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
    if (state.equippedWeapon == WeaponSlot::LightningGun) {
        if (state.weaponAnimMode == WeaponAnimMode::LightningGunStartup) {
            const int step = static_cast<int>((now - state.weaponAnimStartAt) / LightningGunStartupFrameDuration);
            const int index = std::clamp(step, 0, state.lightningGunStartupFrameCount - 1);
            frame = &state.lightningGunStartupFrames[index];
        } else if (state.weaponAnimMode == WeaponAnimMode::LightningGunFiring) {
            frame = &state.lightningGunFiring;
        } else if (state.weaponAnimMode == WeaponAnimMode::LightningGunEquip) {
            frame = &state.lightningGunIdle;
        } else if (state.weaponAnimMode == WeaponAnimMode::LightningGunWinddown) {
            const int step = static_cast<int>((now - state.weaponAnimStartAt) / LightningGunWinddownFrameDuration);
            const int reverseIndex = (state.lightningGunStartupFrameCount - 1) - std::clamp(step, 0, state.lightningGunStartupFrameCount - 1);
            frame = &state.lightningGunStartupFrames[reverseIndex];
        } else {
            frame = &state.lightningGunIdle;
        }
    } else if (state.equippedWeapon == WeaponSlot::Knife) {
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
    const bool lgEquipped = state.equippedWeapon == WeaponSlot::LightningGun;
    const float targetWidth = screenWidth * (knifeEquipped ? 0.47f : (lgEquipped ? 0.54f : 0.50f));
    const float scale = targetWidth / static_cast<float>(frameTex.width);
    const float drawWidth = static_cast<float>(frameTex.width) * scale;
    const float drawHeight = static_cast<float>(frameTex.height) * scale;
    const float bobY = std::sin(state.weaponBobPhase) * (knifeEquipped ? 6.0f : 8.0f);
    const float recoilY = (knifeEquipped || lgEquipped) ? 0.0f : state.recoilOffset;
    float equipSlideY = 0.0f;
    const bool equipSliding = state.weaponAnimMode == WeaponAnimMode::KnifeEquip ||
        state.weaponAnimMode == WeaponAnimMode::ShotgunEquip ||
        state.weaponAnimMode == WeaponAnimMode::LightningGunEquip;
    if (equipSliding) {
        const float t = std::clamp(static_cast<float>((now - state.weaponAnimStartAt) / KnifeEquipSlideDuration), 0.0f, 1.0f);
        equipSlideY = (1.0f - t) * (drawHeight + 80.0f);
    }

    const float baseX = (screenWidth - drawWidth) * 0.5f;
    const float baseY = knifeEquipped ? (screenHeight - drawHeight + 70.0f) : (lgEquipped ? (screenHeight - drawHeight + 88.0f) : (screenHeight - drawHeight + 92.0f));
    const Vector2 pos{
        baseX,
        baseY + bobY + recoilY + equipSlideY
    };

    DrawTextureEx(frameTex, pos, 0.0f, scale, WHITE);
}

void drawLightningBeamVfx(const ClientState& state, double now) {
    const bool lgEquipped = state.equippedWeapon == WeaponSlot::LightningGun;
    const bool active = state.weaponAnimMode == WeaponAnimMode::LightningGunFiring || state.weaponAnimMode == WeaponAnimMode::LightningGunStartup;
    if (!lgEquipped || !active) {
        return;
    }

    const float sw = static_cast<float>(GetScreenWidth());
    const float sh = static_cast<float>(GetScreenHeight());
    const Vector2 start{sw * 0.545f, sh * 0.82f};
    const Vector2 end{sw * 0.5f, sh * 0.5f};

    Vector2 dir{end.x - start.x, end.y - start.y};
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len <= 0.001f) {
        return;
    }
    dir.x /= len;
    dir.y /= len;
    const Vector2 normal{-dir.y, dir.x};

    auto hash01 = [](uint32_t x) -> float {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return static_cast<float>(x & 0x00ffffffU) / 16777215.0f;
    };

    constexpr int maxPoints = 256;
    std::array<Vector2, maxPoints> points{};
    int count = 2;
    points[0] = start;
    points[1] = end;

    // Midpoint-displacement bolt (classic realtime lightning method).
    const int timeSlice = static_cast<int>(now * 45.0);
    float displacement = 30.0f;
    for (int level = 0; level < 5; ++level) {
        std::array<Vector2, maxPoints> next{};
        int nextCount = 0;
        for (int i = 0; i < count - 1 && nextCount < maxPoints - 2; ++i) {
            const Vector2 a = points[i];
            const Vector2 b = points[i + 1];
            next[nextCount++] = a;

            const Vector2 mid{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
            const uint32_t seed = static_cast<uint32_t>((i + 1) * 131 + level * 977 + timeSlice * 43);
            const float j = (hash01(seed) * 2.0f - 1.0f) * displacement;
            next[nextCount++] = {mid.x + normal.x * j, mid.y + normal.y * j};
        }
        next[nextCount++] = points[count - 1];
        points = next;
        count = nextCount;
        displacement *= 0.52f;
    }
    points[count - 1] = end;

    BeginBlendMode(BLEND_ADDITIVE);
    for (int i = 0; i < count - 1; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(std::max(1, count - 1));
        const float widthCore = 7.6f * (1.0f - t * 0.48f);
        const float widthGlow = widthCore * 3.0f;
        DrawLineEx(points[i], points[i + 1], widthGlow, Color{120, 255, 245, 75});
        DrawLineEx(points[i], points[i + 1], widthCore, Color{135, 255, 245, 245});
        DrawLineEx(points[i], points[i + 1], std::max(1.4f, widthCore * 0.32f), Color{235, 255, 250, 205});

        // Secondary branch arcs for a more "lightning" silhouette.
        if ((i % 6) == 2) {
            const Vector2 p = points[i];
            const uint32_t bseed = static_cast<uint32_t>((i + 7) * 193 + timeSlice * 57);
            const float side = hash01(bseed) > 0.5f ? 1.0f : -1.0f;
            const float blen = 10.0f + hash01(bseed + 11) * 18.0f;
            const Vector2 bend{p.x + normal.x * blen * side, p.y + normal.y * blen * side};
            DrawLineEx(p, bend, widthCore * 0.38f, Color{120, 245, 238, 140});
            DrawLineEx(p, bend, std::max(1.0f, widthCore * 0.16f), Color{220, 255, 250, 150});
        }
    }

    DrawCircleV(end, 7.0f, Color{200, 255, 250, 235});
    DrawCircleV(end, 15.0f, Color{115, 250, 242, 135});
    DrawCircleV(end, 25.0f, Color{80, 230, 245, 70});
    EndBlendMode();
}

void drawScoreboardOverlay(const ClientState& state) {
    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();
    const int panelW = static_cast<int>(sw * 0.76f);
    const int panelH = static_cast<int>(sh * 0.68f);
    const int x = (sw - panelW) / 2;
    const int y = static_cast<int>(sh * 0.12f);

    DrawRectangle(x, y, panelW, panelH, Color{8, 12, 20, 220});
    DrawRectangleLinesEx(Rectangle{static_cast<float>(x), static_cast<float>(y), static_cast<float>(panelW), static_cast<float>(panelH)}, 2.0f, Color{180, 194, 215, 230});
    DrawText("Scoreboard", x + 18, y + 14, 34, Color{242, 234, 210, 255});

    const int headerY = y + 62;
    DrawText("Name", x + 26, headerY, 24, Color{224, 228, 236, 255});
    DrawText("Ping", x + panelW - 360, headerY, 24, Color{224, 228, 236, 255});
    DrawText("DMG", x + panelW - 270, headerY, 24, Color{224, 228, 236, 255});
    DrawText("K", x + panelW - 180, headerY, 24, Color{224, 228, 236, 255});
    DrawText("D", x + panelW - 130, headerY, 24, Color{224, 228, 236, 255});
    DrawText("Tm", x + panelW - 82, headerY, 24, Color{224, 228, 236, 255});
    DrawLine(x + 16, headerY + 30, x + panelW - 16, headerY + 30, Color{120, 136, 160, 255});

    std::vector<std::pair<uint32_t, RemotePlayer>> rows;
    rows.reserve(state.players.size());
    for (const auto& entry : state.players) {
        rows.push_back(entry);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        if (a.second.teamId != b.second.teamId) return a.second.teamId < b.second.teamId;
        if (a.second.kills != b.second.kills) return a.second.kills > b.second.kills;
        return a.first < b.first;
    });

    int rowY = headerY + 42;
    for (const auto& [id, p] : rows) {
        const bool local = id == state.localPlayerId;
        const Color rowColor = local ? Color{255, 230, 178, 255} : Color{218, 224, 232, 255};
        const Color dimColor = local ? Color{255, 220, 155, 255} : Color{188, 196, 210, 255};
        const std::string nm = p.name.empty() ? ("Player " + std::to_string(id)) : p.name;
        DrawText(nm.c_str(), x + 26, rowY, 23, rowColor);
        DrawText(TextFormat("%ums", p.pingMs), x + panelW - 360, rowY, 23, dimColor);
        DrawText(TextFormat("%u", p.damageDealt), x + panelW - 270, rowY, 23, dimColor);
        DrawText(TextFormat("%u", p.kills), x + panelW - 180, rowY, 23, dimColor);
        DrawText(TextFormat("%u", p.deaths), x + panelW - 130, rowY, 23, dimColor);
        DrawText(TextFormat("%u", p.teamId), x + panelW - 82, rowY, 23, dimColor);
        rowY += 28;
        if (rowY > y + panelH - 30) {
            break;
        }
    }
}

void render(ClientState& state, double now) {
    const float dt = std::max(GetFrameTime(), 0.0001f);
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
    constexpr bool PreviewIdleModelOnly = false;
    if (PreviewIdleModelOnly) {
        drawIdleEnemyModelPreview(state, dt);
    }
    for (const auto& [id, player] : state.players) {
        if (id == state.localPlayerId) {
            continue;
        }
        if (player.dead) {
            continue;
        }
        if (PreviewIdleModelOnly) {
            continue;
        }
        drawAnimatedRemotePlayer(state, id, player, dt);
        drawRemoteWeaponProxy(player);
    }
    drawRemoteServerLightningBeams(state);

    EndMode3D();
    drawLightningBeamVfx(state, now);
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

    if (state.damageFlash > 0.001f) {
        const unsigned char alpha = static_cast<unsigned char>(std::clamp(state.damageFlash, 0.0f, 1.0f) * 125.0f);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{210, 42, 42, alpha});
    }

    const int cx = GetScreenWidth() / 2;
    const int cy = GetScreenHeight() / 2;
    DrawLine(cx - 8, cy, cx + 8, cy, RED);
    DrawLine(cx, cy - 8, cx, cy + 8, RED);
    const float displaySpeedUnits = state.currentSpeed * 100.0f;
    DrawText(TextFormat("%.0f u/s", displaySpeedUnits), cx + 14, cy + 14, 20, Color{225, 232, 245, 255});

    sanitizeEnemyTune(state);
    if (ShowBottomDebugOverlay) {
        const std::string status = state.connected
            ? "Connected as " + state.localPlayerName + " (#" + std::to_string(state.localPlayerId) + ") | players: " + std::to_string(state.players.size())
            : "Connecting to server...";
        DrawText(status.c_str(), 16, GetScreenHeight() - 58, 20, RAYWHITE);
        if (!state.enemyAnimSetReady) {
            DrawText(state.enemyAnimStatus.c_str(), 16, GetScreenHeight() - 82, 18, Color{255, 130, 130, 255});
        }
        const EnemyClipAsset& idleClipDbg = state.enemyClipAssets[static_cast<size_t>(EnemyAnimClip::Idle)];
        std::string srcLabel = "enemy src: n/a";
        if (!idleClipDbg.sourcePath.empty()) {
            const size_t slash = idleClipDbg.sourcePath.find_last_of("\\/");
            const std::string file = (slash == std::string::npos) ? idleClipDbg.sourcePath : idleClipDbg.sourcePath.substr(slash + 1);
            srcLabel = "enemy src: " + file;
        }
        DrawText(srcLabel.c_str(), 16, GetScreenHeight() - 178, 18, Color{255, 180, 120, 255});
        DrawText(state.enemyModelDebug.c_str(), 16, GetScreenHeight() - 106, 18, Color{190, 225, 255, 255});
        const std::string enemyTuneHud =
            "enemy tune: scale=" + std::to_string(state.enemyTuneScale) +
            " off(" + std::to_string(state.enemyTuneOffsetX) + "," + std::to_string(state.enemyTuneOffsetY) + "," + std::to_string(state.enemyTuneOffsetZ) + ")" +
            " rot(" + std::to_string(state.enemyTuneRotX) + "," + std::to_string(state.enemyTuneRotY) + "," + std::to_string(state.enemyTuneRotZ) + ")";
        DrawText(enemyTuneHud.c_str(), 16, GetScreenHeight() - 130, 18, Color{255, 210, 130, 255});
        DrawText("I/O scale+/- | J/K x-/x+ | N/M y+/y- | L/P z-/z+ | 8/9/0 rot +90 | 7/U rotX-/+ | Y/H rotY-/+ | B/V rotZ-/+ | T reset", 16, GetScreenHeight() - 154, 18, Color{255, 210, 130, 255});
    }

    const std::string t1Clock = formatClock(state.team1TimeLeftSeconds);
    const std::string t2Clock = formatClock(state.team2TimeLeftSeconds);
    const std::string topHud = "TEAM 1  " + t1Clock + "  PTS " + std::to_string(state.team1RoundPoints) +
        "    |    TEAM 2  " + t2Clock + "  PTS " + std::to_string(state.team2RoundPoints);
    const int topW = MeasureText(topHud.c_str(), 30);
    const int topX = (GetScreenWidth() - topW) / 2;
    DrawRectangle(topX - 18, 10, topW + 36, 42, Color{10, 15, 24, 205});
    DrawRectangleLines(topX - 18, 10, topW + 36, 42, Color{190, 204, 224, 220});
    DrawText(topHud.c_str(), topX, 18, 30, Color{255, 241, 206, 255});
    if (!state.serverAnnouncementText.empty() && arena::secondsNow() < state.serverAnnouncementUntil) {
        const int fontSize = 34;
        const int textW = MeasureText(state.serverAnnouncementText.c_str(), fontSize);
        const int x = (GetScreenWidth() - textW) / 2;
        const int y = 64;
        DrawText(state.serverAnnouncementText.c_str(), x + 2, y + 2, fontSize, Color{0, 0, 0, 180});
        DrawText(state.serverAnnouncementText.c_str(), x, y, fontSize, Color{255, 220, 140, 255});
    }
    std::string weaponLabel = "Shotgun";
    if (state.equippedWeapon == WeaponSlot::Knife) weaponLabel = "Karambit";
    if (state.equippedWeapon == WeaponSlot::LightningGun) weaponLabel = "Lightning Gun";
    const std::string ammoText = (state.equippedWeapon == WeaponSlot::Shotgun)
        ? ("Shotgun: " + std::to_string(state.shellsInGun) + "/" + std::to_string(MaxShells) + " | Reserve: " + std::to_string(state.reserveAmmo))
        : ((state.equippedWeapon == WeaponSlot::LightningGun) ? "LG beam active while holding fire" : "Karambit ready");
    DrawText(("Weapon: " + weaponLabel).c_str(), 16, 42, 20, RAYWHITE);
    DrawText(ammoText.c_str(), 16, 68, 20, RAYWHITE);
    const std::string teamText = "Team " + std::to_string(state.localTeamId == 0 ? 1 : state.localTeamId) +
        " | HP: " + std::to_string(state.localHealth);
    DrawText(teamText.c_str(), 16, 94, 20, RAYWHITE);
    DrawText(TextFormat("Ping: %.0f ms", state.smoothedPingMs), 16, 120, 20, RAYWHITE);
    std::string hillText = "Hill: Neutral";
    if (state.matchWinnerTeam != 0) {
        hillText = "Match Over: Team " + std::to_string(state.matchWinnerTeam) + " wins";
    } else if (state.hillWinnerTeam != 0) {
        hillText = "Round Over: Team " + std::to_string(state.hillWinnerTeam) + " wins";
    } else if (state.hillOvertime != 0) {
        hillText = "Hill: OVERTIME";
    } else if (state.hillContested != 0) {
        hillText = "Hill: Contested";
    } else if (state.hillCaptureTeam != 0) {
        hillText = "Hill: Team " + std::to_string(state.hillCaptureTeam) + " capturing " +
            std::to_string(static_cast<int>(state.hillCaptureProgress * 100.0f)) + "%";
    } else if (state.hillOwnerTeam == 1) {
        hillText = "Hill: Owned by Team 1";
    } else if (state.hillOwnerTeam == 2) {
        hillText = "Hill: Owned by Team 2";
    }
    DrawText(hillText.c_str(), 16, 146, 20, Color{245, 235, 170, 255});
    if (state.matchWinnerTeam != 0) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{0, 0, 0, 165});
        const std::string winText = "TEAM " + std::to_string(state.matchWinnerTeam) + " VICTORY";
        const int fontSize = 72;
        const int winW = MeasureText(winText.c_str(), fontSize);
        DrawText(winText.c_str(), (GetScreenWidth() - winW) / 2, GetScreenHeight() / 2 - 56, fontSize, Color{255, 232, 120, 255});
        const std::string resetText = "Next match in " + std::to_string(state.matchResetSecondsLeft) + "s";
        const int resetW = MeasureText(resetText.c_str(), 34);
        DrawText(resetText.c_str(), (GetScreenWidth() - resetW) / 2, GetScreenHeight() / 2 + 28, 34, Color{230, 235, 245, 255});
    }
    if (state.localDead) {
        DrawText("You are dead - respawning...", GetScreenWidth() / 2 - 190, GetScreenHeight() / 2 - 70, 30, RED);
    }
    int feedY = 182;
    for (size_t i = 0; i < state.killFeed.size(); ++i) {
        const KillFeedEntry& entry = state.killFeed[state.killFeed.size() - 1 - i];
        const float t = std::clamp(entry.age / std::max(0.001f, entry.lifetime), 0.0f, 1.0f);
        const unsigned char alpha = static_cast<unsigned char>((1.0f - t) * 255.0f);
        const int fontSize = 26;
        const int textWidth = MeasureText(entry.text.c_str(), fontSize);
        const int x = GetScreenWidth() - textWidth - 24;
        DrawText(entry.text.c_str(), x + 2, feedY + 2, fontSize, Color{0, 0, 0, static_cast<unsigned char>(alpha * 0.7f)});
        DrawText(entry.text.c_str(), x, feedY, fontSize, Color{255, 80, 80, alpha});
        feedY += 30;
        if (feedY > GetScreenHeight() - 80) {
            break;
        }
    }
    if (ShowBottomDebugOverlay) {
        DrawText("WASD move | Mouse look | Space/wheel-down jump | wheel-up forward | A/D double-tap air dash | Shift crouch | 1 shotgun | 2 lightning gun | 3 knife | LMB attack/fire | F inspect | TAB scoreboard | Esc menu", 16, GetScreenHeight() - 32, 18, LIGHTGRAY);
    }
    if (keyDown(KEY_TAB)) {
        drawScoreboardOverlay(state);
    }
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

        SetConfigFlags(FLAG_MSAA_4X_HINT);
        InitWindow(WindowWidth, WindowHeight, "KOTH");
        SetExitKey(KEY_NULL);
        EnableCursor();
        state.mainMenuOpenedAt = GetTime();
        applyDisplayMode(state, state.displayMode);
        initAssets(state);

        double nextHelloAt = 0.0;
        double nextInputAt = 0.0;
        std::cout << "Connecting to " << host << ":" << port << "\n";

        while (!WindowShouldClose()) {
            if (IsKeyPressed(KEY_ENTER) && (keyDown(KEY_LEFT_ALT) || keyDown(KEY_RIGHT_ALT))) {
                const DisplayMode nextMode = (state.displayMode == DisplayMode::Fullscreen)
                    ? DisplayMode::Borderless
                    : DisplayMode::Fullscreen;
                applyDisplayMode(state, nextMode);
            }

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
            const bool ctrlHeld = keyDown(KEY_LEFT_CONTROL) || keyDown(KEY_RIGHT_CONTROL);
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
            if (!ctrlHeld && IsKeyPressed(KEY_ONE)) {
                equipWeapon(state, WeaponSlot::Shotgun, arena::secondsNow());
            }
            if (!ctrlHeld && IsKeyPressed(KEY_TWO)) {
                equipWeapon(state, WeaponSlot::LightningGun, arena::secondsNow());
            }
            if (!ctrlHeld && IsKeyPressed(KEY_THREE)) {
                equipWeapon(state, WeaponSlot::Knife, arena::secondsNow());
            }
            if (IsKeyPressed(KEY_Q)) {
                WeaponSlot target = state.lastEquippedWeapon;
                if (target == state.equippedWeapon) {
                    target = (state.equippedWeapon == WeaponSlot::Knife) ? WeaponSlot::Shotgun : WeaponSlot::Knife;
                }
                equipWeapon(state, target, arena::secondsNow());
            }
            if (IsKeyPressed(KEY_I)) {
                state.enemyTuneScale = std::min(20.0f, state.enemyTuneScale + 0.05f);
            }
            if (IsKeyPressed(KEY_O)) {
                state.enemyTuneScale = std::max(0.01f, state.enemyTuneScale - 0.05f);
            }
            if (IsKeyPressed(KEY_J)) {
                state.enemyTuneOffsetX -= 0.05f;
            }
            if (IsKeyPressed(KEY_K)) {
                state.enemyTuneOffsetX += 0.05f;
            }
            if (IsKeyPressed(KEY_N)) {
                state.enemyTuneOffsetY += 0.05f;
            }
            if (IsKeyPressed(KEY_M)) {
                state.enemyTuneOffsetY -= 0.05f;
            }
            if (IsKeyPressed(KEY_L)) {
                state.enemyTuneOffsetZ -= 0.05f;
            }
            if (IsKeyPressed(KEY_P)) {
                state.enemyTuneOffsetZ += 0.05f;
            }
            if (IsKeyPressed(KEY_EIGHT)) {
                state.enemyTuneRotX = std::fmod(state.enemyTuneRotX + 90.0f, 360.0f);
            }
            if (IsKeyPressed(KEY_NINE)) {
                state.enemyTuneRotY = std::fmod(state.enemyTuneRotY + 90.0f, 360.0f);
            }
            if (IsKeyPressed(KEY_ZERO)) {
                state.enemyTuneRotZ = std::fmod(state.enemyTuneRotZ + 90.0f, 360.0f);
            }
            constexpr float FineRotStep = 2.0f;
            if (IsKeyPressed(KEY_SEVEN)) {
                state.enemyTuneRotX = std::fmod(state.enemyTuneRotX - FineRotStep + 360.0f, 360.0f);
            }
            if (IsKeyPressed(KEY_U)) {
                state.enemyTuneRotX = std::fmod(state.enemyTuneRotX + FineRotStep, 360.0f);
            }
            if (IsKeyPressed(KEY_Y)) {
                state.enemyTuneRotY = std::fmod(state.enemyTuneRotY - FineRotStep + 360.0f, 360.0f);
            }
            if (IsKeyPressed(KEY_H)) {
                state.enemyTuneRotY = std::fmod(state.enemyTuneRotY + FineRotStep, 360.0f);
            }
            if (IsKeyPressed(KEY_B)) {
                state.enemyTuneRotZ = std::fmod(state.enemyTuneRotZ - FineRotStep + 360.0f, 360.0f);
            }
            if (IsKeyPressed(KEY_V)) {
                state.enemyTuneRotZ = std::fmod(state.enemyTuneRotZ + FineRotStep, 360.0f);
            }
            if (IsKeyPressed(KEY_T)) {
                resetEnemyTune(state);
            }
            sanitizeEnemyTune(state);
            if (IsKeyPressed(KEY_ESCAPE)) {
                state.screenMode = ScreenMode::MainMenu;
                if (state.lightningLoopPlaying && state.lightningAudioLoaded) {
                    StopMusicStream(state.lightningFireLoop);
                    state.lightningLoopPlaying = false;
                    state.lightningLoopFadingOut = false;
                    state.lightningLoopVolume = SfxVolumeMaster * SfxVolumeLightningLoop;
                    SetMusicVolume(state.lightningFireLoop, state.lightningLoopVolume);
                }
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
            state.damageFlash = std::max(0.0f, state.damageFlash - dt * 2.8f);
            updateSpeedCounter(state, dt);
            updateDamagePopups(state, dt);
            updateKillFeed(state, dt);
            updateViewmodelAndFootsteps(state, now, dt);
            updateWeaponAnimationState(state, now);
            updateLightningAudio(state);
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
