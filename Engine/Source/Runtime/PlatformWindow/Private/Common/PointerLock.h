#pragma once

#include <Base/Types.h>

// 끄는 동안 커서를 잠금 자리에 붙여 두고 이동량만 내는 판정이다(Docs/Design/UiToolkit 23.5절 — UE
// FReply::UseHighPrecisionMouseMovement). 창 backend가 마우스 이동 메시지를 바꾸는 자리에서
// 부른다. 큐를 처리하는 쪽에서 되돌리면 이미 쌓인 절대 위치가 옛 기준이라 이동을 두 번 센다. 플랫폼
// 헤더 없이 값만 다뤄 EngineTests로 시험한다. 창 소유 스레드에서만 쓴다(스레드 안전하지 않다).
//
// 사용 예(창 메시지 처리):
//   PointerLock lock;
//   lock.NoteMove(x, y);                       // 잠그지 않은 이동·버튼 위치
//   lock.Lock(anchorX, anchorY);               // UI가 잠금을 요청
//   const PointerLockMove move = lock.Move(x, y);
//   if (move.warp) { SetCursorPos(anchor); }   // 커서를 잠금 자리로 되돌린다
//   if (move.push) { Push(MouseMoveRelative(move.x, move.y, move.deltaX, move.deltaY)); }
namespace rocks::platform {

// 잠근 동안의 이동 하나를 어떻게 낼지다.
struct PointerLockMove {
    // 이벤트를 낼지다(이동량 0 — 되돌림이 만든 이동 — 이면 거짓).
    bool push = false;
    // 커서를 잠금 자리로 되돌려야 하는지다.
    bool warp = false;
    // 이벤트 위치(잠금 자리)와 직전 기준에서 움직인 client 픽셀이다.
    float x = 0;
    float y = 0;
    float deltaX = 0;
    float deltaY = 0;
};

// 시험(PointerLockTests)이 부르므로 export한다(Modular 링크).
class ROCKS_PLATFORMWINDOW_API PointerLock {
public:
    // 잠그지 않은 동안 창이 바꾼 마우스 위치를 적는다(잠글 때의 첫 기준이 된다).
    void NoteMove(float x, float y) noexcept;

    // 잠금 자리(client)를 정한다. 첫 기준은 마지막으로 적은 위치다 — 잠금 요청 전에 움직였지만 아직
    // 받지 않은 이동을 잃지 않는다. 이미 잠갔으면 잠금 자리만 바꾼다.
    void Lock(float anchorX, float anchorY) noexcept;

    // 잠금을 푼다. 마지막 위치는 잠금 자리가 된다(커서가 거기 있다). 잠그지 않았으면 아무것도 하지
    // 않는다.
    void Unlock() noexcept;

    // 커서를 옮겼다(SetCursorPosition) — 마지막 위치를 그 자리로 둔다.
    void NoteWarp(float x, float y) noexcept;

    bool IsLocked() const noexcept;
    float AnchorX() const noexcept;
    float AnchorY() const noexcept;

    // 잠근 동안의 이동 메시지 하나다. 이동량은 위치 - 기준이고, 0이 아니면 되돌린 뒤 기준을 잠금
    // 자리로 둔다. 잠그지 않았으면 push·warp가 거짓이다(위치만 적는다).
    PointerLockMove Move(float x, float y) noexcept;

private:
    bool locked_ = false;
    float anchorX_ = 0;
    float anchorY_ = 0;
    float referenceX_ = 0;
    float referenceY_ = 0;
    float lastX_ = 0;
    float lastY_ = 0;
};

} // namespace rocks::platform
