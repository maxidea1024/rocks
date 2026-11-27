#include "SwapChainShareSelfTest.h"

#include "SelfTestEnvironment.h"

#include <Core/Containers/Array.h>
#include <Core/Memory/FrameArena.h>
#include <Platform/Memory.h>
#include <PlatformWindow/Window.h>
#include <RenderGraph/RenderGraph.h>
#include <Task/WorkerScheduler.h>
#include <stdio.h>

namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using rocks::render::GraphLoadAction;
using rocks::render::GraphMode;
using rocks::render::GraphPassAccesses;
using rocks::render::GraphPassContext;
using rocks::render::GraphPassFlags;
using rocks::render::GraphPassKind;
using rocks::render::GraphResourcePool;
using rocks::render::GraphStats;
using rocks::render::GraphTexture;
using rocks::render::RenderGraph;
using rocks::render::RenderGraphDesc;
using rocks::render::RenderHandleTable;

// 창·스왑체인·그래프 텍스처 한 변이다.
constexpr uint32 Size = 32;
constexpr usize ImageBytes = static_cast<usize>(Size) * Size * 4;
// 직렬 실행 프레임 수다(프레임마다 present 하나를 센다).
constexpr uint32 SerialFrames = 3;
// 병렬 기록 worker 수다.
constexpr uint32 Workers = 3;
// 채널 허용 오차다(RenderBase 8절 공통 기준 ±8).
constexpr int ChannelTolerance = 8;
// overlay 사각형의 크기 배율이다(화면 가운데 절반을 덮는다).
constexpr float OverlayScale = 0.5f;
// 가장자리·가운데 표본 좌표다.
constexpr uint32 EdgeSample = 2;
constexpr uint32 CenterSample = Size / 2;

TraceScopeName SceneName("SwapChainShare.Scene");
TraceScopeName GreenName("SwapChainShare.Green");
TraceScopeName TonemapName("SwapChainShare.Tonemap");
TraceScopeName CopyName("SwapChainShare.Copy");
TraceScopeName OverlayName("SwapChainShare.Overlay");

uint32 failures = 0;
const char* backendName = "";

void Check(bool condition, const char* what) noexcept {
    if (!condition) {
        ++failures;
        printf("  [FAIL] %s swap chain share: %s\n", backendName, what);
    }
}

// 로컬 Y(오른쪽)·Z(위)를 clip X·Y로 보내 단위 사각형이 화면을 가득 채우는 변환이다(UE 좌표).
Mat4 FillClip() noexcept {
    Mat4 matrix = Mat4::Zero();
    matrix.m[4] = 2.0f;
    matrix.m[9] = 2.0f;
    matrix.m[14] = 0.5f;
    matrix.m[15] = 1.0f;
    return matrix;
}

bool PixelIs(const Array<uint8>& pixels, uint32 x, uint32 y, uint8 r, uint8 g, uint8 b) noexcept {
    const usize offset = (static_cast<usize>(y) * Size + x) * 4;
    auto near = [](uint8 actual, uint8 expected) noexcept {
        const int difference = static_cast<int>(actual) - static_cast<int>(expected);
        return difference <= ChannelTolerance && difference >= -ChannelTolerance;
    };

    const bool same =
        near(pixels[offset], r) && near(pixels[offset + 1], g) && near(pixels[offset + 2], b);
    if (!same) {
        printf("    (%u, %u) is (%u, %u, %u), expected (%u, %u, %u)\n", x, y, pixels[offset],
               pixels[offset + 1], pixels[offset + 2], r, g, b);
    }

    return same;
}

// 그리는 pass(장면·tone map·overlay)마다 하나인 샘플 렌더러의 번호다.
enum PassRenderer : uint32 {
    SceneRenderer,
    TonemapRenderer,
    OverlayRenderer,
    PassRendererCount
};

