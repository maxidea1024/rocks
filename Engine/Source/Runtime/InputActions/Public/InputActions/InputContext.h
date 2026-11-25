#pragma once

#include <Base/Guid.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputAction.h>
#include <InputActions/InputContextData.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>

namespace rocks::input {

// 입력 컨텍스트다(UE UInputMappingContext — Docs/Design/InputActions 2.4절). 바인딩은 액션을 id로
// 가리키고, actions가 그 액션들을 GC 간선(패키지면 Hard 의존)으로 붙잡는다(2.7절). 평가기에는
// CollectInputContext로 모은 사본을 넣는다.
//
// 사용 예(액션 붙잡기):
//   registry.AddReference(access, context.Handle(), contextPointer->actions, jump.Handle());
//
// 스레드: 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_INPUTACTIONS_API InputContext final : public Object {
    ROCKS_GENERATED_BODY()

public:
    explicit InputContext(Allocator& allocator) noexcept : actions(allocator), data(allocator) {
    }

    // 평가기의 컨텍스트 키다. 0이면 잘못된 컨텍스트다(검증이 거부한다).
    ROCKS_PROPERTY() Guid id;
    // 바인딩이 가리키는 액션이다(null 원소 없음). chord 대상은 넣어도 되고 안 넣어도 된다.
    ROCKS_PROPERTY(Required) ObjectRefArray<InputAction> actions;
    ROCKS_PROPERTY() InputContextData data;
};

} // namespace rocks::input
