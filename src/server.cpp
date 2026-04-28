#include "net.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

enum class WeaponSlot : uint8_t {
    Shotgun = 1,
    LightningGun = 2,
    Knife = 3,
};

constexpr float BaseMoveSpeed = 8.9f;
constexpr float KnifeSpeedMultiplier = 1.2f;
constexpr float KnifeJumpMultiplier = 1.03f;
constexpr float SvAccelerate = 12.0f;
constexpr float SvAirAccelerate = 5.0f;
constexpr float SvFriction = 7.0f;
constexpr float SvStopSpeed = 8.0f;
constexpr float SvAirSpeedCap = 2.0f;
constexpr float MaxBhopSpeedFactor = 1.45f;
constexpr float AirDashImpulse = 18.5f;
constexpr float AirDashMaxSpeed = 26.0f;
constexpr float AirDashBoostDuration = 0.075f;
constexpr float AirDashBoostSpeedFactor = 2.2f;
constexpr float GravityScale = 1.36f;
constexpr float JumpVelocityScale = 0.82f;
constexpr float JumpStrafeAssist = 1.2f;
constexpr float JumpBufferSeconds = 0.18f;
constexpr int MaxHealth = 100;
constexpr float ShotgunRange = 36.0f;
constexpr int ShotgunDamage = 34;
constexpr float ShotgunFireInterval = 0.24f;
constexpr float KnifeRange = 2.2f;
constexpr int KnifeDamage = 45;
constexpr float KnifeFireInterval = 0.35f;
constexpr float LightningGunRange = 46.0f;
constexpr int LightningGunDamagePerTick = 7;
constexpr float LightningGunTickInterval = 0.05f;
constexpr float RespawnDelaySeconds = 3.0f;
constexpr float HillRoundTimeSeconds = 180.0f;
constexpr uint8_t MatchPointTarget = 3;
constexpr double RoundIntermissionSeconds = 2.5;
constexpr double MatchVictoryScreenSeconds = 6.0;

struct StaticSolid {
    arena::Vec3 center{};
    arena::Vec3 size{};
    bool isRamp = false;
};

struct ServerMap {
    uint16_t width = 0;
    uint16_t height = 0;
    float cellSize = 4.0f;
    float originX = 0.0f;
    float originZ = 0.0f;
    std::array<char, arena::MapMaxWidth * arena::MapMaxHeight> cells{};
    std::vector<StaticSolid> solids;
};

ServerMap& worldMap() {
    static ServerMap map{};
    static bool initialized = false;
    if (initialized) {
        return map;
    }
    initialized = true;
    // Map legend:
    // '.' = empty
    // 'B' = big sight-blocking solid
    // 'R' = low solid that can be stood on and shot over, but still blocks movement and sight at lower heights
    // 'C' = capture point center (not treated specially by server, just for map authoring reference)
    // kill myself
    static const std::array<const char*, 13> ascii = {{
        ".............",
        ".............",
        ".....BBB.....",
        ".............",
        ".............",
        ".............",
        "......C....a.",
        ".............",
        ".............",
        "...B..R..B...",
        ".............",
        ".............",
        ".............",
    }};

    map.width = static_cast<uint16_t>(ascii[0] ? std::char_traits<char>::length(ascii[0]) : 0);
    map.height = static_cast<uint16_t>(ascii.size());
    map.cellSize = 4.0f;
    map.originX = -0.5f * static_cast<float>(map.width - 1) * map.cellSize;
    map.originZ = -0.5f * static_cast<float>(map.height - 1) * map.cellSize;
    map.cells.fill('.');

    for (uint16_t z = 0; z < map.height; ++z) {
        for (uint16_t x = 0; x < map.width; ++x) {
            const char symbol = ascii[z][x];
            map.cells[z * arena::MapMaxWidth + x] = symbol;
            float solidHeight = 0.0f;
            switch (symbol) {
            case 'B': solidHeight = 5.0f; break;   // Big sight blocker
            case 'R': solidHeight = 2.0f; break;   // Ramp/low block
            default: break;
            }
            if (solidHeight <= 0.0f) {
                continue;
            }
            const float cx = map.originX + static_cast<float>(x) * map.cellSize;
            const float cz = map.originZ + static_cast<float>(z) * map.cellSize;
            map.solids.push_back({{cx, solidHeight * 0.5f, cz}, {map.cellSize * 0.95f, solidHeight, map.cellSize * 0.95f}, symbol == 'R'});
        }
    }
    return map;
}

bool horizontalCircleOverlapsBox(const arena::Vec3& pos, float radius, const StaticSolid& s) {
    const float minX = s.center.x - s.size.x * 0.5f;
    const float maxX = s.center.x + s.size.x * 0.5f;
    const float minZ = s.center.z - s.size.z * 0.5f;
    const float maxZ = s.center.z + s.size.z * 0.5f;
    const float closestX = arena::clamp(pos.x, minX, maxX);
    const float closestZ = arena::clamp(pos.z, minZ, maxZ);
    const float dx = pos.x - closestX;
    const float dz = pos.z - closestZ;
    return (dx * dx + dz * dz) <= (radius * radius);
}

float rampSurfaceYAt(const StaticSolid& s, float x, float z) {
    const float minX = s.center.x - s.size.x * 0.5f;
    const float maxX = s.center.x + s.size.x * 0.5f;
    const float minZ = s.center.z - s.size.z * 0.5f;
    const float maxZ = s.center.z + s.size.z * 0.5f;
    const float clampedX = arena::clamp(x, minX, maxX);
    const float clampedZ = arena::clamp(z, minZ, maxZ);
    (void)clampedZ; // Keeps interface generic if ramp orientation changes later.

    const float t = (maxX > minX) ? (clampedX - minX) / (maxX - minX) : 0.0f;
    const float minY = s.center.y - s.size.y * 0.5f;
    const float maxY = s.center.y + s.size.y * 0.5f;
    return minY + t * (maxY - minY);
}

