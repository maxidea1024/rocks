#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG1 완료 기준 (a)–(d)의 기록 backend 시험이다(Docs/Design/RenderBase.ko.md 8절).
using namespace rocks;
using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;

namespace {

TraceScopeName PassNames[] = {TraceScopeName("Graph.Pass0"), TraceScopeName("Graph.Pass1"),
                              TraceScopeName("Graph.Pass2"), TraceScopeName("Graph.Pass3"),
                              TraceScopeName("Graph.Pass4"), TraceScopeName("Graph.Pass5"),
                              TraceScopeName("Graph.Pass6"), TraceScopeName("Graph.Pass7")};

PassLog passLog;

// 실행되면 순번을 기록하고 삼각형 하나를 그리는 pass 함수를 만든다.
auto Logged(uint32 id) noexcept {
    return [id](GraphPassContext& pass) noexcept {
        passLog.Add(id);
        pass.Commands().Draw(3);
    };
}

// 실행되면 texture를 셰이더로 읽고 그리는 pass 함수를 만든다.
auto Sampling(uint32 id, GraphTexture texture) noexcept {
    return [id, texture](GraphPassContext& pass) noexcept {
        passLog.Add(id);
        pass.Commands().SetTexture(pass.Texture(texture));
        pass.Commands().Draw(3);
    };
}

bool SameRecord(const GraphTransitionRecord& record, uint32 pass, uint32 resource,
                ResourceAccess before, ResourceAccess after, bool attachment, uint32 firstMip = 0,
                uint32 mipCount = 1) noexcept {
    return record.pass == pass && record.resource == resource && record.before == before &&
           record.after == after && record.attachment == attachment &&
           record.firstMip == firstMip && record.mipCount == mipCount;
}

} // namespace

// (a) 미사용 체인 제거와 HasSideEffects·외부 출력·export 보존.
TEST(RenderGraph_CullsUnusedChainsAndKeepsOutputs) {
    GraphBench bench;
    passLog.Reset();
    const RenderHandle external = bench.ExternalTarget();
    const usize createdBefore = bench.Count(RecordingDevice::Op::CreateTexture);

    RenderGraph graph(bench.Next());
    auto unusedA = graph.CreateTexture(TargetDesc(), "UnusedA");
    auto unusedB = graph.CreateTexture(TargetDesc(), "UnusedB");
    auto history = graph.CreateTexture(TargetDesc(), "History");
    auto debug = graph.CreateTexture(TargetDesc(), "Debug");
    auto output = graph.ImportTexture(external, "Output");
    REQUIRE(unusedA && unusedB && history && debug && output);

    // 0 → 1: 아무도 쓰지 않는 체인이다.
    GraphPassAccesses a0(graph);
    a0.RenderTarget(unusedA.Value());
    REQUIRE(
        graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::None, a0, Logged(0)));
    GraphPassAccesses a1(graph);
    a1.Texture(unusedA.Value(), ResourceAccess::SrvGraphics).RenderTarget(unusedB.Value());
    REQUIRE(graph.AddPass(PassNames[1], GraphPassKind::Raster, GraphPassFlags::None, a1,
                          Sampling(1, unusedA.Value())));
    // 2: export로 살린다(Execute 때).
    GraphPassAccesses a2(graph);
    a2.RenderTarget(history.Value());
    auto historyPass =
        graph.AddPass(PassNames[2], GraphPassKind::Raster, GraphPassFlags::None, a2, Logged(2));
    REQUIRE(historyPass);
    // export 전에는 아직 아무도 기대지 않는다(점진 culling: export 뿌리는 Execute 때 정한다).
    CHECK(graph.IsCulled(historyPass.Value()));
    // 3: import 자원에 쓰는 외부 출력이다(AddPass 때 살아난다).
    GraphPassAccesses a3(graph);
    a3.RenderTarget(output.Value());
    auto outputPass =
        graph.AddPass(PassNames[3], GraphPassKind::Raster, GraphPassFlags::None, a3, Logged(3));
    REQUIRE(outputPass);
    CHECK(!graph.IsCulled(outputPass.Value()));
    // 4: 부수 효과 pass다.
    GraphPassAccesses a4(graph);
    a4.RenderTarget(debug.Value());
    auto sidePass = graph.AddPass(PassNames[4], GraphPassKind::Raster,
                                  GraphPassFlags::HasSideEffects, a4, Logged(4));
    REQUIRE(sidePass);
    CHECK(!graph.IsCulled(sidePass.Value()));
    // 5: 읽기만 하는 compute pass는 결과가 없으므로 제거된다.
    GraphPassAccesses a5(graph);
    a5.Texture(unusedB.Value(), ResourceAccess::SrvCompute);
    REQUIRE(
        graph.AddPass(PassNames[5], GraphPassKind::Compute, GraphPassFlags::None, a5, Logged(5)));
    REQUIRE(graph.ExportTexture(history.Value()));
    REQUIRE(graph.Execute());

    CHECK(graph.IsCulled(GraphPass{0}));
    CHECK(graph.IsCulled(GraphPass{1}));
    CHECK(!graph.IsCulled(GraphPass{2}));
    CHECK(!graph.IsCulled(GraphPass{3}));
    CHECK(!graph.IsCulled(GraphPass{4}));
    CHECK(graph.IsCulled(GraphPass{5}));
    const uint32 executed[] = {2, 3, 4};
    CHECK(passLog.Equals(Span<const uint32>(executed)));
    CHECK(graph.Stats().culledPasses == 3);
    CHECK(graph.Stats().executedPasses == 3);
    // 제거된 체인의 자원은 만들지 않는다(History·Debug만 풀에서 만든다).
    CHECK(bench.Count(RecordingDevice::Op::CreateTexture) == createdBefore + 2);
    CHECK(graph.Lifetime(unusedA.Value()).firstPass == GraphIndexNone);
    CHECK(graph.Lifetime(unusedB.Value()).references == 0);
}

