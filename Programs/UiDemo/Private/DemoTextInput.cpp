#include "DemoTextInput.h"

namespace rocks::ui {

DemoTextInput::DemoTextInput(platform::Window& window) noexcept : window_(&window) {
}

DemoTextInput::~DemoTextInput() noexcept {
    Detach();
}

void DemoTextInput::Detach() noexcept {
    if (window_ && client_.context) {
        window_->DeactivateTextInput(client_);
    }

    client_.context = nullptr;
    window_ = nullptr;
}

void DemoTextInput::Activate(TextInputContext& context) noexcept {
    if (!window_) {
        return;
    }

    // 플랫폼 문맥 객체는 하나다. 다른 UI 문맥으로 바꿀 때는 먼저 끄고(앞 문맥의 조합을 확정한다)
    // 새로 켠다. 같은 문맥이면 다시 켜기 — IME 허용 여부를 다시 읽는다.
    if (client_.context && client_.context != &context) {
        window_->DeactivateTextInput(client_);
    }

    client_.context = &context;
    window_->ActivateTextInput(client_);
}

void DemoTextInput::Deactivate(TextInputContext& context) noexcept {
    if (client_.context != &context) {
        return;
    }

    if (window_) {
        window_->DeactivateTextInput(client_);
    }

    client_.context = nullptr;
}

void DemoTextInput::CancelComposition(TextInputContext& context) noexcept {
    if (window_ && client_.context == &context) {
        window_->CancelTextComposition(client_);
    }
}

void DemoTextInput::NotifyLayoutChanged(TextInputContext& context) noexcept {
    if (window_ && client_.context == &context) {
        window_->NotifyTextInputLayoutChanged(client_);
    }
}

void DemoTextInput::Process(uint32 record) noexcept {
    if (window_) {
        window_->ProcessTextInput(record);
    }
}

} // namespace rocks::ui
