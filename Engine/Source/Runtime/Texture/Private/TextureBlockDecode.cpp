#include "TextureBlockBptcDecode.h"

#include <Base/NumericLimits.h>
#include <Texture/TextureBlockDecode.h>

// 블록 형식 CPU 디코더다(Docs/Design/AssetSystem 12.17.7). BC1–BC5는 NVTT BlockDXT
// 정수식(BlockDXT.cpp 36–140·343–380 — 12.17절 TD7)이고, BC6H·BC7은 TextureBlockDecodeBptc.cpp,
// EAC R11은 OpenGL ES 3.0 부록 C.1.5다(12.17.8 4).
namespace rocks {

namespace {

// 유니티 빌드에서 같은 묶음의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace block_decode {

constexpr uint32 SmallBlockBytes = 8;
constexpr uint32 LargeBlockBytes = 16;
constexpr uint32 ByteBits = 8;
// 알파 블록 index는 끝점 두 byte 뒤의 3 bit씩이다.
constexpr uint32 AlphaIndexShift = 16;
constexpr uint32 AlphaIndexBits = 3;
constexpr uint64 AlphaIndexMask = 0x7;
// 색 블록 index는 끝점 네 byte 뒤의 2 bit씩이다.
constexpr uint32 ColorIndexOffset = 4;
constexpr uint32 ColorIndexBits = 2;
constexpr uint32 ColorIndexMask = 0x3;
constexpr uint32 ColorPaletteSize = 4;
constexpr uint32 ColorChannels = 3;
constexpr uint8 Opaque = 0xFF;
// ASTC 블록 byte 수다(블록 크기와 무관하게 128 bit).
constexpr uint32 AstcBlockBytes = 16;
// EAC R11 블록(big-endian 64 bit)의 칸 위치다: base bit 63–56, multiplier 55–52, 표 51–48, index는
// texel a(0,0)부터 열 우선으로 3 bit씩 47–0.
constexpr uint32 EacBaseShift = 56;
constexpr uint32 EacMultiplierShift = 52;
constexpr uint32 EacTableShift = 48;
constexpr uint32 EacFieldMask = 0xF;
constexpr uint32 EacIndexTopShift = 45;
constexpr uint32 EacIndexBits = 3;
constexpr uint64 EacIndexMask = 0x7;

uint64 ReadLittle64(const uint8* bytes) noexcept {
    uint64 value = 0;
    for (uint32 i = 0; i < SmallBlockBytes; ++i) {
        value |= uint64{bytes[i]} << (ByteBits * i);
    }

    return value;
}

// 알파 블록 하나를 16 값으로 푼다(AlphaBlockDXT5::decodeBlock).
void DecodeAlpha(const uint8* bytes, uint8 (&values)[TextureBlockTexels]) noexcept {
    const uint64 block = ReadLittle64(bytes);
    uint8 palette[TextureBlockAlphaPaletteSize];
    TextureBlockAlphaPalette(bytes[0], bytes[1], palette);
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        values[i] = palette[(block >> (AlphaIndexShift + AlphaIndexBits * i)) & AlphaIndexMask];
    }
}

// 565 끝점을 8비트 RGB로 넓힌다(위 bit를 아래에 되풀이).
void Expand565(uint32 color, uint8 (&rgb)[ColorChannels]) noexcept {
    const uint32 r = (color >> 11) & 0x1F;
    const uint32 g = (color >> 5) & 0x3F;
    const uint32 b = color & 0x1F;
    rgb[0] = static_cast<uint8>((r << 3) | (r >> 2));
    rgb[1] = static_cast<uint8>((g << 2) | (g >> 4));
    rgb[2] = static_cast<uint8>((b << 3) | (b >> 2));
}

// 색 블록 하나를 RGBA로 푼다(BlockDXT1::decodeBlock — 끝점 비교로 4색·3색). fourColor면 끝점 순서와
// 무관하게 4색이다(D3D 명세 — BC2·BC3 색 블록은 늘 4색, 하드웨어·DirectXTex D3DXDecodeBC3와 같다).
void DecodeColour(const uint8* bytes, bool fourColor,
                  uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    const uint32 col0 = uint32{bytes[0]} | (uint32{bytes[1]} << ByteBits);
    const uint32 col1 = uint32{bytes[2]} | (uint32{bytes[3]} << ByteBits);
    uint8 palette[ColorPaletteSize][TextureBlockTexelBytes] = {};
    uint8 c0[ColorChannels];
    uint8 c1[ColorChannels];
    Expand565(col0, c0);
    Expand565(col1, c1);
    const bool four = fourColor || col0 > col1;
    for (uint32 k = 0; k < ColorChannels; ++k) {
        palette[0][k] = c0[k];
        palette[1][k] = c1[k];
        if (four) {
            palette[2][k] = static_cast<uint8>((2 * c0[k] + c1[k]) / 3);
            palette[3][k] = static_cast<uint8>((2 * c1[k] + c0[k]) / 3);
        } else {
            palette[2][k] = static_cast<uint8>((c0[k] + c1[k]) / 2);
            palette[3][k] = 0;
        }
    }

    palette[0][3] = Opaque;
    palette[1][3] = Opaque;
    palette[2][3] = Opaque;
    palette[3][3] = four ? Opaque : uint8{0};
    for (uint32 i = 0; i < TextureBlockTexels; ++i) {
        const uint32 index = (bytes[ColorIndexOffset + i / TextureBlockSize] >>
                              (ColorIndexBits * (i % TextureBlockSize))) &
                             ColorIndexMask;
        for (uint32 k = 0; k < TextureBlockTexelBytes; ++k) {
            texels[i * TextureBlockTexelBytes + k] = palette[index][k];
        }
    }
}

uint64 ReadBig64(const uint8* bytes) noexcept {
    uint64 value = 0;
    for (uint32 i = 0; i < SmallBlockBytes; ++i) {
        value = (value << ByteBits) | bytes[i];
    }

    return value;
}

// ASTC 형식의 블록 한 변 크기다(BC·EAC는 0).
uint32 AstcBlockEdge(TextureBlockFormat format) noexcept {
    switch (format) {
    case TextureBlockFormat::Astc4x4:
        return 4;
    case TextureBlockFormat::Astc6x6:
        return 6;
    case TextureBlockFormat::Astc8x8:
        return 8;
    case TextureBlockFormat::Astc10x10:
        return 10;
    case TextureBlockFormat::Astc12x12:
        return 12;
    default:
        return 0;
    }
}

} // namespace block_decode

} // namespace

