#include "ScreenSelfTest.h"

#include "CookedSelfTestShaders.h"
#include "CpuDisplayColor.h"
#include "SandboxGame.h"
#include "ScreenHost.h"
#include "SandboxShaders.h"
#include "SelfTestEnvironment.h"

#include <Core/Containers/Array.h>
#include <Engine/Engine.h>
#include <Gameplay/GameInstance.generated.h>
#include <Gameplay/GameInstance.h>
#include <Gameplay/GameInstanceClient.h>
#include <Input/InputEvent.h>
#include <Input/InputState.h>
#include <PlatformWindow/Window.h>
#include <math.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;

// 창(=capture) 크기와 프레임 수다. 16:9이고 수평 FOV 90°다.
constexpr uint32 ScreenWidth = 160;
constexpr uint32 ScreenHeight = 90;
// 쿠킹 화면 검사 이름("<장치> cooked") 버퍼 byte 수다.
constexpr usize MaxCookedLabelBytes = 96;
// 프레임마다 그릴 view다(프레임 번호 1부터 — capture 자리는 프레임 번호 − 1). 1은 장면, 나머지는
// 텍스처 표시(18.5절 6항)다.
struct FrameView {
    screen::InspectorView view;
    uint32 layer;
    uint32 mip;
};

constexpr FrameView FrameViews[] = {
    {screen::InspectorView::None, 0, 0},           {screen::InspectorView::Texture2D, 0, 0},
    {screen::InspectorView::Texture2D, 0, 1},      {screen::InspectorView::Hdr2D, 0, 0},
    {screen::InspectorView::Texture2DArray, 2, 0}, {screen::InspectorView::Cube, 0, 0},
    {screen::InspectorView::CubeArray, 1, 0},      {screen::InspectorView::Volume, 2, 0}};
constexpr uint64 ScreenFrames = sizeof(FrameViews) / sizeof(FrameViews[0]);
static_assert(ScreenFrames <= screen::MaxCaptureSlots, "one capture slot per frame");
constexpr double FrameSeconds = 1.0 / 60.0;
// GPU 결과와 CPU 기준의 채널 허용 차(8 bit)다.
constexpr int ChannelTolerance = 2;

struct Expected {
    int rgb[3];
};

Expected ExpectedPixel(const float (&scene)[3]) noexcept {
    const DisplayPixel pixel = ExpectedTonemapPixel(scene);
    return Expected{{pixel.rgb[0], pixel.rgb[1], pixel.rgb[2]}};
}

// --- 엔진 실행 ---

uint32 screenFailures = 0;

void ScreenCheck(bool condition, const char* name, const char* mode, const char* what) noexcept {
    if (!condition) {
        ++screenFailures;
        printf("  [FAIL] %s screen (%s): %s\n", name, mode, what);
    }
}

// 엔진 루프를 ScreenHost에 잇는다(EngineHost HostLoopBridge의 렌더 부분과 같다).
class ScreenTestHooks final : public EngineLoopHooks {
public:
    ScreenTestHooks(screen::ScreenHost& screen, platform::Window& window, input::InputQueue& queue,
                    GameInstanceClient& client) noexcept
        : screen_(screen), window_(window), queue_(queue), client_(client) {
    }

    render::RenderLanePump Pump() noexcept {
        return {&PumpWindow, this};
    }

    void PumpEvents(Engine&) noexcept override {
        window_.PumpEvents(queue_);
        queue_.Clear();
    }

    void EndFrame(Engine& engine) noexcept override {
        // 첫 프레임 끝에 capture를 요청한다(자원 작업이 그 프레임 packet보다 먼저 lane에 들어간다).
        const uint64 frame = engine.Time().frameIndex;
        if (frame == 1) {
            captureRequested = screen_.RequestCapture(ScreenWidth, ScreenHeight).IsOk();
        }

        if (frame >= 1 && frame <= ScreenFrames) {
            const FrameView& view = FrameViews[frame - 1];
            screen_.SetInspector(view.view, view.layer, view.mip);
            screen_.SetCaptureSlot(static_cast<uint32>(frame - 1));
        }

        screen_.SubmitFrame(&client_);
    }

