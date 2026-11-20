#include "RenderLaneSelfTest.h"

#include <Core/Containers/Array.h>
#include <Engine/Engine.h>
#include <PlatformWindow/Window.h>
#include <RenderBase/RenderLane.h>
#include "SampleRenderer.h"
#include "SelfTestEnvironment.h"
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;

// 프레임 수와 offscreen 대상 크기다.
constexpr uint32 LaneFrames = 5;
constexpr uint32 LaneTargetSize = 64;

// 게임 lane이 World에서 추출한 그리기 항목이다. 텍스처는 장치 handle이 아니라 예약한 렌더
// handle이다.
struct SceneItem {
    Mat4 world = Mat4::Identity();
    LinearColor color = LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
    render::RenderHandle texture;
    sample::MeshKind mesh = sample::MeshKind::Quad;
};

// 프레임 packet이다. 목록은 그 slot의 프레임 arena에 있다. scratch는 lane이 장치 handle로 바꾼
// 항목을 쓰는 같은 길이의 칸이다.
struct ScenePacket {
    sample::RenderView view;
    Span<const SceneItem> items;
    Span<sample::RenderItem> scratch;
    bool capture = false;
};

// 2x2 텍스처 생성 작업 값이다.
struct CheckerUpload {
    render::RenderHandle handle;
    render::RenderBlob pixels;
};

// 스왑체인 크기 변경 표면 작업 값이다.
struct SurfaceResize {
    uint32 width = 0;
    uint32 height = 0;
};

} // namespace

} // namespace sandbox

ROCKS_RENDER_TRANSFERABLE(sandbox::ScenePacket);
ROCKS_RENDER_TRANSFERABLE(sandbox::CheckerUpload);
ROCKS_RENDER_TRANSFERABLE(sandbox::SurfaceResize);

namespace sandbox {

namespace {

// 로컬 Y(오른쪽)·Z(위)를 clip X(오른쪽)·Y(위)로 보내는 변환이다(+X를 바라보는 정면, UE 좌표).
// 단위 메시가 화면을 가득 채운다.
Mat4 FillClip() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = 2.0f;
    matrix.m[9] = 2.0f;
    matrix.m[14] = 0.5f;
    matrix.m[15] = 1.0f;
    return matrix;
}

// 렌더 lane 쪽 처리기다. Renderer·offscreen 대상·스왑체인을 lane에서 만들고 lane에서 정리한다.
class SandboxProcessor final : public render::RenderLaneProcessor {
public:
    explicit SandboxProcessor(Allocator& allocator) noexcept : pixels(allocator) {
    }

    Result<void> OnLaneStart(render::RenderLaneContext& context) noexcept override {
        graphics::Device& device = context.Device();
        graphics::SwapChainDesc swapDesc;
        swapDesc.surface = surface;
        swapDesc.width = 96;
        swapDesc.height = 64;
        swapDesc.vsync = false;
        auto createdSwapChain = device.CreateSwapChain(swapDesc);
        if (!createdSwapChain) {
            return Result<void>::Failure(createdSwapChain.Error());
        }

        swapChain = createdSwapChain.Value();
        auto initialized = renderer.Initialize(device, graphics::PixelFormat::RGBA8UnormSrgb);
        if (!initialized) {
            return initialized;
        }

        graphics::TextureDesc targetDesc;
        targetDesc.width = LaneTargetSize;
        targetDesc.height = LaneTargetSize;
        targetDesc.usage = graphics::TextureUsage::RenderTarget;
        auto createdTarget = device.CreateTexture(targetDesc, {});
        if (!createdTarget) {
            return Result<void>::Failure(createdTarget.Error());
        }

        target = createdTarget.Value();
        return Result<void>::Success();
    }

    void OnLaneStop(render::RenderLaneContext& context) noexcept override {
        renderer.Shutdown();
        context.Device().Retire(target);
        context.Device().Retire(swapChain);
    }

