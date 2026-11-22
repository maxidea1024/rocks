#include <SparksVm/VmSession.h>

// session·instance의 구현이다(Docs/Design/SparksRuntime.ko.md 2.2절, LC6a).

namespace rocks::sparks {

namespace {

// 기반 사슬을 따라가는 깊이 상한이다(모듈 사이 순환 방어 — 컴파일러는 순환을 막는다).
constexpr uint32 VmSessionMaxBaseDepth = 64;

// handle이 이 표에 없다는 표지다.
constexpr uint32 VmNoInstance = NoBytecodeIndex;

// hook·메서드 Ref 인자(this 포함)의 상한이다(frame bank 상한과 같다).
constexpr usize VmSessionMaxRefArguments = 256;

} // namespace

VmSession::VmSession(Allocator& allocator, VmHost* host) noexcept
    : allocator_(&allocator), host_(host), execution_(allocator), modules_(allocator),
      instances_(allocator), byHandle_(allocator), connections_(allocator), queue_(allocator),
      itemValues_(allocator), itemBuffers_(allocator), writes_(allocator), writeValues_(allocator),
      writeBuffers_(allocator), deliveries_(allocator), suspended_(allocator), waits_(allocator),
      freeWaits_(allocator), fragmentWaits_(allocator), resumeOrder_(allocator),
      resumeWaits_(allocator), requests_(allocator), applying_(allocator), destroys_(allocator),
      faults_(allocator), retired_(allocator) {
    execution_.session_ = this;
}

VmSession::~VmSession() noexcept {
    // 남은 발행 항목·Changed 시작 값의 참조를 놓고 instance를 모두 Detach한다(CloseSession).
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        ReleaseItem(queue_[at]);
    }

    for (VmBuffer* buffer : writeBuffers_) {
        execution_.Release(buffer);
    }

    writes_.Clear();
    writeBuffers_.Clear();
    for (Suspended& entry : suspended_) {
        if (entry.live) {
            ReleaseSuspended(entry);
        }
    }

    DetachAll();
}

