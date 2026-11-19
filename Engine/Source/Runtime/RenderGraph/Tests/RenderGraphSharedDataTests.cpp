#include "GraphTestSupport.h"

#include <RenderGraph/GraphSharedData.h>
#include <TestSupport/Test.h>

// RG1 남은 것 C부 시험이다(Docs/Design/RenderBase.ko.md 7.5절): GraphSharedData(타입마다 값 하나,
// 중복 생성·없는 값 필수 조회 검증 오류, 다른 그래프 handle을 담은 값 거부, 그래프 파괴 때 소멸),
// SceneTextureTable((ViewSet, view) 키), 검증 실패 그래프의 history export.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;

namespace {

TraceScopeName SceneName("Shared.Scene");
TraceScopeName ReadName("Shared.Read");

// 시험용 공유 값이다. 소멸 수를 센다.
struct ShadowSetup {
    GraphTexture atlas;
    uint32* destroyed = nullptr;

    explicit ShadowSetup(uint32* counter) noexcept : destroyed(counter) {
    }

    ~ShadowSetup() noexcept {
        if (destroyed != nullptr) {
            ++*destroyed;
        }
    }
};

struct LightSetup {
    uint32 count = 0;
};

// 렌더러의 SceneTextureSet 자리다(view마다 장면 텍스처 묶음).
struct SceneTextures {
    GraphTexture color;
    GraphTexture depth;
};

using SceneTextureTable = GraphViewTable<SceneTextures>;

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

} // namespace

ROCKS_GRAPH_SHARED_DATA(ShadowSetup);
ROCKS_GRAPH_SHARED_DATA(LightSetup);
ROCKS_GRAPH_SHARED_DATA(SceneTextureTable);

// 타입마다 값 하나: 만들고 찾고, 그래프가 파괴될 때 소멸한다. 타입 번호는 프로세스 안에서 다르다.
TEST(RenderGraph_SharedDataHoldsOneValuePerType) {
    GraphBench bench;
    uint32 destroyed = 0;
    {
        RenderGraph graph(bench.Next());
        CHECK(graph.SharedData().Find<ShadowSetup>() == nullptr);
        ShadowSetup* shadows = graph.SharedData().Create<ShadowSetup>(&destroyed);
        LightSetup* lights = graph.SharedData().Create<LightSetup>();
        REQUIRE(shadows != nullptr && lights != nullptr);
        lights->count = 3;
        CHECK(graph.SharedData().Find<ShadowSetup>() == shadows);
        CHECK(graph.SharedData().Require<LightSetup>()->count == 3);
        CHECK(GraphSharedDataType<ShadowSetup>::Index() !=
              GraphSharedDataType<LightSetup>::Index());
        CHECK(graph.Stats().validationErrors == 0);
        CHECK(destroyed == 0);
    }

    CHECK(destroyed == 1);
    // 그래프마다 따로다.
    RenderGraph next(bench.Next());
    CHECK(next.SharedData().Find<ShadowSetup>() == nullptr);
}

// 중복 생성과 없는 값의 필수 조회는 검증 오류이며 그 그래프는 Execute하지 않는다.
TEST(RenderGraph_SharedDataRejectsDuplicatesAndMissingRequiredValues) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    CHECK(graph.SharedData().Require<LightSetup>() == nullptr);
    CHECK(ContainsText(graph.LastError(), "'LightSetup' is required before it was created"));
    REQUIRE(graph.SharedData().Create<LightSetup>() != nullptr);
    CHECK(graph.SharedData().Create<LightSetup>() == nullptr);
    CHECK(ContainsText(graph.LastError(), "'LightSetup' is created twice"));
    CHECK(graph.Stats().validationErrors == 2);
    CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
}

// 공유 값에 다른 그래프의 handle을 담으면 그것을 쓰는 pass 선언이 거부된다.
TEST(RenderGraph_SharedDataWithForeignHandlesIsRejected) {
    GraphBench bench;
    RenderGraph other(bench.Next());
    auto foreign = other.CreateTexture(TargetDesc(), "Foreign");
    REQUIRE(foreign);
    RenderGraph graph(bench.Next());
    ShadowSetup* shadows = graph.SharedData().Create<ShadowSetup>(nullptr);
    REQUIRE(shadows != nullptr);
    shadows->atlas = foreign.Value();
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    REQUIRE(target);
    GraphPassAccesses accesses(graph);
    accesses.Texture(graph.SharedData().Require<ShadowSetup>()->atlas, ResourceAccess::SrvGraphics)
        .RenderTarget(target.Value());
    CHECK(graph
              .AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, accesses,
                       Draw())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(graph.LastError(), "another graph"));
}

