#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Texture/TextureEnums.h>

// 텍스처 원본 이미지 디코더다(Docs/Design/AssetSystem 12.2·12.9·15절). 모든 형식을 같은 결과
// (TextureSourceFormat 픽셀 — 12.2절 표, 위 행부터)와 같은 진단으로 낸다. 입력은 신뢰하지 않는다:
// 모든 길이·오프셋·곱셈을 넘침 검사하고 출력 크기는 헤더로 계산해 한 번 할당한다. 색 공간은
// 변환하지 않는다(sRGB가 아닌 표시는 NonSrgbSource 경고).
namespace rocks::image {

// 원본 파일 형식이다. Dds·Tiff·Hdr·Exr은 A5f, Jpeg는 A5g가 구현한다(그 전에는
// UnsupportedSourceFormat). WebP는 손실이면 LossySource, 무손실이면 UnsupportedSourceFormat이다.
enum class ImageFileFormat : uint8 {
    Unknown,
    Bmp,
    Tga,
    Png,
    Psd,
    Dds,
    Tiff,
    Hdr,
    Exr,
    Jpeg,
    WebP
};

// 형식 이름("Bmp" 등 — 진단·시험용)이다.
ROCKS_IMAGEDECODERS_API const char* ImageFileFormatName(ImageFileFormat format) noexcept;

// 서명으로 형식을 판별한다. 서명이 없는 TGA는 TGA 2.0 footer("TRUEVISION-XFILE.") 또는 pathHint
// 확장자(.tga·.icb·.vda·.vst, 대소문자 무시)로 판별한다. 모르면 Unknown이다.
ROCKS_IMAGEDECODERS_API ImageFileFormat DetectImageFileFormat(Span<const uint8> file,
                                                              StringView pathHint = {}) noexcept;

// 디코딩한 이미지다. pixels는 width × height × numSlices × (형식의 texel byte)이고 행 순서는 위에서
// 아래, 행 사이 패딩 없음, 채널은 형식 순서(G·GA·RGBA), 16비트 정수와 float는 little-endian이다.
// slice는 차례로 이어 붙인다(A5f DDS — A5e 형식은 늘 1).
struct DecodedImage {
    explicit DecodedImage(Allocator& allocator) noexcept : pixels(allocator) {
    }

    TextureSourceFormat format = TextureSourceFormat::Invalid;
    uint32 width = 0;
    uint32 height = 0;
    uint32 numSlices = 1;
    Array<uint8> pixels;
};

// 디코딩 한도다. 한 변·결과 byte가 넘으면 InvalidSource/CapacityExceeded다(할당 전에 판정). 16384²
// RGBA32F가 정확히 4 GiB다. 임포트 원본 파일 합 한도(2 GiB)는 입력 byte라 따로 본다.
struct ImageDecodeLimits {
    static constexpr uint32 DefaultMaxDimension = 16384;
    static constexpr uint64 DefaultMaxPixelBytes = uint64{4} << 30;

    uint32 maxDimension = DefaultMaxDimension;
    uint64 maxPixelBytes = DefaultMaxPixelBytes;
};

// 경고 하나다(NonSrgbSource·IgnoredSourceChannels — detail은 버린 채널 수 등).
struct ImageDecodeWarning {
    AssetDiagnosticCode code = AssetDiagnosticCode::NonSrgbSource;
    uint32 detail = 0;
};

// 실패의 분류다: 구조·길이·CRC·체크섬 위반 InvalidSource/InvalidArgument, 받지 않는 변형
// UnsupportedSourceFormat/Unsupported, 손실 형식 LossySource/Unsupported, 한도
// InvalidSource/CapacityExceeded, 할당 실패 None/OutOfMemory. offset은 판정한 byte 위치(진단용)다.
struct ImageDecodeFailure {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    uint64 offset = 0;
};

// 디코딩 결과다. 성공이면 image와 warnings, 실패면 failure가 뜻이 있다.
struct ImageDecodeResult {
    explicit ImageDecodeResult(Allocator& allocator) noexcept
        : image(allocator), warnings(allocator) {
    }

    DecodedImage image;
    Array<ImageDecodeWarning> warnings;
    ImageDecodeFailure failure;
};

// file을 format으로 디코딩해 out에 쓴다(Unknown이면 DetectImageFileFormat(file)로 판별 — 판별
// 실패는 UnsupportedSourceFormat). 반환 오류는 out.failure.error와 같다.
//
// 사용 예:
//   image::ImageDecodeResult decoded(allocator);
//   auto done = image::DecodeImage(allocator, bytes.View(), image::ImageFileFormat::Unknown, {},
//                                  decoded);
//   if (!done) { /* decoded.failure.code로 진단 */ }
ROCKS_IMAGEDECODERS_API Result<void> DecodeImage(Allocator& allocator, Span<const uint8> file,
                                                 ImageFileFormat format,
                                                 const ImageDecodeLimits& limits,
                                                 ImageDecodeResult& out) noexcept;

// TextureSourceFormat 한 texel의 byte 수다(Invalid는 0).
ROCKS_IMAGEDECODERS_API uint32 TextureSourceTexelBytes(TextureSourceFormat format) noexcept;

} // namespace rocks::image