Result<void> VmSession::AddModule(const VmLinkedModule& linked) noexcept {
    if (closed_ || execution_.running_ || !linked.Linked() || linked.Host() != host_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const BytecodeModule& added = linked.Module();
    for (const VmLinkedModule* existing : modules_) {
        if (existing == &linked) {
            return Result<void>::Success();
        }

        // 같은 경로의 다른 모듈은 거부한다(LC6a 리뷰 S4).
        const BytecodeModule& other = existing->Module();
        if (added.path != NoBytecodeIndex && other.path != NoBytecodeIndex &&
            added.String(added.path) == other.String(other.path)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return modules_.TryPushBack(&linked);
}

bool VmSession::BaseOf(const VmLinkedModule& module, uint32 classIndex,
                       const VmLinkedModule*& baseModule, uint32& baseClass) const noexcept {
    const BytecodeModule& bytecode = module.Module();
    const BytecodeClass& entry = bytecode.classes[classIndex];
    if (entry.baseClass != NoBytecodeIndex) {
        baseModule = &module;
        baseClass = entry.baseClass;
        return true;
    }

    if (entry.baseName == NoBytecodeIndex) {
        return false;
    }

    // `<경로>:Class`면 session 모듈에서 찾는다(엔진 기반 이름에는 `:`가 없다).
    const StringView name = bytecode.String(entry.baseName);
    usize colon = name.ByteCount();
    for (usize at = 0; at < name.ByteCount() && colon == name.ByteCount(); ++at) {
        colon = name.Data()[at] == ':' ? at : colon;
    }

    if (colon == name.ByteCount()) {
        return false;
    }

    const StringView path(name.Data(), colon);
    const StringView className(name.Data() + colon + 1, name.ByteCount() - colon - 1);
    for (const VmLinkedModule* candidate : modules_) {
        const BytecodeModule& other = candidate->Module();
        if (other.path == NoBytecodeIndex || other.String(other.path) != path) {
            continue;
        }

        for (uint32 index = 0; index < other.classes.Size(); ++index) {
            if (other.String(other.classes[index].name) == className) {
                baseModule = candidate;
                baseClass = index;
                return true;
            }
        }
    }

    return false;
}

bool VmSession::RegionStart(const VmLinkedModule& module, uint32 classIndex,
                            uint32 (&start)[BytecodeBankCount]) const noexcept {
    for (uint32& value : start) {
        value = 0;
    }

    const VmLinkedModule* current = &module;
    uint32 currentClass = classIndex;
    for (uint32 depth = 0; depth < VmSessionMaxBaseDepth; ++depth) {
        const VmLinkedModule* baseModule = nullptr;
        uint32 baseClass = NoBytecodeIndex;
        if (!BaseOf(*current, currentClass, baseModule, baseClass)) {
            // 엔진 기반 또는 기반 없음이면 끝이다. `<경로>:Class`를 못 찾으면 실패다.
            const BytecodeClass& entry = current->Module().classes[currentClass];
            if (entry.baseName == NoBytecodeIndex) {
                return true;
            }

            const StringView name = current->Module().String(entry.baseName);
            for (usize at = 0; at < name.ByteCount(); ++at) {
                if (name.Data()[at] == ':') {
                    return false;
                }
            }

            return true;
        }

        const BytecodeClass& base = baseModule->Module().classes[baseClass];
        for (uint32 bank = 0; bank < BytecodeBankCount; ++bank) {
            start[bank] += base.slots[bank];
        }

        current = baseModule;
        currentClass = baseClass;
    }

    return false;
}

Result<uint32> VmSession::CreateInstance(uint64 handle, const VmLinkedModule& module,
                                         uint32 classIndex, VmCallResult& init) noexcept {
    init = VmCallResult();
    if (closed_ || execution_.running_) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument); // 종료 뒤 admission 없음
    }

    bool known = false;
    for (const VmLinkedModule* existing : modules_) {
        known = known || existing == &module;
    }

    if (!known || classIndex >= module.Module().classes.Size() || Find(handle) != VmNoInstance ||
        handle == 0 || host_ == nullptr || !host_->IsAlive(handle)) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    uint32 start[BytecodeBankCount] = {};
    if (!RegionStart(module, classIndex, start)) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 저장소는 기반 사슬 전체(자기 구역 끝까지)다. 0·null로 시작한다.
    const BytecodeClass& entry = module.Module().classes[classIndex];
    Instance instance(*allocator_);
    instance.handle = handle;
    instance.module = &module;
    instance.classIndex = classIndex;
    instance.attached = true;
    if (auto resized = instance.scalars.TryResize(start[0] + entry.slots[0]); !resized) {
        return Result<uint32>::Failure(resized.Error());
    }

    if (auto resized = instance.refs.TryResize(start[1] + entry.slots[1]); !resized) {
        return Result<uint32>::Failure(resized.Error());
    }

    if (auto resized = instance.buffers.TryResize(start[2] + entry.slots[2]); !resized) {
        return Result<uint32>::Failure(resized.Error());
    }

    // slot별 쓰기 bit(SparksReload K5 — 0으로 시작, `$init`은 표시하지 않는다).
    const usize totalSlots =
        instance.scalars.Size() + instance.refs.Size() + instance.buffers.Size();
    if (auto resized =
            instance.written.TryResize((totalSlots + VmWrittenWordBits - 1) / VmWrittenWordBits);
        !resized) {
        return Result<uint32>::Failure(resized.Error());
    }

    for (uint64& value : instance.written) {
        value = 0;
    }

    for (uint64& value : instance.scalars) {
        value = 0;
    }

    for (uint64& value : instance.refs) {
        value = 0;
    }

    for (VmBuffer*& value : instance.buffers) {
        value = nullptr;
    }

    // Detach된 번호를 다시 쓴다(LC6a 리뷰 B3 — spawn·despawn 반복에도 표가 자라지 않는다).
    uint32 number = static_cast<uint32>(instances_.Size());
    for (uint32 at = 0; at < instances_.Size(); ++at) {
        if (!instances_[at].attached) {
            number = at;
            break;
        }
    }

    instance.serial = nextSerial_++;
    if (number < instances_.Size()) {
        instances_[number] = static_cast<Instance&&>(instance);
    } else if (auto pushed = instances_.TryPushBack(static_cast<Instance&&>(instance)); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    if (auto inserted = byHandle_.Insert(handle, number); !inserted) {
        static_cast<void>(Detach(number));
        return Result<uint32>::Failure(inserted.Error());
    }

    // 기반 먼저 `$init`(기반 사슬을 모은 뒤 뒤에서부터).
    const VmLinkedModule* chainModules[VmSessionMaxBaseDepth] = {};
    uint32 chainClasses[VmSessionMaxBaseDepth] = {};
    uint32 depth = 0;
    const VmLinkedModule* current = &module;
    uint32 currentClass = classIndex;
    while (depth < VmSessionMaxBaseDepth) {
        chainModules[depth] = current;
        chainClasses[depth] = currentClass;
        ++depth;
        const VmLinkedModule* baseModule = nullptr;
        uint32 baseClass = NoBytecodeIndex;
        if (!BaseOf(*current, currentClass, baseModule, baseClass)) {
            break;
        }

        current = baseModule;
        currentClass = baseClass;
    }

    // 사슬의 모든 `$init` 기록·명령 수를 모은다(LC6a 리뷰 B4 — RunHook과 같은 규칙). 초기화의
    // 쓰기는 Changed를 내지 않는다(SL 5.3절).
    BeginEntry();
    execution_.keepLogs_ = true;
    suppressChanged_ = true;
    initializing_ = true;
    struct KeepLogs {
        VmSession& session;
        ~KeepLogs() noexcept {
            // 시작 값 기록을 억제 중에 놓은 뒤 억제를 푼다(LC6b 리뷰 B1).
            static_cast<void>(session.EndFragment());
            session.execution_.keepLogs_ = false;
            session.suppressChanged_ = false;
            session.initializing_ = false;
        }
    } keepLogs{*this};
    for (uint32 at = depth; at > 0; --at) {
        const BytecodeClass& link = chainModules[at - 1]->Module().classes[chainClasses[at - 1]];
        if (link.initializer == NoBytecodeIndex) {
            continue;
        }

        const uint64 self[] = {handle};
        auto called = execution_.Call(*chainModules[at - 1], link.initializer, {},
                                      Span<const uint64>(self, 1), {}, FragmentLimits(false));
        if (!called) {
            static_cast<void>(Detach(number));
            return Result<uint32>::Failure(called.Error());
        }

        const uint64 steps = init.steps + called.Value().steps;
        init = called.Value();
        init.steps = steps;
        execution_.ReleaseReturned();
        if (init.outcome != VmOutcome::Returned) {
            static_cast<void>(Detach(number));
            return Result<uint32>::Success(VmNoInstance);
        }
    }

    return Result<uint32>::Success(number);
}

Result<VmCallResult> VmSession::RunHook(uint32 instance, BytecodeHookKind kind,
                                        Span<const uint64> scalars, Span<const uint64> refs,
                                        Span<VmBuffer* const> buffers) noexcept {
    return RunHookImpl(instance, kind, scalars, refs, buffers, nullptr, NoBytecodeIndex);
}

Result<VmCallResult> VmSession::RunHookImpl(uint32 instance, BytecodeHookKind kind,
                                            Span<const uint64> scalars, Span<const uint64> refs,
                                            Span<VmBuffer* const> buffers,
                                            const VmLinkedModule* onlyModule,
                                            uint32 onlyFunction) noexcept {
    if (closed_ || execution_.running_ || instance >= instances_.Size() ||
        !instances_[instance].attached ||
        (instances_[instance].pendingDestroy && kind != BytecodeHookKind::Disable &&
         kind != BytecodeHookKind::Destroy)) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    // 격리: Destroy hook만(SL 8.3절). Tick 중지: 모듈이 다시 링크되기 전까지 Tick 계열 없음.
    const bool tickKind = kind == BytecodeHookKind::Tick || kind == BytecodeHookKind::FixedTick ||
                          kind == BytecodeHookKind::LateTick;
    Instance& target = instances_[instance];
    if (target.ticksStopped && target.tickStopModule != nullptr &&
        target.tickStopModule->Linked() &&
        target.tickStopModule->generation_ != target.tickStopGeneration) {
        target.ticksStopped = false; // fault가 난 모듈이 새로 링크됐다(새 revision)
    }

    // 격리 instance의 Destroy hook은 1회만(SL 8.3절).
    if ((target.isolated && (kind != BytecodeHookKind::Destroy || target.destroyRan)) ||
        (target.ticksStopped && tickKind)) {
        BeginEntry(); // 앞 진입의 기록을 남기지 않는다
        VmCallResult skipped;
        skipped.skipped = true;
        return Result<VmCallResult>::Success(skipped);
    }

    // 기반 사슬(파생 → 기반)을 모아 기반부터 부른다(SL 5.1절 누적).
    const VmLinkedModule* chainModules[VmSessionMaxBaseDepth] = {};
    uint32 chainClasses[VmSessionMaxBaseDepth] = {};
    uint32 depth = 0;
    const VmLinkedModule* current = instances_[instance].module;
    uint32 currentClass = instances_[instance].classIndex;
    while (depth < VmSessionMaxBaseDepth) {
        chainModules[depth] = current;
        chainClasses[depth] = currentClass;
        ++depth;
        const VmLinkedModule* baseModule = nullptr;
        uint32 baseClass = NoBytecodeIndex;
        if (!BaseOf(*current, currentClass, baseModule, baseClass)) {
            break;
        }

        current = baseModule;
        currentClass = baseClass;
    }

    if (kind == BytecodeHookKind::Destroy) {
        instances_[instance].destroyRan = true;
    }

    VmCallResult result;
    // 사슬의 모든 호출 기록을 모은다. Awake·Enable·Disable·Destroy 문맥의 쓰기는 Changed를 내지
    // 않는다(SL 5.3절). Disable은 연결을 끄고(항목 유지) Enable은 연결을 풀어 켠다(SL 5.5절).
    BeginEntry();
    execution_.keepLogs_ = true;
    suppressChanged_ = kind == BytecodeHookKind::Awake || kind == BytecodeHookKind::Enable ||
                       kind == BytecodeHookKind::Disable || kind == BytecodeHookKind::Destroy;
    struct KeepLogs {
        VmSession& session;
        uint32 instance;
        bool disable;
        ~KeepLogs() noexcept {
            // 비활성화: Disable hook이 끝난 뒤(fault·ENDEXEC·예산·할당 실패여도) 유지 자원 필드를
            // 멈춘다(SL 9.2절 — LC6d1 리뷰 B3).
            if (disable) {
                session.ReleaseHeldFields(instance);
            }

            session.execution_.keepLogs_ = false;
            session.suppressChanged_ = false;
        }
    } keepLogs{*this, instance, kind == BytecodeHookKind::Disable};
    if (kind == BytecodeHookKind::Disable) {
        CancelExecutions(instance, false); // 정지 실행 취소(SL 6.1절)
        instances_[instance].enabled = false;
        for (Connection& connection : connections_) {
            connection.active = connection.subscriber == instance ? false : connection.active;
        }
    }
    // this(Ref 0번) 뒤에 hook 인자 Ref를 둔다.
    uint64 refArguments[VmSessionMaxRefArguments] = {};
    if (refs.Size() + 1 > VmSessionMaxRefArguments) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    refArguments[0] = instances_[instance].handle;
    for (usize at = 0; at < refs.Size(); ++at) {
        refArguments[at + 1] = refs[at];
    }

    for (uint32 at = depth; at > 0; --at) {
        const BytecodeModule& bytecode = chainModules[at - 1]->Module();
        const BytecodeClass& entry = bytecode.classes[chainClasses[at - 1]];
        for (uint32 hook = entry.firstHook; hook < entry.firstHook + entry.hookCount; ++hook) {
            if (bytecode.hooks[hook].kind != kind ||
                (onlyModule != nullptr && (chainModules[at - 1] != onlyModule ||
                                           bytecode.hooks[hook].function != onlyFunction))) {
                continue;
            }

            currentInstance_ = instance;
            // Disable·Destroy(Teardown)는 명령 예산이 작다(SL 9.3절).
            const VmLimits limits = FragmentLimits(kind == BytecodeHookKind::Disable ||
                                                   kind == BytecodeHookKind::Destroy);

            auto called =
                execution_.Call(*chainModules[at - 1], bytecode.hooks[hook].function, scalars,
                                Span<const uint64>(refArguments, refs.Size() + 1), buffers, limits);
            currentInstance_ = NoBytecodeIndex;
            SettleWaits(called);
            if (!called) {
                return called;
            }

            const uint64 steps = result.steps + called.Value().steps;
            result = called.Value();
            result.steps = steps;
            execution_.ReleaseReturned();
            // 정지한 hook은 끝난 것으로 보고 사슬을 잇는다(재개는 Advance — SparksRuntime 2.4절).
            if (result.outcome != VmOutcome::Returned && result.outcome != VmOutcome::Suspended) {
                // fault 위치별 처리(SL 8.3절 표): spawn 경로(Awake·첫 Enable)는 host가 spawn 실패로
                // 처리하고, 재활성화 Enable·Start는 격리, Tick 계열은 Tick 중지, 그 밖은 격리 창에
                // 센다.
                if (result.outcome == VmOutcome::Faulted) {
                    // spawn 경로: host가 직접 부른 Awake·첫 Enable(적용 루프의 Enable은 재활성화 —
                    // LC6d3 리뷰 3).
                    const bool spawning = kind == BytecodeHookKind::Awake ||
                                          (kind == BytecodeHookKind::Enable &&
                                           !instances_[instance].enabledOnce && !nested_);
                    // Teardown(Disable·Destroy)은 그 hook만 끊고 종료를 잇고, Validate는 편집기
                    // 경계다(SL 8.3·8.4절) — 기록만 한다.
                    const bool recordOnly = kind == BytecodeHookKind::Disable ||
                                            kind == BytecodeHookKind::Destroy ||
                                            kind == BytecodeHookKind::Validate;
                    if (tickKind) {
                        RecordFault(result.fault, instance);
                        instances_[instance].ticksStopped = true;
                        instances_[instance].tickStopModule = result.fault.module;
                        instances_[instance].tickStopGeneration =
                            result.fault.module != nullptr ? result.fault.module->generation_ : 0;
                    } else if (spawning) {
                        // spawn 실패(host가 Detach): 이 진입이 낸 발행을 전달하지 않는다(SL 8.3절).
                        RecordFault(result.fault, instance);
                        for (usize at = queueHead_; currentRoot_ != 0 && at < queue_.Size(); ++at) {
                            if (queue_[at].root == currentRoot_) {
                                ReleaseItem(queue_[at]);
                                queue_[at].publisher = 0;
                            }
                        }
                    } else if (kind == BytecodeHookKind::Start ||
                               kind == BytecodeHookKind::Enable) {
                        RecordFault(result.fault, instance);
                        Isolate(instance);
                    } else {
                        NoteFault(instance, result, !recordOnly);
                    }
                }

                // hook 쓰기의 기록을 억제 중에 정리한 뒤 전달은 억제 없이(LC6b 리뷰 B2).
                if (auto ended = EndFragment(); !ended) {
                    return Result<VmCallResult>::Failure(ended.Error());
                }

                suppressChanged_ = false;
                if (auto drained = Drain(); !drained) {
                    return Result<VmCallResult>::Failure(drained.Error());
                }

                return Result<VmCallResult>::Success(result);
            }
        }
    }

    if (auto ended = EndFragment(); !ended) {
        return Result<VmCallResult>::Failure(ended.Error());
    }

    suppressChanged_ = false;
    if (kind == BytecodeHookKind::Enable) {
        instances_[instance].enabled = true;
        instances_[instance].enabledOnce = true;
        for (Connection& connection : connections_) {
            connection.active = connection.subscriber == instance ? true : connection.active;
        }

        // 모든 handler의 낡은 연결을 먼저 지우고 그다음 더한다(상한 판정이 handler 순서를 타지
        // 않게 — LC6d4 리뷰 B2).
        auto removed = Connect(instance, nullptr, NoBytecodeIndex, ConnectRemove);
        if (!removed) {
            return Result<VmCallResult>::Failure(removed.Error());
        }

        if (auto connected = Connect(instance, nullptr, NoBytecodeIndex, ConnectAdd); !connected) {
            return Result<VmCallResult>::Failure(connected.Error());
        }
    }

    if (auto drained = Drain(); !drained) {
        return Result<VmCallResult>::Failure(drained.Error());
    }

    return Result<VmCallResult>::Success(result);
}

Result<VmCallResult> VmSession::Call(uint32 instance, const VmLinkedModule& module, uint32 function,
                                     Span<const uint64> scalars, Span<const uint64> refs,
                                     Span<VmBuffer* const> buffers) noexcept {
    if (closed_ || execution_.running_ || instance >= instances_.Size() ||
        !instances_[instance].attached || instances_[instance].pendingDestroy ||
        refs.Size() + 1 > VmSessionMaxRefArguments) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 refArguments[VmSessionMaxRefArguments] = {};
    refArguments[0] = instances_[instance].handle;
    for (usize at = 0; at < refs.Size(); ++at) {
        refArguments[at + 1] = refs[at];
    }

    // 동시 실행 상한: 새 실행을 건너뛴다(fault 아님 — SL 9.3절). 인자 모양이 틀린 호출은 먼저
    // 거부한다(상한이 호출 오류를 가리지 않게 — LC6d4 리뷰 R3).
    if (function >= module.Module().functions.Size()) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    const BytecodeFunction& shape = module.Module().functions[function];
    if (scalars.Size() != shape.parameters[0] || refs.Size() + 1 != shape.parameters[1] ||
        buffers.Size() != shape.parameters[2]) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    if (ExecutionsFull(instance)) {
        BeginEntry();
        execution_.ReleaseReturned();
        execution_.returned_.Clear();
        ++skippedExecutions_;
        VmCallResult skipped;
        skipped.skipped = true;
        return Result<VmCallResult>::Success(skipped);
    }

    if (instances_[instance].isolated) {
        BeginEntry();
        execution_.ReleaseReturned(); // 앞 호출의 반환 값을 남기지 않는다(LC6d3 리뷰 1)
        execution_.returned_.Clear();
        VmCallResult skipped;
        skipped.skipped = true; // 격리: admission 없음(SL 8.3절)
        return Result<VmCallResult>::Success(skipped);
    }

    BeginEntry();
    execution_.keepLogs_ = true;
    struct KeepLogs {
        VmExecution& execution;
        ~KeepLogs() noexcept {
            execution.keepLogs_ = false;
        }
    } keepLogs{execution_};
    currentInstance_ = instance;
    auto called = execution_.Call(module, function, scalars,
                                  Span<const uint64>(refArguments, refs.Size() + 1), buffers,
                                  FragmentLimits(false));
    currentInstance_ = NoBytecodeIndex;
    SettleWaits(called);
    if (!called) {
        return called;
    }

    if (called.Value().outcome == VmOutcome::Faulted) {
        NoteFault(instance, called.Value(), true);
    }

    // 반환 값은 전달(다른 Call) 뒤에도 읽을 수 있어야 한다: 반환 slot·버퍼를 보관하고 전달한 뒤
    // 실행 문맥의 반환 자리로 되돌린다(버퍼 참조는 옮긴다).
    Array<uint64> slots(*allocator_);
    Array<VmBuffer*> returned(*allocator_);
    if (!slots.TryAppend(execution_.returned_.View()) ||
        !returned.TryAppend(execution_.returnedBuffers_.View())) {
        return Result<VmCallResult>::Failure(ErrorCode::OutOfMemory);
    }

    execution_.returned_.Clear();
    execution_.returnedBuffers_.Clear(); // 참조는 returned가 쥔다
    auto drained = Drain();
    execution_.ReleaseReturned();
    static_cast<void>(execution_.returned_.TryAppend(slots.View()));
    if (!execution_.returnedBuffers_.TryAppend(returned.View())) {
        for (VmBuffer* buffer : returned) {
            execution_.Release(buffer);
        }

        return Result<VmCallResult>::Failure(ErrorCode::OutOfMemory);
    }

    if (!drained) {
        return Result<VmCallResult>::Failure(drained.Error());
    }

    return called;
}

void VmSession::ReleaseHeldFields(uint32 instance) noexcept {
    const VmLinkedModule* chainModules[MaxBaseDepth] = {};
    uint32 chainClasses[MaxBaseDepth] = {};
    const uint32 depth = Chain(instance, chainModules, chainClasses);
    // 파생 → 기반(선언 역순 — 기반 필드가 앞에 선언됐다), 클래스 안에서는 뒤에서부터.
    for (uint32 at = 0; at < depth; ++at) {
        uint32 start[BytecodeBankCount] = {};
        if (!RegionStart(*chainModules[at], chainClasses[at], start)) {
            continue;
        }

        const BytecodeModule& bytecode = chainModules[at]->Module();
        const BytecodeClass& entry = bytecode.classes[chainClasses[at]];
        for (uint32 field = entry.firstField + entry.fieldCount; field > entry.firstField;
             --field) {
            const BytecodeTypeField& declared = bytecode.classFields[field - 1];
            if ((declared.flags & BytecodeFieldHeld) == 0) {
                continue;
            }

            const usize slot = usize(start[1]) + declared.offset;
            Array<uint64>& refs = instances_[instance].refs;
            if (slot < refs.Size() && refs[slot] != 0) {
                if (host_ != nullptr) {
                    host_->ReleaseHeld(refs[slot]);
                }

                refs[slot] = 0;
            }
        }
    }
}

void VmSession::ReleaseFields(Instance& instance) noexcept {
    for (VmBuffer*& buffer : instance.buffers) {
        execution_.Release(buffer);
        buffer = nullptr;
    }
}

Result<void> VmSession::Detach(uint32 instance) noexcept {
    if (execution_.running_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument); // 실행 중(LC6b 리뷰 B4)
    }

    if (instance >= instances_.Size() || !instances_[instance].attached) {
        return Result<void>::Success();
    }

    Disconnect(instance);
    CancelExecutions(instance, true);
    ReleaseHeldFields(instance);                // 필드 유지 자원(선언 역순 — SL 9.1절 Detach)
    EndEventWaits(instances_[instance].handle); // 발행자 파괴(SD13)
    DropWrites(instance); // 번호 재사용 뒤 남은 기록이 새 instance에 닿지 않게(LC6b 리뷰 B3)
    Instance& entry = instances_[instance];
    ReleaseFields(entry);
    static_cast<void>(byHandle_.Erase(entry.handle));
    entry.attached = false;
    entry.scalars.Empty();
    entry.refs.Empty();
    entry.buffers.Empty();
    entry.written.Empty();
    return Result<void>::Success();
}

void VmSession::DetachAll() noexcept {
    for (uint32 instance = 0; instance < instances_.Size(); ++instance) {
        static_cast<void>(Detach(instance));
    }
}

uint32 VmSession::AttachedCount() const noexcept {
    uint32 count = 0;
    for (const Instance& instance : instances_) {
        count += instance.attached ? 1 : 0;
    }

    return count;
}

uint32 VmSession::Find(uint64 handle) const noexcept {
    const uint32* found = byHandle_.Find(handle);
    return found == nullptr ? VmNoInstance : *found;
}

VmSession::FieldView VmSession::Fields(uint32 instance) const noexcept {
    FieldView view;
    if (instance < instances_.Size() && instances_[instance].attached) {
        const Instance& entry = instances_[instance];
        view.scalars = entry.scalars.View();
        view.refs = entry.refs.View();
        view.buffers = Span<VmBuffer* const>(entry.buffers.Data(), entry.buffers.Size());
    }

    return view;
}

uint32 VmSession::ClassChain(uint32 instance, Span<const VmLinkedModule*> modules,
                             Span<uint32> classes) const noexcept {
    if (instance >= instances_.Size() || !instances_[instance].attached) {
        return 0;
    }

    const VmLinkedModule* chainModules[MaxBaseDepth] = {};
    uint32 chainClasses[MaxBaseDepth] = {};
    const uint32 depth = Chain(instance, chainModules, chainClasses);
    uint32 count = 0;
    for (; count < depth && count < modules.Size() && count < classes.Size(); ++count) {
        modules[count] = chainModules[count];
        classes[count] = chainClasses[count];
    }

    return count;
}

const VmLinkedModule* VmSession::ModuleOf(uint32 instance) const noexcept {
    return instance < instances_.Size() ? instances_[instance].module : nullptr;
}

uint32 VmSession::ClassOf(uint32 instance) const noexcept {
    return instance < instances_.Size() ? instances_[instance].classIndex : VmNoInstance;
}

bool VmSession::FieldSlots(uint64 handle, const VmLinkedModule& module, uint32 classIndex,
                           uint32 bank, uint32 offset, uint32 slots, uint64*& values,
                           VmBuffer**& buffers, uint32& instanceOut, uint32& atOut) noexcept {
    // reload 기본값 계산(SparksReload 5절): 그 클래스 임시 저장소다(instance 번호 없음 — Changed
    // 기록·instance 메모리 예산·쓰기 bit를 건너뛴다).
    if (IsStaging(module, classIndex)) {
        if (bank >= BytecodeBankCount || usize(offset) + slots > staging_.sizes[bank]) {
            return false;
        }

        values = bank == 0   ? staging_.scalars + offset
                 : bank == 1 ? staging_.refs + offset
                             : nullptr;
        buffers = bank == 2 ? staging_.buffers + offset : nullptr;
        instanceOut = VmNoInstance;
        atOut = offset;
        return true;
    }

    const uint32 number = Find(handle);
    if (number == VmNoInstance) {
        return false;
    }

    // 필드의 클래스가 instance 클래스의 기반 사슬에 있어야 한다(다른 클래스 instance의 저장소를
    // 같은 위치로 읽지 않는다).
    Instance& instance = instances_[number];
    const VmLinkedModule* fieldModule = &module;
    uint32 fieldClass = classIndex;
    uint32 fieldOffset = offset;
    auto relatedTo = [&]() noexcept {
        const VmLinkedModule* current = instance.module;
        uint32 currentClass = instance.classIndex;
        for (uint32 depth = 0; depth < VmSessionMaxBaseDepth; ++depth) {
            if (current == fieldModule && currentClass == fieldClass) {
                return true;
            }

            const VmLinkedModule* baseModule = nullptr;
            uint32 baseClass = NoBytecodeIndex;
            if (!BaseOf(*current, currentClass, baseModule, baseClass)) {
                return false;
            }

            current = baseModule;
            currentClass = baseClass;
        }

        return false;
    };
    bool related = bank < BytecodeBankCount && relatedTo();
    // 이전 revision 코드(Keep 실행)의 필드는 지금 revision 위치로 옮겨 쓴다(SparksReload K1).
    for (uint32 hop = 0; !related && bank < BytecodeBankCount && hop < MaxRevisions &&
                         TranslateField(fieldModule, fieldClass, bank, slots, fieldOffset);
         ++hop) {
        related = relatedTo();
    }

    uint32 start[BytecodeBankCount] = {};
    if (bank >= BytecodeBankCount || !related || !RegionStart(*fieldModule, fieldClass, start)) {
        return false;
    }

    const usize at = usize(start[bank]) + fieldOffset;
    const usize size = bank == 0   ? instance.scalars.Size()
                       : bank == 1 ? instance.refs.Size()
                                   : instance.buffers.Size();
    if (bank >= BytecodeBankCount || at + slots > size) {
        return false;
    }

    values = bank == 0   ? instance.scalars.Data() + at
             : bank == 1 ? instance.refs.Data() + at
                         : nullptr;
    buffers = bank == 2 ? instance.buffers.Data() + at : nullptr;
    instanceOut = number;
    atOut = static_cast<uint32>(at);
    return true;
}

void VmSession::MarkWritten(uint32 instance, uint32 bank, uint32 at, uint32 slots) noexcept {
    if (initializing_ || instance >= instances_.Size() || bank >= BytecodeBankCount) {
        return;
    }

    Instance& entry = instances_[instance];
    const usize base = bank == 0   ? 0
                       : bank == 1 ? entry.scalars.Size()
                                   : entry.scalars.Size() + entry.refs.Size();
    for (uint32 slot = 0; slot < slots; ++slot) {
        const usize bit = base + at + slot;
        if (bit / VmWrittenWordBits < entry.written.Size()) {
            entry.written[bit / VmWrittenWordBits] |= uint64(1) << (bit % VmWrittenWordBits);
        }
    }
}

Result<void> VmSession::RequestEnabled(uint32 instance, bool enabled) noexcept {
    if (closed_ || instance >= instances_.Size() || !instances_[instance].attached) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Request request;
    request.instance = instance;
    request.serial = instances_[instance].serial;
    request.kind = enabled ? RequestKind::Enable : RequestKind::Disable;
    if (requests_.Size() >= MaxPendingRequests) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded); // 요청 큐 상한(리뷰 위험)
    }

    return requests_.TryPushBack(request) ? Result<void>::Success()
                                          : Result<void>::Failure(ErrorCode::OutOfMemory);
}

