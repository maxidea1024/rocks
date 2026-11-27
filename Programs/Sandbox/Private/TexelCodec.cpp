#include "TexelCodec.h"

#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <Platform/Memory.h>
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

// 블록 압축 형식의 배치다(D3D BC1·BC3·BC4·BC5).
enum class BlockKind : uint8 {
    Bc1,
    Bc3,
    Bc4,
    Bc5
};

// 블록 한 변 texel 수와 블록 texel 수, 8 byte·16 byte 블록이다.
constexpr uint32 BlockSize = 4;
constexpr uint32 BlockTexels = BlockSize * BlockSize;
constexpr usize SmallBlockBytes = 8;
constexpr usize LargeBlockBytes = 16;
// RGBA8 texel byte 수다.
constexpr usize Rgba8Bytes = 4;
// 565 성분 최대값이다.
constexpr uint32 Max5 = 31;
constexpr uint32 Max6 = 63;
// BC1 2-bit index와 BC4 3-bit index의 비트 수, 값 블록의 값 수다.
constexpr uint32 ColorIndexBits = 2;
constexpr uint32 ValueIndexBits = 3;
constexpr uint32 BlockValues = 8;
// BC4 8값 모드(끝점 0 > 끝점 1)의 보간 분모와 6값 모드의 분모다.
constexpr uint32 EightValueDivisor = 7;
constexpr uint32 SixValueDivisor = 5;

bool BlockKindOf(graphics::PixelFormat format, BlockKind& out) noexcept {
    switch (format) {
    case graphics::PixelFormat::BC1Unorm:
    case graphics::PixelFormat::BC1UnormSrgb:
        out = BlockKind::Bc1;
        return true;
    case graphics::PixelFormat::BC3Unorm:
    case graphics::PixelFormat::BC3UnormSrgb:
        out = BlockKind::Bc3;
        return true;
    case graphics::PixelFormat::BC4Unorm:
        out = BlockKind::Bc4;
        return true;
    case graphics::PixelFormat::BC5Unorm:
        out = BlockKind::Bc5;
        return true;
    default:
        return false;
    }
}

usize BlockBytesOf(BlockKind kind) noexcept {
    return kind == BlockKind::Bc1 || kind == BlockKind::Bc4 ? SmallBlockBytes : LargeBlockBytes;
}

// 565 색 하나를 8 bit 셋으로 편다(비트 복제 — D3D 명세).
void Expand565(uint32 packed, double (&rgb)[3]) noexcept {
    const uint32 r = (packed >> 11) & Max5;
    const uint32 g = (packed >> 5) & Max6;
    const uint32 b = packed & Max5;
    rgb[0] = static_cast<double>((r << 3) | (r >> 2));
    rgb[1] = static_cast<double>((g << 2) | (g >> 4));
    rgb[2] = static_cast<double>((b << 3) | (b >> 2));
}

uint32 Pack565(const uint8* rgba) noexcept {
    const uint32 r = static_cast<uint32>(floor(rgba[0] * Max5 / Unorm8Max + 0.5));
    const uint32 g = static_cast<uint32>(floor(rgba[1] * Max6 / Unorm8Max + 0.5));
    const uint32 b = static_cast<uint32>(floor(rgba[2] * Max5 / Unorm8Max + 0.5));
    return (r << 11) | (g << 5) | b;
}

// BC1·BC3 색 블록의 네 색(8 bit 척도)이다. fourColor가 거짓이면 BC1 3색 모드(가운데 + 투명
// 검정)다.
void ColorPalette(uint32 c0, uint32 c1, bool fourColor, double (&palette)[4][4]) noexcept {
    double a[3];
    double b[3];
    Expand565(c0, a);
    Expand565(c1, b);
    for (uint32 channel = 0; channel < 3; ++channel) {
        palette[0][channel] = a[channel];
        palette[1][channel] = b[channel];
        if (fourColor) {
            palette[2][channel] = (2.0 * a[channel] + b[channel]) / 3.0;
            palette[3][channel] = (a[channel] + 2.0 * b[channel]) / 3.0;
        } else {
            palette[2][channel] = (a[channel] + b[channel]) / 2.0;
            palette[3][channel] = 0.0;
        }
    }

    palette[0][3] = Unorm8Max;
    palette[1][3] = Unorm8Max;
    palette[2][3] = Unorm8Max;
    palette[3][3] = fourColor ? Unorm8Max : 0.0;
}

