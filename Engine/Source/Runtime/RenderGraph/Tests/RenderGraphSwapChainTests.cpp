#include "GraphTestSupport.h"

#include <Task/WorkerScheduler.h>
#include <TestSupport/Test.h>

// 스왑체인 공유 시험이다(Docs/Design/RenderBase 8절 "스왑체인 공유 설계"). 한 그래프에서 같은
// 스왑체인을 대상으로 하는 pass들이 백버퍼 하나를 함께 쓰고(뒤 pass는 load), 그 스왑체인의 마지막
// pass 기록에서만 present하는지를 기록 장치의 호출 열(BeginFrame의 load·present 표시, Present)로
// 결정적으로 단언한다.
using namespace rocks;
namespace render_graph_swap_chain_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::LoadAction;
using graphics::ResourceAccess;
using Op = RecordingDevice::Op;

namespace {

TraceScopeName SceneName("SwapChain.Scene");
TraceScopeName TonemapName("SwapChain.Tonemap");
TraceScopeName CopyName("SwapChain.Copy");
TraceScopeName OverlayName("SwapChain.Overlay");
TraceScopeName ComputeName("SwapChain.Compute");

// 시험 버퍼 크기(byte)다.
constexpr uint64 BufferBytes = 64;
// 병렬 기록 시험의 worker 수다.
constexpr uint32 Workers = 3;
// pass마다 그리는 정점 수다(호출 열에서 pass를 알아본다).
constexpr uint32 SceneVertices = 3;
constexpr uint32 TonemapVertices = 4;
constexpr uint32 OverlayVertices = 5;
// 기록 장치 Op::Draw value에서 정점 수의 bit 위치다.
constexpr uint32 DrawVertexShift = 40;

auto DrawCount(uint32 vertices) noexcept {
    return [vertices](GraphPassContext& pass) noexcept { pass.Commands().Draw(vertices); };
}

graphics::SwapChainHandle CreateSwapChain(GraphBench& bench) noexcept {
    graphics::SwapChainDesc desc;
    desc.width = 4;
    desc.height = 4;
    auto created = bench.device.CreateSwapChain(desc);
    if (!created) {
        platform::Fatal("render graph test swap chain creation failed");
    }

    return created.Value();
}

// nth번째(0부터) BeginFrame의 value다(없으면 NumericLimits<uint64>::Max()).
uint64 BeginFrameValue(const GraphBench& bench, usize nth) noexcept {
    const usize index = bench.IndexOf(Op::BeginFrame, nth);
    return index < bench.device.Calls().Size() ? bench.device.Calls()[index].value
                                               : NumericLimits<uint64>::Max();
}

// 장면(그래프 텍스처) → tone map(스왑체인 clear) → 복사(그래프 텍스처 사이) → overlay(스왑체인
// load). 실행 순서가 선언 순서이므로 tone map과 overlay 사이에 Copy pass가 끼어 있다.
bool BuildSharedGraph(RenderGraph& graph, graphics::SwapChainHandle swapChain) noexcept {
    auto scene = graph.CreateTexture(TargetDesc(), "Scene");
    auto copied = graph.CreateTexture(TargetDesc(), "Copied");
    if (!scene || !copied) {
        return false;
    }

    GraphPassAccesses sceneAccesses(graph);
    sceneAccesses.RenderTarget(scene.Value());
    GraphPassAccesses tonemap(graph);
    tonemap.Texture(scene.Value(), ResourceAccess::SrvGraphics)
        .SwapChainTarget(swapChain, GraphLoadAction::Clear, {0.1f, 0.2f, 0.3f, 1.0f});
    GraphPassAccesses copy(graph);
    copy.Texture(scene.Value(), ResourceAccess::CopySource)
        .Texture(copied.Value(), ResourceAccess::CopyDestination);
    GraphPassAccesses overlay(graph);
    overlay.Texture(copied.Value(), ResourceAccess::SrvGraphics)
        .SwapChainTarget(swapChain, GraphLoadAction::Load);
    return graph
               .AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, sceneAccesses,
                        DrawCount(SceneVertices))
               .IsOk() &&
           graph
               .AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, tonemap,
                        DrawCount(TonemapVertices))
               .IsOk() &&
           graph
               .AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::None, copy,
                        [](GraphPassContext& pass) noexcept {
                            pass.Commands().PushMarker(StringView("Copy"));
                            pass.Commands().PopMarker();
                        })
               .IsOk() &&
           graph
               .AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, overlay,
                        DrawCount(OverlayVertices))
               .IsOk();
}

