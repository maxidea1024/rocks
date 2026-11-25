#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Graphics/ConstantBuffer.h>

// D3D12 root signature 예산 계획이다(Docs/Design/RenderBase 9.11절 5항, ShaderSystem 7.2절 4항).
// API 객체를 모르는 순수 계산이라 CPU 시험으로 확인하고 D3D12 backend가 root signature를 만들 때
// 쓴다. 예산식: 루트 상수 DWORD + table 수 + 2 × root CBV 수 + 2 × 진단 root UAV ≤ 64.
//
// 사용 예(backend):
//   RootBudgetInput input;
//   input.stageCount = 2;
//   input.stages[0] = {1, slots.constantSlots.usedMask[0]};   // VS: SRV table 1개
//   input.stages[1] = {2, slots.constantSlots.usedMask[1]};   // PS: SRV·Sampler table
//   input.rootConstantDwords = desc.drawConstantBytes / 4;
//   auto plan = PlanRootBudget(input);   // plan.Value().rootMask[s]·tableMask[s]
namespace rocks::graphics {

// root signature 하나의 DWORD 한도다(D3D12 사양).
inline constexpr uint32 MaxRootSignatureDwords = 64;
// root descriptor(CBV·UAV) 하나의 DWORD 수다.
inline constexpr uint32 RootDescriptorDwords = 2;
// descriptor table 하나의 DWORD 수다.
inline constexpr uint32 RootTableDwords = 1;
// 계획이 받는 단계 수(그래픽 VS·PS, compute CS 하나)다.
inline constexpr uint32 MaxRootBudgetStages = 2;

// 단계 하나의 입력이다. tables는 CBV table 밖의 table 수(SRV·Sampler·UAV)이고 constantMask는 그
// 단계가 읽는 상수 버퍼 slot 마스크다.
struct RootBudgetStage {
    uint32 tables = 0;
    uint32 constantMask = 0;
};

struct RootBudgetInput {
    RootBudgetStage stages[MaxRootBudgetStages] = {};
    uint32 stageCount = 0;
    // 루트 상수 DWORD 수(파이프라인 drawConstantBytes ÷ 4)다.
    uint32 rootConstantDwords = 0;
    // PS 진단 버퍼의 root UAV가 있다(ShaderSystem 7.2절 12항).
    bool debugUav = false;
};

// 계획이다. rootMask는 root CBV로 가는 slot, tableMask는 그 단계 CBV table로 가는 slot이다.
// rootOrder는 root CBV 매개변수 순서((slot, 단계) 오름차순)의 (단계, slot)이다.
struct RootBudgetPlan {
    uint32 rootMask[MaxRootBudgetStages] = {};
    uint32 tableMask[MaxRootBudgetStages] = {};
    uint32 rootCbvCount = 0;
    uint8 rootOrderStage[MaxRootBudgetStages * MaxConstantBufferSlots] = {};
    uint8 rootOrderSlot[MaxRootBudgetStages * MaxConstantBufferSlots] = {};
    // CBV table까지 센 table 수와 쓰는 DWORD 수다.
    uint32 tableCount = 0;
    uint32 dwords = 0;
};

// 예산 안에서 root CBV를 가장 많이 두는 계획을 만든다: 모든 CBV가 root로 들어가면 CBV table이 없고,
// 아니면 넘치는 CBV가 생기는 단계마다 CBV table을 세어 식을 만족하는 가장 큰 root 수 R을 고른다.
// root는 (slot, 단계) 순 앞 R개라 각 단계에서 넘치는 것은 높은 slot이다. 결과는 입력에만 기대는
// 결정적 값이다. 실패: stageCount가 0이거나 MaxRootBudgetStages를 넘음·마스크가 slot 범위 밖·루트
// 상수가 계약 밖 InvalidArgument, root CBV 0개로도 64 DWORD를 넘으면 CapacityExceeded.
ROCKS_GRAPHICS_API Result<RootBudgetPlan> PlanRootBudget(const RootBudgetInput& input) noexcept;

} // namespace rocks::graphics
