#include "GraphTestSupport.h"

#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderFormatTypes.h>
#include <ShaderCore/ShaderParameterLayout.h>
#include <TestSupport/Test.h>

// 그래프 상수 시험이다(Docs/Design/RenderBase 9.11절 8항 GX6): 선언 때 복사·실행 시작(Immediate는
// 선언 때) 생성·그래프 끝 Retire, pass 안 묶기(Constants·SetConstants), 다른 그래프 handle·크기
// 위반의 검증 오류, 병렬 기록(RG3)에서 SetConstantBuffer·SetConstantData 재생이 직렬과 같은 호출
// 열.
using namespace rocks;
namespace render_graph_constant_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;

namespace {

TraceScopeName DrawName("Constants.Draw");
TraceScopeName SecondName("Constants.Second");

// 시험에 쓰는 worker 수다.
constexpr uint32 Workers = 2;

constexpr shader::ShaderParameterMember ViewMembers[] = {
    {"scale", shader::ShaderParameterType::Float4, 0, 0}};
constexpr shader::ShaderParameterLayout ViewLayout = {"View", false, 16, ViewMembers};

uint32 WordOf(float value) noexcept {
    uint32 word = 0;
    platform::MemoryCopy(&word, &value, sizeof(word));
    return word;
}

// PS slot 3에 View를 둔 바인딩 지도다.
struct ViewMap {
    ViewMap() noexcept : pixel(allocator) {
        pixel.stage = shader::ShaderStagePixel;
        shader::ShaderBindingMember member{"scale", 0, 16};
        (void)pixel.members.TryPushBack(member);
        shader::ShaderBindingResource buffer;
        buffer.kind = shader::ShaderResourceKind::ConstantBuffer;
        buffer.slot = 3;
        buffer.name = "View";
        buffer.constantBufferBytes = 16;
        buffer.memberCount = 1;
        (void)pixel.resources.TryPushBack(buffer);
        const shader::ShaderBindingLayout* stages[] = {&pixel};
        auto built = ShaderBindingMap::Build(ViewLayout, stages);
        if (!built) {
            platform::Fatal("render graph constant test map build failed");
        }

        map = built.Value();
    }

    SystemAllocator allocator;
    shader::ShaderBindingLayout pixel;
    ShaderBindingMap map;
};

// 상수 둘을 선언하고 Raster pass 둘이 묶는 그래프다(둘째 pass는 기록 명령도 쓴다).
bool BuildGraph(RenderGraph& graph, const ViewMap& view) noexcept {
    const float half[4] = {0.5f, 0.5f, 0.5f, 1.0f};
    const float quarter[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    auto first = graph.CreateConstants(
        Span<const uint8>(reinterpret_cast<const uint8*>(half), sizeof(half)), "Half");
    auto second = graph.CreateConstants(
        Span<const uint8>(reinterpret_cast<const uint8*>(quarter), sizeof(quarter)), "Quarter");
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    if (!first || !second || !target) {
        return false;
    }

    const GraphConstantBuffer a = first.Value();
    const GraphConstantBuffer b = second.Value();
    const ShaderBindingMap* map = &view.map;
    GraphPassAccesses accesses(graph);
    accesses.RenderTarget(target.Value());
    auto drawn = graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                               accesses, [a, map](GraphPassContext& pass) noexcept {
                                   (void)pass.SetConstants(*map, a);
                                   pass.Commands().Draw(3);
                               });
    GraphPassAccesses again(graph);
    again.RenderTarget(target.Value());
    auto second2 = graph.AddPass(
        SecondName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, again,
        [b](GraphPassContext& pass) noexcept {
            pass.Commands().SetConstantBuffer(graphics::ShaderStage::Vertex, 0, pass.Constants(b));
            const float tint[4] = {0.75f, 0.0f, 0.0f, 1.0f};
            (void)pass.Commands().SetConstantData(
                graphics::ShaderStage::Pixel, 1,
                Span<const uint8>(reinterpret_cast<const uint8*>(tint), sizeof(tint)));
            pass.Commands().Draw(4);
        });
    return drawn && second2;
}

bool IsCreation(RecordingDevice::Op op) noexcept {
    return op == RecordingDevice::Op::CreateTexture || op == RecordingDevice::Op::CreateBuffer ||
           op == RecordingDevice::Op::CreateTextureView;
}

} // namespace

