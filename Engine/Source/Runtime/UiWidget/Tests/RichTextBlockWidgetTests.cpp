#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiWidget/RichTextBlockWidget.generated.h>
#include <UiWidget/RichTextImageDecorator.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

// 테스트 셰이퍼는 ASCII와 인라인 객체만 처리한다. 실제 GPU 없이도 아틀라스 선택과 색을 검증한다.
class RunShaper final : public WidgetTextShaper {
public:
    Result<void> Shape(TextLabel& label, const Text& text, float size) noexcept override {
        TextPersistentView view;
        auto form = text.TryGetPersistentForm(view);
        if (!form) {
            return form;
        }

        if (view.source != "AB\xEF\xBF\xBC") {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }

        const text::TextCluster clusters[] = {{.byteEnd = 1, .advance = 4, .breakAfter = true},
                                              {.byteEnd = 2, .advance = 4, .breakAfter = true},
                                              {.byteEnd = 5, .advance = 4, .breakAfter = true}};
        const PaintGlyph letter{{0, 0}, {4, 8}, {0, 1}, {{0, 0}, {1, 1}}};
        const LabelGlyph glyphs[] = {{0, letter}, {1, letter}, {2, letter}};
        return label.SetShapedText(view.source, clusters, glyphs, size, false);
    }
};

} // namespace

TEST(UiWidget_RichTextStylesAndInlineImagePreserveClusterLayout) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration modules(types, allocator);
    REQUIRE(RegisterWidgetTypes(modules));
    ObjectRegistry objects(allocator, types, {0x554957, 41});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);

    auto root = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(root);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, root.Value().Handle());
    REQUIRE(tree);
    auto* widgets = tree.Value().Get().Value();
    auto handle =
        widgets->ConstructWidget(objects, GeneratedType<RichTextBlockWidget>::Id(), "Tooltip");
    REQUIRE(handle);
    auto rich = objects.ResolveForWrite<RichTextBlockWidget>(access, handle.Value().Handle());
    REQUIRE(rich);
    auto* widget = rich.Value().Get().Value();

    auto shaper = SharedPtr<RunShaper>::TryMake(allocator);
    auto decorator = SharedPtr<RichTextImageDecorator>::TryMake(allocator, allocator);
    REQUIRE(shaper && decorator);
    REQUIRE(decorator.Value()->SetImage("coin", {{0, 0}, {8, 8}, {1, 1}, {{0, 0}, {1, 1}}}));
    widgets->SetTextShaper(shaper.Value());
    RichTextStyle style(allocator);
    REQUIRE(style.name.TryAssign("rare"));
    const LinearColor gold{1, 0.5f, 0, 1};
    style.tint = gold;
    REQUIRE(widget->SetStyles({&style, 1}));
    REQUIRE(widget->SetDecorator(decorator.Value()));
    REQUIRE(widget->SetFontSize(8));
    REQUIRE(widget->SetWrapWidth(8));
    auto markup = Text::Invariant(allocator, "A<rare>B</><img id=\"coin\"/>");
    REQUIRE(markup && widget->SetText(markup.Value()));

    auto native = widget->TakeElement(objects);
    REQUIRE(native);
    REQUIRE(native.Value()->Prepass(1));
    CHECK(native.Value()->GetDesiredSize() == Vec2(8, 16));
    REQUIRE(native.Value()->Arrange(Geometry::Root({8, 16}, {})));
    PaintList list(allocator);
    REQUIRE(native.Value()->Paint(list, 0));
    REQUIRE(list.Boxes().Size() == 3);
    CHECK(list.Boxes()[0].color == LinearColor(1, 1, 1, 1));
    CHECK(list.Boxes()[1].color == gold);
    CHECK(list.Boxes()[2].resource.index == 1);
    CHECK(list.Boxes()[2].geometry.layout.Apply({0, 0}) == Vec2(0, 8));

    style.underline = true;
    style.strikeThrough = true;
    REQUIRE(widget->SetStyles({&style, 1}));
    widget->SetClipText(true);
    REQUIRE(native.Value()->ClipsToBounds());
    list.Clear();
    REQUIRE(native.Value()->Paint(list, 0));
    REQUIRE(list.Boxes().Size() == 5);
    CHECK(list.Boxes()[3].color == gold && list.Boxes()[4].color == gold);
    CHECK(list.Boxes()[3].geometry.size == Vec2(4, 1));
    CHECK(list.Boxes()[3].geometry.layout.Apply({}) == Vec2(4, 7));
    CHECK(list.Boxes()[4].geometry.layout.Apply({}) == Vec2(4, 3.5f));
    CHECK(list.Boxes()[3].hasClip);

    auto invalid = Text::Invariant(allocator, "<other>A</>");
    REQUIRE(invalid);
    CHECK(!widget->SetText(invalid.Value()));
    CHECK(static_cast<const TextLabel&>(*native.Value()).GetText() == "AB\xEF\xBF\xBC");
}

} // namespace rocks::ui
