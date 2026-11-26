#pragma once

#include <Core/Containers/Array.h>
#include <Material/Material.h>
#include <Meta/Annotations.h>
#include <Object/SoftObjectRef.h>
#include <World/Primitives/PrimitiveComponent.h>

namespace rocks {

// 머티리얼을 가진 메시 컴포넌트의 기반이다(UE UMeshComponent — Docs/Design/EngineRuntime 30.2절).
// 데이터만 있다: 머티리얼 해석(override → 메시 슬롯 머티리얼, overlay 우선순위)은 메시 슬롯 정보를
// 아는 렌더 proxy(RS1) 몫이다. 머티리얼 참조는 SoftObjectRef다(World 객체 → 에셋 — 30.1절).
//
// 규약(24.3.5절과 같다): setter는 Mobility를 보지 않는다(UE SetMaterial·SetOverlayMaterial). 값
// 검증 실패는 InvalidArgument이고 값을 바꾸지 않는다. 바뀌면 Render 다시 보내기를 표시한다. 할당
// 실패면 값을 유지한다.
//
// 사용 예:
//   world.With<StaticMeshComponent>(mesh, [&](StaticMeshComponent& component) noexcept {
//       SoftObjectRef<MaterialBase> red(allocator);
//       (void)red.TryAssign(redMaterialAsset, {});
//       return component.SetMaterial(1, red);   // 슬롯 0은 null(메시 머티리얼)로 늘어난다
//   });
//
// 스레드: World의 게임 스레드 전용이다.
ROCKS_CLASS(Abstract)
class ROCKS_WORLDINTEGRATION_API MeshComponent : public PrimitiveComponent {
    ROCKS_GENERATED_BODY()

public:
    // 슬롯 번호 상한(배타)이다(메시 머티리얼 슬롯 상한 StaticMeshMaxMaterialSlots와 같다 — 30.2절,
    // StaticMeshComponent.cpp가 static_assert로 묶는다).
    static constexpr int32 MaxMaterialSlots = 65535;

    // UE MeshComponent.cpp:27 — 메시 계열은 그림자와 occluder를 켠다.
    explicit MeshComponent(Allocator& allocator) noexcept
        : PrimitiveComponent(allocator), overrideMaterials_(allocator), overlayMaterial_(allocator),
          materialSlotsOverlayMaterial_(allocator) {
        castShadow_ = true;
        useAsOccluder_ = true;
    }

    // --- 머티리얼 override ---

    usize NumOverrideMaterials() const noexcept {
        return overrideMaterials_.Size();
    }

    // 슬롯 index의 override다(범위 밖이면 nullptr — 원소가 null 참조여도 메시 슬롯 머티리얼을
    // 쓴다).
    const SoftObjectRef<MaterialBase>* OverrideMaterial(int32 index) const noexcept;

    // 슬롯 index의 override를 정한다(UE SetMaterial — 짧으면 null로 늘린다, MeshComponent.cpp:76).
    // 오류: index < 0 또는 ≥ MaxMaterialSlots, 잘못된 soft 참조 값(InvalidArgument), 할당 실패.
    // 같은 값이면 아무것도 하지 않는다.
    Result<void> SetMaterial(int32 index, const SoftObjectRef<MaterialBase>& material) noexcept;

    // 메시 슬롯 수다(UE GetNumMaterials — 기반은 0, StaticMeshComponent는 로드 정보의 슬롯 수).
    // override 수는 NumOverrideMaterials다.
    virtual usize NumMaterials() const noexcept {
        return 0;
    }

    // --- overlay ---

    const SoftObjectRef<MaterialBase>& OverlayMaterial() const noexcept {
        return overlayMaterial_;
    }

    // 오류: 잘못된 soft 참조 값, 할당 실패.
    Result<void> SetOverlayMaterial(const SoftObjectRef<MaterialBase>& material) noexcept;

    usize NumSlotOverlayMaterials() const noexcept {
        return materialSlotsOverlayMaterial_.Size();
    }

    // 슬롯 slot의 overlay override다(범위 밖이면 nullptr — 전체 overlay를 쓴다).
    const SoftObjectRef<MaterialBase>* SlotOverlayMaterial(int32 slot) const noexcept;

    // 슬롯 overlay를 정한다(UE SetOverlayMaterial(…, true, slot)). 이미 있는 slot은 그대로 쓰고,
    // 짧으면 slot < SlotOverlayLimit()일 때만 slot + 1로 늘린다(UE MeshComponent.cpp:369 — 늘릴
    // 때만 GetNumMaterials를 본다, 30.2절). 오류: slot < 0, 늘려야 하는데 상한 밖, 잘못된 soft 참조
    // 값, 할당 실패.
    Result<void> SetSlotOverlayMaterial(int32 slot,
                                        const SoftObjectRef<MaterialBase>& material) noexcept;

    float OverlayMaterialMaxDrawDistance() const noexcept {
        return overlayMaterialMaxDrawDistance_;
    }

    // 0이면 primitive 최대 거리로 culling한다(cm). 오류: 음수·비유한.
    Result<void> SetOverlayMaterialMaxDrawDistance(float distance) noexcept;

protected:
    // 슬롯 overlay 배열을 늘릴 수 있는 슬롯 수 상한(배타)이다(기반은 NumMaterials — UE와 같다).
    virtual usize SlotOverlayLimit() const noexcept {
        return NumMaterials();
    }

private:
    // slots[index]를 material로 정한다(짧으면 null로 늘린다).
    Result<void> AssignSlot(Array<SoftObjectRef<MaterialBase>>& slots, usize index,
                            const SoftObjectRef<MaterialBase>& material) noexcept;

    ROCKS_PROPERTY(ReadOnly, Name = "overrideMaterials")
    Array<SoftObjectRef<MaterialBase>> overrideMaterials_;
    ROCKS_PROPERTY(ReadOnly, Name = "overlayMaterial") SoftObjectRef<MaterialBase> overlayMaterial_;
    ROCKS_PROPERTY(ReadOnly, Name = "overlayMaterialMaxDrawDistance")
    float overlayMaterialMaxDrawDistance_ = 0.0f;
    ROCKS_PROPERTY(ReadOnly, Name = "materialSlotsOverlayMaterial")
    Array<SoftObjectRef<MaterialBase>> materialSlotsOverlayMaterial_;
};

} // namespace rocks
