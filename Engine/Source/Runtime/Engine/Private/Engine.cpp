#include <Base/Compiler.h>
#include <Config/ConfigContext.h>
#include <Core/Assert.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogContext.h>
#include <Core/ScopeExit.h>
#include <Engine/Engine.h>
#include <Engine/EngineLog.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <Platform/Thread.h>

namespace rocks {

namespace {

// 초기화 단계 번호다. Teardown은 완료한 단계를 역순으로 정리한다.
constexpr uint32 StepTypes = 1;
constexpr uint32 StepTypesRegistered = 2;
constexpr uint32 StepObjects = 3;
constexpr uint32 StepSubsystems = 4;
constexpr uint32 StepClient = 5;

// World 실행 문맥 ID의 상위 값이다. 엔진 Registry 문맥(EngineConfig::context)과 겹치지 않는다.
constexpr uint64 WorldContextHigh = 0x524f434b53574f52ull;

// 종료 hook 진행 정도다(Engine::shutdownProgress_).
constexpr uint32 ShutdownNotStarted = 0;
constexpr uint32 ShutdownBlocked = 1;
constexpr uint32 ShutdownWorkCompleted = 2;

// 초기화 단계다(Docs/Design/LoggingSystem.ko.md 13절). 실패 로그의 {Stage}와 단계마다 거는 "Stage"
// 로그 문맥이 이 표에서 온다. 단계를 더할 때는(예: GC 영구 창 닫기 PermanentRegion — Subsystems
// 바로 뒤) 열거자와 InitStageNames의 같은 자리에 함께 넣는다.
enum class InitStage : uint8 {
    TypeRegistry,
    ModuleRegistration,
    RegisterTypes,
    ObjectRegistry,
    GcScheduler,
    Subsystems,
    PermanentRegion,
    ClientStart,
    PostEngineInit,
    Count
};

constexpr const char* InitStageNames[] = {
    "TypeRegistry", "ModuleRegistration", "RegisterTypes", "ObjectRegistry", "GcScheduler",
    "Subsystems",   "PermanentRegion",    "ClientStart",   "PostEngineInit",
};

static_assert(sizeof(InitStageNames) / sizeof(InitStageNames[0]) ==
                  static_cast<usize>(InitStage::Count),
              "every InitStage needs a name in InitStageNames");

const char* InitStageName(InitStage stage) noexcept {
    return InitStageNames[static_cast<usize>(stage)];
}

// 초기화 단계 하나 동안 "Stage" 로그 문맥을 건다.
class InitStageContext {
public:
    explicit InitStageContext(InitStage stage) noexcept
        : scope_("Stage", StringView(InitStageName(stage))) {
    }

private:
    LogContextScope scope_;
};

// 실패를 하나 모은다. 첫 오류와 그 단계 이름만 남긴다.
void KeepFirst(Result<void>& outcome, const char*& failedStage, Result<void>&& step,
               const char* stage) noexcept {
    if (!step && outcome) {
        outcome = Move(step);
        failedStage = stage;
    }
}

// 엔진 Registry는 서브시스템 초기화 동안 영구 창을 연다(Docs/Design/GarbageCollectionScale.ko.md
// 13.7절).
RegistryOptions EngineRegistryOptions() noexcept {
    RegistryOptions options;
    options.permanentRegion = true;
    return options;
}

} // namespace

const char* EngineStateName(EngineState state) noexcept {
    switch (state) {
    case EngineState::Uninitialized:
        return "Uninitialized";
    case EngineState::Initializing:
        return "Initializing";
    case EngineState::Running:
        return "Running";
    case EngineState::Stopping:
        return "Stopping";
    case EngineState::Stopped:
        return "Stopped";
    case EngineState::Failed:
        return "Failed";
    }

    return "Unknown";
}

namespace {

// 초기화 동안 드라이버·외부 라이브러리가 처리되지 않은 예외 filter를 바꿨는지 본다(설계 8.6절).
// 바꿨으면 그 filter를 두고(교환 확인이 되돌린다) 외부 스레드 크래시가 보고되지 않는다고 알린다.
void WarnIfCrashFilterReplaced() noexcept {
    if (!platform::CheckUnhandledCrashFilter()) {
        Log::Warning(LogEngine,
                     "Another component replaced the unhandled exception filter; crashes "
                     "on threads outside Rocks may not be reported");
    }
}

} // namespace

void Engine::RequestExit() noexcept {
    if constexpr (!IsReleaseBuild) {
        // 소유 스레드 규약(설계 20절). Initialize 전에는 소유자가 없다.
        ROCKS_ASSERT(ownerThread_ == 0 || ownerThread_ == platform::CurrentThreadId());
    }

    exitRequested_ = true;
}

Engine::Engine(Allocator& allocator) noexcept
    : allocator_(allocator), subsystems_(allocator), worlds_(allocator) {
}

Engine::~Engine() {
    auto finished = Shutdown();
    (void)finished;
    // 종료 hook의 실패는 보고만 하고 정리를 마친다. 끝내지 못한 경우(World·객체를 밖에서 붙잡음)만
    // 수명 오류다.
    if (state_ != EngineState::Stopped && state_ != EngineState::Failed &&
        state_ != EngineState::Uninitialized) {
        Log::Fatal(
            LogEngine,
            "Engine destroyed in state {State} while worlds or objects are still held outside",
            EngineStateName(state_));
    }
}

ObjectRegistry& Engine::Objects() noexcept {
    if (!objects_) {
        Log::Fatal(LogEngine, "Engine::Objects called before initialization");
    }

    return *objects_;
}

Result<void> Engine::Initialize(const EngineConfig& config) noexcept {
    // 호출 순서 오류는 호출자가 다룬다(로그를 남기지 않는다).
    if (state_ != EngineState::Uninitialized) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 started = platform::MonotonicNanoseconds();
    state_ = EngineState::Initializing;
    config_ = config;
    ownerThread_ = platform::CurrentThreadId();
    // 크래시 기록의 단계 이름이다(설계 LC6). 프레임마다가 아니라 단계가 바뀔 때만 쓴다.
    platform::SetRunPhase(platform::RunPhase::Startup, EngineStageInitialize);
    // 실패를 확정하는 곳이 여기라 한 번만 기록한다(호스트는 다시 기록하지 않는다).
    auto fail = [this](InitStage stage, ErrorCode error) noexcept {
        Log::Error(LogEngine, "Engine initialization failed at {Stage}: {Error}",
                   InitStageName(stage), ErrorCodeName(error));
        // 초기화 실패의 되돌리기는 정리 단계이고, 끝나면 정리 끝이다(설계 LC6 리뷰).
        platform::SetRunPhase(platform::RunPhase::Shutdown, EngineStageShutdown);
        Teardown(initializedSteps_, InitStageName(stage));
        platform::SetRunPhase(platform::RunPhase::Shutdown, EngineStageStopped);
        state_ = EngineState::Failed;
        return Result<void>::Failure(error);
    };

    {
        InitStageContext stage(InitStage::TypeRegistry);
        auto types = UniquePtr<TypeRegistry>::TryMake(allocator_, allocator_);
        if (!types) {
            return fail(InitStage::TypeRegistry, types.Error());
        }

        types_ = Move(types.Value());
        initializedSteps_ = StepTypes;
    }

    {
        InitStageContext stage(InitStage::ModuleRegistration);
        auto modules = UniquePtr<ModuleRegistration>::TryMake(allocator_, *types_, allocator_);
        if (!modules) {
            return fail(InitStage::ModuleRegistration, modules.Error());
        }

        modules_ = Move(modules.Value());
    }

    {
        // 모듈 타입(로더의 타입 등록기 — World 등) -> 게임 리플렉션 모듈(엔진 ModuleRegistration)
        // -> native 타입 순서다. native 파생은 리플렉션 base가 등록된 뒤에만 등록된다.
        InitStageContext stage(InitStage::RegisterTypes);
        auto registered = AttachModuleTypes();
        if (registered && config_.registerModuleTypes != nullptr) {
            registered = config_.registerModuleTypes(*modules_);
        }

        if (registered && config_.registerTypes != nullptr) {
            registered = config_.registerTypes(*types_);
        }

        if (!registered) {
            return fail(InitStage::RegisterTypes, registered.Error());
        }

        initializedSteps_ = StepTypesRegistered;
    }

    {
        InitStageContext stage(InitStage::ObjectRegistry);
        // 서브시스템 초기화 동안 만든 객체는 영구 영역에 둔다(GC 표식·sweep 비용 없음 — GC 설계
        // 13.7절, Docs/Design/GarbageCollectionScale.ko.md). 초기화 뒤 닫는다.
        auto objects = UniquePtr<ObjectRegistry>::TryMake(allocator_, allocator_, *types_,
                                                          config_.context, EngineRegistryOptions());
        if (!objects) {
            return fail(InitStage::ObjectRegistry, objects.Error());
        }

        objects_ = Move(objects.Value());
        initializedSteps_ = StepObjects;
    }

    {
        // World GC 표식 scheduler다(GC 설계 17.4절). World를 만들기 전에 시작하고 World를 모두
        // 파괴한 뒤 끝낸다.
        InitStageContext stage(InitStage::GcScheduler);
        uint32 workers = config_.gcWorkers;
        if (workers != EngineConfig::GcWorkersAuto && workers > EngineConfig::MaxGcWorkers) {
            return fail(InitStage::GcScheduler, ErrorCode::InvalidArgument);
        }

        if (workers == EngineConfig::GcWorkersAuto) {
            const uint32 processors = platform::LogicalProcessorCount();
            workers = processors > 1 ? processors - 1 : 0;
            workers =
                workers < EngineConfig::MaxAutoGcWorkers ? workers : EngineConfig::MaxAutoGcWorkers;
        }

        if (workers != 0) {
            auto scheduler = UniquePtr<WorkerScheduler>::TryMake(allocator_, allocator_);
            if (!scheduler) {
                return fail(InitStage::GcScheduler, scheduler.Error());
            }

            WorkerSchedulerConfig schedulerConfig;
            schedulerConfig.workerCount = workers;
            if (auto started = scheduler.Value()->Start(schedulerConfig); !started) {
                return fail(InitStage::GcScheduler, started.Error());
            }

            gcScheduler_ = Move(scheduler.Value());
        }
    }

    {
        InitStageContext stage(InitStage::Subsystems);
        Result<void> subsystems = Result<void>::Success();
        {
            ObjectAccessScope scope(objects_->OwnerAccess());
            subsystems = subsystems_.Initialize(config_.subsystems, *this);
        }

        if (!subsystems) {
            return fail(InitStage::Subsystems, subsystems.Error());
        }

        for (const SkippedSubsystem& skipped : subsystems_.Skipped()) {
            if (skipped.reason == SubsystemSkipReason::MissingDependency) {
                Log::Warning(
                    LogEngine,
                    "Skipped engine subsystem {Subsystem}: dependency {Dependency} was not "
                    "created",
                    skipped.name, skipped.missing);
            }
        }

        initializedSteps_ = StepSubsystems;
    }

    {
        // 영구 창은 서브시스템 초기화까지만 연다(13.7절). 닫지 못하면 초기화가 실패한다.
        InitStageContext stage(InitStage::PermanentRegion);
        if (auto closed = objects_->ClosePermanentRegion(objects_->OwnerAccess()); !closed) {
            return fail(InitStage::PermanentRegion, closed.Error());
        }
    }

    {
        InitStageContext stage(InitStage::ClientStart);
        if (config_.client != nullptr) {
            Result<void> clientStarted = Result<void>::Success();
            {
                ObjectAccessScope scope(objects_->OwnerAccess());
                clientStarted = config_.client->OnEngineStart(*this);
            }

            if (!clientStarted) {
                return fail(InitStage::ClientStart, clientStarted.Error());
            }
        }

        initializedSteps_ = StepClient;
    }

    {
        // 클라이언트 시작 뒤 첫 프레임 전 단계다(UE PostEngineInit). 실패하면 시작한 클라이언트를
        // 먼저 멈춘 뒤 초기화 실패 정리를 한다.
        InitStageContext stage(InitStage::PostEngineInit);
        if (auto started = StartPostEngineInitModules(); !started) {
            if (config_.client != nullptr) {
                ObjectAccessScope scope(objects_->OwnerAccess());
                config_.client->OnEngineShutdown(*this);
            }

            return fail(InitStage::PostEngineInit, started.Error());
        }
    }

    state_ = EngineState::Running;
    platform::SetRunPhase(platform::RunPhase::Running, EngineStageReady);
    WarnIfCrashFilterReplaced();
    lastClock_ = platform::MonotonicNanoseconds();
    Log::Info(LogEngine, "Engine initialized with {SubsystemCount} subsystems in {Ms:.2f} ms",
              static_cast<uint32>(subsystems_.Count()),
              static_cast<double>(lastClock_ - started) * 1.0e-6);
    return Result<void>::Success();
}

void Engine::Teardown(uint32 completedSteps, const char* stage) noexcept {
    // 초기화 실패 경로다. World·client는 아직 없거나 시작하지 않았다. 남은 단계를 역순으로
    // 정리한다. Subsystem 초기화 도중 실패했어도 앞서 성공한 Subsystem은 Initialize가 이미
    // 정리했다.
    if (completedSteps >= StepSubsystems && objects_) {
        ObjectAccessScope scope(objects_->OwnerAccess());
        subsystems_.Deinitialize(*this);
    }

    // World는 아직 없다(초기화 실패 경로). scheduler를 끝내지 못하는 것은 worker가 남은 작업을
    // 돌려주지 않는 불변식 위반이다.
    if (auto stopped = StopGcScheduler(); !stopped) {
        Log::Fatal(LogEngine,
                   "Engine GC scheduler could not shut down after initialization failure at "
                   "{Stage}: {Error}",
                   stage, ErrorCodeName(stopped.Error()));
    }

    if (completedSteps >= StepObjects && objects_) {
        auto collected = objects_->Collect(objects_->OwnerAccess());
        (void)collected;
        auto closed = objects_->Shutdown(objects_->OwnerAccess());
        if (!closed) {
            Log::Fatal(LogEngine,
                       "Engine {Registry} could not shut down after initialization failure at "
                       "{Stage}: {Error}",
                       "ObjectRegistry", stage, ErrorCodeName(closed.Error()));
        }

        objects_.Reset();
    }

    // 타입은 Unregister하지 않는다(Shutdown 설명). 붙잡은 lease를 놓고 모듈 타입 등록기를 뗀 뒤
    // TypeRegistry가 등록 기록(등록 함수가 버린 native 토큰 포함)을 dependent 순서로 반납한다.
    modules_.Reset();
    DetachModuleTypes();
    if (completedSteps >= StepTypes && types_) {
        auto closed = types_->Shutdown();
        if (!closed) {
            Log::Fatal(LogEngine,
                       "Engine {Registry} could not shut down after initialization failure at "
                       "{Stage}: {Error}",
                       "TypeRegistry", stage, ErrorCodeName(closed.Error()));
        }

        types_.Reset();
    }

    initializedSteps_ = 0;
}

OuterSubsystemLookup Engine::SubsystemLookup() const noexcept {
    OuterSubsystemLookup lookup;
    lookup.context = this;
    lookup.find = [](const void* context, SubsystemKey key) noexcept -> void* {
        // 조회 창은 EngineSubsystem* 를 void*로 넘긴다(GetEngineSubsystem이 되돌린다).
        return static_cast<const Engine*>(context)->FindSubsystem(key);
    };
    return lookup;
}

Result<World*> Engine::CreateWorld(const WorldCreateDesc& desc) noexcept {
    if (state_ != EngineState::Running) {
        return Result<World*>::Failure(ErrorCode::InvalidArgument);
    }

    auto reserved = worlds_.TryEnsureCapacity(worlds_.Size() + 1);
    if (!reserved) {
        return Result<World*>::Failure(reserved.Error());
    }

    WorldDesc worldDesc;
    worldDesc.name = desc.name;
    worldDesc.context = {WorldContextHigh, nextWorldContext_++};
    worldDesc.timeStep = desc.timeStep;
    worldDesc.objects = desc.objects;
    worldDesc.gcBudget = desc.gcBudget;
    worldDesc.gcSchedule = desc.gcSchedule;
    worldDesc.gcScheduler = desc.gcScheduler != nullptr ? desc.gcScheduler : gcScheduler_.Get();
    worldDesc.subsystems = desc.subsystems;
    worldDesc.kind = desc.kind;
    worldDesc.outerSubsystems =
        desc.outerSubsystems.find != nullptr ? desc.outerSubsystems : SubsystemLookup();
    // World가 엔진 설정 문맥에 자기 CVar 범위를 만든다(CVarSystem 14.2절).
    worldDesc.settings = config_.settings;
    auto created = UniquePtr<World>::TryMake(allocator_, allocator_, *types_, worldDesc);
    if (!created) {
        return Result<World*>::Failure(created.Error());
    }

    UniquePtr<World> world = Move(created.Value());
    Result<void> initialized = Result<void>::Success();
    {
        LogContextScope scope("World", world->Name());
        const char* stage = "Initialize";
        initialized = world->Initialize();
        if (initialized && desc.beginPlay) {
            stage = "BeginPlay";
            initialized = world->BeginPlay();
        }

        if (!initialized) {
            // 상태 오류(입력 검사)는 위에서 호출자에게 돌려줬다. 여기 실패는 World 내부 단계다.
            Log::Error(LogEngine, "World {World} failed during {Stage}: {Error}", world->Name(),
                       stage, ErrorCodeName(initialized.Error()));
        }
    }

    if (!initialized) {
        auto finished = world->Shutdown();
        (void)finished;
        return Result<World*>::Failure(initialized.Error());
    }

    World* pointer = world.Get();
    auto added = worlds_.TryEmplaceBack();
    // 용량을 위에서 확보했다.
    added.Value()->world = Move(world);
    return Result<World*>::Success(pointer);
}

Result<void> Engine::DestroyWorldNow(usize index) noexcept {
    WorldContext& context = worlds_[index];
    auto finished = context.world->Shutdown();
    if (!finished) {
        // 외부가 Borrow·RootRef를 보유하고 있다. 지연 작업에서 다시 시도한다.
        context.pendingDestroy = true;
        context.destroyError = finished.Error();
        if (!context.destroyDeferred) {
            context.destroyDeferred = true;
            context.destroyDeferredFrame = time_.frameIndex;
            Log::Debug(LogEngine, "World {World} destroy deferred: {Error}; retrying each frame",
                       context.world->Name(), ErrorCodeName(finished.Error()));
        }

        return finished;
    }

    if (context.tickFailing) {
        EndTickFailureStreak(context);
    }

    if (context.destroyDeferred) {
        Log::Info(LogEngine, "World {World} destroyed after {Frames} deferred frames",
                  context.world->Name(), time_.frameIndex - context.destroyDeferredFrame);
    }

    UniquePtr<World> world = Move(context.world);
    auto removed = worlds_.TryRemoveAt(index);
    (void)removed;
    const bool trim = world->GcSchedule().trimMemory;
    world.Reset();
    // World 하나의 객체·저장소가 모두 반환됐다: 쥐고만 있는 메모리를 OS에 돌려준다(UE가 맵을 내린
    // 뒤 메모리를 줄이는 것에 대응 — GC 설계 18.11.6절). 그 World의 일정 설정으로 끈다.
    if (trim) {
        allocator_.TrimMemory();
    }

    return Result<void>::Success();
}

Result<void> Engine::DestroyWorld(World* world) noexcept {
    for (usize index = 0; index < worlds_.Size(); ++index) {
        if (worlds_[index].world.Get() != world) {
            continue;
        }

        if (inFrame_) {
            worlds_[index].pendingDestroy = true;
            return Result<void>::Success();
        }

        return DestroyWorldNow(index);
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

void Engine::ProcessDeferred() noexcept {
    for (usize index = worlds_.Size(); index > 0; --index) {
        if (!worlds_[index - 1].pendingDestroy) {
            continue;
        }

        auto destroyed = DestroyWorldNow(index - 1);
        if (destroyed) {
            continue;
        }

        // 오래 남으면 사람이 봐야 한다(외부가 보유권을 놓지 않음). 한 번만 경고한다.
        WorldContext& context = worlds_[index - 1];
        const uint64 frames = time_.frameIndex - context.destroyDeferredFrame;
        if (!context.destroyWarned && frames > WorldDestroyWarnFrames) {
            context.destroyWarned = true;
            Log::Warning(LogEngine,
                         "World {World} destroy still deferred after {Frames} frames: {Error}; "
                         "something outside still holds its objects",
                         context.world->Name(), frames, ErrorCodeName(context.destroyError));
        }
    }
}

void Engine::NoteWorldTick(WorldContext& context, const Result<void>& ticked) noexcept {
    if (ticked) {
        if (context.tickFailing) {
            EndTickFailureStreak(context);
        }

        return;
    }

    context.lastTickError = ticked.Error();
    if (context.tickFailing) {
        ++context.suppressedTickFailures;
        return;
    }

    context.tickFailing = true;
    context.suppressedTickFailures = 0;
    Log::Error(LogEngine, "World {World} tick failed: {Error}", context.world->Name(),
               ErrorCodeName(ticked.Error()));
}

void Engine::EndTickFailureStreak(WorldContext& context) noexcept {
    if (context.suppressedTickFailures != 0) {
        Log::Error(LogEngine,
                   "World {World} tick failed {Count} more times after the first failure "
                   "(last: {Error})",
                   context.world->Name(), context.suppressedTickFailures,
                   ErrorCodeName(context.lastTickError));
    }

    context.tickFailing = false;
    context.suppressedTickFailures = 0;
}

Result<void> Engine::RunFrame() noexcept {
    if (state_ != EngineState::Running) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (inFrame_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    const uint64 frameStart = platform::MonotonicNanoseconds();
    inFrame_ = true;
    frameRunning_ = true;
    // hook은 Registry scope 없이 부른다(World 객체는 World::With가 자기 scope를 세운다). 엔진
    // Registry를 다루는 Subsystem·client 호출 구간에만 엔진 scope를 세운다.

    // 1) 이벤트 펌프.
    if (config_.hooks != nullptr) {
        config_.hooks->PumpEvents(*this);
    }

    // 2) 시간 갱신.
    const uint64 now = platform::MonotonicNanoseconds();
    const double measured = static_cast<double>(now - lastClock_) * 1.0e-9;
    lastClock_ = now;
    time_.deltaSeconds =
        config_.fixedFrameDeltaSeconds > 0.0 ? config_.fixedFrameDeltaSeconds : measured;
    ++time_.frameIndex;
    Log::SetFrame(time_.frameIndex);
    time_.realTimeSeconds += time_.deltaSeconds;
    time_.averageFrameSeconds = time_.frameIndex == 1
                                    ? time_.deltaSeconds
                                    : time_.averageFrameSeconds * 0.9 + time_.deltaSeconds * 0.1;

    // 2-1) 설정 sync 지점(ConfigSystem 5.3절): 큐에 쌓인 설정 변경·콘솔 명령을 프레임 시작 전에
    //      적용한다. 실패하면 값은 Sync 전 그대로이고 프레임은 계속한다(첫 오류를 돌려준다).
    Result<void> synced = Result<void>::Success();
    if (config_.settings != nullptr) {
        synced = config_.settings->Sync();
        if (!synced && !settingsSyncFailing_) {
            Log::Error(LogEngine, "Config sync failed at frame {Frame}: {Error}", time_.frameIndex,
                       ErrorCodeName(synced.Error()));
        }

        settingsSyncFailing_ = !synced;
    }

    // 3) 프레임 시작.
    if (config_.hooks != nullptr) {
        config_.hooks->BeginFrame(*this);
    }

    {
        ObjectAccessScope scope(objects_->OwnerAccess());
        for (usize index = 0; index < subsystems_.Count(); ++index) {
            subsystems_.At(index)->OnFrameBegin(*this);
        }

        if (config_.client != nullptr) {
            config_.client->OnEngineTick(*this);
        }
    }

    // 다른 스레드가 건 전체 수집 요청을 살아 있는 World에 나눠 준다(GC 설계 17.4절 — World 배열은
    // 게임 스레드만 돈다).
    if (platform::AtomicExchange<platform::MemoryOrder::AcquireRelease>(&fullCollectionRequested_,
                                                                        uint32{0}) != 0) {
        for (WorldContext& context : worlds_) {
            if (!context.pendingDestroy && context.world) {
                context.world->RequestFullCollection();
            }
        }
    }

    // 4) World Tick. hook이 World를 추가·파괴 요청해도 index로 다시 읽는다.
    Result<void> outcome = Move(synced);
    const uint64 worldStart = platform::MonotonicNanoseconds();
    for (usize index = 0; index < worlds_.Size(); ++index) {
        // 비활성 World는 Tick하지 않는다(16.4절).
        if (worlds_[index].pendingDestroy || worlds_[index].world->Kind() == WorldKind::Inactive) {
            continue;
        }

        World* world = worlds_[index].world.Get();
        Result<void> ticked = Result<void>::Success();
        {
            LogContextScope scope("World", world->Name());
            ticked = world->Tick(time_.deltaSeconds);
            // Tick 안에서 World가 추가돼 배열이 옮겨졌을 수 있어 index로 다시 읽는다.
            NoteWorldTick(worlds_[index], ticked);
        }

        if (!ticked && outcome) {
            outcome = Move(ticked);
        }
    }

    worldTickNanoseconds_ = platform::MonotonicNanoseconds() - worldStart;

    // 5) 지연 작업.
    inFrame_ = false;
    ProcessDeferred();
    inFrame_ = true;

    // 6) 프레임 끝.
    {
        ObjectAccessScope scope(objects_->OwnerAccess());
        for (usize index = 0; index < subsystems_.Count(); ++index) {
            subsystems_.At(index)->OnFrameEnd(*this);
        }
    }

    if (config_.hooks != nullptr) {
        config_.hooks->EndFrame(*this);
    }

    inFrame_ = false;
    frameRunning_ = false;
    frameNanoseconds_ = platform::MonotonicNanoseconds() - frameStart;
    if (config_.maxFrames != 0 && time_.frameIndex >= config_.maxFrames) {
        exitRequested_ = true;
        maxFramesReached_ = true;
    }

    return outcome;
}

Result<void> Engine::Run() noexcept {
    if (state_ != EngineState::Running) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Result<void> outcome = Result<void>::Success();
    platform::SetRunPhase(platform::RunPhase::Running, EngineStageRun);
    while (!exitRequested_) {
        auto frame = RunFrame();
        if (!frame) {
            outcome = Move(frame);
            break;
        }
    }

    state_ = EngineState::Stopping;
    // 프레임 실패의 원인은 World Tick 실패 기록이 이미 남겼다.
    const char* reason = !outcome ? "FrameFailed" : maxFramesReached_ ? "MaxFrames" : "RequestExit";
    Log::Info(LogEngine, "Engine run finished after {Frames} frames: {Reason}", time_.frameIndex,
              reason);
    return outcome;
}

Result<void> Engine::Shutdown() noexcept {
    if (state_ == EngineState::Uninitialized || state_ == EngineState::Stopped ||
        state_ == EngineState::Failed) {
        return Result<void>::Success();
    }

    if (inFrame_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    // 초기화 중(Subsystem Initialize·client OnEngineStart와 초기화 실패 정리 — 그 안에서 불린
    // callback 포함)이나 종료 단계 안(Subsystem·hook·client의 종료 함수와 그 안에서 불린
    // callback)에서 부르면 Busy다 — 종료 단계를 돌면 바깥 호출(Initialize·Shutdown)이 쓰는
    // 상태(정리 중인 Subsystem·객체 Registry 등)를 지운다. 초기화 실패는 Initialize가 스스로
    // 정리한다.
    if (state_ == EngineState::Initializing || shutdownRunning_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    shutdownRunning_ = true;
    auto running = MakeScopeExit([this]() noexcept { shutdownRunning_ = false; });
    if (state_ != EngineState::Stopping) {
        state_ = EngineState::Stopping;
    }

    // 재시도(앞 호출이 실패해 Stopping에 머묾)에서는 시작을 다시 알리지 않는다(단계 표시·filter
    // 확인도 첫 호출에서만).
    if (shutdownProgress_ == ShutdownNotStarted) {
        platform::SetRunPhase(platform::RunPhase::Shutdown, EngineStageShutdown);
        WarnIfCrashFilterReplaced();
        Log::Info(LogEngine, "Engine shutting down with {Worlds} worlds after {Frames} frames",
                  static_cast<uint32>(worlds_.Size()), time_.frameIndex);
    }

    const char* failedStage = "ShutdownHooks";
    Result<void> outcome = RunShutdownHooksBeforeWorlds();
    if (initializedSteps_ >= StepClient) {
        // client 종료는 한 번만 부른다(재시도해도 다시 부르지 않도록 단계를 내린다).
        if (config_.client != nullptr) {
            ObjectAccessScope scope(objects_->OwnerAccess());
            config_.client->OnEngineShutdown(*this);
        }

        initializedSteps_ = StepSubsystems;
    }

    // 나중에 만든 World부터 파괴한다.
    while (worlds_.Size() != 0) {
        auto destroyed = DestroyWorldNow(worlds_.Size() - 1);
        if (!destroyed) {
            LogShutdownFailure("Worlds", destroyed.Error());
            return destroyed;
        }
    }

    // World가 모두 없어졌으므로 빌려준 GC scheduler를 끝낸다(GC 설계 17.4절).
    if (auto stopped = StopGcScheduler(); !stopped) {
        LogShutdownFailure("GcScheduler", stopped.Error());
        return stopped;
    }

    // 진행 중인 GPU 프레임·비동기 작업 완료(계획 3.2절 "GPU fence·audio epoch·physics step 완료").
    if (shutdownProgress_ < ShutdownWorkCompleted) {
        if (initializedSteps_ >= StepSubsystems) {
            ObjectAccessScope scope(objects_->OwnerAccess());
            for (usize index = subsystems_.Count(); index > 0; --index) {
                KeepFirst(outcome, failedStage,
                          subsystems_.At(index - 1)->CompleteInFlightWork(*this),
                          "CompleteInFlightWork");
            }
        }

        if (config_.hooks != nullptr) {
            KeepFirst(outcome, failedStage, config_.hooks->CompleteInFlightWork(*this),
                      "CompleteInFlightWork");
        }

        shutdownProgress_ = ShutdownWorkCompleted;
    }

    if (initializedSteps_ >= StepSubsystems) {
        ObjectAccessScope scope(objects_->OwnerAccess());
        subsystems_.Deinitialize(*this);
        initializedSteps_ = StepObjects;
    }

    const char* closeStage = failedStage;
    auto closedAfter = CloseAfterSubsystems(closeStage);
    KeepFirst(outcome, failedStage, Move(closedAfter), closeStage);
    // 남은 수는 Registry를 닫기 전에 센다(닫은 뒤에는 0이다).
    if (!outcome) {
        LogShutdownFailure(failedStage, outcome.Error());
    }

    if (objects_) {
        auto closed = objects_->Shutdown(objects_->OwnerAccess());
        if (!closed) {
            // 기존 실패 처리(Stopping에 머물고 오류 반환) 전에 남은 수와 함께 한 번 기록한다.
            LogShutdownFailure("ObjectRegistry", closed.Error());
            return closed;
        }

        objects_.Reset();
    }

    // World·게임 타입은 Unregister하지 않는다 — 게임이 토큰을 버린 native 파생이 리플렉션 base의
    // dependent로 남아 Unregister가 Busy로 멈춘다. ModuleRegistration 파괴로 붙잡은 lease만 놓고
    // TypeRegistry::Shutdown이 남은 레코드를 dependent 순서로 반납한다. 모듈 타입 등록기는 그 전에
    // 떼어 모듈별 기록을 놓는다(Docs/Design/ModuleLoader.ko.md 3.11절).
    modules_.Reset();
    DetachModuleTypes();
    if (types_) {
        auto closed = types_->Shutdown();
        if (!closed) {
            LogShutdownFailure("TypeRegistry", closed.Error());
            return closed;
        }

        types_.Reset();
    }

    initializedSteps_ = 0;
    shutdownProgress_ = ShutdownNotStarted;
    state_ = EngineState::Stopped;
    platform::SetRunPhase(platform::RunPhase::Shutdown, EngineStageStopped);

    Log::Info(LogEngine, "Engine shut down");
    return outcome;
}

Result<void> Engine::AttachModuleTypes() noexcept {
    ModuleLoader* loader = config_.modules;
    if (loader == nullptr) {
        Log::Error(LogEngine, "EngineConfig::modules is required: pass the process module loader "
                              "(Docs/Design/ModuleLoader.ko.md section 3.11)");
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto registrations =
        UniquePtr<ModuleTypeRegistrations>::TryMake(allocator_, *types_, allocator_);
    if (!registrations) {
        return Result<void>::Failure(registrations.Error());
    }

    moduleTypes_ = Move(registrations.Value());
    const ModuleLoadReport attached = loader->AttachTypeRegistrar(*moduleTypes_);
    if (!attached.IsOk()) {
        Log::Error(LogEngine,
                   "Module reflection types could not be registered: {Module} failed at {Stage}: "
                   "{Message}",
                   StringView(attached.failed), StringView(attached.stage),
                   StringView(attached.message));
        return Result<void>::Failure(attached.error != ErrorCode::None ? attached.error
                                                                       : ErrorCode::WrongThread);
    }

    attachedModules_ = loader;
    // 엔진 단계(PreDefault·Default·PostDefault) 모듈을 시작한다. 등록기가 붙어 있으므로 각 모듈의
    // 타입은 Startup 직전에 등록된다. 이미 다른 Engine이 시작했으면 사용자 수만 는다.
    const ModuleLoadReport started = loader->StartEngineGroup();
    if (!started.IsOk()) {
        Log::Error(LogEngine,
                   "Engine modules could not start ({Code}): {Module} failed at {Stage}: {Message}",
                   ModuleLoadCodeName(started.code), StringView(started.failed),
                   StringView(started.stage), StringView(started.message));
        return Result<void>::Failure(
            started.error != ErrorCode::None ? started.error : ErrorCode::DependencyFailed);
    }

    engineGroupStarted_ = true;
    return Result<void>::Success();
}

Result<void> Engine::StartPostEngineInitModules() noexcept {
    const ModuleLoadReport started = attachedModules_->StartPhase(LoadingPhase::PostEngineInit);
    if (!started.IsOk()) {
        Log::Error(LogEngine,
                   "PostEngineInit modules could not start ({Code}): {Module} failed at {Stage}: "
                   "{Message}",
                   ModuleLoadCodeName(started.code), StringView(started.failed),
                   StringView(started.stage), StringView(started.message));
        return Result<void>::Failure(
            started.error != ErrorCode::None ? started.error : ErrorCode::DependencyFailed);
    }

    return Result<void>::Success();
}

void Engine::DetachModuleTypes() noexcept {
    if (attachedModules_ != nullptr) {
        // 엔진 묶음을 먼저 놓는다(마지막 Engine이면 엔진 묶음 모듈의 Shutdown 뒤 그 타입을 모든
        // 등록기에서 되돌린다). 그다음 이 Engine의 등록기에 남은 등록(앞 묶음 모듈)을 되돌린다.
        if (engineGroupStarted_) {
            attachedModules_->ShutdownEngineGroup();
            engineGroupStarted_ = false;
        }

        if (moduleTypes_) {
            attachedModules_->DetachTypeRegistrar(*moduleTypes_);
        }

        attachedModules_ = nullptr;
    }

    moduleTypes_.Reset();
}

Result<void> Engine::StopGcScheduler() noexcept {
    if (!gcScheduler_) {
        return Result<void>::Success();
    }

    auto stopped = gcScheduler_->Shutdown();
    if (!stopped) {
        return stopped;
    }

    gcScheduler_.Reset();
    return Result<void>::Success();
}

void Engine::RequestFullCollection() noexcept {
    platform::AtomicStore<platform::MemoryOrder::Release>(&fullCollectionRequested_, uint32{1});
}

Result<void> Engine::RunShutdownHooksBeforeWorlds() noexcept {
    if (shutdownProgress_ >= ShutdownBlocked) {
        return Result<void>::Success();
    }

    // 신규 요청 차단(계획 3.2절 첫 단계). Subsystem은 역순, 그다음 실행 파일 hook이다.
    if (initializedSteps_ >= StepSubsystems) {
        ObjectAccessScope scope(objects_->OwnerAccess());
        for (usize index = subsystems_.Count(); index > 0; --index) {
            subsystems_.At(index - 1)->BeginShutdown(*this);
        }
    }

    if (config_.hooks != nullptr) {
        config_.hooks->BeginShutdown(*this);
    }

    shutdownProgress_ = ShutdownBlocked;
    return Result<void>::Success();
}

Result<void> Engine::CloseAfterSubsystems(const char*& failedStage) noexcept {
    Result<void> outcome = Result<void>::Success();
    // 최종 GC와 그 GC가 만든 정리 작업(Retire·backend 회수)을 둘 다 없어질 때까지 반복한다.
    bool closed = false;
    for (uint32 pass = 0; pass < MaxShutdownCollectPasses; ++pass) {
        uint64 reclaimed = 0;
        if (objects_) {
            auto collected = objects_->Collect(objects_->OwnerAccess());
            if (!collected) {
                KeepFirst(outcome, failedStage, Result<void>::Failure(collected.Error()),
                          "Collect");
                break;
            }

            const GcReport& report = collected.Value();
            reclaimed = report.reclaimed;
            Log::Debug(LogEngine,
                       "Engine GC pass {Pass} reclaimed {Reclaimed} of {Examined} examined objects "
                       "in {Microseconds} us",
                       pass + 1, report.reclaimed, report.examined,
                       report.durationNanoseconds / 1000);
        }

        uint64 drained = 0;
        if (config_.hooks != nullptr) {
            auto cleaned = config_.hooks->DrainShutdownCleanup(*this);
            if (cleaned) {
                drained = cleaned.Value();
            } else {
                KeepFirst(outcome, failedStage, Result<void>::Failure(cleaned.Error()),
                          "DrainShutdownCleanup");
            }
        }

        if (reclaimed == 0 && drained == 0) {
            closed = true;
            break;
        }
    }

    if (!closed) {
        KeepFirst(outcome, failedStage, Result<void>::Failure(ErrorCode::Busy), "Closure");
    }

    // owner·retire·queue closure 확인 → 전용 실행기 종료. 앞 단계가 실패해도 건너뛰지 않는다.
    if (config_.hooks != nullptr) {
        KeepFirst(outcome, failedStage, config_.hooks->VerifyShutdownClosure(*this),
                  "VerifyShutdownClosure");
        KeepFirst(outcome, failedStage, config_.hooks->StopExecutors(*this), "StopExecutors");
    }

    return outcome;
}

void Engine::LogShutdownFailure(const char* stage, ErrorCode error) const noexcept {
    uint32 pending = 0;
    for (const WorldContext& context : worlds_) {
        pending += context.pendingDestroy ? 1u : 0u;
    }

    const uint32 liveObjects = objects_ ? objects_->Stats().liveObjects : 0u;
    Log::Error(LogEngine,
               "Engine shutdown failed at {Stage}: {Error} ({Worlds} worlds, {PendingDestroys} "
               "pending destroys, {LiveObjects} live objects)",
               stage, ErrorCodeName(error), static_cast<uint32>(worlds_.Size()), pending,
               liveObjects);
}

EngineStats Engine::Stats() const noexcept {
    EngineStats stats;
    stats.worlds = static_cast<uint32>(worlds_.Size());
    stats.subsystems = static_cast<uint32>(subsystems_.Count());
    stats.frameNanoseconds = frameNanoseconds_;
    stats.worldTickNanoseconds = worldTickNanoseconds_;
    for (const WorldContext& context : worlds_) {
        stats.pendingWorldDestroys += context.pendingDestroy ? 1u : 0u;
    }

    return stats;
}

} // namespace rocks
