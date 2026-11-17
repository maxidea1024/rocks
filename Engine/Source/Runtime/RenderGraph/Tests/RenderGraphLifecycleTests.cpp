#include "GraphTestSupport.h"

#include <Core/Text/StringView.h>
#include <RenderBase/RenderLane.h>
#include <TestSupport/Test.h>

// RG1 완료 기준 (e) 음성 검증, (f) 풀 축출 fence gate와 import lease·export·GpuScope·렌더 lane 연결
// 시험이다(Docs/Design/RenderBase.ko.md 7.4·8절).
using namespace rocks;
using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;

namespace {

TraceScopeName WriteName("Lifecycle.Write");
TraceScopeName ReadName("Lifecycle.Read");
TraceScopeName ComputeName("Lifecycle.Compute");

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

bool Contains(StringView text, const char* part) noexcept {
    const StringView needle(part);
    if (needle.ByteCount() > text.ByteCount()) {
        return false;
    }

    for (usize offset = 0; offset + needle.ByteCount() <= text.ByteCount(); ++offset) {
        if (platform::MemoryEquals(text.Data() + offset, needle.Data(), needle.ByteCount())) {
            return true;
        }
    }

    return false;
}

} // namespace

// (e) 생산 전 읽기: pass를 넣지 않고 그래프 전체가 실패한다.
TEST(RenderGraph_RejectsReadBeforeProduce) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto source = graph.CreateTexture(TargetDesc(), "Source");
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    REQUIRE(source && target);
    GraphPassAccesses accesses(graph);
    accesses.Texture(source.Value(), ResourceAccess::SrvGraphics).RenderTarget(target.Value());
    auto added = graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                               accesses, Draw());
    CHECK(added.Error() == ErrorCode::InvalidArgument);
    CHECK(Contains(graph.LastError(), "before any pass produced it"));
    CHECK(graph.Stats().validationErrors == 1);
    CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    CHECK(bench.Count(RecordingDevice::Op::BeginOffscreen) == 0);
}

// (e) 한 pass 안 같은 subresource의 상충 접근(렌더 타깃이면서 SRV, SRV이면서 UAV).
TEST(RenderGraph_RejectsConflictingAccessWithinAPass) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto texture = graph.CreateTexture(TargetDesc(), "Texture");
    REQUIRE(texture);
    GraphPassAccesses produce(graph);
    produce.RenderTarget(texture.Value());
    REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, produce, Draw()));
    GraphPassAccesses feedback(graph);
    feedback.Texture(texture.Value(), ResourceAccess::SrvGraphics).RenderTarget(texture.Value());
    CHECK(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::None, feedback, Draw())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(Contains(graph.LastError(), "conflicting accesses"));
    GraphPassAccesses readWrite(graph);
    readWrite.Texture(texture.Value(), ResourceAccess::SrvCompute)
        .Texture(texture.Value(), ResourceAccess::UavCompute);
    CHECK(
        graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::None, readWrite, Draw())
            .Error() == ErrorCode::InvalidArgument);
    // 읽기끼리는 합친다(상충 아님).
    GraphPassAccesses reads(graph);
    reads.Texture(texture.Value(), ResourceAccess::SrvCompute)
        .Texture(texture.Value(), ResourceAccess::SrvCompute);
    CHECK(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::None, reads, Draw()));
    CHECK(graph.Stats().validationErrors == 2);
}

