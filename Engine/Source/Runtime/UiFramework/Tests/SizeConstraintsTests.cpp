#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/FitBox.h>
#include <UiFramework/Frame.h>
#include <UiFramework/SizeBox.h>

namespace rocks::ui {

TEST(UiFramework_AspectRatioLimitsArrangementAndExplicitScaleAffectsMeasurement) {
    SystemAllocator allocator;
    auto box = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    auto fit = SharedPtr<FitBox>::TryMake(allocator, allocator);
    auto item = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(box && fit && item);
    item.Value()->SetDesiredSize({10, 5});
    REQUIRE(fit.Value()->TryAddChild(item.Value()));
    REQUIRE(box.Value()->TryAddChild(fit.Value()));
    fit.Value()->SetFit(FitMode::User);
    REQUIRE(fit.Value()->SetUserScale(2));
    REQUIRE(box.Value()->SetAspectRatioRange(1, 2));
    REQUIRE(!box.Value()->SetAspectRatioRange(2, 1));
    REQUIRE(box.Value()->SetHeightOverride(30));
    REQUIRE(box.Value()->Prepass(1));
    CHECK(box.Value()->GetDesiredSize() == Vec2(20, 30));
    REQUIRE(box.Value()->Arrange(Geometry::Root({100, 20}, {})));
    CHECK(fit.Value()->GetGeometry().size == Vec2(40, 20));
    CHECK(fit.Value()->GetGeometry().layout.Apply({0, 0}) == Vec2(30, 0));
    CHECK(item.Value()->GetGeometry().size == Vec2(10, 5));
    const auto& layout = item.Value()->GetGeometry().layout;
    CHECK(layout.Apply({10, 5}) - layout.Apply({0, 0}) == Vec2(20, 10));
    box.Value()->ClearHeightOverride();
    REQUIRE(box.Value()->Prepass(1));
    CHECK(box.Value()->GetDesiredSize() == Vec2(20, 10));
}
} // namespace rocks::ui
