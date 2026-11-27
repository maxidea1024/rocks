#include <Core/Memory/SystemAllocator.h>
#include <PlatformWindow/Window.h>
#include <TestSupport/Test.h>
#include <TestSupport/WindowMessageProbe.h>

// 펌프 수준 대기 시험이다(Docs/Design/UiToolkit 19.2절). PumpMessages가 스레드의 OS 메시지를 창마다
// 나누고, 가져가지 않은 창 이벤트가 있으면 대기 조건(HasPendingWindowEvents)이 서며, 두 번 누르기
// 메시지는 누름 횟수 2로, 깨우기는 펌프 스레드에 빈 메시지로 간다. 대기가 "바로 돌아오는지"를
// 시간으로 재지 않고 그 조건을 단언한다(시간에 기대는 시험 금지).

namespace rocks::platform {

namespace {

constexpr uint32 PumpWindowSize = 64;
constexpr usize PumpQueueCapacity = 64;

Result<UniquePtr<Window>> CreateHidden(Allocator& allocator, StringView title) noexcept {
    return Window::Create(
        allocator,
        {.title = title, .width = PumpWindowSize, .height = PumpWindowSize, .visible = false});
}

} // namespace

TEST(PlatformWindow_PumpMessagesSplitsEventsPerWindow) {
    if (!test::SupportsWindowMessages()) {
        return;
    }

    SystemAllocator allocator;
    auto first = CreateHidden(allocator, "PumpFirst");
    auto second = CreateHidden(allocator, "PumpSecond");
    REQUIRE(first && second);
    // 앞 시험이 남긴 이벤트를 비운다.
    PumpMessages();
    input::InputQueue queue(allocator);
    REQUIRE(queue.TryInitialize(PumpQueueCapacity));
    first.Value()->TakeEvents(queue);
    second.Value()->TakeEvents(queue);
    queue.Clear();
    CHECK(!HasPendingWindowEvents());
    // 두 번째 창에 두 번 누르기를 보낸다. 한 번의 펌프가 그 창 대기 큐에 넣는다.
    REQUIRE(test::PostWindowDoubleClick(second.Value()->NativeHandle().window, 10, 12));
    PumpMessages();
    // 첫 창만 가져가면 두 번째 창 이벤트가 남아 대기가 바로 돌아와야 한다(19.1절 50 ms 결함).
    first.Value()->TakeEvents(queue);
    CHECK(queue.Events().IsEmpty());
    CHECK(HasPendingWindowEvents());
    second.Value()->TakeEvents(queue);
    CHECK(!HasPendingWindowEvents());
    REQUIRE(queue.Events().Size() >= 1);
    const input::InputEvent& press = queue.Events()[0];
    CHECK(press.type == input::InputEventType::MouseDown);
    CHECK(press.clickCount == 2);
    CHECK(press.x == 10.0f && press.y == 12.0f);
}

TEST(PlatformWindow_WakeMessageWaitPostsToPumpThread) {
    if (!test::SupportsWindowMessages()) {
        return;
    }

    SystemAllocator allocator;
    auto window = CreateHidden(allocator, "PumpWake");
    REQUIRE(window);
    // 펌프 스레드를 이 스레드로 기록하고 스레드 큐를 비운다.
    PumpMessages();
    CHECK(!test::HasThreadWakeMessage());
    WakeMessageWait();
    // 깨우기는 펌프 스레드 큐에 빈 메시지를 넣는다(MsgWaitForMultipleObjectsEx가 깬다).
    CHECK(test::HasThreadWakeMessage());
    // 다음 펌프가 빈 메시지를 버리고 창 이벤트는 만들지 않는다.
    PumpMessages();
    CHECK(!HasPendingWindowEvents());
}

TEST(PlatformWindow_WindowModeDefaultsToWindowed) {
    SystemAllocator allocator;
    auto window = CreateHidden(allocator, "WindowMode");
    if (!window && window.Error() == ErrorCode::Unsupported) {
        return;
    }

    REQUIRE(window);
    // 같은 모드는 아무것도 하지 않는다. 전체 화면 전환은 창을 화면에 띄우므로 사람 확인 목록이다.
    CHECK(window.Value()->GetWindowMode() == WindowMode::Windowed);
    CHECK(window.Value()->SetWindowMode(WindowMode::Windowed));
    CHECK(window.Value()->GetWindowMode() == WindowMode::Windowed);
}

} // namespace rocks::platform
