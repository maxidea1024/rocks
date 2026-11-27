#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/EditableTextBox.h>

// Element 층 편집 텍스트 시험이다(Docs/Design/UiToolkit 21.2절 — UE SEditableText). Widget·객체
// 등록부 없이 Element가 글자·이력·사건을 스스로 드는지 본다: 사용자 편집만 onTextChanged, 프로그램
// 교체는 통지·이력 없음, Enter·초점 잃음 확정, 비밀번호 선택 막기, Initialize 전에 정한 힌트,
// 이력 99단계·힌트 불투명도·caret 폭(22.5절), 상자의 상태별 바탕·전경·여백·오류 표지(22.2·22.6절).

namespace rocks::ui {

namespace {

// 코드 포인트마다 8 px 폭 클러스터 하나다.
class FixedAdvanceShaper final : public TextShaper {
public:
    explicit FixedAdvanceShaper(Allocator& allocator) noexcept : allocator_(allocator) {
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

constexpr float EditFontSize = 12.0f;
constexpr uint32 EditUser = 0;

struct EditBench {
    explicit EditBench(Allocator& allocator) noexcept : changes(allocator), commits(allocator) {
    }

    SharedPtr<EditableText> editor;
    Array<String> changes;
    Array<TextCommitReason> commits;
};

bool MakeEditor(Allocator& allocator, EditBench& bench) noexcept {
    auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
    if (!shaper) {
        return false;
    }

    auto made =
        SharedPtr<EditableText>::TryMake(allocator, allocator, shaper.Value(), Platform{}, false);
    if (!made) {
        return false;
    }

    bench.editor = made.Value();
    bench.editor->SetOnTextChanged(
        Delegate<void(StringView)>::CreateLambda([&bench, &allocator](StringView text) noexcept {
            String copy(allocator);
            if (copy.TryAssign(text)) {
                (void)bench.changes.TryPushBack(Move(copy));
            }
        }));
    bench.editor->SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
        [&bench](StringView, TextCommitReason reason) noexcept {
            (void)bench.commits.TryPushBack(reason);
        }));
    return true;
}

void Type(EditableText& editor, uint32 codePoint) noexcept {
    (void)editor.SendKey(EditUser, input::InputEvent::Text(codePoint));
}

void Press(EditableText& editor, input::Key key, KeyChord chord = {}) noexcept {
    const auto event = input::InputEvent::KeyDown(key);
    (void)editor.OnPreviewKeyChord(EditUser, event, chord);
    (void)editor.SendKey(EditUser, event, chord);
}

} // namespace

TEST(UiFramework_EditableTextOwnsItsTextAndReportsOnlyUserEdits) {
    SystemAllocator allocator;
    EditBench bench(allocator);
    REQUIRE(MakeEditor(allocator, bench));
    auto& editor = *bench.editor;
    REQUIRE(editor.Initialize("ab", EditFontSize));
    CHECK(editor.GetText() == "ab");
    // Initialize는 한 번뿐이다.
    CHECK(editor.Initialize("x", EditFontSize).Error() == ErrorCode::AlreadyExists);
    // 프로그램 교체는 통지하지 않는다. 한 줄에 줄바꿈은 거부하고 바꾸지 않는다.
    REQUIRE(editor.SetText("abc"));
    CHECK(editor.SetText("a\nb").Error() == ErrorCode::InvalidArgument);
    CHECK(editor.GetText() == "abc");
    CHECK(bench.changes.IsEmpty());
    // 사용자 타자는 편집마다 한 번 통지한다(caret은 SetText 뒤 끝).
    editor.OnFocusReceived(EditUser);
    Type(editor, 'd');
    REQUIRE(bench.changes.Size() == 1);
    CHECK(bench.changes[0].View() == "abcd");
    CHECK(editor.GetText() == "abcd");
    // 되돌리기도 사용자 편집이다.
    Press(editor, input::Key::Z, {.control = true});
    CHECK(editor.GetText() == "abc");
    REQUIRE(bench.changes.Size() == 2);
    // SetText는 이력을 지운다 — 되돌릴 것이 없다.
    REQUIRE(editor.SetText("zz"));
    Press(editor, input::Key::Z, {.control = true});
    CHECK(editor.GetText() == "zz");
    CHECK(bench.changes.Size() == 2);
    // 읽기 전용은 편집을 막는다.
    editor.SetReadOnly(true);
    Type(editor, 'q');
    CHECK(editor.GetText() == "zz");
    CHECK(!editor.AllowsInputMethod());
}

TEST(UiFramework_EditableTextCommitsOnEnterAndFocusLoss) {
    SystemAllocator allocator;
    EditBench bench(allocator);
    REQUIRE(MakeEditor(allocator, bench));
    auto& editor = *bench.editor;
    REQUIRE(editor.Initialize("", EditFontSize));
    editor.OnFocusReceived(EditUser);
    Type(editor, 'x');
    Press(editor, input::Key::Enter);
    REQUIRE(bench.commits.Size() == 1);
    CHECK(bench.commits[0] == TextCommitReason::Enter);
    editor.OnFocusLost(EditUser);
    REQUIRE(bench.commits.Size() == 2);
    CHECK(bench.commits[1] == TextCommitReason::FocusLost);
    // 다른 사용자의 초점 잃음은 무시한다.
    editor.OnFocusReceived(EditUser);
    editor.OnFocusLost(EditUser + 1);
    CHECK(bench.commits.Size() == 2);
}

TEST(UiFramework_EditableTextPasswordAndHintBeforeInitialize) {
    SystemAllocator allocator;
    EditBench bench(allocator);
    REQUIRE(MakeEditor(allocator, bench));
    auto& editor = *bench.editor;
    auto hint = Text::Invariant(allocator, "Name");
    REQUIRE(hint);
    // Initialize 전에 정한 값도 쓰인다.
    REQUIRE(editor.SetHintText(hint.Value()));
    REQUIRE(editor.SetPassword(true));
    REQUIRE(editor.Initialize("secret", EditFontSize));
    // 레이블 + 힌트다. 힌트는 전경 × 0.35다(UE HintTextOpacity — 22.5절).
    REQUIRE(editor.ChildCount() == 2);
    CHECK(editor.ChildAt(1)->GetRenderOpacity() == 0.35f);
    editor.SelectAllText();
    CHECK(editor.SelectedText().IsEmpty());
    CHECK(!editor.AllowsInputMethod());
    REQUIRE(editor.SetPassword(false));
    editor.SelectAllText();
    CHECK(editor.SelectedText() == "secret");
    // 빈 불변 글자는 지우기와 같다.
    auto empty = Text::Invariant(allocator, "");
    REQUIRE(empty);
    REQUIRE(editor.SetHintText(empty.Value()));
    CHECK(editor.ChildCount() == 1);
    // 스타일 불투명도는 0–1이다.
    EditableTextStyle style = DefaultEditableTextStyle();
    style.hintOpacity = 1.5f;
    CHECK(editor.SetStyle(style).Error() == ErrorCode::InvalidArgument);
}

TEST(UiFramework_EditableTextKeepsNinetyNineUndoSteps) {
    SystemAllocator allocator;
    EditBench bench(allocator);
    REQUIRE(MakeEditor(allocator, bench));
    auto& editor = *bench.editor;
    REQUIRE(editor.Initialize("", EditFontSize));
    editor.OnFocusReceived(EditUser);
    // 편집마다 이력 한 단계다. 100번 넣으면 가장 오래된 단계가 빠진다(UE MaxUndoLevels 99).
    constexpr uint32 Edits = 100;
    for (uint32 i = 0; i < Edits; ++i) {
        Type(editor, 'a');
    }

    for (uint32 i = 0; i < Edits + 10; ++i) {
        Press(editor, input::Key::Z, {.control = true});
    }

    CHECK(editor.GetText() == "a");
}

TEST(UiFramework_EditableTextBoxPicksStateBackgroundAndForeground) {
    SystemAllocator allocator;
    auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
    REQUIRE(shaper);
    auto made = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper.Value(),
                                                    Platform{}, false);
    REQUIRE(made);
    auto& box = *made.Value();
    CHECK(!box.IsInitialized());
    REQUIRE(box.Initialize("abc", EditFontSize));
    CHECK(box.Initialize("x", EditFontSize).Error() == ErrorCode::AlreadyExists);
    auto& editor = box.Editor();
    // 상자 여백은 편집기의 내용 여백이다(편집기가 상자 안쪽 전체를 덮는다 — 22.2절).
    const EditableTextBoxStyle& style = DefaultEditableTextBoxStyle();
    CHECK(editor.ContentPadding() == style.padding);
    CHECK(editor.GetStyle() == style.text);
    REQUIRE(box.ChildCount() == 1 && box.ChildAt(0) == &editor);
    const Vec2 desired = box.GetDesiredSize();
    CHECK(desired == editor.GetDesiredSize());
    REQUIRE(box.Arrange(Geometry::Root({120, 30}, {})));
    CHECK(editor.GetGeometry().size == Vec2(120, 30));
    // 바탕: 보통 → 올림 → 초점 → 읽기 전용(UE GetBorderImage 순서).
    const ThemeColors& theme = DefaultThemeColors();
    const auto background = [&]() noexcept {
        PaintList list(allocator);
        (void)box.OnPaint(list, 0);
        return list.Boxes().IsEmpty() ? LinearColor{} : list.Boxes()[0].color;
    };
    const auto fill = [&](const StyleBrush& brush) noexcept {
        return brush.Resolve(theme, {}).tint;
    };
    CHECK(background() == fill(style.normal));
    box.OnMouseEnter(EditUser);
    CHECK(background() == fill(style.hovered));
    editor.OnFocusReceived(EditUser);
    CHECK(background() == fill(style.focused));
    // 초점이면 전경은 흰색이다(UE focused fg White), 아니면 물려받는다.
    const LinearColor inherited{0.5f, 0.5f, 0.5f, 1};
    CHECK(box.CurrentForeground().Resolve(theme, {inherited, inherited}) ==
          style.focusedForeground.Resolve(theme, {inherited, inherited}));
    editor.SetReadOnly(true);
    CHECK(background() == fill(style.readOnly));
    CHECK(box.CurrentForeground().Resolve(theme, {inherited, inherited}) == inherited);
    editor.SetReadOnly(false);
    editor.OnFocusLost(EditUser);
    box.OnMouseLeave(EditUser);
    CHECK(background() == fill(style.normal));
    CHECK(box.CurrentForeground().Resolve(theme, {inherited, inherited}) == inherited);
    // 상자에 준 초점은 안쪽 편집기가 받는다(FocusDelegate — 상자는 Tab 정지가 아니다).
    Application app(allocator);
    app.SetRoot(made.Value());
    CHECK(!box.SupportsKeyboardFocus());
    REQUIRE(app.SetFocus(EditUser, made.Value()));
    CHECK(app.GetFocus(EditUser).Get() == &editor && editor.IsFocused());
    app.Shutdown();
}

