#pragma once

#include <Core/Result.h>

// InputActions 모듈의 리플렉션 타입 등록이다(Docs/Design/InputActions 2.3–2.5절).
namespace rocks {

class ModuleRegistration;

namespace input {

// 액션·컨텍스트·바인딩·트리거·모디파이어 리플렉션 타입을 registration에 등록한다(의존 모듈 타입 —
// Curves·CoreTypeBindings·Localization과 Input을 거친 Config — 도 함께). 엔진은 모듈 로더가
// 등록하므로 도구·시험만 직접 부른다.
//
// 오류: registration의 등록 오류를 그대로 돌려준다. 스레드: registration 소유자의 규칙을 따른다.
ROCKS_INPUTACTIONS_API Result<void>
RegisterInputActionsTypes(ModuleRegistration& registration) noexcept;

} // namespace input

} // namespace rocks
