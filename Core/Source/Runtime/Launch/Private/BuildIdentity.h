#pragma once

#include <Base/Types.h>

namespace rocks::launch::detail {

// 빌드 식별 블록(설계 14.2절)의 칸 크기다. 엔진 버전(최대 17자)·도구 빌드 ID(hex 64자)와
// 키·따옴표를 담는다.
inline constexpr usize BuildIdentityBytes = 160;

// 크래시 메타데이터 "build" 객체에 붙일 JSON 멤버 조각(중괄호 없음)을 buffer에 NUL 종료로 쓰고
// 길이를 돌려준다. 예: "engineVersion": "0.1.0", "toolBuildId": "e3b0...". 값은 숫자·'.'·hex뿐이라
// escape가 필요 없다. 다 들어가지 않으면 0이고 buffer는 빈 문자열이다(capacity가 1 이상일 때).
ROCKS_LAUNCH_API usize FormatBuildIdentity(char* buffer, usize capacity) noexcept;

} // namespace rocks::launch::detail
