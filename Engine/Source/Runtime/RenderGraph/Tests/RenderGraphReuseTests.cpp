#include "GraphTestSupport.h"

#include <RenderBase/RenderLane.h>
#include <TestSupport/Test.h>

// RG2 A부 시험이다(Docs/Design/RenderBase 8절 RG2, 7.4·7.5절): 같은 그래프 안 풀
// 재사용(수명이 겹치지 않는 같은 desc 자원이 장치 객체 하나를 쓴다, view 사이 재사용), export
// 자원의 fence 뒤 풀 반납(결정 R15), 그래프 안 업로드.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;
using Op = RecordingDevice::Op;

namespace {

TraceScopeName SceneName("Reuse.Scene");
TraceScopeName PostName("Reuse.Post");
TraceScopeName OutputName("Reuse.Output");
TraceScopeName WriteName("Reuse.Write");
TraceScopeName ComputeName("Reuse.Compute");
TraceScopeName HistoryName("Reuse.History");

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

// pass가 본 텍스처 장치 handle을 out에 남기고 그린다.
auto Capture(GraphTexture texture, graphics::TextureHandle* out) noexcept {
    return [texture, out](GraphPassContext& pass) noexcept {
        *out = pass.Texture(texture);
        pass.Commands().Draw(3);
    };
}

// 텍스처를 셰이더로 읽고 그린다.
auto Sampling(GraphTexture texture) noexcept {
    return [texture](GraphPassContext& pass) noexcept {
        pass.Commands().SetTexture(graphics::ShaderStage::Pixel, 0, pass.Texture(texture));
        pass.Commands().Draw(3);
    };
}

// 전이 기록 가운데 (pass, resource)의 첫 것이다(없으면 기본값).
GraphTransitionRecord FindTransition(const RenderGraph& graph, uint32 pass,
                                     uint32 resource) noexcept {
    for (const GraphTransitionRecord& record : graph.Transitions()) {
        if (record.pass == pass && record.resource == resource) {
            return record;
        }
    }

    return {};
}

// view 둘을 차례로 그린다: view마다 Scene → Post → 출력(import). passes가 Immediate이면 즉시
// 실행이다.
struct ViewResult {
    GraphTexture scenes[2];
    GraphTexture posts[2];
    graphics::TextureHandle seen[2][2] = {};
};

void BuildTwoViews(RenderGraph& graph, const RenderHandle* outputs, ViewResult& result) noexcept {
    for (uint32 view = 0; view < 2; ++view) {
        auto scene = graph.CreateTexture(TargetDesc(), "Scene");
        auto post = graph.CreateTexture(TargetDesc(), "Post");
        auto output = graph.ImportTexture(outputs[view], "Output");
        REQUIRE(scene && post && output);
        result.scenes[view] = scene.Value();
        result.posts[view] = post.Value();
        GraphPassAccesses a0(graph);
        a0.RenderTarget(scene.Value());
        REQUIRE(graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, a0,
                              Capture(scene.Value(), &result.seen[view][0])));
        GraphPassAccesses a1(graph);
        a1.Texture(scene.Value(), ResourceAccess::SrvGraphics).RenderTarget(post.Value());
        REQUIRE(graph.AddPass(PostName, GraphPassKind::Raster, GraphPassFlags::None, a1,
                              Capture(post.Value(), &result.seen[view][1])));
        GraphPassAccesses a2(graph);
        a2.Texture(post.Value(), ResourceAccess::SrvGraphics).RenderTarget(output.Value());
        REQUIRE(graph.AddPass(OutputName, GraphPassKind::Raster, GraphPassFlags::None, a2,
                              Sampling(post.Value())));
    }
}

} // namespace