uint32 TextureBlockBytes(TextureBlockFormat format) noexcept {
    switch (format) {
    case TextureBlockFormat::Bc1:
    case TextureBlockFormat::Bc4:
    case TextureBlockFormat::EacR11:
        return block_decode::SmallBlockBytes;
    case TextureBlockFormat::Bc3:
    case TextureBlockFormat::Bc5:
    case TextureBlockFormat::Bc6h:
    case TextureBlockFormat::Bc7:
        return block_decode::LargeBlockBytes;
    case TextureBlockFormat::Astc4x4:
    case TextureBlockFormat::Astc6x6:
    case TextureBlockFormat::Astc8x8:
    case TextureBlockFormat::Astc10x10:
    case TextureBlockFormat::Astc12x12:
        return block_decode::AstcBlockBytes;
    }

    return 0;
}

uint32 TextureBlockWidth(TextureBlockFormat format) noexcept {
    const uint32 astc = block_decode::AstcBlockEdge(format);
    return astc != 0 ? astc : TextureBlockBytes(format) != 0 ? TextureBlockSize : 0;
}

uint32 TextureBlockHeight(TextureBlockFormat format) noexcept {
    return TextureBlockWidth(format);
}

Result<uint64> TextureBlockOutputBytes(TextureBlockFormat format, uint32 sizeX, uint32 sizeY,
                                       uint32 numSlices) noexcept {
    if (sizeX == 0 || sizeY == 0 || numSlices == 0 || TextureBlockBytes(format) == 0) {
        return Result<uint64>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 width = TextureBlockWidth(format);
    const uint64 height = TextureBlockHeight(format);
    const uint64 blocksX = (uint64{sizeX} + width - 1) / width;
    const uint64 blocksY = (uint64{sizeY} + height - 1) / height;
    // blocksX·blocksY는 2^30 이하라 둘의 곱은 넘치지 않는다. 블록 byte·slice를 곱할 때 넘칠 수
    // 있다.
    const uint64 blocks = blocksX * blocksY;
    const uint64 blockBytes = TextureBlockBytes(format);
    if (blocks > NumericLimits<uint64>::Max() / blockBytes) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    const uint64 perSlice = blocks * blockBytes;
    if (perSlice > NumericLimits<uint64>::Max() / numSlices) {
        return Result<uint64>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<uint64>::Success(perSlice * numSlices);
}

void TextureBlockAlphaPalette(uint32 alpha0, uint32 alpha1,
                              uint8 (&palette)[TextureBlockAlphaPaletteSize]) noexcept {
    // 8값 모드의 보간 단계 수(7)와 6값 모드의 단계 수(5)다.
    constexpr uint32 EightSteps = 7;
    constexpr uint32 SixSteps = 5;
    const uint32 a0 = alpha0 & 0xFF;
    const uint32 a1 = alpha1 & 0xFF;
    palette[0] = static_cast<uint8>(a0);
    palette[1] = static_cast<uint8>(a1);
    if (a0 > a1) {
        for (uint32 k = 1; k < EightSteps; ++k) {
            palette[1 + k] = static_cast<uint8>(((EightSteps - k) * a0 + k * a1) / EightSteps);
        }
    } else {
        for (uint32 k = 1; k < SixSteps; ++k) {
            palette[1 + k] = static_cast<uint8>(((SixSteps - k) * a0 + k * a1) / SixSteps);
        }

        palette[6] = 0x00;
        palette[7] = 0xFF;
    }
}

void DecodeTextureBlock(TextureBlockFormat format, const uint8* block,
                        uint8 (&texels)[TextureBlockRgbaBytes]) noexcept {
    using block_decode::DecodeAlpha;
    using block_decode::DecodeColour;
    using block_decode::SmallBlockBytes;
    uint8 first[TextureBlockTexels];
    uint8 second[TextureBlockTexels];
    switch (format) {
    case TextureBlockFormat::Bc1:
        DecodeColour(block, false, texels);
        break;
    case TextureBlockFormat::Bc3:
        DecodeColour(block + SmallBlockBytes, true, texels);
        DecodeAlpha(block, first);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            texels[i * TextureBlockTexelBytes + 3] = first[i];
        }

        break;
    case TextureBlockFormat::Bc4:
        DecodeAlpha(block, first);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            uint8* texel = texels + i * TextureBlockTexelBytes;
            texel[0] = first[i];
            texel[1] = 0;
            texel[2] = 0;
            texel[3] = block_decode::Opaque;
        }

        break;
    case TextureBlockFormat::Bc5:
        DecodeAlpha(block, first);
        DecodeAlpha(block + SmallBlockBytes, second);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            uint8* texel = texels + i * TextureBlockTexelBytes;
            texel[0] = first[i];
            texel[1] = second[i];
            texel[2] = 0;
            texel[3] = block_decode::Opaque;
        }

        break;
    case TextureBlockFormat::Bc7:
        texture_block_detail::DecodeBc7Block(block, texels);
        break;
    case TextureBlockFormat::EacR11: {
        uint16 values[TextureBlockTexels];
        DecodeEacR11Block(block, values);
        for (uint32 i = 0; i < TextureBlockTexels; ++i) {
            uint8* texel = texels + i * TextureBlockTexelBytes;
            // 11 bit → 8 bit 가장 가까운 값(round(x · 255 / 2047)).
            texel[0] = static_cast<uint8>((uint32{values[i]} * 255 + TextureEacR11Max / 2) /
                                          TextureEacR11Max);
            texel[1] = 0;
            texel[2] = 0;
            texel[3] = block_decode::Opaque;
        }

        break;
    }
    case TextureBlockFormat::Bc6h:
    case TextureBlockFormat::Astc4x4:
    case TextureBlockFormat::Astc6x6:
    case TextureBlockFormat::Astc8x8:
    case TextureBlockFormat::Astc10x10:
    case TextureBlockFormat::Astc12x12:
        for (uint8& value : texels) {
            value = 0;
        }

        break;
    }
}

void DecodeTextureBlockHalf(const uint8* block, uint16 (&halfs)[TextureBlockTexels * 4]) noexcept {
    texture_block_detail::DecodeBc6hBlock(block, halfs);
}

void DecodeEacR11Block(const uint8* block, uint16 (&values)[TextureBlockTexels]) noexcept {
    using namespace block_decode;
    const uint64 bits = ReadBig64(block);
    const uint32 base = static_cast<uint32>(bits >> EacBaseShift) & 0xFF;
    const uint32 multiplier = static_cast<uint32>(bits >> EacMultiplierShift) & EacFieldMask;
    const uint32 table = static_cast<uint32>(bits >> EacTableShift) & EacFieldMask;
    for (uint32 column = 0; column < TextureBlockSize; ++column) {
        for (uint32 row = 0; row < TextureBlockSize; ++row) {
            const uint32 order = column * TextureBlockSize + row;
            const uint32 index =
                static_cast<uint32>(bits >> (EacIndexTopShift - EacIndexBits * order)) &
                EacIndexMask;
            values[row * TextureBlockSize + column] = static_cast<uint16>(
                TextureEacR11Value(base, multiplier, TextureEacModifiers[table][index]));
        }
    }
}

} // namespace rocks
