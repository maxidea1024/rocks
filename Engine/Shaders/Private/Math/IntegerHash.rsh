// 정수 해시다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 결과는 모든 형식에서 bit 단위로 같다(정수
// 곱·덧셈·xor·자리 이동은 2^32 나머지로 정의된다).
// - HashPcg·HashPcg2D·3D·4D: Jarzynski, Olano, "Hash Functions for GPU Rendering", JCGT 9(3) 2020
//   (pcg — O'Neill PCG RXS-M-XS 32 출력 함수 한 번, pcg2d·pcg3d·pcg4d).
// - HashMix32: Appleby MurmurHash3 fmix32 finalizer(공개 도메인).
// - HashCombine: 시드에 값을 섞는 비대칭 결합(Boost hash_combine 식 — 황금비 상수 2^32/φ).
#pragma once

static const uint PcgMultiplier = 747796405u;
static const uint PcgIncrement = 2891336453u;
static const uint PcgOutputMultiplier = 277803737u;
// pcg2d·3d·4d의 LCG 상수(Numerical Recipes)다.
static const uint PcgVectorMultiplier = 1664525u;
static const uint PcgVectorIncrement = 1013904223u;
static const uint GoldenRatio32 = 0x9e3779b9u;

// PCG RXS-M-XS 출력 함수(32 bit 상태 → 32 bit)다.
uint PcgOutput(uint state) {
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * PcgOutputMultiplier;
    return (word >> 22u) ^ word;
}

uint HashPcg(uint value) {
    return PcgOutput(value * PcgMultiplier + PcgIncrement);
}

uint2 HashPcg2D(uint2 v) {
    v = v * PcgVectorMultiplier + PcgVectorIncrement;
    v.x += v.y * PcgVectorMultiplier;
    v.y += v.x * PcgVectorMultiplier;
    v = v ^ (v >> 16u);
    v.x += v.y * PcgVectorMultiplier;
    v.y += v.x * PcgVectorMultiplier;
    v = v ^ (v >> 16u);
    return v;
}

uint3 HashPcg3D(uint3 v) {
    v = v * PcgVectorMultiplier + PcgVectorIncrement;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v = v ^ (v >> 16u);
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

uint4 HashPcg4D(uint4 v) {
    v = v * PcgVectorMultiplier + PcgVectorIncrement;
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    v = v ^ (v >> 16u);
    v.x += v.y * v.w;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.w += v.y * v.z;
    return v;
}

uint HashMix32(uint h) {
    h ^= h >> 16u;
    h *= 0x85ebca6bu;
    h ^= h >> 13u;
    h *= 0xc2b2ae35u;
    h ^= h >> 16u;
    return h;
}

// seed에 value를 섞는다(순서가 다르면 결과가 다르다).
uint HashCombine(uint seed, uint value) {
    return seed ^ (HashMix32(value) + GoldenRatio32 + (seed << 6u) + (seed >> 2u));
}
