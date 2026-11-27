#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/DrawBatch.h>
#include <UiCore/PaintList.h>

using namespace rocks;
using namespace rocks::ui;

// 둥근 상자 브러시 시험이다(Docs/Design/UiToolkit 9.1절 UW3a — UE FSlateRoundedBoxBrush). 그리기
// 목록과 배치의 값(반경 방식, 명령 묶음)을 단언한다. 화소는 UI 갤러리 self-test가 본다.

namespace {

constexpr LinearColor Fill{0.2f, 0.3f, 0.4f, 1.0f};
constexpr LinearColor Outline{0.0f, 0.4f, 0.9f, 1.0f};

Geometry Box(float width, float height, float x = 0.0f) noexcept {
    return Geometry::Root({width, height}, {1, 0, 0, 1, x, 0});
}

} // namespace

TEST(UiCore_RoundedBoxBrushRecordsRadiiAndOutline) {
    SystemAllocator allocator;
    PaintList list(allocator);
    REQUIRE(list.TryAddBrush(Box(80, 24), Brush::RoundedBox(Fill, 4.0f, Outline, 1.0f),
                             {1, 1, 1, 1}, 0));
    // 반경 없이 만든 둥근 상자는 높이의 절반(알약)이다(UE HalfHeightRadius).
    REQUIRE(list.TryAddBrush(Box(60, 20, 100), Brush::PillBox(Fill), {1, 1, 1, 1}, 0));
    const Span<const PaintBox> boxes = list.Boxes();
    REQUIRE(boxes.Size() == 2);
    CHECK(boxes[0].roundedBox && boxes[0].roundedSize == Vec2(80, 24));
    CHECK(boxes[0].cornerRadius == Vec4(4, 4, 4, 4));
    CHECK(boxes[0].outlineWidth == 1.0f && boxes[0].outlineColor == Outline);
    CHECK(boxes[0].color == Fill);
    CHECK(boxes[0].uvStart == Vec2(0, 0) && boxes[0].uvEnd == Vec2(1, 1));
    CHECK(boxes[1].roundedBox && boxes[1].cornerRadius == Vec4(10, 10, 10, 10));
    CHECK(boxes[1].outlineWidth == 0.0f);
}

TEST(UiCore_RoundedBoxBatchesOnlyIdenticalBoxes) {
    SystemAllocator allocator;
    PaintList list(allocator);
    const Brush brush = Brush::RoundedBox(Fill, 4.0f, Outline, 1.0f);
    // 같은 크기·반경·외곽선은 한 명령, 크기가 다르면 새 명령(UE도 ShaderParams가 배치 키다), 평범한
    // 상자는 다시 새 명령이다.
    REQUIRE(list.TryAddBrush(Box(80, 24), brush, {1, 1, 1, 1}, 0));
    REQUIRE(list.TryAddBrush(Box(80, 24, 100), brush, {1, 1, 1, 1}, 0));
    REQUIRE(list.TryAddBrush(Box(40, 24, 200), brush, {1, 1, 1, 1}, 0));
    Brush plain;
    plain.mode = BrushMode::Box;
    REQUIRE(list.TryAddBrush(Box(40, 24, 300), plain, {1, 1, 1, 1}, 0));
    DrawBatch batch(allocator);
    REQUIRE(batch.Build(list));
    const Span<const DrawCommand> commands = batch.Commands();
    REQUIRE(commands.Size() >= 3);
    CHECK(commands[0].roundedBox && commands[0].indexCount == 12);
    CHECK(commands[0].roundedSize == Vec2(80, 24) && commands[0].outlineColor == Outline);
    CHECK(commands[1].roundedBox && commands[1].roundedSize == Vec2(40, 24));
    CHECK(!commands[2].roundedBox);
}

TEST(UiCore_RoundedBoxRejectsBadOutline) {
    SystemAllocator allocator;
    PaintList list(allocator);
    CHECK(!list.TryAddBrush(Box(80, 24), Brush::RoundedBox(Fill, -1.0f), {1, 1, 1, 1}, 0));
    CHECK(!list.TryAddBrush(Box(80, 24), Brush::RoundedBox(Fill, 4.0f, Outline, -1.0f),
                            {1, 1, 1, 1}, 0));
    CHECK(list.Boxes().Size() == 0);
}
