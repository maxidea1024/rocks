#pragma once

#include <Core/Delegate.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Localization/Text.h>
#include <UiCore/Element.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiCore/TextInputContext.h>
#include <UiCore/TextInputService.h>
#include <UiFramework/Application/Platform.h>
#include <UiFramework/Controls/TextCommitReason.h>
#include <UiFramework/Text/TextShaper.h>

namespace rocks::ui {

// 한 줄·여러 줄 글자 편집 Element다(UE SEditableText·SMultiLineEditableText — Docs/Design/UiToolkit
// 21.2절). 글자·선택·IME 조합·편집 이력을 스스로 든다. 셰이핑 결과의 UTF-8 클러스터 경계에서만
// 커서와 선택 끝을 움직인다.
//   - SetText는 프로그램에 의한 교체다. 이력을 지우고 onTextChanged를 보내지 않는다.
//   - 사용자 입력(타자·붙여 넣기·삭제·되돌리기·IME 확정·Escape 복원)으로 바뀔 때만 onTextChanged다.
//   - Enter(한 줄)나 초점 잃음이면 onTextCommitted가 지금 글자와 사유를 전달한다.
//   - 콜백의 StringView는 호출 동안만 빌린다. 콜백 안에서 SetText를 불러도 받은 글자는 유효하다.
//
// 텍스트 입력 문맥이다(19.2절). 초점을 받으면 창의 서비스에 Activate하고, 잃거나 소멸하면
// Deactivate한다. 조합 중의 글자는 미리보기 층(preview_)에만 있고 글자·편집 이력·변경 알림은
// EndComposition에서 한 번 바뀐다. 조합 중 선택(anchor_·caret_)은 미리보기 안 위치다.
//
// 바탕이 없는 편집기다. 상자 모양(바탕·여백·오류 표지)은 EditableTextBox가 이것을 담아 그린다
// (Docs/Design/UiToolkit 22.2절 — UE SEditableText ↔ SEditableTextBox).
//
// 사용 예:
//   auto name = SharedPtr<EditableText>::TryMake(allocator, allocator, shaper, platform,
//                                                /*multiline*/ false);
//   (void)name.Value()->Initialize("Untitled", 13);
//   name.Value()->SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
//       [](StringView text, TextCommitReason) noexcept { Rename(text); }));
class ROCKS_UIFRAMEWORK_API EditableText final : public Element, public TextInputContext {
    ROCKS_UI_ELEMENT(EditableText, Element)

public:
    // shaper가 없으면 빈 글자만 받는다(글자를 넣으면 NotReady).
    EditableText(Allocator& allocator, SharedPtr<TextShaper> shaper, Platform platform,
                 bool multiline) noexcept;
    ~EditableText() noexcept override;

    // 글자 레이블 자식을 만들고 처음 글자로 셰이핑한다. 한 번만 부른다.
    Result<void> Initialize(StringView text, float fontSize) noexcept;

    // 프로그램에 의한 교체다. 한 줄 편집에 줄바꿈이 있으면 InvalidArgument이고 바꾸지 않는다.
    Result<void> SetText(StringView text) noexcept;
    StringView GetText() const noexcept;
    // 본문이 비었을 때만 보이는 안내문이다. 복사·선택·IME 입력에는 포함되지 않는다.
    Result<void> SetHintText(const Text& hint) noexcept;
    const Text& HintText() const noexcept;
    Result<void> SetFontSize(float size) noexcept;
    float FontSize() const noexcept;
    // 비밀번호는 화면에 가림 문자로 표시하고 복사·잘라내기·편집 이력·IME를 막는다.
    Result<void> SetPassword(bool password) noexcept;
    bool IsPassword() const noexcept;
    // 읽기 전용은 조합을 버리고 IME를 끈다.
    void SetReadOnly(bool readOnly) noexcept;
    void SetSelectAllTextWhenFocused(bool enabled) noexcept;
    void SetSelectAllTextOnCommit(bool enabled) noexcept;
    // 초점 시점의 내용을 Escape로 복원한다. 비밀번호의 원문은 편집 이력에 남기지 않는다.
    void SetRevertTextOnEscape(bool enabled) noexcept;
    void SelectAllText() noexcept;
    // 비밀번호면 비어 있다.
    StringView SelectedText() const noexcept;
    bool IsMultiline() const noexcept;
    // 지금 초점을 가진 사용자가 있는지다(상자 바탕 선택용).
    bool IsFocused() const noexcept;
    // 글자 영역 둘레의 내용 여백(왼쪽, 위, 오른쪽, 아래)이다. 여백도 이 Element의 영역이라 누르면
    // 편집기가 초점을 받는다 — EditableTextBox가 상자 여백을 여기로 넘긴다(22.2절). 음수·NaN은
    // InvalidArgument다.
    Result<void> SetContentPadding(Vec4 padding) noexcept;
    Vec4 ContentPadding() const noexcept;

