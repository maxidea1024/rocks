#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/HitGrid.h>

namespace rocks::ui {

TEST(UiCore_HitGridKeepsPaintedPositionsAndCombinesBroadAndCellEntries) {
    SystemAllocator allocator;
    auto root = SharedPtr<Element>::TryMake(allocator, allocator);
    auto clip = SharedPtr<Element>::TryMake(allocator, allocator);
    auto background = SharedPtr<Element>::TryMake(allocator, allocator);
    auto front = SharedPtr<Element>::TryMake(allocator, allocator);
    REQUIRE(root && clip && background && front);
    REQUIRE(root.Value()->TryAddChild(background.Value()));
    REQUIRE(root.Value()->TryAddChild(clip.Value()));
    REQUIRE(clip.Value()->TryAddChild(front.Value()));
    root.Value()->SetGeometry(Geometry::Root({4096, 4096}, {}));
    background.Value()->SetGeometry(root.Value()->GetGeometry());
    clip.Value()->SetClipToBounds(true);
    clip.Value()->SetGeometry(Geometry::Root({64, 64}, {1, 0, 0, 1, 100, 100}));
    front.Value()->SetGeometry(Geometry::Root({160, 160}, {1, 0, 0, 1, 120, 120}));
    HitGrid grid(allocator);
    REQUIRE(grid.Update(root.Value()));
    CHECK(grid.Find({130, 130}, *root.Value()).Get() == front.Value().Get());
    CHECK(grid.Find({170, 150}, *root.Value()).Get() == background.Value().Get());
    front.Value()->SetGeometry(Geometry::Root({10, 10}, {1, 0, 0, 1, 100, 100}));
    CHECK(grid.Find({130, 130}, *root.Value()).Get() == front.Value().Get());
    REQUIRE(grid.Update(root.Value()));
    CHECK(grid.Find({130, 130}, *root.Value()).Get() == clip.Value().Get());
    clip.Value()->SetVisibility(Visibility::Hidden);
    CHECK(grid.Find({105, 105}, *root.Value()).Get() == background.Value().Get());
    REQUIRE(root.Value()->RemoveChild(*background.Value()));
    CHECK(grid.Find({130, 130}, *root.Value()).Get() == root.Value().Get());
}
} // namespace rocks::ui
