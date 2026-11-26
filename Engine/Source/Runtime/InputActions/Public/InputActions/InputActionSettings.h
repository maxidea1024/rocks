#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Optional.h>
#include <InputActions/InputDataEnums.h>
#include <InputActions/InputModifier.h>
#include <InputActions/InputTrigger.h>
#include <InputActions/InputValue.h>
#include <InputActions/PlayerMappingSettings.h>
#include <Localization/Text.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 입력 액션 하나의 설정이다(UE UInputAction의 필드 — Docs/Design/InputActions 2.3절). 평가기는 이
// struct를 그대로 복사해 쓴다(2.7절). 기본값은 UE와 같다.
ROCKS_STRUCT()
struct InputActionSettings {
    ROCKS_GENERATED_BODY()

    explicit InputActionSettings(Allocator& allocator) noexcept
        : triggers(allocator), modifiers(allocator) {
    }

    // 편집기 설명이다(UE ActionDescription).
    ROCKS_PROPERTY() Text description;
    ROCKS_PROPERTY() InputValueType valueType = InputValueType::Boolean;
    // 일시정지 중에도 이벤트를 낸다.
    ROCKS_PROPERTY() bool triggerWhenPaused = false;
    // 하위 우선순위 컨텍스트의 같은 키 바인딩을 막는다(4.2절 3).
    ROCKS_PROPERTY() bool consumeInput = true;
    // 표시만 한다(UE도 강제하지 않는다).
    ROCKS_PROPERTY() bool reserveAllMappings = false;
    ROCKS_PROPERTY() ActionAccumulation accumulation = ActionAccumulation::TakeHighestAbsoluteValue;
    // 바인딩 결합 뒤에 이어서 평가한다(4.3절 4).
    ROCKS_PROPERTY() Array<InputTrigger> triggers;
    ROCKS_PROPERTY() Array<InputModifier> modifiers;
    ROCKS_PROPERTY() Optional<PlayerMappingSettings> mappingSettings;
};

} // namespace rocks::input