Result<void> VmSession::RequestDestroy(uint32 instance) noexcept {
    if (closed_ || instance >= instances_.Size() || !instances_[instance].attached) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Request request;
    request.instance = instance;
    request.serial = instances_[instance].serial;
    request.kind = RequestKind::Destroy;
    if (requests_.Size() >= MaxPendingRequests) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return requests_.TryPushBack(request) ? Result<void>::Success()
                                          : Result<void>::Failure(ErrorCode::OutOfMemory);
}

Result<void> VmSession::ApplyHook(uint32 instance, BytecodeHookKind kind,
                                  const VmLinkedModule* onlyModule, uint32 onlyFunction) noexcept {
    // 수명 hook 하나를 돌리고 적용 기록(`  hook` 줄)으로 남긴다. 기록은 그 hook이 낸 전달보다
    // 앞이다 (결과는 돌린 뒤 채운다).
    Delivery delivery;
    delivery.subscriber = instances_[instance].handle;
    delivery.module = instances_[instance].module;
    delivery.hook = true;
    delivery.hookKind = kind;
    const usize at = deliveries_.Size();
    if (!deliveries_.TryPushBack(delivery)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    auto ran = RunHookImpl(instance, kind, Span<const uint64>(), Span<const uint64>(),
                           Span<VmBuffer* const>(), onlyModule, onlyFunction);
    execution_.keepLogs_ = true; // RunHook이 끈 기록 모음을 다시 켠다(적용 루프는 한 진입)
    if (!ran) {
        // 돌지 못한 hook의 거짓 기록을 남기지 않는다(그 뒤 기록은 없다 — 실패에서 멈췄다).
        static_cast<void>(deliveries_.TryResize(at));
        return Result<void>::Failure(ran.Error());
    }

    deliveries_[at].result = ran.Value();
    return Result<void>::Success();
}

Result<void> VmSession::ApplyRequests() noexcept {
    if (closed_ || execution_.running_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 회차마다 그때까지의 요청을 활성 전환 먼저(요청 순), 파괴 나중(요청 순)으로 적용하고(SL 5.1·
    // 5.4절 "활성 전환 적용 → 파괴 적용") 그동안 생긴 요청은 다음 회차에(64회 — 남으면 다음 단계로,
    // 횟수를 센다). 할당 실패는 남은 요청을 큐 앞에 되돌려 다음 적용이 잇는다(LC6d2 리뷰 B1).
    BeginEntry();
    struct Nested {
        VmSession& session;
        ~Nested() noexcept {
            session.nested_ = false;
            session.execution_.keepLogs_ = false;
        }
    } nested{*this};
    nested_ = true;
    execution_.keepLogs_ = true;
    ErrorCode failure = ErrorCode::None;
    uint32 round = 0;
    for (; round < MaxApplyRounds && !requests_.IsEmpty() && failure == ErrorCode::None; ++round) {
        applying_.Clear();
        if (!applying_.TryAppend(requests_.View())) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        requests_.Clear();
        auto live = [&](const Request& request) noexcept {
            return request.instance < instances_.Size() && instances_[request.instance].attached &&
                   instances_[request.instance].serial == request.serial &&
                   !instances_[request.instance].pendingDestroy;
        };
        usize done = 0;
        uint32 failedPass = 0;
        for (uint32 pass = 0; pass < 2 && failure == ErrorCode::None; ++pass) {
            for (usize at = 0; at < applying_.Size() && failure == ErrorCode::None; ++at) {
                const Request request = applying_[at];
                const bool destroy = request.kind == RequestKind::Destroy;
                if ((pass == 0) == destroy || !live(request)) {
                    continue;
                }

                Instance& entry = instances_[request.instance];
                Result<void> step = Result<void>::Success();
                if (destroy) {
                    // 정리 목록에 넣은 뒤 표지를 둔다(넣지 못하면 표지도 없다 — 리뷰 B2).
                    if (!destroys_.TryPushBack(request)) {
                        step = Result<void>::Failure(ErrorCode::OutOfMemory);
                    } else {
                        entry.pendingDestroy = true;
                    }
                } else if (entry.isolated) {
                    // 격리: 다시 활성화할 수 없다(SL 8.3절).
                } else if (request.kind == RequestKind::Enable && !entry.enabled) {
                    step = ApplyHook(request.instance, BytecodeHookKind::Enable);
                } else if (request.kind == RequestKind::Disable && entry.enabled) {
                    step = ApplyHook(request.instance, BytecodeHookKind::Disable);
                }

                if (!step) {
                    failure = step.Error();
                    done = at;
                    failedPass = pass;
                }
            }
        }

        if (failure != ErrorCode::None) {
            // 실패한 요청 뒤의 요청(그 회차에서 아직 적용하지 않은 것)을 큐 앞에 되돌린다.
            Array<Request> kept(*allocator_);
            // 활성 전환 pass에서 실패했으면 그 뒤 활성 전환과 모든 파괴를, 파괴 pass에서 실패했으면
            // 그 파괴부터를 되돌린다(실패한 hook은 다시 돌리지 않는다 — 넣지 못한 파괴는 되돌린다).
            for (usize at = 0; at < applying_.Size(); ++at) {
                const bool destroy = applying_[at].kind == RequestKind::Destroy;
                const bool requeue = failedPass == 0 ? destroy || at > done : destroy && at >= done;
                if (requeue && live(applying_[at]) && !kept.TryPushBack(applying_[at])) {
                    break; // 되돌리지 못한 요청은 버린다(할당 실패 — 보고는 Result)
                }
            }

            static_cast<void>(kept.TryAppend(requests_.View()));
            requests_.Clear();
            static_cast<void>(requests_.TryAppend(kept.View()));
        }
    }

    if (round == MaxApplyRounds && !requests_.IsEmpty()) {
        ++deferredApplies_; // 64회 안에 끝나지 않았다(다음 단계로 — 보고)
    }

    return failure == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failure);
}

Result<void> VmSession::EndFrame() noexcept {
    if (closed_ || execution_.running_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 프레임 끝 정리(SL 4.3절 — 정리 목록이 한 묶음): Disable 전부(활성이면) → Destroy 전부 →
    // Detach 전부. 엔진이 연 진입이라 새 스크립트 단계다(이월 항목 포함). hook의 fault·예산
    // 초과·할당 실패와 무관하게 끝까지 하고 첫 오류를 돌려준다(LC6d2 리뷰 B3).
    BeginEntry();
    struct Nested {
        VmSession& session;
        ~Nested() noexcept {
            session.nested_ = false;
            session.execution_.keepLogs_ = false;
        }
    } nested{*this};
    nested_ = true;
    execution_.keepLogs_ = true;
    deliveredInPhase_ = 0;
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        queue_[at].carried = true;
    }

    ErrorCode failure = ErrorCode::None;
    auto note = [&](const Result<void>& step) noexcept {
        if (!step && failure == ErrorCode::None) {
            failure = step.Error();
        }
    };
    auto live = [&](const Request& request) noexcept {
        return request.instance < instances_.Size() && instances_[request.instance].attached &&
               instances_[request.instance].serial == request.serial;
    };
    for (const Request& request : destroys_) {
        if (live(request) && instances_[request.instance].enabled) {
            note(ApplyHook(request.instance, BytecodeHookKind::Disable));
        }
    }

    for (const Request& request : destroys_) {
        if (live(request)) {
            note(ApplyHook(request.instance, BytecodeHookKind::Destroy));
        }
    }

    for (const Request& request : destroys_) {
        if (live(request)) {
            note(Detach(request.instance));
        }
    }

    destroys_.Clear();
    return failure == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(failure);
}

Result<void> VmSession::CloseSession() noexcept {
    if (execution_.running_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // admission 차단 → 모든 instance Detach(정지 실행 취소·유지 자원·연결) → 큐·Changed 자리 폐기 →
    // 연결·대기 표 해제(SL 9.1절). 사용자 코드를 돌리지 않는다.
    closed_ = true;
    DetachAll();
    for (uint32 at = 0; at < suspended_.Size(); ++at) {
        if (suspended_[at].live) {
            CancelSuspended(at, false); // instance 없는 실행(안전장치)
        }
    }

    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        ReleaseItem(queue_[at]);
    }

    queue_.Clear();
    queueHead_ = 0;
    itemValues_.Clear();
    itemBuffers_.Clear();
    for (VmBuffer* buffer : writeBuffers_) {
        execution_.Release(buffer);
    }

    writes_.Clear();
    writeValues_.Clear();
    writeBuffers_.Clear();
    connections_.Clear();
    waits_.Clear();
    freeWaits_.Clear();
    fragmentWaits_.Clear();
    requests_.Clear();
    destroys_.Clear();
    deliveries_.Clear(); // 모듈 포인터를 남기지 않는다(닫은 뒤 모듈을 먼저 놓아도 된다)
    faults_.Clear();
    execution_.logs_.Clear();
    execution_.logText_.Clear();
    execution_.Unwind();
    execution_.ReleaseReturned();
    return Result<void>::Success();
}

VmSession::InstanceState VmSession::StateOf(uint32 instance) const noexcept {
    if (instance >= instances_.Size() || !instances_[instance].attached) {
        return InstanceState::Normal;
    }

    const Instance& entry = instances_[instance];
    const bool released = entry.tickStopModule != nullptr && entry.tickStopModule->Linked() &&
                          entry.tickStopModule->generation_ != entry.tickStopGeneration;
    return entry.isolated                    ? InstanceState::Isolated
           : entry.ticksStopped && !released ? InstanceState::TicksStopped
                                             : InstanceState::Normal;
}

void VmSession::RecordFault(const VmFault& fault, uint32 instance) noexcept {
    const uint64 generation = fault.module != nullptr ? fault.module->generation_ : 0;
    for (FaultRecord& record : faults_) {
        if (record.module == fault.module && record.generation == generation &&
            record.function == fault.function && record.address == fault.address &&
            record.kind == fault.kind) {
            ++record.count;
            return;
        }
    }

    FaultRecord record;
    record.module = fault.module;
    record.generation = generation;
    record.location = fault.location;
    record.instance = instance < instances_.Size() ? instances_[instance].handle : 0;
    record.function = fault.function;
    record.address = fault.address;
    record.kind = fault.kind;
    record.count = 1;
    static_cast<void>(faults_.TryPushBack(record)); // 기록 실패는 기록만 버린다
}

void VmSession::NoteFault(uint32 instance, const VmCallResult& result, bool counts) noexcept {
    RecordFault(result.fault, instance);
    if (!counts || instance >= instances_.Size() || !instances_[instance].attached ||
        instances_[instance].isolated) {
        return;
    }

    // 직전 IsolationWindowFrames 프레임(일시 정지 제외, 경계 포함) 안의 세 번째 fault면
    // 격리(SL 8.3절 SD42). 고리에는 마지막 세 fault의 프레임이 있다.
    Instance& entry = instances_[instance];
    entry.faultFrames[entry.faultCount % IsolationFaults] = activeFrame_;
    ++entry.faultCount;
    if (entry.faultCount >= IsolationFaults) {
        const uint64 oldest = entry.faultFrames[entry.faultCount % IsolationFaults];
        if (activeFrame_ - oldest <= IsolationWindowFrames) {
            Isolate(instance);
        }
    }
}

void VmSession::Isolate(uint32 instance) noexcept {
    Instance& entry = instances_[instance];
    if (entry.isolated) {
        return;
    }

    entry.isolated = true;
    entry.enabled = false;
    CancelExecutions(instance, true);
    ReleaseHeldFields(instance);
    Disconnect(instance);
    DropWrites(instance);
    EndEventWaits(entry.handle);
}

VmLimits VmSession::FragmentLimits(bool teardown) const noexcept {
    VmLimits limits;
    limits.instructions = teardown ? budgets_.teardownInstructions : budgets_.fragmentInstructions;
    limits.callDepth = budgets_.callDepth;
    limits.memoryBytes = budgets_.sessionMemory; // 실행 문맥 ledger는 session 하나다(R1)
    return limits;
}

bool VmSession::ExecutionsFull(uint32 instance) const noexcept {
    return instance < instances_.Size() &&
           instances_[instance].suspendedCount >= budgets_.executionsPerInstance;
}

Result<void> VmSession::SetBudgets(const VmSessionBudgets& budgets) noexcept {
    // 0인 상한은 뜻이 항목마다 달라지므로 받지 않는다(LC6d4 리뷰 S3).
    if (execution_.running_ || budgets.fragmentInstructions == 0 ||
        budgets.teardownInstructions == 0 || budgets.callDepth == 0 || budgets.sessionMemory == 0 ||
        budgets.instanceMemory == 0 || budgets.connectionsPerInstance == 0 ||
        budgets.connectionsPerSession == 0 || budgets.executionsPerInstance == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    budgets_ = budgets;
    return Result<void>::Success();
}

namespace {

// 버퍼의 논리 크기(원소 수 × 원소 크기 — SL 9.3절, LC6d4 리뷰 B1): 머리·여유 용량·해시 색인은 세지
// 않는다. 문자열은 byte 수, 컨테이너는 원소마다 key·값 slot, struct·Result는 slot 수다.
uint64 VmLogicalBytes(const VmBuffer* buffer) noexcept {
    if (buffer == nullptr) {
        return 0;
    }

    constexpr uint64 SlotBytes = sizeof(uint64);
    switch (buffer->kind) {
    case VmBufferKind::String:
        return buffer->count;
    case VmBufferKind::Struct:
    case VmBufferKind::Result:
        return uint64(buffer->count) * SlotBytes;
    default:
        return uint64(buffer->count) * (uint64(buffer->keySlots) + buffer->valueSlots) * SlotBytes;
    }
}

} // namespace

bool VmSession::FitsInstanceMemory(uint32 instance, uint32 at,
                                   const VmBuffer* incoming) const noexcept {
    // instance 필드가 직접 쥔 버퍼의 크기 합(원소 버퍼는 바깥 버퍼에 원소 크기로만 든다 — R22
    // 근사).
    uint64 total = VmLogicalBytes(incoming);
    const Array<VmBuffer*>& buffers = instances_[instance].buffers;
    for (usize slot = 0; slot < buffers.Size(); ++slot) {
        if (slot != at) {
            total += VmLogicalBytes(buffers[slot]);
        }
    }

    return total <= budgets_.instanceMemory;
}

} // namespace rocks::sparks
