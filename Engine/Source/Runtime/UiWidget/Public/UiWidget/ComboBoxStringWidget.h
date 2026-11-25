#pragma once

#include <UiWidget/ComboBoxWidgetBase.h>

namespace rocks::ui {

// 문자열을 복사해 보관하는 콤보 상자다. 같은 문자열이 여러 개면 첫 항목을 선택한다.
// 항목을 삭제하면 같은 항목이 계속 선택되도록 인덱스를 조정한다. 선택된 항목을 삭제하면 선택을
// 해제한다.
ROCKS_CLASS()
class ROCKS_UIWIDGET_API ComboBoxStringWidget final : public ComboBoxWidgetBase {
    ROCKS_GENERATED_BODY()
public:
    explicit ComboBoxStringWidget(Allocator& allocator) noexcept;

    usize OptionCount() const noexcept override;
    Result<StringView> OptionAt(usize index) const noexcept;
    Result<void> AddOption(StringView option) noexcept;
    Result<void> RemoveOptionAt(usize index) noexcept;
    Result<void> ClearOptions() noexcept;
    Optional<usize> FindOptionIndex(StringView option) const noexcept;
    Result<StringView> SelectedOption() const noexcept;
    Result<void> RemoveOption(StringView option) noexcept;
    Result<void> SelectOption(StringView option) noexcept;

protected:
    Result<void> WriteLabel(ObjectRegistry& objects, usize index,
                            String& out) const noexcept override;

private:
    ROCKS_PROPERTY(ReadOnly, Name = "options") Array<String> options_;
};

} // namespace rocks::ui