// (a) 생산자 간선: 살린 pass가 읽는 자원의 생산자 체인도 산다.
TEST(RenderGraph_SideEffectPassKeepsItsProducerChain) {
    GraphBench bench;
    passLog.Reset();
    RenderGraph graph(bench.Next());
    auto first = graph.CreateTexture(TargetDesc(), "First");
    auto second = graph.CreateTexture(TargetDesc(), "Second");
    auto third = graph.CreateTexture(TargetDesc(), "Third");
    REQUIRE(first && second && third);
    GraphPassAccesses a0(graph);
    a0.RenderTarget(first.Value());
    REQUIRE(
        graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::None, a0, Logged(0)));
    GraphPassAccesses a1(graph);
    a1.Texture(first.Value(), ResourceAccess::SrvGraphics).RenderTarget(second.Value());
    REQUIRE(graph.AddPass(PassNames[1], GraphPassKind::Raster, GraphPassFlags::None, a1,
                          Sampling(1, first.Value())));
    CHECK(graph.IsCulled(GraphPass{0}));
    GraphPassAccesses a2(graph);
    a2.Texture(second.Value(), ResourceAccess::SrvGraphics).RenderTarget(third.Value());
    REQUIRE(graph.AddPass(PassNames[2], GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a2,
                          Sampling(2, second.Value())));
    // 부수 효과 pass를 넣는 순간 생산자 체인이 살아난다.
    CHECK(!graph.IsCulled(GraphPass{0}));
    CHECK(!graph.IsCulled(GraphPass{1}));
    REQUIRE(graph.Execute());
    const uint32 executed[] = {0, 1, 2};
    CHECK(passLog.Equals(Span<const uint32>(executed)));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// (b) 기대 전이 열: 읽기 → 읽기 전이 0, UAV → UAV는 생략 표시가 없을 때만, attachment 전이는
