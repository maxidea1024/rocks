#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG2 B부 시험이다(Docs/Design/RenderBase.ko.md 8절 RG2): 분리 전이(begin·end)의 위치가 기대 열과
// 같고, 분리를 켜고 끈 결과(장치 최종 상태·전이를 뺀 호출 열)가 같으며, 나누지 않는 경우(바로 다음
// pass, 사이의 Raster pass, Immediate 모드)를 확인한다.
using namespace rocks;
namespace render_graph_split_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;
using Op = RecordingDevice::Op;
using Phase = RecordingDevice::TransitionPhase;

namespace {

TraceScopeName SceneName("Split.Scene");
TraceScopeName ComputeName("Split.Compute");
TraceScopeName ReadName("Split.Read");

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

struct SplitGraph {
    GraphTexture scene;
    GraphBuffer first;
    GraphBuffer second;
};

// p0 Raster: scene에 그린다. p1 Compute: first에 UAV로 쓴다. p2 Compute: second에 UAV로 쓴다.
// p3 Compute: scene과 first를 읽는다. scene(p0 → p3)과 first(p1 → p3)는 사이에 pass가 있어
// 나뉘고, second(첫 사용)는 나뉘지 않는다.
SplitGraph BuildSplitGraph(RenderGraph& graph) noexcept {
    SplitGraph result;
    auto scene = graph.CreateTexture(TargetDesc(), "Scene");
    auto first = graph.CreateBuffer(BufferOf(64), "First");
    auto second = graph.CreateBuffer(BufferOf(32), "Second");
    if (!scene || !first || !second) {
        platform::Fatal("split graph setup failed");
    }

    result.scene = scene.Value();
    result.first = first.Value();
    result.second = second.Value();
    GraphPassAccesses a0(graph);
    a0.RenderTarget(scene.Value());
    GraphPassAccesses a1(graph);
    a1.Buffer(first.Value(), ResourceAccess::UavCompute);
    GraphPassAccesses a2(graph);
    a2.Buffer(second.Value(), ResourceAccess::UavCompute);
    GraphPassAccesses a3(graph);
    a3.Texture(scene.Value(), ResourceAccess::SrvCompute)
        .Buffer(first.Value(), ResourceAccess::SrvCompute);
    if (!graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, a0, Draw()) ||
        !graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::None, a1, Draw()) ||
        !graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a2,
                       Draw()) ||
        !graph.AddPass(ReadName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a3,
                       Draw())) {
        platform::Fatal("split graph pass setup failed");
    }

    return result;
}

bool IsTransitionOp(Op op) noexcept {
    return op == Op::Transition || op == Op::BeginTransitions || op == Op::EndTransitions;
}

bool SameCallsWithoutTransitions(const RecordingDevice& left,
                                 const RecordingDevice& right) noexcept {
    Span<const RecordingDevice::Call> a = left.Calls();
    Span<const RecordingDevice::Call> b = right.Calls();
    usize l = 0;
    usize r = 0;
    for (;;) {
        while (l < a.Size() && IsTransitionOp(a[l].op)) {
            ++l;
        }

        while (r < b.Size() && IsTransitionOp(b[r].op)) {
            ++r;
        }

        if (l == a.Size() || r == b.Size()) {
            return l == a.Size() && r == b.Size();
        }

        if (!(a[l] == b[r])) {
            return false;
        }

        ++l;
        ++r;
    }
}

bool SameCall(const RecordingDevice::TransitionCall& call, bool texture, Phase phase,
              ResourceAccess before, ResourceAccess after) noexcept {
    return call.texture == texture && call.phase == phase && call.before == before &&
           call.after == after;
}

} // namespace

