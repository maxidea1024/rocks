#include <Base/InitializerList.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Clock.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <World/WorldLog.h>

namespace rocks {

namespace {

// hook별 실행 단계다. OnTick은 설정한 단계, OnFixedTick은 Physics, OnLateTick은 LateUpdate다.
TickPhase HookPhase(uint8 hook, const TickSettings& settings) noexcept {
    switch (hook) {
    case detail::TickHookFixed:
        return TickPhase::Physics;
    case detail::TickHookLate:
        return TickPhase::LateUpdate;
    default:
        return settings.phase;
    }
}

bool HookWanted(uint8 hook, const TickSettings& settings) noexcept {
    switch (hook) {
    case detail::TickHookFixed:
        return settings.fixedTick;
    case detail::TickHookLate:
        return settings.lateTick;
    default:
        return settings.tick;
    }
}

} // namespace

// --- Tick 등록 ---

void World::RegisterTicks(ObjectHandle object, uint8 kind, const TickSettings& settings,
                          bool enabled, TickHandle (&handles)[detail::TickHookCount]) noexcept {
    for (uint8 hook = 0; hook < detail::TickHookCount; ++hook) {
        if (!handles[hook].IsNull() || !HookWanted(hook, settings)) {
            continue;
        }

        TickRegistration registration;
        registration.target = {object, kind, hook};
        registration.phase = HookPhase(hook, settings);
        registration.priority = settings.priority;
        registration.intervalSeconds = settings.intervalSeconds;
        registration.tickWhenPaused = settings.tickWhenPaused;
        registration.enabled = enabled;
        // 설정은 FinishSpawning·AddComponent에서 검증했다. 할당 실패만 남으며 그 hook은 Tick 없이
        // 동작한다.
        auto registered = ticks_.Register(registration);
        if (registered) {
            handles[hook] = registered.Value();
            // 시작 전에 선언했거나 상대가 먼저 등록하며 남긴 의존을 건다(15.3절 ERD5).
            ApplyTickDeclarations(object, hook, handles);
        }
    }
}

void World::UnregisterTicks(TickHandle (&handles)[detail::TickHookCount]) noexcept {
    for (TickHandle& handle : handles) {
        if (!handle.IsNull()) {
            auto removed = ticks_.Unregister(handle);
            (void)removed;
            handle = {};
        }
    }
}

void World::SetTicksEnabled(TickHandle (&handles)[detail::TickHookCount], bool enabled) noexcept {
    for (TickHandle& handle : handles) {
        if (!handle.IsNull()) {
            auto changed = ticks_.SetEnabled(handle, enabled);
            (void)changed;
        }
    }
}

void World::RegisterComponentTicks(ObjectHandle component) noexcept {
    TickSettings settings;
    bool enabled = false;
    ObjectHandle owner;
    TickHandle handles[detail::TickHookCount];
    auto read = With<Component>(component, [&](Component& value) noexcept {
        settings = value.tickSettings_;
        enabled = value.enabledApplied_ && value.tickEnabled_;
        owner = value.owner_;
        for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
            handles[hook] = value.ticks_[hook];
        }

        return value.state_ == ComponentState::Playing;
    });
    const ActorRecord* record = FindActorRecord(owner);
    if (!read || !read.Value() || record == nullptr || record->state != ActorState::Playing) {
        return;
    }

    // 소유 Actor 뒤에 두는 자동 간선은 없다(UE에 Actor → Component 기본 의존이 없다 — 15.3절 ERD1).
    // Actor Tick이 먼저 등록되므로 같은 priority면 등록 순서로 Actor가 먼저다.
    RegisterTicks(component, detail::TickKindComponent, settings, enabled, handles);

    auto stored = With<Component>(component, [&](Component& value) noexcept {
        for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
            value.ticks_[hook] = handles[hook];
        }
    });
    if (!stored) {
        UnregisterTicks(handles);
        return;
    }

    // Component 색인의 사본도 맞춘다(우회 파괴된 Component의 등록을 정리 단계가 해제한다).
    if (ComponentRecord* entry = componentRecords_.Find(component.slot);
        entry != nullptr && entry->component == component) {
        for (uint32 hook = 0; hook < detail::TickHookCount; ++hook) {
            entry->ticks[hook] = handles[hook];
        }
    }
}

// --- 프레임 ---

void World::ExecuteTick(const TickTarget& target, const TickContext& context) noexcept {
    if (target.kind == detail::TickKindActor) {
        const ActorRecord* record = FindActorRecord(target.object);
        if (record == nullptr || record->state != ActorState::Playing || !record->enabledApplied) {
            ++skippedTicks_;
            return;
        }

        auto called = With<Actor>(target.object, [&](Actor& actor) noexcept {
            switch (target.hook) {
            case detail::TickHookFixed:
                actor.OnFixedTick(context);
                break;
            case detail::TickHookLate:
                actor.OnLateTick(context);
                break;
            default:
                actor.OnTick(context);
                break;
            }
        });
        if (!called) {
            // Registry를 직접 조작해 우회 파괴된 Actor다. 레코드를 정리 대기로 돌린다(hook은 받지
            // 못한다).
            ++skippedTicks_;
            ++repairedBypasses_;
            auto removed = ticks_.Unregister(context.handle);
            (void)removed;
            auto destroyed = DestroyActor(target.object);
            (void)destroyed;
        }

        return;
    }

    auto called = With<Component>(target.object, [&](Component& component) noexcept {
        // 파괴 대기·비활성 Component와 소유 Actor가 활성 Playing이 아닌 경우는 건너뛴다.
        const ActorRecord* owner = FindActorRecord(component.owner_);
        if (component.state_ != ComponentState::Playing || !component.enabledApplied_ ||
            owner == nullptr || owner->state != ActorState::Playing || !owner->enabledApplied) {
            return false;
        }

        switch (target.hook) {
        case detail::TickHookFixed:
            component.OnFixedTick(context);
            break;
        case detail::TickHookLate:
            component.OnLateTick(context);
            break;
        default:
            component.OnTick(context);
            break;
        }

        return true;
    });
    if (!called) {
        // 우회 파괴된 Component다. 등록을 해제하고 소유 Actor의 간선 정리를 예약한다(복구와 같은
        // 큐 — 한 번만 넣는다).
        auto removed = ticks_.Unregister(context.handle);
        (void)removed;
        if (QueueBypassedComponent(target.object)) {
            ++repairedBypasses_;
        }
    }

    if (!called || !called.Value()) {
        ++skippedTicks_;
    }
}

