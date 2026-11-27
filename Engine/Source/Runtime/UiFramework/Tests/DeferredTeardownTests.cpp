#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/ManualClock.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/Button.h>
#include <UiFramework/Lists/ListView.h>

// 소멸자 사용자 코드 규칙 시험이다(Docs/Design/UiToolkit 21.4절): 미룬 해체 통지는 소멸자 밖(프레임
// 시작·Shutdown)에서 정확히 한 번 돌고, 앱 소멸 때 남은 통지는 돌지 않는다. 호스트 묶음 바뀜 통지,
// ListView는 소멸자에서 행 원본을 부르지 않고 명시 해제에서 부른다.

namespace rocks::ui {

namespace {

// 마지막으로 측정한 앱의 묶음을 기억했다가, 소멸자에서 통지를 그 대기열에 넣는다(UserWidgetRoot와
// 같은 모양).
class TeardownProbe final : public Element {
    ROCKS_UI_ELEMENT(TeardownProbe, Element)

public:
    TeardownProbe(Allocator& allocator, uint32& delivered, uint32& hostChanges) noexcept
        : Element(allocator), delivered_(delivered), hostChanges_(hostChanges) {
    }

    ~TeardownProbe() noexcept override {
        if (auto host = host_.Lock()) {
            (void)host->DeferTeardown(Delegate<void()>::CreateLambda(
                [&delivered = delivered_]() noexcept { ++delivered; }));
        }
    }

    void OnUiHostServicesChanged() noexcept override {
        ++hostChanges_;
        host_ = WeakPtr<UiHostServices>(FindUiHostServicesRef());
    }

private:
    uint32& delivered_;
    uint32& hostChanges_;
    WeakPtr<UiHostServices> host_;
};

class CountingRows final : public TableRowSource {
public:
    uint32 RootCount() const noexcept override {
        return 100;
    }

    uint64 RootKey(uint32 index) const noexcept override {
        return index + 1;
    }

    Result<SharedPtr<Element>> CreateRow(Allocator& allocator) noexcept override {
        auto row = SharedPtr<Button>::TryMake(allocator, allocator);
        if (!row) {
            return Result<SharedPtr<Element>>::Failure(row.Error());
        }

        return Result<SharedPtr<Element>>::Success(Move(row.Value()));
    }

    Result<void> BindRow(Element&, TableItem) noexcept override {
        return Result<void>::Success();
    }

    void ReleaseRow(Element&, TableItem) noexcept override {
        ++released;
    }