// RG2 완료 기준: 분리 전이의 begin·end 위치가 기대 열과 같다. begin은 마지막 사용 pass의 기록 함수
// 뒤, end는 소비 pass 기록 함수 앞(다른 전이보다 먼저)이고, end pass가 같은 묶음은 begin pass별로
// 짝 하나다.
TEST(RenderGraph_SplitTransitionsMatchTheExpectedSequence) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    const SplitGraph built = BuildSplitGraph(graph);
    REQUIRE(graph.Execute());

    Span<const GraphTransitionRecord> records = graph.Transitions();
    uint32 split = 0;
    for (const GraphTransitionRecord& record : records) {
        if (record.resource == built.scene.index && record.pass == 3) {
            CHECK(record.beginPass == 0);
            ++split;
        } else if (record.resource == built.first.index && record.pass == 3) {
            CHECK(record.beginPass == 1);
            ++split;
        } else {
            CHECK(record.beginPass == GraphIndexNone);
        }
    }

    CHECK(split == 2);
    CHECK(graph.Stats().splitTransitions == 2);
    CHECK(graph.Stats().transitions == 4);

    // 장치 열: p0 Draw → begin(scene) → p1 전이(first 첫 사용) → Draw → begin(first) → p2
    // 전이(second) → Draw → end(scene) → end(first) → Draw.
    Span<const RecordingDevice::TransitionCall> log = bench.device.TransitionLog();
    REQUIRE(log.Size() == 6);
    const ResourceAccess sceneRead = ResourceAccess::SrvCompute;
    CHECK(SameCall(log[0], true, Phase::Begin, ResourceAccess::RenderTarget, sceneRead));
    CHECK(
        SameCall(log[1], false, Phase::Full, ResourceAccess::Unknown, ResourceAccess::UavCompute));
    CHECK(SameCall(log[2], false, Phase::Begin, ResourceAccess::UavCompute, sceneRead));
    CHECK(
        SameCall(log[3], false, Phase::Full, ResourceAccess::Unknown, ResourceAccess::UavCompute));
    CHECK(SameCall(log[4], true, Phase::End, ResourceAccess::RenderTarget, sceneRead));
    CHECK(SameCall(log[5], false, Phase::End, ResourceAccess::UavCompute, sceneRead));
    CHECK(log[0].split != 0 && log[0].split == log[4].split);
    CHECK(log[2].split != 0 && log[2].split == log[5].split && log[2].split != log[0].split);

    // 호출 위치: 각 begin은 그 pass의 그리기 뒤, end는 마지막 pass의 그리기 앞이다.
    const usize firstDraw = bench.IndexOf(Op::Draw, 0);
    const usize secondDraw = bench.IndexOf(Op::Draw, 1);
    const usize thirdDraw = bench.IndexOf(Op::Draw, 2);
    const usize lastDraw = bench.IndexOf(Op::Draw, 3);
    const usize beginScene = bench.IndexOf(Op::BeginTransitions, 0);
    const usize beginFirst = bench.IndexOf(Op::BeginTransitions, 1);
    const usize endScene = bench.IndexOf(Op::EndTransitions, 0);
    const usize endFirst = bench.IndexOf(Op::EndTransitions, 1);
    REQUIRE(endFirst != NumericLimits<usize>::Max());
    CHECK(firstDraw < beginScene && beginScene < secondDraw);
    CHECK(secondDraw < beginFirst && beginFirst < thirdDraw);
    CHECK(thirdDraw < endScene && endScene < endFirst && endFirst < lastDraw);
    CHECK(bench.Count(Op::BeginTransitions) == 2 && bench.Count(Op::EndTransitions) == 2);
    // 짝이 맞고 추적 상태가 선언과 어긋나지 않았다.
    CHECK(bench.device.ValidationErrorCount() == 0);
    CHECK(bench.device.IgnoredDraws() == 0);
}

// 분리 전이를 켜고 끈 결과가 같다: 전이를 뺀 장치 호출 열, 각 자원의 최종 추적 상태, 검증 오류 0.
TEST(RenderGraph_SplitTransitionsMatchUnsplitResults) {
    GraphBench split;
    GraphBench whole;
    {
        RenderGraph graph(split.Next());
        (void)BuildSplitGraph(graph);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().splitTransitions == 2);
    }

    {
        RenderGraphDesc desc = whole.Next();
        desc.splitTransitions = false;
        RenderGraph graph(desc);
        (void)BuildSplitGraph(graph);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().splitTransitions == 0);
        CHECK(graph.Stats().transitions == 4);
    }

    CHECK(whole.Count(Op::BeginTransitions) == 0 && whole.Count(Op::EndTransitions) == 0);
    CHECK(SameCallsWithoutTransitions(split.device, whole.device));
    CHECK(split.device.ValidationErrorCount() == 0);
    CHECK(whole.device.ValidationErrorCount() == 0);
    // 같은 순서로 만든 자원은 같은 slot 번호다. 최종 상태가 같다.
    Span<const RecordingDevice::TransitionCall> a = split.device.TransitionLog();
    Span<const RecordingDevice::TransitionCall> b = whole.device.TransitionLog();
    for (const RecordingDevice::TransitionCall& call : b) {
        bool found = false;
        for (const RecordingDevice::TransitionCall& other : a) {
            found = found || (other.texture == call.texture && other.index == call.index &&
                              other.before == call.before && other.after == call.after &&
                              other.phase != Phase::Begin);
        }

        CHECK(found);
    }
}

