#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiFramework/Popups/ToolTip.h>
#include <UiFramework/Text/TextLabel.h>

// 글자 넘침 정책 시험이다(Docs/Design/UiToolkit 9.1절 UW4o). 글자마다 폭 10인 "ABCDEFGHIJ"(100)를
// 폭 50에 넣고, 정책마다 남는 클러스터와 표지(폭 10) 위치를 글리프 상자로 단언한다. 글리프
// resource 번호로 어느 클러스터인지 가린다.

namespace rocks::ui {

namespace {

constexpr usize LetterCount = 10;
constexpr float Advance = 10;
constexpr float LineHeight = 12;
constexpr uint32 MarkerResource = 99;
constexpr uint32 FirstLetterResource = 10;

struct OverflowBench {
    SystemAllocator allocator;
    TextLabel label{allocator};
    SharedPtr<TextLabel> marker;
    PaintList paint{allocator};

    // rightToLeft면 모든 클러스터가 RTL(bidi 1)인 단락이다.
    bool Build(TextOverflow overflow, bool rightToLeft = false) noexcept {
        auto made = SharedPtr<TextLabel>::TryMake(allocator, allocator);
        if (!made) {
            return false;
        }

        marker = Move(made.Value());
        const text::TextCluster dot[] = {{.byteEnd = 3, .advance = Advance}};
        const LabelGlyph dots[] = {{0, {{}, {Advance, 8}, {MarkerResource, 1}, {{}, {1, 1}}}}};
        if (!marker->SetShapedText("\xE2\x80\xA6", dot, dots, LineHeight, false)) {
            return false;
        }

        text::TextCluster clusters[LetterCount];
        LabelGlyph glyphs[LetterCount];
        const uint8 level = rightToLeft ? 1 : 0;
        for (usize i = 0; i < LetterCount; ++i) {
            clusters[i] = {.byteEnd = i + 1, .advance = Advance, .bidiLevel = level};
            glyphs[i] = {i,
                         {{},
                          {Advance, 8},
                          {static_cast<uint32>(FirstLetterResource + i), 1},
                          {{}, {1, 1}}}};
        }

        label.SetEllipsisMarker(marker);
        label.SetOverflow(overflow);
        return static_cast<bool>(
            label.SetShapedText("ABCDEFGHIJ", clusters, glyphs, LineHeight, rightToLeft));
    }

    bool Paint(Vec2 size) noexcept {
        paint.Clear();
        return label.Arrange(Geometry::Root(size, {})) && label.OnPaint(paint, 0);
    }

    // resource 번호의 글리프 x다(없으면 -1).
    float XOf(uint32 resource) const noexcept {
        for (const PaintBox& box : paint.Boxes()) {
            if (box.resource == SlotHandle{resource, 1}) {
                return box.geometry.layout.Apply({}).x;
            }
        }

        return -1;
    }

    float LetterX(usize index) const noexcept {
        return XOf(static_cast<uint32>(FirstLetterResource + index));
    }

    float MarkerX() const noexcept {
        return XOf(MarkerResource);
    }

    usize BoxCount() const noexcept {
        return paint.Boxes().Size();
    }
};

} // namespace

TEST(UiFramework_TextOverflowEndKeepsStart) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::End));
    REQUIRE(bench.Paint({50, LineHeight}));
    // 표지 자리(10)를 빼고 40에 들어가는 A–D, 그 뒤 표지다.
    REQUIRE(bench.BoxCount() == 5);
    CHECK(bench.LetterX(0) == 0 && bench.LetterX(3) == 30 && bench.LetterX(4) == -1);
    CHECK(bench.MarkerX() == 40);
    CHECK(bench.label.IsTruncated());
}