Result<void> World::Tick(double realDeltaSeconds) noexcept {
    if (inTick_ || hookDepth_ != 0) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!CanMutate()) {
        return Result<void>::Failure(state_ == WorldState::Created ? ErrorCode::InvalidArgument
                                                                   : ErrorCode::ShuttingDown);
    }

    auto plan = clock_.Advance(realDeltaSeconds);
    if (!plan) {
        return Result<void>::Failure(plan.Error());
    }

    const uint64 start = platform::MonotonicNanoseconds();
    inTick_ = true;
    ObjectAccessScope scope(Access());
    HookScope hooks(*this);
    lastStep_ = plan.Value();
    // GC 주기 간격은 실제 경과 시간으로 센다(일시정지·시간 배율과 무관 — GC 설계 17.3절).
    lastRealDeltaSeconds_ = realDeltaSeconds;
    ticks_.BeginFrame();
    skippedTicks_ = 0;
    for (uint64& value : phaseNanoseconds_) {
        value = 0;
    }

    ProcessStarts();

    Result<void> outcome = Result<void>::Success();
    TickPhaseFrame frame;
    frame.deltaSeconds = lastStep_.deltaSeconds;
    frame.unscaledDeltaSeconds = lastStep_.unscaledDeltaSeconds;
    frame.frameIndex = lastStep_.frameIndex;
    // 디버그 단일 스텝 프레임은 pause가 아닌 것처럼 실행한다(일반 게임플레이 Tick도 돈다).
    frame.paused = clock_.IsPaused() && !lastStep_.singleStep;
    for (uint32 phaseIndex = 0; phaseIndex < TickPhaseCount; ++phaseIndex) {
        const TickPhase phase = static_cast<TickPhase>(phaseIndex);
        const uint64 phaseStart = platform::MonotonicNanoseconds();
        if (phase == TickPhase::Physics) {
            TickPhaseFrame fixed = frame;
            fixed.deltaSeconds = lastStep_.fixedStepSeconds;
            fixed.unscaledDeltaSeconds = lastStep_.fixedStepSeconds;
            for (uint32 step = 0; step < lastStep_.fixedSteps; ++step) {
                fixed.fixedStepIndex = lastStep_.firstFixedStepIndex + step;
                auto ran = ticks_.RunPhase(phase, fixed, *this);
                if (!ran && outcome) {
                    outcome = Move(ran);
                }
            }

            // Physics 끝 엔진 단계: 고정 스텝 수와 무관하게 프레임에 한 번(15.4절).
            for (EngineStage stage :
                 {EngineStage::PhysicsResults, EngineStage::SkeletalPhysicsFinalize}) {
                auto ran = RunEngineStage(stage, TickPhase::Physics, frame);
                if (!ran && outcome) {
                    outcome = Move(ran);
                }
            }
        } else {
            // LateUpdate 시작 엔진 단계: 타이머 → tickable Subsystem → 카메라(UE LevelTick 순서).
            // pause 중 카메라는 갱신하지 않는다(UE ShouldPerformFullTickWhenPaused 기본 거짓).
            if (phase == TickPhase::LateUpdate) {
                for (EngineStage stage : {EngineStage::Timers, EngineStage::TickableSubsystems,
                                          EngineStage::CameraUpdate}) {
                    if (stage == EngineStage::CameraUpdate && frame.paused) {
                        continue;
                    }

                    // Timers 자리는 World의 LifeSpan 만료를, TickableSubsystems 자리는 tickable
                    // Subsystem을 먼저 처리하고 그다음 등록된 콜백을 부른다(16.5·17.5절).
                    if (stage == EngineStage::Timers) {
                        ExpireLifeSpans();
                    }

                    if (stage == EngineStage::TickableSubsystems) {
                        RunTickableSubsystems(frame);
                    }

                    auto ran = RunEngineStage(stage, TickPhase::LateUpdate, frame);
                    if (!ran && outcome) {
                        outcome = Move(ran);
                    }
                }
            }

            auto ran = ticks_.RunPhase(phase, frame, *this);
            if (!ran && outcome) {
                outcome = Move(ran);
            }
        }

        phaseNanoseconds_[phaseIndex] += platform::MonotonicNanoseconds() - phaseStart;
    }

    FlushDestroys();
    StepGarbageCollection();
    inTick_ = false;
    tickNanoseconds_ = platform::MonotonicNanoseconds() - start;
    return outcome;
}

} // namespace rocks
