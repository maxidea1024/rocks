#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>

// 글리프 픽셀 맞춤 시험이다. 축 정렬 변환이면 글리프 왼쪽 위를 창 픽셀에 맞추고(가운데 정렬로 생긴
// 0.5 px가 보간으로 뿌옇게 보이지 않게), 회전·기울임이 있으면 그대로 둔다.

namespace rocks::ui {

namespace {

PaintGlyph MakeGlyph(Vec2 position) noexcept {
    return {position, {6, 10}, SlotHandle{3, 1}, {{0, 0}, {0.5f, 0.5f}}};
}

} // namespace

TEST(UiCore_GlyphRunSnapsToWholePixelsWhenAxisAligned) {
    SystemAllocator allocator;
    PaintList list(allocator);
    // 창 좌표 (10.5, 20.25)에 놓인 레이블 안 (0.25, 0.5) 글리프다.
    const Geometry label = Geometry::Root({100, 20}, {1, 0, 0, 1, 10.5f, 20.25f});
    const PaintGlyph glyphs[] = {MakeGlyph({0.25f, 0.5f}), MakeGlyph({6.5f, 0.5f})};
    REQUIRE(list.TryAddGlyphRun(label, glyphs, {1, 1, 1, 1}, 0));
    REQUIRE(list.Boxes().Size() == 2);
    for (const PaintBox& box : list.Boxes()) {
        const AffineTransform2D& at = box.geometry.render;
        CHECK(at.tx == math::Round(at.tx) && at.ty == math::Round(at.ty));
    }

    // 10.75 → 11, 20.75 → 21이다. 크기는 바뀌지 않는다.
    CHECK(list.Boxes()[0].geometry.render.tx == 11 && list.Boxes()[0].geometry.render.ty == 21);
    CHECK((list.Boxes()[0].geometry.size == Vec2{6, 10}));
}

TEST(UiCore_GlyphRunSnapsInScaledSpace) {
    SystemAllocator allocator;
    PaintList list(allocator);
    // 배율 1.5(DPI)에서는 창 픽셀 기준으로 맞춘다: 로컬 0.3 → 창 10 + 0.45 → 10.
    const Geometry label = Geometry::Root({100, 20}, {1.5f, 0, 0, 1.5f, 10, 4});
    const PaintGlyph glyph = MakeGlyph({0.3f, 0.5f});
    REQUIRE(list.TryAddGlyphRun(label, {&glyph, 1}, {1, 1, 1, 1}, 0));
    const AffineTransform2D& at = list.Boxes()[0].geometry.render;
    CHECK(math::Abs(at.tx - 10.0f) < 1e-4f && math::Abs(at.ty - 5.0f) < 1e-4f);
}

TEST(UiCore_GlyphRunKeepsSubpixelPositionWhenSheared) {
    SystemAllocator allocator;
    PaintList list(allocator);
    // 기울인 글자는 픽셀 격자와 맞지 않으므로 맞추지 않는다.
    const Geometry label = Geometry::Root({100, 20}, {1, 0, 0.4f, 1, 10.5f, 20.25f});
    const PaintGlyph glyph = MakeGlyph({0.25f, 0});
    REQUIRE(list.TryAddGlyphRun(label, {&glyph, 1}, {1, 1, 1, 1}, 0));
    CHECK(math::Abs(list.Boxes()[0].geometry.render.tx - 10.75f) < 1e-4f);
}

} // namespace rocks::ui