    graphics::NativeSurface surface;
    sample::Renderer renderer;
    graphics::TextureHandle target;
    graphics::SwapChainHandle swapChain;
    Array<uint8> pixels;
    uint32 presented = 0;
    bool captured = false;
};

void RenderScene(render::RenderFrameContext& context, ScenePacket& packet) noexcept {
    auto* processor = static_cast<SandboxProcessor*>(context.Processor());
    graphics::Device& device = context.Device();
    for (usize index = 0; index < packet.items.Size(); ++index) {
        const SceneItem& item = packet.items[index];
        sample::RenderItem& drawn = packet.scratch[index];
        drawn.world = item.world;
        drawn.color = item.color;
        drawn.mesh = item.mesh;
        // 아직 준비되지 않은(Pending) 텍스처는 흰색으로 대신한다(대체 자원 계수는 RB2).
        auto texture = context.Handles().Texture(item.texture);
        drawn.texture = texture ? texture.Value() : graphics::TextureHandle{};
    }

    const Span<const sample::RenderItem> items(packet.scratch.Data(), packet.scratch.Size());
    auto offscreen = device.BeginOffscreen(processor->target, packet.view.clear);
    if (!offscreen) {
        context.ReportFailure(offscreen.Error());
        return;
    }

    processor->renderer.Draw(*offscreen.Value(), packet.view, items);
    if (!device.EndFrame()) {
        context.ReportFailure(ErrorCode::DependencyFailed);
        return;
    }

    if (packet.capture &&
        processor->pixels.TryResize(static_cast<usize>(LaneTargetSize) * LaneTargetSize * 4) &&
        device.ReadPixels(processor->target, processor->pixels.View())) {
        processor->captured = true;
    }

    auto frame = device.BeginFrame(processor->swapChain, packet.view.clear);
    if (!frame) {
        context.ReportFailure(frame.Error());
        return;
    }

    processor->renderer.Draw(*frame.Value(), packet.view, items);
    if (device.EndFrame()) {
        ++processor->presented;
    }
}

void CreateChecker(render::RenderLaneContext& context, CheckerUpload& upload) noexcept {
    graphics::TextureDesc desc;
    desc.width = 2;
    desc.height = 2;
    desc.sampler.filter = graphics::Filter::Point;
    desc.sampler.addressU = graphics::AddressMode::Clamp;
    desc.sampler.addressV = graphics::AddressMode::Clamp;
    auto created = context.Device().CreateTexture(desc, upload.pixels.View());
    render::RenderNotice notice;
    notice.handle = upload.handle;
    if (!created) {
        auto failed = context.Handles().MarkFailed(upload.handle, created.Error());
        (void)failed;
        notice.kind = render::RenderNoticeKind::ResourceFailed;
        notice.error = created.Error();
    } else if (!context.Handles().BindTexture(upload.handle, created.Value())) {
        context.Device().Retire(created.Value());
        return;
    } else {
        notice.kind = render::RenderNoticeKind::ResourceReady;
    }

    context.Notify(notice);
}

void ResizeSurface(render::RenderLaneContext& context, SurfaceResize& resize) noexcept {
    auto* processor = static_cast<SandboxProcessor*>(context.Processor());
    auto resized =
        context.Device().ResizeSwapChain(processor->swapChain, resize.width, resize.height);
    if (!resized) {
        context.ReportFailure(resized.Error());
    }
}

// 게임 lane의 창 메시지 펌프다(엔진 루프와 렌더 lane slot 대기가 함께 쓴다).
struct WindowPump {
    platform::Window* window = nullptr;
    input::InputQueue* queue = nullptr;

    static void Pump(void* raw) noexcept {
        auto* self = static_cast<WindowPump*>(raw);
        self->window->PumpEvents(*self->queue);
    }
};

// 엔진 루프를 렌더 lane에 잇는다. 프레임 끝에 장면을 추출해 프레임 packet으로 넘기고, 종료 hook으로
// 렌더 lane 종료 순서를 실행한다.
class LaneEngineHooks final : public EngineLoopHooks {
public:
    LaneEngineHooks(render::RenderLane& lane, WindowPump& pump, Allocator& allocator) noexcept
        : notices(allocator), lane_(lane), pump_(pump) {
    }

    void PumpEvents(Engine&) noexcept override {
        WindowPump::Pump(&pump_);
    }

