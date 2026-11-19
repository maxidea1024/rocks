#include "GraphTestSupport.h"

#include <TestSupport/Test.h>

// RG1 남은 것 B부(GraphTextureView) 시험이다(Docs/Design/RenderBase.ko.md 7.5절 mip view 그래프
// 자원): 장치 view로 낮추기, mip k 읽기 + mip k + 1 쓰기 한 pass 통과, 범위가 겹치는 pass와 view
// 규칙 위반 거부, view 기준 mip.
using namespace rocks;
using namespace rocks::render;
using namespace rocks::render::testing;
using graphics::ResourceAccess;
using graphics::ViewAccess;

namespace {

TraceScopeName FillName("View.Fill");
TraceScopeName ReduceName("View.Reduce");

// HZB 시험 텍스처다(Storage R32Float 8×8, mip 4).
constexpr uint32 PyramidMips = 4;

graphics::TextureDesc PyramidDesc() noexcept {
    graphics::TextureDesc desc = TargetDesc(8, PyramidMips);
    desc.format = graphics::PixelFormat::R32Float;
    desc.usage = graphics::TextureUsage::Storage;
    return desc;
}

graphics::TextureViewDesc MipView(uint32 mip, ViewAccess access, uint32 count = 1) noexcept {
    graphics::TextureViewDesc desc;
    desc.firstMip = mip;
    desc.mipCount = count;
    desc.access = access;
    return desc;
}

} // namespace

// HZB: mip 0을 쓰기 view로 채우고 mip k 읽기 view + mip k + 1 쓰기 view pass로 줄인다. 장치 view는
// view마다 한 번 만들고 그래프 끝에 놓으며, 묶인 view의 범위는 그 접근 상태다.
TEST(RenderGraph_TextureViewsLowerToDeviceViews) {
    GraphBench bench;
    uint32 baseMips[PyramidMips] = {};
    graphics::TextureHandle seen[PyramidMips] = {};
    {
        RenderGraph graph(bench.Next());
        auto pyramid = graph.CreateTexture(PyramidDesc(), "Hzb");
        REQUIRE(pyramid);
        graphics::TextureHandle* seenViews = seen;
        uint32* bases = baseMips;
        GraphTextureView writes[PyramidMips];
        GraphTextureView reads[PyramidMips];
        for (uint32 mip = 0; mip < PyramidMips; ++mip) {
            auto write =
                graph.CreateTextureView(pyramid.Value(), MipView(mip, ViewAccess::ReadWrite));
            auto read = graph.CreateTextureView(pyramid.Value(), MipView(mip, ViewAccess::Read));
            REQUIRE(write && read);
            writes[mip] = write.Value();
            reads[mip] = read.Value();
        }

        GraphPassAccesses fill(graph);
        fill.TextureView(writes[0], ResourceAccess::UavCompute);
        const GraphTextureView first = writes[0];
        REQUIRE(graph.AddPass(FillName, GraphPassKind::Compute, GraphPassFlags::None, fill,
                              [first](GraphPassContext& pass) noexcept {
                                  pass.Commands().SetWritableTexture(0, pass.View(first));
                                  pass.Commands().Dispatch(1, 1);
                              }));
        for (uint32 mip = 0; mip + 1 < PyramidMips; ++mip) {
            GraphPassAccesses reduce(graph);
            reduce.TextureView(reads[mip], ResourceAccess::SrvCompute)
                .TextureView(writes[mip + 1], ResourceAccess::UavCompute);
            const GraphTextureView read = reads[mip];
            const GraphTextureView write = writes[mip + 1];
            const GraphTextureView foreign = writes[0];
            REQUIRE(graph.AddPass(
                ReduceName, GraphPassKind::Compute,
                mip + 2 == PyramidMips ? GraphPassFlags::HasSideEffects : GraphPassFlags::None,
                reduce, [=](GraphPassContext& pass) noexcept {
                    seenViews[mip] = pass.View(read);
                    bases[mip] = pass.ViewBaseMip(read);
                    // 선언하지 않은 view는 null이다.
                    if (!pass.View(foreign).IsNull() || pass.ViewBaseMip(foreign) != 0) {
                        bases[mip] = 99;
                    }

                    pass.Commands().SetTexture(pass.View(read));
                    pass.Commands().SetWritableTexture(0, pass.View(write));
                    pass.Commands().Dispatch(1, 1);
                }));
        }

        REQUIRE(graph.Execute());
        CHECK(graph.Stats().validationErrors == 0);
        CHECK(graph.Stats().recordings == 1);
    }

    // 기준 mip은 장치 기능을 따른다(범위 view가 있으면 0, 없으면 view의 첫 mip, OpenGL ES 경로).
    const bool ranges = bench.device.Caps().features.textureViewRanges;
    for (uint32 mip = 0; mip + 1 < PyramidMips; ++mip) {
        CHECK(!seen[mip].IsNull());
        CHECK(baseMips[mip] == (ranges ? 0u : mip));
    }

    // view 7개(mip 0 쓰기, mip 0–2 읽기, mip 1–3 쓰기)를 만들고 그래프 끝에 모두 놓는다.
    CHECK(bench.Count(RecordingDevice::Op::CreateTextureView) == 7);
    CHECK(bench.Count(RecordingDevice::Op::SetWritableTexture) == 4);
    CHECK(bench.Count(RecordingDevice::Op::RetireTexture) == 7);
    CHECK(bench.device.ValidationErrorCount() == 0);
}