    void BeginShutdown(Engine&) noexcept override {
        screen_.BlockFrames();
    }

    Result<void> CompleteInFlightWork(Engine&) noexcept override {
        return screen_.WaitFramesIdle();
    }

    Result<uint64> DrainShutdownCleanup(Engine&) noexcept override {
        return screen_.DrainRetires();
    }

    Result<void> VerifyShutdownClosure(Engine&) noexcept override {
        return screen_.CloseResources();
    }

    Result<void> StopExecutors(Engine&) noexcept override {
        return screen_.Stop();
    }

    bool captureRequested = false;

private:
    static void PumpWindow(void* raw) noexcept {
        auto* self = static_cast<ScreenTestHooks*>(raw);
        self->window_.PumpEvents(self->queue_);
    }

    screen::ScreenHost& screen_;
    platform::Window& window_;
    input::InputQueue& queue_;
    GameInstanceClient& client_;
};

struct Pixel {
    int rgb[3];
};

Pixel PixelAt(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const usize offset = (static_cast<usize>(y) * ScreenWidth + x) * 4;
    return Pixel{{pixels[offset], pixels[offset + 1], pixels[offset + 2]}};
}

void CheckExpected(const Array<uint8>& pixels, uint32 x, uint32 y, const Expected& expected,
                   const char* name, const char* mode, const char* what) noexcept {
    const Pixel pixel = PixelAt(pixels, x, y);
    bool close = true;
    for (int channel = 0; channel < 3; ++channel) {
        const int delta = pixel.rgb[channel] - expected.rgb[channel];
        close = close && delta <= ChannelTolerance && delta >= -ChannelTolerance;
    }

    ScreenCheck(close, name, mode, what);
    if (!close) {
        printf("    pixel (%u, %u) is (%d, %d, %d), expected (%d, %d, %d)\n", x, y, pixel.rgb[0],
               pixel.rgb[1], pixel.rgb[2], expected.rgb[0], expected.rgb[1], expected.rgb[2]);
    }
}

// 장면 색(선형 HDR)을 CPU tone map으로 바꿔 대조한다.
void CheckPixel(const Array<uint8>& pixels, uint32 x, uint32 y, const float (&scene)[3],
                const char* name, const char* mode, const char* what) noexcept {
    CheckExpected(pixels, x, y, ExpectedPixel(scene), name, mode, what);
}

constexpr Expected RedPixel = {{255, 0, 0}};
constexpr Expected GreenPixel = {{0, 255, 0}};
constexpr Expected BluePixel = {{0, 0, 255}};
constexpr Expected WhitePixel = {{255, 255, 255}};
constexpr Expected YellowPixel = {{255, 255, 0}};
constexpr Expected BlackPixel = {{0, 0, 0}};

// capture 자리가 찼는가.
bool HasCapture(const Array<uint8>& pixels) noexcept {
    return pixels.Size() == static_cast<usize>(ScreenWidth) * ScreenHeight * 4;
}

