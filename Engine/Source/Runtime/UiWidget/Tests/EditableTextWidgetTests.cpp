#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/InputAdapter.h>
#include <UiWidget/EditableTextBoxWidget.generated.h>
#include <UiWidget/EditableTextWidget.generated.h>
#include <UiWidget/MultiLineEditableTextBoxWidget.generated.h>
#include <UiWidget/MultiLineEditableTextWidget.generated.h>
#include <UiWidget/WidgetTextShaper.h>
#include <UiWidget/WidgetTree.generated.h>

namespace rocks::ui {

namespace {

class InputTestShaper final : public WidgetTextShaper {
public:
    explicit InputTestShaper(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    Result<void> Shape(TextLabel& label, const Text& text, float size) noexcept override {
        TextPersistentView view;
        auto got = text.TryGetPersistentForm(view);
        if (!got) {
            return got;
        }

        Array<text::TextCluster> clusters(allocator_);
        auto points = view.source.TryCodePoints();
        if (!points) {
            return Result<void>::Failure(points.Error());
        }

        usize index = 0;
        for (char32_t point : points.Value()) {
            auto offset = view.source.TryByteOffsetOfCodePoint(++index);
            if (!offset) {
                return Result<void>::Failure(offset.Error());
            }

            if (point == 0x301 && !clusters.IsEmpty()) {
                clusters[clusters.Size() - 1].byteEnd = offset.Value();
                continue;
            }

            auto added = clusters.TryPushBack({.byteEnd = offset.Value(),
                                               .advance = point == '\n' ? 0.0f : 8.0f,
                                               .hardBreak = point == '\n'});
            if (!added) {
                return added;
            }
        }

        return label.SetShapedText(view.source, clusters.View(), {}, size, false);
    }

private:
    Allocator& allocator_;
};
} // namespace

TEST(UiWidget_EditableTextUsesClusterBoundariesAndPersistsInput) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 42});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto treeRef = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(treeRef);
    auto tree = objects.ResolveForWrite<WidgetTree>(access, treeRef.Value().Handle());
    REQUIRE(tree);
    auto shaper = SharedPtr<InputTestShaper>::TryMake(allocator, allocator);
    REQUIRE(shaper);
    auto& owner = *tree.Value().Get().Value();
    owner.SetTextShaper(shaper.Value());
    owner.SetPlatform({[](String& out) noexcept { return out.TryAssign("하\r\n🙂"); },
                       [](StringView text) noexcept {
                           return text == "🙂" ? Result<void>::Success()
                                               : Result<void>::Failure(ErrorCode::InvalidArgument);
                       }});

    const TypeId typesToTest[] = {GeneratedType<EditableTextWidget>::Id(),
                                  GeneratedType<EditableTextBoxWidget>::Id(),
                                  GeneratedType<MultiLineEditableTextWidget>::Id(),
                                  GeneratedType<MultiLineEditableTextBoxWidget>::Id()};
    constexpr StringView names[] = {"plain", "box", "lines", "linesBox"};
    for (usize i = 0; i < 4; ++i) {
        auto created = owner.ConstructWidget(objects, typesToTest[i], names[i]);
        REQUIRE(created);
        auto borrowed =
            objects.ResolveForWrite<EditableTextWidget>(access, created.Value().Handle());
        REQUIRE(borrowed);
        auto* widget = borrowed.Value().Get().Value();
        REQUIRE(widget->SetText("한"));
        auto live = widget->TakeElement(objects);
        REQUIRE(live);
        uint32 changes = 0, enters = 0;
        widget->SetOnChanged(Delegate<void(StringView)>::CreateLambda(
            [&changes](StringView) noexcept { ++changes; }));
        widget->SetOnCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
            [&enters](StringView, TextCommitReason why) noexcept {
                if (why == TextCommitReason::Enter) {
                    ++enters;
                }
            }));
        Application app(allocator);
        app.SetRoot(live.Value());
        REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 50}, {}), 1));
        REQUIRE(app.SetFocus(0, live.Value()));
        InputAdapter inputAdapter(app);
        using input::InputEvent;
        using input::Key;
        REQUIRE(inputAdapter.Route(InputEvent::Text('e')));
        REQUIRE(inputAdapter.Route(InputEvent::Text(0x301)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Backspace)));
        CHECK(widget->GetText() == "한");
        CHECK(changes == 3);
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Enter)));
        REQUIRE(inputAdapter.Route(InputEvent::Text('\r')));
        CHECK(enters == (i < 2 ? 1u : 0u));
        CHECK(widget->GetText() == (i < 2 ? StringView("한") : StringView("한\n")));

        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::A)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyUp(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::Text(0x1f642)));
        CHECK(widget->GetText() == "🙂");
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Z)));
        CHECK(widget->GetText() == (i < 2 ? StringView("한") : StringView("한\n")));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Y)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyUp(Key::LeftControl)));
        CHECK(widget->GetText() == "🙂");

        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::A)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::X)));
        CHECK(widget->GetText().IsEmpty());
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::V)));
        CHECK(widget->GetText() == (i < 2 ? StringView("하 🙂") : StringView("하\n🙂")));
        REQUIRE(inputAdapter.Route(InputEvent::KeyUp(Key::LeftControl)));
        REQUIRE(widget->SetText("🙂"));

        REQUIRE(widget->SetPassword(true));
        auto& label = static_cast<TextLabel&>(*live.Value()->ChildAt(0));
        CHECK(label.ShapedClusters().Size() == 1);
        CHECK(label.ShapedClusters()[0].byteEnd == 4);
        REQUIRE(inputAdapter.Route(InputEvent::Text('x')));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Z)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyUp(Key::LeftControl)));
        CHECK(widget->GetText() == "🙂x");
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Backspace)));
        REQUIRE(widget->SetPassword(false));

        const uint32 beforeComposition = changes;
        auto preedit = SharedPtr<String>::TryMake(allocator, allocator);
        REQUIRE(preedit);
        REQUIRE(preedit.Value()->TryAssign("가"));
        REQUIRE(inputAdapter.Route(InputEvent::Composition(preedit.Value(), 3)));
        CHECK(widget->GetText() == "🙂");
        CHECK(label.GetText() == "🙂가");
        CHECK(changes == beforeComposition);
        REQUIRE(inputAdapter.Route(InputEvent::Composition()));
        CHECK(label.GetText() == "🙂");
        REQUIRE(inputAdapter.Route(InputEvent::Composition(preedit.Value(), 3, true)));
        CHECK(widget->GetText() == "🙂가");
        CHECK(changes == beforeComposition + 1);
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Z)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyUp(Key::LeftControl)));
        CHECK(widget->GetText() == "🙂");
        widget->SetReadOnly(true);
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Backspace)));
        CHECK(widget->GetText() == "🙂");
        REQUIRE(app.UpdatePaintCache(Geometry::Root({80, 32}, {}), 1));
        app.SetRoot({});
        widget->ReleaseElements();
        REQUIRE(widget->TakeElement(objects));
        CHECK(widget->GetText() == "🙂");
        widget->SetOnChanged({});
        widget->SetOnCommitted({});
    }
}

