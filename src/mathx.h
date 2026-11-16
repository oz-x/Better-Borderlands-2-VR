#pragma once
// Conversions between OpenXR (right-handed, +Y up, -Z forward, metres) and Unreal Engine 3
// (left-handed, +Z up, +X forward, +Y right, integer rotators with 65536 units per turn).
#include <cmath>
#include <cstdint>

#include <openxr/openxr.h>

namespace bl2vr::mathx {

constexpr float kPi = 3.14159265358979f;
constexpr float kUnitsToRad = kPi / 32768.0f;
constexpr float kRadToUnits = 32768.0f / kPi;

struct Vec3 {
    float x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 rotate(const XrQuaternionf& q, Vec3 v) {
    // v' = v + 2w(u x v) + 2u x (u x v)
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t{2.0f * (u.y * v.z - u.z * v.y), 2.0f * (u.z * v.x - u.x * v.z),
                 2.0f * (u.x * v.y - u.y * v.x)};
    const Vec3 c{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}

// OpenXR vector (metres) -> Unreal axes (same unit).
inline Vec3 xr_to_ue(Vec3 v) {
    return {-v.z, v.x, v.y};
}

// Rotation about the Unreal Z axis by `yaw` radians (positive turns +X towards +Y).
inline Vec3 yaw_ue(Vec3 v, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c - v.y * s, v.x * s + v.y * c, v.z};
}

// Rotation about the OpenXR Y axis by `yaw` radians (positive turns -Z towards -X, i.e. left).
inline Vec3 yaw_xr(Vec3 v, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    return {v.x * c + v.z * s, v.y, -v.x * s + v.z * c};
}

// Yaw of an OpenXR orientation around +Y, radians, 0 = looking down -Z, positive = left.
inline float xr_yaw(const XrQuaternionf& q) {
    const Vec3 f = rotate(q, {0, 0, -1});
    return std::atan2(-f.x, -f.z);
}

struct Rotator {
    int32_t pitch, yaw, roll;
};

// Unreal rotator from orthonormal forward/right/up axes (Unreal space), matching
// FMatrix::Rotator() in UE3.
inline Rotator axes_to_rotator(Vec3 forward, Vec3 right, Vec3 up) {
    Rotator r{};
    const float pitch = std::atan2(forward.z, std::sqrt(forward.x * forward.x + forward.y * forward.y));
    const float yaw = std::atan2(forward.y, forward.x);
    r.pitch = static_cast<int32_t>(std::lround(pitch * kRadToUnits));
    r.yaw = static_cast<int32_t>(std::lround(yaw * kRadToUnits));
    // Right axis of the roll-free rotation (FRotationMatrix(pitch, yaw, 0).GetAxis(1)).
    const Vec3 sy{-std::sin(yaw), std::cos(yaw), 0.0f};
    const float roll = std::atan2(dot(up, sy), dot(right, sy));
    r.roll = static_cast<int32_t>(std::lround(roll * kRadToUnits));
    return r;
}

inline int32_t wrap_units(int32_t v) {
    return static_cast<int16_t>(static_cast<uint16_t>(v & 0xFFFF));
}

}  // namespace bl2vr::mathx
