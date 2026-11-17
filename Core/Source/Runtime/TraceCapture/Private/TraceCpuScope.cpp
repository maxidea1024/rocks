#include "TraceSessionInternal.h"

#include <Platform/Clock.h>
#include <Platform/Memory.h>

// CPU 구간이다(Docs/Design/TraceSystem.ko.md 6.1절). 시작·끝은 스레드 전용 배치(TLS)에 varint로
// 쌓고, 배치가 차거나 깊이가 0이 되면 이벤트 하나로 낸다. 시작·끝 자체는 세션 메모리를 보지
// 않는다(정적 이름 id는 호출 지점에 캐시한다).
namespace rocks::detail {

using platform::AtomicLoad;
using platform::AtomicStore;

namespace {

using Format = TraceStandardFormat;

// LEB128 varint 하나의 최대 byte 수(64비트)와 사건 하나의 최대 byte 수(시작: tick + 이름 id)다.
constexpr usize MaxVarintBytes = 10;
constexpr usize MaxScopeEventBytes = 2 * MaxVarintBytes;

usize PutVarint(uint8* out, uint64 value) noexcept {
    constexpr uint64 ContinueBit = 0x80;
    usize written = 0;
    while (value >= ContinueBit) {
        out[written++] = static_cast<uint8>(value | ContinueBit);
        value >>= 7;
    }

    out[written++] = static_cast<uint8>(value);
    return written;
}

// 배치가 다른 세션의 것이면 비운다(세션이 바뀌면 이전 세션의 배치는 낼 곳이 없다).
void AdoptSession(TraceThreadSlot& slot, uint64 serial) noexcept {
    if (slot.scopeSerial != serial) {
        slot.scopeSerial = serial;
        slot.scopeBytes = 0;
        slot.scopeLastTick = 0;
        slot.scopeDepth = 0;
    }
}

// 사건 하나를 쌓는다. 배치에 자리가 없으면 먼저 낸다. Δtick은 배치 안 앞 사건 기준이며, 배치를 낸
// 뒤 첫 사건은 절대 tick이다.
void AppendScopeEvent(TraceThreadSlot& slot, bool begin, uint32 nameId) noexcept {
    if (slot.scopeBytes + MaxScopeEventBytes > Format::ScopeBatchBytes) {
        FlushCpuScopeBatch(slot);
    }

    const uint64 tick = platform::MonotonicTicks();
    const uint64 delta = tick - slot.scopeLastTick;
    uint8* cursor = slot.scopeBatch + slot.scopeBytes;
    usize written = PutVarint(cursor, (delta << Format::ScopeTagBits) |
                                          (begin ? Format::ScopeBeginTag : uint64{0}));
    if (begin) {
        written += PutVarint(cursor + written, nameId);
    }

    slot.scopeBytes += static_cast<uint32>(written);
    slot.scopeLastTick = tick;
}

uint64 BeginWithName(uint64 serial, uint32 nameId) noexcept {
    TraceThreadSlot& slot = CurrentTraceThreadSlot();
    AdoptSession(slot, serial);
    AppendScopeEvent(slot, true, nameId);
    ++slot.scopeDepth;
    return serial;
}

} // namespace

uint32 ResolveTraceName(uint64 serial, StringView name, bool useThreadCache) noexcept {
    const bool truncated = name.ByteCount() > Format::MaxScopeNameBytes;
    const StringView key = truncated ? StringView(name.Data(), Format::MaxScopeNameBytes) : name;
    const uint64 hash = TraceNameTable::HashName(key);
    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return 0;
    }

    // 동적 이름은 스레드 앞단 캐시를 먼저 본다(세션 이름 표의 byte와 비교하므로 해시 충돌에도
    // 맞다).
    TraceNameCacheEntry* entry = nullptr;
    if (useThreadCache) {
        if (TraceThreadState* state = AcquireThreadState(*session, guard.Slot())) {
            entry = &state->nameCache[hash & (TraceThreadState::NameCacheEntries - 1)];
            if (entry->id != 0 && entry->hash == hash && entry->length == key.ByteCount() &&
                platform::MemoryEquals(entry->text, key.Data(), key.ByteCount())) {
                return entry->id;
            }
        }
    }

    const char* stored = nullptr;
    uint32 storedLength = 0;
    const uint32 id =
        session->names.FindOrInsert(*session, key, hash, truncated, stored, storedLength);
    if (id != 0 && entry != nullptr) {
        *entry = TraceNameCacheEntry{hash, stored, storedLength, id};
    }

    return id;
}

uint64 BeginCpuScope(TraceScopeName& name) noexcept {
    const uint64 serial = AtomicLoad<MemoryOrder::Acquire>(&currentSessionSerial);
    if (serial == 0) {
        return 0;
    }

    uint64* cache = TraceScopeNameAccess::Cache(name);
    uint32 id = CachedSessionId(cache, serial);
    if (id == 0) {
        // 여러 스레드가 동시에 와도 이름 표가 같은 id를 돌려주므로 같은 값으로 수렴한다.
        id = ResolveTraceName(serial, StringView(name.Name()), false);
        if (id == 0) {
            return 0;
        }

        AtomicStore<MemoryOrder::Relaxed>(cache, PackSessionId(serial, id));
    }

    return BeginWithName(serial, id);
}

uint64 BeginCpuScope(StringView name) noexcept {
    const uint64 serial = AtomicLoad<MemoryOrder::Acquire>(&currentSessionSerial);
    if (serial == 0) {
        return 0;
    }

    const uint32 id = ResolveTraceName(serial, name, true);
    return id != 0 ? BeginWithName(serial, id) : 0;
}

void EndCpuScope(uint64 serial) noexcept {
    TraceThreadSlot& slot = CurrentTraceThreadSlot();
    // 시작 뒤 세션이 바뀌었으면 시작이 든 배치는 이미 버려졌다. 짝 없는 끝을 남기지 않는다.
    if (slot.scopeSerial != serial || slot.scopeDepth == 0) {
        return;
    }

    AppendScopeEvent(slot, false, 0);
    if (--slot.scopeDepth == 0) {
        FlushCpuScopeBatch(slot);
    }
}

void FlushCpuScopeBatch(TraceThreadSlot& slot) noexcept {
    if (slot.scopeBytes == 0) {
        return;
    }

    {
        const TraceVariablePart parts[] = {{slot.scopeBatch, slot.scopeBytes}};
        TraceSessionGuard guard(slot);
        TraceSessionState* session = guard.Session(slot.scopeSerial);
        const uint16 typeId = TraceEventTypeAccess::AcquireId(TraceCpuScopeBatchType);
        // 채널 상태는 구간을 시작할 때 이미 읽었으므로 여기서는 다시 보지 않는다.
        if (session != nullptr && typeId != TraceEventType::NoId) {
            const TraceWriteResult written = WriteSessionRecord(
                *session, slot, TraceCpuScopeBatchType, typeId, nullptr, 0, parts);
            (void)written;
        }
    }

    slot.scopeBytes = 0;
    slot.scopeLastTick = 0;
}

} // namespace rocks::detail
