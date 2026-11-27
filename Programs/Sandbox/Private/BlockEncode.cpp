#include "BlockEncode.h"

#include "TexelCodec.h"

#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <Platform/Memory.h>
#include <Texture/TextureBlockDecode.h>
#include <math.h>

namespace sandbox {

namespace {

// 유니티 빌드에서 같은 묶음의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace block_encode {

constexpr uint32 BlockSize = TextureBlockSize;
constexpr uint32 BlockTexels = TextureBlockTexels;
constexpr uint32 Channels = 4;
constexpr uint32 ColorChannels = 3;
// 8 byte 블록(BC1·BC4, BC3·BC5의 앞 절반)이다.
constexpr uint32 SmallBlockBytes = 8;
// 16 byte 블록(BC3·BC5·BC6H·BC7)이다.
constexpr uint32 LargeBlockBytes = 16;
constexpr double Unorm8Max = 255.0;
// 565 성분 최대값이다.
constexpr uint32 Max5 = 31;
constexpr uint32 Max6 = 63;
// BC1 2-bit index와 BC4 3-bit index의 비트 수다.
constexpr uint32 ColorIndexBits = 2;
constexpr uint32 ValueIndexBits = 3;
constexpr uint32 ColorPaletteSize = 4;
// BC1 블록 index byte 첫 칸에 texel 0…3 = index 0…3을 담는 값이다(팔레트 계산 — 2 bit씩).
constexpr uint8 ColorPaletteIndices = 0xE4;
// BC6H·BC7 4 bit index 하나의 bit 수, 팔레트 크기, anchor(texel 0)의 bit 수다(맨 위 bit가 0).
constexpr uint32 BptcIndexBits = 4;
constexpr uint32 BptcPaletteSize = 16;
constexpr uint32 BptcAnchorBits = BptcIndexBits - 1;
constexpr uint32 BptcAnchorLimit = BptcPaletteSize / 2;
// BC7 mode 6: 머리 7 bit(0이 여섯, 그다음 1), 끝점 7 bit, p-bit 하나씩이다.
constexpr uint32 Bc7Mode6Bits = 7;
constexpr uint32 Bc7Mode6Marker = 1u << 6;
constexpr uint32 Bc7EndpointBits = 7;
constexpr uint32 Bc7EndpointMax = (1u << Bc7EndpointBits) - 1;
// BC6H mode 11: 머리 5 bit(값 0x03), 끝점 10 bit다.
constexpr uint32 Bc6hModeBits = 5;
constexpr uint32 Bc6hMode11 = 0x03;
constexpr uint32 Bc6hEndpointBits = 10;
constexpr uint32 Bc6hEndpointMax = (1u << Bc6hEndpointBits) - 1;

uint64 LoadBits(const uint8* in, uint32 bytes) noexcept {
    uint64 value = 0;
    for (uint32 index = 0; index < bytes; ++index) {
        value |= static_cast<uint64>(in[index]) << (8 * index);
    }

    return value;
}

void StoreBits(uint8* out, uint64 value, uint32 bytes) noexcept {
    for (uint32 index = 0; index < bytes; ++index) {
        out[index] = static_cast<uint8>(value >> (8 * index));
    }
}

// 16 byte 블록에 낮은 bit부터 쓴다(BC6H·BC7 배치).
struct BitWriter {
    uint8* bytes;
    uint32 position = 0;

    void Write(uint32 value, uint32 count) noexcept {
        for (uint32 bit = 0; bit < count; ++bit) {
            if (((value >> bit) & 1u) != 0) {
                bytes[position >> 3] =
                    static_cast<uint8>(bytes[position >> 3] | (1u << (position & 7)));
            }

            ++position;
        }
    }
};

// 블록 하나의 staging texel이다(RGBA8 — 0…255, RGBA16Float — float 값).
struct BlockTexelsIn {
    double value[BlockTexels][Channels];
};

double SquaredDistance(const double* a, const double* b, uint32 channels) noexcept {
    double sum = 0.0;
    for (uint32 channel = 0; channel < channels; ++channel) {
        const double delta = a[channel] - b[channel];
        sum += delta * delta;
    }

    return sum;
}

// 가장 먼 두 texel이다(channels 성분 거리). 모두 같으면 둘 다 0이다.
void FarthestPair(const BlockTexelsIn& texels, uint32 channels, uint32& first,
                  uint32& second) noexcept {
    first = 0;
    second = 0;
    double farthest = 0.0;
    for (uint32 a = 0; a < BlockTexels; ++a) {
        for (uint32 b = a + 1; b < BlockTexels; ++b) {
            const double distance = SquaredDistance(texels.value[a], texels.value[b], channels);
            if (distance > farthest) {
                farthest = distance;
                first = a;
                second = b;
            }
        }
    }
}

// palette 가운데 texel에 가장 가까운 index다(같으면 앞 index).
uint32 NearestIndex(const double (*palette)[Channels], uint32 size, const double* texel,
                    uint32 channels) noexcept {
    uint32 best = 0;
    double bestDistance = SquaredDistance(palette[0], texel, channels);
    for (uint32 index = 1; index < size; ++index) {
        const double distance = SquaredDistance(palette[index], texel, channels);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = index;
        }
    }