// RG2 완료 기준: 수명이 겹치지 않는 같은 desc 자원이 실제 객체 하나를 쓴다. view 단위
// 선언(7.5절)에서 뒤 view의 장면 텍스처가 앞 view의 것을 이어 쓴다.
TEST(RenderGraph_ViewsReuseSceneTexturesWithinAGraph) {
    GraphBench bench;
    const RenderHandle outputs[] = {bench.ExternalTarget(), bench.ExternalTarget()};
    const usize createdBefore = bench.Count(Op::CreateTexture);
    ViewResult result;
    {
        RenderGraph graph(bench.Next());
        BuildTwoViews(graph, outputs, result);
        REQUIRE(graph.Execute());
        // view 0의 Scene·Post 두 객체를 view 1이 이어 쓴다.
        CHECK(graph.Stats().createdResources == 2);
        CHECK(graph.Stats().reusedWithinGraph == 2);
        CHECK(!graph.Lifetime(result.scenes[0]).reusedWithinGraph);
        CHECK(!graph.Lifetime(result.posts[0]).reusedWithinGraph);
        CHECK(graph.Lifetime(result.scenes[1]).reusedWithinGraph);
        CHECK(graph.Lifetime(result.posts[1]).reusedWithinGraph);
        // 이어 쓰는 자원의 첫 전이(attachment)는 이전 내용 무효에서 시작한다.
        const GraphTransitionRecord first = FindTransition(graph, 3, result.scenes[1].index);
        CHECK(first.before == ResourceAccess::Discard &&
              first.after == ResourceAccess::RenderTarget && first.attachment);
        CHECK(FindTransition(graph, 0, result.scenes[0].index).before == ResourceAccess::Unknown);
    }

    CHECK(bench.Count(Op::CreateTexture) == createdBefore + 2);
    CHECK(result.seen[0][0] == result.seen[1][0]);
    CHECK(result.seen[0][1] == result.seen[1][1]);
    CHECK(!(result.seen[0][0] == result.seen[0][1]));
    CHECK(bench.pool.Stats().resources == 2 && bench.pool.Stats().inUse == 0);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// Immediate 모드는 앞을 내다볼 수 없어 같은 그래프 안 재사용을 하지 않는다.
TEST(RenderGraph_ImmediateModeDoesNotReuseWithinAGraph) {
    GraphBench bench;
    const RenderHandle outputs[] = {bench.ExternalTarget(), bench.ExternalTarget()};
    ViewResult result;
    RenderGraph graph(bench.Next(GraphMode::Immediate));
    BuildTwoViews(graph, outputs, result);
    REQUIRE(graph.Execute());
    CHECK(graph.Stats().createdResources == 4);
    CHECK(graph.Stats().reusedWithinGraph == 0);
    CHECK(!(result.seen[0][0] == result.seen[1][0]));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 수명이 겹치거나 desc가 다르거나 export되는 자원은 객체를 나누지 않는다.
TEST(RenderGraph_ReuseKeepsExportedOverlappingAndOtherDescsApart) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto exported = graph.CreateTexture(TargetDesc(), "Exported");
    auto second = graph.CreateTexture(TargetDesc(), "Second");
    auto other = graph.CreateTexture(TargetDesc(8), "OtherSize");
    auto fourth = graph.CreateTexture(TargetDesc(), "Fourth");
    REQUIRE(exported && second && other && fourth);
    const GraphTexture targets[] = {exported.Value(), second.Value(), other.Value(),
                                    fourth.Value()};
    for (uint32 pass = 0; pass < 4; ++pass) {
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(targets[pass]);
        REQUIRE(graph
                    .AddPass(WriteName, GraphPassKind::Raster,
                             pass == 0 ? GraphPassFlags::None : GraphPassFlags::HasSideEffects,
                             accesses, Draw())
                    .IsOk());
    }

    REQUIRE(graph.ExportTexture(exported.Value()));
    REQUIRE(graph.Execute());
    // export 자원은 그래프 끝까지 놓지 않는다. Second는 pass 1 뒤에 놓이고 Fourth가 이어 쓴다.
    CHECK(!graph.Lifetime(second.Value()).reusedWithinGraph);
    CHECK(!graph.Lifetime(other.Value()).reusedWithinGraph);
    CHECK(graph.Lifetime(fourth.Value()).reusedWithinGraph);
    CHECK(graph.Stats().createdResources == 3);
    CHECK(graph.Stats().reusedWithinGraph == 1);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 이어 쓰는 버퍼의 첫 전이는 Discard에서 UAV로 장치에 내린다(검증 오류 0).
TEST(RenderGraph_ReusedBufferStartsFromDiscard) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    auto first = graph.CreateBuffer(BufferOf(64), "First");
    auto second = graph.CreateBuffer(BufferOf(64), "Second");
    REQUIRE(target && first && second);
    // 기록을 여는 Raster pass다(열린 기록이 없는 Compute는 GX4 전까지 Unsupported).
    GraphPassAccesses a0(graph);
    a0.RenderTarget(target.Value());
    REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a0,
                          Draw()));
    GraphPassAccesses a1(graph);
    a1.Buffer(first.Value(), ResourceAccess::UavCompute);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::None, a1, Draw()));
    GraphPassAccesses a2(graph);
    a2.Buffer(first.Value(), ResourceAccess::SrvCompute);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a2,
                          Draw()));
    GraphPassAccesses a3(graph);
    a3.Buffer(second.Value(), ResourceAccess::UavCompute);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a3,
                          Draw()));
    REQUIRE(graph.Execute());

    CHECK(graph.Lifetime(second.Value()).reusedWithinGraph);
    CHECK(bench.Count(Op::CreateBuffer) == 1);
    const GraphTransitionRecord record = FindTransition(graph, 3, second.Value().index);
    CHECK(record.before == ResourceAccess::Discard && record.after == ResourceAccess::UavCompute &&
          !record.attachment);
    bool lowered = false;
    for (const RecordingDevice::TransitionCall& call : bench.device.TransitionLog()) {
        lowered = lowered || (!call.texture && call.before == ResourceAccess::Discard &&
                              call.after == ResourceAccess::UavCompute);
    }

    CHECK(lowered);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

