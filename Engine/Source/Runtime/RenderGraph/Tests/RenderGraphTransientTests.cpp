#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG5 transient aliasing 시험이다(Docs/Design/RenderBase.ko.md 8절 "GX12·RG5 transient aliasing
// 설계"). 기록 장치에 transient heap을 공개하고 할당 순서·aliasing·획득·heap 재사용을 결정적으로
// 단언한다. 기록 장치는 비활성 placed 자원 사용(획득 빠짐, 다른 자원이 같은 구간을 획득한 뒤의
// 사용)을 검증 오류로 센다.
using namespace rocks;
namespace render_graph_transient_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::QueueKind;
using graphics::ResourceAccess;
using Op = RecordingDevice::Op;

namespace {

TraceScopeName FirstName("Transient.First");
TraceScopeName SecondName("Transient.Second");
TraceScopeName ThirdName("Transient.Third");
TraceScopeName AsyncName("Transient.Async");

// 시험 heap 크기다(작게 두어 heap 하나에 다 든다).
constexpr uint64 TestHeapBytes = uint64{1} << 20;

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

GraphResourcePoolConfig SmallHeaps() noexcept {
    GraphResourcePoolConfig config;
    config.transientHeapBytes = TestHeapBytes;
    return config;
}

struct Chain {
    GraphTexture first;
    GraphTexture second;
    GraphTexture third;
};

// 사슬: P0가 A(8×8)를, P1이 A를 읽어 B(16×16)를, P2가 B를 읽어 C(8×8)를 그린다. A는 P1 뒤에
// 놓이므로 C가 A의 구간을 이어 받는다(aliasing). A·C는 desc가 같지만 RG2 재사용 대신 heap 구간이다.
Chain BuildChain(RenderGraph& graph) noexcept {
    Chain chain;
    auto first = graph.CreateTexture(TargetDesc(8), "First");
    auto second = graph.CreateTexture(TargetDesc(16), "Second");
    auto third = graph.CreateTexture(TargetDesc(8), "Third");
    if (!first || !second || !third) {
        return chain;
    }

    chain = {first.Value(), second.Value(), third.Value()};
    GraphPassAccesses p0(graph);
    p0.RenderTarget(chain.first);
    (void)graph.AddPass(FirstName, GraphPassKind::Raster, GraphPassFlags::None, p0, Draw());
    GraphPassAccesses p1(graph);
    p1.Texture(chain.first, ResourceAccess::SrvGraphics).RenderTarget(chain.second);
    (void)graph.AddPass(SecondName, GraphPassKind::Raster, GraphPassFlags::None, p1, Draw());
    GraphPassAccesses p2(graph);
    p2.Texture(chain.second, ResourceAccess::SrvGraphics).RenderTarget(chain.third);
    (void)graph.AddPass(ThirdName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, p2,
                        Draw());
    return chain;
}

bool IsCreationOrAliasing(Op op) noexcept {
    return op == Op::CreateTexture || op == Op::CreateBuffer || op == Op::CreateTextureView ||
           op == Op::CreateTransientHeap || op == Op::CreatePlaced || op == Op::Acquire;
}

} // namespace

// 할당 순서와 aliasing: A·B는 동시에 살아 따로이고, C는 A가 놓은 구간을 이어 받는다. 자원마다
// 첫 사용에 획득이 있고 기록 장치의 alias 겹침 검사가 오류 0이다.
TEST(RenderGraph_TransientResourcesAliasInPassOrder) {
    GraphBench bench(SmallHeaps());
    bench.device.SetTransientHeaps(true);
    RenderGraph graph(bench.Next());
    const Chain chain = BuildChain(graph);
    REQUIRE(!chain.third.IsNull());
    REQUIRE(graph.Execute());
    const GraphStats stats = graph.Stats();
    CHECK(stats.transientResources == 3);
    CHECK(stats.aliasedResources == 1);
    CHECK(stats.transientHeaps == 1);
    CHECK(stats.transientPeakBytes < stats.transientBytes);
    CHECK(stats.reusedWithinGraph == 0);
    CHECK(bench.Count(Op::CreateTransientHeap) == 1);
    // C는 A와 같은 heap 자리·desc지만 같은 그래프 안이라 다른 placed 객체다(UE 자원 캐시와 같다).
    CHECK(bench.Count(Op::CreatePlaced) == 3);
    CHECK(bench.Count(Op::Acquire) == 3);
    CHECK(bench.device.ValidationErrorCount() == 0);
    // 첫 전이는 획득이고 before는 Discard다(attachment 전이라 장치 BeginPass가 냈다).
    uint32 acquires = 0;
    for (const GraphTransitionRecord& record : graph.Transitions()) {
        if (record.acquire) {
            CHECK(record.before == ResourceAccess::Discard);
            CHECK(record.attachment);
            ++acquires;
        }
    }

    CHECK(acquires == 3);
}

