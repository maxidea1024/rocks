#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// TIFF·EXR 쓰기(부호화)다(Docs/Design/AssetSystem 12.10.1절 — libtiff·OpenEXRCore는 프로세스 전역
// 상태가 있어 ImageCodec만 직접 의존하므로, 다른 모듈·시험은 이 API로 파일을 만든다). 값 조합은
// 검증하지 않고 라이브러리가 받는 대로 쓴다(디코더의 거부 시험을 만들 수 있게). 공개 헤더는
// 서드파티 헤더를 include하지 않는다 — TIFF 값은 TIFF 6.0 명세 숫자(libtiff 매크로와 같은 값), EXR
// 값은 OpenEXRCore 열거와 같은 번호의 Rocks 열거다.
//
// 사용 예:
//   TiffWriteImage image;
//   image.width = 2;
//   image.samples = 3;
//   image.photometric = tiff::PhotometricRgb;
//   image.compression = tiff::CompressionLzw;
//   image.raw = rgb;  // 2 × 3 byte
//   Array<uint8> file(allocator);
//   if (auto written = EncodeTiff(allocator, image, file); !written) { ... }
namespace rocks::image {

// TIFF 6.0 태그 번호와 필드 값이다(명세 숫자 — libtiff tiff.h의 같은 이름 매크로와 같은 값).
namespace tiff {

inline constexpr uint16 TagImageWidth = 256;
inline constexpr uint16 TagImageLength = 257;
inline constexpr uint16 TagBitsPerSample = 258;
inline constexpr uint16 TagCompression = 259;
inline constexpr uint16 TagPhotometric = 262;
inline constexpr uint16 TagStripOffsets = 273;
inline constexpr uint16 TagOrientation = 274;
inline constexpr uint16 TagSamplesPerPixel = 277;
inline constexpr uint16 TagRowsPerStrip = 278;
inline constexpr uint16 TagStripByteCounts = 279;
inline constexpr uint16 TagPlanarConfig = 284;
inline constexpr uint16 TagPredictor = 317;
inline constexpr uint16 TagColorMap = 320;
inline constexpr uint16 TagTileWidth = 322;
inline constexpr uint16 TagTileLength = 323;
inline constexpr uint16 TagExtraSamples = 338;
inline constexpr uint16 TagSampleFormat = 339;
inline constexpr uint16 TagYCbCrSubSampling = 530;

inline constexpr uint16 CompressionNone = 1;
inline constexpr uint16 CompressionCcittFax3 = 3;
inline constexpr uint16 CompressionCcittFax4 = 4;
inline constexpr uint16 CompressionLzw = 5;
inline constexpr uint16 CompressionOJpeg = 6;
inline constexpr uint16 CompressionJpeg = 7;
inline constexpr uint16 CompressionAdobeDeflate = 8;
inline constexpr uint16 CompressionPackBits = 32773;
inline constexpr uint16 CompressionPixarLog = 32909;
inline constexpr uint16 CompressionSgiLog = 34676;
inline constexpr uint16 CompressionZstd = 50000;

inline constexpr uint16 PhotometricMinIsWhite = 0;
inline constexpr uint16 PhotometricMinIsBlack = 1;
inline constexpr uint16 PhotometricRgb = 2;
inline constexpr uint16 PhotometricPalette = 3;
inline constexpr uint16 PhotometricSeparated = 5;
inline constexpr uint16 PhotometricYCbCr = 6;
inline constexpr uint16 PhotometricLogL = 32844;
inline constexpr uint16 PhotometricLogLuv = 32845;

inline constexpr uint16 PlanarContig = 1;
inline constexpr uint16 PlanarSeparate = 2;

inline constexpr uint16 SampleFormatUint = 1;
inline constexpr uint16 SampleFormatInt = 2;
inline constexpr uint16 SampleFormatIeeeFp = 3;

inline constexpr uint16 OrientationTopLeft = 1;
inline constexpr uint16 OrientationTopRight = 2;
inline constexpr uint16 OrientationBotLeft = 4;

inline constexpr uint16 PredictorNone = 1;
inline constexpr uint16 PredictorHorizontal = 2;
inline constexpr uint16 PredictorFloatingPoint = 3;

inline constexpr uint16 ExtraSampleUnspecified = 0;
inline constexpr uint16 ExtraSampleAssocAlpha = 1;
inline constexpr uint16 ExtraSampleUnassAlpha = 2;

} // namespace tiff

// TIFF 파일의 byte 순서다.
enum class TiffByteOrder : uint8 {
    Little,
    Big,
};

// 쓸 TIFF 이미지다. raw는 TIFF 저장 순서의 표본 행(contig: 행마다 모든 표본, separate: 평면마다
// 모든 행)이고 한 행은 ceil(width × 행 표본 × bits / 8) byte다. pages만큼 같은 IFD를 잇는다.
struct TiffWriteImage {
    uint32 width = 1;
    uint32 height = 1;
    uint16 bits = 8;
    uint16 samples = 1;
    uint16 sampleFormat = tiff::SampleFormatUint;
    uint16 photometric = tiff::PhotometricMinIsBlack;
    uint16 compression = tiff::CompressionNone;
    // PredictorNone이면 태그를 쓰지 않는다.
    uint16 predictor = tiff::PredictorNone;
    uint16 planar = tiff::PlanarContig;
    uint16 orientation = tiff::OrientationTopLeft;
    // 0이면 태그를 쓰지 않는다(libtiff 기본).
    uint32 rowsPerStrip = 0;
    // tileWidth가 0이 아니면 tile로 쓴다(가장자리 tile은 0으로 채운다).
    uint32 tileWidth = 0;
    uint32 tileHeight = 0;
    // writeExtraSamples면 ExtraSamples 태그를 이 값들로 쓴다(빈 목록 포함).
    Span<const uint16> extraSamples{};
    bool writeExtraSamples = false;
    // 색표(R·G·B 각 2^bits 항목, 연이어)다. nullptr이면 쓰지 않는다.
    const uint16* colormap = nullptr;
    // YCbCrSubSampling을 1, 1로 쓴다.
    bool ycbcr11 = false;
    // JPEG 압축의 입력이 RGB다(libtiff가 YCbCr 4:2:0으로 바꿔 쓴다 — JPEGCOLORMODE_RGB).
    bool jpegColorModeRgb = false;
    Span<const uint8> raw{};
    uint32 pages = 1;
    TiffByteOrder byteOrder = TiffByteOrder::Little;
    bool bigTiff = false;
};

// image를 TIFF 메모리 파일로 out에 쓴다(out을 바꾼다). 선택 태그(Predictor·ExtraSamples·색표·YCbCr
// subsampling·JPEG 색 모드)를 libtiff가 받지 않으면(예: PackBits의 Predictor) 쓰지 않고 계속한다.
// 오류: 크기·표본·비트 0, raw 길이 부족,
// libtiff가 거부한 조합 InvalidArgument, 할당 실패 OutOfMemory. 실패하면 out은 비어 있다.
ROCKS_IMAGECODEC_API Result<void> EncodeTiff(Allocator& allocator, const TiffWriteImage& image,
                                             Array<uint8>& out) noexcept;

// EXR 채널 원소 형식이다(OpenEXRCore exr_pixel_type_t와 같은 번호).
enum class ExrPixelType : uint8 {
    Uint = 0,
    Half = 1,
    Float = 2,
};

// EXR 압축이다(OpenEXRCore exr_compression_t와 같은 번호).
enum class ExrCompression : uint8 {
    None = 0,
    Rle = 1,
    Zips = 2,
    Zip = 3,
    Piz = 4,
    Pxr24 = 5,
    B44 = 6,
    B44a = 7,
    Dwaa = 8,
    Dwab = 9,
    Htj2k256 = 10,
    Htj2k32 = 11,
};

// EXR line order다(OpenEXRCore exr_lineorder_t와 같은 번호).
enum class ExrLineOrder : uint8 {
    IncreasingY = 0,
    DecreasingY = 1,
    RandomY = 2,
};

// EXR part 저장 형식이다(OpenEXRCore exr_storage_t와 같은 번호). deep은 머리만 쓴다.
enum class ExrStorage : uint8 {
    Scanline = 0,
    Tiled = 1,
    DeepScanline = 2,
    DeepTiled = 3,
};

// tile level 방식이다(OpenEXRCore exr_tile_level_mode_t와 같은 번호).
enum class ExrLevelMode : uint8 {
    OneLevel = 0,
    MipmapLevels = 1,
    RipmapLevels = 2,
};

// 쓸 채널 하나다. data는 data window의 행 우선 원소(Half 2 byte, Uint·Float 4 byte)이고
// sampling은 x·y 공통 subsampling이다.
struct ExrChannelWrite {
    StringView name{};
    ExrPixelType type = ExrPixelType::Half;
    Span<const uint8> data{};
    int32 sampling = 1;
};

// 쓸 EXR 이미지다. parts만큼 같은 채널의 part를 쓴다(이름 a·b·c — 최대 3). tile이면 tileWidth·
// tileHeight·levels를 쓰고 level > 0은 0으로 채운다. RandomY tile은 거꾸로 쓴다.
struct ExrWriteImage {
    int32 minX = 0;
    int32 minY = 0;
    int32 width = 1;
    int32 height = 1;
    ExrCompression compression = ExrCompression::None;
    uint32 tileWidth = 0;
    uint32 tileHeight = 0;
    ExrLevelMode levels = ExrLevelMode::OneLevel;
    ExrLineOrder lineOrder = ExrLineOrder::IncreasingY;
    ExrStorage storage = ExrStorage::Scanline;
    Span<const ExrChannelWrite> channels{};
    int32 parts = 1;
};

// 최대 part 수다.
inline constexpr int32 ExrWriteMaxParts = 3;

// image를 EXR 메모리 파일로 out에 쓴다(out을 바꾼다). 오류: 크기 0, 채널 없음, part 수 범위 밖,
// 채널 data 길이 부족, OpenEXRCore가 거부한 조합 InvalidArgument, 할당 실패 OutOfMemory. 실패하면
// out은 비어 있다.
ROCKS_IMAGECODEC_API Result<void> EncodeExr(Allocator& allocator, const ExrWriteImage& image,
                                            Array<uint8>& out) noexcept;

} // namespace rocks::image
