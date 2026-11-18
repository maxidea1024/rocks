#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Memory.h>
#include <TraceCapture/TraceLogSink.h>

namespace rocks {

namespace {

using detail::TraceSessionGuard;
using detail::TraceSessionState;
using platform::MemoryOrder;
using Format = TraceStandardFormat;

// 이 스레드가 TraceLogSink::Write 안에 있는지다(trace 쓰기가 남긴 로그가 다시 들어오는 재귀 차단).
thread_local constinit bool inTraceLogWrite = false;

class TraceLogWriteScope {
public:
    TraceLogWriteScope() noexcept {
        inTraceLogWrite = true;
    }

    ~TraceLogWriteScope() noexcept {
        inTraceLogWrite = false;
    }

    TraceLogWriteScope(const TraceLogWriteScope&) = delete;
    TraceLogWriteScope& operator=(const TraceLogWriteScope&) = delete;
};

// trace 자신의 카테고리("Trace"와 "Trace.*")인지다. 대소문자는 카테고리 일치 규칙대로 무시한다.
bool IsTraceCategory(StringView category) noexcept {
    constexpr StringView Root = "Trace";
    if (category.ByteCount() < Root.ByteCount() ||
        !EqualsIgnoringAsciiCase(StringView(category.Data(), Root.ByteCount()), Root)) {
        return false;
    }

    return category.ByteCount() == Root.ByteCount() || category.Data()[Root.ByteCount()] == '.';
}

uint64 LoadCounter(const uint64& value) noexcept {
    return platform::AtomicLoad<MemoryOrder::Relaxed>(&value);
}

void AddCounter(uint64& value) noexcept {
    (void)platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&value, uint64{1});
}

} // namespace

bool TraceLogSink::NeedsSpec(uint64 serial, uint64 eventId) noexcept {
    // 세션이 바뀌면 표를 비운다(정의는 세션마다 다시 쓴다).
    if (serial != specSerial_) {
        platform::MemoryZero(specIds_, sizeof(specIds_));
        specCount_ = 0;
        specSerial_ = serial;
    }

    // 0은 빈 칸 표지이므로 EventId 0은 매번 쓴다(리터럴·런타임 템플릿의 해시가 0일 일은 드물다).
    if (eventId == 0) {
        return true;
    }

    constexpr usize Mask = TraceLogSpecTableSlots - 1;
    if (specCount_ * 4 >= TraceLogSpecTableSlots * 3) {
        // 표가 3/4 넘게 찼다. 더 넣지 않고 매번 쓴다(정의가 늘지만 분석은 같은 결과다).
        for (usize probe = 0, index = eventId & Mask; probe < TraceLogSpecTableSlots;
             ++probe, index = (index + 1) & Mask) {
            if (specIds_[index] == eventId) {
                return false;
            }

            if (specIds_[index] == 0) {
                break;
            }
        }

        AddCounter(specOverflows_);
        return true;
    }

    for (usize index = eventId & Mask;; index = (index + 1) & Mask) {
        if (specIds_[index] == eventId) {
            return false;
        }

        if (specIds_[index] == 0) {
            specIds_[index] = eventId;
            ++specCount_;
            return true;
        }
    }
}

Result<void> TraceLogSink::Write(const LogEvent& event) noexcept {
    // 채널이 꺼져 있으면 분기 하나다.
    if (!TraceLogChannel.IsEnabled() || inTraceLogWrite || IsTraceCategory(event.category)) {
        return Result<void>::Success();
    }

    TraceLogWriteScope scope;
    const uint64 serial = platform::AtomicLoad<MemoryOrder::Acquire>(&detail::currentSessionSerial);
    if (serial == 0) {
        return Result<void>::Success();
    }

    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return Result<void>::Success();
    }

    if (NeedsSpec(serial, event.eventId)) {
        uint8 fixed[Format::LogSpecFixedBytes];
        StoreLittleEndian<uint64>(fixed + Format::LogSpecEventIdOffset, event.eventId);
        StoreLittleEndian<uint32>(fixed + Format::LogSpecLineOffset, event.location.line);
        fixed[Format::LogSpecLevelOffset] = static_cast<uint8>(event.level);
        const TraceVariablePart parts[] = {
            TraceVariablePart::FromString(event.category),
            TraceVariablePart::FromString(event.messageTemplate),
            TraceVariablePart::FromString(
                StringView(event.location.file != nullptr ? event.location.file : "")),
            TraceVariablePart::FromString(
                StringView(event.location.function != nullptr ? event.location.function : ""))};
        (void)detail::WriteStandardDefinition(*session, detail::TraceLogMessageSpecType, fixed,
                                              sizeof(fixed), parts);
    }

    const uint16 typeId = detail::TraceEventTypeAccess::AcquireId(detail::TraceLogMessageType);
    if (typeId == TraceEventType::NoId) {
        return Result<void>::Success();
    }

    uint8 fixed[Format::LogMessageFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::LogMessageTickOffset, event.monotonicTicks);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageThreadOffset, event.threadId);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageFrameOffset, event.frame);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageEventIdOffset, event.eventId);
    const CorrelationIds& ids = event.correlation;
    const uint64 values[Format::ContextValueCount] = {ids.session, ids.task, ids.asset, ids.entity,
                                                      ids.request};
    uint8 mask = 0;
    for (usize index = 0; index < Format::ContextValueCount; ++index) {
        const uint64 value = event.hasCorrelation ? values[index] : 0;
        StoreLittleEndian<uint64>(
            fixed + Format::LogMessageCorrelationOffset + index * sizeof(uint64), value);
        mask = static_cast<uint8>(mask | (value != 0 ? 1u << index : 0u));
    }

    StringView message = event.message;
    bool truncated = false;
    if (message.ByteCount() > MaxTraceLogMessageBytes) {
        // UTF-8 문자 경계에서 자른다(이어지는 byte는 10xxxxxx).
        constexpr uint8 ContinuationMask = 0xC0;
        constexpr uint8 ContinuationBits = 0x80;
        usize bytes = MaxTraceLogMessageBytes;
        while (bytes > 0 &&
               (static_cast<uint8>(message.Data()[bytes]) & ContinuationMask) == ContinuationBits) {
            --bytes;
        }

        message = StringView(message.Data(), bytes);
        truncated = true;
    }

    StoreLittleEndian<uint32>(fixed + Format::LogMessageFlagsOffset,
                              static_cast<uint32>(event.flags));
    fixed[Format::LogMessageLevelOffset] = static_cast<uint8>(event.level);
    fixed[Format::LogMessageCorrelationMaskOffset] = mask;
    fixed[Format::LogMessageTruncatedOffset] = truncated ? 1 : 0;
    const TraceVariablePart parts[] = {TraceVariablePart::FromString(message)};
    if (detail::WriteSessionRecord(*session, guard.Slot(), detail::TraceLogMessageType, typeId,
                                   fixed, sizeof(fixed), parts) == TraceWriteResult::Lost) {
        AddCounter(lost_);
    }

    return Result<void>::Success();
}

Result<void> TraceLogSink::Flush() noexcept {
    return Result<void>::Success();
}

uint64 TraceLogSink::Lost() const noexcept {
    return LoadCounter(lost_);
}

uint64 TraceLogSink::SpecTableOverflows() const noexcept {
    return LoadCounter(specOverflows_);
}

} // namespace rocks
