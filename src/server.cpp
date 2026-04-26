#include "net.hpp"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

namespace {

enum class WeaponSlot : uint8_t {
    Shotgun = 1,
    Knife = 3,
};

constexpr float BaseMoveSpeed = 8.9f;
constexpr float KnifeSpeedMultiplier = 1.2f;
constexpr float KnifeJumpMultiplier = 1.08f;
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
constexpr float GravityScale = 1.18f;
constexpr float JumpVelocityScale = 0.9f;
constexpr float JumpStrafeAssist = 1.2f;
constexpr float JumpBufferSeconds = 0.18f;
constexpr int MaxHealth = 100;
constexpr float ShotgunRange = 36.0f;
constexpr int ShotgunDamage = 34;
constexpr float ShotgunFireInterval = 0.24f;
constexpr float KnifeRange = 2.2f;
constexpr int KnifeDamage = 45;
constexpr float KnifeFireInterval = 0.35f;
constexpr float RespawnDelaySeconds = 3.0f;

struct Player {
    uint32_t id = 0;
    sockaddr_in address{};
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
    double lastHeardAt = 0.0;
};

struct HillState {
    arena::Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 8.0f;
    float team1TimeLeft = 180.0f;
    float team2TimeLeft = 180.0f;
    uint8_t ownerTeam = 0;
    uint8_t captureTeam = 0;
    uint8_t contested = 0;
    uint8_t overtime = 0;
    uint8_t winnerTeam = 0;
    uint8_t overtimeCheckTeam = 0;
    float captureProgress = 0.0f;
    double lastUpdateAt = 0.0;
};

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

void sendWelcome(SOCKET socket, const Player& player) {
    arena::WelcomePacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Welcome);
    packet.playerId = player.id;

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

    const WeaponSlot inputWeapon = (input.weaponSlot == static_cast<uint8_t>(WeaponSlot::Shotgun)) ? WeaponSlot::Shotgun : WeaponSlot::Knife;
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

    player.velocityY -= arena::Gravity * GravityScale * arena::TickSeconds;
    player.position.x += player.velocity.x * arena::TickSeconds;
    player.position.z += player.velocity.z * arena::TickSeconds;
    player.position.y += player.velocityY * arena::TickSeconds;

