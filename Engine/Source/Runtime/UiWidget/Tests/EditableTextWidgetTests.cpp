#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Application/InputAdapter.h>
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

// 창 호스트 대신 문맥 호출을 기록하는 서비스다(19.2절). 시험이 플랫폼 기록 세션처럼 문맥을 직접
// 부른다.
class FakeTextInput final : public TextInputService {
public:
    void Activate(TextInputContext& context) noexcept override {
        active = &context;
        ++activations;
    }

    void Deactivate(TextInputContext& context) noexcept override {
        // 플랫폼처럼 만들던 글자를 확정한 뒤 끈다(19.2절).
        if (active == &context && completeOnDeactivate && context.IsComposing()) {
            context.EndComposition();
        }

        if (active == &context) {
            active = nullptr;
        }

        ++deactivations;
    }

    void CancelComposition(TextInputContext&) noexcept override {
        ++cancels;
    }

    void NotifyLayoutChanged(TextInputContext&) noexcept override {
        ++layouts;
    }

    // 표지 자리에서 플랫폼 기록처럼 확정 하나를 문맥에 적용한다(commit이 있으면).
    void Process(uint32 record) noexcept override {
        processed = record;
        if (active && !commit.IsEmpty()) {
            uint32 begin = 0, length = 0;
            active->GetSelection(begin, length);
            active->BeginComposition();
            active->SetText(begin, length, commit);
            active->SetSelection(begin + static_cast<uint32>(commit.Size()), 0);
            active->EndComposition();
        }
    }

    Span<const char16_t> commit;
    bool completeOnDeactivate = false;
    TextInputContext* active = nullptr;
    uint32 activations = 0;
    uint32 deactivations = 0;
    uint32 cancels = 0;
    uint32 layouts = 0;
    uint32 processed = 0;
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
        auto service = SharedPtr<FakeTextInput>::TryMake(allocator);
        REQUIRE(service);
        Application app(allocator);
        app.SetRoot(live.Value());
        REQUIRE(app.SetTextInputService(service.Value()));
        REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 50}, {}), 1));
        REQUIRE(app.SetFocus(0, live.Value()));
        // 초점을 받은 편집 요소가 창의 텍스트 입력 문맥이 된다.
        TextInputContext* context = service.Value()->active;
        REQUIRE(context);
        CHECK(service.Value()->activations == 1);
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
        // 비밀번호는 IME를 끈다 — 서비스에 다시 알린다.
        CHECK(!context->AllowsInputMethod());
        CHECK(service.Value()->activations == 2);
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

        CHECK(context->AllowsInputMethod());

        // 위치는 UTF-16이다: "🙂"는 surrogate 쌍 둘이다.
        CHECK(context->GetTextLength() == 2);
        uint32 selectionBegin = 0, selectionLength = 0;
        context->GetSelection(selectionBegin, selectionLength);
        CHECK(selectionBegin == 2);
        CHECK(selectionLength == 0);

        // 조합은 미리보기 층만 바꾼다(위젯 글자·변경 알림 없음). 조합 중 키는 IME 몫이다.
        const uint32 beforeComposition = changes;
        const char16_t ga[] = {0xAC00};
        const char16_t gak[] = {0xAC01};
        context->BeginComposition();
        CHECK(context->IsComposing());
        context->SetText(2, 0, ga);
        context->UpdateCompositionRange(2, 1);
        context->SetSelection(3, 0);
        CHECK(widget->GetText() == "🙂");
        CHECK(label.GetText() == "🙂가");
        CHECK(context->GetTextLength() == 3);
        CHECK(changes == beforeComposition);
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Backspace)));
        CHECK(label.GetText() == "🙂가");
        context->SetText(2, 1, gak);
        CHECK(label.GetText() == "🙂각");
        // 다시 배치하면 후보 창 위치를 다시 당기게 알린다.
        const uint32 layouts = service.Value()->layouts;
        REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 50}, {}), 1));
        CHECK(service.Value()->layouts > layouts);
        Box2<float> caret;
        CHECK(context->GetTextBounds(3, 0, caret));
        CHECK(caret.max.y > caret.min.y);
        Box2<float> field;
        CHECK(context->GetScreenBounds(field));
        CHECK(field.min.x <= caret.min.x);

        // 확정 없이 끝나면(취소) 조합 전과 같다 — 이력·알림 없음.
        context->SetText(2, 1, {});
        context->SetSelection(2, 0);
        context->EndComposition();
        CHECK(!context->IsComposing());
        CHECK(label.GetText() == "🙂");
        CHECK(changes == beforeComposition);

        // 확정은 한 번이다: 조합 갱신이 여럿이어도 변경 알림 하나, 편집 이력 하나.
        context->BeginComposition();
        context->SetText(2, 0, ga);
        context->SetText(2, 1, gak);
        context->SetText(2, 1, ga);
        context->SetSelection(3, 0);
        context->EndComposition();
        CHECK(widget->GetText() == "🙂가");
        CHECK(changes == beforeComposition + 1);
        Array<char16_t> units(allocator);
        REQUIRE(context->GetText(0, 3, units));
        REQUIRE(units.Size() == 3);
        CHECK(units[0] == 0xD83D);
        CHECK(units[1] == 0xDE42);
        CHECK(units[2] == 0xAC00);
        CHECK(context->GetCharacterIndexFromPoint({1000, 10}).HasValue());
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::LeftControl)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyDown(Key::Z)));
        REQUIRE(inputAdapter.Route(InputEvent::KeyUp(Key::LeftControl)));
        CHECK(widget->GetText() == "🙂");

        // IME 표지 이벤트는 그 자리에서 서비스로 간다.
        REQUIRE(inputAdapter.Route(InputEvent::TextInputMethod(7)));
        CHECK(service.Value()->processed == 7);

        // 조합 중 앱이 글자를 바꾸면 미리보기를 버리고 IME 조합을 취소한다.
        context->BeginComposition();
        context->SetText(2, 0, ga);
        const uint32 cancels = service.Value()->cancels;
        REQUIRE(widget->SetText("x"));
        CHECK(service.Value()->cancels == cancels + 1);
        CHECK(!context->IsComposing());
        CHECK(label.GetText() == "x");
        REQUIRE(widget->SetText("🙂"));
        widget->SetReadOnly(true);
        CHECK(!context->AllowsInputMethod());
        CHECK(context->IsReadOnly());
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