// (e) Raster pass의 UAV와 pass 종류에 맞지 않는 접근.
TEST(RenderGraph_RejectsUavInRasterPasses) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    graphics::BufferDesc desc;
    desc.size = 16;
    auto buffer = graph.CreateBuffer(desc, "Buffer");
    REQUIRE(target && buffer);
    GraphPassAccesses accesses(graph);
    accesses.Buffer(buffer.Value(), ResourceAccess::UavGraphics).RenderTarget(target.Value());
    CHECK(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, accesses, Draw())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(Contains(graph.LastError(), "UAVs are compute only"));
    GraphPassAccesses copy(graph);
    copy.Buffer(buffer.Value(), ResourceAccess::UavCompute);
    CHECK(
        graph.AddPass(WriteName, GraphPassKind::Copy, GraphPassFlags::None, copy, Draw()).Error() ==
        ErrorCode::InvalidArgument);
    GraphPassAccesses noTarget(graph);
    CHECK(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, noTarget, Draw())
              .Error() == ErrorCode::InvalidArgument);
    GraphPassAccesses async(graph);
    async.Buffer(buffer.Value(), ResourceAccess::UavCompute);
    CHECK(
        graph.AddPass(ComputeName, GraphPassKind::AsyncCompute, GraphPassFlags::None, async, Draw())
            .Error() == ErrorCode::Unsupported);
}

// (e) Retire된 자원 import는 lease를 얻지 못해 실패한다. 다른 그래프의 handle도 거부한다.
TEST(RenderGraph_RejectsRetiredImportsAndForeignHandles) {
    GraphBench bench;
    const RenderHandle external = bench.ExternalTarget();
    auto retired = bench.handles.BeginRetire(external);
    REQUIRE(retired);
    bench.device.Retire(graphics::TextureHandle{retired.Value().device});
    RenderGraph graph(bench.Next());
    auto imported = graph.ImportTexture(external, "Retired");
    CHECK(imported.Error() == ErrorCode::StaleHandle);
    CHECK(Contains(graph.LastError(), "retired"));
    CHECK(graph.Execute().Error() == ErrorCode::StaleHandle);

    RenderGraph other(bench.Next());
    auto foreign = other.CreateTexture(TargetDesc(), "Foreign");
    REQUIRE(foreign);
    RenderGraph graph2(bench.Next());
    GraphPassAccesses accesses(graph2);
    accesses.RenderTarget(foreign.Value());
    CHECK(graph2.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, accesses, Draw())
              .Error() == ErrorCode::InvalidArgument);
}

// (e) 생산 전 export는 Execute에서 거부한다.
TEST(RenderGraph_RejectsExportBeforeProduce) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto texture = graph.CreateTexture(TargetDesc(), "Never");
    REQUIRE(texture);
    auto exported = graph.ExportTexture(texture.Value());
    REQUIRE(exported);
    CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    CHECK(Contains(graph.LastError(), "exported before any pass produced it"));
    // 실패한 그래프의 export handle은 Failed다.
    auto state = bench.handles.State(exported.Value());
    REQUIRE(state);
    CHECK(state.Value() == RenderResourceState::Failed);
}

// import lease: 그래프가 잡은 동안 Retire는 새 lease를 막고 장치 Retire를 미룬다. 그래프가 끝나면
// 미룬 Retire가 장치 handle과 함께 나온다.
TEST(RenderGraph_ImportLeaseDefersRetire) {
    GraphBench bench;
    const RenderHandle external = bench.ExternalTarget();
    const graphics::TextureHandle device = bench.DeviceTexture(external);
    {
        RenderGraph graph(bench.Next());
        auto output = graph.ImportTexture(external, "Output");
        REQUIRE(output);
        CHECK(bench.handles.LeaseCount(external) == 1);
        // 실행 전 Retire: 승인된 기록이 있으므로 장치 Retire를 미룬다.
        auto retired = bench.handles.BeginRetire(external);
        REQUIRE(retired);
        CHECK(retired.Value().deferred);
        CHECK(retired.Value().device.IsNull());
        CHECK(bench.handles.AcquireLease(external, RenderResourceKind::Texture).Error() ==
              ErrorCode::StaleHandle);
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(output.Value());
        REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              Draw()));
        REQUIRE(graph.Execute());
        // 장치 자원은 그래프가 끝날 때까지 살아 있었다(stale handle 사용 없음).
        CHECK(bench.device.TextureAccess(device) == ResourceAccess::RenderTarget);
    }

    CHECK(bench.handles.LeaseCount(external) == 0);
    RenderHandle handle;
    RenderHandleTable::RetiredResource released;
    REQUIRE(bench.handles.TakeReleasedRetire(handle, released));
    CHECK(handle == external);
    CHECK(released.device == device.slot);
    CHECK(!bench.handles.TakeReleasedRetire(handle, released));
}

