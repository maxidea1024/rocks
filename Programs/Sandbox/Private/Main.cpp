#include "EngineHost.h"
#include "GraphicsSelfTest.h"

#include <Launch/EntryPoint.h>
#include <Platform/Memory.h>
#include <stdio.h>

// Sandbox 진입점이다.
//   Sandbox [--self-test] [--gpu-validation]
//                                  창 없이 그래픽 backend·엔진 호스트 검사(종료 코드 0 = 통과).
//   Sandbox --engine-shader-self-test
//                                  장치마다 엔진 셰이더 SL0 GPU probe만
//                                  돈다(Docs/Design/EngineShaders 7.6절 — 반복 검증용,
//                                  --self-test에도 들어 있다)
//                                  --gpu-validation은 D3D12 장치를 GPU 기반 검증으로 만든다(UE
//                                  -gpuvalidation, 아주 느리다)
//   Sandbox --engine [--window] [--frames N] [--config <Engine.json>] [--set=<target>=<value>]...
//                  [--trace=<목록>]... [--trace-file <path>]
//                                  엔진을 N 프레임(헤드리스 기본 60) 돌린다. --window는 창을 열고
//                                  키보드·마우스·게임패드 입력을 Pawn에 넘긴다(실제 시간,
//                                  --frames가 없으면 창을 닫을 때까지). 엔진 층 설정
//                                  파일(Engine/Config) 다음 설정 파일(Project 층 — "Settings"
//                                  섹션의 log.* 등), 그다음 --set(Session 층) 순으로 합친다.
//                                  --trace는 trace 채널·묶음을 켠다(TraceControl)
//                                  첫 Ctrl-C는 다음 프레임 경계에서 정상 종료한다(두 번째는 즉시
//                                  종료).
//                                  --request-termination-at-frame N은 시험용으로 N번째 프레임에서
//                                  같은 종료 요청 표시를 남긴다
//                                  --inject-key-at-frame N은 시험용으로 N번째 프레임에 입력 큐에
//                                  W 키 눌림을 넣는다
//                                  --graphics=<auto|d3d12|d3d11|vulkan|opengl>는 창 렌더 경로의
//                                  장치다(기본 auto — D3D12 → Vulkan → OpenGL ES),
//                                  --graphics-validation은 API 검증 계층을 켜고, --no-render는 렌더
//                                  없이 창만 연다 (Docs/Design/RenderBase 18.5절)
//
// 진입점은 Launch 공통 실행 계층이다(Docs/Design/CrashHandlingAndLaunch.ko.md 16절). Launch가
// 명령줄을 UTF-8로 캡처하고 크래시 수집(덤프 도우미 포함)을 설치한다. 로그(Docs/Design/
// LoggingSystem.ko.md 5.1절): 자체 검사는 Launch 도구 로그(Sync LogSystem·stderr sink)이고, 엔진
// 실행은 LogHost가 설정으로 만든 LogSystem(기본 Async)을 stderr sink와 함께 설치한다(configure가
// --engine을 보고 도구 로그를 끈다). 자체 검사의 PASS/FAIL 줄은 시험 결과 출력이라 표준 출력
// printf로 남긴다.
namespace {

using rocks::Span;
using rocks::StringView;

int Usage() noexcept {
    fprintf(stderr,
            "usage: Sandbox [--self-test] [--gpu-validation] | --engine-shader-self-test | "
            "--cooked-shader-self-test | "
            "--engine [--window] [--frames N] "
            "[--graphics=auto|d3d12|d3d11|vulkan|opengl] [--graphics-validation] [--no-render] "
            "[--config <file>] "
            "[--set=<target>=<value>]... [--trace=<channels>]... "
            "[--trace-file <path>]\n");
    return 2;
}

bool Is(StringView argument, const char* text) noexcept {
    return argument == StringView(text);
}

// 1 이상 10진 정수만 받는다.
bool ParseFrames(StringView text, rocks::uint64& frames) noexcept {
    rocks::uint64 value = 0;
    if (text.ByteCount() == 0) {
        return false;
    }

    for (rocks::usize index = 0; index < text.ByteCount(); ++index) {
        const char digit = text.Data()[index];
        if (digit < '0' || digit > '9' || value > 1000000000u) {
            return false;
        }

        value = value * 10 + static_cast<rocks::uint64>(digit - '0');
    }

    frames = value;
    return value != 0;
}

// 자체 검사(Launch 도구 로그가 설치된 상태)다. 도구 로그를 시작하지 못했어도 검사는 돈다(Warning
// 이상은 비상 경로로 stderr에 간다).
int RunSelfTest(rocks::Allocator& allocator, rocks::ModuleLoader& modules,
                bool gpuValidation) noexcept {
    const int graphics = sandbox::RunGraphicsSelfTest(allocator, modules, gpuValidation);
    const rocks::uint32 engine = sandbox::RunEngineHostSelfTest();
    return graphics == 0 && engine == 0 ? 0 : 1;
}

// 엔진 실행(--engine)이면 Launch 도구 로그를 쓰지 않고(LogHost가 설정으로 로그를 만든다), 첫
// Ctrl-C·Ctrl-Break·SIGINT·SIGTERM을 종료 요청 표시로 받는다(엔진 PumpEvents의 다리가 정상 종료로
// 잇는다. 두 번째 요청은 OS 기본 동작으로 바로 끝난다).
void Configure(Span<const StringView> arguments,
               rocks::launch::ApplicationSettings& settings) noexcept {
    for (const StringView argument : arguments) {
        if (Is(argument, "--engine")) {
            settings.toolLogging = rocks::launch::ToolLogging::None;
            settings.handleTerminationRequests = true;
        }
    }
}

int Run(rocks::launch::ApplicationContext& context) noexcept {
    const Span<const StringView> arguments = context.Arguments();
    bool engine = false;
    sandbox::EngineHostOptions options;
    // --set·--trace 인자다(나온 순서). Launch 명령줄 저장소를 빌린다(프로세스 수명).
    constexpr rocks::usize MaxSettingArguments = 64;
    StringView settingArguments[MaxSettingArguments];
    rocks::usize settingCount = 0;
    bool gpuValidation = false;
    bool engineShaders = false;
    bool cookedShaders = false;
    bool framesGiven = false;
    for (rocks::usize index = 0; index < arguments.Size(); ++index) {
        const StringView argument = arguments[index];
        const bool hasValue = index + 1 < arguments.Size();
        if (Is(argument, "--self-test")) {
            continue;
        }

        if (Is(argument, "--engine")) {
            engine = true;
        } else if (Is(argument, "--engine-shader-self-test")) {
            engineShaders = true;
        } else if (Is(argument, "--cooked-shader-self-test")) {
            cookedShaders = true;
        } else if (Is(argument, "--gpu-validation")) {
            gpuValidation = true;
        } else if (Is(argument, "--window")) {
            options.window = true;
        } else if (Is(argument, "--no-render")) {
            options.render = false;
        } else if (Is(argument, "--graphics-validation")) {
            options.graphicsValidation = true;
        } else if (argument.StartsWith("--graphics=", rocks::StringComparison::CaseSensitive)) {
            constexpr rocks::usize PrefixBytes = sizeof("--graphics=") - 1;
            const StringView name(argument.Data() + PrefixBytes,
                                  argument.ByteCount() - PrefixBytes);
            if (!sandbox::screen::ParseScreenBackend(name, options.graphics)) {
                return Usage();
            }
        } else if (Is(argument, "--frames") && hasValue &&
                   ParseFrames(arguments[index + 1], options.frames)) {
            framesGiven = true;
            ++index;
        } else if (Is(argument, "--config") && hasValue) {
            options.configPath = arguments[++index].Data();
        } else if (Is(argument, "--trace-file") && hasValue) {
            options.traceFile = arguments[++index].Data();
        } else if (Is(argument, "--request-termination-at-frame") && hasValue &&
                   ParseFrames(arguments[index + 1], options.requestTerminationAtFrame)) {
            ++index;
        } else if (Is(argument, "--inject-key-at-frame") && hasValue &&
                   ParseFrames(arguments[index + 1], options.injectKeyAtFrame)) {
            ++index;
        } else if ((argument.StartsWith("--set=", rocks::StringComparison::CaseSensitive) ||
                    argument.StartsWith("--trace=", rocks::StringComparison::CaseSensitive)) &&
                   settingCount < MaxSettingArguments) {
            settingArguments[settingCount++] = argument;
        } else {
            return Usage();
        }
    }

    // 창 모드는 --frames가 없으면 창을 닫을 때까지 돈다.
    if (options.window && !framesGiven) {
        options.frames = 0;
    }

    options.arguments = Span<const StringView>(settingArguments, settingCount);
    if (engineShaders || cookedShaders) {
        return sandbox::RunGraphicsSelfTest(context.DefaultAllocator(), context.Modules(), false,
                                            engineShaders
                                                ? sandbox::GraphicsSelfTestScope::EngineShaders
                                                : sandbox::GraphicsSelfTestScope::CookedShaders);
    }

    return engine ? sandbox::RunEngineHost(context, options)
                  : RunSelfTest(context.DefaultAllocator(), context.Modules(), gpuValidation);
}

// Android APK를 런처에서 띄우면(Intent extra 없음) 창 모드 엔진을
// 돌린다(Docs/Design/AndroidPlatform.ko.md 6절). 액티비티가 끝날 때(onDestroy) 종료 요청 다리로
// 정상 종료한다.
constexpr StringView AndroidDefaultArguments[] = {"--engine", "--window"};

const rocks::launch::ApplicationDescription& Describe() noexcept {
    static constexpr rocks::launch::ApplicationDescription Description{
        .name = "Sandbox",
        .kind = rocks::launch::ApplicationKind::EngineHost,
        .settings = {.toolLogging = rocks::launch::ToolLogging::Stderr},
        .configure = &Configure,
        .run = &Run,
        .androidDefaultArguments = Span<const StringView>(AndroidDefaultArguments)};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
