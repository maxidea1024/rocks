#pragma once

#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>

namespace rocks::ui::test {

// 시험 하나 동안 Warning 이상 로그를 범주별로 센다. 수명 경고(Docs/Design/UiToolkit 21.4절 —
// 명시 해제 없이 버려진 UserWidget Element, Shutdown 없이 부서진 Application)가 없음을 단언할 때
// 쓴다. 이 범위 동안 Log::* 가 여기로 가고, 끝나면 이전 대상으로 돌아간다. Manual 모드라 세는
// 함수가 쌓인 기록을 먼저 내보낸다.
//
// 사용 예:
//   test::WarningCapture warnings;
//   REQUIRE(warnings.started);
//   ...
//   CHECK(warnings.UiWarnings() == 0);
struct WarningCapture {
    WarningCapture() noexcept : logs(logAllocator, Config()), installed(logs) {
        started = logs.Start().IsOk() && logs.AddSink(sink).IsOk();
    }

    static LogSystemConfig Config() noexcept {
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        config.globalMinimum = LogLevel::Warning;
        return config;
    }

    // UI 범주(UiWidget·UiFramework)의 Warning 이상 기록 수다.
    uint32 UiWarnings() noexcept {
        (void)logs.Pump();
        return sink.ui;
    }

    // 그 밖의 범주의 Warning 이상 기록 수다.
    uint32 OtherWarnings() noexcept {
        (void)logs.Pump();
        return sink.other;
    }

    class Sink final : public RuntimeLogSink {
    public:
        Result<void> Write(const LogEvent& event) noexcept override {
            if (event.level < LogLevel::Warning) {
                return Result<void>::Success();
            }

            if (event.category == "UiWidget" || event.category == "UiFramework") {
                ++ui;
            } else {
                ++other;
            }

            return Result<void>::Success();
        }

        uint32 ui = 0;
        uint32 other = 0;
    };

    Sink sink;
    SystemAllocator logAllocator;
    LogSystem logs;
    LogInstallScope installed;
    bool started = false;
};

} // namespace rocks::ui::test
