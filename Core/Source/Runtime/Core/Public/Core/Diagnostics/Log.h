#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogCategory.h>
#include <Core/Diagnostics/LogRecord.h>
#include <Core/Diagnostics/LogTemplate.h>
#include <Core/Diagnostics/SourceLocation.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>

// 로그 호출 API다(Docs/Design/LoggingSystem.ko.md 6·7·11절). 매크로 없이 함수로 부른다.
//
// 사용 예:
//   inline constexpr LogCategory LogEngine{"Engine"};
//   Log::Info(LogEngine, "Engine initialized with {Count} subsystems", count);
//   Log::Warning(LogEngine, "Frame took {Milliseconds:.1f} ms", milliseconds);
//   Log::DebugLazy(LogEngine, [&](LogLine& line) noexcept {
//       line.Write("World has {Actors} actors", world.CountActors()); // 꺼져 있으면 부르지 않는다
//   });
//
// 레벨 판정 순서: 컴파일 타임 하한(LogCompiledMinimum) → 설치된 LogSystem의 카테고리 재정의(점
// 접두사 최장 일치) → 전역 최소. Fatal은 어떤 필터로도 막지 않는다.
//
// 비활성 비용: 꺼진 레벨은 메시지 서식화와 큐 기록을 하지 않는다. 일반 함수 인자는 C++ 규칙대로
// 평가된다. 비싼 값은 Lazy를 쓴다.
//
// 설치 전·해제 뒤(LogInstallScope 밖): Warning 이상은 고정 스택 버퍼에 렌더해 stderr로 바로 쓰고,
// 그 아래는 버리고 센다(다음 설치 때 한 번 알린다).
namespace rocks {

// 컴파일에 포함하는 최소 레벨이다. 빌드가 ROCKS_LOG_COMPILED_MINIMUM(LogLevel 값 0–5)을 타깃 전체에
// 하나로 정의하면 그것을, 아니면 구성 기본값(Debug: Trace, Development: Debug, Release: Info)을
// 쓴다. 모듈마다 다르면 inline 함수 본문이 TU마다 달라져 ODR 위반이므로 모듈이 따로 정의하지
// 않는다.
#ifdef ROCKS_LOG_COMPILED_MINIMUM
inline constexpr LogLevel LogCompiledMinimum = static_cast<LogLevel>(ROCKS_LOG_COMPILED_MINIMUM);
#else
inline constexpr LogLevel LogCompiledMinimum = IsDebugBuild         ? LogLevel::Trace
                                               : IsDevelopmentBuild ? LogLevel::Debug
                                                                    : LogLevel::Info;
#endif

static_assert(IsValidLogLevel(LogCompiledMinimum),
              "ROCKS_LOG_COMPILED_MINIMUM must be a LogLevel value from 0 (Trace) to 5 (Fatal)");

// Lazy 람다가 한 기록에 쓸 수 있는 메시지 byte 수다(넘치면 잘린다).
inline constexpr usize LogLazyMessageBytes = 384;

class LogSystem;

namespace detail {

// 설치된 LogSystem의 빠른 거부 하한(전역 최소와 재정의 중 가장 낮은 레벨)이다. 설치가 없으면 0이라
// 모든 호출이 느린 경로(설치 전 처리)로 간다. LogSystem이 설치·레벨 변경 때 갱신한다.
extern uint8 logFastMinimum;

void LogSubmit(LogLevel level, const LogCategory& category, const LogCaptureTemplate& text,
               const FormatArg* args, usize count) noexcept;
// 설치된 LogSystem 기준 활성 여부다. 설치가 없으면 Warning 이상만 참이고, countDrop이면 거짓일 때
// 설치 전 버림으로 센다(Lazy가 쓴다).
bool LogIsEnabledSlow(LogLevel level, const LogCategory& category, bool countDrop) noexcept;
void LogSubmitLazy(LogLevel level, const LogCategory& category, StringView message,
                   SourceLocation where) noexcept;
void LogAppendLine(FixedStringBuilder& out, const LogCaptureTemplate& text, const FormatArg* args,
                   usize count) noexcept;
[[noreturn]] void LogFatal(const LogCategory& category, const LogCaptureTemplate& text,
                           const FormatArg* args, usize count) noexcept;

inline bool LogFastReject(LogLevel level) noexcept {
    return static_cast<uint8>(level) <
           platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&logFastMinimum);
}

template <typename... Args>
void LogWrite(LogLevel level, const LogCategory& category, const LogCaptureTemplate& text,
              const Args&... args) noexcept {
    static_assert(sizeof...(Args) <= MaxLogArguments,
                  "A log call takes at most MaxLogArguments arguments");
    if (LogFastReject(level)) {
        return;
    }

    if constexpr (sizeof...(Args) == 0) {
        LogSubmit(level, category, text, nullptr, 0);
    } else {
        const FormatArg list[] = {MakeLogArg(args)...};
        LogSubmit(level, category, WithArgumentInfo<Args...>(text), list, sizeof...(Args));
    }
}

} // namespace detail

