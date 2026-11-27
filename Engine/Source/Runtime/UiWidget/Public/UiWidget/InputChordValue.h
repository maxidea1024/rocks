#pragma once

#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <UiCore/KeyChord.h>

namespace rocks::ui {

// 키 선택기 이벤트 인자다(UE FInputChord — Docs/Design/UiToolkit 21.5a절). 키는 KeyChord 입력 이름
// (KeyChord::InputName — 설정 파일과 같은 철자)이고 보조 키 넷을 따로 든다. 키 enum 전체를
// 리플렉션에 옮기지 않으려고 이름으로 넘긴다. 빈 이름은 키 없음이다.
//
// 사용 예:
//   selector->OnSelected().TryAddLambda([](const InputChordValue& chord) noexcept {
//       Rebind(chord.keyName.View(), chord.control);
//   });
ROCKS_STRUCT()
struct InputChordValue {
    ROCKS_GENERATED_BODY()

    explicit InputChordValue(Allocator& allocator) noexcept : keyName(allocator) {
    }

    ROCKS_PROPERTY() String keyName;
    ROCKS_PROPERTY() bool shift = false;
    ROCKS_PROPERTY() bool control = false;
    ROCKS_PROPERTY() bool alt = false;
    ROCKS_PROPERTY() bool super = false;
};

} // namespace rocks::ui
