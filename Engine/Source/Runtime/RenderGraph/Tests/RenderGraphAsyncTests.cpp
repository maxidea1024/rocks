#include "GraphTestSupport.h"

#include <RenderBase/GpuTrace.h>
#include <TestSupport/Test.h>
#include <TraceCapture/TraceSession.h>

// RG4 async compute 시험이다(Docs/Design/RenderBase.ko.md 8절 "RG4 async compute 설계"). 기록
// 장치에 compute queue를 공개하고 fork·join 구조(QueueWait 열, compute 기록, fork 전이 자리, 넓힌
// 수명)를 결정적으로 단언한다. 기록 장치는 queue 사이 경쟁(기다리지 않은 다른 queue의 앞 접근)과
// compute 기록의 graphics 상태 전이를 검증 오류로 센다.
using namespace rocks;
namespace render_graph_async_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::QueueKind;
using graphics::ResourceAccess;
using Op = RecordingDevice::Op;

namespace {

TraceScopeName RasterName("Async.Raster");
TraceScopeName ProduceName("Async.Produce");
TraceScopeName ConsumeName("Async.Consume");
TraceScopeName OverlapName("Async.Overlap");
TraceScopeName JoinName("Async.Join");
TraceScopeName AfterName("Async.After");

// 시험 버퍼 크기(byte)다.
constexpr uint64 BufferBytes = 64;

// 그래프 A의 pass 번호다(선언 순서).
constexpr uint32 ForkPass = 0;
constexpr uint32 BetweenPass = 1;
constexpr uint32 FirstAsyncPass = 2;
constexpr uint32 SecondAsyncPass = 3;
constexpr uint32 OverlapPass = 4;
constexpr uint32 JoinPass = 5;

struct GraphA {
    GraphTexture produced;
    GraphBuffer first;
    GraphBuffer second;
    GraphPass passes[6];
};

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

// 그래프 A: G0(T를 그림) → G1(무관) → A0(T 읽기, B 쓰기) → A1(B 읽기, C 쓰기) → G2(무관, A와 겹침)
// → G3(C를 읽어 그림, join). asyncKind가 Compute면 같은 그래프를 graphics pipe로 짓는다.
GraphA BuildGraphA(RenderGraph& graph, GraphPassKind asyncKind) noexcept {
    GraphA built;
    auto produced = graph.CreateTexture(TargetDesc(), "Produced");
    auto between = graph.CreateTexture(TargetDesc(), "Between");
    auto overlap = graph.CreateTexture(TargetDesc(), "Overlap");
    auto joined = graph.CreateTexture(TargetDesc(), "Joined");
    auto first = graph.CreateBuffer(BufferOf(BufferBytes), "First");
    auto second = graph.CreateBuffer(BufferOf(BufferBytes), "Second");
    if (!produced || !between || !overlap || !joined || !first || !second) {
        return built;
    }

    built.produced = produced.Value();
    built.first = first.Value();
    built.second = second.Value();
    GraphPassAccesses g0(graph);
    g0.RenderTarget(produced.Value());
    built.passes[ForkPass] =
        graph.AddPass(RasterName, GraphPassKind::Raster, GraphPassFlags::None, g0, Draw()).Value();
    GraphPassAccesses g1(graph);
    g1.RenderTarget(between.Value());
    built.passes[BetweenPass] =
        graph.AddPass(RasterName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, g1, Draw())
            .Value();
    GraphPassAccesses a0(graph);
    a0.Texture(produced.Value(), ResourceAccess::SrvCompute)
        .Buffer(first.Value(), ResourceAccess::UavCompute);
    built.passes[FirstAsyncPass] =
        graph
            .AddPass(ProduceName, asyncKind, GraphPassFlags::None, a0,
                     [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(1); })
            .Value();
    GraphPassAccesses a1(graph);
    a1.Buffer(first.Value(), ResourceAccess::SrvCompute)
        .Buffer(second.Value(), ResourceAccess::UavCompute);
    built.passes[SecondAsyncPass] =
        graph
            .AddPass(ConsumeName, asyncKind, GraphPassFlags::None, a1,
                     [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(2); })
            .Value();
    GraphPassAccesses g2(graph);
    g2.RenderTarget(overlap.Value());
    built.passes[OverlapPass] =
        graph
            .AddPass(OverlapName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, g2, Draw())
            .Value();
    GraphPassAccesses g3(graph);
    g3.Buffer(second.Value(), ResourceAccess::SrvGraphics).RenderTarget(joined.Value());
    built.passes[JoinPass] =
        graph.AddPass(JoinName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, g3, Draw())
            .Value();
    return built;
}

// 기록한 장치 호출 열(op만)이다.
struct OpList {
    static constexpr usize Capacity = RecordingDevice::MaxCalls;
    Op ops[Capacity] = {};
    usize count = 0;
};

void CopyOps(const RecordingDevice& device, OpList& out) noexcept {
    out.count = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        out.ops[out.count++] = call.op;
    }
}