struct Bench {
    Device* device = nullptr;
    sample::Renderer* renderers[PassRendererCount] = {};
    RenderHandleTable* handles = nullptr;
    GraphResourcePool* pool = nullptr;
    FrameArena* arena = nullptr;
    SwapChainHandle swapChain;
    TextureHandle red;
    TextureHandle capture;
    uint64 frame = 0;
};

// texture(null이면 그래프 자원 source)를 scale 배율 사각형으로 그리는 pass 함수다.
auto Quad(sample::Renderer* renderer, TextureHandle texture, GraphTexture source,
          float scale) noexcept {
    return [renderer, texture, source, scale](GraphPassContext& pass) noexcept {
        sample::RenderView view;
        view.viewProjection = FillClip();
        sample::RenderItem item;
        item.world = Mat4::Identity();
        item.world.m[0] = scale;
        item.world.m[5] = scale;
        item.world.m[10] = scale;
        item.texture = source.IsNull() ? texture : pass.Texture(source);
        renderer->Draw(pass.Commands(), view, Span<const sample::RenderItem>(&item, 1));
    };
}

struct FrameResult {
    bool executed = false;
    GraphStats stats;
    uint64 presents = 0;
};

// 장면 → 초록 → tone map(스왑체인 clear) → Copy(초록 → 복사본) → overlay(스왑체인 load, 가운데
// 사각형, CopyPassTarget 캡처) 그래프 하나를 실행한다.
FrameResult RunFrame(Bench& bench, GraphMode mode, WorkerScheduler* scheduler,
                     Allocator& allocator) noexcept {
    FrameResult result;
    auto cleared = bench.arena->Reset();
    (void)cleared;
    RenderGraphDesc desc;
    desc.device = bench.device;
    desc.handles = bench.handles;
    desc.pool = bench.pool;
    desc.arena = bench.arena;
    desc.frame = ++bench.frame;
    desc.mode = mode;
    desc.scheduler = scheduler;
    desc.scratch = &allocator;
    const uint64 presentsBefore = bench.device->PresentCount();
    {
        RenderGraph graph(desc);
        TextureDesc textureDesc;
        textureDesc.width = Size;
        textureDesc.height = Size;
        textureDesc.usage = TextureUsage::RenderTarget;
        textureDesc.sampler.filter = Filter::Point;
        auto scene = graph.CreateTexture(textureDesc, "Scene");
        auto green = graph.CreateTexture(textureDesc, "Green");
        auto copied = graph.CreateTexture(textureDesc, "Copied");
        if (!scene || !green || !copied) {
            return result;
        }

        GraphPassAccesses sceneAccesses(graph);
        sceneAccesses.RenderTarget(scene.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
        GraphPassAccesses greenAccesses(graph);
        greenAccesses.RenderTarget(green.Value(), GraphLoadAction::Clear, {0, 1, 0, 1});
        GraphPassAccesses tonemap(graph);
        tonemap.Texture(scene.Value(), ResourceAccess::SrvGraphics)
            .SwapChainTarget(bench.swapChain, GraphLoadAction::Clear, {0, 0, 1, 1});
        GraphPassAccesses copy(graph);
        copy.Texture(green.Value(), ResourceAccess::CopySource)
            .Texture(copied.Value(), ResourceAccess::CopyDestination);
        GraphPassAccesses overlay(graph);
        overlay.Texture(copied.Value(), ResourceAccess::SrvGraphics)
            .SwapChainTarget(bench.swapChain, GraphLoadAction::Load);
        const GraphTexture greenTexture = green.Value();
        const GraphTexture copiedTexture = copied.Value();
        const auto overlayQuad =
            Quad(bench.renderers[OverlayRenderer], {}, copiedTexture, OverlayScale);
        const TextureHandle capture = bench.capture;
        const bool added =
            graph
                .AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, sceneAccesses,
                         Quad(bench.renderers[SceneRenderer], bench.red, {}, 1.0f))
                .IsOk() &&
            graph
                .AddPass(GreenName, GraphPassKind::Raster, GraphPassFlags::None, greenAccesses,
                         [](GraphPassContext&) noexcept {})
                .IsOk() &&
            graph
                .AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, tonemap,
                         Quad(bench.renderers[TonemapRenderer], {}, scene.Value(), 1.0f))
                .IsOk() &&
            graph
                .AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, copy,
                         [greenTexture, copiedTexture](GraphPassContext& pass) noexcept {
                             (void)pass.Commands().CopyTexture(pass.Texture(copiedTexture),
                                                               pass.Texture(greenTexture));
                         })
                .IsOk() &&
            graph
                .AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, overlay,
                         [overlayQuad, capture](GraphPassContext& pass) noexcept {
                             overlayQuad(pass);
                             (void)pass.Commands().CopyPassTarget(capture);
                         })
                .IsOk();
        result.executed = added && graph.Execute().IsOk();
        if (!result.executed) {
            printf("    graph error: %.*s\n", static_cast<int>(graph.LastError().ByteCount()),
                   graph.LastError().Data());
        }

        result.stats = graph.Stats();
    }

    result.presents = bench.device->PresentCount() - presentsBefore;
    return result;
}

