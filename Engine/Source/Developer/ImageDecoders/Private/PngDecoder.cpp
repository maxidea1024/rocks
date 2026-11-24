#include "ImageDecodersInternal.h"

#include <Compression/Compression.h>
#include <Core/Encoding/Crc32.h>

// PNG 디코더다(Docs/Design/AssetSystem 15·12.2·12.9절): 색 형식 0·2·3·4·6, 비트 깊이 1–16, Adam7,
// PLTE·tRNS, chunk CRC, zlib(Core Compression — Adler-32 포함), 해제 크기 = IHDR 계산값. 결과
// 형식은 12.2절 표다(회색 1–8 G8, 16 G16, tRNS·알파 GA8·GA16, 팔레트·색 8 RGBA8, 색 16 RGBA16). 색
// 공간은 변환하지 않고 sRGB가 아닌 표시는 NonSrgbSource 경고다.
namespace rocks::image::detail {
namespace {

constexpr usize PngSignatureBytes = 8;
constexpr uint32 PngMaxChunkBytes = static_cast<uint32>(NumericLimits<int32>::Max());
constexpr uint32 PngMaxPaletteEntries = 256;
// gAMA 1/2.2 ≈ 45455, 허용 ±500(12.9절).
constexpr uint32 PngSrgbGamma = 45455;
constexpr uint32 PngGammaTolerance = 500;
// cHRM sRGB 값(백색·R·G·B의 x·y, 100000배)과 허용 1000(0.01).
constexpr uint32 PngSrgbChromaticities[8] = {31270, 32900, 64000, 33000, 30000, 60000, 15000, 6000};
constexpr uint32 PngChromaticityTolerance = 1000;

enum PngColorType : uint32 {
    PngGray = 0,
    PngRgb = 2,
    PngPalette = 3,
    PngGrayAlpha = 4,
    PngRgba = 6
};

constexpr bool PngIsChunk(const uint8* type, const char* name) noexcept {
    return type[0] == static_cast<uint8>(name[0]) && type[1] == static_cast<uint8>(name[1]) &&
           type[2] == static_cast<uint8>(name[2]) && type[3] == static_cast<uint8>(name[3]);
}

uint32 PngChannels(uint32 colorType) noexcept {
    switch (colorType) {
    case PngGray:
    case PngPalette:
        return 1;
    case PngGrayAlpha:
        return 2;
    case PngRgb:
        return 3;
    default:
        return 4;
    }
}

bool PngValidDepth(uint32 colorType, uint32 depth) noexcept {
    switch (colorType) {
    case PngGray:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PngPalette:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case PngRgb:
    case PngGrayAlpha:
    case PngRgba:
        return depth == 8 || depth == 16;
    default:
        return false;
    }
}

// Adam7 패스의 시작·간격(x0, y0, dx, dy)이다.
struct PngPass {
    uint32 x0;
    uint32 y0;
    uint32 dx;
    uint32 dy;
};

constexpr PngPass PngAdam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                                 {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};

uint32 PngPassExtent(uint32 size, uint32 start, uint32 step) noexcept {
    return size <= start ? 0 : (size - start + step - 1) / step;
}

uint8 PngPaeth(uint8 a, uint8 b, uint8 c) noexcept {
    const int32 p = int32{a} + b - c;
    const int32 pa = p > a ? p - a : a - p;
    const int32 pb = p > b ? p - b : b - p;
    const int32 pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) {
        return a;
    }