// 장치 Begin이 낸다.
TEST(RenderGraph_TransitionsMatchTheExpectedSequence) {
    GraphBench bench;
    passLog.Reset();
    const RenderHandle external = bench.ExternalTarget();
    RenderGraph graph(bench.Next());
    auto scene = graph.CreateTexture(TargetDesc(), "SceneColor");
    graphics::BufferDesc particlesDesc;
    particlesDesc.size = 64;
    auto particles = graph.CreateBuffer(particlesDesc, "Particles");
    GraphImportOptions importOptions;
    importOptions.initialAccess = ResourceAccess::SrvGraphics;
    importOptions.finalAccess = ResourceAccess::SrvGraphics;
    auto output = graph.ImportTexture(external, "Output", importOptions);
    REQUIRE(scene && particles && output);
    const uint32 sceneIndex = scene.Value().index;
    const uint32 particlesIndex = particles.Value().index;
    const uint32 outputIndex = output.Value().index;

    GraphPassAccesses a0(graph);
    a0.RenderTarget(scene.Value());
    REQUIRE(
        graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::None, a0, Logged(0)));
    // 1·2: 순서가 필요한 UAV 쓰기 둘이다(UAV barrier).
    for (uint32 pass = 1; pass <= 2; ++pass) {
        GraphPassAccesses uav(graph);
        uav.Buffer(particles.Value(), ResourceAccess::UavCompute);
        REQUIRE(graph.AddPass(PassNames[pass], GraphPassKind::Compute, GraphPassFlags::None, uav,
                              Logged(pass)));
    }

    // 3·4: 겹쳐도 되는 UAV 쓰기다. 3은 앞 쓰기(표시 없음) 뒤라 barrier, 4는 생략한다.
    for (uint32 pass = 3; pass <= 4; ++pass) {
        GraphPassAccesses uav(graph);
        uav.Buffer(particles.Value(), ResourceAccess::UavCompute, GraphAccessFlags::NoUavBarrier);
        REQUIRE(graph.AddPass(PassNames[pass], GraphPassKind::Compute, GraphPassFlags::None, uav,
                              Logged(pass)));
    }

    // 5: compute가 SceneColor를 읽고(뒤 pass 6의 그래픽 읽기와 한 상태로 합친다) 버퍼도 읽는다.
    GraphPassAccesses a5(graph);
    a5.Texture(scene.Value(), ResourceAccess::SrvCompute)
        .Buffer(particles.Value(), ResourceAccess::SrvCompute);
    REQUIRE(graph.AddPass(PassNames[5], GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a5,
                          Logged(5)));
    GraphPassAccesses a6(graph);
    a6.Texture(scene.Value(), ResourceAccess::SrvGraphics).RenderTarget(output.Value());
    REQUIRE(graph.AddPass(PassNames[6], GraphPassKind::Raster, GraphPassFlags::None, a6,
                          Sampling(6, scene.Value())));
    REQUIRE(graph.Execute());

    const ResourceAccess sceneRead = ResourceAccess::SrvCompute | ResourceAccess::SrvGraphics;
    Span<const GraphTransitionRecord> transitions = graph.Transitions();
    REQUIRE(transitions.Size() == 8);
    CHECK(SameRecord(transitions[0], 0, sceneIndex, ResourceAccess::Unknown,
                     ResourceAccess::RenderTarget, true));
    CHECK(SameRecord(transitions[1], 1, particlesIndex, ResourceAccess::Unknown,
                     ResourceAccess::UavCompute, false));
    CHECK(SameRecord(transitions[2], 2, particlesIndex, ResourceAccess::UavCompute,
                     ResourceAccess::UavCompute, false));
    CHECK(SameRecord(transitions[3], 3, particlesIndex, ResourceAccess::UavCompute,
                     ResourceAccess::UavCompute, false));
    CHECK(
        SameRecord(transitions[4], 5, sceneIndex, ResourceAccess::RenderTarget, sceneRead, false));
    CHECK(SameRecord(transitions[5], 5, particlesIndex, ResourceAccess::UavCompute,
                     ResourceAccess::SrvCompute, false));
    CHECK(SameRecord(transitions[6], 6, outputIndex, ResourceAccess::SrvGraphics,
                     ResourceAccess::RenderTarget, true));
    CHECK(SameRecord(transitions[7], GraphIndexNone, outputIndex, ResourceAccess::RenderTarget,
                     ResourceAccess::SrvGraphics, false));

    // 읽기 → 읽기 전이는 없다.
    for (const GraphTransitionRecord& record : transitions) {
        CHECK(!(graphics::IsReadOnlyAccess(record.before) &&
                graphics::IsReadOnlyAccess(record.after)));
    }

    // 장치에는 attachment를 뺀 같은 열이 같은 순서로 내려간다.
    Span<const RecordingDevice::TransitionCall> lowered = bench.device.TransitionLog();
    REQUIRE(lowered.Size() == 6);
    const usize expected[] = {1, 2, 3, 4, 5, 7};
    for (usize index = 0; index < lowered.Size(); ++index) {
        const GraphTransitionRecord& record = transitions[expected[index]];
        CHECK(lowered[index].before == record.before);
        CHECK(lowered[index].after == record.after);
        CHECK(lowered[index].texture == (record.kind == GraphResourceKind::Texture));
    }

    CHECK(graph.Stats().transitions == 6);
    CHECK(graph.Stats().attachmentTransitions == 2);
    // 장치가 추적한 상태와 선언이 어긋나지 않았다(검증 오류 0, 그리기는 읽을 수 있는 상태만
    // 묶었다).
    CHECK(bench.device.ValidationErrorCount() == 0);
    CHECK(bench.device.TextureAccess(bench.DeviceTexture(external)) == ResourceAccess::SrvGraphics);
    const uint32 executed[] = {0, 1, 2, 3, 4, 5, 6};
    CHECK(passLog.Equals(Span<const uint32>(executed)));
}

