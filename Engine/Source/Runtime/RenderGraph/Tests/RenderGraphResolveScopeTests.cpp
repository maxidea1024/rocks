#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG1 남은 것 설계 2 시험이다(Docs/Design/RenderBase 8절): 그래프 attachment resolve(대상
// ResolveDestination 접근, 장치가 낸 전이, 다음 읽기의 before)와 그래프 구간 GraphScope(중첩,
// culled 구간 생략, 기록 경계의 marker 재push, 짝 검증).
using namespace rocks;
namespace render_graph_resolve_scope_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;

namespace {

TraceScopeName DrawName("ResolveScope.Draw");
TraceScopeName ReadName("ResolveScope.Read");
TraceScopeName UnusedName("ResolveScope.Unused");
// 구간 이름은 marker 기록 값(이름 byte 수)으로 알아보도록 길이가 서로 다르다.
TraceScopeName OuterName("Outer");
TraceScopeName InnerName("InnerScope");
TraceScopeName LonelyName("LonelyScopeName");

// 4x MSAA 렌더 타깃이다.
constexpr uint32 MsaaSamples = 4;

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

graphics::TextureDesc MsaaDesc() noexcept {
    graphics::TextureDesc desc = TargetDesc();
    desc.sampleCount = MsaaSamples;
    return desc;
}

usize NameBytes(const TraceScopeName& name) noexcept {
    return StringView(name.Name()).ByteCount();
}

} // namespace

// MSAA pass가 1표본 그래프 텍스처로 resolve하고 다음 pass가 그것을 SRV로 읽는다. resolve 대상
// 전이는 장치 몫(attachment)이고, 다음 읽기 전이의 before는 ResolveDestination이다.
TEST(RenderGraph_ResolveTargetsAreDeviceTransitions) {
    GraphBench bench;
    uint32 resolved = GraphIndexNone;
    {
        RenderGraph graph(bench.Next());
        auto msaa = graph.CreateTexture(MsaaDesc(), "Msaa");
        auto single = graph.CreateTexture(TargetDesc(), "Single");
        auto output = graph.CreateTexture(TargetDesc(), "Output");
        REQUIRE(msaa && single && output);
        resolved = single.Value().index;
        GraphColorTarget color;
        color.texture = msaa.Value();
        color.resolveTarget = single.Value();
        GraphPassAccesses draw(graph);
        draw.ColorTarget(color);
        REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, draw, Draw()));
        GraphPassAccesses read(graph);
        read.Texture(single.Value(), ResourceAccess::SrvGraphics).RenderTarget(output.Value());
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, read,
                              Draw()));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
        bool attachment = false;
        bool readTransition = false;
        for (const GraphTransitionRecord& transition : graph.Transitions()) {
            if (transition.resource != resolved) {
                continue;
            }

            attachment = attachment || (transition.pass == 0 && transition.attachment &&
                                        transition.after == ResourceAccess::ResolveDestination);
            readTransition =
                readTransition || (transition.pass == 1 && !transition.attachment &&
                                   transition.before == ResourceAccess::ResolveDestination &&
                                   transition.after == ResourceAccess::SrvGraphics);
        }

        CHECK(attachment);
        CHECK(readTransition);
    }

    // 장치 추적 상태와 그래프의 before가 맞았다(기록 장치는 다르면 검증 오류를 센다).
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// resolve 규칙 위반: 1표본 원본, MSAA 대상, 3D 대상, 같은 대상 subresource 둘, 렌더 타깃이 아닌
// 대상.
TEST(RenderGraph_RejectsInvalidResolveTargets) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto msaa = graph.CreateTexture(MsaaDesc(), "Msaa");
    auto other = graph.CreateTexture(MsaaDesc(), "OtherMsaa");
    auto single = graph.CreateTexture(TargetDesc(), "Single");
    graphics::TextureDesc volumeDesc = TargetDesc();
    volumeDesc.dimension = graphics::TextureDimension::Texture3D;
    volumeDesc.depth = 2;
    auto volume = graph.CreateTexture(volumeDesc, "Volume");
    graphics::TextureDesc storageDesc = TargetDesc();
    storageDesc.format = graphics::PixelFormat::R32Float;
    storageDesc.usage = graphics::TextureUsage::Storage;
    auto storage = graph.CreateTexture(storageDesc, "Storage");
    REQUIRE(msaa && other && single && volume && storage);
    const auto reject = [&](GraphTexture source, GraphTexture target, bool twice) noexcept {
        GraphColorTarget color;
        color.texture = source;
        color.resolveTarget = target;
        GraphPassAccesses accesses(graph);
        accesses.ColorTarget(color);
        if (twice) {
            GraphColorTarget second = color;
            second.texture = other.Value();
            accesses.ColorTarget(second);
        }

        return graph
                   .AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, accesses, Draw())
                   .Error() == ErrorCode::InvalidArgument;
    };
    CHECK(reject(single.Value(), single.Value(), false));
    CHECK(reject(msaa.Value(), other.Value(), false));
    CHECK(reject(msaa.Value(), volume.Value(), false));
    CHECK(reject(msaa.Value(), single.Value(), true));
    CHECK(reject(msaa.Value(), storage.Value(), false));
    CHECK(graph.Stats().validationErrors == 5);
    // resolve 대상은 선언 접근으로 고를 수 없다(렌더 타깃 binding으로만).
    GraphPassAccesses declared(graph);
    declared.Texture(single.Value(), ResourceAccess::ResolveDestination).RenderTarget(msaa.Value());
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, declared, Draw())
              .Error() == ErrorCode::InvalidArgument);
}

