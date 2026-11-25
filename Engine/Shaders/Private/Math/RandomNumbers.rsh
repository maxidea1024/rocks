// 난수다(Docs/Design/EngineShaders 4.2절 A02·7.3절). 정수 → [0, 1) 변환 규칙을 하나로 고정한다(상위
// 24 bit × 2^-24 — 결과가 float로 정확하고 1이 되지 않는다). 상태 있는 수열은 O'Neill, "PCG: A Family
// of Simple Fast Space-Efficient Statistically Good Algorithms for Random Number Generation", 2014의
// pcg32 one-sequence RXS-M-XS(32 bit 상태)다.
//
// 사용 예:
//   RandomPcgState rng = RandomPcgSeed(HashCombine(pixelIndex, frame));
//   float u = RandomPcgNextFloat(rng);
#pragma once

#include "/Engine/Private/Math/IntegerHash.rsh"

// 2^-24다.
static const float UnitFloatScale = 5.9604644775390625e-8;

// 상위 24 bit → [0, 1 − 2^-24]다(정확).
float UintToUnitFloat(uint value) {
    return (float)(value >> 8u) * UnitFloatScale;
}

float2 UintToUnitFloat(uint2 value) {
    return (float2)(value >> 8u) * UnitFloatScale;
}

struct RandomPcgState {
    uint state;
};

// seed(아무 값)로 수열을 시작한다(seed를 HashPcg로 섞어 상태를 만든다).
RandomPcgState RandomPcgSeed(uint seed) {
    RandomPcgState rng;
    rng.state = HashPcg(seed);
    return rng;
}

// 다음 32 bit 값이다(옛 상태의 출력, 상태는 LCG 한 걸음).
uint RandomPcgNextUint(inout RandomPcgState rng) {
    const uint old = rng.state;
    rng.state = old * PcgMultiplier + PcgIncrement;
    return PcgOutput(old);
}

// 다음 [0, 1) 값이다.
float RandomPcgNextFloat(inout RandomPcgState rng) {
    return UintToUnitFloat(RandomPcgNextUint(rng));
}
