#include <Launch/EntryPoint.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>
#include <ShaderWorker/ShaderWorker.h>
#include <stdio.h>

// ShaderCompileWorker 진입점이다(Docs/Design/ShaderSystem 11절, 11.1절 SC4–SC7). 본체는
// ShaderWorker다. 오류 대화상자를 끄고(abort·assert·OS 오류 상자), 크래시는 작업 디렉터리의 Crashes
// 아래에 프로세스 안 덤프로 남긴다(워커는 대화상자에 막히면 안 된다 — SC6). 종료 코드는
// ShaderWorkerExit다.
namespace {

using rocks::shader::ShaderWorkerExit;

// 크래시 폴더 경로(<work-dir>/Crashes)다. Launch가 설정 적용 단계까지 읽는다.
char crashDirectory[rocks::shader::MaxShaderWorkerPathBytes];

void Configure(rocks::Span<const rocks::StringView> arguments,
               rocks::launch::ApplicationSettings& settings) noexcept {
    rocks::shader::ShaderWorkerArguments parsed;
    if (!rocks::shader::ParseShaderWorkerArguments(arguments, parsed)) {
        return;
    }

    const int written =
        snprintf(crashDirectory, sizeof(crashDirectory), "%.*s/Crashes",
                 static_cast<int>(parsed.workDirectory.ByteCount()), parsed.workDirectory.Data());
    if (written > 0 && static_cast<rocks::usize>(written) < sizeof(crashDirectory)) {
        settings.crashDirectory = crashDirectory;
    }
}

int Run(rocks::launch::ApplicationContext& context) noexcept {
    rocks::shader::ShaderWorkerArguments arguments;
    if (!rocks::shader::ParseShaderWorkerArguments(context.Arguments(), arguments)) {
        fprintf(stderr,
                "usage: ShaderCompileWorker --work-dir <dir> --parent-pid <pid> --worker <n> "
                "[--keep-input] [--test-hooks]\n");
        return static_cast<int>(ShaderWorkerExit::BadArguments);
    }

    return rocks::shader::RunShaderWorker(arguments, rocks::shader::StandardShaderWorkerControl(),
                                          context.DefaultAllocator());
}

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "ShaderCompileWorker",
        .kind = rocks::launch::ApplicationKind::Tool,
        .settings = {.crashReporting = rocks::launch::CrashReportingMode::InProcess,
                     .suppressSystemErrorDialogs = true},
        .configure = &Configure,
        .run = &Run,
        .commandLineFailureExitCode = static_cast<int>(ShaderWorkerExit::BadArguments)};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