    return pb <= pc ? b : c;
}

// 한 행의 필터를 푼다(previous는 위 행 — 없으면 0).
bool PngUnfilter(uint8 filter, uint8* row, const uint8* previous, usize bytes, usize bpp) noexcept {
    switch (filter) {
    case 0:
        return true;
    case 1:
        for (usize i = bpp; i < bytes; ++i) {
            row[i] = static_cast<uint8>(row[i] + row[i - bpp]);
        }

        return true;
    case 2:
        for (usize i = 0; i < bytes; ++i) {
            row[i] = static_cast<uint8>(row[i] + (previous ? previous[i] : 0));
        }

        return true;
    case 3:
        for (usize i = 0; i < bytes; ++i) {
            const uint32 left = i >= bpp ? row[i - bpp] : 0;
            const uint32 up = previous ? previous[i] : 0;
            row[i] = static_cast<uint8>(row[i] + ((left + up) >> 1));
        }

        return true;
    case 4:
        for (usize i = 0; i < bytes; ++i) {
            const uint8 left = i >= bpp ? row[i - bpp] : 0;
            const uint8 up = previous ? previous[i] : 0;
            const uint8 corner = (i >= bpp && previous) ? previous[i - bpp] : 0;
            row[i] = static_cast<uint8>(row[i] + PngPaeth(left, up, corner));
        }

        return true;
    default:
        return false;
    }
}

// 행의 n번째 표본(비트 깊이 depth, 16이면 big-endian 2 byte)이다.
uint32 PngSample(const uint8* row, usize index, uint32 depth) noexcept {
    if (depth == 16) {
        return LoadBe16(row + index * 2);
    }

    if (depth == 8) {
        return row[index];
    }

    const usize bit = index * depth;
    return (row[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1u);
}

// 해석한 머리 정보다.
struct PngHeader {
    uint32 width = 0;
    uint32 height = 0;
    uint32 depth = 0;
    uint32 colorType = 0;
    bool interlaced = false;
    // PLTE(RGB 3 byte씩)와 tRNS.
    const uint8* palette = nullptr;
    uint32 paletteCount = 0;
    const uint8* transparency = nullptr;
    uint32 transparencyBytes = 0;
};

TextureSourceFormat PngOutputFormat(const PngHeader& header) noexcept {
    const bool keyed = header.transparency != nullptr;
    switch (header.colorType) {
    case PngGray:
        if (keyed) {
            return header.depth == 16 ? TextureSourceFormat::GA16 : TextureSourceFormat::GA8;
        }

        return header.depth == 16 ? TextureSourceFormat::G16 : TextureSourceFormat::G8;
    case PngGrayAlpha:
        return header.depth == 16 ? TextureSourceFormat::GA16 : TextureSourceFormat::GA8;
    case PngPalette:
        return TextureSourceFormat::RGBA8;
    default:
        return header.depth == 16 ? TextureSourceFormat::RGBA16 : TextureSourceFormat::RGBA8;
    }
}

// 행 하나의 표본을 결과 texel로 옮긴다(x = 결과 열 시작, step = 열 간격).
void PngEmitRow(const PngHeader& header, const uint8* row, uint32 count, uint32 y, uint32 x0,
                uint32 step, DecodedImage& image) noexcept {
    const uint32 channels = PngChannels(header.colorType);
    const uint32 texelBytes = TextureSourceTexelBytes(image.format);
    const bool wide = header.depth == 16;
    const uint32 keyGray = header.transparency != nullptr && header.colorType == PngGray
                               ? LoadBe16(header.transparency)
                               : NumericLimits<uint32>::Max();
    for (uint32 n = 0; n < count; ++n) {
        uint8* texel = image.pixels.Data() +
                       (static_cast<usize>(y) * image.width + x0 + n * step) * texelBytes;
        const usize base = static_cast<usize>(n) * channels;
        switch (header.colorType) {
        case PngGray: {
            const uint32 value = PngSample(row, base, header.depth);
            const bool transparent = header.transparency != nullptr && value == keyGray;
            if (wide) {
                StoreLe16(texel, static_cast<uint16>(value));
                if (header.transparency != nullptr) {
                    StoreLe16(texel + 2, transparent ? 0 : 0xFFFF);
                }
            } else {
                texel[0] = header.depth == 8 ? static_cast<uint8>(value)
                                             : ExpandToByte(value, header.depth);
                if (header.transparency != nullptr) {
                    texel[1] = transparent ? 0 : 255;
                }
            }

            break;
        }
        case PngGrayAlpha:
            if (wide) {
                StoreLe16(texel, static_cast<uint16>(PngSample(row, base, 16)));
                StoreLe16(texel + 2, static_cast<uint16>(PngSample(row, base + 1, 16)));
            } else {
                texel[0] = row[base];
                texel[1] = row[base + 1];
            }

            break;
        case PngPalette: {
            const uint32 index = PngSample(row, base, header.depth);
            if (index < header.paletteCount) {
                const uint8* entry = header.palette + static_cast<usize>(index) * 3;
                texel[0] = entry[0];
                texel[1] = entry[1];
                texel[2] = entry[2];
                texel[3] = index < header.transparencyBytes ? header.transparency[index] : 255;
            } else {
                // 팔레트 밖 index는 디코딩 전에 거부했다(PngPaletteIndicesValid) — 여기 오지
                // 않는다.
                texel[0] = texel[1] = texel[2] = 0;
                texel[3] = 255;
            }

            break;
        }
        default: {
            const bool alpha = header.colorType == PngRgba;
            uint32 rgb[3];
            for (uint32 c = 0; c < 3; ++c) {
                rgb[c] = PngSample(row, base + c, header.depth);
            }

            bool transparent = false;
            if (!alpha && header.transparency != nullptr) {
                transparent = rgb[0] == LoadBe16(header.transparency) &&
                              rgb[1] == LoadBe16(header.transparency + 2) &&
                              rgb[2] == LoadBe16(header.transparency + 4);
            }

            if (wide) {
                for (uint32 c = 0; c < 3; ++c) {
                    StoreLe16(texel + c * 2, static_cast<uint16>(rgb[c]));
                }

                StoreLe16(texel + 6, alpha ? static_cast<uint16>(PngSample(row, base + 3, 16))
                                           : (transparent ? 0 : 0xFFFF));
            } else {
                for (uint32 c = 0; c < 3; ++c) {
                    texel[c] = static_cast<uint8>(rgb[c]);
                }

                texel[3] = alpha ? row[base + 3] : (transparent ? 0 : 255);
            }

            break;
        }
        }
    }
}

// 행의 팔레트 index가 모두 팔레트 안인지 본다(밖이면 InvalidSource — PNG 규격 오류).
bool PngPaletteIndicesValid(const PngHeader& header, const uint8* row, uint32 count) noexcept {
    for (uint32 n = 0; n < count; ++n) {
        if (PngSample(row, n, header.depth) >= header.paletteCount) {
            return false;
        }
    }

    return true;
}

bool PngNonSrgb(bool srgb, bool icc, const uint8* gamma, const uint8* chromaticities) noexcept {
    if (srgb) {
        return false;
    }

    if (icc) {
        return true;
    }

    if (gamma != nullptr) {
        const uint32 value = LoadBe32(gamma);
        const uint32 difference =
            value > PngSrgbGamma ? value - PngSrgbGamma : PngSrgbGamma - value;
        if (difference > PngGammaTolerance) {
            return true;
        }
    }

    if (chromaticities != nullptr) {
        for (uint32 index = 0; index < 8; ++index) {
            const uint32 value = LoadBe32(chromaticities + index * 4);
            const uint32 expected = PngSrgbChromaticities[index];
            const uint32 difference = value > expected ? value - expected : expected - value;
            if (difference > PngChromaticityTolerance) {
                return true;
            }
        }
    }

    return false;
}

} // namespace

Result<void> DecodePng(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    static constexpr uint8 Signature[PngSignatureBytes] = {0x89, 'P',  'N',  'G',
                                                           '\r', '\n', 0x1A, '\n'};
    if (file.Size() < PngSignatureBytes) {
        return Invalid(out, 0);
    }

    for (usize index = 0; index < PngSignatureBytes; ++index) {
        if (file[index] != Signature[index]) {
            return Invalid(out, index);
        }
    }

    PngHeader header;
    bool sawHeader = false;
    bool sawEnd = false;
    bool sawData = false;
    bool srgb = false;
    bool icc = false;
    const uint8* gamma = nullptr;
    const uint8* chromaticities = nullptr;
    Array<uint8> compressed(allocator);
    usize at = PngSignatureBytes;
    while (!sawEnd) {
        if (at + 12 > file.Size()) {
            return Invalid(out, at);
        }

        const uint32 length = LoadBe32(file.Data() + at);
        if (length > PngMaxChunkBytes || uint64{at} + 12 + length > file.Size()) {
            return Invalid(out, at);
        }

        const uint8* type = file.Data() + at + 4;
        const uint8* data = type + 4;
        const uint32 stored = LoadBe32(data + length);
        if (Crc32(Span<const uint8>(type, length + 4)) != stored) {
            return Invalid(out, at);
        }

        if (!sawHeader && !PngIsChunk(type, "IHDR")) {
            return Invalid(out, at);
        }

        if (PngIsChunk(type, "IHDR")) {
            if (sawHeader || length != 13) {
                return Invalid(out, at);
            }

            header.width = LoadBe32(data);
            header.height = LoadBe32(data + 4);
            header.depth = data[8];
            header.colorType = data[9];
            if (header.width == 0 || header.height == 0 || header.width > PngMaxChunkBytes ||
                header.height > PngMaxChunkBytes ||
                !PngValidDepth(header.colorType, header.depth) || data[10] != 0 || data[11] != 0 ||
                data[12] > 1) {
                return Invalid(out, at);
            }

            header.interlaced = data[12] == 1;
            sawHeader = true;
        } else if (PngIsChunk(type, "PLTE")) {
            if (sawData || header.palette != nullptr || length % 3 != 0 || length == 0 ||
                length / 3 > PngMaxPaletteEntries || header.colorType == PngGray ||
                header.colorType == PngGrayAlpha) {
                return Invalid(out, at);
            }

            header.palette = data;
            header.paletteCount = length / 3;
        } else if (PngIsChunk(type, "tRNS")) {
            const bool valid = !sawData && header.transparency == nullptr &&
                               ((header.colorType == PngGray && length == 2) ||
                                (header.colorType == PngRgb && length == 6) ||
                                (header.colorType == PngPalette && header.palette != nullptr &&
                                 length <= header.paletteCount));
            if (!valid) {
                return Invalid(out, at);
            }

            header.transparency = data;
            header.transparencyBytes = length;
        } else if (PngIsChunk(type, "IDAT")) {
            if (header.colorType == PngPalette && header.palette == nullptr) {
                return Invalid(out, at);
            }

            sawData = true;
            if (auto appended = compressed.TryAppend(Span<const uint8>(data, length)); !appended) {
                return Resource(out, appended.Error());
            }
        } else if (PngIsChunk(type, "IEND")) {
            if (!sawData || length != 0) {
                return Invalid(out, at);
            }

            sawEnd = true;
        } else if (PngIsChunk(type, "sRGB")) {
            srgb = true;
        } else if (PngIsChunk(type, "iCCP")) {
            icc = true;
        } else if (PngIsChunk(type, "gAMA")) {
            if (length != 4) {
                return Invalid(out, at);
            }

            gamma = data;
        } else if (PngIsChunk(type, "cHRM")) {
            if (length != 32) {
                return Invalid(out, at);
            }

            chromaticities = data;
        } else if ((type[0] & 0x20u) == 0) {
            // 모르는 필수 chunk(첫 글자 대문자).
            return Unsupported(out, at);
        }

        at += 12 + length;
    }

    const TextureSourceFormat format = PngOutputFormat(header);
    if (auto allocated = AllocateImage(limits, format, header.width, header.height, out);
        !allocated) {
        return allocated;
    }

    // 해제 크기 = 패스마다 행 수 × (필터 byte 1 + 행 byte).
    const uint32 channels = PngChannels(header.colorType);
    const uint64 bitsPerPixel = uint64{channels} * header.depth;
    const usize bpp = static_cast<usize>(bitsPerPixel < 8 ? 1 : bitsPerPixel / 8);
    const uint32 passCount = header.interlaced ? 7 : 1;
    uint64 expected = 0;
    for (uint32 pass = 0; pass < passCount; ++pass) {
        const PngPass shape = header.interlaced ? PngAdam7[pass] : PngPass{0, 0, 1, 1};
        const uint64 columns = PngPassExtent(header.width, shape.x0, shape.dx);
        const uint64 rows = PngPassExtent(header.height, shape.y0, shape.dy);
        if (columns == 0 || rows == 0) {
            continue;
        }

        expected += rows * (1 + (columns * bitsPerPixel + 7) / 8);
    }

    if (expected > NumericLimits<usize>::Max() || expected > limits.maxPixelBytes * 2 + 64) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded, 0);
    }

    Array<uint8> raw(allocator);
    if (auto sized = raw.TryResize(static_cast<usize>(expected)); !sized) {
        return Resource(out, sized.Error());
    }

    auto inflated =
        TryDecompress(CompressionFormat::Zlib, compressed.View(), raw.View(), allocator);
    if (!inflated) {
        return inflated.Error() == ErrorCode::OutOfMemory ? Resource(out, inflated.Error())
                                                          : Invalid(out, PngSignatureBytes);
    }

    if (inflated.Value() != expected) {
        return Invalid(out, PngSignatureBytes);
    }

    usize offset = 0;
    for (uint32 pass = 0; pass < passCount; ++pass) {
        const PngPass shape = header.interlaced ? PngAdam7[pass] : PngPass{0, 0, 1, 1};
        const uint32 columns = PngPassExtent(header.width, shape.x0, shape.dx);
        const uint32 rows = PngPassExtent(header.height, shape.y0, shape.dy);
        if (columns == 0 || rows == 0) {
            continue;
        }

        const usize rowBytes = static_cast<usize>((uint64{columns} * bitsPerPixel + 7) / 8);
        const uint8* previous = nullptr;
        for (uint32 row = 0; row < rows; ++row) {
            uint8* line = raw.Data() + offset;
            if (!PngUnfilter(line[0], line + 1, previous, rowBytes, bpp)) {
                return Invalid(out, PngSignatureBytes);
            }

            if (header.colorType == PngPalette &&
                !PngPaletteIndicesValid(header, line + 1, columns)) {
                return Invalid(out, PngSignatureBytes);
            }

            PngEmitRow(header, line + 1, columns, shape.y0 + row * shape.dy, shape.x0, shape.dx,
                       out.image);
            previous = line + 1;
            offset += rowBytes + 1;
        }
    }

    if (PngNonSrgb(srgb, icc, gamma, chromaticities)) {
        return Warn(out, AssetDiagnosticCode::NonSrgbSource, 0);
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
