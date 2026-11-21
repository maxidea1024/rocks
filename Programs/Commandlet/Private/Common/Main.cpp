#include "CommandletHost.h"

#include <Launch/EntryPoint.h>

// RocksCommandlet 진입점이다(Launch 공통 실행 계층, Docs/Design/CrashHandlingAndLaunch.ko.md 16절).
// Launch가 명령줄을 UTF-8로 캡처하고(잘못된 인코딩은 사용법 오류 2), 크래시 수집과 도구 로그(Sync
// LogSystem·stderr sink)를 설치한다. 본문은 commandlet 인수 상한을 확인한 뒤 명령을 돌린다. 종료
// 코드: 성공 0, 사용법·알 수 없는 명령 2, 내부 실행·출력 오류 1(README.ko.md).
namespace {

constexpr int UsageExitCode = 2;
constexpr int OutputFailureExitCode = 1;

int Usage(const rocks::CommandletOutput& output, rocks::StringView message) noexcept {
    return output.write(output.context, rocks::CommandletStream::Error, message)
               ? UsageExitCode
               : OutputFailureExitCode;
}

int Run(rocks::launch::ApplicationContext& context) noexcept {
    using namespace rocks;
    const CommandletOutput output = commandlet_host::StandardOutput();
    if (!commandlet_host::PrepareStandardOutput()) {
        return OutputFailureExitCode;
    }

    const Span<const StringView> arguments = context.Arguments();
    if (arguments.Size() > CommandletRegistry::MaxArguments) {
        return Usage(output, "Too many or invalid arguments.\n");
    }

    for (const StringView argument : arguments) {
        if (argument.ByteCount() > CommandletRegistry::MaxArgumentBytes) {
            return Usage(output, "Invalid or oversized UTF-8 argument.\n");
        }
    }

    return commandlet_host::Run(arguments, output, context.DefaultAllocator(), context.Modules());
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "RocksCommandlet",
        .kind = rocks::launch::ApplicationKind::Tool,
        .settings = {.toolLogging = rocks::launch::ToolLogging::Stderr},
        .run = &Run,
        .commandLineFailureExitCode = UsageExitCode};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
