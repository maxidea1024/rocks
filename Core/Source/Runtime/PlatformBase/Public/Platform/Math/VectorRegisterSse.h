#pragma once

#include <Base/Compiler.h>

// x64 전용 구현이다. 다른 CPU에서 단독 include하면 아무 선언도 만들지 않는다.
#if ROCKS_ARCH_X64

#include <Base/Types.h>

#include <emmintrin.h>

namespace rocks::platform::sse {

// SSE2 4-lane float 구현이다. 함수 목록·연산 순서·특수값 규약은 VectorRegisterScalar.h와 같다.
// Load/Store는 정렬되지 않은 주소를 허용한다. SSE4.1·AVX·FMA 명령은 사용하지 않는다.
using Register4f = __m128;

inline Register4f Load(const float* source) noexcept {
    return _mm_loadu_ps(source);
}

inline void Store(float* destination, Register4f value) noexcept {
    _mm_storeu_ps(destination, value);
}

inline Register4f Set(float x, float y, float z, float w) noexcept {
    return _mm_setr_ps(x, y, z, w);
}

inline Register4f Splat(float value) noexcept {
    return _mm_set1_ps(value);
}

inline Register4f Zero() noexcept {
    return _mm_setzero_ps();
}

template <int Index>
inline float Get(Register4f value) noexcept {
    static_assert(Index >= 0 && Index < 4);
    return _mm_cvtss_f32(_mm_shuffle_ps(value, value, _MM_SHUFFLE(Index, Index, Index, Index)));
}

inline Register4f Add(Register4f a, Register4f b) noexcept {
    return _mm_add_ps(a, b);
}

inline Register4f Subtract(Register4f a, Register4f b) noexcept {
    return _mm_sub_ps(a, b);
}

inline Register4f Multiply(Register4f a, Register4f b) noexcept {
    return _mm_mul_ps(a, b);
}

inline Register4f Divide(Register4f a, Register4f b) noexcept {
    return _mm_div_ps(a, b);
}

inline Register4f MultiplyAdd(Register4f a, Register4f b, Register4f c) noexcept {
    return _mm_add_ps(_mm_mul_ps(a, b), c);
}

inline Register4f Negate(Register4f a) noexcept {
    return _mm_xor_ps(a, _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u))));
}

inline Register4f Abs(Register4f a) noexcept {
    return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
}

// minps/maxps의 결과는 (a < b) ? a : b, (a > b) ? a : b다. scalar 기준 규약과 같다.
inline Register4f Min(Register4f a, Register4f b) noexcept {
    return _mm_min_ps(a, b);
}

inline Register4f Max(Register4f a, Register4f b) noexcept {
    return _mm_max_ps(a, b);
}

inline Register4f Sqrt(Register4f a) noexcept {
    return _mm_sqrt_ps(a);
}

inline float Dot3(Register4f a, Register4f b) noexcept {
    const __m128 product = _mm_mul_ps(a, b);
    const __m128 first =
        _mm_add_ss(product, _mm_shuffle_ps(product, product, _MM_SHUFFLE(1, 1, 1, 1)));
    return _mm_cvtss_f32(
        _mm_add_ss(first, _mm_shuffle_ps(product, product, _MM_SHUFFLE(2, 2, 2, 2))));
}

inline float Dot4(Register4f a, Register4f b) noexcept {
    const __m128 product = _mm_mul_ps(a, b);
    // [p1, p0, p3, p2]를 더하면 lane0 = p0+p1, lane2 = p2+p3이다.
    const __m128 pairs =
        _mm_add_ps(product, _mm_shuffle_ps(product, product, _MM_SHUFFLE(2, 3, 0, 1)));
    return _mm_cvtss_f32(_mm_add_ss(pairs, _mm_movehl_ps(pairs, pairs)));
}

inline Register4f Cross3(Register4f a, Register4f b) noexcept {
    const __m128 aYzx = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 bZxy = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2));
    const __m128 aZxy = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 1, 0, 2));
    const __m128 bYzx = _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1));
    const __m128 cross = _mm_sub_ps(_mm_mul_ps(aYzx, bZxy), _mm_mul_ps(aZxy, bYzx));
    // w lane은 입력의 NaN/Inf와 무관하게 +0으로 고정한다.
    const __m128 xyzMask = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
    return _mm_and_ps(cross, xyzMask);
}

inline Register4f QuaternionMultiply(Register4f a, Register4f b) noexcept {
    // 부호 패턴을 xor로 적용한다. 부호 반전은 정확하므로 scalar의 -b 성분과 같다.
    const __m128 signX = _mm_castsi128_ps(
        _mm_setr_epi32(0, static_cast<int>(0x80000000u), 0, static_cast<int>(0x80000000u)));
    const __m128 signY = _mm_castsi128_ps(
        _mm_setr_epi32(0, 0, static_cast<int>(0x80000000u), static_cast<int>(0x80000000u)));
    const __m128 signZ = _mm_castsi128_ps(
        _mm_setr_epi32(static_cast<int>(0x80000000u), 0, 0, static_cast<int>(0x80000000u)));
    const __m128 bWzyx = _mm_xor_ps(_mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 1, 2, 3)), signX);
    const __m128 bZwxy = _mm_xor_ps(_mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 0, 3, 2)), signY);
    const __m128 bYxwz = _mm_xor_ps(_mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 3, 0, 1)), signZ);
    const __m128 termW = _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 3, 3, 3)), b);
    const __m128 termX = _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(0, 0, 0, 0)), bWzyx);
    const __m128 termY = _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(1, 1, 1, 1)), bZwxy);
    const __m128 termZ = _mm_mul_ps(_mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 2, 2, 2)), bYxwz);
    return _mm_add_ps(_mm_add_ps(_mm_add_ps(termW, termX), termY), termZ);
}

inline Register4f TransformVector4(const float* matrix, Register4f vector) noexcept {
    const __m128 c0 = _mm_mul_ps(_mm_loadu_ps(matrix + 0),
                                 _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(0, 0, 0, 0)));
    const __m128 c1 = _mm_mul_ps(_mm_loadu_ps(matrix + 4),
                                 _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(1, 1, 1, 1)));
    const __m128 c2 = _mm_mul_ps(_mm_loadu_ps(matrix + 8),
                                 _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(2, 2, 2, 2)));
    const __m128 c3 = _mm_mul_ps(_mm_loadu_ps(matrix + 12),
                                 _mm_shuffle_ps(vector, vector, _MM_SHUFFLE(3, 3, 3, 3)));
    return _mm_add_ps(_mm_add_ps(_mm_add_ps(c0, c1), c2), c3);
}

inline void MatrixMultiply(const float* a, const float* b, float* output) noexcept {
    const __m128 c0 = TransformVector4(a, _mm_loadu_ps(b + 0));
    const __m128 c1 = TransformVector4(a, _mm_loadu_ps(b + 4));
    const __m128 c2 = TransformVector4(a, _mm_loadu_ps(b + 8));
    const __m128 c3 = TransformVector4(a, _mm_loadu_ps(b + 12));
    _mm_storeu_ps(output + 0, c0);
    _mm_storeu_ps(output + 4, c1);
    _mm_storeu_ps(output + 8, c2);
    _mm_storeu_ps(output + 12, c3);
}

} // namespace rocks::platform::sse

#endif
