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
// - InputUserSettingsEntrySkipped: 사용자 설정을 불러올 때 모르는 키 이름·범위 밖 slot의 매핑을
//   건너뛰었다(Warning — 5.3절).
inline constexpr char DiagInputUserSettingsEntrySkipped[] = "InputUserSettingsEntrySkipped";
// - InputMappingSlotOverflow: 한 컨텍스트에서 같은 매핑 이름의 바인딩이 KeySlotCount개를 넘어 넘친
//   바인딩을 리바인딩 행에 넣지 않았다(Warning — 5.2절).
inline constexpr char DiagInputMappingSlotOverflow[] = "InputMappingSlotOverflow";

// 함수 바인딩을 부르지 못했다(대상 사라짐·파괴 대기 밖의 오류 — 바인딩마다 한 번, 5.1절 IA7).
inline constexpr char DiagInputFunctionBindingFailed[] = "InputFunctionBindingFailed";

} // namespace rocks::input
