#include "ExternalBindings.h"

#include <ImageCodec/ImageEncode.h>
#include <Platform/Memory.h>
#include <tiffio.h>

// TIFF 쓰기다(Docs/Design/AssetSystem 12.10.1절 — libtiff 쓰기 API를 메모리 파일에). 값 조합은
// libtiff가 받는 대로 쓴다. 할당은 디코더와 같은 스레드별 묶음(libtiff·libjpeg — JPEG TIFF)이고
// 실패 표지로 OutOfMemory를 가른다. Rocks 쪽 할당(출력 파일·줄·tile 버퍼)은 묶음 밖이므로 그 오류를
// 따로 들고 와 libtiff의 뒤따른 실패보다 앞세운다.
namespace rocks::image {
namespace {

using detail::ExternalAllocationScope;

// libtiff가 읽고 쓰는 메모리 파일이다.
struct TiffWriteFile {
    explicit TiffWriteFile(Array<uint8>& bytes) noexcept : data(bytes) {
    }

    Array<uint8>& data;
    uint64 position = 0;
    // 출력 버퍼를 늘리지 못한 오류다(None이면 실패 없음).
    ErrorCode error = ErrorCode::None;
};

tmsize_t TiffWriteRead(thandle_t handle, void* buffer, tmsize_t size) noexcept {
    auto* file = static_cast<TiffWriteFile*>(handle);
    if (size <= 0 || file->position >= file->data.Size()) {
        return 0;
    }

    const uint64 left = file->data.Size() - file->position;
    const uint64 count = left < static_cast<uint64>(size) ? left : static_cast<uint64>(size);
    platform::MemoryCopy(buffer, file->data.Data() + file->position, static_cast<usize>(count));
    file->position += count;
    return static_cast<tmsize_t>(count);
}

tmsize_t TiffWriteWrite(thandle_t handle, void* buffer, tmsize_t size) noexcept {
    auto* file = static_cast<TiffWriteFile*>(handle);
    if (size < 0) {
        return 0;
    }

    const uint64 end = file->position + static_cast<uint64>(size);
    if (end > file->data.Size()) {
        if (auto resized = file->data.TryResize(static_cast<usize>(end)); !resized) {
            file->error = resized.Error();
            return 0;
        }
    }

    platform::MemoryCopy(file->data.Data() + file->position, buffer, static_cast<usize>(size));
    file->position = end;
    return size;
}

toff_t TiffWriteSeek(thandle_t handle, toff_t offset, int whence) noexcept {
    auto* file = static_cast<TiffWriteFile*>(handle);
    if (whence == SEEK_SET) {
        file->position = offset;
    } else if (whence == SEEK_CUR) {
        file->position += offset;
    } else {
        file->position = file->data.Size() + offset;
    }

    return file->position;
}

int TiffWriteClose(thandle_t) noexcept {
    return 0;
}

toff_t TiffWriteSize(thandle_t handle) noexcept {
    return static_cast<TiffWriteFile*>(handle)->data.Size();
}

int TiffWriteMap(thandle_t, void**, toff_t*) noexcept {
    return 0;
}

void TiffWriteUnmap(thandle_t, void*, toff_t) noexcept {
}

int TiffWriteMessage(TIFF*, void*, const char*, const char*, va_list) noexcept {
    return 1;
}

// 디렉터리 태그를 쓴다(값 조합은 검사하지 않는다). 기본 태그는 실패하면 쓰기 실패이고, 선택 태그
// (Predictor·ExtraSamples·색표·YCbCr subsampling·JPEG 색 모드)는 libtiff가 받지 않으면(예:
// PackBits의 Predictor) 쓰지 않고 계속한다.
bool TiffWriteFields(TIFF* tiff, const TiffWriteImage& image) noexcept {
    bool ok = TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, image.width) == 1 &&
              TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, image.height) == 1 &&
              TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, static_cast<int>(image.bits)) == 1 &&
              TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, static_cast<int>(image.samples)) == 1 &&
              TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, static_cast<int>(image.sampleFormat)) == 1 &&
              TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, static_cast<int>(image.photometric)) == 1 &&
              TIFFSetField(tiff, TIFFTAG_COMPRESSION, static_cast<int>(image.compression)) == 1 &&
              TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, static_cast<int>(image.planar)) == 1 &&
              TIFFSetField(tiff, TIFFTAG_ORIENTATION, static_cast<int>(image.orientation)) == 1;
    if (!ok) {
        return false;
    }

    if (image.predictor != tiff::PredictorNone) {
        (void)TIFFSetField(tiff, TIFFTAG_PREDICTOR, static_cast<int>(image.predictor));
    }

    if (image.writeExtraSamples) {
        (void)TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, static_cast<int>(image.extraSamples.Size()),
                           image.extraSamples.Data());
    }

    if (image.colormap != nullptr) {
        const usize entries = usize{1} << image.bits;
        (void)TIFFSetField(tiff, TIFFTAG_COLORMAP, image.colormap, image.colormap + entries,
                           image.colormap + 2 * entries);
    }

    if (image.ycbcr11) {
        (void)TIFFSetField(tiff, TIFFTAG_YCBCRSUBSAMPLING, 1, 1);
    }

    if (image.jpegColorModeRgb) {
        (void)TIFFSetField(tiff, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
    }

    return true;
}

