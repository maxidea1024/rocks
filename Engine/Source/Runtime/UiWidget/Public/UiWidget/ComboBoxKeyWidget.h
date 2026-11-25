#pragma once

#include <UiWidget/ComboBoxWidgetBase.h>
#include <Core/Text/Name.h>
namespace rocks::ui {

// Name 키를 유일하게 보관하는 콤보 상자다. 기본 행은 이름 풀에서 얻은 표시 이름을 사용한다.
// 항목을 삭제하면 같은 항목이 계속 선택되도록 인덱스를 조정한다. 선택된 항목을 삭제하면 선택을
// 해제한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ComboBoxKeyWidget final : public ComboBoxWidgetBase {
    ROCKS_GENERATED_BODY()
public:
    explicit ComboBoxKeyWidget(Allocator& allocator) noexcept;

    usize OptionCount() const noexcept override;
    Result<Name> OptionAt(usize index) const noexcept;
    Result<void> AddOption(ObjectRegistry& objects, Name option) noexcept;
    Result<void> RemoveOptionAt(usize index) noexcept;
    Result<void> ClearOptions() noexcept;
    Optional<usize> FindOptionIndex(Name option) const noexcept;
    Result<Name> SelectedOption() const noexcept;
    Result<void> RemoveOption(Name option) noexcept;
    Result<void> SelectOption(Name option) noexcept;

protected:
    Result<void> WriteLabel(ObjectRegistry& objects, usize index,
                            String& out) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "options") Array<Name> options_;
};

} // namespace rocks::ui