void resolveHorizontalMapCollisions(arena::Vec3& position, arena::Vec3& velocity, float currentHeight) {
    const float radius = arena::PlayerRadius;
    const float playerBottom = position.y;
    const float playerTop = playerBottom + currentHeight;

    for (int iter = 0; iter < 3; ++iter) {
        bool resolvedAny = false;
        for (const StaticSolid& s : worldMap().solids) {
            const float minY = s.center.y - s.size.y * 0.5f;
            const float maxY = s.center.y + s.size.y * 0.5f;
            if (s.isRamp) {
                const float rampY = rampSurfaceYAt(s, position.x, position.z);
                if (playerBottom >= rampY - 0.01f) {
                    continue;
                }
            }
            if (playerTop <= minY || playerBottom >= maxY) {
                continue;
            }

            const float minX = s.center.x - s.size.x * 0.5f;
            const float maxX = s.center.x + s.size.x * 0.5f;
            const float minZ = s.center.z - s.size.z * 0.5f;
            const float maxZ = s.center.z + s.size.z * 0.5f;

            const float playerMinX = position.x - radius;
            const float playerMaxX = position.x + radius;
            const float playerMinZ = position.z - radius;
            const float playerMaxZ = position.z + radius;

            if (playerMaxX <= minX || playerMinX >= maxX || playerMaxZ <= minZ || playerMinZ >= maxZ) {
                continue;
            }

            const float penLeft = playerMaxX - minX;
            const float penRight = maxX - playerMinX;
            const float penX = std::min(penLeft, penRight);
            const float penBack = playerMaxZ - minZ;
            const float penFront = maxZ - playerMinZ;
            const float penZ = std::min(penBack, penFront);

            if (penX < penZ) {
                const float dir = (position.x >= s.center.x) ? 1.0f : -1.0f;
                position.x += dir * (penX + 0.001f);
                velocity.x = 0.0f;
            } else {
                const float dir = (position.z >= s.center.z) ? 1.0f : -1.0f;
                position.z += dir * (penZ + 0.001f);
                velocity.z = 0.0f;
            }
            resolvedAny = true;
        }
        if (!resolvedAny) {
            break;
        }
    }
}

bool resolveVerticalMapCollisions(arena::Vec3& position, float& velocityY, float prevY, float currentHeight) {
    bool landed = false;
    const float radius = arena::PlayerRadius;
    for (const StaticSolid& s : worldMap().solids) {
        if (!horizontalCircleOverlapsBox(position, radius, s)) {
            continue;
        }
        if (s.isRamp) {
            const float rampY = rampSurfaceYAt(s, position.x, position.z);
            if (velocityY <= 0.0f && prevY >= rampY && position.y <= rampY) {
                position.y = rampY;
                velocityY = 0.0f;
                landed = true;
            }
            continue;
        }
        const float minY = s.center.y - s.size.y * 0.5f;
        const float maxY = s.center.y + s.size.y * 0.5f;

        if (velocityY <= 0.0f && prevY >= maxY && position.y <= maxY) {
            position.y = maxY;
            velocityY = 0.0f;
            landed = true;
            continue;
        }

        const float prevHead = prevY + currentHeight;
        const float currentHead = position.y + currentHeight;
        if (velocityY > 0.0f && prevHead <= minY && currentHead >= minY) {
            position.y = minY - currentHeight;
            velocityY = 0.0f;
        }
    }
    return landed;
}

struct Player {
    uint32_t id = 0;
    sockaddr_in address{};
    std::string name = "Player";
    arena::Vec3 position{};
    arena::Vec3 velocity{};
    float velocityY = 0.0f;
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool grounded = true;
    bool crouched = false;
    bool wasForwardHeld = false;
    bool jumpedSinceGround = false;
    int airDashCharges = 0;
    double dashBoostUntil = 0.0;
    double jumpBufferedUntil = 0.0;
    uint8_t teamId = 1;
    WeaponSlot equippedWeapon = WeaponSlot::Knife;
    float airborneSpeedMultiplier = 1.0f;
    int health = MaxHealth;
    bool dead = false;
    uint16_t hitConfirmCount = 0;
    uint8_t lastDamageDealt = 0;
    uint32_t lastHitTargetId = 0;
    double respawnAt = 0.0;
    double nextFireAt = 0.0;
    double fireVisualUntil = 0.0;
    double lgBeamUntil = 0.0;
    arena::Vec3 lgBeamEnd{};
    double lastHeardAt = 0.0;
    bool mapSent = false;
    uint32_t lastAckedServerTick = 0;
    uint16_t pingMs = 0;
    uint16_t kills = 0;
    uint16_t deaths = 0;
    uint32_t damageDealt = 0;
};

struct HillState {
    arena::Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 8.0f;
    float team1TimeLeft = HillRoundTimeSeconds;
    float team2TimeLeft = HillRoundTimeSeconds;
    uint8_t ownerTeam = 0;
    uint8_t captureTeam = 0;
    uint8_t contested = 0;
    uint8_t overtime = 0;
    uint8_t winnerTeam = 0;
    uint8_t overtimeCheckTeam = 0;
    uint8_t team1RoundPoints = 0;
    uint8_t team2RoundPoints = 0;
    uint8_t matchWinnerTeam = 0;
    float captureProgress = 0.0f;
    double roundResetAt = 0.0;
    double matchResetAt = 0.0;
    double lastUpdateAt = 0.0;
};

void spawnPlayerOnTeam(Player& player, int teamSlot) {
    if (player.teamId == 1) {
        const float z = -12.0f + static_cast<float>(teamSlot) * 4.0f;
        player.position = {-16.0f, 0.0f, z};
    } else {
        const float z = -12.0f + static_cast<float>(teamSlot) * 4.0f;
        player.position = {16.0f, 0.0f, z};
    }
}

void resetPlayerForSpawn(Player& player, double now, int teamSlot) {
    player.dead = false;
    player.health = MaxHealth;
    player.velocity = {};
    player.velocityY = 0.0f;
    player.nextFireAt = now + 0.2;
    player.wasForwardHeld = false;
    player.airborneSpeedMultiplier = 1.0f;
    player.jumpedSinceGround = false;
    player.airDashCharges = 0;
    player.dashBoostUntil = 0.0;
    player.lgBeamUntil = 0.0;
    player.lgBeamEnd = {};
    player.grounded = true;
    player.crouched = false;
    player.respawnAt = 0.0;
    spawnPlayerOnTeam(player, teamSlot);
}

void resetAllPlayersForNewRound(std::vector<Player>& players, double now) {
    int team1Slot = 0;
    int team2Slot = 0;
    for (Player& player : players) {
        if (player.teamId == 1) {
            resetPlayerForSpawn(player, now, team1Slot++);
        } else {
            resetPlayerForSpawn(player, now, team2Slot++);
        }
    }
}

