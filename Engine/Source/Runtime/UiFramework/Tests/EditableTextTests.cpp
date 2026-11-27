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

namespace {

// 편집기를 앱에 붙여 배치한다(포인터 위치를 글자 위치로 바꾸려면 레이블 배치가 있어야 한다).
struct PointerBench {
    explicit PointerBench(Allocator& allocator) noexcept : bench(allocator), app(allocator) {
    }

    bool Start(Allocator& allocator, StringView text, bool multiline, bool password) noexcept {
        auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
        if (!shaper) {
            return false;
        }

        auto made = SharedPtr<EditableText>::TryMake(allocator, allocator, shaper.Value(),
                                                     Platform{}, multiline);
        if (!made || !made.Value()->SetPassword(password) ||
            !made.Value()->Initialize(text, EditFontSize)) {
            return false;
        }

        bench.editor = made.Value();
        app.SetRoot(bench.editor);
        return app.PrepareLayout(Geometry::Root({400, 120}, {}), 1).IsOk() &&
               app.SetFocus(EditUser, bench.editor).IsOk();
    }

    // index번째 code point(고정 폭 8 px) 안 inside px를 줄 line에서 누른다(기본은 가운데 — 가운데는
    // 다음 위치로 잡힌다, 왼쪽 가장자리는 그 글자 앞 위치다).
    void Click(usize index, uint8 clicks, float line = 0, float inside = 4) noexcept {
        PointerEvent down{
            .type = PointerEventType::Down,
            .position = {static_cast<float>(index) * 8 + inside, line * EditFontSize + 2},
            .clickCount = clicks};
        (void)bench.editor->OnPointerEvent(down);
    }

    void Drag(usize index, PointerEventType type = PointerEventType::Move) noexcept {
        PointerEvent move{.type = type, .position = {static_cast<float>(index) * 8 + 4, 2}};
        (void)bench.editor->OnPointerEvent(move);
    }

    EditBench bench;
    Application app;
};

} // namespace

TEST(UiFramework_TextShaperDefaultWordBoundariesSplitByClass) {
    SystemAllocator allocator;
    FixedAdvanceShaper shaper(allocator);
    Array<usize> breaks(allocator);
    // 부류(글자·공백·문장부호)가 바뀌는 곳과 문장부호 하나하나에서 나눈다. 0은 빼고 끝은 넣는다.
    REQUIRE(shaper.FindWordBoundaries("ab, c..", breaks));
    const usize expected[] = {2, 3, 4, 5, 6, 7};
    REQUIRE(breaks.Size() == 6);
    for (usize i = 0; i < 6; ++i) {
        CHECK(breaks[i] == expected[i]);
    }

    // 한글은 한 부류다("한글"은 6 byte).
    REQUIRE(shaper.FindWordBoundaries("한글 a", breaks));
    REQUIRE(breaks.Size() == 3);
    CHECK(breaks[0] == 6 && breaks[1] == 7 && breaks[2] == 8);
    REQUIRE(shaper.FindWordBoundaries("", breaks));
    CHECK(breaks.IsEmpty());
    const char bad[] = {static_cast<char>(0xC3)};
    CHECK(!shaper.FindWordBoundaries(StringView(bad, 1), breaks));
}

TEST(UiFramework_EditableTextDoubleClickSelectsWordAndDragExtendsByWord) {
    SystemAllocator allocator;
    PointerBench pointer(allocator);
    REQUIRE(pointer.Start(allocator, "hello world foo", false, false));
    auto& editor = *pointer.bench.editor;
    // 두 번 누르면 그 단어다(UE SelectWordAt — caret은 끝).
    pointer.Click(7, 2);
    CHECK(editor.SelectedText() == "world");
    // 끌면 단어 단위로 넓힌다: 뒤로는 처음 단어의 시작부터, 앞으로는 처음 단어의 끝부터.
    pointer.Drag(13);
    CHECK(editor.SelectedText() == "world foo");
    pointer.Drag(1);
    CHECK(editor.SelectedText() == "hello world");
    pointer.Drag(1, PointerEventType::Up);
    // 단어 바로 뒤(공백의 왼쪽)를 두 번 누르면 앞 단어다(UE GetWordAt — 공백만인 구간은 건너뛴다).
    pointer.Click(5, 2, 0, 1);
    CHECK(editor.SelectedText() == "hello");
    pointer.Drag(5, PointerEventType::Up);
    // 한 번 누름은 클러스터 단위로 돌아간다.
    pointer.Click(3, 1);
    CHECK(editor.SelectedText().IsEmpty());
    pointer.Drag(3, PointerEventType::Up);
}