// heap 재사용은 두 queue의 마지막 사용이 끝난 뒤뿐이다: 완료를 미룬 동안 다음 그래프는 heap을 새로
// 만들고, 완료 뒤에는 heap과 placed 자원을 다시 쓴다(장치 생성 없음).
TEST(RenderGraph_TransientHeapsWaitForTheirFences) {
    GraphBench bench(SmallHeaps());
    bench.device.SetTransientHeaps(true);
    bench.device.HoldCompletions(true);
    {
        RenderGraph graph(bench.Next());
        BuildChain(graph);
        REQUIRE(graph.Execute());
    }

    {
        RenderGraph graph(bench.Next());
        BuildChain(graph);
        REQUIRE(graph.Execute());
    }

    CHECK(bench.Count(Op::CreateTransientHeap) == 2);
    CHECK(bench.Count(Op::CreatePlaced) == 6);
    bench.device.CompleteAll();
    {
        RenderGraph graph(bench.Next());
        BuildChain(graph);
        REQUIRE(graph.Execute());
    }

    CHECK(bench.Count(Op::CreateTransientHeap) == 2);
    CHECK(bench.Count(Op::CreatePlaced) == 6);
    CHECK(bench.pool.Stats().transientHeaps == 2);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// aliasing 끔·켬: 장치 호출 열이 생성·획득 말고 같고(풀은 RG2 재사용), 장치에 transient heap이
// 없거나 즉시 실행 모드면 풀이다.
TEST(RenderGraph_TransientAliasingOffMatchesPool) {
    struct Ops {
        Op ops[RecordingDevice::MaxCalls] = {};
        usize count = 0;
    };

    const auto run = [](bool heaps, bool aliasing, Ops& out, GraphStats& stats) noexcept {
        GraphBench bench(SmallHeaps());
        bench.device.SetTransientHeaps(heaps);
        RenderGraphDesc desc = bench.Next();
        desc.transientAliasing = aliasing;
        RenderGraph graph(desc);
        BuildChain(graph);
        const bool executed = graph.Execute().IsOk();
        stats = graph.Stats();
        out.count = 0;
        for (const RecordingDevice::Call& call : bench.device.Calls()) {
            if (!IsCreationOrAliasing(call.op)) {
                out.ops[out.count++] = call.op;
            }
        }

        return executed && bench.device.ValidationErrorCount() == 0;
    };

    Ops on;
    Ops off;
    Ops none;
    GraphStats onStats;
    GraphStats offStats;
    GraphStats noneStats;
    CHECK(run(true, true, on, onStats));
    CHECK(run(true, false, off, offStats));
    CHECK(run(false, true, none, noneStats));
    CHECK(onStats.transientResources == 3);
    CHECK(offStats.transientResources == 0);
    CHECK(noneStats.transientResources == 0);
    CHECK(on.count == off.count && off.count == none.count);
    for (usize index = 0; index < on.count && index < off.count; ++index) {
        CHECK(on.ops[index] == off.ops[index]);
    }

    GraphBench bench(SmallHeaps());
    bench.device.SetTransientHeaps(true);
    RenderGraph graph(bench.Next(GraphMode::Immediate));
    BuildChain(graph);
    REQUIRE(graph.Execute());
    CHECK(graph.Stats().transientResources == 0);
}

// async compute 자원은 기본으로 aliasing하지 않고(1차), 설정으로 켜면 fork·join 수명으로 heap에
// 둔다(획득은 fork의 graphics 기록에서).
TEST(RenderGraph_TransientAsyncComputeAliasingIsOptIn) {
    const bool optIns[] = {false, true};
    for (const bool optIn : optIns) {
        GraphBench bench(SmallHeaps());
        bench.device.SetTransientHeaps(true);
        bench.device.SetAsyncCompute(true);
        RenderGraphDesc desc = bench.Next();
        desc.asyncCompute = GraphAsyncCompute::Always;
        desc.asyncComputeAliasing = optIn;
        RenderGraph graph(desc);
        auto buffer = graph.CreateBuffer(
            [] {
                graphics::BufferDesc desc = BufferOf(256);
                desc.usage = graphics::BufferUsage::ShaderWrite;
                return desc;
            }(),
            "AsyncBuffer");
        REQUIRE(buffer);
        GraphPassAccesses accesses(graph);
        accesses.Buffer(buffer.Value(), ResourceAccess::UavCompute);
        (void)graph.AddPass(AsyncName, GraphPassKind::AsyncCompute, GraphPassFlags::HasSideEffects,
                            accesses,
                            [](GraphPassContext& pass) noexcept { pass.Commands().Dispatch(1); });
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().transientResources == (optIn ? 1u : 0u));
        CHECK(bench.Count(Op::Acquire) == (optIn ? 1u : 0u));
        CHECK(bench.device.ValidationErrorCount() == 0);
    }
}

