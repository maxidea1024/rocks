#include "ImageCodecInternal.h"

#include <Core/Math/MathUtility.h>

// TGA 디코더다(Docs/Design/AssetSystem 12.2·12.9절): 형식 1·2·3·9·10·11, 색표 항목 15·16·24·32비트,
// truecolor 15·16·24·32비트, 회색 8비트, RLE(패킷이 행을 넘어도 받는다), 원점 비트, 알파 규칙(알파
// 비트 수 0이면 무시, TGA 2.0 확장 영역 attributes type 0–2 무시·3 일반·4 premultiplied →
// straight). 색은 RGBA8, 회색은 G8이다. 형식 1의 색표는 적용한다(UE는 index를 회색으로 읽는다
// — 12.9절).
namespace rocks::image::detail {
namespace {

constexpr usize TgaHeaderBytes = 18;
constexpr usize TgaFooterBytes = 26;
constexpr usize TgaExtensionAttributesOffset = 494;

enum TgaImageType : uint8 {
    TgaColorMapped = 1,
    TgaTrueColor = 2,
    TgaGray = 3,
    TgaRleColorMapped = 9,
    TgaRleTrueColor = 10,
    TgaRleGray = 11
};

// 15·16·24·32비트 색 하나를 RGBA로 푼다(16비트의 attribute 비트는 alphaBits ≥ 1일 때만 알파).
void TgaUnpackColor(const uint8* data, uint32 depth, uint32 alphaBits, uint8 rgba[4]) noexcept {
    if (depth == 15 || depth == 16) {
        const uint32 value = LoadLe16(data);
        rgba[0] = ExpandToByte((value >> 10) & 0x1Fu, 5);
        rgba[1] = ExpandToByte((value >> 5) & 0x1Fu, 5);
        rgba[2] = ExpandToByte(value & 0x1Fu, 5);
        rgba[3] = (depth == 16 && alphaBits != 0) ? ((value & 0x8000u) != 0 ? 255 : 0) : 255;
        return;
    }

    rgba[0] = data[2];
    rgba[1] = data[1];
    rgba[2] = data[0];
    rgba[3] = (depth == 32 && alphaBits != 0) ? data[3] : 255;
}

} // namespace

Result<void> DecodeTga(Allocator&, Span<const uint8> file, const ImageDecodeLimits& limits,
                       ImageDecodeResult& out) noexcept {
    if (file.Size() < TgaHeaderBytes) {
        return Invalid(out, 0);
    }

    const uint8* header = file.Data();
    const uint32 idLength = header[0];
    const uint32 colorMapType = header[1];
    const uint32 imageType = header[2];
    const uint32 mapFirst = LoadLe16(header + 3);
    const uint32 mapLength = LoadLe16(header + 5);
    const uint32 mapDepth = header[7];
    const uint32 width = LoadLe16(header + 12);
    const uint32 height = LoadLe16(header + 14);
    const uint32 depth = header[16];
    const uint32 descriptor = header[17];
    uint32 alphaBits = descriptor & 0x0Fu;
    const bool rightToLeft = (descriptor & 0x10u) != 0;
    const bool topToBottom = (descriptor & 0x20u) != 0;
    if (imageType == 0) {
        return Invalid(out, 2);
    }

    const bool mapped = imageType == TgaColorMapped || imageType == TgaRleColorMapped;
    const bool gray = imageType == TgaGray || imageType == TgaRleGray;
    const bool trueColor = imageType == TgaTrueColor || imageType == TgaRleTrueColor;
    const bool rle = imageType >= TgaRleColorMapped;
    if (!mapped && !gray && !trueColor) {
        return Unsupported(out, 2);
    }

    if ((mapped && (colorMapType != 1 || (depth != 8 && depth != 16) ||
                    (mapDepth != 15 && mapDepth != 16 && mapDepth != 24 && mapDepth != 32))) ||
        (gray && depth != 8) ||
        (trueColor && depth != 15 && depth != 16 && depth != 24 && depth != 32)) {
        return Unsupported(out, 16);
    }

    // TGA 2.0 확장 영역의 attributes type(있으면 알파 비트 수보다 우선).
    bool premultiplied = false;
    if (file.Size() >= TgaHeaderBytes + TgaFooterBytes) {
        const uint8* footer = file.Data() + file.Size() - TgaFooterBytes;
        bool signature = true;
        const char* text = "TRUEVISION-XFILE.";
        for (usize index = 0; index < 17; ++index) {
            signature = signature && footer[8 + index] == static_cast<uint8>(text[index]);
        }

        const uint32 extension = LoadLe32(footer);
        if (signature && extension != 0) {
            if (uint64{extension} + TgaExtensionAttributesOffset + 1 > file.Size()) {
                return Invalid(out, file.Size() - TgaFooterBytes);
            }

            const uint32 attributes = file[extension + TgaExtensionAttributesOffset];
            if (attributes <= 2) {
                alphaBits = 0;
            } else if (attributes == 4) {
                premultiplied = true;
            }
        }
    }

    // 색표.
    usize at = TgaHeaderBytes + idLength;
    const uint8* palette = nullptr;
    const uint32 entryBytes = (mapDepth + 7) / 8;
    if (colorMapType == 1) {
        const uint64 bytes = uint64{mapLength} * entryBytes;
        if (at + bytes > file.Size()) {
            return Invalid(out, at);
        }

        palette = file.Data() + at;
        at += static_cast<usize>(bytes);
    } else if (colorMapType != 0) {
        return Invalid(out, 1);
    }

    if (auto allocated =
            AllocateImage(limits, gray ? TextureSourceFormat::G8 : TextureSourceFormat::RGBA8,
                          width, height, out);
        !allocated) {
        return allocated;
    }

    DecodedImage& image = out.image;
    const uint32 pixelBytes = (depth + 7) / 8;
    const uint64 total = uint64{width} * height;
    // 순서 번호 i의 texel을 놓는다(원점 비트를 따른다).
    const auto place = [&](uint64 index, const uint8* value) noexcept {
        const uint32 row = static_cast<uint32>(index / width);
        const uint32 column = static_cast<uint32>(index % width);
        const uint32 y = topToBottom ? row : height - 1 - row;
        const uint32 x = rightToLeft ? width - 1 - column : column;
        if (gray) {
            image.pixels[static_cast<usize>(y) * width + x] = value[0];
            return;
        }

        uint8 rgba[4];
        if (mapped) {
            const uint32 raw = depth == 8 ? value[0] : LoadLe16(value);
            if (raw < mapFirst || raw - mapFirst >= mapLength) {
                rgba[0] = rgba[1] = rgba[2] = 0;
                rgba[3] = 255;
            } else {
                TgaUnpackColor(palette + static_cast<usize>(raw - mapFirst) * entryBytes, mapDepth,
                               alphaBits, rgba);
            }
        } else {
            TgaUnpackColor(value, depth, alphaBits, rgba);
        }

        if (premultiplied && alphaBits != 0) {
            const uint32 alpha = rgba[3];
            for (usize channel = 0; channel < 3; ++channel) {
                const uint32 straight =
                    alpha == 0 ? 0u : (rgba[channel] * 255u + alpha / 2) / alpha;
                rgba[channel] = static_cast<uint8>(straight < 255u ? straight : 255u);
            }
        }

        uint8* texel = image.pixels.Data() + (static_cast<usize>(y) * width + x) * 4;
        for (usize channel = 0; channel < 4; ++channel) {
            texel[channel] = rgba[channel];
        }
    };

    if (!rle) {
        if (at + total * pixelBytes > file.Size()) {
            return Invalid(out, at);
        }

        for (uint64 index = 0; index < total; ++index) {
            place(index, file.Data() + at + index * pixelBytes);
        }

        return Result<void>::Success();
    }

    uint64 index = 0;
    while (index < total) {
        if (at >= file.Size()) {
            return Invalid(out, at);
        }

        const uint8 packet = file[at++];
        const uint32 count = (packet & 0x7Fu) + 1;
        if (index + count > total) {
            return Invalid(out, at - 1);
        }

        if ((packet & 0x80u) != 0) {
            if (at + pixelBytes > file.Size()) {
                return Invalid(out, at);
            }

            for (uint32 n = 0; n < count; ++n) {
                place(index++, file.Data() + at);
            }

            at += pixelBytes;
        } else {
            if (at + uint64{count} * pixelBytes > file.Size()) {
                return Invalid(out, at);
            }

            for (uint32 n = 0; n < count; ++n) {
                place(index++, file.Data() + at);
                at += pixelBytes;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
