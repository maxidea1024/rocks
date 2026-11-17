#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// 문맥 스택과 스레드 정보다(Docs/Design/TraceSystem.ko.md 6.4·6.5절). 스택·이름은 스레드 TLS 칸에
// 있어 세션이 없어도 동작하고, 세션이 있으면 기록한다.
namespace rocks {

using detail::TraceContextEntry;
using detail::TraceSessionGuard;
using detail::TraceSessionState;
using detail::TraceThreadSlot;
using platform::AtomicLoad;
using platform::MemoryOrder;
using Format = TraceStandardFormat;

namespace {

uint64 CurrentSerial() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&detail::currentSessionSerial);
}

// 문맥 정의를 세션마다 한 번 쓰고 칸에 (serial, id)를 캐시한다. 칸은 이 스레드 것이라 잠그지
// 않는다.
uint32 DefineContext(TraceSessionState& session, TraceContextEntry& entry, uint64 serial) noexcept {
    if (const uint32 id = detail::CachedSessionId(&entry.cache, serial); id != 0) {
        return id;
    }

    const uint32 id =
        platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&session.nextContextId, 1u) + 1;
    uint8 fixed[Format::ContextDefinitionFixedBytes];
    StoreLittleEndian<uint32>(fixed + Format::ContextDefinitionIdOffset, id);
    const uint64 values[Format::ContextValueCount] = {entry.values.session, entry.values.task,
                                                      entry.values.asset, entry.values.entity,
                                                      entry.values.request};
    for (usize index = 0; index < Format::ContextValueCount; ++index) {
        StoreLittleEndian<uint64>(fixed + Format::ContextDefinitionValuesOffset + index * 8,
                                  values[index]);
    }

    if (!detail::WriteStandardDefinition(session, detail::TraceContextDefinitionType, fixed,
                                         sizeof(fixed), {})) {
        return 0;
    }

    entry.cache = detail::PackSessionId(serial, id);
    return id;
}

TraceWriteResult WriteContextRecord(TraceSessionGuard& guard, TraceSessionState& session,
                                    const TraceEventType& type, const void* fixed,
                                    usize fixedBytes) noexcept {
    const uint16 typeId = detail::TraceEventTypeAccess::AcquireId(type);
    if (typeId == TraceEventType::NoId) {
        return TraceWriteResult::Disabled;
    }

    return detail::WriteSessionRecord(session, guard.Slot(), type, typeId, fixed, fixedBytes, {});
}

// 쌓기(새 문맥·복원)를 기록한다. 같은 세션에서 정의를 이미 썼으면 id만 기록한다.
void RecordPush(TraceThreadSlot& slot, TraceContextEntry& entry) noexcept {
    const uint64 serial = CurrentSerial();
    if (serial == 0) {
        return;
    }

    TraceSessionGuard guard(slot);
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return;
    }

    const uint32 id = DefineContext(*session, entry, serial);
    if (id == 0) {
        return;
    }

    uint8 fixed[Format::ContextPushFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::ContextPushTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint32>(fixed + Format::ContextPushIdOffset, id);
    if (WriteContextRecord(guard, *session, detail::TraceContextPushType, fixed, sizeof(fixed)) ==
        TraceWriteResult::Recorded) {
        entry.recorded = serial;
    }
}

void RecordPop(TraceThreadSlot& slot, uint64 serial) noexcept {
    TraceSessionGuard guard(slot);
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return;
    }

    uint8 fixed[Format::ContextPopFixedBytes];
    StoreLittleEndian<uint64>(fixed, platform::MonotonicTicks());
    const TraceWriteResult written =
        WriteContextRecord(guard, *session, detail::TraceContextPopType, fixed, sizeof(fixed));
    (void)written;
}