// ViewSet 둘·view 셋인 packet: 장면 텍스처 표를 (ViewSet, view)로 찾는다. 만들기 전 조회와 같은 키
// 두 번은 검증 오류다.
TEST(RenderGraph_SceneTextureTableFindsTexturesByViewSetAndView) {
    GraphBench bench;
    {
        RenderGraph graph(bench.Next());
        SceneTextureTable* table =
            graph.SharedData().Create<SceneTextureTable>(graph, "SceneTextureTable");
        REQUIRE(table != nullptr);
        CHECK(table->Get({0, 0}) == nullptr);
        CHECK(ContainsText(graph.LastError(), "has no entry for view set 0 view 0"));
        // ViewSet 0은 view 둘, ViewSet 1은 view 하나다.
        const GraphViewKey keys[] = {{0, 0}, {0, 1}, {1, 0}};
        for (const GraphViewKey& key : keys) {
            auto color = graph.CreateTexture(TargetDesc(4 + key.viewSet * 4 + key.view), "Color");
            REQUIRE(color);
            SceneTextures textures;
            textures.color = color.Value();
            REQUIRE(table->Add(key, textures));
        }

        CHECK(table->Size() == 3);
        CHECK(table->Add({0, 1}, SceneTextures{}).Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "already has view set 0 view 1"));
        CHECK(table->Find({2, 0}) == nullptr);
        CHECK(graph.Stats().validationErrors == 2);
        // pass는 자기 (ViewSet, view)의 장면 텍스처를 꺼내 쓴다.
        uint32 index = 0;
        for (const GraphViewKey& key : keys) {
            const SceneTextures* textures =
                graph.SharedData().Require<SceneTextureTable>()->Get(key);
            REQUIRE(textures != nullptr);
            CHECK(textures->color.index == index++);
            CHECK(textures->depth.IsNull());
        }

        CHECK(graph.Stats().validationErrors == 2);
    }
}

// 검증 실패로 버린 그래프는 history를 export하지 않는다: 새 handle은 Failed이고 풀은 export 0건,
// 이전 history는 그대로다(소유자는 무효로 표시해 다음 프레임에 reset한다, 7.5절).
TEST(RenderGraph_FailedGraphExportsNoHistory) {
    GraphBench bench;
    RenderHandle previous;
    {
        RenderGraph graph(bench.Next());
        auto history = graph.CreateTexture(TargetDesc(), "History");
        REQUIRE(history);
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(history.Value());
        REQUIRE(graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              Draw()));
        auto exported = graph.ExportTexture(history.Value());
        REQUIRE(exported);
        REQUIRE(graph.Execute());
        previous = exported.Value();
    }

    REQUIRE(bench.pool.Stats().exported == 1);
    RenderHandle failed;
    {
        RenderGraph graph(bench.Next());
        auto imported = graph.ImportTexture(previous, "PreviousHistory");
        auto history = graph.CreateTexture(TargetDesc(), "History");
        REQUIRE(imported && history);
        GraphPassAccesses accesses(graph);
        accesses.Texture(imported.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(history.Value());
        REQUIRE(graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, accesses,
                              Draw()));
        auto exported = graph.ExportTexture(history.Value());
        REQUIRE(exported);
        failed = exported.Value();
        // 뒤 기능 함수의 검증 오류(생산 전 읽기)가 그래프 전체를 버린다.
        auto never = graph.CreateTexture(TargetDesc(), "Never");
        REQUIRE(never);
        GraphPassAccesses bad(graph);
        bad.Texture(never.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(history.Value(), GraphLoadAction::Load);
        CHECK(!graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::None, bad, Draw()));
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    auto failedState = bench.handles.State(failed);
    auto previousState = bench.handles.State(previous);
    REQUIRE(failedState && previousState);
    CHECK(failedState.Value() == RenderResourceState::Failed);
    CHECK(previousState.Value() == RenderResourceState::Ready);
    CHECK(bench.pool.Stats().exported == 1);
    CHECK(bench.Count(RecordingDevice::Op::BeginOffscreen) == 1);
    CHECK(!bench.RetireHandle(previous));
}
