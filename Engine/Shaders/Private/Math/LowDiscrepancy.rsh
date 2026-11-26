// 저불일치 수열이다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 정수부(bit 패턴)는 모든 형식에서 bit
// 단위로 같고(golden), 실수 값은 상위 24 bit × 2^-24 변환(RandomNumbers 규칙)이다.
// - RadicalInverseBase2·Hammersley: van der Corput 근역(bit 뒤집기), Hammersley 1960.
// - Halton3: 3진 근역(Halton 1960). 정확한 정수 분자·분모를 쓰므로 index < 3^15에서 float 하나 오차다.
// - SobolOwen2D: Sobol 처음 두 차원(Joe, Kuo 2008 방향수 — 1차원 vdC, 2차원 다항식 x + 1)과 Owen
//   scramble(Burley, "Practical Hash-based Owen Scrambling", JCGT 9(4) 2020 — Laine–Karras 순열,
//   index shuffle).
// - R2Sequence: Roberts, "The Unreasonable Effectiveness of Quasirandom Sequences", 2018(α = 1/φ₂의
//   거듭제곱). 실수 대신 32 bit 고정소수점으로 더해 index와 무관하게 정확하다(실수 정의와의 차이는
//   index × 2^-32 이하).
#pragma once

#include "/Engine/Private/Math/IntegerHash.rsh"
#include "/Engine/Private/Math/RandomNumbers.rsh"

// R2의 0.5 시작 오프셋(2^31)과 고정소수점 증분(α₁·2^32, α₂·2^32 반올림)이다.
static const uint R2Start = 0x80000000u;
static const uint R2SequenceStepX = 3242174889u;
static const uint R2SequenceStepY = 2447445414u;
// Halton 3진 정확 범위(3^15)다.
static const uint Halton3ExactLimit = 14348907u;
// Laine–Karras 순열 상수(Burley 2020 Listing 2)다.
static const uint LaineKarrasMultiplier0 = 0x6c50b47cu;
static const uint LaineKarrasMultiplier1 = 0xb82f1e52u;
static const uint LaineKarrasMultiplier2 = 0xc7afe638u;
static const uint LaineKarrasMultiplier3 = 0x8d22f6e6u;

// van der Corput 근역의 32 bit 고정소수점 값(bit 뒤집기)이다.
uint RadicalInverseBase2Bits(uint index) {
    return reversebits(index);
}

// [0, 1) 값이다.
float RadicalInverseBase2(uint index) {
    return UintToUnitFloat(reversebits(index));
}

// n개 Hammersley 점의 index번째(index/n, vdC(index))다.
float2 Hammersley(uint index, uint count) {
    return float2((float)index / (float)count, RadicalInverseBase2(index));
}

// 2진 Halton(= vdC)이다.
float Halton2(uint index) {
    return RadicalInverseBase2(index);
}

// 3진 근역의 정확한 (분자, 분모 3^자릿수)다. index < Halton3ExactLimit에서 둘 다 2^24 미만이다.
uint2 Halton3Fraction(uint index) {
    uint numerator = 0u;
    uint denominator = 1u;
    uint remaining = index;
    [loop] while (remaining > 0u) {
        numerator = numerator * 3u + remaining % 3u;
        denominator *= 3u;
        remaining /= 3u;
    }

    return uint2(numerator, denominator);
}

float Halton3(uint index) {
    const uint2 fraction = Halton3Fraction(index);
    return (float)fraction.x / (float)fraction.y;
}

// Laine–Karras 순열(reverse된 bit에 적용하면 Owen scramble — Burley 2020)이다.
uint LaineKarrasPermutation(uint x, uint seed) {
    x += seed;
    x ^= x * LaineKarrasMultiplier0;
    x ^= x * LaineKarrasMultiplier1;
    x ^= x * LaineKarrasMultiplier2;
    x ^= x * LaineKarrasMultiplier3;
    return x;
}

// 중첩 균일 scramble(Owen)이다.
uint NestedUniformScramble(uint x, uint seed) {
    return reversebits(LaineKarrasPermutation(reversebits(x), seed));
}

// Sobol 2차원(다항식 x + 1, 방향수 v₁ = 2^31, vₖ = vₖ₋₁ ⊕ (vₖ₋₁ >> 1))의 32 bit 값이다.
uint SobolSecondDimension(uint index) {
    uint result = 0u;
    uint direction = 0x80000000u;
    [loop] for (uint bit = 0u; bit < 32u; ++bit) {
        if (((index >> bit) & 1u) != 0u) {
            result ^= direction;
        }

        direction ^= direction >> 1u;
    }

    return result;
}

// Owen scramble한 Sobol 2D 점의 32 bit 고정소수점 값이다(Burley 2020 shuffled_scrambled_sobol2d:
// index를 scramble로 섞고 차원마다 다른 시드로 scramble). 같은 seed의 점 집합은 (0, 2)-수열의 층화를
// 지킨다.
uint2 SobolOwen2DBits(uint index, uint seed) {
    const uint shuffled = NestedUniformScramble(index, HashMix32(seed));
    const uint x = NestedUniformScramble(reversebits(shuffled), HashCombine(seed, 0u));
    const uint y = NestedUniformScramble(SobolSecondDimension(shuffled), HashCombine(seed, 1u));
    return uint2(x, y);
}

float2 SobolOwen2D(uint index, uint seed) {
    return UintToUnitFloat(SobolOwen2DBits(index, seed));
}

// R2 수열의 32 bit 고정소수점 값이다(index 0은 (0.5, 0.5)).
uint2 R2SequenceBits(uint index) {
    return uint2(R2Start + index * R2SequenceStepX, R2Start + index * R2SequenceStepY);
}

float2 R2Sequence(uint index) {
    return UintToUnitFloat(R2SequenceBits(index));
}
