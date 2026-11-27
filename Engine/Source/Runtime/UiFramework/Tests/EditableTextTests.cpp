#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiCore/PaintList.h>
#include <UiCore/InputMetrics.h>
#include <UiCore/UiHostServices.h>
#include <UiFramework/Application/Application.h>
#include <UiFramework/Controls/EditableTextBox.h>
#include <UiFramework/Layout/AnchorCanvas.h>
#include <UiFramework/Layout/Overlay.h>
#include <UiFramework/Popups/MenuAnchor.h>
#include <UiFramework/Scrolling/ScrollBar.h>

// Element 층 편집 텍스트 시험이다(Docs/Design/UiToolkit 21.2절 — UE SEditableText). Widget·객체
// 등록부 없이 Element가 글자·이력·사건을 스스로 드는지 본다: 사용자 편집만 onTextChanged, 프로그램
// 교체는 통지·이력 없음, Enter·초점 잃음 확정, 비밀번호 선택 막기, Initialize 전에 정한 힌트,
// 이력 99단계·힌트 불투명도·caret 폭(22.5절), 상자의 상태별 바탕·전경·여백·오류 표지와
// 팝업(22.2·22.6절), 스크롤 유지·휠·여러 줄 상자의 스크롤 막대(22.7절).

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
    // 표지는 "!"를 담은 MenuAnchor다(UE SPopupErrorText — 초점을 받지 않는 콤보 버튼).
    REQUIRE(box.ErrorMarker() != nullptr && box.ChildAt(1) == box.ErrorMarker());
    const MenuAnchor& marker = *box.ErrorMarker();
    CHECK(StaticElementCast<TextLabel>(marker.ChildAt(0))->GetText() == "!");
    CHECK(!marker.SupportsKeyboardFocus());
    CHECK(marker.GetStyle().normal == DefaultEditableTextBoxStyle().errorMarker);
    CHECK(marker.GetStyle().hovered == DefaultEditableTextBoxStyle().errorMarker);
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
    CHECK(!box.HasError() && box.ChildCount() == 1 && box.ErrorMarker() == nullptr);
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

namespace {

// 루트 Overlay 안 캔버스에 상자 하나를 놓는다(오류 팝업이 루트 Overlay 팝업 층에 붙는다).
struct BoxBench {
    explicit BoxBench(Allocator& allocator) noexcept : app(allocator) {
    }

    bool Make(Allocator& allocator, bool multiline) noexcept {
        auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
        auto overlay = SharedPtr<Overlay>::TryMake(allocator, allocator);
        auto made = SharedPtr<AnchorCanvas>::TryMake(allocator, allocator);
        if (!shaper || !overlay || !made) {
            return false;
        }

        auto made2 = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper.Value(),
                                                         Platform{}, multiline);
        if (!made2) {
            return false;
        }

        root = overlay.Value();
        canvas = made.Value();
        box = made2.Value();
        return root->TryAddChild(canvas).IsOk();
    }

    bool Place() noexcept {
        return canvas->TryAddSlot(box, {.offset = {10, 10, 120, 30}, .alignment = {}}).IsOk();
    }

    bool Layout() noexcept {
        return app.UpdatePaintCache(Geometry::Root({200, 120}, {}), 1).IsOk();
    }

    void Click(const Element& target) noexcept {
        const Geometry& geometry = target.GetGeometry();
        const Vec2 center = geometry.render.Apply(geometry.size * 0.5f);
        (void)app.RoutePointer({PointerEventType::Down, center, 0});
        (void)app.RoutePointer({PointerEventType::Up, center, 0});
    }

    SharedPtr<Overlay> root;
    SharedPtr<AnchorCanvas> canvas;
    SharedPtr<EditableTextBox> box;
    Application app;
};

// 팝업 내용(오류 팝업 → 글자 레이블)의 글자다.
StringView PopupText(const MenuAnchor& marker) noexcept {
    const Element* popup = marker.MenuContent();
    const Element* label = popup && popup->ChildCount() > 0 ? popup->ChildAt(0) : nullptr;
    return label ? StaticElementCast<TextLabel>(label)->GetText() : StringView{};
}