    if (player.position.y <= 0.0f) {
        player.position.y = 0.0f;
        player.velocityY = 0.0f;
        player.grounded = true;
        player.airborneSpeedMultiplier = 1.0f;
        player.wasForwardHeld = false;
        player.jumpedSinceGround = false;
        player.airDashCharges = 0;
        player.dashBoostUntil = 0.0;
    } else {
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

bool processCombatInput(Player& attacker, std::vector<Player>& players, const arena::InputPacket& input, double now) {
    if (attacker.dead || input.firePressed == 0 || now < attacker.nextFireAt) {
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
    } else {
        attacker.nextFireAt = now + ShotgunFireInterval;
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

    bestTarget->health = std::max(0, bestTarget->health - damage);
    attacker.hitConfirmCount++;
    attacker.lastDamageDealt = static_cast<uint8_t>(std::clamp(damage, 0, 255));
    attacker.lastHitTargetId = bestTarget->id;
    if (bestTarget->health == 0) {
        bestTarget->dead = true;
        bestTarget->respawnAt = now + RespawnDelaySeconds;
        bestTarget->velocity = {};
        bestTarget->velocityY = 0.0f;
    }
    return true;
}

void processRespawns(std::vector<Player>& players, double now) {
    int team1Count = 0;
    int team2Count = 0;
    for (const Player& p : players) {
        if (p.teamId == 1) team1Count++;
        if (p.teamId == 2) team2Count++;
    }
    int team1SpawnIndex = 0;
    int team2SpawnIndex = 0;

    for (Player& player : players) {
        if (!player.dead || now < player.respawnAt) {
            continue;
        }
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
        player.grounded = true;

        if (player.teamId == 1) {
            const float z = -12.0f + static_cast<float>(team1SpawnIndex % std::max(1, team1Count)) * 4.0f;
            player.position = {-16.0f, 0.0f, z};
            team1SpawnIndex++;
        } else {
            const float z = -12.0f + static_cast<float>(team2SpawnIndex % std::max(1, team2Count)) * 4.0f;
            player.position = {16.0f, 0.0f, z};
            team2SpawnIndex++;
        }
    }
}

void updateHillState(HillState& hill, const std::vector<Player>& players, double now) {
    if (hill.lastUpdateAt <= 0.0) {
        hill.lastUpdateAt = now;
    }
    const float frameDt = static_cast<float>(std::max(0.0, now - hill.lastUpdateAt));
    hill.lastUpdateAt = now;
    if (hill.winnerTeam != 0) {
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
    const float delta = arena::TickSeconds / captureTimeSeconds;

    if (hill.contested != 0) {
        // Contested point: freeze progress exactly where it is.
    } else if (capturingTeam == 0) {
        if (hill.ownerTeam == 0 && hill.captureProgress > 0.0f) {
            hill.captureProgress = std::max(0.0f, hill.captureProgress - delta * 0.5f);
            if (hill.captureProgress == 0.0f) {
                hill.captureTeam = 0;
            }
        }
    } else if (hill.ownerTeam == capturingTeam) {
        hill.captureTeam = capturingTeam;
        hill.captureProgress = 1.0f;
    } else if (hill.ownerTeam != 0 && hill.ownerTeam != capturingTeam) {
        hill.captureProgress = std::max(0.0f, hill.captureProgress - delta);
        hill.captureTeam = capturingTeam;
        if (hill.captureProgress <= 0.0f) {
            hill.ownerTeam = 0;
            hill.captureProgress = 0.0f;
        }
    } else {
        if (hill.captureTeam != capturingTeam) {
            hill.captureTeam = capturingTeam;
            hill.captureProgress = 0.0f;
        }
        hill.captureProgress = std::min(1.0f, hill.captureProgress + delta);
        if (hill.captureProgress >= 1.0f) {
            hill.ownerTeam = capturingTeam;
            hill.captureTeam = capturingTeam;
            hill.captureProgress = 1.0f;
        }
    }

    // TF2-style KOTH timer: owning team clock counts down.
    if (hill.overtime == 0 && hill.contested == 0 && hill.ownerTeam != 0 && frameDt > 0.0f) {
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
            const bool otherCapturing = (hill.captureTeam == otherTeam && hill.captureProgress > 0.0f && hill.ownerTeam != otherTeam);
            if (otherCapturing) {
                hill.overtime = 1;
                hill.overtimeCheckTeam = zeroTeam;
            } else {
                hill.winnerTeam = zeroTeam;
            }
        }
    } else {
        // Overtime resolution:
        // - Team at 0 wins if enemy capture progress is fully cleared.
        // - Enemy wins if they successfully overtake point.
        const uint8_t checkTeam = hill.overtimeCheckTeam;
        const uint8_t otherTeam = (checkTeam == 1) ? 2 : 1;
        const bool otherCapturing = (hill.captureTeam == otherTeam && hill.captureProgress > 0.0f && hill.ownerTeam != otherTeam);
        if (!otherCapturing && hill.ownerTeam == checkTeam) {
            hill.winnerTeam = checkTeam;
            hill.overtime = 0;
        } else if (hill.ownerTeam == otherTeam) {
            hill.winnerTeam = otherTeam;
            hill.overtime = 0;
        }
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
    packet.hillCaptureProgress = hill.captureProgress;

    for (uint32_t i = 0; i < packet.playerCount; ++i) {
        packet.players[i].playerId = players[i].id;
        packet.players[i].x = players[i].position.x;
        packet.players[i].y = players[i].position.y;
        packet.players[i].z = players[i].position.z;
        packet.players[i].yaw = players[i].yaw;
        packet.players[i].pitch = players[i].pitch;
        packet.players[i].crouched = players[i].crouched ? 1 : 0;
        packet.players[i].teamId = players[i].teamId;
        packet.players[i].health = static_cast<uint8_t>(std::clamp(players[i].health, 0, MaxHealth));
        packet.players[i].dead = players[i].dead ? 1 : 0;
        packet.players[i].hitConfirmCount = players[i].hitConfirmCount;
        packet.players[i].lastDamageDealt = players[i].lastDamageDealt;
        packet.players[i].lastHitTargetId = players[i].lastHitTargetId;
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
                    Player* existing = findPlayer(players, from);
                    if (existing == nullptr && players.size() < arena::MaxPlayers) {
                        Player player{};
                        player.id = nextPlayerId++;
                        player.address = from;
                        player.position = {static_cast<float>((player.id % 8) * 2), 0.0f, 0.0f};
                        player.teamId = (players.size() % 2 == 0) ? 1 : 2;
                        player.health = MaxHealth;
                        player.lastHeardAt = arena::secondsNow();
                        players.push_back(player);
                        existing = &players.back();
                        std::cout << "Player " << existing->id << " joined from " << arena::addressToString(from)
                                  << " (team " << static_cast<int>(existing->teamId) << ")\n";
                    }
                    if (existing != nullptr) {
                        sendWelcome(socket, *existing);
                    }
                } else if (received == sizeof(arena::InputPacket) && arena::hasValidHeader(buffer, received, arena::PacketType::Input)) {
                    arena::InputPacket input{};
                    std::memcpy(&input, buffer, sizeof(input));

                    Player* player = findPlayer(players, from);
                    if (player != nullptr) {
                        const double now = arena::secondsNow();
                        player->lastHeardAt = now;
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