    void EndFrame(Engine& engine) noexcept override {
        // 완료 통지를 프레임마다 수거한다.
        auto taken = lane_.Notices().TakeAll(notices);
        (void)taken;
        auto ticket = lane_.BeginFrame();
        if (!ticket) {
            ++frameErrors;
            return;
        }

        FrameArena& arena = lane_.Arena(ticket.Value());
        auto items = arena.TryNewArray<SceneItem>(2);
        auto scratch = arena.TryNewArray<sample::RenderItem>(2);
        if (!items || !scratch) {
            ++frameErrors;
            lane_.AbandonFrame(Move(ticket.Value()));
            return;
        }

        // 텍스처 사각형이 화면을 채우고, 그 위에 작은 삼각형을 가운데 둔다.
        items.Value()[0].texture = texture;
        items.Value()[1].mesh = sample::MeshKind::Triangle;
        Mat4 small = Mat4::Identity();
        small.m[5] = 0.25f;
        small.m[10] = 0.25f;
        items.Value()[1].world = small;
        ScenePacket packet;
        packet.view.viewProjection = FillClip();
        packet.view.clear = {0.0f, 0.0f, 0.0f, 1.0f};
        packet.items = items.Value();
        packet.scratch = scratch.Value();
        packet.capture = engine.Time().frameIndex == LaneFrames;
        if (!lane_.SubmitFrame(Move(ticket.Value()), Move(packet), &RenderScene)) {
            ++frameErrors;
        }

        // 프레임 중간에 창 크기 변경을 표면 작업으로 넘긴다.
        if (engine.Time().frameIndex == 2 &&
            !lane_.PostSurface(SurfaceResize{128, 80}, &ResizeSurface)) {
            ++frameErrors;
        }
    }

    void BeginShutdown(Engine&) noexcept override {
        lane_.BlockFrames();
    }

    Result<void> CompleteInFlightWork(Engine&) noexcept override {
        return lane_.WaitFramesIdle();
    }

    Result<uint64> DrainShutdownCleanup(Engine&) noexcept override {
        // 최종 GC가 텍스처 에셋을 파괴해 생긴 Retire를 흉내 낸다.
        if (!textureRetired) {
            textureRetired = lane_.Retire(texture).IsOk();
        }

        return lane_.DrainRetires();
    }

    Result<void> VerifyShutdownClosure(Engine&) noexcept override {
        return lane_.CloseResources();
    }

    Result<void> StopExecutors(Engine&) noexcept override {
        return lane_.Stop();
    }

    render::RenderHandle texture;
    Array<render::RenderNotice> notices;
    uint32 frameErrors = 0;
    bool textureRetired = false;

private:
    render::RenderLane& lane_;
    WindowPump& pump_;
};

struct Pixel {
    uint8 r;
    uint8 g;
    uint8 b;
};

Pixel PixelAt(const Array<uint8>& pixels, uint32 x, uint32 y) noexcept {
    const usize offset = (static_cast<usize>(y) * LaneTargetSize + x) * 4;
    return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
}

uint32 laneFailures = 0;

void LaneCheck(bool condition, const char* name, const char* mode, const char* what) noexcept {
    if (!condition) {
        ++laneFailures;
        printf("  [FAIL] %s render lane (%s): %s\n", name, mode, what);
    }
}

void RunMode(const char* name, render::RenderLaneMode mode, graphics::Device& device,
             Allocator& allocator, ModuleLoader& modules) noexcept {
    const char* modeName = mode == render::RenderLaneMode::Thread ? "thread" : "inline";
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s render lane (%s): %s\n", name, modeName, reason);
        return;
    }

    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks render lane self-test";
    windowDesc.width = 96;
    windowDesc.height = 64;
    windowDesc.visible = false;
    auto window = platform::Window::Create(allocator, windowDesc);
    input::InputQueue queue(allocator);
    if (!window || !queue.TryInitialize(64)) {
        printf("  [NOT RUN] %s render lane (%s): window unavailable\n", name, modeName);
        return;
    }

    WindowPump pump{window.Value().Get(), &queue};
    SandboxProcessor processor(allocator);
    const platform::NativeWindowHandle native = window.Value()->NativeHandle();
    processor.surface.window = native.window;
    processor.surface.instance = native.instance;
    render::RenderLaneConfig config;
    config.mode = mode;
    config.processor = &processor;
    config.pump = {&WindowPump::Pump, &pump};
    const uint64 validationBefore = device.ValidationErrorCount();
    render::RenderLane lane(allocator, device);
    auto started = lane.Start(config);
    LaneCheck(started.IsOk(), name, modeName, "lane starts and takes the device");
    if (!started) {
        return;
    }

