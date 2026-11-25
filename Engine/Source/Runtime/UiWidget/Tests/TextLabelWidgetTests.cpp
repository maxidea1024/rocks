#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/TextLabelWidget.generated.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

// 실제 글꼴 서비스 대신 클러스터 하나의 폭을 고정해 재구성과 실패 처리를 검사한다.
class TestTextShaper final : public WidgetTextShaper {
public:
    Result<void> Shape(TextLabel& label, const Text& value, float size) noexcept override {
        TextPersistentView view;
        auto persisted = value.TryGetPersistentForm(view);
        if (!persisted) {
            return persisted;
        }

        if (view.source == "…") {
            const text::TextCluster clusters[] = {{.byteEnd = 3, .advance = size / 2}};
            const LabelGlyph glyphs[] = {{0, {{}, {size / 2, size}, {2, 1}, {{}, {1, 1}}}}};
            return label.SetShapedText(view.source, clusters, glyphs, size, false);
        }

        if (view.source != "A") {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const text::TextCluster clusters[] = {{.byteEnd = 1, .advance = size}};
        return label.SetShapedText(view.source, clusters, {}, size, false);
    }
};

} // namespace

TEST(UiWidget_TextShaperFailurePreservesPublishedText) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 29});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto root = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(root);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, root.Value().Handle());
    REQUIRE(tree);
    auto* widgets = tree.Value().Get().Value();
    auto handle = widgets->ConstructWidget(objects, GeneratedType<TextLabelWidget>::Id(), "Score");
    REQUIRE(handle);
    auto widget = objects.ResolveForWrite<TextLabelWidget>(access, handle.Value().Handle());
    REQUIRE(widget);
    auto* label = widget.Value().Get().Value();
    auto text = Text::Invariant(allocator, "A");
    REQUIRE(text && label->SetText(text.Value()));
    auto unconfigured = label->TakeElement(objects);
    CHECK(!unconfigured && unconfigured.Error() == ErrorCode::NotReady);
    auto shaper = SharedPtr<TestTextShaper>::TryMake(allocator);
    REQUIRE(shaper);
    widgets->SetTextShaper(shaper.Value());
    auto element = label->TakeElement(objects);
    REQUIRE(element);
    auto& native = static_cast<TextLabel&>(*element.Value());
    CHECK(native.GetText() == "A");
    REQUIRE(label->SetJustification(text::TextAlignment::Right));
    REQUIRE(label->SetWrapWidth(80));
    REQUIRE(native.ArrangeIfNeeded(Geometry::Root({100, 40}, {})));
    CHECK(native.Layout().Lines()[0].x == 84);
    CHECK(native.HitText({85, 0}) == 0);
    REQUIRE(label->SetFontSize(24));
    REQUIRE(native.Prepass(1));
    CHECK(native.GetDesiredSize().x == 24);
    auto unsupported = Text::Invariant(allocator, "B");
    REQUIRE(unsupported);
    CHECK(!label->SetText(unsupported.Value()));
    CHECK(native.GetText() == "A");
    REQUIRE(label->SetEllipsis(true));
    REQUIRE(native.ArrangeIfNeeded(Geometry::Root({14, 28}, {})));
    PaintList shortLine(allocator);
    REQUIRE(native.OnPaint(shortLine, 0));
    REQUIRE(shortLine.Boxes().Size() == 1);
    CHECK(shortLine.Boxes()[0].geometry.size.x == 12);
    CHECK(native.DefaultAccessibleText() == "A");
    REQUIRE(label->SetText({}));
    CHECK(native.GetText().IsEmpty());
}

TEST(UiWidget_AutoWrapReflowsClustersAtTheAllottedWidth) {
    SystemAllocator allocator;
    TextLabel label(allocator);
    const text::TextCluster clusters[] = {
        {.byteEnd = 1, .advance = 8},
        {.byteEnd = 2, .advance = 8, .breakAfter = true},
        {.byteEnd = 3, .advance = 8},
    };
    REQUIRE(label.SetShapedText("abc", clusters, {}, 10, false));
    REQUIRE(label.SetLayoutOptions(24, text::TextAlignment::Center, true));
    REQUIRE(label.ArrangeIfNeeded(Geometry::Root({16, 30}, {})));
    REQUIRE(label.Layout().Lines().Size() == 2);
    CHECK(label.Layout().Lines()[1].x == 4);
    const auto caret = label.CaretPosition(2, false);
    REQUIRE(caret);
    CHECK(caret.Value() == Vec2(4, 10));
    REQUIRE(label.Prepass(1));
    CHECK(label.GetDesiredSize() == Vec2(16, 20));
    REQUIRE(label.SetLayoutOptions(6, text::TextAlignment::Left, true, text::TextWrapPolicy::Word));
    REQUIRE(label.Layout().Lines().Size() == 2);
    CHECK(label.Layout().Lines()[0].width == 16);
    REQUIRE(label.SetLayoutOptions(6, text::TextAlignment::Left, true,
                                   text::TextWrapPolicy::WordOrCluster));
    REQUIRE(label.Layout().Lines().Size() == 3);
    REQUIRE(label.SetLayoutOptions(24, text::TextAlignment::Center, true));
    REQUIRE(label.ArrangeIfNeeded(Geometry::Root({40, 30}, {})));
    REQUIRE(label.Layout().Lines().Size() == 1);
    CHECK(label.Layout().Lines()[0].x == 8);
    REQUIRE(label.Prepass(1));
    CHECK(label.GetDesiredSize() == Vec2(24, 10));
}

