#include "TexelCodec.h"

#include <Core/Math/Float16.h>
#include <Platform/Memory.h>
#include <Texture/TextureBlockDecode.h>
#include <math.h>

namespace sandbox {

namespace {

constexpr double Unorm8Max = 255.0;
constexpr double Unorm16Max = 65535.0;
// sRGB 전달 함수의 꺾임과 계수다(IEC 61966-2-1).
constexpr double SrgbLinearCut = 0.0031308;
constexpr double SrgbEncodedCut = 0.04045;
constexpr double SrgbLinearSlope = 12.92;
constexpr double SrgbScale = 1.055;
constexpr double SrgbOffset = 0.055;
constexpr double SrgbGamma = 2.4;

// 형식의 성분 배치다.
struct Layout {
    uint32 channels = 0;
    uint32 componentBytes = 0;
    graphics::PixelComponentType type = graphics::PixelComponentType::None;
    bool srgb = false;
    // 저장 순서가 B·G·R·A다.
    bool bgra = false;
};

uint32 ChannelCount(uint8 mask) noexcept {
    uint32 count = 0;
    for (uint8 bits = mask; bits != 0; bits = static_cast<uint8>(bits >> 1)) {
        count += bits & 1u;
    }

    return count;
}

bool LayoutOf(graphics::PixelFormat format, Layout& out) noexcept {
    const graphics::PixelFormatInfo& info = graphics::GetPixelFormatInfo(format);
    if (info.kind != graphics::PixelFormatKind::Color || info.blockWidth != 1 ||
        info.blockHeight != 1) {
        return false;
    }

    // R·RG·RGBA만 받는다(RGB 3성분·A8·packed는 없다).
    const uint8 rgba = static_cast<uint8>(graphics::PixelChannel::R) |
                       static_cast<uint8>(graphics::PixelChannel::G) |
                       static_cast<uint8>(graphics::PixelChannel::B) |
                       static_cast<uint8>(graphics::PixelChannel::A);
    const uint8 rg = static_cast<uint8>(graphics::PixelChannel::R) |
                     static_cast<uint8>(graphics::PixelChannel::G);
    const uint8 r = static_cast<uint8>(graphics::PixelChannel::R);
    if (info.channels != rgba && info.channels != rg && info.channels != r) {
        return false;
    }

    out.channels = ChannelCount(info.channels);
    if (out.channels == 0 || info.blockBytes % out.channels != 0) {
        return false;
    }

    out.componentBytes = info.blockBytes / out.channels;
    out.type = info.componentType;
    out.srgb = info.srgb;
    out.bgra = format == graphics::PixelFormat::BGRA8Unorm ||
               format == graphics::PixelFormat::BGRA8UnormSrgb;
    const bool size = out.componentBytes == 1 || out.componentBytes == 2 || out.componentBytes == 4;
    const bool type = out.type == graphics::PixelComponentType::Unorm ||
                      out.type == graphics::PixelComponentType::Uint ||
                      out.type == graphics::PixelComponentType::Sint ||
                      (out.type == graphics::PixelComponentType::Float && out.componentBytes != 1);
    return size && type &&
           !(out.type == graphics::PixelComponentType::Unorm && out.componentBytes == 4);
}

// 저장 칸 index의 논리 성분(0 R … 3 A)이다.
uint32 LogicalChannel(const Layout& layout, uint32 slot) noexcept {
    if (layout.bgra && slot < 3) {
        return 2 - slot;
    }

    return slot;
}

double Clamp01(double value) noexcept {
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

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

} // namespace

double LinearToSrgb(double linear) noexcept {
    const double value = Clamp01(linear);
    return value <= SrgbLinearCut ? value * SrgbLinearSlope
                                  : SrgbScale * pow(value, 1.0 / SrgbGamma) - SrgbOffset;
}

double SrgbToLinear(double encoded) noexcept {
    const double value = Clamp01(encoded);
    return value <= SrgbEncodedCut ? value / SrgbLinearSlope
                                   : pow((value + SrgbOffset) / SrgbScale, SrgbGamma);
}

bool IsTexelCodecFormat(graphics::PixelFormat format) noexcept {
    Layout layout;
    return LayoutOf(format, layout);
}

bool EncodeTexel(graphics::PixelFormat format, const float (&value)[4], uint8* out) noexcept {
    Layout layout;
    if (!LayoutOf(format, layout)) {
        return false;
    }

    for (uint32 slot = 0; slot < layout.channels; ++slot) {
        const uint32 channel = LogicalChannel(layout, slot);
        double component = value[channel];
        uint8* target = out + slot * layout.componentBytes;
        switch (layout.type) {
        case graphics::PixelComponentType::Unorm: {
            if (layout.srgb && channel < 3) {
                component = LinearToSrgb(component);
            }

            const double maximum = layout.componentBytes == 1 ? Unorm8Max : Unorm16Max;
            StoreBits(target, static_cast<uint64>(floor(Clamp01(component) * maximum + 0.5)),
                      layout.componentBytes);
            break;
        }

        case graphics::PixelComponentType::Uint:
            StoreBits(target, static_cast<uint64>(component < 0.0 ? 0.0 : component),
                      layout.componentBytes);
            break;
        case graphics::PixelComponentType::Sint:
            StoreBits(target, static_cast<uint64>(static_cast<int64>(component)),
                      layout.componentBytes);
            break;
        case graphics::PixelComponentType::Float:
            if (layout.componentBytes == 2) {
                StoreBits(target, Float16::FromFloat(static_cast<float>(component)).bits, 2);
            } else {
                const float single = static_cast<float>(component);
                platform::MemoryCopy(target, &single, sizeof(single));
            }

            break;
        default:
            return false;
        }
    }

    return true;
}

bool DecodeTexel(graphics::PixelFormat format, const uint8* in, float (&out)[4]) noexcept {
    Layout layout;
    if (!LayoutOf(format, layout)) {
        return false;
    }

    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    for (uint32 slot = 0; slot < layout.channels; ++slot) {
        const uint32 channel = LogicalChannel(layout, slot);
        const uint8* source = in + slot * layout.componentBytes;
        const uint64 bits = LoadBits(source, layout.componentBytes);
        double component = 0.0;
        switch (layout.type) {
        case graphics::PixelComponentType::Unorm: {
            const double maximum = layout.componentBytes == 1 ? Unorm8Max : Unorm16Max;
            component = static_cast<double>(bits) / maximum;
            if (layout.srgb && channel < 3) {
                component = SrgbToLinear(component);
            }

            break;
        }

        case graphics::PixelComponentType::Uint:
            component = static_cast<double>(bits);
            break;
        case graphics::PixelComponentType::Sint: {
            const uint32 shift = 64 - 8 * layout.componentBytes;
            component = static_cast<double>(static_cast<int64>(bits << shift) >> shift);
            break;
        }

        case graphics::PixelComponentType::Float:
            if (layout.componentBytes == 2) {
                component = Float16::FromBits(static_cast<uint16>(bits)).ToFloat();
            } else {
                float single = 0.0f;
                platform::MemoryCopy(&single, source, sizeof(single));
                component = single;
            }

            break;
        default:
            return false;
        }

        out[channel] = static_cast<float>(component);
    }

    return true;
}

bool TextureBlockFormatOf(graphics::PixelFormat format, TextureBlockFormat& out) noexcept {
    switch (format) {
    case graphics::PixelFormat::BC1Unorm:
    case graphics::PixelFormat::BC1UnormSrgb:
        out = TextureBlockFormat::Bc1;
        return true;
    case graphics::PixelFormat::BC3Unorm:
    case graphics::PixelFormat::BC3UnormSrgb:
        out = TextureBlockFormat::Bc3;
        return true;
    case graphics::PixelFormat::BC4Unorm:
        out = TextureBlockFormat::Bc4;
        return true;
    case graphics::PixelFormat::BC5Unorm:
        out = TextureBlockFormat::Bc5;
        return true;
    case graphics::PixelFormat::BC6HUfloat:
        out = TextureBlockFormat::Bc6h;
        return true;
    case graphics::PixelFormat::BC7Unorm:
    case graphics::PixelFormat::BC7UnormSrgb:
        out = TextureBlockFormat::Bc7;
        return true;
    default:
        return false;
    }
}

bool DecodeImageTexel(graphics::PixelFormat format, Span<const uint8> bytes, uint32 width,
                      uint32 height, uint32 plane, uint32 x, uint32 y, float (&out)[4]) noexcept {
    if (x >= width || y >= height) {
        return false;
    }

    if (IsTexelCodecFormat(format)) {
        const usize texelBytes = graphics::GetPixelFormatInfo(format).blockBytes;
        const usize index = (static_cast<usize>(plane) * height + y) * width + x;
        if ((index + 1) * texelBytes > bytes.Size()) {
            return false;
        }

        return DecodeTexel(format, bytes.Data() + index * texelBytes, out);
    }

    TextureBlockFormat block = TextureBlockFormat::Bc1;
    if (!TextureBlockFormatOf(format, block)) {
        return false;
    }

    const usize blockBytes = TextureBlockBytes(block);
    const usize blocksX = (width + TextureBlockSize - 1) / TextureBlockSize;
    const usize planeBytes =
        static_cast<usize>(graphics::PixelFormatImageBytes(format, width, height));
    const usize offset = static_cast<usize>(plane) * planeBytes +
                         ((y / TextureBlockSize) * blocksX + x / TextureBlockSize) * blockBytes;
    if (offset + blockBytes > bytes.Size()) {
        return false;
    }

    const uint8* source = bytes.Data() + offset;
    const uint32 texel = (y % TextureBlockSize) * TextureBlockSize + x % TextureBlockSize;
    if (block == TextureBlockFormat::Bc6h) {
        // BC6H Ufloat는 half로 풀린다(알파 1 — 형식에 알파가 없다).
        uint16 halfs[TextureBlockTexels * 4];
        DecodeTextureBlockHalf(source, halfs);
        for (uint32 channel = 0; channel < 4; ++channel) {
            out[channel] = Float16::FromBits(halfs[texel * 4 + channel]).ToFloat();
        }

        return true;
    }

    uint8 texels[TextureBlockRgbaBytes];
    DecodeTextureBlock(block, source, texels);
    const bool srgb = graphics::GetPixelFormatInfo(format).srgb;
    for (uint32 channel = 0; channel < 4; ++channel) {
        double value = texels[texel * TextureBlockTexelBytes + channel] / Unorm8Max;
        if (srgb && channel < 3) {
            value = SrgbToLinear(value);
        }

        out[channel] = static_cast<float>(value);
    }

    return true;
}

} // namespace sandbox