// 같은 mip을 읽기 view와 쓰기 view로 함께 쓰거나, 범위가 겹치는 view는 한 pass 안 상충 접근이다.
TEST(RenderGraph_OverlappingViewRangesConflict) {
    GraphBench bench;
    RenderGraph graph(bench.Next());
    auto pyramid = graph.CreateTexture(PyramidDesc(), "Hzb");
    REQUIRE(pyramid);
    auto upper = graph.CreateTextureView(pyramid.Value(), MipView(0, ViewAccess::Read, 2));
    auto write1 = graph.CreateTextureView(pyramid.Value(), MipView(1, ViewAccess::ReadWrite));
    auto write0 = graph.CreateTextureView(pyramid.Value(), MipView(0, ViewAccess::ReadWrite));
    REQUIRE(upper && write1 && write0);
    GraphPassAccesses produce(graph);
    produce.TextureView(write0.Value(), ResourceAccess::UavCompute)
        .Texture(pyramid.Value(), ResourceAccess::UavCompute, {1, 1, 0, 1});
    REQUIRE(graph.AddPass(FillName, GraphPassKind::Compute, GraphPassFlags::None, produce,
                          [](GraphPassContext& pass) noexcept { (void)pass; }));
    // mip 0–1 읽기 view와 mip 1 쓰기 view는 mip 1에서 겹친다.
    GraphPassAccesses overlap(graph);
    overlap.TextureView(upper.Value(), ResourceAccess::SrvCompute)
        .TextureView(write1.Value(), ResourceAccess::UavCompute);
    CHECK(graph
              .AddPass(ReduceName, GraphPassKind::Compute, GraphPassFlags::None, overlap,
                       [](GraphPassContext& pass) noexcept { (void)pass; })
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ContainsText(graph.LastError(), "conflicting accesses"));
    CHECK(graph.Stats().validationErrors == 1);
}

// view 규칙 위반: 범위 밖, Storage가 아닌 텍스처의 쓰기 view, mip 둘인 쓰기 view, 읽기 view로 UAV,
// 쓰기 view로 SRV, 다른 그래프의 view.
TEST(RenderGraph_RejectsInvalidTextureViews) {
    GraphBench bench;
    {
        RenderGraph other(bench.Next());
        auto otherTexture = other.CreateTexture(PyramidDesc(), "Other");
        REQUIRE(otherTexture);
        auto otherView =
            other.CreateTextureView(otherTexture.Value(), MipView(0, ViewAccess::Read));
        REQUIRE(otherView);

        RenderGraph graph(bench.Next());
        auto pyramid = graph.CreateTexture(PyramidDesc(), "Hzb");
        auto target = graph.CreateTexture(TargetDesc(8, 2), "Target");
        REQUIRE(pyramid && target);
        CHECK(graph.CreateTextureView(pyramid.Value(), MipView(4, ViewAccess::Read)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "texture view of 'Hzb' is invalid"));
        CHECK(graph.CreateTextureView(target.Value(), MipView(0, ViewAccess::ReadWrite)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(graph.CreateTextureView(pyramid.Value(), MipView(0, ViewAccess::ReadWrite, 2))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(graph.CreateTextureView(otherTexture.Value(), MipView(0, ViewAccess::Read)).Error() ==
              ErrorCode::InvalidArgument);
        auto read = graph.CreateTextureView(pyramid.Value(), MipView(0, ViewAccess::Read));
        auto write = graph.CreateTextureView(pyramid.Value(), MipView(1, ViewAccess::ReadWrite));
        REQUIRE(read && write);
        GraphPassAccesses uavThroughRead(graph);
        uavThroughRead.TextureView(read.Value(), ResourceAccess::UavCompute);
        CHECK(graph
                  .AddPass(FillName, GraphPassKind::Compute, GraphPassFlags::None, uavThroughRead,
                           [](GraphPassContext& pass) noexcept { (void)pass; })
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "through a read view"));
        GraphPassAccesses srvThroughWrite(graph);
        srvThroughWrite.TextureView(write.Value(), ResourceAccess::SrvCompute);
        CHECK(graph
                  .AddPass(FillName, GraphPassKind::Compute, GraphPassFlags::None, srvThroughWrite,
                           [](GraphPassContext& pass) noexcept { (void)pass; })
                  .Error() == ErrorCode::InvalidArgument);
        GraphPassAccesses foreign(graph);
        foreign.TextureView(otherView.Value(), ResourceAccess::SrvCompute);
        CHECK(graph
                  .AddPass(FillName, GraphPassKind::Compute, GraphPassFlags::None, foreign,
                           [](GraphPassContext& pass) noexcept { (void)pass; })
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ContainsText(graph.LastError(), "texture view from another graph"));
        CHECK(graph.Stats().validationErrors == 7);
    }

    CHECK(bench.Count(RecordingDevice::Op::CreateTextureView) == 0);
}

// 범위 view가 없는 장치(OpenGL ES)의 view 기준 mip은 view의 첫 mip이다(개수 0 = 끝까지도 푼다).
TEST(RenderGraph_ViewBaseMipFollowsDeviceFeatures) {
    graphics::DeviceFeatures noRanges;
    noRanges.textureViewRanges = false;
    graphics::DeviceFeatures ranges;
    ranges.textureViewRanges = true;
    graphics::TextureViewDesc desc = MipView(2, ViewAccess::Read, 0);
    CHECK(graphics::TextureViewBaseMip(noRanges, desc) == 2);
    CHECK(graphics::TextureViewBaseMip(ranges, desc) == 0);
}
