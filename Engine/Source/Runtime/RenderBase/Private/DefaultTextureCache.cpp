#include <Core/Math/Color.h>
#include <Core/Math/Float16.h>
#include <Core/Math/MathUtility.h>
#include <Platform/Memory.h>
#include <RenderBase/DefaultTextureCache.h>

// 기본 텍스처 캐시다(Docs/Design/RenderBase 11.1절 RR17, 엔진 셰이더 14.5절 4항).
namespace rocks::render {

namespace {

using graphics::PixelFormat;
using graphics::TextureDimension;

// cube 면 수와, 기본 텍스처 하나가 가질 수 있는 slice 수 상한(cube 배열 cube 1 = 면 6)이다.
constexpr uint32 CubeFaces = 6;
constexpr uint32 MaxDefaultSlices = CubeFaces;

// 부호화 값의 종류다.
enum class TexelEncoding : uint8 {
    Unorm8,
    Snorm8,
    Srgb8,
    Uint8,
    Unorm16,
    Uint16,
    Half,
    Float32,
    Uint32,
    Sint32
};

// 부호화 표 한 행이다(성분 수, 종류, B·R을 바꾸는지).
struct TexelLayout {
    PixelFormat format;
    uint32 components;
    TexelEncoding encoding;
    bool swapRedBlue;
};

constexpr TexelLayout TexelLayouts[] = {
    {PixelFormat::R8Unorm, 1, TexelEncoding::Unorm8, false},
    {PixelFormat::RG8Unorm, 2, TexelEncoding::Unorm8, false},
    {PixelFormat::RGBA8Unorm, 4, TexelEncoding::Unorm8, false},
    {PixelFormat::RGBA8Snorm, 4, TexelEncoding::Snorm8, false},
    {PixelFormat::RGBA8UnormSrgb, 4, TexelEncoding::Srgb8, false},
    {PixelFormat::BGRA8Unorm, 4, TexelEncoding::Unorm8, true},
    {PixelFormat::BGRA8UnormSrgb, 4, TexelEncoding::Srgb8, true},
    {PixelFormat::R8Uint, 1, TexelEncoding::Uint8, false},
    {PixelFormat::RGBA8Uint, 4, TexelEncoding::Uint8, false},
    {PixelFormat::R16Float, 1, TexelEncoding::Half, false},
    {PixelFormat::RG16Float, 2, TexelEncoding::Half, false},
    {PixelFormat::RGBA16Float, 4, TexelEncoding::Half, false},
    {PixelFormat::R16Unorm, 1, TexelEncoding::Unorm16, false},
    {PixelFormat::RG16Unorm, 2, TexelEncoding::Unorm16, false},
    {PixelFormat::RGBA16Unorm, 4, TexelEncoding::Unorm16, false},
    {PixelFormat::R16Uint, 1, TexelEncoding::Uint16, false},
    {PixelFormat::RGBA16Uint, 4, TexelEncoding::Uint16, false},
    {PixelFormat::R32Float, 1, TexelEncoding::Float32, false},
    {PixelFormat::RG32Float, 2, TexelEncoding::Float32, false},
    {PixelFormat::RGBA32Float, 4, TexelEncoding::Float32, false},
    {PixelFormat::R32Uint, 1, TexelEncoding::Uint32, false},
    {PixelFormat::R32Sint, 1, TexelEncoding::Sint32, false},
    {PixelFormat::RG32Uint, 2, TexelEncoding::Uint32, false},
    {PixelFormat::RGBA32Uint, 4, TexelEncoding::Uint32, false}};

// 8·16 bit 정규화 정수의 최댓값과 snorm 8 bit 배율이다.
constexpr float Unorm8Scale = 255.0f;
constexpr float Snorm8Scale = 127.0f;
constexpr float Unorm16Scale = 65535.0f;
constexpr float Uint8Max = 255.0f;
constexpr float Uint16Max = 65535.0f;
// float로 정확한 uint32·int32 범위 끝(2^32 − 256 — 그 위 float는 2^32 이상이다)과 int32 범위다.
constexpr float Uint32MaxFloat = 4294967040.0f;
constexpr float Int32MinFloat = -2147483648.0f;
constexpr float Int32MaxFloat = 2147483520.0f;

const TexelLayout* FindLayout(PixelFormat format) noexcept {
    for (const TexelLayout& layout : TexelLayouts) {
        if (layout.format == format) {
            return &layout;
        }
    }

    return nullptr;
}

// x를 [low, high]로 자른다(NaN은 0).
float ClampOrZero(float x, float low, float high) noexcept {
    return math::IsNaN(x) ? 0.0f : (x < low ? low : (x > high ? high : x));
}

// 정규화 값의 가장 가까운 정수(0.5는 0에서 먼 쪽)다.
int32 RoundToInt(float x) noexcept {
    return static_cast<int32>(x >= 0.0f ? x + 0.5f : x - 0.5f);
}

void StoreBytes(uint8* out, usize offset, const void* value, usize bytes) noexcept {
    platform::MemoryCopy(out + offset, value, bytes);
}

bool SameKey(const uint32 (&a)[4], const uint32 (&b)[4]) noexcept {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

} // namespace

Result<usize> EncodeDefaultTexel(PixelFormat format, const graphics::ClearColor& value,
                                 Span<uint8> out) noexcept {
    if (out.Size() < MaxDefaultTexelBytes) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const TexelLayout* layout = FindLayout(format);
    if (layout == nullptr) {
        return Result<usize>::Failure(ErrorCode::Unsupported);
    }

    const float source[4] = {value.r, value.g, value.b, value.a};
    float channels[4] = {source[0], source[1], source[2], source[3]};
    if (layout->swapRedBlue) {
        channels[0] = source[2];
        channels[2] = source[0];
    }

    uint8* bytes = out.Data();
    usize size = 0;
    if (layout->encoding == TexelEncoding::Srgb8) {
        const LinearColor linear(channels[0], channels[1], channels[2], channels[3]);
        const Color srgb = linear.ToSRGB();
        const Color quantized = linear.QuantizeRound();
        const uint8 packed[4] = {srgb.r, srgb.g, srgb.b, quantized.a};
        StoreBytes(bytes, 0, packed, sizeof(packed));
        return Result<usize>::Success(sizeof(packed));
    }

    for (uint32 component = 0; component < layout->components; ++component) {
        const float x = channels[component];
        switch (layout->encoding) {
        case TexelEncoding::Unorm8: {
            const uint8 q =
                static_cast<uint8>(RoundToInt(ClampOrZero(x, 0.0f, 1.0f) * Unorm8Scale));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Snorm8: {
            const int8 q = static_cast<int8>(RoundToInt(ClampOrZero(x, -1.0f, 1.0f) * Snorm8Scale));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Uint8: {
            const uint8 q = static_cast<uint8>(ClampOrZero(x, 0.0f, Uint8Max));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Unorm16: {
            const uint16 q =
                static_cast<uint16>(RoundToInt(ClampOrZero(x, 0.0f, 1.0f) * Unorm16Scale));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Uint16: {
            const uint16 q = static_cast<uint16>(ClampOrZero(x, 0.0f, Uint16Max));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Half: {
            const uint16 q = Float16::FromFloat(x).bits;
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Float32: {
            StoreBytes(bytes, size, &x, sizeof(x));
            size += sizeof(x);
            break;
        }

        case TexelEncoding::Uint32: {
            const uint32 q = static_cast<uint32>(ClampOrZero(x, 0.0f, Uint32MaxFloat));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Sint32: {
            const int32 q = static_cast<int32>(ClampOrZero(x, Int32MinFloat, Int32MaxFloat));
            StoreBytes(bytes, size, &q, sizeof(q));
            size += sizeof(q);
            break;
        }

        case TexelEncoding::Srgb8:
            break;
        }
    }

    return Result<usize>::Success(size);
}

Result<graphics::TextureHandle>
DefaultTextureCache::Find(graphics::Device& device, TextureDimension dimension, PixelFormat format,
                          const graphics::ClearColor& value) noexcept {
    const float values[4] = {value.r, value.g, value.b, value.a};
    uint32 bits[4] = {};
    platform::MemoryCopy(bits, values, sizeof(bits));
    for (usize index = 0; index < count_; ++index) {
        const Entry& entry = entries_[index];
        if (entry.dimension == dimension && entry.format == format &&
            SameKey(entry.valueBits, bits)) {
            return Result<graphics::TextureHandle>::Success(entry.texture);
        }
    }

    uint8 texel[MaxDefaultTexelBytes] = {};
    auto encoded = EncodeDefaultTexel(format, value, Span<uint8>(texel, sizeof(texel)));
    if (!encoded) {
        return Result<graphics::TextureHandle>::Failure(encoded.Error());
    }

    if (dimension == TextureDimension::TextureCubeArray &&
        !device.Caps().features.textureCubeArray) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::Unsupported);
    }

    if (count_ == MaxDefaultTextures) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::CapacityExceeded);
    }

    graphics::TextureDesc desc;
    desc.dimension = dimension;
    desc.width = 1;
    desc.height = 1;
    desc.format = format;
    desc.usage = graphics::TextureUsage::Sampled;
    desc.sampler.filter = graphics::Filter::Point;
    const uint32 slices = graphics::TextureSliceCount(desc);
    if (slices == 0 || slices > MaxDefaultSlices) {
        return Result<graphics::TextureHandle>::Failure(ErrorCode::InvalidArgument);
    }

    uint8 data[MaxDefaultTexelBytes * MaxDefaultSlices] = {};
    for (uint32 slice = 0; slice < slices; ++slice) {
        platform::MemoryCopy(data + slice * encoded.Value(), texel, encoded.Value());
    }

    auto created = device.CreateTexture(
        desc, Span<const uint8>(data, static_cast<usize>(slices) * encoded.Value()));
    if (!created) {
        return created;
    }

    Entry& entry = entries_[count_++];
    entry.dimension = dimension;
    entry.format = format;
    platform::MemoryCopy(entry.valueBits, bits, sizeof(bits));
    entry.texture = created.Value();
    return created;
}

void DefaultTextureCache::Retire(graphics::Device& device) noexcept {
    for (usize index = 0; index < count_; ++index) {
        device.Retire(entries_[index].texture);
        entries_[index] = Entry{};
    }

    count_ = 0;
}

} // namespace rocks::render
