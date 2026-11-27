#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/ElementAttribute.h>
#include <UiFramework/Application/Application.h>

// Element 동작 정렬 시험 묶음이다(Docs/Design/UiToolkit 20.12절 — 8장 UV2 무효화·UV3 입력
// 라우팅·UV4 focus·navigation 중 이 단계까지 구현된 항목). 기대값은 UE 5.8 근거를 따른다: 같은 값은
// 무효화 없음 (SlateAttribute 비교), 보이기 변경은 부모 Layout(SWidget::SetVisibility), 크기 불변
// Layout은 부모로 번지지 않음(Prepass desired 비교), cache root 재사용과 volatile만 다시
// 그림(SInvalidationPanel), tunnel 뒤 bubble(FEventRouter), 보이기 5종의 hit(EVisibility), 규칙
// Explicit·Custom, 사용자 둘, tab 순서의 비활성·접힘 건너뜀.

namespace rocks::ui {

namespace {

constexpr Vec2 ParityViewport{200, 100};

// 측정·그리기·입력 순서를 센다.
class Probe final : public Element {
    ROCKS_UI_ELEMENT(Probe, Element)

public:
    explicit Probe(Allocator& allocator, Array<const Element*>* order = nullptr) noexcept
        : Element(allocator), width_(*this, 10.0f), order_(order) {
    }

    Vec2 ComputeDesiredSize(float) const noexcept override {
        ++measures;
        return {width_.Get(), 10};
    }

    Result<void> Arrange(const Geometry& geometry) noexcept override {
        SetGeometry(geometry);
        for (usize i = 0; i < ChildCount(); ++i) {
            if (auto arranged = ChildAt(i)->ArrangeIfNeeded(geometry); !arranged) {
                return arranged;
            }
        }

        return Result<void>::Success();
    }

    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override {
        ++paints;
        return list.TryAddBox({GetGeometry(), {1, 1, 1, 1}, layer});
    }

    PointerReply OnPreviewPointerDown(const PointerEvent&) noexcept override {
        if (order_) {
            (void)order_->TryPushBack(this);
        }

        return {};
    }

    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override {
        if (order_ && event.type == PointerEventType::Down) {
            (void)order_->TryPushBack(this);
        }

        return {};
    }

    bool SupportsKeyboardFocus() const noexcept override {
        return focusable;
    }

    void SetWidth(float width) noexcept {
        width_.Set(width);
    }

    mutable int measures = 0;
    mutable int paints = 0;
    bool focusable = false;

private:
    ElementAttribute<float, InvalidateReason::Layout> width_;
    Array<const Element*>* order_;
};

Result<SharedPtr<Probe>> Add(Allocator& allocator, Element& parent,
                             Array<const Element*>* order = nullptr) noexcept {
    auto probe = SharedPtr<Probe>::TryMake(allocator, allocator, order);
    if (!probe) {
        return probe;
    }

    if (auto added = parent.TryAddChild(probe.Value()); !added) {
        return Result<SharedPtr<Probe>>::Failure(added.Error());
    }

    return probe;
}

} // namespace

TEST(UiFramework_ParitySameValueDoesNotInvalidate) {
    SystemAllocator allocator;
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, nullptr);
    REQUIRE(root);
    auto child = Add(allocator, *root.Value());
    REQUIRE(child);
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto viewport = Geometry::Root(ParityViewport, {});
    REQUIRE(app.PrepareLayout(viewport, 1));
    const int measured = child.Value()->measures;
    // 같은 값은 무효화 0회다.
    child.Value()->SetWidth(10);
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(child.Value()->measures == measured);
    // 다른 값은 다시 잰다.
    child.Value()->SetWidth(20);
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(child.Value()->measures == measured + 1);
}