void resetHillForNewRound(HillState& hill, double now) {
    hill.team1TimeLeft = HillRoundTimeSeconds;
    hill.team2TimeLeft = HillRoundTimeSeconds;
    hill.ownerTeam = 0;
    hill.captureTeam = 0;
    hill.contested = 0;
    hill.overtime = 0;
    hill.winnerTeam = 0;
    hill.overtimeCheckTeam = 0;
    hill.captureProgress = 0.0f;
    hill.roundResetAt = 0.0;
    hill.lastUpdateAt = now;
}

void resetMatchState(HillState& hill, std::vector<Player>& players, double now) {
    hill.team1RoundPoints = 0;
    hill.team2RoundPoints = 0;
    hill.matchWinnerTeam = 0;
    hill.matchResetAt = 0.0;
    resetHillForNewRound(hill, now);
    resetAllPlayersForNewRound(players, now);
}

float horizontalSpeed(const arena::Vec3& v) {
    return std::sqrt(v.x * v.x + v.z * v.z);
}

void applyGroundFriction(Player& player, float dt) {
    const float speed = horizontalSpeed(player.velocity);
    if (speed <= 0.0001f) {
        player.velocity.x = 0.0f;
        player.velocity.z = 0.0f;
        return;
    }

    const float control = std::max(speed, SvStopSpeed);
    const float drop = control * SvFriction * dt;
    const float newSpeed = std::max(0.0f, speed - drop);
    if (newSpeed == 0.0f) {
        player.velocity.x = 0.0f;
        player.velocity.z = 0.0f;
        return;
    }

    const float scale = newSpeed / speed;
    player.velocity.x *= scale;
    player.velocity.z *= scale;
}

void accelerate(Player& player, arena::Vec3 wishDir, float wishSpeed, float accel, float dt) {
    if (wishSpeed <= 0.0f) {
        return;
    }
    const arena::Vec3 horizontalVel{player.velocity.x, 0.0f, player.velocity.z};
    const float currentSpeed = arena::dot(horizontalVel, wishDir);
    const float addSpeed = wishSpeed - currentSpeed;
    if (addSpeed <= 0.0f) {
        return;
    }

    float accelSpeed = accel * wishSpeed * dt;
    if (accelSpeed > addSpeed) {
        accelSpeed = addSpeed;
    }

    player.velocity.x += wishDir.x * accelSpeed;
    player.velocity.z += wishDir.z * accelSpeed;
}

void airAccelerate(Player& player, arena::Vec3 wishDir, float wishSpeed, float accel, float dt) {
    if (wishSpeed <= 0.0f) {
        return;
    }
    const float wishSpd = std::min(wishSpeed, SvAirSpeedCap);
    const arena::Vec3 horizontalVel{player.velocity.x, 0.0f, player.velocity.z};
    const float currentSpeed = arena::dot(horizontalVel, wishDir);
    const float addSpeed = wishSpd - currentSpeed;
    if (addSpeed <= 0.0f) {
        return;
    }
    // Source behavior: acceleration scales by full wishspeed, not capped wishSpd.
    float accelSpeed = accel * wishSpeed * dt;
    if (accelSpeed > addSpeed) {
        accelSpeed = addSpeed;
    }
    player.velocity.x += wishDir.x * accelSpeed;
    player.velocity.z += wishDir.z * accelSpeed;
}

void capHorizontalVelocity(Player& player, float maxSpeed) {
    if (maxSpeed <= 0.0f) {
        player.velocity.x = 0.0f;
        player.velocity.z = 0.0f;
        return;
    }
    const float speed = horizontalSpeed(player.velocity);
    if (speed <= maxSpeed || speed <= 0.0001f) {
        return;
    }
    const float scale = maxSpeed / speed;
    player.velocity.x *= scale;
    player.velocity.z *= scale;
}

void applyTapStrafeRedirect(Player& player, const arena::Vec3& forward, float weaponSpeedScale) {
    const arena::Vec3 forwardFlat = arena::normalize(arena::Vec3{forward.x, 0.0f, forward.z});
    if (arena::length(forwardFlat) <= 0.0001f) {
        return;
    }

    const arena::Vec3 velFlat{player.velocity.x, 0.0f, player.velocity.z};
    const float speed = arena::length(velFlat);
    if (speed <= 0.1f) {
        return;
    }

    const arena::Vec3 velDir = arena::normalize(velFlat);
    constexpr float RedirectStrength = 0.78f;
    arena::Vec3 turned = arena::normalize(velDir * (1.0f - RedirectStrength) + forwardFlat * RedirectStrength);
    if (arena::length(turned) <= 0.0001f) {
        turned = forwardFlat;
    }

    const float redirectSpeed = speed * 1.015f;
    const float redirectCap = BaseMoveSpeed * weaponSpeedScale * MaxBhopSpeedFactor;
    const float finalSpeed = std::min(redirectSpeed, redirectCap);
    player.velocity.x = turned.x * finalSpeed;
    player.velocity.z = turned.z * finalSpeed;
}

void applyAirDash(Player& player, const arena::Vec3& right, const arena::InputPacket& input, double now) {
    if (input.dashPressed == 0 || player.grounded || !player.jumpedSinceGround || player.airDashCharges <= 0) {
        return;
    }

    arena::Vec3 dashWish = right * static_cast<float>(input.dashMoveX);
    dashWish = arena::normalize(dashWish);
    if (arena::length(dashWish) <= 0.0001f) {
        return;
    }

    player.velocity.x += dashWish.x * AirDashImpulse;
    player.velocity.z += dashWish.z * AirDashImpulse;
    const float speed = horizontalSpeed(player.velocity);
    if (speed > AirDashMaxSpeed) {
        const float s = AirDashMaxSpeed / speed;
        player.velocity.x *= s;
        player.velocity.z *= s;
    }
    player.airDashCharges--;
    player.dashBoostUntil = now + AirDashBoostDuration;
}

Player* findPlayer(std::vector<Player>& players, const sockaddr_in& address) {
    for (auto& player : players) {
        if (arena::sameAddress(player.address, address)) {
            return &player;
        }
    }
    return nullptr;
}

