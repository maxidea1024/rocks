#pragma once

#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Optional.h>
#include <Core/Text/String.h>
#include <InputActions/InputDataEnums.h>
#include <InputActions/InputModifier.h>
#include <InputActions/InputTrigger.h>
#include <InputActions/PlayerMappingSettings.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 키 하나를 액션 하나에 묶는다(UE FEnhancedActionKeyMapping — Docs/Design/InputActions 2.4절).
// action은 액션 id(2.7절), key는 UE EKeys 이름(IAD11 — 평가기가 ParseInputKey로 읽는다)이다.
//
// 사용 예:
//   InputBinding jump(allocator);
//   jump.action = jumpActionId;
//   (void)jump.key.TryAssign("SpaceBar");
ROCKS_STRUCT()
struct InputBinding {
    ROCKS_GENERATED_BODY()

    explicit InputBinding(Allocator& allocator) noexcept
        : key(allocator), triggers(allocator), modifiers(allocator) {
    }

    ROCKS_PROPERTY() Guid action;
    ROCKS_PROPERTY() String key;
    ROCKS_PROPERTY() Array<InputTrigger> triggers;
    ROCKS_PROPERTY() Array<InputModifier> modifiers;
    ROCKS_PROPERTY()
    MappingSettingBehavior settingBehavior = MappingSettingBehavior::InheritFromAction;
    // settingBehavior가 Override일 때 쓴다.
    ROCKS_PROPERTY() Optional<PlayerMappingSettings> mappingSettings;
};

} // namespace rocks::input
