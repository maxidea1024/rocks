#pragma once

#include <AssetRuntime/Package.h>
#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <CurveAssets/FloatCurveAsset.h>
#include <InputActions/InputAction.h>
#include <InputActions/InputContextData.h>
#include <Meta/Annotations.h>
#include <Object/ObjectRefArray.h>

namespace rocks::input {

// 입력 컨텍스트다(UE UInputMappingContext — Docs/Design/InputActions 2.4절). 바인딩은 액션을 id로
// 가리키고, actions가 그 액션들을 GC 간선(패키지면 Hard 의존)으로 붙잡는다(2.7절). 평가기에는
// CollectInputContext로 모은 사본을 넣는다. UE처럼 에셋 클래스다(Asset 파생 — 2.8절). 에셋이면
// actions는 다른 패키지 InputAction으로의 Hard 참조라 함께 로드되고, lease로 읽을 때는
// InputAssetAccess.h의 CollectInputContext(assets, lease, bundle)를 쓴다.
//
// 사용 예(액션 붙잡기):
//   registry.AddReference(access, context.Handle(), contextPointer->actions, jump.Handle());
//
// 스레드: 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_INPUTACTIONS_API InputContext final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit InputContext(Allocator& allocator) noexcept
        : actions(allocator), data(allocator), curveAssets(allocator) {
    }

    // 실효 id다(Docs/Design/InputActions 2.8절): id가 0이 아니면 id, 0이면 대표 에셋의 AssetId
    // 값(패키지 없는 객체는 0 그대로). 모으기·사본이 이 값을 쓴다.
    Guid EffectiveId() const noexcept {
        return id != Guid{} ? id : Id().value;
    }

    // 평가기의 컨텍스트 키다. 에셋은 0으로 두면 AssetId를 쓴다(EffectiveId). 실효 id가 0이면 잘못된
    // 컨텍스트다(검증이 거부한다).
    ROCKS_PROPERTY() Guid id;
    // 바인딩이 가리키는 액션이다(null 원소 없음). chord 대상은 넣어도 되고 안 넣어도 된다.
    ROCKS_PROPERTY(Required) ObjectRefArray<InputAction> actions;
    ROCKS_PROPERTY() InputContextData data;
    // data의 바인딩(기본·프로필) 모디파이어의 응답 커브 slot이 가리키는 커브 에셋이다(Hard — 2.9절
    // IA9).
    ROCKS_PROPERTY(Required) ObjectRefArray<FloatCurveAsset> curveAssets;
};

} // namespace rocks::input
