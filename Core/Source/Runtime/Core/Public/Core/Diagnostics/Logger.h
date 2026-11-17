#pragma once

#include <Core/Diagnostics/LogCategory.h>
#include <Core/Diagnostics/LogEvent.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Diagnostics/SourceLocation.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Platform/Sync.h>

namespace rocks {

// Logger는 sink를 소유하지 않는다. sink는 등록부터 Remove 반환까지 살아 있어야 한다.
// 하나의 sink를 여러 Logger에 등록할 수 있으며 그 경우 sink가 동시 호출을 직접 보호한다.
// callback은 noexcept이며 event의 문자열을 보관하려면 복사한다. callback 안에서 어떤 Logger에도
// 재진입하거나 다른 logger 사용자의 완료를 기다리지 않는다. 순환 대기를 피하기 위함이다.
class RuntimeLogSink {
public:
    virtual ~RuntimeLogSink() noexcept = default;
    // 기록 한 건을 출력한다. event의 view는 이 호출 동안만 유효하다(LogEvent 참고).
    virtual Result<void> Write(const LogEvent& event) noexcept = 0;
};

// sink 하나의 기록 필터다. level이 minimum 이상이고 카테고리가 category 접두사 아래면 통과한다.
// Fatal은 카테고리 조건과 무관하게 늘 통과한다(어떤 필터도 Fatal을 막지 않는다). category
// 기본값(빈 접두사)은 모든 카테고리를 통과시킨다. category를 LogCategory에서 만들면 이름이
// 정적 저장소라 등록 수명 제약이 없다. 직접 채운 접두사는 등록된 동안 이름 저장소가 살아 있어야
// 한다.
//
// 사용 예:
//   inline constexpr LogCategory LogGraphics{"Graphics"};
//   logger.Add(sink, {LogLevel::Warning, LogGraphics}); // Graphics·Graphics.* 의 Warning 이상
struct LogFilter {
    LogLevel minimum = LogLevel::Trace;
    LogCategoryPrefix category{};
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
    // 기록 한 건을 필터를 통과한 sink에 등록 순서대로 넘긴다. sink가 받는 LogEvent에는 level·
    // category 이름·message·where·호출 스레드 id·단조 시각(ns)이 들어간다.
    // - message: 유효한 UTF-8이어야 한다(embedded NUL 허용). 아니면 InvalidArgument.
    // - level: 정의된 LogLevel이어야 한다. 아니면 InvalidArgument. Fatal은 sink 필터의 카테고리
    //   조건을 무시하고 모든 sink에 넘기며, 기록만 하고 종료하지 않는다.
    // - where: 기본 인자가 호출 지점의 파일·함수·줄을 담는다. 보통 넘기지 않는다.
    // 필터를 통과한 sink가 하나도 없어도 성공이다. 한 sink가 실패해도 나머지는 호출하며 하나라도
    // 실패하면 DependencyFailed를 반환한다. sink callback에서 Logger에 재진입하면 잠그기 전에
    // Busy를 반환한다.
    //
    // 사용 예:
    //   inline constexpr LogCategory LogEngine{"Engine"};
    //   (void)logger.Write(LogLevel::Info, LogEngine, "Engine initialized");
    Result<void> Write(LogLevel level, const LogCategory& category, StringView message,
                       SourceLocation where = SourceLocation::Current()) noexcept;

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

// 출력 정책을 주입하는 동기식 텍스트 sink다. 호출마다 prefix `[레벨][카테고리 이름] `, 원본 byte,
// 줄바꿈을 순서대로 출력하고 flush한다(예: `[Warning][Graphics.D3D12] Device lost`). 부분 출력 뒤
// 실패하면 되돌리지 않는다. 같은 인스턴스의 호출은 직렬화한다.
// 잘못된 레벨, UTF-8이 아닌 message, 카테고리 이름 문법(ValidateLogCategoryName)을 어긴 category는
// InvalidArgument다.
class TextLogSink final : public RuntimeLogSink {
public:
    explicit TextLogSink(LogTextOutput output) noexcept : output_(output) {
    }
    Result<void> Write(const LogEvent& event) noexcept override;

private:
    platform::Mutex mutex_;
    LogTextOutput output_;
};

// stderr로 UTF-8 byte를 출력하고 즉시 flush한다. Windows console의 실제 글자 표시는
// console code page/환경에 의존한다. CRT의 text mode는 줄바꿈을 변환할 수 있다.
// 다른 stderr 사용자와의 원자성은 보장하지 않는다.
// stderr의 기존 error flag도 실패로 보고하며 clearerr는 호출하지 않는다. 복구와 오류 상태
// 초기화는 다른 stderr 사용자와의 동기화를 포함하여 호스트가 책임진다.
// Unix에서 읽는 쪽이 닫힌 pipe는 기본 SIGPIPE 설정이면 프로세스를 종료할 수 있다.
// 엔진은 전역 signal 처리를 바꾸지 않으며, 오류 반환을 원하면 호스트가 정책을 설정해야 한다.
class StderrLogSink final : public RuntimeLogSink {
public:
    StderrLogSink() noexcept;
    Result<void> Write(const LogEvent& event) noexcept override;

private:
    TextLogSink sink_;
};

} // namespace rocks
