#pragma once

#include <Platform/Math/GenericPlatformMath.h>
#include <Platform/Memory.h>

namespace rocks::platform::scalar {

// 4개 float lane의 기준 구현이다. SIMD backend가 없는 CPU의 fallback이자, SSE/NEON 구현을
// 검증하는 차등 oracle이다. 모든 CPU에서 항상 컴파일된다.
//
// 연산 순서 계약(모든 backend 공통):
// - MultiplyAdd(a, b, c)는 a*b를 먼저 반올림한 뒤 c를 더한다. FMA를 사용하지 않는다.
// - Dot3 = (p0 + p1) + p2, Dot4 = (p0 + p1) + (p2 + p3)이며 pi = ai * bi다.
// - 행렬 곱과 변환은 행렬의 행(16-float 배열의 4개 묶음) 순서대로 ((r0*s0 + r1*s1) + r2*s2) +
//   r3*s3를 누적한다.
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
            platform::BitCast<float>(platform::BitCast<uint32>(a.lane[i]) ^ 0x80000000u);
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

// xyz의 표준 외적 (a.y·b.z − a.z·b.y, a.z·b.x − a.x·b.z, a.x·b.y − a.y·b.x)이다(UE 외적과
// 같은 식 — 왼손 좌표계에서 Forward × Right = Up). w는 입력과 무관하게 +0이다.
inline Register4f Cross3(Register4f a, Register4f b) noexcept {
    const float yz = a.lane[1] * b.lane[2];
    const float zy = a.lane[2] * b.lane[1];
    const float zx = a.lane[2] * b.lane[0];
    const float xz = a.lane[0] * b.lane[2];
    const float xy = a.lane[0] * b.lane[1];
    const float yx = a.lane[1] * b.lane[0];
    return Register4f{{yz - zy, zx - xz, xy - yx, 0.0f}};
}

// (x, y, z, w) 순서 quaternion의 Hamilton 곱 a*b다. 회전으로는 b를 먼저, a를 나중에
// 적용한다(UE FQuat 곱과 같은 식·순서).
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

// 행 우선 4x4 행렬 M(m[row * 4 + column] = M[row][column])과 행 벡터 v의 곱 v · M이다
// (Core Matrix4·UE FMatrix 규약, UE VectorTransformVector와 같은 식).
// result[j] = ((v0*M[0][j] + v1*M[1][j]) + v2*M[2][j]) + v3*M[3][j]
inline Register4f TransformVector4(const float* matrix, Register4f vector) noexcept {
    const Register4f c0 = Multiply(Load(matrix + 0), Splat(vector.lane[0]));
    const Register4f c1 = Multiply(Load(matrix + 4), Splat(vector.lane[1]));
    const Register4f c2 = Multiply(Load(matrix + 8), Splat(vector.lane[2]));
    const Register4f c3 = Multiply(Load(matrix + 12), Splat(vector.lane[3]));
    return Add(Add(Add(c0, c1), c2), c3);
}

// 행 우선 행렬 곱 output = b * a다(행 벡터 규약: b를 먼저, a를 나중에 적용한다). output의 행 r은
// b의 행 r을 행 벡터로 a에 곱한 TransformVector4(a, b[r])다. Core Matrix4의 A * B(A 먼저)는
// MatrixMultiply(B, A)로 부른다. output은 a 또는 b와 같은 저장소여도 된다.
inline void MatrixMultiply(const float* a, const float* b, float* output) noexcept {
    Register4f rows[4];
    for (int row = 0; row < 4; ++row) {
        rows[row] = TransformVector4(a, Load(b + row * 4));
    }
    for (int row = 0; row < 4; ++row) {
        Store(output + row * 4, rows[row]);
    }
}

// ---- 비교·mask·선택(SoA 일괄 판정용) ----
// 비교 결과는 lane마다 모든 bit가 1(참) 또는 0(거짓)인 mask다. 같은 Register4f 타입으로 돌려주며
// BitAnd·BitOr·BitXor·BitAndNot으로 조합하고 Select·MoveMask로 소비한다. IEEE 순서 비교이므로
// 어느 쪽이든 NaN이면 모든 비교가 거짓이고, -0과 +0은 같다(C++ 비교 연산자와 같은 규약). mask가
// 아닌 값에 bit 연산·Select를 써도 bit 단위로 정의된다. 모든 backend가 bit 단위로 같은 결과를 낸다.
//
// 사용 예(4개 AABB와 질의 박스의 x축 겹침):
//   const Register4f overlap = BitAnd(CompareLessEqual(minX, Splat(queryMaxX)),
//                                     CompareGreaterEqual(maxX, Splat(queryMinX)));
//   uint32 hits = MoveMask(overlap);   // bit i가 lane i의 결과다
//   while (hits != 0) { ...; hits &= hits - 1; }

inline float MaskLane(bool value) noexcept {
    return platform::BitCast<float>(value ? NumericLimits<uint32>::Max() : 0u);
}

// lane별 a == b다.
inline Register4f CompareEqual(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = MaskLane(a.lane[i] == b.lane[i]);
    }
    return result;
}

// lane별 a < b다.
inline Register4f CompareLess(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = MaskLane(a.lane[i] < b.lane[i]);
    }
    return result;
}

// lane별 a <= b다.
inline Register4f CompareLessEqual(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = MaskLane(a.lane[i] <= b.lane[i]);
    }
    return result;
}

// lane별 a > b다.
inline Register4f CompareGreater(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = MaskLane(a.lane[i] > b.lane[i]);
    }
    return result;
}

// lane별 a >= b다.
inline Register4f CompareGreaterEqual(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = MaskLane(a.lane[i] >= b.lane[i]);
    }
    return result;
}

// bit 단위 a & b다.
inline Register4f BitAnd(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = platform::BitCast<float>(platform::BitCast<uint32>(a.lane[i]) &
                                                  platform::BitCast<uint32>(b.lane[i]));
    }
    return result;
}

// bit 단위 a | b다.
inline Register4f BitOr(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = platform::BitCast<float>(platform::BitCast<uint32>(a.lane[i]) |
                                                  platform::BitCast<uint32>(b.lane[i]));
    }
    return result;
}

// bit 단위 a ^ b다.
inline Register4f BitXor(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = platform::BitCast<float>(platform::BitCast<uint32>(a.lane[i]) ^
                                                  platform::BitCast<uint32>(b.lane[i]));
    }
    return result;
}

// bit 단위 a & ~b다(인자 순서에 주의: b 쪽을 뒤집는다).
inline Register4f BitAndNot(Register4f a, Register4f b) noexcept {
    Register4f result;
    for (int i = 0; i < 4; ++i) {
        result.lane[i] = platform::BitCast<float>(platform::BitCast<uint32>(a.lane[i]) &
                                                  ~platform::BitCast<uint32>(b.lane[i]));
    }
    return result;
}

// bit 단위 (mask & a) | (~mask & b)다. 비교 mask면 lane별 mask ? a : b다.
inline Register4f Select(Register4f mask, Register4f a, Register4f b) noexcept {
    return BitOr(BitAnd(mask, a), BitAndNot(b, mask));
}

// lane i의 부호 bit를 결과의 bit i로 모은다(0–15). 비교 mask면 참인 lane의 bit가 1이다.
inline uint32 MoveMask(Register4f value) noexcept {
    uint32 bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits |= (platform::BitCast<uint32>(value.lane[i]) >> 31) << i;
    }
    return bits;
}

} // namespace rocks::platform::scalar
