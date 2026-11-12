#pragma once

#include <Platform/Math/GenericPlatformMath.h>

namespace rocks::platform::scalar {

// 4개 float lane의 기준 구현이다. SIMD backend가 없는 CPU의 fallback이자, SSE/NEON 구현을
// 검증하는 차등 oracle이다. 모든 CPU에서 항상 컴파일된다.
//
// 연산 순서 계약(모든 backend 공통):
// - MultiplyAdd(a, b, c)는 a*b를 먼저 반올림한 뒤 c를 더한다. FMA를 사용하지 않는다.
// - Dot3 = (p0 + p1) + p2, Dot4 = (p0 + p1) + (p2 + p3)이며 pi = ai * bi다.
// - 행렬 곱과 변환은 열 순서대로 ((c0*s0 + c1*s1) + c2*s2) + c3*s3를 누적한다.
// 같은 순서이므로 FMA 축약이 없는 x64에서는 scalar와 SIMD 결과가 bit 단위로 같다. 단 NaN은
// 값(NaN 여부)만 같고 부호·payload bit는 backend마다 다를 수 있다(SSE는 첫 피연산자 NaN을 전파).
// compiler의 부동소수점 축약을 막기 위해 곱과 합을 별도 문장으로 계산하고, 빌드도
// -ffp-contract=off로 축약을 금지한다.
struct Register4f {
    float lane[4];
};

inline Register4f Load(const float* source) noexcept {
    return Register4f{{source[0], source[1], source[2], source[3]}};
}

inline void Store(float* destination, Register4f value) noexcept {
    destination[0] = value.lane[0];
    destination[1] = value.lane[1];
    destination[2] = value.lane[2];
    destination[3] = value.lane[3];
}

inline Register4f Set(float x, float y, float z, float w) noexcept {
    return Register4f{{x, y, z, w}};
}

inline Register4f Splat(float value) noexcept {
    return Register4f{{value, value, value, value}};
}

inline Register4f Zero() noexcept {
    return Splat(0.0f);
}

template <int Index>
inline float Get(Register4f value) noexcept {
    static_assert(Index >= 0 && Index < 4);
    return value.lane[Index];
}

inline Register4f Add(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = a.lane[i] + b.lane[i];
    }
    return result;
}

inline Register4f Subtract(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = a.lane[i] - b.lane[i];
    }
    return result;
}

inline Register4f Multiply(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = a.lane[i] * b.lane[i];
    }
    return result;
}

inline Register4f Divide(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = a.lane[i] / b.lane[i];
    }
    return result;
}

inline Register4f MultiplyAdd(Register4f a, Register4f b, Register4f c) noexcept {
    return Add(Multiply(a, b), c);
}

// 부호 bit만 뒤집는다. NaN의 부호 bit도 바뀐다.
inline Register4f Negate(Register4f a) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] =
            __builtin_bit_cast(float, __builtin_bit_cast(uint32, a.lane[i]) ^ 0x80000000u);
    }
    return result;
}

inline Register4f Abs(Register4f a) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = GenericPlatformMath::Abs(a.lane[i]);
    }
    return result;
}

// lane별 (a < b) ? a : b. NaN·부호 있는 0의 규약은 GenericPlatformMath::Min과 같다.
inline Register4f Min(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = GenericPlatformMath::Min(a.lane[i], b.lane[i]);
    }
    return result;
}

inline Register4f Max(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = GenericPlatformMath::Max(a.lane[i], b.lane[i]);
    }
    return result;
}

inline Register4f Sqrt(Register4f a) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = GenericPlatformMath::Sqrt(a.lane[i]);
    }
    return result;
}

inline float Dot3(Register4f a, Register4f b) noexcept {
    const Register4f product = Multiply(a, b);
    const float first = product.lane[0] + product.lane[1];
    return first + product.lane[2];
}

inline float Dot4(Register4f a, Register4f b) noexcept {
    const Register4f product = Multiply(a, b);
    const float low = product.lane[0] + product.lane[1];
    const float high = product.lane[2] + product.lane[3];
    return low + high;
}

// xyz의 오른손 외적이다. w는 입력과 무관하게 +0이다.
inline Register4f Cross3(Register4f a, Register4f b) noexcept {
    const float yz = a.lane[1] * b.lane[2];
    const float zy = a.lane[2] * b.lane[1];
    const float zx = a.lane[2] * b.lane[0];
    const float xz = a.lane[0] * b.lane[2];
    const float xy = a.lane[0] * b.lane[1];
    const float yx = a.lane[1] * b.lane[0];
    return Register4f{{yz - zy, zx - xz, xy - yx, 0.0f}};
}

// (x, y, z, w) 순서 quaternion의 Hamilton 곱 a*b다. 열 벡터 회전에서 b를 먼저, a를 나중에 적용한다.
// 누적 순서: ((aw*b + ax*[bw,-bz,by,-bx]) + ay*[bz,bw,-bx,-by]) + az*[-by,bx,bw,-bz]
inline Register4f QuaternionMultiply(Register4f a, Register4f b) noexcept {
    const float bx = b.lane[0];
    const float by = b.lane[1];
    const float bz = b.lane[2];
    const float bw = b.lane[3];
    const Register4f termW = Multiply(Splat(a.lane[3]), b);
    const Register4f termX = Multiply(Splat(a.lane[0]), Set(bw, -bz, by, -bx));
    const Register4f termY = Multiply(Splat(a.lane[1]), Set(bz, bw, -bx, -by));
    const Register4f termZ = Multiply(Splat(a.lane[2]), Set(-by, bx, bw, -bz));
    return Add(Add(Add(termW, termX), termY), termZ);
}

// 열 우선 4x4 행렬 m(m[column * 4 + row])과 열 벡터 v의 곱 m * v다.
inline Register4f TransformVector4(const float* matrix, Register4f vector) noexcept {
    const Register4f c0 = Multiply(Load(matrix + 0), Splat(vector.lane[0]));
    const Register4f c1 = Multiply(Load(matrix + 4), Splat(vector.lane[1]));
    const Register4f c2 = Multiply(Load(matrix + 8), Splat(vector.lane[2]));
    const Register4f c3 = Multiply(Load(matrix + 12), Splat(vector.lane[3]));
    return Add(Add(Add(c0, c1), c2), c3);
}

// 열 우선 행렬 곱 output = a * b다. output은 a 또는 b와 같은 저장소여도 된다.
inline void MatrixMultiply(const float* a, const float* b, float* output) noexcept {
    Register4f columns[4];
    for (int column = 0; column < 4; ++column) {
        columns[column] = TransformVector4(a, Load(b + column * 4));
    }
    for (int column = 0; column < 4; ++column) {
        Store(output + column * 4, columns[column]);
    }
}

} // namespace rocks::platform::scalar