std::string trimName(const std::string& raw) {
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

bool isNameTaken(const std::vector<Player>& players, const std::string& candidate) {
    for (const Player& p : players) {
        if (p.name == candidate) {
            return true;
        }
    }
    return false;
}

std::string uniqueJoinName(const std::vector<Player>& players, const std::string& requestedRaw) {
    const std::string base = trimName(requestedRaw);
    if (!isNameTaken(players, base)) {
        return base;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
        std::string candidate = base + " " + std::to_string(suffix);
        if (candidate.size() >= static_cast<size_t>(arena::MaxPlayerNameChars)) {
            const std::string suffixText = " " + std::to_string(suffix);
            size_t maxBaseLen = static_cast<size_t>(arena::MaxPlayerNameChars - 1);
            if (suffixText.size() < maxBaseLen) {
                maxBaseLen -= suffixText.size();
            } else {
                maxBaseLen = 1;
            }
            candidate = base.substr(0, maxBaseLen) + suffixText;
        }
        if (!isNameTaken(players, candidate)) {
            return candidate;
        }
    }
    return base;
}

void sendWelcome(SOCKET socket, const Player& player) {
    arena::WelcomePacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Welcome);
    packet.playerId = player.id;
    std::memset(packet.assignedName, 0, sizeof(packet.assignedName));
    std::memcpy(packet.assignedName, player.name.c_str(), std::min(player.name.size(), sizeof(packet.assignedName) - 1));

    sendto(
        socket,
        reinterpret_cast<const char*>(&packet),
        sizeof(packet),
        0,
        reinterpret_cast<const sockaddr*>(&player.address),
        sizeof(player.address));
}

void sendMapData(SOCKET socket, const Player& player) {
    const ServerMap& map = worldMap();
    arena::MapDataPacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::MapData);
    packet.width = map.width;
    packet.height = map.height;
    packet.cellSize = map.cellSize;
    packet.originX = map.originX;
    packet.originZ = map.originZ;
    std::memcpy(packet.cells, map.cells.data(), sizeof(packet.cells));

    sendto(
        socket,
        reinterpret_cast<const char*>(&packet),
        sizeof(packet),
        0,
        reinterpret_cast<const sockaddr*>(&player.address),
        sizeof(player.address));
}

void integrateInput(Player& player, const arena::InputPacket& input, double now) {
    if (player.dead) {
        return;
    }

    WeaponSlot inputWeapon = WeaponSlot::Knife;
    if (input.weaponSlot == static_cast<uint8_t>(WeaponSlot::Shotgun)) {
        inputWeapon = WeaponSlot::Shotgun;
    } else if (input.weaponSlot == static_cast<uint8_t>(WeaponSlot::LightningGun)) {
        inputWeapon = WeaponSlot::LightningGun;
    }
    player.equippedWeapon = inputWeapon;
    player.yaw = input.yaw;
    player.pitch = arena::clamp(input.pitch, -arena::MaxLookPitch, arena::MaxLookPitch);

    const float sinYaw = static_cast<float>(std::sin(player.yaw));
    const float cosYaw = static_cast<float>(std::cos(player.yaw));
    const arena::Vec3 forward{sinYaw, 0.0f, -cosYaw};
    const arena::Vec3 right{cosYaw, 0.0f, sinYaw};

    const bool wantsCrouch = input.crouchHeld != 0;
    if (wantsCrouch) {
        player.crouched = true;
    } else if (player.crouched) {
        const float standingCeiling = arena::RoomHeight - arena::PlayerHeight;
        if (player.position.y <= standingCeiling + 0.001f) {
            player.crouched = false;
        }
    }

    const float currentHeight = player.crouched ? arena::CrouchHeight : arena::PlayerHeight;
    arena::Vec3 wish = right * input.moveX + forward * input.moveZ;
    wish = arena::normalize(wish);
    const bool forwardHeld = input.moveZ > 0.1f;

    if (input.jumpPressed != 0) {
        player.jumpBufferedUntil = now + JumpBufferSeconds;
    }
    const bool willJump = player.grounded && now <= player.jumpBufferedUntil;
    if (willJump) {
        const float jumpScale = (player.equippedWeapon == WeaponSlot::Knife) ? KnifeJumpMultiplier : 1.0f;
        player.velocityY = arena::JumpVelocity * jumpScale * JumpVelocityScale;
        // Preserve directional intent on takeoff, including diagonals.
        if (arena::length(wish) > 0.0001f) {
            player.velocity.x += wish.x * JumpStrafeAssist;
            player.velocity.z += wish.z * JumpStrafeAssist;
        }
        player.airborneSpeedMultiplier = (player.equippedWeapon == WeaponSlot::Knife) ? KnifeSpeedMultiplier : 1.0f;
        player.jumpedSinceGround = true;
        player.airDashCharges = 1;
        player.jumpBufferedUntil = 0.0;
        player.grounded = false;
    }

    const float weaponSpeedScale = player.grounded
        ? ((player.equippedWeapon == WeaponSlot::Knife) ? KnifeSpeedMultiplier : 1.0f)
        : player.airborneSpeedMultiplier;
    const float baseSpeed = BaseMoveSpeed * weaponSpeedScale;
    const float moveSpeed = player.crouched ? baseSpeed * arena::CrouchSpeedScale : baseSpeed;

    if (player.grounded && !willJump) {
        applyGroundFriction(player, arena::TickSeconds);
        accelerate(player, wish, moveSpeed, SvAccelerate, arena::TickSeconds);
    } else {
        airAccelerate(player, wish, moveSpeed, SvAirAccelerate, arena::TickSeconds);
        // Apex-like tap strafe redirect: trigger on fresh forward input in air (W or wheel-up).
        if (!player.grounded && !willJump && forwardHeld && !player.wasForwardHeld) {
            applyTapStrafeRedirect(player, forward, weaponSpeedScale);
        }
    }
    applyAirDash(player, right, input, now);
    float speedCap = moveSpeed * MaxBhopSpeedFactor;
    if (now < player.dashBoostUntil) {
        speedCap = std::max(speedCap, moveSpeed * AirDashBoostSpeedFactor);
    }
    capHorizontalVelocity(player, speedCap);
    player.wasForwardHeld = forwardHeld;

    const float prevY = player.position.y;
    player.velocityY -= arena::Gravity * GravityScale * arena::TickSeconds;
    player.position.x += player.velocity.x * arena::TickSeconds;
    player.position.z += player.velocity.z * arena::TickSeconds;
    player.position.y += player.velocityY * arena::TickSeconds;

    resolveHorizontalMapCollisions(player.position, player.velocity, currentHeight);
    const bool landedOnSolid = resolveVerticalMapCollisions(player.position, player.velocityY, prevY, currentHeight);
    if (landedOnSolid) {
        player.grounded = true;
        player.airborneSpeedMultiplier = 1.0f;
        player.wasForwardHeld = false;
        player.jumpedSinceGround = false;
        player.airDashCharges = 0;
        player.dashBoostUntil = 0.0;
    }

    if (player.position.y <= 0.0f) {
        player.position.y = 0.0f;
        player.velocityY = 0.0f;
        player.grounded = true;
        player.airborneSpeedMultiplier = 1.0f;
        player.wasForwardHeld = false;
        player.jumpedSinceGround = false;
        player.airDashCharges = 0;
        player.dashBoostUntil = 0.0;
    } else if (player.velocityY != 0.0f) {
        player.grounded = false;
    }

    const float ceilingLimit = arena::RoomHeight - currentHeight;
    if (player.position.y > ceilingLimit) {
        player.position.y = ceilingLimit;
        if (player.velocityY > 0.0f) {
            player.velocityY = 0.0f;
        }
    }

    player.position.x = arena::clamp(player.position.x, -arena::RoomHalfSize + arena::PlayerRadius, arena::RoomHalfSize - arena::PlayerRadius);
    player.position.z = arena::clamp(player.position.z, -arena::RoomHalfSize + arena::PlayerRadius, arena::RoomHalfSize - arena::PlayerRadius);
    if (player.position.x <= -arena::RoomHalfSize + arena::PlayerRadius || player.position.x >= arena::RoomHalfSize - arena::PlayerRadius) {
        player.velocity.x = 0.0f;
    }
    if (player.position.z <= -arena::RoomHalfSize + arena::PlayerRadius || player.position.z >= arena::RoomHalfSize - arena::PlayerRadius) {
        player.velocity.z = 0.0f;
    }
}

