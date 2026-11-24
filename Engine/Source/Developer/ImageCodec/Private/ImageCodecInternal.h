#pragma once

#include <Base/NumericLimits.h>
#include <ImageCodec/ImageDecode.h>

// 디코더 공통부다(Docs/Design/AssetSystem 12.9절): 실패·경고 기록, 결과 할당(한도 판정), 정수 읽기.
namespace rocks::image::detail {

inline Result<void> Fail(ImageDecodeResult& out, AssetDiagnosticCode code, ErrorCode error,
                         uint64 offset) noexcept {
    out.failure.code = code;
    out.failure.error = error;
    out.failure.offset = offset;
    return Result<void>::Failure(error);
}

// 구조·길이 위반이다(InvalidSource/InvalidArgument).
inline Result<void> Invalid(ImageDecodeResult& out, uint64 offset) noexcept {
    return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, offset);
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

// 결과를 정하고 0으로 채운 픽셀 버퍼를 잡는다. 한 변·결과 byte 한도는 할당 전에 본다.
inline Result<void> AllocateImage(const ImageDecodeLimits& limits, TextureSourceFormat format,
                                  uint32 width, uint32 height, ImageDecodeResult& out) noexcept {
    if (width == 0 || height == 0) {
        return Invalid(out, 0);
    }

    if (width > limits.maxDimension || height > limits.maxDimension) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded, 0);
    }

    const uint64 bytes = uint64{width} * height * TextureSourceTexelBytes(format);
    if (bytes > limits.maxPixelBytes || bytes > NumericLimits<usize>::Max()) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded, 0);
    }

    out.image.format = format;
    out.image.width = width;
    out.image.height = height;
    out.image.numSlices = 1;
    out.image.pixels.Clear();
    if (auto sized = out.image.pixels.TryResize(static_cast<usize>(bytes)); !sized) {
        return Resource(out, sized.Error());
    }

    for (uint8& value : out.image.pixels) {
        value = 0;
    }

    return Result<void>::Success();
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

} // namespace rocks::image::detail
