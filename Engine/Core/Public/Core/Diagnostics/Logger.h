#pragma once

#include <Core/Diagnostics/LogRecord.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/Sync.h>

namespace rocks {

// 문자열은 호출 동안만 빌린다. 저장하거나 비동기로 전달할 sink는 반드시 복사해야 한다.
// timestamp는 단조 시계(ns)이며 UTC가 아니다. message는 embedded NUL을 포함할 수 있다.
struct LogMessage {
    uint64 timestamp;
    uint64 thread;
    uint32 category;
    LogLevel level;
    StringView message;
};

// Logger는 sink를 소유하지 않는다. sink는 등록부터 Remove 반환까지 살아 있어야 한다.
// 하나의 sink를 여러 Logger에 등록할 수 있으며 그 경우 sink가 동시 호출을 직접 보호한다.
// callback은 noexcept이며 메시지를 보관하려면 복사한다. callback 안에서 어떤 Logger에도
// 재진입하거나 다른 logger 사용자의 완료를 기다리지 않는다. 순환 대기를 피하기 위함이다.
class RuntimeLogSink {
public:
    virtual ~RuntimeLogSink() noexcept = default;
    virtual Result<void> Write(const LogMessage& message) noexcept = 0;
};

struct LogFilter {
    LogLevel minimum = LogLevel::Trace;
    bool matchCategory = false;
    uint32 category = 0;
};

// 고정 용량의 동기식 fan-out이다. bootstrap 단일 스레드 LogBuffer와 별개다.
// 모든 메서드는 동시 호출 가능하며 등록 순서대로 sink를 호출한다. 호출과 등록/해제는
// 직렬화된다. Remove가 성공하면 이 Logger의 해당 sink callback은 더 이상 실행 중이 아니다.
// 파괴 전에 모든 Logger 호출을 종료하고 sink 수명을 정리해야 한다. 내부 할당은 없다.
// 느리거나 멈춘 sink는 다른 기록과 등록/제거도 지연시킨다. timeout이나 비동기 큐는 없다.
class Logger {
public:
    static constexpr usize MaxSinks = 16;

    Logger() noexcept = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // 중복 등록은 AlreadyExists, 용량 초과는 CapacityExceeded다. filter는 복사한다.
    Result<void> Add(RuntimeLogSink& sink, LogFilter filter = {}) noexcept;
    // 미등록 sink는 NotFound다. 모든 Logger에서 제거하고 다른 사용도 끝나야 sink를 파괴한다.
    Result<void> Remove(RuntimeLogSink& sink) noexcept;
    // 미등록 sink는 NotFound다. 변경은 진행 중인 Write가 끝난 뒤 적용된다.
    Result<void> SetFilter(RuntimeLogSink& sink, LogFilter filter) noexcept;
    // 유효한 UTF-8을 요구한다. 필터를 통과한 sink가 하나도 없어도 성공이다.
    // 한 sink가 실패해도 나머지는 호출하며 하나라도 실패하면 DependencyFailed를 반환한다.
    // sink callback에서 Logger에 재진입하면 잠그기 전에 Busy를 반환한다.
    Result<void> Write(LogLevel level, uint32 category, StringView message) noexcept;

private:
    struct Slot {
        RuntimeLogSink* sink = nullptr;
        LogFilter filter{};
    };
    platform::Mutex mutex_;
    Slot sinks_[MaxSinks]{};
    usize count_ = 0;
};

// callback과 context는 sink가 소멸하고 모든 호출이 끝날 때까지 살아야 한다. write는 전달받은
// byte 전체를 소비하거나 실패해야 하며 원본을 보관하려면 복사한다. callback에서 Logger에
// 재진입하거나 다른 Logger 호출을 기다리지 않는다. 같은 sink에 직접 재진입하지 않는다.
struct LogTextOutput {
    void* context = nullptr;
    Result<void> (*write)(void*, StringView) noexcept = nullptr;
    Result<void> (*flush)(void*) noexcept = nullptr;
};

// 출력 정책을 주입하는 동기식 텍스트 sink다. 호출마다 prefix, 원본 byte, 줄바꿈을 순서대로
// 출력하고 flush한다. 부분 출력 뒤 실패하면 되돌리지 않는다. 같은 인스턴스의 호출은 직렬화한다.
class TextLogSink final : public RuntimeLogSink {
public:
    explicit TextLogSink(LogTextOutput output) noexcept : output_(output) {
    }
    Result<void> Write(const LogMessage& message) noexcept override;

private:
    platform::Mutex mutex_;
    LogTextOutput output_;
};

// stderr로 UTF-8 byte를 출력하고 즉시 flush한다. Windows console의 실제 글자 표시는
// console code page/환경에 의존한다. CRT의 text mode는 줄바꿈을 변환할 수 있다.
// 다른 stderr 사용자와의 원자성은 보장하지 않는다.
// Unix에서 읽는 쪽이 닫힌 pipe는 기본 SIGPIPE 설정이면 프로세스를 종료할 수 있다.
// 엔진은 전역 signal 처리를 바꾸지 않으며, 오류 반환을 원하면 호스트가 정책을 설정해야 한다.
class StderrLogSink final : public RuntimeLogSink {
public:
    StderrLogSink() noexcept;
    Result<void> Write(const LogMessage& message) noexcept override;

private:
    TextLogSink sink_;
};

} // namespace rocks
