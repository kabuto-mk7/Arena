#include "net.hpp"

#include <algorithm>
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
constexpr float SvAccelerate = 10.0f;
constexpr float SvAirAccelerate = 6.0f;
constexpr float SvFriction = 4.0f;
constexpr float SvStopSpeed = 2.8f;
constexpr float SvAirSpeedCap = 2.2f;
constexpr float MaxBhopSpeedFactor = 1.45f;

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
    uint8_t teamId = 1;
    WeaponSlot equippedWeapon = WeaponSlot::Knife;
    float airborneSpeedMultiplier = 1.0f;
    double lastHeardAt = 0.0;
};

struct HillState {
    arena::Vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 8.0f;
    uint16_t team1Score = 0;
    uint16_t team2Score = 0;
    uint8_t ownerTeam = 0;
    uint8_t captureTeam = 0;
    float captureProgress = 0.0f;
    double nextScoreAt = 0.0;
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

void integrateInput(Player& player, const arena::InputPacket& input) {
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
    const bool jumpPressed = input.jumpPressed != 0;
    const bool willJump = jumpPressed && player.grounded;
    if (willJump) {
        const float jumpScale = (player.equippedWeapon == WeaponSlot::Knife) ? KnifeJumpMultiplier : 1.0f;
        player.velocityY = arena::JumpVelocity * jumpScale;
        player.airborneSpeedMultiplier = (player.equippedWeapon == WeaponSlot::Knife) ? KnifeSpeedMultiplier : 1.0f;
        player.grounded = false;
    }

    arena::Vec3 wish = right * input.moveX + forward * input.moveZ;
    wish = arena::normalize(wish);
    const bool forwardHeld = input.moveZ > 0.1f;

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
        if (!player.grounded && forwardHeld && !player.wasForwardHeld) {
            applyTapStrafeRedirect(player, forward, weaponSpeedScale);
        }
    }
    capHorizontalVelocity(player, moveSpeed * MaxBhopSpeedFactor);
    player.wasForwardHeld = forwardHeld;

    player.velocityY -= arena::Gravity * arena::TickSeconds;
    player.position.x += player.velocity.x * arena::TickSeconds;
    player.position.z += player.velocity.z * arena::TickSeconds;
    player.position.y += player.velocityY * arena::TickSeconds;

    if (player.position.y <= 0.0f) {
        player.position.y = 0.0f;
        player.velocityY = 0.0f;
        player.grounded = true;
        player.airborneSpeedMultiplier = 1.0f;
        player.wasForwardHeld = false;
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

void updateHillState(HillState& hill, const std::vector<Player>& players, double now) {
    int team1OnHill = 0;
    int team2OnHill = 0;

    const float radiusSq = hill.radius * hill.radius;
    for (const Player& player : players) {
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
    if (team1OnHill > 0 && team2OnHill == 0) {
        capturingTeam = 1;
    } else if (team2OnHill > 0 && team1OnHill == 0) {
        capturingTeam = 2;
    }

    constexpr float captureTimeSeconds = 8.0f;
    const float delta = arena::TickSeconds / captureTimeSeconds;

    if (capturingTeam == 0) {
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

    if (hill.ownerTeam != 0 && capturingTeam == hill.ownerTeam && now >= hill.nextScoreAt) {
        if (hill.ownerTeam == 1) {
            hill.team1Score++;
        } else if (hill.ownerTeam == 2) {
            hill.team2Score++;
        }
        hill.nextScoreAt = now + 1.0;
    }
}

void broadcastSnapshot(SOCKET socket, const std::vector<Player>& players, uint32_t serverTick, const HillState& hill) {
    arena::SnapshotPacket packet{};
    packet.header = arena::makeHeader(arena::PacketType::Snapshot);
    packet.serverTick = serverTick;
    packet.playerCount = static_cast<uint32_t>(std::min<size_t>(players.size(), arena::MaxPlayers));
    packet.team1Score = hill.team1Score;
    packet.team2Score = hill.team2Score;
    packet.hillOwnerTeam = hill.ownerTeam;
    packet.hillCaptureTeam = hill.captureTeam;
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
    }

    const int bytes = static_cast<int>(sizeof(arena::PacketHeader) + sizeof(uint32_t) * 2 + sizeof(uint16_t) * 2 + sizeof(uint8_t) * 2 + sizeof(float) +
        sizeof(arena::PlayerStatePacket) * packet.playerCount);
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
        hill.nextScoreAt = nextTickAt + 1.0;

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
                        player->lastHeardAt = arena::secondsNow();
                        integrateInput(*player, input);
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
