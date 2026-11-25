#pragma once

#include <Core/Math/Float16.h>
#include <Core/Math/Vector3.h>

// 정점 원소 CPU 패킹이다(Docs/Design/RenderBase 10.4절 GX3a). 메시 쿠커와 시험이 정점 형식
// (GraphicsTypes/VertexFormat.h)에 맞춰 값을 담을 때 쓴다. half 변환은 Core/Math/Float16.h를 쓴다
// (IEEE binary16, round-to-nearest-even, 비정규수·무한대·NaN 보존).
//
// 규칙(그래픽 API 표준 정규화의 역):
// - snorm(b bit): round(clamp(v, −1, 1) × (2^(b−1) − 1)). 그래서 −1은 −(2^(b−1) − 1)이고 가장 작은
//   정수(−2^(b−1))는 만들지 않는다(GPU가 그것도 −1로 읽는다).
// - unorm(b bit): round(clamp(v, 0, 1) × (2^b − 1)).
// - 반올림은 가장 가까운 정수, 정확히 가운데면 0에서 먼 쪽이다. 곱과 반올림은 double로 해서 float
//   반올림 오차가 결과를 바꾸지 않는다. NaN은 0이다.
// - 결과는 호스트와 무관하게 비트가 같다(정수 결과만 쓰고 FMA·SIMD 경로가 없다).
//
// 사용 예:
//   const PackedNormal normal = PackNormal(Vec3(0.0f, 0.0f, 1.0f));          // Byte4Snorm (0, 0,
//   127, 127) const PackedNormal tangent = PackTangent(Vec3(1.0f, 0.0f, 0.0f), -1.0f); // w =
//   −127(쌍법선 부호) const uint32 color = PackRgb10A2Unorm(1.0f, 0.5f, 0.25f, 1.0f); const Float16
//   half = Float16::FromFloat(0.1f);
namespace rocks::math {

namespace detail {

// clamp한 값에 scale을 곱해 가장 가까운 정수로 반올림한다(가운데는 0에서 먼 쪽).
inline int32 RoundScaled(float value, float low, float high, double scale) noexcept {
    if (IsNaN(value)) {
        return 0;
    }

    const float clamped = value < low ? low : (value > high ? high : value);
    const double scaled = static_cast<double>(clamped) * scale;
    return scaled >= 0.0 ? static_cast<int32>(scaled + 0.5) : -static_cast<int32>(-scaled + 0.5);
}

} // namespace detail

inline int8 PackSnorm8(float value) noexcept {
    return static_cast<int8>(detail::RoundScaled(value, -1.0f, 1.0f, 127.0));
}

inline uint8 PackUnorm8(float value) noexcept {
    return static_cast<uint8>(detail::RoundScaled(value, 0.0f, 1.0f, 255.0));
}

inline int16 PackSnorm16(float value) noexcept {
    return static_cast<int16>(detail::RoundScaled(value, -1.0f, 1.0f, 32767.0));
}

inline uint16 PackUnorm16(float value) noexcept {
    return static_cast<uint16>(detail::RoundScaled(value, 0.0f, 1.0f, 65535.0));
}

// GPU가 읽는 값과 같은 역변환이다(snorm의 가장 작은 정수는 −1).
inline float UnpackSnorm8(int8 value) noexcept {
    const float unpacked = static_cast<float>(value) / 127.0f;
    return unpacked < -1.0f ? -1.0f : unpacked;
}

inline float UnpackUnorm8(uint8 value) noexcept {
    return static_cast<float>(value) / 255.0f;
}

inline float UnpackSnorm16(int16 value) noexcept {
    const float unpacked = static_cast<float>(value) / 32767.0f;
    return unpacked < -1.0f ? -1.0f : unpacked;
}

inline float UnpackUnorm16(uint16 value) noexcept {
    return static_cast<float>(value) / 65535.0f;
}

// 10:10:10:2 unorm 패킹이다(VertexFormat::Rgb10A2Unorm). r가 bit 0–9, g가 10–19, b가 20–29, a가
// 30–31이다(D3D12 R10G10B10A2_UNORM·GL UNSIGNED_INT_2_10_10_10_REV와 같은 배치).
inline uint32 PackRgb10A2Unorm(float r, float g, float b, float a) noexcept {
    const auto red = static_cast<uint32>(detail::RoundScaled(r, 0.0f, 1.0f, 1023.0));
    const auto green = static_cast<uint32>(detail::RoundScaled(g, 0.0f, 1.0f, 1023.0));
    const auto blue = static_cast<uint32>(detail::RoundScaled(b, 0.0f, 1.0f, 1023.0));
    const auto alpha = static_cast<uint32>(detail::RoundScaled(a, 0.0f, 1.0f, 3.0));
    return red | (green << 10) | (blue << 20) | (alpha << 30);
}

// 10:10:10:2 패킹의 성분 하나(0–3, r·g·b·a 순)를 정규화 값으로 읽는다. 범위 밖 index는 0이다.
inline float UnpackRgb10A2Unorm(uint32 packed, uint32 component) noexcept {
    if (component < 3) {
        return static_cast<float>((packed >> (component * 10)) & 0x3ffu) / 1023.0f;
    }

    return component == 3 ? static_cast<float>(packed >> 30) / 3.0f : 0.0f;
}

// Byte4Snorm에 담는 법선·탄젠트다(UE FPackedNormal과 같은 의미, xyz는 snorm8 방향, w는 법선이면
// 127, 탄젠트면 쌍법선 부호 ±127).
struct PackedNormal {
    int8 x = 0;
    int8 y = 0;
    int8 z = 0;
    int8 w = 127;

    bool operator==(const PackedNormal&) const noexcept = default;
};

// 법선을 담는다(w = 127). 길이는 바꾸지 않는다(단위 벡터를 넘긴다).
inline PackedNormal PackNormal(const Vec3& normal) noexcept {
    return {PackSnorm8(normal.x), PackSnorm8(normal.y), PackSnorm8(normal.z), 127};
}

// 탄젠트와 쌍법선 부호를 담는다. binormalSign이 0 이상이면 w = 127, 음수면 −127이다.
inline PackedNormal PackTangent(const Vec3& tangent, float binormalSign) noexcept {
    return {PackSnorm8(tangent.x), PackSnorm8(tangent.y), PackSnorm8(tangent.z),
            static_cast<int8>(binormalSign < 0.0f ? -127 : 127)};
}

} // namespace rocks::math