TEST(UiFramework_ParityVisibilityReMeasuresParentButSizeInvariantLayoutDoesNot) {
    SystemAllocator allocator;
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, nullptr);
    REQUIRE(root);
    auto child = Add(allocator, *root.Value());
    REQUIRE(child);
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto viewport = Geometry::Root(ParityViewport, {});
    REQUIRE(app.PrepareLayout(viewport, 1));
    // 크기가 그대로인 Layout 무효화는 부모로 번지지 않는다.
    const int parent = root.Value()->measures;
    child.Value()->InvalidateLayout();
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(root.Value()->measures == parent);
    // 보이기 변경(접힘)은 부모 Layout이다.
    child.Value()->SetVisibility(Visibility::Collapsed);
    REQUIRE(app.PrepareLayout(viewport, 1));
    CHECK(root.Value()->measures == parent + 1);
}

TEST(UiFramework_ParityCacheRootReusesAndRepaintsOnlyVolatile) {
    SystemAllocator allocator;
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, nullptr);
    REQUIRE(root);
    auto still = Add(allocator, *root.Value());
    auto moving = Add(allocator, *root.Value());
    REQUIRE(still && moving);
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto viewport = Geometry::Root(ParityViewport, {});
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    // 바뀐 것이 없으면 다시 그리지 않는다(빠른 경로).
    const int stillPaints = still.Value()->paints;
    auto cache = app.UpdatePaintCache(viewport, 1);
    REQUIRE(cache);
    CHECK(cache.Value()->LastUpdate().paintCalls == 0);
    CHECK(still.Value()->paints == stillPaints);
    // volatile 요소만 매번 다시 그린다.
    moving.Value()->SetVolatile(true);
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    const int movingPaints = moving.Value()->paints;
    cache = app.UpdatePaintCache(viewport, 1);
    REQUIRE(cache);
    CHECK(moving.Value()->paints == movingPaints + 1);
    CHECK(still.Value()->paints == stillPaints);
    // volatile 여부 변경은 그 요소만 무효화한다(UE SWidget::Invalidate — PaintAndVolatility,
    // UiToolkit 20.12절). 다시 끄면 한 번 더 기록해 최신 모양을 남기고, 그 뒤로는 그리지 않는다.
    moving.Value()->SetVolatile(false);
    const int lastMoving = moving.Value()->paints;
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    cache = app.UpdatePaintCache(viewport, 1);
    REQUIRE(cache);
    CHECK(cache.Value()->LastUpdate().paintCalls == 0);
    CHECK(moving.Value()->paints == lastMoving + 1);
    CHECK(still.Value()->paints == stillPaints);
    // 창 전체 cache 없는 기본 경로는 매번 전부 그린다.
    PaintList list(allocator);
    REQUIRE(app.BuildPaintList(viewport, 1, list));
    REQUIRE(app.BuildPaintList(viewport, 1, list));
    CHECK(still.Value()->paints == stillPaints + 2);
}

TEST(UiFramework_ParityPreviewTunnelsThenBubbles) {
    SystemAllocator allocator;
    Array<const Element*> order(allocator);
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, &order);
    REQUIRE(root);
    auto middle = Add(allocator, *root.Value(), &order);
    REQUIRE(middle);
    auto leaf = Add(allocator, *middle.Value(), &order);
    REQUIRE(leaf);
    Application app(allocator);
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root(ParityViewport, {}), 1));
    REQUIRE(app.RoutePointer({PointerEventType::Down, {5, 5}}));
    // preview는 루트에서 잎으로(tunnel), 본 이벤트는 잎에서 루트로(bubble)다.
    REQUIRE(order.Size() == 6);
    CHECK(order[0] == root.Value().Get());
    CHECK(order[1] == middle.Value().Get());
    CHECK(order[2] == leaf.Value().Get());
    CHECK(order[3] == leaf.Value().Get());
    CHECK(order[4] == middle.Value().Get());
    CHECK(order[5] == root.Value().Get());
}