TEST(UiFramework_TextOverflowStartKeepsEnd) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::Start));
    REQUIRE(bench.Paint({50, LineHeight}));
    // 앞을 표지로 바꾸고 끝 G–J를 남긴다(경로).
    REQUIRE(bench.BoxCount() == 5);
    CHECK(bench.MarkerX() == 0);
    CHECK(bench.LetterX(6) == 10 && bench.LetterX(9) == 40 && bench.LetterX(5) == -1);
}

TEST(UiFramework_TextOverflowMiddleKeepsBothEnds) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::Middle));
    REQUIRE(bench.Paint({50, LineHeight}));
    // 남긴 폭이 작은 쪽부터 번갈아 더한다: A·J·B·I, 표지는 가운데다.
    REQUIRE(bench.BoxCount() == 5);
    CHECK(bench.LetterX(0) == 0 && bench.LetterX(1) == 10);
    CHECK(bench.MarkerX() == 20);
    CHECK(bench.LetterX(8) == 30 && bench.LetterX(9) == 40);
    CHECK(bench.LetterX(2) == -1 && bench.LetterX(7) == -1);
}

TEST(UiFramework_TextOverflowClipDrawsNoMarker) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::Clip));
    REQUIRE(bench.Paint({50, LineHeight}));
    // 표지 없이 모든 글리프를 그대로 둔다(경계 자르기는 요소 설정 — UE Clip).
    CHECK(bench.BoxCount() == LetterCount && bench.MarkerX() == -1);
    CHECK(bench.label.IsTruncated());
}

TEST(UiFramework_TextOverflowNotAppliedWhenTextFits) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::Middle));
    REQUIRE(bench.Paint({100, LineHeight}));
    CHECK(bench.BoxCount() == LetterCount && bench.MarkerX() == -1);
    CHECK(!bench.label.IsTruncated());
}

TEST(UiFramework_TextOverflowCentersKeptTextWithMarker) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::End));
    REQUIRE(bench.label.SetLayoutOptions(0, text::TextAlignment::Center, false));
    REQUIRE(bench.Paint({55, LineHeight}));
    // 남긴 40 + 표지 10 = 50, 남는 5의 절반(2.5)만큼 민다. 글리프는 픽셀에 맞춰져 3·43이다.
    CHECK(bench.LetterX(0) == 3.0f && bench.MarkerX() == 43.0f);
}

TEST(UiFramework_TextOverflowRightToLeftUsesVisualOrder) {
    // RTL 단락은 논리 시작(A)이 시각 오른쪽이다. End는 논리 끝(시각 왼쪽)을 자르고 표지를 왼쪽에
    // 둔다.
    OverflowBench end;
    REQUIRE(end.Build(TextOverflow::End, true));
    REQUIRE(end.Paint({50, LineHeight}));
    REQUIRE(end.BoxCount() == 5);
    CHECK(end.MarkerX() == 0);
    CHECK(end.LetterX(3) == 10 && end.LetterX(0) == 40 && end.LetterX(4) == -1);
    // Start는 논리 앞(시각 오른쪽)을 자른다: J–G가 왼쪽에 남고 표지는 오른쪽이다.
    OverflowBench start;
    REQUIRE(start.Build(TextOverflow::Start, true));
    REQUIRE(start.Paint({50, LineHeight}));
    REQUIRE(start.BoxCount() == 5);
    CHECK(start.LetterX(9) == 0 && start.LetterX(6) == 30 && start.MarkerX() == 40);
}

TEST(UiFramework_TextOverflowSkipsLineWhenMarkerDoesNotFit) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::End));
    REQUIRE(bench.Paint({8, LineHeight}));
    // 표지(10)조차 들어가지 않는 폭이면 그 줄을 그리지 않는다(반쪽 글자를 남기지 않는다).
    CHECK(bench.BoxCount() == 0);
}

