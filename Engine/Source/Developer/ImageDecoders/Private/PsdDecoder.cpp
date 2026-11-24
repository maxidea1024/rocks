#include "ImageDecodersInternal.h"

#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>

// PSD 디코더다(Docs/Design/AssetSystem 12.2·12.9절): 버전 1(PSD)·2(PSB), 색 모드 Bitmap·Grayscale·
// Indexed·RGB, 깊이 1·8·16·32, 병합 이미지 압축 raw·RLE(PackBits). 알파는 UE와 같다(RGB 넷째·
// Grayscale 둘째 채널), 그보다 많은 채널은 IgnoredSourceChannels 경고와 함께 버린다. float는 NaN →
// 0, ±Inf → ±최대 유한값.
namespace rocks::image::detail {
namespace {

constexpr usize PsdHeaderBytes = 26;
constexpr uint32 PsdMaxChannels = 56;
constexpr uint32 PsdIndexedPaletteBytes = 768;
constexpr uint32 PsdIndexedEntries = 256;
constexpr uint16 PsdResourceVersionInfo = 0x0421;
constexpr uint16 PsdResourceIndexedCount = 0x0416;
constexpr uint16 PsdResourceTransparentIndex = 0x0417;

enum PsdColorMode : uint16 {
    PsdBitmap = 0,
    PsdGrayscale = 1,
    PsdIndexed = 2,
    PsdRgb = 3
};

// 크기 칸 하나를 읽는다(PSB는 8 byte, PSD는 4 byte — 큰 것만 PSB에서 넓어진다).
bool PsdReadLength(Span<const uint8> file, usize& at, bool wide, uint64& out) noexcept {
    const usize bytes = wide ? 8 : 4;
    if (at + bytes > file.Size()) {
        return false;
    }

    out = wide ? LoadBe64(file.Data() + at) : LoadBe32(file.Data() + at);
    at += bytes;
    return true;
}

// PackBits 한 행을 풀어 dst(정확히 rowBytes)에 쓴다. 실패면 false다.
bool PsdUnpackBits(const uint8* source, usize sourceBytes, uint8* dst, usize rowBytes) noexcept {
    usize in = 0;
    usize written = 0;
    while (written < rowBytes) {
        if (in >= sourceBytes) {
            return false;
        }

        const int8 control = static_cast<int8>(source[in++]);
        if (control >= 0) {
            const usize count = static_cast<usize>(control) + 1;
            if (in + count > sourceBytes || written + count > rowBytes) {
                return false;
            }

            for (usize n = 0; n < count; ++n) {
                dst[written++] = source[in++];
            }
        } else if (control != -128) {
            const usize count = static_cast<usize>(1 - control);
            if (in >= sourceBytes || written + count > rowBytes) {
                return false;
            }

            const uint8 value = source[in++];
            for (usize n = 0; n < count; ++n) {
                dst[written++] = value;
            }
        }
    }

    return true;
}

float PsdSanitize(float value) noexcept {
    if (math::IsNaN(value)) {
        return 0.0f;
    }

    if (!math::IsFinite(value)) {
        return value > 0 ? NumericLimits<float>::Max() : -NumericLimits<float>::Max();
    }

    return value;
}

} // namespace

Result<void> DecodePsd(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    if (file.Size() < PsdHeaderBytes || file[0] != '8' || file[1] != 'B' || file[2] != 'P' ||
        file[3] != 'S') {
        return Invalid(out, 0);
    }

    const uint16 version = LoadBe16(file.Data() + 4);
    if (version != 1 && version != 2) {
        return Unsupported(out, 4);
    }

    const bool wide = version == 2;
    const uint32 channels = LoadBe16(file.Data() + 12);
    const uint32 height = LoadBe32(file.Data() + 14);
    const uint32 width = LoadBe32(file.Data() + 18);
    const uint32 depth = LoadBe16(file.Data() + 22);
    const uint32 mode = LoadBe16(file.Data() + 24);
    if (channels == 0 || channels > PsdMaxChannels || width == 0 || height == 0) {
        return Invalid(out, 12);
    }

    if (mode != PsdBitmap && mode != PsdGrayscale && mode != PsdIndexed && mode != PsdRgb) {
        return Unsupported(out, 24);
    }

    const bool depthValid =
        (mode == PsdBitmap && depth == 1) || (mode == PsdIndexed && depth == 8) ||
        ((mode == PsdGrayscale || mode == PsdRgb) && (depth == 8 || depth == 16 || depth == 32));
    if (!depthValid) {
        return Unsupported(out, 22);
    }

    const uint32 colorChannels = mode == PsdRgb ? 3 : 1;
    if (channels < colorChannels) {
        return Invalid(out, 12);
    }

    // 색 모드 데이터(Indexed는 768 byte 평면 팔레트 R·G·B).
    usize at = PsdHeaderBytes;
    if (at + 4 > file.Size()) {
        return Invalid(out, at);
    }

    const uint32 colorBytes = LoadBe32(file.Data() + at);
    at += 4;
    if (uint64{at} + colorBytes > file.Size()) {
        return Invalid(out, at);
    }

    const uint8* palette = nullptr;
    if (mode == PsdIndexed) {
        if (colorBytes < PsdIndexedPaletteBytes) {
            return Invalid(out, at);
        }

        palette = file.Data() + at;
    }

    at += colorBytes;
    // image resource: 병합 표시·Indexed 색 수·투명 index.
    if (at + 4 > file.Size()) {
        return Invalid(out, at);
    }

    const uint32 resourceBytes = LoadBe32(file.Data() + at);
    at += 4;
    if (uint64{at} + resourceBytes > file.Size()) {
        return Invalid(out, at);
    }

    uint32 indexedCount = PsdIndexedEntries;
    uint32 transparentIndex = NumericLimits<uint32>::Max();
    {
        usize cursor = at;
        const usize end = at + resourceBytes;
        while (cursor + 12 <= end) {
            if (file[cursor] != '8' || file[cursor + 1] != 'B' || file[cursor + 2] != 'I' ||
                file[cursor + 3] != 'M') {
                return Invalid(out, cursor);
            }

            const uint16 id = LoadBe16(file.Data() + cursor + 4);
            // Pascal 이름(길이 byte 포함 짝수로 채움).
            const usize nameBytes = (usize{file[cursor + 6]} + 2) & ~usize{1};
            const usize sizeAt = cursor + 6 + nameBytes;
            if (sizeAt + 4 > end) {
                return Invalid(out, cursor);
            }

            const uint32 size = LoadBe32(file.Data() + sizeAt);
            const usize dataAt = sizeAt + 4;
            if (uint64{dataAt} + size > end) {
                return Invalid(out, sizeAt);
            }

            const uint8* data = file.Data() + dataAt;
            if (id == PsdResourceVersionInfo && size >= 5 && data[4] == 0) {
                // hasRealMergedData false — 병합 이미지가 비어 있다(레이어 합성은 하지 않는다).
                return Unsupported(out, dataAt);
            }

            if (id == PsdResourceIndexedCount && size >= 2) {
                indexedCount = math::Min<uint32>(LoadBe16(data), PsdIndexedEntries);
            }

            if (id == PsdResourceTransparentIndex && size >= 2) {
                transparentIndex = LoadBe16(data);
            }

            cursor = dataAt + ((size + 1) & ~uint32{1});
        }
    }

    at += resourceBytes;
    // 레이어·마스크 정보는 건너뛴다.
    uint64 layerBytes = 0;
    if (!PsdReadLength(file, at, wide, layerBytes) || uint64{at} + layerBytes > file.Size()) {
        return Invalid(out, at);
    }

    at += static_cast<usize>(layerBytes);
    if (at + 2 > file.Size()) {
        return Invalid(out, at);
    }

    const uint16 compression = LoadBe16(file.Data() + at);
    at += 2;
    if (compression == 2 || compression == 3) {
        return Unsupported(out, at - 2);
    }

    if (compression > 3) {
        return Invalid(out, at - 2);
    }

    // 결과 형식과 쓰는 채널 수.
    const bool hasAlpha =
        (mode == PsdRgb && channels >= 4) || (mode == PsdGrayscale && channels >= 2);
    const uint32 used = colorChannels + (hasAlpha ? 1 : 0);
    TextureSourceFormat format = TextureSourceFormat::G8;
    if (mode == PsdBitmap) {
        format = TextureSourceFormat::G8;
    } else if (mode == PsdIndexed) {
        format = TextureSourceFormat::RGBA8;
    } else if (mode == PsdGrayscale) {
        format = depth == 8 ? (hasAlpha ? TextureSourceFormat::GA8 : TextureSourceFormat::G8)
                 : depth == 16
                     ? (hasAlpha ? TextureSourceFormat::GA16 : TextureSourceFormat::G16)
                     : (hasAlpha ? TextureSourceFormat::RGBA32F : TextureSourceFormat::R32F);
    } else {
        format = depth == 8    ? TextureSourceFormat::RGBA8
                 : depth == 16 ? TextureSourceFormat::RGBA16
                               : TextureSourceFormat::RGBA32F;
    }

    if (auto allocated = AllocateImage(limits, format, width, height, out); !allocated) {
        return allocated;
    }

    // 쓰는 채널의 평면을 푼다.
    const usize rowBytes = static_cast<usize>((uint64{width} * depth + 7) / 8);
    const uint64 planeBytes = uint64{rowBytes} * height;
    Array<uint8> planes(allocator);
    if (auto sized = planes.TryResize(static_cast<usize>(planeBytes * used)); !sized) {
        return Resource(out, sized.Error());
    }

    if (compression == 0) {
        if (uint64{at} + planeBytes * used > file.Size()) {
            return Invalid(out, at);
        }

        for (usize index = 0; index < planes.Size(); ++index) {
            planes[index] = file[at + index];
        }
    } else {
        // RLE: 모든 채널·행의 byte 수 표(PSD 2 byte·PSB 4 byte) 뒤 행들.
        const usize countBytes = wide ? 4 : 2;
        const uint64 tableBytes = uint64{channels} * height * countBytes;
        if (uint64{at} + tableBytes > file.Size()) {
            return Invalid(out, at);
        }

        const usize table = at;
        usize data = at + static_cast<usize>(tableBytes);
        for (uint32 channel = 0; channel < channels; ++channel) {
            for (uint32 row = 0; row < height; ++row) {
                const usize entry =
                    table + (static_cast<usize>(channel) * height + row) * countBytes;
                const uint32 packed =
                    wide ? LoadBe32(file.Data() + entry) : LoadBe16(file.Data() + entry);
                if (uint64{data} + packed > file.Size()) {
                    return Invalid(out, entry);
                }

                if (channel < used &&
                    !PsdUnpackBits(file.Data() + data, packed,
                                   planes.Data() + static_cast<usize>(channel) * planeBytes +
                                       static_cast<usize>(row) * rowBytes,
                                   rowBytes)) {
                    return Invalid(out, data);
                }

                data += packed;
            }
        }
    }

    // 평면을 결과 texel로 엮는다.
    DecodedImage& image = out.image;
    const uint32 texelBytes = TextureSourceTexelBytes(format);
    const auto plane = [&](uint32 channel, uint32 x, uint32 y) noexcept {
        return planes.Data() + static_cast<usize>(channel) * planeBytes +
               static_cast<usize>(y) * rowBytes + static_cast<usize>(x) * (depth / 8);
    };
    for (uint32 y = 0; y < height; ++y) {
        for (uint32 x = 0; x < width; ++x) {
            uint8* texel = image.pixels.Data() + (static_cast<usize>(y) * width + x) * texelBytes;
            if (mode == PsdBitmap) {
                const uint8 bits = planes[static_cast<usize>(y) * rowBytes + x / 8];
                texel[0] = ((bits >> (7 - x % 8)) & 1u) != 0 ? 0 : 255;
                continue;
            }

            if (mode == PsdIndexed) {
                const uint32 index = *plane(0, x, y);
                if (index < indexedCount) {
                    texel[0] = palette[index];
                    texel[1] = palette[PsdIndexedEntries + index];
                    texel[2] = palette[PsdIndexedEntries * 2 + index];
                    texel[3] = index == transparentIndex ? 0 : 255;
                } else {
                    texel[0] = texel[1] = texel[2] = 0;
                    texel[3] = 255;
                }

                continue;
            }

            if (depth == 8) {
                for (uint32 c = 0; c < used; ++c) {
                    texel[c] = *plane(c, x, y);
                }

                if (mode == PsdRgb && !hasAlpha) {
                    texel[3] = 255;
                }

                continue;
            }

            if (depth == 16) {
                for (uint32 c = 0; c < used; ++c) {
                    StoreLe16(texel + c * 2, LoadBe16(plane(c, x, y)));
                }

                if (mode == PsdRgb && !hasAlpha) {
                    StoreLe16(texel + 6, 0xFFFF);
                }

                continue;
            }

            // 32비트 float(big-endian) — 회색+알파는 RGBA32F(r = g = b).
            float values[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (uint32 c = 0; c < used; ++c) {
                values[c] = PsdSanitize(platform::BitCast<float>(LoadBe32(plane(c, x, y))));
            }

            if (mode == PsdGrayscale && hasAlpha) {
                const float alpha = values[1];
                values[1] = values[0];
                values[2] = values[0];
                values[3] = alpha;
            }

            const uint32 written = format == TextureSourceFormat::R32F ? 1 : 4;
            for (uint32 c = 0; c < written; ++c) {
                const uint32 bits = platform::BitCast<uint32>(values[c]);
                texel[c * 4 + 0] = static_cast<uint8>(bits);
                texel[c * 4 + 1] = static_cast<uint8>(bits >> 8);
                texel[c * 4 + 2] = static_cast<uint8>(bits >> 16);
                texel[c * 4 + 3] = static_cast<uint8>(bits >> 24);
            }
        }
    }

    if (channels > used) {
        return Warn(out, AssetDiagnosticCode::IgnoredSourceChannels, channels - used);
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
