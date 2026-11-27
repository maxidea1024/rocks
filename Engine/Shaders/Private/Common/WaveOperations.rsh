// 그룹 단위 합·최소·최대·prefix·broadcast다(Docs/Design/EngineShaders 4.1절 A01·14.2절 2항, 결정 SLD5·SLD25,
// UE WaveOpUtil.ush·WaveBroadcastIntrinsics.ush의 그룹 단위 판). CS 전용 API다.
//
// 결과는 웨이브가 아니라 그룹(numthreads 전체) 단위로 정의한다. 형식 기능 waveOperations가 있는 형식
// (지금 D3D12_SM6)은 웨이브 intrinsic + 웨이브 사이 groupshared 합성, 나머지는 groupshared만으로 같은
// 결과를 낸다: 정수 함수(float 최소·최대 포함)는 두 경로 bit 일치, float 합은 더하는 순서가 달라 허용
// 오차 안이다. 웨이브 크기는 가정하지 않는다(WaveGetLaneCount). 웨이브는 연속한 SV_GroupIndex 구간이라고
// 본다(D3D12 구현·UE 원문과 같은 가정 — Sandbox 엔진 셰이더 probe가 장치마다 확인한다).
//
// 쓰는 규칙:
// - include 전에 GROUP_OPERATION_THREADS(그룹 스레드 수 1–1024 = numthreads 곱)를 정의한다.
// - 그룹의 모든 스레드가 균일한 흐름에서 같은 순서로 부른다(함수 안에 barrier가 있다). 인자 groupIndex는
//   SV_GroupIndex다.
// - 이 파일이 groupshared uint GroupOperationScratch[2 × 스레드 수]를 선언한다(1024 스레드에서 8 KiB).
// - 시험은 GROUP_OPERATION_FORCE_GROUPSHARED = 1로 웨이브 형식에서도 groupshared 경로를 고른다(정의하지
//   않으면 0).
//
// 사용 예:
//   #define GROUP_OPERATION_THREADS 64
//   #include "/Engine/Private/Common/WaveOperations.rsh"
//   [numthreads(64, 1, 1)]
//   void CountCompute(uint groupIndex : SV_GroupIndex, uint3 groupId : SV_GroupID) {
//       const uint offset = GroupPrefixSumUint(isVisible ? 1u : 0u, groupIndex);
//       const uint total = GroupSumUint(isVisible ? 1u : 0u, groupIndex);
//   }
#pragma once

#if !defined(ROCKS_COMPUTE_SHADER)
#error "WaveOperations.rsh is a compute-only API (include it from a compute shader)"
#endif

#if !defined(GROUP_OPERATION_THREADS)
#error "define GROUP_OPERATION_THREADS (the numthreads product) before including WaveOperations.rsh"
#endif

#if GROUP_OPERATION_THREADS < 1 || GROUP_OPERATION_THREADS > 1024
#error "GROUP_OPERATION_THREADS must be in [1, 1024]"
#endif

#if !defined(GROUP_OPERATION_FORCE_GROUPSHARED)
#define GROUP_OPERATION_FORCE_GROUPSHARED 0
#endif

#include "/Engine/Public/ShaderFeatures.rsh"

#if ROCKS_FEATURE_WAVE_OPERATIONS && !GROUP_OPERATION_FORCE_GROUPSHARED
#define GROUP_OPERATION_USE_WAVE 1
#else
#define GROUP_OPERATION_USE_WAVE 0
#endif

// 그룹 스레드 수와, 그보다 작은 가장 큰 2의 거듭제곱(트리 축소의 첫 폭 — 스레드 하나면 0)이다.
static const uint GroupOperationThreads = GROUP_OPERATION_THREADS;
static const uint GroupOperationFirstStride =
    GroupOperationThreads > 512u ? 512u
    : GroupOperationThreads > 256u ? 256u
    : GroupOperationThreads > 128u ? 128u
    : GroupOperationThreads > 64u  ? 64u
    : GroupOperationThreads > 32u  ? 32u
    : GroupOperationThreads > 16u  ? 16u
    : GroupOperationThreads > 8u   ? 8u
    : GroupOperationThreads > 4u   ? 4u
    : GroupOperationThreads > 2u   ? 2u
    : GroupOperationThreads > 1u   ? 1u
                                   : 0u;

