#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>

#include <new>

// 생산자 경로다(Docs/Design/TraceSystem.ko.md 4.1절). 스레드마다 자기 블록에 잠금 없이 이어 쓰고,
// 다 쓴 기록의 끝을 published에 release로 공개한다. 블록을 받지 못하면 기다리지 않고 버리며 손실을
// 센다.
namespace rocks::detail {

using platform::AtomicExchange;
using platform::AtomicFetchAdd;
using platform::AtomicLoad;
using platform::AtomicStore;

constinit TraceSessionState* currentSession = nullptr;
constinit uint64 sessionSerialCounter = 0;

namespace {

struct ThreadSlot;

// 한 번이라도 기록한 모든 스레드의 TLS 칸 목록이다. Stop이 이 목록으로 진행 중인 기록을 기다린다.
// 목록 변경(스레드의 첫 기록·종료)은 드물어 spin 잠금으로 직렬화한다. 상수 초기화되는 전역이다.
struct SlotRegistry {
    uint32 lock = 0;
    ThreadSlot* head = nullptr;
};

constinit SlotRegistry slotRegistry;

void LockSlotRegistry() noexcept {
    while (AtomicExchange<MemoryOrder::Acquire>(&slotRegistry.lock, 1u) != 0) {
        platform::YieldThread();
    }
}

void UnlockSlotRegistry() noexcept {
    AtomicStore<MemoryOrder::Release>(&slotRegistry.lock, 0u);
}

void RetireThread(TraceSessionState& session, TraceThreadState& state) noexcept;

// 스레드의 TLS 칸이다. 세션 메모리가 아니라 스레드 수명에 묶여 있어, Stop이 아직 세션에 등록하지
// 않은 스레드의 진행 중 기록도 기다릴 수 있다.
//
// 기록 호출은 busy를 올린 뒤(seq_cst) 현재 세션을 읽고, 끝나면 내린다. Stop은 현재 세션을 null로 쓴
// 뒤(seq_cst) 모든 칸의 busy가 0이 되기를 기다린다. 기록 호출이 세션을 읽었다면 그 전에 올린 busy를
// Stop이 반드시 보므로, Stop이 세션 메모리를 해제할 때 그 세션을 쓰는 기록 호출은 없다.
struct ThreadSlot {
    // 진행 중인 기록 호출의 깊이다. 이 스레드만 쓰고 Stop이 읽는다.
    uint32 busy = 0;
    bool linked = false;
    // state가 속한 세션의 serial이다. 다르면 state는 이미 해제된 세션의 것이므로 보지 않는다.
    uint64 serial = 0;
    TraceThreadState* state = nullptr;
    ThreadSlot* previous = nullptr;
    ThreadSlot* next = nullptr;

    constexpr ThreadSlot() noexcept = default;
    ThreadSlot(const ThreadSlot&) = delete;
    ThreadSlot& operator=(const ThreadSlot&) = delete;

    // 스레드 종료(TLS 정리) 때 현재 세션의 스레드 상태를 은퇴로 표시한다(UE TlsBuffer와 같다).
    // writer가 남은 내용을 보낸 뒤 블록을 풀로 돌려준다.
    ~ThreadSlot() noexcept;

    uint32 Enter() noexcept {
        const uint32 depth = AtomicLoad<MemoryOrder::Relaxed>(&busy);
        AtomicStore<MemoryOrder::SequentiallyConsistent>(&busy, depth + 1);
        return depth;
    }

    void Leave(uint32 depth) noexcept {
        AtomicStore<MemoryOrder::Release>(&busy, depth);
    }

    void Link() noexcept {
        LockSlotRegistry();
        next = slotRegistry.head;
        if (next != nullptr) {
            next->previous = this;
        }

        slotRegistry.head = this;
        linked = true;
        UnlockSlotRegistry();
    }

