#pragma once

#include <UiCore/TextInputContext.h>
#include <UiFramework/Application/Platform.h>
#include <UiWidget/EditableTextWidget.h>
#include <UiWidget/WidgetTextShaper.h>

namespace rocks::ui {

// 단일행·여러 행 위젯의 공용 편집 동작이다. 셰이핑 결과의 UTF-8 클러스터 경계에서만
// 커서와 선택 끝을 움직인다. 편집 콜백은 저장소에서 소유자를 다시 조회해 수명을 확인한다.
//
// 텍스트 입력 문맥이다(Docs/Design/UiToolkit 19.2절). 초점을 받으면 창의 서비스에 Activate하고,
// 잃거나 소멸하면 Deactivate한다. 조합 중의 글자는 미리보기 층(preview_)에만 있고 위젯 글자·편집
// 이력·변경 알림은 EndComposition에서 한 번 바뀐다. 조합 중 선택(anchor_·caret_)은 미리보기 안
// 위치다.
class EditableTextElement final : public Element, public TextInputContext {
public:
    EditableTextElement(Allocator& allocator, ObjectRegistry& registry, ObjectHandle owner,
                        SharedPtr<WidgetTextShaper> shaper, Platform platform, bool multiline,
                        bool boxed) noexcept;
    ~EditableTextElement() noexcept override;
    Result<void> Initialize(StringView text, float size, bool password) noexcept;
    Result<void> Shape(StringView text, float size, bool password) noexcept;
    Result<void> SetCaption(const Text& text, float size, bool error) noexcept;
    Result<void> ResizeText(EditableTextWidget& model, float size) noexcept;
    void SelectAll() noexcept;
    StringView Selection(StringView text) const noexcept;
    void RememberFocusText(const EditableTextWidget& widget) noexcept;
    TextLabel& Label() const noexcept;
    // 조합을 버린다(읽기 전용 전환 등). 미리보기와 IME 조합이 함께 취소된다.
    void CancelComposition(EditableTextWidget& owner) noexcept;
    // IME 허용 여부(비밀번호·읽기 전용)가 바뀌었다. 활성이면 서비스에 다시 알린다.
    void RefreshTextInput() noexcept;
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
    Result<SharedPtr<TextLabel>> AddCaption(const Text& text, float size, bool error) noexcept;
    void PublishCaption(SharedPtr<TextLabel> caption, bool error) noexcept;
    bool RevertToFocusText(EditableTextWidget& owner) noexcept;
    struct Snapshot {
        String text;
        usize anchor;
        usize caret;
    };
    Result<void> ShapePassword(StringView text, float size) noexcept;
    // 글자를 셰이핑만 한다(조합 상태를 건드리지 않는다 — 미리보기용).
    Result<void> ShapeText(StringView text, float size, bool password) noexcept;
    // 조합 중이면 미리보기를 버리고 조합 전 선택으로 돌린 뒤 IME 조합을 취소한다.
    void AbandonComposition() noexcept;
    // 붙여 넣거나 IME가 넣는 글자의 줄바꿈을 맞춘다(CRLF·CR → LF, 한 줄이면 공백).
    Result<void> AppendInsertion(StringView text, StringBuilder<>& out) const noexcept;
    usize ByteOffset(uint32 index) const noexcept;
    void Replace(EditableTextWidget& owner, usize begin, usize end, StringView insertion) noexcept;
    void Paste(EditableTextWidget& owner, usize begin, usize end, StringView text) noexcept;
    void RestoreHistory(EditableTextWidget& owner, bool redo) noexcept;
    Array<Snapshot> undo_;
    Array<Snapshot> redo_;
    usize Adjacent(bool forward) const noexcept;
    usize PointerOffset(Vec2 point) const noexcept;
    ObjectRegistry& registry_;
    ObjectHandle owner_;
    SharedPtr<WidgetTextShaper> shaper_;
    SharedPtr<TextLabel> label_;
    SharedPtr<TextLabel> hint_;
    SharedPtr<TextLabel> error_;
    String focusText_;
    bool canRevert_ = false;
    bool keepPointerSelection_ = false;
    usize pointerAnchor_ = 0;
    Platform platform_;
    Vec2 scroll_{};
    float fontSize_ = 16;
    float padding_ = 0;
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
    bool focused_ = false;
    bool dragging_ = false;
    KeyChord chord_;
};

} // namespace rocks::ui