TEST(UiFramework_EditableTextTripleClickSelectsLineAndPasswordSelectsAll) {
    SystemAllocator allocator;
    // 여러 줄: 세 번 누르면 그 논리 줄(줄바꿈 제외)이다.
    PointerBench lines(allocator);
    REQUIRE(lines.Start(allocator, "ab\ncd ef\ngh", true, false));
    lines.Click(1, 3, 1);
    CHECK(lines.bench.editor->SelectedText() == "cd ef");
    lines.Drag(1, PointerEventType::Up);
    // 한 줄: 세 번 누르면 전체다.
    PointerBench single(allocator);
    REQUIRE(single.Start(allocator, "one two", false, false));
    single.Click(1, 3);
    CHECK(single.bench.editor->SelectedText() == "one two");
    single.Drag(1, PointerEventType::Up);
    // 비밀번호: 두 번 눌러도 전체를 고른다(단어 길이를 드러내지 않는다 — 22.4절). 선택 글자는 비어
    // 보이므로(복사 막기) 입력 문맥의 선택 범위(UTF-16)로 본다.
    PointerBench secret(allocator);
    REQUIRE(secret.Start(allocator, "pass word", false, true));
    secret.Click(1, 2);
    secret.Drag(1, PointerEventType::Up);
    CHECK(secret.bench.editor->SelectedText().IsEmpty());
    uint32 begin = 0;
    uint32 length = 0;
    secret.bench.editor->GetSelection(begin, length);
    CHECK(begin == 0 && length == 9);
}

TEST(UiFramework_EditableTextCaretBlinksLikeUe) {
    SystemAllocator allocator;
    PointerBench pointer(allocator);
    REQUIRE(pointer.Start(allocator, "abc", false, false));
    auto& editor = *pointer.bench.editor;
    auto& app = pointer.app;
    // 초점 동안 volatile이다(매 프레임 다시 기록 — 22.5절).
    CHECK(editor.IsVolatile());
    const auto caretVisible = [&]() noexcept {
        PaintList list(allocator);
        (void)editor.OnPaintOverlay(list, 0);
        return list.Boxes().Size() == 1;
    };
    // 상호작용 시각 10초 — 0.1초 동안은 늘 보인다.
    app.BeginFrame(FrameTime{10.0, 0});
    editor.MoveCaret(1, false);
    CHECK(caretVisible());
    app.BeginFrame(FrameTime{10.05, 0.05});
    CHECK(caretVisible());
    // 0.3초: 0.5 + 0.5·sin(π/2 + 0.6π) ≈ 0.35 → 꺼짐. 0.8초: ≈ 0.65 → 켜짐(UE 사각파).
    app.BeginFrame(FrameTime{10.3, 0.25});
    CHECK(!caretVisible());
    app.BeginFrame(FrameTime{10.8, 0.5});
    CHECK(caretVisible());
    // 타자·이동은 깜박임을 처음부터 센다.
    app.BeginFrame(FrameTime{11.3, 0.5});
    CHECK(!caretVisible());
    editor.MoveCaret(2, false);
    CHECK(caretVisible());
    // 초점을 잃으면 volatile을 끄고 caret도 없다.
    REQUIRE(app.SetFocus(EditUser, {}));
    CHECK(!editor.IsVolatile() && !caretVisible());
}

TEST(UiFramework_EditableTextControlArrowsMoveAndDeleteByWord) {
    SystemAllocator allocator;
    EditBench bench(allocator);
    REQUIRE(MakeEditor(allocator, bench));
    auto& editor = *bench.editor;
    REQUIRE(editor.Initialize("one two, three", EditFontSize));
    editor.OnFocusReceived(EditUser);
    editor.MoveCaret(0, false);
    const KeyChord control{.control = true};
    const KeyChord select{.shift = true, .control = true};
    // Ctrl+→는 다음 단어 시작(문장부호도 단어다)이나 줄 끝에서 멈춘다(UE ScanForWordBoundary).
    Press(editor, input::Key::Right, control);
    Press(editor, input::Key::Right, select);
    CHECK(editor.SelectedText() == "two");
    Press(editor, input::Key::Right, select);
    CHECK(editor.SelectedText() == "two, ");
    Press(editor, input::Key::Right, select);
    CHECK(editor.SelectedText() == "two, three");
    // Ctrl+←는 앞 단어 시작으로 간다.
    Press(editor, input::Key::Left, control);
    Press(editor, input::Key::Right, select);
    CHECK(editor.SelectedText() == "three");
    // Ctrl+Backspace는 앞 단어를, Ctrl+Delete는 뒤 단어를 지운다(사용자 편집 — 통지).
    Press(editor, input::Key::End);
    Press(editor, input::Key::Backspace, control);
    CHECK(editor.GetText() == "one two, ");
    editor.MoveCaret(0, false);
    Press(editor, input::Key::Delete, control);
    CHECK(editor.GetText() == "two, ");
    CHECK(bench.changes.Size() == 2);
    // 비밀번호는 줄 처음·끝으로 간다(단어 경계를 드러내지 않는다).
    REQUIRE(editor.SetPassword(true));
    editor.MoveCaret(0, false);
    Press(editor, input::Key::Right, select);
    uint32 begin = 0;
    uint32 length = 0;
    editor.GetSelection(begin, length);
    CHECK(begin == 0 && length == 5);
}

} // namespace rocks::ui