    void Unlink() noexcept {
        LockSlotRegistry();
        if (previous != nullptr) {
            previous->next = next;
        } else {
            slotRegistry.head = next;
        }

        if (next != nullptr) {
            next->previous = previous;
        }

        previous = nullptr;
        next = nullptr;
        linked = false;
        UnlockSlotRegistry();
    }
};

ThreadSlot::~ThreadSlot() noexcept {
    if (!linked) {
        return;
    }

    const uint32 depth = Enter();
    TraceSessionState* session = AtomicLoad<MemoryOrder::SequentiallyConsistent>(&currentSession);
    if (session != nullptr && state != nullptr && serial == session->serial) {
        RetireThread(*session, *state);
    }

    Leave(depth);
    Unlink();
}

thread_local ThreadSlot currentSlot;

// ---- 블록 쓰기 ----

// 블록 사슬을 따라 byte를 쓰는 sink다. 다음 블록은 미리 확보해 header.next로 이어 둔다.
struct BlockSink {
    TraceBlock* block;
    usize offset;

    void Write(const void* source, usize bytes) noexcept {
        const auto* cursor = static_cast<const uint8*>(source);
        while (bytes != 0) {
            if (offset == TraceBlockLayout::PayloadBytes) {
                block = AtomicLoad<MemoryOrder::Relaxed>(&block->header.next);
                offset = 0;
            }

            const usize space = TraceBlockLayout::PayloadBytes - offset;
            const usize take = bytes < space ? bytes : space;
            platform::MemoryCopy(block->payload + offset, cursor, take);
            offset += take;
            cursor += take;
            bytes -= take;
        }
    }
};

void Publish(TraceThreadState& state, usize bytes) noexcept {
    state.total += bytes;
    AtomicStore<MemoryOrder::Release>(&state.published, state.total);
}

// 블록을 이어 붙인다. 새 블록의 startTotal을 먼저 쓰고 연결을 release로 공개한다. 연결된 앞
// 블록에는 더 쓰지 않으므로 writer가 다 읽으면 풀로 돌려준다.
void LinkBlock(TraceThreadState& state, TraceBlock* block) noexcept {
    if (state.block != nullptr) {
        AtomicStore<MemoryOrder::Release>(&state.block->header.next, block);
    } else {
        AtomicStore<MemoryOrder::Release>(&state.firstBlock, block);
    }
}

bool AdvanceBlock(TraceBlockPool& pool, TraceThreadState& state) noexcept {
    TraceBlock* block = pool.TryAcquire();
    if (block == nullptr) {
        return false;
    }

    block->header.startTotal = state.total;
    AtomicStore<MemoryOrder::Relaxed>(&block->header.next, static_cast<TraceBlock*>(nullptr));
    LinkBlock(state, block);
    state.block = block;
    state.offset = 0;
    return true;
}

// 블록 하나보다 큰 기록(큰 가변부)을 여러 블록에 나눠 쓴다(UE Field.cpp와 같다). 필요한 블록을 모두
// 먼저 확보하고, 하나라도 못 받으면 받은 블록을 돌려주고 기록 전체를 버린다(일부만 쓰지 않는다).
bool AppendSpanning(TraceBlockPool& pool, TraceThreadState& state, const TraceRecordShape& shape,
                    usize bytes, usize remaining) noexcept {
    const usize extraBlocks =
        (bytes - remaining + TraceBlockLayout::PayloadBytes - 1) / TraceBlockLayout::PayloadBytes;
    TraceBlock* first = nullptr;
    TraceBlock* last = nullptr;
    uint64 start = state.total + remaining;
    for (usize index = 0; index < extraBlocks; ++index) {
        TraceBlock* block = pool.TryAcquire();
        if (block == nullptr) {
            while (first != nullptr) {
                TraceBlock* next = AtomicLoad<MemoryOrder::Relaxed>(&first->header.next);
                pool.Free(first);
                first = next;
            }

            return false;
        }

        block->header.startTotal = start;
        start += TraceBlockLayout::PayloadBytes;
        AtomicStore<MemoryOrder::Relaxed>(&block->header.next, static_cast<TraceBlock*>(nullptr));
        if (last != nullptr) {
            AtomicStore<MemoryOrder::Release>(&last->header.next, block);
        } else {
            first = block;
        }

        last = block;
    }

    // writer는 published 앞까지만 읽으므로, 기록을 다 쓰기 전에 사슬을 연결해도 반쯤 쓴 기록이
    // 나가지 않는다. 앞 블록은 consumed가 다음 블록 startTotal에 닿아야 회수되므로 쓰는 중에
    // 회수되지 않는다.
    BlockSink sink{state.block != nullptr ? state.block : first,
                   state.block != nullptr ? state.offset : 0};
    LinkBlock(state, first);
    WriteTraceRecord(sink, shape);
    state.block = last;
    state.offset = sink.offset;
    Publish(state, bytes);
    return true;
}

bool AppendRecord(TraceBlockPool& pool, TraceThreadState& state, const TraceRecordShape& shape,
                  usize bytes) noexcept {
    const usize remaining =
        state.block != nullptr ? TraceBlockLayout::PayloadBytes - state.offset : 0;
    if (bytes > remaining) {
        if (bytes > TraceBlockLayout::PayloadBytes) {
            return AppendSpanning(pool, state, shape, bytes, remaining);
        }

        // 남은 공간에 들어가지 않으면 블록을 은퇴시키고 새 블록을 받는다. 남은 꼬리는 버린다.
        if (!AdvanceBlock(pool, state)) {
            return false;
        }
    }

    EncodeTraceRecord(state.block->payload + state.offset, shape);
    state.offset += bytes;
    Publish(state, bytes);
    return true;
}

// 손실 상태를 끝내는 손실 기록을 쓴다. 블록을 받지 못해 생긴 손실이므로 항상 새 블록을 받아 그
// 첫 기록으로 쓴다(4.1절). 받지 못하면 손실 상태를 유지한다.
bool WriteLossRecord(TraceBlockPool& pool, TraceThreadState& state) noexcept {
    if (!AdvanceBlock(pool, state)) {
        return false;
    }

    const uint64 endTick = platform::MonotonicTicks();
    uint16 channels[TraceFormat::MaxChannels];
    uint64 counts[TraceFormat::MaxChannels];
    usize entries = 0;
    for (uint16 channel = 0; channel < TraceFormat::MaxChannels; ++channel) {
        const uint64 bit = 1ull << (channel % 64);
        if ((state.lostChannelMask[channel / 64] & bit) != 0) {
            channels[entries] = channel;
            counts[entries] = state.lostCounts[channel];
            ++entries;
            state.lostCounts[channel] = 0;
        }
    }

    uint8 fixed[TraceFormat::LossFixedBytes];
    StoreLittleEndian<uint64>(fixed + TraceFormat::LossStartTickOffset, state.lossStartTick);
    StoreLittleEndian<uint64>(fixed + TraceFormat::LossEndTickOffset, endTick);
    const TraceVariablePart parts[] = {{channels, entries * sizeof(uint16)},
                                       {counts, entries * sizeof(uint64)}};
    TraceRecordShape shape;
    shape.typeId = TraceFormat::LossTypeId;
    shape.fixed = fixed;
    shape.fixedBytes = sizeof(fixed);
    shape.variable = parts;
    const usize bytes = TraceRecordBytes(shape);
    // 채널 수 상한이 있어 손실 기록은 항상 빈 블록 하나에 들어간다.
    static_assert(sizeof(uint16) + TraceFormat::LossFixedBytes + 2 * sizeof(uint32) +
                          TraceFormat::MaxChannels * (sizeof(uint16) + sizeof(uint64)) <=
                      TraceBlockLayout::PayloadBytes,
                  "a loss record must fit in one trace block");
    EncodeTraceRecord(state.block->payload, shape);
    state.offset = bytes;
    Publish(state, bytes);
    for (uint64& mask : state.lostChannelMask) {
        mask = 0;
    }

    state.lossActive = false;
    return true;
}

} // namespace

bool WriteThreadLoss(TraceBlockPool& pool, TraceThreadState& state) noexcept {
    return WriteLossRecord(pool, state);
}

namespace {

void NoteLoss(TraceThreadState& state, uint16 channel) noexcept {
    if (!state.lossActive) {
        state.lossActive = true;
        state.lossStartTick = platform::MonotonicTicks();
    }

    if (channel < TraceFormat::MaxChannels) {
        ++state.lostCounts[channel];
        state.lostChannelMask[channel / 64] |= 1ull << (channel % 64);
    }

    AtomicFetchAdd<MemoryOrder::Relaxed>(&state.lostTotal, 1);
}

void RetireThread(TraceSessionState& session, TraceThreadState& state) noexcept {
    if (state.lossActive) {
        WriteLossRecord(session.pool, state);
    }

    AtomicStore<MemoryOrder::Release>(&state.exited, 1u);
}

// 이 스레드를 세션에 등록한다(스레드의 세션 첫 기록). 스레드 정보는 정의 기록으로 남긴다.
TraceThreadState* AcquireThreadState(TraceSessionState& session, ThreadSlot& slot) noexcept {
    if (slot.state != nullptr && slot.serial == session.serial) {
        return slot.state;
    }

    platform::LockGuard lock(session.registryMutex);
    if (session.nextThreadId > TraceFormat::MaxThreadId) {
        return nullptr;
    }

    auto memory = session.allocator->Allocate(sizeof(TraceThreadState), alignof(TraceThreadState));
    if (!memory) {
        return nullptr;
    }

    auto* state = ::new (memory.Value()) TraceThreadState{};
    state->threadId = session.nextThreadId++;
    state->osThreadId = platform::CurrentThreadId();
    state->next = session.threads;

    uint8 fixed[TraceFormat::ThreadInfoFixedBytes];
    StoreLittleEndian<uint32>(fixed + TraceFormat::ThreadInfoIdOffset, state->threadId);
    StoreLittleEndian<uint64>(fixed + TraceFormat::ThreadInfoOsIdOffset, state->osThreadId);
    TraceRecordShape info;
    info.typeId = TraceFormat::ThreadInfoTypeId;
    info.fixed = fixed;
    info.fixedBytes = sizeof(fixed);
    if (!session.WriteDefinition(info)) {
        session.definitions.NoteDropped();
    }

    AtomicStore<MemoryOrder::Release>(&session.threads, state);
    slot.state = state;
    slot.serial = session.serial;
    return state;
}

// 가변부 인자가 형의 가변 필드와 맞는지 확인한다. 어긋남은 호출 지점의 프로그래밍 오류다.
void CheckVariableParts(const TraceEventType& type,
                        Span<const TraceVariablePart> variable) noexcept {
    usize part = 0;
    for (usize index = 0; index < type.FieldCount(); ++index) {
        const TraceField& field = type.Fields()[index];
        if (!IsVariableTraceField(field.type)) {
            continue;
        }

        if (part == variable.Size()) {
            platform::Fatal(
                "TraceWrite received fewer variable parts than the event type declares");
        }

        const TraceVariablePart& value = variable[part++];
        if ((value.data == nullptr && value.bytes != 0) ||
            value.bytes % TraceFieldElementBytes(field.type) != 0) {
            platform::Fatal("TraceWrite variable part does not match its field type");
        }
    }

    if (part != variable.Size()) {
        platform::Fatal("TraceWrite received more variable parts than the event type declares");
    }
}

TraceWriteResult WriteWithSession(TraceSessionState& session, ThreadSlot& slot,
                                  const TraceEventType& type, const void* fixed, usize fixedBytes,
                                  Span<const TraceVariablePart> variable) noexcept {
    const uint16 typeId = type.Id();
    if (typeId == TraceEventType::NoId) {
        return TraceWriteResult::Disabled;
    }

    TraceRecordShape shape;
    shape.typeId = typeId;
    shape.ordered = HasTraceEventFlag(type.Flags(), TraceEventFlags::Ordered);
    shape.fixed = fixed;
    shape.fixedBytes = fixedBytes;
    shape.variable = variable;
    if (shape.ordered) {
        // 사건 사이의 happens-before가 같은 원자 칸의 수정 순서로 이어지므로 relaxed로 충분하다.
        shape.order = AtomicFetchAdd<MemoryOrder::Relaxed>(&session.nextOrder, 1);
    }

    if (HasTraceEventFlag(type.Flags(), TraceEventFlags::Definition)) {
        if (session.WriteDefinition(shape)) {
            return TraceWriteResult::Recorded;
        }

        session.definitions.NoteDropped();
        return TraceWriteResult::Lost;
    }

    TraceThreadState* state = AcquireThreadState(session, slot);
    if (state == nullptr || state->writing) {
        AtomicFetchAdd<MemoryOrder::Relaxed>(&session.unattributedLost, 1);
        return TraceWriteResult::Lost;
    }

    state->writing = true;
    const usize bytes = TraceRecordBytes(shape);
    bool written = bytes != MaxSize;
    if (written && state->lossActive) {
        written = WriteLossRecord(session.pool, *state);
    }

    if (written) {
        written = AppendRecord(session.pool, *state, shape, bytes);
    }

    if (!written) {
        NoteLoss(*state, type.Channel().Id());
    }

    state->writing = false;
    return written ? TraceWriteResult::Recorded : TraceWriteResult::Lost;
}

} // namespace

TraceWriteResult TraceWriteRecord(const TraceEventType& type, const void* fixed, usize fixedBytes,
                                  Span<const TraceVariablePart> variable) noexcept {
    if (fixedBytes != type.FixedBytes()) {
        platform::Fatal("TraceWrite fixed part size does not match the event type");
    }

    CheckVariableParts(type, variable);
    ThreadSlot& slot = currentSlot;
    if (!slot.linked) {
        slot.Link();
    }

    const uint32 depth = slot.Enter();
    TraceWriteResult result = TraceWriteResult::Disabled;
    TraceSessionState* session = AtomicLoad<MemoryOrder::SequentiallyConsistent>(&currentSession);
    if (session != nullptr) {
        result = WriteWithSession(*session, slot, type, fixed, fixedBytes, variable);
    }

    slot.Leave(depth);
    return result;
}

void WaitForTraceProducers() noexcept {
    LockSlotRegistry();
    for (ThreadSlot* slot = slotRegistry.head; slot != nullptr; slot = slot->next) {
        while (AtomicLoad<MemoryOrder::SequentiallyConsistent>(&slot->busy) != 0) {
            platform::YieldThread();
        }
    }

    UnlockSlotRegistry();
}

bool TraceSessionState::WriteDefinition(const TraceRecordShape& shape) noexcept {
    const usize bytes = TraceRecordBytes(shape);
    if (bytes == MaxSize) {
        return false;
    }

    uint8* record = definitions.TryReserve(bytes);
    if (record == nullptr) {
        return false;
    }

    EncodeTraceRecord(record, shape);
    TraceDefinitionBuffer::Commit(record, bytes);
    return true;
}

void TraceSessionState::FlushPendingLoss() noexcept {
    for (TraceThreadState* state = AtomicLoad<MemoryOrder::Acquire>(&threads); state != nullptr;
         state = state->next) {
        // 손실 상태인 스레드의 마지막 블록은 writer가 회수하지 않으므로(DrainThread) done이 아니다.
        if (state->lossActive && !state->done) {
            WriteLossRecord(pool, *state);
        }
    }
}

} // namespace rocks::detail
