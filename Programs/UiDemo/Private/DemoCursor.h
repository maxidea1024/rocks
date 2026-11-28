#pragma once

#include <PlatformWindow/Window.h>
#include <UiCore/CursorService.h>

namespace rocks::ui {

// 창 하나의 커서 서비스다(Docs/Design/UiToolkit 23.5절). UI 창 좌표 = client 픽셀이라 그대로
// 넘긴다. 창 호스트가 Application::SetCursorService로 등록하고, 창을 부수기 전에 Detach한다(잠근
// capture가 서비스를 더 오래 쥘 수 있다).
//
// 사용 예:
//   auto cursor = SharedPtr<DemoCursor>::TryMake(allocator, *window);
//   (void)app.SetCursorService(cursor.Value());
//   ...
//   cursor.Value()->Detach();   // 창을 부수기 전
class DemoCursor final : public CursorService {
public:
    explicit DemoCursor(platform::Window& window) noexcept;
    ~DemoCursor() noexcept override;

    // 잠금을 풀고 창과의 연결을 끊는다. 그 뒤 LockPointer는 NotReady, UnlockPointer는 아무것도 하지
    // 않는다.
    void Detach() noexcept;

    Result<void> LockPointer(Vec2 at) noexcept override;
    void UnlockPointer(Vec2 releaseAt) noexcept override;

private:
    platform::Window* window_;
};

} // namespace rocks::ui