arena::Vec3 viewForward(float yaw, float pitch) {
    return arena::normalize({
        static_cast<float>(std::sin(yaw)) * static_cast<float>(std::cos(pitch)),
        static_cast<float>(std::sin(pitch)),
        -static_cast<float>(std::cos(yaw)) * static_cast<float>(std::cos(pitch)),
    });
}

float eyeHeightForPlayer(const Player& player) {
    return player.crouched ? arena::CrouchEyeHeight : arena::StandEyeHeight;
}

float currentHeightForPlayer(const Player& player) {
    return player.crouched ? arena::CrouchHeight : arena::PlayerHeight;
}

bool rayHitsSphere(const arena::Vec3& origin, const arena::Vec3& dir, float maxDistance, const arena::Vec3& center, float radius) {
    const arena::Vec3 toCenter = center - origin;
    const float t = arena::dot(toCenter, dir);
    if (t < 0.0f || t > maxDistance) {
        return false;
    }
    const arena::Vec3 closest = origin + dir * t;
    const arena::Vec3 delta = center - closest;
    return arena::dot(delta, delta) <= radius * radius;
}

bool rayHitsPlayerVolumes(const arena::Vec3& origin, const arena::Vec3& dir, float maxDistance, const Player& target) {
    const float h = currentHeightForPlayer(target);
    const float bodyRadius = arena::PlayerRadius + 0.14f;
    const float headRadius = arena::PlayerRadius + 0.10f;
    const arena::Vec3 bodyCenter{target.position.x, target.position.y + h * 0.46f, target.position.z};
    const arena::Vec3 headCenter{target.position.x, target.position.y + h - headRadius * 1.05f, target.position.z};
    return rayHitsSphere(origin, dir, maxDistance, bodyCenter, bodyRadius) ||
        rayHitsSphere(origin, dir, maxDistance, headCenter, headRadius);
}

bool rayIntersectsAabb(const arena::Vec3& origin, const arena::Vec3& dir, float maxDistance, const StaticSolid& s, float& outT) {
    const float minX = s.center.x - s.size.x * 0.5f;
    const float maxX = s.center.x + s.size.x * 0.5f;
    const float minY = s.center.y - s.size.y * 0.5f;
    const float maxY = s.center.y + s.size.y * 0.5f;
    const float minZ = s.center.z - s.size.z * 0.5f;
    const float maxZ = s.center.z + s.size.z * 0.5f;

    float tMin = 0.0f;
    float tMax = maxDistance;
    auto slab = [&](float o, float d, float mn, float mx) -> bool {
        if (std::abs(d) < 0.00001f) {
            return o >= mn && o <= mx;
        }
        const float invD = 1.0f / d;
        float t1 = (mn - o) * invD;
        float t2 = (mx - o) * invD;
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        return tMax >= tMin;
    };

    if (!slab(origin.x, dir.x, minX, maxX)) return false;
    if (!slab(origin.y, dir.y, minY, maxY)) return false;
    if (!slab(origin.z, dir.z, minZ, maxZ)) return false;

    outT = tMin;
    return outT >= 0.0f && outT <= maxDistance;
}

float firstWorldBlockerDistance(const arena::Vec3& origin, const arena::Vec3& dir, float maxDistance) {
    float best = maxDistance + 1.0f;
    for (const StaticSolid& s : worldMap().solids) {
        float t = 0.0f;
        if (rayIntersectsAabb(origin, dir, maxDistance, s, t) && t < best) {
            best = t;
        }
    }
    return best;
}

