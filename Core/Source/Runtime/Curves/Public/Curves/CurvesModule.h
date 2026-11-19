#pragma once

#include <Core/Result.h>
#include <Object/ModuleRegistration.h>

namespace rocks {

// Curves 모듈의 리플렉션 타입(커브 enum·키·ScalarCurveBase·TangentCurve·PlainCurve)을 등록한다.
// 커브를 필드로 쓰는 모듈의 생성 등록 코드가 이 모듈 등록을 먼저 부르므로 대개 직접 부를 일이 없다.
// 커브 타입만 따로 쓰는 도구·시험이 모듈 등록 단계에서 부른다. 이미 등록되어 있으면 아무것도 하지
// 않는다. 실패: ModuleRegistration의 오류.
//
// 사용 예:
//   if (auto registered = RegisterCurvesTypes(registration); !registered) { return registered; }
Result<void> RegisterCurvesTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
