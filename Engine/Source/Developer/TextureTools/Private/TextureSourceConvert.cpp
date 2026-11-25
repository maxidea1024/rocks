#include "TextureBuildInternal.h"
#include "TextureImportInternal.h"

#include <Core/Math/Float16.h>
#include <Platform/Memory.h>
#include <TextureTools/TextureImport.h>
#include <TextureTools/TextureSourceConvert.h>

// 텍스처 원본 형식 변환·재표본이다(Docs/Design/AssetSystem 12.14절 A5j2b — UE 5.8
// GetCommonSourceFormat·CopyImage·ResizeImageAllocDest). 풀기·재표본·양자화는 빌드와 같은
// texture_build_detail 단계를 쓴다.
namespace rocks {
namespace {

using texture_build_detail::Alpha;
using texture_build_detail::Blue;
using texture_build_detail::Channels;
using texture_build_detail::Green;
using texture_build_detail::Level;
using texture_build_detail::Red;
using texture_build_detail::TexelGamma;

// 16비트 정수 최댓값과, 8비트 값을 16비트로 넓히는 배수(65535 / 255)다.
constexpr uint16 SourceUnorm16Max = 65535;
constexpr uint16 SourceUnorm8To16 = 257;
constexpr uint8 SourceUnorm8Max = 255;

bool IsFloatSource(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::R16F || format == TextureSourceFormat::RGBA16F ||
           format == TextureSourceFormat::R32F || format == TextureSourceFormat::RGBA32F;
}

bool IsEightBit(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::G8 || format == TextureSourceFormat::GA8 ||
           format == TextureSourceFormat::RGBA8;
}

bool IsSixteenBit(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::G16 || format == TextureSourceFormat::GA16 ||
           format == TextureSourceFormat::RGBA16;
}

bool HasColor(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::RGBA8 || format == TextureSourceFormat::RGBA16;
}

bool HasAlpha(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::GA8 || format == TextureSourceFormat::GA16 ||
           HasColor(format);
}

TexelGamma ToTexel(TextureSourceGamma gamma) noexcept {
    return gamma == TextureSourceGamma::Srgb    ? TexelGamma::Srgb
           : gamma == TextureSourceGamma::Pow22 ? TexelGamma::Pow22
                                                : TexelGamma::Linear;
}

void ConvertStoreLe16(uint8* data, uint16 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
}

uint16 ConvertLoadLe16(const uint8* data) noexcept {
    return static_cast<uint16>(data[0] | (data[1] << 8));
}

void ConvertStoreLe32(uint8* data, uint32 value) noexcept {
    for (uint32 index = 0; index < 4; ++index) {
        data[index] = static_cast<uint8>(value >> (index * 8));
    }
}

// 8비트 색 채널 부호화다(sRGB면 UE ToFColorSRGB 표, 아니면 QuantizeRound — 알파는 늘 선형).
uint8 EncodeColor8(float value, bool srgb) noexcept {
    return srgb ? texture_import::LinearToSrgb8(value)
                : texture_build_detail::QuantizeUnorm8Value(value);
}

// 작업 픽셀을 원본 형식으로 부호화해 out 끝에 붙인다(UE CopyImage RGBA32F → 대상 — 12.14절 A5j2b
// 2번: G 형식은 R만, GA는 R과 A, half는 RTNE·넘치면 ±Inf).
Result<void> EncodeSourceTexels(const Level& level, TextureSourceFormat format, bool srgb,
                                Array<uint8>& out) noexcept {
    const usize texelBytes = TextureSourceTexelBytes(format);
    const usize texels = level.texels.Size() / Channels;
    const usize start = out.Size();
    if (auto sized = out.TryResize(start + texels * texelBytes); !sized) {
        return sized;
    }

    for (usize index = 0; index < texels; ++index) {
        const float* texel = level.texels.Data() + index * Channels;
        uint8* target = out.Data() + start + index * texelBytes;
        switch (format) {
        case TextureSourceFormat::G8:
            target[0] = EncodeColor8(texel[Red], srgb);
            break;
        case TextureSourceFormat::GA8:
            target[0] = EncodeColor8(texel[Red], srgb);
            target[1] = texture_build_detail::QuantizeUnorm8Value(texel[Alpha]);
            break;
        case TextureSourceFormat::RGBA8:
            for (usize channel = 0; channel < Alpha; ++channel) {
                target[channel] = EncodeColor8(texel[channel], srgb);
            }

            target[Alpha] = texture_build_detail::QuantizeUnorm8Value(texel[Alpha]);
            break;
        case TextureSourceFormat::G16:
            ConvertStoreLe16(target, texture_build_detail::QuantizeUnorm16Value(texel[Red]));
            break;
        case TextureSourceFormat::GA16:
            ConvertStoreLe16(target, texture_build_detail::QuantizeUnorm16Value(texel[Red]));
            ConvertStoreLe16(target + 2, texture_build_detail::QuantizeUnorm16Value(texel[Alpha]));
            break;
        case TextureSourceFormat::RGBA16:
            for (usize channel = 0; channel < Channels; ++channel) {
                ConvertStoreLe16(target + channel * 2,
                                 texture_build_detail::QuantizeUnorm16Value(texel[channel]));
            }

            break;
        case TextureSourceFormat::R16F:
            ConvertStoreLe16(target, Float16::FromFloat(texel[Red]).bits);
            break;
        case TextureSourceFormat::RGBA16F:
            for (usize channel = 0; channel < Channels; ++channel) {
                ConvertStoreLe16(target + channel * 2, Float16::FromFloat(texel[channel]).bits);
            }

            break;
        case TextureSourceFormat::R32F:
            ConvertStoreLe32(target, platform::BitCast<uint32>(texel[Red]));
            break;
        case TextureSourceFormat::RGBA32F:
            for (usize channel = 0; channel < Channels; ++channel) {
                ConvertStoreLe32(target + channel * 4, platform::BitCast<uint32>(texel[channel]));
            }

            break;
        case TextureSourceFormat::Invalid:
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

// 같은 감마의 넓히는 변환 직접 경로다(UE CopyImage 같은 감마 직접 경로 — 12.14절 A5j2b 2번): 8 →
// 8비트 byte 복제, 선형 8 → 16비트 v·257, 16 → 16비트 복제. 해당하지 않으면 false(out 그대로).
Result<bool> TryDirect(const TextureSourceImage& image, TextureSourceFormat format,
                       TextureSourceGamma gamma, Array<uint8>& out) noexcept {
    const TextureSourceFormat from = image.format;
    const bool eightToEight = IsEightBit(from) && IsEightBit(format) && image.gamma == gamma;
    const bool eightToSixteen =
        IsEightBit(from) && IsSixteenBit(format) && image.gamma == TextureSourceGamma::Linear;
    const bool sixteenToSixteen = IsSixteenBit(from) && IsSixteenBit(format);
    // 넓히는 변환만이다(색 채널·알파를 잃지 않는다).
    const bool widening =
        (!HasColor(from) || HasColor(format)) && (!HasAlpha(from) || HasAlpha(format));
    if (!(eightToEight || eightToSixteen || sixteenToSixteen) || !widening) {
        return Result<bool>::Success(false);
    }

    const usize fromBytes = TextureSourceTexelBytes(from);
    const usize toBytes = TextureSourceTexelBytes(format);
    const usize texels = image.bytes.Size() / fromBytes;
    const usize start = out.Size();
    if (auto sized = out.TryResize(start + texels * toBytes); !sized) {
        return Result<bool>::Failure(sized.Error());
    }

    const bool wideSource = IsSixteenBit(from);
    for (usize index = 0; index < texels; ++index) {
        const uint8* source = image.bytes.Data() + index * fromBytes;
        // 원본 채널을 16비트 값으로 읽는다(8비트는 넓힐 때만 v·257).
        const auto read = [&](usize channel) noexcept -> uint32 {
            return wideSource ? ConvertLoadLe16(source + channel * 2) : source[channel];
        };
        uint32 color[Channels] = {};
        if (HasColor(from)) {
            for (usize channel = 0; channel < Channels; ++channel) {
                color[channel] = read(channel);
            }
        } else {
            color[Red] = read(0);
            color[Green] = color[Red];
            color[Blue] = color[Red];
            color[Alpha] =
                HasAlpha(from) ? read(1) : (wideSource ? SourceUnorm16Max : SourceUnorm8Max);
        }

        const bool widen = !wideSource && IsSixteenBit(format);
        uint8* target = out.Data() + start + index * toBytes;
        const auto write = [&](usize slot, uint32 value) noexcept {
            if (IsSixteenBit(format)) {
                ConvertStoreLe16(target + slot * 2,
                                 static_cast<uint16>(widen ? value * SourceUnorm8To16 : value));
            } else {
                target[slot] = static_cast<uint8>(value);
            }
        };
        if (HasColor(format)) {
            for (usize channel = 0; channel < Channels; ++channel) {
                write(channel, color[channel]);
            }
        } else {
            write(0, color[Red]);
            if (HasAlpha(format)) {
                write(1, color[Alpha]);
            }
        }
    }

    return Result<bool>::Success(true);
}

} // namespace

TextureSourceFormat CommonTextureSourceFormat(TextureSourceFormat a,
                                              TextureSourceFormat b) noexcept {
    if (a == TextureSourceFormat::Invalid || b == TextureSourceFormat::Invalid) {
        return TextureSourceFormat::Invalid;
    }

    if (a == b) {
        return a;
    }

    // 정수·float가 만나면 RGBA32F다(UE IsHDR 갈래).
    if (IsFloatSource(a) != IsFloatSource(b)) {
        return TextureSourceFormat::RGBA32F;
    }

    if (IsFloatSource(a)) {
        const auto pair = [&](TextureSourceFormat x, TextureSourceFormat y) noexcept {
            return (a == x && b == y) || (a == y && b == x);
        };
        if (pair(TextureSourceFormat::R16F, TextureSourceFormat::RGBA16F)) {
            return TextureSourceFormat::RGBA16F;
        }

        if (pair(TextureSourceFormat::R16F, TextureSourceFormat::R32F)) {
            return TextureSourceFormat::R32F;
        }

        return TextureSourceFormat::RGBA32F;
    }

    // 정수 무리: 가장 낮은 공통 조상 = (16비트 여부, 색 여부, 알파 여부)의 합(12.14절 A5j2b 1번 표
    // — G8 → GA8 → RGBA8 → RGBA16, G8 → G16 → GA16 → RGBA16, GA8 → GA16).
    const bool sixteen = IsSixteenBit(a) || IsSixteenBit(b);
    const bool color = HasColor(a) || HasColor(b);
    const bool alpha = HasAlpha(a) || HasAlpha(b);
    if (color) {
        return sixteen ? TextureSourceFormat::RGBA16 : TextureSourceFormat::RGBA8;
    }

    if (alpha) {
        return sixteen ? TextureSourceFormat::GA16 : TextureSourceFormat::GA8;
    }

    return sixteen ? TextureSourceFormat::G16 : TextureSourceFormat::G8;
}

Result<void> ConvertTextureSource(Allocator& allocator, const TextureSourceImage& image,
                                  TextureSourceFormat format, TextureSourceGamma gamma,
                                  uint32 sizeX, uint32 sizeY, Array<uint8>& out) noexcept {
    const uint64 fromBytes = TextureSourceTexelBytes(image.format);
    const uint64 texels = uint64{image.sizeX} * image.sizeY * image.numSlices;
    const bool validGamma = gamma != TextureSourceGamma::Pow22 &&
                            (gamma == TextureSourceGamma::Linear || IsEightBit(format)) &&
                            (image.gamma == TextureSourceGamma::Linear || IsEightBit(image.format));
    if (fromBytes == 0 || TextureSourceTexelBytes(format) == 0 || image.sizeX == 0 ||
        image.sizeY == 0 || image.numSlices == 0 || sizeX == 0 || sizeY == 0 || !validGamma ||
        image.bytes.Size() != texels * fromBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const uint64 workX = sizeX > image.sizeX ? sizeX : image.sizeX;
    const uint64 workY = sizeY > image.sizeY ? sizeY : image.sizeY;
    if (!texture_build_detail::WithinTextureWorkingBytes(workX, workY, 1, image.numSlices)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const bool resize = sizeX != image.sizeX || sizeY != image.sizeY;
    // 1. 형식·감마(같으면 byte 그대로, 넓히는 같은 감마는 직접 경로, 그 밖은 RGBA32F 선형을
    // 거친다).
    Array<uint8> converted(allocator);
    Array<uint8>& stage = resize ? converted : out;
    if (image.format == format && image.gamma == gamma) {
        if (!resize) {
            return out.TryAppend(image.bytes);
        }

        if (auto copied = converted.TryAppend(image.bytes); !copied) {
            return copied;
        }
    } else {
        auto direct = TryDirect(image, format, gamma, stage);
        if (!direct) {
            return Result<void>::Failure(direct.Error());
        }

        if (!direct.Value()) {
            Level level(allocator);
            if (auto decoded = texture_build_detail::DecodeTexels(
                    image.format, ToTexel(image.gamma), image.sizeX, image.sizeY, 1,
                    image.numSlices, image.bytes, level);
                !decoded) {
                return decoded;
            }

            if (auto encoded =
                    EncodeSourceTexels(level, format, gamma == TextureSourceGamma::Srgb, stage);
                !encoded) {
                return encoded;
            }
        }
    }

    if (!resize) {
        return Result<void>::Success();
    }

    // 2. 재표본(UE ResizeTo — 같은 형식·감마로 풀고, AdaptiveSharp, 같은 형식·감마로 부호화).
    Level level(allocator);
    if (auto decoded =
            texture_build_detail::DecodeTexels(format, ToTexel(gamma), image.sizeX, image.sizeY, 1,
                                               image.numSlices, converted.View(), level);
        !decoded) {
        return decoded;
    }

    if (auto resampled = texture_build_detail::ResampleLevel(allocator, level, sizeX, sizeY);
        !resampled) {
        return resampled;
    }

    return EncodeSourceTexels(level, format, gamma == TextureSourceGamma::Srgb, out);
}

} // namespace rocks