bool processCombatInput(Player& attacker, std::vector<Player>& players, const arena::InputPacket& input, double now) {
    if (attacker.dead) {
        return false;
    }

    bool wantsAttack = false;
    if (attacker.equippedWeapon == WeaponSlot::LightningGun) {
        wantsAttack = input.fireHeld != 0;
    } else {
        wantsAttack = input.firePressed != 0;
    }

    if (!wantsAttack) {
        return false;
    }
    if (attacker.equippedWeapon == WeaponSlot::LightningGun) {
        const arena::Vec3 lgOrigin{
            attacker.position.x,
            attacker.position.y + eyeHeightForPlayer(attacker),
            attacker.position.z
        };
        const arena::Vec3 lgDir = viewForward(attacker.yaw, attacker.pitch);
        attacker.fireVisualUntil = now + 0.10;
        attacker.lgBeamUntil = now + 0.10;
        attacker.lgBeamEnd = lgOrigin + lgDir * LightningGunRange;
    }
    if (now < attacker.nextFireAt) {
        return false;
    }

    const arena::Vec3 origin{
        attacker.position.x,
        attacker.position.y + eyeHeightForPlayer(attacker),
        attacker.position.z
    };
    const arena::Vec3 dir = viewForward(attacker.yaw, attacker.pitch);

    float range = ShotgunRange;
    int damage = ShotgunDamage;
    if (attacker.equippedWeapon == WeaponSlot::Knife) {
        range = KnifeRange;
        damage = KnifeDamage;
        attacker.nextFireAt = now + KnifeFireInterval;
        attacker.fireVisualUntil = now + 0.08;
    } else if (attacker.equippedWeapon == WeaponSlot::LightningGun) {
        range = LightningGunRange;
        damage = LightningGunDamagePerTick;
        attacker.nextFireAt = now + LightningGunTickInterval;
        attacker.fireVisualUntil = now + 0.10;
    } else {
        attacker.nextFireAt = now + ShotgunFireInterval;
        attacker.fireVisualUntil = now + 0.10;
    }

    Player* bestTarget = nullptr;
    float bestDistance = range;
    for (Player& target : players) {
        if (target.id == attacker.id || target.dead || target.teamId == attacker.teamId) {
            continue;
        }
        if (rayHitsPlayerVolumes(origin, dir, range, target)) {
            const arena::Vec3 toTarget = target.position - attacker.position;
            const float dist = arena::length(toTarget);
            if (dist < bestDistance) {
                bestDistance = dist;
                bestTarget = &target;
            }
        }
    }

    if (bestTarget == nullptr) {
        return true;
    }

    const float worldBlockDistance = firstWorldBlockerDistance(origin, dir, bestDistance);
    if (worldBlockDistance <= bestDistance - 0.02f) {
        return true;
    }

    const int previousHealth = bestTarget->health;
    bestTarget->health = std::max(0, bestTarget->health - damage);
    const int damageApplied = std::max(0, previousHealth - bestTarget->health);
    attacker.hitConfirmCount++;
    attacker.lastDamageDealt = static_cast<uint8_t>(std::clamp(damageApplied, 0, 255));
    attacker.lastHitTargetId = bestTarget->id;
    attacker.damageDealt += static_cast<uint32_t>(damageApplied);
    if (bestTarget->health == 0) {
        bestTarget->dead = true;
        bestTarget->respawnAt = now + RespawnDelaySeconds;
        bestTarget->velocity = {};
        bestTarget->velocityY = 0.0f;
        attacker.kills = static_cast<uint16_t>(std::min<int>(65535, attacker.kills + 1));
        bestTarget->deaths = static_cast<uint16_t>(std::min<int>(65535, bestTarget->deaths + 1));
    }
    return true;
}

void processRespawns(std::vector<Player>& players, double now) {
    int team1SpawnIndex = 0;
    int team2SpawnIndex = 0;

    for (Player& player : players) {
        if (!player.dead || now < player.respawnAt) {
            continue;
        }
        if (player.teamId == 1) {
            resetPlayerForSpawn(player, now, team1SpawnIndex++);
        } else {
            resetPlayerForSpawn(player, now, team2SpawnIndex++);
        }
    }
}

void updateHillState(HillState& hill, const std::vector<Player>& players, double now) {
    if (hill.lastUpdateAt <= 0.0) {
        hill.lastUpdateAt = now;
    }
    const float frameDt = static_cast<float>(std::max(0.0, now - hill.lastUpdateAt));
    hill.lastUpdateAt = now;
    if (hill.matchWinnerTeam != 0 || hill.winnerTeam != 0) {
        return;
    }

    int team1OnHill = 0;
    int team2OnHill = 0;

    const float radiusSq = hill.radius * hill.radius;
    for (const Player& player : players) {
        if (player.dead) {
            continue;
        }
        const float dx = player.position.x - hill.center.x;
        const float dz = player.position.z - hill.center.z;
        const float distSq = dx * dx + dz * dz;
        if (distSq > radiusSq) {
            continue;
        }
        if (player.teamId == 1) {
            team1OnHill++;
        } else if (player.teamId == 2) {
            team2OnHill++;
        }
    }

    uint8_t capturingTeam = 0;
    hill.contested = (team1OnHill > 0 && team2OnHill > 0) ? 1 : 0;
    if (team1OnHill > 0 && team2OnHill == 0) {
        capturingTeam = 1;
    } else if (team2OnHill > 0 && team1OnHill == 0) {
        capturingTeam = 2;
    }

    constexpr float captureTimeSeconds = 8.0f;
    const float captureStep = (frameDt > 0.0f) ? (frameDt / captureTimeSeconds) : 0.0f;
    const float neutralDecayStep = captureStep * 0.5f;

    if (hill.contested != 0) {
        // Contested point: freeze progress exactly where it is.
    } else if (capturingTeam == 0) {
        if (hill.ownerTeam == 0 && hill.captureProgress > 0.0f) {
            hill.captureProgress = std::max(0.0f, hill.captureProgress - neutralDecayStep);
            if (hill.captureProgress == 0.0f) {
                hill.captureTeam = 0;
            }
        }
    } else if (hill.ownerTeam == capturingTeam) {
        hill.captureTeam = capturingTeam;
        hill.captureProgress = 1.0f;
    } else if (hill.ownerTeam == 0) {
        if (hill.captureTeam != 0 && hill.captureTeam != capturingTeam) {
            // Neutral point with opposite team progress: burn down first.
            hill.captureProgress = std::max(0.0f, hill.captureProgress - captureStep);
            if (hill.captureProgress == 0.0f) {
                hill.captureTeam = capturingTeam;
            }
        } else {
            hill.captureTeam = capturingTeam;
            hill.captureProgress = std::min(1.0f, hill.captureProgress + captureStep);
            if (hill.captureProgress >= 1.0f) {
                hill.ownerTeam = capturingTeam;
                hill.captureTeam = capturingTeam;
                hill.captureProgress = 1.0f;
            }
        }
    } else {
        // Enemy on owned point: neutralize to 0 first, then begin capture on following ticks.
        hill.captureTeam = capturingTeam;
        hill.captureProgress = std::max(0.0f, hill.captureProgress - captureStep);
        if (hill.captureProgress <= 0.0f) {
            hill.ownerTeam = 0;
            hill.captureProgress = 0.0f;
        }
    }

    // TF2-style KOTH timer: owning team clock counts down as long as they still own the point,
    // even when enemies are contesting.
    if (hill.overtime == 0 && hill.ownerTeam != 0 && frameDt > 0.0f) {
        if (hill.ownerTeam == 1) {
            hill.team1TimeLeft = std::max(0.0f, hill.team1TimeLeft - frameDt);
        } else if (hill.ownerTeam == 2) {
            hill.team2TimeLeft = std::max(0.0f, hill.team2TimeLeft - frameDt);
        }
    }

    // Win/overtime checks.
    if (hill.overtime == 0) {
        uint8_t zeroTeam = 0;
        if (hill.team1TimeLeft <= 0.0f) zeroTeam = 1;
        if (hill.team2TimeLeft <= 0.0f) zeroTeam = 2;
        if (zeroTeam != 0) {
            const uint8_t otherTeam = (zeroTeam == 1) ? 2 : 1;
            const bool zeroTeamOwnsPoint = (hill.ownerTeam == zeroTeam);
            const bool enemyPressuringPoint = (zeroTeam == 1) ? (team2OnHill > 0) : (team1OnHill > 0);
            if (zeroTeamOwnsPoint && enemyPressuringPoint) {
                hill.overtime = 1;
                hill.overtimeCheckTeam = zeroTeam;
            } else {
                hill.winnerTeam = zeroTeam;
            }
        }
    } else {
        // Overtime resolution:
        // - Team at 0 wins once enemy pressure ends without a completed takeover.
        // - Enemy wins if they successfully overtake point.
        const uint8_t checkTeam = hill.overtimeCheckTeam;
        const uint8_t otherTeam = (checkTeam == 1) ? 2 : 1;
        const bool enemyStillPressuring = (checkTeam == 1) ? (team2OnHill > 0) : (team1OnHill > 0);
        if (hill.ownerTeam == otherTeam) {
            hill.winnerTeam = otherTeam;
            hill.overtime = 0;
        } else if (!enemyStillPressuring) {
            hill.winnerTeam = checkTeam;
            hill.overtime = 0;
        }
    }
}

