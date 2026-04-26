#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOUSER
#define NOUSER
#endif
#ifndef NOGDI
#define NOGDI
#endif
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace arena {

constexpr uint32_t ProtocolMagic = 0x41524650; // ARFP
constexpr uint16_t DefaultPort = 40000;
constexpr int MaxPlayers = 32;
constexpr float TickSeconds = 1.0f / 60.0f;
constexpr float RoomHalfSize = 80.0f;
constexpr float RoomHeight = 18.0f;
constexpr float PlayerRadius = 0.45f;
constexpr float PlayerHeight = 1.8f;
constexpr float CrouchHeight = 1.15f;
constexpr float JumpVelocity = 5.4f;
constexpr float Gravity = 14.0f;
constexpr float CrouchSpeedScale = 0.55f;
constexpr float StandEyeHeight = 1.55f;
constexpr float CrouchEyeHeight = 0.95f;
constexpr float MaxLookPitch = 1.35f;

enum class PacketType : uint8_t {
    Hello = 1,
    Input = 2,
    Welcome = 3,
    Snapshot = 4,
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t type;
};

struct HelloPacket {
    PacketHeader header;
};

struct InputPacket {
    PacketHeader header;
    uint32_t sequence;
    float moveX;
    float moveZ;
    float yaw;
    float pitch;
    uint8_t jumpPressed;
    uint8_t crouchHeld;
    uint8_t weaponSlot;
    uint8_t firePressed;
    uint8_t dashPressed;
    int8_t dashMoveX;
    int8_t dashMoveZ;
};

struct WelcomePacket {
    PacketHeader header;
    uint32_t playerId;
};

struct PlayerStatePacket {
    uint32_t playerId;
    float x;
    float y;
    float z;
    float yaw;
    float pitch;
    uint8_t crouched;
    uint8_t teamId;
    uint8_t health;
    uint8_t dead;
    uint16_t hitConfirmCount;
    uint8_t lastDamageDealt;
    uint32_t lastHitTargetId;
};

struct SnapshotPacket {
    PacketHeader header;
    uint32_t serverTick;
    uint32_t playerCount;
    uint16_t team1TimeLeftSeconds;
    uint16_t team2TimeLeftSeconds;
    uint8_t hillOwnerTeam;
    uint8_t hillCaptureTeam;
    uint8_t hillContested;
    uint8_t hillOvertime;
    uint8_t hillWinnerTeam;
    float hillCaptureProgress;
    PlayerStatePacket players[MaxPlayers];
};
#pragma pack(pop)

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(Vec3 v) {
    const float len = length(v);
    if (len <= 0.0001f) {
        return {};
    }
    return v * (1.0f / len);
}

inline float clamp(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

inline double secondsNow() {
    using Clock = std::chrono::steady_clock;
    static const auto start = Clock::now();
    const auto elapsed = Clock::now() - start;
    return std::chrono::duration<double>(elapsed).count();
}

inline void requireWinsock() {
    WSADATA data{};
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0) {
        throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
    }
}

inline void makeNonBlocking(SOCKET socket) {
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) != 0) {
        throw std::runtime_error("ioctlsocket(FIONBIO) failed");
    }
}

inline bool sameAddress(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

inline std::string addressToString(const sockaddr_in& address) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, const_cast<in_addr*>(&address.sin_addr), ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
}

inline PacketHeader makeHeader(PacketType type) {
    return {ProtocolMagic, static_cast<uint8_t>(type)};
}

inline bool hasValidHeader(const char* bytes, int size, PacketType type) {
    if (size < static_cast<int>(sizeof(PacketHeader))) {
        return false;
    }

    PacketHeader header{};
    std::memcpy(&header, bytes, sizeof(header));
    return header.magic == ProtocolMagic && header.type == static_cast<uint8_t>(type);
}

inline sockaddr_in makeAddress(const char* host, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        throw std::runtime_error("Invalid IPv4 address: " + std::string(host));
    }
    return address;
}

} // namespace arena
