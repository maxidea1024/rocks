#include "ConfigState.h"

#include <Config/CVar.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

// CVar 범위 계층이다(Docs/Design/CVarSystem.ko.md 8·10.1·13.1절 — CVR3): 범위 생성·파괴(게임 lane
// 상태, 다음 Sync가 게시), Sync가 copy-on-write 사본에 적용하는 범위 쓰기, 범위 치트 판정.
namespace rocks::config {

namespace {

// 다음에 줄 범위 id다(프로세스 전체 — 상수 초기화, 전역 생성자 없음). 다시 쓰지 않으므로 다른
// 문맥의 범위 id와도 겹치지 않는다.
uint64 NextScopeId = 0;

// values에서 번호 index의 자리다(같으면 그 자리, 없으면 넣을 자리 — 번호 순서).
usize LowerBound(const Array<SharedPtr<const detail::ScopeValue>>& values, uint32 index) noexcept {
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

// `cheat.enabled`의 번호다(등록되지 않았으면 None).
uint32 CheatsIndex(const detail::ConfigState& state) noexcept {
    if (state.FindSetting(cvar::CheatsEnabled.Name()) == IndexNone) {
        return cvar::Index::None;
    }

    return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(
        &cvar::detail::HandleAccess::Definition(cvar::CheatsEnabled).index);
}

// 이번 Sync에서 처음 바뀌는 범위면 사본을 만들어 next에 둔다(값 배열은 포인터 복사 — 노드 캐시는
// 비운다). 이미 사본이면 그대로다. 사본을 만들 수 없으면 할당 오류다.
Result<detail::ScopeState*> MutableScope(detail::ConfigState& state,
                                         Array<SharedPtr<detail::ScopeState>>& next,
                                         usize position) noexcept {
    using Made = Result<detail::ScopeState*>;
    detail::ScopeState* current = next[position].Get();
    if (position >= state.scopes.Size() || state.scopes[position].Get() != current) {
        return Made::Success(current);
    }

    auto copy = SharedPtr<detail::ScopeState>::TryMake(state.allocator, state.allocator);
    if (!copy) {
        return Made::Failure(copy.Error());
    }

    copy.Value()->id = current->id;
    copy.Value()->parent = current->parent;
    if (auto copied = copy.Value()->values.TryAppend(current->values.View()); !copied) {
        return Made::Failure(copied.Error());
    }

    next[position] = Move(copy.Value());
    return Made::Success(next[position].Get());
}

// 콘솔이 출처인 쓰기·지우기의 권한(콘솔 읽기 전용·치트)을 적용 때 본다(13.1절). 거부하면 진단을
// 남기고 false다.
Result<bool> CheckScopePermission(detail::ConfigState& state,
                                  const Array<SharedPtr<detail::ScopeState>>& next,
                                  const detail::ConsoleSettingState& setting,
                                  const detail::QueuedChange& change, ConfigLayer layer,
                                  StringView source) noexcept {
    if (layer != ConfigLayer::Console) {
        return Result<bool>::Success(true);
    }

    ConfigDiagnosticCode code = ConfigDiagnosticCode::LayerNotAllowed;
    StringView problem =
        detail::CheckConsoleLayer(state.releaseRules, layer, setting.flags, code, false);
    if (problem.IsEmpty() && cvar::HasFlag(setting.definition->flags, cvar::VarFlags::Cheat) &&
        !detail::ScopeCheatsAllowed(state, next, change.scope, change.globalCheats)) {
        code = ConfigDiagnosticCode::CheatsDisabled;
        problem = "is a cheat and cheats are disabled (cheat.enabled is false)";
    }

    if (problem.IsEmpty()) {
        return Result<bool>::Success(true);
    }

    if (auto reported = detail::Report(state, code, ConfigSeverity::Error, layer, source, nullptr,
                                       setting.name.View(), problem);
        !reported) {
        return Result<bool>::Failure(reported.Error());
    }

    return Result<bool>::Success(false);
}

} // namespace

namespace detail {

usize FindScope(const Array<SharedPtr<ScopeState>>& scopes, uint64 id) noexcept {
    // 범위는 id 순서다(늘어나는 id로 만들고 뒤에 붙인다).
    usize low = 0;
    usize high = scopes.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const uint64 found = scopes[middle]->id;
        if (found == id) {
            return middle;
        }

        if (found < id) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return IndexNone;
}

bool ScopeCheatsAllowed(const ConfigState& state, const Array<SharedPtr<ScopeState>>& scopes,
                        uint64 scope, bool global) noexcept {
    // Global 실효값이 false면 범위와 무관하게 거부한다(범위는 좁히기만 한다 — 13.1절).
    const uint32 index = CheatsIndex(state);
    if (!global || index == cvar::Index::None) {
        return global;
    }

    // 사슬은 부모로만 가고 깊이는 범위 수를 넘지 않는다.
    usize position = FindScope(scopes, scope);
    for (usize depth = 0; position != IndexNone && depth <= scopes.Size(); ++depth) {
        const ScopeState& node = *scopes[position];
        const usize at = LowerBound(node.values, index);
        if (at < node.values.Size() && node.values[at]->index == index) {
            return node.values[at]->value.boolean;
        }

        position = node.parent != 0 ? FindScope(scopes, node.parent) : IndexNone;
    }

    return true;
}

Result<void> ApplyScopeChange(ConfigState& state, Array<SharedPtr<ScopeState>>& next,
                              const QueuedChange& change) noexcept {
    // 없는 범위(파괴됐거나 다른 문맥의 범위)·이 문맥에 등록되지 않은 이름의 쓰기는 진단 없이
    // 버린다(6.2·8절).
    const usize position = FindScope(next, change.scope);
    const usize settingIndex = state.FindSetting(change.path.View());
    if (position == IndexNone || settingIndex == IndexNone ||
        state.settings[settingIndex]->definition == nullptr) {
        return Result<void>::Success();
    }

    const ConsoleSettingState& setting = *state.settings[settingIndex];
    const uint32 index =
        platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting.definition->index);
    const ScopeState& current = *next[position];
    const usize at = LowerBound(current.values, index);
    const bool found = at < current.values.Size() && current.values[at]->index == index;

