#pragma once

#include <Input/InputTypes.h>

namespace rocks::input {

// OS 포인터 ID에 손가락 번호(0..MaxTouches-1)를 배정한다(Docs/Design/TouchInput.ko.md 3.2절 TD2).
// 창 backend(Windows WM_POINTER·Android MotionEvent)가 쓴다.
// - Acquire: 비어 있는 가장 작은 번호를 준다. 이미 배정된 ID면 그 번호다. 빈 번호가 없으면(11번째
//   손가락) NoTouchIndex를 돌려주고 DroppedCount를 늘린다. 그 포인터의 뒤 이벤트는 Find가
//   NoTouchIndex라 무시된다.
// - Release: 끝난 포인터의 번호를 돌려주고 그 번호를 "끝남"으로 잡아 둔다. 끝난 번호는 EndFrame까지
//   다시 주지 않는다 — 한 프레임 안에서 같은 번호가 끝났다가 다시 시작하면 InputState의 edge·시작
//   위치가 깨지기 때문이다.
// - EndFrame: 프레임 경계(backend의 PumpEvents가 이벤트를 호스트 큐로 옮긴 뒤)에 끝난 번호를
//   돌려받는다.
//
// 사용 예(backend, 같은 잠금 아래):
//   const uint8 index = allocator.Acquire(pointerId);        // DOWN
//   if (index != NoTouchIndex) { Push(InputEvent::TouchBegan(index, x, y)); }
//   const uint8 moved = allocator.Find(pointerId);            // MOVE
//   const uint8 ended = allocator.Release(pointerId);         // UP
//   allocator.EndFrame();                                     // PumpEvents 끝
//
// 스레드: 동기화하지 않는다. backend가 입력 스레드와 게임 스레드 사이를 자기 잠금으로 지킨다.
inline constexpr uint8 NoTouchIndex = 0xFF;

class ROCKS_INPUT_API TouchIndexAllocator {
public:
    uint8 Acquire(int64 pointerId) noexcept;

    // 배정된 번호다(끝났거나 없으면 NoTouchIndex).
    uint8 Find(int64 pointerId) const noexcept;

    // 포인터가 끝났다. 그 번호를 돌려주고 EndFrame까지 잡아 둔다. 배정되지 않은 ID면
    // NoTouchIndex다.
    uint8 Release(int64 pointerId) noexcept;

    // 닿아 있는 모든 포인터를 끝낸다(OS 취소·포커스 상실). 끝낸 번호를 indices에 쓰고 그 수를
    // 돌려준다.
    uint32 ReleaseAll(uint8 (&indices)[MaxTouches]) noexcept;

    void EndFrame() noexcept;

    // 번호가 없어 버린 포인터 수다(진단).
    uint64 DroppedCount() const noexcept {
        return dropped_;
    }

private:
    enum class SlotState : uint8 {
        Free,
        Active,
        Ended
    };

    struct Slot {
        int64 pointerId = 0;
        SlotState state = SlotState::Free;
    };

    Slot slots_[MaxTouches];
    uint64 dropped_ = 0;
};

} // namespace rocks::input
