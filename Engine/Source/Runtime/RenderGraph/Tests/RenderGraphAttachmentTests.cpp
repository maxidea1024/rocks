#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG1 남은 것 A부(attachment 기록과 풀 desc 확장) 시험이다(Docs/Design/RenderBase.ko.md 8절):
// MRT·깊이·mip·slice attachment, attachment load의 생산 전 읽기 검사, 렌더 타깃 없는
// Compute·Copy pass의 기록, 깊이·Storage·MSAA 그래프 텍스처와 풀 키.
using namespace rocks;
namespace render_graph_attachment_tests_file {

using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;

namespace {

TraceScopeName DrawName("Attachment.Draw");
TraceScopeName ReadName("Attachment.Read");
TraceScopeName ComputeName("Attachment.Compute");

auto Draw() noexcept {
    return [](GraphPassContext& pass) noexcept { pass.Commands().Draw(3); };
}

auto Nothing() noexcept {
    return [](GraphPassContext& pass) noexcept { (void)pass; };
}

graphics::TextureDesc ArrayTargetDesc() noexcept {
    graphics::TextureDesc desc = TargetDesc();
    desc.dimension = graphics::TextureDimension::Texture2DArray;
    desc.arraySize = 3;
    desc.mipLevels = 2;
    return desc;
}

graphics::TextureDesc DepthDesc() noexcept {
    graphics::TextureDesc desc = TargetDesc();
    desc.format = graphics::PixelFormat::D32Float;
    desc.usage = graphics::TextureUsage::DepthStencil;
    return desc;
}

} // namespace

// MRT·깊이·배열 층 attachment로 그리고, 같은 텍스처의 다른 subresource를 읽으며 다른 mip에 그린다.
TEST(RenderGraph_RecordsMrtDepthAndSliceAttachments) {
    GraphBench bench;
    {
        RenderGraph graph(bench.Next());
        auto layers = graph.CreateTexture(ArrayTargetDesc(), "Layers");
        auto second = graph.CreateTexture(TargetDesc(), "Second");
        auto depth = graph.CreateTexture(DepthDesc(), "Depth");
        REQUIRE(layers && second && depth);
        GraphColorTarget layer;
        layer.texture = layers.Value();
        layer.slice = 1;
        GraphDepthTarget depthTarget;
        depthTarget.texture = depth.Value();
        GraphPassAccesses mrt(graph);
        mrt.ColorTarget(layer).RenderTarget(second.Value()).DepthTarget(depthTarget);
        REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, mrt, Draw()));
        // mip 0 slice 1을 읽으며 mip 1 slice 0에 그린다(다른 subresource라 상충 아님).
        GraphColorTarget lower;
        lower.texture = layers.Value();
        lower.mipLevel = 1;
        GraphPassAccesses chain(graph);
        chain.Texture(layers.Value(), ResourceAccess::SrvGraphics, {0, 1, 1, 1})
            .Texture(second.Value(), ResourceAccess::SrvGraphics)
            .ColorTarget(lower);
        REQUIRE(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              chain, Draw()));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
    }

    CHECK(bench.Count(RecordingDevice::Op::BeginOffscreen) == 2);
    CHECK(bench.Count(RecordingDevice::Op::Draw) == 2);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// attachment load는 이전 내용을 읽으므로 생산 전이면 검증 오류다. 생산 뒤에는 받는다.
TEST(RenderGraph_AttachmentLoadNeedsAProducer) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto target = graph.CreateTexture(TargetDesc(), "Target");
    auto depth = graph.CreateTexture(DepthDesc(), "Depth");
    REQUIRE(target && depth);
    GraphPassAccesses early(graph);
    early.RenderTarget(target.Value(), GraphLoadAction::Load);
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, early, Draw())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(graph.LastError(), "before any pass produced it"));
    GraphDepthTarget readOnly;
    readOnly.texture = depth.Value();
    readOnly.readOnly = true;
    GraphPassAccesses earlyDepth(graph);
    earlyDepth.RenderTarget(target.Value()).DepthTarget(readOnly);
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, earlyDepth, Draw())
              .Error() == ErrorCode::InvalidArgument);
    GraphPassAccesses produce(graph);
    GraphDepthTarget write;
    write.texture = depth.Value();
    produce.RenderTarget(target.Value()).DepthTarget(write);
    REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, produce, Draw()));
    GraphPassAccesses load(graph);
    load.RenderTarget(target.Value(), GraphLoadAction::Load).DepthTarget(readOnly);
    CHECK(graph.AddPass(ReadName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects, load,
                        Draw()));
    CHECK(graph.Stats().validationErrors == 2);
}