bool SameOps(const OpList& left, const OpList& right) noexcept {
    if (left.count != right.count) {
        return false;
    }

    for (usize index = 0; index < left.count; ++index) {
        if (left.ops[index] != right.ops[index]) {
            return false;
        }
    }

    return true;
}

// [begin, end) 안의 장치 전이 가운데 after가 access인 것이 있는지다.
bool HasTransitionBetween(const RecordingDevice& device, usize begin, usize end,
                          ResourceAccess after) noexcept {
    Span<const RecordingDevice::Call> calls = device.Calls();
    for (usize index = begin; index < end && index < calls.Size(); ++index) {
        if (calls[index].op == Op::Transition &&
            device.TransitionLog()[static_cast<usize>(calls[index].value)].after == after) {
            return true;
        }
    }

    return false;
}

} // namespace

// fork·join 구조: fork pass 뒤 기록을 닫고 compute queue가 그 지점을 기다리며, 이어진 async pass는
// compute 기록 하나, join pass는 새 기록 앞에서 compute 지점을 기다린다. fork 전이는 fork pass의
// graphics 기록에, async → async 전이는 compute 기록에, async → graphics 전이는 join pass에 있다.
TEST(RenderGraph_AsyncComputeForksAndJoinsAcrossQueues) {
    GraphBench bench;
    bench.device.SetAsyncCompute(true);
    RenderGraphDesc desc = bench.Next();
    desc.asyncCompute = GraphAsyncCompute::Always;
    RenderGraph graph(desc);
    const GraphA built = BuildGraphA(graph, GraphPassKind::AsyncCompute);
    REQUIRE(!built.first.IsNull());
    REQUIRE(graph.Execute());

    CHECK(graph.PassPipe(built.passes[ForkPass]) == GraphPassPipe::Graphics);
    CHECK(graph.PassPipe(built.passes[FirstAsyncPass]) == GraphPassPipe::AsyncCompute);
    CHECK(graph.PassPipe(built.passes[SecondAsyncPass]) == GraphPassPipe::AsyncCompute);
    CHECK(graph.PassPipe(built.passes[JoinPass]) == GraphPassPipe::Graphics);
    const GraphStats stats = graph.Stats();
    CHECK(stats.asyncComputePasses == 2);
    CHECK(stats.computeRecordings == 1);
    CHECK(stats.queueWaits == 2);
    CHECK(bench.device.ValidationErrorCount() == 0);

    // 장치 열: G0 기록(fork 전이 포함) 제출 → G1 기록 제출 → compute가 G0 지점을 기다림 → compute
    // 기록 → G2 기록 → graphics가 compute 지점을 기다림 → G3 기록.
    CHECK(bench.Count(Op::BeginCompute) == 1);
    CHECK(bench.Count(Op::EndCompute) == 1);
    CHECK(bench.Count(Op::QueueWait) == 2);
    const usize forkBegin = bench.IndexOf(Op::BeginOffscreen, 0);
    const usize forkEnd = bench.IndexOf(Op::EndFrame, 0);
    const usize betweenEnd = bench.IndexOf(Op::EndFrame, 1);
    const usize forkWait = bench.IndexOf(Op::QueueWait, 0);
    const usize computeBegin = bench.IndexOf(Op::BeginCompute);
    const usize computeEnd = bench.IndexOf(Op::EndCompute);
    const usize overlapEnd = bench.IndexOf(Op::EndFrame, 2);
    const usize joinWait = bench.IndexOf(Op::QueueWait, 1);
    const usize joinBegin = bench.IndexOf(Op::BeginOffscreen, 3);
    Span<const RecordingDevice::Call> calls = bench.device.Calls();
    CHECK(forkBegin < forkEnd && forkEnd < betweenEnd && betweenEnd < forkWait);
    CHECK(forkWait < computeBegin && computeBegin < computeEnd && computeEnd < overlapEnd);
    CHECK(overlapEnd < joinWait && joinWait < joinBegin);
    // compute queue는 G0 기록(첫 graphics 제출, 값 1)을, graphics queue는 compute 제출 1을
    // 기다린다.
    CHECK(calls[forkEnd].value == 1);
    CHECK(calls[forkWait].value == RecordingDevice::QueueWaitValue(QueueKind::AsyncCompute, 1));
    CHECK(calls[computeEnd].value == 1);
    CHECK(calls[joinWait].value == RecordingDevice::QueueWaitValue(QueueKind::Graphics, 1));
    // fork 전이(T: RenderTarget → SrvCompute, B: 처음 → UavCompute)는 G0의 graphics 기록에 있다.
    CHECK(HasTransitionBetween(bench.device, forkBegin, forkEnd, ResourceAccess::SrvCompute));
    CHECK(HasTransitionBetween(bench.device, forkBegin, forkEnd, ResourceAccess::UavCompute));
    // async → async 전이(B: UavCompute → SrvCompute)는 compute 기록 안에 있다.
    CHECK(HasTransitionBetween(bench.device, computeBegin, computeEnd, ResourceAccess::SrvCompute));
    // async → graphics 전이(C: UavCompute → SrvGraphics)는 join 기록에 있다.
    CHECK(HasTransitionBetween(bench.device, joinBegin, calls.Size(), ResourceAccess::SrvGraphics));
    uint32 forkRecords = 0;
    for (const GraphTransitionRecord& record : graph.Transitions()) {
        if (record.forkPass != GraphIndexNone) {
            CHECK(record.forkPass == ForkPass);
            CHECK(record.pass == FirstAsyncPass || record.pass == SecondAsyncPass);
            ++forkRecords;
        }
    }

    // T(A0), B(A0), C(A1)의 첫 전이다.
    CHECK(forkRecords == 3);

    // 수명 연장: async pass 자원은 fork pass부터 join pass까지다.
    CHECK(graph.Lifetime(built.produced).firstPass == ForkPass);
    CHECK(graph.Lifetime(built.produced).lastPass == JoinPass);
    CHECK(graph.Lifetime(built.first).firstPass == ForkPass);
    CHECK(graph.Lifetime(built.first).lastPass == JoinPass);
    CHECK(graph.Lifetime(built.second).lastPass == JoinPass);
}

