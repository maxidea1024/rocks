#include "SpikeRun.h"
#include "SpikeTools.h"

#include <Platform/Memory.h>
#include <stdio.h>

// 셰이더 도구 스파이크(SH0)다. bootstrap한 사용자 저장소의 도구로 지금의 내장 Renderer 셰이더를
// 모든 형식으로 만들고 검증·측정한다(Docs/Design/ShaderSystem.ko.md 5.4·16절).
//
// 사용:
//   ShaderToolSpike --out <디렉터리> [--sm 6_0]
//
// 종료 코드: 0 필수 검사 통과, 1 검사 실패·쓰기 실패, 2 도구를 열지 못함(bootstrap 필요), 64 인자
// 오류. 결정성 측정 S3은 실행마다 만든 <디렉터리>/manifest.json을 비교한다.
namespace {

constexpr int UsageExitCode = 64;

void PrintUsage() noexcept {
    fprintf(stderr, "usage: ShaderToolSpike --out <directory> [--sm 6_0]\n");
}

// DXIL 셰이더 모델은 "6_0"–"6_9"만 받는다.
bool IsShaderModel(const char* text) noexcept {
    return text[0] == '6' && text[1] == '_' && text[2] >= '0' && text[2] <= '9' && text[3] == '\0';
}

} // namespace

int main(int argc, char** argv) {
    rocks::spike::PrepareSpikeProcess();
    rocks::spike::RunOptions options;
    for (int index = 1; index < argc; ++index) {
        const bool hasValue = index + 1 < argc;
        if (rocks::platform::CStringEquals(argv[index], "--out") && hasValue) {
            options.outputDirectory = argv[++index];
        } else if (rocks::platform::CStringEquals(argv[index], "--sm") && hasValue &&
                   IsShaderModel(argv[index + 1])) {
            options.shaderModel = argv[++index];
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
