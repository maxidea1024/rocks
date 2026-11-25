#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputActionData.h>
#include <InputActions/InputContextData.h>

namespace rocks::input {

// 평가기에 넣는 컨텍스트 사본이다(Docs/Design/InputActions 2.7절): 컨텍스트 id, 데이터, actions
// 전체의 사본. 객체 없이 만들 수도 있다(시험·코드 정의 컨텍스트). 이동만 된다.
//
// 사용 예:
//   InputContextBundle bundle(allocator);
//   auto collected = CollectInputContext(registry, access, context, bundle);
//   if (collected) {
//       playerInput.AddContext(Move(bundle), 0, {});
//   }
struct InputContextBundle {
    explicit InputContextBundle(Allocator& allocator) noexcept
        : data(allocator), actions(allocator) {
    }

    Guid id;
    InputContextData data;
    Array<InputActionData> actions;
};

} // namespace rocks::input
