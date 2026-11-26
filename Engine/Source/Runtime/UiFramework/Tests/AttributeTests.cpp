#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Layout/Frame.h>

using namespace rocks;
using namespace rocks::ui;

TEST(UiFramework_BoundColorOnlyRepaintsOnChangeAndTracksReparenting) {
    SystemAllocator allocator;
    Application app(allocator);
    auto first = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto second = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto leaf = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(first && second && leaf);
    unsigned calls = 0;
    LinearColor color{1, 0, 0, 1};
    leaf.Value()->BindColor({{}, Delegate<LinearColor()>::CreateLambda([&]() noexcept {
                                 ++calls;
                                 return color;
                             })});
    REQUIRE(first.Value()->TryAddChild(leaf.Value()));
    app.SetRoot(first.Value());
    const auto viewport = Geometry::Root({80, 60}, {});
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    auto unchanged = app.UpdatePaintCache(viewport, 1);
    REQUIRE(unchanged);
    CHECK(calls == 2);
    CHECK(unchanged.Value()->LastUpdate().paintCalls == 0);

    color = {0, 1, 0, 1};
    auto changed = app.UpdatePaintCache(viewport, 1);
    REQUIRE(changed);
    CHECK(changed.Value()->LastUpdate().paintCalls == 1);
    CHECK(leaf.Value()->GetColor() == color);
    REQUIRE(first.Value()->RemoveChild(*leaf.Value()));
    REQUIRE(second.Value()->TryAddChild(leaf.Value()));
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(calls == 3);
    app.SetRoot(second.Value());
    leaf.Value()->SetVisibility(Visibility::Hidden);
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(calls == 3);
    leaf.Value()->SetVisibility(Visibility::Visible);
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(calls == 4);
    leaf.Value()->SetColor(color);
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(calls == 4);
}
