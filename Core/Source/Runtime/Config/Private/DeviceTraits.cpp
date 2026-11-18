#include <Config/DeviceTraits.h>

#include <Core/Algorithms.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>

namespace rocks::config {

namespace {

Span<const char> ToSpan(StringView text) noexcept {
    return Span<const char>(text.Data(), text.ByteCount());
}

// 특성 이름 문법이다: 소문자 점 경로(`[a-z][a-z0-9_]*`를 `.`으로 이은 마디 하나 이상 — `platform`·
// `gpu.memory_mib`).
bool IsTraitName(StringView name) noexcept {
    bool start = true;
    for (const char c : name) {
        if (c == '.') {
            if (start) {
                return false;
            }

            start = true;
            continue;
        }

        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (start ? !lower : !(lower || digit || c == '_')) {
            return false;
        }

        start = false;
    }

    return !start;
}

bool IsFinite(double value) noexcept {
    return value == value && value - value == 0.0;
}

} // namespace

DeviceTraits::DeviceTraits(Allocator& allocator) noexcept
    : allocator_(allocator), entries_(allocator) {
}

Result<DeviceTraits::Entry*> DeviceTraits::Slot(StringView name) noexcept {
    if (!IsTraitName(name)) {
        return Result<Entry*>::Failure(ErrorCode::InvalidArgument);
    }

    for (UniquePtr<Entry>& entry : entries_) {
        if (entry->name.View() == name) {
            return Result<Entry*>::Success(entry.Get());
        }
    }

    auto made = UniquePtr<Entry>::TryMake(allocator_, allocator_);
    if (!made) {
        return Result<Entry*>::Failure(made.Error());
    }

    if (auto assigned = made.Value()->name.TryAssign(name); !assigned) {
        return Result<Entry*>::Failure(assigned.Error());
    }

    Entry* slot = made.Value().Get();
    if (auto pushed = entries_.TryPushBack(Move(made.Value())); !pushed) {
        return Result<Entry*>::Failure(pushed.Error());
    }

    return Result<Entry*>::Success(slot);
}

Result<void> DeviceTraits::SetNumber(StringView name, double value) noexcept {
    if (!IsFinite(value)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto slot = Slot(name);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    slot.Value()->text.Clear();
    slot.Value()->number = value;
    slot.Value()->numeric = true;
    return Result<void>::Success();
}

Result<void> DeviceTraits::SetText(StringView name, StringView value) noexcept {
    auto slot = Slot(name);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    if (auto assigned = slot.Value()->text.TryAssign(value); !assigned) {
        return assigned;
    }

    slot.Value()->number = 0;
    slot.Value()->numeric = false;
    return Result<void>::Success();
}

bool DeviceTraits::Find(StringView name, DeviceTrait& out) const noexcept {
    for (const UniquePtr<Entry>& entry : entries_) {
        if (entry->name.View() == name) {
            out =
                DeviceTrait{entry->name.View(), entry->text.View(), entry->number, entry->numeric};
            return true;
        }
    }

    return false;
}

DeviceTrait DeviceTraits::At(usize index) const noexcept {
    if (index >= entries_.Size()) {
        platform::Fatal("DeviceTraits::At index out of range");
    }

    const Entry& entry = *entries_[index];
    return DeviceTrait{entry.name.View(), entry.text.View(), entry.number, entry.numeric};
}

Result<bool> MatchDeviceCondition(const DeviceTraits& traits, const DeviceCondition& condition,
                                  String& reason) noexcept {
    reason.Clear();
    StringBuilder<128> text(reason.GetAllocator());
    DeviceTrait trait;
    bool matched = true;
    if (!traits.Find(condition.trait.View(), trait)) {
        static_cast<void>(
            text.TryAppendFormat("device trait {} is unknown", condition.trait.View()));
        matched = false;
    } else if (!condition.equals.IsEmpty() &&
               (trait.numeric ||
                !EqualsIgnoringAsciiCase(ToSpan(trait.text), ToSpan(condition.equals.View())))) {
        static_cast<void>(
            text.TryAppendFormat("{} is not {}", condition.trait.View(), condition.equals.View()));
        matched = false;
    } else if (!condition.contains.IsEmpty() &&
               (trait.numeric || !trait.text.Contains(condition.contains.View(),
                                                      StringComparison::IgnoreAsciiCase))) {
        static_cast<void>(text.TryAppendFormat("{} does not contain {}", condition.trait.View(),
                                               condition.contains.View()));
        matched = false;
    } else if (condition.atLeast.HasValue() &&
               (!trait.numeric || !(trait.number >= condition.atLeast.Value()))) {
        static_cast<void>(text.TryAppendFormat("{} {} is below {}", condition.trait.View(),
                                               trait.number, condition.atLeast.Value()));
        matched = false;
    } else if (condition.below.HasValue() &&
               (!trait.numeric || !(trait.number < condition.below.Value()))) {
        static_cast<void>(text.TryAppendFormat("{} {} is not below {}", condition.trait.View(),
                                               trait.number, condition.below.Value()));
        matched = false;
    }

    if (auto status = text.Status(); !status) {
        return Result<bool>::Failure(status.Error());
    }

    if (auto assigned = reason.TryAssign(text.View()); !assigned) {
        return Result<bool>::Failure(assigned.Error());
    }

    return Result<bool>::Success(matched);
}

Result<bool> MatchDeviceConditions(const DeviceTraits& traits,
                                   Span<const DeviceCondition> conditions,
                                   String& reason) noexcept {
    reason.Clear();
    for (const DeviceCondition& condition : conditions) {
        auto matched = MatchDeviceCondition(traits, condition, reason);
        if (!matched || !matched.Value()) {
            return matched;
        }
    }

    return Result<bool>::Success(true);
}

} // namespace rocks::config
