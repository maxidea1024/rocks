#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Grid.h>
#include <UiFramework/SizeBox.h>
#include <UiFramework/UniformGrid.h>

namespace rocks::ui {

TEST(UiFramework_GridNudgeOffsetsContentWithoutChangingTrackSize) {
    SystemAllocator allocator;
    auto item = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    REQUIRE(item);
    REQUIRE(item.Value()->SetWidthOverride(10));
    REQUIRE(item.Value()->SetHeightOverride(6));

    Grid grid(allocator);
    REQUIRE(grid.SetColumnFill(0, 1));
    REQUIRE(grid.SetRowFill(0, 1));
    REQUIRE(grid.TryAddSlot(item.Value(), {.padding = {2, 4, 6, 8},
                                           .horizontal = StackAlignment::End,
                                           .vertical = StackAlignment::Center,
                                           .nudge = {-3, 1}}));
    REQUIRE(grid.Prepass(1));
    CHECK(grid.GetDesiredSize() == Vec2(18, 18));
    REQUIRE(grid.Arrange(Geometry::Root({64, 40}, {})));
    CHECK(item.Value()->GetGeometry().size == Vec2(10, 6));
    CHECK(item.Value()->GetGeometry().layout.Apply({0, 0}) == Vec2(45, 16));
    auto other = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    REQUIRE(other && grid.TryAddSlot(other.Value(), {.layer = 1}));
    REQUIRE(!grid.SetSlot(*item.Value(), {.rowSpan = 0}));
    CHECK(grid.ChildAt(0) == item.Value().Get());
    REQUIRE(grid.SetSlot(*item.Value(), {.layer = 2}));
    CHECK(grid.ChildAt(0) == other.Value().Get());
    CHECK(grid.ChildAt(1) == item.Value().Get());
    CHECK(item.Value()->Parent() == &grid);
    REQUIRE(grid.RemoveChild(*item.Value()));

    UniformGrid uniform(allocator);
    REQUIRE(uniform.SetCellPadding({2, 4, 6, 8}));
    REQUIRE(uniform.TryAddSlot(item.Value(), 0, 1, StackAlignment::End, StackAlignment::Center));
    REQUIRE(uniform.Prepass(1));
    CHECK(uniform.GetDesiredSize() == Vec2(36, 18));
    REQUIRE(uniform.Arrange(Geometry::Root({128, 40}, {})));
    CHECK(item.Value()->GetGeometry().size == Vec2(10, 6));
    CHECK(item.Value()->GetGeometry().layout.Apply({0, 0}) == Vec2(112, 15));
}

} // namespace rocks::ui