// export(history): 풀 자원의 소유가 렌더 handle로 넘어가고 다음 그래프가 import로 읽는다.
TEST(RenderGraph_ExportedTextureIsImportedByTheNextGraph) {
    GraphBench bench;
    RenderHandle history;
    {
        RenderGraph graph(bench.Next());
        auto texture = graph.CreateTexture(TargetDesc(), "History");
        REQUIRE(texture);
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(texture.Value());
        REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              Draw()));
        auto exported = graph.ExportTexture(texture.Value());
        REQUIRE(exported);
        history = exported.Value();
        CHECK(bench.handles.State(history).Value() == RenderResourceState::Pending);
        REQUIRE(graph.Execute());
    }

    CHECK(bench.handles.State(history).Value() == RenderResourceState::Ready);
    CHECK(bench.pool.Stats().detached == 1 && bench.pool.Stats().resources == 0);
    const graphics::TextureHandle device = bench.DeviceTexture(history);
    CHECK(bench.device.TextureAccess(device) == ResourceAccess::SrvGraphics);
    {
        RenderGraph graph(bench.Next());
        GraphImportOptions options;
        options.initialAccess = ResourceAccess::SrvGraphics;
        auto previous = graph.ImportTexture(history, "PreviousHistory", options);
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        REQUIRE(previous && target);
        GraphPassAccesses accesses(graph);
        accesses.Texture(previous.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(target.Value());
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              accesses,
                              [previous = previous.Value()](GraphPassContext& pass) noexcept {
                                  pass.Commands().SetTexture(pass.Texture(previous));
                                  pass.Commands().Draw(3);
                              }));
        REQUIRE(graph.Execute());
        // 이미 SRV 상태로 import했으므로 읽기 전이가 없다.
        CHECK(graph.Stats().transitions == 0);
    }

    CHECK(bench.device.ValidationErrorCount() == 0);
}

// (f) 풀 축출은 마지막 사용 fence가 끝나기 전에는 일어나지 않는다(fence gate).
TEST(RenderGraph_PoolEvictionWaitsForFenceCompletion) {
    GraphResourcePoolConfig config;
    config.budgetBytes = 0;
    config.retainFrames = 1;
    GraphBench bench(config);
    bench.device.HoldCompletions(true);
    {
        RenderGraph graph(bench.Next());
        auto texture = graph.CreateTexture(TargetDesc(), "Transient");
        REQUIRE(texture);
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(texture.Value());
        REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              accesses, Draw()));
        REQUIRE(graph.Execute());
    }

    CHECK(bench.pool.Stats().resources == 1);
    // 프레임이 지나도 GPU가 끝내지 않았으면 축출하지 않는다.
    for (uint32 frame = 0; frame < 3; ++frame) {
        RenderGraph graph(bench.Next());
        REQUIRE(graph.Execute());
        CHECK(bench.pool.Stats().resources == 1);
        CHECK(bench.Count(RecordingDevice::Op::RetireTexture) == 0);
    }

    bench.device.CompleteAll();
    {
        RenderGraph graph(bench.Next());
        REQUIRE(graph.Execute());
    }

    CHECK(bench.pool.Stats().resources == 0);
    CHECK(bench.pool.Stats().evicted == 1);
    CHECK(bench.Count(RecordingDevice::Op::RetireTexture) == 1);
}