// 자원 생성은 병렬 기록이 앞당기므로 호출 열 대조에서 뺀다.
bool IsCreation(Op op) noexcept {
    return op == Op::CreateTexture || op == Op::CreateBuffer || op == Op::CreateTextureView;
}

bool SameCallsWithoutCreation(const RecordingDevice& left, const RecordingDevice& right) noexcept {
    Span<const RecordingDevice::Call> a = left.Calls();
    Span<const RecordingDevice::Call> b = right.Calls();
    usize leftIndex = 0;
    usize rightIndex = 0;
    while (true) {
        while (leftIndex < a.Size() && IsCreation(a[leftIndex].op)) {
            ++leftIndex;
        }

        while (rightIndex < b.Size() && IsCreation(b[rightIndex].op)) {
            ++rightIndex;
        }

        if (leftIndex == a.Size() || rightIndex == b.Size()) {
            return leftIndex == a.Size() && rightIndex == b.Size();
        }

        if (!(a[leftIndex] == b[rightIndex])) {
            return false;
        }

        ++leftIndex;
        ++rightIndex;
    }
}

} // namespace

// 같은 스왑체인 pass 둘(사이에 Copy pass)이 백버퍼 하나를 쓴다: 앞 기록은 clear·present 없음, 뒤
// 기록은 load·present이고 present는 마지막 기록의 EndFrame 바로 뒤 한 번이다. Copy pass는 앞
// 스왑체인 기록에 이어 쓴다.
TEST(RenderGraph_SwapChainPassesShareOneBackBuffer) {
    GraphBench bench;
    const graphics::SwapChainHandle swapChain = CreateSwapChain(bench);
    RenderGraph graph(bench.Next());
    REQUIRE(BuildSharedGraph(graph, swapChain));
    REQUIRE(graph.Execute());

    CHECK(bench.Count(Op::BeginFrame) == 2);
    CHECK(BeginFrameValue(bench, 0) ==
          RecordingDevice::SwapChainPassValue(LoadAction::Clear, false));
    CHECK(BeginFrameValue(bench, 1) == RecordingDevice::SwapChainPassValue(LoadAction::Load, true));
    CHECK(bench.Count(Op::Present) == 1);
    CHECK(bench.device.PresentCount() == 1);
    CHECK(graph.Stats().presents == 1);
    CHECK(graph.Stats().recordings == 3);

    Span<const RecordingDevice::Call> calls = bench.device.Calls();
    const usize present = bench.IndexOf(Op::Present);
    REQUIRE(present < calls.Size());
    // present는 마지막 기록(overlay)의 EndFrame 바로 뒤이고 그 뒤에 기록이 없다.
    CHECK(calls[present - 1].op == Op::EndFrame);
    CHECK(present + 1 == calls.Size());
    CHECK(calls[present].value == swapChain.slot.index);
    // Copy pass marker는 tone map 기록(첫 BeginFrame과 그 뒤 첫 EndFrame 사이) 안이다.
    const usize tonemapBegin = bench.IndexOf(Op::BeginFrame, 0);
    const usize overlayBegin = bench.IndexOf(Op::BeginFrame, 1);
    usize copyMarker = NumericLimits<usize>::Max();
    usize tonemapEnd = NumericLimits<usize>::Max();
    for (usize index = tonemapBegin; index < overlayBegin; ++index) {
        if (calls[index].op == Op::PushMarker && copyMarker == NumericLimits<usize>::Max()) {
            copyMarker = index;
        }

        if (calls[index].op == Op::EndFrame) {
            tonemapEnd = index;
        }
    }

    CHECK(copyMarker < tonemapEnd);
    CHECK(tonemapEnd + 1 == overlayBegin);
    // overlay의 그리기는 load·present 기록 안이다.
    bool overlayDrawn = false;
    for (usize index = overlayBegin; index < present; ++index) {
        overlayDrawn = overlayDrawn || (calls[index].op == Op::Draw &&
                                        (calls[index].value >> DrawVertexShift) == OverlayVertices);
    }

    CHECK(overlayDrawn);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 스왑체인 둘은 각자의 마지막 pass에서 present한다(A1 → B1 → A2: B는 B1 기록, A는 A2 기록).
TEST(RenderGraph_SwapChainsPresentAtTheirOwnLastPass) {
    GraphBench bench;
    const graphics::SwapChainHandle first = CreateSwapChain(bench);
    const graphics::SwapChainHandle second = CreateSwapChain(bench);
    REQUIRE(!(first == second));
    RenderGraph graph(bench.Next());
    GraphPassAccesses a1(graph);
    a1.SwapChainTarget(first);
    GraphPassAccesses b1(graph);
    b1.SwapChainTarget(second, GraphLoadAction::DontCare);
    GraphPassAccesses a2(graph);
    a2.SwapChainTarget(first, GraphLoadAction::Load);
    REQUIRE(graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, a1,
                          DrawCount(TonemapVertices)));
    REQUIRE(graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, b1,
                          DrawCount(TonemapVertices)));
    REQUIRE(graph.AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, a2,
                          DrawCount(OverlayVertices)));
    REQUIRE(graph.Execute());

    CHECK(bench.Count(Op::BeginFrame) == 3);
    CHECK(BeginFrameValue(bench, 0) ==
          RecordingDevice::SwapChainPassValue(LoadAction::Clear, false));
    CHECK(BeginFrameValue(bench, 1) ==
          RecordingDevice::SwapChainPassValue(LoadAction::DontCare, true));
    CHECK(BeginFrameValue(bench, 2) == RecordingDevice::SwapChainPassValue(LoadAction::Load, true));
    REQUIRE(bench.Count(Op::Present) == 2);
    Span<const RecordingDevice::Call> calls = bench.device.Calls();
    CHECK(calls[bench.IndexOf(Op::Present, 0)].value == second.slot.index);
    CHECK(calls[bench.IndexOf(Op::Present, 1)].value == first.slot.index);
    CHECK(bench.IndexOf(Op::Present, 0) < bench.IndexOf(Op::BeginFrame, 2));
    CHECK(graph.Stats().presents == 2);
    CHECK(bench.device.PresentCount() == 2);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 그래프에서 그 스왑체인의 첫 pass가 Load면 검증 오류다(다른 스왑체인을 앞에서 그렸어도 같다).