void updateRoundAndMatchFlow(HillState& hill, std::vector<Player>& players, double now) {
    if (hill.matchWinnerTeam != 0) {
        if (hill.matchResetAt > 0.0 && now >= hill.matchResetAt) {
            resetMatchState(hill, players, now);
        }
        return;
    }

    if (hill.winnerTeam == 0) {
        return;
    }

    if (hill.roundResetAt <= 0.0) {
        if (hill.winnerTeam == 1) {
            hill.team1RoundPoints = static_cast<uint8_t>(std::min<int>(255, hill.team1RoundPoints + 1));
        } else if (hill.winnerTeam == 2) {
            hill.team2RoundPoints = static_cast<uint8_t>(std::min<int>(255, hill.team2RoundPoints + 1));
        }

        if (hill.team1RoundPoints >= MatchPointTarget || hill.team2RoundPoints >= MatchPointTarget) {
            hill.matchWinnerTeam = (hill.team1RoundPoints >= MatchPointTarget) ? 1 : 2;
            hill.matchResetAt = now + MatchVictoryScreenSeconds;
            return;
        }

        hill.roundResetAt = now + RoundIntermissionSeconds;
    }

    if (now >= hill.roundResetAt) {
        resetHillForNewRound(hill, now);
        resetAllPlayersForNewRound(players, now);
    }
}