    return best;
}

uint32 Pack565(const double* rgba) noexcept {
    const uint32 r = static_cast<uint32>(floor(rgba[0] * Max5 / Unorm8Max + 0.5));
    const uint32 g = static_cast<uint32>(floor(rgba[1] * Max6 / Unorm8Max + 0.5));
    const uint32 b = static_cast<uint32>(floor(rgba[2] * Max5 / Unorm8Max + 0.5));
    return (r << 11) | (g << 5) | b;
}

// BC1·BC3 색 블록(8 byte)이다. 4색 모드(c0 > c1)를 지킨다(같으면 모든 index 0). 팔레트는 공용
// 디코더가 푼 texel 0…3(index 0…3)이다.
void EncodeColor(const BlockTexelsIn& texels, uint8* out) noexcept {
    uint32 first = 0;
    uint32 second = 0;
    FarthestPair(texels, ColorChannels, first, second);
    uint32 c0 = Pack565(texels.value[first]);
    uint32 c1 = Pack565(texels.value[second]);
    if (c0 < c1) {
        const uint32 swap = c0;
        c0 = c1;
        c1 = swap;
    }

    uint64 indices = 0;
    if (c0 != c1) {
        uint8 block[SmallBlockBytes] = {};
        StoreBits(block, c0, 2);
        StoreBits(block + 2, c1, 2);
        block[4] = ColorPaletteIndices;
        uint8 decoded[TextureBlockRgbaBytes];
        DecodeTextureBlock(TextureBlockFormat::Bc1, block, decoded);
        double palette[ColorPaletteSize][Channels];
        for (uint32 index = 0; index < ColorPaletteSize; ++index) {
            for (uint32 channel = 0; channel < Channels; ++channel) {
                palette[index][channel] = decoded[index * TextureBlockTexelBytes + channel];
            }
        }

        for (uint32 texel = 0; texel < BlockTexels; ++texel) {
            const uint32 best =
                NearestIndex(palette, ColorPaletteSize, texels.value[texel], ColorChannels);
            indices |= static_cast<uint64>(best) << (ColorIndexBits * texel);
        }
    }

    StoreBits(out, c0, 2);
    StoreBits(out + 2, c1, 2);
    StoreBits(out + 4, indices, 4);
}

// channel 성분 값 블록(8 byte)이다(끝점 최대·최소 — 8값 모드, 같으면 모든 index 0). 팔레트는 공용
// TextureBlockAlphaPalette다.
void EncodeValue(const BlockTexelsIn& texels, uint32 channel, uint8* out) noexcept {
    uint32 high = 0;
    uint32 low = NumericLimits<uint8>::Max();
    for (uint32 texel = 0; texel < BlockTexels; ++texel) {
        const uint32 value = static_cast<uint32>(texels.value[texel][channel]);
        high = value > high ? value : high;
        low = value < low ? value : low;
    }

    uint64 indices = 0;
    if (high != low) {
        uint8 palette[TextureBlockAlphaPaletteSize];
        TextureBlockAlphaPalette(high, low, palette);
        for (uint32 texel = 0; texel < BlockTexels; ++texel) {
            const double value = texels.value[texel][channel];
            uint32 best = 0;
            double bestDistance = fabs(palette[0] - value);
            for (uint32 index = 1; index < TextureBlockAlphaPaletteSize; ++index) {
                const double distance = fabs(palette[index] - value);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }

            indices |= static_cast<uint64>(best) << (ValueIndexBits * texel);
        }
    }

    out[0] = static_cast<uint8>(high);
    out[1] = static_cast<uint8>(low);
    StoreBits(out + 2, indices, 6);
}

// 4 bit index 열(texel 0은 anchor 3 bit)을 쓴다. indices[0]은 BptcAnchorLimit보다 작아야 한다.
void WriteBptcIndices(BitWriter& writer, const uint32 (&indices)[BlockTexels]) noexcept {
    writer.Write(indices[0], BptcAnchorBits);
    for (uint32 texel = 1; texel < BlockTexels; ++texel) {
        writer.Write(indices[texel], BptcIndexBits);
    }
}

// 모든 texel의 index를 고르고 anchor 규칙(texel 0의 맨 위 bit 0)을 맞춘다. 맞추려면 끝점을 바꾸고
// index를 뒤집는다(가중치 표가 대칭 — w[15 − i] = 64 − w[i]). 바꿨으면 true다.
bool ChooseBptcIndices(const double (&palette)[BptcPaletteSize][Channels],
                       const BlockTexelsIn& texels, uint32 channels,
                       uint32 (&indices)[BlockTexels]) noexcept {
    for (uint32 texel = 0; texel < BlockTexels; ++texel) {
        indices[texel] = NearestIndex(palette, BptcPaletteSize, texels.value[texel], channels);
    }

    if (indices[0] < BptcAnchorLimit) {
        return false;
    }

    for (uint32& index : indices) {
        index = BptcPaletteSize - 1 - index;
    }

    return true;
}

// BC7 mode 6 블록(끝점 둘 — RGBA 7 bit와 p-bit)을 쓴다.
void WriteBc7Mode6(const uint32 (&endpoint)[2][Channels], const uint32 (&pBit)[2],
                   const uint32 (&indices)[BlockTexels], uint8* out) noexcept {
    for (uint32 index = 0; index < LargeBlockBytes; ++index) {
        out[index] = 0;
    }

    BitWriter writer{out};
    writer.Write(Bc7Mode6Marker, Bc7Mode6Bits);
    for (uint32 channel = 0; channel < Channels; ++channel) {
        writer.Write(endpoint[0][channel], Bc7EndpointBits);
        writer.Write(endpoint[1][channel], Bc7EndpointBits);
    }

    writer.Write(pBit[0], 1);
    writer.Write(pBit[1], 1);
    WriteBptcIndices(writer, indices);
}

// 8 bit 끝점 값(RGBA)을 mode 6의 7 bit 넷과 p-bit 하나로 줄인다(오차 제곱합이 작은 p-bit).
void QuantizeBc7Endpoint(const double* rgba, uint32 (&endpoint)[Channels], uint32& pBit) noexcept {
    double bestError = -1.0;
    for (uint32 p = 0; p < 2; ++p) {
        uint32 candidate[Channels];
        double error = 0.0;
        for (uint32 channel = 0; channel < Channels; ++channel) {
            double quantized = floor((rgba[channel] - p) / 2.0 + 0.5);
            quantized =
                quantized < 0.0 ? 0.0 : (quantized > Bc7EndpointMax ? Bc7EndpointMax : quantized);
            candidate[channel] = static_cast<uint32>(quantized);
            const double value = static_cast<double>((candidate[channel] << 1) | p);
            error += (value - rgba[channel]) * (value - rgba[channel]);
        }

        if (bestError < 0.0 || error < bestError) {
            bestError = error;
            pBit = p;
            for (uint32 channel = 0; channel < Channels; ++channel) {
                endpoint[channel] = candidate[channel];
            }
        }
    }
}

// texel i의 index = i인 index 열이다(팔레트 계산 — 팔레트 값 i가 texel i에 풀린다).
void IdentityIndices(uint32 (&indices)[BlockTexels]) noexcept {
    for (uint32 texel = 0; texel < BlockTexels; ++texel) {
        indices[texel] = texel;
    }
}

// BC7 블록(16 byte, mode 6)이다.
void EncodeBc7(const BlockTexelsIn& texels, uint8* out) noexcept {
    uint32 first = 0;
    uint32 second = 0;
    FarthestPair(texels, Channels, first, second);
    uint32 endpoint[2][Channels];
    uint32 pBit[2];
    QuantizeBc7Endpoint(texels.value[first], endpoint[0], pBit[0]);
    QuantizeBc7Endpoint(texels.value[second], endpoint[1], pBit[1]);
    uint32 indices[BlockTexels];
    IdentityIndices(indices);
    WriteBc7Mode6(endpoint, pBit, indices, out);
    uint8 decoded[TextureBlockRgbaBytes];
    DecodeTextureBlock(TextureBlockFormat::Bc7, out, decoded);
    double palette[BptcPaletteSize][Channels];
    for (uint32 index = 0; index < BptcPaletteSize; ++index) {
        for (uint32 channel = 0; channel < Channels; ++channel) {
            palette[index][channel] = decoded[index * TextureBlockTexelBytes + channel];
        }
    }

    if (ChooseBptcIndices(palette, texels, Channels, indices)) {
        for (uint32 channel = 0; channel < Channels; ++channel) {
            const uint32 swap = endpoint[0][channel];
            endpoint[0][channel] = endpoint[1][channel];
            endpoint[1][channel] = swap;
        }

        const uint32 swap = pBit[0];
        pBit[0] = pBit[1];
        pBit[1] = swap;
    }

    WriteBc7Mode6(endpoint, pBit, indices, out);
}

// BC6H mode 11 블록(끝점 둘 — RGB 10 bit)을 쓴다.
void WriteBc6hMode11(const uint32 (&endpoint)[2][ColorChannels],
                     const uint32 (&indices)[BlockTexels], uint8* out) noexcept {
    for (uint32 index = 0; index < LargeBlockBytes; ++index) {
        out[index] = 0;
    }

    BitWriter writer{out};
    writer.Write(Bc6hMode11, Bc6hModeBits);
    for (uint32 point = 0; point < 2; ++point) {
        for (uint32 channel = 0; channel < ColorChannels; ++channel) {
            writer.Write(endpoint[point][channel], Bc6hEndpointBits);
        }
    }

    WriteBptcIndices(writer, indices);
}

// 10 bit 끝점 값 q가 풀리는 half bit다(끝점 둘이 q인 블록을 공용 디코더로 푼다).
uint16 DecodedBc6hEndpoint(uint32 q) noexcept {
    const uint32 endpoint[2][ColorChannels] = {{q, q, q}, {q, q, q}};
    const uint32 indices[BlockTexels] = {};
    uint8 block[LargeBlockBytes];
    WriteBc6hMode11(endpoint, indices, block);
    uint16 halfs[TextureBlockTexels * 4];
    DecodeTextureBlockHalf(block, halfs);
    return halfs[0];
}

// half bit(0 이상)에 가장 가까운 10 bit 끝점 값이다. 풀린 값은 q에 대해 단조 증가라 이분 탐색으로
// 경계를 찾고 양옆을 비교한다.
uint32 QuantizeBc6hEndpoint(double value) noexcept {
    const double target = value > 0.0 ? value : 0.0;
    uint32 low = 0;
    uint32 high = Bc6hEndpointMax;
    while (low < high) {
        const uint32 middle = (low + high) / 2;
        if (Float16::FromBits(DecodedBc6hEndpoint(middle)).ToFloat() < target) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == 0) {
        return 0;
    }

    const double above = Float16::FromBits(DecodedBc6hEndpoint(low)).ToFloat();
    const double below = Float16::FromBits(DecodedBc6hEndpoint(low - 1)).ToFloat();
    return target - below <= above - target ? low - 1 : low;
}

// BC6H 블록(16 byte, mode 11 — Ufloat)이다.
void EncodeBc6h(const BlockTexelsIn& texels, uint8* out) noexcept {
    uint32 first = 0;
    uint32 second = 0;
    FarthestPair(texels, ColorChannels, first, second);
    uint32 endpoint[2][ColorChannels];
    for (uint32 channel = 0; channel < ColorChannels; ++channel) {
        endpoint[0][channel] = QuantizeBc6hEndpoint(texels.value[first][channel]);
        endpoint[1][channel] = QuantizeBc6hEndpoint(texels.value[second][channel]);
    }

    uint32 indices[BlockTexels];
    IdentityIndices(indices);
    WriteBc6hMode11(endpoint, indices, out);
    uint16 halfs[TextureBlockTexels * 4];
    DecodeTextureBlockHalf(out, halfs);
    double palette[BptcPaletteSize][Channels];
    for (uint32 index = 0; index < BptcPaletteSize; ++index) {
        for (uint32 channel = 0; channel < Channels; ++channel) {
            palette[index][channel] =
                Float16::FromBits(halfs[index * Channels + channel]).ToFloat();
        }
    }

    if (ChooseBptcIndices(palette, texels, ColorChannels, indices)) {
        for (uint32 channel = 0; channel < ColorChannels; ++channel) {
            const uint32 swap = endpoint[0][channel];
            endpoint[0][channel] = endpoint[1][channel];
            endpoint[1][channel] = swap;
        }
    }

    WriteBc6hMode11(endpoint, indices, out);
}

// staging texel 하나를 값 넷으로 읽는다(RGBA8은 byte 값, RGBA16Float는 float 값).
void LoadStagingTexel(const uint8* in, bool half, double (&out)[Channels]) noexcept {
    for (uint32 channel = 0; channel < Channels; ++channel) {
        out[channel] =
            half ? Float16::FromBits(static_cast<uint16>(LoadBits(in + 2 * channel, 2))).ToFloat()
                 : static_cast<double>(in[channel]);
    }
}

} // namespace block_encode

} // namespace

