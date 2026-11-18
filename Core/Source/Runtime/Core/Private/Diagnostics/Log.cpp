#include "LogInternal.h"

#include <Core/Diagnostics/Log.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks::detail {
namespace {

// Lazy 기록의 템플릿이다(람다가 만든 메시지 하나를 담는다).
constexpr const char LazyTemplateText[] = "{Message}";

// assert 실패 기록의 카테고리다.
constexpr LogCategory LogAssert{"Assert"};

// LogLine 조각을 렌더할 때 캡처에 쓰는 카테고리다(조각은 기록이 되지 않는다).
constexpr LogCategory LogLineCategory{"Log"};

// 이 스레드가 Fatal을 보고하는 중인지다(로깅 안의 assert → Fatal → 로깅 재귀를 끊는다).
thread_local bool fatalInProgress = false;

// 이 스레드가 출력 쪽인지다.
thread_local bool writerThread = false;

LogCaptureTemplate LazyTemplate(SourceLocation where) noexcept {
    return {StringView(LazyTemplateText, sizeof(LazyTemplateText) - 1),
            HashStringBytes(StringView(LazyTemplateText, sizeof(LazyTemplateText) - 1)), where,
            false, nullptr};
}

// 캡처 한 건을 스택 버퍼에 렌더해 비상 출력으로 낸다.
void WriteCaptureEmergency(Span<const uint8> capture, LogLevel level, StringView category,
                           StringView fallback) noexcept {
    char message[LogEmergencyMessageBytes];
    char texts[MaxLogArguments * 8];
    LogProperty properties[MaxLogArguments];
    LogEvent event;
    const LogRenderTarget target{
        Span<char>(message), Span<char>(texts), Span<LogProperty>(properties), {}};
    if (RenderLogCapture(capture, nullptr, target, event)) {
        WriteLogEmergencyText(event.level, event.category, event.message);
    } else {
        WriteLogEmergencyText(level, category, fallback);
    }
}

} // namespace

LogWriterScope::LogWriterScope() noexcept : previous_(writerThread) {
    writerThread = true;
}

LogWriterScope::~LogWriterScope() noexcept {
    writerThread = previous_;
}

bool OnLogWriterThread() noexcept {
    return writerThread;
}

void WriteLogEmergencyText(LogLevel level, StringView category, StringView message) noexcept {
    // "[" 레벨 "][" 카테고리 "] " 메시지 "\n". 끝 줄바꿈 자리를 남기고 잘라 담는다.
    char line[LogEmergencyMessageBytes + MaxLogCategoryNameBytes + 16];
    FixedStringBuilder text{Span<char>(line, sizeof(line) - 1)};
    (void)text.TryAppendAscii('[');
    (void)text.TryAppend(IsValidLogLevel(level) ? LogLevelName(level) : StringView("Unknown"));
    (void)text.TryAppend("][");
    (void)text.TryAppend(category);
    (void)text.TryAppend("] ");
    (void)text.TryAppend(message);
    const usize bytes = text.ByteCount();
    line[bytes] = '\n';
    platform::WriteDiagnostic(line, bytes + 1);
}

void WriteLogEmergency(LogLevel level, const LogCategory& category, const LogCaptureTemplate& text,
                       const FormatArg* args, usize count) noexcept {
    uint8 capture[LogSlotBytes];
    const auto captured = WriteLogCapture(
        {Span<uint8>(capture), nullptr, platform::MonotonicTicks(), platform::CurrentThreadId(), 0},
        level, category, text, args, count);
    if (!captured) {
        WriteLogEmergencyText(level, category.Name(), text.text);
        return;
    }

    WriteCaptureEmergency(Span<const uint8>(capture, captured.Value()), level, category.Name(),
                          text.text);
}

void LogSubmit(LogLevel level, const LogCategory& category, const LogCaptureTemplate& text,
               const FormatArg* args, usize count) noexcept {
    LogReadScope scope;
    LogSystem* system = scope.System();
    if (system == nullptr) {
        // 설치 전·해제 뒤: Warning 이상은 비상 출력, 그 아래는 세기만 한다(재생하지 않는다).
        if (level >= LogLevel::Warning) {
            WriteLogEmergency(level, category, text, args, count);
        } else {
            CountUninstalledDrop();
        }

        return;
    }

    if (system->IsEnabled(level, category)) {
        SubmitToLogSystem(*system, level, category, text, args, count);
    }
}

