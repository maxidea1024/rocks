#pragma once

#include <UiWidget/UiTypeBindings.h>
#include <Core/Delegate.h>
#include <UiFramework/Controls/CheckBox.h>
#include <UiWidget/ContentWidget.h>

namespace rocks::ui {

// 체크 상태를 유지한다. SetChecked로 초기화하고 OnChanged 이벤트로 사용자 입력을 받는다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API CheckBoxWidget final : public ContentWidget {
    ROCKS_GENERATED_BODY()
public:
    explicit CheckBoxWidget(Allocator& allocator) noexcept;
    void SetChecked(bool checked) noexcept;
    // 여러 항목이 섞여 있을 때 Indeterminate로 표시한다. 다음 사용자 입력은 Checked로 전환한다.
    Result<void> SetState(CheckState state) noexcept;
    CheckState GetState() const noexcept;
    // 사건은 리플렉션 이벤트다(Docs/Design/UiToolkit 21.5a절): C++는 TryAddLambda·TryAdd(native
    // 구독), 에셋·Sparks는 TryBind(리플렉션 함수)로 받는다. 구독은 여럿일 수 있다.
    Event<CheckState>& OnStateChanged() noexcept;
    bool IsChecked() const noexcept;
    Event<bool>& OnChanged() noexcept;

protected:
    Result<SharedPtr<Element>> CreatePanel(Allocator& allocator) noexcept override;
    Result<SharedPtr<Element>> BuildElement(ObjectRegistry& objects) noexcept override;
    void ApplyProperties(Element& element) noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "checked") bool checked_ = false;
    ROCKS_PROPERTY(ReadOnly, Name = "indeterminate") bool indeterminate_ = false;
    ROCKS_PROPERTY(Transient, Name = "stateChanged", Params = "state")
    Event<CheckState> stateChanged_;
    ROCKS_PROPERTY(Transient, Name = "changed", Params = "checked") Event<bool> changed_;
};

} // namespace rocks::ui