// 칸 하나를 쌓는다. 깊이 상한이면 세기만 하고 2를 돌려준다.
uint8 PushEntry(const TraceContextValues& values, uint64 cache) noexcept {
    TraceThreadSlot& slot = detail::CurrentTraceThreadSlot();
    if (slot.contextDepth == Format::MaxContextDepth) {
        ++slot.contextOverflow;
        return 2;
    }

    TraceContextEntry& entry = slot.contexts[slot.contextDepth++];
    entry.values = values;
    entry.cache = cache;
    entry.recorded = 0;
    if (TraceContextChannel.IsEnabled()) {
        RecordPush(slot, entry);
    }

    return 1;
}

usize CopyName(StringView source, char* destination) noexcept {
    const usize bytes = source.ByteCount() < Format::MaxThreadNameBytes
                            ? source.ByteCount()
                            : Format::MaxThreadNameBytes;
    platform::MemoryCopy(destination, source.Data(), bytes);
    return bytes;
}

} // namespace

TraceContextScope::TraceContextScope(const TraceContextValues& values) noexcept
    : pushed_(PushEntry(values, 0)) {
}

TraceContextScope::TraceContextScope(const TraceContextToken& token) noexcept
    : pushed_(token.valid ? PushEntry(token.values, token.cache) : 0) {
}

TraceContextScope::~TraceContextScope() noexcept {
    if (pushed_ == 0) {
        return;
    }

    TraceThreadSlot& slot = detail::CurrentTraceThreadSlot();
    if (pushed_ == 2) {
        --slot.contextOverflow;
        return;
    }

    // 상한을 넘겨 세기만 한 쌓기가 남아 있으면 LIFO 순서가 깨진 것이다.
    if (slot.contextDepth == 0 || slot.contextOverflow != 0) {
        platform::Fatal("TraceContextScope destroyed out of order");
    }

    const TraceContextEntry& entry = slot.contexts[slot.contextDepth - 1];
    if (entry.recorded != 0) {
        RecordPop(slot, entry.recorded);
    }

    --slot.contextDepth;
}

TraceContextToken TraceCaptureContext() noexcept {
    TraceThreadSlot& slot = detail::CurrentTraceThreadSlot();
    if (slot.contextDepth == 0) {
        return {};
    }

    TraceContextEntry& entry = slot.contexts[slot.contextDepth - 1];
    // 정의를 미리 써 두면 같은 세션에서의 복원은 id만 기록한다.
    const uint64 serial = CurrentSerial();
    if (serial != 0 && TraceContextChannel.IsEnabled()) {
        TraceSessionGuard guard(slot);
        if (TraceSessionState* session = guard.Session(serial)) {
            const uint32 defined = DefineContext(*session, entry, serial);
            (void)defined;
        }
    }

    TraceContextToken token;
    token.values = entry.values;
    token.cache = entry.cache;
    token.valid = true;
    return token;
}

bool TraceCurrentContext(TraceContextValues& out) noexcept {
    const TraceThreadSlot& slot = detail::CurrentTraceThreadSlot();
    if (slot.contextDepth == 0) {
        return false;
    }

    out = slot.contexts[slot.contextDepth - 1].values;
    return true;
}

void TraceSetThreadInfo(StringView name, StringView group, int32 sortOrder) noexcept {
    TraceThreadSlot& slot = detail::CurrentTraceThreadSlot();
    slot.threadNameBytes = static_cast<uint8>(CopyName(name, slot.threadName));
    slot.threadGroupBytes = static_cast<uint8>(CopyName(group, slot.threadGroup));
    slot.threadSortOrder = sortOrder;

    TraceSessionGuard guard(slot);
    TraceSessionState* session = guard.Session();
    if (session == nullptr) {
        return;
    }

    // 이미 등록한 스레드면 정보를 다시 쓴다(나중 기록이 이긴다). 아니면 지금 등록하며 쓴다.
    if (slot.state != nullptr && slot.serial == session->serial) {
        detail::WriteThreadInfo(*session, *slot.state, slot);
    } else {
        const detail::TraceThreadState* state = detail::AcquireThreadState(*session, slot);
        (void)state;
    }
}

} // namespace rocks
