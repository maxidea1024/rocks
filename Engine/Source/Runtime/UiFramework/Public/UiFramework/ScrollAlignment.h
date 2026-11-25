#pragma once
#include <Base/Types.h>
namespace rocks::ui {
// 요청한 항목이 이미 보이면 Nearest는 현재 위치를 유지한다.
enum class ScrollIntoViewAlignment : uint8 { Nearest, Start, Center, End };
} // namespace rocks::ui
