#pragma once

#include <Core/Diagnostics/LogCategory.h>

namespace rocks {

// World 모듈의 로그 카테고리다(Docs/Design/LoggingSystem.ko.md 13절). World 수명과 GC 통계를
// 기록한다.
//
// 사용 예:
//   Log::Debug(LogWorld, "World {World} GC cycle reclaimed {Reclaimed} objects", name, count);
inline constexpr LogCategory LogWorld{"World"};

// Tick 등록·의존(선행 순서) 진단이다. "World" 아래 이름이라 SetLevel("World")로 함께 조절된다.
inline constexpr LogCategory LogWorldTick{"World.Tick"};

} // namespace rocks