    // 세로 스크롤(px)이다(Docs/Design/UiToolkit 22.7절). 값은 마지막 배치 기준이다(배치 전에는 0).
    // caret 따라가기는 caret이 움직이거나 글자가 바뀔 때만 다음 배치에서 한 번 한다 — 막대·휠로
    // 옮긴 스크롤은 다음 배치에도 남는다(UE PositionToScrollIntoView).
    float ScrollOffset() const noexcept;
    // 줄 배치 높이(px)다.
    float ContentHeight() const noexcept;
    // 글자가 보이는 높이(배치 높이 - 위·아래 내용 여백)다.
    float ViewHeight() const noexcept;
    // 세로 스크롤을 옮긴다. [0, max(0, ContentHeight - ViewHeight)]로 자르고 비유한 값은 무시한다.
    // 예: 스크롤 막대 onScrolled(fraction) -> SetScrollOffset(fraction * ContentHeight()).
    void SetScrollOffset(float offset) noexcept;
    // 편집기 배치 폭이 width(내용 여백 포함)일 때의 줄 배치 높이다. 줄 배치를 그 폭으로 바꾸므로
    // 실제로 놓을 폭으로만 부른다(상자가 스크롤 막대를 정할 때 — 22.7절). 배치 전이면 0이다.
    float MeasureContentHeight(float width) noexcept;
    // 스타일을 바꾼다(UE FEditableTextStyle). 부르지 않으면 활성 앱 스타일의 "NormalEditableText"
    // (없으면 DefaultEditableTextStyle)다(20.11절). 불투명도가 0–1 밖이면 InvalidArgument다.
    Result<void> SetStyle(const EditableTextStyle& style) noexcept;
    const EditableTextStyle& GetStyle() const noexcept;
    // 초점·읽기 전용이 바뀌면 부른다(상자가 바탕·전경을 다시 고른다).
    void SetOnStateChanged(Delegate<void()>&& callback) noexcept;
    void SetOnTextChanged(Delegate<void(StringView)>&& callback) noexcept;
    void SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>&& callback) noexcept;

    // 글자 레이블이다(배치 설정 — 줄바꿈·정렬 — 을 바꿀 때 쓴다). Initialize 뒤에만 부른다(전에는
    // assert). Initialize 전의 측정·배치·그리기는 빈 편집기로 다룬다.
    TextLabel& Label() const noexcept;
    // 조합을 버린다. 미리보기와 IME 조합이 함께 취소된다.
    void CancelComposition() noexcept;
    void ClearHistory() noexcept;
    void MoveCaret(usize pos, bool extend) noexcept;

    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    bool SupportsKeyboardFocus() const noexcept override;
    const StyleBrush* FocusOutlineBrush() const noexcept override;
    void OnFocusReceived(uint32 user) noexcept override;
    void OnFocusLost(uint32 user) noexcept override;
    bool OnPreviewKeyChord(uint32 user, const input::InputEvent& event,
                           KeyChord chord) noexcept override;
    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;
    void ApplyStyles(const UiHostServices& host) noexcept override;

