#include "ExternalBindings.h"
#include "ImageCodecInternal.h"

// JPEG 디코더다(Docs/Design/AssetSystem 12.11절 — libjpeg-turbo libjpeg API, UE JpegImageWrapper와
// 같은 받는 범위에 비표준 sampling factor를 더한 상위집합). 머리를 먼저 읽어 정밀도·성분·색 공간을
// 디코딩 전에 판정하고, 회색은 G8, 색은 RGBA8(알파 255)로 푼다. IDCT는 JDCT_ISLOW(UE는 FASTDCT —
// 영구 원본 정확도), 손상 경고는 InvalidSource(UE TurboJPEG도 실패), progressive scan은
// JpegMaxScans까지만 받는다. longjmp는 rocksjpeg.c(C) 안에 갇혀 있다.
namespace rocks::image::detail {
namespace {

// TurboJPEG TJPARAM_SCANLIMIT·djpeg -maxscans와 같은 방식의 progressive scan 상한이다.
constexpr unsigned JpegMaxScans = 1000;
// 8비트 API가 받는 최대 정밀도다(lossless 2–8, lossy 8).
constexpr int JpegMaxPrecision = 8;
constexpr int JpegGrayComponents = 1;
constexpr int JpegColorComponents = 3;

Result<void> JpegFailure(RocksJpegResult result, const ExternalAllocationScope& scope,
                         ImageDecodeResult& out) noexcept {
    switch (result) {
    case ROCKS_JPEG_OK:
        return Result<void>::Success();
    case ROCKS_JPEG_SCAN_LIMIT:
        return OverLimit(out, 0);
    case ROCKS_JPEG_WARNING:
        return Invalid(out, 0);
    case ROCKS_JPEG_ERROR:
        break;
    }

    // 오류 종료는 손상·받지 않는 스트림 또는 할당 실패다 — 할당 실패 표지로 가른다(12.10절 공통
    // 규칙 2).
    return scope.AllocationFailed() ? Resource(out, ErrorCode::OutOfMemory) : Invalid(out, 0);
}

} // namespace

Result<void> DecodeJpeg(Allocator& allocator, Span<const uint8> file,
                        const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    ExternalAllocationScope scope(allocator);
    JpegAllocatorBinding binding;
    RocksJpegInfo info{};
    if (const RocksJpegResult read = RocksJpegReadInfo(file.Data(), file.Size(), &info);
        read != ROCKS_JPEG_OK) {
        return JpegFailure(read, scope, out);
    }

    // 판정은 디코딩 전에 한다: 정밀도 9–16(lossless 포함)은 8비트 API 밖(UE도 실패), 성분 1은 회색,
    // 성분 3은 YCbCr·RGB(lossless는 색 변환이 없어 RGB만 — jdcolor.c, UE도 실패), CMYK·YCCK·그 밖은
    // 받지 않는다.
    if (info.precision > JpegMaxPrecision) {
        return Unsupported(out, 0);
    }

    bool gray = false;
    if (info.components == JpegGrayComponents) {
        gray = true;
    } else if (info.components == JpegColorComponents) {
        const bool rgb = info.colorSpace == ROCKS_JPEG_COLOR_RGB;
        const bool ycbcr = info.colorSpace == ROCKS_JPEG_COLOR_YCBCR;
        if (!(rgb || (ycbcr && info.lossless == 0))) {
            return Unsupported(out, 0);
        }
    } else {
        return Unsupported(out, 0);
    }

    const TextureSourceFormat format = gray ? TextureSourceFormat::G8 : TextureSourceFormat::RGBA8;
    if (auto allocated = AllocateImage(limits, format, info.width, info.height, out); !allocated) {
        return allocated;
    }

    const usize rowBytes = static_cast<usize>(info.width) * TextureSourceTexelBytes(format);
    const RocksJpegResult decoded = RocksJpegDecode(
        file.Data(), file.Size(), gray ? 1 : 0, JpegMaxScans, out.image.pixels.Data(), rowBytes);
    return JpegFailure(decoded, scope, out);
}

} // namespace rocks::image::detail