TEST(RenderGraph_ConstantsAreCreatedAtExecuteAndRetiredAtTheEnd) {
    ViewMap view;
    GraphBench bench;
    {
        RenderGraph graph(bench.Next());
        REQUIRE(BuildGraph(graph, view));
        // Deferred는 실행 시작까지 장치 버퍼를 만들지 않는다.
        CHECK(bench.Count(RecordingDevice::Op::CreateConstantBuffer) == 0);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
    }

    CHECK(bench.Count(RecordingDevice::Op::CreateConstantBuffer) == 2);
    CHECK(bench.Count(RecordingDevice::Op::RetireConstantBuffer) == 2);
    CHECK(bench.device.LiveConstantBuffers() == 0);
    // 생성은 첫 기록(BeginPass) 앞이고 Retire는 마지막 제출 뒤다.
    CHECK(bench.IndexOf(RecordingDevice::Op::CreateConstantBuffer, 1) <
          bench.IndexOf(RecordingDevice::Op::BeginOffscreen));
    // 첫 pass는 지도의 자리(PS slot 3)에 Half를, 둘째는 VS slot 0에 Quarter와 PS slot 1 기록 명령.
    const auto valueOf = [&](RecordingDevice::Op op, usize nth) noexcept {
        return bench.device.Calls()[bench.IndexOf(op, nth)].value;
    };
    CHECK(valueOf(RecordingDevice::Op::SetConstantBuffer, 0) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 3, WordOf(0.5f)));
    CHECK(valueOf(RecordingDevice::Op::SetConstantBuffer, 1) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Vertex, 0, WordOf(0.25f)));
    CHECK(valueOf(RecordingDevice::Op::SetConstantData, 0) ==
          RecordingDevice::ConstantValue(graphics::ShaderStage::Pixel, 1, WordOf(0.75f)));
    REQUIRE(bench.device.WaitIdle());
    CHECK(bench.device.PendingRetireCount() == 0);

    // Immediate는 선언 때 만든다.
    {
        RenderGraph graph(bench.Next(GraphMode::Immediate));
        const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        const usize before = bench.Count(RecordingDevice::Op::CreateConstantBuffer);
        auto made = graph.CreateConstants(
            Span<const uint8>(reinterpret_cast<const uint8*>(one), sizeof(one)), "One");
        REQUIRE(made);
        CHECK(bench.Count(RecordingDevice::Op::CreateConstantBuffer) == before + 1);
        REQUIRE(graph.Execute());
    }

    CHECK(bench.device.LiveConstantBuffers() == 0);
}

TEST(RenderGraph_ConstantsRejectForeignHandlesAndBadSizes) {
    ViewMap view;
    GraphBench bench;
    RenderGraph other(bench.Next());
    const float one[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    auto foreign = other.CreateConstants(
        Span<const uint8>(reinterpret_cast<const uint8*>(one), sizeof(one)), "Foreign");
    REQUIRE(foreign);
    RenderGraph graph(bench.Next());
    // 16 byte 배수가 아닌 크기는 검증 오류이고 그 뒤 Execute는 실패한다.
    CHECK(graph.CreateConstants(Span<const uint8>(reinterpret_cast<const uint8*>(one), 12), "Odd")
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(graph.LastError(), "Odd"));
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    REQUIRE(target);
    GraphPassAccesses accesses(graph);
    accesses.RenderTarget(target.Value());
    bool foreignNull = false;
    bool rejected = false;
    const GraphConstantBuffer stranger = foreign.Value();
    const ShaderBindingMap* map = &view.map;
    (void)graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, accesses,
                        [&, stranger, map](GraphPassContext& pass) noexcept {
                            foreignNull = pass.Constants(stranger).IsNull();
                            rejected = pass.SetConstants(*map, stranger).Error() ==
                                       ErrorCode::InvalidArgument;
                        });
    CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    CHECK(other.Execute());
    // 검증 오류 그래프는 pass를 실행하지 않는다(위 람다 결과는 거짓 그대로다).
    CHECK(!foreignNull && !rejected);

    // 다른 그래프 handle을 실행 중에 묶으면 null이고 SetConstants는 거부한다.
    RenderGraph third(bench.Next());
    auto thirdTarget = third.CreateTexture(TargetDesc(), "Third");
    REQUIRE(thirdTarget);
    GraphPassAccesses thirdAccesses(third);
    thirdAccesses.RenderTarget(thirdTarget.Value());
    REQUIRE(third.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                          thirdAccesses, [&, stranger, map](GraphPassContext& pass) noexcept {
                              foreignNull = pass.Constants(stranger).IsNull();
                              rejected = pass.SetConstants(*map, stranger).Error() ==
                                         ErrorCode::InvalidArgument;
                          }));
    REQUIRE(third.Execute());
    CHECK(foreignNull && rejected);
}

TEST(RenderGraph_ParallelRecordingReplaysConstantCommands) {
    ViewMap view;
    GraphBench serial;
    {
        RenderGraph graph(serial.Next());
        REQUIRE(BuildGraph(graph, view));
        REQUIRE(graph.Execute());
    }

    GraphBench parallel;
    WorkerScheduler scheduler(parallel.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = Workers;
    REQUIRE(scheduler.Start(config));
    uint32 parallelPasses = 0;
    {
        RenderGraphDesc desc = parallel.Next();
        desc.scheduler = &scheduler;
        desc.scratch = &parallel.allocator;
        RenderGraph graph(desc);
        REQUIRE(BuildGraph(graph, view));
        REQUIRE(graph.Execute());
        parallelPasses = graph.Stats().parallelPasses;
    }

    REQUIRE(scheduler.Shutdown());
    CHECK(parallelPasses == 2);
    Span<const RecordingDevice::Call> left = serial.device.Calls();
    Span<const RecordingDevice::Call> right = parallel.device.Calls();
    usize leftIndex = 0;
    usize rightIndex = 0;
    bool same = true;
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
    }

    CHECK(same);
    CHECK(parallel.Count(RecordingDevice::Op::SetConstantData) == 1 &&
          parallel.Count(RecordingDevice::Op::SetConstantBuffer) == 2);
}

} // namespace render_graph_constant_tests_file
