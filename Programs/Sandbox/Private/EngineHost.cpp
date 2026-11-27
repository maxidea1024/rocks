#include "EngineHost.h"

#include "CookedTextureLibrary.h"
#include "GraphicsDeviceTraits.h"
#include "SandboxGame.h"
#include "SandboxShaders.h"
#include "ScreenHost.h"

#include <Base/PlacementNew.h>
#include <Collision/CollisionSettings.h>
#include <Core/Containers/Array.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Move.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <AssetRuntime/CookedManifest.h>
#include <Engine/Engine.h>
#include <Engine/ModuleStartProgress.h>
#include <Engine/SplashSettings.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <EngineAudio/AudioEngineSubsystem.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstance.h>
#include <Gameplay/GameInstanceClient.h>
#include <Gameplay/LocalPlayer.generated.h>
#include <Gameplay/PlayerController.generated.h>
#include <Gameplay/PlayerController.h>
#include <HostConfig/HostSettings.h>
#include <Input/InputEvent.h>
#include <Input/InputSettings.h>
#include <Input/InputState.h>
#include <Launch/Application.h>
#include <Physics/PhysicsSettings.h>
#include <Platform/ApkFile.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <LogHost/LogHost.h>
#include <ModuleLoader/HostServices.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Progress/ProgressHub.h>
#include <Localization/Text.h>
#include <Core/Optional.h>
#include <Platform/Thread.h>
#include <PlatformWindow/Window.h>
#include <RenderBase/CookedManifestShaderLibraries.h>
#include <SplashScreen/SplashScreen.h>
#include <SplashScreen/StartupProgress.h>
#if SANDBOX_TRACE_ANALYSIS
#include <TraceAnalysis/TraceModel.h>
#endif
#include <TraceCapture/TraceLogSink.h>
#include <TraceCapture/TraceSession.h>
#include <TraceControl/TraceController.h>
#include <TraceControl/TraceSettings.generated.h>
#include <stdio.h>
#include <stdlib.h>

namespace sandbox {

namespace {

using namespace rocks;

// 엔진을 멈춘 뒤 trace sink를 빼기 전에 로그 출력 스레드를 기다리는 상한이다.
constexpr uint32 TraceLogFlushTimeoutMilliseconds = 2000;

// Sandbox 엔진 호스트의 로그 카테고리다.
inline constexpr LogCategory LogSandboxHost{"Sandbox"};

// 로컬 플레이어마다 PlayerController의 진동 합성 값을 그 패드에 쓴다(Docs/Design/InputActions
// 6.3절 출력). PlayerController가 없거나 끊겼으면 0이다. ControllerId가 음수인 플레이어는 쓰지
// 않는다(입력은 패드 0을 읽지만 패드 0의 진동 주인은 ControllerId 0이다 — 둘이 번갈아 쓰지 않게).
// 지난 프레임에 0이 아닌 값을 썼는데 이번에 아무도 쓰지 않은 패드(LocalPlayer 제거·인스턴스 정지)는
// 0으로 멈춘다. 연결되지 않은 패드(NotFound)·진동 없는 backend(Unsupported)는 조용히 넘긴다.
// 프레임마다 부른다(backend는 바뀐 값만 장치에 쓰고, Android는 이 호출에서 1초 one-shot을 다시
// 건다). 돌려주는 값은 이번에 0이 아닌 값을 쓴 패드 비트다.
uint32 WriteLocalPlayerMotors(GameInstanceClient& client, platform::GamepadBackend& gamepads,
                              uint32 previousActivePads) noexcept {
    uint32 writtenPads = 0;
    uint32 activePads = 0;
    auto written = client.WithInstance([&](GameInstance& instance) noexcept {
        World* world = instance.CurrentWorld();
        for (uint32 index = 0; index < instance.LocalPlayerCount(); ++index) {
            int32 controllerId = 0;
            ObjectHandle controller;
            (void)instance.WithLocalPlayer(instance.LocalPlayerAt(index),
                                           [&](LocalPlayer& player) noexcept {
                                               controllerId = player.ControllerId();
                                               controller = player.PlayerControllerHandle();
                                           });
            if (controllerId < 0 || static_cast<uint32>(controllerId) >= input::MaxGamepads) {
                continue;
            }

            const auto gamepad = static_cast<uint32>(controllerId);
            input::MotorValues motors;
            if (world != nullptr && !controller.IsNull() && world->HasActor(controller)) {
                (void)world->With<PlayerController>(
                    controller,
                    [&motors](PlayerController& value) noexcept { motors = value.MotorOutput(); });
            }

            (void)gamepads.SetMotors(gamepad, motors);
            writtenPads |= 1u << gamepad;
            if (!(motors == input::MotorValues{})) {
                activePads |= 1u << gamepad;
            }
        }
    });
    (void)written;
    for (uint32 gamepad = 0; gamepad < input::MaxGamepads; ++gamepad) {
        const uint32 bit = 1u << gamepad;
        if ((previousActivePads & bit) != 0 && (writtenPads & bit) == 0) {
            (void)gamepads.SetMotors(gamepad, input::MotorValues{});
        }
    }

    return activePads;
}

// 헤드리스 실행의 고정 프레임 시간이다(결정적 시간 진행).
constexpr double HeadlessFrameSeconds = 1.0 / 60.0;

// 자체 시험이 띄우는 자식의 프레임 수와 대기 상한이다(상한은 멈춘 자식을 끊는 장치일 뿐이다).
constexpr uint64 SelfTestFrames = 3;
constexpr uint32 SelfTestTimeoutMilliseconds = 60000;

// 자식 stderr를 모으는 버퍼 크기다.
constexpr usize SelfTestOutputBytes = 64 * 1024;

// 프레임 입력 큐 용량이다(한 프레임에 이보다 많은 이벤트는 버리고 DroppedCount로 센다).
constexpr usize InputQueueCapacity = 256;

// 창 모드의 프레임 간격 상한이다(약 60 Hz). 렌더가 없어 present 대기가 없으므로 호스트가 맞춘다.
constexpr uint64 NanosecondsPerSecond = 1000000000ull;
constexpr uint64 NanosecondsPerMillisecond = 1000000ull;
constexpr uint64 WindowFramesPerSecond = 60;
constexpr uint64 WindowFrameNanoseconds = NanosecondsPerSecond / WindowFramesPerSecond;

// 스플래시에서 주 창으로 넘기는 프레임이다(Docs/Design/SplashScreen 9.4절): 첫 프레임을 낸 뒤 주
// 창을 보이고(최소 표시 시간 ui.splash.min_seconds도 지나야 한다 — 9.7절), 보인 뒤 한 프레임을 더
// 낸 다음 스플래시를 숨긴다.
constexpr uint64 ShowWindowAfterFrames = 1;

// 창 크기다(client 영역 픽셀).
constexpr uint32 WindowWidth = 1280;
constexpr uint32 WindowHeight = 720;

// 호스트 엔진 루프 hook이다(Docs/Design/EngineRuntime 21.5절). 프레임 첫 단계(PumpEvents)에서
// 종료 요청 다리 → 창 펌프 → 게임패드 폴링 → 시험 주입 → InputState::BeginFrame 순으로 돈다.
//
// 종료 요청 다리(설계 20절): Ctrl-C·Ctrl-Break·SIGINT·SIGTERM 처리기는 launch::RequestTermination
// 원자 표시만 남긴다. 게임 스레드가 여기서 그 표시를 보고 Engine::RequestExit를 불러 정상 종료로
// 잇는다. 창 닫기 버튼도 같은 길로 끝낸다.
class HostLoopBridge final : public EngineLoopHooks {
public:
    struct Options {
        // 시험용: 0이 아니면 이 번째 PumpEvents에서 종료 요청 표시를 남긴다.
        uint64 requestTerminationAtFrame = 0;
        // 시험용: 0이 아니면 이 번째 PumpEvents에서 큐에 KeyDown(W)를 넣는다.
        uint64 injectKeyAtFrame = 0;
        // 창(선택)과 게임패드 backend(선택)다. hook보다 오래 산다.
        platform::Window* window = nullptr;
        platform::GamepadBackend* gamepads = nullptr;
        // 엔진 카메라를 읽을 게임 세션이다(렌더 view). hook보다 오래 산다.
        GameInstanceClient* client = nullptr;
        // 시작 화면(선택)이다. 주 창은 숨긴 채 만들어져 있고, 첫 프레임이 끝나면 보이고 한 프레임을
        // 더 그린 뒤 스플래시를 숨긴다(Docs/Design/SplashScreen 9.4절). hook보다 오래 산다.
        splash::SplashScreen* splash = nullptr;
        // 시작 진행(선택)이다. 스플래시를 숨기기 직전에 끝낸다(9.4절 4). hook보다 오래 산다.
        splash::StartupProgress* startup = nullptr;
    };

