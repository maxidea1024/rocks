#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application.h>
#include <UiFramework/Frame.h>

namespace rocks::ui {

TEST(UiFramework_FrameContentAlignmentPlacesDesiredSize) {
    SystemAllocator allocator;
    auto outer = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto inner = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(outer && inner);
    inner.Value()->SetDesiredSize({20, 10});
    REQUIRE(outer.Value()->Construct({.content = inner.Value(), .padding = {2, 4, 6, 8}}));
    Application app(allocator);
    app.SetRoot(outer.Value());
    // 기본은 Fill: 여백 안 전체(100-8, 60-12)를 채운다.
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    const auto& filled = inner.Value()->GetGeometry();
    CHECK(filled.size == Vec2(92, 48));
    CHECK(filled.layout.tx == 2 && filled.layout.ty == 4);
    // 가운데: 원하는 크기 그대로 여백 안 가운데에 놓는다.
    outer.Value()->SetContentAlignment(StackAlignment::Center, StackAlignment::Center);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    const auto& centered = inner.Value()->GetGeometry();
    CHECK(centered.size == Vec2(20, 10));
    CHECK(centered.layout.tx == 2 + 36 && centered.layout.ty == 4 + 19);
    // 끝·시작과 한 축만 Fill.
    outer.Value()->SetContentAlignment(StackAlignment::End, StackAlignment::Fill);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    const auto& ended = inner.Value()->GetGeometry();
    CHECK(ended.size == Vec2(20, 48) && ended.layout.tx == 2 + 72 && ended.layout.ty == 4);
    outer.Value()->SetContentAlignment(StackAlignment::Start, StackAlignment::End);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    const auto& started = inner.Value()->GetGeometry();
    CHECK(started.layout.tx == 2 && started.layout.ty == 4 + 38);
    // 칸이 원하는 크기보다 작으면 칸 크기로 줄인다.
    REQUIRE(app.UpdatePaintCache(Geometry::Root({16, 14}, {}), 1));
    CHECK(inner.Value()->GetGeometry().size == Vec2(8, 2));
}

TEST(UiFramework_FrameContentAlignmentArgsAndEdgeCases) {
    SystemAllocator allocator;
    auto outer = SharedPtr<Frame>::TryMake(allocator, allocator);
    auto inner = SharedPtr<Frame>::TryMake(allocator, allocator);
    REQUIRE(outer && inner);
    inner.Value()->SetDesiredSize({20, 10});
    // 선언형 인자로도 정렬을 준다(UE SBorder HAlign·VAlign).
    REQUIRE(outer.Value()->Construct({.content = inner.Value(),
                                      .horizontalAlignment = StackAlignment::Center,
                                      .verticalAlignment = StackAlignment::End}));
    Application app(allocator);
    app.SetRoot(outer.Value());
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    CHECK(inner.Value()->GetGeometry().layout.tx == 40 &&
          inner.Value()->GetGeometry().layout.ty == 50);
    // 인자를 비우고 다시 만들면 지금 정렬을 그대로 둔다(파생 부품이 생성자에서 정한 정렬 보존).
    REQUIRE(outer.Value()->RemoveChild(*inner.Value()));
    REQUIRE(outer.Value()->Construct({.content = inner.Value()}));
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    CHECK(inner.Value()->GetGeometry().layout.tx == 40 &&
          inner.Value()->GetGeometry().layout.ty == 50);
    // 칸이 원하는 크기보다 작으면 가운데·끝 정렬도 칸 크기로 줄인다.
    REQUIRE(app.UpdatePaintCache(Geometry::Root({8, 6}, {}), 1));
    CHECK(inner.Value()->GetGeometry().size == Vec2(8, 6));
    // 접힌 자식은 배치하지 않고 원하는 크기에서도 빠진다.
    inner.Value()->SetVisibility(Visibility::Collapsed);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 60}, {}), 1));
    CHECK(outer.Value()->GetDesiredSize() == Vec2(0, 0));
}

} // namespace rocks::ui
