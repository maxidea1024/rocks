#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Application/InputAdapter.h>

// 끄는 동안 커서 숨김·되돌림 시험이다(Docs/Design/UiToolkit 23.5절 — UE
// UseHighPrecisionMouseMovement·SetMousePos): 서비스가 받은 잠금·풀기 자리와 순서, 창 이동량
// 그대로 전달, 서비스 없을 때 위치 차이, Up·capture 잃음·FocusLost·releaseCursorAt, 잠금 중 hover
// 멈춤, 잠금 실패, capture 없는 lockCursor.

namespace rocks::ui {

namespace {

// 받은 호출을 순서대로 적는 가짜 커서 서비스다.
class LockCursorProbe final : public CursorService {
public:
    Result<void> LockPointer(Vec2 at) noexcept override {
        ++locks;
        lockedAt = at;
        return failLock ? Result<void>::Failure(ErrorCode::Unsupported) : Result<void>::Success();
    }

    void UnlockPointer(Vec2 releaseAt) noexcept override {
        ++unlocks;
        releasedAt = releaseAt;
    }

    int locks = 0;
    int unlocks = 0;
    Vec2 lockedAt{};
    Vec2 releasedAt{};
    bool failLock = false;
};

// 누르면 capture하고(lockOnDown이면 그때 잠근다) Move마다 이동량을 적는 요소다.
class LockDragProbe final : public Element {
    ROCKS_UI_ELEMENT(LockDragProbe, Element)

public:
    explicit LockDragProbe(Allocator& allocator) noexcept : Element(allocator) {
    }

    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override {
        if (event.type == PointerEventType::Down) {
            return {.handled = true, .capture = captureOnDown, .lockCursor = lockOnDown};
        }

        if (event.type == PointerEventType::Move) {
            lastHasDelta = event.hasDelta;
            lastDelta = event.delta;
            lastPosition = event.position;
            total += event.delta;
            PointerReply reply{.handled = true, .lockCursor = lockOnMove};
            if (releaseAtOnMove) {
                reply.releaseCursorAt = Vec2{1, 2};
            }

            return reply;
        }

        return {.handled = true};
    }

    void OnMouseEnter(uint32 user) noexcept override {
        Element::OnMouseEnter(user);
        ++enters;
    }

    bool captureOnDown = true;
    bool lockOnDown = true;
    bool lockOnMove = false;
    bool releaseAtOnMove = false;
    bool lastHasDelta = false;
    Vec2 lastDelta{};
    Vec2 lastPosition{};
    Vec2 total{};
    int enters = 0;
};

// 왼쪽 반은 끌기 요소, 오른쪽 반은 hover를 세는 요소다.
struct LockBench {
    explicit LockBench(Allocator& allocator) noexcept : app(allocator) {
    }

    Result<void> Build(Allocator& allocator) noexcept {
        auto made = SharedPtr<Element>::TryMake(allocator, allocator);
        auto left = SharedPtr<LockDragProbe>::TryMake(allocator, allocator);
        auto right = SharedPtr<LockDragProbe>::TryMake(allocator, allocator);
        if (!made || !left || !right) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        root = made.Value();
        drag = left.Value();
        other = right.Value();
        const Geometry window = Geometry::Root({100, 40}, {});
        root->SetGeometry(window);
        drag->SetGeometry(window.MakeChild({50, 40}, {}, {}, {}));
        other->SetGeometry(window.MakeChild({50, 40}, {1, 0, 0, 1, 50, 0}, {}, {}));
        auto added = root->TryAddChild(drag);
        if (added) {
            added = root->TryAddChild(other);
        }

        if (added) {
            app.SetRoot(root);
        }

        return added;
    }

    Result<bool> Press(PointerEventType type, Vec2 at) noexcept {
        return app.RoutePointer({.type = type, .position = at});
    }