// 텍스처 표시 프레임의 픽셀을 단언한다(capture 자리 = 프레임 번호 − 1).
void CheckInspector(const screen::ScreenRenderer& renderer, bool hasCubeArray, const char* name,
                    const char* mode) noexcept {
    // 2D 4×4 mip 0: texel (0, 0) 빨강, (1, 0) 초록(화면 40 pixel = texel 하나).
    const Array<uint8>& plane = renderer.CapturedPixels(1);
    ScreenCheck(HasCapture(plane), name, mode, "inspector 2D frame is captured");
    if (HasCapture(plane)) {
        CheckExpected(plane, 10, 10, RedPixel, name, mode, "inspector 2D mip 0 texel (0, 0)");
        CheckExpected(plane, 50, 10, GreenPixel, name, mode, "inspector 2D mip 0 texel (1, 0)");
    }

    const Array<uint8>& mip = renderer.CapturedPixels(2);
    if (HasCapture(mip)) {
        CheckExpected(mip, ScreenWidth / 2, ScreenHeight / 2, BluePixel, name, mode,
                      "inspector 2D shows the selected mip 1");
    }

    // HDR 2D(2, 1, 0.5): 장면 HDR cube와 같은 filmic 결과.
    const Array<uint8>& hdr = renderer.CapturedPixels(3);
    if (HasCapture(hdr)) {
        CheckPixel(hdr, ScreenWidth / 2, ScreenHeight / 2, screen::InspectorHdrColor, name, mode,
                   "inspector HDR texel equals the CPU filmic tone map");
    }

    const Array<uint8>& layers = renderer.CapturedPixels(4);
    if (HasCapture(layers)) {
        CheckExpected(layers, ScreenWidth / 2, ScreenHeight / 2, BluePixel, name, mode,
                      "inspector 2D array shows layer 2");
    }

    // cube 가로 십자: (2, 1) 칸 +X 빨강, (1, 0) +Y 초록, (3, 1) -Z 노랑, (1, 1) +Z 파랑, (0, 0) 밖
    // 검정.
    const Array<uint8>& cube = renderer.CapturedPixels(5);
    if (HasCapture(cube)) {
        CheckExpected(cube, 100, 45, RedPixel, name, mode, "inspector cube cross +X face");
        CheckExpected(cube, 60, 15, GreenPixel, name, mode, "inspector cube cross +Y face");
        CheckExpected(cube, 140, 45, YellowPixel, name, mode, "inspector cube cross -Z face");
        CheckExpected(cube, 60, 45, BluePixel, name, mode, "inspector cube cross +Z face");
        CheckExpected(cube, 20, 15, BlackPixel, name, mode, "inspector cube cross outside cells");
    }

    if (hasCubeArray) {
        const Array<uint8>& cubes = renderer.CapturedPixels(6);
        if (HasCapture(cubes)) {
            CheckExpected(cubes, 100, 45, WhitePixel, name, mode,
                          "inspector cube array shows cube 1");
        }
    } else {
        printf("  [NOT RUN] %s screen (%s): inspector cube array (no textureCubeArray)\n", name,
               mode);
    }

    const Array<uint8>& volume = renderer.CapturedPixels(7);
    if (HasCapture(volume)) {
        CheckExpected(volume, ScreenWidth / 2, ScreenHeight / 2, BluePixel, name, mode,
                      "inspector volume shows z slice 2");
    }
}

