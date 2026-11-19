#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// 카운터·표지·시간 띠·프레임 경계다(Docs/Design/TraceSystem.ko.md 6.2·6.3절).
namespace rocks {

using detail::TraceCounterAccess;
using detail::TraceMarkSiteAccess;
using detail::TraceSessionGuard;
using detail::TraceSessionState;
using platform::AtomicLoad;
using platform::AtomicStore;
using platform::MemoryOrder;
using Format = TraceStandardFormat;

namespace {

// 등록된 표준 형의 기록 하나를 현재 세션에 쓴다(채널 검사 없음, 호출자가 했다). serial이 0이 아니면
// 그 세션일 때만 쓴다.
TraceWriteResult WriteStandard(TraceSessionGuard& guard, TraceSessionState& session,
                               const TraceEventType& type, const void* fixed, usize fixedBytes,
                               Span<const TraceVariablePart> variable = {}) noexcept {
    const uint16 typeId = detail::TraceEventTypeAccess::AcquireId(type);
    if (typeId == TraceEventType::NoId) {
        return TraceWriteResult::Disabled;
    }

    return detail::WriteSessionRecord(session, guard.Slot(), type, typeId, fixed, fixedBytes,
                                      variable);
}

uint64 TraceEventsCurrentSerial() noexcept {
    return AtomicLoad<MemoryOrder::Acquire>(&detail::currentSessionSerial);
}

// 카운터 정의를 세션마다 한 번 쓰고 id를 캐시한다. 잠금은 카운터마다 세션당 한 번만 잡는다.
uint32 DefineCounter(TraceSessionState& session, TraceCounter& counter, uint64 serial) noexcept {
    uint64* cache = TraceCounterAccess::Cache(counter);
    platform::LockGuard lock(session.registryMutex);
    if (const uint32 id = detail::CachedSessionId(cache, serial); id != 0) {
        return id;
    }

    const uint32 id = session.nextCounterId + 1;
    uint8 fixed[Format::CounterDefinitionFixedBytes];
    StoreLittleEndian<uint32>(fixed + Format::CounterDefinitionIdOffset, id);
    fixed[Format::CounterDefinitionKindOffset] = static_cast<uint8>(counter.Kind());
    fixed[Format::CounterDefinitionUnitOffset] = static_cast<uint8>(counter.Unit());
    fixed[Format::CounterDefinitionModeOffset] = static_cast<uint8>(counter.Mode());
    const TraceVariablePart parts[] = {TraceVariablePart::FromString(StringView(counter.Name()))};
    if (!detail::WriteStandardDefinition(session, detail::TraceCounterDefinitionType, fixed,
                                         sizeof(fixed), parts)) {
        return 0;
    }

    session.nextCounterId = id;
    AtomicStore<MemoryOrder::Relaxed>(cache, detail::PackSessionId(serial, id));
    return id;
}

uint32 DefineMarkSite(TraceSessionState& session, TraceMarkSite& site, uint64 serial) noexcept {
    uint64* cache = TraceMarkSiteAccess::Cache(site);
    platform::LockGuard lock(session.registryMutex);
    if (const uint32 id = detail::CachedSessionId(cache, serial); id != 0) {
        return id;
    }

    const uint32 id = session.nextMarkSiteId + 1;
    uint8 fixed[Format::MarkSiteFixedBytes];
    StoreLittleEndian<uint32>(fixed + Format::MarkSiteIdOffset, id);
    StoreLittleEndian<uint32>(fixed + Format::MarkSiteLineOffset, site.Line());
    const TraceVariablePart parts[] = {
        TraceVariablePart::FromString(StringView(site.Format() != nullptr ? site.Format() : "")),
        TraceVariablePart::FromString(StringView(site.File() != nullptr ? site.File() : ""))};
    if (!detail::WriteStandardDefinition(session, detail::TraceMarkSiteType, fixed, sizeof(fixed),
                                         parts)) {
        return 0;
    }

    session.nextMarkSiteId = id;
    AtomicStore<MemoryOrder::Relaxed>(cache, detail::PackSessionId(serial, id));
    return id;
}

} // namespace

// ---- 카운터 ----

void TraceCounter::Set(int64 value) noexcept {
    if (kind_ != TraceCounterKind::Integer) {
        platform::Fatal("TraceCounter::Set on a float counter");
    }

    const uint64 bits = static_cast<uint64>(value);
    AtomicStore<MemoryOrder::Relaxed>(&value_, bits);
    if (TraceCounterChannel.IsEnabled()) {
        detail::EmitTraceCounter(*this, bits);
    }
}

void TraceCounter::Add(int64 delta) noexcept {
    if (kind_ != TraceCounterKind::Integer) {
        platform::Fatal("TraceCounter::Add on a float counter");
    }

    const uint64 step = static_cast<uint64>(delta);
    const uint64 bits = platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&value_, step) + step;
    if (TraceCounterChannel.IsEnabled()) {
        detail::EmitTraceCounter(*this, bits);
    }
}

