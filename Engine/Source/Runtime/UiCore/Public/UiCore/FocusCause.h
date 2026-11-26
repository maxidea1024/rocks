#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 초점이 옮겨진 원인이다(Docs/Design/UiToolkit 9.1절 UW4f1). 초점 외곽선은 Navigation일 때만
// 보인다 — 마우스로 누른 컨트롤에는 외곽선을 그리지 않는다.
enum class FocusCause : uint8 {
    // 포인터 누름이 고른 초점이다.
    Pointer,
    // Tab·방향키·게임패드 이동이다.
    Navigation,
    // 코드가 직접 정한 초점이다(메뉴 닫힘 뒤 복원, 화면 전환 등).
    Direct
};

} // namespace rocks::ui