    // TextInputContext(UTF-16 위치 — 미리보기 중이면 미리보기 글자 기준이다).
    bool IsReadOnly() const noexcept override;
    bool AllowsInputMethod() const noexcept override;
    bool IsComposing() const noexcept override;
    uint32 GetTextLength() const noexcept override;
    void GetSelection(uint32& begin, uint32& length) const noexcept override;
    void SetSelection(uint32 begin, uint32 length) noexcept override;
    Result<void> GetText(uint32 begin, uint32 length, Array<char16_t>& out) const noexcept override;
    void SetText(uint32 begin, uint32 length, Span<const char16_t> text) noexcept override;
    Optional<uint32> GetCharacterIndexFromPoint(Vec2 window) const noexcept override;
    bool GetTextBounds(uint32 begin, uint32 length, Box2<float>& window) const noexcept override;
    bool GetScreenBounds(Box2<float>& window) const noexcept override;
    void BeginComposition() noexcept override;
    void UpdateCompositionRange(uint32 begin, uint32 length) noexcept override;
    void EndComposition() noexcept override;

private:
    struct Snapshot {
        String text;
        usize anchor;
        usize caret;
    };

    Result<SharedPtr<TextLabel>> AddHint(const Text& text, float size) noexcept;
    void PublishHint(SharedPtr<TextLabel> hint) noexcept;
    Result<void> ApplyStyle(const EditableTextStyle& style) noexcept;
    void NotifyStateChanged() noexcept;
    // 확정 글자로 셰이핑한다(조합 중이면 버린다).
    Result<void> Shape(StringView text, float size, bool password) noexcept;
    Result<void> ShapePassword(StringView text, float size) noexcept;
    // 글자를 셰이핑만 한다(조합 상태를 건드리지 않는다 — 미리보기용).
    Result<void> ShapeText(StringView text, float size, bool password) noexcept;
    // 조합 중이면 미리보기를 버리고 조합 전 선택으로 돌린 뒤 IME 조합을 취소한다.
    void AbandonComposition() noexcept;
    // 붙여 넣거나 IME가 넣는 글자의 줄바꿈을 맞춘다(CRLF·CR → LF, 한 줄이면 공백).
    Result<void> AppendInsertion(StringView text, StringBuilder<>& out) const noexcept;
    // 셰이핑하고 caret을 끝에 둔 뒤 글자를 바꾼다. next는 이전 글자가 된다.
    Result<void> ExchangeText(String& next) noexcept;
    usize ByteOffset(uint32 index) const noexcept;
    void Replace(usize begin, usize end, StringView insertion) noexcept;
    void Paste(usize begin, usize end, StringView text) noexcept;
    void RestoreHistory(bool redo) noexcept;
    void RememberFocusText() noexcept;
    bool RevertToFocusText() noexcept;
    // 사용자 변경을 알린다(받은 글자는 이 호출이 소유한 사본이다).
    void NotifyChanged(StringView text) noexcept;
    void Commit(TextCommitReason reason) noexcept;
    usize Adjacent(bool forward) const noexcept;
    // pos에서 클러스터 하나 옮긴 위치다.
    usize AdjacentFrom(usize pos, bool forward) const noexcept;
    // Ctrl+방향키가 멈출 위치다(UE ScanForWordBoundary — 한 글자 옮긴 뒤 줄 처음·끝·문서
    // 처음·끝이나 단어 시작까지). 비밀번호는 줄 처음·끝이다(단어 경계를 드러내지 않는다).
    usize WordStop(bool forward) const noexcept;
    usize PointerOffset(Vec2 point) const noexcept;

    // 포인터 선택 단위다(22.4절 — 한 번 누름은 클러스터, 두 번은 단어, 세 번은 줄).
    enum class SelectionUnit : uint8 {
        Cluster,
        Word,
        Line
    };

