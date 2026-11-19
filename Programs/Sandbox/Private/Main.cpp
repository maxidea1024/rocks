#include "EngineHost.h"
#include "GraphicsSelfTest.h"

#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <stdio.h>

// Sandbox 진입점이다.
//   Sandbox [--self-test]          창 없이 그래픽 backend·엔진 호스트 검사(종료 코드 0 = 통과)
//   Sandbox --engine [--frames N] [--config <Engine.json>] [--set=<target>=<value>]...
//                  [--trace=<목록>]... [--trace-file <path>]
//                                  헤드리스 엔진을 N 프레임(기본 60) 돌린다. 엔진 층 설정
//                                  파일(Engine/Config) 다음 설정 파일(Project 층 — "Settings"
//                                  섹션의 log.* 등), 그다음 --set(Session 층) 순으로 합친다.
//                                  --trace는 trace 채널·묶음을 켠다(TraceControl)
//
// 로그(Docs/Design/LoggingSystem.ko.md 5.1절): 자체 검사는 명령줄 도구처럼 Sync, 엔진 실행은
// LogHost가 설정으로 만든 LogSystem(기본 Async)을 stderr sink와 함께 설치한다. 자체 검사의
// PASS/FAIL 줄은 시험 결과 출력이라 표준 출력 printf로 남긴다.
namespace {

int Usage() noexcept {
    fprintf(stderr, "usage: Sandbox [--self-test] | --engine [--frames N] [--config <file>] "
                    "[--set=<target>=<value>]... [--trace=<channels>]... "
                    "[--trace-file <path>]\n");
    return 2;
}

// 1 이상 10진 정수만 받는다.
bool ParseFrames(const char* text, rocks::uint64& frames) noexcept {
    rocks::uint64 value = 0;
    if (text[0] == '\0') {
        return false;
    }

    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9' || value > 1000000000u) {
            return false;
        }

        value = value * 10 + static_cast<rocks::uint64>(*digit - '0');
    }

    frames = value;
    return value != 0;
}

int RunSelfTest(rocks::Allocator& allocator) noexcept {
    rocks::StderrLogSink stderrSink;
    rocks::LogSystem logs(allocator, rocks::LogSystemConfig::ForTool());
    if (!logs.AddSink(stderrSink) || !logs.Start()) {
        fprintf(stderr, "Sandbox: the log system could not start\n");
        return 1;
    }

    int graphics = 0;
    rocks::uint32 engine = 0;
    {
        rocks::LogInstallScope installed(logs);
        graphics = sandbox::RunGraphicsSelfTest(allocator);
        engine = sandbox::RunEngineHostSelfTest();
    }

    logs.Stop();
    return graphics == 0 && engine == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    rocks::SystemAllocator allocator;
    bool engine = false;
    sandbox::EngineHostOptions options;
    // --set·--trace 인자다(나온 순서). argv를 빌린다.
    constexpr int MaxSettingArguments = 64;
    rocks::StringView settingArguments[MaxSettingArguments];
    int settingCount = 0;
    for (int index = 1; index < argc; ++index) {
        if (rocks::platform::CStringEquals(argv[index], "--self-test")) {
            continue;
        }

        if (rocks::platform::CStringEquals(argv[index], "--engine")) {
            engine = true;
        } else if (rocks::platform::CStringEquals(argv[index], "--frames") && index + 1 < argc &&
                   ParseFrames(argv[index + 1], options.frames)) {
            ++index;
        } else if (rocks::platform::CStringEquals(argv[index], "--config") && index + 1 < argc) {
            options.configPath = argv[++index];
        } else if (rocks::platform::CStringEquals(argv[index], "--trace-file") &&
                   index + 1 < argc) {
            options.traceFile = argv[++index];
        } else if ((rocks::StringView(argv[index])
                        .StartsWith("--set=", rocks::StringComparison::CaseSensitive) ||
                    rocks::StringView(argv[index])
                        .StartsWith("--trace=", rocks::StringComparison::CaseSensitive)) &&
                   settingCount < MaxSettingArguments) {
            settingArguments[settingCount++] = rocks::StringView(argv[index]);
        } else {
            return Usage();
        }
    }

    options.arguments = rocks::Span<const rocks::StringView>(
        settingArguments, static_cast<rocks::usize>(settingCount));
    return engine ? sandbox::RunEngineHost(allocator, options) : RunSelfTest(allocator);
}