void TraceCounter::SetFloat(double value) noexcept {
    if (kind_ != TraceCounterKind::Float) {
        platform::Fatal("TraceCounter::SetFloat on an integer counter");
    }

    const uint64 bits = platform::BitCast<uint64>(value);
    AtomicStore<MemoryOrder::Relaxed>(&value_, bits);
    if (TraceCounterChannel.IsEnabled()) {
        detail::EmitTraceCounter(*this, bits);
    }
}

void TraceCounter::AddFloat(double delta) noexcept {
    if (kind_ != TraceCounterKind::Float) {
        platform::Fatal("TraceCounter::AddFloat on an integer counter");
    }

    uint64 current = AtomicLoad<MemoryOrder::Relaxed>(&value_);
    uint64 next = 0;
    do {
        next = platform::BitCast<uint64>(platform::BitCast<double>(current) + delta);
    } while (!platform::AtomicCompareExchangeWeak<MemoryOrder::Relaxed, MemoryOrder::Relaxed>(
        &value_, current, next));

    if (TraceCounterChannel.IsEnabled()) {
        detail::EmitTraceCounter(*this, next);
    }
}

int64 TraceCounter::Value() const noexcept {
    return static_cast<int64>(AtomicLoad<MemoryOrder::Relaxed>(&value_));
}

double TraceCounter::FloatValue() const noexcept {
    return platform::BitCast<double>(AtomicLoad<MemoryOrder::Relaxed>(&value_));
}

void detail::EmitTraceCounter(TraceCounter& counter, uint64 bits) noexcept {
    const uint64 serial = TraceEventsCurrentSerial();
    if (serial == 0) {
        return;
    }

    const bool onChange = counter.Mode() == TraceCounterMode::OnChange;
    if (onChange &&
        AtomicLoad<MemoryOrder::Relaxed>(TraceCounterAccess::LastSerial(counter)) == serial &&
        AtomicLoad<MemoryOrder::Relaxed>(TraceCounterAccess::LastBits(counter)) == bits) {
        return;
    }

    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return;
    }

    uint32 id = CachedSessionId(TraceCounterAccess::Cache(counter), serial);
    if (id == 0) {
        id = DefineCounter(*session, counter, serial);
        if (id == 0) {
            return;
        }
    }

    uint8 fixed[Format::CounterValueFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::CounterValueTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint32>(fixed + Format::CounterValueIdOffset, id);
    StoreLittleEndian<uint64>(fixed + Format::CounterValueOffset, bits);
    const TraceEventType& type = counter.Kind() == TraceCounterKind::Integer
                                     ? TraceCounterIntegerType
                                     : TraceCounterFloatType;
    if (WriteStandard(guard, *session, type, fixed, sizeof(fixed)) == TraceWriteResult::Recorded &&
        onChange) {
        AtomicStore<MemoryOrder::Relaxed>(TraceCounterAccess::LastBits(counter), bits);
        AtomicStore<MemoryOrder::Relaxed>(TraceCounterAccess::LastSerial(counter), serial);
    }
}

// ---- 표지 ----

void TraceMarkArguments::AddBool(bool value) noexcept {
    if (truncated_ || Format::MaxMarkArgumentBytes - size_ < 2) {
        truncated_ = true;
        return;
    }

    bytes_[size_++] = Format::MarkArgumentBool;
    bytes_[size_++] = value ? 1 : 0;
}