// 기록 장치의 alias 겹침 검출 대조: 같은 구간의 두 placed 자원에서 B를 획득한 뒤 A를 쓰면 검증
// 오류이고, 획득하지 않은 placed 자원을 써도 검증 오류다.
TEST(RenderGraph_TransientRecordingDeviceDetectsAliasOverlap) {
    GraphBench bench;
    bench.device.SetTransientHeaps(true);
    auto placement = bench.device.GetTexturePlacement(TargetDesc(8));
    REQUIRE(placement);
    auto heap = bench.device.CreateTransientHeap({TestHeapBytes, placement.Value().kind});
    REQUIRE(heap);
    auto first = bench.device.CreatePlacedTexture(heap.Value(), 0, TargetDesc(8));
    auto second = bench.device.CreatePlacedTexture(heap.Value(), 0, TargetDesc(8));
    REQUIRE(first && second);
    CHECK(bench.device.CreatePlacedTexture(heap.Value(), 1, TargetDesc(8)).Error() ==
          ErrorCode::InvalidArgument);
    const auto transition = [&](graphics::TextureHandle texture, bool acquire,
                                ResourceAccess before, ResourceAccess after) noexcept {
        graphics::ResourceTransition change;
        change.texture = texture;
        change.before = before;
        change.after = after;
        change.acquire = acquire;
        return change;
    };
    auto commands = bench.device.BeginPass(graphics::PassDesc{});
    REQUIRE(commands);
    const graphics::ResourceTransition steps[] = {
        transition(first.Value(), true, ResourceAccess::Discard, ResourceAccess::CopySource),
        transition(second.Value(), true, ResourceAccess::Discard, ResourceAccess::CopyDestination),
    };
    commands.Value()->Transition(Span<const graphics::ResourceTransition>(steps));
    CHECK(bench.device.ValidationErrorCount() == 0);
    // second가 같은 구간을 획득했으므로 first는 비활성이다.
    const graphics::ResourceTransition stale =
        transition(first.Value(), false, ResourceAccess::CopySource, ResourceAccess::SrvGraphics);
    commands.Value()->Transition(Span<const graphics::ResourceTransition>(&stale, 1));
    CHECK(bench.device.ValidationErrorCount() == 1);
    // 획득하지 않은 placed 자원을 써도 오류다.
    auto third =
        bench.device.CreatePlacedTexture(heap.Value(), placement.Value().bytes * 2, TargetDesc(8));
    REQUIRE(third);
    const graphics::ResourceTransition fresh =
        transition(third.Value(), false, ResourceAccess::Unknown, ResourceAccess::SrvGraphics);
    commands.Value()->Transition(Span<const graphics::ResourceTransition>(&fresh, 1));
    CHECK(bench.device.ValidationErrorCount() == 2);
    REQUIRE(bench.device.EndFrame());
}

} // namespace render_graph_transient_tests_file
