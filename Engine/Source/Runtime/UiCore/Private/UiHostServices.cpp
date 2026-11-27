#include <UiCore/UiHostServices.h>

namespace rocks::ui {

UiHostServices::UiHostServices(Allocator& allocator) noexcept : appStyle_(allocator) {
}

void UiHostServices::SetPopupWorkArea(PopupWorkArea&& provider) noexcept {
    popupWorkArea_ = Move(provider);
}

bool UiHostServices::QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept {
    return popupWorkArea_.IsBound() && popupWorkArea_.Execute(window, area);
}

void UiHostServices::SetTextInputService(SharedPtr<TextInputService> service) noexcept {
    textInput_ = Move(service);
}

const SharedPtr<TextInputService>& UiHostServices::TextInput() const noexcept {
    return textInput_;
}

void UiHostServices::SetFrameTime(const FrameTime& time) noexcept {
    time_ = time;
}

const FrameTime& UiHostServices::Time() const noexcept {
    return time_;
}

Result<void> UiHostServices::SetStyles(SharedPtr<StyleRegistry> registry,
                                       StringView appStyle) noexcept {
    if (auto assigned = appStyle_.TryAssign(appStyle); !assigned) {
        return assigned;
    }

    // 빠지는 등록부의 revision을 접어 넣어 합이 줄지 않게 한다.
    styleChanges_ += 1 + (registry_ ? registry_->Revision() : 0);
    registry_ = Move(registry);
    return Result<void>::Success();
}

uint64 UiHostServices::StyleRevision() const noexcept {
    return styleChanges_ + (registry_ ? registry_->Revision() : 0);
}

} // namespace rocks::ui
