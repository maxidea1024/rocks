#include "ConfigState.h"

#include <Config/CVar.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

// CVar 범위 계층이다(Docs/Design/CVarSystem.ko.md 8·10.1절 — CVR3): 범위 생성·파괴(게임 lane 상태,
// 다음 Sync가 게시), Sync가 사본에 적용하는 범위 쓰기, 범위 사슬의 치트 허용 상태.
namespace rocks::config {

namespace {

// 다음에 줄 범위 id다(프로세스 전체 — 상수 초기화, 전역 생성자 없음). 다시 쓰지 않으므로 다른
// 문맥의 범위 id와도 겹치지 않는다.
uint64 NextScopeId = 0;

// values에서 번호 index의 자리다(같으면 그 자리, 없으면 넣을 자리 — 번호 순서).
usize LowerBound(const Array<UniquePtr<detail::ScopeValue>>& values, uint32 index) noexcept {
    usize low = 0;
    usize high = values.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (values[middle]->index < index) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low;
}

} // namespace

namespace detail {

ScopeState* FindScope(const Array<UniquePtr<ScopeState>>& scopes, uint64 id) noexcept {
    // 범위는 id 순서다(늘어나는 id로 만들고 뒤에 붙인다).
    usize low = 0;
    usize high = scopes.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const uint64 found = scopes[middle]->id;
        if (found == id) {
            return scopes[middle].Get();
        }

        if (found < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return nullptr;
}

Result<void> CopyScopes(Allocator& allocator, const Array<UniquePtr<ScopeState>>& from,
                        Array<UniquePtr<ScopeState>>& to) noexcept {
    to.Clear();
    if (auto reserved = to.TryReserve(from.Size()); !reserved) {
        return reserved;
    }

    for (const UniquePtr<ScopeState>& scope : from) {
        auto copy = UniquePtr<ScopeState>::TryMake(allocator, allocator);
        if (!copy) {
            return Result<void>::Failure(copy.Error());
        }

        copy.Value()->id = scope->id;
        copy.Value()->parent = scope->parent;
        if (auto reserved = copy.Value()->values.TryReserve(scope->values.Size()); !reserved) {
            return reserved;
        }

        for (const UniquePtr<ScopeValue>& value : scope->values) {
            auto made = UniquePtr<ScopeValue>::TryMake(allocator, allocator);
            if (!made) {
                return Result<void>::Failure(made.Error());
            }

            ScopeValue& target = *made.Value();
            target.index = value->index;
            target.layer = value->layer;
            Result<void> copied = CopyFieldValue(value->value, target.value);
            if (copied) {
                copied = target.text.TryAssign(value->text.View());
            }

            if (copied) {
                copied = target.source.TryAssign(value->source.View());
            }

            if (!copied) {
                return copied;
            }

            static_cast<void>(copy.Value()->values.TryPushBack(Move(made.Value()))); // 예약했다
        }

        static_cast<void>(to.TryPushBack(Move(copy.Value()))); // 예약했다
    }

    return Result<void>::Success();
}

bool ScopeCheatsAllowed(const ConfigState& state, const Array<UniquePtr<ScopeState>>& scopes,
                        uint64 scope, bool global) noexcept {
    const uint32 index = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(
        &cvar::detail::HandleAccess::Definition(cvar::CheatsEnabled).index);
    if (index == cvar::Index::None || state.FindSetting(cvar::CheatsEnabled.Name()) == IndexNone) {
        return global;
    }

    // 사슬은 부모로만 가고 깊이는 범위 수를 넘지 않는다.
    const ScopeState* node = FindScope(scopes, scope);
    for (usize depth = 0; node != nullptr && depth <= scopes.Size(); ++depth) {
        const usize at = LowerBound(node->values, index);
        if (at < node->values.Size() && node->values[at]->index == index) {
            return node->values[at]->value.boolean;
        }

        node = node->parent != 0 ? FindScope(scopes, node->parent) : nullptr;
    }

    return global;
}

Result<void> ApplyScopeChange(ConfigState& state, Array<UniquePtr<ScopeState>>& scopes,
                              const QueuedChange& change) noexcept {
    // 없는 범위(파괴됐거나 다른 문맥의 범위)·이 문맥에 등록되지 않은 이름의 쓰기는 버린다(6.2절).
    ScopeState* scope = FindScope(scopes, change.scope);
    const usize settingIndex = state.FindSetting(change.path.View());
    if (scope == nullptr || settingIndex == IndexNone ||
        state.settings[settingIndex]->definition == nullptr) {
        return Result<void>::Success();
    }

    const ConsoleSettingState& setting = *state.settings[settingIndex];
    const uint32 index =
        platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting.definition->index);
    const usize at = LowerBound(scope->values, index);
    const bool found = at < scope->values.Size() && scope->values[at]->index == index;
    if (change.entry.Get() == nullptr) {
        return found ? scope->values.TryRemoveAt(at) : Result<void>::Success();
    }

    const ConfigEntry& entry = *change.entry;
    // 콘솔이 출처인 Cheat 변수 쓰기는 그 범위의 치트 허용 상태로 판정한다(큐 순서 — 13.1절).
    if (entry.layer == ConfigLayer::Console &&
        cvar::HasFlag(setting.definition->flags, cvar::VarFlags::Cheat) &&
        !ScopeCheatsAllowed(state, scopes, change.scope, change.globalCheats)) {
        return Report(state, ConfigDiagnosticCode::CheatsDisabled, ConfigSeverity::Error,
                      entry.layer, entry.sourceName.View(), nullptr, setting.name.View(),
                      "is a cheat and cheats are disabled (cheat.enabled is false)");
    }

    ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
    FieldValue parsed(state.allocator);
    auto checked =
        CheckConsoleValue(state, setting, entry.layer, entry.value.View(), parsed, code, false);
    if (!checked) {
        return Result<void>::Failure(checked.Error());
    }

    if (!checked.Value().IsEmpty()) {
        return Report(state, code, ConfigSeverity::Error, entry.layer, entry.sourceName.View(),
                      nullptr, setting.name.View(), checked.Value());
    }

    // 보정(Clamped)은 진단을 남기지 않는다: 쓰기(`Set`)는 Sync가 꺼낼 때 이미 다시 검사해
    // Clamped를 남겼다(6.2절 — 같은 값에 두 번 알리지 않는다).
    auto made = UniquePtr<ScopeValue>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ScopeValue& value = *made.Value();
    value.index = index;
    value.layer = entry.layer;
    Result<void> filled = CopyFieldValue(parsed, value.value);
    if (filled) {
        filled = FormatConsoleValue(state, setting, value.value, value.text);
    }

    if (filled) {
        filled = value.source.TryAssign(entry.sourceName.View());
    }

    if (!filled) {
        return filled;
    }

    if (found) {
        scope->values[at] = Move(made.Value());
        return Result<void>::Success();
    }

    return scope->values.TryInsert(Move(made.Value()), at);
}

} // namespace detail

Result<cvar::CVarScopeId> ConfigContext::CreateScope(cvar::CVarScopeId parent) noexcept {
    detail::ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<cvar::CVarScopeId>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<cvar::CVarScopeId>::Failure(ErrorCode::WrongThread);
    }

