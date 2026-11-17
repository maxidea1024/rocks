#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Diagnostics/SourceLocation.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 이름·값 한 쌍이다. 메시지 템플릿의 속성(`{Count}` → Count=3)과 문맥(`World=Main`)에 쓴다.
// 값은 렌더된 UTF-8 텍스트다. 두 view 모두 LogEvent와 같은 수명(sink 콜백 동안)만 유효하다.
struct LogProperty {
    StringView name;
    StringView value;
};

// LogEvent::flags의 비트다. 여러 비트를 함께 켤 수 있다.
enum class LogEventFlags : uint32 {
    None = 0,
    // 렌더 버퍼가 모자라 message가 잘렸다(후속 단계 LG2·LG3이 켠다).
    MessageTruncated = 1u << 0,
    // 문맥 스택 깊이를 넘어 안쪽 문맥을 버렸다(후속 단계 LG4가 켠다).
    ContextTruncated = 1u << 1
};

constexpr LogEventFlags operator|(LogEventFlags left, LogEventFlags right) noexcept {
    return static_cast<LogEventFlags>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr LogEventFlags operator&(LogEventFlags left, LogEventFlags right) noexcept {
    return static_cast<LogEventFlags>(static_cast<uint32>(left) & static_cast<uint32>(right));
}

// value에 flags 중 하나라도 켜져 있는지다.
constexpr bool HasAnyLogEventFlags(LogEventFlags value, LogEventFlags flags) noexcept {
    return (value & flags) != LogEventFlags::None;
}

// sink가 받는 로그 기록 한 건이다(Docs/Design/LoggingSystem.ko.md 9절). 렌더된 메시지와 원본 정보를
// 함께 담아 sink가 출력 형식을 고를 수 있게 한다.
//
// 수명: 모든 view(문자열·Span)는 RuntimeLogSink::Write 콜백 동안만 유효하다. 보관하거나 다른
// 스레드로 넘기려면 복사한다. location의 문자열만 정적 저장소다.
//
// 기록 경로가 아직 채우지 않는 칸은 빈 값·0이다. 지금(LG1) Logger가 채우는 칸은 level·category·
// message·location·threadId·monotonicNanoseconds이며, 나머지는 칸 주석의 단계에서 채운다.
//
// 사용 예(sink 구현):
//   Result<void> Write(const LogEvent& event) noexcept override {
//       if (event.level >= LogLevel::Error) {
//           ++errorCount_;
//       }
//
//       return CopyToRing(event.category, event.message); // 보관하려면 복사한다
//   }
struct LogEvent {
    LogLevel level = LogLevel::Info;
    // 카테고리 이름(LogCategory::Name). ASCII이며 점으로 구간을 나눈다.
    StringView category;
    // 원본 메시지 템플릿(`"Loaded {Count} assets"`). LG2에서 채운다. 그 전에는 빈 값이다.
    StringView messageTemplate;
    // 템플릿 해시로 만든 사건 종류 id. LG2에서 채운다. 그 전에는 0이다.
    uint64 eventId = 0;
    // 렌더된 메시지. 유효한 UTF-8이며 embedded NUL을 포함할 수 있다.
    StringView message;
    // 템플릿 자리에 묶인 속성. LG2에서 채운다. 그 전에는 비어 있다.
    Span<const LogProperty> properties;
    // 기록 시점의 문맥(LogContextScope). LG4에서 채운다. 그 전에는 비어 있다.
    Span<const LogProperty> context;
    // 기록을 부른 소스 위치.
    SourceLocation location;
    // 기록한 스레드의 OS id(platform::CurrentThreadId).
    uint64 threadId = 0;
    // 기록한 스레드의 이름. LG3에서 채운다. 그 전에는 빈 값이다(출력은 id로 대신한다).
    StringView threadName;
    // 엔진 프레임 번호. LG6(LogSystem::SetFrame)에서 채운다. 그 전에는 0이다.
    uint64 frame = 0;
    // 기록 시각, 단조 시계 ns(platform::MonotonicNanoseconds). UTC가 아니다.
    uint64 monotonicNanoseconds = 0;
    // 기록 시각, UTC tick(platform::UtcTicks 단위). LG3 출력 스레드가 환산해 채운다. 그 전에는
    // 0이다.
    int64 utcTicks = 0;
    LogEventFlags flags = LogEventFlags::None;
};

} // namespace rocks