    LaneEngineHooks hooks(lane, pump, allocator);
    // 2x2 텍스처: 좌상 빨강, 우상 초록, 좌하 파랑, 우하 흰색. 게임 lane은 handle을 바로 받는다.
    const uint8 checker[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    auto reserved = lane.ReserveHandle(render::RenderResourceKind::Texture);
    auto blob = render::RenderBlob::TryCopy(allocator, Span<const uint8>(checker, sizeof(checker)));
    LaneCheck(
        reserved && blob &&
            lane.PostResource(CheckerUpload{reserved.Value(), Move(blob.Value())}, &CreateChecker),
        name, modeName, "texture handle reserved and created on the lane");
    if (reserved) {
        hooks.texture = reserved.Value();
    }

    {
        EngineConfig engineConfig;
        engineConfig.hooks = &hooks;
        engineConfig.modules = &modules;
        engineConfig.maxFrames = LaneFrames;
        engineConfig.fixedFrameDeltaSeconds = 1.0 / 60.0;
        Engine engine(allocator);
        LaneCheck(engine.Initialize(engineConfig).IsOk() && engine.Run().IsOk(), name, modeName,
                  "engine runs frames through the render lane");
        auto shut = engine.Shutdown();
        LaneCheck(shut.IsOk(), name, modeName, "engine shutdown hooks close the render lane");
        if (!shut) {
            printf("    shutdown error: %s\n", ErrorCodeName(shut.Error()));
        }
    }

    // 렌더 lane이 끝까지 닫혔다(lane이 아직 돌면 Stop으로 닫는다).
    auto stopped = lane.Shutdown();
    (void)stopped;
    const render::RenderLaneStats stats = lane.Stats();
    LaneCheck(hooks.frameErrors == 0 && lane.Failure() == ErrorCode::None, name, modeName,
              "frames submit without lane failure");
    LaneCheck(stats.framesCompleted == LaneFrames && processor.presented == LaneFrames, name,
              modeName, "every frame is processed, presented and GPU-completed");
    LaneCheck(stats.retiresCompleted == 1 && stats.retiresPending == 0 &&
                  lane.Handles().Count() == 0,
              name, modeName, "retired texture is reclaimed after its fence");
    LaneCheck(stats.tasksPosted == stats.tasksCompleted && !stats.running, name, modeName,
              "lane drained with no remaining tasks");
    LaneCheck(device.IsOwnerThread() && device.PendingRetireCount() == 0, name, modeName,
              "device ownership returned with nothing pending");
    LaneCheck(device.ValidationErrorCount() == validationBefore, name, modeName,
              "no validation errors from lane frames");
    if (mode == render::RenderLaneMode::Thread) {
        LaneCheck(stats.ownershipReturned, name, modeName, "ownership came back from the lane");
    }

    LaneCheck(processor.captured, name, modeName, "last frame captured on the lane");
    if (processor.captured) {
        // 텍스처 사각형의 네 귀퉁이(텍스처가 Ready였다)와 가운데 삼각형(빨강 꼭짓점 쪽)이다.
        const Pixel topLeft = PixelAt(processor.pixels, 4, 4);
        const Pixel bottomRight = PixelAt(processor.pixels, 59, 59);
        LaneCheck(topLeft.r > 200 && topLeft.g < 40 && topLeft.b < 40, name, modeName,
                  "lane frame samples the lane-created texture");
        LaneCheck(bottomRight.r > 200 && bottomRight.g > 200 && bottomRight.b > 200, name, modeName,
                  "lane frame keeps the texture orientation");
    }

    bool ready = false;
    uint32 completedNotices = 0;
    auto taken = lane.Notices().TakeAll(hooks.notices);
    (void)taken;
    for (const render::RenderNotice& notice : hooks.notices) {
        ready = ready || notice.kind == render::RenderNoticeKind::ResourceReady;
        completedNotices += notice.kind == render::RenderNoticeKind::FrameCompleted ? 1u : 0u;
    }

    LaneCheck(ready && completedNotices == LaneFrames, name, modeName,
              "completion notices reach the game lane");
    printf("  %s render lane (%s): %llu frames, %llu slot waits, %llu pumps, %llu tasks\n", name,
           modeName, static_cast<unsigned long long>(stats.framesCompleted),
           static_cast<unsigned long long>(stats.slotWaits),
           static_cast<unsigned long long>(stats.pumpCalls),
           static_cast<unsigned long long>(stats.tasksCompleted));
}

} // namespace

uint32 RunRenderLaneChecks(const char* name, graphics::Device& device, Allocator& allocator,
                           ModuleLoader& modules) noexcept {
    laneFailures = 0;
    RunMode(name, render::RenderLaneMode::Thread, device, allocator, modules);
    RunMode(name, render::RenderLaneMode::Inline, device, allocator, modules);
    return laneFailures;
}

} // namespace sandbox
