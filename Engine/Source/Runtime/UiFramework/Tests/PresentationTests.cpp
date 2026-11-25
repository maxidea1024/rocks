#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/DrawBatch.h>
#include <UiFramework/Application.h>
#include <UiFramework/Frame.h>

namespace rocks::ui {

TEST(UiFramework_PartialPaintPreservesInheritedOpacityForVolatileChildren) {
    SystemAllocator allocator;
    auto root = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto child = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(root && child);
    REQUIRE(root.Value()->SetRenderOpacity(0.5f));
    REQUIRE(child.Value()->SetRenderOpacity(0.5f));
    REQUIRE(root.Value()->TryAddChild(child.Value()));
    child.Value()->SetVolatile(true);
    Application app(allocator);
    app.SetRoot(root.Value());
    const auto viewport = Geometry::Root({40, 20}, {});
    auto cache = app.UpdatePaintCache(viewport, 1);
    REQUIRE(cache);
    DrawBatch batch(allocator);
    REQUIRE(batch.Update(*cache.Value()));
    REQUIRE(batch.Vertices().Size() == 8);
    CHECK(batch.Vertices()[0].color.a == 0.5f);
    CHECK(batch.Vertices()[4].color.a == 0.25f);

    child.Value()->SetColor({1, 0, 0, 0.5f});
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(cache.Value()->LastUpdate().paintCalls == 1);
    REQUIRE(batch.Update(*cache.Value()));
    CHECK(batch.Vertices()[4].color.a == 0.125f);
    REQUIRE(root.Value()->SetRenderOpacity(1));
    REQUIRE(app.UpdatePaintCache(viewport, 1));
    CHECK(!cache.Value()->LastUpdate().rebuilt);
    REQUIRE(batch.Update(*cache.Value()));
    CHECK(batch.Vertices()[4].color.a == 0.25f);
}

namespace {
class HoverTarget final : public Element {
public:
    explicit HoverTarget(Allocator& allocator) noexcept : Element(allocator) {}
    void OnMouseEnter(uint32) noexcept override {
        ++entered;
        if (clearRoot) {clearRoot->SetRoot({});}
    }
    void OnMouseLeave(uint32) noexcept override {++left;}
    PointerReply OnPointerEvent(const PointerEvent&) noexcept override {
        ++events;
        return {.handled=true};
    }
    int entered=0, left=0, events=0;
    Application* clearRoot=nullptr;
};
}

TEST(UiFramework_HoverPathsTrackUsersAndReentrantRootRemoval) {
    SystemAllocator allocator;
    auto target=SharedPtr<HoverTarget>::TryMake(allocator,allocator);
    REQUIRE(target);
    auto& item=*target.Value();
    item.SetGeometry(Geometry::Root({40,20},{}));
    Application app(allocator);
    app.SetRoot(target.Value());
    PointerEvent move{PointerEventType::Move,{3,3}};
    REQUIRE(app.RoutePointer(move));
    REQUIRE(app.RoutePointer(move));
    move.userId=1;
    REQUIRE(app.RoutePointer(move));
    CHECK(item.entered==2 && item.left==0 && item.events==3);
    CHECK(app.IsHovered(item,0) && app.IsHovered(item,1));
    move.position={55,30};
    REQUIRE(app.RoutePointer(move));
    CHECK(item.left==1 && !app.IsHovered(item,1) && app.IsHovered(item,0));

    item.clearRoot=&app;
    move.position={3,3};
    auto dispatched=app.RoutePointer(move);
    REQUIRE(dispatched);
    CHECK(!dispatched.Value());
    CHECK(item.events==3);
}
} // namespace rocks::ui
