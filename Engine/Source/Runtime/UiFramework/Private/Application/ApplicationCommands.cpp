#include <UiFramework/Application/Application.h>

namespace rocks::ui {

Result<KeyChord> Application::UpdateChord(uint32 userId, const input::InputEvent& event) noexcept {
    const auto bit = KeyModifierBit(event.key);
    const bool down = event.type == input::InputEventType::KeyDown;
    uint8 mask = down ? bit : 0;
    usize i = 0;
    while (i < modifiers_.Size() && modifiers_[i].userId != userId) {
        ++i;
    }

    if (i < modifiers_.Size()) {
        mask = modifiers_[i].mask;
        if (down) {
            mask |= bit;
        } else if (event.type == input::InputEventType::KeyUp) {
            mask &= static_cast<uint8>(~bit);
        }

        if (mask) {
            modifiers_[i].mask = mask;
        } else {
            (void)modifiers_.TryRemoveAt(i);
        }
    } else if (mask) {
        auto added = modifiers_.TryPushBack({userId, mask});
        if (!added) {
            return Result<KeyChord>::Failure(added.Error());
        }
    }

    return Result<KeyChord>::Success(ChordFromModifiers(event.key, mask));
}

bool Application::IsControlDown(uint32 userId) const noexcept {
    for (const auto& user : modifiers_) {
        if (user.userId == userId) {
            return ChordFromModifiers(input::Key::None, user.mask).control;
        }
    }

    return false;
}

bool Application::IsAltDown(uint32 userId) const noexcept {
    for (const auto& user : modifiers_) {
        if (user.userId == userId) {
            return ChordFromModifiers(input::Key::None, user.mask).alt;
        }
    }

    return false;
}

bool Application::IsShiftDown(uint32 userId) const noexcept {
    for (const auto& user : modifiers_) {
        if (user.userId == userId) {
            return ChordFromModifiers(input::Key::None, user.mask).shift;
        }
    }

    return false;
}

Result<void> Application::SetCommands(const SharedPtr<Element>& scope,
                                      const SharedPtr<CommandList>& commands) noexcept {
    if (!scope) {
        globalCommands_ = commands;
        return Result<void>::Success();
    }

    for (usize i = 0; i < commandScopes_.Size();) {
        auto target = commandScopes_[i].element.Lock();
        if (target.Get() == scope.Get()) {
            if (commands) {
                commandScopes_[i].list = commands;
            } else {
                (void)commandScopes_.TryRemoveAt(i);
            }

            return Result<void>::Success();
        }

        if (!target) {
            (void)commandScopes_.TryRemoveAt(i);
        } else {
            ++i;
        }
    }

    return commands ? commandScopes_.TryPushBack({WeakPtr<Element>(scope), commands})
                    : Result<void>::Success();
}

Result<void> Application::AddWindowCommands(const SharedPtr<CommandList>& commands) noexcept {
    if (!commands) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (const auto& entry : windowCommands_) {
        if (entry.Get() == commands.Get()) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    return windowCommands_.TryPushBack(commands);
}

void Application::RemoveWindowCommands(const CommandList& commands) noexcept {
    for (usize i = 0; i < windowCommands_.Size(); ++i) {
        if (windowCommands_[i].Get() == &commands) {
            (void)windowCommands_.TryRemoveAt(i);
            return;
        }
    }
}

bool Application::ProcessCommands(const Element* scope, KeyChord chord,
                                  bool repeat) const noexcept {
    if (!scope) {
        if (globalCommands_ && globalCommands_->Process(chord, repeat)) {
            return true;
        }

        // 창 단위 목록(메뉴 막대 가속키 등)이다. 실행이 목록을 뗄 수 있어 참조를 잡고 부른다.
        for (usize i = 0; i < windowCommands_.Size(); ++i) {
            const SharedPtr<CommandList> list = windowCommands_[i];
            if (list && list->Process(chord, repeat)) {
                return true;
            }
        }

        return false;
    }

    SharedPtr<CommandList> commands;
    for (const auto& entry : commandScopes_) {
        if (entry.element.Lock().Get() == scope) {
            commands = entry.list;
            break;
        }
    }

    return commands && commands->Process(chord, repeat);
}

} // namespace rocks::ui
