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

namespace {

Result<void> RegisterSet(Allocator& allocator, StyleRegistry& registry, StringView name,
                         Result<void> (*fill)(StyleSet&) noexcept) noexcept {
    auto set = SharedPtr<StyleSet>::TryMake(allocator, allocator);
    if (!set) {
        return Result<void>::Failure(set.Error());
    }

    if (auto filled = fill(*set.Value()); !filled) {
        return filled;
    }

    return registry.Register(name, set.Value());
}

} // namespace

Result<SharedPtr<StyleRegistry>> CreateStandardStyleRegistry(Allocator& allocator) noexcept {
    auto registry = SharedPtr<StyleRegistry>::TryMake(allocator, allocator);
    if (!registry) {
        return registry;
    }

    auto registered = RegisterSet(allocator, *registry.Value(), CoreStyleName, &AddCoreStyles);
    if (registered) {
        registered = RegisterSet(allocator, *registry.Value(), EditorStyleName, &AddEditorStyles);
    }

    if (registered) {
        registered =
            RegisterSet(allocator, *registry.Value(), GameWidgetStyleName, &AddGameWidgetStyles);
    }

    if (!registered) {
        return Result<SharedPtr<StyleRegistry>>::Failure(registered.Error());
    }

    return registry;
}

uint64 StyleRegistry::Revision() const noexcept {
    uint64 total = revision_;
    for (const auto& entry : sets_) {
        total += entry.set->Revision();
    }

    return total;
}

} // namespace rocks::ui