TEST(UiWidget_ParagraphMarginsAndLineHeightUseTheSameCoordinatesAsTheCaret) {
    SystemAllocator allocator;
    TextLabel label(allocator);
    const text::TextCluster clusters[] = {
        {.byteEnd = 1, .advance = 8}, {.byteEnd = 2, .advance = 8}, {.byteEnd = 3, .advance = 8}};
    REQUIRE(label.SetShapedText("abc", clusters, {}, 10, false));
    REQUIRE(label.SetLayoutOptions(27, text::TextAlignment::Right, false));
    REQUIRE(label.SetParagraphStyle({{3, 4, 5, 6}, 2, false}, {}, {}));
    REQUIRE(label.Prepass(1));
    CHECK(label.GetDesiredSize() == Vec2(24, 40));
    auto rows = label.Layout().Lines();
    REQUIRE(rows.Size() == 2);
    CHECK(rows[0].x == 6 && rows[0].y == 4 && rows[0].height == 20);
    CHECK(rows[1].x == 14 && rows[1].y == 24 && rows[1].height == 10);
    auto point = label.CaretPosition(2, false);
    REQUIRE(point);
    CHECK(point.Value() == Vec2(14, 24));
    CHECK(label.HitText(point.Value()) == 2);
    CHECK(!label.SetParagraphStyle({{}, 0, true}, {}, {}));
    CHECK(label.Layout().Lines()[1].y == 24);
    REQUIRE(label.SetParagraphStyle({{3, 4, 5, 6}, 2, true}, {}, {}));
    REQUIRE(label.Prepass(1));
    CHECK(label.GetDesiredSize() == Vec2(24, 50));
}

TEST(UiWidget_TextShadowPreservesGlyphAlphaAndReservesNegativeOffsets) {
    SystemAllocator allocator;
    TextLabel label(allocator);
    const text::TextCluster clusters[] = {{.byteEnd = 1, .advance = 6}};
    const LabelGlyph glyphs[] = {{0, {{0, 0}, {6, 8}, {1, 1}, {{0, 0}, {1, 1}}, {1, 0, 0, 0.5f}}}};
    REQUIRE(label.SetShapedText("x", clusters, glyphs, 8, false));
    REQUIRE(label.SetParagraphStyle({}, {-2, 3}, {0, 1, 0, 0.5f}));
    REQUIRE(label.Prepass(1));
    CHECK(label.GetDesiredSize() == Vec2(8, 11));
    REQUIRE(label.Arrange(Geometry::Root({8, 11}, {})));
    PaintList list(allocator);
    REQUIRE(label.OnPaint(list, 0));
    REQUIRE(list.Boxes().Size() == 2);
    CHECK(list.Boxes()[0].geometry.layout.Apply({}) == Vec2(0, 3));
    CHECK(list.Boxes()[0].color == LinearColor(0, 1, 0, 0.25f));
    CHECK(list.Boxes()[1].geometry.layout.Apply({}) == Vec2(2, 0));
    CHECK(list.Boxes()[1].color == LinearColor(1, 0, 0, 0.5f));
}

TEST(UiWidget_EllipsisKeepsWholeVisualClustersAndMarksHiddenLines) {
    SystemAllocator allocator;
    TextLabel label(allocator);
    auto marker = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    REQUIRE(marker);
    const text::TextCluster dot[] = {{.byteEnd = 3, .advance = 3}};
    const LabelGlyph dots[] = {{0, {{}, {3, 8}, {9, 1}, {{}, {1, 1}}}}};
    REQUIRE(marker.Value()->SetShapedText("…", dot, dots, 8, false));
    label.SetEllipsisMarker(marker.Value());

    // 첫 클러스터는 기본 글자와 결합 부호 두 글리프로 그린다.
    const text::TextCluster source[] = {{.byteEnd = 3, .advance = 6, .bidiLevel = 1},
                                        {.byteEnd = 4, .advance = 6, .bidiLevel = 1},
                                        {.byteEnd = 5, .advance = 6, .bidiLevel = 1}};
    const LabelGlyph glyphs[] = {{0, {{}, {6, 8}, {1, 1}, {{}, {1, 1}}}},
                                 {0, {{1, 0}, {2, 2}, {2, 1}, {{}, {1, 1}}}},
                                 {1, {{}, {6, 8}, {3, 1}, {{}, {1, 1}}}},
                                 {2, {{}, {6, 8}, {4, 1}, {{}, {1, 1}}}}};
    REQUIRE(label.SetShapedText("a\xcc\x81"
                                "bc",
                                source, glyphs, 8, true));
    REQUIRE(label.Arrange(Geometry::Root({10, 8}, {})));
    PaintList paint(allocator);
    REQUIRE(label.OnPaint(paint, 0));
    REQUIRE(paint.Boxes().Size() == 3);
    CHECK(paint.Boxes()[0].resource == glyphs[0].glyph.resource);
    CHECK(paint.Boxes()[1].resource == glyphs[1].glyph.resource);
    CHECK(paint.Boxes()[0].geometry.layout.Apply({}).x == 3);
    CHECK(paint.Boxes()[2].resource == dots[0].glyph.resource);
    CHECK(paint.Boxes()[2].geometry.layout.Apply({}).x == 0);

    REQUIRE(label.SetLayoutOptions(12, text::TextAlignment::Left, false));
    REQUIRE(label.Arrange(Geometry::Root({18, 8}, {})));
    paint.Clear();
    REQUIRE(label.OnPaint(paint, 0));
    REQUIRE(paint.Boxes().Size() == 4);
    CHECK(paint.Boxes()[3].resource == dots[0].glyph.resource);
    REQUIRE(label.Arrange(Geometry::Root({18, 16}, {})));
    paint.Clear();
    REQUIRE(label.OnPaint(paint, 0));
    REQUIRE(paint.Boxes().Size() == 4);
    CHECK(paint.Boxes()[3].resource == glyphs[3].glyph.resource);
}

} // namespace rocks::ui
