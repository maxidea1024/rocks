#pragma once

#include <Core/Math/Vector2.h>
#include <Core/Result.h>

namespace rocks::ui {

// 창 호스트가 주는 커서 서비스다(Docs/Design/UiToolkit 23.5절 — UE
// FReply::UseHighPrecisionMouseMovement· SetMousePos). Application이 잠근
// capture(PointerReply::lockCursor)에 LockPointer, capture가 풀릴 때 UnlockPointer를 부른다. 좌표는
// UI 창 좌표다. 구현은 창 호스트가 platform::Window LockPointer·UnlockPointer·SetCursorPosition으로
// 한다(UiCore는 PlatformWindow에 의존하지 않는다).
//
// 스레드: 게임 lane에서만 부른다.
//
// 사용 예(창 호스트):
//   class HostCursor final : public CursorService {
//       Result<void> LockPointer(Vec2 at) noexcept override { return window.LockPointer(at.x,
//       at.y); } void UnlockPointer(Vec2 at) noexcept override {
//           window.UnlockPointer();
//           (void)window.SetCursorPosition(at.x, at.y);
//       }
//   };
//   (void)app.SetCursorService(cursor);
class CursorService {
public:
    virtual ~CursorService() noexcept = default;

    // 커서를 숨기고 at에 붙여 둔다. 그 뒤 Move는 이동량(relative)을 싣는다. 실패하면(Unsupported
    // 등) Application은 경고를 남기고 capture만 한다.
    virtual Result<void> LockPointer(Vec2 at) noexcept = 0;
    // 잠금을 풀고 커서를 보이며 releaseAt으로 옮긴다.
    virtual void UnlockPointer(Vec2 releaseAt) noexcept = 0;
};

} // namespace rocks::ui