namespace {

// 시험 준비 단계의 실패는 그 자리에서 멈춘다(값을 돌려주는 함수라 REQUIRE를 쓸 수 없다).
void ExpectSetup(bool condition) noexcept {
    if (!condition) {
        platform::Fatal("render graph history frame setup failed");
    }
}

// history 프레임 하나: previous가 있으면 import해 읽고, 새 history를 그려 export한다.
RenderHandle RunHistoryFrame(GraphBench& bench, RenderHandle previous) noexcept {
    RenderGraph graph(bench.Next());
    auto history = graph.CreateTexture(TargetDesc(), "History");
    ExpectSetup(history.IsOk());
    GraphPassAccesses accesses(graph);
    accesses.RenderTarget(history.Value());
    if (!previous.IsNull()) {
        GraphImportOptions options;
        options.initialAccess = ResourceAccess::SrvGraphics;
        auto imported = graph.ImportTexture(previous, "PreviousHistory", options);
        ExpectSetup(imported.IsOk());
        accesses.Texture(imported.Value(), ResourceAccess::SrvGraphics);
    }

    ExpectSetup(
        graph.AddPass(HistoryName, GraphPassKind::Raster, GraphPassFlags::None, accesses, Draw())
            .IsOk());
    auto exported = graph.ExportTexture(history.Value());
    ExpectSetup(exported.IsOk());
    ExpectSetup(graph.Execute().IsOk());
    return exported.Value();
}

} // namespace