// BC4 값 블록의 여덟 값(8 bit 척도)이다.
void ValuePalette(uint32 v0, uint32 v1, double (&palette)[BlockValues]) noexcept {
    const double a = static_cast<double>(v0);
    const double b = static_cast<double>(v1);
    palette[0] = a;
    palette[1] = b;
    if (v0 > v1) {
        for (uint32 step = 1; step < EightValueDivisor; ++step) {
            palette[step + 1] = ((EightValueDivisor - step) * a + step * b) /
                                static_cast<double>(EightValueDivisor);
        }

        return;
    }

    for (uint32 step = 1; step < SixValueDivisor; ++step) {
        palette[step + 1] =
            ((SixValueDivisor - step) * a + step * b) / static_cast<double>(SixValueDivisor);
    }

    palette[6] = 0.0;
    palette[7] = Unorm8Max;
}

// 값 블록(8 byte)의 texel 하나다(8 bit 척도).
double DecodeValue(const uint8* block, uint32 texel) noexcept {
    double palette[BlockValues];
    ValuePalette(block[0], block[1], palette);
    const uint64 bits = LoadBits(block + 2, 6);
    return palette[(bits >> (ValueIndexBits * texel)) & (BlockValues - 1)];
}

// 색 블록(8 byte)의 texel 하나다(rgba 8 bit 척도). BC3 색 블록은 끝점 순서와 무관하게 4색이다(D3D
// 명세).
void DecodeColor(const uint8* block, bool bc1, uint32 texel, double (&rgba)[4]) noexcept {
    const uint32 c0 = static_cast<uint32>(LoadBits(block, 2));
    const uint32 c1 = static_cast<uint32>(LoadBits(block + 2, 2));
    double palette[4][4];
    ColorPalette(c0, c1, !bc1 || c0 > c1, palette);
    const uint64 bits = LoadBits(block + 4, 4);
    const uint32 index = static_cast<uint32>((bits >> (ColorIndexBits * texel)) & 3u);
    for (uint32 channel = 0; channel < 4; ++channel) {
        rgba[channel] = palette[index][channel];
    }
}

double SquaredDistance(const double* a, const uint8* b) noexcept {
    double sum = 0.0;
    for (uint32 channel = 0; channel < 3; ++channel) {
        const double delta = a[channel] - static_cast<double>(b[channel]);
        sum += delta * delta;
    }

    return sum;
}

// 16 texel(RGBA8)의 색 블록이다. 끝점은 가장 먼 두 texel이고 4색 모드(c0 > c1)를 지킨다(같으면 모든
// index 0).
void EncodeColor(const uint8 (&texels)[BlockTexels][Rgba8Bytes], uint8* out) noexcept {
    uint32 first = 0;
    uint32 second = 0;
    double farthest = -1.0;
    for (uint32 a = 0; a < BlockTexels; ++a) {
        const double color[3] = {static_cast<double>(texels[a][0]),
                                 static_cast<double>(texels[a][1]),
                                 static_cast<double>(texels[a][2])};
        for (uint32 b = a + 1; b < BlockTexels; ++b) {
            const double distance = SquaredDistance(color, texels[b]);
            if (distance > farthest) {
                farthest = distance;
                first = a;
                second = b;
            }
        }
    }

    uint32 c0 = Pack565(texels[first]);
    uint32 c1 = Pack565(texels[second]);
    if (c0 < c1) {
        const uint32 swap = c0;
        c0 = c1;
        c1 = swap;
    }

    uint64 indices = 0;
    if (c0 != c1) {
        double palette[4][4];
        ColorPalette(c0, c1, true, palette);
        for (uint32 texel = 0; texel < BlockTexels; ++texel) {
            uint32 best = 0;
            double bestDistance = SquaredDistance(palette[0], texels[texel]);
            for (uint32 index = 1; index < 4; ++index) {
                const double distance = SquaredDistance(palette[index], texels[texel]);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }

            indices |= static_cast<uint64>(best) << (ColorIndexBits * texel);
        }
    }

    StoreBits(out, c0, 2);
    StoreBits(out + 2, c1, 2);
    StoreBits(out + 4, indices, 4);
}

