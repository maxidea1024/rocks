#include "SpikeRun.h"
#include "SpikeTools.h"

#include <Launch/EntryPoint.h>
#include <Platform/Memory.h>
#include <stdio.h>

// 셰이더 도구 스파이크(SH0)다. bootstrap한 사용자 저장소의 도구로 지금의 내장 Renderer 셰이더를
// 모든 형식으로 만들고 검증·측정한다(Docs/Design/ShaderSystem 5.4·16절).
//
// 사용:
//   ShaderToolSpike --out <디렉터리> [--sm 6_0]
//
// 종료 코드: 0 필수 검사 통과, 1 검사 실패·쓰기 실패, 2 도구를 열지 못함(bootstrap 필요), 64 인자
// 오류. 결정성 측정 S3은 실행마다 만든 <디렉터리>/manifest.json을 비교한다.
//
// 진입점은 Launch 공통 실행 계층이다(Docs/Design/CrashHandlingAndLaunch.ko.md 16절). 크래시 수집,
// 도구 로그(Sync LogSystem·stderr sink, 도구 열기 실패 등 Log 호출), 무인 실행용 OS·CRT 오류
// 대화상자 끄기를 Launch 설정으로 둔다. 도구 로그를 시작하지 못해도 본문은 돈다.
namespace {

constexpr int UsageExitCode = 64;

void PrintUsage() noexcept {
    fprintf(stderr, "usage: ShaderToolSpike --out <directory> [--sm 6_0]\n");
}

// DXIL 셰이더 모델은 "6_0"–"6_9"만 받는다.
bool IsShaderModel(const char* text) noexcept {
    return text[0] == '6' && text[1] == '_' && text[2] >= '0' && text[2] <= '9' && text[3] == '\0';
}

int Run(rocks::launch::ApplicationContext& context) noexcept {
    rocks::spike::RunOptions options;
    const rocks::Span<const rocks::StringView> arguments = context.Arguments();
    for (rocks::usize index = 0; index < arguments.Size(); ++index) {
        const bool hasValue = index + 1 < arguments.Size();
        if (rocks::platform::CStringEquals(arguments[index].Data(), "--out") && hasValue) {
            options.outputDirectory = arguments[++index].Data();
        } else if (rocks::platform::CStringEquals(arguments[index].Data(), "--sm") && hasValue &&
                   IsShaderModel(arguments[index + 1].Data())) {
            options.shaderModel = arguments[++index].Data();
        } else {
            PrintUsage();
            return UsageExitCode;
        }
    }

    if (options.outputDirectory == nullptr) {
        PrintUsage();
        return UsageExitCode;
    }

    return rocks::spike::RunSpike(options);
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "ShaderToolSpike",
        .kind = rocks::launch::ApplicationKind::Tool,
        .settings = {.suppressSystemErrorDialogs = true,
                     .toolLogging = rocks::launch::ToolLogging::Stderr},
        .run = &Run};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
