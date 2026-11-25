#include <Graphics/ConstantBuffer.h>
#include <Graphics/DeviceTypes.h>
#include <Graphics/RootBudget.h>

// 상수 버퍼 배치 규칙과 D3D12 root 예산 계획이다(Docs/Design/RenderBase 9.11절).
namespace rocks::graphics {

namespace {

constexpr uint32 AllSlotsMask = (1u << MaxConstantBufferSlots) - 1;

uint32 CountBits(uint32 value) noexcept {
    uint32 count = 0;
    for (; value != 0; value &= value - 1) {
        ++count;
    }

    return count;
}

// (slot, 단계) 순 앞 root개를 root로 둘 때의 마스크를 채운다.
void SplitRoot(const RootBudgetInput& input, uint32 root, RootBudgetPlan& plan) noexcept {
    uint32 taken = 0;
    for (uint32 stage = 0; stage < MaxRootBudgetStages; ++stage) {
        plan.rootMask[stage] = 0;
        plan.tableMask[stage] = 0;
    }

    for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
        for (uint32 stage = 0; stage < input.stageCount; ++stage) {
            const uint32 bit = 1u << slot;
            if ((input.stages[stage].constantMask & bit) == 0) {
                continue;
            }

            if (taken < root) {
                plan.rootMask[stage] |= bit;
                plan.rootOrderStage[taken] = static_cast<uint8>(stage);
                plan.rootOrderSlot[taken] = static_cast<uint8>(slot);
                ++taken;
            } else {
                plan.tableMask[stage] |= bit;
            }
        }
    }

    plan.rootCbvCount = taken;
}

} // namespace

const char* ConstantBufferLifetimeName(ConstantBufferLifetime lifetime) noexcept {
    switch (lifetime) {
    case ConstantBufferLifetime::SingleFrame:
        return "SingleFrame";
    case ConstantBufferLifetime::MultiFrame:
        return "MultiFrame";
    }

    return "Unknown";
}

bool IsValidConstantBufferBytes(usize bytes) noexcept {
    return bytes != 0 && bytes % ConstantBufferGranularity == 0 && bytes <= MaxConstantBufferBytes;
}

bool AddConstantBufferSlot(ConstantBufferSlots& slots, ShaderStage stage, uint32 slot,
                           uint32 bytes) noexcept {
    const uint32 index = ConstantBufferStageIndex(stage);
    if (index >= ConstantBufferStageCount || slot >= MaxConstantBufferSlots ||
        !IsValidConstantBufferBytes(bytes) || (slots.usedMask[index] & (1u << slot)) != 0) {
        return false;
    }

    slots.usedMask[index] |= 1u << slot;
    slots.bytes[index][slot] = bytes;
    return true;
}

bool ConstantBuffersSatisfied(
    const ConstantBufferSlots& slots,
    const uint32 (&bound)[ConstantBufferStageCount][MaxConstantBufferSlots]) noexcept {
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        for (uint32 slot = 0; slot < MaxConstantBufferSlots; ++slot) {
            if ((slots.usedMask[stage] & (1u << slot)) != 0 &&
                bound[stage][slot] < slots.bytes[stage][slot]) {
                return false;
            }
        }
    }

    return true;
}

uint32 ConstantBufferCount(const ConstantBufferSlots& slots) noexcept {
    uint32 count = 0;
    for (uint32 stage = 0; stage < ConstantBufferStageCount; ++stage) {
        count += CountBits(slots.usedMask[stage]);
    }

    return count;
}

Result<RootBudgetPlan> PlanRootBudget(const RootBudgetInput& input) noexcept {
    using Outcome = Result<RootBudgetPlan>;
    if (input.stageCount == 0 || input.stageCount > MaxRootBudgetStages ||
        input.rootConstantDwords > MaxDrawConstantBytes / sizeof(uint32)) {
        return Outcome::Failure(ErrorCode::InvalidArgument);
    }

    RootBudgetPlan plan;
    uint32 tables = 0;
    uint32 total = 0;
    for (uint32 stage = 0; stage < input.stageCount; ++stage) {
        if ((input.stages[stage].constantMask & ~AllSlotsMask) != 0) {
            return Outcome::Failure(ErrorCode::InvalidArgument);
        }

        tables += input.stages[stage].tables;
        total += CountBits(input.stages[stage].constantMask);
    }

    const uint32 fixed = input.rootConstantDwords + tables * RootTableDwords +
                         (input.debugUav ? RootDescriptorDwords : 0u);
    if (fixed > MaxRootSignatureDwords) {
        return Outcome::Failure(ErrorCode::CapacityExceeded);
    }

    const uint32 budget = MaxRootSignatureDwords - fixed;
    // 넘치는 CBV가 생기는 단계마다 table 하나를 더 센다. 가장 큰 root 수부터 내려가며 맞는 첫 수를
    // 고른다(R = 0이면 모든 CBV가 table이다).
    for (uint32 root = total + 1; root-- > 0;) {
        SplitRoot(input, root, plan);
        uint32 cbvTables = 0;
        for (uint32 stage = 0; stage < input.stageCount; ++stage) {
            cbvTables += plan.tableMask[stage] != 0 ? 1u : 0u;
        }

        const uint32 needed = root * RootDescriptorDwords + cbvTables * RootTableDwords;
        if (needed <= budget) {
            plan.tableCount = tables + cbvTables;
            plan.dwords = fixed + needed;
            return Outcome::Success(plan);
        }
    }

    return Outcome::Failure(ErrorCode::CapacityExceeded);
}

} // namespace rocks::graphics
