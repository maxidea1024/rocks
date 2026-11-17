#include <Graphics/DeviceTypes.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

TEST(TextureView_ResolvesSingleAndSrgbPairFormats) {
    auto same = ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, {});
    REQUIRE(same);
    CHECK(same.Value() == PixelFormat::RGBA8Unorm);
    auto pair = ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, {PixelFormat::RGBA8UnormSrgb, 0});
    REQUIRE(pair);
    CHECK(pair.Value() == PixelFormat::RGBA8UnormSrgb);
    auto back = ResolveTextureViewFormat(PixelFormat::BC7UnormSrgb, {PixelFormat::BC7Unorm, 0});
    REQUIRE(back);
    CHECK(back.Value() == PixelFormat::BC7Unorm);
    // 짝이 아닌 형식, plane 범위 밖, 무효 형식은 거부한다.
    CHECK(ResolveTextureViewFormat(PixelFormat::RGBA8Unorm, {PixelFormat::BGRA8Unorm, 0}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ResolveTextureViewFormat(PixelFormat::R16Float, {PixelFormat::Unknown, 1}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ResolveTextureViewFormat(PixelFormat::Unknown, {}).Error() == ErrorCode::InvalidArgument);
}

TEST(TextureView_ResolvesPlanes) {
    auto depth = ResolveTextureViewFormat(PixelFormat::D24UnormS8Uint, {});
    REQUIRE(depth);
    CHECK(depth.Value() == PixelFormat::D24Unorm);
    auto stencil = ResolveTextureViewFormat(PixelFormat::D32FloatS8Uint, {PixelFormat::Unknown, 1});
    REQUIRE(stencil);
    CHECK(stencil.Value() == PixelFormat::R8Uint);
    auto chroma = ResolveTextureViewFormat(PixelFormat::P010, {PixelFormat::RG16Unorm, 1});
    REQUIRE(chroma);
    CHECK(chroma.Value() == PixelFormat::RG16Unorm);
    auto luma = ResolveTextureViewFormat(PixelFormat::NV12, {});
    REQUIRE(luma);
    CHECK(luma.Value() == PixelFormat::R8Unorm);
    // plane 형식이 아닌 형식과 plane 2는 거부한다.
    CHECK(ResolveTextureViewFormat(PixelFormat::NV12, {PixelFormat::RG8Unorm, 0}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ResolveTextureViewFormat(PixelFormat::NV12, {PixelFormat::Unknown, 2}).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(TextureView_RequiredCapsPerUsage) {
    CHECK(RequiredPixelFormatCaps(TextureUsage::Sampled, 1) == PixelFormatCaps::Sampled);
    CHECK(RequiredPixelFormatCaps(TextureUsage::Sampled, 3) ==
          (PixelFormatCaps::Sampled | PixelFormatCaps::Mipmaps));
    CHECK(RequiredPixelFormatCaps(TextureUsage::RenderTarget, 1) ==
          (PixelFormatCaps::RenderTarget | PixelFormatCaps::Sampled));
    CHECK(RequiredPixelFormatCaps(TextureUsage::DepthStencil, 1) == PixelFormatCaps::DepthStencil);
}