// "0\n1\n…" 꼴의 count줄 글자다.
String Lines(Allocator& allocator, usize count) noexcept {
    String text(allocator);
    for (usize i = 0; i < count; ++i) {
        const char line[] = {static_cast<char>('0' + i % 10), '\n', '\0'};
        (void)text.TryAppend(i + 1 < count ? StringView(line) : StringView(line, 1));
    }

    return text;
}

} // namespace

TEST(UiFramework_HostFrameActionsRunOncePerFrameInOrder) {
    SystemAllocator allocator;
    UiHostServices host(allocator);
    CHECK(host.DeferFrameAction({}).Error() == ErrorCode::InvalidArgument);
    Array<int32> order(allocator);
    usize reentered = 1;
    REQUIRE(host.DeferFrameAction(Delegate<void()>::CreateLambda([&]() noexcept {
        (void)order.TryPushBack(1);
        // 도는 중 다시 부르면 0이다. 도는 중 넣은 항목은 다음 차례다(22.6절).
        reentered = host.RunFrameActions();
        (void)host.DeferFrameAction(
            Delegate<void()>::CreateLambda([&]() noexcept { (void)order.TryPushBack(3); }));
    })));
    REQUIRE(host.DeferFrameAction(
        Delegate<void()>::CreateLambda([&]() noexcept { (void)order.TryPushBack(2); })));
    CHECK(host.PendingFrameActionCount() == 2);
    CHECK(host.RunFrameActions() == 2);
    CHECK(reentered == 0);
    REQUIRE(order.Size() == 2);
    CHECK(order[0] == 1 && order[1] == 2);
    CHECK(host.PendingFrameActionCount() == 1);
    CHECK(host.DiscardFrameActions() == 1);
    CHECK(host.RunFrameActions() == 0 && order.Size() == 2);
}

TEST(UiFramework_EditableTextBoxErrorPopupFollowsSetErrorWithoutStealingFocus) {
    SystemAllocator allocator;
    BoxBench bench(allocator);
    REQUIRE(bench.Make(allocator, false) && bench.Place());
    REQUIRE(bench.box->Initialize("abc", EditFontSize));
    bench.app.SetRoot(bench.root);
    REQUIRE(bench.Layout());
    REQUIRE(bench.app.SetFocus(EditUser, bench.box));
    EditableText& editor = bench.box->Editor();
    auto required = Text::Invariant(allocator, "Required");
    auto tooLong = Text::Invariant(allocator, "Too long");
    REQUIRE(required && tooLong);
    // SetError가 팝업을 바로 연다(UE SetIsOpen(HasError, false)) — 초점은 편집기에 남는다.
    REQUIRE(bench.box->SetError(required.Value()));
    MenuAnchor* marker = bench.box->ErrorMarker();
    REQUIRE(marker != nullptr && marker->IsOpen());
    CHECK(bench.app.GetFocus(EditUser).Get() == &editor);
    CHECK(bench.root->ChildCount() == 2);
    CHECK(PopupText(*marker) == "Required");
    REQUIRE(bench.Layout());
    // 표지 왼쪽에 맞춰 상자 아래 끝에 붙는다(UE BelowAnchor는 표지 바로 아래 — 22.6절).
    REQUIRE(marker->MenuPosition());
    const Geometry& markerArea = marker->GetGeometry();
    CHECK(marker->MenuPosition().Value() == Vec2(markerArea.render.Apply({}).x, 40));
    // 팝업 바탕은 오류 표지 brush, 여백은 (3, 0, 3, 0)이다(UE SErrorText).
    const Element* popup = marker->MenuContent();
    REQUIRE(popup != nullptr && popup->ChildCount() == 1);
    CHECK(popup->ChildAt(0)->GetGeometry().render.Apply({}) -
              popup->GetGeometry().render.Apply({}) ==
          Vec2(3, 0));
    // 열린 채 다시 부르면 새 문구다.
    REQUIRE(bench.box->SetError(tooLong.Value()));
    CHECK(marker->IsOpen() && PopupText(*marker) == "Too long");
    CHECK(bench.app.GetFocus(EditUser).Get() == &editor);
    // 팝업 층은 바깥 누름을 받지 않는다 — 편집기를 누르면 편집기가 받고 팝업은 열린 채다.
    REQUIRE(bench.Layout());
    bench.Click(editor);
    CHECK(marker->IsOpen() && bench.app.GetFocus(EditUser).Get() == &editor);
    // anchor가 숨으면(접힌 조상 — 고르지 않은 탭과 같다) 팝업 층을 접고, 다시 보이면 되돌린다.
    Element* layer = bench.root->ChildAt(1);
    REQUIRE(layer != nullptr);
    const Visibility layerShown = layer->GetVisibility();
    bench.canvas->SetVisibility(Visibility::Collapsed);
    REQUIRE(bench.Layout());
    CHECK(marker->IsOpen() && layer->GetVisibility() == Visibility::Collapsed);
    bench.canvas->SetVisibility(Visibility::Visible);
    REQUIRE(bench.Layout());
    CHECK(marker->IsOpen() && layer->GetVisibility() == layerShown);
    // 비활성 상자의 오류 팝업은 그대로 보인다.
    bench.box->SetEnabled(false);
    REQUIRE(bench.Layout());
    CHECK(layer->GetVisibility() == layerShown);
    bench.box->SetEnabled(true);
    // 표지를 누르면 닫히고 다시 누르면 열린다(초점을 옮기지 않는 Open).
    REQUIRE(bench.Layout());
    bench.Click(*marker);
    CHECK(!marker->IsOpen());
    REQUIRE(bench.Layout());
    bench.Click(*marker);
    CHECK(marker->IsOpen() && PopupText(*marker) == "Too long");
    // 지우면 닫히고 표지도 없어진다.
    REQUIRE(bench.box->SetError({}));
    CHECK(bench.box->ErrorMarker() == nullptr && bench.root->ChildCount() == 1);
    bench.app.Shutdown();
}