bool IsBlockCodecFormat(graphics::PixelFormat format) noexcept {
    TextureBlockFormat block = TextureBlockFormat::Bc1;
    return TextureBlockFormatOf(format, block);
}

graphics::PixelFormat BlockStagingFormat(graphics::PixelFormat format) noexcept {
    TextureBlockFormat block = TextureBlockFormat::Bc1;
    if (!TextureBlockFormatOf(format, block)) {
        return format;
    }

    if (block == TextureBlockFormat::Bc6h) {
        return graphics::PixelFormat::RGBA16Float;
    }

    return graphics::GetPixelFormatInfo(format).srgb ? graphics::PixelFormat::RGBA8UnormSrgb
                                                     : graphics::PixelFormat::RGBA8Unorm;
}

bool EncodeBlockImage(graphics::PixelFormat format, Span<const uint8> rgba, uint32 width,
                      uint32 height, uint32 planes, Span<uint8> out) noexcept {
    TextureBlockFormat block = TextureBlockFormat::Bc1;
    if (!TextureBlockFormatOf(format, block)) {
        return false;
    }

    const bool half = block == TextureBlockFormat::Bc6h;
    const usize texelBytes = half ? TextureBlockHalfTexelBytes : TextureBlockTexelBytes;
    if (width == 0 || height == 0 || planes == 0 ||
        rgba.Size() != static_cast<usize>(width) * height * planes * texelBytes ||
        out.Size() != graphics::PixelFormatImageBytes(format, width, height) * planes) {
        return false;
    }

    const usize blockBytes = TextureBlockBytes(block);
    const uint32 blocksX = (width + block_encode::BlockSize - 1) / block_encode::BlockSize;
    const uint32 blocksY = (height + block_encode::BlockSize - 1) / block_encode::BlockSize;
    uint8* target = out.Data();
    for (uint32 plane = 0; plane < planes; ++plane) {
        for (uint32 blockY = 0; blockY < blocksY; ++blockY) {
            for (uint32 blockX = 0; blockX < blocksX; ++blockX) {
                block_encode::BlockTexelsIn texels;
                for (uint32 texel = 0; texel < block_encode::BlockTexels; ++texel) {
                    uint32 x = blockX * block_encode::BlockSize + texel % block_encode::BlockSize;
                    uint32 y = blockY * block_encode::BlockSize + texel / block_encode::BlockSize;
                    x = x < width ? x : width - 1;
                    y = y < height ? y : height - 1;
                    const usize source =
                        ((static_cast<usize>(plane) * height + y) * width + x) * texelBytes;
                    block_encode::LoadStagingTexel(rgba.Data() + source, half, texels.value[texel]);
                }

                switch (block) {
                case TextureBlockFormat::Bc1:
                    block_encode::EncodeColor(texels, target);
                    break;
                case TextureBlockFormat::Bc3:
                    block_encode::EncodeValue(texels, 3, target);
                    block_encode::EncodeColor(texels, target + block_encode::SmallBlockBytes);
                    break;
                case TextureBlockFormat::Bc4:
                    block_encode::EncodeValue(texels, 0, target);
                    break;
                case TextureBlockFormat::Bc5:
                    block_encode::EncodeValue(texels, 0, target);
                    block_encode::EncodeValue(texels, 1, target + block_encode::SmallBlockBytes);
                    break;
                case TextureBlockFormat::Bc6h:
                    block_encode::EncodeBc6h(texels, target);
                    break;
                case TextureBlockFormat::Bc7:
                    block_encode::EncodeBc7(texels, target);
                    break;
                }

                target += blockBytes;
            }
        }
    }

    return true;
}

} // namespace sandbox
