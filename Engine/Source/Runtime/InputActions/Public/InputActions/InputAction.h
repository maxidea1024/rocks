#pragma once

#include <AssetRuntime/Package.h>
#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputActionSettings.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 입력 액션이다(UE UInputAction — Docs/Design/InputActions 2.3절). 바인딩은 id로 가리킨다(2.7절).
// 같은 액션을 쓰는 컨텍스트가 actions로 붙잡는다. UE처럼 에셋 클래스다(Asset 파생 — 2.8절):
// 패키지의 대표 에셋으로 저장·쿠킹하고, 패키지 없는 Registry에서는 코드로 만든 객체로
// 쓴다(assetId는 null).
//
// 사용 예(객체 만들기):
//   auto jump = registry.NewObject<InputAction>(access, TypeIdOf<InputAction>());
//
// 스레드: 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_INPUTACTIONS_API InputAction final : public Asset {
    ROCKS_GENERATED_BODY()

public:
    explicit InputAction(Allocator& allocator) noexcept : settings(allocator) {
    }

    // 실효 id다(Docs/Design/InputActions 2.8절): id가 0이 아니면 id, 0이면 대표 에셋의 AssetId
    // 값(패키지 없는 객체는 0 그대로). 모으기·사본이 이 값을 쓴다.
    Guid EffectiveId() const noexcept {
        return id != Guid{} ? id : Id().value;
    }

    // 저장·리바인딩·바인딩 참조 키다. 에셋은 0으로 두면 AssetId를 쓴다(EffectiveId). 실효 id가
    // 0이면 잘못된 액션이다(검증이 거부한다).
    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() InputActionSettings settings;
};

} // namespace rocks::input
