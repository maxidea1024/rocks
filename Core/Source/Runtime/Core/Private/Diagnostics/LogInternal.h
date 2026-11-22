#pragma once

#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Time/TimeSpan.h>

namespace rocks {

// 로그 모듈이 스스로 남기는 진단(버림 알림·sink 차단·파일 sink 등)의 카테고리다.
inline constexpr LogCategory LogLogging{"Log"};

// 로그 모듈이 스스로 만드는 알림 한 줄의 버퍼 byte 수다.
inline constexpr usize LogNoticeBytes = 256;

// 시간 단위다. UTC tick(DateTime·TimeSpan) 하나는 100 ns다.
inline constexpr uint64 LogNanosecondsPerMillisecond = 1000000;
inline constexpr uint64 LogNanosecondsPerUtcTick = 100;
static_assert(TimeSpan::TicksPerSecond * LogNanosecondsPerUtcTick == 1000000000,
              "a DateTime tick must be 100 ns");

} // namespace rocks

// 로그 모듈 내부 공용 함수다(Log.cpp·LogSystem.cpp·Diagnostics.cpp). 공개 API가 아니다.
namespace rocks::detail {

// 비상 출력 한 줄의 최대 메시지 byte 수다(설계 5절의 고정 1 KiB 스택 버퍼).
inline constexpr usize LogEmergencyMessageBytes = 1024;

// 설치 읽기 범위다. 생성에서 현재 세대 카운터를 올리고 설치된 LogSystem을 읽으며, 소멸에서 내린다.
// 이 범위 안에서 System()이 가리키는 LogSystem은 해제(LogInstallScope 소멸)가 기다려 주므로 살아
// 있다.
class LogReadScope {
public:
    LogReadScope() noexcept;
    ~LogReadScope() noexcept;
    LogReadScope(const LogReadScope&) = delete;
    LogReadScope& operator=(const LogReadScope&) = delete;

    LogSystem* System() const noexcept {
        return system_;
    }

private:
    LogSystem* system_ = nullptr;
    uint64 generation_ = 0;
};

// 이 스레드가 출력 쪽(출력 스레드 또는 Pump)인 동안 세운다. 그 안의 로그는 비상 경로로 간다.
class LogWriterScope {
public:
    LogWriterScope() noexcept;
    ~LogWriterScope() noexcept;
    LogWriterScope(const LogWriterScope&) = delete;
    LogWriterScope& operator=(const LogWriterScope&) = delete;

private:
    bool previous_;
};

bool OnLogWriterThread() noexcept;

// "[레벨][카테고리] 메시지\n"을 OS 쓰기로 바로 낸다(힙·lock 없음). 실패는 무시한다.
void WriteLogEmergencyText(LogLevel level, StringView category, StringView message) noexcept;

// 기록 한 건을 스택 버퍼에 캡처·렌더해 비상 출력으로 낸다.
void WriteLogEmergency(LogLevel level, const LogCategory& category, const LogCaptureTemplate& text,
                       const FormatArg* args, usize count) noexcept;

// 설치 전·해제 뒤 버린 낮은 레벨 기록을 센다.
void CountUninstalledDrop() noexcept;

// system이 설치된 대상이면 빠른 거부 하한을 minimum으로 맞춘다(설치 lock 안).
void RefreshLogFastMinimum(LogSystem& system, uint8 minimum) noexcept;

void SubmitToLogSystem(LogSystem& system, LogLevel level, const LogCategory& category,
                       const LogCaptureTemplate& text, const FormatArg* args, usize count) noexcept;
void DeliverFatalToLogSystem(LogSystem& system, Span<const uint8> capture) noexcept;

// 출력 스레드의 CurrentThreadId 값이다(아직 없으면 0). 종료 flush의 출력 스레드 판정을 시험이
// 재현할 때 쓴다.
ROCKS_CORE_API uint64 LogWriterThreadId(const LogSystem& system) noexcept;

// AssertFailed의 보고 경로다(설계 11절): 비상 출력 → 설치된 LogSystem에 Fatal 기록 전달 → 종료.
// 같은 스레드에서 Fatal 보고 중이면 바로 platform::FatalAt으로 간다. 종료는
// platform::FatalAt(file·line)이다.
[[noreturn]] void ReportAssertFailure(const char* message, const char* file, uint32 line) noexcept;

// 특정 LogSystem에 바로 기록한다(설치와 무관하게, 레벨 필터는 지난다). LogSystem 내부 알림용이다.
template <typename... Args>
void LogTo(LogSystem& system, LogLevel level, const LogCategory& category,
           LogTemplate<TypeIdentityT<Args>...> text, const Args&... args) noexcept {
    if (!system.IsEnabled(level, category)) {
        return;
    }

    if constexpr (sizeof...(Args) == 0) {
        SubmitToLogSystem(system, level, category, ToCaptureTemplate(text), nullptr, 0);
    } else {
        const FormatArg list[] = {MakeLogArg(args)...};
        SubmitToLogSystem(system, level, category,
                          WithArgumentInfo<Args...>(ToCaptureTemplate(text)), list,
                          sizeof...(Args));
    }
}

} // namespace rocks::detail