// attachment 규칙 위반: 같은 subresource 둘, 범위 밖 slice, 용도가 다른 텍스처, 색 attachment 9개.
TEST(RenderGraph_RejectsInvalidAttachments) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto layers = graph.CreateTexture(ArrayTargetDesc(), "Layers");
    auto depth = graph.CreateTexture(DepthDesc(), "Depth");
    REQUIRE(layers && depth);
    GraphPassAccesses twice(graph);
    twice.RenderTarget(layers.Value()).RenderTarget(layers.Value());
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, twice, Draw())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(graph.LastError(), "twice"));
    GraphColorTarget outside;
    outside.texture = layers.Value();
    outside.slice = 3;
    GraphPassAccesses beyond(graph);
    beyond.ColorTarget(outside);
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, beyond, Draw())
              .Error() == ErrorCode::InvalidArgument);
    GraphPassAccesses depthAsColor(graph);
    depthAsColor.RenderTarget(depth.Value());
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, depthAsColor, Draw())
              .Error() == ErrorCode::InvalidArgument);
    GraphDepthTarget colorAsDepth;
    colorAsDepth.texture = layers.Value();
    GraphPassAccesses wrongDepth(graph);
    wrongDepth.DepthTarget(colorAsDepth);
    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, wrongDepth, Draw())
              .Error() == ErrorCode::InvalidArgument);
    // 색 attachment 9개도 검증 오류다.
    GraphPassAccesses many(graph);
    for (uint32 index = 0; index <= graphics::MaxColorTargets; ++index) {
        GraphColorTarget color;
        color.texture = layers.Value();
        color.slice = index % 3;
        color.mipLevel = index / 3 % 2;
        many.ColorTarget(color);
    }

    CHECK(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::None, many, Draw())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(graph.LastError(), "more than"));
    CHECK(graph.Stats().validationErrors == 5);
}

// 렌더 타깃이 없는 Compute·Copy pass는 attachment 없는 기록을 연다.
TEST(RenderGraph_ComputeAndCopyPassesOpenTheirOwnRecording) {
    GraphBench bench;
    {
        RenderGraph graph(bench.Next());
        graphics::BufferDesc desc;
        desc.size = 64;
        desc.usage = graphics::BufferUsage::ShaderRead | graphics::BufferUsage::ShaderWrite |
                     graphics::BufferUsage::Raw;
        auto buffer = graph.CreateBuffer(desc, "Buffer");
        REQUIRE(buffer);
        GraphPassAccesses write(graph);
        write.Buffer(buffer.Value(), ResourceAccess::UavCompute);
        REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects,
                              write, Nothing()));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
        CHECK(graph.Stats().recordings == 1);
    }

    CHECK(bench.Count(RecordingDevice::Op::BeginOffscreen) == 1);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 깊이·Storage·MSAA·배열 렌더 타깃 그래프 텍스처를 풀이 만들고, 표본 수가 다르면 다른 풀 키다.
TEST(RenderGraph_PoolCreatesDepthStorageAndMsaaTextures) {
    GraphBench bench;
    {
        RenderGraph graph(bench.Next());
        graphics::TextureDesc storageDesc = TargetDesc();
        storageDesc.format = graphics::PixelFormat::R32Float;
        storageDesc.usage = graphics::TextureUsage::Storage;
        storageDesc.mipLevels = 2;
        graphics::TextureDesc msaaDesc = TargetDesc();
        msaaDesc.sampleCount = 4;
        auto storage = graph.CreateTexture(storageDesc, "Storage");
        auto msaa = graph.CreateTexture(msaaDesc, "Msaa");
        auto single = graph.CreateTexture(TargetDesc(), "Single");
        auto depth = graph.CreateTexture(DepthDesc(), "Depth");
        auto layers = graph.CreateTexture(ArrayTargetDesc(), "Layers");
        REQUIRE(storage && msaa && single && depth && layers);
        GraphPassAccesses fill(graph);
        fill.Texture(storage.Value(), ResourceAccess::UavCompute, {0, 1, 0, 1});
        REQUIRE(graph.AddPass(ComputeName, GraphPassKind::Compute, GraphPassFlags::HasSideEffects,
                              fill, Nothing()));
        GraphDepthTarget depthTarget;
        depthTarget.texture = depth.Value();
        GraphPassAccesses msaaPass(graph);
        msaaPass.RenderTarget(msaa.Value());
        REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              msaaPass, Draw()));
        GraphPassAccesses singlePass(graph);
        singlePass.RenderTarget(single.Value()).DepthTarget(depthTarget);
        REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              singlePass, Draw()));
        GraphColorTarget layer;
        layer.texture = layers.Value();
        layer.slice = 2;
        GraphPassAccesses layerPass(graph);
        layerPass.ColorTarget(layer);
        REQUIRE(graph.AddPass(DrawName, GraphPassKind::Raster, GraphPassFlags::HasSideEffects,
                              layerPass, Draw()));
        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
    }

    // 텍스처 다섯이 각자 장치 객체다(MSAA와 1표본은 같은 desc라도 풀 키가 다르다).
    CHECK(bench.Count(RecordingDevice::Op::CreateTexture) == 5);
    CHECK(bench.pool.Stats().created == 5);
    CHECK(bench.device.ValidationErrorCount() == 0);
}
} // namespace render_graph_attachment_tests_file