    HostLoopBridge(input::InputQueue& queue, const Options& options) noexcept
        : queue_(queue), options_(options) {
    }

    // 엔진 LocalPlayer가 읽는 입력 원천이다(GameInstance::SetLocalPlayerInput).
    const input::InputState& Input() const noexcept {
        return input_;
    }

    // 창 렌더 경로를 붙인다(Docs/Design/RenderBase 18.5절 1항). screen은 엔진 Shutdown 뒤까지 산다.
    void AttachScreen(screen::ScreenHost* screen) noexcept {
        screen_ = screen;
    }

    // 쿠킹 텍스처 목록을 붙인다(18.5절 7·8항). 프레임 끝에 텍스처 렌더 캐시를 갱신해 준비된
    // 텍스처를 화면에 올리고, 엔진 종료 시작에 handle을 Retire한 뒤 lease를 놓는다(EngineAssets
    // 종료 계약 — AssetSystem 11.8절 6).
    void AttachTextures(CookedTextureLibrary* textures) noexcept {
        textures_ = textures;
    }

    // 렌더 lane slot 대기 중 창 메시지 펌프다(5.3절 — 펌프한 이벤트는 다음 PumpEvents가 모은다).
    render::RenderLanePump LanePump() noexcept {
        return {&PumpWindow, this};
    }

    void PumpEvents(Engine& engine) noexcept override {
        ++pumped_;
        if (options_.requestTerminationAtFrame != 0 &&
            pumped_ == options_.requestTerminationAtFrame) {
            launch::RequestTermination();
        }

        if (launch::IsTerminationRequested()) {
            engine.RequestExit();
        }

        if (options_.window != nullptr) {
            options_.window->PumpEvents(queue_);
            // 창 포커스가 비활성 창 볼륨(audio.unfocused.volume)을 정한다(3.35.9절).
            if (auto* audio = engine.GetSubsystem<AudioEngineSubsystem>()) {
                audio->SetApplicationForeground(options_.window->HasFocus());
            }

            const bool rendering = screen_ != nullptr && screen_->IsRunning();
            // 잃은 surface는 렌더 lane이 스왑체인을 놓은 뒤(handshake) 놓았다고 알린다(Android 창
            // 파괴 gate — Docs/Design/AndroidPlatform.ko.md 4절).
            if (options_.window->ConsumeSurfaceLost()) {
                if (rendering) {
                    screen_->ReleaseSurface();
                }

                options_.window->AcknowledgeSurfaceLost();
            }

            // 크기 변경은 렌더 lane의 표면 작업이다(새 surface면 스왑체인을 다시 만든다). 렌더가
            // 없으면 빈 프레임을 낸다(Android는 첫 프레임 전까지 입력을 보내지 않는다).
            uint32 width = 0;
            uint32 height = 0;
            if (options_.window->ConsumeResize(width, height)) {
                if (rendering) {
                    screen_->RecreateSurface(*options_.window);
                    screen_->Resize(width, height);
                } else {
                    options_.window->PresentBlankFrame();
                }
            }

            if (options_.window->IsCloseRequested()) {
                options_.window->ClearCloseRequest();
                Log::Info(LogSandboxHost, "Window close requested");
                engine.RequestExit();
            }
        }

        if (options_.gamepads != nullptr) {
            options_.gamepads->Poll(queue_);
        }

        // input.use_mouse_for_touch(설정 문맥이 묶인 게임 스레드 —
        // Docs/Design/TouchInput.ko.md 3.4절).
        input_.SetMouseTouchEmulation(input::UseMouseForTouchSetting());

        if (options_.injectKeyAtFrame != 0 && pumped_ == options_.injectKeyAtFrame) {
            (void)queue_.Push(input::InputEvent::KeyDown(input::Key::W));
        }

        // 이번 프레임 이벤트를 모으는 동안 큐가 버렸으면 손실 복구를
        // 한다(Docs/Design/TouchInput.ko.md 3.3절).
        const uint64 dropped = queue_.DroppedCount();
        input_.BeginFrame(queue_.Events(), dropped != droppedSeen_);
        droppedSeen_ = dropped;
        queue_.Clear();
    }

    // 창 모드면 프레임 간격을 WindowFrameNanoseconds로 맞춘다. OS sleep 해상도(Windows 기본 약
    // 15.6 ms)가 거칠어 프레임마다 남은 시간을 자면 간격이 해상도 배수로 늘어나므로, 누적
    // deadline을 두고 deadline 전이면 남은 만큼 잔다(늦게 깬 만큼 다음 프레임이 덜 자서
    // 평균이 맞는다). 한 간격 넘게 밀리면 deadline을 지금으로 다시 잡는다(밀린 프레임을 몰아 돌리지
    // 않는다). 헤드리스는 기다리지 않는다.
    void EndFrame(Engine&) noexcept override {
        if (options_.gamepads != nullptr && options_.client != nullptr) {
            activeMotorPads_ =
                WriteLocalPlayerMotors(*options_.client, *options_.gamepads, activeMotorPads_);
        }

        if (screen_ != nullptr) {
            if (textures_ != nullptr) {
                textures_->Update();
            }

            screen_->HandleInspectorInput(input_);
            screen_->SubmitFrame(options_.client);
        }

        HandOffFromSplash();
        if (options_.window == nullptr) {
            return;
        }

        const uint64 now = platform::MonotonicNanoseconds();
        if (deadline_ == 0 || now > deadline_ + WindowFrameNanoseconds) {
            deadline_ = now;
        }

        deadline_ += WindowFrameNanoseconds;
        if (now < deadline_) {
            platform::SleepFor(static_cast<uint32>((deadline_ - now) / NanosecondsPerMillisecond));
        }
    }

    // 스플래시에서 주 창으로 넘긴다(Docs/Design/SplashScreen 9.4절): 첫 프레임을 낸 뒤 숨겨 둔 주
    // 창을 보이고 앞으로 가져오고, 보인 뒤 한 프레임을 더 낸 다음 스플래시를 숨긴다(숨긴 창에 낸
    // 프레임이 보이는 순간 화면에 남는다는 보장이 없어서다). 최소 표시 시간(9.7절)이 지날 때까지는
    // 주 창을 숨긴 채 엔진 프레임을 계속 돈다.
    void HandOffFromSplash() noexcept {
        if (options_.splash == nullptr) {
            return;
        }

        ++handOffFrames_;
        if (windowShown_) {
            if (options_.startup != nullptr) {
                options_.startup->Finish();
                options_.startup = nullptr;
            }

            options_.splash->Hide();
            options_.splash = nullptr;
        } else if (handOffFrames_ >= ShowWindowAfterFrames && options_.splash->CanHide()) {
            if (options_.window != nullptr) {
                options_.window->ShowAndActivate();
            }

            windowShown_ = true;
        }
    }

