#include <UiCore/UiHostServices.h>

namespace rocks::ui {

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

void UiHostServices::SetStyles(SharedPtr<StyleRegistry> registry,
                               SharedPtr<StyleSet> appStyle) noexcept {
    registry_ = Move(registry);
    appStyle_ = Move(appStyle);
    ++styleChanges_;
}

uint64 UiHostServices::StyleRevision() const noexcept {
    return styleChanges_ + (registry_ ? registry_->Revision() : 0);
}

} // namespace rocks::ui