// 문맥을 플랫폼처럼 부르는 경로다(19.2절): 표지 뒤의 키는 확정 뒤에 움직이고, 초점을 잃으면 만들던
// 글자가 확정되어 Commit(FocusLost)이 보며, 조합 중 소멸은 확정 없이 서비스에서 빠진다.
TEST(UiWidget_EditableTextInputContextFollowsPlatformOrder) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    ModuleRegistration registration(types, allocator);
    REQUIRE(RegisterWidgetTypes(registration));
    ObjectRegistry objects(allocator, types, {0x554957, 1515});
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
    auto created = owner.ConstructWidget(objects, GeneratedType<EditableTextWidget>::Id(), "ime");
    REQUIRE(created);
    auto borrowed = objects.ResolveForWrite<EditableTextWidget>(access, created.Value().Handle());
    REQUIRE(borrowed);
    auto* widget = borrowed.Value().Get().Value();
    REQUIRE(widget->SetText("a"));
    uint32 focusLostCommits = 0;
    String committedText(allocator);
    widget->SetOnCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
        [&](StringView text, TextCommitReason why) noexcept {
            if (why == TextCommitReason::FocusLost) {
                ++focusLostCommits;
                (void)committedText.TryAssign(text);
            }
        }));
    auto live = widget->TakeElement(objects);
    REQUIRE(live);
    auto service = SharedPtr<FakeTextInput>::TryMake(allocator);
    REQUIRE(service);
    Application app(allocator);
    app.SetRoot(live.Value());
    REQUIRE(app.SetTextInputService(service.Value()));
    REQUIRE(app.UpdatePaintCache(Geometry::Root({100, 50}, {}), 1));
    REQUIRE(app.SetFocus(0, live.Value()));
    TextInputContext* context = service.Value()->active;
    REQUIRE(context);
    InputAdapter inputAdapter(app);
    using input::InputEvent;
    using input::Key;

    // [확정 "가" 표지, KeyDown(Left)]: 확정이 먼저 들어가고 Left는 그 앞으로 간다.
    const char16_t ga[] = {0xAC00};
    service.Value()->commit = ga;
    const InputEvent batch[] = {InputEvent::TextInputMethod(1), InputEvent::KeyDown(Key::Left)};
    uint8 consumed[2] = {};
    REQUIRE(inputAdapter.RouteBatch(batch, consumed, 0));
    CHECK(widget->GetText() == "a가");
    uint32 begin = 0, length = 0;
    context->GetSelection(begin, length);
    CHECK(begin == 1);
    CHECK(length == 0);
    service.Value()->commit = {};

    // 초점을 잃으면 만들던 글자를 확정하고, Commit(FocusLost)이 그 글자를 본다.
    service.Value()->completeOnDeactivate = true;
    context->BeginComposition();
    context->SetText(1, 0, ga);
    context->UpdateCompositionRange(1, 1);
    CHECK(widget->GetText() == "a가");
    REQUIRE(app.SetFocus(0, SharedPtr<Element>{}));
    CHECK(!service.Value()->active);
    CHECK(widget->GetText() == "a가가");
    CHECK(focusLostCommits == 1);
    CHECK(committedText.View() == "a가가");

    // 조합 중 소멸: 초점 잃음 없이 부서지면 미리보기를 확정하지 않고, 조합 취소 → 끄기 순으로
    // 서비스에서 빠진다.
    app.SetRoot({});
    widget->ReleaseElements();
    live.Value().Reset();
    auto orphan = widget->TakeElement(objects);
    REQUIRE(orphan);
    auto orphanHost = SharedPtr<UiHostServices>::TryMake(allocator);
    REQUIRE(orphanHost);
    orphanHost.Value()->SetTextInputService(service.Value());
    REQUIRE(orphan.Value()->SetUiHostServices(orphanHost.Value()));
    orphan.Value()->OnFocusReceived(0);
    context = service.Value()->active;
    REQUIRE(context);
    context->BeginComposition();
    context->SetText(0, 0, ga);
    const uint32 cancels = service.Value()->cancels;
    const uint32 deactivations = service.Value()->deactivations;
    widget->ReleaseElements();
    orphan.Value().Reset();
    CHECK(service.Value()->cancels == cancels + 1);
    CHECK(service.Value()->deactivations == deactivations + 1);
    CHECK(!service.Value()->active);
    CHECK(widget->GetText() == "a가가");
    widget->SetOnCommitted({});
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
