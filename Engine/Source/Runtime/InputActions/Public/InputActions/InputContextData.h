#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <InputActions/InputBinding.h>
#include <InputActions/InputDataEnums.h>
#include <InputActions/InputProfileBindings.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 입력 컨텍스트의 데이터다(UE UInputMappingContext의 필드 — Docs/Design/InputActions 2.4절).
// 평가기는 이 struct를 그대로 복사해 쓴다(2.7절).
ROCKS_STRUCT()
struct InputContextData {
    ROCKS_GENERATED_BODY()

    explicit InputContextData(Allocator& allocator) noexcept
        : bindings(allocator), profileOverrides(allocator) {
    }

    ROCKS_PROPERTY() Array<InputBinding> bindings;
    ROCKS_PROPERTY() Array<InputProfileBindings> profileOverrides;
    ROCKS_PROPERTY()
    ContextRegistrationTracking registrationTracking = ContextRegistrationTracking::Untracked;
};

} // namespace rocks::input
