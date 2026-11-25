#pragma once

#include <Core/Diagnostics/LogCategory.h>

namespace rocks::input {

// InputActions 모듈의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). 평가기 다시 만들기의
// 데이터 진단을 남긴다.
//
// 사용 예:
//   Log::Warning(LogInputActions, "Input binding key '{Key}' is unknown ({Code})", name, code);
inline constexpr LogCategory LogInputActions{"InputActions"};

// 진단 코드다(Docs/Design/InputActions 2.7·4.2절). 로그의 {Code} 속성으로 싣는다.
// - InputBindingKeyUnknown: 바인딩 키 이름을 읽을 수 없어 그 바인딩을 건너뛰었다(Warning).
inline constexpr char DiagInputBindingKeyUnknown[] = "InputBindingKeyUnknown";
// - InputActionSettingsConflict: 같은 id 액션의 설정이 컨텍스트마다 달라 정렬 순 첫 사본을
//   썼다(Warning).
inline constexpr char DiagInputActionSettingsConflict[] = "InputActionSettingsConflict";

} // namespace rocks::input
