#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG3 병렬 기록 시험이다(Docs/Design/RenderBase.ko.md 8절 "RG3 병렬 기록 설계"): worker에서 기록한
// 기록 목록을 소유 스레드가 선언 순서로 재생하면 장치 호출 열(자원 생성 제외)이 직렬 실행과 같다.
// 순서는 ParallelFor join과 재생 순서로만 정해진다(시간에 기대지 않는다).
using namespace rocks;
using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;

namespace {

TraceScopeName RasterName("Parallel.Raster");
TraceScopeName ComputeName("Parallel.Compute");
TraceScopeName CopyName("Parallel.Copy");
TraceScopeName UnusedName("Parallel.Unused");
TraceScopeName GroupName("Parallel.Group");

// 시험 그래프의 Raster pass 수다(pass마다 그리는 정점 수가 달라 순서가 호출 열에 드러난다).
constexpr uint32 RasterPasses = 4;
// 시험에 쓰는 worker 수다.
constexpr uint32 Workers = 3;
// 버퍼 크기(byte)다.
constexpr uint64 BufferBytes = 64;

// 자원 생성은 병렬 기록이 앞당기므로 대조에서 뺀다.
bool IsCreation(RecordingDevice::Op op) noexcept {
    return op == RecordingDevice::Op::CreateTexture || op == RecordingDevice::Op::CreateBuffer ||
           op == RecordingDevice::Op::CreateTextureView;
}

// 사슬 그래프: Raster 0..3(앞 결과를 읽어 다음에 그림) → Compute(버퍼 UAV) → Copy(버퍼 복사) →
// 출력. culled pass 하나와 구간 하나를 둔다.
void BuildGraph(RenderGraph& graph) noexcept {
    GraphTexture previous;
    GraphScope group(graph, GroupName);
    for (uint32 index = 0; index < RasterPasses; ++index) {
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        if (!target) {
            return;
        }

        GraphPassAccesses accesses(graph);
        if (!previous.IsNull()) {
            accesses.Texture(previous, ResourceAccess::SrvGraphics);
        }

        accesses.RenderTarget(target.Value());
        const GraphTexture read = previous;
        const bool last = index + 1 == RasterPasses;
        auto added = graph.AddPass(RasterName, GraphPassKind::Raster,
                                   last ? GraphPassFlags::HasSideEffects : GraphPassFlags::None,
                                   accesses, [read, index](GraphPassContext& pass) noexcept {
                                       pass.Commands().PushMarker(StringView("Inside"));
                                       pass.Commands().SetTexture(pass.Texture(read));
                                       pass.Commands().Draw(3 + index);
                                       pass.Commands().PopMarker();
                                   });
        (void)added;
        previous = target.Value();
    }

    graphics::BufferDesc bufferDesc;
    bufferDesc.size = BufferBytes;
    bufferDesc.usage = graphics::BufferUsage::ShaderRead | graphics::BufferUsage::ShaderWrite |
                       graphics::BufferUsage::Raw;
    auto source = graph.CreateBuffer(bufferDesc, "Source");
    auto destination = graph.CreateBuffer(bufferDesc, "Destination");
    if (!source || !destination) {
        return;
    }

    GraphPassAccesses compute(graph);
    compute.Buffer(source.Value(), ResourceAccess::UavCompute);
    (void)graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::None, compute,
                        [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(2, 1, 1); });
    GraphPassAccesses copy(graph);
    copy.Buffer(source.Value(), ResourceAccess::CopySource)
        .Buffer(destination.Value(), ResourceAccess::CopyDestination);
    const GraphBuffer from = source.Value();
    const GraphBuffer to = destination.Value();
    (void)graph.AddPass(CopyName, GraphPassKind::Copy, GraphPassFlags::HasSideEffects, copy,
                        [from, to](GraphPassContext& pass) noexcept {
                            (void)pass.Commands().CopyBuffer(pass.Buffer(to), 0, pass.Buffer(from),
                                                             0, BufferBytes);
                        });
    // 아무도 읽지 않아 제거된다(기록하지 않는다).
    auto unused = graph.CreateTexture(TargetDesc(), "Unused");
    if (unused) {
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(unused.Value());
        (void)graph.AddPass(UnusedName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                            [](GraphPassContext& pass) noexcept { pass.Commands().Draw(99); });
    }
}

struct RunResult {
    uint32 parallelPasses = 0;
    uint32 validationErrors = 0;
    bool executed = false;
};

RunResult Run(GraphBench& bench, GpuTrace& trace, WorkerScheduler* scheduler) noexcept {
    RunResult result;
    RenderGraphDesc desc = bench.Next(GraphMode::Deferred, &trace);
    desc.scheduler = scheduler;
    desc.scratch = &bench.allocator;
    RenderGraph graph(desc);
    BuildGraph(graph);
    result.executed = graph.Execute().IsOk();
    result.parallelPasses = graph.Stats().parallelPasses;
    result.validationErrors = graph.Stats().validationErrors;
    return result;
}

} // namespace