bool LogIsEnabledSlow(LogLevel level, const LogCategory& category, bool countDrop) noexcept {
    LogReadScope scope;
    LogSystem* system = scope.System();
    if (system != nullptr) {
        return system->IsEnabled(level, category);
    }

    if (level >= LogLevel::Warning) {
        return true;
    }

    if (countDrop) {
        CountUninstalledDrop();
    }

    return false;
}

void LogSubmitLazy(LogLevel level, const LogCategory& category, StringView message,
                   SourceLocation where) noexcept {
    const FormatArg arg = MakeFormatArg(message);
    LogSubmit(level, category, LazyTemplate(where), &arg, 1);
}

void LogAppendLine(FixedStringBuilder& out, const LogCaptureTemplate& text, const FormatArg* args,
                   usize count) noexcept {
    uint8 capture[LogSlotBytes];
    const auto captured =
        WriteLogCapture({Span<uint8>(capture)}, LogLevel::Info, LogLineCategory, text, args, count);
    if (!captured) {
        return;
    }

    char message[LogLazyMessageBytes];
    char texts[MaxLogArguments * 8];
    LogProperty properties[MaxLogArguments];
    LogEvent event;
    const LogRenderTarget target{
        Span<char>(message), Span<char>(texts), Span<LogProperty>(properties), {}};
    if (RenderLogCapture(Span<const uint8>(capture, captured.Value()), nullptr, target, event)) {
        (void)out.TryAppend(event.message);
    }
}

[[noreturn]] void LogFatal(const LogCategory& category, const LogCaptureTemplate& text,
                           const FormatArg* args, usize count) noexcept {
    if (fatalInProgress) {
        platform::Fatal("Recursive fatal error while reporting a fatal log record");
    }

    fatalInProgress = true;
    uint8 capture[LogSlotBytes];
    const auto captured = WriteLogCapture(
        {Span<uint8>(capture), nullptr, platform::MonotonicTicks(), platform::CurrentThreadId(), 0},
        LogLevel::Fatal, category, text, args, count);
    if (captured) {
        const Span<const uint8> record(capture, captured.Value());
        // 1) 비상 출력 2) 설치된 LogSystem에 전달(조건부, 제한 시간) 3) 종료
        WriteCaptureEmergency(record, LogLevel::Fatal, category.Name(), text.text);
        LogReadScope scope;
        if (scope.System() != nullptr) {
            DeliverFatalToLogSystem(*scope.System(), record);
        }
    } else {
        WriteLogEmergencyText(LogLevel::Fatal, category.Name(), text.text);
    }

    platform::Fatal("Rocks fatal log record; aborting");
}

[[noreturn]] void ReportAssertFailure(const char* message) noexcept {
    if (fatalInProgress) {
        platform::Fatal(message);
    }

    fatalInProgress = true;
    const StringView text(message);
    WriteLogEmergencyText(LogLevel::Fatal, LogAssert.Name(), text);
    uint8 capture[LogSlotBytes];
    const FormatArg arg = MakeFormatArg(text);
    const auto captured = WriteLogCapture(
        {Span<uint8>(capture), nullptr, platform::MonotonicTicks(), platform::CurrentThreadId(), 0},
        LogLevel::Fatal, LogAssert, LazyTemplate(SourceLocation()), &arg, 1);
    if (captured) {
        LogReadScope scope;
        if (scope.System() != nullptr) {
            DeliverFatalToLogSystem(*scope.System(), Span<const uint8>(capture, captured.Value()));
        }
    }

    platform::Fatal("Rocks invariant failed; aborting");
}

} // namespace rocks::detail

namespace rocks {

void Log::SetFrame(uint64 frame) noexcept {
    detail::LogReadScope scope;
    if (LogSystem* system = scope.System(); system != nullptr) {
        system->SetFrame(frame);
    }
}

} // namespace rocks
