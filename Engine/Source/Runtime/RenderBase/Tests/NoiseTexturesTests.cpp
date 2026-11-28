#include <Core/Hash/Sha256.h>
#include <RenderBase/NoiseTextures.h>
#include <TestSupport/Test.h>

// noise 시스템 텍스처 시험이다(Docs/Design/RenderBase 11.5절): 두 표가 UE 5.8 알고리즘(독립 Python
// 모델로 계산한 SHA-256 golden)과 byte 단위로 같고, desc·성질(2D 성분 값·A 0, 3D 경계 복제·A =
// 내적 + 127)이 맞는다. 팩이 두 텍스처를 만드는지는 BuiltinResourcesTests가 본다.
using namespace rocks;
using namespace rocks::render;

namespace noise_textures_tests_file {

// UE SystemTextures.cpp의 생성 절차를 따로 옮긴 Python 모델로 계산한 SHA-256이다(11.5절 8항).
constexpr char Gradient2DSha256[] =
    "b5912ef59469abca3ab27758ca0593fea07226ed3201e5f44dbb885dfa158d56";
constexpr char Gradient3DSha256[] =
    "fd01a19cf42c9e66bb281833ddb0620b77375d9c12f22c9fecdfe621146b8006";

bool DigestIs(Span<const uint8> bytes,
              const char (&expected)[Sha256Digest::HexBytes + 1]) noexcept {
    char hex[Sha256Digest::HexBytes + 1] = {};
    Sha256::Hash(bytes).ToHex(hex);
    for (usize index = 0; index < Sha256Digest::HexBytes; ++index) {
        if (hex[index] != expected[index]) {
            return false;
        }
    }

    return true;
}

// 3D 표 texel 하나의 시작 byte 위치다.
usize Texel3D(uint32 x, uint32 y, uint32 z) noexcept {
    constexpr uint32 Extent = PerlinGradient3DExtent;
    return (static_cast<usize>(z) * Extent * Extent + static_cast<usize>(y) * Extent + x) *
           NoiseTexelBytes;
}

TEST(RenderBase_NoiseTexturesMatchUeData) {
    const Span<const uint8> gradient2D = PerlinGradient2DTexels();
    const Span<const uint8> gradient3D = PerlinGradient3DTexels();
    REQUIRE(gradient2D.Size() == PerlinGradient2DBytes && PerlinGradient2DBytes == 65536);
    REQUIRE(gradient3D.Size() == PerlinGradient3DBytes && PerlinGradient3DBytes == 16384);
    CHECK(DigestIs(gradient2D, Gradient2DSha256));
    CHECK(DigestIs(gradient3D, Gradient3DSha256));
    // 앞 texel 넷(모델 출력 그대로 — 표 번호 5·7·6·9).
    const uint8 first2D[] = {0xff, 0x00, 0x88, 0x00, 0x00, 0x88, 0xff, 0x00,
                             0x88, 0x00, 0xff, 0x00, 0x00, 0x00, 0x88, 0x00};
    const uint8 first3D[] = {0xfe, 0xfe, 0x7f, 0x7f, 0xfe, 0x7f, 0x00, 0x80,
                             0x00, 0x00, 0x7f, 0x7d, 0x7f, 0xfe, 0xfe, 0x7f};
    for (usize index = 0; index < sizeof(first2D); ++index) {
        CHECK(gradient2D[index] == first2D[index]);
        CHECK(gradient3D[index] == first3D[index]);
    }

    // 2D: 성분은 0·0x88·0xff뿐이고 A는 0이다. 같은 seed의 두 번째 호출도 같은 byte다(정적 표).
    bool components = true;
    for (usize index = 0; index < gradient2D.Size(); ++index) {
        const uint8 value = gradient2D[index];
        components = components &&
                     (index % NoiseTexelBytes == 3 ? value == 0
                                                   : value == 0 || value == 0x88 || value == 0xff);
    }

    CHECK(components);
    CHECK(PerlinGradient2DTexels().Data() == gradient2D.Data());

    // 3D: 마지막 면은 첫 면의 복제이고, A는 성분(0·0x7f·0xfe → −1·0·1)과 좌표의 내적 + 127이다.
    constexpr uint32 Extent = PerlinGradient3DExtent;
    constexpr uint32 Last = Extent - 1;
    bool borders = true;
    bool dots = true;
    for (uint32 z = 0; z < Extent; ++z) {
        for (uint32 y = 0; y < Extent; ++y) {
            for (uint32 x = 0; x < Extent; ++x) {
                const usize at = Texel3D(x, y, z);
                const uint32 sx = x == Last ? 0 : x;
                const uint32 sy = y == Last ? 0 : y;
                const uint32 sz = z == Last ? 0 : z;
                const usize source = Texel3D(sx, sy, sz);
                int32 dot = 0;
                const int32 coordinates[3] = {static_cast<int32>(x), static_cast<int32>(y),
                                              static_cast<int32>(z)};
                for (uint32 channel = 0; channel < 3; ++channel) {
                    const uint8 value = gradient3D[at + channel];
                    borders = borders && value == gradient3D[source + channel];
                    dots = dots && (value == 0 || value == 0x7f || value == 0xfe);
                    dot += (value / 0x7f - 1) * coordinates[channel];
                }

                dots = dots && gradient3D[at + 3] == static_cast<uint8>(dot + 127);
            }
        }
    }

    CHECK(borders);
    CHECK(dots);
}

TEST(RenderBase_NoiseTextureDescs) {
    const graphics::TextureDesc gradient2D = PerlinGradient2DDesc();
    CHECK(gradient2D.dimension == graphics::TextureDimension::Texture2D);
    CHECK(gradient2D.width == PerlinGradient2DExtent &&
          gradient2D.height == PerlinGradient2DExtent && gradient2D.depth == 1 &&
          gradient2D.arraySize == 1 && gradient2D.mipLevels == 1);
    CHECK(gradient2D.format == graphics::PixelFormat::RGBA8Unorm);
    CHECK(gradient2D.usage == graphics::TextureUsage::Sampled && gradient2D.sampleCount == 1);
    CHECK(gradient2D.sampler.filter == graphics::Filter::Point &&
          gradient2D.sampler.addressU == graphics::AddressMode::Wrap &&
          gradient2D.sampler.addressV == graphics::AddressMode::Wrap &&
          gradient2D.sampler.addressW == graphics::AddressMode::Wrap &&
          !gradient2D.sampler.comparison);
    CHECK(graphics::TextureMipPlaneBytes(gradient2D, 0, 0) == PerlinGradient2DBytes);

    const graphics::TextureDesc gradient3D = PerlinGradient3DDesc();
    CHECK(gradient3D.dimension == graphics::TextureDimension::Texture3D);
    CHECK(gradient3D.width == PerlinGradient3DExtent &&
          gradient3D.height == PerlinGradient3DExtent &&
          gradient3D.depth == PerlinGradient3DExtent && gradient3D.arraySize == 1 &&
          gradient3D.mipLevels == 1);
    CHECK(gradient3D.format == graphics::PixelFormat::RGBA8Unorm);
    CHECK(gradient3D.sampler.filter == graphics::Filter::Bilinear &&
          gradient3D.sampler.addressU == graphics::AddressMode::Wrap &&
          gradient3D.sampler.addressV == graphics::AddressMode::Wrap &&
          gradient3D.sampler.addressW == graphics::AddressMode::Wrap &&
          !gradient3D.sampler.comparison);
    CHECK(graphics::TextureMipPlaneBytes(gradient3D, 0, 0) == PerlinGradient3DBytes);
}

} // namespace noise_textures_tests_file