// pass마다 GpuScope 구간(marker push·pop, pass 이름)을 둔다.
TEST(RenderGraph_EachPassGetsAGpuScope) {
    GraphBench bench;
    GpuTraceSettings settings;
    settings.markers = true;
    GpuTrace trace(bench.allocator, settings);
    {
        RenderGraph graph(bench.Next(GraphMode::Deferred, &trace));
        auto first = graph.CreateTexture(TargetDesc(), "First");
        auto second = graph.CreateTexture(TargetDesc(), "Second");
        REQUIRE(first && second);
        GraphPassAccesses a0(graph);
        a0.RenderTarget(first.Value());
        REQUIRE(graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, a0, Draw()));
        GraphPassAccesses a1(graph);
        a1.Texture(first.Value(), ResourceAccess::SrvGraphics).RenderTarget(second.Value());
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a1,
                              Draw()));
        REQUIRE(graph.Execute());
    }

    // 각 pass: marker push(이름 byte 수) → 그리기 → marker pop.
    const usize writeBytes = StringView(WriteName.Name()).ByteCount();
    const usize readBytes = StringView(ReadName.Name()).ByteCount();
    usize pushes = 0;
    bool namesMatch = true;
    bool drawInsideScope = false;
    uint32 depth = 0;
    for (const RecordingDevice::Call& call : bench.device.Calls()) {
        if (call.op == RecordingDevice::Op::PushMarker) {
            namesMatch = namesMatch && call.value == (pushes == 0 ? writeBytes : readBytes);
            ++pushes;
            ++depth;
        } else if (call.op == RecordingDevice::Op::PopMarker) {
            --depth;
        } else if (call.op == RecordingDevice::Op::Draw) {
            drawInsideScope = depth == 1;
        }
    }

    CHECK(pushes == 2);
    CHECK(namesMatch);
    CHECK(drawInsideScope);
    CHECK(bench.Count(RecordingDevice::Op::PopMarker) == 2);
}

namespace {

// 렌더 lane 연결 시험의 프레임 packet이다.
struct GraphFramePacket {
    RenderHandle output;
};

// 렌더 lane 쪽 렌더러다. 그래프 풀을 가진다.
class GraphRenderer final : public RenderLaneProcessor {
public:
    GraphRenderer(Allocator& allocator, graphics::Device& device) noexcept
        : pool(allocator, device) {
    }

    void OnLaneStop(RenderLaneContext& context) noexcept override {
        (void)context;
        pool.Clear();
    }

    GraphResourcePool pool;
    uint32 frames = 0;
    uint64 arenaBytes = 0;
};

// lease 시험 값이다.
struct LeaseStep {
    RenderHandle handle;
    bool acquire = true;
};

RenderResourceLease heldLease;

void RenderGraphFrame(RenderFrameContext& context, GraphFramePacket& packet) noexcept {
    auto* renderer = static_cast<GraphRenderer*>(context.Processor());
    RenderGraph graph(MakeRenderGraphDesc(context, renderer->pool));
    auto scene = graph.CreateTexture(TargetDesc(), "Scene");
    auto output = graph.ImportTexture(packet.output, "Output");
    if (!scene || !output) {
        context.ReportFailure(ErrorCode::DependencyFailed);
        return;
    }

    GraphPassAccesses a0(graph);
    a0.RenderTarget(scene.Value());
    auto first = graph.AddPass(WriteName, GraphPassKind::Raster, GraphPassFlags::None, a0, Draw());
    GraphPassAccesses a1(graph);
    a1.Texture(scene.Value(), ResourceAccess::SrvGraphics).RenderTarget(output.Value());
    auto second = graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::None, a1, Draw());
    if (!first || !second || !graph.Execute()) {
        context.ReportFailure(ErrorCode::DependencyFailed);
        return;
    }

    ++renderer->frames;
    renderer->arenaBytes = context.Arena().Stats().requestedBytes;
}

void RunLeaseStep(RenderLaneContext& context, LeaseStep& step) noexcept {
    if (step.acquire) {
        auto lease = context.Handles().AcquireLease(step.handle, RenderResourceKind::Texture);
        heldLease = lease ? lease.Value() : RenderResourceLease{};
    } else {
        context.Handles().ReleaseLease(heldLease);
        heldLease = {};
    }
}