// 결정 R15: export한 history의 Retire는 장치 해제가 아니라 마지막 사용 fence 뒤 풀 반납이다.
// fence가 끝나기 전에는 다시 주지 않고(fence gate), 끝난 뒤에는 같은 desc의 다음 할당이 장치 생성
// 없이 쓴다. 매 프레임 history를 만들고 놓아도 정상 상태에서는 장치 생성·해제가 없다.
TEST(RenderGraph_ExportedHistoryReturnsToThePoolAfterItsFence) {
    GraphBench bench;
    bench.device.HoldCompletions(true);
    const RenderHandle first = RunHistoryFrame(bench, {});
    const RenderHandle second = RunHistoryFrame(bench, first);
    CHECK(bench.pool.Stats().created == 2 && bench.pool.Stats().exported == 2);
    CHECK(!bench.RetireHandle(first));
    CHECK(bench.pool.Stats().exported == 1 && bench.pool.Stats().returning == 1);
    // first의 마지막 사용 fence가 끝나지 않았으므로 다시 주지 않는다.
    const RenderHandle third = RunHistoryFrame(bench, second);
    CHECK(bench.pool.Stats().created == 3);
    CHECK(bench.pool.Stats().returned == 0);
    CHECK(!bench.RetireHandle(second));
    bench.device.CompleteAll();
    const RenderHandle fourth = RunHistoryFrame(bench, third);
    CHECK(bench.pool.Stats().created == 3);
    CHECK(bench.pool.Stats().returned == 2);
    CHECK(!bench.RetireHandle(third));

    // 정상 상태: 완료를 미루지 않으면 같은 객체들이 돈다(장치 생성·해제 0).
    bench.device.HoldCompletions(false);
    bench.device.CompleteAll();
    RenderHandle previous = fourth;
    for (uint32 frame = 0; frame < 8; ++frame) {
        const RenderHandle next = RunHistoryFrame(bench, previous);
        CHECK(!bench.RetireHandle(previous));
        previous = next;
    }

    CHECK(bench.pool.Stats().created == 3);
    CHECK(bench.Count(Op::CreateTexture) == 3);
    CHECK(bench.Count(Op::RetireTexture) == 0);
    CHECK(bench.pool.Stats().exported == 1);
    CHECK(bench.device.ValidationErrorCount() == 0);
    CHECK(!bench.RetireHandle(previous));
}