// 그래프는 실행되지 않아 기록·present가 없다.
TEST(RenderGraph_SwapChainLoadNeedsAnEarlierPassOfTheGraph) {
    GraphBench bench;
    const graphics::SwapChainHandle first = CreateSwapChain(bench);
    const graphics::SwapChainHandle second = CreateSwapChain(bench);
    {
        RenderGraph graph(bench.Next());
        GraphPassAccesses load(graph);
        load.SwapChainTarget(first, GraphLoadAction::Load);
        auto added = graph.AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, load,
                                   DrawCount(OverlayVertices));
        CHECK(added.Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "loads a swap chain back buffer"));
        CHECK(graph.Stats().validationErrors == 1);
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    {
        RenderGraph graph(bench.Next());
        GraphPassAccesses drawFirst(graph);
        drawFirst.SwapChainTarget(first);
        GraphPassAccesses loadSecond(graph);
        loadSecond.SwapChainTarget(second, GraphLoadAction::Load);
        REQUIRE(graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, drawFirst,
                              DrawCount(TonemapVertices)));
        CHECK(graph
                  .AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, loadSecond,
                           DrawCount(OverlayVertices))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    CHECK(bench.Count(Op::BeginFrame) == 0);
    CHECK(bench.Count(Op::Present) == 0);
    CHECK(bench.device.PresentCount() == 0);
}