    if (!parent.IsGlobal() && detail::FindScope(state->scopes, parent.value) == nullptr) {
        return Result<cvar::CVarScopeId>::Failure(ErrorCode::NotFound);
    }

    auto made = UniquePtr<detail::ScopeState>::TryMake(*allocator_, *allocator_);
    if (!made) {
        return Result<cvar::CVarScopeId>::Failure(made.Error());
    }

    if (auto reserved = state->scopes.TryReserve(state->scopes.Size() + 1); !reserved) {
        return Result<cvar::CVarScopeId>::Failure(reserved.Error());
    }

    // 늘어나는 id라 뒤에 붙이면 id 순서가 유지된다.
    const uint64 id =
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&NextScopeId, uint64{1}) + 1;
    made.Value()->id = id;
    made.Value()->parent = parent.value;
    static_cast<void>(state->scopes.TryPushBack(Move(made.Value()))); // 예약했다
    // 다음 Sync가 새 범위 표를 게시한다(그 전에는 다른 스레드의 Get(scope)가 Global 값이다).
    state->scopesChanged = true;
    state->dirty = true;
    return Result<cvar::CVarScopeId>::Success(cvar::CVarScopeId{id});
}

Result<void> ConfigContext::DestroyScope(cvar::CVarScopeId scope) noexcept {
    detail::ConfigState* state = state_.Get();
    if (state == nullptr || scope.IsGlobal()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    usize position = IndexNone;
    for (usize index = 0; index < state->scopes.Size(); ++index) {
        position = state->scopes[index]->id == scope.value ? index : position;
    }

    if (position == IndexNone) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 자식은 조부모(파괴한 범위의 부모)에 다시 붙는다. 범위 값은 버린다(8절).
    const uint64 grandparent = state->scopes[position]->parent;
    for (UniquePtr<detail::ScopeState>& other : state->scopes) {
        if (other->parent == scope.value) {
            other->parent = grandparent;
        }
    }

    if (auto removed = state->scopes.TryRemoveAt(position); !removed) {
        return removed;
    }

    state->scopesChanged = true;
    state->dirty = true;
    return Result<void>::Success();
}

} // namespace rocks::config