// Lazy 람다가 메시지를 쓰는 곳이다. 한 기록에 Write를 여러 번 이어 붙일 수 있다. 람다가 끝나면 그
// 내용이 기록 하나가 된다. LogLazyMessageBytes를 넘으면 잘린다.
class LogLine {
public:
    explicit LogLine(FixedStringBuilder& out) noexcept : out_(out) {
    }

    LogLine(const LogLine&) = delete;
    LogLine& operator=(const LogLine&) = delete;

    // 메시지 템플릿으로 렌더해 이어 붙인다(검증은 Log::Info와 같다).
    template <typename... Args>
    void Write(LogTemplate<TypeIdentityT<Args>...> text, const Args&... args) noexcept {
        if constexpr (sizeof...(Args) == 0) {
            detail::LogAppendLine(out_, detail::ToCaptureTemplate(text), nullptr, 0);
        } else {
            const detail::FormatArg list[] = {detail::MakeLogArg(args)...};
            detail::LogAppendLine(
                out_, detail::WithArgumentInfo<Args...>(detail::ToCaptureTemplate(text)), list,
                sizeof...(Args));
        }
    }

private:
    FixedStringBuilder& out_;
};

// 로그 호출 모음이다. 생성할 수 없다(rocks의 네임스페이스는 소문자라 namespace Log 대신 class).
//
// 각 함수:
// - category: 정적 저장소일 필요가 없다(캡처는 이름만 담는다).
// - text: 문자열 리터럴. 컴파일 타임에 검증된다(LogTemplate). 런타임 문자열은
// RuntimeTemplate{text}.
// - args: 자리 수와 같아야 한다. 정수·실수·bool·char·포인터·enum·문자열·Formatter<T> 타입, 그리고
//   "{@Name}" 자리에는 LogStructure<T> 타입(LG16, Core/Diagnostics/LogStructure.h).
// - 반환은 없다. 로깅 실패는 버림·집계로 끝난다.
class Log {
public:
    Log() = delete;

    template <typename... Args>
    static void Trace(const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                      const Args&... args) noexcept {
        Emit<LogLevel::Trace>(category, detail::ToCaptureTemplate(text), args...);
    }

    template <typename... Args>
    static void Debug(const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                      const Args&... args) noexcept {
        Emit<LogLevel::Debug>(category, detail::ToCaptureTemplate(text), args...);
    }

    template <typename... Args>
    static void Info(const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                     const Args&... args) noexcept {
        Emit<LogLevel::Info>(category, detail::ToCaptureTemplate(text), args...);
    }

    template <typename... Args>
    static void Warning(const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                        const Args&... args) noexcept {
        Emit<LogLevel::Warning>(category, detail::ToCaptureTemplate(text), args...);
    }

    template <typename... Args>
    static void Error(const LogCategory& category, LogTemplate<TypeIdentityT<Args>...> text,
                      const Args&... args) noexcept {
        Emit<LogLevel::Error>(category, detail::ToCaptureTemplate(text), args...);
    }

