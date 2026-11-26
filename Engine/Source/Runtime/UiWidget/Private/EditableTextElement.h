#pragma once

#include <UiFramework/Platform.h>
#include <UiWidget/EditableTextWidget.h>
#include <UiWidget/WidgetTextShaper.h>

namespace rocks::ui {

// 단일행·여러 행 위젯의 공용 편집 동작이다. 셰이핑 결과의 UTF-8 클러스터 경계에서만
// 커서와 선택 끝을 움직인다. 편집 콜백은 저장소에서 소유자를 다시 조회해 수명을 확인한다.
class EditableTextElement final : public Element {
public:
    EditableTextElement(Allocator& allocator, ObjectRegistry& registry, ObjectHandle owner,
                        SharedPtr<WidgetTextShaper> shaper, Platform platform, bool multiline,
                        bool boxed) noexcept;
    Result<void> Initialize(StringView text, float size, bool password) noexcept;
    Result<void> Shape(StringView text, float size, bool password) noexcept;
    Result<void> SetCaption(const Text& text, float size, bool error) noexcept;
    Result<void> ResizeText(EditableTextWidget& model, float size) noexcept;
    void SelectAll() noexcept;
    StringView Selection(StringView text) const noexcept;
    void RememberFocusText(const EditableTextWidget& widget) noexcept;
    TextLabel& Label() const noexcept;
    void EndComposition(EditableTextWidget& owner) noexcept;
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
    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override;
    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override;
    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override;

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
    void UpdateComposition(EditableTextWidget& owner, const input::InputEvent& event) noexcept;
    void PositionCompositionWindow() const noexcept;
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
    usize compositionEnd_ = 0;
    usize compositionCaret_ = 0;
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
