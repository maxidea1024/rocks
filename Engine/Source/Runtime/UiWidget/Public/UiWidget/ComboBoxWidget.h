#pragma once

#include <UiWidget/ComboBoxWidgetBase.h>
#include <Object/ObjectRefArray.h>

namespace rocks::ui {

// 객체를 강하게 참조하는 콤보 상자다. 기본 행은 객체 이름을 표시한다.
// 항목을 삭제하면 같은 항목이 계속 선택되도록 인덱스를 조정한다. 선택된 항목을 삭제하면 선택을
// 해제한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ComboBoxWidget final : public ComboBoxWidgetBase {
    ROCKS_GENERATED_BODY()
public:
    explicit ComboBoxWidget(Allocator& allocator) noexcept;

    usize OptionCount() const noexcept override;
    Result<ObjectHandle> OptionAt(usize index) const noexcept;
    Result<void> AddOption(ObjectRegistry& objects, ObjectHandle option) noexcept;
    Result<void> RemoveOptionAt(ObjectRegistry& objects, usize index) noexcept;
    Result<void> ClearOptions(ObjectRegistry& objects) noexcept;
    Optional<usize> FindOptionIndex(ObjectHandle option) const noexcept;
    Result<ObjectHandle> SelectedOption() const noexcept;
    Result<void> RemoveOption(ObjectRegistry& objects, ObjectHandle option) noexcept;
    Result<void> SelectOption(ObjectRegistry& objects, ObjectHandle option) noexcept;

protected:
    Result<void> WriteLabel(ObjectRegistry& objects, usize index,
                            String& out) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "options") ObjectRefArray<Object> options_;
};

} // namespace rocks::ui