    Result<bool> Move(Vec2 at) noexcept {
        return app.RoutePointer({.type = PointerEventType::Move, .position = at});
    }

    Application app;
    SharedPtr<Element> root;
    SharedPtr<LockDragProbe> drag;
    SharedPtr<LockDragProbe> other;
};

} // namespace

TEST(UiFramework_CursorLockCallsTheServiceAndRestoresAtThePressPoint) {
    SystemAllocator allocator;
    LockBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    auto cursor = SharedPtr<LockCursorProbe>::TryMake(allocator);
    REQUIRE(cursor && bench.app.SetCursorService(cursor.Value()));
    REQUIRE(bench.Press(PointerEventType::Down, {10, 10}));
    CHECK(bench.app.IsCursorLocked(PointerSource::Mouse, 0, 0));
    CHECK(cursor.Value()->locks == 1 && cursor.Value()->lockedAt == Vec2(10, 10));
    // 창이 잰 이동량(relative)은 그대로 온다. 위치는 잠금 자리다.
    PointerEvent relative{.type = PointerEventType::Move, .position = {10, 10}};
    relative.hasDelta = true;
    relative.delta = {3, -2};
    REQUIRE(bench.app.RoutePointer(relative));
    CHECK(bench.drag->lastHasDelta && bench.drag->lastDelta == Vec2(3, -2));
    CHECK(bench.drag->lastPosition == Vec2(10, 10));
    // 놓으면 누른 자리로 되돌린다.
    REQUIRE(bench.Press(PointerEventType::Up, {10, 10}));
    CHECK(cursor.Value()->unlocks == 1 && cursor.Value()->releasedAt == Vec2(10, 10));
    CHECK(!bench.app.IsCursorLocked(PointerSource::Mouse, 0, 0));
    // 잠그지 않은 Move는 이동량이 없다(adapter가 남긴 relative 값도 지운다).
    REQUIRE(bench.app.RoutePointer(relative));
    CHECK(!bench.drag->lastHasDelta);
    bench.app.Shutdown();
}

TEST(UiFramework_CursorLockWithoutServiceUsesPositionDifferences) {
    SystemAllocator allocator;
    LockBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    REQUIRE(bench.Press(PointerEventType::Down, {10, 10}));
    CHECK(bench.app.IsCursorLocked(PointerSource::Mouse, 0, 0));
    REQUIRE(bench.Move({14, 10}));
    CHECK(bench.drag->lastHasDelta && bench.drag->lastDelta == Vec2(4, 0));
    REQUIRE(bench.Move({14, 4}));
    CHECK(bench.drag->lastDelta == Vec2(0, -6) && bench.drag->total == Vec2(4, -6));
    REQUIRE(bench.Press(PointerEventType::Up, {14, 4}));
    bench.app.Shutdown();
}

TEST(UiFramework_CursorLockStartsOnAMoveReplyAndHonorsReleaseCursorAt) {
    SystemAllocator allocator;
    LockBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    auto cursor = SharedPtr<LockCursorProbe>::TryMake(allocator);
    REQUIRE(cursor && bench.app.SetCursorService(cursor.Value()));
    bench.drag->lockOnDown = false;
    bench.drag->lockOnMove = true;
    REQUIRE(bench.Press(PointerEventType::Down, {10, 10}));
    CHECK(cursor.Value()->locks == 0);
    // 끌기가 시작된 Move에서 잠근다 — 잠금 자리는 그 위치다(UE UseHighPrecisionMouseMovement).
    REQUIRE(bench.Move({20, 12}));
    CHECK(!bench.drag->lastHasDelta);
    CHECK(cursor.Value()->locks == 1 && cursor.Value()->lockedAt == Vec2(20, 12));
    // 이미 잠갔으면 다시 부르지 않는다.
    bench.drag->releaseAtOnMove = true;
    REQUIRE(bench.Move({20, 12}));
    CHECK(cursor.Value()->locks == 1);
    // releaseCursorAt을 정했으면 거기로 되돌린다.
    REQUIRE(bench.Press(PointerEventType::Up, {20, 12}));
    CHECK(cursor.Value()->unlocks == 1 && cursor.Value()->releasedAt == Vec2(1, 2));
    bench.app.Shutdown();
}

TEST(UiFramework_CursorLockIsReleasedWhenTheCaptureIsLost) {
    SystemAllocator allocator;
    LockBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    auto cursor = SharedPtr<LockCursorProbe>::TryMake(allocator);
    REQUIRE(cursor && bench.app.SetCursorService(cursor.Value()));
    // 창 초점 잃음(FocusLost → ResetInput)도 잠금을 푼다.
    InputAdapter adapter(bench.app);
    REQUIRE(adapter.Route(input::InputEvent::MouseDown(input::MouseButton::Left, 10, 10)));
    CHECK(cursor.Value()->locks == 1);
    REQUIRE(adapter.Route(input::InputEvent::MouseMoveRelative(10, 10, 5, 0)));
    CHECK(bench.drag->lastHasDelta && bench.drag->lastDelta == Vec2(5, 0));
    REQUIRE(adapter.Route(input::InputEvent::FocusLost()));
    CHECK(cursor.Value()->unlocks == 1 && cursor.Value()->releasedAt == Vec2(10, 10));
    // 모든 포인터 취소도 푼다.
    REQUIRE(bench.Press(PointerEventType::Down, {30, 5}));
    CHECK(cursor.Value()->locks == 2);
    bench.app.CancelAllPointers();
    CHECK(cursor.Value()->unlocks == 2 && cursor.Value()->releasedAt == Vec2(30, 5));
    // 요소를 떼면 다음 정리에서 푼다.
    REQUIRE(bench.Press(PointerEventType::Down, {30, 5}));
    REQUIRE(bench.root->RemoveChild(*bench.drag));
    REQUIRE(bench.Move({31, 5}));
    CHECK(cursor.Value()->unlocks == 3);
    bench.app.Shutdown();
}

TEST(UiFramework_CursorLockFreezesHoverAndToleratesServiceFailure) {
    SystemAllocator allocator;
    LockBench bench(allocator);
    REQUIRE(bench.Build(allocator));
    auto cursor = SharedPtr<LockCursorProbe>::TryMake(allocator);
    REQUIRE(cursor && bench.app.SetCursorService(cursor.Value()));
    REQUIRE(bench.Press(PointerEventType::Down, {10, 10}));
    // 잠근 동안 다른 요소 위로 가도 hover가 바뀌지 않는다(서비스 없음 경로의 위치로 시험한다).
    const int enters = bench.other->enters;
    REQUIRE(bench.Move({70, 10}));
    CHECK(bench.other->enters == enters);
    REQUIRE(bench.Press(PointerEventType::Up, {70, 10}));
    // 잠금이 실패하면 capture만 한다 — 풀 때 서비스를 부르지 않고, 이동량은 위치 차이다.
    cursor.Value()->failLock = true;
    REQUIRE(bench.Press(PointerEventType::Down, {10, 10}));
    CHECK(cursor.Value()->locks == 2 && bench.app.IsCursorLocked(PointerSource::Mouse, 0, 0));
    REQUIRE(bench.Move({12, 10}));
    CHECK(bench.drag->lastDelta == Vec2(2, 0));
    REQUIRE(bench.Press(PointerEventType::Up, {12, 10}));
    CHECK(cursor.Value()->unlocks == 1);
    // capture 없는 lockCursor는 무시한다.
    bench.drag->captureOnDown = false;
    REQUIRE(bench.Press(PointerEventType::Down, {10, 10}));
    CHECK(!bench.app.IsCursorLocked(PointerSource::Mouse, 0, 0) && cursor.Value()->locks == 2);
    bench.app.Shutdown();
}

} // namespace rocks::ui