// 그래프가 import lease를 잡은 동안의 Retire는 lease가 풀린 뒤 풀 반납으로 이어진다(GPU가 읽는 동안
// 반납하지 않는다).
TEST(RenderGraph_HistoryRetiredDuringImportReturnsAfterTheLease) {
    GraphBench bench;
    const RenderHandle first = RunHistoryFrame(bench, {});
    {
        RenderGraph graph(bench.Next());
        GraphImportOptions options;
        options.initialAccess = ResourceAccess::SrvGraphics;
        auto previous = graph.ImportTexture(first, "PreviousHistory", options);
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        REQUIRE(previous && target);
        CHECK(bench.RetireHandle(first));
        GraphPassAccesses accesses(graph);
        accesses.Texture(previous.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(target.Value());
        REQUIRE(graph.AddPass(HistoryName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              accesses, Sampling(previous.Value())));
        REQUIRE(graph.Execute());
        CHECK(bench.pool.Stats().returning == 0);
    }

    RenderHandle handle;
    RenderHandleTable::RetiredResource retired;
    REQUIRE(bench.handles.TakeReleasedRetire(handle, retired));
    CHECK(handle == first && retired.sink == &bench.pool);
    ReleaseRetiredResource(bench.device, handle, retired);
    CHECK(bench.pool.Stats().returning == 1);
    // 반납된 객체를 다음 할당이 다시 쓴다.
    const RenderHandle next = RunHistoryFrame(bench, {});
    CHECK(bench.pool.Stats().created == 2);
    CHECK(bench.pool.Stats().returned == 1);
    CHECK(bench.Count(Op::RetireTexture) == 0);
    CHECK(!bench.RetireHandle(next));
}

// 풀이 먼저 사라지면 export 자원의 소유는 handle로 넘어가고 그 Retire는 장치 Retire다.
TEST(RenderGraph_PoolDestructionHandsExportsToTheirHandles) {
    GraphBench bench;
    RenderHandle exported;
    {
        GraphResourcePool pool(bench.allocator, bench.device);
        RenderGraphDesc desc = bench.Next();
        desc.pool = &pool;
        {
            RenderGraph graph(desc);
            auto texture = graph.CreateTexture(TargetDesc(), "History");
            REQUIRE(texture);
            GraphPassAccesses accesses(graph);
            accesses.RenderTarget(texture.Value());
            REQUIRE(graph
                        .AddPass(HistoryName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                                 Draw())
                        .IsOk());
            auto handle = graph.ExportTexture(texture.Value());
            REQUIRE(handle);
            exported = handle.Value();
            REQUIRE(graph.Execute());
        }

        CHECK(pool.Stats().exported == 1);
    }

    CHECK(bench.Count(Op::RetireTexture) == 0);
    CHECK(!bench.RetireHandle(exported));
    CHECK(bench.Count(Op::RetireTexture) == 1);
}

namespace {

// history 렌더 lane 시험의 프레임 packet이다.
struct HistoryPacket {
    RenderHandle previous;
};

// 렌더 lane 쪽 렌더러다. 그래프 풀과 마지막 history handle을 가진다.
class HistoryRenderer final : public RenderLaneProcessor {
public:
    HistoryRenderer(Allocator& allocator, graphics::Device& device) noexcept
        : pool(allocator, device) {
    }

    void OnLaneStop(RenderLaneContext& context) noexcept override {
        (void)context;
        pool.Clear();
    }

    GraphResourcePool pool;
    RenderHandle latest;
};

void RenderHistoryFrame(RenderFrameContext& context, HistoryPacket& packet) noexcept {
    auto* renderer = static_cast<HistoryRenderer*>(context.Processor());
    RenderGraph graph(MakeRenderGraphDesc(context, renderer->pool));
    auto history = graph.CreateTexture(TargetDesc(), "History");
    if (!history) {
        context.ReportFailure(ErrorCode::DependencyFailed);
        return;
    }

    GraphPassAccesses accesses(graph);
    accesses.RenderTarget(history.Value());
    if (!packet.previous.IsNull()) {
        GraphImportOptions options;
        options.initialAccess = ResourceAccess::SrvGraphics;
        auto previous = graph.ImportTexture(packet.previous, "PreviousHistory", options);
        if (!previous) {
            context.ReportFailure(ErrorCode::DependencyFailed);
            return;
        }

        accesses.Texture(previous.Value(), ResourceAccess::SrvGraphics);
    }

    auto added =
        graph.AddPass(HistoryName, GraphPassKind::Raster, GraphPassFlags::None, accesses, Draw());
    auto exported = graph.ExportTexture(history.Value());
    if (!added || !exported || !graph.Execute()) {
        context.ReportFailure(ErrorCode::DependencyFailed);
        return;
    }

    renderer->latest = exported.Value();
}

} // namespace

ROCKS_LANE_TRANSFERABLE(HistoryPacket);

// 결정 R15를 렌더 lane Retire로: lane.Retire(history)가 장치 해제 대신 풀 반납이 되고, fence 통지
// 뒤 handle이 표에서 빠진다. 프레임마다 history를 바꿔도 장치 생성은 두 번뿐이다.
TEST(RenderGraph_HistoryReturnsThroughTheRenderLane) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    HistoryRenderer renderer(allocator, device);
    RenderLane lane(allocator, device);
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Inline;
    config.processor = &renderer;
    REQUIRE(lane.Start(config));
    RenderHandle previous;
    for (uint32 frame = 0; frame < 6; ++frame) {
        auto ticket = lane.BeginFrame();
        REQUIRE(ticket);
        REQUIRE(
            lane.SubmitFrame(Move(ticket.Value()), HistoryPacket{previous}, &RenderHistoryFrame));
        REQUIRE(lane.Drain(RenderDrainReason::Test));
        const RenderHandle next = renderer.latest;
        REQUIRE(!(next == previous));
        if (!previous.IsNull()) {
            REQUIRE(lane.Retire(previous));
            REQUIRE(lane.Drain(RenderDrainReason::Test));
        }

        previous = next;
    }

    CHECK(lane.Failure() == ErrorCode::None);
    CHECK(renderer.pool.Stats().created == 2);
    CHECK(renderer.pool.Stats().returned == 4);
    usize retires = 0;
    for (const RecordingDevice::Call& call : device.Calls()) {
        retires += call.op == Op::RetireTexture ? 1u : 0u;
    }

    CHECK(retires == 0);
    REQUIRE(lane.Retire(previous));
    auto drained = lane.DrainRetires();
    REQUIRE(drained);
    CHECK(lane.Handles().Count() == 0);
    CHECK(device.ValidationErrorCount() == 0);
    CHECK(lane.Shutdown());
}

