#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputActionSettings.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

namespace rocks::input {

// 입력 액션이다(UE UInputAction — Docs/Design/InputActions 2.3절). 바인딩은 id로 가리킨다(2.7절).
// 같은 액션을 쓰는 컨텍스트가 actions로 붙잡는다.
//
// 사용 예(객체 만들기):
//   auto jump = registry.NewObject<InputAction>(access, TypeIdOf<InputAction>());
//
// 스레드: 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_INPUTACTIONS_API InputAction final : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit InputAction(Allocator& allocator) noexcept : settings(allocator) {
    }

    // 저장·리바인딩·바인딩 참조 키다. 0이면 잘못된 액션이다(검증이 거부한다).
    ROCKS_PROPERTY() Guid id;
    ROCKS_PROPERTY() InputActionSettings settings;
};

} // namespace rocks::input