TEST(UiFramework_ParityVisibilityKindsForHitTesting) {
    SystemAllocator allocator;
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, nullptr);
    REQUIRE(root);
    auto parent = Add(allocator, *root.Value());
    REQUIRE(parent);
    auto child = Add(allocator, *parent.Value());
    REQUIRE(child);
    Application app(allocator);
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root(ParityViewport, {}), 1));
    const Vec2 point{5, 5};
    CHECK(root.Value()->HitTest(point) == child.Value().Get());
    // 자신만 hit 불가: 자식은 맞는다.
    parent.Value()->SetVisibility(Visibility::SelfHitTestInvisible);
    CHECK(root.Value()->HitTest(point) == child.Value().Get());
    // hit 불가: 자신과 자식 모두 아니다 — 루트가 맞는다.
    parent.Value()->SetVisibility(Visibility::HitTestInvisible);
    CHECK(root.Value()->HitTest(point) == root.Value().Get());
    // 숨김·접힘은 맞지 않는다.
    parent.Value()->SetVisibility(Visibility::Hidden);
    CHECK(root.Value()->HitTest(point) == root.Value().Get());
    parent.Value()->SetVisibility(Visibility::Collapsed);
    CHECK(root.Value()->HitTest(point) == root.Value().Get());
}

TEST(UiFramework_ParityExplicitCustomRulesAndTwoUsers) {
    SystemAllocator allocator;
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, nullptr);
    REQUIRE(root);
    SharedPtr<Probe> boxes[3];
    for (auto& box : boxes) {
        auto made = Add(allocator, *root.Value());
        REQUIRE(made);
        box = made.Value();
        box->focusable = true;
    }

    constexpr Guid Target{0, 7};
    boxes[2]->SetNavigationIdentity({}, Target);
    Application app(allocator);
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root(ParityViewport, {}), 1));
    // Explicit: 지정 대상으로 간다(탐색 전에 정한다 — UE CalculateDestinationWidget).
    REQUIRE(
        root.Value()->SetNavigationRule(FocusDirection::Right, {NavigationRule::Explicit, Target}));
    REQUIRE(app.SetFocus(0, boxes[0]));
    REQUIRE(app.Navigate(0, FocusDirection::Right, {}, FocusCause::Navigation));
    CHECK(app.GetFocus(0).Get() == boxes[2].Get());
    // Custom: 처리기가 정한다.
    REQUIRE(root.Value()->SetNavigationHandler(NavigationHandler::CreateLambda(
        [](uint32, FocusDirection) noexcept { return Guid{0, 7}; })));
    REQUIRE(root.Value()->SetNavigationRule(FocusDirection::Left, {NavigationRule::Custom, {}}));
    REQUIRE(app.SetFocus(0, boxes[1]));
    REQUIRE(app.Navigate(0, FocusDirection::Left, {}, FocusCause::Navigation));
    CHECK(app.GetFocus(0).Get() == boxes[2].Get());
    // 사용자 둘의 초점은 서로 독립이다.
    REQUIRE(app.SetFocus(1, boxes[0]));
    REQUIRE(app.Navigate(1, FocusDirection::Right, {}, FocusCause::Navigation));
    CHECK(app.GetFocus(1).Get() == boxes[2].Get());
    REQUIRE(app.SetFocus(0, boxes[1]));
    CHECK(app.GetFocus(1).Get() == boxes[2].Get());
    CHECK(app.GetFocus(0).Get() == boxes[1].Get());
}

TEST(UiFramework_ParityTabOrderSkipsDisabledAndCollapsed) {
    SystemAllocator allocator;
    auto root = SharedPtr<Probe>::TryMake(allocator, allocator, nullptr);
    REQUIRE(root);
    SharedPtr<Probe> boxes[4];
    for (auto& box : boxes) {
        auto made = Add(allocator, *root.Value());
        REQUIRE(made);
        box = made.Value();
        box->focusable = true;
    }

    boxes[1]->SetEnabled(false);
    boxes[2]->SetVisibility(Visibility::Collapsed);
    Application app(allocator);
    app.SetRoot(root.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root(ParityViewport, {}), 1));
    REQUIRE(app.SetFocus(0, boxes[0]));
    REQUIRE(app.MoveFocus(0, true, false, FocusCause::Navigation));
    CHECK(app.GetFocus(0).Get() == boxes[3].Get());
    REQUIRE(app.MoveFocus(0, false, false, FocusCause::Navigation));
    CHECK(app.GetFocus(0).Get() == boxes[0].Get());
}

} // namespace rocks::ui
