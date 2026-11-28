#include "TextureBuildSteps.h"

#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include "TextureImportInternal.h"

#include <TextureTools/TextureBlockEncode.h>
#include <TextureTools/TextureBuild.h>
#include <TextureTools/TextureFormatSelect.h>
#include <TextureTools/TextureImport.h>

#include <math.h>

// 텍스처 빌드의 출력 부호화·채널 복제·알파 판정이다(Docs/Design/AssetSystem 12.2절 3·5번, 12.17.5절
// — 12.18 TP-D6로 TextureBuild.cpp에서 나눔).
namespace rocks::texture_build_detail {

namespace build_encode {

// 16F 상한이다.
constexpr float Half16Max = 65504.0f;

// ---- 6. 출력(12.2절 3·5번) ----

// UE FColor::QuantizeUNormFloatTo8·16이다((uint)(0.5f + Clamp01NansTo0(v) × 최대) — float 식).
float Clamp01NansTo0(float value) noexcept {
    return value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
}

uint8 QuantizeUnorm8(float value) noexcept {
    const float scaled = Clamp01NansTo0(value) * 255.0f;
    return static_cast<uint8>(0.5f + scaled);
}

uint16 QuantizeUnorm16(float value) noexcept {
    const float scaled = Clamp01NansTo0(value) * 65535.0f;
    return static_cast<uint16>(0.5f + scaled);
}

uint16 QuantizeHalf(float value) noexcept {
    if (value != value) {
        return 0;
    }

    const float clamped = value > Half16Max ? Half16Max : (value < -Half16Max ? -Half16Max : value);
    return Float16::FromFloat(clamped).bits;
}

float QuantizeSingle(float value) noexcept {
    if (value != value) {
        return 0.0f;
    }

    const float maximum = NumericLimits<float>::Max();
    return value > maximum ? maximum : (value < -maximum ? -maximum : value);
}

void StoreLe16(uint8* data, uint16 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
}

void StoreLe32(uint8* data, uint32 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
    data[2] = static_cast<uint8>(value >> 16);
    data[3] = static_cast<uint8>(value >> 24);
}

// 8비트 색 채널 하나다(sRGB 형식이면 sRGB 부호화 — 알파는 늘 선형).
uint8 EncodeUnorm8(float value, bool srgb) noexcept {
    return srgb ? texture_import::LinearToSrgb8(value) : QuantizeUnorm8(value);
}

// ASTC 블록 크기는 PixelFormat이 정한다(12.17.8 5). 형식 선택이 다섯 크기의 LDR만 고르므로 그 밖은
// 오지 않는다(오면 Astc6x6로 두지 않고 Unsupported — Encode가 거부한다).
bool AstcBlockFormatOf(graphics::PixelFormat format, TextureBlockFormat& out) noexcept {
    switch (format) {
    case graphics::PixelFormat::ASTC4x4Unorm:
    case graphics::PixelFormat::ASTC4x4UnormSrgb:
        out = TextureBlockFormat::Astc4x4;
        return true;
    case graphics::PixelFormat::ASTC6x6Unorm:
    case graphics::PixelFormat::ASTC6x6UnormSrgb:
        out = TextureBlockFormat::Astc6x6;
        return true;
    case graphics::PixelFormat::ASTC8x8Unorm:
    case graphics::PixelFormat::ASTC8x8UnormSrgb:
        out = TextureBlockFormat::Astc8x8;
        return true;
    case graphics::PixelFormat::ASTC10x10Unorm:
    case graphics::PixelFormat::ASTC10x10UnormSrgb:
        out = TextureBlockFormat::Astc10x10;
        return true;
    case graphics::PixelFormat::ASTC12x12Unorm:
    case graphics::PixelFormat::ASTC12x12UnormSrgb:
        out = TextureBlockFormat::Astc12x12;
        return true;
    default:
        return false;
    }
}

TextureBlockFormat BlockFormatOf(const TextureFormatChoice& choice) noexcept {
    switch (choice.encoding) {
    case TextureFormatEncoding::EacR11:
        return TextureBlockFormat::EacR11;
    case TextureFormatEncoding::Astc: {
        TextureBlockFormat format = TextureBlockFormat::Astc6x6;
        (void)AstcBlockFormatOf(choice.pixelFormat, format);
        return format;
    }
    case TextureFormatEncoding::Bc3:
        return TextureBlockFormat::Bc3;
    case TextureFormatEncoding::Bc4:
        return TextureBlockFormat::Bc4;
    case TextureFormatEncoding::Bc5:
        return TextureBlockFormat::Bc5;
    case TextureFormatEncoding::Bc6h:
        return TextureBlockFormat::Bc6h;
    case TextureFormatEncoding::Bc7:
        return TextureBlockFormat::Bc7;
    default:
        return TextureBlockFormat::Bc1;
    }
}

// level을 choice 형식으로 부호화한다(slice·깊이 평면을 차례로, 행 사이 패딩 없음). 블록 형식은
// RGBA8(같은 sRGB 부호화 — BC6H는 선형 RGBA16F, 가장 가까운 짝수 반올림, EAC R11은 선형 R16)을
// 만들어 EncodeTextureBlocks로 압축한다. ASTC의 BC6H 대체는 선형 RGBA8로 [0, 1]에 자른다(12.17.8
// 6).
Result<void> Encode(Allocator& allocator, const Level& level, const TextureFormatChoice& choice,
                    const EncodeOptions& options, Array<uint8>& out) noexcept {
    const usize texels = level.texels.Size() / Channels;
    const bool srgb = choice.srgb;
    if (choice.encoding == TextureFormatEncoding::Astc) {
        TextureBlockFormat unused = TextureBlockFormat::Astc6x6;
        if (!AstcBlockFormatOf(choice.pixelFormat, unused)) {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    if (choice.encoding != TextureFormatEncoding::Uncompressed) {
        const TextureBlockFormat block = BlockFormatOf(choice);
        const usize texelBytes = TextureBlockSourceTexelBytes(block);
        Array<uint8> rgba(allocator);
        if (auto sized = rgba.TryResize(texels * texelBytes); !sized) {
            return sized;
        }

        for (usize index = 0; index < texels; ++index) {
            const float* texel = level.texels.Data() + index * Channels;
            uint8* target = rgba.Data() + index * texelBytes;
            if (block == TextureBlockFormat::Bc6h) {
                // UE CopyTo(RGBA16F, Linear) 뒤 SanitizeFloat16AndSetAlphaOpaqueForBC6H — 정리는
                // 인코더가 한다.
                for (uint32 c = 0; c < Channels; ++c) {
                    StoreLe16(target + c * 2, QuantizeHalf(texel[c]));
                }

                continue;
            }

            if (block == TextureBlockFormat::EacR11) {
                // UE는 RGBA32F를 넣는다 — 8 bit를 거치지 않게 16 bit로 넘긴다(12.17.8 4).
                StoreLe16(target, QuantizeUnorm16(texel[Red]));
                continue;
            }

            target[0] = EncodeUnorm8(texel[Red], srgb);
            target[1] = EncodeUnorm8(texel[Green], srgb);
            target[2] = EncodeUnorm8(texel[Blue], srgb);
            target[3] = QuantizeUnorm8(texel[Alpha]);
        }

        const uint32 planes = level.layers * level.depth;
        auto bytes = TextureBlockOutputBytes(block, level.width, level.height, planes);
        if (!bytes) {
            return Result<void>::Failure(bytes.Error());
        }

        if (auto sized = out.TryResize(static_cast<usize>(bytes.Value())); !sized) {
            return sized;
        }

        TextureBlockImage image;
        image.rgba = rgba.View();
        image.sizeX = level.width;
        image.sizeY = level.height;
        image.numSlices = planes;
        TextureBlockOptions blockOptions;
        blockOptions.alphaProfile = options.alphaProfile;
        blockOptions.astcMode = options.astcMode;
        blockOptions.srgb = srgb;
        blockOptions.scheduler = options.execution.scheduler;
        blockOptions.scratch = options.execution.scratch;
        return EncodeTextureBlocks(block, image, blockOptions, out.View());
    }

    const graphics::PixelFormat format = choice.pixelFormat;
    const usize texelBytes = graphics::GetPixelFormatInfo(format).blockBytes;
    if (texelBytes == 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (auto sized = out.TryResize(texels * texelBytes); !sized) {
        return sized;
    }

    for (usize index = 0; index < texels; ++index) {
        const float* texel = level.texels.Data() + index * Channels;
        uint8* target = out.Data() + index * texelBytes;
        const float alpha = texel[Alpha];
        switch (format) {
        case graphics::PixelFormat::R8Unorm:
            target[0] = QuantizeUnorm8(texel[Red]);
            break;
        case graphics::PixelFormat::R16Unorm:
            StoreLe16(target, QuantizeUnorm16(texel[Red]));
            break;
        case graphics::PixelFormat::R16Float:
            StoreLe16(target,
                      QuantizeHalf(options.clampUnit ? Clamp01NansTo0(texel[Red]) : texel[Red]));
            break;
        case graphics::PixelFormat::R32Float:
            StoreLe32(target, platform::BitCast<uint32>(QuantizeSingle(texel[Red])));
            break;
        case graphics::PixelFormat::BGRA8Unorm:
        case graphics::PixelFormat::BGRA8UnormSrgb:
            target[0] = EncodeUnorm8(texel[Blue], srgb);
            target[1] = EncodeUnorm8(texel[Green], srgb);
            target[2] = EncodeUnorm8(texel[Red], srgb);
            target[3] = QuantizeUnorm8(alpha);
            break;
        case graphics::PixelFormat::RGBA8Unorm:
        case graphics::PixelFormat::RGBA8UnormSrgb:
            target[0] = EncodeUnorm8(texel[Red], srgb);
            target[1] = EncodeUnorm8(texel[Green], srgb);
            target[2] = EncodeUnorm8(texel[Blue], srgb);
            target[3] = QuantizeUnorm8(alpha);
            break;
        case graphics::PixelFormat::RGBA16Float:
            for (uint32 c = 0; c < Channels; ++c) {
                StoreLe16(target + c * 2, QuantizeHalf(c == Alpha ? alpha : texel[c]));
            }

            break;
        case graphics::PixelFormat::RGBA32Float:
            for (uint32 c = 0; c < Channels; ++c) {
                StoreLe32(target + c * 4,
                          platform::BitCast<uint32>(QuantizeSingle(c == Alpha ? alpha : texel[c])));
            }

            break;
        default:
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    return Result<void>::Success();
}

// 채널 복제(12.17절 TD6 — UE ReplicateRedChannel·ReplicateAlphaChannel, 선형 float).
void Replicate(Level& level, TextureChannelReplicate replicate) noexcept {
    if (replicate == TextureChannelReplicate::None) {
        return;
    }

    const usize source = replicate == TextureChannelReplicate::Red ? Red : Alpha;
    for (usize index = 0; index < level.texels.Size(); index += Channels) {
        const float value = level.texels[index + source];
        for (usize c = 0; c < Channels; ++c) {
            level.texels[index + c] = value;
        }
    }
}

// 알파 판정(UE DetectAlphaChannel의 8비트 문턱 — 최소 알파 ≤ 254.5 / 255면 투명)이다.
bool HasTransparentAlpha(const Level& level) noexcept {
    constexpr float Threshold = 254.5f / 255.0f;
    for (usize index = Alpha; index < level.texels.Size(); index += Channels) {
        if (level.texels[index] <= Threshold) {
            return true;
        }
    }

    return false;
}

} // namespace build_encode

uint8 QuantizeUnorm8Value(float value) noexcept {
    using namespace build_encode;
    return QuantizeUnorm8(value);
}

uint16 QuantizeUnorm16Value(float value) noexcept {
    using namespace build_encode;
    return QuantizeUnorm16(value);
}

} // namespace rocks::texture_build_detail