struct TargetRequest {
    RenderHandle handle;
};

void CreateTargetOnLane(RenderLaneContext& context, TargetRequest& request) noexcept {
    auto created = context.Device().CreateTexture(TargetDesc(), {});
    if (!created || !context.Handles().BindTexture(request.handle, created.Value())) {
        context.ReportFailure(ErrorCode::DependencyFailed);
    }
}

} // namespace

ROCKS_RENDER_TRANSFERABLE(GraphFramePacket);
ROCKS_RENDER_TRANSFERABLE(LeaseStep);
ROCKS_RENDER_TRANSFERABLE(TargetRequest);

// 렌더 lane 프레임 작업 안의 그래프: 프레임 slot arena·handle 표·장치를 문맥에서 받고, 그 프레임의
// 마지막 제출이 slot GPU 완료 표지가 된다.
TEST(RenderGraph_RunsInsideRenderLaneFrames) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    GraphRenderer renderer(allocator, device);
    RenderLane lane(allocator, device);
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Inline;
    config.processor = &renderer;
    REQUIRE(lane.Start(config));
    auto output = lane.ReserveHandle(RenderResourceKind::Texture);
    REQUIRE(output);
    REQUIRE(lane.PostResource(TargetRequest{output.Value()}, &CreateTargetOnLane));
    for (uint32 frame = 0; frame < 3; ++frame) {
        auto ticket = lane.BeginFrame();
        REQUIRE(ticket);
        REQUIRE(lane.SubmitFrame(Move(ticket.Value()), GraphFramePacket{output.Value()},
                                 &RenderGraphFrame));
    }

    REQUIRE(lane.Drain(RenderDrainReason::Test));
    CHECK(renderer.frames == 3);
    CHECK(renderer.arenaBytes != 0);
    // 첫 프레임에 만든 풀 자원을 뒤 프레임이 다시 쓴다.
    CHECK(renderer.pool.Stats().created == 1);
    CHECK(renderer.pool.Stats().reused == 2);
    CHECK(device.ValidationErrorCount() == 0);
    CHECK(lane.Failure() == ErrorCode::None);
    REQUIRE(lane.Retire(output.Value()));
    CHECK(lane.Shutdown());
}

// lane의 Retire는 lease가 남은 동안 장치 Retire를 미루고, 마지막 lease를 놓은 lane 작업 끝에
// 장치로 넘긴다.
TEST(RenderGraph_LaneRetireWaitsForTheLastLease) {
    SystemAllocator allocator;
    RecordingDevice device(allocator);
    RenderLane lane(allocator, device);
    RenderLaneConfig config;
    config.mode = RenderLaneMode::Inline;
    REQUIRE(lane.Start(config));
    auto target = lane.ReserveHandle(RenderResourceKind::Texture);
    REQUIRE(target);
    REQUIRE(lane.PostResource(TargetRequest{target.Value()}, &CreateTargetOnLane));
    REQUIRE(lane.PostResource(LeaseStep{target.Value(), true}, &RunLeaseStep));
    REQUIRE(heldLease.IsValid());
    REQUIRE(lane.Retire(target.Value()));
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    auto countRetires = [&device]() noexcept {
        usize count = 0;
        for (const RecordingDevice::Call& call : device.Calls()) {
            count += call.op == RecordingDevice::Op::RetireTexture ? 1u : 0u;
        }

        return count;
    };

    CHECK(countRetires() == 0);
    CHECK(lane.Handles().State(target.Value()).Value() == RenderResourceState::Retired);
    REQUIRE(lane.PostResource(LeaseStep{target.Value(), false}, &RunLeaseStep));
    REQUIRE(lane.Drain(RenderDrainReason::Test));
    CHECK(countRetires() == 1);
    auto drained = lane.DrainRetires();
    REQUIRE(drained);
    CHECK(lane.Handles().Count() == 0);
    CHECK(lane.Shutdown());
}