    // 엔진 종료 hook을 렌더 lane 종료 단계에 잇는다(Docs/Design/RenderBase 5.5절).
    void BeginShutdown(Engine&) noexcept override {
        if (textures_ != nullptr) {
            textures_->Release();
        }

        if (screen_ != nullptr) {
            screen_->BlockFrames();
        }
    }

    Result<void> CompleteInFlightWork(Engine&) noexcept override {
        return screen_ != nullptr ? screen_->WaitFramesIdle() : Result<void>::Success();
    }

    Result<uint64> DrainShutdownCleanup(Engine&) noexcept override {
        return screen_ != nullptr ? screen_->DrainRetires() : Result<uint64>::Success(0);
    }

    Result<void> VerifyShutdownClosure(Engine&) noexcept override {
        return screen_ != nullptr ? screen_->CloseResources() : Result<void>::Success();
    }

    Result<void> StopExecutors(Engine&) noexcept override {
        return screen_ != nullptr ? screen_->Stop() : Result<void>::Success();
    }

private:
    static void PumpWindow(void* raw) noexcept {
        auto* self = static_cast<HostLoopBridge*>(raw);
        if (self->options_.window != nullptr) {
            self->options_.window->PumpEvents(self->queue_);
        }
    }

    input::InputQueue& queue_;
    Options options_;
    // 스플래시 넘기기에서 끝난 프레임 수와 주 창을 보였는지다(보인 다음 프레임에 스플래시를
    // 숨긴다).
    uint64 handOffFrames_ = 0;
    bool windowShown_ = false;
    screen::ScreenHost* screen_ = nullptr;
    CookedTextureLibrary* textures_ = nullptr;
    input::InputState input_;
    uint64 pumped_ = 0;
    // 지난 프레임까지 본 큐 손실 수다.
    uint64 droppedSeen_ = 0;
    // 지난 프레임에 0이 아닌 진동을 쓴 패드 비트다(빠진 플레이어의 패드를 멈춘다).
    uint32 activeMotorPads_ = 0;
    // 창 모드 다음 프레임 끝 목표 시각(MonotonicNanoseconds)이다. 0이면 아직 없다.
    uint64 deadline_ = 0;
};

// 창 모드의 창을 만든다. 스플래시가 떠 있으면 숨긴 채 만든다(첫 프레임 뒤 보인다 — HostLoopBridge).
// 실패는 Error를 남기고 그대로 돌려준다(호스트는 실패 코드로 끝난다).
Result<UniquePtr<platform::Window>> CreateSandboxWindow(Allocator& allocator,
                                                        bool visible) noexcept {
    platform::WindowDesc desc;
    desc.title = "Rocks Sandbox";
    desc.width = WindowWidth;
    desc.height = WindowHeight;
    desc.visible = visible;
    platform::WindowCreateReport report;
    auto window = platform::Window::Create(allocator, desc, &report);
    if (!window) {
        Log::Error(platform::LogPlatformWindow,
                   "Window creation failed at {Call}: {Error} (OS error {OsError})",
                   StringView(report.call), ErrorCodeName(window.Error()), report.osError);
    }

    return window;
}

// 게임패드 backend를 만든다. 없으면 Info 한 줄만 남기고 null을 돌려준다(키보드·마우스로 돈다).
UniquePtr<platform::GamepadBackend> CreateSandboxGamepads(Allocator& allocator) noexcept {
    auto gamepads = platform::GamepadBackend::Create(allocator);
    if (!gamepads) {
        Log::Info(LogSandboxHost, "Gamepad input is not available: {Error}",
                  ErrorCodeName(gamepads.Error()));
        return UniquePtr<platform::GamepadBackend>();
    }

    return Move(gamepads.Value());
}

// 호스트 서비스를 등록한다(Docs/Design/ModuleLoader 3.12절). 서비스는 선택이라 실패하면
// Warning을 남기고 모듈은 서비스 없이 시작한다.
template <class Service>
void RegisterHostService(HostServices& services, Service& service) noexcept {
    if (auto registered = services.Register(service); !registered) {
        Log::Warning(LogSandboxHost, "Host service {Service} could not be registered: {Error}",
                     StringView(HostServiceTraits<Service>::Key().name),
                     ErrorCodeName(registered.Error()));
    }
}

// APK 안 파일 확인이다(Android APK만 — Docs/Design/AndroidPlatform.ko.md 7절). 패키저가 넣은
// assets/SandboxProbe.txt를 platform::ApkFileSystem으로 읽어 로그로 남긴다. APK가 없는 플랫폼은
// Unsupported라 아무것도 하지 않는다.
void LogApkProbe() noexcept {
    constexpr char ProbePath[] = "SandboxProbe.txt";
    constexpr usize ProbeBytes = sizeof(ProbePath) - 1;
    platform::FileSystem& apk = platform::ApkFileSystem();
    platform::FileHandle* handle = nullptr;
    const platform::FileStatus opened =
        apk.Open(ProbePath, ProbeBytes, platform::FileOpenMode::ReadExisting, handle);
    if (opened.code == platform::FileCode::Unsupported) {
        return;
    }

    if (!opened) {
        Log::Warning(LogSandboxHost, "APK asset {Path} could not be opened", StringView(ProbePath));
        return;
    }

    char text[64] = {};
    const platform::FileTransfer read = handle->Read(text, sizeof(text) - 1);
    uint64 size = 0;
    (void)handle->Size(size);
    (void)handle->Close();
    handle->Destroy();
    usize length = read.bytes;
    while (length != 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) {
        --length;
    }

    Log::Info(LogSandboxHost, "APK asset {Path} ({Size} bytes): {Text}", StringView(ProbePath),
              size, StringView(text, length));
}

// 쿠킹 결과의 매니페스트가 있는가다(ShaderSystem 16.1절 SSD48, RenderBase 18.5절 7항). 쿠킹 루트는
// EngineAssets와 같은 규칙이다: asset.cooked_root(설정·`--set=` — 시험 콘텐츠 쿠킹 등)가 있으면
// 그것, 없으면 엔진 전용 기본값(`asset-cook --engine --profile=<실행 프로파일>`)이다.
bool HasEngineCook(Allocator& allocator, StringView engineDirectory,
                   const config::ConfigContext& settings) noexcept {
    char executable[4096];
    usize executableBytes = 0;
    if (engineDirectory.IsEmpty() ||
        !platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return false;
    }

    StringView configured;
    if (auto setting = settings.FindConsoleSetting("asset.cooked_root")) {
        configured = settings.ConsoleValue(setting.Value()).string.View();
    }

    String root(allocator);
    if (!ResolveCookedRoot(configured, engineDirectory, ResolveRuntimeProfile(StringView()),
                           ExecutableDirectoryOf(StringView(executable, executableBytes)), root) ||
        !root.TryAppend("/") || !root.TryAppend(CookedManifestFileName)) {
        return false;
    }

    bool exists = false;
    return platform::PhysicalFileSystem().FileExists(root.CStr(), root.View().ByteCount(),
                                                     exists) &&
           exists;
}

// 엔진 셰이더 호스트에 붙인 쿠킹 라이브러리 원천이다(SSD48). 파괴하면 떼고 놓는다 — 렌더 장치·
// lane(ScreenHost)보다 먼저 선언해 그 뒤에 파괴한다.
class CookedShaderAttachment {
public:
    explicit CookedShaderAttachment(render::EngineShaderHost& host) noexcept : host_(host) {
    }

    ~CookedShaderAttachment() noexcept {
        if (source_) {
            host_.DetachCookedLibraries(*source_);
        }
    }

