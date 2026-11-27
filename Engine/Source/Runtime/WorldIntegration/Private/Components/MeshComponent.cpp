#include <Collision/BodySettings.generated.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/ComponentValueChecks.h>
#include <World/Primitives/PrimitiveComponent.generated.h>
#include <World/SceneComponent.generated.h>
#include <WorldIntegration/Components/MeshComponent.generated.h>
#include <WorldIntegration/Components/MeshComponent.h>

// 메시 컴포넌트 기반이다(Docs/Design/EngineRuntime 30.2절, D2b). 머티리얼 override 데이터만 있고
// 해석은 렌더 proxy(RS1)다.
namespace rocks {

const SoftObjectRef<MaterialBase>* MeshComponent::OverrideMaterial(int32 index) const noexcept {
    if (index < 0 || static_cast<usize>(index) >= overrideMaterials_.Size()) {
        return nullptr;
    }

    return &overrideMaterials_[static_cast<usize>(index)];
}

Result<void> MeshComponent::SetMaterial(int32 index,
                                        const SoftObjectRef<MaterialBase>& material) noexcept {
    if (index < 0 || index >= MaxMaterialSlots) {
        return component_checks::Invalid();
    }

    return AssignSlot(overrideMaterials_, static_cast<usize>(index), material);
}

Result<void>
MeshComponent::SetOverlayMaterial(const SoftObjectRef<MaterialBase>& material) noexcept {
    if (auto valid = material.Validate(); !valid) {
        return valid;
    }

    if (overlayMaterial_ == material) {
        return Result<void>::Success();
    }

    if (auto assigned = overlayMaterial_.TryAssign(material); !assigned) {
        return assigned;
    }

    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

const SoftObjectRef<MaterialBase>* MeshComponent::SlotOverlayMaterial(int32 slot) const noexcept {
    if (slot < 0 || static_cast<usize>(slot) >= materialSlotsOverlayMaterial_.Size()) {
        return nullptr;
    }

    return &materialSlotsOverlayMaterial_[static_cast<usize>(slot)];
}

Result<void>
MeshComponent::SetSlotOverlayMaterial(int32 slot,
                                      const SoftObjectRef<MaterialBase>& material) noexcept {
    // UE SetOverlayMaterial(MeshComponent.cpp:369)은 있는 원소는 쓰고 slot < GetNumMaterials일 때만
    // 늘린다.
    const bool exists =
        slot >= 0 && static_cast<usize>(slot) < materialSlotsOverlayMaterial_.Size();
    if (slot < 0 || (!exists && static_cast<usize>(slot) >= SlotOverlayLimit())) {
        return component_checks::Invalid();
    }

    return AssignSlot(materialSlotsOverlayMaterial_, static_cast<usize>(slot), material);
}

Result<void> MeshComponent::SetOverlayMaterialMaxDrawDistance(float distance) noexcept {
    if (!component_checks::NonNegative(distance)) {
        return component_checks::Invalid();
    }

    Change(overlayMaterialMaxDrawDistance_, distance);
    return Result<void>::Success();
}

Result<void> MeshComponent::AssignSlot(Array<SoftObjectRef<MaterialBase>>& slots, usize index,
                                       const SoftObjectRef<MaterialBase>& material) noexcept {
    if (auto valid = material.Validate(); !valid) {
        return valid;
    }

    if (index < slots.Size() && slots[index] == material) {
        return Result<void>::Success();
    }

    // 할당할 수 있는 것을 먼저 모두 마친다(실패하면 배열·값이 그대로다).
    Allocator& allocator = slots.GetAllocator();
    auto copy = material.TryClone(allocator);
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }

    if (auto reserved = slots.TryReserve(index + 1); !reserved) {
        return reserved;
    }

    while (slots.Size() <= index) {
        (void)slots.TryPushBack(SoftObjectRef<MaterialBase>(allocator));
    }

    slots[index] = Move(copy.Value());
    MarkRenderStateDirtyIfPresent();
    return Result<void>::Success();
}

} // namespace rocks
