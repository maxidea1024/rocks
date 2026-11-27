#pragma once

#include <Core/Delegate.h>
#include <Core/Text/String.h>
#include <Localization/Text.h>
#include <UiFramework/Controls/TextCommitReason.h>
#include <UiWidget/TextLayoutWidget.h>

namespace rocks::ui {

// 한 줄의 UTF-8 원문을 편집한다(UE UEditableText — Element는 UiFramework EditableText, UiToolkit
// 21.2절). 표시 문자열은 소유 WidgetTree의 셰이퍼를 사용한다. 속성은 Element가 없을 때도 값을
// 들고, Element가 있으면 그쪽으로 내린다. 사용자 편집은 Element 사건으로 받아 text 속성에 반영한다.
// SetText는 프로그램에 의한 교체이며, 사용자 입력으로 바뀔 때만 OnChanged를 보낸다.
// Enter로 입력을 확정하거나 포커스를 잃으면 OnCommitted가 현재 원문과 확정 사유를 전달한다.
// 콜백의 StringView는 호출 동안만 빌린다. 보관하려면 소유 문자열에 복사한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API EditableTextWidget : public TextLayoutWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit EditableTextWidget(Allocator& allocator) noexcept;
    Result<void> SetText(StringView text) noexcept;
    StringView GetText() const noexcept;
    // 본문이 비었을 때만 보이는 안내문이다. 복사·선택·IME 입력에는 포함되지 않는다.
    Result<void> SetHintText(const Text& hint) noexcept;
    const Text& HintText() const noexcept;
    // 상자형 편집기의 입력 영역 아래에 지역화한 오류를 표시한다. 빈 Text로 지운다.
    Result<void> SetError(const Text& message) noexcept;
    bool HasError() const noexcept;
    void SetSelectAllTextWhenFocused(bool enabled) noexcept;
    void SetSelectAllTextOnCommit(bool enabled) noexcept;
    // 포커스 시점의 내용을 Escape로 복원한다. 비밀번호의 원문은 편집 이력에 남기지 않는다.
    void SetRevertTextOnEscape(bool enabled) noexcept;
    void SelectAllText() noexcept;
    StringView SelectedText() const noexcept;
    void SetReadOnly(bool readOnly) noexcept;
    bool IsReadOnly() const noexcept;
    // 비밀번호는 화면에 가림 문자로 표시하고 복사·잘라내기와 편집 이력을 차단한다.
    Result<void> SetPassword(bool password) noexcept;
    bool IsPassword() const noexcept;
    Result<void> SetFontSize(float size) noexcept;
    void SetOnChanged(Delegate<void(StringView)>&& callback) noexcept;
    void SetOnCommitted(Delegate<void(StringView, TextCommitReason)>&& callback) noexcept;

protected:
    TextLabel& ResolveTextLabel(Element& element) const noexcept override;
    virtual bool IsMultiline() const noexcept;
    virtual bool HasBox() const noexcept;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;

private:
    // Element의 사용자 편집을 받는다(text 속성 갱신·통지 뒤 onChanged).
    void OnEditedText(StringView text) noexcept;
    Result<void> SetCaption(const Text& value, bool error) noexcept;
    ROCKS_PROPERTY(ReadOnly, Name = "hintText") Text hintText_;
    ROCKS_PROPERTY(ReadOnly, Name = "errorText") Text errorText_;
    ROCKS_PROPERTY(ReadOnly, Name = "selectAllOnFocus") bool selectAllOnFocus_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "selectAllOnCommit") bool selectAllOnCommit_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "revertOnEscape") bool revertOnEscape_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "text") String text_;
    ROCKS_PROPERTY(ReadOnly, Name = "fontSize") float fontSize_ = 16;
    ROCKS_PROPERTY(Name = "readOnly") bool readOnly_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "password") bool password_ = false;
    ROCKS_NATIVE() Delegate<void(StringView)> changed_;
    ROCKS_NATIVE() Delegate<void(StringView, TextCommitReason)> committed_;
};

} // namespace rocks::ui