    // 런타임 템플릿 기록이다. 검증은 출력 때 하고, 잘못되면 원본 + "[template error: <code>]"로
    // 남는다.
    template <typename... Args>
    static void Write(LogLevel level, const LogCategory& category, const RuntimeTemplate& text,
                      const Args&... args) noexcept {
        if (IsValidLogLevel(level) && level >= LogCompiledMinimum) {
            detail::LogWrite(level, category, detail::ToCaptureTemplate(text), args...);
        }
    }

    // 기록하고 프로세스를 끝낸다(설계 11절). 필터·sink 상태와 무관하게 반드시 종료한다.
    // 1) 고정 스택 버퍼에 렌더해 stderr로 바로 쓴다. 2) 설치된 LogSystem이 Async면 큐에 넣고 제한
    // 시간까지 flush를 기다린다(출력 스레드 자신·Manual·비상 모드에서는 건너뛴다. Sync면 sink에
    // 바로 쓴다). 3) platform::Fatal. 같은 스레드에서 다시 들어오면 바로 platform::Fatal로 간다.
    template <typename... Args>
    [[noreturn]] static void Fatal(const LogCategory& category,
                                   LogTemplate<TypeIdentityT<Args>...> text,
                                   const Args&... args) noexcept {
        if constexpr (sizeof...(Args) == 0) {
            detail::LogFatal(category, detail::ToCaptureTemplate(text), nullptr, 0);
        } else {
            const detail::FormatArg list[] = {detail::MakeLogArg(args)...};
            detail::LogFatal(category,
                             detail::WithArgumentInfo<Args...>(detail::ToCaptureTemplate(text)),
                             list, sizeof...(Args));
        }
    }

    // 해당 레벨이 컴파일·런타임에서 활성일 때만 build(LogLine&)를 호출 중 즉시 한 번
    // 부른다(보관하지 않는다). 꺼져 있으면 부르지 않는다. 람다 캡처 초기화([x = Build()])는 호출
    // 전에 평가된다.
    template <typename Build>
    static void TraceLazy(const LogCategory& category, Build&& build,
                          SourceLocation where = SourceLocation::Current()) noexcept {
        Lazy<LogLevel::Trace>(category, build, where);
    }

    template <typename Build>
    static void DebugLazy(const LogCategory& category, Build&& build,
                          SourceLocation where = SourceLocation::Current()) noexcept {
        Lazy<LogLevel::Debug>(category, build, where);
    }

    // level·category의 기록이 지금 남는지다(컴파일 타임 하한 포함). 설치된 LogSystem이 없으면
    // Warning 이상만 참이다(비상 출력).
    static bool IsEnabled(LogLevel level, const LogCategory& category) noexcept {
        if (!IsValidLogLevel(level) || level < LogCompiledMinimum) {
            return level == LogLevel::Fatal;
        }

        return detail::LogIsEnabledSlow(level, category, false);
    }

    // 설치된 LogSystem에 엔진 프레임 번호를 알린다(LogSystem::SetFrame). 이후 기록의
    // LogEvent::frame에 실린다. 설치된 것이 없으면 아무것도 하지 않는다. 원자 저장 하나라 매 프레임
    // 불러도 된다.
    //
    // 사용 예(엔진 루프):
    //   Log::SetFrame(time.frameIndex);
    static void SetFrame(uint64 frame) noexcept;

private:
    template <LogLevel Level, typename... Args>
    static void Emit(const LogCategory& category, const detail::LogCaptureTemplate& text,
                     const Args&... args) noexcept {
        if constexpr (Level >= LogCompiledMinimum) {
            detail::LogWrite(Level, category, text, args...);
        } else {
            (void)category;
            (void)text;
            ((void)args, ...);
        }
    }

    template <LogLevel Level, typename Build>
    static void Lazy(const LogCategory& category, Build& build, SourceLocation where) noexcept {
        if constexpr (Level >= LogCompiledMinimum) {
            if (detail::LogFastReject(Level) || !detail::LogIsEnabledSlow(Level, category, true)) {
                return;
            }

            char storage[LogLazyMessageBytes];
            FixedStringBuilder message{Span<char>(storage)};
            LogLine line(message);
            build(line);
            detail::LogSubmitLazy(Level, category, message.View(), where);
        } else {
            (void)category;
            (void)build;
            (void)where;
        }
    }
};

} // namespace rocks