// 첫 BeginFrame(백버퍼 acquire) 실패: 그래프 진단이 남고 present가 없다. 둘째 BeginFrame 실패:
// 앞 기록은 present 없이 제출되고 이 그래프의 present가 없다. 다음 그래프는 clear로 새 프레임을
// 그려 present한다.
TEST(RenderGraph_SwapChainBeginFailureSkipsThePresent) {
    {
        GraphBench bench;
        const graphics::SwapChainHandle swapChain = CreateSwapChain(bench);
        bench.device.SetBeginFrameFailure(0, ErrorCode::DependencyFailed);
        RenderGraph graph(bench.Next());
        REQUIRE(BuildSharedGraph(graph, swapChain));
        CHECK(graph.Execute().Error() == ErrorCode::DependencyFailed);
        CHECK(ContainsText(graph.LastError(), "could not begin its render target"));
        CHECK(bench.Count(Op::BeginFrame) == 0);
        CHECK(bench.Count(Op::Present) == 0);
        CHECK(graph.Stats().presents == 0);
    }

    GraphBench bench;
    const graphics::SwapChainHandle swapChain = CreateSwapChain(bench);
    bench.device.SetBeginFrameFailure(1, ErrorCode::Destroyed);
    {
        RenderGraph graph(bench.Next());
        REQUIRE(BuildSharedGraph(graph, swapChain));
        CHECK(graph.Execute().Error() == ErrorCode::Destroyed);
        CHECK(ContainsText(graph.LastError(), "SwapChain.Overlay"));
        CHECK(bench.Count(Op::BeginFrame) == 1);
        CHECK(BeginFrameValue(bench, 0) ==
              RecordingDevice::SwapChainPassValue(LoadAction::Clear, false));
        CHECK(bench.Count(Op::Present) == 0);
        CHECK(graph.Stats().presents == 0);
    }

    RenderGraph next(bench.Next());
    REQUIRE(BuildSharedGraph(next, swapChain));
    REQUIRE(next.Execute());
    CHECK(bench.Count(Op::Present) == 1);
    CHECK(bench.device.PresentCount() == 1);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 장치 규칙: Load는 그리는 중인 프레임(present하지 않은 기록 뒤)에만 받는다. present 뒤·처음·크기
// 변경 뒤의 Load는 InvalidArgument다(그래프 밖에서 present한 스왑체인에 이어 쓰기를 거부한다).
TEST(RenderGraph_SwapChainDeviceRejectsLoadAfterPresent) {
    GraphBench bench;
    const graphics::SwapChainHandle swapChain = CreateSwapChain(bench);
    RecordingDevice& device = bench.device;
    graphics::SwapChainPassDesc load;
    load.swapChain = swapChain;
    load.load = LoadAction::Load;
    CHECK(device.BeginFrame(load).Error() == ErrorCode::InvalidArgument);

    graphics::SwapChainPassDesc open;
    open.swapChain = swapChain;
    open.present = false;
    REQUIRE(device.BeginFrame(open));
    REQUIRE(device.EndFrame());
    CHECK(device.PresentCount() == 0);
    REQUIRE(device.BeginFrame(load));
    REQUIRE(device.EndFrame());
    CHECK(device.PresentCount() == 1);
    CHECK(device.BeginFrame(load).Error() == ErrorCode::InvalidArgument);

    // 간단한 형태는 clear + present다.
    REQUIRE(device.BeginFrame(swapChain, {}));
    REQUIRE(device.EndFrame());
    CHECK(device.PresentCount() == 2);

    REQUIRE(device.BeginFrame(open));
    REQUIRE(device.EndFrame());
    REQUIRE(device.ResizeSwapChain(swapChain, 8, 8));
    CHECK(device.BeginFrame(load).Error() == ErrorCode::InvalidArgument);
    CHECK(device.PresentCount() == 2);
}

// 병렬 기록(RG3)과 직렬 실행의 장치 호출 열(present 표시·Present 포함, 자원 생성 제외)이 같다.
TEST(RenderGraph_SwapChainParallelRecordingMatchesSerial) {
    GraphBench serial;
    const graphics::SwapChainHandle serialSwapChain = CreateSwapChain(serial);
    {
        RenderGraph graph(serial.Next());
        REQUIRE(BuildSharedGraph(graph, serialSwapChain));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().parallelPasses == 0);
    }

    GraphBench parallel;
    const graphics::SwapChainHandle parallelSwapChain = CreateSwapChain(parallel);
    WorkerScheduler scheduler(parallel.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = Workers;
    REQUIRE(scheduler.Start(config));
    {
        RenderGraphDesc desc = parallel.Next();
        desc.scheduler = &scheduler;
        desc.scratch = &parallel.allocator;
        RenderGraph graph(desc);
        REQUIRE(BuildSharedGraph(graph, parallelSwapChain));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().parallelPasses == 4);
        CHECK(graph.Stats().presents == 1);
    }

    REQUIRE(scheduler.Shutdown());
    CHECK(SameCallsWithoutCreation(serial.device, parallel.device));
    CHECK(parallel.Count(Op::Present) == 1);
    CHECK(parallel.device.ValidationErrorCount() == 0);
}