    uint32 released = 0;
};

const Geometry TeardownViewport = Geometry::Root({100, 60}, {});

} // namespace

TEST(UiFramework_DeferredTeardownRunsOnceOutsideDestructors) {
    SystemAllocator allocator;
    ManualClock clock;
    Application app(allocator);
    app.SetClock(clock.Function());
    uint32 delivered = 0;
    uint32 hostChanges = 0;
    {
        auto probe =
            SharedPtr<TeardownProbe>::TryMake(allocator, allocator, delivered, hostChanges);
        REQUIRE(probe);
        app.SetRoot(probe.Value());
        REQUIRE(app.PrepareLayout(TeardownViewport, 1));
        CHECK(hostChanges == 1);
        // 다시 측정해도 같은 묶음이면 통지하지 않는다.
        REQUIRE(app.PrepareLayout(TeardownViewport, 1));
        CHECK(hostChanges == 1);
        app.SetRoot({});
    }

    // 소멸자 안에서는 돌지 않았다. 다음 프레임 시작에 한 번 돈다.
    CHECK(delivered == 0);
    CHECK(app.PendingTeardownCount() == 1);
    app.BeginFrame();
    CHECK(delivered == 1);
    app.BeginFrame();
    CHECK(delivered == 1);

    // 통지 안에서 넣은 통지는 같은 호출에서 돈다. 통지 안에서 다시 돌리면 0이다.
    uint32 nested = 0;
    usize reentered = 1;
    REQUIRE(app.DeferTeardown(Delegate<void()>::CreateLambda([&]() noexcept {
        reentered = app.RunDeferredTeardown();
        (void)app.DeferTeardown(Delegate<void()>::CreateLambda([&nested]() noexcept { ++nested; }));
    })));
    CHECK(app.RunDeferredTeardown() == 2);
    CHECK(nested == 1 && reentered == 0);
    // 빈 통지는 받지 않는다.
    CHECK(app.DeferTeardown({}).Error() == ErrorCode::InvalidArgument);
}

TEST(UiFramework_ShutdownDeliversAndDestructionDiscardsTeardown) {
    SystemAllocator allocator;
    uint32 delivered = 0;
    uint32 hostChanges = 0;
    {
        Application app(allocator);
        auto probe =
            SharedPtr<TeardownProbe>::TryMake(allocator, allocator, delivered, hostChanges);
        REQUIRE(probe);
        app.SetRoot(probe.Value());
        REQUIRE(app.PrepareLayout(TeardownViewport, 1));
        probe = {};
        // Shutdown이 루트를 떼고 통지를 돌린다.
        app.Shutdown();
        CHECK(delivered == 1);
    }

    {
        Application app(allocator);
        auto probe =
            SharedPtr<TeardownProbe>::TryMake(allocator, allocator, delivered, hostChanges);
        REQUIRE(probe);
        app.SetRoot(probe.Value());
        REQUIRE(app.PrepareLayout(TeardownViewport, 1));
        probe = {};
        // Shutdown 없이 부수면 통지는 돌지 않는다(경고 로그만).
    }

    CHECK(delivered == 1);
}

TEST(UiFramework_ElementsHearWhenMovedToAnotherApplication) {
    SystemAllocator allocator;
    Application first(allocator);
    Application second(allocator);
    uint32 delivered = 0;
    uint32 hostChanges = 0;
    auto probe = SharedPtr<TeardownProbe>::TryMake(allocator, allocator, delivered, hostChanges);
    REQUIRE(probe);
    first.SetRoot(probe.Value());
    REQUIRE(first.PrepareLayout(TeardownViewport, 1));
    first.SetRoot({});
    // 다른 앱으로 옮기면 revision이 같아도 묶음이 바뀐 것을 안다 — 해체 통지는 새 앱에 간다.
    second.SetRoot(probe.Value());
    REQUIRE(second.PrepareLayout(TeardownViewport, 1));
    CHECK(hostChanges == 2);
    second.SetRoot({});
    probe = {};
    CHECK(first.PendingTeardownCount() == 0);
    CHECK(second.PendingTeardownCount() == 1);
    CHECK(second.RunDeferredTeardown() == 1 && delivered == 1);
}

TEST(UiFramework_ListViewReleasesRowsOnlyOnTheExplicitPath) {
    SystemAllocator allocator;
    CountingRows source;
    Application app(allocator);
    {
        auto view = SharedPtr<ListView>::TryMake(allocator, allocator, app, source);
        REQUIRE(view);
        REQUIRE(view.Value()->SetRowHeight(20));
        app.SetRoot(view.Value());
        REQUIRE(app.UpdatePaintCache(TeardownViewport, 1));
        const usize rows = view.Value()->ChildCount();
        REQUIRE(rows == 3);
        // 명시 해제는 보이는 행마다 원본에 돌려준다(UE HandleRowReleased).
        view.Value()->ReleaseAllRows();
        CHECK(source.released == rows);
        CHECK(view.Value()->ChildCount() == 0);
        // 다음 배치에서 다시 만든다.
        REQUIRE(app.UpdatePaintCache(TeardownViewport, 1));
        CHECK(view.Value()->ChildCount() == rows);
        app.SetRoot({});
    }

    // 소멸자는 원본(사용자 코드)을 부르지 않는다.
    CHECK(source.released == 3);
}

} // namespace rocks::ui