void broadcastSnapshot(SOCKET socket, const std::vector<Player>& players, uint32_t serverTick, const HillState& hill) {
    arena::SnapshotPacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Snapshot);
    packet.serverTick = serverTick;
    packet.playerCount = static_cast<uint32_t>(std::min<size_t>(players.size(), arena::MaxPlayers));
    packet.team1TimeLeftSeconds = static_cast<uint16_t>(std::clamp(static_cast<int>(std::ceil(hill.team1TimeLeft)), 0, 65535));
    packet.team2TimeLeftSeconds = static_cast<uint16_t>(std::clamp(static_cast<int>(std::ceil(hill.team2TimeLeft)), 0, 65535));
    packet.hillOwnerTeam = hill.ownerTeam;
    packet.hillCaptureTeam = hill.captureTeam;
    packet.hillContested = hill.contested;
    packet.hillOvertime = hill.overtime;
    packet.hillWinnerTeam = hill.winnerTeam;
    packet.team1RoundPoints = hill.team1RoundPoints;
    packet.team2RoundPoints = hill.team2RoundPoints;
    packet.matchWinnerTeam = hill.matchWinnerTeam;
    const double now = arena::secondsNow();
    if (hill.matchWinnerTeam != 0 && hill.matchResetAt > now) {
        packet.matchResetSecondsLeft = static_cast<uint16_t>(
            std::clamp(static_cast<int>(std::ceil(hill.matchResetAt - now)), 0, 65535));
    } else {
        packet.matchResetSecondsLeft = 0;
    }
    packet.hillCaptureProgress = hill.captureProgress;

    const double snapshotNow = arena::secondsNow();
    for (uint32_t i = 0; i < packet.playerCount; ++i) {
        packet.players[i].playerId = players[i].id;
        std::memset(packet.players[i].name, 0, sizeof(packet.players[i].name));
        std::memcpy(packet.players[i].name, players[i].name.c_str(), std::min(players[i].name.size(), sizeof(packet.players[i].name) - 1));
        packet.players[i].x = players[i].position.x;
        packet.players[i].y = players[i].position.y;
        packet.players[i].z = players[i].position.z;
        packet.players[i].vx = players[i].velocity.x;
        packet.players[i].vy = players[i].velocity.y;
        packet.players[i].vz = players[i].velocity.z;
        packet.players[i].yaw = players[i].yaw;
        packet.players[i].pitch = players[i].pitch;
        packet.players[i].crouched = players[i].crouched ? 1 : 0;
        packet.players[i].teamId = players[i].teamId;
        packet.players[i].health = static_cast<uint8_t>(std::clamp(players[i].health, 0, MaxHealth));
        packet.players[i].dead = players[i].dead ? 1 : 0;
        packet.players[i].weaponSlot = static_cast<uint8_t>(players[i].equippedWeapon);
        packet.players[i].firing = (players[i].fireVisualUntil > snapshotNow) ? 1 : 0;
        packet.players[i].lgBeamActive = (players[i].equippedWeapon == WeaponSlot::LightningGun && players[i].lgBeamUntil > snapshotNow) ? 1 : 0;
        packet.players[i].lgBeamEndX = players[i].lgBeamEnd.x;
        packet.players[i].lgBeamEndY = players[i].lgBeamEnd.y;
        packet.players[i].lgBeamEndZ = players[i].lgBeamEnd.z;
        packet.players[i].hitConfirmCount = players[i].hitConfirmCount;
        packet.players[i].lastDamageDealt = players[i].lastDamageDealt;
        packet.players[i].lastHitTargetId = players[i].lastHitTargetId;
        packet.players[i].pingMs = players[i].pingMs;
        packet.players[i].kills = players[i].kills;
        packet.players[i].deaths = players[i].deaths;
        packet.players[i].damageDealt = players[i].damageDealt;
    }

    const int bytes = static_cast<int>(offsetof(arena::SnapshotPacket, players) + sizeof(arena::PlayerStatePacket) * packet.playerCount);
    for (const auto& player : players) {
        sendto(
            socket,
            reinterpret_cast<const char*>(&packet),
            bytes,
            0,
            reinterpret_cast<const sockaddr*>(&player.address),
            sizeof(player.address));
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const uint16_t port = argc >= 2 ? static_cast<uint16_t>(std::stoi(argv[1])) : arena::DefaultPort;

        arena::requireWinsock();
        SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) {
            throw std::runtime_error("socket() failed");
        }

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
        bindAddress.sin_port = htons(port);

        if (bind(socket, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
            throw std::runtime_error("bind() failed. Is the port already in use?");
        }

        arena::makeNonBlocking(socket);

        std::vector<Player> players;
        HillState hill{};
        uint32_t nextPlayerId = 1;
        uint32_t serverTick = 0;
        double nextTickAt = arena::secondsNow();
        hill.lastUpdateAt = nextTickAt;

        std::cout << "Arena server listening on UDP port " << port << "\n";
        std::cout << "Press Ctrl+C to stop.\n";

        while (true) {
            char buffer[1500]{};
            sockaddr_in from{};
            int fromLength = sizeof(from);

            while (true) {
                const int received = recvfrom(
                    socket,
                    buffer,
                    sizeof(buffer),
                    0,
                    reinterpret_cast<sockaddr*>(&from),
                    &fromLength);

                if (received == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK) {
                        break;
                    }
                    std::cerr << "recvfrom failed: " << error << "\n";
                    break;
                }

                if (arena::hasValidHeader(buffer, received, arena::PacketType::Hello)) {
                    arena::HelloPacket hello{};
                    if (received >= static_cast<int>(sizeof(arena::HelloPacket))) {
                        std::memcpy(&hello, buffer, sizeof(hello));
                    } else {
                        hello.header = arena::makeHeader(arena::PacketType::Hello);
                    }
                    Player* existing = findPlayer(players, from);
                    if (existing == nullptr && players.size() < arena::MaxPlayers) {
                        Player player{};
                        player.id = nextPlayerId++;
                        player.address = from;
                        player.teamId = (players.size() % 2 == 0) ? 1 : 2;
                        player.health = MaxHealth;
                        player.lastHeardAt = arena::secondsNow();
                        player.name = uniqueJoinName(players, std::string(hello.desiredName));
                        const int teamSlot = static_cast<int>(std::count_if(
                            players.begin(), players.end(), [&](const Player& p) { return p.teamId == player.teamId; }));
                        resetPlayerForSpawn(player, player.lastHeardAt, teamSlot);
                        players.push_back(player);
                        existing = &players.back();
                        std::cout << "Player " << existing->id << " (" << existing->name << ") joined from " << arena::addressToString(from)
                                  << " (team " << static_cast<int>(existing->teamId) << ")\n";
                    }
                    if (existing != nullptr) {
                        sendWelcome(socket, *existing);
                        if (!existing->mapSent) {
                            sendMapData(socket, *existing);
                            existing->mapSent = true;
                        }
                    }
                } else if (received == sizeof(arena::InputPacket) && arena::hasValidHeader(buffer, received, arena::PacketType::Input)) {
                    arena::InputPacket input{};
                    std::memcpy(&input, buffer, sizeof(input));

                    Player* player = findPlayer(players, from);
                    if (player != nullptr) {
                        const double now = arena::secondsNow();
                        player->lastHeardAt = now;
                        if (input.lastReceivedServerTick <= serverTick) {
                            const uint32_t deltaTicks = serverTick - input.lastReceivedServerTick;
                            const uint32_t estimatedMs = static_cast<uint32_t>(std::round(static_cast<double>(deltaTicks) * arena::TickSeconds * 1000.0));
                            player->pingMs = static_cast<uint16_t>(std::min<uint32_t>(estimatedMs, 65535));
                            player->lastAckedServerTick = input.lastReceivedServerTick;
                        }
                        integrateInput(*player, input, now);
                        const bool performedAction = processCombatInput(*player, players, input, now);
                        if (performedAction && !player->grounded && player->jumpedSinceGround) {
                            // GunZ-style cancel chaining: airborne attack action refunds one dash.
                            player->airDashCharges = std::max(player->airDashCharges, 1);
                        }
                    }
                }
            }

            const double now = arena::secondsNow();
            if (now >= nextTickAt) {
                players.erase(
                    std::remove_if(players.begin(), players.end(), [now](const Player& player) {
                        return now - player.lastHeardAt > 10.0;
                    }),
                    players.end());

                processRespawns(players, now);
                updateHillState(hill, players, now);
                updateRoundAndMatchFlow(hill, players, now);
                broadcastSnapshot(socket, players, serverTick++, hill);
                nextTickAt += arena::TickSeconds;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    } catch (const std::exception& error) {
        std::cerr << "Server error: " << error.what() << "\n";
        return 1;
    }
}