// 한 모드를 돌리고 capture를 out에 남긴다. 돌리지 못했으면 false다.
// host는 엔진 셰이더를 열 호스트다(시험 호스트 또는 쿠킹 호스트 — expectCooked면 쿠킹 라이브러리로
// 열렸는지도 단언한다).
bool RunMode(const char* name, render::RenderLaneMode mode, graphics::Device& device,
             Allocator& allocator, ModuleLoader& modules, const render::EngineShaderHost& host,
             bool expectCooked, Array<uint8>& out) noexcept {
    const char* modeName = mode == render::RenderLaneMode::Thread ? "thread" : "inline";
    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks screen self-test";
    windowDesc.width = ScreenWidth;
    windowDesc.height = ScreenHeight;
    windowDesc.visible = false;
    auto window = platform::Window::Create(allocator, windowDesc);
    input::InputQueue queue(allocator);
    if (!window || !queue.TryInitialize(64)) {
        printf("  [NOT RUN] %s screen (%s): window unavailable\n", name, modeName);
        return false;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    SandboxGame game;
    GameInstanceClient client(allocator, game.config);
    screen::ScreenHost screen(allocator, device, host);
    ScreenTestHooks hooks(screen, *window.Value(), queue, client);
    auto started = screen.Start(*window.Value(), mode, hooks.Pump());
    ScreenCheck(started.IsOk(), name, modeName, "render lane starts with the engine render path");
    if (!started) {
        return false;
    }

    {
        EngineConfig config;
        config.hooks = &hooks;
        config.client = &client;
        config.modules = &modules;
        config.maxFrames = ScreenFrames;
        config.fixedFrameDeltaSeconds = FrameSeconds;
        config.registerTypes = &RegisterSandboxTypes;
        Engine engine(allocator);
        ScreenCheck(engine.Initialize(config).IsOk() && engine.Run().IsOk(), name, modeName,
                    "engine runs frames through the screen render path");
        auto shut = engine.Shutdown();
        ScreenCheck(shut.IsOk(), name, modeName, "engine shutdown hooks close the render lane");
        if (!shut) {
            printf("    shutdown error: %s\n", ErrorCodeName(shut.Error()));
        }
    }

    const render::RenderLaneStats stats = screen.Lane().Stats();
    const screen::ScreenRendererStats& rendered = screen.Renderer().Stats();
    ScreenCheck(screen.Lane().Failure() == ErrorCode::None && rendered.graphFailures == 0, name,
                modeName, "frames render without lane or graph failure");
    ScreenCheck(screen.Renderer().OpenedResources() && rendered.framesRendered == ScreenFrames &&
                    screen.FramesSubmitted() == ScreenFrames,
                name, modeName, "every engine frame is rendered and presented");
    ScreenCheck(hooks.captureRequested && rendered.captures != 0, name, modeName,
                "tone map result is captured on the lane");
    ScreenCheck(stats.tasksPosted == stats.tasksCompleted && !stats.running &&
                    screen.Lane().Handles().Count() == 0,
                name, modeName, "lane drained with no remaining tasks or handles");
    ScreenCheck(device.IsOwnerThread() && device.PendingRetireCount() == 0, name, modeName,
                "device ownership returned with nothing pending");
    ScreenCheck(device.ValidationErrorCount() == validationBefore, name, modeName,
                "no validation errors from screen frames");
    if (expectCooked) {
        // 쿠킹 엔진 라이브러리(ShaderSystem 16.1절 SSD48)로 열었고 매니페스트 순서의 형식이다.
        const StringView format = screen.Renderer().ShaderFormat();
        ScreenCheck(screen.Renderer().OpenedCookedShaders(), name, modeName,
                    "the engine shader set opens from the cooked engine library");
        printf("  %s screen (%s): cooked shader format %.*s\n", name, modeName,
               static_cast<int>(format.ByteCount()), format.Data());
    }

    const Array<uint8>& pixels = screen.Renderer().CapturedPixels(0);
    if (rendered.captures == 0 || !HasCapture(pixels)) {
        return false;
    }

    CheckInspector(screen.Renderer(),
                   screen.EngineEntry(screen::InspectorView::CubeArray) !=
                       screen::ScreenHost::NoInspectorEntry,
                   name, modeName);

    // 화면 가운데는 HDR cube(2, 1, 0.5), 오른쪽 cube(8, 8, 8)는 x = 80 + 80·250/450, 위 가운데는
    // Pending 텍스처 cube(흰 typed 대체 × 1), 왼쪽 위 모서리는 배경이다(카메라 원점, 수평 FOV 90°).
    constexpr float PendingColor[3] = {1.0f, 1.0f, 1.0f};
    const uint32 rightX =
        ScreenWidth / 2 + static_cast<uint32>((ScreenWidth / 2) * screen::SceneRightOffset /
                                              (screen::SceneCubeDistance - 50.0));
    CheckPixel(pixels, ScreenWidth / 2, ScreenHeight / 2, screen::SceneCenterColor, name, modeName,
               "HDR cube pixel equals the CPU filmic tone map");
    CheckPixel(pixels, rightX, ScreenHeight / 2, screen::SceneRightColor, name, modeName,
               "overexposed cube pixel equals the CPU filmic tone map");
    CheckPixel(pixels, ScreenWidth / 2, ScreenHeight / 9, PendingColor, name, modeName,
               "pending texture handle draws with the typed fallback");
    CheckPixel(pixels, 2, 2, screen::SceneClearColor, name, modeName,
               "background pixel equals the tone-mapped clear color");
    // Thread·Inline 대조는 모든 capture 자리를 잇는다.
    out.Clear();
    for (uint32 slot = 0; slot < ScreenFrames; ++slot) {
        (void)out.TryAppend(screen.Renderer().CapturedPixels(slot).View());
    }

    printf("  %s screen (%s): %llu frames, %llu slot waits, %llu tasks\n", name, modeName,
           static_cast<unsigned long long>(rendered.framesRendered),
           static_cast<unsigned long long>(stats.slotWaits),
           static_cast<unsigned long long>(stats.tasksCompleted));
    return true;
}

// 엔진 입력 원천을 시험이 직접 채운다(창 대신).
class InputTestHooks final : public EngineLoopHooks {
public:
    explicit InputTestHooks(GameInstanceClient& client) noexcept : client_(client) {
    }

    void PumpEvents(Engine& engine) noexcept override {
        // 1번째 프레임에 W를 누르고, TurnFrame에 W를 떼고 오른쪽 화살표를 누른다(이동과 회전을
        // 따로 잰다).
        const uint64 frame = engine.Time().frameIndex;
        if (frame <= 1) {
            const input::InputEvent events[] = {input::InputEvent::KeyDown(input::Key::W)};
            input.BeginFrame(Span<const input::InputEvent>(events));
        } else if (frame == TurnFrame) {
            const input::InputEvent events[] = {input::InputEvent::KeyUp(input::Key::W),
                                                input::InputEvent::KeyDown(input::Key::Right)};
            input.BeginFrame(Span<const input::InputEvent>(events));
        } else {
            input.BeginFrame(Span<const input::InputEvent>());
        }
    }

    void EndFrame(Engine& engine) noexcept override {
        CameraView camera;
        if (screen::FindPlayerCamera(client_, camera) && engine.Time().frameIndex < MaxRecorded) {
            views[engine.Time().frameIndex] = camera;
            recorded[engine.Time().frameIndex] = true;
        }
    }

    static constexpr uint64 MaxRecorded = 32;
    static constexpr uint64 TurnFrame = 11;
    input::InputState input;
    CameraView views[MaxRecorded];
    bool recorded[MaxRecorded] = {};

private:
    GameInstanceClient& client_;
};

} // namespace