// 그래프 안 업로드: 첫 기록을 열기 전에 장치 업로드 경로로 올리고, 뒤 pass의 읽기 전이를 그래프가
// 낸다(장치 업로드는 상태 중립이므로 before는 장치 추적 상태 Unknown).
TEST(RenderGraph_UploadsRunBeforeTheFirstPass) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    auto vertices = graph.CreateBuffer(BufferOf(64), "Vertices");
    REQUIRE(target && vertices);
    uint8 data[48] = {};
    for (uint32 index = 0; index < 48; ++index) {
        data[index] = static_cast<uint8>(index);
    }

    REQUIRE(graph.UploadBuffer(vertices.Value(), Span<const uint8>(data, 32)));
    REQUIRE(graph.UploadBuffer(vertices.Value(), Span<const uint8>(data + 32, 16), 32));
    GraphPassAccesses accesses(graph);
    accesses.Buffer(vertices.Value(), ResourceAccess::VertexOrIndex).RenderTarget(target.Value());
    REQUIRE(graph
                .AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, accesses,
                         Draw())
                .IsOk());
    REQUIRE(graph.Execute());

    CHECK(graph.Stats().uploads == 2);
    const usize begin = bench.IndexOf(Op::BeginOffscreen);
    const usize firstUpload = bench.IndexOf(Op::UpdateBuffer, 0);
    const usize secondUpload = bench.IndexOf(Op::UpdateBuffer, 1);
    REQUIRE(begin != NumericLimits<usize>::Max() && secondUpload != NumericLimits<usize>::Max());
    CHECK(firstUpload < secondUpload && secondUpload < begin);
    // value는 offset + 크기다(기록 장치).
    CHECK(bench.device.Calls()[firstUpload].value == 32);
    CHECK(bench.device.Calls()[secondUpload].value == 48);
    const GraphTransitionRecord record = FindTransition(graph, 0, vertices.Value().index);
    CHECK(record.before == ResourceAccess::Unknown &&
          record.after == ResourceAccess::VertexOrIndex);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 남은 pass가 쓰지 않는 자원의 업로드는 하지 않는다(할당도 없다).
TEST(RenderGraph_UploadIntoACulledResourceIsSkipped) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    auto unused = graph.CreateBuffer(BufferOf(16), "Unused");
    REQUIRE(target && unused);
    const uint8 data[16] = {};
    REQUIRE(graph.UploadBuffer(unused.Value(), Span<const uint8>(data)));
    GraphPassAccesses accesses(graph);
    accesses.Buffer(unused.Value(), ResourceAccess::VertexOrIndex).RenderTarget(target.Value());
    auto culled =
        graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, accesses, Draw());
    REQUIRE(culled);
    REQUIRE(graph.Execute());
    CHECK(graph.IsCulled(culled.Value()));
    CHECK(graph.Stats().uploads == 0);
    CHECK(bench.Count(Op::CreateBuffer) == 0);
    CHECK(bench.Count(Op::UpdateBuffer) == 0);
}