// (b) subresource 추적: mip 범위마다 상태를 따로 추적하고 연속 mip을 한 범위로 묶는다.
TEST(RenderGraph_TracksSubresourcesPerMip) {
    GraphBench bench;
    passLog.Reset();
    RenderGraph graph(bench.Next());
    auto chain = graph.CreateTexture(TargetDesc(8, 4), "MipChain");
    graphics::BufferDesc bufferDesc;
    bufferDesc.size = 16;
    auto sink = graph.CreateBuffer(bufferDesc, "Sink");
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    REQUIRE(chain && sink && target);
    GraphPassAccesses a0(graph);
    a0.RenderTarget(target.Value());
    REQUIRE(graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::HasSideEffects, a0,
                          Logged(0)));
    // compute가 mip 0–3 전체를 UAV로 쓴다.
    GraphPassAccesses a1(graph);
    a1.Texture(chain.Value(), ResourceAccess::UavCompute);
    REQUIRE(
        graph.AddPass(PassNames[1], GraphPassKind::Compute, GraphPassFlags::None, a1, Logged(1)));
    // mip 0은 읽고 mip 1–2는 다시 쓴다(mip마다 다른 전이).
    GraphPassAccesses a2(graph);
    a2.Texture(chain.Value(), ResourceAccess::SrvCompute, {0, 1, 0, 1})
        .Texture(chain.Value(), ResourceAccess::UavCompute, {1, 2, 0, 1})
        .Buffer(sink.Value(), ResourceAccess::UavCompute);
    REQUIRE(graph.AddPass(PassNames[2], GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a2,
                          Logged(2)));
    REQUIRE(graph.Execute());

    const uint32 chainIndex = chain.Value().index;
    Span<const GraphTransitionRecord> transitions = graph.Transitions();
    REQUIRE(transitions.Size() == 5);
    CHECK(SameRecord(transitions[1], 1, chainIndex, ResourceAccess::Unknown,
                     ResourceAccess::UavCompute, false, 0, 4));
    CHECK(SameRecord(transitions[2], 2, chainIndex, ResourceAccess::UavCompute,
                     ResourceAccess::SrvCompute, false, 0, 1));
    CHECK(SameRecord(transitions[3], 2, chainIndex, ResourceAccess::UavCompute,
                     ResourceAccess::UavCompute, false, 1, 2));
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// (c) 할당·해제 pass 번호(참조 수 수명).
TEST(RenderGraph_LifetimesFollowReferenceCounts) {
    GraphBench bench;
    passLog.Reset();
    const RenderHandle external = bench.ExternalTarget();
    {
        RenderGraph graph(bench.Next());
        auto a = graph.CreateTexture(TargetDesc(), "A");
        auto b = graph.CreateTexture(TargetDesc(), "B");
        auto c = graph.CreateTexture(TargetDesc(), "C");
        auto unused = graph.CreateTexture(TargetDesc(), "Unused");
        auto output = graph.ImportTexture(external, "Output");
        REQUIRE(a && b && c && unused && output);
        GraphPassAccesses a0(graph);
        a0.RenderTarget(a.Value());
        REQUIRE(graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::None, a0,
                              Logged(0)));
        GraphPassAccesses a1(graph);
        a1.Texture(a.Value(), ResourceAccess::SrvGraphics).RenderTarget(b.Value());
        REQUIRE(graph.AddPass(PassNames[1], GraphPassKind::Raster, GraphPassFlags::None, a1,
                              Sampling(1, a.Value())));
        // 2: 제거되는 pass다. A를 읽어도 A의 수명을 늘리지 않는다.
        GraphPassAccesses a2(graph);
        a2.Texture(a.Value(), ResourceAccess::SrvGraphics).RenderTarget(unused.Value());
        REQUIRE(graph.AddPass(PassNames[2], GraphPassKind::Raster, GraphPassFlags::None, a2,
                              Sampling(2, a.Value())));
        GraphPassAccesses a3(graph);
        a3.Texture(b.Value(), ResourceAccess::SrvGraphics).RenderTarget(c.Value());
        REQUIRE(graph.AddPass(PassNames[3], GraphPassKind::Raster, GraphPassFlags::None, a3,
                              Sampling(3, b.Value())));
        GraphPassAccesses a4(graph);
        a4.Texture(c.Value(), ResourceAccess::SrvGraphics).RenderTarget(output.Value());
        REQUIRE(graph.AddPass(PassNames[4], GraphPassKind::Raster, GraphPassFlags::None, a4,
                              Sampling(4, c.Value())));
        REQUIRE(graph.Execute());

        const GraphLifetime lifeA = graph.Lifetime(a.Value());
        CHECK(lifeA.firstPass == 0 && lifeA.lastPass == 1 && lifeA.references == 2);
        CHECK(lifeA.released);
        const GraphLifetime lifeB = graph.Lifetime(b.Value());
        CHECK(lifeB.firstPass == 1 && lifeB.lastPass == 3 && lifeB.references == 2);
        const GraphLifetime lifeC = graph.Lifetime(c.Value());
        CHECK(lifeC.firstPass == 3 && lifeC.lastPass == 4 && lifeC.released);
        const GraphLifetime lifeUnused = graph.Lifetime(unused.Value());
        CHECK(lifeUnused.firstPass == GraphIndexNone && lifeUnused.references == 0 &&
              !lifeUnused.released);
        // import 자원은 해제하지 않는다.
        const GraphLifetime lifeOutput = graph.Lifetime(output.Value());
        CHECK(lifeOutput.firstPass == 4 && lifeOutput.lastPass == 4 && !lifeOutput.released);
        CHECK(graph.Stats().createdResources == 3);
        const uint32 executed[] = {0, 1, 3, 4};
        CHECK(passLog.Equals(Span<const uint32>(executed)));
    }

    // RG1은 그래프 안에서 재사용하지 않고 그래프 끝에 풀로 돌려준다. 다음 그래프는 같은 desc를 다시
    // 쓴다(GPU 완료를 기다리지 않는다: 단일 queue).
    const GraphPoolStats pooled = bench.pool.Stats();
    CHECK(pooled.resources == 3 && pooled.inUse == 0);
    {
        RenderGraph graph(bench.Next());
        auto again = graph.CreateTexture(TargetDesc(), "Again");
        REQUIRE(again);
        GraphPassAccesses a0(graph);
        a0.RenderTarget(again.Value());
        REQUIRE(graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              a0, Logged(0)));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().reusedResources == 1 && graph.Stats().createdResources == 0);
    }

    CHECK(bench.device.ValidationErrorCount() == 0);
}

