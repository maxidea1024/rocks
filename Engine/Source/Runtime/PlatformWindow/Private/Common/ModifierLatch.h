#pragma once

#include <Base/Types.h>
#include <Input/InputTypes.h>

// 창이 누름을 보고한 modifier를 적어 두고, 놓친 뗌을 찾는 판정이다(Docs/Design/UiToolkit 23.5a절 —
// UE FWindowsApplication::CheckForShiftUpEvents를 모든 modifier로 넓힘). Win+. 이모지 패널처럼 OS가
// 뗌 메시지를 다른 창에 주면 UI가 그 modifier를 누른 채로 안다. 창 backend가 키 메시지 앞·펌프
// 끝에서 OS 물리 상태를 주어 부른다. 플랫폼 헤더 없이 값만 다뤄 EngineTests로 시험한다. 창 소유
// 스레드에서만 쓴다(스레드 안전하지 않다).
//
// 사용 예(창 메시지 처리):
//   ModifierLatch latch;
//   latch.NoteDown(key);                          // KeyDown을 보낼 때
//   if (latch.NoteUp(key)) { Push(KeyUp(key)); }  // 늦은 뗌은 버린다
//   input::Key released[ModifierLatch::Capacity];
//   const uint32 count = latch.Reconcile(keyMessagesPending, physicalDownMask, released);
namespace rocks::platform {

// 시험(ModifierLatchTests)이 부르므로 export한다(Modular 링크).
class ROCKS_PLATFORMWINDOW_API ModifierLatch {
public:
    // 적을 수 있는 modifier 수다(Shift·Control·Alt·Super 좌우).
    static constexpr uint32 Capacity = 8;

    // modifier면 그 비트(0–7)이고, 아니면 0이다. Reconcile의 물리 상태 mask가 이 비트를 쓴다.
    static uint8 Bit(input::Key key) noexcept;

    // Bit의 반대다(비트 하나 — 범위 밖이면 Key::None).
    static input::Key KeyOf(uint8 bit) noexcept;

    // 창이 KeyDown을 보낸다. modifier면 적는다(반복 누름은 그대로다).
    void NoteDown(input::Key key) noexcept;

    // 창이 KeyUp을 보내려 한다. 보낼지다: modifier가 아니면 늘 참, 적힌 modifier면 지우고 참,
    // 적히지 않은 modifier면 거짓(보정이 이미 뗌을 냈거나 창 밖에서 누른 키다 — 뗌이 두 번 가지
    // 않는다).
    bool NoteUp(input::Key key) noexcept;

    // 적힌 modifier의 mask다(0이면 Reconcile할 것이 없다).
    uint8 Held() const noexcept;

    // 놓친 뗌을 찾는다. 적힌 modifier 가운데 physicalDown(Bit mask)에 없는 것을 지우고 released에
    // 비트 순서로 담아 그 수를 돌려준다. keyMessagesPending이면(스레드 큐에 키 메시지가 남음) 진짜
    // 뗌이 순서대로 오도록 아무것도 하지 않고 0이다.
    uint32 Reconcile(bool keyMessagesPending, uint8 physicalDown,
                     input::Key (&released)[Capacity]) noexcept;

    // 초점을 잃었다 — 적힘을 비운다(FocusLost가 입력 상태 전체를 되돌린다).
    void Clear() noexcept;

private:
    uint8 held_ = 0;
};

} // namespace rocks::platform