    // 지우기: 저장된 값의 출처 층보다 약한 층의 지우기는 무시한다(Global 층 지우기와 같은 뜻).
    if (change.entry.Get() == nullptr) {
        if (!found || change.layer < current.values[at]->layer) {
            return Result<void>::Success();
        }

        auto allowed =
            CheckScopePermission(state, next, setting, change, change.layer, change.tag.View());
        if (!allowed || !allowed.Value()) {
            return allowed ? Result<void>::Success() : Result<void>::Failure(allowed.Error());
        }

        auto mutated = MutableScope(state, next, position);
        if (!mutated) {
            return Result<void>::Failure(mutated.Error());
        }

        return mutated.Value()->values.TryRemoveAt(at);
    }

    const ConfigEntry& entry = *change.entry;
    auto allowed =
        CheckScopePermission(state, next, setting, change, entry.layer, entry.sourceName.View());
    if (!allowed || !allowed.Value()) {
        return allowed ? Result<void>::Success() : Result<void>::Failure(allowed.Error());
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
    auto made = SharedPtr<ScopeValue>::TryMake(state.allocator, state.allocator);
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

    auto mutated = MutableScope(state, next, position);
    if (!mutated) {
        return Result<void>::Failure(mutated.Error());
    }

    SharedPtr<const ScopeValue> stored(Move(made.Value()));
    if (found) {
        mutated.Value()->values[at] = Move(stored);
        return Result<void>::Success();
    }

    return mutated.Value()->values.TryInsert(Move(stored), at);
}

Result<void> PrepareScopes(ConfigState& state, Array<SharedPtr<ScopeState>>& next,
                           bool& changed) noexcept {
    bool writes = false;
    for (const SharedPtr<QueuedChange>& change : state.queue) {
        writes = writes || (change->scope != 0 && change->line.IsEmpty());
    }

    changed = writes || state.scopesChanged;
    next.Clear();
    if (!changed) {
        return Result<void>::Success();
    }

    // 얕은 사본이다(바뀌는 범위만 적용 때 사본을 만든다 — copy-on-write).
    if (auto copied = next.TryAppend(state.scopes.View()); !copied) {
        return copied;
    }

    for (const SharedPtr<QueuedChange>& change : state.queue) {
        if (change->scope == 0 || !change->line.IsEmpty()) {
            continue;
        }

        if (auto applied = ApplyScopeChange(state, next, *change); !applied) {
            return applied;
        }
    }

    return Result<void>::Success();
}

bool CheckScopeClear(WriteChannel& channel, const cvar::detail::VarDefinition& definition,
                     const cvar::SetOptions& options, ConfigDiagnosticCode& code) noexcept {
    if (options.scope.IsGlobal() || options.layer != ConfigLayer::Console) {
        return true;
    }

    if (!CheckConsoleLayer(channel.releaseRules, options.layer,
                           ConsoleFlagsOfDefinition(definition), code, false)
             .IsEmpty()) {
        return false;
    }

    if (!cvar::HasFlag(definition.flags, cvar::VarFlags::Cheat)) {
        return true;
    }

    SharedPtr<const ConfigSnapshot> current;
    {
        platform::LockGuard lock(channel.mutex);
        current = channel.current;
    }

    if (!SnapshotCheatsAllowed(current.Get(), options.scope.value)) {
        code = ConfigDiagnosticCode::CheatsDisabled;
        return false;
    }

    return true;
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

    if (!parent.IsGlobal() && detail::FindScope(state->scopes, parent.value) == IndexNone) {
        return Result<cvar::CVarScopeId>::Failure(ErrorCode::NotFound);
    }

    auto made = SharedPtr<detail::ScopeState>::TryMake(*allocator_, *allocator_);
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
    // 다음 Sync가 새 범위 표를 게시한다(범위만 바뀐 Sync — Global 다시 합치기는 하지 않는다. 그
    // 전에는 다른 스레드의 Get(scope)가 Global 값이다).
    state->scopesChanged = true;
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

    const usize position = detail::FindScope(state->scopes, scope.value);
    if (position == IndexNone) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // 자식은 조부모(파괴한 범위의 부모)에 다시 붙는다(부모가 바뀐 범위는 노드를 다시 만든다). 범위
    // 값은 버린다(8절). Sync 밖이므로 범위 상태를 공유하는 사본은 없다.
    const uint64 grandparent = state->scopes[position]->parent;
    for (SharedPtr<detail::ScopeState>& other : state->scopes) {
        if (other->parent == scope.value) {
            other->parent = grandparent;
            other->node = SharedPtr<const detail::ScopeNode>();
        }
    }

    if (auto removed = state->scopes.TryRemoveAt(position); !removed) {
        return removed;
    }

    state->scopesChanged = true;
    return Result<void>::Success();
}

} // namespace rocks::config