    CookedShaderAttachment(const CookedShaderAttachment&) = delete;
    CookedShaderAttachment& operator=(const CookedShaderAttachment&) = delete;

    // 엔진 에셋 Subsystem의 매니페스트(backend·셰이더 라이브러리 행 사본)로 원천을 만들어 붙인다.
    void Attach(Allocator& allocator, const EngineAssetsSubsystem& assets) noexcept {
        auto created = render::CookedManifestShaderLibraries::TryCreate(
            allocator, platform::PhysicalFileSystem(), assets.Manifest(), assets.CookedRoot());
        if (!created || !host_.AttachCookedLibraries(*created.Value())) {
            Log::Warning(LogSandboxHost,
                         "The cooked engine shader libraries could not be attached; the shader "
                         "provider is used");
            return;
        }

        source_ = Move(created.Value());
        Log::Info(LogSandboxHost, "Engine shaders come from the cooked engine libraries at {Root}",
                  assets.CookedRoot());
    }

    const render::CookedShaderLibrarySource* Source() const noexcept {
        return source_.Get();
    }

private:
    render::EngineShaderHost& host_;
    UniquePtr<render::CookedManifestShaderLibraries> source_;
};

// 출력 backend 등록부와 디코더 등록부를 엔진 수명 동안 호스트 서비스에 올린다
// (Docs/Design/AudioSystem 3.35.11·3.37.1절). AudioMiniaudio·AudioOpus 모듈 Startup이 엔진 묶음
// 시작(엔진 Initialize) 때 여기에 factory를 넣으므로 그보다 먼저 올려야 한다(늦으면 조용히 빠진다).
// RunEngine의 중간 반환에서도 떼도록 소멸자가 Unregister한다(엔진보다 먼저 선언해 엔진 뒤에
// 소멸한다).
class AudioHostRegistries {
public:
    explicit AudioHostRegistries(HostServices& services) noexcept : services_(services) {
        backendsRegistered_ = services_.Register(backends_).IsOk();
        decodersRegistered_ = services_.Register(decoders_).IsOk();
    }

    ~AudioHostRegistries() noexcept {
        if (decodersRegistered_) {
            (void)services_.Unregister(decoders_);
        }

        if (backendsRegistered_) {
            (void)services_.Unregister(backends_);
        }
    }

