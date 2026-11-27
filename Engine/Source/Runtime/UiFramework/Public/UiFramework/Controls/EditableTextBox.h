#pragma once

#include <UiFramework/Controls/EditableText.h>

namespace rocks::ui {

// 상자 모양 편집기다(UE SEditableTextBox·SMultiLineEditableTextBox — Docs/Design/UiToolkit 22.2절).
// EditableText 하나를 담아 상태(보통·올림·초점·읽기 전용)마다 바탕 brush·전경색을 고르고, 오류가
// 있으면 오른쪽에 "!" 표지를 둔다. 상자 여백은 안쪽 편집기의 내용 여백으로 넘긴다 — 편집기가
// 상자 안쪽 전체를 덮어 여백을 눌러도 편집기가 초점을 받고 Tab 정지도 하나다.
//
// 편집 API(글자·힌트·비밀번호·읽기 전용·사건)는 Editor()로 쓴다 — 상자가 같은 setter를 다시 두지
// 않는다. Initialize 전의 Editor()는 assert다.
//
// 사용 예:
//   auto box = SharedPtr<EditableTextBox>::TryMake(allocator, allocator, shaper, platform,
//                                                  /*multiline*/ false);
//   (void)box.Value()->Initialize("Untitled", 13);
//   (void)box.Value()->Editor().SetHintText(hint);
//   (void)box.Value()->SetError(message);   // 빈 Text로 지운다
class ROCKS_UIFRAMEWORK_API EditableTextBox final : public Element {
    ROCKS_UI_ELEMENT(EditableTextBox, Element)

public:
    // shaper가 없으면 안쪽 편집기는 빈 글자만 받고 오류 표지를 만들지 못한다(NotReady).
    EditableTextBox(Allocator& allocator, SharedPtr<TextShaper> shaper, Platform platform,
                    bool multiline) noexcept;
    ~EditableTextBox() noexcept override;

    // 안쪽 편집기를 만들고 처음 글자로 셰이핑한다. 한 번만 부른다(두 번째는 AlreadyExists).
    // 실패: EditableText::Initialize와 같다.
    Result<void> Initialize(StringView text, float fontSize) noexcept;
    bool IsInitialized() const noexcept;
    EditableText& Editor() const noexcept;

    // 오류를 표시한다(UE SEditableTextBox::SetError). 빈 Text로 지운다. 실패: shaper 없음
    // NotReady, 셰이핑·할당 실패는 그 오류(실패하면 이전 오류를 그대로 둔다).
    Result<void> SetError(const Text& message) noexcept;
    bool HasError() const noexcept;
    const Text& ErrorText() const noexcept;

    // 스타일을 바꾼다(UE FEditableTextBoxStyle). 부르지 않으면 활성 앱 스타일의
    // "NormalEditableTextBox"(없으면 DefaultEditableTextBoxStyle)다(20.11절). 안쪽 편집기에는
    // style.text를 넘긴다. 바탕 brush가 해석되지 않거나 여백이 음수면 InvalidArgument다.
    Result<void> SetStyle(const EditableTextBoxStyle& style) noexcept;
    const EditableTextBoxStyle& GetStyle() const noexcept;
    // 지금 상태의 전경이다(UE SEditableTextBox::DetermineForegroundColor — 읽기 전용 > 초점 >
    // 보통). 자식(편집기·힌트)이 이 색을 물려받는다.
    ColorRef CurrentForeground() const noexcept;

    Vec2 ComputeDesiredSize(float scale) const noexcept override;
    Result<void> Arrange(const Geometry& geometry) noexcept override;
    Result<void> OnPaint(PaintList& list, int32 layer) const noexcept override;
    void OnMouseEnter(uint32 user) noexcept override;
    void OnMouseLeave(uint32 user) noexcept override;
    void ApplyStyles(const UiHostServices& host) noexcept override;
    // 상자에 준 초점은 안쪽 편집기가 받는다(UE SEditableTextBox::OnFocusReceived).
    SharedPtr<Element> FocusDelegate() const noexcept override;

protected:
    ColorRef PaintForegroundColor(bool disabled) const noexcept override;

private:
    Result<void> ApplyStyle(const EditableTextBoxStyle& style) noexcept;
    const StyleBrush& CurrentBackground() const noexcept;
    // 오류 표지 한 변(px)이다(줄 높이 — 0이면 표지 없음).
    float MarkerExtent() const noexcept;
    void OnEditorStateChanged() noexcept;

    Allocator& allocator_;
    SharedPtr<TextShaper> shaper_;
    Platform platform_;
    SharedPtr<EditableText> editor_;
    SharedPtr<TextLabel> marker_;
    Text errorText_;
    EditableTextBoxStyle style_ = DefaultEditableTextBoxStyle();
    uint32 hoverCount_ = 0;
    bool multiline_;
    bool styleExplicit_ = false;
};

} // namespace rocks::ui
