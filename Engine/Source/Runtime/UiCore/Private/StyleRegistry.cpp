#include <UiCore/StyleRegistry.h>

namespace rocks::ui {

StyleRegistry::StyleRegistry(Allocator& allocator) noexcept
    : allocator_(allocator), sets_(allocator) {
}

Result<void> StyleRegistry::Register(StringView name, const SharedPtr<StyleSet>& set) noexcept {
    if (name.IsEmpty() || !set) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ++revision_;
    for (auto& entry : sets_) {
        if (entry.name.View() == name) {
            entry.set = set;
            return Result<void>::Success();
        }
    }

    String key(allocator_);
    if (auto assigned = key.TryAssign(name); !assigned) {
        return assigned;
    }

    return sets_.TryPushBack(Entry{Move(key), set});
}

void StyleRegistry::Unregister(StringView name) noexcept {
    for (usize i = 0; i < sets_.Size(); ++i) {
        if (sets_[i].name.View() == name) {
            ++revision_;
            (void)sets_.TryRemoveAt(i);
            return;
        }
    }
}

SharedPtr<StyleSet> StyleRegistry::FindSet(StringView name) const noexcept {
    for (const auto& entry : sets_) {
        if (entry.name.View() == name) {
            return entry.set;
        }
    }

    return {};
}

const StyleSet* StyleRegistry::FindSetRaw(StringView name) const noexcept {
    for (const auto& entry : sets_) {
        if (entry.name.View() == name) {
            return entry.set.Get();
        }
    }

    return nullptr;
}

uint64 StyleRegistry::Revision() const noexcept {
    uint64 total = revision_;
    for (const auto& entry : sets_) {
        total += entry.set->Revision();
    }

    return total;
}

} // namespace rocks::ui
