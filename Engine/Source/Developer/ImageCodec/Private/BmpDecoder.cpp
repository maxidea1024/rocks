#include "ImageCodecInternal.h"

// BMP 디코더다(Docs/Design/AssetSystem 12.2·12.9절): BITMAPINFOHEADER(40)·V2(52)·V3(56)·V4(108)·
// V5(124), 1·4·8비트 색표(BI_RGB·RLE8·RLE4), 16·24·32비트 BI_RGB·BITFIELDS·ALPHABITFIELDS. 결과는
// 늘 RGBA8이다. 알파는 UE BmpImageWrapper와 같다(32비트 BI_RGB 넷째 byte — 모두 0이면 255, 마스크
// 알파는 V4·V5만).
namespace rocks::image::detail {
namespace {

constexpr usize BmpFileHeaderBytes = 14;
constexpr uint32 BmpInfoHeaderBytes = 40;
constexpr uint32 BmpV2HeaderBytes = 52;
constexpr uint32 BmpV3HeaderBytes = 56;
constexpr uint32 BmpV4HeaderBytes = 108;
constexpr uint32 BmpV5HeaderBytes = 124;
constexpr uint32 BmpCoreHeaderBytes = 12;
constexpr uint32 BmpPaletteEntryBytes = 4;
constexpr uint32 BmpMaxPaletteEntries = 256;

enum BmpCompression : uint32 {
    BiRgb = 0,
    BiRle8 = 1,
    BiRle4 = 2,
    BiBitfields = 3,
    BiJpeg = 4,
    BiPng = 5,
    BiAlphaBitfields = 6
};

// 채널 마스크 하나의 위치와 폭이다.
struct BmpChannel {
    uint32 mask = 0;
    uint32 shift = 0;
    uint32 bits = 0;
};

// 마스크가 연속 비트인지 보고 위치·폭을 구한다(0이면 폭 0).
bool BmpMakeChannel(uint32 mask, BmpChannel& out) noexcept {
    out = BmpChannel{mask, 0, 0};
    if (mask == 0) {
        return true;
    }

    uint32 shift = 0;
    while (((mask >> shift) & 1u) == 0) {
        ++shift;
    }

    uint32 bits = 0;
    while (shift + bits < 32 && ((mask >> (shift + bits)) & 1u) != 0) {
        ++bits;
    }

    // 연속이 아니면(위에 남은 비트가 있으면) 거부한다.
    if (shift + bits < 32 && (mask >> (shift + bits)) != 0) {
        return false;
    }

    out.shift = shift;
    out.bits = bits;
    return true;
}

uint8 BmpExtract(uint32 value, const BmpChannel& channel) noexcept {
    return channel.bits == 0 ? 0
                             : ExpandToByte((value & channel.mask) >> channel.shift, channel.bits);
}

void BmpPutTexel(DecodedImage& image, uint32 x, uint32 y, uint8 r, uint8 g, uint8 b,
                 uint8 a) noexcept {
    uint8* texel = image.pixels.Data() + (static_cast<usize>(y) * image.width + x) * 4;
    texel[0] = r;
    texel[1] = g;
    texel[2] = b;
    texel[3] = a;
}

// 색표 index의 색이다(범위 밖은 불투명 검정 — UE와 같다).
void BmpPutIndex(DecodedImage& image, uint32 x, uint32 y, const uint8* palette, uint32 count,
                 uint32 index) noexcept {
    if (index >= count) {
        BmpPutTexel(image, x, y, 0, 0, 0, 255);
        return;
    }

    const uint8* entry = palette + static_cast<usize>(index) * BmpPaletteEntryBytes;
    BmpPutTexel(image, x, y, entry[2], entry[1], entry[0], 255);
}

// RLE8·RLE4 해제다(아래→위 행). 그리지 않은 texel은 색표 0번이다.
Result<void> BmpDecodeRle(Span<const uint8> file, usize offset, bool four, const uint8* palette,
                          uint32 count, ImageDecodeResult& out) noexcept {
    DecodedImage& image = out.image;
    for (uint32 y = 0; y < image.height; ++y) {
        for (uint32 x = 0; x < image.width; ++x) {
            BmpPutIndex(image, x, y, palette, count, 0);
        }
    }

    uint32 x = 0;
    uint32 row = 0;
    usize at = offset;
    const auto put = [&](uint32 index) noexcept {
        if (x < image.width && row < image.height) {
            BmpPutIndex(image, x, image.height - 1 - row, palette, count, index);
        }

        ++x;
    };
    while (true) {
        if (at + 2 > file.Size()) {
            return Invalid(out, at);
        }

        const uint8 first = file[at];
        const uint8 second = file[at + 1];
        at += 2;
        if (first != 0) {
            // 반복: first개 texel, 4비트는 두 index가 번갈아.
            for (uint32 n = 0; n < first; ++n) {
                const uint32 index = four ? ((n & 1u) == 0 ? second >> 4 : second & 0x0Fu) : second;
                put(index);
            }

            continue;
        }

        if (second == 0) {
            // 행 끝.
            x = 0;
            ++row;
            continue;
        }

        if (second == 1) {
            // 이미지 끝.
            return Result<void>::Success();
        }

        if (second == 2) {
            // delta(오른쪽 dx, 위 dy).
            if (at + 2 > file.Size()) {
                return Invalid(out, at);
            }

            x += file[at];
            row += file[at + 1];
            at += 2;
            continue;
        }

        // 그대로(second개 index — 2 byte 경계로 채운다).
        const uint32 literal = second;
        const usize bytes = four ? (literal + 1) / 2 : literal;
        const usize padded = (bytes + 1) & ~usize{1};
        if (at + padded > file.Size()) {
            return Invalid(out, at);
        }

        for (uint32 n = 0; n < literal; ++n) {
            const uint8 value = file[at + (four ? n / 2 : n)];
            const uint32 index = four ? ((n & 1u) == 0 ? value >> 4 : value & 0x0Fu) : value;
            put(index);
        }

        at += padded;
    }
}

} // namespace

Result<void> DecodeBmp(Allocator&, Span<const uint8> file, const ImageDecodeLimits& limits,
                       ImageDecodeResult& out) noexcept {
    if (file.Size() < BmpFileHeaderBytes + 4 || file[0] != 'B' || file[1] != 'M') {
        return Invalid(out, 0);
    }

    const uint32 pixelOffset = LoadLe32(file.Data() + 10);
    const uint32 headerSize = LoadLe32(file.Data() + BmpFileHeaderBytes);
    if (headerSize == BmpCoreHeaderBytes) {
        return Unsupported(out, BmpFileHeaderBytes);
    }

    if (headerSize != BmpInfoHeaderBytes && headerSize != BmpV2HeaderBytes &&
        headerSize != BmpV3HeaderBytes && headerSize != BmpV4HeaderBytes &&
        headerSize != BmpV5HeaderBytes) {
        return Unsupported(out, BmpFileHeaderBytes);
    }

    if (BmpFileHeaderBytes + headerSize > file.Size()) {
        return Invalid(out, BmpFileHeaderBytes);
    }

    const uint8* header = file.Data() + BmpFileHeaderBytes;
    const int32 width = static_cast<int32>(LoadLe32(header + 4));
    const int32 rawHeight = static_cast<int32>(LoadLe32(header + 8));
    const uint16 planes = LoadLe16(header + 12);
    const uint16 bitCount = LoadLe16(header + 14);
    const uint32 compression = LoadLe32(header + 16);
    const uint32 colorsUsed = LoadLe32(header + 32);
    if (width <= 0 || rawHeight == 0 || rawHeight == NumericLimits<int32>::Min() || planes != 1) {
        return Invalid(out, BmpFileHeaderBytes + 4);
    }

    if (compression == BiJpeg || compression == BiPng) {
        return Unsupported(out, BmpFileHeaderBytes + 16);
    }

    const bool topDown = rawHeight < 0;
    const uint32 height = static_cast<uint32>(topDown ? -rawHeight : rawHeight);
    const bool masked = compression == BiBitfields || compression == BiAlphaBitfields;
    const bool rle = compression == BiRle8 || compression == BiRle4;
    // 비트 깊이와 압축의 조합.
    const bool valid =
        (compression == BiRgb && (bitCount == 1 || bitCount == 4 || bitCount == 8 ||
                                  bitCount == 16 || bitCount == 24 || bitCount == 32)) ||
        (compression == BiRle8 && bitCount == 8) || (compression == BiRle4 && bitCount == 4) ||
        (masked && (bitCount == 16 || bitCount == 32));
    if (!valid) {
        return Unsupported(out, BmpFileHeaderBytes + 14);
    }

    if (rle && topDown) {
        return Invalid(out, BmpFileHeaderBytes + 8);
    }

    // 마스크: 40 byte 헤더는 헤더 뒤(BITFIELDS 3·ALPHABITFIELDS 4), 52 이상은 헤더 안.
    usize afterHeader = BmpFileHeaderBytes + headerSize;
    uint32 masks[4] = {};
    if (masked) {
        const uint32 count = compression == BiAlphaBitfields ? 4u : 3u;
        if (headerSize == BmpInfoHeaderBytes) {
            if (afterHeader + count * 4 > file.Size()) {
                return Invalid(out, afterHeader);
            }

            for (uint32 index = 0; index < count; ++index) {
                masks[index] = LoadLe32(file.Data() + afterHeader + index * 4);
            }

            afterHeader += count * 4;
        } else {
            for (uint32 index = 0; index < 3; ++index) {
                masks[index] = LoadLe32(header + 40 + index * 4);
            }

            if (headerSize >= BmpV3HeaderBytes) {
                masks[3] = LoadLe32(header + 52);
            }
        }
    }

    // 알파 마스크는 V4·V5 헤더만 쓴다(UE와 같다).
    if (headerSize < BmpV4HeaderBytes) {
        masks[3] = 0;
    }

    BmpChannel channels[4];
    if (masked) {
        for (uint32 index = 0; index < 4; ++index) {
            if (!BmpMakeChannel(masks[index], channels[index])) {
                return Invalid(out, BmpFileHeaderBytes + 40);
            }
        }

        // 마스크가 서로 겹치면 거부한다.
        for (uint32 left = 0; left < 4; ++left) {
            for (uint32 right = left + 1; right < 4; ++right) {
                if ((masks[left] & masks[right]) != 0) {
                    return Invalid(out, BmpFileHeaderBytes + 40);
                }
            }
        }
    }

    // 색표.
    const uint8* palette = nullptr;
    uint32 paletteCount = 0;
    if (bitCount <= 8) {
        const uint32 maximum = 1u << bitCount;
        paletteCount = colorsUsed == 0 ? maximum : colorsUsed;
        if (paletteCount > BmpMaxPaletteEntries) {
            return Invalid(out, BmpFileHeaderBytes + 32);
        }

        if (afterHeader + uint64{paletteCount} * BmpPaletteEntryBytes > file.Size()) {
            return Invalid(out, afterHeader);
        }

        palette = file.Data() + afterHeader;
    }

    if (pixelOffset >= file.Size()) {
        return Invalid(out, 10);
    }

    if (auto allocated = AllocateImage(limits, TextureSourceFormat::RGBA8,
                                       static_cast<uint32>(width), height, out);
        !allocated) {
        return allocated;
    }

    if (rle) {
        return BmpDecodeRle(file, pixelOffset, compression == BiRle4, palette, paletteCount, out);
    }

    const uint64 stride = ((uint64{static_cast<uint32>(width)} * bitCount + 31) / 32) * 4;
    if (pixelOffset + stride * height > file.Size()) {
        return Invalid(out, pixelOffset);
    }

    DecodedImage& image = out.image;
    bool anyAlpha = false;
    for (uint32 row = 0; row < height; ++row) {
        const uint8* source = file.Data() + pixelOffset + stride * row;
        const uint32 y = topDown ? row : height - 1 - row;
        for (uint32 x = 0; x < image.width; ++x) {
            switch (bitCount) {
            case 1:
            case 4:
            case 8: {
                const uint32 bitOffset = x * bitCount;
                const uint32 value = (source[bitOffset / 8] >> (8 - bitCount - bitOffset % 8)) &
                                     ((1u << bitCount) - 1u);
                BmpPutIndex(image, x, y, palette, paletteCount, value);
                break;
            }
            case 16: {
                const uint32 value = LoadLe16(source + x * 2);
                if (masked) {
                    BmpPutTexel(image, x, y, BmpExtract(value, channels[0]),
                                BmpExtract(value, channels[1]), BmpExtract(value, channels[2]),
                                channels[3].bits != 0 ? BmpExtract(value, channels[3]) : 255);
                } else {
                    // 555(최상위 비트 무시).
                    BmpPutTexel(image, x, y, ExpandToByte((value >> 10) & 0x1Fu, 5),
                                ExpandToByte((value >> 5) & 0x1Fu, 5),
                                ExpandToByte(value & 0x1Fu, 5), 255);
                }

                break;
            }
            case 24: {
                const uint8* texel = source + x * 3;
                BmpPutTexel(image, x, y, texel[2], texel[1], texel[0], 255);
                break;
            }
            default: {
                const uint8* texel = source + x * 4;
                if (masked) {
                    const uint32 value = LoadLe32(texel);
                    BmpPutTexel(image, x, y, BmpExtract(value, channels[0]),
                                BmpExtract(value, channels[1]), BmpExtract(value, channels[2]),
                                channels[3].bits != 0 ? BmpExtract(value, channels[3]) : 255);
                } else {
                    BmpPutTexel(image, x, y, texel[2], texel[1], texel[0], texel[3]);
                    anyAlpha = anyAlpha || texel[3] != 0;
                }

                break;
            }
            }
        }
    }

    // 32비트 BI_RGB의 알파가 모두 0이면 XRGB로 본다(UE와 같다).
    if (bitCount == 32 && !masked && !anyAlpha) {
        for (usize at = 3; at < image.pixels.Size(); at += 4) {
            image.pixels[at] = 255;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