// 16 texel의 channel 성분 값 블록이다(끝점 최대·최소 — 8값 모드, 같으면 모든 index 0).
void EncodeValue(const uint8 (&texels)[BlockTexels][Rgba8Bytes], uint32 channel,
                 uint8* out) noexcept {
    uint32 high = 0;
    uint32 low = NumericLimits<uint8>::Max();
    for (uint32 texel = 0; texel < BlockTexels; ++texel) {
        const uint32 value = texels[texel][channel];
        high = value > high ? value : high;
        low = value < low ? value : low;
    }

    uint64 indices = 0;
    if (high != low) {
        double palette[BlockValues];
        ValuePalette(high, low, palette);
        for (uint32 texel = 0; texel < BlockTexels; ++texel) {
            const double value = static_cast<double>(texels[texel][channel]);
            uint32 best = 0;
            double bestDistance = fabs(palette[0] - value);
            for (uint32 index = 1; index < BlockValues; ++index) {
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

bool IsBlockCodecFormat(graphics::PixelFormat format) noexcept {
    BlockKind kind = BlockKind::Bc1;
    return BlockKindOf(format, kind);
}

graphics::PixelFormat BlockStagingFormat(graphics::PixelFormat format) noexcept {
    if (!IsBlockCodecFormat(format)) {
        return format;
    }

    return graphics::GetPixelFormatInfo(format).srgb ? graphics::PixelFormat::RGBA8UnormSrgb
                                                     : graphics::PixelFormat::RGBA8Unorm;
}

bool EncodeBlockImage(graphics::PixelFormat format, Span<const uint8> rgba, uint32 width,
                      uint32 height, uint32 planes, Span<uint8> out) noexcept {
    BlockKind kind = BlockKind::Bc1;
    if (!BlockKindOf(format, kind) || width == 0 || height == 0 || planes == 0 ||
        rgba.Size() != static_cast<usize>(width) * height * planes * Rgba8Bytes ||
        out.Size() != graphics::PixelFormatImageBytes(format, width, height) * planes) {
        return false;
    }

    const usize blockBytes = BlockBytesOf(kind);
    const uint32 blocksX = (width + BlockSize - 1) / BlockSize;
    const uint32 blocksY = (height + BlockSize - 1) / BlockSize;
    uint8* target = out.Data();
    for (uint32 plane = 0; plane < planes; ++plane) {
        for (uint32 blockY = 0; blockY < blocksY; ++blockY) {
            for (uint32 blockX = 0; blockX < blocksX; ++blockX) {
                uint8 texels[BlockTexels][Rgba8Bytes];
                for (uint32 texel = 0; texel < BlockTexels; ++texel) {
                    uint32 x = blockX * BlockSize + texel % BlockSize;
                    uint32 y = blockY * BlockSize + texel / BlockSize;
                    x = x < width ? x : width - 1;
                    y = y < height ? y : height - 1;
                    const usize source =
                        ((static_cast<usize>(plane) * height + y) * width + x) * Rgba8Bytes;
                    platform::MemoryCopy(texels[texel], rgba.Data() + source, Rgba8Bytes);
                }

                switch (kind) {
                case BlockKind::Bc1:
                    EncodeColor(texels, target);
                    break;
                case BlockKind::Bc3:
                    EncodeValue(texels, 3, target);
                    EncodeColor(texels, target + SmallBlockBytes);
                    break;
                case BlockKind::Bc4:
                    EncodeValue(texels, 0, target);
                    break;
                case BlockKind::Bc5:
                    EncodeValue(texels, 0, target);
                    EncodeValue(texels, 1, target + SmallBlockBytes);
                    break;
                }

                target += blockBytes;
            }
        }
    }

    return true;
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

    BlockKind kind = BlockKind::Bc1;
    if (!BlockKindOf(format, kind)) {
        return false;
    }

    const usize blockBytes = BlockBytesOf(kind);
    const usize blocksX = (width + BlockSize - 1) / BlockSize;
    const usize planeBytes =
        static_cast<usize>(graphics::PixelFormatImageBytes(format, width, height));
    const usize offset = static_cast<usize>(plane) * planeBytes +
                         ((y / BlockSize) * blocksX + x / BlockSize) * blockBytes;
    if (offset + blockBytes > bytes.Size()) {
        return false;
    }

    const uint8* block = bytes.Data() + offset;
    const uint32 texel = (y % BlockSize) * BlockSize + x % BlockSize;
    double rgba[4] = {0.0, 0.0, 0.0, Unorm8Max};
    switch (kind) {
    case BlockKind::Bc1:
        DecodeColor(block, true, texel, rgba);
        break;
    case BlockKind::Bc3:
        DecodeColor(block + SmallBlockBytes, false, texel, rgba);
        rgba[3] = DecodeValue(block, texel);
        break;
    case BlockKind::Bc4:
        rgba[0] = DecodeValue(block, texel);
        break;
    case BlockKind::Bc5:
        rgba[0] = DecodeValue(block, texel);
        rgba[1] = DecodeValue(block + SmallBlockBytes, texel);
        break;
    }

    const bool srgb = graphics::GetPixelFormatInfo(format).srgb;
    for (uint32 channel = 0; channel < 4; ++channel) {
        double value = rgba[channel] / Unorm8Max;
        if (srgb && channel < 3) {
            value = SrgbToLinear(value);
        }

        out[channel] = static_cast<float>(value);
    }

    return true;
}

} // namespace sandbox
