#pragma once

#include <Core/Diagnostics/LogCategory.h>

namespace rocks {

// Engine 모듈의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). 초기화·종료 단계, 메인
// 루프, World 실행 문맥(생성·Tick 실패·지연 파괴)과 엔진 Registry GC를 기록한다.
//
// 사용 예:
//   Log::Info(LogEngine, "Engine run finished after {Frames} frames: {Reason}", frames, reason);
inline constexpr LogCategory LogEngine{"Engine"};

} // namespace rocks
