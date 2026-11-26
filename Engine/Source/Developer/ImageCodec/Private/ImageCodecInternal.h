#pragma once

#include <Base/NumericLimits.h>
#include <Core/Math/MathUtility.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/Memory.h>

// 디코더 공통부다(Docs/Design/AssetSystem 12.9절): 실패·경고 기록, 결과 할당(한도 판정), 정수 읽기.
namespace rocks::image::detail {

inline Result<void> Fail(ImageDecodeResult& out, AssetDiagnosticCode code, ErrorCode error,
                         uint64 offset) noexcept {
    out.failure.code = code;
    out.failure.error = error;
    out.failure.offset = offset;
    return Result<void>::Failure(error);
}

// 구조·길이 위반이다(InvalidSource/CorruptData — 읽은 데이터 손상, AssetSystem 19장).
inline Result<void> Invalid(ImageDecodeResult& out, uint64 offset) noexcept {
    return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData, offset);
}

// 받지 않는 변형이다(UnsupportedSourceFormat/Unsupported).
inline Result<void> Unsupported(ImageDecodeResult& out, uint64 offset) noexcept {
    return Fail(out, AssetDiagnosticCode::UnsupportedSourceFormat, ErrorCode::Unsupported, offset);
}

// 자원 실패(할당 등)다 — 진단 None, 오류 그대로.
inline Result<void> Resource(ImageDecodeResult& out, ErrorCode error) noexcept {
    return Fail(out, AssetDiagnosticCode::None, error, 0);
}

inline Result<void> Warn(ImageDecodeResult& out, AssetDiagnosticCode code, uint32 detail) noexcept {
    if (auto pushed = out.warnings.TryPushBack(ImageDecodeWarning{code, detail}); !pushed) {
        return Resource(out, pushed.Error());
    }

    return Result<void>::Success();
}

// 한도 초과다(InvalidSource/CapacityExceeded).
inline Result<void> OverLimit(ImageDecodeResult& out, uint64 offset) noexcept {
    return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded, offset);
}

// 결과를 정하고 0으로 채운 픽셀 버퍼를 잡는다(slice numSlices개). 한 변·slice 수·결과 byte 한도는
// 할당 전에 본다.
inline Result<void> AllocateImage(const ImageDecodeLimits& limits, TextureSourceFormat format,
                                  uint32 width, uint32 height, ImageDecodeResult& out,
                                  uint32 numSlices = 1) noexcept {
    if (width == 0 || height == 0 || numSlices == 0) {
        return Invalid(out, 0);
    }

    if (width > limits.maxDimension || height > limits.maxDimension ||
        numSlices > limits.maxSlices) {
        return OverLimit(out, 0);
    }

    // 한도 값이 크게 주어져도 곱이 넘치지 않게 나눗셈으로 비교한다(width·height < 2^32, texel ≤
    // 16).
    const uint64 cap = limits.maxPixelBytes < NumericLimits<usize>::Max()
                           ? limits.maxPixelBytes
                           : static_cast<uint64>(NumericLimits<usize>::Max());
    const uint64 texelBytes = TextureSourceTexelBytes(format);
    const uint64 rowBytes = uint64{width} * texelBytes;
    if (rowBytes > cap / height) {
        return OverLimit(out, 0);
    }

    const uint64 sliceBytes = rowBytes * height;
    if (sliceBytes > cap / numSlices) {
        return OverLimit(out, 0);
    }

    const uint64 bytes = sliceBytes * numSlices;
    out.image.format = format;
    out.image.width = width;
    out.image.height = height;
    out.image.numSlices = numSlices;
    out.image.pixels.Clear();
    if (auto sized = out.image.pixels.TryResize(static_cast<usize>(bytes)); !sized) {
        return Resource(out, sized.Error());
    }

    for (uint8& value : out.image.pixels) {
        value = 0;
    }

    return Result<void>::Success();
}

// float 원본의 NaN·Inf를 정리한다(12.2절): NaN → 0, ±Inf → ±float 최대 유한값.
inline float SanitizeFloat(float value) noexcept {
    if (math::IsNaN(value)) {
        return 0.0f;
    }

    if (!math::IsFinite(value)) {
        return value > 0 ? NumericLimits<float>::Max() : -NumericLimits<float>::Max();
    }

    return value;
}

// half 비트의 NaN·Inf를 같은 규칙으로 정리한다(NaN → +0, ±Inf → ±65504).
inline uint16 SanitizeHalfBits(uint16 bits) noexcept {
    constexpr uint16 HalfExponentMask = 0x7C00;
    constexpr uint16 HalfMantissaMask = 0x03FF;
    constexpr uint16 HalfSignMask = 0x8000;
    constexpr uint16 HalfMaxFinite = 0x7BFF;
    if ((bits & HalfExponentMask) != HalfExponentMask) {
        return bits;
    }

    if ((bits & HalfMantissaMask) != 0) {
        return 0;
    }

    return static_cast<uint16>((bits & HalfSignMask) | HalfMaxFinite);
}

constexpr uint16 LoadLe16(const uint8* data) noexcept {
    return static_cast<uint16>(data[0] | (data[1] << 8));
}

constexpr uint32 LoadLe32(const uint8* data) noexcept {
    return static_cast<uint32>(data[0]) | (static_cast<uint32>(data[1]) << 8) |
           (static_cast<uint32>(data[2]) << 16) | (static_cast<uint32>(data[3]) << 24);
}

constexpr uint16 LoadBe16(const uint8* data) noexcept {
    return static_cast<uint16>((data[0] << 8) | data[1]);
}

constexpr uint32 LoadBe32(const uint8* data) noexcept {
    return (static_cast<uint32>(data[0]) << 24) | (static_cast<uint32>(data[1]) << 16) |
           (static_cast<uint32>(data[2]) << 8) | static_cast<uint32>(data[3]);
}

constexpr uint64 LoadBe64(const uint8* data) noexcept {
    return (static_cast<uint64>(LoadBe32(data)) << 32) | LoadBe32(data + 4);
}

inline void StoreLe16(uint8* data, uint16 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
}

inline void StoreLe32(uint8* data, uint32 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
    data[2] = static_cast<uint8>(value >> 16);
    data[3] = static_cast<uint8>(value >> 24);
}

// float 하나를 정리해 little-endian으로 쓴다.
inline void StoreSanitizedFloat(uint8* data, float value) noexcept {
    StoreLe32(data, platform::BitCast<uint32>(SanitizeFloat(value)));
}

// w비트 값 v를 8비트로 넓힌다(v × 255 / (2^w − 1), round-half-up — 12.2절). w는 1–16이다.
constexpr uint8 ExpandToByte(uint32 value, uint32 bits) noexcept {
    const uint32 maximum = bits >= 32 ? NumericLimits<uint32>::Max() : ((1u << bits) - 1u);
    return maximum == 0 ? 0 : static_cast<uint8>((uint64{value} * 255u + maximum / 2) / maximum);
}

Result<void> DecodeBmp(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodeTga(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodePng(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodePsd(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
// DDS 머리의 차원이다(PeekImageDimension — DecodeDds와 같은 규칙).
Result<TextureDimension> PeekDdsDimension(Span<const uint8> file,
                                          ImageDecodeFailure* failure) noexcept;
Result<void> DecodeDds(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodeTiff(Allocator& allocator, Span<const uint8> file,
                        const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodeHdr(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodeExr(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;
Result<void> DecodeJpeg(Allocator& allocator, Span<const uint8> file,
                        const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept;

} // namespace rocks::image::detail