// 업로드 대상은 그래프 시작에 할당하므로 앞 자원의 객체를 이어 쓰지 않는다(업로드가 첫 pass 앞에서
// 실행되어 그 객체의 앞 내용을 덮기 때문이다). 업로드 대상이 놓은 객체는 뒤 자원이 이어 쓴다.
TEST(RenderGraph_UploadTargetsDoNotTakeEarlierObjects) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    auto early = graph.CreateBuffer(BufferOf(64), "Early");
    auto uploaded = graph.CreateBuffer(BufferOf(64), "Uploaded");
    auto late = graph.CreateBuffer(BufferOf(64), "Late");
    REQUIRE(target && early && uploaded && late);
    const uint8 data[64] = {};
    REQUIRE(graph.UploadBuffer(uploaded.Value(), Span<const uint8>(data)));
    GraphPassAccesses a0(graph);
    a0.RenderTarget(target.Value());
    REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a0,
                          Draw()));
    GraphPassAccesses a1(graph);
    a1.Buffer(early.Value(), ResourceAccess::UavCompute);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a1,
                          Draw()));
    GraphPassAccesses a2(graph);
    a2.Buffer(uploaded.Value(), ResourceAccess::SrvCompute);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a2,
                          Draw()));
    GraphPassAccesses a3(graph);
    a3.Buffer(late.Value(), ResourceAccess::UavCompute);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a3,
                          Draw()));
    REQUIRE(graph.Execute());

    CHECK(!graph.Lifetime(early.Value()).reusedWithinGraph);
    CHECK(!graph.Lifetime(uploaded.Value()).reusedWithinGraph);
    CHECK(graph.Lifetime(late.Value()).reusedWithinGraph);
    CHECK(bench.Count(Op::CreateBuffer) == 2);
    // 업로드 대상의 생성·업로드가 첫 기록보다 앞이다.
    CHECK(bench.IndexOf(Op::UpdateBuffer) < bench.IndexOf(Op::BeginOffscreen));
    CHECK(bench.IndexOf(Op::CreateBuffer) < bench.IndexOf(Op::BeginOffscreen));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// Immediate 모드는 업로드 대상의 첫 pass 직전에 할당하고 올린다.
TEST(RenderGraph_ImmediateModeUploadsBeforeTheFirstUse) {
    GraphBench bench;
    RenderGraph graph(bench.Next(GraphMode::Immediate));
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    auto constants = graph.CreateBuffer(BufferOf(32), "Constants");
    REQUIRE(target && constants);
    const uint8 data[32] = {};
    REQUIRE(graph.UploadBuffer(constants.Value(), Span<const uint8>(data)));
    GraphPassAccesses a0(graph);
    a0.RenderTarget(target.Value());
    REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a0,
                          Draw()));
    GraphPassAccesses a1(graph);
    a1.Buffer(constants.Value(), ResourceAccess::Constant);
    REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a1,
                          Draw()));
    REQUIRE(graph.Execute());
    CHECK(graph.Stats().uploads == 1);
    CHECK(bench.IndexOf(Op::BeginOffscreen) < bench.IndexOf(Op::UpdateBuffer));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

namespace {

graphics::TextureDesc LutDesc() noexcept {
    graphics::TextureDesc desc;
    desc.width = 4;
    desc.height = 4;
    desc.mipLevels = 2;
    desc.format = graphics::PixelFormat::RGBA8Unorm;
    desc.usage = graphics::TextureUsage::Sampled;
    return desc;
}

// Sampled 그래프 텍스처에 mip 둘을 올리고 읽는 그래프다.
void RunLutGraph(GraphBench& bench) noexcept {
    RenderGraph graph(bench.Next());
    auto lut = graph.CreateTexture(LutDesc(), "Lut");
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    REQUIRE(lut && target);
    uint8 mip0[64] = {};
    uint8 mip1[16] = {};
    REQUIRE(graph.UploadTexture(lut.Value(), 0, Span<const uint8>(mip0)));
    REQUIRE(graph.UploadTexture(lut.Value(), 1, Span<const uint8>(mip1)));
    GraphPassAccesses accesses(graph);
    accesses.Texture(lut.Value(), ResourceAccess::SrvGraphics).RenderTarget(target.Value());
    REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                          accesses, Sampling(lut.Value())));
    REQUIRE(graph.Execute());
    CHECK(graph.Stats().uploads == 2);
}

} // namespace