namespace {

// 두 lane 모드를 host로 돌리고 픽셀이 같은지 본다. 실패 수를 돌려준다.
uint32 RunScreenModes(const char* name, graphics::Device& device, Allocator& allocator,
                      ModuleLoader& modules, const render::EngineShaderHost& host,
                      bool expectCooked) noexcept {
    Array<uint8> thread(allocator);
    Array<uint8> inline_(allocator);
    const bool threadRan = RunMode(name, render::RenderLaneMode::Thread, device, allocator, modules,
                                   host, expectCooked, thread);
    const bool inlineRan = RunMode(name, render::RenderLaneMode::Inline, device, allocator, modules,
                                   host, expectCooked, inline_);
    if (threadRan && inlineRan) {
        bool same = thread.Size() == inline_.Size();
        for (usize index = 0; same && index < thread.Size(); ++index) {
            same = thread[index] == inline_[index];
        }

        ScreenCheck(same, name, "both", "thread and inline lane modes produce the same pixels");
    }

    return screenFailures;
}

} // namespace

uint32 RunScreenChecks(const char* name, graphics::Device& device, Allocator& allocator,
                       ModuleLoader& modules) noexcept {
    screenFailures = 0;
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s screen: %s\n", name, reason);
        return 0;
    }

    if (!screen::CanOpenEngineShaders(shaders::Host())) {
        printf("  [NOT RUN] %s screen: no shader provider (Release)\n", name);
        return 0;
    }

    return RunScreenModes(name, device, allocator, modules, shaders::Host(), false);
}

uint32 RunCookedScreenChecks(const char* name, graphics::Device& device, Allocator& allocator,
                             ModuleLoader& modules) noexcept {
    screenFailures = 0;
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s cooked screen: %s\n", name, reason);
        return 0;
    }

    CookedSelfTestShaders& cooked = CookedSelfTestShaders::Get();
    if (!cooked.IsReady()) {
        printf("  [NOT RUN] %s cooked screen: %s\n", name, cooked.Reason());
        return 0;
    }

    // 쿠킹에 없는 backend(Windows의 OpenGL ES 미리보기 — GLES31_PC는 쿠킹하지 않는다)는 공급자
    // 경로라 이 검사의 대상이 아니다.
    if (!cooked.Cooks(device.Info().backend)) {
        printf("  [NOT RUN] %s cooked screen: the engine cook has no format for this backend\n",
               name);
        return 0;
    }

    char label[MaxCookedLabelBytes];
    snprintf(label, sizeof(label), "%s cooked", name);
    const uint32 failures = RunScreenModes(label, device, allocator, modules, cooked.Host(), true);
    printf("  [%s] %s cooked screen: the engine render path draws with the cooked engine "
           "shader library\n",
           failures == 0 ? "PASS" : "FAIL", name);
    return failures;
}

