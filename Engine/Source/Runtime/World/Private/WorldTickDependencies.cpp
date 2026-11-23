#include <Base/InitializerList.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Math/MathUtility.h>
#include <Core/Text/StringBuilder.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>

namespace rocks {

namespace {

// Tick 순환 경고에 싣는 대상 표시("타입#slot")의 최대 byte 수다. 넘치면 잘린다.
constexpr usize MaxTickLabelBytes = 128;

// 선언 색인의 키는 slot × 이 값 + hook이다(hook 세 종류를 담는 2비트).
constexpr uint64 TickHooksPerObject = 4;

constexpr TickHook AllTickHooks[] = {TickHook::Update, TickHook::Fixed, TickHook::Late};

uint8 HookIndex(TickHook hook) noexcept {
    return static_cast<uint8>(hook);
}

bool HookWantedBy(TickHook hook, const TickSettings& settings) noexcept {
    switch (hook) {
    case TickHook::Fixed:
        return settings.fixedTick;
    case TickHook::Late:
        return settings.lateTick;
    case TickHook::Update:
        break;
    }

    return settings.tick;
}

bool HasSource(uint8 sources, TickEdgeSource source) noexcept {
    return (sources & static_cast<uint8>(source)) != 0;
}

// 정의된 출처 하나인지다(0이나 여러 비트를 합친 값은 사용자 순환 거부를 우회할 수 있어 받지
// 않는다).
bool IsValidTickEdgeSource(TickEdgeSource source) noexcept {
    switch (source) {
    case TickEdgeSource::User:
    case TickEdgeSource::Attachment:
    case TickEdgeSource::Framework:
        return true;
    }

    return false;
}

} // namespace

const char* EngineStageName(EngineStage stage) noexcept {
    switch (stage) {
    case EngineStage::PhysicsResults:
        return "PhysicsResults";
    case EngineStage::SkeletalPhysicsFinalize:
        return "SkeletalPhysicsFinalize";
    case EngineStage::Timers:
        return "Timers";
    case EngineStage::TickableSubsystems:
        return "TickableSubsystems";
    case EngineStage::CameraUpdate:
        return "CameraUpdate";
    case EngineStage::Count:
        break;
    }

    return "Unknown";
}

// --- 대상 조회 ---

uint64 World::TickPointKey(const TickPoint& point) noexcept {
    return static_cast<uint64>(point.object.slot) * TickHooksPerObject + HookIndex(point.hook);
}

bool World::IsTickPointAlive(const TickPoint& point) const noexcept {
    if (FindActorRecord(point.object) != nullptr) {
        return true;
    }

    const ComponentRecord* entry = componentRecords_.Find(point.object.slot);
    return entry != nullptr && entry->component == point.object;
}

TickHandle World::RegisteredTick(const TickPoint& point) const noexcept {
    const uint8 hook = HookIndex(point.hook);
    if (const ActorRecord* record = FindActorRecord(point.object); record != nullptr) {
        return record->ticks[hook];
    }

    const ComponentRecord* entry = componentRecords_.Find(point.object.slot);
    if (entry != nullptr && entry->component == point.object) {
        return entry->ticks[hook];
    }

    return {};
}

bool World::IsTickTargetDestroying(ObjectHandle object) noexcept {
    if (const ActorRecord* record = FindActorRecord(object); record != nullptr) {
        return record->state == ActorState::PendingDestroy;
    }

    auto state = With<Component>(object, [](Component& component) noexcept {
        return component.state_ == ComponentState::PendingDestroy;
    });
    return state && state.Value();
}

Result<TickSettings> World::ReadTickSettings(ObjectHandle object) noexcept {
    if (FindActorRecord(object) != nullptr) {
        return With<Actor>(object, [](Actor& actor) noexcept { return actor.tickSettings_; });
    }

    const ComponentRecord* entry = componentRecords_.Find(object.slot);
    if (entry != nullptr && entry->component == object) {
        return With<Component>(
            object, [](Component& component) noexcept { return component.tickSettings_; });
    }

    return Result<TickSettings>::Failure(ErrorCode::StaleHandle);
}

Result<void> World::WriteTickSettings(ObjectHandle object, const TickSettings& settings) noexcept {
    if (FindActorRecord(object) != nullptr) {
        return With<Actor>(object, [&](Actor& actor) noexcept { actor.tickSettings_ = settings; });
    }

    return With<Component>(
        object, [&](Component& component) noexcept { component.tickSettings_ = settings; });
}

// --- 선언 그래프 ---

void World::PruneTickDeclarations(TickDeclarations& node) noexcept {
    for (usize index = node.prerequisites.Size(); index > 0; --index) {
        if (!IsTickPointAlive(node.prerequisites[index - 1].point)) {
            auto removed = node.prerequisites.TryRemoveAt(index - 1);
            (void)removed;
        }
    }

    for (usize index = node.dependents.Size(); index > 0; --index) {
        if (!IsTickPointAlive(node.dependents[index - 1])) {
            auto removed = node.dependents.TryRemoveAt(index - 1);
            (void)removed;
        }
    }
}

Result<bool> World::TickDeclarationReaches(const TickPoint& from, const TickPoint& to) noexcept {
    // 방문 번호가 한 바퀴 돌면 모든 표시를 지운다.
    if (++tickDeclarationMark_ == 0) {
        tickDeclarations_.ForEach(
            [](const uint64&, TickDeclarations& node) noexcept { node.mark = 0; });

        tickDeclarationMark_ = 1;
    }

    const uint32 mark = tickDeclarationMark_;
    tickDeclarationStack_.Clear();
    auto pushed = tickDeclarationStack_.TryPushBack(from);
    if (!pushed) {
        return Result<bool>::Failure(pushed.Error());
    }

    // 선행 간선을 따라간다. 각 선언을 한 번만 펼친다(선형 시간).
    while (!tickDeclarationStack_.IsEmpty()) {
        const TickPoint current = tickDeclarationStack_.TryPopBack().Value();
        if (current == to) {
            return Result<bool>::Success(true);
        }

        TickDeclarations* node = tickDeclarations_.Find(TickPointKey(current));
        if (node == nullptr || !(node->self == current) || node->mark == mark) {
            continue;
        }

        node->mark = mark;
        for (const TickDependency& dependency : node->prerequisites) {
            if (!IsTickPointAlive(dependency.point)) {
                continue;
            }

            auto next = tickDeclarationStack_.TryPushBack(dependency.point);
            if (!next) {
                return Result<bool>::Failure(next.Error());
            }
        }
    }

    return Result<bool>::Success(false);
}

void World::WarnTickCycle(const TickPoint& before, const TickPoint& after,
                          bool automatic) noexcept {
    // 이름이 있는 곳이 이 경계다. Actor·Component에는 이름이 없어 "타입#slot"으로 알린다.
    auto describe = [this](ObjectHandle object, FixedStringBuilder& out) noexcept {
        auto named = With<Object>(object, [&](Object& value) noexcept {
            const TypeLease type = value.GetType();
            (void)out.TryAppendFormat("{}#{}", type.Name(), object.slot);
        });
        if (!named) {
            (void)out.TryAppendFormat("Object#{}", object.slot);
        }
    };

    char beforeStorage[MaxTickLabelBytes];
    char afterStorage[MaxTickLabelBytes];
    FixedStringBuilder beforeLabel{Span<char>(beforeStorage)};
    FixedStringBuilder afterLabel{Span<char>(afterStorage)};
    describe(before.object, beforeLabel);
    describe(after.object, afterLabel);
    LogContextScope scope("World", Name());
    if (automatic) {
        Log::Warning(
            LogWorldTick,
            "Skipped automatic tick prerequisite {Before} -> {After}: would create a cycle",
            beforeLabel.View(), afterLabel.View());
    } else {
        Log::Warning(LogWorldTick,
                     "Rejected tick prerequisite {Before} -> {After}: would create a cycle",
                     beforeLabel.View(), afterLabel.View());
    }
}

Result<void> World::AddTickPrerequisite(TickPoint dependent, TickPoint prerequisite,
                                        TickEdgeSource source) noexcept {
    if (!IsValidTickEdgeSource(source)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!IsTickPointAlive(dependent) || !IsTickPointAlive(prerequisite)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (IsTickTargetDestroying(dependent.object) || IsTickTargetDestroying(prerequisite.object)) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    // 같은 hook 자신, 고정 스텝 hook과 가변 hook 사이(실행 횟수가 다르다)는 뜻이 없다.
    if (dependent == prerequisite ||
        (dependent.hook == TickHook::Fixed) != (prerequisite.hook == TickHook::Fixed)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto afterSettings = ReadTickSettings(dependent.object);
    auto beforeSettings = ReadTickSettings(prerequisite.object);
    if (!afterSettings || !beforeSettings) {
        return Result<void>::Failure(!afterSettings ? afterSettings.Error()
                                                    : beforeSettings.Error());
    }

    if (!HookWantedBy(dependent.hook, afterSettings.Value()) ||
        !HookWantedBy(prerequisite.hook, beforeSettings.Value())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 두 선언을 만든다(재배치될 수 있으므로 포인터는 둘 다 만든 뒤 다시 찾는다).
    for (const TickPoint& point : {dependent, prerequisite}) {
        auto added = tickDeclarations_.FindOrAddWith(TickPointKey(point), [this]() noexcept {
            return Result<TickDeclarations>::Success(TickDeclarations(allocator_));
        });
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        TickDeclarations& node = *added.Value().value;
        if (!(node.self == point)) {
            // 같은 slot을 다시 쓴 객체다(옛 선언은 파괴 정리가 지웠어야 하지만 남았으면 버린다).
            node.self = point;
            node.prerequisites.Clear();
            node.dependents.Clear();
        }
    }

    TickDeclarations* after = tickDeclarations_.Find(TickPointKey(dependent));
    TickDeclarations* before = tickDeclarations_.Find(TickPointKey(prerequisite));
    PruneTickDeclarations(*after);
    PruneTickDeclarations(*before);
    const uint8 bit = static_cast<uint8>(source);
    const TickHandle afterTick = RegisteredTick(dependent);
    const TickHandle beforeTick = RegisteredTick(prerequisite);
    const bool registered = ticks_.IsRegistered(afterTick) && ticks_.IsRegistered(beforeTick);
    for (TickDependency& existing : after->prerequisites) {
        if (existing.point == prerequisite) {
            existing.sources = static_cast<uint8>(existing.sources | bit);
            // 앞서 스케줄러 적용이 할당 실패로 빠졌을 수 있다. 중복 추가는 성공이므로 다시 맞춘다.
            return registered ? ticks_.AddPrerequisite(afterTick, beforeTick)
                              : Result<void>::Success();
        }
    }

    // prerequisite가 이미 (간접적으로) dependent 뒤에 있으면 새 간선은 순환이다. 등록 여부와 무관한
    // 선언 전체에서 본다(시작 전 선언끼리의 순환도 이 호출에서 거부한다 — ERD2·ERD5).
    auto cycle = TickDeclarationReaches(prerequisite, dependent);
    if (!cycle) {
        return Result<void>::Failure(cycle.Error());
    }

    if (cycle.Value()) {
        const bool automatic = source != TickEdgeSource::User;
        WarnTickCycle(prerequisite, dependent, automatic);
        return automatic ? Result<void>::Success()
                         : Result<void>::Failure(ErrorCode::CycleDetected);
    }

    after = tickDeclarations_.Find(TickPointKey(dependent));
    before = tickDeclarations_.Find(TickPointKey(prerequisite));
    auto linked = after->prerequisites.TryPushBack(TickDependency{prerequisite, bit});
    if (!linked) {
        return linked;
    }

    auto back = before->dependents.TryPushBack(dependent);
    if (!back) {
        auto undone = after->prerequisites.TryPopBack();
        (void)undone;
        return back;
    }

    // 둘 다 이미 등록했으면 지금 스케줄러에 건다(아니면 늦게 등록하는 쪽이 건다).
    if (registered) {
        auto applied = ticks_.AddPrerequisite(afterTick, beforeTick);
        if (!applied) {
            // 선언 그래프가 스케줄러 그래프를 포함하므로 순환은 날 수 없다. 할당 실패면 되돌린다.
            after = tickDeclarations_.Find(TickPointKey(dependent));
            before = tickDeclarations_.Find(TickPointKey(prerequisite));
            auto undoneLink = after->prerequisites.TryPopBack();
            auto undoneBack = before->dependents.TryPopBack();
            (void)undoneLink;
            (void)undoneBack;
            return applied;
        }
    }

    return Result<void>::Success();
}

Result<void> World::RemoveTickPrerequisite(TickPoint dependent, TickPoint prerequisite,
                                           TickEdgeSource source) noexcept {
    if (!IsValidTickEdgeSource(source)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TickDeclarations* after = tickDeclarations_.Find(TickPointKey(dependent));
    if (after == nullptr || !(after->self == dependent)) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    const uint8 bit = static_cast<uint8>(source);
    for (usize index = 0; index < after->prerequisites.Size(); ++index) {
        TickDependency& existing = after->prerequisites[index];
        if (!(existing.point == prerequisite) || !HasSource(existing.sources, source)) {
            continue;
        }

        existing.sources = static_cast<uint8>(existing.sources & ~bit);
        if (existing.sources != 0) {
            return Result<void>::Success();
        }

        auto removed = after->prerequisites.TryRemoveAt(index);
        (void)removed;
        if (TickDeclarations* before = tickDeclarations_.Find(TickPointKey(prerequisite));
            before != nullptr && before->self == prerequisite) {
            for (usize at = 0; at < before->dependents.Size(); ++at) {
                if (before->dependents[at] == dependent) {
                    auto unlinked = before->dependents.TryRemoveAt(at);
                    (void)unlinked;
                    break;
                }
            }
        }

        const TickHandle afterTick = RegisteredTick(dependent);
        const TickHandle beforeTick = RegisteredTick(prerequisite);
        if (ticks_.IsRegistered(afterTick)) {
            // 선행이 이미 해제됐으면 스케줄러가 그 간선을 스스로 지운다(NotFound·StaleHandle 무시).
            auto detached = ticks_.RemovePrerequisite(afterTick, beforeTick);
            (void)detached;
        }

        return Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

void World::ApplyTickDeclarations(ObjectHandle object, uint8 hook,
                                  const TickHandle (&handles)[detail::TickHookCount]) noexcept {
    const TickPoint self{object, static_cast<TickHook>(hook)};
    TickDeclarations* node = tickDeclarations_.Find(TickPointKey(self));
    if (node == nullptr || !(node->self == self)) {
        return;
    }

    // 같은 객체의 다른 hook은 아직 레코드 사본에 기록되지 않았을 수 있다(Component는 모든 hook을
    // 등록한 뒤 사본을 맞춘다). 등록 중인 배열에서 찾는다.
    auto lookup = [&](const TickPoint& point) noexcept {
        return point.object == object ? handles[HookIndex(point.hook)] : RegisteredTick(point);
    };

    PruneTickDeclarations(*node);
    const TickHandle handle = handles[hook];
    // 실패(할당)는 순서 보장만 잃는다. 같은 의존을 다시 선언하면 다시 건다(AddTickPrerequisite).
    for (const TickDependency& dependency : node->prerequisites) {
        const TickHandle before = lookup(dependency.point);
        if (ticks_.IsRegistered(before)) {
            auto applied = ticks_.AddPrerequisite(handle, before);
            (void)applied;
        }
    }

    for (const TickPoint& dependent : node->dependents) {
        const TickHandle after = lookup(dependent);
        if (ticks_.IsRegistered(after)) {
            auto applied = ticks_.AddPrerequisite(after, handle);
            (void)applied;
        }
    }
}

void World::ForgetTickDeclarations(ObjectHandle object) noexcept {
    for (TickHook hook : AllTickHooks) {
        const TickPoint point{object, hook};
        TickDeclarations* node = tickDeclarations_.Find(TickPointKey(point));
        if (node == nullptr || !(node->self == point)) {
            continue;
        }

        // 상대 선언에 남은 이 객체의 참조를 지금 지운다(다시 건드리지 않는 선언에 죽은 참조가
        // 쌓이지 않게). 상대를 찾는 동안 이 선언은 재배치되지 않는다(Find만 한다).
        for (const TickDependency& dependency : node->prerequisites) {
            TickDeclarations* before = tickDeclarations_.Find(TickPointKey(dependency.point));
            if (before == nullptr || !(before->self == dependency.point)) {
                continue;
            }

            for (usize at = before->dependents.Size(); at > 0; --at) {
                if (before->dependents[at - 1] == point) {
                    auto removed = before->dependents.TryRemoveAt(at - 1);
                    (void)removed;
                }
            }
        }

        for (const TickPoint& dependent : node->dependents) {
            TickDeclarations* after = tickDeclarations_.Find(TickPointKey(dependent));
            if (after == nullptr || !(after->self == dependent)) {
                continue;
            }

            for (usize at = after->prerequisites.Size(); at > 0; --at) {
                if (after->prerequisites[at - 1].point == point) {
                    auto removed = after->prerequisites.TryRemoveAt(at - 1);
                    (void)removed;
                }
            }
        }

        tickDeclarations_.Erase(TickPointKey(point));
    }
}

void World::UpdateAttachmentTicks(ObjectHandle child, ObjectHandle oldParent,
                                  ObjectHandle newParent) noexcept {
    for (TickHook hook : AllTickHooks) {
        if (!oldParent.IsNull()) {
            auto detached = RemoveTickPrerequisite({child, hook}, {oldParent, hook},
                                                   TickEdgeSource::Attachment);
            (void)detached;
        }

        // 어느 한쪽이 그 hook을 받지 않으면 InvalidArgument이며 간선이 없다(UE: 둘 다 tick 가능할
        // 때만).
        if (!newParent.IsNull()) {
            auto attached =
                AddTickPrerequisite({child, hook}, {newParent, hook}, TickEdgeSource::Attachment);
            (void)attached;
        }
    }
}

// --- 실행 중 설정 변경 ---

Result<void> World::SetTickPhase(ObjectHandle object, TickPhase phase) noexcept {
    auto settings = ReadTickSettings(object);
    if (!settings) {
        return Result<void>::Failure(settings.Error());
    }

    if (IsTickTargetDestroying(object)) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    TickSettings changed = settings.Value();
    changed.phase = phase;
    if (!changed.tick || !IsValidTickSettings(changed)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const TickHandle handle = RegisteredTick({object, TickHook::Update});
    if (ticks_.IsRegistered(handle)) {
        auto applied = ticks_.SetPhase(handle, phase);
        if (!applied) {
            return applied;
        }
    }

    return WriteTickSettings(object, changed);
}

Result<void> World::SetTickPriority(ObjectHandle object, int32 priority) noexcept {
    auto settings = ReadTickSettings(object);
    if (!settings) {
        return Result<void>::Failure(settings.Error());
    }

    if (IsTickTargetDestroying(object)) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    TickSettings changed = settings.Value();
    changed.priority = priority;
    for (TickHook hook : AllTickHooks) {
        const TickHandle handle = RegisteredTick({object, hook});
        if (ticks_.IsRegistered(handle)) {
            auto applied = ticks_.SetPriority(handle, priority);
            (void)applied;
        }
    }

    return WriteTickSettings(object, changed);
}

Result<void> World::SetTickInterval(ObjectHandle object, double intervalSeconds) noexcept {
    if (!math::IsFinite(intervalSeconds) || intervalSeconds < 0.0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto settings = ReadTickSettings(object);
    if (!settings) {
        return Result<void>::Failure(settings.Error());
    }

    if (IsTickTargetDestroying(object)) {
        return Result<void>::Failure(ErrorCode::Destroyed);
    }

    TickSettings changed = settings.Value();
    changed.intervalSeconds = intervalSeconds;
    for (TickHook hook : AllTickHooks) {
        const TickHandle handle = RegisteredTick({object, hook});
        if (ticks_.IsRegistered(handle)) {
            auto applied = ticks_.SetInterval(handle, intervalSeconds);
            (void)applied;
        }
    }

    return WriteTickSettings(object, changed);
}

Result<TickPhase> World::GetTickEffectivePhase(TickPoint point) const noexcept {
    const TickHandle handle = RegisteredTick(point);
    if (!ticks_.IsRegistered(handle)) {
        return Result<TickPhase>::Failure(ErrorCode::NotFound);
    }

    return ticks_.EffectivePhase(handle);
}

// --- 엔진 단계 자리 ---

Result<void> World::SetEngineStage(EngineStage stage, EngineStageFunction function,
                                   void* user) noexcept {
    const uint32 index = static_cast<uint32>(stage);
    if (index >= EngineStageCount || function == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (engineStages_[index].function != nullptr) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    engineStages_[index] = {function, user};
    return Result<void>::Success();
}

void World::ClearEngineStage(EngineStage stage) noexcept {
    const uint32 index = static_cast<uint32>(stage);
    if (index < EngineStageCount) {
        engineStages_[index] = {};
    }
}

void World::RunTickableSubsystems(const TickPhaseFrame& frame) noexcept {
    TickContext context;
    context.phase = TickPhase::LateUpdate;
    context.unscaledDeltaSeconds = frame.unscaledDeltaSeconds;
    context.frameIndex = frame.frameIndex;
    context.paused = frame.paused;
    for (WorldSubsystem* subsystem : tickables_) {
        // pause 중에는 TickWhenPaused인 것만 unscaled delta로(TickScheduler pause 정책과 같다).
        if (frame.paused && !subsystem->TickWhenPaused()) {
            continue;
        }

        context.deltaSeconds = frame.paused ? frame.unscaledDeltaSeconds : frame.deltaSeconds;
        subsystem->OnTick(*this, context);
    }
}

Result<void> World::RunEngineStage(EngineStage stage, TickPhase phase,
                                   const TickPhaseFrame& frame) noexcept {
    const EngineStageSlot slot = engineStages_[static_cast<uint32>(stage)];
    if (slot.function == nullptr) {
        return Result<void>::Success();
    }

    TickContext context;
    context.phase = phase;
    context.deltaSeconds = frame.deltaSeconds;
    context.unscaledDeltaSeconds = frame.unscaledDeltaSeconds;
    context.frameIndex = frame.frameIndex;
    context.paused = frame.paused;
    auto ran = slot.function(*this, context, slot.user);
    if (!ran) {
        ++engineStageFailures_;
    }

    return ran;
}

} // namespace rocks