void TraceMarkArguments::AddWord(uint8 tag, uint64 value) noexcept {
    if (truncated_ || Format::MaxMarkArgumentBytes - size_ < 1 + sizeof(uint64)) {
        truncated_ = true;
        return;
    }

    bytes_[size_++] = tag;
    StoreLittleEndian<uint64>(bytes_ + size_, value);
    size_ += sizeof(uint64);
}

void TraceMarkArguments::AddFloat(double value) noexcept {
    AddWord(Format::MarkArgumentFloat64, platform::BitCast<uint64>(value));
}

void TraceMarkArguments::AddString(StringView value) noexcept {
    const usize header = 1 + sizeof(uint32);
    if (truncated_ || Format::MaxMarkArgumentBytes - size_ < header + value.ByteCount()) {
        truncated_ = true;
        return;
    }

    bytes_[size_++] = Format::MarkArgumentString;
    StoreLittleEndian<uint32>(bytes_ + size_, static_cast<uint32>(value.ByteCount()));
    size_ += sizeof(uint32);
    platform::MemoryCopy(bytes_ + size_, value.Data(), value.ByteCount());
    size_ += value.ByteCount();
}

void detail::EmitTraceMark(TraceMarkSite& site, const TraceMarkArguments& arguments) noexcept {
    const uint64 serial = TraceEventsCurrentSerial();
    if (serial == 0) {
        return;
    }

    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return;
    }

    uint32 id = CachedSessionId(TraceMarkSiteAccess::Cache(site), serial);
    if (id == 0) {
        id = DefineMarkSite(*session, site, serial);
        if (id == 0) {
            return;
        }
    }

    uint8 fixed[Format::MarkFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::MarkTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint32>(fixed + Format::MarkSiteOffset, id);
    const TraceVariablePart parts[] = {{arguments.Data(), arguments.Size()}};
    const TraceWriteResult written =
        WriteStandard(guard, *session, TraceMarkEventType, fixed, sizeof(fixed), parts);
    (void)written;
}

// ---- 시간 띠 ----

TraceSpanId detail::BeginTraceSpan(StringView name) noexcept {
    const uint64 serial = TraceEventsCurrentSerial();
    if (serial == 0) {
        return {};
    }

    const uint32 nameId = ResolveTraceName(serial, name, true);
    if (nameId == 0) {
        return {};
    }

    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return {};
    }

    const uint64 spanId =
        platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&session->nextSpanId, 1) + 1;
    uint8 fixed[Format::SpanBeginFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::SpanTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint64>(fixed + Format::SpanIdOffset, spanId);
    StoreLittleEndian<uint32>(fixed + Format::SpanNameOffset, nameId);
    if (WriteStandard(guard, *session, TraceSpanBeginType, fixed, sizeof(fixed)) !=
        TraceWriteResult::Recorded) {
        return {};
    }

    return TraceSpanId{spanId, serial};
}

void detail::EndTraceSpan(TraceSpanId span) noexcept {
    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(span.serial);
    if (session == nullptr) {
        return;
    }

    uint8 fixed[Format::SpanEndFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::SpanTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint64>(fixed + Format::SpanIdOffset, span.value);
    const TraceWriteResult written =
        WriteStandard(guard, *session, TraceSpanEndType, fixed, sizeof(fixed));
    (void)written;
}

// ---- 프레임 ----

void detail::WriteTraceFrame(bool begin, TraceFrameKind kind, uint64 frame) noexcept {
    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session();
    if (session == nullptr) {
        return;
    }

    uint8 fixed[Format::FrameFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::FrameTickOffset, platform::MonotonicTicks());
    StoreLittleEndian<uint64>(fixed + Format::FrameNumberOffset, frame);
    fixed[Format::FrameKindOffset] = static_cast<uint8>(kind);
    const TraceWriteResult written = WriteStandard(
        guard, *session, begin ? TraceFrameBeginType : TraceFrameEndType, fixed, sizeof(fixed));
    (void)written;
}

} // namespace rocks
