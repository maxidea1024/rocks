// compute 스레드·그룹 번호 보조다(Docs/Design/EngineShaders 4.1절 A01·14.2절 1항, UE ComputeShaderUtils.ush의
// 그룹 번호 보조 역할). CS 전용 API다(다른 단계에서 include하면 #error).
//
// 큰 1D dispatch: 그룹 수가 MaxDispatchGroupsPerAxis를 넘으면 C++(RenderBase WrapGroupCount)가
// (65535, ⌈g / 65535⌉, 1)로 감싸 낸다. 셰이더는 UnwrapLinearGroupId·UnwrapDispatchThreadId로 선형 번호를
// 되돌리고 마지막 행의 남는 그룹은 원소 수로 거른다(결정 SLD26).
//
// 사용 예([numthreads(64, 1, 1)], 원소 count개):
//   #include "/Engine/Private/Common/ComputeDispatch.rsh"
//   [numthreads(64, 1, 1)]
//   void ScaleCompute(uint3 groupId : SV_GroupID, uint groupIndex : SV_GroupIndex) {
//       const uint index = UnwrapDispatchThreadId(groupId, groupIndex, 64u);
//       if (index < count) { ... }
//   }
#pragma once

#if !defined(ROCKS_COMPUTE_SHADER)
#error "ComputeDispatch.rsh is a compute-only API (include it from a compute shader)"
#endif

#include "/Engine/Private/Math/MortonOrder.rsh"

// 모든 형식이 보장하는 축당 dispatch 그룹 수 하한(D3D12·Vulkan maxComputeWorkGroupCount 최소·GLES 3.1
// MAX_COMPUTE_WORK_GROUP_COUNT 최소)이자 큰 1D dispatch의 감싸는 폭이다.
static const uint MaxDispatchGroupsPerAxis = 65535u;

// value / divisor의 올림이다(divisor > 0, 넘침 없음 — value + divisor - 1을 만들지 않는다).
uint DivideAndRoundUp(uint value, uint divisor) {
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

uint2 DivideAndRoundUp(uint2 value, uint2 divisor) {
    return uint2(DivideAndRoundUp(value.x, divisor.x), DivideAndRoundUp(value.y, divisor.y));
}

uint3 DivideAndRoundUp(uint3 value, uint3 divisor) {
    return uint3(DivideAndRoundUp(value.x, divisor.x), DivideAndRoundUp(value.y, divisor.y),
                 DivideAndRoundUp(value.z, divisor.z));
}

// 그룹 안 스레드 좌표의 선형 번호다(x + y·sx + z·sx·sy — SV_GroupIndex와 같은 값).
uint LinearThreadIndex(uint3 groupThreadId, uint3 groupSize) {
    return groupThreadId.x + groupSize.x * (groupThreadId.y + groupSize.y * groupThreadId.z);
}

// 선형 번호 → 행 폭 groupsPerRow의 2D 좌표(l mod n, l / n)다(groupsPerRow > 0).
uint2 UnflattenGroupId2D(uint linearGroup, uint groupsPerRow) {
    return uint2(linearGroup % groupsPerRow, linearGroup / groupsPerRow);
}

// 행 폭 tilesPerRow·한 변 tileSize인 타일 격자에서 선형 타일 번호의 원점 texel이다.
uint2 TileOrigin2D(uint linearTile, uint tilesPerRow, uint tileSize) {
    return UnflattenGroupId2D(linearTile, tilesPerRow) * tileSize;
}

// 2의 거듭제곱 정사각 그룹 안의 Z 순서 좌표다(SV_GroupIndex → Morton 복호 — 이웃 스레드가 이웃 texel을
// 읽어 캐시 지역성이 좋다). 한 변 2^k 그룹은 groupIndex < 4^k다.
uint2 GroupThreadMortonCoordinate(uint groupIndex) {
    return MortonDecode2D(groupIndex);
}

// 감싼 1D dispatch의 선형 그룹 번호다(x + y · MaxDispatchGroupsPerAxis). z 축은 쓰지 않는다.
uint UnwrapLinearGroupId(uint3 groupId) {
    return groupId.x + groupId.y * MaxDispatchGroupsPerAxis;
}

// 감싼 1D dispatch의 선형 스레드 번호다(groupThreads = numthreads 곱).
uint UnwrapDispatchThreadId(uint3 groupId, uint groupIndex, uint groupThreads) {
    return UnwrapLinearGroupId(groupId) * groupThreads + groupIndex;
}

// threads개를 groupSize개씩 덮는 감싼 dispatch 인자(x, y, 1)다. threads가 0이면 (0, 1, 1)(빈
// dispatch)이다. C++ RenderBase WrapGroupCount와 같은 규칙이다.
uint3 DispatchArgumentsForThreads(uint threads, uint groupSize) {
    const uint groups = DivideAndRoundUp(threads, groupSize);
    const uint wrapped = groups > MaxDispatchGroupsPerAxis ? DivideAndRoundUp(groups, MaxDispatchGroupsPerAxis) : 1u;
    return uint3(min(groups, MaxDispatchGroupsPerAxis), wrapped, 1u);
}

// 간접 dispatch 인자(DispatchIndirectArguments — x·y·z 그룹 수)를 byteOffset(4 byte 배수)에 쓴다.
// 버퍼 자리 규칙은 C++ RenderBase ValidateDispatchArguments다.
void WriteDispatchArguments(RWByteAddressBuffer buffer, uint byteOffset, uint3 groups) {
    buffer.Store3(byteOffset, groups);
}
