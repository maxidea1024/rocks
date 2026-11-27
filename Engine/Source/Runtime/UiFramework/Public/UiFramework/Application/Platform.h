#pragma once

#include <Core/Text/String.h>

namespace rocks::ui {

// 창 호스트가 UI에 제공하는 OS 서비스다. 화면을 붙이기 전에 해당 플랫폼의 구현을 지정한다.
// 헤드리스 실행에서는 가짜 서비스를 주입하거나 지원하지 않는 동작을 빈 포인터로 둔다.
struct Platform {
    Result<void> (*readClipboardText)(String& output) noexcept = nullptr;
    Result<void> (*writeClipboardText)(StringView text) noexcept = nullptr;
    // 명령 수정 키가 Cmd(super)인지다(macOS 호스트는 참, 그 밖은 Ctrl). 편집 문맥 메뉴의 단축키
    // 표시가 쓴다(Docs/Design/UiToolkit 22.7a절). 키 처리는 둘 다 받는다.
    bool commandIsSuper = false;
};

} // namespace rocks::ui