TEST(UiWidget_EditableHintErrorAndFocusPoliciesUseTheModelText) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 1514});
    const auto access = objects.OwnerAccess();
    ObjectAccessScope scope(access);
    auto tree = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id());
    REQUIRE(tree);
    auto borrowed = objects.ResolveForWrite<WidgetTree>(access, tree.Value().Handle());
    REQUIRE(borrowed);
    auto* owner = borrowed.Value().Get().Value();
    auto shaper = SharedPtr<InputTestShaper>::TryMake(allocator, allocator);
    REQUIRE(shaper);
    owner->SetTextShaper(shaper.Value());
    auto handle =
        owner->ConstructWidget(objects, GeneratedType<EditableTextBoxWidget>::Id(), "Entry");
    REQUIRE(handle);
    auto widget = objects.ResolveForWrite<EditableTextBoxWidget>(access, handle.Value().Handle());
    REQUIRE(widget);
    auto* box = widget.Value().Get().Value();
    auto hint = Text::Invariant(allocator, "Hint");
    auto error = Text::Invariant(allocator, "Invalid");
    REQUIRE(hint && error && box->SetHintText(hint.Value()) && box->SetError(error.Value()));
    REQUIRE(box->SetFontSize(10));
    REQUIRE(box->SetMinDesiredWidth(60));
    auto built = box->TakeElement(objects);
    REQUIRE(built);
    auto live = built.Value();
    Application app(allocator);
    app.SetRoot(live);
    REQUIRE(app.UpdatePaintCache(Geometry::Root({120, 40}, {}), 1));
    CHECK(box->GetText().IsEmpty() && box->HasError());
    REQUIRE(live->ChildCount() == 3);
    CHECK(static_cast<TextLabel*>(live->ChildAt(1))->GetText() == "Hint");
    CHECK(static_cast<TextLabel*>(live->ChildAt(2))->GetText() == "Invalid");
    CHECK(live->ChildAt(2)->GetGeometry().layout.ty > live->ChildAt(0)->GetGeometry().layout.ty);
    CHECK(live->GetDesiredSize().x >= 68);
    REQUIRE(box->SetText("Before"));
    CHECK(live->ChildAt(1)->GetVisibility() == Visibility::Collapsed);
    box->SetRevertTextOnEscape(true);
    box->SetSelectAllTextWhenFocused(true);
    box->SetSelectAllTextOnCommit(true);
    REQUIRE(app.SetFocus(0, live));
    CHECK(box->SelectedText() == "Before");
    REQUIRE(app.RouteKey(0, input::InputEvent::Text('Z')));
    CHECK(box->GetText() == "Z");
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Escape)));
    CHECK(box->GetText() == "Before");
    REQUIRE(app.RouteKey(0, input::InputEvent::KeyDown(input::Key::Enter)));
    CHECK(box->SelectedText() == "Before");
    REQUIRE(box->SetText(""));
    CHECK(live->ChildAt(1)->GetVisibility() == Visibility::HitTestInvisible);
    REQUIRE(box->SetError({}));
    CHECK(!box->HasError() && live->ChildCount() == 2);
    app.SetRoot({});
}

} // namespace rocks::ui