// Sampled 그래프 텍스처: 새로 만들면 mip 사슬이 생성 초기 데이터이고, 풀에서 다시 받으면 mip마다
// 장치 갱신으로 올린다.
TEST(RenderGraph_SampledTextureUploadsEveryMip) {
    GraphBench bench;
    RunLutGraph(bench);
    CHECK(bench.Count(Op::CreateTexture) == 2);
    CHECK(bench.Count(Op::UpdateTexture) == 0);
    RunLutGraph(bench);
    CHECK(bench.Count(Op::CreateTexture) == 2);
    REQUIRE(bench.Count(Op::UpdateTexture) == 2);
    CHECK(bench.device.Calls()[bench.IndexOf(Op::UpdateTexture, 0)].value == 64);
    CHECK(bench.device.Calls()[bench.IndexOf(Op::UpdateTexture, 1)].value ==
          ((uint64{1} << 32) | 16));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 업로드 음성 검증.
TEST(RenderGraph_RejectsInvalidUploads) {
    GraphBench bench;
    const uint8 bytes[64] = {};
    {
        RenderGraph graph(bench.Next());
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        auto buffer = graph.CreateBuffer(BufferOf(32), "Buffer");
        auto lut = graph.CreateTexture(LutDesc(), "Lut");
        REQUIRE(target && buffer && lut);
        // 렌더 타깃 그래프 텍스처에는 올릴 수 없다(버퍼 → 텍스처 복사 명령이 없다).
        CHECK(graph.UploadTexture(target.Value(), 0, Span<const uint8>(bytes)).Error() ==
              ErrorCode::Unsupported);
        // 범위 밖·빈 데이터.
        CHECK(graph.UploadBuffer(buffer.Value(), Span<const uint8>(bytes, 16), 24).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(graph.UploadBuffer(buffer.Value(), Span<const uint8>()).Error() ==
              ErrorCode::InvalidArgument);
        // mip 크기 불일치와 같은 mip 두 번.
        CHECK(graph.UploadTexture(lut.Value(), 1, Span<const uint8>(bytes)).Error() ==
              ErrorCode::InvalidArgument);
        REQUIRE(graph.UploadTexture(lut.Value(), 1, Span<const uint8>(bytes, 16)));
        CHECK(graph.UploadTexture(lut.Value(), 1, Span<const uint8>(bytes, 16)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "uploaded twice"));
        // Sampled 그래프 텍스처에 쓰는 pass와 그것을 렌더 타깃으로 쓰는 pass.
        GraphPassAccesses write(graph);
        write.Texture(lut.Value(), ResourceAccess::UavCompute);
        CHECK(
            graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::None, write, Draw())
                .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "uploads only"));
        GraphPassAccesses render(graph);
        render.RenderTarget(lut.Value());
        CHECK(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, render, Draw())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "not a render target graph texture"));
        // 올리지 않은 mip 0을 읽으면 생산 전 읽기다.
        GraphPassAccesses read(graph);
        read.Texture(lut.Value(), ResourceAccess::SrvGraphics, {0, 1, 0, 1})
            .RenderTarget(target.Value());
        CHECK(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, read, Draw())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "before any pass produced it"));
        // pass가 이미 선언한 자원에는 올릴 수 없다.
        GraphPassAccesses produce(graph);
        produce.RenderTarget(target.Value());
        REQUIRE(
            graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, produce, Draw()));
        CHECK(graph.UploadTexture(target.Value(), 0, Span<const uint8>(bytes)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "after a pass declared it"));
    }

    {
        // import 자원에는 올릴 수 없다.
        const RenderHandle external = bench.ExternalTarget();
        RenderGraph graph(bench.Next());
        auto imported = graph.ImportTexture(external, "External");
        REQUIRE(imported);
        CHECK(graph.UploadTexture(imported.Value(), 0, Span<const uint8>(bytes)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "is imported"));
    }

    {
        // 쓰이는 Sampled 그래프 텍스처는 모든 mip이 올라와야 한다(Execute 검증).
        RenderGraph graph(bench.Next());
        auto lut = graph.CreateTexture(LutDesc(), "Lut");
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        REQUIRE(lut && target);
        REQUIRE(graph.UploadTexture(lut.Value(), 0, Span<const uint8>(bytes)));
        GraphPassAccesses accesses(graph);
        accesses.Texture(lut.Value(), ResourceAccess::SrvGraphics, {0, 1, 0, 1})
            .RenderTarget(target.Value());
        REQUIRE(graph
                    .AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                             accesses, Draw())
                    .IsOk());
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "has no upload for mip 1"));
    }

    CHECK(bench.Count(Op::UpdateBuffer) == 0);
    CHECK(bench.Count(Op::UpdateTexture) == 0);
}
