#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// 문맥 스택과 스레드 정보다(Docs/Design/TraceSystem.ko.md 6.4·6.5절). 스택·이름은 스레드 TLS 칸에
// 있어 세션이 없어도 동작하고, 세션이 있으면 기록한다.
namespace rocks {

using detail::TraceSessionGuard;
using detail::TraceSessionState;
using detail::TraceThreadSlot;
using platform::AtomicLoad;
using platform::MemoryOrder;
using Format = TraceStandardFormat;

namespace {

static_assert(Format::MaxContextDepth == MaxCorrelationDepth,
              "trace context depth must match the correlation stack depth");

// Core 스택 칸의 확장 칸 쓰임새다(Core는 뜻을 모른다).
constexpr usize CacheExtension = 0;
constexpr usize RecordedExtension = 1;
static_assert(RecordedExtension < CorrelationExtensionCount,
              "trace needs two correlation extension slots");

uint64 CurrentSerial() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&detail::currentSessionSerial);
}

// 문맥 정의를 세션마다 한 번 쓰고 칸에 (serial, id)를 캐시한다. 칸은 이 스레드 것이라 잠그지
// 않는다.
uint32 DefineContext(TraceSessionState& session, CorrelationEntry& entry, uint64 serial) noexcept {
    if (const uint32 id = detail::CachedSessionId(&entry.ext[CacheExtension], serial); id != 0) {
        return id;
    }

    const uint32 id =
        platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&session.nextContextId, 1u) + 1;
    uint8 fixed[Format::ContextDefinitionFixedBytes];
    StoreLittleEndian<uint32>(fixed + Format::ContextDefinitionIdOffset, id);
    const uint64 values[Format::ContextValueCount] = {
        entry.ids.session, entry.ids.task, entry.ids.asset, entry.ids.entity, entry.ids.request};
    for (usize index = 0; index < Format::ContextValueCount; ++index) {
        StoreLittleEndian<uint64>(fixed + Format::ContextDefinitionValuesOffset + index * 8,
                                  values[index]);
    }

    if (!detail::WriteStandardDefinition(session, detail::TraceContextDefinitionType, fixed,
                                         sizeof(fixed), {})) {
        return 0;
    }

    entry.ext[CacheExtension] = detail::PackSessionId(serial, id);
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
void RecordPush(TraceThreadSlot& slot, CorrelationEntry& entry) noexcept {
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
        entry.ext[RecordedExtension] = serial;
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

// Core 스택의 쌓기 관찰자다. 기록 여부는 채널·세션을 보고 정한다(지금까지의 TraceContextScope와
// 같다). 복원한 칸도 기록 여부는 새로 정한다.
void OnContextPush(CorrelationEntry& entry, bool) noexcept {
    entry.ext[RecordedExtension] = 0;
    if (TraceContextChannel.IsEnabled()) {
        RecordPush(detail::CurrentTraceThreadSlot(), entry);
    }
}

// Core 스택의 내림 관찰자다. 쌓기를 기록한 칸만 내림을 기록한다.
void OnContextPop(const CorrelationEntry& entry) noexcept {
    if (entry.ext[RecordedExtension] != 0) {
        RecordPop(detail::CurrentTraceThreadSlot(), entry.ext[RecordedExtension]);
    }
}

constinit const CorrelationObserver ContextObserver{&OnContextPush, &OnContextPop};

usize CopyName(StringView source, char* destination) noexcept {
    const usize bytes = source.ByteCount() < Format::MaxThreadNameBytes
                            ? source.ByteCount()
                            : Format::MaxThreadNameBytes;
    platform::MemoryCopy(destination, source.Data(), bytes);
    return bytes;
}

} // namespace

namespace detail {

void InstallTraceContextObserver() noexcept {
    // 같은 관찰자를 다시 걸면 성공한다. 다른 관찰자가 이미 있으면 trace 기록만 빠진다.
    (void)SetCorrelationObserver(&ContextObserver);
}

} // namespace detail

TraceContextToken TraceCaptureContext() noexcept {
    CorrelationEntry* entry = detail::TopCorrelationEntry();
    if (entry == nullptr) {
        return {};
    }

    // 정의를 미리 써 두면 같은 세션에서의 복원은 id만 기록한다.
    const uint64 serial = CurrentSerial();
    if (serial != 0 && TraceContextChannel.IsEnabled()) {
        TraceSessionGuard guard(detail::CurrentTraceThreadSlot());
        if (TraceSessionState* session = guard.Session(serial)) {
            const uint32 defined = DefineContext(*session, *entry, serial);
            (void)defined;
        }
    }

    return CaptureCorrelation();
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
