#include <RenderBase/NoiseTextures.h>

// noise 시스템 텍스처 표다(Docs/Design/RenderBase 11.5절 4–6항). 두 표는 컴파일 때 만든다.
namespace rocks::render {

namespace {

// UE 5.8 FRandomStream과 같은 순서를 내는 stream이다(11.5절 4항 — UE 표와 byte 단위로 같게 만드는
// 데에만 쓴다. 범용 난수는 Core RandomStream).
class UeOrderStream {
public:
    explicit constexpr UeOrderStream(uint32 seed) noexcept : seed_(seed) {
    }

    // [0, 1) 분수다: 상태 상위 23 bit를 2^-23 간격 값으로 바꾼다(float 정확 — 23 bit 정수와 2의
    // 거듭제곱 곱).
    constexpr float NextFraction() noexcept {
        seed_ = seed_ * Multiplier + Increment;
        return static_cast<float>(seed_ >> FractionShift) * FractionScale;
    }

private:
    static constexpr uint32 Multiplier = 196314165u;
    static constexpr uint32 Increment = 907633515u;
    static constexpr uint32 FractionShift = 9;
    static constexpr float FractionScale = 1.0f / 8388608.0f;

    uint32 seed_;
};

// 12방향 표의 크기와 번호 배율이다. 배율은 UE 상수 그대로이며 float로 12.0이다(분수 < 1이므로
// 번호는 늘 0–11).
constexpr uint32 GradientDirections = 12;
constexpr float DirectionScale = 11.9999999f;

// 표 번호를 하나 뽑는다.
constexpr uint32 NextDirection(UeOrderStream& stream) noexcept {
    return static_cast<uint32>(stream.NextFraction() * DirectionScale);
}

// 2D 표의 seed와 방향(0x00RRGGBB — cube 모서리 중점, 0x88이 0 자리)이다(UE와 같다).
constexpr uint32 Gradient2DSeed = 12345;
constexpr uint32 Gradient2DTable[GradientDirections] = {0x88ffffu, 0xff88ffu, 0xffff88u, 0x88ff00u,
                                                        0xff8800u, 0xff0088u, 0x8800ffu, 0x0088ffu,
                                                        0x00ff88u, 0x880000u, 0x008800u, 0x000088u};

// 3D 표의 seed와 방향(0·0x7f·0xfe가 −1·0·1)이다(UE와 같다).
constexpr uint32 Gradient3DSeed = 0x1234;
constexpr uint32 Gradient3DTable[GradientDirections] = {0x7ffefeu, 0xfe7ffeu, 0xfefe7fu, 0x7ffe00u,
                                                        0xfe7f00u, 0xfe007fu, 0x7f00feu, 0x007ffeu,
                                                        0x00fe7fu, 0x7f0000u, 0x007f00u, 0x00007fu};
// 3D 성분 byte를 −1·0·1로 바꾸는 나눗수와 내적을 A에 담는 치우침이다.
constexpr int32 ComponentStep = 0x7f;
constexpr int32 DotBias = 127;

// 0x00RRGGBB 값의 성분 byte다.
constexpr uint32 RedShift = 16;
constexpr uint32 GreenShift = 8;
constexpr uint32 ByteMask = 0xffu;

// RGBA texel을 쓴다.
constexpr void WriteTexel(uint8* out, uint32 rgb, uint8 alpha) noexcept {
    out[0] = static_cast<uint8>((rgb >> RedShift) & ByteMask);
    out[1] = static_cast<uint8>((rgb >> GreenShift) & ByteMask);
    out[2] = static_cast<uint8>(rgb & ByteMask);
    out[3] = alpha;
}

struct Gradient2DData {
    uint8 bytes[PerlinGradient2DBytes];
};

struct Gradient3DData {
    uint8 bytes[PerlinGradient3DBytes];
};

// 11.5절 4항: 행 우선으로 texel마다 방향 하나, A는 0.
constexpr Gradient2DData MakeGradient2D() noexcept {
    Gradient2DData data{};
    UeOrderStream stream(Gradient2DSeed);
    constexpr usize Texels = static_cast<usize>(PerlinGradient2DExtent) * PerlinGradient2DExtent;
    for (usize texel = 0; texel < Texels; ++texel) {
        WriteTexel(data.bytes + texel * NoiseTexelBytes, Gradient2DTable[NextDirection(stream)], 0);
    }

    return data;
}

// 11.5절 5항: 안쪽 15³을 z·y·x 순으로 채우고, x·y·z 마지막 면을 첫 면에서 복제한 뒤, A에 내적 +
// 127.
constexpr Gradient3DData MakeGradient3D() noexcept {
    constexpr uint32 Extent = PerlinGradient3DExtent;
    constexpr uint32 Last = Extent - 1;
    constexpr uint32 Square = Extent * Extent;
    uint32 rgb[Extent * Extent * Extent] = {};
    UeOrderStream stream(Gradient3DSeed);
    for (uint32 z = 0; z < Last; ++z) {
        for (uint32 y = 0; y < Last; ++y) {
            for (uint32 x = 0; x < Last; ++x) {
                rgb[x + y * Extent + z * Square] = Gradient3DTable[NextDirection(stream)];
            }
        }
    }

    for (uint32 z = 0; z < Extent; ++z) {
        for (uint32 y = 0; y < Extent; ++y) {
            rgb[Last + y * Extent + z * Square] = rgb[y * Extent + z * Square];
        }
    }

    for (uint32 z = 0; z < Extent; ++z) {
        for (uint32 x = 0; x < Extent; ++x) {
            rgb[x + Last * Extent + z * Square] = rgb[x + z * Square];
        }
    }

    for (uint32 y = 0; y < Extent; ++y) {
        for (uint32 x = 0; x < Extent; ++x) {
            rgb[x + y * Extent + Last * Square] = rgb[x + y * Extent];
        }
    }

    Gradient3DData data{};
    for (uint32 z = 0; z < Extent; ++z) {
        for (uint32 y = 0; y < Extent; ++y) {
            for (uint32 x = 0; x < Extent; ++x) {
                const uint32 index = x + y * Extent + z * Square;
                const uint32 value = rgb[index];
                const int32 nx =
                    static_cast<int32>((value >> RedShift) & ByteMask) / ComponentStep - 1;
                const int32 ny =
                    static_cast<int32>((value >> GreenShift) & ByteMask) / ComponentStep - 1;
                const int32 nz = static_cast<int32>(value & ByteMask) / ComponentStep - 1;
                const int32 dot = nx * static_cast<int32>(x) + ny * static_cast<int32>(y) +
                                  nz * static_cast<int32>(z);
                WriteTexel(data.bytes + static_cast<usize>(index) * NoiseTexelBytes, value,
                           static_cast<uint8>(dot + DotBias));
            }
        }
    }

    return data;
}

constexpr Gradient2DData Gradient2D = MakeGradient2D();
constexpr Gradient3DData Gradient3D = MakeGradient3D();

graphics::TextureDesc NoiseDesc(graphics::TextureDimension dimension, uint32 extent,
                                graphics::Filter filter) noexcept {
    graphics::TextureDesc desc;
    desc.dimension = dimension;
    desc.width = extent;
    desc.height = extent;
    desc.depth = dimension == graphics::TextureDimension::Texture3D ? extent : 1;
    desc.format = graphics::PixelFormat::RGBA8Unorm;
    desc.usage = graphics::TextureUsage::Sampled;
    desc.mipLevels = 1;
    desc.sampler.filter = filter;
    desc.sampler.addressU = graphics::AddressMode::Wrap;
    desc.sampler.addressV = graphics::AddressMode::Wrap;
    desc.sampler.addressW = graphics::AddressMode::Wrap;
    return desc;
}

} // namespace

Span<const uint8> PerlinGradient2DTexels() noexcept {
    return Span<const uint8>(Gradient2D.bytes, PerlinGradient2DBytes);
}

Span<const uint8> PerlinGradient3DTexels() noexcept {
    return Span<const uint8>(Gradient3D.bytes, PerlinGradient3DBytes);
}

graphics::TextureDesc PerlinGradient2DDesc() noexcept {
    return NoiseDesc(graphics::TextureDimension::Texture2D, PerlinGradient2DExtent,
                     graphics::Filter::Point);
}

graphics::TextureDesc PerlinGradient3DDesc() noexcept {
    return NoiseDesc(graphics::TextureDimension::Texture3D, PerlinGradient3DExtent,
                     graphics::Filter::Bilinear);
}

} // namespace rocks::render
