#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/ElementAttribute.h>

// 보이기 묶음과 속성 갱신 순서 시험이다(Docs/Design/UiToolkit 20.6절 — UE
// SWidget::Prepass_ChildLoop 1846–1859행): 보이기는 Collapsed여도 평가되어 다시 보이고, Hidden
// 요소의 다른 묶음도 평가된다.

using namespace rocks;
using namespace rocks::ui;

namespace visibility_binding_tests_file {

namespace {

// 묶음 하나를 가진 요소다(평가 횟수를 센다).
class ProbeElement final : public Element {
    ROCKS_UI_ELEMENT(ProbeElement, Element)

public:
    explicit ProbeElement(Allocator& allocator) noexcept : Element(allocator), width_(*this, 0) {
    }

    void BindWidth(int* calls) noexcept {
        width_.Assign(Delegate<float()>::CreateLambda([calls]() noexcept {
            ++*calls;
            return static_cast<float>(*calls);
        }));
    }

    float Width() const noexcept {
        return width_.Get();
    }

private:
    ElementAttribute<float, InvalidateReason::Layout> width_;
};

} // namespace

TEST(UiCore_BoundVisibilityIsEvaluatedEvenWhenCollapsed) {
    SystemAllocator allocator;
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto child = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root && child);
    REQUIRE(root.Value()->TryAddChild(child.Value()));
    Visibility wanted = Visibility::Collapsed;
    REQUIRE(child.Value()->BindVisibility(
        Delegate<Visibility()>::CreateLambda([&wanted]() noexcept { return wanted; })));
    root.Value()->UpdateBoundAttributes();
    CHECK(child.Value()->GetVisibility() == Visibility::Collapsed);
    // 접힌 요소도 보이기 묶음은 평가된다 — 다시 보인다.
    wanted = Visibility::Visible;
    root.Value()->UpdateBoundAttributes();
    CHECK(child.Value()->GetVisibility() == Visibility::Visible);
    // 값으로 주면 묶음이 풀린다.
    child.Value()->SetVisibility(Visibility::Hidden);
    wanted = Visibility::Collapsed;
    root.Value()->UpdateBoundAttributes();
    CHECK(child.Value()->GetVisibility() == Visibility::Hidden);
    // 빈 delegate도 푼다.
    REQUIRE(child.Value()->BindVisibility(
        Delegate<Visibility()>::CreateLambda([&wanted]() noexcept { return wanted; })));
    REQUIRE(child.Value()->BindVisibility({}));
    root.Value()->UpdateBoundAttributes();
    CHECK(child.Value()->GetVisibility() == Visibility::Hidden);
}

TEST(UiCore_HiddenElementsKeepUpdatingOtherBindings) {
    SystemAllocator allocator;
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto probe = SharedPtr<ProbeElement>::TryMake(allocator, allocator);
    REQUIRE(root && probe);
    REQUIRE(root.Value()->TryAddChild(probe.Value()));
    int calls = 0;
    probe.Value()->BindWidth(&calls);
    root.Value()->UpdateBoundAttributes();
    CHECK(calls == 1);
    // Hidden은 공간을 차지하므로 크기 묶음을 계속 평가한다.
    probe.Value()->SetVisibility(Visibility::Hidden);
    root.Value()->UpdateBoundAttributes();
    CHECK(calls == 2);
    CHECK(probe.Value()->Width() == 2);
    // Collapsed는 평가하지 않는다.
    probe.Value()->SetVisibility(Visibility::Collapsed);
    root.Value()->UpdateBoundAttributes();
    CHECK(calls == 2);
    // 숨긴 조상 아래도 평가한다(Hidden), 접힌 조상 아래는 하지 않는다.
    probe.Value()->SetVisibility(Visibility::Visible);
    root.Value()->SetVisibility(Visibility::Hidden);
    root.Value()->UpdateBoundAttributes();
    CHECK(calls == 3);
    root.Value()->SetVisibility(Visibility::Collapsed);
    root.Value()->UpdateBoundAttributes();
    CHECK(calls == 3);
}

TEST(UiCore_VisibilityBindingRunsBeforeOtherBindingsInOnePass) {
    SystemAllocator allocator;
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto probe = SharedPtr<ProbeElement>::TryMake(allocator, allocator);
    REQUIRE(root && probe);
    REQUIRE(root.Value()->TryAddChild(probe.Value()));
    int calls = 0;
    probe.Value()->BindWidth(&calls);
    // 같은 회차에서 보이기가 먼저 Collapsed가 되면 나머지 묶음은 평가하지 않는다.
    REQUIRE(probe.Value()->BindVisibility(
        Delegate<Visibility()>::CreateLambda([]() noexcept { return Visibility::Collapsed; })));
    root.Value()->UpdateBoundAttributes();
    CHECK(probe.Value()->GetVisibility() == Visibility::Collapsed);
    CHECK(calls == 0);
    // 소멸 때 묶음 목록에서 빠진다(조상 계수가 0으로 돌아온다 — 다음 갱신이 아무것도 하지 않는다).
    REQUIRE(root.Value()->RemoveChild(*probe.Value()));
    probe.Value().Reset();
    root.Value()->UpdateBoundAttributes();
    // 새 요소의 묶음은 정상으로 평가된다(옛 계수가 남지 않았다).
    auto again = SharedPtr<ProbeElement>::TryMake(allocator, allocator);
    REQUIRE(again);
    REQUIRE(root.Value()->TryAddChild(again.Value()));
    int againCalls = 0;
    again.Value()->BindWidth(&againCalls);
    root.Value()->UpdateBoundAttributes();
    CHECK(againCalls == 1);
}

TEST(UiCore_VisibilityGetterMayRebindItsOwnBinding) {
    SystemAllocator allocator;
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto child = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root && child);
    REQUIRE(root.Value()->TryAddChild(child.Value()));
    Element* target = child.Value().Get();
    // getter가 자기 묶음을 값으로 바꿔도 안전하다(옛 묶음은 갱신이 끝난 뒤 해제된다).
    REQUIRE(child.Value()->BindVisibility(Delegate<Visibility()>::CreateLambda([target]() noexcept {
        target->SetVisibility(Visibility::Hidden);
        return Visibility::Collapsed;
    })));
    root.Value()->UpdateBoundAttributes();
    CHECK(child.Value()->GetVisibility() == Visibility::Collapsed);
    // 묶음은 풀렸다 — 다음 갱신은 값을 바꾸지 않는다.
    child.Value()->SetVisibility(Visibility::Visible);
    root.Value()->UpdateBoundAttributes();
    CHECK(child.Value()->GetVisibility() == Visibility::Visible);
}

} // namespace visibility_binding_tests_file
