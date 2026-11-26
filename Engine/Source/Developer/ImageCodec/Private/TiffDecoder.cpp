#include "ExternalBindings.h"
#include "ImageCodecInternal.h"

#include <Core/Math/Float16.h>
#include <tiffio.h>

// TIFF 디코더다(Docs/Design/AssetSystem 12.10절 — libtiff, UE TiffImageWrapper.cpp 701–820과 같은
// 받는 범위). 첫 IFD만 디코딩하고 나머지 IFD는 IgnoredSourceImages 경고다. 회색·RGB·팔레트는
// strip·tile을 직접 읽어(libtiff가 압축·Predictor·바이트 순서·FillOrder를 푼다) 저장된 값을 그대로
// 옮기고, YCbCr· CIELab 계열만 libtiff RGBA 변환으로 RGBA8을 만든다. 알파는 기본 표본 뒤 첫 추가
// 표본이다 (ExtraSamples 값과 무관 — premultiply·되돌림 없음). 수치 변환·MinIsWhite 반전·방향
// 뒤집기는 UE와 같다(64비트 float는 자르지 않는다 — UE와 다르다).
namespace rocks::image::detail {
namespace {

constexpr uint16 TiffCompressionPixarLog = 32909;
constexpr uint16 TiffCompressionSgiLog = 34676;
constexpr uint16 TiffCompressionSgiLog24 = 34677;
constexpr uint16 TiffPhotometricLogL = 32844;
constexpr uint16 TiffPhotometricLogLuv = 32845;
constexpr uint16 TiffSampleFormatUnspecified = 0;
constexpr usize TiffRgbaMessageBytes = 1024;

// 메모리 입력이다(읽기 전용 — 쓰기·mmap 없음).
struct TiffMemory {
    Span<const uint8> file;
    uint64 position = 0;
};

tmsize_t TiffRead(thandle_t handle, void* buffer, tmsize_t size) noexcept {
    auto* memory = static_cast<TiffMemory*>(handle);
    if (size <= 0 || memory->position >= memory->file.Size()) {
        return 0;
    }

    const uint64 available = memory->file.Size() - memory->position;
    const uint64 count =
        static_cast<uint64>(size) < available ? static_cast<uint64>(size) : available;
    platform::MemoryCopy(buffer, memory->file.Data() + memory->position, static_cast<usize>(count));
    memory->position += count;
    return static_cast<tmsize_t>(count);
}

tmsize_t TiffWrite(thandle_t, void*, tmsize_t) noexcept {
    return 0;
}

toff_t TiffSeek(thandle_t handle, toff_t offset, int whence) noexcept {
    auto* memory = static_cast<TiffMemory*>(handle);
    uint64 base = 0;
    if (whence == SEEK_CUR) {
        base = memory->position;
    } else if (whence == SEEK_END) {
        base = memory->file.Size();
    } else if (whence != SEEK_SET) {
        return static_cast<toff_t>(-1);
    }

    if (offset > NumericLimits<uint64>::Max() - base) {
        return static_cast<toff_t>(-1);
    }

    memory->position = base + offset;
    return memory->position;
}

int TiffClose(thandle_t) noexcept {
    return 0;
}

toff_t TiffSize(thandle_t handle) noexcept {
    return static_cast<TiffMemory*>(handle)->file.Size();
}

int TiffMap(thandle_t, void**, toff_t*) noexcept {
    return 0;
}

void TiffUnmap(thandle_t, void*, toff_t) noexcept {
}

// libtiff 메시지는 버린다(진단은 코드·offset — 12.9절, 실행 중 출력 금지). 1은 "처리했음"이다.
int TiffMessage(TIFF*, void*, const char*, const char*, va_list) noexcept {
    return 1;
}

class TiffHandle {
public:
    explicit TiffHandle(TIFF* tiff) noexcept : tiff_(tiff) {
    }

    ~TiffHandle() noexcept {
        if (tiff_ != nullptr) {
            TIFFClose(tiff_);
        }
    }

    TiffHandle(const TiffHandle&) = delete;
    TiffHandle& operator=(const TiffHandle&) = delete;