    AudioHostRegistries(const AudioHostRegistries&) = delete;
    AudioHostRegistries& operator=(const AudioHostRegistries&) = delete;

private:
    HostServices& services_;
    audio::AudioOutputBackendRegistry backends_;
    audio::AudioDecoderRegistry decoders_;
    bool backendsRegistered_ = false;
    bool decodersRegistered_ = false;
};

// 엔진을 GameInstance(기본 맵 "Main" — PlayerStart, 첫 LocalPlayer의 SandboxPawn)로 돌린다
// (Docs/Design/EngineRuntime 21.5절). 헤드리스는 고정 delta로 options.frames만큼, 창 모드는 실제
// 시간으로 창이 닫히거나 frames(0이면 상한 없음)까지 돈다. 프레임 입력 상태를 첫 LocalPlayer의 입력
// 원천으로 건다. 엔진 실패 원인은 엔진이 한 번 기록한다(호스트는 다시 기록하지 않는다).
int RunEngine(Allocator& allocator, const EngineHostOptions& options,
              config::ConfigContext& settings, ModuleLoader& modules, StringView engineDirectory,
              splash::SplashScreen* splash, splash::StartupProgress* startup) noexcept {
    LogApkProbe();
    // 엔진 전용 실행의 쿠킹 루트(SSD48 — EngineAssets 10.3.1절 A4f4 5번)다. 엔진 쿠킹 매니페스트가
    // 있을 때만 EngineAssets Subsystem을 넣는다(없으면 그 초기화가 NotFound로 엔진 시작을 막는다 —
    // 개발 트리는 쿠킹 없이 공급자로 돈다).
    EngineAssetsSubsystem::SetEngineOnlyDirectory(engineDirectory);
    const bool engineCook = HasEngineCook(allocator, engineDirectory, settings);
    // 오디오는 늘 넣고(장치가 없으면 Null — 3.35.9절), EngineAssets는 엔진 쿠킹이 있을 때만 넣는다.
    const EngineSubsystemType withAssets[] = {EngineAssetsSubsystemType(),
                                              AudioEngineSubsystemType()};
    const EngineSubsystemType audioOnly[] = {AudioEngineSubsystemType()};
    AudioHostRegistries audioRegistries(modules.Services());
    CookedShaderAttachment cooked(shaders::Host());
    UniquePtr<platform::Window> window;
    UniquePtr<platform::GamepadBackend> gamepads;
    // 창 렌더 경로(Docs/Design/RenderBase 18.5절 1항)다. 엔진보다 먼저 선언해 엔진 Shutdown 뒤에
    // 파괴하고(종료 hook이 쓴다), 창보다 뒤에 선언해 창보다 먼저 파괴한다(스왑체인 → 창).
    UniquePtr<graphics::Device> device;
    UniquePtr<screen::ScreenHost> screenHost;
    // 쿠킹 텍스처 목록이다(18.5절 7항). 준비 callback이 가리키므로 엔진(AssetManager Shutdown)보다
    // 먼저 선언해 뒤에 파괴한다.
    UniquePtr<CookedTextureLibrary> textures;
    if (options.window) {
        auto created = CreateSandboxWindow(allocator, splash == nullptr);
        if (!created) {
            return 1;
        }

        window = Move(created.Value());
        gamepads = CreateSandboxGamepads(allocator);
    }

    input::InputQueue queue(allocator);
    if (auto initialized = queue.TryInitialize(InputQueueCapacity); !initialized) {
        Log::Error(LogSandboxHost, "The input queue could not be allocated: {Error}",
                   ErrorCodeName(initialized.Error()));
        return 1;
    }

    SandboxGame game;
    GameInstanceClient client(allocator, game.config);
    HostLoopBridge::Options hookOptions;
    hookOptions.requestTerminationAtFrame = options.requestTerminationAtFrame;
    hookOptions.injectKeyAtFrame = options.injectKeyAtFrame;
    hookOptions.window = window.Get();
    hookOptions.gamepads = gamepads.Get();
    hookOptions.client = &client;
    hookOptions.splash = window ? splash : nullptr;
    hookOptions.startup = window && splash != nullptr ? startup : nullptr;
    HostLoopBridge bridge(queue, hookOptions);
    EngineConfig config;
    config.hooks = &bridge;
    config.client = &client;
    config.modules = &modules;
    config.maxFrames = options.frames;
    config.settings = &settings;
    config.fixedFrameDeltaSeconds = options.window ? 0.0 : HeadlessFrameSeconds;
    config.registerTypes = &RegisterSandboxTypes;
    config.subsystems = engineCook ? Span<const EngineSubsystemType>(withAssets)
                                   : Span<const EngineSubsystemType>(audioOnly);

    Engine engine(allocator);
    if (startup != nullptr) {
        startup->Enter(splash::StartupStep::Engine);
    }

    if (!engine.Initialize(config)) {
        return 1;
    }

    if (startup != nullptr) {
        startup->Enter(splash::StartupStep::FirstFrame);
    }

    if (const EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>()) {
        cooked.Attach(allocator, *assets);
    }

    // 엔진 묶음·PostEngineInit 모듈이 셰이더 타입을 등록했다. 렌더 lane 시작 전 자리에서 등록부를
    // 닫는다(Docs/Design/RenderBase 9.12절 1항 — 장치가 아직 없어 lane은 화면 마일스톤에서 잇는다).
    if (!shaders::Host().Freeze()) {
        (void)engine.Shutdown();
        return 1;
    }

    // 창 모드면 장치와 렌더 lane을 시작한다. 엔진 셰이더를 열 원천이 없거나(Release·Android)
    // 장치·lane을 만들지 못하면 렌더 없이 계속한다(빈 프레임 경로, 18.5절 1항).
    if (window && options.render) {
        // 쿠킹 원천이 있으면 그 backend 순서로 장치를 고르고(auto), 셰이더 없는 쿠킹이면 장치를
        // 만들지 않는다(SSD48 — UE 전용 서버의 NullDrv 자리).
        const render::CookedShaderLibrarySource* source = cooked.Source();
        const bool shaderless = source != nullptr && source->Backends().IsEmpty();
        auto create = [&]() noexcept {
            return source != nullptr && options.graphics == screen::ScreenBackend::Auto
                       ? screen::CreateScreenDeviceInOrder(allocator, source->Backends(),
                                                           options.graphicsValidation)
                       : screen::CreateScreenDevice(allocator, options.graphics,
                                                    options.graphicsValidation);
        };
        if (shaderless) {
            Log::Warning(LogSandboxHost, "Screen rendering is not available (Unsupported): this "
                                         "cook has no shader formats");
        } else if (!screen::CanOpenEngineShaders(shaders::Host())) {
            Log::Info(LogSandboxHost, "Screen rendering is not available: no shader provider or "
                                      "cooked shader libraries");
        } else if (auto created = create()) {
            device = Move(created.Value());
            auto host = UniquePtr<screen::ScreenHost>::TryMake(allocator, allocator, *device,
                                                               shaders::Host());
            if (host &&
                host.Value()->Start(*window, render::RenderLaneMode::Thread, bridge.LanePump())) {
                screenHost = Move(host.Value());
                bridge.AttachScreen(screenHost.Get());
            }

            // 쿠킹 텍스처를 요청한다(패키지 공개 뒤 프레임 끝에 올린다 — 6.5절 "업로드").
            if (EngineAssetsSubsystem* assets = engine.GetSubsystem<EngineAssetsSubsystem>();
                assets != nullptr && screenHost) {
                auto library = UniquePtr<CookedTextureLibrary>::TryMake(
                    allocator, allocator, assets->Manager(), assets->Manifest(), engine.Types(),
                    *screenHost);
                if (library && library.Value()->RequestAll()) {
                    textures = Move(library.Value());
                    bridge.AttachTextures(textures.Get());
                }
            }
        }
    }

    // 첫 LocalPlayer(DedicatedServer면 없다)의 입력 원천이다. bridge는 엔진 Shutdown 뒤까지 산다.
    auto attached = client.WithInstance([&](GameInstance& instance) noexcept -> Result<void> {
        if (instance.LocalPlayerCount() == 0) {
            return Result<void>::Success();
        }

        return instance.SetLocalPlayerInput(instance.LocalPlayerAt(0), &bridge.Input());
    });
    if (!attached) {
        Log::Error(LogSandboxHost, "Local player input could not be attached: {Error}",
                   ErrorCodeName(attached.Error()));
        (void)engine.Shutdown();
        return 1;
    }

    int exitCode = 0;
    if (!engine.Run()) {
        exitCode = 1;
    }

    if (!engine.Shutdown()) {
        exitCode = 1;
    }

    return exitCode;
}

// 자식 실행 결과다.
struct ChildRun {
    bool exited = false;
    uint32 code = 0;
    StringView stderrText;
};

ChildRun RunChild(Span<const platform::ProcessArgument> arguments, char* errorText,
                  usize errorCapacity) noexcept {
    ChildRun child;
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return child;
    }

    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments.Data();
    info.argumentCount = arguments.Size();
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    platform::ProcessRunOptions run;
    run.error = errorText;
    run.errorCapacity = errorCapacity;
    run.timeoutMilliseconds = SelfTestTimeoutMilliseconds;
    const platform::ProcessRunResult result = platform::RunProcess(info, run);
    child.exited =
        result.status && !result.timedOut && result.exit.kind == platform::ProcessExitKind::Exited;
    child.code = result.exit.code;
    child.stderrText = StringView(errorText, result.errorBytes);
    return child;
}

struct FirstFile {
    char name[256]{};
    bool found = false;
};

bool FindFirstFile(const platform::DirectoryEntry& entry, void* raw) noexcept {
    auto& first = *static_cast<FirstFile*>(raw);
    if (entry.info.type != platform::FileType::File || entry.nameBytes >= sizeof(first.name)) {
        return true;
    }

    platform::MemoryCopy(first.name, entry.name, entry.nameBytes);
    first.found = true;
    return false;
}

#if SANDBOX_TRACE_ANALYSIS
// trace 파일에 로그 기록(Trace.LogMessage)이 남는지 확인한다(`--engine --trace=default
// --trace-file <path>`, 로깅 LG15).
bool CheckTraceLogRecords() noexcept {
    char directory[128];
    FixedStringBuilder directoryText{Span<char>(directory)};
    (void)directoryText.TryAppendFormat("rocks-sandbox-trace-{}", platform::CurrentProcessId());
    platform::FileSystem& files = platform::PhysicalFileSystem();
    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    if (!platform::MakeDirectoryTree(files, directory, directoryText.ByteCount())) {
        return false;
    }

    char tracePath[192];
    FixedStringBuilder traceText{Span<char>(tracePath)};
    (void)traceText.TryAppendFormat("{}/run.rtrace", directoryText.View());
    const platform::ProcessArgument arguments[] = {
        {"--engine", 8},         {"--frames", 8},      {"2", 1},
        {"--trace=default", 15}, {"--trace-file", 12}, {tracePath, traceText.ByteCount()}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    bool passed = child.exited && child.code == 0;
    {
        platform::File trace;
        uint64 size = 0;
        passed = passed && trace.Open(tracePath, platform::FileOpenMode::ReadExisting) &&
                 trace.Size(size) && bytes.TryResize(static_cast<usize>(size)) &&
                 trace.ReadExact(bytes.Data(), bytes.Size()).status;
    }

    usize logs = 0;
    bool finished = false;
    if (passed) {
        TraceModel model(allocator);
        passed = static_cast<bool>(model.Build(bytes.View()));
        if (passed) {
            logs = model.Logs().Size();
            for (const TraceModelLog& log : model.Logs()) {
                finished = finished || log.message.Contains("Engine run finished",
                                                            StringComparison::CaseSensitive);
            }
        }
    }

    passed = passed && logs != 0 && finished;
    if (!passed) {
        printf("    exit %u, trace bytes %llu, log records %llu, stderr:\n%.*s\n", child.code,
               static_cast<unsigned long long>(bytes.Size()), static_cast<unsigned long long>(logs),
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    return passed;
}

#endif

// 설정 파일로 Sync 모드와 파일 기록을 켠 자식이 로그 파일에 종료 줄을 남기는지 본다.
bool CheckConfiguredFileLogging() noexcept {
    char directory[128];
    FixedStringBuilder directoryText{Span<char>(directory)};
    (void)directoryText.TryAppendFormat("rocks-sandbox-selftest-{}", platform::CurrentProcessId());
    platform::FileSystem& files = platform::PhysicalFileSystem();
    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    if (!platform::MakeDirectoryTree(files, directory, directoryText.ByteCount())) {
        return false;
    }

    char configPath[192];
    FixedStringBuilder configText{Span<char>(configPath)};
    (void)configText.TryAppendFormat("{}/Engine.json", directoryText.View());
    char json[512];
    FixedStringBuilder jsonText{Span<char>(json)};
    (void)jsonText.TryAppendFormat(
        "{{\"Settings\": {{\"log.mode\": \"sync\", \"log.file.enabled\": true, "
        "\"log.file.directory\": \"{}/Logs\"}}}}\n",
        directoryText.View());
    bool passed = false;
    {
        platform::File config;
        passed = config.Open(configPath, platform::FileOpenMode::CreateAlways) &&
                 config.WriteAll(json, jsonText.ByteCount()).status && config.Close();
    }

    if (passed) {
        const platform::ProcessArgument arguments[] = {{"--engine", 8},
                                                       {"--frames", 8},
                                                       {"2", 1},
                                                       {"--config", 8},
                                                       {configPath, configText.ByteCount()}};
        static char errorText[SelfTestOutputBytes];
        const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
        char logs[192];
        FixedStringBuilder logsText{Span<char>(logs)};
        (void)logsText.TryAppendFormat("{}/Logs", directoryText.View());
        FirstFile first;
        (void)files.EnumerateDirectory(logs, logsText.ByteCount(), &FindFirstFile, &first);
        char logPath[512];
        FixedStringBuilder logPathText{Span<char>(logPath)};
        (void)logPathText.TryAppendFormat("{}/{}", logsText.View(), StringView(first.name));
        static char content[SelfTestOutputBytes];
        usize contentBytes = 0;
        platform::File log;
        if (first.found && log.Open(logPath, platform::FileOpenMode::ReadExisting)) {
            const platform::FileTransfer read = log.Read(content, sizeof(content));
            contentBytes = read.bytes;
        }

        passed = child.exited && child.code == 0 &&
                 StringView(content, contentBytes)
                     .Contains("[Info][Engine] Engine shut down", StringComparison::CaseSensitive);
        if (!passed) {
            printf("    exit %u, log file '%s' (%llu bytes), stderr:\n%.*s\n", child.code,
                   first.found ? first.name : "<none>",
                   static_cast<unsigned long long>(contentBytes),
                   static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
        }
    }

    (void)platform::DeleteDirectoryTree(files, directory, directoryText.ByteCount());
    return passed;
}

// 종료 요청 다리: 자식을 프레임 상한 없이(아주 큰 값) 띄우고 2번째 프레임에서 종료 요청 표시를
// 남기게 한다. 다리가 RequestExit로 이어 "RequestExit" 사유로 정상 종료(0)해야 한다. 순서는 프레임
// 번호로 정해진다(시간·경쟁과 무관).
bool CheckTerminationRequestBridge() noexcept {
    const platform::ProcessArgument arguments[] = {{"--engine", 8},
                                                   {"--frames", 8},
                                                   {"1000000", 7},
                                                   {"--request-termination-at-frame", 30},
                                                   {"2", 1}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    const bool passed =
        child.exited && child.code == 0 &&
        child.stderrText.Contains("frames: RequestExit", StringComparison::CaseSensitive) &&
        child.stderrText.Contains("[Info][Engine] Engine shut down",
                                  StringComparison::CaseSensitive);
    if (!passed) {
        printf("    exit %u, stderr:\n%.*s\n", child.code,
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    return passed;
}

// 입력 경로: 자식을 4 프레임 돌리며 2번째 PumpEvents에서 큐에 KeyDown(W)를 넣게 한다. 큐 →
// InputState → LocalPlayer 입력 원천 → PlayerController 스냅숏 → SandboxPawn 로그로 이어져야 한다
// (창 펌프가 넣는 이벤트와 같은 자리, 순서는 프레임 번호로 정해진다).
bool CheckInjectedInputReachesPawn() noexcept {
    const platform::ProcessArgument arguments[] = {
        {"--engine", 8}, {"--frames", 8}, {"4", 1}, {"--inject-key-at-frame", 21}, {"2", 1}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    const bool passed = child.exited && child.code == 0 &&
                        child.stderrText.Contains("[Info][Sandbox.Game] Pawn received key W",
                                                  StringComparison::CaseSensitive);
    if (!passed) {
        printf("    exit %u, stderr:\n%.*s\n", child.code,
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    return passed;
}

// 장치 능력을 기기 특성으로 옮기면 품질 등급 요구(기기 조건)가 그 값으로 판정된다(설정 CF4 —
// Config는 Graphics를 모르고 호스트가 특성을 채운다).
bool CheckDeviceTraitsFromCaps(Allocator& allocator) noexcept {
    graphics::DeviceCaps caps;
    caps.limits.textureSize2D = 16384;
    caps.limits.colorAttachments = 8;
    caps.features.computeShaders = true;
    config::DeviceTraits traits(allocator);
    if (!AppendGraphicsDeviceTraits(caps, traits)) {
        return false;
    }

    config::DeviceCondition large(allocator);
    config::DeviceCondition huge(allocator);
    String reason(allocator);
    if (!large.trait.TryAssign(config::DeviceTraitNames::GpuTextureSize2D) ||
        !huge.trait.TryAssign(config::DeviceTraitNames::GpuTextureSize2D)) {
        return false;
    }

    large.atLeast = 8192.0;
    huge.atLeast = 32768.0;
    auto fits = config::MatchDeviceCondition(traits, large, reason);
    auto exceeds = config::MatchDeviceCondition(traits, huge, reason);
    config::DeviceTrait compute;
    return fits && fits.Value() && exceeds && !exceeds.Value() &&
           traits.Find(config::DeviceTraitNames::GpuComputeShaders, compute) &&
           compute.number == 1.0;
}

// 설정 struct를 RegisterSettings로 올리는 엔진 모듈(Collision·Physics)의 리플렉션 타입을 등록한다.
Result<void> RegisterSandboxSettingsTypes(ModuleRegistration& registration) noexcept {
    if (auto collision = RegisterCollisionTypes(registration); !collision) {
        return collision;
    }

    return RegisterPhysicsTypes(registration);
}

// Sandbox가 들어가는 시작 단계다(Docs/Design/SplashScreen 8.1절 — 엔진 밖 에셋·셰이더 준비와
// 콘텐츠 단계가 없다).
constexpr splash::StartupStep SandboxStartupSteps[] = {splash::StartupStep::HostServices,
                                                       splash::StartupStep::Engine,
                                                       splash::StartupStep::FirstFrame};

// 띄운 시작 화면과 그 진행이다. 진행은 스플래시가 뜰 때만 있다.
struct SandboxSplash {
    Optional<splash::SplashScreen> screen;
    Optional<splash::StartupProgress> startup;
};

// 창 모드의 시작 화면을 띄운다(Docs/Design/SplashScreen 4·7·10절). 띄우지 않기로 했거나 띄우지
// 못했으면 빈 값이다(못 띄웠으면 Warning, 안 띄우는 사유는 Debug 로그). 설정은 부팅 값
// (ui.splash.*)이고, 제목이 비면 "Rocks Sandbox"다. 시작 진행(시작 작업·단계 구간)을 열어 그 상태
// 원천을 스플래시에 준다 — ProgressHub가 켜져 있지 않으면 진행 없이(막대 불확정) 띄운다.
SandboxSplash ShowSandboxSplash(Allocator& allocator, const EngineHostOptions& options,
                                StringView engineDirectory) noexcept {
    SandboxSplash result;
    const SplashSettingValues values = ReadSplashSettings();
    splash::SplashVisibilityInput input = splash::MakeSplashVisibilityInput();
    input.interactiveHost = true;
    input.opensMainWindow = options.window;
    input.noSplashOption = options.noSplash;
    input.settingsEnabled = values.enabled;
    input.desktop = platform::QueryDesktopSessionState();
    const splash::SplashSkipReason reason = splash::DecideSplashVisibility(input);
    if (reason != splash::SplashSkipReason::None) {
        Log::Debug(LogSandboxHost, "The splash screen is not shown ({Reason})",
                   splash::SplashSkipReasonName(reason));
        return result;
    }

    const StringView title =
        values.gameTitle.IsEmpty() ? StringView("Rocks Sandbox") : values.gameTitle;

    const splash::SplashTextValue texts[] = {
        {splash::SplashTextSlot::Title, title},
        {splash::SplashTextSlot::Copyright, values.gameCopyright},
    };
    splash::SplashShowDesc desc;
    desc.kind = splash::SplashKind::Game;
    desc.configuredImage = values.gameImage;
    desc.engineDirectory = engineDirectory;
    desc.texts = texts;
    desc.showTitle = values.gameShowTitle;
    desc.showProgressText = values.gameShowProgressText;
    desc.showProgressBar = values.gameShowProgressBar;
    desc.minimumDisplaySeconds = values.minimumSeconds;
    if (auto appTitle = Text::Invariant(allocator, title)) {
        splash::StartupProgressDesc startupDesc;
        startupDesc.appTitle = &appTitle.Value();
        startupDesc.steps = Span<const splash::StartupStep>(SandboxStartupSteps);
        if (auto begun = splash::StartupProgress::Begin(allocator, startupDesc)) {
            if (auto source = begun.Value().MakeSplashSource(allocator, nullptr)) {
                desc.source = source.Value();
            }

            result.startup = Move(begun.Value());
        } else {
            Log::Debug(LogSandboxHost, "Startup progress is not reported: {Error}",
                       ErrorCodeName(begun.Error()));
        }
    }

    auto shown = splash::SplashScreen::Show(allocator, desc);
    if (!shown) {
        Log::Warning(LogSandboxHost, "The splash screen could not be shown: {Error}",
                     ErrorCodeName(shown.Error()));
        // 진행을 보일 곳이 없으니 시작 작업을 닫는다(끝까지 간 시작이 아니다).
        if (result.startup) {
            result.startup.Value().Finish(ProgressOutcome::Abandoned);
            result.startup = NullOpt;
        }

        return result;
    }

    result.screen = Move(shown.Value());
    if (result.startup) {
        result.startup.Value().Enter(splash::StartupStep::HostServices);
    }

    return result;
}

} // namespace

// 엔진 호스트 본문이다(Docs/Design/ModuleLoader 3.11절). Launch가 소유한 프로세스 모듈
// 로더(부트 모듈이 시작해 있다)로 설정(HostSettings — 모든 모듈의 RegisterSettings) → LogHost·trace
// → PostConfigInit 단계 → 엔진(엔진 단계) → 호스트 묶음 종료를 돈다. 프로세스 묶음은 Launch가
// 내린다. 멈춘 로그 출력 스레드를 포기했으면 LogHost·설정 문맥을 파괴하지 않고 정적 소멸 없이
// 끝내도록 요청한다(설정 문맥이 모듈 lease를 잡은 채 남으므로 Launch도 로더를 내리지 않는다).
int RunEngineHost(launch::ApplicationContext& context, const EngineHostOptions& options) noexcept {
    // 프로세스의 범용 할당자다(기본 BinnedAllocator — Launch가 소유한다).
    Allocator& allocator = context.DefaultAllocator();
    ModuleLoader& modules = context.Modules();
    char engineDir[4096] = {};
    HostSettingsOptions boot;
    boot.modules = &modules;
    // 설정 struct를 RegisterSettings로 올리는 엔진 모듈의 리플렉션 타입을 설정 문맥의 타입 등록에
    // 넣는다(HostSettings는 Config·TraceControl 타입만 스스로 넣는다). Collision 모듈의
    // CollisionSettings(Engine branch Collision 섹션, Docs/Design/CollisionData.ko.md 3절)와
    // Physics 모듈의 PhysicsSettings(Physics 섹션, Docs/Design/PhysicsModule 6.8절)다 — 없으면 모듈
    // 설정 등록이 NotFound로 실패한다.
    boot.registerModuleTypes = &RegisterSandboxSettingsTypes;
    if (auto found = FindEngineDirectory(Span<char>(engineDir)); found) {
        boot.engineDir = StringView(engineDir, found.Value());
    } else {
        // 엔진 층 파일 없이 코드 기본값으로 돈다(설치 전이므로 비상 경로로 나간다).
        Log::Warning(LogSandboxHost,
                     "Engine/Config was not found above the executable ({Error}); engine config "
                     "files are not read",
                     ErrorCodeName(found.Error()));
    }

    // 1)–5) 설정 문맥과 시작 설정. 실패는 HostSettings가 진단과 함께 기록했다. 설정 문맥과
    // LogHost는 정적 저장소에 만들고 끝에서 직접 파괴한다(멈춘 출력 스레드를 포기했으면 파괴하지
    // 않고 Launch가 정적 소멸 없이 끝낸다).
    alignas(HostSettings) static unsigned char settingsStorage[sizeof(HostSettings)];
    alignas(LogHost) static unsigned char hostStorage[sizeof(LogHost)];
    HostSettings& settings = *::new (settingsStorage) HostSettings(allocator);
    struct SettingsScope {
        HostSettings* settings;
        ~SettingsScope() noexcept {
            if (settings != nullptr) {
                settings->~HostSettings();
            }
        }
    } settingsScope{&settings};
    if (!settings.Initialize(boot)) {
        return 1;
    }

    if (!settings.LoadFiles() ||
        (options.configPath != nullptr &&
         !settings.AddFile(config::ConfigLayer::Project, "Engine", options.configPath)) ||
        !settings.ApplyCommandLine(options.arguments) || !settings.CompleteStartup()) {
        return 2;
    }

    // 6) 로그. log.mode·log.file.* 는 5)에서 고정됐다. 접근 위반·치명 신호 때 마지막 로그는
    // Launch가 설치한 크래시 수집기의 종료 콜백이 flush한다.
    LogHost& host = *::new (hostStorage) LogHost(allocator);
    struct HostScope {
        LogHost* host;
        ~HostScope() noexcept {
            if (host != nullptr) {
                host->~LogHost();
            }
        }
    } hostScope{&host};
    if (!host.Start(LogSystemConfig::ForEngineHost(), &settings.Context(), "Sandbox")) {
        Log::Error(LogSandboxHost, "The log system could not start");
        return 1;
    }

    int exitCode = 0;
    {
        LogInstallScope installed(host.System());
        (void)host.System().SetThreadName("GameThread");
        (void)host.Watch(settings.Context());
        // 개발 빌드는 설정 파일을 감시해 프레임 sync 지점에서 다시 읽는다(출시 규칙이면 Unsupported
        // — 기록하지 않는다).
        (void)settings.StartHotReload();
        // 시작 화면(Docs/Design/SplashScreen 10.3절): 설정·로그 뒤, trace·호스트 단계 모듈·엔진
        // 전에 띄우고 주 창 첫 프레임 뒤 숨긴다(HostLoopBridge). 띄우지 못해도 시작은 계속한다.
        // 엔진이 일찍 끝나면 이 범위가 끝날 때 숨긴다.
        // 진행 중계(Docs/Design/ProgressReporting 7.1절)를 켠다. 스플래시보다 먼저 켜고 나중에
        // 끈다.
        const bool progressReady = static_cast<bool>(ProgressHub::Get().Initialize(allocator, {}));
        struct ProgressScopeExit {
            bool ready;
            ~ProgressScopeExit() noexcept {
                if (ready) {
                    ProgressHub::Get().Shutdown();
                }
            }
        } progressExit{progressReady};
        SandboxSplash shownSplash = ShowSandboxSplash(allocator, options, boot.engineDir);
        splash::SplashScreen* splashScreen =
            shownSplash.screen ? &shownSplash.screen.Value() : nullptr;
        splash::StartupProgress* startup =
            shownSplash.startup ? &shownSplash.startup.Value() : nullptr;
        // 7) trace 세션(trace.* 설정 — 5)에서 고정됐다)·콘솔 명령·--trace. 세션을 열지 못해도
        // 엔진은 돈다.
        TraceSession trace;
        auto captureSettings = trace_control::ReadTraceCaptureSettings(settings.Context());
        TraceSessionSettings traceSettings = trace_control::MakeTraceSessionSettings(
            captureSettings ? captureSettings.Value() : trace_control::TraceCaptureSettings{},
            allocator);
        if (auto started = trace.Start(traceSettings); !started) {
            Log::Warning(LogSandboxHost, "Trace capture could not start: {Error}",
                         ErrorCodeName(started.Error()));
        }

        // 로그를 trace 채널 "log"로도 보낸다(채널이 꺼져 있으면 sink 비용은 분기 하나). 순서: trace
        // 시작 → AddSink … 엔진 정리 → LogSystem::Flush → RemoveSink → trace 정지.
        TraceLogSink traceLog;
        const bool traceLogAdded = trace.IsRunning() && host.System().AddSink(traceLog);
        {
            trace_control::TraceController control(allocator, trace, settings.Context());
            if (trace.IsRunning()) {
                (void)control.RegisterConsoleCommands();
                (void)control.ApplyCommandLine(options.arguments);
                if (options.traceFile != nullptr) {
                    // 크래시 메타데이터 associatedFiles.traceFile에 남긴다(설계 LC6).
                    const StringView traceFile(options.traceFile);
                    if (control.OpenFile(traceFile)) {
                        (void)platform::SetCrashAnnotation(platform::CrashAnnotation::TraceFile,
                                                           traceFile.Data(), traceFile.ByteCount());
                    }
                }
            }

            // 8) 호스트 서비스 등록(3.12절 — 로그·설정 문맥·실행 중인 trace 세션) → 호스트 단계
            // 모듈(UE PostConfigInit) → 엔진 루프(프레임마다 sync 지점) → 호스트 묶음 종료(trace·
            // LogHost가 아직 살아 있을 때 — 3.7절) → 서비스 떼기.
            HostServices& services = modules.Services();
            RegisterHostService(services, host.System());
            RegisterHostService(services, settings.Context());
            if (trace.IsRunning()) {
                RegisterHostService(services, trace);
            }

            // 엔진 셰이더 호스트(Docs/Design/RenderBase 9.12절): Sandbox 타입·원문 디렉터리를 넣고
            // 등록부와 함께 서비스로 올린다. 모듈이 Startup에서 엔진 셰이더 타입·공급자를 넣는다.
            // 실패하면 Error를 남기고 모듈은 등록부 없이 시작한다.
            (void)shaders::AttachHost(modules, shaders::SandboxShaderScope::Engine);

            ModuleLoadReport hostModules;
            {
                ModuleStartProgress moduleProgress(modules, allocator);
                hostModules = modules.StartPhase(LoadingPhase::PostConfigInit);
            }

            if (hostModules.IsOk()) {
                exitCode = RunEngine(allocator, options, settings.Context(), modules,
                                     boot.engineDir, splashScreen, startup);
            } else {
                Log::Error(LogSandboxHost,
                           "Host module {Module} failed to start ({Code}) at {Stage}: {Message}",
                           StringView(hostModules.failed), ModuleLoadCodeName(hostModules.code),
                           StringView(hostModules.stage), StringView(hostModules.message));
                exitCode = 1;
            }

            // 주 창으로 넘기기 전에 엔진이 끝났으면(시작 중 창 닫기·짧은 --frames·실패) 시작 작업을
            // 결과대로 닫는다. 넘겼으면 이미 끝나 아무 일도 하지 않는다.
            if (startup != nullptr) {
                startup->Finish(exitCode == 0 ? ProgressOutcome::Abandoned
                                              : ProgressOutcome::Failed);
            }

            modules.ShutdownGroup(ModuleGroup::Host);
            shaders::DetachHost(modules);
            (void)services.Unregister(trace);
            (void)services.Unregister(settings.Context());
            (void)services.Unregister(host.System());
            // 출력 스레드에 남은 기록이 trace에 닿게 한 뒤 sink를 뺀다(trace 출력이 닫히기 전에).
            if (traceLogAdded) {
                (void)host.System().Flush(TraceLogFlushTimeoutMilliseconds);
                (void)host.System().RemoveSink(traceLog);
            }
        }

        trace.Stop();
    }

    host.Stop();
    if (host.WriterAbandoned()) {
        // 멈춘 sink에 걸린 출력 스레드가 남았다. LogHost·설정 문맥·전역 소멸자·CRT 종료 처리를
        // 돌리면 그 스레드와 겹칠 수 있으므로 파괴하지 않고, Launch가 크래시 수집을 정상 종료로 뗀
        // 뒤 정적 소멸 없이 끝낸다(LogHost::Stop 설명).
        hostScope.host = nullptr;
        settingsScope.settings = nullptr;
        context.RequestExitWithoutStaticTeardown();
    }

    return exitCode;
}

uint32 RunEngineHostSelfTest() noexcept {
    char frames[24];
    FixedStringBuilder framesText{Span<char>(frames)};
    (void)framesText.TryAppendFormat("{}", SelfTestFrames);
    const platform::ProcessArgument arguments[] = {
        {"--engine", 8}, {"--frames", 8}, {frames, framesText.ByteCount()}};
    static char errorText[SelfTestOutputBytes];
    const ChildRun child = RunChild(arguments, errorText, sizeof(errorText));
    const char* const expected[] = {
        "[Info][Engine] Engine initialized with ",
        "[Info][RenderBase] Engine shader types are frozen: ",
        "[Info][Gameplay] Opened world Main",
        "[Info][Sandbox.Game] Pawn possessed by the local player",
        "[Info][Engine] Engine run finished after 3 frames: MaxFrames",
        "[Info][Engine] Engine shutting down with 1 worlds after 3 frames",
        "[Info][Engine] Engine shut down"};
    bool passed = child.exited && child.code == 0;
    for (const char* line : expected) {
        passed =
            passed && child.stderrText.Contains(StringView(line), StringComparison::CaseSensitive);
    }

    printf("  [%s] engine host: --engine --frames 3 logs start, run finished and shutdown\n",
           passed ? "PASS" : "FAIL");
    if (!passed) {
        printf("    exit %u, stderr:\n%.*s\n", child.code,
               static_cast<int>(child.stderrText.ByteCount()), child.stderrText.Data());
    }

    const bool configured = CheckConfiguredFileLogging();
    printf("  [%s] engine host: --config switches to sync and writes the log file\n",
           configured ? "PASS" : "FAIL");
#if SANDBOX_TRACE_ANALYSIS
    const bool traced = CheckTraceLogRecords();
    printf("  [%s] engine host: --trace=default writes log records into the trace file\n",
           traced ? "PASS" : "FAIL");
#else
    // Release는 Developer 모듈(TraceAnalysis)을 링크하지 않으므로 trace 파일을 읽지 않는다.
    const bool traced = true;
    printf("  [NOT RUN] engine host: trace file log records (TraceAnalysis is not linked in "
           "Release)\n");
#endif
    SystemAllocator allocator;
    const bool traits = CheckDeviceTraitsFromCaps(allocator);
    printf("  [%s] engine host: device caps become config device traits\n",
           traits ? "PASS" : "FAIL");
    const bool terminated = CheckTerminationRequestBridge();
    printf("  [%s] engine host: a termination request ends the engine run normally\n",
           terminated ? "PASS" : "FAIL");
    const bool input = CheckInjectedInputReachesPawn();
    printf("  [%s] engine host: a queued key reaches the possessed pawn through the local player\n",
           input ? "PASS" : "FAIL");
    return (passed ? 0u : 1u) + (configured ? 0u : 1u) + (traced ? 0u : 1u) + (traits ? 0u : 1u) +
           (terminated ? 0u : 1u) + (input ? 0u : 1u);
}

} // namespace sandbox
