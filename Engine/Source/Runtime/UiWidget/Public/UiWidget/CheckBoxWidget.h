#pragma once

#include <Core/Delegate.h>
#include <UiFramework/Controls/CheckBox.h>
#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 체크 상태를 유지한다. SetChecked로 초기화하고 SetOnChanged로 사용자 입력을 받는다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API CheckBoxWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit CheckBoxWidget(Allocator& allocator) noexcept;
    void SetChecked(bool checked) noexcept;
    // 여러 항목이 섞여 있을 때 Indeterminate로 표시한다. 다음 사용자 입력은 Checked로 전환한다.
    Result<void> SetState(CheckState state) noexcept;
    CheckState GetState() const noexcept;
    void SetOnStateChanged(Delegate<void(CheckState)>&& callback) noexcept;
    bool IsChecked() const noexcept;
    void SetOnChanged(Delegate<void(bool)>&& callback) noexcept;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "checked") bool checked_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "indeterminate") bool indeterminate_ = false;
    ROCKS_NATIVE() Delegate<void(CheckState)> stateChanged_;
    ROCKS_NATIVE() Delegate<void(bool)> changed_;
};

} // namespace rocks::ui