TEST(UiFramework_EditableTextBoxOpensErrorPopupAtFrameStartAfterAttach) {
    SystemAllocator allocator;
    BoxBench bench(allocator);
    REQUIRE(bench.Make(allocator, false));
    auto required = Text::Invariant(allocator, "Required");
    REQUIRE(required);
    // 붙기 전 오류: 표지는 있지만 팝업 호스트가 없어 열리지 않는다.
    REQUIRE(bench.box->SetError(required.Value()));
    REQUIRE(bench.box->Initialize("abc", EditFontSize));
    MenuAnchor* marker = bench.box->ErrorMarker();
    REQUIRE(marker != nullptr && !marker->IsOpen());
    REQUIRE(bench.Place());
    // 붙는 통지 안에서는 열지 않고(트리 순회 중) 프레임 시작에 연다(UE는 버린다 — 22.6절).
    bench.app.SetRoot(bench.root);
    CHECK(!marker->IsOpen() && bench.root->ChildCount() == 1);
    const UiHostServices* host = bench.box->FindUiHostServices();
    REQUIRE(host != nullptr);
    CHECK(host->PendingFrameActionCount() == 1);
    bench.app.BeginFrame(FrameTime{1, 0});
    CHECK(marker->IsOpen() && PopupText(*marker) == "Required");
    CHECK(host->PendingFrameActionCount() == 0);
    // 떼면 닫히고 다시 붙이면 다음 프레임에 다시 연다.
    REQUIRE(bench.canvas->RemoveChild(*bench.box));
    CHECK(!marker->IsOpen());
    REQUIRE(bench.Place());
    CHECK(!marker->IsOpen() && host->PendingFrameActionCount() == 1);
    bench.app.BeginFrame(FrameTime{2, 0});
    CHECK(marker->IsOpen());
    // 대기 중 버려진 상자는 아무 일도 하지 않는다.
    REQUIRE(bench.canvas->RemoveChild(*bench.box));
    REQUIRE(bench.Place());
    CHECK(host->PendingFrameActionCount() == 1);
    REQUIRE(bench.canvas->RemoveChild(*bench.box));
    bench.box = {};
    bench.app.BeginFrame(FrameTime{3, 0});
    CHECK(host->PendingFrameActionCount() == 0 && bench.root->ChildCount() == 1);
    // Shutdown은 남은 프레임 동작을 돌리지 않고 버린다.
    auto again = SharedPtr<EditableTextBox>::TryMake(
        allocator, allocator, SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator).Value(),
        Platform{}, false);
    REQUIRE(again && again.Value()->Initialize("x", EditFontSize));
    REQUIRE(again.Value()->SetError(required.Value()));
    bench.box = again.Value();
    REQUIRE(bench.Place());
    CHECK(host->PendingFrameActionCount() == 1);
    bench.app.Shutdown();
    CHECK(host->PendingFrameActionCount() == 0 && !bench.box->ErrorMarker()->IsOpen());
}