// 구간: Outer { Inner { p0 }, p1 }, p2(구간 밖), Lonely { culled pass }. 장치 기록은 Raster
// pass마다 열리므로 Outer marker는 p0·p1 기록마다 다시 push되고, p2 기록에는 없다. culled pass만
// 있는 Lonely는 남지 않는다.
TEST(RenderGraph_ScopesFollowRecordingsAndSkipCulledPasses) {
    GraphBench bench;
    GpuTraceSettings settings;
    settings.markers = true;
    GpuTrace trace(bench.allocator, settings);
    {
        RenderGraph graph(bench.Next(GraphMode::Deferred, &trace));
        auto first = graph.CreateTexture(TargetDesc(), "First");
        auto second = graph.CreateTexture(TargetDesc(), "Second");
        auto third = graph.CreateTexture(TargetDesc(), "Third");
        auto unused = graph.CreateTexture(TargetDesc(), "Unused");
        REQUIRE(first && second && third && unused);
        {
            GraphScope outer(graph, OuterName, 7);
            {
                GraphScope inner(graph, InnerName);
                GraphPassAccesses a0(graph);
                a0.RenderTarget(first.Value());
                REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, a0,
                                      Draw()));
            }

            GraphPassAccesses a1(graph);
            a1.Texture(first.Value(), ResourceAccess::SrvGraphics).RenderTarget(second.Value());
            REQUIRE(
                graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::None, a1, Draw()));
        }

        GraphPassAccesses a2(graph);
        a2.Texture(second.Value(), ResourceAccess::SrvGraphics).RenderTarget(third.Value());
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a2,
                              Draw()));
        {
            GraphScope lonely(graph, LonelyName);
            GraphPassAccesses unusedAccesses(graph);
            unusedAccesses.RenderTarget(unused.Value());
            REQUIRE(graph.AddPass(UnusedName, GraphPassKind::Raster, GraphPassFlags::None,
                                  unusedAccesses, Draw()));
        }

        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
        CHECK(graph.Stats().executedPasses == 3);
    }

    // 기록마다 marker 열(push는 이름 byte 수, pop은 pop 뒤 깊이)을 모은다.
    struct Marker {
        bool push = false;
        uint64 value = 0;
    };
    Marker markers[3][8];
    usize counts[3] = {};
    uint32 recording = 0;
    bool started = false;
    for (const RecordingDevice::Call& call : bench.device.Calls()) {
        if (call.op == RecordingDevice::Op::BeginOffscreen) {
            recording = started ? recording + 1 : 0;
            started = true;
        } else if (started && recording < 3 &&
                   (call.op == RecordingDevice::Op::PushMarker ||
                    call.op == RecordingDevice::Op::PopMarker) &&
                   counts[recording] < 8) {
            markers[recording][counts[recording]++] = {call.op == RecordingDevice::Op::PushMarker,
                                                       call.value};
        }
    }

    // 기록 0: Outer, Inner, p0, pop p0, pop Inner, pop Outer.
    REQUIRE(counts[0] == 6);
    CHECK(markers[0][0].push && markers[0][0].value == NameBytes(OuterName));
    CHECK(markers[0][1].push && markers[0][1].value == NameBytes(InnerName));
    CHECK(markers[0][2].push && markers[0][2].value == NameBytes(DrawName));
    CHECK(!markers[0][3].push && markers[0][3].value == 2);
    CHECK(!markers[0][4].push && markers[0][4].value == 1);
    CHECK(!markers[0][5].push && markers[0][5].value == 0);
    // 기록 1: Outer를 다시 push하고 p1을 그 안에 둔다.
    REQUIRE(counts[1] == 4);
    CHECK(markers[1][0].push && markers[1][0].value == NameBytes(OuterName));
    CHECK(markers[1][1].push && markers[1][1].value == NameBytes(ReadName));
    CHECK(!markers[1][3].push && markers[1][3].value == 0);
    // 기록 2: 구간 밖 p2만 있다(Lonely는 culled pass뿐이라 없다).
    REQUIRE(counts[2] == 2);
    CHECK(markers[2][0].push && markers[2][0].value == NameBytes(ReadName));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 짝 없는 PopScope와 열린 채 Execute는 검증 오류다.
TEST(RenderGraph_ScopeImbalanceIsAValidationError) {
    GraphBench bench;
    {
        RenderGraph graph(bench.Next());
        graph.PopScope();
        CHECK(ContainsText(graph.LastError(), "PopScope has no matching PushScope"));
        CHECK(graph.Stats().validationErrors == 1);
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    {
        RenderGraph graph(bench.Next());
        graph.PushScope(OuterName);
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "graph scope 'Outer' is still open at Execute"));
    }
}

// 즉시 실행 모드도 같은 구간 marker를 낸다(AddPass 때 연다).
TEST(RenderGraph_ImmediateModeOpensScopesAtAddPass) {
    GraphBench bench;
    GpuTraceSettings settings;
    settings.markers = true;
    GpuTrace trace(bench.allocator, settings);
    {
        RenderGraph graph(bench.Next(GraphMode::Immediate, &trace));
        auto target = graph.CreateTexture(TargetDesc(), "Target");
        REQUIRE(target);
        GraphScope outer(graph, OuterName);
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(target.Value());
        REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              accesses, Draw()));
    }

    usize outerPushes = 0;
    for (const RecordingDevice::Call& call : bench.device.Calls()) {
        outerPushes +=
            call.op == RecordingDevice::Op::PushMarker && call.value == NameBytes(OuterName) ? 1u
                                                                                             : 0u;
    }

    CHECK(outerPushes == 1);
    CHECK(bench.Count(RecordingDevice::Op::PushMarker) ==
          bench.Count(RecordingDevice::Op::PopMarker));
}
} // namespace render_graph_resolve_scope_tests_file