TEST(UiFramework_EditableTextBoxShowsErrorMarker) {
    SystemAllocator allocator;
    auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
    REQUIRE(shaper);
    auto made = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper.Value(),
                                                    Platform{}, false);
    REQUIRE(made);
    auto& box = *made.Value();
    auto error = Text::Invariant(allocator, "Required");
    REQUIRE(error);
    // Initialize 전에 정한 오류도 쓰인다.
    REQUIRE(box.SetError(error.Value()));
    CHECK(box.HasError());
    REQUIRE(box.Initialize("abc", EditFontSize));
    REQUIRE(box.ChildCount() == 2);
    const Element& marker = *box.ChildAt(1);
    CHECK(StaticElementCast<TextLabel>(&marker)->GetText() == "!");
    // 표지는 오른쪽 여백 안의 정사각이고 편집 칸이 그만큼 줄어든다(22.6절).
    REQUIRE(box.Prepass(1));
    REQUIRE(box.Arrange(Geometry::Root({120, 30}, {})));
    const float extent = marker.GetGeometry().size.x;
    CHECK(extent > 0 && marker.GetGeometry().size.y == extent);
    const float padding = DefaultEditableTextBoxStyle().padding.z;
    CHECK(box.Editor().GetGeometry().size.x == 120 - extent - padding);
    CHECK(marker.GetGeometry().layout.tx == 120 - padding - extent);
    // 빈 불변 글자는 지우기와 같다.
    auto empty = Text::Invariant(allocator, "");
    REQUIRE(empty && box.SetError(empty.Value()));
    CHECK(!box.HasError() && box.ChildCount() == 1);
    REQUIRE(box.Prepass(1));
    REQUIRE(box.Arrange(Geometry::Root({120, 30}, {})));
    CHECK(box.Editor().GetGeometry().size.x == 120);
    // shaper가 없으면 표지를 만들지 못한다.
    auto bare = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, SharedPtr<TextShaper>{},
                                                    Platform{}, false);
    REQUIRE(bare && bare.Value()->Initialize("", EditFontSize));
    CHECK(bare.Value()->SetError(error.Value()).Error() == ErrorCode::NotReady);
}

} // namespace rocks::ui