// 합치기 연산 번호다(내부).
static const uint GroupOperationSumUint = 0u;
static const uint GroupOperationMinUint = 1u;
static const uint GroupOperationMaxUint = 2u;
static const uint GroupOperationSumFloat = 3u;

// 부호 bit 위치다.
static const uint GroupOperationSignBit = 0x80000000u;

// scratch 두 벌(prefix scan이 번갈아 쓴다 — 합·최소·최대·broadcast는 앞 벌만)이다.
groupshared uint GroupOperationScratch[GROUP_OPERATION_THREADS * 2];

// float → 순서 보존 uint(음수는 전체 반전, 양수는 부호 bit 켜기)다. 정수 비교가 float 순서와 같고 −0 < +0이다.
// NaN은 정의하지 않는다.
uint FloatToOrderedUint(float value) {
    const uint bits = asuint(value);
    return (bits & GroupOperationSignBit) != 0u ? ~bits : (bits | GroupOperationSignBit);
}

// FloatToOrderedUint의 역이다.
float OrderedUintToFloat(uint ordered) {
    return asfloat((ordered & GroupOperationSignBit) != 0u ? (ordered & ~GroupOperationSignBit) : ~ordered);
}

// 두 값을 연산 번호로 합친다(float 합은 bit 표현으로 받는다).
uint GroupOperationCombine(uint a, uint b, uint operation) {
    const uint sum = operation == GroupOperationSumFloat ? asuint(asfloat(a) + asfloat(b)) : a + b;
    const uint extreme = operation == GroupOperationMinUint ? min(a, b) : max(a, b);
    return operation == GroupOperationSumUint || operation == GroupOperationSumFloat ? sum : extreme;
}

#if GROUP_OPERATION_USE_WAVE

// 웨이브 안을 줄인 값이다(웨이브의 모든 활성 lane이 같은 값을 받는다).
uint GroupOperationWaveReduce(uint value, uint operation) {
    uint reduced = 0u;
    if (operation == GroupOperationSumUint) {
        reduced = WaveActiveSum(value);
    } else if (operation == GroupOperationMinUint) {
        reduced = WaveActiveMin(value);
    } else if (operation == GroupOperationMaxUint) {
        reduced = WaveActiveMax(value);
    } else {
        reduced = asuint(WaveActiveSum(asfloat(value)));
    }

    return reduced;
}

// 웨이브 경로 축소다: 웨이브 첫 lane이 웨이브 결과를 scratch[웨이브 번호]에 쓰고 모든 스레드가 번호 순서로
// 합친다.
uint GroupOperationReduce(uint value, uint groupIndex, uint operation) {
    const uint laneCount = WaveGetLaneCount();
    const uint waveValue = GroupOperationWaveReduce(value, operation);
    if (WaveIsFirstLane()) {
        GroupOperationScratch[groupIndex / laneCount] = waveValue;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint waveCount = (GroupOperationThreads + laneCount - 1u) / laneCount;
    uint result = GroupOperationScratch[0];
    for (uint wave = 1u; wave < waveCount; ++wave) {
        result = GroupOperationCombine(result, GroupOperationScratch[wave], operation);
    }

    GroupMemoryBarrierWithGroupSync();
    return result;
}

// 웨이브 경로 배타 prefix 합이다(자기 웨이브 앞 웨이브 합 + 웨이브 안 prefix).
uint GroupOperationExclusivePrefixSum(uint value, uint groupIndex) {
    const uint laneCount = WaveGetLaneCount();
    const uint wavePrefix = WavePrefixSum(value);
    const uint waveTotal = WaveActiveSum(value);
    const uint waveIndex = groupIndex / laneCount;
    if (WaveIsFirstLane()) {
        GroupOperationScratch[waveIndex] = waveTotal;
    }

    GroupMemoryBarrierWithGroupSync();
    uint before = 0u;
    for (uint wave = 0u; wave < waveIndex; ++wave) {
        before += GroupOperationScratch[wave];
    }

    GroupMemoryBarrierWithGroupSync();
    return before + wavePrefix;
}

#else

// groupshared 경로 축소다: 첫 폭부터 반씩 접는 트리(스레드 수가 2의 거듭제곱이 아니어도 된다).
uint GroupOperationReduce(uint value, uint groupIndex, uint operation) {
    GroupOperationScratch[groupIndex] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = GroupOperationFirstStride; stride > 0u; stride >>= 1u) {
        if (groupIndex < stride && groupIndex + stride < GroupOperationThreads) {
            GroupOperationScratch[groupIndex] = GroupOperationCombine(
                GroupOperationScratch[groupIndex], GroupOperationScratch[groupIndex + stride], operation);
        }

        GroupMemoryBarrierWithGroupSync();
    }

    const uint result = GroupOperationScratch[0];
    GroupMemoryBarrierWithGroupSync();
    return result;
}

