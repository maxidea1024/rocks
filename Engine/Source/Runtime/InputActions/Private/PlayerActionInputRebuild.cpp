#include "InputMappingLookup.h"

#include <Base/InitializerList.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Guid.h>
#include <InputActions/InputActionSettings.generated.h>
#include <InputActions/InputActionsLog.h>
#include <InputActions/InputModifier.generated.h>
#include <InputActions/InputTrigger.generated.h>
#include <InputActions/InputUserSettings.h>
#include <InputActions/PlayerActionInput.h>
#include <Object/ValueCopy.h>

// 평가기의 다시 만들기다(Docs/Design/InputActions 4.2절 — UE EnhancedInputSubsystemInterface.cpp
// :579-612 InjectChordBlockers, :776-884 ReorderMappings, :886-1203 RebuildControlMappings).
namespace rocks::input {

namespace {

// 재정렬의 단 수(chord 대상 → chord → 나머지 — UE ReorderMappings)다. chord 대상이면서 chord인
// 바인딩은 단 앞에 따로 둔다.
constexpr usize RebuildTierCount = 3;
// ChordBlocker를 넣을 때 바인딩 트리거에 더할 수 있는 칸(Down + ChordBlocker)이다.
constexpr usize RebuildBlockerTriggerSlots = 2;

bool RebuildHasChord(Span<const InputTrigger> triggers) noexcept {
    for (const InputTrigger& trigger : triggers) {
        if (trigger.type == InputTriggerType::ChordAction) {
            return true;
        }
    }

    return false;
}

bool RebuildHasExplicit(Span<const InputTrigger> triggers) noexcept {
    for (const InputTrigger& trigger : triggers) {
        if (KindOf(trigger.type) == TriggerKind::Explicit) {
            return true;
        }
    }

    return false;
}

bool RebuildHasAlwaysTick(Span<const InputTrigger> triggers) noexcept {
    for (const InputTrigger& trigger : triggers) {
        if (AlwaysTicks(trigger.type)) {
            return true;
        }
    }

    return false;
}

bool RebuildContainsGuid(Span<const Guid> values, const Guid& value) noexcept {
    for (const Guid& item : values) {
        if (item == value) {
            return true;
        }
    }

    return false;
}

bool RebuildContainsKey(Span<const InputKey> keys, InputKey key) noexcept {
    for (const InputKey& item : keys) {
        if (item == key) {
            return true;
        }
    }

    return false;
}

// 활성 프로필의 바인딩 목록이다(profileOverrides에 그 이름이 있으면 그것, 없으면 기본 — 4.2절 2).
Span<const InputBinding> RebuildProfileBindings(const InputContextData& data,
                                                StringView profile) noexcept {
    if (!profile.IsEmpty()) {
        for (const InputProfileBindings& entry : data.profileOverrides) {
            if (entry.profile.View() == profile) {
                return entry.bindings.View();
            }
        }
    }

    return data.bindings.View();
}

// 바인딩의 매핑 설정이 활성 프로필을 지원하는지다(UE :986-990). 프로필이 없으면 거르지 않는다.
bool RebuildSupportsProfile(const InputBinding& binding, const InputActionSettings& action,
                            StringView profile) noexcept {
    return profile.IsEmpty() || MappingSupportsProfile(MappingSettingsOf(binding, action), profile);
}

bool RebuildSameCurve(const TangentCurve& left, const TangentCurve& right) noexcept {
    if (left.KeyCount() != right.KeyCount() || left.preExtrapolation != right.preExtrapolation ||
        left.postExtrapolation != right.postExtrapolation ||
        left.defaultValue.HasValue() != right.defaultValue.HasValue() ||
        (left.defaultValue.HasValue() && left.defaultValue.Value() != right.defaultValue.Value())) {
        return false;
    }

    const Span<const TangentCurveKey> a = left.Keys();
    const Span<const TangentCurveKey> b = right.Keys();
    for (usize index = 0; index < a.Size(); ++index) {
        const TangentCurveKey& x = a[index];
        const TangentCurveKey& y = b[index];
        if (x.time != y.time || x.value != y.value || x.interp != y.interp ||
            x.tangentMode != y.tangentMode || x.tangentWeight != y.tangentWeight ||
            x.arriveTangent != y.arriveTangent || x.arriveTangentWeight != y.arriveTangentWeight ||
            x.leaveTangent != y.leaveTangent || x.leaveTangentWeight != y.leaveTangentWeight) {
            return false;
        }
    }

    return true;
}

// 데이터 칸(PROPERTY)이 모두 같은지다. 실행 중 색인(chordAction)은 보지 않는다.
bool RebuildSameTrigger(const InputTrigger& a, const InputTrigger& b) noexcept {
    return a.type == b.type && a.actuationThreshold == b.actuationThreshold &&
           a.affectedByTimeDilation == b.affectedByTimeDilation && a.holdTime == b.holdTime &&
           a.oneShot == b.oneShot && a.holdAndReleaseTime == b.holdAndReleaseTime &&
           a.tapReleaseTime == b.tapReleaseTime && a.repeatDelay == b.repeatDelay &&
           a.tapCount == b.tapCount && a.triggerOnStart == b.triggerOnStart &&
           a.pulseInterval == b.pulseInterval && a.triggerLimit == b.triggerLimit &&
           a.chordActionId == b.chordActionId;
}

bool RebuildSameModifier(const InputModifier& a, const InputModifier& b) noexcept {
    return a.kind == b.kind && a.lowerThreshold == b.lowerThreshold &&
           a.upperThreshold == b.upperThreshold && a.deadZoneType == b.deadZoneType &&
           a.scalar == b.scalar && a.negateX == b.negateX && a.negateY == b.negateY &&
           a.negateZ == b.negateZ && a.swizzle == b.swizzle && a.curveExponent == b.curveExponent &&
           a.fovScale == b.fovScale && a.smoothMethod == b.smoothMethod &&
           a.smoothSpeed == b.smoothSpeed && a.easingExponent == b.easingExponent &&
           RebuildSameCurve(a.responseX, b.responseX) &&
           RebuildSameCurve(a.responseY, b.responseY) &&
           RebuildSameCurve(a.responseZ, b.responseZ) && a.responseXAsset == b.responseXAsset &&
           a.responseYAsset == b.responseYAsset && a.responseZAsset == b.responseZAsset;
}

// 평가에 쓰는 설정이 같은지다(설명·표시 이름은 보지 않는다 — 충돌 진단용).
bool RebuildSameSettings(const InputActionSettings& a, const InputActionSettings& b) noexcept {
    if (a.valueType != b.valueType || a.triggerWhenPaused != b.triggerWhenPaused ||
        a.consumeInput != b.consumeInput || a.reserveAllMappings != b.reserveAllMappings ||
        a.accumulation != b.accumulation || a.triggers.Size() != b.triggers.Size() ||
        a.modifiers.Size() != b.modifiers.Size() ||
        a.mappingSettings.HasValue() != b.mappingSettings.HasValue()) {
        return false;
    }

    if (a.mappingSettings.HasValue() &&
        a.mappingSettings.Value().name.View() != b.mappingSettings.Value().name.View()) {
        return false;
    }

    for (usize index = 0; index < a.triggers.Size(); ++index) {
        if (!RebuildSameTrigger(a.triggers[index], b.triggers[index])) {
            return false;
        }
    }

    for (usize index = 0; index < a.modifiers.Size(); ++index) {
        if (!RebuildSameModifier(a.modifiers[index], b.modifiers[index])) {
            return false;
        }
    }

    return true;
}

// 트리거 종류 열·모디파이어 종류 열이 같은지다(상태 짝짓기 — UE FEnhancedActionKeyMapping::Equals).
bool RebuildSameShape(Span<const InputTrigger> leftTriggers,
                      Span<const InputModifier> leftModifiers,
                      Span<const InputTrigger> rightTriggers,
                      Span<const InputModifier> rightModifiers) noexcept {
    if (leftTriggers.Size() != rightTriggers.Size() ||
        leftModifiers.Size() != rightModifiers.Size()) {
        return false;
    }

    for (usize index = 0; index < leftTriggers.Size(); ++index) {
        if (leftTriggers[index].type != rightTriggers[index].type) {
            return false;
        }
    }

    for (usize index = 0; index < leftModifiers.Size(); ++index) {
        if (leftModifiers[index].kind != rightModifiers[index].kind) {
            return false;
        }
    }

    return true;
}

// 짝지은 바인딩의 트리거 상태를 넘긴다(UE OnTriggerReinstanced — LastValue, TimedBase
// HeldDuration, Hold bTriggered, Pulse TriggerCount만 — InputTriggers.cpp:68-80·133-139·199-207·
// 336-344). RepeatedTap의 탭 진행은 넘기지 않는다(UE도 새 instance다).
void RebuildTransferTrigger(InputTriggerState& target, const InputTriggerState& source) noexcept {
    target.lastValue = source.lastValue;
    target.heldDuration = source.heldDuration;
    target.holdTriggered = source.holdTriggered;
    target.pulseCount = source.pulseCount;
}

Result<void> RebuildCopyTriggers(Array<InputTrigger>& out, Span<const InputTrigger> source,
                                 usize extra) noexcept {
    if (auto reserved = out.TryReserve(source.Size() + extra); !reserved) {
        return reserved;
    }

    for (const InputTrigger& trigger : source) {
        (void)out.TryPushBack(trigger);
    }

    return Result<void>::Success();
}

Result<void> RebuildCopyModifiers(Array<InputModifier>& out, Span<const InputModifier> source,
                                  Allocator& allocator) noexcept {
    if (auto reserved = out.TryReserve(source.Size()); !reserved) {
        return reserved;
    }

    for (const InputModifier& modifier : source) {
        InputModifier copy(allocator);
        if (auto copied = copy.TryCopyFrom(modifier); !copied) {
            return copied;
        }

        (void)out.TryPushBack(Move(copy));
    }

    return Result<void>::Success();
}

} // namespace

bool PlayerActionInput::KeyDownForRebuild(const InputState* state, uint32 gamepad,
                                          InputKey key) const noexcept {
    if (state != nullptr) {
        return ReadKey(*state, gamepad, key).down;
    }

    // 즉시 다시 만들기: 마지막 평가에서 눌려 있던 키다(4.1절).
    return LastDown(key);
}

Result<void> PlayerActionInput::SortedContexts(Array<uint32>& order) const noexcept {
    if (auto reserved = order.TryReserve(contexts_.Size()); !reserved) {
        return reserved;
    }

    // 삽입 정렬: 우선순위 내림차순, 같으면 추가 순(안정 — 7.3절).
    for (uint32 index = 0; index < contexts_.Size(); ++index) {
        (void)order.TryPushBack(index);
        for (usize at = order.Size() - 1; at > 0; --at) {
            const ContextEntry& left = contexts_[order[at - 1]];
            const ContextEntry& right = contexts_[order[at]];
            const bool before = right.priority > left.priority ||
                                (right.priority == left.priority && right.order < left.order);
            if (!before) {
                break;
            }

            const uint32 swapped = order[at - 1];
            order[at - 1] = order[at];
            order[at] = swapped;
        }
    }

    return Result<void>::Success();
}

Result<void> PlayerActionInput::Rebuild(const InputState* state, uint32 gamepad) noexcept {
    Allocator& allocator = *allocator_;
    Array<uint32> order(allocator);
    if (auto sorted = SortedContexts(order); !sorted) {
        return sorted;
    }

    // ---- 1. 새 목록을 따로 다 만든다(실패면 이전 목록 그대로 — 4.6절) ----
    Array<ActionEntry> actions(allocator);
    Array<BindingEntry> bindings(allocator);
    Array<InputKey> applied(allocator);
    Array<InputKey> contextApplied(allocator);
    Array<uint32> chorded(allocator);
    Array<ChordDependency> dependencies(allocator);
    Array<Guid> chordingActions(allocator);
    Array<const InputBinding*> ordered(allocator);
    Array<InputKey> userKeys(allocator);
    // 활성 프로필: 사용자 설정이 붙어 있으면 그 현재 프로필, 아니면 SetKeyProfile 값이다(5.2절).
    const StringView activeProfile =
        settings_ != nullptr ? settings_->CurrentProfileId() : profile_.View();
    Array<const InputBinding*> tiers[RebuildTierCount] = {Array<const InputBinding*>(allocator),
                                                          Array<const InputBinding*>(allocator),
                                                          Array<const InputBinding*>(allocator)};
    // 액션 설정 원천: 정렬 순 bundle의 actions에서 처음 나온 사본이다(2.7절 — 키 소비·필터와
    // 무관하다). 설정이 다른 같은 id는 액션당 한 번 경고한다.
    Array<const InputActionData*> sources(allocator);
    Array<Guid> conflicted(allocator);
    for (const uint32 contextIndex : order) {
        for (const InputActionData& action : contexts_[contextIndex].bundle.actions) {
            const InputActionData* existing = nullptr;
            for (const InputActionData* source : sources) {
                existing = source->id == action.id ? source : existing;
            }

            if (existing == nullptr) {
                if (auto pushed = sources.TryPushBack(&action); !pushed) {
                    return pushed;
                }

                continue;
            }

            if (!RebuildSameSettings(existing->settings, action.settings) &&
                !RebuildContainsGuid(conflicted.View(), action.id)) {
                if (auto pushed = conflicted.TryPushBack(action.id); !pushed) {
                    return pushed;
                }

                Log::Warning(LogInputActions,
                             "Input action {Action} has different settings in two contexts; the "
                             "higher priority copy is used ({Code})",
                             action.id, DiagInputActionSettingsConflict);
            }
        }
    }

    auto sourceOf = [&sources](const Guid& id) noexcept -> const InputActionData* {
        for (const InputActionData* source : sources) {
            if (source->id == id) {
                return source;
            }
        }

        return nullptr;
    };

    auto findAction = [&actions](const Guid& id) noexcept -> usize {
        for (usize index = 0; index < actions.Size(); ++index) {
            if (actions[index].id == id) {
                return index;
            }
        }

        return actions.Size();
    };

    for (const uint32 contextIndex : order) {
        const ContextEntry& context = contexts_[contextIndex];
        const InputContextBundle& bundle = context.bundle;
        const Span<const InputBinding> source = RebuildProfileBindings(bundle.data, activeProfile);
        // 3단 재정렬(UE ReorderMappings): chord 대상 액션의 바인딩(chord 바인딩 먼저) → chord
        // 바인딩 → 나머지. Rocks는 원래 순서를 지키는 안정 분할이다(7.3절).
        chordingActions.Clear();
        for (const InputBinding& binding : source) {
            const InputActionData* action = sourceOf(binding.action);
            for (const Span<const InputTrigger> triggers :
                 {binding.triggers.View(), action->settings.triggers.View()}) {
                for (const InputTrigger& trigger : triggers) {
                    if (trigger.type == InputTriggerType::ChordAction &&
                        !RebuildContainsGuid(chordingActions.View(), trigger.chordActionId)) {
                        if (auto pushed = chordingActions.TryPushBack(trigger.chordActionId);
                            !pushed) {
                            return pushed;
                        }
                    }
                }
            }
        }

        ordered.Clear();
        for (Array<const InputBinding*>& tier : tiers) {
            tier.Clear();
        }

        for (const InputBinding& binding : source) {
            const InputActionData* action = sourceOf(binding.action);
            const bool chord = RebuildHasChord(binding.triggers.View()) ||
                               RebuildHasChord(action->settings.triggers.View());
            const bool chording = RebuildContainsGuid(chordingActions.View(), binding.action);
            // chord 대상의 바인딩은 chord 쪽부터(UE ExtractChords(Chorded) → ExtractChords(Other)).
            Array<const InputBinding*>& tier = chording ? tiers[0] : (chord ? tiers[1] : tiers[2]);
            if (chording && chord) {
                if (auto pushed = ordered.TryPushBack(&binding); !pushed) {
                    return pushed;
                }

                continue;
            }

            if (auto pushed = tier.TryPushBack(&binding); !pushed) {
                return pushed;
            }
        }

        for (const Array<const InputBinding*>& tier : tiers) {
            if (auto appended = ordered.TryAppend(tier.View()); !appended) {
                return appended;
            }
        }

        contextApplied.Clear();
        // 바인딩 하나를 키 하나로 넣는다(소비·액션 표·트리거 복제·chord 짝 — 4.2절 2-4).
        auto addBinding = [&](const InputBinding* binding, const InputActionData* action,
                              InputKey key) noexcept -> Result<void> {
            // 앞(상위) 컨텍스트가 쓴 키는 넣지 않는다(4.2절 3 — 소비).
            if (RebuildContainsKey(applied.View(), key)) {
                return Result<void>::Success();
            }

            const bool actionChords = RebuildHasChord(action->settings.triggers.View());
            const bool chords = actionChords || RebuildHasChord(binding->triggers.View());
            // chord 바인딩은 소비하지 않는다(대상 액션을 가리면 안 된다 — UE :1041).
            if (!chords && action->settings.consumeInput &&
                !RebuildContainsKey(contextApplied.View(), key)) {
                if (auto pushed = contextApplied.TryPushBack(key); !pushed) {
                    return pushed;
                }
            }

            // 액션 표: 정렬 순 첫 사본을 쓴다(2.7절). 설정이 다르면 경고한다.
            usize actionIndex = findAction(action->id);
            if (actionIndex == actions.Size()) {
                ActionEntry entry(allocator);
                entry.id = action->id;
                if (auto copied = detail::TryCopyValue(entry.settings, action->settings, allocator);
                    !copied) {
                    return copied;
                }

                if (auto pushed = actions.TryPushBack(Move(entry)); !pushed) {
                    return pushed;
                }
            }

            BindingEntry entry(allocator);
            entry.action = static_cast<uint32>(actionIndex);
            entry.context = bundle.id;
            entry.key = key;
            // 액션 수준 chord는 바인딩 수준에도 복제한다(UE :1076-1085). ChordBlocker·Down 자리도
            // 둔다.
            const usize chordCopies = actionChords ? action->settings.triggers.Size() : 0;
            if (auto copied =
                    RebuildCopyTriggers(entry.triggers, binding->triggers.View(), chordCopies);
                !copied) {
                return copied;
            }

            if (actionChords) {
                for (const InputTrigger& trigger : action->settings.triggers) {
                    if (trigger.type == InputTriggerType::ChordAction) {
                        (void)entry.triggers.TryPushBack(trigger);
                    }
                }
            }

            if (auto copied =
                    RebuildCopyModifiers(entry.modifiers, binding->modifiers.View(), allocator);
                !copied) {
                return copied;
            }

            // 같은 액션·키·트리거·모디파이어 종류 항목이 이미 있으면 넣지 않는다(UE AddMapping
            // Equals — EnhancedPlayerInput.cpp:1088-1101). 사용자 키로 같은 키가 여러 바인딩에
            // 붙거나 데이터가 겹쳐도 한 번만 평가한다. UE는 뒤 바인딩의 값으로 앞 항목을 다시
            // 만들지만 Rocks는 정렬 순 첫 것을 둔다(2.7절 첫 사본 규칙과 같다).
            for (const BindingEntry& existing : bindings) {
                if (existing.action == entry.action && existing.key == key &&
                    RebuildSameShape(existing.triggers.View(), existing.modifiers.View(),
                                     entry.triggers.View(), entry.modifiers.View())) {
                    return Result<void>::Success();
                }
            }

            for (const Span<const InputTrigger> triggers :
                 {binding->triggers.View(), action->settings.triggers.View()}) {
                for (const InputTrigger& trigger : triggers) {
                    if (trigger.type != InputTriggerType::ChordAction) {
                        continue;
                    }

                    bool known = false;
                    for (const ChordDependency& dependency : dependencies) {
                        known = known || (dependency.source == action->id &&
                                          dependency.target == trigger.chordActionId);
                    }

                    if (!known) {
                        if (auto pushed =
                                dependencies.TryPushBack({action->id, trigger.chordActionId});
                            !pushed) {
                            return pushed;
                        }
                    }
                }
            }

            if (chords) {
                if (auto pushed = chorded.TryPushBack(static_cast<uint32>(bindings.Size()));
                    !pushed) {
                    return pushed;
                }
            }

            if (auto pushed = bindings.TryPushBack(Move(entry)); !pushed) {
                return pushed;
            }

            return Result<void>::Success();
        };

        for (const InputBinding* binding : ordered) {
            const InputActionData* action = sourceOf(binding->action);
            if (!RebuildSupportsProfile(*binding, action->settings, activeProfile)) {
                continue;
            }

            // 기본 키: 빈 키는 무효(조용히), 모르는 이름은 경고 뒤 무효다(2.7절 — UE도 빈 키 매핑을
            // 받는다).
            InputKey defaultKey;
            if (!binding->key.IsEmpty()) {
                auto parsed = ParseInputKey(binding->key.View());
                if (parsed) {
                    defaultKey = parsed.Value();
                } else {
                    Log::Warning(LogInputActions,
                                 "Input binding key '{Key}' of context {Context} is unknown; only "
                                 "user keys apply to the binding ({Code})",
                                 binding->key.View(), bundle.id, DiagInputBindingKeyUnknown);
                }
            }

            // 사용자 키가 있으면 그 키들, 없으면 기본 키다(5.2절 적용 — UE :1004-1020).
            userKeys.Clear();
            const PlayerMappingSettings* mapping = MappingSettingsOf(*binding, action->settings);
            if (settings_ != nullptr && mapping != nullptr) {
                if (auto keys =
                        settings_->KeysForBinding(mapping->name.View(), defaultKey, userKeys);
                    !keys) {
                    return keys;
                }
            }

            if (userKeys.IsEmpty()) {
                if (!defaultKey.IsValid()) {
                    continue;
                }

                if (auto pushed = userKeys.TryPushBack(defaultKey); !pushed) {
                    return pushed;
                }
            }

            for (const InputKey key : userKeys) {
                if (auto added = addBinding(binding, action, key); !added) {
                    return added;
                }
            }
        }

        // 차단 집합은 컨텍스트 하나를 다 처리한 뒤 합친다(같은 컨텍스트 안 키 공유 — UE :1091).
        if (auto appended = applied.TryAppend(contextApplied.View()); !appended) {
            return appended;
        }
    }

    // ---- 2. ChordBlocker: chord 바인딩보다 뒤이고 같은 키인 바인딩(모든 컨텍스트 — UE :579-612)
    // ----
    for (const uint32 chordIndex : chorded) {
        const Guid chordAction = actions[bindings[chordIndex].action].id;
        const InputKey chordKey = bindings[chordIndex].key;
        for (usize index = chordIndex + 1; index < bindings.Size(); ++index) {
            BindingEntry& binding = bindings[index];
            if (!(binding.key == chordKey)) {
                continue;
            }

            if (auto reserved = binding.triggers.TryEnsureCapacity(binding.triggers.Size() +
                                                                   RebuildBlockerTriggerSlots);
                !reserved) {
                return reserved;
            }

            // Explicit이 하나도 없으면 "트리거 없음"과 같게 아주 작은 임계 Down을 먼저 넣는다(UE
            // :596).
            if (!RebuildHasExplicit(binding.triggers.View()) &&
                !RebuildHasExplicit(actions[binding.action].settings.triggers.View())) {
                InputTrigger down;
                down.actuationThreshold = ChordBlockerDownThreshold;
                (void)binding.triggers.TryPushBack(down);
            }

            InputTrigger blocker;
            blocker.type = InputTriggerType::ChordBlocker;
            blocker.chordActionId = chordAction;
            (void)binding.triggers.TryPushBack(blocker);
        }
    }

    // ---- 3. 사라진 액션: 이전 표에서 진행 중이던 액션은 남긴다(4.2절 7 — 평가 전에 다시 만들기가
    // 또 와도 다시 남긴다, UE RemovedActions는 매번 다시 모은다) ----
    const usize mappedCount = actions.Size();
    Array<usize> carried(allocator);
    // 주입 중인 액션(주입 전용·직전 주입·연속 주입)은 주입 전용으로 남긴다(4.3절 3 — 주입이
    // 이어진다).
    auto stillInjected = [this](const ActionEntry& old) noexcept {
        if (old.injectedOnly || old.injectedLast) {
            return true;
        }

        for (const InjectionEntry& injection : injections_) {
            if (injection.action == old.id &&
                (injection.continuous || !injection.pending.IsEmpty())) {
                return true;
            }
        }

        return false;
    };

    for (const bool injectedPass : {false, true}) {
        for (usize index = 0; index < actions_.Size(); ++index) {
            const ActionEntry& old = actions_[index];
            if (findAction(old.id) < mappedCount || stillInjected(old) != injectedPass) {
                continue;
            }

            if (!injectedPass && old.state == TriggerState::None &&
                old.events == ActionEvent::None) {
                continue;
            }

            ActionEntry entry(allocator);
            entry.id = old.id;
            if (auto copied = detail::TryCopyValue(entry.settings, old.settings, allocator);
                !copied) {
                return copied;
            }

            entry.unmapped = !injectedPass;
            entry.injectedOnly = injectedPass;
            if (auto pushed = actions.TryPushBack(Move(entry)); !pushed) {
                return pushed;
            }

            if (auto pushed = carried.TryPushBack(index); !pushed) {
                return pushed;
            }
        }
    }

    // ---- 4. chord 색인·상태 배열·alwaysTick ----
    // chord 대상은 앞쪽(바인딩이 가리키는 액션)에서만 찾는다(사라진 액션은 대상이 아니다).
    auto resolve = [&](Array<InputTrigger>& triggers) noexcept {
        ResolveChords(triggers.View(), actions, mappedCount);
    };

    for (ActionEntry& action : actions) {
        resolve(action.settings.triggers);
        if (auto sized = action.triggerStates.TryResize(action.settings.triggers.Size()); !sized) {
            return sized;
        }

        if (auto sized = action.modifierStates.TryResize(action.settings.modifiers.Size());
            !sized) {
            return sized;
        }

        action.value = InputValue(action.settings.valueType, Vec3d());
    }

    for (BindingEntry& binding : bindings) {
        resolve(binding.triggers);
        binding.alwaysTick = RebuildHasAlwaysTick(binding.triggers.View()) ||
                             RebuildHasAlwaysTick(actions[binding.action].settings.triggers.View());
        if (auto sized = binding.triggerStates.TryResize(binding.triggers.Size()); !sized) {
            return sized;
        }

        if (auto sized = binding.modifierStates.TryResize(binding.modifiers.Size()); !sized) {
            return sized;
        }
    }

    Array<bool> usedOld(allocator);
    if (auto sized = usedOld.TryResize(bindings_.Size()); !sized) {
        return sized;
    }

    Array<TriggerState> chordStates(allocator);
    if (auto sized = chordStates.TryResize(mappedCount); !sized) {
        return sized;
    }

    // 이벤트 목록은 액션 수만큼 미리 잡는다(평가 무할당 — 4.6절). 전달 중(즉시 다시 만들기)에는
    // 전달하는 쪽이 Events()를 돌고 있으므로 잡지 않는다(다음 평가가 필요하면 늘린다).
    if (!dispatchPending_) {
        if (auto reserved = events_.TryReserve(actions.Size()); !reserved) {
            return reserved;
        }
    }

    // ---- 5. 상태 보존(할당 없음 — UE :1113-1192) ----
    for (BindingEntry& binding : bindings) {
        const Guid actionId = actions[binding.action].id;
        bool matched = false;
        for (usize index = 0; index < bindings_.Size() && !matched; ++index) {
            BindingEntry& old = bindings_[index];
            if (usedOld[index] || !(old.key == binding.key) ||
                !(actions_[old.action].id == actionId) ||
                !RebuildSameShape(old.triggers.View(), old.modifiers.View(),
                                  binding.triggers.View(), binding.modifiers.View())) {
                continue;
            }

            // 같은 액션·키·종류 열: 실행 상태를 넘기고 매개변수는 새 값이다.
            usedOld[index] = true;
            matched = true;
            for (usize at = 0; at < binding.triggerStates.Size(); ++at) {
                RebuildTransferTrigger(binding.triggerStates[at], old.triggerStates[at]);
            }

            for (usize at = 0; at < binding.modifierStates.Size(); ++at) {
                binding.modifierStates[at] = old.modifierStates[at];
            }

            binding.ignored = old.ignored;
        }

        // 새 Boolean 바인딩의 키가 지금 눌려 있으면 놓을 때까지 무시한다.
        if (!matched && pendingIgnorePressed_ &&
            actions[binding.action].settings.valueType == InputValueType::Boolean &&
            KeyDownForRebuild(state, gamepad, binding.key)) {
            binding.ignored = true;
        }
    }

    for (ActionEntry& action : actions) {
        if (action.unmapped || action.injectedOnly) {
            continue;
        }

        for (const ActionEntry& old : actions_) {
            if (!(old.id == action.id)) {
                continue;
            }

            action.value = old.value.ConvertTo(action.settings.valueType);
            action.state = old.state;
            action.events = old.events;
            action.elapsedSeconds = old.elapsedSeconds;
            action.triggeredSeconds = old.triggeredSeconds;
            action.injectedLast = old.injectedLast;
            // 액션 트리거·모디파이어 상태는 종류 열이 같을 때만 넘긴다(4.2절 6 — 7.3절).
            if (RebuildSameShape(old.settings.triggers.View(), old.settings.modifiers.View(),
                                 action.settings.triggers.View(),
                                 action.settings.modifiers.View())) {
                for (usize at = 0; at < action.triggerStates.Size(); ++at) {
                    action.triggerStates[at] = old.triggerStates[at];
                }

                for (usize at = 0; at < action.modifierStates.Size(); ++at) {
                    action.modifierStates[at] = old.modifierStates[at];
                }
            }

            break;
        }
    }

    // 사라진 액션·주입 전용 액션은 실행 상태를 그대로 옮긴다(사라진 액션은 다음 평가가 이벤트 없는
    // 액션으로 마무리한다).
    usize carriedAt = 0;
    for (ActionEntry& action : actions) {
        if (!action.unmapped && !action.injectedOnly) {
            continue;
        }

        const ActionEntry& old = actions_[carried[carriedAt++]];
        // 사라진 액션의 값은 0이다(UE NotifyInputActionsUnmapped의 Value.Reset). 주입 전용은
        // 그대로다.
        action.value = action.unmapped ? InputValue(action.settings.valueType, Vec3d()) : old.value;
        action.injectedLast = old.injectedLast;
        action.state = old.state;
        action.events = old.events;
        action.elapsedSeconds = old.elapsedSeconds;
        action.triggeredSeconds = old.triggeredSeconds;
        for (usize at = 0; at < action.triggerStates.Size(); ++at) {
            action.triggerStates[at] = old.triggerStates[at];
        }

        for (usize at = 0; at < action.modifierStates.Size(); ++at) {
            action.modifierStates[at] = old.modifierStates[at];
        }
    }

    actions_ = Move(actions);
    bindings_ = Move(bindings);
    chordStates_ = Move(chordStates);
    chordDependencies_ = Move(dependencies);
    mappedActions_ = mappedCount;
    // 주입 트리거의 chord 색인도 새 표로 다시 푼다(할당 없음).
    for (InjectionEntry& injection : injections_) {
        ResolveChords(injection.triggers.View(), actions_, mappedCount);
    }

    // ---- 6. 모디파이어 초기 계산(dt 0 — UE InitializeMappingActionModifiers :1212-1219) ----
    for (BindingEntry& binding : bindings_) {
        ActionEntry& action = actions_[binding.action];
        for (usize at = 0; at < binding.modifiers.Size(); ++at) {
            action.value = ApplyInputModifier(binding.modifiers[at], binding.modifierStates[at],
                                              action.value, ModifierContext{});
        }
    }

    rebuildPending_ = false;
    pendingIgnorePressed_ = true;
    settingsRevision_ = settings_ != nullptr ? settings_->Revision() : 0;
    return Result<void>::Success();
}

} // namespace rocks::input