TEST(UiFramework_EditableTextKeepsScrollUntilTheCaretMoves) {
    SystemAllocator allocator;
    PointerBench pointer(allocator);
    const String text = Lines(allocator, 20);
    REQUIRE(pointer.Start(allocator, text.View(), true, false));
    EditableText& editor = *pointer.bench.editor;
    Application& app = pointer.app;
    const Geometry area = Geometry::Root({100, 40}, {});
    REQUIRE(app.PrepareLayout(area, 1));
    const float line = editor.Label().LineHeight();
    REQUIRE(line > 0);
    CHECK(editor.ContentHeight() >= 19 * line);
    CHECK(editor.ViewHeight() == 40);
    const float maxScroll = editor.ContentHeight() - 40;
    REQUIRE(maxScroll > WheelScrollStep);
    // 막대·코드로 옮긴 스크롤은 다음 배치에도 남는다(caret을 끌어오지 않는다 — 22.7절).
    editor.MoveCaret(0, false);
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(editor.ScrollOffset() == 0);
    editor.SetScrollOffset(50);
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(editor.ScrollOffset() == 50);
    CHECK(editor.Label().GetGeometry().layout.ty == -50);
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(editor.ScrollOffset() == 50);
    // clamp와 비유한 값.
    editor.SetScrollOffset(10000);
    CHECK(editor.ScrollOffset() == maxScroll);
    editor.SetScrollOffset(-5);
    CHECK(editor.ScrollOffset() == 0);
    editor.SetScrollOffset(platform::PlatformMath::QuietNaN<float>());
    CHECK(editor.ScrollOffset() == 0);
    // caret이 움직이면 caret 줄을 보이게 한다.
    editor.SetScrollOffset(maxScroll);
    REQUIRE(app.PrepareLayout(area, 1));
    Press(editor, input::Key::Right);
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(editor.ScrollOffset() == 0);
    // 휠: 한 칸에 WheelScrollStep, 끝에서는 처리하지 않는다(바깥 스크롤로).
    PointerEvent wheel{.type = PointerEventType::Wheel, .position = {10, 10}, .wheel = -1};
    CHECK(editor.OnPointerEvent(wheel).handled);
    CHECK(editor.ScrollOffset() == math::Min(maxScroll, WheelScrollStep));
    for (int32 i = 0; i < 20; ++i) {
        (void)editor.OnPointerEvent(wheel);
    }

    CHECK(editor.ScrollOffset() == maxScroll);
    CHECK(!editor.OnPointerEvent(wheel).handled);
    wheel.wheel = 1;
    CHECK(editor.OnPointerEvent(wheel).handled);
    // 한 줄 편집기는 휠을 받지 않는다.
    EditBench single(allocator);
    REQUIRE(MakeEditor(allocator, single) && single.editor->Initialize("abc", EditFontSize));
    CHECK(!single.editor->OnPointerEvent(wheel).handled);
}