    TIFF* Get() const noexcept {
        return tiff_;
    }

private:
    TIFF* tiff_;
};

// 라이브러리 실패다: 할당이 실패했으면 OutOfMemory, 아니면 손상(12.10절 공통 규칙 2).
Result<void> TiffLibraryFailure(const ExternalAllocationScope& scope,
                                ImageDecodeResult& out) noexcept {
    return scope.AllocationFailed() ? Resource(out, ErrorCode::OutOfMemory) : Invalid(out, 0);
}

enum class TiffColor : uint8 {
    Gray,
    Rgb,
    Palette
};

// 디코딩할 이미지의 모양이다.
struct TiffShape {
    uint32 width = 0;
    uint32 height = 0;
    uint16 bits = 1;
    uint16 samples = 1;
    uint16 sampleFormat = SAMPLEFORMAT_UINT;
    uint16 planar = PLANARCONFIG_CONTIG;
    uint16 orientation = ORIENTATION_TOPLEFT;
    TiffColor color = TiffColor::Gray;
    bool minIsWhite = false;
    bool alpha = false;
    uint16 baseSamples = 1;
};

uint64 TiffLoadLe64(const uint8* data) noexcept {
    return static_cast<uint64>(LoadLe32(data)) | (static_cast<uint64>(LoadLe32(data + 4)) << 32);
}

// 행 안 index번째 표본의 비트다(libtiff가 바이트 순서를 호스트로, FillOrder를 MSB 먼저로 맞췄다).
uint64 TiffSample(const uint8* row, uint64 index, uint16 bits) noexcept {
    switch (bits) {
    case 8:
        return row[index];
    case 16:
        return LoadLe16(row + index * 2);
    case 32:
        return LoadLe32(row + index * 4);
    case 64:
        return TiffLoadLe64(row + index * 8);
    default: {
        // 1·2·4비트: 행 첫 비트부터 MSB 먼저.
        const uint64 bit = index * bits;
        const uint32 shift = 8u - bits - static_cast<uint32>(bit % 8);
        return (row[bit / 8] >> shift) & ((1u << bits) - 1u);
    }
    }
}

// 정수 표본을 16비트로 옮긴다(UE ConvertToWriteFormat: 32·64비트는 v / max × 65535 + 0.5 내림).
uint16 TiffToUnorm16(uint64 value, uint16 bits) noexcept {
    if (bits == 16) {
        return static_cast<uint16>(value);
    }

    const double maximum = bits == 32 ? static_cast<double>(NumericLimits<uint32>::Max())
                                      : static_cast<double>(NumericLimits<uint64>::Max());
    const double scaled = static_cast<double>(value) * (1.0 / maximum);
    return static_cast<uint16>(scaled * static_cast<double>(NumericLimits<uint16>::Max()) + 0.50);
}

// float 표본(16·32·64비트)을 float로 읽는다(64비트 double은 float로 — 자르지 않는다).
float TiffToFloat(uint64 value, uint16 bits) noexcept {
    if (bits == 16) {
        return Float16::FromBits(static_cast<uint16>(value)).ToFloat();
    }

    if (bits == 32) {
        return platform::BitCast<float>(static_cast<uint32>(value));
    }

    return static_cast<float>(platform::BitCast<double>(value));
}

TextureSourceFormat TiffOutputFormat(const TiffShape& shape) noexcept {
    const bool floating = shape.sampleFormat == SAMPLEFORMAT_IEEEFP;
    const uint16 bits = shape.bits;
    if (floating) {
        if (shape.color == TiffColor::Palette || (bits != 16 && bits != 32 && bits != 64)) {
            return TextureSourceFormat::Invalid;
        }

        const bool half = bits == 16;
        if (shape.color == TiffColor::Gray && !shape.alpha) {
            return half ? TextureSourceFormat::R16F : TextureSourceFormat::R32F;
        }

        return half ? TextureSourceFormat::RGBA16F : TextureSourceFormat::RGBA32F;
    }

    if (bits == 1 || bits == 2 || bits == 4 || bits == 8) {
        if (shape.color == TiffColor::Gray) {
            return shape.alpha ? TextureSourceFormat::GA8 : TextureSourceFormat::G8;
        }

        return TextureSourceFormat::RGBA8;
    }

    if (bits == 16 || ((bits == 32 || bits == 64) && shape.color != TiffColor::Palette)) {
        if (shape.color == TiffColor::Gray) {
            return shape.alpha ? TextureSourceFormat::GA16 : TextureSourceFormat::G16;
        }

        return TextureSourceFormat::RGBA16;
    }

    return TextureSourceFormat::Invalid;
}

// 색표다(3 × 2^bits 항목, 16비트). eightBit는 libtiff checkcmap과 같다: 모든 값이 256 미만이면
// 8비트 색표로 보고 그대로, 아니면 >> 8로 8비트를 만든다.
struct TiffPalette {
    const uint16* red = nullptr;
    const uint16* green = nullptr;
    const uint16* blue = nullptr;
    bool eightBit = false;
};

// 한 texel을 결과 형식으로 쓴다. samples는 이 texel의 표본(기본 + 알파) 값이다.
void TiffStoreTexel(const TiffShape& shape, const TiffPalette& palette, TextureSourceFormat format,
                    const uint64* samples, uint8* d) noexcept {
    const uint16 bits = shape.bits;
    switch (format) {
    case TextureSourceFormat::G8:
    case TextureSourceFormat::GA8: {
        uint8 gray = ExpandToByte(static_cast<uint32>(samples[0]), bits);
        if (shape.minIsWhite) {
            gray = static_cast<uint8>(255 - gray);
        }

        d[0] = gray;
        if (format == TextureSourceFormat::GA8) {
            d[1] = ExpandToByte(static_cast<uint32>(samples[1]), bits);
        }

        return;
    }
    case TextureSourceFormat::G16:
    case TextureSourceFormat::GA16: {
        uint16 gray = TiffToUnorm16(samples[0], bits);
        if (shape.minIsWhite) {
            gray = static_cast<uint16>(NumericLimits<uint16>::Max() - gray);
        }

        StoreLe16(d, gray);
        if (format == TextureSourceFormat::GA16) {
            StoreLe16(d + 2, TiffToUnorm16(samples[1], bits));
        }

        return;
    }
    case TextureSourceFormat::RGBA8:
        if (shape.color == TiffColor::Palette) {
            const usize index = static_cast<usize>(samples[0]);
            const auto convert = [&palette](uint16 value) noexcept {
                return static_cast<uint8>(palette.eightBit ? value : value >> 8);
            };
            d[0] = convert(palette.red[index]);
            d[1] = convert(palette.green[index]);
            d[2] = convert(palette.blue[index]);
            d[3] = shape.alpha ? ExpandToByte(static_cast<uint32>(samples[1]), bits) : 255;
            return;
        }

        for (uint32 c = 0; c < 3; ++c) {
            d[c] = ExpandToByte(static_cast<uint32>(samples[c]), bits);
        }

        d[3] = shape.alpha ? ExpandToByte(static_cast<uint32>(samples[3]), bits) : 255;
        return;
    case TextureSourceFormat::RGBA16:
        if (shape.color == TiffColor::Palette) {
            const usize index = static_cast<usize>(samples[0]);
            StoreLe16(d, palette.red[index]);
            StoreLe16(d + 2, palette.green[index]);
            StoreLe16(d + 4, palette.blue[index]);
            StoreLe16(d + 6,
                      shape.alpha ? TiffToUnorm16(samples[1], bits) : NumericLimits<uint16>::Max());
            return;
        }

        for (uint32 c = 0; c < 3; ++c) {
            StoreLe16(d + c * 2, TiffToUnorm16(samples[c], bits));
        }

        StoreLe16(d + 6,
                  shape.alpha ? TiffToUnorm16(samples[3], bits) : NumericLimits<uint16>::Max());
        return;
    case TextureSourceFormat::R16F:
    case TextureSourceFormat::RGBA16F: {
        // 16비트 float 원본만 여기 온다 — half 비트를 그대로(MinIsWhite는 1 − v, UE와 같다).
        const auto half = [&shape](uint64 value, bool color) noexcept {
            uint16 bitsValue = static_cast<uint16>(value);
            if (color && shape.minIsWhite) {
                bitsValue = Float16::FromFloat(1.0f - Float16::FromBits(bitsValue).ToFloat()).bits;
            }

            return SanitizeHalfBits(bitsValue);
        };
        if (format == TextureSourceFormat::R16F) {
            StoreLe16(d, half(samples[0], true));
            return;
        }

        const bool gray = shape.color == TiffColor::Gray;
        const uint32 alphaIndex = gray ? 1 : 3;
        for (uint32 c = 0; c < 3; ++c) {
            StoreLe16(d + c * 2, half(samples[gray ? 0 : c], true));
        }

        StoreLe16(d + 6,
                  shape.alpha ? half(samples[alphaIndex], false) : Float16::FromFloat(1.0f).bits);
        return;
    }
    case TextureSourceFormat::R32F:
    case TextureSourceFormat::RGBA32F: {
        const auto single = [&shape, bits](uint64 value, bool color) noexcept {
            float result = TiffToFloat(value, bits);
            if (color && shape.minIsWhite) {
                result = 1.0f - result;
            }

            return result;
        };
        if (format == TextureSourceFormat::R32F) {
            StoreSanitizedFloat(d, single(samples[0], true));
            return;
        }

        const bool gray = shape.color == TiffColor::Gray;
        const uint32 alphaIndex = gray ? 1 : 3;
        for (uint32 c = 0; c < 3; ++c) {
            StoreSanitizedFloat(d + c * 4, single(samples[gray ? 0 : c], true));
        }

        StoreSanitizedFloat(d + 12, shape.alpha ? single(samples[alphaIndex], false) : 1.0f);
        return;
    }
    case TextureSourceFormat::Invalid:
        return;
    }
}

// UE가 libtiff RGBA 경로(TIFFReadRGBAImageOriented)로 읽는 형식이다 — 그 경로는 Orientation의
// 상하·좌우 뒤집기를 적용한다(전치 방향 5–8도 뒤집기로만). RGB 1·2·4비트는 RGBA 경로가 받지 않아
// UE도 직접 경로로 읽는다(뒤집기 없음).
bool TiffAppliesOrientation(const TiffShape& shape) noexcept {
    if (shape.sampleFormat == SAMPLEFORMAT_IEEEFP) {
        return false;
    }

    switch (shape.color) {
    case TiffColor::Rgb:
        return shape.bits == 8;
    case TiffColor::Palette:
        return shape.bits <= 8;
    case TiffColor::Gray:
        return shape.alpha && shape.bits == 8;
    }

    return false;
}

// libtiff setorientation(요청 TOPLEFT)과 같은 뒤집기를 결과에 적용한다.
void TiffApplyOrientation(uint16 orientation, ImageDecodeResult& out) noexcept {
    bool horizontal = false;
    bool vertical = false;
    switch (orientation) {
    case ORIENTATION_TOPRIGHT:
    case ORIENTATION_RIGHTTOP:
        horizontal = true;
        break;
    case ORIENTATION_BOTRIGHT:
    case ORIENTATION_RIGHTBOT:
        horizontal = true;
        vertical = true;
        break;
    case ORIENTATION_BOTLEFT:
    case ORIENTATION_LEFTBOT:
        vertical = true;
        break;
    default:
        break;
    }

    const usize texel = TextureSourceTexelBytes(out.image.format);
    const usize rowBytes = static_cast<usize>(out.image.width) * texel;
    uint8* pixels = out.image.pixels.Data();
    if (vertical) {
        for (uint32 y = 0; y < out.image.height / 2; ++y) {
            uint8* top = pixels + static_cast<usize>(y) * rowBytes;
            uint8* bottom = pixels + static_cast<usize>(out.image.height - 1 - y) * rowBytes;
            for (usize index = 0; index < rowBytes; ++index) {
                const uint8 value = top[index];
                top[index] = bottom[index];
                bottom[index] = value;
            }
        }
    }

    if (horizontal) {
        for (uint32 y = 0; y < out.image.height; ++y) {
            uint8* row = pixels + static_cast<usize>(y) * rowBytes;
            for (uint32 x = 0; x < out.image.width / 2; ++x) {
                uint8* left = row + static_cast<usize>(x) * texel;
                uint8* right = row + static_cast<usize>(out.image.width - 1 - x) * texel;
                for (usize c = 0; c < texel; ++c) {
                    const uint8 value = left[c];
                    left[c] = right[c];
                    right[c] = value;
                }
            }
        }
    }
}

// strip·tile을 띠(band — strip 높이 또는 tile 높이의 행 묶음) 단위로 읽어 결과로 옮긴다.
Result<void> TiffDecodeDirect(Allocator& allocator, TIFF* tiff, const TiffShape& shape,
                              const TiffPalette& palette, const ImageDecodeLimits& limits,
                              const ExternalAllocationScope& scope,
                              ImageDecodeResult& out) noexcept {
    const bool separate = shape.planar == PLANARCONFIG_SEPARATE && shape.samples > 1;
    const uint16 planes = separate ? shape.samples : 1;
    const uint16 rowSamples = separate ? 1 : shape.samples;
    const bool tiled = TIFFIsTiled(tiff) != 0;
    uint32 tileWidth = 0;
    uint32 bandRows = 0;
    if (tiled) {
        uint32 tileLength = 0;
        if (TIFFGetField(tiff, TIFFTAG_TILEWIDTH, &tileWidth) != 1 ||
            TIFFGetField(tiff, TIFFTAG_TILELENGTH, &tileLength) != 1 || tileWidth == 0 ||
            tileLength == 0) {
            return Invalid(out, 0);
        }

        // tile 한 변도 한 변 한도를 따른다 — 1×1 그림에 거대한 tile을 선언해 띠·tile 버퍼를 키우지
        // 못하게 한다.
        if (tileWidth > limits.maxDimension || tileLength > limits.maxDimension) {
            return OverLimit(out, 0);
        }

        bandRows = tileLength;
    } else {
        uint32 rowsPerStrip = 0;
        TIFFGetFieldDefaulted(tiff, TIFFTAG_ROWSPERSTRIP, &rowsPerStrip);
        bandRows = rowsPerStrip == 0 || rowsPerStrip > shape.height ? shape.height : rowsPerStrip;
    }

    // 띠의 행 byte(평면 하나) — tile이면 tile 열 전체 폭이다.
    const uint64 tilesAcross = tiled ? (uint64{shape.width} + tileWidth - 1) / tileWidth : 1;
    const uint64 bandWidth = tiled ? tilesAcross * tileWidth : shape.width;
    const uint64 bandRowBytes = (bandWidth * rowSamples * shape.bits + 7) / 8;
    const uint64 tileRowBytes = tiled ? (uint64{tileWidth} * rowSamples * shape.bits + 7) / 8 : 0;
    const uint64 bandBytes = bandRowBytes * bandRows;
    if (bandBytes == 0 || bandBytes > limits.maxPixelBytes ||
        bandBytes * planes > limits.maxPixelBytes) {
        return OverLimit(out, 0);
    }

    Array<uint8> band(allocator);
    if (auto sized = band.TryResize(static_cast<usize>(bandBytes * planes)); !sized) {
        return Resource(out, sized.Error());
    }

    Array<uint8> tile(allocator);
    if (tiled) {
        const tmsize_t tileBytes = TIFFTileSize(tiff);
        if (tileBytes <= 0 || static_cast<uint64>(tileBytes) != tileRowBytes * bandRows) {
            return Invalid(out, 0);
        }

        if (auto sized = tile.TryResize(static_cast<usize>(tileBytes)); !sized) {
            return Resource(out, sized.Error());
        }
    }

    const TextureSourceFormat format = out.image.format;
    const usize texelBytes = TextureSourceTexelBytes(format);
    constexpr uint32 MaxTexelSamples = 4;
    uint8* destination = out.image.pixels.Data();
    for (uint32 row = 0; row < shape.height; row += bandRows) {
        const uint32 rows = shape.height - row < bandRows ? shape.height - row : bandRows;
        for (uint16 plane = 0; plane < planes; ++plane) {
            uint8* planeBand = band.Data() + static_cast<usize>(bandBytes * plane);
            if (!tiled) {
                const uint32 strip = TIFFComputeStrip(tiff, row, plane);
                const tmsize_t needed = static_cast<tmsize_t>(bandRowBytes * rows);
                if (TIFFReadEncodedStrip(tiff, strip, planeBand, needed) < needed) {
                    return TiffLibraryFailure(scope, out);
                }

                continue;
            }

            for (uint64 column = 0; column < tilesAcross; ++column) {
                const uint32 x = static_cast<uint32>(column * tileWidth);
                const uint32 index = TIFFComputeTile(tiff, x, row, 0, plane);
                if (TIFFReadEncodedTile(tiff, index, tile.Data(),
                                        static_cast<tmsize_t>(tile.Size())) <
                    static_cast<tmsize_t>(tile.Size())) {
                    return TiffLibraryFailure(scope, out);
                }

                for (uint32 r = 0; r < rows; ++r) {
                    platform::MemoryCopy(planeBand + r * bandRowBytes + column * tileRowBytes,
                                         tile.Data() + r * tileRowBytes,
                                         static_cast<usize>(tileRowBytes));
                }
            }
        }

        // 띠의 행을 결과로 옮긴다. 쓰는 표본은 기본 표본과 알파(첫 추가 표본)다.
        const uint16 usedSamples = static_cast<uint16>(shape.baseSamples + (shape.alpha ? 1 : 0));
        for (uint32 r = 0; r < rows; ++r) {
            for (uint32 x = 0; x < shape.width; ++x) {
                uint64 samples[MaxTexelSamples] = {};
                for (uint16 s = 0; s < usedSamples; ++s) {
                    const uint16 plane = separate ? s : 0;
                    const uint8* line = band.Data() + static_cast<usize>(bandBytes * plane) +
                                        static_cast<usize>(r * bandRowBytes);
                    const uint64 index = separate ? x : uint64{x} * shape.samples + s;
                    // RGB 알파는 넷째 칸, 회색·팔레트 알파는 둘째 칸이다.
                    const uint32 slot = (shape.color == TiffColor::Rgb && s == 3) ? 3 : s;
                    samples[slot] = TiffSample(line, index, shape.bits);
                }

                TiffStoreTexel(shape, palette, format, samples, destination);
                destination += texelBytes;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> DecodeTiff(Allocator& allocator, Span<const uint8> file,
                        const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    ExternalAllocationScope scope(allocator);
    // JPEG TIFF의 libjpeg 할당도 같은 묶음으로 간다(12.11절).
    TiffAllocatorBinding tiffBinding;
    JpegAllocatorBinding jpegBinding;
    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    if (options == nullptr) {
        return Resource(out, ErrorCode::OutOfMemory);
    }

    const uint64 cap = limits.maxPixelBytes < static_cast<uint64>(NumericLimits<int64>::Max())
                           ? limits.maxPixelBytes
                           : static_cast<uint64>(NumericLimits<int64>::Max());
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, static_cast<tmsize_t>(cap));
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(options, static_cast<tmsize_t>(cap));
    TIFFOpenOptionsSetErrorHandlerExtR(options, TiffMessage, nullptr);
    TIFFOpenOptionsSetWarningHandlerExtR(options, TiffMessage, nullptr);
    TiffMemory memory{file, 0};
    TIFF* opened = TIFFClientOpenExt("memory", "rm", &memory, TiffRead, TiffWrite, TiffSeek,
                                     TiffClose, TiffSize, TiffMap, TiffUnmap, options);
    TIFFOpenOptionsFree(options);
    if (opened == nullptr) {
        return TiffLibraryFailure(scope, out);
    }

    TiffHandle handle(opened);
    TIFF* tiff = handle.Get();
    TiffShape shape;
    uint16 photometric = 0;
    uint16 compression = COMPRESSION_NONE;
    if (TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &shape.width) != 1 ||
        TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &shape.height) != 1 ||
        TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric) != 1) {
        return Invalid(out, 0);
    }

    TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &shape.bits);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &shape.samples);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &shape.sampleFormat);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_COMPRESSION, &compression);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &shape.planar);
    TIFFGetFieldDefaulted(tiff, TIFFTAG_ORIENTATION, &shape.orientation);

    // 형식 판정은 디코딩 전에 태그로 한다(12.10절 공통 규칙 2).
    if (compression == TiffCompressionPixarLog || compression == TiffCompressionSgiLog ||
        compression == TiffCompressionSgiLog24 || photometric == TiffPhotometricLogL ||
        photometric == TiffPhotometricLogLuv) {
        return Fail(out, AssetDiagnosticCode::LossySource, ErrorCode::Unsupported, 0);
    }

    // JPEG·옛 JPEG은 libtiff 코덱이 libjpeg-turbo로 푼다(12.11절 — UE Windows는 받지 않는다,
    // Rocks는 상위집합).
    if (TIFFIsCODECConfigured(compression) == 0) {
        return Unsupported(out, 0);
    }

    if (shape.sampleFormat == TiffSampleFormatUnspecified) {
        shape.sampleFormat = SAMPLEFORMAT_UINT;
    }

    if (shape.sampleFormat != SAMPLEFORMAT_UINT && shape.sampleFormat != SAMPLEFORMAT_IEEEFP) {
        return Unsupported(out, 0);
    }

    if (shape.width == 0 || shape.height == 0) {
        return Invalid(out, 0);
    }

    switch (photometric) {
    case PHOTOMETRIC_MINISWHITE:
        shape.minIsWhite = true;
        shape.color = TiffColor::Gray;
        break;
    case PHOTOMETRIC_MINISBLACK:
        shape.color = TiffColor::Gray;
        break;
    case PHOTOMETRIC_RGB:
        shape.color = TiffColor::Rgb;
        shape.baseSamples = 3;
        break;
    case PHOTOMETRIC_PALETTE:
        shape.color = TiffColor::Palette;
        break;
    case PHOTOMETRIC_YCBCR:
    case PHOTOMETRIC_CIELAB:
    case PHOTOMETRIC_ICCLAB:
    case PHOTOMETRIC_ITULAB: {
        // libtiff RGBA 변환으로 RGBA8(UE와 같은 경로, 요청 방향 TOPLEFT).
        char message[TiffRgbaMessageBytes] = {};
        if (TIFFRGBAImageOK(tiff, message) != 1) {
            return Unsupported(out, 0);
        }

        if (auto allocated =
                AllocateImage(limits, TextureSourceFormat::RGBA8, shape.width, shape.height, out);
            !allocated) {
            return allocated;
        }

        Array<uint32> raster(allocator);
        if (auto sized = raster.TryResize(static_cast<usize>(shape.width) * shape.height); !sized) {
            return Resource(out, sized.Error());
        }

        if (TIFFReadRGBAImageOriented(tiff, shape.width, shape.height, raster.Data(),
                                      ORIENTATION_TOPLEFT, 0) != 1) {
            return TiffLibraryFailure(scope, out);
        }

        // raster는 ABGR 묶음(R이 낮은 byte)이다 — byte로 R·G·B·A 순서를 쓴다.
        uint8* destination = out.image.pixels.Data();
        for (uint32 packed : raster) {
            destination[0] = static_cast<uint8>(TIFFGetR(packed));
            destination[1] = static_cast<uint8>(TIFFGetG(packed));
            destination[2] = static_cast<uint8>(TIFFGetB(packed));
            destination[3] = static_cast<uint8>(TIFFGetA(packed));
            destination += 4;
        }

        return TIFFNumberOfDirectories(tiff) > 1
                   ? Warn(out, AssetDiagnosticCode::IgnoredSourceImages,
                          static_cast<uint32>(TIFFNumberOfDirectories(tiff) - 1))
                   : Result<void>::Success();
    }
    default:
        return Unsupported(out, 0);
    }

    if (shape.samples < shape.baseSamples) {
        return Invalid(out, 0);
    }

    const uint16 extra = static_cast<uint16>(shape.samples - shape.baseSamples);
    shape.alpha = extra >= 1;
    const TextureSourceFormat format = TiffOutputFormat(shape);
    if (format == TextureSourceFormat::Invalid) {
        return Unsupported(out, 0);
    }

    TiffPalette palette;
    if (shape.color == TiffColor::Palette) {
        uint16* red = nullptr;
        uint16* green = nullptr;
        uint16* blue = nullptr;
        if (TIFFGetField(tiff, TIFFTAG_COLORMAP, &red, &green, &blue) != 1 || red == nullptr ||
            green == nullptr || blue == nullptr) {
            return Invalid(out, 0);
        }

        palette.red = red;
        palette.green = green;
        palette.blue = blue;
        palette.eightBit = true;
        const usize entries = usize{1} << shape.bits;
        for (usize index = 0; index < entries; ++index) {
            if (red[index] >= 256 || green[index] >= 256 || blue[index] >= 256) {
                palette.eightBit = false;
                break;
            }
        }
    }

    if (auto allocated = AllocateImage(limits, format, shape.width, shape.height, out);
        !allocated) {
        return allocated;
    }

    if (auto decoded = TiffDecodeDirect(allocator, tiff, shape, palette, limits, scope, out);
        !decoded) {
        return decoded;
    }

    if (TiffAppliesOrientation(shape)) {
        TiffApplyOrientation(shape.orientation, out);
    }

    if (extra > 1) {
        if (auto warned = Warn(out, AssetDiagnosticCode::IgnoredSourceChannels, extra - 1u);
            !warned) {
            return warned;
        }
    }

    const tdir_t directories = TIFFNumberOfDirectories(tiff);
    if (directories > 1) {
        return Warn(out, AssetDiagnosticCode::IgnoredSourceImages,
                    static_cast<uint32>(directories - 1));
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