// 한 IFD의 표본을 strip 행 또는 tile로 쓴다. 오류: 줄·tile 버퍼 할당 실패, libtiff가 거부한 쓰기
// InvalidArgument.
Result<void> TiffWritePixels(Allocator& allocator, TIFF* tiff, const TiffWriteImage& image,
                             uint32 rowSamples, usize rowBytes, uint32 planes) noexcept {
    const auto rejected = []() noexcept {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };
    if (image.tileWidth != 0) {
        if (TIFFSetField(tiff, TIFFTAG_TILEWIDTH, image.tileWidth) != 1 ||
            TIFFSetField(tiff, TIFFTAG_TILELENGTH, image.tileHeight) != 1) {
            return rejected();
        }

        const usize tileRowBytes =
            (static_cast<usize>(image.tileWidth) * rowSamples * image.bits + 7) / 8;
        Array<uint8> tile(allocator);
        if (auto resized = tile.TryResize(tileRowBytes * image.tileHeight); !resized) {
            return resized;
        }

        for (uint32 plane = 0; plane < planes; ++plane) {
            for (uint32 y = 0; y < image.height; y += image.tileHeight) {
                for (uint32 x = 0; x < image.width; x += image.tileWidth) {
                    platform::MemoryZero(tile.Data(), tile.Size());
                    const usize xByte = static_cast<usize>(x) * rowSamples * image.bits / 8;
                    const usize copy =
                        rowBytes - xByte < tileRowBytes ? rowBytes - xByte : tileRowBytes;
                    for (uint32 r = 0; r < image.tileHeight && y + r < image.height; ++r) {
                        const usize source =
                            (static_cast<usize>(plane) * image.height + y + r) * rowBytes;
                        platform::MemoryCopy(tile.Data() + r * tileRowBytes,
                                             image.raw.Data() + source + xByte, copy);
                    }

                    const uint32 index = TIFFComputeTile(tiff, x, y, 0, static_cast<uint16>(plane));
                    if (TIFFWriteEncodedTile(tiff, index, tile.Data(),
                                             static_cast<tmsize_t>(tile.Size())) < 0) {
                        return rejected();
                    }
                }
            }
        }

        return Result<void>::Success();
    }

    if (image.rowsPerStrip != 0 &&
        TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, image.rowsPerStrip) != 1) {
        return rejected();
    }

    Array<uint8> line(allocator);
    if (auto resized = line.TryResize(rowBytes); !resized) {
        return resized;
    }

    for (uint32 plane = 0; plane < planes; ++plane) {
        for (uint32 row = 0; row < image.height; ++row) {
            const usize source = (static_cast<usize>(plane) * image.height + row) * rowBytes;
            platform::MemoryCopy(line.Data(), image.raw.Data() + source, rowBytes);
            if (TIFFWriteScanline(tiff, line.Data(), row, static_cast<uint16>(plane)) != 1) {
                return rejected();
            }
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> EncodeTiff(Allocator& allocator, const TiffWriteImage& image,
                        Array<uint8>& out) noexcept {
    out.Clear();
    if (image.width == 0 || image.height == 0 || image.samples == 0 || image.bits == 0 ||
        image.pages == 0 || (image.tileWidth != 0 && image.tileHeight == 0)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool separate = image.planar == tiff::PlanarSeparate && image.samples > 1;
    const uint32 rowSamples = separate ? 1 : image.samples;
    const uint32 planes = separate ? image.samples : 1;
    const uint64 rowBytes = (static_cast<uint64>(image.width) * rowSamples * image.bits + 7) / 8;
    if (static_cast<uint64>(planes) * image.height * rowBytes > image.raw.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ExternalAllocationScope scope(allocator);
    detail::TiffAllocatorBinding tiffBinding;
    detail::JpegAllocatorBinding jpegBinding;
    TiffWriteFile file(out);
    // 오류는 할당 실패를 앞세운다: libtiff·libjpeg 할당(묶음 표지), 출력 버퍼, 그다음 error(줄·tile
    // 버퍼 할당 실패나 libtiff 거부 InvalidArgument). 할당 실패 뒤의 libtiff 실패는 거부가 아니다.
    const auto failed = [&](ErrorCode error) noexcept {
        out.Clear();
        if (scope.AllocationFailed()) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Failure(file.error != ErrorCode::None ? file.error : error);
    };
    TIFFOpenOptions* options = TIFFOpenOptionsAlloc();
    if (options == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    TIFFOpenOptionsSetErrorHandlerExtR(options, TiffWriteMessage, nullptr);
    TIFFOpenOptionsSetWarningHandlerExtR(options, TiffWriteMessage, nullptr);
    // libtiff 모드: w = 쓰기, l·b = byte 순서, 8 = BigTIFF.
    char mode[4] = {'w', image.byteOrder == TiffByteOrder::Big ? 'b' : 'l', '\0', '\0'};
    if (image.bigTiff) {
        mode[2] = '8';
    }

    TIFF* opened =
        TIFFClientOpenExt("memory", mode, &file, TiffWriteRead, TiffWriteWrite, TiffWriteSeek,
                          TiffWriteClose, TiffWriteSize, TiffWriteMap, TiffWriteUnmap, options);
    TIFFOpenOptionsFree(options);
    if (opened == nullptr) {
        return failed(ErrorCode::InvalidArgument);
    }

    ErrorCode error = ErrorCode::None;
    for (uint32 page = 0; error == ErrorCode::None && page < image.pages; ++page) {
        if (!TiffWriteFields(opened, image)) {
            error = ErrorCode::InvalidArgument;
        } else if (auto written = TiffWritePixels(allocator, opened, image, rowSamples,
                                                  static_cast<usize>(rowBytes), planes);
                   !written) {
            error = written.Error();
        } else if (image.pages > 1 && TIFFWriteDirectory(opened) != 1) {
            error = ErrorCode::InvalidArgument;
        }
    }

    // TIFFClose가 마지막 디렉터리를 쓴다(실패는 쓰기 콜백의 표지로 안다).
    TIFFClose(opened);
    if (error != ErrorCode::None || file.error != ErrorCode::None || scope.AllocationFailed()) {
        return failed(error != ErrorCode::None ? error : ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::image