TEST(UiFramework_TextOverflowMultiLineEndMarksHiddenLines) {
    SystemAllocator allocator;
    const auto build = [&](TextLabel& label, SharedPtr<TextLabel>& marker,
                           TextOverflow overflow) noexcept {
        auto made = SharedPtr<TextLabel>::TryMake(allocator, allocator);
        if (!made) {
            return false;
        }

        marker = Move(made.Value());
        const text::TextCluster dot[] = {{.byteEnd = 3, .advance = Advance}};
        const LabelGlyph dots[] = {{0, {{}, {Advance, 8}, {MarkerResource, 1}, {{}, {1, 1}}}}};
        if (!marker->SetShapedText("\xE2\x80\xA6", dot, dots, LineHeight, false)) {
            return false;
        }

        // "AAA BBB": 공백 뒤에서 나뉘어 두 줄이다.
        constexpr StringView Text = "AAA BBB";
        text::TextCluster clusters[7];
        LabelGlyph glyphs[7];
        for (usize i = 0; i < Text.ByteCount(); ++i) {
            clusters[i] = {
                .byteEnd = i + 1, .advance = Advance, .breakAfter = Text.Data()[i] == ' '};
            glyphs[i] = {i, {{}, {Advance, 8}, {static_cast<uint32>(i + 1), 1}, {{}, {1, 1}}}};
        }

        label.SetEllipsisMarker(marker);
        label.SetOverflow(overflow);
        return label.SetShapedText(Text, clusters, glyphs, LineHeight, false) &&
               label.SetLayoutOptions(40, text::TextAlignment::Left, true);
    };
    const auto markerX = [](const PaintList& paint) noexcept {
        for (const PaintBox& box : paint.Boxes()) {
            if (box.resource == SlotHandle{MarkerResource, 1}) {
                return box.geometry.layout.Apply({}).x;
            }
        }

        return -1.0f;
    };

    // 한 줄 높이에 두 줄 글자: 여러 줄 끝은 첫 줄(들어가도) 끝에 표지를 둔다.
    TextLabel multi(allocator);
    SharedPtr<TextLabel> multiMarker;
    REQUIRE(build(multi, multiMarker, TextOverflow::MultiLineEnd));
    PaintList paint(allocator);
    REQUIRE(multi.Arrange(Geometry::Root({40, LineHeight}, {})));
    REQUIRE(multi.OnPaint(paint, 0));
    CHECK(markerX(paint) >= 0);
    // End(UE Ellipsis)는 높이 넘침에 표지를 두지 않는다.
    TextLabel end(allocator);
    SharedPtr<TextLabel> endMarker;
    REQUIRE(build(end, endMarker, TextOverflow::End));
    paint.Clear();
    REQUIRE(end.Arrange(Geometry::Root({40, LineHeight}, {})));
    REQUIRE(end.OnPaint(paint, 0));
    CHECK(markerX(paint) < 0);
    CHECK(end.IsTruncated());
}

TEST(UiFramework_TextOverflowFullTextToolTipOnlyWhenTruncated) {
    OverflowBench bench;
    REQUIRE(bench.Build(TextOverflow::End));
    REQUIRE(bench.label.SetFullTextToolTip(true));
    CHECK(bench.label.HasToolTip());
    // 잘렸으면 전체 글자(복사한 레이블)를 담은 툴팁이다.
    REQUIRE(bench.Paint({50, LineHeight}));
    auto tip = bench.label.MakeToolTip();
    REQUIRE(tip);
    REQUIRE(tip.Value()->ChildCount() == 1);
    auto* copy = static_cast<TextLabel*>(tip.Value()->ChildAt(0));
    CHECK(copy != &bench.label && copy->GetText() == "ABCDEFGHIJ");
    // 다 들어가면 툴팁을 만들지 않는다.
    REQUIRE(bench.Paint({120, LineHeight}));
    auto none = bench.label.MakeToolTip();
    CHECK(!none && none.Error() == ErrorCode::NotFound);
    // 끄면 툴팁 factory가 없다.
    REQUIRE(bench.label.SetFullTextToolTip(false));
    CHECK(!bench.label.HasToolTip());
}

} // namespace rocks::ui
