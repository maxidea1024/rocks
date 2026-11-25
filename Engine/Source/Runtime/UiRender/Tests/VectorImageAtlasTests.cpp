#include "../../RenderBase/Tests/RecordingDevice.h"

#include <Core/Memory/LibraryHeap.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>
#include <TextLayout/FontAtlas.h>
#include <UiRender/VectorImageAtlas.h>

// 벡터 이미지 atlas 시험이다(Docs/Design/UiToolkit 17.2·17.5절). nanosvg 래스터 결과의 화소, 같은
// 키·크기 재사용, 비균등 배율, 단독 페이지, 비우기와 revision, 오류, Rocks 힙 경유 할당을 단언한다.

namespace rocks::ui {

namespace {

constexpr StringView FullSquare =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">"
    "<rect x=\"0\" y=\"0\" width=\"16\" height=\"16\" fill=\"#FFFFFF\"/></svg>";

// 왼쪽 절반만 채운 사각형이다.
constexpr StringView LeftHalf =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\" viewBox=\"0 0 16 16\">"
    "<rect x=\"0\" y=\"0\" width=\"8\" height=\"16\" fill=\"#FFFFFF\"/></svg>";

Span<const uint8> Bytes(StringView text) noexcept {
    return {reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()};
}

// 첫 페이지에 든 자리 안 (x, y) 화소의 알파다(시험의 작은 그림은 모두 첫 페이지에 들어간다).
uint8 AlphaAt(const VectorImageAtlas& atlas, const ResolvedVectorImage& image, uint32 x,
              uint32 y) noexcept {
    uint32 width = 0;
    uint32 height = 0;
    const Span<const uint8> pixels = atlas.PagePixels(0, width, height);
    const uint32 left = static_cast<uint32>(image.uv.min.x * static_cast<float>(width) + 0.5f);
    const uint32 top = static_cast<uint32>(image.uv.min.y * static_cast<float>(height) + 0.5f);
    return pixels[(static_cast<usize>(top + y) * width + left + x) * 4 + 3];
}

struct AtlasBench {
    SystemAllocator allocator;
    render::testing::RecordingDevice device{allocator};
    SlotArray<BrushTexture> textures{allocator};
};

} // namespace

TEST(UiRender_VectorImageAtlasRasterizesAndReuses) {
    AtlasBench bench;
    VectorImageAtlas atlas(bench.allocator, bench.device, bench.textures);
    auto full = atlas.Register("Test/Full", Bytes(FullSquare));
    auto half = atlas.Register("Test/Half", Bytes(LeftHalf));
    REQUIRE(full && half);
    CHECK(full.Value() == VectorImageKey::FromPath("Test/Full"));
    CHECK(atlas.ImageCount() == 2 && atlas.ImagePath(1) == "Test/Half");
    // 16x16 가득 찬 사각형: 안쪽 모든 화소가 불투명이다.
    auto a = atlas.Resolve(full.Value(), 16, 16);
    REQUIRE(a);
    CHECK(atlas.RasterCount() == 1 && atlas.PageCount() == 1);
    CHECK(AlphaAt(atlas, a.Value(), 0, 0) == 255 && AlphaAt(atlas, a.Value(), 15, 15) == 255);
    // 같은 키·크기는 래스터화하지 않고 같은 자리다.
    auto again = atlas.Resolve(full.Value(), 16, 16);
    REQUIRE(again);
    CHECK(atlas.RasterCount() == 1);
    CHECK(again.Value().uv.min == a.Value().uv.min && again.Value().resource == a.Value().resource);
    // 다른 크기는 다른 자리다.
    auto big = atlas.Resolve(full.Value(), 32, 32);
    REQUIRE(big);
    CHECK(atlas.RasterCount() == 2 && big.Value().uv.min != a.Value().uv.min);
    // 왼쪽 절반: 왼쪽 열은 불투명, 오른쪽 열은 투명이다.
    auto h = atlas.Resolve(half.Value(), 16, 16);
    REQUIRE(h);
    CHECK(AlphaAt(atlas, h.Value(), 2, 8) == 255 && AlphaAt(atlas, h.Value(), 13, 8) == 0);
    // 비균등 배율(가로 2배)도 늘려 그린다(UE nsvgRasterizeFull과 같다): 왼쪽 16열이 불투명이다.
    auto wide = atlas.Resolve(half.Value(), 32, 16);
    REQUIRE(wide);
    CHECK(AlphaAt(atlas, wide.Value(), 14, 8) == 255 && AlphaAt(atlas, wide.Value(), 18, 8) == 0);
}

TEST(UiRender_VectorImageAtlasRejectsBadInput) {
    AtlasBench bench;
    VectorImageAtlas atlas(bench.allocator, bench.device, bench.textures);
    REQUIRE(atlas.Register("Test/Full", Bytes(FullSquare)));
    CHECK(atlas.Register("Test/Full", Bytes(FullSquare)).Error() == ErrorCode::AlreadyExists);
    CHECK(atlas.Register("", Bytes(FullSquare)).Error() == ErrorCode::InvalidArgument);
    CHECK(atlas.Register("Test/Garbage", Bytes("not an svg")).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(atlas.Register("Test/Empty", Bytes("")).Error() == ErrorCode::InvalidArgument);
    const VectorImageKey full = VectorImageKey::FromPath("Test/Full");
    CHECK(atlas.Resolve(VectorImageKey::FromPath("Test/Missing"), 16, 16).Error() ==
          ErrorCode::NotFound);
    CHECK(atlas.Resolve(full, 0, 16).Error() == ErrorCode::InvalidArgument);
    CHECK(atlas.Resolve(full, MaxVectorImagePixels + 1, 16).Error() == ErrorCode::InvalidArgument);
    CHECK(atlas.ImageCount() == 1);
}

TEST(UiRender_VectorImageAtlasSyncsAndFlushes) {
    AtlasBench bench;
    VectorImageAtlas atlas(bench.allocator, bench.device, bench.textures);
    auto full = atlas.Register("Test/Full", Bytes(FullSquare));
    REQUIRE(full);
    auto first = atlas.Resolve(full.Value(), 16, 16);
    REQUIRE(first);
    // 자리는 바로 받고, 텍스처는 Sync가 채운다.
    const BrushTexture* entry = bench.textures.Find(first.Value().resource);
    REQUIRE(entry != nullptr);
    CHECK(entry->texture.IsNull());
    CHECK(!atlas.Sync({}));
    const graphics::PipelineHandle pipeline{SlotHandle{1, 1}};
    REQUIRE(atlas.Sync(pipeline));
    entry = bench.textures.Find(first.Value().resource);
    CHECK(entry != nullptr && !entry->texture.IsNull() && entry->pipeline == pipeline);
    // 이미 텍스처가 있는 페이지에 새 래스터가 들면 Sync가 페이지를 다시 올린다(UpdateTexture).
    usize updates = 0;
    REQUIRE(atlas.Resolve(full.Value(), 24, 24));
    REQUIRE(atlas.Sync(pipeline));
    for (const auto& call : bench.device.Calls()) {
        updates += call.op == render::testing::RecordingDevice::Op::UpdateTexture ? 1 : 0;
    }

    CHECK(updates == 1 && atlas.PageCount() == 1);
    // 페이지에 들지 않는 그림은 여백 없이 정확한 크기의 단독 텍스처이고 uv는 0..1이다(UE 비atlas
    // 텍스처). 단독 텍스처가 MaxVectorStandaloneImages를 넘으면 AdvanceFrame이 모두 놓고 revision을
    // 올린다(atlas 페이지와 따로 센다).
    const uint64 revision = atlas.Revision();
    auto large = atlas.Resolve(full.Value(), VectorAtlasPageSize, 4);
    REQUIRE(large);
    CHECK(large.Value().uv.min == Vec2(0, 0) && large.Value().uv.max == Vec2(1, 1));
    uint32 width = 0;
    uint32 height = 0;
    (void)atlas.PagePixels(1, width, height);
    CHECK(width == VectorAtlasPageSize && height == 4);
    REQUIRE(atlas.Sync(pipeline));
    CHECK(!atlas.AdvanceFrame());
    for (uint32 i = 0; i < MaxVectorStandaloneImages; ++i) {
        REQUIRE(atlas.Resolve(full.Value(), VectorAtlasPageSize + 1 + i, 4));
    }

    CHECK(atlas.PageCount() == MaxVectorStandaloneImages + 2);
    REQUIRE(atlas.Sync(pipeline));
    CHECK(atlas.AdvanceFrame());
    CHECK(atlas.PageCount() == 0 && atlas.Revision() == revision + 1);
    CHECK(bench.textures.Find(first.Value().resource) == nullptr);
    CHECK(!atlas.AdvanceFrame());
    // 비운 뒤에도 다시 래스터화한다.
    REQUIRE(atlas.Resolve(full.Value(), 16, 16));
    CHECK(atlas.PageCount() == 1);
}

TEST(UiRender_VectorImageAtlasRejectsDamagedSvgWithoutLeaks) {
    const usize before = LibraryHeapLiveBlocks();
    {
        AtlasBench bench;
        VectorImageAtlas atlas(bench.allocator, bench.device, bench.textures);
        // 거대한 수(유한하지 않은 크기)와 크기 0인 그림은 InvalidArgument이고 등록되지 않는다.
        const StringView rejected[] = {
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1e40\" height=\"1e40\"></svg>",
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"0\" height=\"0\"></svg>",
        };
        for (StringView text : rejected) {
            auto added = atlas.Register("Test/Rejected", Bytes(text));
            CHECK(!added && added.Error() == ErrorCode::InvalidArgument);
        }

        CHECK(atlas.ImageCount() == 0);
        // 잘린 태그는 nanosvg가 관대하게 읽어 도형 없는 그림이 된다(UE도 같은 라이브러리다). 등록은
        // 되고 래스터는 비어 있다.
        constexpr StringView Truncated =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\"><rect x=\"0\" y";
        auto truncated = atlas.Register("Test/Truncated", Bytes(Truncated));
        REQUIRE(truncated);
        auto drawn = atlas.Resolve(truncated.Value(), 16, 16);
        REQUIRE(drawn);
        CHECK(AlphaAt(atlas, drawn.Value(), 8, 8) == 0);
    }

    // 실패한 파싱과 래스터도 nanosvg 메모리를 모두 놓는다.
    CHECK(LibraryHeapLiveBlocks() == before);
}

TEST(UiRender_VectorImageAtlasKeepsGradientAspect) {
    AtlasBench bench;
    VectorImageAtlas atlas(bench.allocator, bench.device, bench.textures);
    // 그라데이션이 있는 그림은 비균등 배율에서 늘이지 않고 작은 배율로 가운데에 둔다(17.6절).
    constexpr StringView Gradient =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
        "<defs><linearGradient id=\"g\"><stop offset=\"0\" stop-color=\"#FFFFFF\"/>"
        "<stop offset=\"1\" stop-color=\"#FFFFFF\"/></linearGradient></defs>"
        "<rect x=\"0\" y=\"0\" width=\"16\" height=\"16\" fill=\"url(#g)\"/></svg>";
    auto gradient = atlas.Register("Test/Gradient", Bytes(Gradient));
    REQUIRE(gradient);
    auto wide = atlas.Resolve(gradient.Value(), 32, 16);
    REQUIRE(wide);
    // 16x16 그림이 32x16 칸 가운데(8..24)에 놓인다: 가장자리 열은 투명, 가운데 열은 불투명이다.
    CHECK(AlphaAt(atlas, wide.Value(), 2, 8) == 0 && AlphaAt(atlas, wide.Value(), 16, 8) == 255);
}

TEST(UiRender_VectorImageAtlasAllocatesThroughLibraryHeap) {
    const usize before = LibraryHeapLiveBlocks();
    {
        AtlasBench bench;
        VectorImageAtlas atlas(bench.allocator, bench.device, bench.textures);
        auto full = atlas.Register("Test/Full", Bytes(FullSquare));
        REQUIRE(full);
        // 파싱한 그림(shape·path·점)은 nanosvg가 malloc으로 잡으며 glue가 Rocks 힙으로 돌린다.
        CHECK(LibraryHeapLiveBlocks() > before);
        REQUIRE(atlas.Resolve(full.Value(), 16, 16));
    }

    // atlas가 사라지면 그림·래스터라이저가 모두 풀린다.
    CHECK(LibraryHeapLiveBlocks() == before);
}

} // namespace rocks::ui
