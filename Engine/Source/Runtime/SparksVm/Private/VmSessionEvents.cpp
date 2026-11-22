#include <Platform/Memory.h>
#include <SparksVm/VmSession.h>

// 발행·전달의 구현이다(Docs/Design/SparksRuntime.ko.md 2.3절, LC6b): 연결 표, 발행 큐(FIFO·깊이·
// 단계 전달 상한·이월), Changed(조각의 시작 값과 끝 값 비교), handler 실행.

namespace rocks::sparks {

namespace {

constexpr uint32 VmEventScalarBank = static_cast<uint32>(BytecodeBank::Scalar);
constexpr uint32 VmEventRefBank = static_cast<uint32>(BytecodeBank::Ref);
constexpr uint32 VmEventBufBank = static_cast<uint32>(BytecodeBank::Buf);
constexpr uint32 VmEventMaxArguments = 256;

VmBuffer* VmEventChild(uint64 bits) noexcept {
    return reinterpret_cast<VmBuffer*>(static_cast<usize>(bits));
}

// 값 비교(SL 5.3절): 문자열 byte, 컨테이너·struct·Result 원소별(Buf 원소는 다시 비교), 그 밖은
// 비트.
bool VmValuesEqual(const VmBuffer* left, const VmBuffer* right) noexcept {
    if (left == right) {
        return true;
    }

    if (left == nullptr || right == nullptr || left->kind != right->kind ||
        left->count != right->count) {
        return false;
    }

    if (left->kind == VmBufferKind::String) {
        return VmStringOf(left) == VmStringOf(right);
    }

    if (left->keyBank != right->keyBank || left->keySlots != right->keySlots ||
        left->valueBank != right->valueBank || left->valueSlots != right->valueSlots ||
        (left->kind == VmBufferKind::Struct &&
         (left->capacity != right->capacity || left->indexCapacity != right->indexCapacity))) {
        return false;
    }

    const uint64* a = reinterpret_cast<const uint64*>(left->Bytes());
    const uint64* b = reinterpret_cast<const uint64*>(right->Bytes());
    if (left->kind == VmBufferKind::Struct || left->kind == VmBufferKind::Result) {
        // struct: 끝의 capacity slot이 Buf, Result: 값 bank가 Buf면 1번부터.
        const uint32 firstBuffer = left->kind == VmBufferKind::Struct ? left->count - left->capacity
                                   : left->valueBank == VmEventBufBank ? 1
                                                                       : left->count;
        for (uint32 slot = 0; slot < left->count; ++slot) {
            const bool buffer = slot >= firstBuffer;
            if (buffer ? !VmValuesEqual(VmEventChild(a[slot]), VmEventChild(b[slot]))
                       : a[slot] != b[slot]) {
                return false;
            }
        }

        return true;
    }

    // 컨테이너: 원소마다 key slot 뒤 값 slot(Set은 key만, Array는 값만).
    const uint32 stride = uint32(left->keySlots) + left->valueSlots;
    for (uint32 element = 0; element < left->count; ++element) {
        for (uint32 slot = 0; slot < stride; ++slot) {
            const uint64 at = uint64(element) * stride + slot;
            const bool key = slot < left->keySlots;
            const bool buffer = (key ? left->keyBank : left->valueBank) == VmEventBufBank;
            if (buffer ? !VmValuesEqual(VmEventChild(a[at]), VmEventChild(b[at]))
                       : a[at] != b[at]) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

uint32 VmSession::QueuedCount() const noexcept {
    return static_cast<uint32>(queue_.Size() - queueHead_);
}

void VmSession::BeginEntry() noexcept {
    // 진입 하나의 기록(로그·전달)을 모은다.
    execution_.logs_.Clear();
    execution_.logText_.Clear();
    execution_.droppedLogs_ = 0;
    deliveries_.Clear();
}

void VmSession::ReleaseItem(Item& item) noexcept {
    for (uint32 at = item.firstBuffer; at < item.firstBuffer + item.bufferCount; ++at) {
        execution_.Release(itemBuffers_[at]);
        itemBuffers_[at] = nullptr;
    }

    item.bufferCount = 0;
}

VmFaultKind VmSession::Enqueue(uint64 publisher, const VmEventKey& key,
                               const VmLinkedModule* module, uint32 eventRef, uint32 depth,
                               uint64 root, Span<const uint64> scalars, Span<const uint64> refs,
                               Span<VmBuffer* const> buffers, ErrorCode& error) noexcept {
    if (depth > MaxCascadeDepth) {
        // 같은 루트의 미전달 항목을 폐기한다(SL 5.4절).
        for (usize at = queueHead_; at < queue_.Size(); ++at) {
            if (queue_[at].root == root) {
                ReleaseItem(queue_[at]);
                queue_[at].publisher = 0; // 전달하지 않는다
            }
        }

        return VmFaultKind::CascadeLimit;
    }

    if (QueuedCount() >= MaxQueuedItems) {
        return VmFaultKind::BudgetExceeded;
    }

    Item item;
    item.publisher = publisher;
    item.key = key;
    item.module = module;
    item.eventRef = eventRef;
    item.depth = depth;
    item.root = root;
    item.firstScalar = static_cast<uint32>(itemValues_.Size());
    item.scalarCount = static_cast<uint32>(scalars.Size());
    item.firstRef = item.firstScalar + item.scalarCount;
    item.refCount = static_cast<uint32>(refs.Size());
    item.firstBuffer = static_cast<uint32>(itemBuffers_.Size());
    item.bufferCount = static_cast<uint32>(buffers.Size());
    if (!itemValues_.TryAppend(scalars) || !itemValues_.TryAppend(refs) ||
        !itemBuffers_.TryAppend(buffers) || !queue_.TryPushBack(item)) {
        error = ErrorCode::OutOfMemory;
        static_cast<void>(itemValues_.TryResize(item.firstScalar));
        static_cast<void>(itemBuffers_.TryResize(item.firstBuffer));
        return VmFaultKind::None;
    }

    for (VmBuffer* buffer : buffers) {
        execution_.Retain(buffer); // 항목이 참조를 가진다
    }

    return VmFaultKind::None;
}

VmFaultKind VmSession::EnqueueFromScript(const VmLinkedModule& module, uint32 eventRef,
                                         Span<const uint64> scalars, Span<const uint64> refs,
                                         Span<VmBuffer* const> buffers, ErrorCode& error) noexcept {
    // Ref 0번이 발행자, 그 뒤가 인자다. 깊이는 지금 조각의 깊이 + 1.
    if (refs.IsEmpty() || refs[0] == 0) {
        return VmFaultKind::ContractViolation;
    }

    const uint64 root = currentRoot_ == 0 ? nextRoot_++ : currentRoot_;
    if (currentRoot_ == 0) {
        currentRoot_ = root;
    }

    return Enqueue(refs[0], module.keys_[eventRef], &module, eventRef, currentDepth_ + 1, root,
                   scalars, Span<const uint64>(refs.Data() + 1, refs.Size() - 1), buffers, error);
}

void VmSession::NoteWrite(uint32 instance, const VmLinkedModule& module, uint32 fieldRef,
                          uint32 bank, uint32 at, uint32 slots) noexcept {
    for (const Write& write : writes_) {
        if (write.instance == instance && write.bank == bank && write.at == at) {
            return; // 조각에서 처음 쓴 것만(시작 값)
        }
    }

    Instance& entry = instances_[instance];
    Write write;
    write.instance = instance;
    write.module = &module;
    write.fieldRef = fieldRef;
    write.bank = bank;
    write.at = at;
    write.slots = slots;
    if (bank == VmEventBufBank) {
        write.firstValue = static_cast<uint32>(writeBuffers_.Size());
        VmBuffer* start = entry.buffers[at];
        if (!writeBuffers_.TryPushBack(start)) {
            return; // 기록 못 하면 Changed를 낼 수 없다(할당 실패 — 다음 조각에서 다시 기록)
        }

        execution_.Retain(start);
    } else {
        write.firstValue = static_cast<uint32>(writeValues_.Size());
        const Array<uint64>& values = bank == VmEventScalarBank ? entry.scalars : entry.refs;
        if (!writeValues_.TryAppend(Span<const uint64>(values.Data() + at, slots))) {
            return;
        }
    }

    if (!writes_.TryPushBack(write)) {
        if (bank == VmEventBufBank) {
            execution_.Release(writeBuffers_[write.firstValue]);
            static_cast<void>(writeBuffers_.TryResize(write.firstValue));
        }
    }
}

Result<void> VmSession::EndFragment() noexcept {
    // Changed: 시작 값과 끝 값이 다르면 항목을 넣는다(억제 문맥이 아니면). 원천 필드가 바뀐 연결은
    // 다시 푼다.
    Result<void> status = Result<void>::Success();
    for (usize index = 0; index < writes_.Size(); ++index) {
        const Write write = writes_[index];
        if (write.instance >= instances_.Size() || !instances_[write.instance].attached) {
            if (write.bank == VmEventBufBank) {
                execution_.Release(writeBuffers_[write.firstValue]);
            }

            continue;
        }

        Instance& entry = instances_[write.instance];
        bool same = true;
        if (write.bank == VmEventBufBank) {
            VmBuffer* start = writeBuffers_[write.firstValue];
            same = VmValuesEqual(start, entry.buffers[write.at]);
            execution_.Release(start);
        } else {
            const Array<uint64>& values =
                write.bank == VmEventScalarBank ? entry.scalars : entry.refs;
            for (uint32 slot = 0; slot < write.slots && same; ++slot) {
                same = values[write.at + slot] == writeValues_[write.firstValue + slot];
            }
        }

        if (!same && !suppressChanged_ && status) {
            // key: 필드 key + `.Changed`(이름은 모듈 문자열과 붙여야 하므로 연결 쪽은 같은 규칙의
            // 필드 key로 맞춘다 — Changed 연결의 key.name은 필드 이름, path는 필드 key 경로이고
            // 전달 대조는 eventRef NoIndex로 Changed를 가린다).
            VmEventKey key = write.module->keys_[write.fieldRef];
            ErrorCode error = ErrorCode::None;
            const uint64 root = currentRoot_ == 0 ? nextRoot_++ : currentRoot_;
            const VmFaultKind fault =
                Enqueue(entry.handle, key, write.module, NoBytecodeIndex, currentDepth_ + 1, root,
                        Span<const uint64>(), Span<const uint64>(), Span<VmBuffer* const>(), error);
            if (error != ErrorCode::None) {
                status = Result<void>::Failure(error);
            }

            static_cast<void>(fault); // Changed의 연쇄 상한은 그 조각의 fault로 보고하지 않는다
        }

        // 경로 원천 필드가 바뀌었으면 활성 instance의 그 handler 연결을 다시 푼다(SL 5.5절 — 그
        // 필드를 경로 첫 마디로 쓰는 handler, 원천이 처음 채워진 경우 포함).
        if (!same && status && entry.enabled) {
            const BytecodeModule& bytecode = write.module->Module();
            for (uint32 index = 0; index < bytecode.handlers.Size() && status; ++index) {
                const BytecodeHandler& handler = bytecode.handlers[index];
                if (handler.pathCount > 0 &&
                    bytecode.handlerPaths[handler.firstPath] == write.fieldRef) {
                    status = Connect(write.instance, write.module, index);
                }
            }
        }
    }

    writes_.Clear();
    writeValues_.Clear();
    writeBuffers_.Clear();
    return status;
}

Result<void> VmSession::ResolvePath(uint32 instance, const VmLinkedModule& module, uint32 firstPath,
                                    uint32 pathCount, Array<uint64>& publishers,
                                    bool& fromArray) noexcept {
    publishers.Clear();
    fromArray = false;
    if (!publishers.TryPushBack(instances_[instance].handle)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    Array<uint64> next(*allocator_);
    for (uint32 step = firstPath; step < firstPath + pathCount; ++step) {
        const uint32 ref = module.Module().handlerPaths[step];
        const BytecodeRef& entry = module.Module().refs[ref];
        next.Clear();
        fromArray = false;
        if (entry.kind == BytecodeRefKind::Context) {
            // 문맥 객체(host).
            if (host_ == nullptr) {
                publishers.Clear();
                return Result<void>::Success();
            }

            uint64 handle = 0;
            VmHostCall call;
            call.execution_ = &execution_;
            call.binding_ = module.hosts_[ref].id;
            call.refs_ = Span<uint64>(&handle, 1);
            if (host_->GetContext(call) != VmFaultKind::None || call.error_ != ErrorCode::None) {
                publishers.Clear();
                return call.error_ == ErrorCode::None ? Result<void>::Success()
                                                      : Result<void>::Failure(call.error_);
            }

            if (!next.TryPushBack(handle)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else {
            for (const uint64 object : publishers) {
                if (object == 0 || host_ == nullptr || !host_->IsAlive(object)) {
                    continue;
                }

                // 로컬 필드는 instance 저장소, 엔진 필드는 host.
                const VmLinkedModule::LocalField& local = module.localFields_[ref];
                uint64 value = 0;
                VmBuffer* container = nullptr;
                if (local.classIndex != NoBytecodeIndex) {
                    uint64* values = nullptr;
                    VmBuffer** buffers = nullptr;
                    uint32 owner = NoBytecodeIndex;
                    uint32 where = 0;
                    if (!FieldSlots(object, module, local.classIndex, local.bank, local.offset,
                                    local.slots, values, buffers, owner, where)) {
                        continue;
                    }

                    value = local.bank == VmEventRefBank ? values[0] : 0;
                    container = local.bank == VmEventBufBank ? buffers[0] : nullptr;
                } else if (module.hosts_[ref].bank == VmEventRefBank) {
                    VmHostCall call;
                    call.execution_ = &execution_;
                    call.binding_ = module.hosts_[ref].id;
                    call.refs_ = Span<uint64>(&value, 1);
                    if (host_->GetField(call, object) != VmFaultKind::None ||
                        call.error_ != ErrorCode::None) {
                        value = 0;
                    }
                }

                if (container != nullptr && container->kind == VmBufferKind::Array &&
                    container->valueBank == VmEventRefBank && container->valueSlots == 1) {
                    // 배열 원천: 원소마다(같은 객체는 하나 — SL 5.5절).
                    fromArray = true;
                    const uint64* elements = reinterpret_cast<const uint64*>(container->Bytes());
                    for (uint32 element = 0; element < container->count; ++element) {
                        bool seen = false;
                        for (const uint64 known : next) {
                            seen = seen || known == elements[element];
                        }

                        if (!seen && elements[element] != 0 &&
                            !next.TryPushBack(elements[element])) {
                            return Result<void>::Failure(ErrorCode::OutOfMemory);
                        }
                    }
                } else if (value != 0 && !next.TryPushBack(value)) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }
        }

        publishers.Clear();
        if (!publishers.TryAppend(next.View())) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

Result<void> VmSession::Connect(uint32 instance, const VmLinkedModule* only,
                                uint32 onlyHandler) noexcept {
    // instance의 클래스 사슬(기반 먼저)의 handler마다 경로를 풀어 연결을 맞춘다: 남는 발행자의
    // 항목은 순번을 지키고, 사라진 것은 지우고, 새것은 끝에 더한다.
    const VmLinkedModule* chainModules[64] = {};
    uint32 chainClasses[64] = {};
    uint32 depth = 0;
    const VmLinkedModule* current = instances_[instance].module;
    uint32 currentClass = instances_[instance].classIndex;
    while (depth < 64) {
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

    Array<uint64> publishers(*allocator_);
    for (uint32 at = depth; at > 0; --at) {
        const VmLinkedModule& module = *chainModules[at - 1];
        const BytecodeModule& bytecode = module.Module();
        const BytecodeClass& entry = bytecode.classes[chainClasses[at - 1]];
        for (uint32 index = entry.firstHandler; index < entry.firstHandler + entry.handlerCount;
             ++index) {
            if ((only != nullptr && (&module != only || index != onlyHandler))) {
                continue;
            }

            const BytecodeHandler& handler = bytecode.handlers[index];
            const bool changed = handler.eventRef == NoBytecodeIndex && handler.pathCount > 0 &&
                                 bytecode.String(handler.event) == "Changed";
            if (handler.eventRef == NoBytecodeIndex && !changed) {
                continue; // 풀지 못한 경로(widget 등 — 연동 단계)
            }

            // Changed는 마지막 마디가 감시 필드: 발행자는 그 앞까지.
            const uint32 sourceCount = changed ? handler.pathCount - 1 : handler.pathCount;
            bool fromArray = false;
            if (auto resolved = ResolvePath(instance, module, handler.firstPath, sourceCount,
                                            publishers, fromArray);
                !resolved) {
                return resolved;
            }

            const VmEventKey key =
                changed ? module.keys_[bytecode.handlerPaths[handler.firstPath + sourceCount]]
                        : module.keys_[handler.eventRef];
            // 사라진 발행자의 항목을 지운다.
            usize kept = 0;
            for (usize at2 = 0; at2 < connections_.Size(); ++at2) {
                Connection& connection = connections_[at2];
                bool remove = false;
                if (connection.subscriber == instance && connection.module == &module &&
                    connection.handler == index) {
                    remove = true;
                    for (const uint64 publisher : publishers) {
                        remove = remove && publisher != connection.publisher;
                    }
                }

                if (!remove) {
                    connections_[kept++] = connection;
                }
            }

            static_cast<void>(connections_.TryResize(kept));
            // 새 발행자를 끝에 더한다.
            for (const uint64 publisher : publishers) {
                bool exists = false;
                for (const Connection& connection : connections_) {
                    exists = exists ||
                             (connection.subscriber == instance && connection.module == &module &&
                              connection.handler == index && connection.publisher == publisher);
                }

                if (exists) {
                    continue;
                }

                Connection connection;
                connection.publisher = publisher;
                connection.key = key;
                connection.subscriber = instance;
                connection.module = &module;
                connection.handler = index;
                connection.fromArray = fromArray;
                connection.order = nextOrder_++;
                if (!connections_.TryPushBack(connection)) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }
        }
    }

    return Result<void>::Success();
}

void VmSession::Disconnect(uint32 instance) noexcept {
    // 구독자가 instance이거나 발행자가 그 handle인 항목을 지운다(SL 5.5절).
    const uint64 handle = instances_[instance].handle;
    usize kept = 0;
    for (usize at = 0; at < connections_.Size(); ++at) {
        const Connection& connection = connections_[at];
        if (connection.subscriber != instance && connection.publisher != handle) {
            connections_[kept++] = connection;
        }
    }

    static_cast<void>(connections_.TryResize(kept));
}

Result<void> VmSession::Drain() noexcept {
    // FIFO: 항목 하나를 꺼내 그때 활성인 연결을 순번대로 실행한 뒤 다음 항목(SL 5.4절).
    if (auto ended = EndFragment(); !ended) {
        return ended;
    }

    while (queueHead_ < queue_.Size()) {
        if (deliveredInPhase_ >= MaxDeliveriesPerPhase) {
            for (usize at = queueHead_; at < queue_.Size(); ++at) {
                queue_[at].carried = true; // 다음 단계로 이월
            }

            break;
        }

        Item item = queue_[queueHead_++];
        if (item.publisher == 0) {
            continue; // 폐기된 항목(연쇄 상한)
        }

        ++deliveredInPhase_;
        // 연결 표를 순번대로(전달 중 바뀔 수 있으므로 번호로 다시 읽는다).
        uint64 lastOrder = 0;
        for (;;) {
            usize chosen = connections_.Size();
            for (usize at = 0; at < connections_.Size(); ++at) {
                const Connection& connection = connections_[at];
                if (connection.active && connection.order > lastOrder &&
                    connection.publisher == item.publisher && connection.key == item.key &&
                    (chosen == connections_.Size() ||
                     connection.order < connections_[chosen].order)) {
                    chosen = at;
                }
            }

            if (chosen == connections_.Size()) {
                break;
            }

            const Connection connection = connections_[chosen];
            lastOrder = connection.order;
            const BytecodeModule& bytecode = connection.module->Module();
            const BytecodeHandler& handler = bytecode.handlers[connection.handler];
            const BytecodeFunction& function = bytecode.functions[handler.function];
            // 재진입(SL 6.1절 SD11): 같은 발행자로 시작된 이 handler 실행이 정지해 있으면
            // `@skipWhileBusy`는 새 이벤트를 버리고(횟수를 센다) `@restart`는 그 실행을 취소한다.
            if (handler.reentry != BytecodeReentry::None) {
                const uint32 busy = FindBusy(connection.subscriber, connection.module,
                                             connection.handler, item.publisher);
                if (busy != NoBytecodeIndex) {
                    if (handler.reentry == BytecodeReentry::SkipWhileBusy) {
                        ++skippedWhileBusy_;
                        continue;
                    }

                    CancelSuspended(busy);
                }
            }

            // 인자: handler가 선언한 만큼 앞에서부터(배열 원천은 발행한 원소가 첫 Ref 인자).
            uint64 refs[VmEventMaxArguments] = {};
            uint32 refCount = 0;
            refs[refCount++] = instances_[connection.subscriber].handle;
            if (connection.fromArray && refCount < function.parameters[VmEventRefBank]) {
                refs[refCount++] = item.publisher;
            }

            for (uint32 at = 0;
                 at < item.refCount && refCount < function.parameters[VmEventRefBank] &&
                 refCount < VmEventMaxArguments;
                 ++at) {
                // 이월 항목의 죽은 객체 인자는 null로 전달한다(T? — non-null 인자의 폐기는
                // 컴파일러 타입 정보가 필요해 LC6d에서 정한다).
                const uint64 value = itemValues_[item.firstRef + at];
                refs[refCount++] =
                    item.carried && value != 0 && (host_ == nullptr || !host_->IsAlive(value))
                        ? 0
                        : value;
            }

            const uint32 scalarCount = function.parameters[VmEventScalarBank] < item.scalarCount
                                           ? function.parameters[VmEventScalarBank]
                                           : item.scalarCount;
            const uint32 bufferCount = function.parameters[VmEventBufBank] < item.bufferCount
                                           ? function.parameters[VmEventBufBank]
                                           : item.bufferCount;
            currentDepth_ = item.depth;
            currentRoot_ = item.root;
            const uint32 outerInstance = currentInstance_;
            currentInstance_ = connection.subscriber;
            currentPublisher_ = item.publisher;
            currentHandlerModule_ = connection.module;
            currentHandler_ = connection.handler;
            auto called = execution_.Call(
                *connection.module, handler.function,
                Span<const uint64>(itemValues_.Data() + item.firstScalar, scalarCount),
                Span<const uint64>(refs, refCount),
                Span<VmBuffer* const>(itemBuffers_.Data() + item.firstBuffer, bufferCount));
            currentInstance_ = outerInstance;
            currentPublisher_ = 0;
            currentHandlerModule_ = nullptr;
            currentHandler_ = NoBytecodeIndex;
            currentDepth_ = 0;
            currentRoot_ = 0;
            if (!called) {
                ReleaseItem(item);
                return Result<void>::Failure(called.Error());
            }

            Delivery delivery;
            delivery.subscriber = instances_[connection.subscriber].handle;
            delivery.module = connection.module;
            delivery.function = handler.function;
            delivery.result = called.Value();
            execution_.ReleaseReturned();
            if (!deliveries_.TryPushBack(delivery)) {
                ReleaseItem(item);
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            currentDepth_ = item.depth;
            currentRoot_ = item.root;
            auto ended = EndFragment();
            currentDepth_ = 0;
            currentRoot_ = 0;
            if (!ended) {
                ReleaseItem(item);
                return ended;
            }
        }

        ReleaseItem(item);
    }

    // 다 비웠으면 저장소를 앞으로 당긴다.
    if (queueHead_ == queue_.Size()) {
        queue_.Clear();
        queueHead_ = 0;
        itemValues_.Clear();
        itemBuffers_.Clear();
    }

    return Result<void>::Success();
}

Result<void> VmSession::OpenPhase() noexcept {
    BeginEntry();
    execution_.keepLogs_ = true;
    deliveredInPhase_ = 0;
    for (usize at = queueHead_; at < queue_.Size(); ++at) {
        queue_[at].carried = true;
    }

    auto drained = Drain();
    execution_.keepLogs_ = false;
    return drained;
}

Result<void> VmSession::Emit(uint64 publisher, const VmEventKey& key, Span<const uint64> scalars,
                             Span<const uint64> refs, Span<VmBuffer* const> buffers) noexcept {
    BeginEntry();
    execution_.keepLogs_ = true;
    ErrorCode error = ErrorCode::None;
    const VmFaultKind fault = Enqueue(publisher, key, nullptr, NoBytecodeIndex, 1, nextRoot_++,
                                      scalars, refs, buffers, error);
    Result<void> result = error != ErrorCode::None ? Result<void>::Failure(error)
                          : fault != VmFaultKind::None
                              ? Result<void>::Failure(ErrorCode::CapacityExceeded)
                              : Drain();
    execution_.keepLogs_ = false;
    return result;
}

} // namespace rocks::sparks
