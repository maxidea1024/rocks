// 엔진 셰이더 PS probe 공용 머리다(Docs/Design/EngineShaders 7.6절, 결정 SLD20). 시험 사례 하나가 픽셀
// 하나다. 사례 번호 = (타일 원점 y + 픽셀 y) × 논리 폭 + 타일 원점 x + 픽셀 x이다(장치 한도보다 넓은
// 배치는 타일로 나눠 돈다).
//
// 입력 생성기는 라이브러리와 독립이다(SLD20): 사례 번호·흐름 번호·seed를 정수 섞기(Wellons lowbias32
// 상수)로 32 bit로 만들고 상위 24 bit × 2^-24로 [0, 1)을 만든다. 범위 변환은 2의 거듭제곱 배율과
// 2의 거듭제곱 빼기뿐이라 정확하다 — CPU(EngineShaderReference)가 같은 식으로 bit 단위로 같은 입력을
// 만든다. 정규화한 방향처럼 정확하지 않은 입력은 CPU가 같은 정확한 원재료에서 double로 다시 만든다.
#pragma once

#include "/Plugin/EngineShaderTests/Shared/ProbeFunctionIds.h"

// C++ 구조체는 Sandbox EngineShaderSelfTest.cpp ProbeConstants다(96 byte).
struct ProbeConstants {
    // x: 함수(하위 16 bit) | 선택자(상위 16 bit), y: 논리 폭, z: 타일 원점 x | y << 16, w: seed
    uint4 control;
    // 함수별 실수 인자
    float4 parameters;
    // 함수별 행렬·추가 인자(행 우선)
    float4x4 values;
};

ROCKS_ROOT_CONSTANTS(ProbeConstants, Probe);

static const uint ProbeLowMask = 0xffffu;
static const uint ProbeCaseMultiplier = 0x9e3779b1u;
static const uint ProbeSeedMultiplier = 0x632be5abu;
static const uint ProbeMixMultiplier0 = 0x7feb352du;
static const uint ProbeMixMultiplier1 = 0x846ca68bu;
static const float ProbeUnitScale = 5.9604644775390625e-8;
// 특수 실수 표의 크기다.
static const uint ProbeSpecialCount = 16u;

// 특수 실수 bit 패턴(비정규수는 넣지 않는다 — GPU가 0으로 내릴 수 있다)이다. C++ ProbeSpecialBits와
// 같은 순서다.
static const uint ProbeSpecialBits[16] = {
    0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0x00800000u, 0x7f7fffffu, 0x7f800000u,
    0xff800000u, 0x7fc00000u, 0x0d000000u, 0x477fe000u, 0x477ff000u, 0x38800000u, 0x33800000u, 0x33000000u};

uint ProbeFunction() {
    return Probe.control.x & ProbeLowMask;
}

uint ProbeSelector() {
    return Probe.control.x >> 16;
}

uint ProbeWidth() {
    return Probe.control.y;
}

uint ProbeCase(float4 position) {
    const uint2 pixel = uint2(position.xy);
    const uint2 origin = uint2(Probe.control.z & ProbeLowMask, Probe.control.z >> 16);
    return (origin.y + pixel.y) * Probe.control.y + origin.x + pixel.x;
}

// 논리 배치 안의 (x, y)다.
uint2 ProbeCoordinates(uint caseIndex) {
    return uint2(caseIndex % Probe.control.y, caseIndex / Probe.control.y);
}

uint ProbeMix(uint x) {
    x ^= x >> 16u;
    x *= ProbeMixMultiplier0;
    x ^= x >> 15u;
    x *= ProbeMixMultiplier1;
    x ^= x >> 16u;
    return x;
}

// 사례·흐름의 32 bit 입력이다.
uint ProbeBits(uint caseIndex, uint stream) {
    return ProbeMix(caseIndex * ProbeCaseMultiplier ^ ProbeMix(stream + Probe.control.w * ProbeSeedMultiplier));
}

// [0, 1)의 정확한 입력(24 bit)이다.
float ProbeUnit(uint caseIndex, uint stream) {
    return (float)(ProbeBits(caseIndex, stream) >> 8u) * ProbeUnitScale;
}

// 2^exponent(정확, exponent ∈ [-126, 127])다.
float ProbePowerOfTwo(int exponent) {
    return asfloat((uint)(exponent + 127) << 23);
}

// [-2^exponent, 2^exponent)의 정확한 입력이다.
float ProbeSigned(uint caseIndex, uint stream, int exponent) {
    precise float scaled = ProbeUnit(caseIndex, stream) * ProbePowerOfTwo(exponent + 1);
    precise float shifted = scaled - ProbePowerOfTwo(exponent);
    return shifted;
}

// 성분이 [-1, 1)인 정확한 벡터다(정규화하지 않는다).
float3 ProbeVector(uint caseIndex, uint stream) {
    return float3(ProbeSigned(caseIndex, stream, 0), ProbeSigned(caseIndex, stream + 1u, 0),
                  ProbeSigned(caseIndex, stream + 2u, 0));
}

// 단위 방향이다(ProbeVector를 정규화한다. 길이 제곱이 1/64 미만이면 +Z — CPU가 같은 규칙을 double로
// 따른다).
float3 ProbeDirection(uint caseIndex, uint stream) {
    const float3 v = ProbeVector(caseIndex, stream);
    const float lengthSquared = dot(v, v);
    return lengthSquared < (1.0 / 64.0) ? float3(0.0, 0.0, 1.0) : v * rsqrt(lengthSquared);
}

// 넓은 지수 범위의 양의 정규 실수다(지수 [2^-20, 2^20), 가수 임의 — 정확).
float ProbeWidePositive(uint caseIndex, uint stream) {
    const uint bits = ProbeBits(caseIndex, stream);
    const uint exponent = 107u + (bits >> 27) + ((bits >> 22) & 0x1fu) % 9u;
    return asfloat((exponent << 23) | (bits & 0x7fffffu));
}

// 특수 값이 앞 ProbeSpecialCount개 사례에 오는 bit 패턴 입력이다(그 뒤는 정규수·0만 — 비정규수는
// 지수를 1로 올린다).
uint ProbeFloatBits(uint caseIndex, uint stream) {
    uint bits = ProbeBits(caseIndex, stream);
    if ((bits & 0x7f800000u) == 0u && (bits & 0x7fffffu) != 0u) {
        bits |= 0x00800000u;
    }

    const uint special = ProbeSpecialBits[min(caseIndex, ProbeSpecialCount - 1u)];
    return caseIndex < ProbeSpecialCount ? special : bits;
}
