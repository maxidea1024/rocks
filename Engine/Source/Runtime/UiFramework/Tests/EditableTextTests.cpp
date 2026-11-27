#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <UiFramework/Controls/EditableText.h>

// Element 층 편집 텍스트 시험이다(Docs/Design/UiToolkit 21.2절 — UE SEditableText). Widget·객체
// 등록부 없이 Element가 글자·이력·사건을 스스로 드는지 본다: 사용자 편집만 onTextChanged, 프로그램
// 교체는 통지·이력 없음, Enter·초점 잃음 확정, 비밀번호 선택 막기, Initialize 전에 정한 힌트.

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

bool MakeEditor(Allocator& allocator, EditBench& bench, bool boxed = false) noexcept {
    auto shaper = SharedPtr<FixedAdvanceShaper>::TryMake(allocator, allocator);
    if (!shaper) {
        return false;
    }

    auto made = SharedPtr<EditableText>::TryMake(allocator, allocator, shaper.Value(), Platform{},
                                                 false, boxed);
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

TEST(UiFramework_EditableTextPasswordAndCaptionsBeforeInitialize) {
    SystemAllocator allocator;
    EditBench bench(allocator);
    REQUIRE(MakeEditor(allocator, bench, true));
    auto& editor = *bench.editor;
    auto hint = Text::Invariant(allocator, "Name");
    auto error = Text::Invariant(allocator, "Required");
    REQUIRE(hint && error);
    // Initialize 전에 정한 값도 쓰인다.
    REQUIRE(editor.SetHintText(hint.Value()));
    REQUIRE(editor.SetError(error.Value()));
    REQUIRE(editor.SetPassword(true));
    REQUIRE(editor.Initialize("secret", EditFontSize));
    CHECK(editor.HasError());
    // 레이블 + 힌트 + 오류 캡션이다.
    CHECK(editor.ChildCount() == 3);
    editor.SelectAllText();
    CHECK(editor.SelectedText().IsEmpty());
    CHECK(!editor.AllowsInputMethod());
    REQUIRE(editor.SetPassword(false));
    editor.SelectAllText();
    CHECK(editor.SelectedText() == "secret");
    // 빈 불변 글자는 지우기와 같다.
    auto empty = Text::Invariant(allocator, "");
    REQUIRE(empty);
    REQUIRE(editor.SetError(empty.Value()));
    CHECK(!editor.HasError());
    CHECK(editor.ChildCount() == 2);
    // 상자 모양이 아니면 오류 캡션은 Unsupported다.
    EditBench plain(allocator);
    REQUIRE(MakeEditor(allocator, plain));
    CHECK(plain.editor->SetError(error.Value()).Error() == ErrorCode::Unsupported);
}

} // namespace rocks::ui
