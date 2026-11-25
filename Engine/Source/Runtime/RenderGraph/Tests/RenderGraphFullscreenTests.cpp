#include "../../RenderBase/Tests/RegistryShaderSet.h"
#include "GraphTestSupport.h"

#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/PipelineCache.h>
#include <ShaderCore/ShaderTypeRegistry.h>
#include <TestSupport/Test.h>

// 그래프 전체화면 pass 시험이다(Docs/Design/RenderBase 11.4절 3항): 기록 순서(SetPipeline → bind →
// Draw(3), 정점 버퍼 없음), 캐시 판의 두 그래프 적중, 검증 오류(파이프라인 없음·렌더 타깃 없음)마다
// 그 뒤 Execute 실패, null 텍스처를 묶지 않는 경로.
using namespace rocks;
namespace render_graph_fullscreen_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;
using Op = RecordingDevice::Op;

namespace {

TraceScopeName CopyName("Fullscreen.Copy");
TraceScopeName BadName("Fullscreen.Bad");

// 엔진 셰이더 타입 등록부와 그 집합이다.
struct EngineShaders {
    explicit EngineShaders(GraphBench& bench) noexcept : registry(bench.allocator) {
        ready = registry.Register(EngineShaderTypes()) && registry.Freeze();
    }

    shader::ShaderTypeRegistry registry;
    bool ready = false;
};

uint64 DrawValue(uint32 vertexCount, uint32 instanceCount, uint32 firstVertex) noexcept {
    return (static_cast<uint64>(vertexCount) << 40) | (static_cast<uint64>(instanceCount) << 20) |
           firstVertex;
}

} // namespace

TEST(RenderGraph_FullscreenPassRecordsDrawAndCachesPipeline) {
    GraphBench bench;
    EngineShaders engine(bench);
    REQUIRE(engine.ready);
    RegistryShaderSet shaders(bench.allocator, bench.device, engine.registry);
    REQUIRE(shaders.ready);
    auto copy = shaders.set.FindEntry(CopyTexturePixelType.hash);
    REQUIRE(copy);
    auto request =
        FullscreenPipelineRequest(shaders.set, copy.Value(), graphics::PixelFormat::RGBA8Unorm);
    REQUIRE(request);
    request.Value().state.bindings.texture = CopyTextureSourceName;
    PipelineCache cache(bench.allocator, bench.device);
    for (uint32 run = 0; run < 2; ++run) {
        RenderGraph graph(bench.Next());
        auto source = graph.CreateTexture(TargetDesc(), "Source");
        auto output = graph.ImportTexture(bench.ExternalTarget(), "Output");
        REQUIRE(source && output);
        GraphPassAccesses fill(graph);
        fill.RenderTarget(source.Value(), GraphLoadAction::Clear, {1, 0, 0, 1});
        REQUIRE(graph.AddPass(CopyName, GraphPassKind::Raster, GraphPassFlags::None, fill,
                              [](GraphPassContext&) noexcept {}));
        GraphPassAccesses accesses(graph);
        accesses.Texture(source.Value(), ResourceAccess::SrvGraphics)
            .RenderTarget(output.Value(), GraphLoadAction::DontCare);
        const GraphTexture read = source.Value();
        auto pass =
            graph.AddFullscreenPass(CopyName, GraphPassFlags::None, accesses, cache,
                                    request.Value(), [read](GraphPassContext& context) noexcept {
                                        context.Commands().SetTexture(context.Texture(read));
                                    });
        REQUIRE(pass);
        REQUIRE(graph.Execute());
        CHECK(!graph.IsCulled(pass.Value()));
        bench.device.CompleteAll();
    }

    CHECK(cache.Misses() == 1 && cache.Hits() == 1 && cache.Size() == 1);
    // 두 그래프의 전체화면 pass마다 SetPipeline → SetTexture → Draw(3, 1, 0)이고 정점 버퍼는 없다.
    CHECK(bench.Count(Op::Draw) == 2 && bench.Count(Op::SetVertexBuffers) == 0);
    const usize setPipeline = bench.IndexOf(Op::SetPipeline);
    const usize setTexture = bench.IndexOf(Op::SetTexture);
    const usize draw = bench.IndexOf(Op::Draw);
    CHECK(setPipeline < setTexture && setTexture < draw);
    CHECK(bench.device.Calls()[draw].value == DrawValue(FullscreenTriangleVertexCount, 1, 0));
    CHECK(bench.device.NullTextureBindings() == 0);
    CHECK(bench.device.ValidationErrorCount() == 0 && bench.device.IgnoredDraws() == 0);
    cache.Clear();
    shaders.set.Retire();
    bench.device.CompleteAll();
    bench.device.ReclaimCompleted();
}

TEST(RenderGraph_FullscreenPassValidation) {
    GraphBench bench;
    // 파이프라인이 없으면 검증 오류다.
    {
        RenderGraph graph(bench.Next());
        auto output = graph.ImportTexture(bench.ExternalTarget(), "Output");
        REQUIRE(output);
        GraphPassAccesses accesses(graph);
        accesses.RenderTarget(output.Value(), GraphLoadAction::DontCare);
        auto pass =
            graph.AddFullscreenPass(BadName, GraphPassFlags::None, accesses,
                                    graphics::PipelineHandle{}, [](GraphPassContext&) noexcept {});
        CHECK(pass.Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "has no pipeline"));
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    // 렌더 타깃이 없으면 검증 오류다.
    graphics::PipelineDesc desc;
    auto pipeline = bench.device.CreatePipeline(desc);
    REQUIRE(pipeline);
    {
        RenderGraph graph(bench.Next());
        GraphPassAccesses accesses(graph);
        auto pass = graph.AddFullscreenPass(BadName, GraphPassFlags::HasSideEffects, accesses,
                                            pipeline.Value(), [](GraphPassContext&) noexcept {});
        CHECK(pass.Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "has no render target"));
        CHECK(graph.Execute().Error() == ErrorCode::InvalidArgument);
    }

    CHECK(bench.Count(Op::Draw) == 0);
    bench.device.Retire(pipeline.Value());
    bench.device.CompleteAll();
    bench.device.ReclaimCompleted();
}

} // namespace render_graph_fullscreen_tests_file