namespace {

// 즉시·지연 대조용 그래프다. readMerge면 compute 읽기와 그래픽 읽기가 이어져 지연 모드에서 한
// 상태로 합쳐진다.
void BuildEquivalenceGraph(RenderGraph& graph, RenderHandle external, bool readMerge) noexcept {
    auto scene = graph.CreateTexture(TargetDesc(), "Scene");
    auto blur = graph.CreateTexture(TargetDesc(), "Blur");
    graphics::BufferDesc bufferDesc;
    bufferDesc.size = 32;
    auto histogram = graph.CreateBuffer(bufferDesc, "Histogram");
    GraphImportOptions options;
    options.finalAccess = ResourceAccess::SrvGraphics;
    auto output = graph.ImportTexture(external, "Output", options);
    REQUIRE(scene && blur && histogram && output);

    GraphPassAccesses a0(graph);
    a0.RenderTarget(scene.Value());
    REQUIRE(
        graph.AddPass(PassNames[0], GraphPassKind::Raster, GraphPassFlags::None, a0, Logged(0)));
    GraphPassAccesses a1(graph);
    a1.Texture(scene.Value(), ResourceAccess::SrvCompute)
        .Buffer(histogram.Value(), ResourceAccess::UavCompute);
    REQUIRE(graph.AddPass(PassNames[1], GraphPassKind::Compute, GraphPassFlags::HasSideEffects, a1,
                          Logged(1)));
    GraphPassAccesses a2(graph);
    if (readMerge) {
        a2.Texture(scene.Value(), ResourceAccess::SrvGraphics);
    }

    a2.RenderTarget(blur.Value());
    if (readMerge) {
        REQUIRE(graph.AddPass(PassNames[2], GraphPassKind::Raster, GraphPassFlags::None, a2,
                              Sampling(2, scene.Value())));
    } else {
        REQUIRE(graph.AddPass(PassNames[2], GraphPassKind::Raster, GraphPassFlags::None, a2,
                              Logged(2)));
    }

    GraphPassAccesses a3(graph);
    a3.Texture(blur.Value(), ResourceAccess::SrvGraphics).RenderTarget(output.Value());
    REQUIRE(graph.AddPass(PassNames[3], GraphPassKind::Raster, GraphPassFlags::None, a3,
                          Sampling(3, blur.Value())));
    REQUIRE(graph.Execute());
}

// 전이를 뺀 장치 호출 열이 같은지다.
bool SameCallsWithoutTransitions(const RecordingDevice& left,
                                 const RecordingDevice& right) noexcept {
    usize l = 0;
    usize r = 0;
    Span<const RecordingDevice::Call> a = left.Calls();
    Span<const RecordingDevice::Call> b = right.Calls();
    for (;;) {
        while (l < a.Size() && a[l].op == RecordingDevice::Op::Transition) {
            ++l;
        }

        while (r < b.Size() && b[r].op == RecordingDevice::Op::Transition) {
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

} // namespace

// (d) 즉시·지연 모드 명령 열 동치: 전이를 뺀 장치 호출 열이 같고, 두 모드 모두 각 pass가 맞는
// 상태를 본다(검증 오류 0). 읽기 병합이 없는 그래프는 전이까지 같다.
TEST(RenderGraph_ImmediateAndDeferredModesAreEquivalent) {
    const bool variants[] = {true, false};
    for (const bool readMerge : variants) {
        GraphBench deferred;
        GraphBench immediate;
        const RenderHandle deferredOutput = deferred.ExternalTarget();
        const RenderHandle immediateOutput = immediate.ExternalTarget();
        passLog.Reset();
        uint32 deferredTransitions = 0;
        {
            RenderGraph graph(deferred.Next(GraphMode::Deferred));
            BuildEquivalenceGraph(graph, deferredOutput, readMerge);
            deferredTransitions = graph.Stats().transitions;
        }

        PassLog deferredLog = passLog;
        passLog.Reset();
        uint32 immediateTransitions = 0;
        {
            RenderGraph graph(immediate.Next(GraphMode::Immediate));
            BuildEquivalenceGraph(graph, immediateOutput, readMerge);
            immediateTransitions = graph.Stats().transitions;
        }

        CHECK(deferredLog.Equals(Span<const uint32>(passLog.values, passLog.count)));
        CHECK(SameCallsWithoutTransitions(deferred.device, immediate.device));
        CHECK(deferred.device.ValidationErrorCount() == 0);
        CHECK(immediate.device.ValidationErrorCount() == 0);
        CHECK(deferred.device.TextureAccess(deferred.DeviceTexture(deferredOutput)) ==
              immediate.device.TextureAccess(immediate.DeviceTexture(immediateOutput)));
        if (readMerge) {
            // 지연 모드는 compute 읽기와 그래픽 읽기를 한 상태로 합쳐 전이가 하나 적다.
            CHECK(deferredTransitions + 1 == immediateTransitions);
        } else {
            CHECK(deferredTransitions == immediateTransitions);
            Span<const RecordingDevice::TransitionCall> a = deferred.device.TransitionLog();
            Span<const RecordingDevice::TransitionCall> b = immediate.device.TransitionLog();
            REQUIRE(a.Size() == b.Size());
            for (usize index = 0; index < a.Size(); ++index) {
                CHECK(a[index] == b[index]);
            }

            Span<const RecordingDevice::Call> calls = deferred.device.Calls();
            REQUIRE(calls.Size() == immediate.device.Calls().Size());
            for (usize index = 0; index < calls.Size(); ++index) {
                CHECK(calls[index] == immediate.device.Calls()[index]);
            }
        }
    }
}
