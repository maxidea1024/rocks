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
      writeBuffers_(allocator), deliveries_(allocator) {
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
    DetachAll();
}

Result<void> VmSession::AddModule(const VmLinkedModule& linked) noexcept {
    if (!linked.Linked() || linked.Host() != host_) {
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

    if (number < instances_.Size()) {
        instances_[number] = static_cast<Instance&&>(instance);
    } else if (auto pushed = instances_.TryPushBack(static_cast<Instance&&>(instance)); !pushed) {
        return Result<uint32>::Failure(pushed.Error());
    }

    if (auto inserted = byHandle_.Insert(handle, number); !inserted) {
        Detach(number);
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
    struct KeepLogs {
        VmSession& session;
        ~KeepLogs() noexcept {
            session.execution_.keepLogs_ = false;
            session.suppressChanged_ = false;
            static_cast<void>(session.EndFragment()); // 시작 값 기록을 놓는다(억제 중)
        }
    } keepLogs{*this};
    for (uint32 at = depth; at > 0; --at) {
        const BytecodeClass& link = chainModules[at - 1]->Module().classes[chainClasses[at - 1]];
        if (link.initializer == NoBytecodeIndex) {
            continue;
        }

        const uint64 self[] = {handle};
        auto called = execution_.Call(*chainModules[at - 1], link.initializer, {},
                                      Span<const uint64>(self, 1), {});
        if (!called) {
            Detach(number);
            return Result<uint32>::Failure(called.Error());
        }

        const uint64 steps = init.steps + called.Value().steps;
        init = called.Value();
        init.steps = steps;
        execution_.ReleaseReturned();
        if (init.outcome != VmOutcome::Returned) {
            Detach(number);
            return Result<uint32>::Success(VmNoInstance);
        }
    }

    return Result<uint32>::Success(number);
}

Result<VmCallResult> VmSession::RunHook(uint32 instance, BytecodeHookKind kind,
                                        Span<const uint64> scalars, Span<const uint64> refs,
                                        Span<VmBuffer* const> buffers) noexcept {
    if (instance >= instances_.Size() || !instances_[instance].attached) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
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

    VmCallResult result;
    // 사슬의 모든 호출 기록을 모은다. Awake·Enable·Disable·Destroy 문맥의 쓰기는 Changed를 내지
    // 않는다(SL 5.3절). Disable은 연결을 끄고(항목 유지) Enable은 연결을 풀어 켠다(SL 5.5절).
    BeginEntry();
    execution_.keepLogs_ = true;
    suppressChanged_ = kind == BytecodeHookKind::Awake || kind == BytecodeHookKind::Enable ||
                       kind == BytecodeHookKind::Disable || kind == BytecodeHookKind::Destroy;
    struct KeepLogs {
        VmSession& session;
        ~KeepLogs() noexcept {
            session.execution_.keepLogs_ = false;
            session.suppressChanged_ = false;
        }
    } keepLogs{*this};
    if (kind == BytecodeHookKind::Disable) {
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
            if (bytecode.hooks[hook].kind != kind) {
                continue;
            }

            auto called =
                execution_.Call(*chainModules[at - 1], bytecode.hooks[hook].function, scalars,
                                Span<const uint64>(refArguments, refs.Size() + 1), buffers);
            if (!called) {
                return called;
            }

            const uint64 steps = result.steps + called.Value().steps;
            result = called.Value();
            result.steps = steps;
            execution_.ReleaseReturned();
            if (result.outcome != VmOutcome::Returned) {
                if (auto drained = Drain(); !drained) {
                    return Result<VmCallResult>::Failure(drained.Error());
                }

                return Result<VmCallResult>::Success(result);
            }
        }
    }

    if (kind == BytecodeHookKind::Enable) {
        instances_[instance].enabled = true;
        for (Connection& connection : connections_) {
            connection.active = connection.subscriber == instance ? true : connection.active;
        }

        if (auto connected = Connect(instance, nullptr, NoBytecodeIndex); !connected) {
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
    if (instance >= instances_.Size() || !instances_[instance].attached ||
        refs.Size() + 1 > VmSessionMaxRefArguments) {
        return Result<VmCallResult>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 refArguments[VmSessionMaxRefArguments] = {};
    refArguments[0] = instances_[instance].handle;
    for (usize at = 0; at < refs.Size(); ++at) {
        refArguments[at + 1] = refs[at];
    }

    BeginEntry();
    execution_.keepLogs_ = true;
    struct KeepLogs {
        VmExecution& execution;
        ~KeepLogs() noexcept {
            execution.keepLogs_ = false;
        }
    } keepLogs{execution_};
    auto called = execution_.Call(module, function, scalars,
                                  Span<const uint64>(refArguments, refs.Size() + 1), buffers);
    if (!called) {
        return called;
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

void VmSession::ReleaseFields(Instance& instance) noexcept {
    for (VmBuffer*& buffer : instance.buffers) {
        execution_.Release(buffer);
        buffer = nullptr;
    }
}

void VmSession::Detach(uint32 instance) noexcept {
    if (instance >= instances_.Size() || !instances_[instance].attached) {
        return;
    }

    Disconnect(instance);
    Instance& entry = instances_[instance];
    ReleaseFields(entry);
    static_cast<void>(byHandle_.Erase(entry.handle));
    entry.attached = false;
    entry.scalars.Empty();
    entry.refs.Empty();
    entry.buffers.Empty();
}

void VmSession::DetachAll() noexcept {
    for (uint32 instance = 0; instance < instances_.Size(); ++instance) {
        Detach(instance);
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

const VmLinkedModule* VmSession::ModuleOf(uint32 instance) const noexcept {
    return instance < instances_.Size() ? instances_[instance].module : nullptr;
}

uint32 VmSession::ClassOf(uint32 instance) const noexcept {
    return instance < instances_.Size() ? instances_[instance].classIndex : VmNoInstance;
}

bool VmSession::FieldSlots(uint64 handle, const VmLinkedModule& module, uint32 classIndex,
                           uint32 bank, uint32 offset, uint32 slots, uint64*& values,
                           VmBuffer**& buffers, uint32& instanceOut, uint32& atOut) noexcept {
    const uint32 number = Find(handle);
    if (number == VmNoInstance) {
        return false;
    }

    // 필드의 클래스가 instance 클래스의 기반 사슬에 있어야 한다(다른 클래스 instance의 저장소를
    // 같은 위치로 읽지 않는다).
    Instance& instance = instances_[number];
    const VmLinkedModule* current = instance.module;
    uint32 currentClass = instance.classIndex;
    bool related = false;
    for (uint32 depth = 0; depth < VmSessionMaxBaseDepth && !related; ++depth) {
        related = current == &module && currentClass == classIndex;
        const VmLinkedModule* baseModule = nullptr;
        uint32 baseClass = NoBytecodeIndex;
        if (related || !BaseOf(*current, currentClass, baseModule, baseClass)) {
            break;
        }

        current = baseModule;
        currentClass = baseClass;
    }

    uint32 start[BytecodeBankCount] = {};
    if (bank >= BytecodeBankCount || !related || !RegionStart(module, classIndex, start)) {
        return false;
    }

    const usize at = usize(start[bank]) + offset;
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

} // namespace rocks::sparks
