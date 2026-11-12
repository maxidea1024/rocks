#pragma once

// ARM64 전용 구현이다. 다른 CPU에서 단독 include하면 아무 선언도 만들지 않는다.
#if defined(_M_ARM64) || defined(__aarch64__)

#include <Base/Types.h>

#include <arm_neon.h>

namespace rocks::platform::neon {

// AArch64 NEON 4-lane float 구현이다. 함수 목록·연산 순서·특수값 규약은 VectorRegisterScalar.h와
// 같다. MultiplyAdd에 vfmaq를 쓰지 않아 곱의 반올림을 scalar 계약과 같게 유지한다.
// vminq/vmaxq는 NaN을 전파하여 scalar 규약과 다르므로 비교·선택으로 구현한다.
using Register4f = float32x4_t;

inline Register4f Load(const float* source) noexcept {
    return vld1q_f32(source);
}

inline void Store(float* destination, Register4f value) noexcept {
    vst1q_f32(destination, value);
}

inline Register4f Set(float x, float y, float z, float w) noexcept {
    const float values[4] = {x, y, z, w};
    return vld1q_f32(values);
}

inline Register4f Splat(float value) noexcept {
    return vdupq_n_f32(value);
}

inline Register4f Zero() noexcept {
    return vdupq_n_f32(0.0f);
}

template <int Index>
inline float Get(Register4f value) noexcept {
    static_assert(Index >= 0 && Index < 4);
    return vgetq_lane_f32(value, Index);
}

inline Register4f Add(Register4f a, Register4f b) noexcept {
    return vaddq_f32(a, b);
}

inline Register4f Subtract(Register4f a, Register4f b) noexcept {
    return vsubq_f32(a, b);
}

inline Register4f Multiply(Register4f a, Register4f b) noexcept {
    return vmulq_f32(a, b);
}

inline Register4f Divide(Register4f a, Register4f b) noexcept {
    return vdivq_f32(a, b);
}

inline Register4f MultiplyAdd(Register4f a, Register4f b, Register4f c) noexcept {
    return vaddq_f32(vmulq_f32(a, b), c);
}

inline Register4f Negate(Register4f a) noexcept {
    return vnegq_f32(a);
}

inline Register4f Abs(Register4f a) noexcept {
    return vabsq_f32(a);
}

inline Register4f Min(Register4f a, Register4f b) noexcept {
    return vbslq_f32(vcltq_f32(a, b), a, b);
}

inline Register4f Max(Register4f a, Register4f b) noexcept {
    return vbslq_f32(vcgtq_f32(a, b), a, b);
}

inline Register4f Sqrt(Register4f a) noexcept {
    return vsqrtq_f32(a);
}

inline float Dot3(Register4f a, Register4f b) noexcept {
    const Register4f product = vmulq_f32(a, b);
    const float first = vgetq_lane_f32(product, 0) + vgetq_lane_f32(product, 1);
    return first + vgetq_lane_f32(product, 2);
}

inline float Dot4(Register4f a, Register4f b) noexcept {
    const Register4f product = vmulq_f32(a, b);
    const float low = vgetq_lane_f32(product, 0) + vgetq_lane_f32(product, 1);
    const float high = vgetq_lane_f32(product, 2) + vgetq_lane_f32(product, 3);
    return low + high;
}

inline Register4f Cross3(Register4f a, Register4f b) noexcept {
    const float ax = vgetq_lane_f32(a, 0);
    const float ay = vgetq_lane_f32(a, 1);
    const float az = vgetq_lane_f32(a, 2);
    const float bx = vgetq_lane_f32(b, 0);
    const float by = vgetq_lane_f32(b, 1);
    const float bz = vgetq_lane_f32(b, 2);
    const Register4f left = vmulq_f32(Set(ay, az, ax, 0.0f), Set(bz, bx, by, 0.0f));
    const Register4f right = vmulq_f32(Set(az, ax, ay, 0.0f), Set(by, bz, bx, 0.0f));
    return vsetq_lane_f32(0.0f, vsubq_f32(left, right), 3);
}

inline Register4f QuaternionMultiply(Register4f a, Register4f b) noexcept {
    const float bx = vgetq_lane_f32(b, 0);
    const float by = vgetq_lane_f32(b, 1);
    const float bz = vgetq_lane_f32(b, 2);
    const float bw = vgetq_lane_f32(b, 3);
    const Register4f termW = vmulq_f32(vdupq_laneq_f32(a, 3), b);
    const Register4f termX = vmulq_f32(vdupq_laneq_f32(a, 0), Set(bw, -bz, by, -bx));
    const Register4f termY = vmulq_f32(vdupq_laneq_f32(a, 1), Set(bz, bw, -bx, -by));
    const Register4f termZ = vmulq_f32(vdupq_laneq_f32(a, 2), Set(-by, bx, bw, -bz));
    return vaddq_f32(vaddq_f32(vaddq_f32(termW, termX), termY), termZ);
}

inline Register4f TransformVector4(const float* matrix, Register4f vector) noexcept {
    const Register4f c0 = vmulq_f32(vld1q_f32(matrix + 0), vdupq_laneq_f32(vector, 0));
    const Register4f c1 = vmulq_f32(vld1q_f32(matrix + 4), vdupq_laneq_f32(vector, 1));
    const Register4f c2 = vmulq_f32(vld1q_f32(matrix + 8), vdupq_laneq_f32(vector, 2));
    const Register4f c3 = vmulq_f32(vld1q_f32(matrix + 12), vdupq_laneq_f32(vector, 3));
    return vaddq_f32(vaddq_f32(vaddq_f32(c0, c1), c2), c3);
}

inline void MatrixMultiply(const float* a, const float* b, float* output) noexcept {
    const Register4f c0 = TransformVector4(a, vld1q_f32(b + 0));
    const Register4f c1 = TransformVector4(a, vld1q_f32(b + 4));
    const Register4f c2 = TransformVector4(a, vld1q_f32(b + 8));
    const Register4f c3 = TransformVector4(a, vld1q_f32(b + 12));
    vst1q_f32(output + 0, c0);
    vst1q_f32(output + 4, c1);
    vst1q_f32(output + 8, c2);
    vst1q_f32(output + 12, c3);
}

} // namespace rocks::platform::neon

#endif