// 병렬 기록과 직렬 실행의 장치 호출 열(자원 생성 제외)과 전이 기록이 같다.
TEST(RenderGraph_ParallelRecordingMatchesSerialCalls) {
    GpuTraceSettings settings;
    settings.markers = true;
    GraphBench serial;
    GpuTrace serialTrace(serial.allocator, settings);
    const RunResult serialRun = Run(serial, serialTrace, nullptr);
    REQUIRE(serialRun.executed);
    CHECK(serialRun.parallelPasses == 0);

    GraphBench parallel;
    GpuTrace parallelTrace(parallel.allocator, settings);
    WorkerScheduler scheduler(parallel.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = Workers;
    REQUIRE(scheduler.Start(config));
    const RunResult parallelRun = Run(parallel, parallelTrace, &scheduler);
    REQUIRE(scheduler.Shutdown());
    REQUIRE(parallelRun.executed);
    // Raster 4 + Compute + Copy를 기록했다(culled pass는 기록하지 않는다).
    CHECK(parallelRun.parallelPasses == RasterPasses + 2);
    CHECK(parallelRun.validationErrors == 0);

    Span<const RecordingDevice::Call> left = serial.device.Calls();
    Span<const RecordingDevice::Call> right = parallel.device.Calls();
    usize leftIndex = 0;
    usize rightIndex = 0;
    bool same = true;
    usize compared = 0;
    while (same) {
        while (leftIndex < left.Size() && IsCreation(left[leftIndex].op)) {
            ++leftIndex;
        }

        while (rightIndex < right.Size() && IsCreation(right[rightIndex].op)) {
            ++rightIndex;
        }

        if (leftIndex == left.Size() || rightIndex == right.Size()) {
            same = leftIndex == left.Size() && rightIndex == right.Size();
            break;
        }

        same = left[leftIndex] == right[rightIndex];
        ++leftIndex;
        ++rightIndex;
        ++compared;
    }

    CHECK(same);
    CHECK(compared > RasterPasses * 4);
    CHECK(serial.device.TransitionLog().Size() == parallel.device.TransitionLog().Size());
    bool transitionsSame =
        serial.device.TransitionLog().Size() == parallel.device.TransitionLog().Size();
    for (usize index = 0; transitionsSame && index < serial.device.TransitionLog().Size();
         ++index) {
        const auto& a = serial.device.TransitionLog()[index];
        const auto& b = parallel.device.TransitionLog()[index];
        transitionsSame = a.texture == b.texture && a.range == b.range && a.before == b.before &&
                          a.after == b.after && a.phase == b.phase;
    }

    CHECK(transitionsSame);
    CHECK(serial.device.ValidationErrorCount() == 0);
    CHECK(parallel.device.ValidationErrorCount() == 0);
}

// 기록 목록에서 쓸 수 없는 호출(WriteTimestamp)은 그래프 실행 오류이고, 재생 때 실패한 호출
// (파이프라인 없는 SetDrawConstants)도 그 pass의 실행 오류다.
TEST(RenderGraph_ParallelRecordingReportsRecordAndReplayFailures) {
    GraphBench bench;
    WorkerScheduler scheduler(bench.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = Workers;
    REQUIRE(scheduler.Start(config));
    const auto runWith = [&](bool timestamp) noexcept {
        RenderGraphDesc desc = bench.Next();
        desc.scheduler = &scheduler;
        desc.scratch = &bench.allocator;
        RenderGraph graph(desc);
        for (uint32 index = 0; index < 2; ++index) {
            auto target = graph.CreateTexture(TargetDesc(), "Target");
            if (!target) {
                return ErrorCode::OutOfMemory;
            }

            GraphPassAccesses accesses(graph);
            accesses.RenderTarget(target.Value());
            (void)graph.AddPass(RasterName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                                accesses, [timestamp](GraphPassContext& pass) noexcept {
                                    if (timestamp) {
                                        (void)pass.Commands().WriteTimestamp();
                                    } else {
                                        const uint8 bytes[16] = {};
                                        (void)pass.Commands().SetDrawConstants(
                                            Span<const uint8>(bytes, sizeof(bytes)));
                                    }
                                });
        }

        const ErrorCode error = graph.Execute().Error();
        if (error == ErrorCode::Unsupported) {
            return ContainsText(graph.LastError(), "parallel pass recording failed")
                       ? error
                       : ErrorCode::None;
        }

        return ContainsText(graph.LastError(), "failed on replay") ? error : ErrorCode::None;
    };
    CHECK(runWith(true) == ErrorCode::Unsupported);
    CHECK(runWith(false) == ErrorCode::InvalidArgument);
    REQUIRE(scheduler.Shutdown());
}

// scheduler가 없거나 parallelRecording이 꺼져 있으면 직렬이다.
TEST(RenderGraph_ParallelRecordingNeedsASchedulerAndTheSwitch) {
    GraphBench bench;
    GpuTraceSettings settings;
    GpuTrace trace(bench.allocator, settings);
    WorkerScheduler scheduler(bench.allocator);
    REQUIRE(scheduler.Start({}));
    {
        RenderGraphDesc desc = bench.Next(GraphMode::Deferred, &trace);
        desc.scheduler = &scheduler;
        desc.scratch = &bench.allocator;
        desc.parallelRecording = false;
        RenderGraph graph(desc);
        BuildGraph(graph);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().parallelPasses == 0);
    }

    {
        RenderGraphDesc desc = bench.Next(GraphMode::Immediate, &trace);
        desc.scheduler = &scheduler;
        desc.scratch = &bench.allocator;
        RenderGraph graph(desc);
        BuildGraph(graph);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().parallelPasses == 0);
    }

    REQUIRE(scheduler.Shutdown());
}
