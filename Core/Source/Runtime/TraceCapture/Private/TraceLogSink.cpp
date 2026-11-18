#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Core/Hash.h>
#include <Platform/Memory.h>
#include <TraceCapture/TraceLogSink.h>

namespace rocks {

namespace {

using detail::TraceSessionGuard;
using detail::TraceSessionState;
using platform::MemoryOrder;
using Format = TraceStandardFormat;

// 이 스레드가 TraceLogSink::Write 안에 있는지다. LogSystem을 거친 호출은 LogSystem의 sink 표시
// (inSink)가 이미 재귀를 막지만, sink를 직접 부르는 호스트·시험 경로와 trace 쓰기 안에서 남는
// 로그를 다시 받지 않도록 sink 자신도 막는다(두 표시는 지키는 범위가 다르다).
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

// 64-bit 섞기(splitmix64 마무리)다.
constexpr uint64 Mix(uint64 value) noexcept {
    constexpr uint64 First = 0xbf58476d1ce4e5b9ull;
    constexpr uint64 Second = 0x94d049bb133111ebull;
    constexpr uint32 ShiftA = 30;
    constexpr uint32 ShiftB = 27;
    constexpr uint32 ShiftC = 31;
    value = (value ^ (value >> ShiftA)) * First;
    value = (value ^ (value >> ShiftB)) * Second;
    return value ^ (value >> ShiftC);
}

// 호출 지점 spec id다: (EventId, 카테고리, 위치). 같은 템플릿을 다른 카테고리·위치에서 쓰면 다른
// spec이다(UE의 로그 지점 주소 키와 같은 구별). 0은 빈 칸 표지라 쓰지 않는다.
uint64 SpecIdOf(const LogEvent& event) noexcept {
    uint64 id = Mix(event.eventId ^ HashStringBytes(event.category));
    id = Mix(id ^ static_cast<uint64>(reinterpret_cast<usize>(event.location.file)));
    id = Mix(id ^ static_cast<uint64>(reinterpret_cast<usize>(event.location.function)) ^
             event.location.line);
    return id != 0 ? id : 1;
}

} // namespace

TraceLogSink::SpecState TraceLogSink::FindSpec(uint64 serial, uint64 specId) noexcept {
    // 세션이 바뀌면 표를 비운다(정의는 세션마다 다시 쓴다).
    if (serial != specSerial_) {
        platform::MemoryZero(specIds_, sizeof(specIds_));
        specCount_ = 0;
        specSerial_ = serial;
    }

    constexpr usize Mask = TraceLogSpecTableSlots - 1;
    for (usize probe = 0, index = specId & Mask; probe < TraceLogSpecTableSlots;
         ++probe, index = (index + 1) & Mask) {
        if (specIds_[index] == specId) {
            return SpecState::Known;
        }

        if (specIds_[index] == 0) {
            break;
        }
    }

    return specCount_ >= TraceLogSpecTableLimit ? SpecState::Full : SpecState::Missing;
}

void TraceLogSink::RememberSpec(uint64 specId) noexcept {
    constexpr usize Mask = TraceLogSpecTableSlots - 1;
    for (usize index = specId & Mask;; index = (index + 1) & Mask) {
        if (specIds_[index] == 0) {
            specIds_[index] = specId;
            ++specCount_;
            return;
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

    // spec: 처음 보는 호출 지점이면 정의를 쓰고, 쓰기가 성공한 뒤에만 표에 넣는다(실패하면 다음
    // 기록에서 다시 쓴다). 표가 차면 정의 스트림을 더 쓰지 않고 이 기록에 카테고리·템플릿을 싣는다.
    const uint64 specId = SpecIdOf(event);
    bool inlineSpec = false;
    const SpecState state = FindSpec(serial, specId);
    if (state == SpecState::Full) {
        inlineSpec = true;
        AddCounter(specOverflows_);
    } else if (state == SpecState::Missing) {
        uint8 fixed[Format::LogSpecFixedBytes];
        StoreLittleEndian<uint64>(fixed + Format::LogSpecIdOffset, specId);
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
        if (detail::WriteStandardDefinition(*session, detail::TraceLogMessageSpecType, fixed,
                                            sizeof(fixed), parts)) {
            RememberSpec(specId);
        } else {
            inlineSpec = true;
            AddCounter(specFailures_);
        }
    }

    const uint16 typeId = detail::TraceEventTypeAccess::AcquireId(detail::TraceLogMessageType);
    if (typeId == TraceEventType::NoId) {
        return Result<void>::Success();
    }

    uint8 fixed[Format::LogMessageFixedBytes];
    StoreLittleEndian<uint64>(fixed + Format::LogMessageTickOffset, event.monotonicTicks);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageThreadOffset, event.threadId);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageFrameOffset, event.frame);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageSpecIdOffset, specId);
    StoreLittleEndian<uint64>(fixed + Format::LogMessageEventIdOffset, event.eventId);
    uint64 values[CorrelationFieldCount]{};
    if (event.hasCorrelation) {
        CorrelationFieldValues(event.correlation, values);
    }

    uint8 mask = 0;
    for (usize index = 0; index < CorrelationFieldCount; ++index) {
        StoreLittleEndian<uint64>(
            fixed + Format::LogMessageCorrelationOffset + index * sizeof(uint64), values[index]);
        mask = static_cast<uint8>(mask | (values[index] != 0 ? 1u << index : 0u));
    }

    // 메시지는 MaxTraceLogMessageBytes까지, UTF-8 문자 경계에서 자른다.
    const StringView message = event.message.LeftBytesAtCodePoint(MaxTraceLogMessageBytes);
    const bool truncated = message.ByteCount() != event.message.ByteCount();
    StoreLittleEndian<uint32>(fixed + Format::LogMessageFlagsOffset,
                              static_cast<uint32>(event.flags));
    fixed[Format::LogMessageLevelOffset] = static_cast<uint8>(event.level);
    fixed[Format::LogMessageCorrelationMaskOffset] = mask;
    fixed[Format::LogMessageTruncatedOffset] = truncated ? 1 : 0;
    const TraceVariablePart parts[] = {
        TraceVariablePart::FromString(message),
        TraceVariablePart::FromString(inlineSpec ? event.category : StringView()),
        TraceVariablePart::FromString(inlineSpec ? event.messageTemplate : StringView())};
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

uint64 TraceLogSink::SpecWriteFailures() const noexcept {
    return LoadCounter(specFailures_);
}

} // namespace rocks