uint32 RunPawnInputChecks(Allocator& allocator, ModuleLoader& modules) noexcept {
    uint32 failures = 0;
    SandboxGame game;
    GameInstanceClient client(allocator, game.config);
    InputTestHooks hooks(client);
    constexpr uint64 Frames = 20;
    {
        EngineConfig config;
        config.hooks = &hooks;
        config.client = &client;
        config.modules = &modules;
        config.maxFrames = Frames;
        config.fixedFrameDeltaSeconds = FrameSeconds;
        config.registerTypes = &RegisterSandboxTypes;
        Engine engine(allocator);
        bool ran = engine.Initialize(config).IsOk();
        auto attached = client.WithInstance([&](GameInstance& instance) noexcept -> Result<void> {
            return instance.SetLocalPlayerInput(instance.LocalPlayerAt(0), &hooks.input);
        });
        ran = ran && attached && engine.Run().IsOk();
        ran = engine.Shutdown().IsOk() && ran;
        if (!ran) {
            ++failures;
            printf("  [FAIL] pawn input: engine run with the injected input failed\n");
        }
    }

    // 두 프레임 사이 이동·회전이 프레임 수 × 고정 delta로 정해진다(입력이 한 프레임 늦게 반영돼도
    // 구간 안에서는 차가 같다). 이동 구간 4→10(W, yaw 0), 회전 구간 13→19(오른쪽 화살표, 정지).
    constexpr uint64 MoveFrom = 4;
    constexpr uint64 MoveTo = 10;
    constexpr uint64 TurnFrom = 13;
    constexpr uint64 TurnTo = 19;
    if (!hooks.recorded[MoveFrom] || !hooks.recorded[MoveTo] || !hooks.recorded[TurnFrom] ||
        !hooks.recorded[TurnTo]) {
        printf("  [FAIL] pawn input: the player camera was not found\n");
        return failures + 1;
    }

    const CameraView& moveFrom = hooks.views[MoveFrom];
    const CameraView& moveTo = hooks.views[MoveTo];
    const CameraView& turnFrom = hooks.views[TurnFrom];
    const CameraView& turnTo = hooks.views[TurnTo];
    const double moveStep = 1200.0 * FrameSeconds * static_cast<double>(MoveTo - MoveFrom);
    const double yawStep = 45.0 * FrameSeconds * static_cast<double>(TurnTo - TurnFrom);
    const Vec3d moved = moveTo.location - moveFrom.location;
    const bool forward =
        fabs(moved.x - moveStep) < 1e-6 && fabs(moved.y) < 1e-9 && fabs(moved.z) < 1e-9;
    const bool turned = fabs(turnTo.rotation.yaw - turnFrom.rotation.yaw - yawStep) < 1e-6 &&
                        fabs(turnTo.rotation.pitch) < 1e-9 &&
                        (turnTo.location - turnFrom.location).Length() < 1e-9;
    printf("  [%s] pawn input: held W moves the camera forward at 1200 cm/s (%.4f cm in %llu "
           "frames)\n",
           forward ? "PASS" : "FAIL", moved.x, static_cast<unsigned long long>(MoveTo - MoveFrom));
    printf("  [%s] pawn input: held Right turns the camera at 45 deg/s (yaw %.4f -> %.4f)\n",
           turned ? "PASS" : "FAIL", turnFrom.rotation.yaw, turnTo.rotation.yaw);
    return failures + (forward ? 0u : 1u) + (turned ? 0u : 1u);
}

} // namespace sandbox