// Immediate 모드는 뒤 pass를 모르므로 스왑체인 pass를 모두 present 없이 열고, Execute 끝에 그리기
// 없는 present 기록(load) 하나를 낸다. 실패한 그래프는 present하지 않는다.
TEST(RenderGraph_SwapChainImmediateModePresentsAtExecute) {
    GraphBench bench;
    const graphics::SwapChainHandle swapChain = CreateSwapChain(bench);
    {
        RenderGraph graph(bench.Next(GraphMode::Immediate));
        REQUIRE(BuildSharedGraph(graph, swapChain));
        CHECK(bench.Count(Op::Present) == 0);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().presents == 1);
    }

    REQUIRE(bench.Count(Op::BeginFrame) == 3);
    CHECK(BeginFrameValue(bench, 0) ==
          RecordingDevice::SwapChainPassValue(LoadAction::Clear, false));
    CHECK(BeginFrameValue(bench, 1) ==
          RecordingDevice::SwapChainPassValue(LoadAction::Load, false));
    CHECK(BeginFrameValue(bench, 2) == RecordingDevice::SwapChainPassValue(LoadAction::Load, true));
    Span<const RecordingDevice::Call> calls = bench.device.Calls();
    const usize presentBegin = bench.IndexOf(Op::BeginFrame, 2);
    REQUIRE(presentBegin + 2 < calls.Size());
    CHECK(calls[presentBegin + 1].op == Op::EndFrame);
    CHECK(calls[presentBegin + 2].op == Op::Present);
    CHECK(bench.Count(Op::Present) == 1);

    // 스왑체인 pass를 실행한 뒤 검증 오류가 나면 Execute가 실패하고 present 기록이 없다.
    {
        RenderGraph graph(bench.Next(GraphMode::Immediate));
        GraphPassAccesses tonemap(graph);
        tonemap.SwapChainTarget(swapChain);
        REQUIRE(graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, tonemap,
                              DrawCount(TonemapVertices)));
        auto unproduced = graph.CreateTexture(TargetDesc(), "Unproduced");
        REQUIRE(unproduced);
        GraphPassAccesses overlay(graph);
        overlay.Texture(unproduced.Value(), ResourceAccess::SrvGraphics)
            .SwapChainTarget(swapChain, GraphLoadAction::Load);
        CHECK(graph
                  .AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, overlay,
                           DrawCount(OverlayVertices))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
        CHECK(graph.Stats().presents == 0);
    }

    CHECK(bench.Count(Op::Present) == 1);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// async compute(RG4)가 두 스왑체인 pass 사이에 끼어 기록이 compute queue로 나뉘어도 present는
// 마지막 스왑체인 pass 기록의 한 번이다.
TEST(RenderGraph_SwapChainPresentsOnceAcrossAsyncCompute) {
    GraphBench bench;
    bench.device.SetAsyncCompute(true);
    const graphics::SwapChainHandle swapChain = CreateSwapChain(bench);
    RenderGraphDesc desc = bench.Next();
    desc.asyncCompute = GraphAsyncCompute::Always;
    RenderGraph graph(desc);
    auto buffer = graph.CreateBuffer(BufferOf(BufferBytes), "Values");
    REQUIRE(buffer);
    GraphPassAccesses tonemap(graph);
    tonemap.SwapChainTarget(swapChain);
    GraphPassAccesses compute(graph);
    compute.Buffer(buffer.Value(), ResourceAccess::UavCompute);
    GraphPassAccesses overlay(graph);
    overlay.Buffer(buffer.Value(), ResourceAccess::SrvGraphics)
        .SwapChainTarget(swapChain, GraphLoadAction::Load);
    REQUIRE(graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, tonemap,
                          DrawCount(TonemapVertices)));
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::AsyncCompute, GraphPassFlags::None, compute,
                          [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(1); }));
    REQUIRE(graph.AddPass(OverlayName, GraphPassKind::Raster, GraphPassFlags::None, overlay,
                          DrawCount(OverlayVertices)));
    REQUIRE(graph.Execute());

    CHECK(graph.Stats().asyncComputePasses == 1);
    CHECK(bench.Count(Op::BeginCompute) == 1);
    CHECK(bench.Count(Op::BeginFrame) == 2);
    CHECK(BeginFrameValue(bench, 0) ==
          RecordingDevice::SwapChainPassValue(LoadAction::Clear, false));
    CHECK(BeginFrameValue(bench, 1) == RecordingDevice::SwapChainPassValue(LoadAction::Load, true));
    CHECK(bench.Count(Op::Present) == 1);
    CHECK(graph.Stats().presents == 1);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

} // namespace render_graph_swap_chain_tests_file