// groupshared 경로 배타 prefix 합이다: Hillis–Steele 포함 scan(scratch 두 벌을 번갈아)에서 자기 값을 뺀다
// (uint 감산은 mod 2^32라 포함 합과 같은 정보다).
uint GroupOperationExclusivePrefixSum(uint value, uint groupIndex) {
    GroupOperationScratch[groupIndex] = value;
    GroupMemoryBarrierWithGroupSync();
    uint source = 0u;
    [unroll] for (uint offset = 1u; offset < GroupOperationThreads; offset <<= 1u) {
        uint sum = GroupOperationScratch[source + groupIndex];
        if (groupIndex >= offset) {
            sum += GroupOperationScratch[source + groupIndex - offset];
        }

        GroupOperationScratch[GroupOperationThreads - source + groupIndex] = sum;
        GroupMemoryBarrierWithGroupSync();
        source = GroupOperationThreads - source;
    }

    const uint inclusive = GroupOperationScratch[source + groupIndex];
    GroupMemoryBarrierWithGroupSync();
    return inclusive - value;
}

#endif

// 그룹 모든 스레드 값의 합이다(mod 2^32).
uint GroupSumUint(uint value, uint groupIndex) {
    return GroupOperationReduce(value, groupIndex, GroupOperationSumUint);
}

// 그룹 모든 스레드 값의 float 합이다(더하는 순서는 경로마다 다르다 — 결과는 허용 오차 안에서 같다).
float GroupSumFloat(float value, uint groupIndex) {
    return asfloat(GroupOperationReduce(asuint(value), groupIndex, GroupOperationSumFloat));
}

uint GroupMinUint(uint value, uint groupIndex) {
    return GroupOperationReduce(value, groupIndex, GroupOperationMinUint);
}

uint GroupMaxUint(uint value, uint groupIndex) {
    return GroupOperationReduce(value, groupIndex, GroupOperationMaxUint);
}

// float 최소·최대다(순서 보존 uint로 비교 — 두 경로 bit 일치, −0 < +0, NaN 입력은 정의하지 않는다).
float GroupMinFloat(float value, uint groupIndex) {
    return OrderedUintToFloat(GroupOperationReduce(FloatToOrderedUint(value), groupIndex, GroupOperationMinUint));
}

float GroupMaxFloat(float value, uint groupIndex) {
    return OrderedUintToFloat(GroupOperationReduce(FloatToOrderedUint(value), groupIndex, GroupOperationMaxUint));
}

// 배타 prefix 합(SV_GroupIndex가 더 작은 스레드 값의 합, mod 2^32)이다.
uint GroupPrefixSumUint(uint value, uint groupIndex) {
    return GroupOperationExclusivePrefixSum(value, groupIndex);
}

// SV_GroupIndex 0 스레드의 값이다.
uint GroupBroadcastFirstUint(uint value, uint groupIndex) {
    if (groupIndex == 0u) {
        GroupOperationScratch[0] = value;
    }

    GroupMemoryBarrierWithGroupSync();
    const uint result = GroupOperationScratch[0];
    GroupMemoryBarrierWithGroupSync();
    return result;
}

float GroupBroadcastFirstFloat(float value, uint groupIndex) {
    return asfloat(GroupBroadcastFirstUint(asuint(value), groupIndex));
}
