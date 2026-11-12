#include <Engine/Engine.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

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

Engine::Engine(Allocator& allocator) noexcept
    : allocator_(allocator), subsystems_(allocator), worlds_(allocator) {
}

Engine::~Engine() {
    auto finished = Shutdown();
    if (!finished) {
        platform::Fatal("Engine destroyed while worlds or objects are still held outside");
    }
}

ObjectRegistry& Engine::Objects() noexcept {
    if (!objects_) {
        platform::Fatal("Engine::Objects called before initialization");
    }
    return *objects_;
}

Result<void> Engine::Initialize(const EngineConfig& config) noexcept {
    if (state_ != EngineState::Uninitialized) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    state_ = EngineState::Initializing;
    config_ = config;
    auto fail = [this](ErrorCode error) noexcept {
        Teardown(initializedSteps_);
        state_ = EngineState::Failed;
        return Result<void>::Failure(error);
    };

    auto types = UniquePtr<TypeRegistry>::TryMake(allocator_, allocator_);
    if (!types) {
        return fail(types.Error());
    }
    types_ = Move(types.Value());
    initializedSteps_ = StepTypes;

    auto registered = RegisterWorldTypes(*types_);
    if (registered && config_.registerTypes != nullptr) {
        registered = config_.registerTypes(*types_);
    }
    if (!registered) {
        return fail(registered.Error());
    }
    initializedSteps_ = StepTypesRegistered;

    auto objects =
        UniquePtr<ObjectRegistry>::TryMake(allocator_, allocator_, *types_, config_.context);
    if (!objects) {
        return fail(objects.Error());
    }
    objects_ = Move(objects.Value());
    initializedSteps_ = StepObjects;

    {
        ObjectAccessScope scope(objects_->OwnerAccess());
        auto subsystems = subsystems_.Initialize(config_.subsystems, *this);
        if (!subsystems) {
            return fail(subsystems.Error());
        }
    }
    initializedSteps_ = StepSubsystems;

    if (config_.client != nullptr) {
        ObjectAccessScope scope(objects_->OwnerAccess());
        auto started = config_.client->OnEngineStart(*this);
        if (!started) {
            return fail(started.Error());
        }
    }
    initializedSteps_ = StepClient;

    state_ = EngineState::Running;
    lastClock_ = platform::MonotonicNanoseconds();
    return Result<void>::Success();
}

void Engine::Teardown(uint32 completedSteps) noexcept {
    // 초기화 실패 경로다. World·client는 아직 없거나 시작하지 않았다. 남은 단계를 역순으로
    // 정리한다. Subsystem 초기화 도중 실패했어도 앞서 성공한 Subsystem은 Initialize가 이미
    // 정리했다.
    if (completedSteps >= StepSubsystems && objects_) {
        ObjectAccessScope scope(objects_->OwnerAccess());
        subsystems_.Deinitialize(*this);
    }

    if (completedSteps >= StepObjects && objects_) {
        auto collected = objects_->Collect(objects_->OwnerAccess());
        (void)collected;
        auto closed = objects_->Shutdown(objects_->OwnerAccess());
        if (!closed) {
            platform::Fatal(
                "Engine object registry could not shut down after initialization failure");
        }
        objects_.Reset();
    }

    if (completedSteps >= StepTypes && types_) {
        auto closed = types_->Shutdown();
        if (!closed) {
            platform::Fatal(
                "Engine type registry could not shut down after initialization failure");
        }
        types_.Reset();
    }

    initializedSteps_ = 0;
}