// 낮추기: 장치에 compute queue가 없거나, 설정이 Disabled이거나, Auto인데 장치가 효율적이지 않거나,
// 즉시 실행 모드면 AsyncCompute pass는 graphics pipe Compute pass와 같은 장치 호출 열이다.
TEST(RenderGraph_AsyncComputeLowersToGraphicsCompute) {
    OpList reference;
    {
        GraphBench bench;
        RenderGraph graph(bench.Next());
        BuildGraphA(graph, GraphPassKind::Compute);
        REQUIRE(graph.Execute());
        CopyOps(bench.device, reference);
        CHECK(bench.device.ValidationErrorCount() == 0);
    }

    struct Case {
        bool queue;
        bool efficient;
        GraphAsyncCompute setting;
    };
    const Case cases[] = {
        {false, false, GraphAsyncCompute::Always},
        {true, true, GraphAsyncCompute::Disabled},
        {true, false, GraphAsyncCompute::Auto},
    };
    for (const Case& test : cases) {
        GraphBench bench;
        bench.device.SetAsyncCompute(test.queue, test.efficient);
        RenderGraphDesc desc = bench.Next();
        desc.asyncCompute = test.setting;
        RenderGraph graph(desc);
        const GraphA built = BuildGraphA(graph, GraphPassKind::AsyncCompute);
        REQUIRE(graph.Execute());
        OpList ops;
        CopyOps(bench.device, ops);
        CHECK(SameOps(ops, reference));
        CHECK(graph.PassPipe(built.passes[FirstAsyncPass]) == GraphPassPipe::Graphics);
        CHECK(graph.Stats().asyncComputePasses == 0);
        CHECK(bench.Count(Op::QueueWait) == 0);
    }

    // 즉시 실행 모드는 늘 graphics pipe다.
    GraphBench bench;
    bench.device.SetAsyncCompute(true);
    RenderGraphDesc desc = bench.Next(GraphMode::Immediate);
    desc.asyncCompute = GraphAsyncCompute::Always;
    RenderGraph graph(desc);
    const GraphA built = BuildGraphA(graph, GraphPassKind::AsyncCompute);
    REQUIRE(graph.Execute());
    CHECK(graph.PassPipe(built.passes[FirstAsyncPass]) == GraphPassPipe::Graphics);
    CHECK(bench.Count(Op::BeginCompute) == 0);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 그래프 시작 fork와 그래프 끝 join: 기댈 graphics 생산자가 없는 async pass는 그래프 시작 기록(fork
// 전이만 담음)을 기다리고, 소비자가 없으면 그래프 끝에 graphics 기록 하나가 compute 제출을
// 기다린다. 넓힌 수명 때문에 그 구간 안의 같은 desc 자원은 async 자원의 장치 객체를 이어 쓰지
// 않는다(끄면 쓴다).
TEST(RenderGraph_AsyncComputePrologueEpilogueAndLifetimeExtension) {
    const auto build = [](RenderGraph& graph, GraphBuffer& asyncBuffer,
                          GraphBuffer& laterBuffer) noexcept {
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        auto early = graph.CreateBuffer(BufferOf(BufferBytes), "Early");
        auto later = graph.CreateBuffer(BufferOf(BufferBytes), "Later");
        if (!target || !early || !later) {
            return;
        }

        asyncBuffer = early.Value();
        laterBuffer = later.Value();
        GraphPassAccesses a0(graph);
        a0.Buffer(early.Value(), ResourceAccess::UavCompute);
        (void)graph.AddPass(ProduceName, GraphPassKind::AsyncCompute,
                            GraphPassFlags::HasSideEffects, a0,
                            [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(1); });
        GraphPassAccesses g0(graph);
        g0.RenderTarget(target.Value());
        (void)graph.AddPass(RasterName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, g0,
                            Draw());
        GraphPassAccesses g1(graph);
        g1.Buffer(later.Value(), ResourceAccess::UavCompute);
        (void)graph.AddPass(AfterName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, g1,
                            [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(1); });
    };

    {
        GraphBench bench;
        bench.device.SetAsyncCompute(true);
        RenderGraphDesc desc = bench.Next();
        desc.asyncCompute = GraphAsyncCompute::Always;
        RenderGraph graph(desc);
        GraphBuffer asyncBuffer;
        GraphBuffer laterBuffer;
        build(graph, asyncBuffer, laterBuffer);
        REQUIRE(graph.Execute());
        CHECK(bench.device.ValidationErrorCount() == 0);
        // prologue 기록(fork 전이) → compute(그 지점을 기다림) → G0·G1 기록 → 그래프 끝 join 기록.
        const usize prologueBegin = bench.IndexOf(Op::BeginOffscreen, 0);
        const usize prologueEnd = bench.IndexOf(Op::EndFrame, 0);
        const usize forkWait = bench.IndexOf(Op::QueueWait, 0);
        const usize computeBegin = bench.IndexOf(Op::BeginCompute);
        const usize joinWait = bench.IndexOf(Op::QueueWait, 1);
        const usize lastBegin = bench.IndexOf(Op::BeginOffscreen, 2);
        Span<const RecordingDevice::Call> calls = bench.device.Calls();
        CHECK(prologueBegin < prologueEnd && prologueEnd < forkWait && forkWait < computeBegin);
        CHECK(HasTransitionBetween(bench.device, prologueBegin, prologueEnd,
                                   ResourceAccess::UavCompute));
        CHECK(calls[forkWait].value ==
              RecordingDevice::QueueWaitValue(QueueKind::AsyncCompute, calls[prologueEnd].value));
        CHECK(joinWait < lastBegin && lastBegin != NumericLimits<usize>::Max());
        CHECK(calls[joinWait].value == RecordingDevice::QueueWaitValue(QueueKind::Graphics, 1));
        // 그래프의 마지막 제출은 graphics다.
        CHECK(calls[calls.Size() - 1].op == Op::EndFrame);
        // prologue·G0(G1이 이어 씀)·그래프 끝 join 기록이다.
        CHECK(bench.Count(Op::BeginOffscreen) == 3);
        CHECK(graph.Stats().queueWaits == 2);
        CHECK(!graph.Lifetime(laterBuffer).reusedWithinGraph);
        CHECK(graph.Stats().reusedWithinGraph == 0);
        bool prologueRecord = false;
        for (const GraphTransitionRecord& record : graph.Transitions()) {
            prologueRecord = prologueRecord || record.forkPass == GraphProloguePass;
        }

        CHECK(prologueRecord);
    }

    // compute queue를 끄면 같은 그래프의 뒤 버퍼가 앞 버퍼의 장치 객체를 이어 쓴다(수명 연장이 위
    // 차이를 만든다는 대조).
    GraphBench bench;
    RenderGraph graph(bench.Next());
    GraphBuffer asyncBuffer;
    GraphBuffer laterBuffer;
    build(graph, asyncBuffer, laterBuffer);
    REQUIRE(graph.Execute());
    CHECK(graph.Lifetime(laterBuffer).reusedWithinGraph);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 기록 장치의 queue 경쟁 검사 대조: graphics가 쓴 텍스처를 compute 기록이 QueueWait 없이 건드리면
// 검증 오류이고, 그 제출을 기다리면 오류가 없다. compute 기록의 graphics 상태 전이도 검증 오류다.
TEST(RenderGraph_AsyncComputeRecordingDeviceDetectsMissingWaits) {
    const bool waits[] = {false, true};
    for (const bool wait : waits) {
        GraphBench bench;
        bench.device.SetAsyncCompute(true);
        auto texture = bench.device.CreateTexture(TargetDesc(), {});
        REQUIRE(texture);
        auto commands = bench.device.BeginOffscreen(texture.Value(), {});
        REQUIRE(commands);
        auto submitted = bench.device.EndFrame();
        REQUIRE(submitted);
        if (wait) {
            REQUIRE(bench.device.QueueWait(QueueKind::AsyncCompute, submitted.Value()));
        }

        graphics::PassDesc compute;
        compute.queue = QueueKind::AsyncCompute;
        auto recording = bench.device.BeginPass(compute);
        REQUIRE(recording);
        graphics::ResourceTransition toCompute;
        toCompute.texture = texture.Value();
        toCompute.before = ResourceAccess::RenderTarget;
        toCompute.after = ResourceAccess::SrvCompute;
        recording.Value()->Transition(Span<const graphics::ResourceTransition>(&toCompute, 1));
        REQUIRE(bench.device.EndFrame());
        // before RenderTarget은 compute 상태가 아니므로 늘 하나, 기다리지 않았으면 경쟁 하나 더.
        CHECK(bench.device.ValidationErrorCount() == (wait ? 1u : 2u));
    }

    // compute queue가 없는 장치는 compute 기록을 받지 않는다.
    GraphBench bench;
    graphics::PassDesc compute;
    compute.queue = QueueKind::AsyncCompute;
    CHECK(bench.device.BeginPass(compute).Error() == ErrorCode::Unsupported);
    CHECK(bench.device.QueueWait(QueueKind::AsyncCompute, {}).Error() == ErrorCode::Unsupported);
}

// compute queue GPU 구간(RG4 남은 것): computeTrace가 있으면 compute 기록에 그 장부의 GPU 프레임
// 시작·끝과 async pass마다 구간 시작·끝 timestamp가 있다(graphics 장부는 graphics 기록에만 쓴다).
TEST(RenderGraph_AsyncComputePassesUseTheComputeTrace) {
    GraphBench bench;
    bench.device.SetAsyncCompute(true);
    TraceSession session;
    TraceSessionSettings sessionSettings;
    sessionSettings.allocator = &bench.allocator;
    sessionSettings.writerMode = TraceWriterMode::Manual;
    REQUIRE(session.Start(sessionSettings));
    REQUIRE(RegisterGpuTraceTypes(session));
    REQUIRE(session.SetChannelEnabled(GpuTraceChannel, true));
    GpuTraceSettings graphicsSettings;
    graphicsSettings.timestamps = true;
    GpuTraceSettings computeSettings = graphicsSettings;
    computeSettings.queue = QueueKind::AsyncCompute;
    computeSettings.queueId = 1;
    computeSettings.queueName = "AsyncCompute";
    GpuTrace graphicsTrace(bench.allocator, graphicsSettings);
    GpuTrace computeTrace(bench.allocator, computeSettings);
    {
        RenderGraphDesc desc = bench.Next(GraphMode::Deferred, &graphicsTrace);
        desc.computeTrace = &computeTrace;
        desc.asyncCompute = GraphAsyncCompute::Always;
        RenderGraph graph(desc);
        BuildGraphA(graph, GraphPassKind::AsyncCompute);
        REQUIRE(graph.Execute());
    }

    const usize computeBegin = bench.IndexOf(Op::BeginCompute);
    const usize computeEnd = bench.IndexOf(Op::EndCompute);
    REQUIRE(computeBegin < computeEnd);
    usize stamps = 0;
    Span<const RecordingDevice::Call> calls = bench.device.Calls();
    for (usize index = computeBegin; index < computeEnd; ++index) {
        stamps += calls[index].op == Op::WriteTimestamp ? 1u : 0u;
    }

    // 프레임 시작·끝 둘과 async pass 둘의 구간 시작·끝 넷이다.
    CHECK(stamps == 6);
    CHECK(computeTrace.Stats().timing);
    CHECK(bench.device.Calibrations() >= 2);
    CHECK(bench.device.ValidationErrorCount() == 0);
    session.Stop();
}

} // namespace render_graph_async_tests_file
