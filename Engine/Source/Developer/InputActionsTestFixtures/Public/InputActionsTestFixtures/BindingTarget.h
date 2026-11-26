#pragma once

#include <Base/Types.h>
#include <InputActions/ActionEventData.h>
#include <Meta/Annotations.h>
#include <Object/Object.h>

// 입력 함수 바인딩 시험 대상이다(Docs/Design/InputActions 5.1절 IA7). 맞는 서명 하나와 맞지 않는
// 서명(다른 인자·반환·static)을 둔다.
namespace rocks::input_fixture {

ROCKS_CLASS()
class BindingTarget final : public Object {
    ROCKS_GENERATED_BODY()

public:
    // 맞는 서명이다. 부른 횟수와 마지막 데이터를 남긴다.
    ROCKS_FUNCTION()
    void OnAction(const input::ActionEventData& event) noexcept {
        ++calls;
        last = event;
    }

    // 다른 인자다(TypeMismatch).
    ROCKS_FUNCTION()
    void OnNumber(int32 number) noexcept {
        (void)number;
    }

    // 반환이 있다(TypeMismatch).
    ROCKS_FUNCTION()
    int32 OnActionCounted(const input::ActionEventData& event) noexcept {
        (void)event;
        return static_cast<int32>(++calls);
    }

    // static이다(InvalidArgument).
    ROCKS_FUNCTION()
    static void OnStaticAction(const input::ActionEventData& event) noexcept {
        (void)event;
    }

    // 맞는 서명 함수가 불린 횟수다.
    ROCKS_NATIVE() uint32 calls = 0;
    // 마지막으로 받은 데이터다.
    ROCKS_NATIVE() input::ActionEventData last;
};

} // namespace rocks::input_fixture