Result<World*> Engine::CreateWorld(const WorldCreateDesc& desc) noexcept {
    if (state_ != EngineState::Running) {
        return Result<World*>::Failure(ErrorCode::InvalidArgument);
    }

    auto reserved = worlds_.TryReserve(worlds_.Size() + 1);
    if (!reserved) {
        return Result<World*>::Failure(reserved.Error());
    }

    WorldDesc worldDesc;
    worldDesc.name = desc.name;
    worldDesc.context = {WorldContextHigh, nextWorldContext_++};
    worldDesc.timeStep = desc.timeStep;
    worldDesc.objects = desc.objects;
    worldDesc.gcBudget = desc.gcBudget;
    worldDesc.subsystems = desc.subsystems;
    auto created = UniquePtr<World>::TryMake(allocator_, allocator_, *types_, worldDesc);
    if (!created) {
        return Result<World*>::Failure(created.Error());
    }

    UniquePtr<World> world = Move(created.Value());
    auto initialized = world->Initialize();
    if (initialized && desc.beginPlay) {
        initialized = world->BeginPlay();
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
    auto finished = worlds_[index].world->Shutdown();
    if (!finished) {
        // 외부가 Borrow·RootRef를 보유하고 있다. 지연 작업에서 다시 시도한다.
        worlds_[index].pendingDestroy = true;
        return finished;
    }

    UniquePtr<World> world = Move(worlds_[index].world);
    auto removed = worlds_.TryRemoveAt(index);
    (void)removed;
    world.Reset();
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
        if (worlds_[index - 1].pendingDestroy) {
            auto destroyed = DestroyWorldNow(index - 1);
            (void)destroyed;
        }
    }
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
    // hook은 Registry scope 없이 부른다(World 객체는 World::With가 자기 scope를 세운다). 엔진
    // Registry를 다루는 Subsystem·client 호출 구간에만 엔진 scope를 세운다.

    // ① 이벤트 펌프.
    if (config_.hooks != nullptr) {
        config_.hooks->PumpEvents(*this);
    }

    // ② 시간 갱신.
    const uint64 now = platform::MonotonicNanoseconds();
    const double measured = static_cast<double>(now - lastClock_) * 1.0e-9;
    lastClock_ = now;
    time_.deltaSeconds =
        config_.fixedFrameDeltaSeconds > 0.0 ? config_.fixedFrameDeltaSeconds : measured;
    ++time_.frameIndex;
    time_.realTimeSeconds += time_.deltaSeconds;
    time_.averageFrameSeconds = time_.frameIndex == 1
                                    ? time_.deltaSeconds
                                    : time_.averageFrameSeconds * 0.9 + time_.deltaSeconds * 0.1;

    // ③ 프레임 시작.
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

    // ④ World Tick. hook이 World를 추가·파괴 요청해도 index로 다시 읽는다.
    Result<void> outcome = Result<void>::Success();
    const uint64 worldStart = platform::MonotonicNanoseconds();
    for (usize index = 0; index < worlds_.Size(); ++index) {
        if (worlds_[index].pendingDestroy) {
            continue;
        }
        auto ticked = worlds_[index].world->Tick(time_.deltaSeconds);
        if (!ticked && outcome) {
            outcome = Move(ticked);
        }
    }
    worldTickNanoseconds_ = platform::MonotonicNanoseconds() - worldStart;

    // ⑤ 지연 작업.
    inFrame_ = false;
    ProcessDeferred();
    inFrame_ = true;

    // ⑥ 프레임 끝.
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
    frameNanoseconds_ = platform::MonotonicNanoseconds() - frameStart;
    if (config_.maxFrames != 0 && time_.frameIndex >= config_.maxFrames) {
        exitRequested_ = true;
    }

    return outcome;
}

Result<void> Engine::Run() noexcept {
    if (state_ != EngineState::Running) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Result<void> outcome = Result<void>::Success();
    while (!exitRequested_) {
        auto frame = RunFrame();
        if (!frame) {
            outcome = Move(frame);
            break;
        }
    }

    state_ = EngineState::Stopping;
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

    if (state_ != EngineState::Stopping) {
        state_ = EngineState::Stopping;
    }

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
            return destroyed;
        }
    }

    if (initializedSteps_ >= StepSubsystems) {
        ObjectAccessScope scope(objects_->OwnerAccess());
        subsystems_.Deinitialize(*this);
        initializedSteps_ = StepObjects;
    }

    if (objects_) {
        for (uint32 attempt = 0; attempt < 8; ++attempt) {
            auto collected = objects_->Collect(objects_->OwnerAccess());
            if (!collected) {
                return Result<void>::Failure(collected.Error());
            }
            if (collected.Value().reclaimed == 0) {
                break;
            }
        }
        auto closed = objects_->Shutdown(objects_->OwnerAccess());
        if (!closed) {
            return closed;
        }
        objects_.Reset();
    }

    if (types_) {
        auto closed = types_->Shutdown();
        if (!closed) {
            return closed;
        }
        types_.Reset();
    }

    initializedSteps_ = 0;
    state_ = EngineState::Stopped;
    return Result<void>::Success();
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