// 캡처를 읽어 가장자리 빨강(tone map 결과를 load로 이어 받음)·가운데 초록(overlay)을 본다.
bool ReadCapture(Bench& bench, Array<uint8>& pixels) noexcept {
    return pixels.TryResize(ImageBytes).IsOk() &&
           bench.device->ReadPixels(bench.capture, pixels.View()).IsOk();
}

} // namespace

uint32 RunSwapChainShareChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    failures = 0;
    backendName = name;
    if (const char* reason = WindowPresentUnavailableReason(device)) {
        printf("  [NOT RUN] %s swap chain share (%s)\n", name, reason);
        return failures;
    }

    platform::WindowDesc windowDesc;
    windowDesc.title = "Rocks swap chain share self-test";
    windowDesc.width = Size;
    windowDesc.height = Size;
    windowDesc.resizable = false;
    windowDesc.visible = false;
    auto window = platform::Window::Create(allocator, windowDesc);
    if (!window) {
        printf("  [NOT RUN] %s swap chain share (window unavailable)\n", name);
        return failures;
    }

    if (window.Value()->Width() != Size || window.Value()->Height() != Size) {
        printf("  [NOT RUN] %s swap chain share (client area is %ux%u, not %ux%u)\n", name,
               window.Value()->Width(), window.Value()->Height(), Size, Size);
        return failures;
    }

    const uint64 validationBefore = device.ValidationErrorCount();
    const platform::NativeWindowHandle native = window.Value()->NativeHandle();
    SwapChainDesc swapDesc;
    swapDesc.surface.window = native.window;
    swapDesc.surface.instance = native.instance;
    swapDesc.width = Size;
    swapDesc.height = Size;
    swapDesc.vsync = false;
    auto swapChain = device.CreateSwapChain(swapDesc);
    const uint8 redPixel[4] = {255, 0, 0, 255};
    TextureDesc redDesc;
    redDesc.width = 1;
    redDesc.height = 1;
    redDesc.sampler.filter = Filter::Point;
    auto red = device.CreateTexture(redDesc, Span<const uint8>(redPixel, sizeof(redPixel)));
    TextureDesc captureDesc;
    captureDesc.width = Size;
    captureDesc.height = Size;
    captureDesc.format = PixelFormat::RGBA8UnormSrgb;
    captureDesc.usage = TextureUsage::RenderTarget;
    auto capture = device.CreateTexture(captureDesc, {});
    sample::Renderer renderers[PassRendererCount];
    bool renderersReady = true;
    for (sample::Renderer& renderer : renderers) {
        renderersReady =
            renderersReady && renderer.Initialize(device, PixelFormat::RGBA8UnormSrgb).IsOk();
    }

    Check(swapChain.IsOk() && red.IsOk() && capture.IsOk() && renderersReady,
          "swap chain, texture and renderer creation");
    if (!swapChain || !red || !capture || !renderersReady) {
        return failures;
    }

    RenderHandleTable handles(allocator);
    GraphResourcePool pool(allocator, device);
    FrameArena arena(allocator);
    Bench bench;
    bench.device = &device;
    for (uint32 index = 0; index < PassRendererCount; ++index) {
        bench.renderers[index] = &renderers[index];
    }

    bench.handles = &handles;
    bench.pool = &pool;
    bench.arena = &arena;
    bench.swapChain = swapChain.Value();
    bench.red = red.Value();
    bench.capture = capture.Value();

    // 직렬 실행: 프레임마다 스왑체인 기록 둘(clear·present 없음, load·present)과 present 하나.
    Array<uint8> serial(allocator);
    bool framesOk = true;
    for (uint32 frame = 0; frame < SerialFrames; ++frame) {
        const FrameResult result = RunFrame(bench, GraphMode::Deferred, nullptr, allocator);
        framesOk =
            framesOk && result.executed && result.presents == 1 && result.stats.presents == 1;
        if (!result.executed || result.presents != 1) {
            printf("    frame %u: executed %s, device presents %llu, graph presents %u\n", frame,
                   result.executed ? "yes" : "no", static_cast<unsigned long long>(result.presents),
                   result.stats.presents);
        }
    }

    Check(framesOk, "each frame of two passes on one swap chain presents exactly once");
    const bool serialRead = ReadCapture(bench, serial);
    Check(serialRead && PixelIs(serial, EdgeSample, EdgeSample, 255, 0, 0),
          "the overlay pass loads the tone map result (edge is the red scene)");
    Check(serialRead && PixelIs(serial, CenterSample, CenterSample, 0, 255, 0),
          "the overlay quad samples the copied green texture (center is green)");

    // 병렬 기록(RG3): 픽셀이 같고 present 하나.
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = Workers;
    const bool started = scheduler.Start(config).IsOk();
    Check(started, "worker scheduler start");
    if (started) {
        Array<uint8> parallel(allocator);
        const FrameResult result = RunFrame(bench, GraphMode::Deferred, &scheduler, allocator);
        Check(result.executed && result.presents == 1 && result.stats.parallelPasses != 0,
              "parallel recording presents once");
        Check(ReadCapture(bench, parallel) &&
                  platform::MemoryEquals(serial.Data(), parallel.Data(), ImageBytes),
              "serial and parallel recording give the same pixels");
        (void)scheduler.Shutdown();
    }

    // 즉시 실행 모드: Execute 끝의 present 기록이 한 번 present하고 픽셀이 같다.
    {
        Array<uint8> immediate(allocator);
        const FrameResult result = RunFrame(bench, GraphMode::Immediate, nullptr, allocator);
        Check(result.executed && result.presents == 1 && result.stats.presents == 1,
              "immediate mode presents once at Execute");
        Check(ReadCapture(bench, immediate) &&
                  platform::MemoryEquals(serial.Data(), immediate.Data(), ImageBytes),
              "immediate and deferred modes give the same pixels");
    }

    // 장치 규칙: present 뒤의 백버퍼는 load할 수 없다.
    SwapChainPassDesc load;
    load.swapChain = bench.swapChain;
    load.load = LoadAction::Load;
    auto rejected = device.BeginFrame(load);
    Check(rejected.Error() == ErrorCode::InvalidArgument,
          "the device rejects loading a swap chain after its present");
    if (rejected) {
        (void)device.EndFrame();
    }

    const uint64 validationErrors = device.ValidationErrorCount() - validationBefore;
    Check(validationErrors == 0, "validation errors are zero");
    printf("  %s swap chain share: frames %u, presents per frame 1, validation errors %llu\n", name,
           SerialFrames + 2, static_cast<unsigned long long>(validationErrors));

    for (sample::Renderer& renderer : renderers) {
        renderer.Shutdown();
    }

    device.Retire(bench.swapChain);
    device.Retire(bench.red);
    device.Retire(bench.capture);
    pool.Clear();
    Check(device.WaitIdle().IsOk(), "wait idle");
    return failures;
}

} // namespace sandbox
