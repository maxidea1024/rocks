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

// 기록의 상관 id 칸은 Core 상관 id 칸과 1:1이다(다르면 고정부 밖에 쓴다).
static_assert(CorrelationFieldCount == Format::ContextValueCount,
              "trace log correlation slots must match the Core correlation fields");

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

StringView LocationText(const char* text) noexcept {
    return StringView(text != nullptr ? text : "");
}

// 호출 지점 spec id다: (EventId, 카테고리, 위치). 같은 템플릿을 다른 카테고리·위치에서 쓰면 다른
// spec이다(UE의 로그 지점 주소 키와 같은 구별). 위치는 파일·함수 문자열의 해시다(포인터가 아니다.
// inline 함수·템플릿이 여러 TU에서 인스턴스화되어 문자열 사본이 여러 개여도 같은 지점은 같은
// spec이다). 0은 빈 칸 표지라 쓰지 않는다.
uint64 SpecIdOf(const LogEvent& event) noexcept {
    uint64 id = Mix(event.eventId ^ HashStringBytes(event.category));
    id = Mix(id ^ HashStringBytes(LocationText(event.location.file)));
    id = Mix(id ^ HashStringBytes(LocationText(event.location.function)) ^ event.location.line);
    return id != 0 ? id : 1;
}

} // namespace

namespace detail {

namespace {

// 이 스레드가 TraceLogSink::Write 안에 있는지다(InTraceLogWrite·TraceLogWriteScope).
thread_local constinit bool inTraceLogWrite = false;

} // namespace

bool InTraceLogWrite() noexcept {
    return inTraceLogWrite;
}

TraceLogWriteScope::TraceLogWriteScope() noexcept : previous_(inTraceLogWrite) {
    inTraceLogWrite = true;
}

TraceLogWriteScope::~TraceLogWriteScope() noexcept {
    inTraceLogWrite = previous_;
}

} // namespace detail

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
    if (!TraceLogChannel.IsEnabled() || detail::InTraceLogWrite() ||
        IsTraceCategory(event.category)) {
        return Result<void>::Success();
    }

    detail::TraceLogWriteScope scope;
    const uint64 serial = platform::AtomicLoad<MemoryOrder::Acquire>(&detail::currentSessionSerial);
    if (serial == 0) {
        return Result<void>::Success();
    }

    TraceSessionGuard guard;
    TraceSessionState* session = guard.Session(serial);
    if (session == nullptr) {
        return Result<void>::Success();
    }

    // spec: 처음 보는 호출 지점이면 정의를 쓰고, 쓰기가 성공한 뒤에만 표에 넣는다(정의 버퍼가 차서
    // 실패하면 다음 기록에서 다시 쓴다). 표가 차거나, spec이 정의 기록 상한을 넘거나, 쓰기가
    // 실패하면 이 기록에 카테고리·템플릿을 싣는다(위치는 잃는다). 실패는 sink만 센다(정보는 기록에
    // 남으므로 세션의 "버린 정의"로 세지 않는다).
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
            TraceVariablePart::FromString(LocationText(event.location.file)),
            TraceVariablePart::FromString(LocationText(event.location.function))};
        if (detail::TryWriteStandardDefinition(*session, detail::TraceLogMessageSpecType, fixed,
                                               sizeof(fixed),
                                               parts) == detail::TraceDefinitionWrite::Written) {
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

    // 메시지와 동봉 템플릿은 각각 MaxTraceLogMessageBytes까지, UTF-8 문자 경계에서 자른다.
    const StringView message = event.message.LeftBytesAtCodePoint(MaxTraceLogMessageBytes);
    const StringView inlineTemplate =
        inlineSpec ? event.messageTemplate.LeftBytesAtCodePoint(MaxTraceLogMessageBytes)
                   : StringView();
    uint8 truncated = 0;
    if (message.ByteCount() != event.message.ByteCount()) {
        truncated = static_cast<uint8>(truncated | Format::LogMessageTruncatedMessage);
    }

    if (inlineSpec && inlineTemplate.ByteCount() != event.messageTemplate.ByteCount()) {
        truncated = static_cast<uint8>(truncated | Format::LogMessageTruncatedTemplate);
    }

    StoreLittleEndian<uint32>(fixed + Format::LogMessageFlagsOffset,
                              static_cast<uint32>(event.flags));
    fixed[Format::LogMessageLevelOffset] = static_cast<uint8>(event.level);
    fixed[Format::LogMessageCorrelationMaskOffset] = mask;
    fixed[Format::LogMessageTruncatedOffset] = truncated;
    const TraceVariablePart parts[] = {
        TraceVariablePart::FromString(message),
        TraceVariablePart::FromString(inlineSpec ? event.category : StringView()),
        TraceVariablePart::FromString(inlineTemplate)};
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