// 나누지 않는 경우: 바로 다음 pass(사이에 pass가 없다), 사이에 Raster pass(새 장치 기록), Immediate
// 모드(앞을 내다볼 수 없다).
TEST(RenderGraph_SplitTransitionsNeedAGapInOneRecording) {
    {
        GraphBench bench;
        RenderGraph graph(bench.Next());
        auto scene = graph.CreateTexture(TargetDesc(), "Scene");
        REQUIRE(scene);
        GraphPassAccesses a0(graph);
        a0.RenderTarget(scene.Value());
        REQUIRE(graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, a0, Draw()));
        GraphPassAccesses a1(graph);
        a1.Texture(scene.Value(), ResourceAccess::SrvCompute);
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a1,
                              Draw()));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().splitTransitions == 0);
    }

    {
        GraphBench bench;
        RenderGraph graph(bench.Next());
        auto scene = graph.CreateTexture(TargetDesc(), "Scene");
        auto other = graph.CreateTexture(TargetDesc(8), "Other");
        REQUIRE(scene && other);
        GraphPassAccesses a0(graph);
        a0.RenderTarget(scene.Value());
        REQUIRE(graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, a0, Draw()));
        GraphPassAccesses a1(graph);
        a1.RenderTarget(other.Value());
        REQUIRE(graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a1,
                              Draw()));
        GraphPassAccesses a2(graph);
        a2.Texture(scene.Value(), ResourceAccess::SrvCompute);
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a2,
                              Draw()));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().splitTransitions == 0);
        CHECK(bench.device.ValidationErrorCount() == 0);
    }

    {
        GraphBench bench;
        RenderGraph graph(bench.Next(GraphMode::Immediate));
        (void)BuildSplitGraph(graph);
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().splitTransitions == 0);
        CHECK(bench.Count(Op::BeginTransitions) == 0);
        CHECK(bench.device.ValidationErrorCount() == 0);
    }
}

// 기록 장치의 분리 전이 계약 검사(backend와 같은 규칙): 모르는 짝의 end는 무시 계수, begin과 다른
// 목록의 end·열린 짝의 subresource를 건드리는 전이·끝나지 않은 짝은 검증 오류다.
TEST(RenderGraph_RecordingDeviceChecksSplitTransitionPairs) {
    GraphBench bench;
    graphics::BufferDesc desc;
    desc.size = 16;
    const uint8 zeros[16] = {};
    auto created = bench.device.CreateBuffer(desc, Span<const uint8>(zeros));
    auto target = bench.device.CreateTexture(TargetDesc(), {});
    REQUIRE(created && target);
    graphics::ResourceTransition toRead;
    toRead.buffer = created.Value();
    toRead.before = ResourceAccess::VertexOrIndex;
    toRead.after = ResourceAccess::SrvCompute;
    const Span<const graphics::ResourceTransition> list(&toRead, 1);
    graphics::ResourceTransition other = toRead;
    other.after = ResourceAccess::Constant;
    const Span<const graphics::ResourceTransition> otherList(&other, 1);

    auto commands = bench.device.BeginOffscreen(target.Value(), {});
    REQUIRE(commands);
    graphics::CommandContext& context = *commands.Value();
    // 정상 짝: end에서 상태가 바뀐다.
    const graphics::SplitTransition split = context.BeginTransitions(list);
    CHECK(!split.IsNull());
    CHECK(bench.device.BufferAccess(created.Value()) == ResourceAccess::VertexOrIndex);
    context.EndTransitions(split, list);
    CHECK(bench.device.BufferAccess(created.Value()) == ResourceAccess::SrvCompute);
    CHECK(bench.device.ValidationErrorCount() == 0);
    // 이미 끝난 짝의 end: 무시 계수(Transition처럼 낮춘다).
    const uint64 ignoredBefore = bench.device.IgnoredDraws();
    toRead.before = ResourceAccess::SrvCompute;
    toRead.after = ResourceAccess::VertexOrIndex;
    context.EndTransitions(split, list);
    CHECK(bench.device.IgnoredDraws() == ignoredBefore + 1);
    // 다른 목록의 end, 열린 짝을 건드리는 전이.
    toRead.before = ResourceAccess::VertexOrIndex;
    toRead.after = ResourceAccess::SrvCompute;
    const graphics::SplitTransition mismatched = context.BeginTransitions(list);
    other.before = ResourceAccess::VertexOrIndex;
    context.Transition(otherList);
    CHECK(bench.device.ValidationErrorCount() == 1);
    // 목록 내용이 begin과 다르다(before는 지금 추적 상태라 상태 대조는 맞는다).
    other.before = ResourceAccess::Constant;
    context.EndTransitions(mismatched, otherList);
    CHECK(bench.device.ValidationErrorCount() == 2);
    // 끝나지 않은 짝: EndFrame이 센다.
    toRead.before = ResourceAccess::Constant;
    (void)context.BeginTransitions(list);
    REQUIRE(bench.device.EndFrame());
    CHECK(bench.device.ValidationErrorCount() == 3);
    bench.device.Retire(created.Value());
    bench.device.Retire(target.Value());
}
} // namespace render_graph_split_tests_file