    // offset을 담는 단위 구간을 [begin, end)로 구한다(단어는 UE FTextLayout::GetWordAt 규칙 —
    // offset 뒤 경계에서 거꾸로 가며 공백이 아닌 글자가 있는 첫 구간, 비밀번호는 전체). 구할 수
    // 없으면 거짓이다.
    bool UnitAt(usize offset, SelectionUnit unit, usize& begin, usize& end) const noexcept;
    // 미리 구한 단어 경계(breaks — FindWordBoundaries 모양)로 offset의 단어를 구한다(UE GetWordAt).
    bool WordAt(Span<const usize> breaks, usize offset, usize& begin, usize& end) const noexcept;
    // 단위 선택을 [begin, end) 쪽으로 넓힌다(끌기 — 처음 고른 단위는 늘 포함한다).
    void ExtendUnitSelection(usize begin, usize end) noexcept;
    // 마지막 상호작용 시각을 지금 프레임 시각으로 둔다(caret 깜박임 — 22.5절).
    void MarkInteraction() noexcept;
    bool IsCaretVisible() const noexcept;
    // caret 폭(px)이다(UE CalculateCaretWidth — 줄 높이 기준).
    float CaretWidth() const noexcept;
    // 다음 배치에서 caret을 보이게 스크롤한다(22.7절).
    void RequestCaretInView() noexcept;
    // 스크롤을 [0, 내용 - 보이는 크기]로 자른다.
    void ClampScroll() noexcept;

    Allocator& allocator_;
    Array<Snapshot> undo_;
    Array<Snapshot> redo_;
    SharedPtr<TextShaper> shaper_;
    SharedPtr<TextLabel> label_;
    SharedPtr<TextLabel> hint_;
    String text_;
    Text hintText_;
    String focusText_;
    Delegate<void(StringView)> changed_;
    Delegate<void(StringView, TextCommitReason)> committed_;
    Delegate<void()> stateChanged_;
    EditableTextStyle style_ = DefaultEditableTextStyle();
    bool styleExplicit_ = false;
    bool canRevert_ = false;
    bool keepPointerSelection_ = false;
    usize pointerAnchor_ = 0;
    Platform platform_;
    Vec2 scroll_{};
    // 마지막 배치의 내용 크기(가로는 caret 폭 포함)와 보이는 크기다.
    Vec2 contentSize_{};
    Vec2 viewSize_{};
    bool followCaret_ = true;
    float fontSize_ = 16;
    Vec4 padding_{};
    bool composing_ = false;
    // 조합 중 보이는 글자 전체, 그 안의 조합 범위(byte), 조합 전 선택이다.
    String preview_;
    usize compositionBegin_ = 0;
    usize compositionEnd_ = 0;
    usize preCompositionAnchor_ = 0;
    usize preCompositionCaret_ = 0;
    // 초점 동안 활성인 창 텍스트 입력 서비스다(없으면 IME 글자가 Text 이벤트로 온다).
    SharedPtr<TextInputService> textInput_;
    usize anchor_ = 0;
    usize caret_ = 0;
    uint32 focusUser_ = 0;
    uint32 dragPointer_ = 0;
    uint32 dragUser_ = 0;
    PointerSource dragSource_ = PointerSource::Mouse;
    bool multiline_;
    bool password_ = false;
    bool readOnly_ = false;
    bool selectAllOnFocus_ = false;
    bool selectAllOnCommit_ = false;
    bool revertOnEscape_ = false;
    bool focused_ = false;
    bool dragging_ = false;
    KeyChord chord_;
    // 지금 포인터 끌기의 선택 단위와 처음 고른 단위 구간이다.
    SelectionUnit dragUnit_ = SelectionUnit::Cluster;
    usize unitBegin_ = 0;
    usize unitEnd_ = 0;
    // 마지막 상호작용(타자·caret 이동·포인터 누름·초점) 프레임 시각(초)이다.
    double lastInteraction_ = 0;
};

} // namespace rocks::ui