TEST(UiFramework_EditableTextBoxShowsScrollBarOnlyWhenOverflowing) {
    SystemAllocator allocator;
    auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
    REQUIRE(shaper);
    auto made =
        SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper.Value(), Platform{}, true);
    REQUIRE(made);
    EditableTextBox& box = *made.Value();
    REQUIRE(box.Initialize("a", EditFontSize));
    Application app(allocator);
    app.SetRoot(made.Value());
    const Geometry area = Geometry::Root({200, 100}, {});
    REQUIRE(app.PrepareLayout(area, 1));
    EditableText& editor = box.Editor();
    ScrollBar* bar = box.VerticalScrollBar();
    REQUIRE(bar != nullptr);
    // 넘치지 않으면 숨고 편집 칸은 상자 폭 그대로다.
    CHECK(bar->GetVisibility() == Visibility::Collapsed);
    CHECK(editor.GetGeometry().size.x == 200);
    // 넘치면 보이고 편집 칸 | 막대 순서로 놓인다(막대 세로는 위·아래 여백 안).
    const String text = Lines(allocator, 20);
    REQUIRE(editor.SetText(text.View()));
    REQUIRE(app.PrepareLayout(area, 1));
    const Vec4 padding = box.GetStyle().padding;
    const float thickness = bar->GetStyle().thickness;
    CHECK(bar->GetVisibility() == Visibility::Visible);
    CHECK(editor.GetGeometry().size.x == 200 - thickness - padding.z);
    CHECK(bar->GetGeometry().layout.tx == 200 - thickness - padding.z);
    CHECK(bar->GetGeometry().layout.ty == padding.y);
    CHECK(bar->GetGeometry().size == Vec2(thickness, 100 - padding.y - padding.w));
    // 막대는 편집기 스크롤을 보인다(SetText 뒤 caret이 끝이라 맨 아래).
    const float content = editor.ContentHeight();
    const float view = editor.ViewHeight();
    REQUIRE(content > view);
    CHECK(bar->ThumbSize() == view / content);
    CHECK(editor.ScrollOffset() == content - view);
    CHECK(bar->Offset() == (content - view) / content);
    // 막대를 맨 위로 끌면 편집기가 맨 위로 스크롤하고, 다음 배치에도 남는다.
    const Geometry& track = bar->GetGeometry();
    // 손잡이는 맨 아래에 있다 — 트랙 끝을 눌러 손잡이를 잡는다.
    const Vec2 grip = track.render.Apply({thickness * 0.5f, track.size.y - 1});
    PointerEvent down{.type = PointerEventType::Down, .position = grip};
    REQUIRE(bar->OnPointerEvent(down).handled);
    PointerEvent move{.type = PointerEventType::Move,
                      .position = track.render.Apply({thickness * 0.5f, -200})};
    REQUIRE(bar->OnPointerEvent(move).handled);
    PointerEvent up{.type = PointerEventType::Up, .position = move.position};
    (void)bar->OnPointerEvent(up);
    CHECK(editor.ScrollOffset() == 0);
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(editor.ScrollOffset() == 0 && bar->Offset() == 0);
    // 편집기 휠도 막대를 맞춘다.
    PointerEvent wheel{.type = PointerEventType::Wheel, .position = {10, 10}, .wheel = -1};
    REQUIRE(editor.OnPointerEvent(wheel).handled);
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(bar->Offset() == editor.ScrollOffset() / content);
    // 오류 표지와 함께면 편집 칸 | 막대 | 표지 순서다.
    auto required = Text::Invariant(allocator, "Required");
    REQUIRE(required && box.SetError(required.Value()));
    REQUIRE(app.PrepareLayout(area, 1));
    const MenuAnchor* marker = box.ErrorMarker();
    REQUIRE(marker != nullptr);
    const float extent = marker->GetGeometry().size.x;
    CHECK(extent > 0);
    CHECK(editor.GetGeometry().size.x == 200 - extent - thickness - padding.z);
    CHECK(bar->GetGeometry().layout.tx == editor.GetGeometry().size.x);
    CHECK(marker->GetGeometry().layout.tx == 200 - padding.z - extent);
    // 다시 짧아지면 숨고 편집 칸이 넓어진다.
    REQUIRE(box.SetError({}));
    REQUIRE(editor.SetText("a"));
    REQUIRE(app.PrepareLayout(area, 1));
    CHECK(bar->GetVisibility() == Visibility::Collapsed);
    CHECK(editor.GetGeometry().size.x == 200);
    // 한 줄 상자에는 막대가 없다.
    auto single = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper.Value(),
                                                      Platform{}, false);
    REQUIRE(single && single.Value()->Initialize("a", EditFontSize));
    CHECK(single.Value()->VerticalScrollBar() == nullptr);
    app.Shutdown();
}

} // namespace rocks::ui
