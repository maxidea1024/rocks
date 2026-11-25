#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <rocksjpeg.h>
#include <tif_rocks.h>
#include <tiffio.h>

// TIFF 디코더 시험이다(Docs/Design/AssetSystem 12.10절 A5f — libtiff, UE TiffImageWrapper와 같은
// 받는 범위). 파일은 libtiff 쓰기 API로 메모리에 만들고(압축·Predictor·planar·tile·BigTIFF·여러
// IFD), 받지 않는 태그 조합은 손으로 만든 byte로 만든다. 결과 형식·값(UE 수치 변환·MinIsWhite·색표
// checkcmap·방향 뒤집기), 알파(ExtraSamples 무관), 경고, LossySource·UnsupportedSourceFormat,
// 손상·절단, 할당 실패, 스레드별 Allocator 묶음을 본다.
using namespace rocks;
using namespace rocks::image;
namespace tiff_decode_tests_file {

// tiff.h가 전역에 deprecated uint8·uint16·uint32·uint64를 두므로 Rocks 정수 타입을 이 이름 공간에서
// 먼저 찾게 한다(전역 이름을 쓰면 -Wdeprecated-declarations).
using rocks::int64;
using rocks::uint16;
using rocks::uint32;
using rocks::uint64;
using rocks::uint8;

namespace {

// ---- 시험 쪽 libtiff 할당(tif_rocks.h 묶음) ----

constexpr usize TestBlockHeader = 16;

void* TestTiffAllocate(size_t bytes) noexcept {
    void* block = platform::AlignedAllocate(bytes + TestBlockHeader, TestBlockHeader);
    if (block == nullptr) {
        return nullptr;
    }

    *static_cast<usize*>(block) = bytes;
    return static_cast<uint8*>(block) + TestBlockHeader;
}

void TestTiffRelease(void* address) noexcept {
    if (address != nullptr) {
        platform::AlignedFree(static_cast<uint8*>(address) - TestBlockHeader);
    }
}

void* TestTiffReallocate(void* address, size_t bytes) noexcept {
    void* resized = TestTiffAllocate(bytes);
    if (resized != nullptr && address != nullptr) {
        const usize kept =
            *reinterpret_cast<usize*>(static_cast<uint8*>(address) - TestBlockHeader);
        platform::MemoryCopy(resized, address, kept < bytes ? kept : bytes);
        TestTiffRelease(address);
    }

    return resized;
}

const RocksTiffAllocator testTiffAllocator = {TestTiffAllocate, TestTiffReallocate,
                                              TestTiffRelease};
const RocksJpegAllocator testJpegAllocator = {TestTiffAllocate, TestTiffRelease};

// libtiff와 그 JPEG 코덱(libjpeg-turbo) 할당을 함께 묶는다(JPEG TIFF 쓰기).
class TestTiffBinding {
public:
    TestTiffBinding() noexcept
        : previous_(RocksTiffBindAllocator(&testTiffAllocator)),
          previousJpeg_(RocksJpegBindAllocator(&testJpegAllocator)) {
    }

    ~TestTiffBinding() noexcept {
        RocksJpegBindAllocator(previousJpeg_);
        RocksTiffBindAllocator(previous_);
    }

private:
    const RocksTiffAllocator* previous_;
    const RocksJpegAllocator* previousJpeg_;
};

// ---- 메모리 쓰기 ----

struct MemoryFile {
    explicit MemoryFile(Allocator& allocator) noexcept : data(allocator) {
    }

    Array<uint8> data;
    uint64 position = 0;
};

tmsize_t MemoryRead(thandle_t handle, void* buffer, tmsize_t size) noexcept {
    auto* file = static_cast<MemoryFile*>(handle);
    if (size <= 0 || file->position >= file->data.Size()) {
        return 0;
    }

    const uint64 count = file->data.Size() - file->position < static_cast<uint64>(size)
                             ? file->data.Size() - file->position
                             : static_cast<uint64>(size);
    platform::MemoryCopy(buffer, file->data.Data() + file->position, static_cast<usize>(count));
    file->position += count;
    return static_cast<tmsize_t>(count);
}

tmsize_t MemoryWrite(thandle_t handle, void* buffer, tmsize_t size) noexcept {
    auto* file = static_cast<MemoryFile*>(handle);
    const uint64 end = file->position + static_cast<uint64>(size);
    if (end > file->data.Size() && !file->data.TryResize(static_cast<usize>(end))) {
        return 0;
    }

    platform::MemoryCopy(file->data.Data() + file->position, buffer, static_cast<usize>(size));
    file->position = end;
    return size;
}

toff_t MemorySeek(thandle_t handle, toff_t offset, int whence) noexcept {
    auto* file = static_cast<MemoryFile*>(handle);
    if (whence == SEEK_SET) {
        file->position = offset;
    } else if (whence == SEEK_CUR) {
        file->position += offset;
    } else {
        file->position = file->data.Size() + offset;
    }

    return file->position;
}

int MemoryClose(thandle_t) noexcept {
    return 0;
}

toff_t MemorySize(thandle_t handle) noexcept {
    return static_cast<MemoryFile*>(handle)->data.Size();
}

int MemoryMap(thandle_t, void**, toff_t*) noexcept {
    return 0;
}

void MemoryUnmap(thandle_t, void*, toff_t) noexcept {
}

// libtiff로 쓸 이미지다. raw는 TIFF 저장 순서의 표본 행(contig: 행마다 모든 표본, separate:
// 평면마다 모든 행)이고 한 행은 ceil(width × 행 표본 × bits / 8) byte다.
struct TiffImage {
    uint32 width = 1;
    uint32 height = 1;
    uint16 bits = 8;
    uint16 samples = 1;
    uint16 sampleFormat = SAMPLEFORMAT_UINT;
    uint16 photometric = PHOTOMETRIC_MINISBLACK;
    uint16 compression = COMPRESSION_NONE;
    uint16 predictor = 1;
    uint16 planar = PLANARCONFIG_CONTIG;
    uint16 orientation = ORIENTATION_TOPLEFT;
    uint32 rowsPerStrip = 0;
    uint32 tileWidth = 0;
    uint32 tileHeight = 0;
    Span<const uint16> extraSamples;
    bool writeExtraSamples = false;
    const uint16* colormap = nullptr;
    bool ycbcr11 = false;
    bool jpegColorModeRgb = false;
    Span<const uint8> raw;
    uint32 pages = 1;
    const char* mode = "w";
};

Array<uint8> WriteTiff(Allocator& allocator, const TiffImage& image) noexcept {
    TestTiffBinding binding;
    MemoryFile file(allocator);
    TIFF* tiff = TIFFClientOpenExt("test", image.mode, &file, MemoryRead, MemoryWrite, MemorySeek,
                                   MemoryClose, MemorySize, MemoryMap, MemoryUnmap, nullptr);
    CHECK(tiff != nullptr);
    if (tiff == nullptr) {
        return Move(file.data);
    }

    const bool separate = image.planar == PLANARCONFIG_SEPARATE && image.samples > 1;
    const uint32 rowSamples = separate ? 1 : image.samples;
    const usize rowBytes = (static_cast<usize>(image.width) * rowSamples * image.bits + 7) / 8;
    for (uint32 page = 0; page < image.pages; ++page) {
        TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, image.width);
        TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, image.height);
        TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, static_cast<int>(image.bits));
        TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, static_cast<int>(image.samples));
        TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, static_cast<int>(image.sampleFormat));
        TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, static_cast<int>(image.photometric));
        TIFFSetField(tiff, TIFFTAG_COMPRESSION, static_cast<int>(image.compression));
        TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, static_cast<int>(image.planar));
        TIFFSetField(tiff, TIFFTAG_ORIENTATION, static_cast<int>(image.orientation));
        if (image.predictor != 1) {
            TIFFSetField(tiff, TIFFTAG_PREDICTOR, static_cast<int>(image.predictor));
        }

        if (image.writeExtraSamples) {
            TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, static_cast<int>(image.extraSamples.Size()),
                         image.extraSamples.Data());
        }

        if (image.colormap != nullptr) {
            const usize entries = usize{1} << image.bits;
            TIFFSetField(tiff, TIFFTAG_COLORMAP, image.colormap, image.colormap + entries,
                         image.colormap + 2 * entries);
        }

        if (image.ycbcr11) {
            TIFFSetField(tiff, TIFFTAG_YCBCRSUBSAMPLING, 1, 1);
        }

        if (image.jpegColorModeRgb) {
            // YCbCr JPEG: libtiff가 RGB 입력을 YCbCr 4:2:0으로 바꿔 쓴다.
            TIFFSetField(tiff, TIFFTAG_JPEGCOLORMODE, JPEGCOLORMODE_RGB);
        }

        const uint32 planes = separate ? image.samples : 1;
        if (image.tileWidth != 0) {
            TIFFSetField(tiff, TIFFTAG_TILEWIDTH, image.tileWidth);
            TIFFSetField(tiff, TIFFTAG_TILELENGTH, image.tileHeight);
            const usize tileRowBytes =
                (static_cast<usize>(image.tileWidth) * rowSamples * image.bits + 7) / 8;
            Array<uint8> tile(allocator);
            CHECK(tile.TryResize(tileRowBytes * image.tileHeight));
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

                        const uint32 index =
                            TIFFComputeTile(tiff, x, y, 0, static_cast<uint16>(plane));
                        CHECK(TIFFWriteEncodedTile(tiff, index, tile.Data(),
                                                   static_cast<tmsize_t>(tile.Size())) >= 0);
                    }
                }
            }
        } else {
            if (image.rowsPerStrip != 0) {
                TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, image.rowsPerStrip);
            }

            Array<uint8> line(allocator);
            CHECK(line.TryResize(rowBytes));
            for (uint32 plane = 0; plane < planes; ++plane) {
                for (uint32 row = 0; row < image.height; ++row) {
                    const usize source =
                        (static_cast<usize>(plane) * image.height + row) * rowBytes;
                    platform::MemoryCopy(line.Data(), image.raw.Data() + source, rowBytes);
                    CHECK(TIFFWriteScanline(tiff, line.Data(), row, static_cast<uint16>(plane)) ==
                          1);
                }
            }
        }

        if (image.pages > 1) {
            CHECK(TIFFWriteDirectory(tiff) == 1);
        }
    }

    TIFFClose(tiff);
    return Move(file.data);
}

// ---- 손으로 만드는 고전 TIFF(little-endian) ----

struct HandTag {
    uint16 tag;
    uint16 type;
    uint32 count;
    uint32 value;
};

constexpr uint16 TypeShort = 3;
constexpr uint16 TypeLong = 4;

// 머리 8 byte, data(offset 8), IFD 순서다. tags는 태그 번호 오름차순이어야 한다.
Array<uint8> HandTiff(Allocator& allocator, Span<const HandTag> tags, Span<const uint8> data,
                      uint16 declaredCount = 0) noexcept {
    Array<uint8> file(allocator);
    const auto u8 = [&file](uint32 value) noexcept {
        CHECK(file.TryPushBack(static_cast<uint8>(value)));
    };
    const auto le16 = [&u8](uint32 value) noexcept {
        u8(value);
        u8(value >> 8);
    };
    const auto le32 = [&le16](uint32 value) noexcept {
        le16(value);
        le16(value >> 16);
    };
    u8('I');
    u8('I');
    le16(42);
    le32(static_cast<uint32>(8 + data.Size()));
    CHECK(file.TryAppend(data));
    le16(declaredCount != 0 ? declaredCount : static_cast<uint16>(tags.Size()));
    for (const HandTag& tag : tags) {
        le16(tag.tag);
        le16(tag.type);
        le32(tag.count);
        le32(tag.value);
    }

    le32(0);
    return file;
}

// 8비트 회색 2×1 무압축의 기본 태그에 바꿀 태그를 얹는다.
Array<uint8> HandGray(Allocator& allocator, uint16 compression, uint16 photometric,
                      uint16 sampleFormat = 1, uint16 samples = 1) noexcept {
    const uint8 pixels[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    const HandTag tags[] = {{TIFFTAG_IMAGEWIDTH, TypeLong, 1, 2},
                            {TIFFTAG_IMAGELENGTH, TypeLong, 1, 1},
                            {TIFFTAG_BITSPERSAMPLE, TypeShort, 1, 8},
                            {TIFFTAG_COMPRESSION, TypeShort, 1, compression},
                            {TIFFTAG_PHOTOMETRIC, TypeShort, 1, photometric},
                            {TIFFTAG_STRIPOFFSETS, TypeLong, 1, 8},
                            {TIFFTAG_SAMPLESPERPIXEL, TypeShort, 1, samples},
                            {TIFFTAG_ROWSPERSTRIP, TypeLong, 1, 1},
                            {TIFFTAG_STRIPBYTECOUNTS, TypeLong, 1, 2u * samples},
                            {TIFFTAG_SAMPLEFORMAT, TypeShort, 1, sampleFormat}};
    return HandTiff(allocator, tags, pixels);
}

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, ImageFileFormat::Unknown, {}, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

bool Pixels(const Decoded& decoded, TextureSourceFormat format,
            Span<const uint8> expected) noexcept {
    const DecodedImage& image = decoded.result.image;
    return decoded.ok && image.format == format && image.pixels.Size() == expected.Size() &&
           platform::MemoryEquals(image.pixels.Data(), expected.Data(), expected.Size());
}

bool HasWarning(const Decoded& decoded, AssetDiagnosticCode code, uint32 detail) noexcept {
    for (const ImageDecodeWarning& warning : decoded.result.warnings) {
        if (warning.code == code && warning.detail == detail) {
            return true;
        }
    }

    return false;
}

void PutLe16(uint8* data, uint16 value) noexcept {
    data[0] = static_cast<uint8>(value);
    data[1] = static_cast<uint8>(value >> 8);
}

void PutLe32(uint8* data, uint32 value) noexcept {
    PutLe16(data, static_cast<uint16>(value));
    PutLe16(data + 2, static_cast<uint16>(value >> 16));
}

uint16 GetLe16(const uint8* data) noexcept {
    return static_cast<uint16>(data[0] | (data[1] << 8));
}

float GetFloat(const uint8* data) noexcept {
    return platform::BitCast<float>(static_cast<uint32>(GetLe16(data)) |
                                    (static_cast<uint32>(GetLe16(data + 2)) << 16));
}

} // namespace

// 회색: 1·2·4·8·16·32·64비트 정수(UE 수치 변환), 16·32·64비트 float, 회색+알파, MinIsWhite.
TEST(TiffDecode_Gray) {
    SystemAllocator allocator;
    TiffImage image;
    image.width = 3;
    const uint8 bilevel[] = {0xA0};
    image.bits = 1;
    image.raw = bilevel;
    const uint8 bilevelOut[] = {255, 0, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G8,
                 bilevelOut));
    const uint8 two[] = {0x1B};
    image.width = 4;
    image.bits = 2;
    image.raw = two;
    const uint8 twoOut[] = {0, 85, 170, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G8,
                 twoOut));
    const uint8 four[] = {0x0F, 0x80};
    image.bits = 4;
    image.raw = four;
    const uint8 fourOut[] = {0, 255, 136, 0};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G8,
                 fourOut));

    // 16비트는 그대로, 32·64비트는 v / max × 65535 + 0.5 내림(UE ConvertToWriteFormat).
    uint8 g16[4] = {};
    PutLe16(g16, 0x1234);
    PutLe16(g16 + 2, 0xFFFF);
    image = TiffImage{};
    image.width = 2;
    image.bits = 16;
    image.raw = g16;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G16,
                 g16));
    uint8 g32[12] = {};
    PutLe32(g32, 0x80000000u);
    PutLe32(g32 + 4, NumericLimits<uint32>::Max());
    PutLe32(g32 + 8, 1);
    image.width = 3;
    image.bits = 32;
    image.raw = g32;
    uint8 g32Out[6] = {};
    PutLe16(g32Out, 32768);
    PutLe16(g32Out + 2, 65535);
    PutLe16(g32Out + 4, 0);
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G16,
                 g32Out));
    uint8 g64[16] = {};
    PutLe32(g64 + 4, 0x80000000u);
    PutLe32(g64 + 8, NumericLimits<uint32>::Max());
    PutLe32(g64 + 12, NumericLimits<uint32>::Max());
    image.width = 2;
    image.bits = 64;
    image.raw = g64;
    uint8 g64Out[4] = {};
    PutLe16(g64Out, 32768);
    PutLe16(g64Out + 2, 65535);
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G16,
                 g64Out));

    // float: 16 → R16F, 32 → R32F, 64 → R32F(자르지 않는다 — 2.5 보존, UE는 [0, 1]로 자른다).
    uint8 half[2] = {};
    PutLe16(half, Float16::FromFloat(0.5f).bits);
    image = TiffImage{};
    image.bits = 16;
    image.sampleFormat = SAMPLEFORMAT_IEEEFP;
    image.raw = half;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::R16F,
                 half));
    const double wide = 2.5;
    uint8 f64[8] = {};
    platform::MemoryCopy(f64, &wide, sizeof(wide));
    image.bits = 64;
    image.raw = f64;
    Decoded decoded = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::R32F &&
          GetFloat(decoded.result.image.pixels.Data()) == 2.5f);

    // 회색+알파: 8 → GA8, 16 → GA16, float32 → RGBA32F(g, g, g, a).
    const uint8 ga8[] = {10, 200, 30, 40};
    image = TiffImage{};
    image.width = 2;
    image.samples = 2;
    image.raw = ga8;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::GA8,
                 ga8));
    float gaFloat[2] = {0.25f, 0.75f};
    image.width = 1;
    image.bits = 32;
    image.sampleFormat = SAMPLEFORMAT_IEEEFP;
    image.raw = Span<const uint8>(reinterpret_cast<const uint8*>(gaFloat), sizeof(gaFloat));
    decoded = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA32F);
    if (decoded.ok) {
        const uint8* p = decoded.result.image.pixels.Data();
        CHECK(GetFloat(p) == 0.25f && GetFloat(p + 4) == 0.25f && GetFloat(p + 8) == 0.25f &&
              GetFloat(p + 12) == 0.75f);
    }

    // MinIsWhite: 8비트 255 − v, 16비트 65535 − v, float 1 − v(알파는 뒤집지 않는다).
    const uint8 white8[] = {0, 55};
    image = TiffImage{};
    image.width = 2;
    image.photometric = PHOTOMETRIC_MINISWHITE;
    image.raw = white8;
    const uint8 white8Out[] = {255, 200};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G8,
                 white8Out));
    uint8 white16[2] = {};
    PutLe16(white16, 5);
    image.width = 1;
    image.bits = 16;
    image.raw = white16;
    uint8 white16Out[2] = {};
    PutLe16(white16Out, 65530);
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G16,
                 white16Out));
    const float whiteFloat = 0.25f;
    image.bits = 32;
    image.sampleFormat = SAMPLEFORMAT_IEEEFP;
    image.raw = Span<const uint8>(reinterpret_cast<const uint8*>(&whiteFloat), sizeof(whiteFloat));
    decoded = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(decoded.ok && GetFloat(decoded.result.image.pixels.Data()) == 0.75f);
}

// RGB·팔레트·알파 규칙과 방향 뒤집기.
TEST(TiffDecode_ColorPaletteAlpha) {
    SystemAllocator allocator;
    const uint8 rgb[] = {1, 2, 3, 4, 5, 6};
    TiffImage image;
    image.width = 2;
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.raw = rgb;
    const uint8 rgbOut[] = {1, 2, 3, 255, 4, 5, 6, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 rgbOut));

    // RGB 2비트: 표본마다 비트 복제 확장. 3비트는 받지 않는다.
    const uint8 rgb2[] = {0x1B, 0x00};
    image.width = 1;
    image.bits = 2;
    image.raw = rgb2;
    const uint8 rgb2Out[] = {0, 85, 170, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 rgb2Out));
    image.bits = 3;
    CHECK(Fails(Decode(allocator, WriteTiff(allocator, image).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));

    // 알파: ExtraSamples 값(0·1·2)·태그 없음 모두 넷째 표본을 그대로 알파로 쓴다. 다섯째는 경고와
    // 함께 버린다.
    const uint8 rgba[] = {100, 50, 25, 128};
    const uint8 rgbaOut[] = {100, 50, 25, 128};
    const uint16 extraValues[] = {EXTRASAMPLE_UNSPECIFIED, EXTRASAMPLE_ASSOCALPHA,
                                  EXTRASAMPLE_UNASSALPHA};
    for (uint16 extra : extraValues) {
        image = TiffImage{};
        image.samples = 4;
        image.photometric = PHOTOMETRIC_RGB;
        image.extraSamples = Span<const uint16>(&extra, 1);
        image.writeExtraSamples = true;
        image.raw = rgba;
        CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()),
                     TextureSourceFormat::RGBA8, rgbaOut));
    }

    // ExtraSamples 태그 없이 표본 4개(DEFAULT_EXTRASAMPLE_AS_ALPHA — UE와 같다).
    image = TiffImage{};
    image.samples = 4;
    image.photometric = PHOTOMETRIC_RGB;
    image.raw = rgba;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 rgbaOut));

    // 한 변 한도는 CapacityExceeded다.
    ImageDecodeLimits tight;
    tight.maxDimension = 1;
    image.width = 2;
    const uint8 twoTexels[] = {100, 50, 25, 128, 100, 50, 25, 128};
    image.raw = twoTexels;
    ImageDecodeResult limited(allocator);
    CHECK(!DecodeImage(allocator, WriteTiff(allocator, image).View(), ImageFileFormat::Tiff, tight,
                       limited));
    CHECK(limited.failure.code == AssetDiagnosticCode::InvalidSource &&
          limited.failure.error == ErrorCode::CapacityExceeded);

    const uint8 rgbaa[] = {100, 50, 25, 128, 77};
    const uint16 twoExtra[] = {EXTRASAMPLE_UNASSALPHA, EXTRASAMPLE_UNSPECIFIED};
    image = TiffImage{};
    image.photometric = PHOTOMETRIC_RGB;
    image.writeExtraSamples = true;
    image.samples = 5;
    image.extraSamples = twoExtra;
    image.raw = rgbaa;
    Decoded decoded = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(Pixels(decoded, TextureSourceFormat::RGBA8, rgbaOut));
    CHECK(HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceChannels, 1));

    // RGB 16·32비트 정수 → RGBA16, 16비트 float → RGBA16F.
    uint8 rgb32[12] = {};
    PutLe32(rgb32, NumericLimits<uint32>::Max());
    image = TiffImage{};
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.bits = 32;
    image.raw = rgb32;
    uint8 rgb32Out[8] = {};
    PutLe16(rgb32Out, 65535);
    PutLe16(rgb32Out + 6, 65535);
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA16,
                 rgb32Out));
    uint8 rgbHalf[6] = {};
    PutLe16(rgbHalf, Float16::FromFloat(1.0f).bits);
    image.bits = 16;
    image.sampleFormat = SAMPLEFORMAT_IEEEFP;
    image.raw = rgbHalf;
    decoded = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(decoded.ok && decoded.result.image.format == TextureSourceFormat::RGBA16F &&
          GetLe16(decoded.result.image.pixels.Data() + 6) == Float16::FromFloat(1.0f).bits);

    // 팔레트: 16비트 색표는 >> 8, 모든 값이 256 미만이면 8비트 색표로 그대로(libtiff checkcmap).
    uint16 colormap[3 * 4] = {};
    colormap[1] = 0x1200;
    colormap[4 + 1] = 0x3400;
    colormap[8 + 1] = 0xFF00;
    const uint8 indices[] = {0x40};
    image = TiffImage{};
    image.width = 2;
    image.bits = 2;
    image.photometric = PHOTOMETRIC_PALETTE;
    image.colormap = colormap;
    image.raw = indices;
    const uint8 paletteOut[] = {0x12, 0x34, 0xFF, 255, 0, 0, 0, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 paletteOut));
    uint16 smallMap[3 * 4] = {};
    smallMap[1] = 0x12;
    smallMap[4 + 1] = 0x34;
    smallMap[8 + 1] = 0xFF;
    image.colormap = smallMap;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 paletteOut));

    // 방향: UE RGBA 경로 형식(8비트 RGB·팔레트·8비트 회색+알파)만 뒤집는다 — BOTLEFT는 상하.
    const uint8 twoRows[] = {1, 2, 3, 4, 5, 6};
    image = TiffImage{};
    image.height = 2;
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.orientation = ORIENTATION_BOTLEFT;
    image.raw = twoRows;
    const uint8 flipped[] = {4, 5, 6, 255, 1, 2, 3, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 flipped));
    uint8 rows16[12] = {};
    PutLe16(rows16, 1);
    PutLe16(rows16 + 6, 2);
    image.bits = 16;
    image.raw = rows16;
    decoded = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(decoded.ok && GetLe16(decoded.result.image.pixels.Data()) == 1);
    const uint8 grayRows[] = {9, 8};
    image = TiffImage{};
    image.height = 2;
    image.orientation = ORIENTATION_BOTLEFT;
    image.raw = grayRows;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::G8,
                 grayRows));
    // 팔레트 TOPRIGHT는 좌우.
    image = TiffImage{};
    image.width = 2;
    image.bits = 2;
    image.photometric = PHOTOMETRIC_PALETTE;
    image.colormap = colormap;
    image.orientation = ORIENTATION_TOPRIGHT;
    image.raw = indices;
    const uint8 mirrored[] = {0, 0, 0, 255, 0x12, 0x34, 0xFF, 255};
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, image).View()), TextureSourceFormat::RGBA8,
                 mirrored));
}

// 압축·Predictor·planar·tile·strip 높이·바이트 순서·BigTIFF·여러 IFD·RGBA 경로.
TEST(TiffDecode_LayoutAndCodecs) {
    SystemAllocator allocator;
    constexpr uint32 Width = 20;
    constexpr uint32 Height = 7;
    uint8 rgb[Width * Height * 3] = {};
    for (uint32 index = 0; index < Width * Height * 3; ++index) {
        rgb[index] = static_cast<uint8>((index * 37) & 0xFF);
    }

    TiffImage image;
    image.width = Width;
    image.height = Height;
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.raw = rgb;
    const Decoded reference = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(reference.ok);
    const auto same = [&reference](const Decoded& decoded) noexcept {
        return decoded.ok &&
               decoded.result.image.pixels.Size() == reference.result.image.pixels.Size() &&
               platform::MemoryEquals(decoded.result.image.pixels.Data(),
                                      reference.result.image.pixels.Data(),
                                      reference.result.image.pixels.Size());
    };

    const uint16 codecs[] = {COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE, COMPRESSION_PACKBITS};
    for (uint16 codec : codecs) {
        image.compression = codec;
        CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
        image.predictor = PREDICTOR_HORIZONTAL;
        CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
        image.predictor = 1;
    }

    // 짧은 마지막 strip(RowsPerStrip 3, 높이 7), big-endian, BigTIFF.
    image.compression = COMPRESSION_LZW;
    image.rowsPerStrip = 3;
    CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
    image.mode = "wb";
    CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
    image.mode = "w8";
    CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
    image.mode = "w";
    image.rowsPerStrip = 0;

    // planar 2(평면마다 모든 행): 같은 표본을 평면 순서로 다시 놓는다.
    uint8 planes[Width * Height * 3] = {};
    for (uint32 texel = 0; texel < Width * Height; ++texel) {
        for (uint32 c = 0; c < 3; ++c) {
            planes[c * Width * Height + texel] = rgb[texel * 3 + c];
        }
    }

    image.planar = PLANARCONFIG_SEPARATE;
    image.raw = planes;
    CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
    // tile 16×16(가장자리 부분 tile), planar + tile.
    image.tileWidth = 16;
    image.tileHeight = 16;
    CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));
    image.planar = PLANARCONFIG_CONTIG;
    image.raw = rgb;
    CHECK(same(Decode(allocator, WriteTiff(allocator, image).View())));

    // 16비트 big-endian + Predictor 2, 32비트 float + Predictor 3(Deflate).
    uint8 gray16[Width * 2] = {};
    for (uint32 x = 0; x < Width; ++x) {
        PutLe16(gray16 + x * 2, static_cast<uint16>(x * 3000));
    }

    TiffImage wide;
    wide.width = Width;
    wide.bits = 16;
    wide.compression = COMPRESSION_LZW;
    wide.predictor = PREDICTOR_HORIZONTAL;
    wide.mode = "wb";
    wide.raw = gray16;
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, wide).View()), TextureSourceFormat::G16,
                 gray16));
    float floats[Width] = {};
    for (uint32 x = 0; x < Width; ++x) {
        floats[x] = static_cast<float>(x) * 0.125f - 1.0f;
    }

    wide.bits = 32;
    wide.sampleFormat = SAMPLEFORMAT_IEEEFP;
    wide.compression = COMPRESSION_ADOBE_DEFLATE;
    wide.predictor = PREDICTOR_FLOATINGPOINT;
    wide.mode = "w";
    wide.raw = Span<const uint8>(reinterpret_cast<const uint8*>(floats), sizeof(floats));
    CHECK(Pixels(Decode(allocator, WriteTiff(allocator, wide).View()), TextureSourceFormat::R32F,
                 Span<const uint8>(reinterpret_cast<const uint8*>(floats), sizeof(floats))));

    // CCITT 그룹 3·4(1비트 MinIsWhite).
    const uint8 bilevel[] = {0xF0, 0x0F};
    TiffImage fax;
    fax.width = 16;
    fax.bits = 1;
    fax.photometric = PHOTOMETRIC_MINISWHITE;
    fax.raw = bilevel;
    const Decoded faxReference = Decode(allocator, WriteTiff(allocator, fax).View());
    CHECK(faxReference.ok);
    const uint16 faxCodecs[] = {COMPRESSION_CCITTFAX3, COMPRESSION_CCITTFAX4};
    for (uint16 codec : faxCodecs) {
        fax.compression = codec;
        const Decoded decoded = Decode(allocator, WriteTiff(allocator, fax).View());
        CHECK(decoded.ok && platform::MemoryEquals(decoded.result.image.pixels.Data(),
                                                   faxReference.result.image.pixels.Data(), 16));
    }

    // 여러 IFD: 첫 이미지만, IgnoredSourceImages(남은 수).
    image = TiffImage{};
    image.raw = rgb;
    image.pages = 3;
    const Decoded pages = Decode(allocator, WriteTiff(allocator, image).View());
    CHECK(pages.ok && HasWarning(pages, AssetDiagnosticCode::IgnoredSourceImages, 2));

    // YCbCr(무압축, subsampling 1×1)은 libtiff RGBA 경로 — 중립 색차는 회색이다.
    const uint8 ycbcr[] = {100, 128, 128};
    TiffImage ycc;
    ycc.samples = 3;
    ycc.photometric = PHOTOMETRIC_YCBCR;
    ycc.ycbcr11 = true;
    ycc.raw = ycbcr;
    const Decoded converted = Decode(allocator, WriteTiff(allocator, ycc).View());
    CHECK(converted.ok && converted.result.image.format == TextureSourceFormat::RGBA8);
    if (converted.ok) {
        const uint8* p = converted.result.image.pixels.Data();
        CHECK(p[0] == p[1] && p[1] == p[2] && p[3] == 255);
    }
}

// 받지 않는 변형·손실·손상·절단.
TEST(TiffDecode_RejectionsAndDamage) {
    SystemAllocator allocator;
    const uint8 handPixels[] = {10, 20};
    CHECK(Pixels(
        Decode(allocator, HandGray(allocator, COMPRESSION_NONE, PHOTOMETRIC_MINISBLACK).View()),
        TextureSourceFormat::G8, handPixels));
    // JPEG·옛 JPEG은 libjpeg-turbo로 푼다(A5g) — 손으로 만든 데이터는 JPEG가 아니라 손상이다.
    // PixarLog·SGILog·LogLuv는 LossySource.
    CHECK(Fails(
        Decode(allocator, HandGray(allocator, COMPRESSION_JPEG, PHOTOMETRIC_MINISBLACK).View()),
        AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(
        Decode(allocator, HandGray(allocator, COMPRESSION_OJPEG, PHOTOMETRIC_MINISBLACK).View()),
        AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(
        Decode(allocator, HandGray(allocator, COMPRESSION_PIXARLOG, PHOTOMETRIC_MINISBLACK).View()),
        AssetDiagnosticCode::LossySource));
    CHECK(
        Fails(Decode(allocator, HandGray(allocator, COMPRESSION_SGILOG, PHOTOMETRIC_LOGLUV).View()),
              AssetDiagnosticCode::LossySource));
    CHECK(Fails(Decode(allocator, HandGray(allocator, COMPRESSION_NONE, PHOTOMETRIC_LOGL).View()),
                AssetDiagnosticCode::LossySource));
    CHECK(Fails(
        Decode(allocator, HandGray(allocator, COMPRESSION_ZSTD, PHOTOMETRIC_MINISBLACK).View()),
        AssetDiagnosticCode::UnsupportedSourceFormat));
    // SampleFormat 2(signed)·CMYK는 UE도 받지 않는다.
    CHECK(Fails(
        Decode(allocator, HandGray(allocator, COMPRESSION_NONE, PHOTOMETRIC_MINISBLACK, 2).View()),
        AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(Fails(Decode(allocator,
                       HandGray(allocator, COMPRESSION_NONE, PHOTOMETRIC_SEPARATED, 1, 4).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));

    // 손상: strip offset이 파일 밖, IFD 항목 수가 파일을 넘는다.
    const uint8 pixels[2] = {1, 2};
    const HandTag outside[] = {{TIFFTAG_IMAGEWIDTH, TypeLong, 1, 2},
                               {TIFFTAG_IMAGELENGTH, TypeLong, 1, 1},
                               {TIFFTAG_BITSPERSAMPLE, TypeShort, 1, 8},
                               {TIFFTAG_PHOTOMETRIC, TypeShort, 1, PHOTOMETRIC_MINISBLACK},
                               {TIFFTAG_STRIPOFFSETS, TypeLong, 1, 4000},
                               {TIFFTAG_STRIPBYTECOUNTS, TypeLong, 1, 2}};
    CHECK(Fails(Decode(allocator, HandTiff(allocator, outside, pixels).View()),
                AssetDiagnosticCode::InvalidSource));
    const HandTag good[] = {{TIFFTAG_IMAGEWIDTH, TypeLong, 1, 2},
                            {TIFFTAG_IMAGELENGTH, TypeLong, 1, 1},
                            {TIFFTAG_BITSPERSAMPLE, TypeShort, 1, 8},
                            {TIFFTAG_PHOTOMETRIC, TypeShort, 1, PHOTOMETRIC_MINISBLACK},
                            {TIFFTAG_STRIPOFFSETS, TypeLong, 1, 8},
                            {TIFFTAG_STRIPBYTECOUNTS, TypeLong, 1, 2}};
    CHECK(Fails(Decode(allocator, HandTiff(allocator, good, pixels, 400).View()),
                AssetDiagnosticCode::InvalidSource));
    // StripByteCounts가 없으면 libtiff가 크기에서 추정한다(UE와 같다).
    CHECK(
        Pixels(Decode(allocator, HandTiff(allocator, Span<const HandTag>(good, 5), pixels).View()),
               TextureSourceFormat::G8, pixels));

    // 절단 전수(LZW RGB, 데이터 뒤 IFD).
    const uint8 rgb[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    TiffImage image;
    image.width = 2;
    image.height = 2;
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.compression = COMPRESSION_LZW;
    image.raw = rgb;
    const Array<uint8> file = WriteTiff(allocator, image);
    CHECK(Decode(allocator, file.View()).ok);
    for (usize length = 0; length < file.Size(); ++length) {
        CHECK(!Decode(allocator, file.View().Slice(0, length)).ok);
    }
}

// JPEG·YCbCr JPEG TIFF(libtiff 코덱 + libjpeg-turbo — 12.11절, JPEGTables 태그는 libtiff가 쓴다),
// 할당 실패는 두 묶음(tif_rocks·jmemrocks)이 같은 Allocator로 가는지 본다.
TEST(TiffDecode_Jpeg) {
    SystemAllocator allocator;
    constexpr uint32 Size = 16;
    uint8 rgb[Size * Size * 3] = {};
    for (uint32 y = 0; y < Size; ++y) {
        for (uint32 x = 0; x < Size; ++x) {
            uint8* p = rgb + (y * Size + x) * 3;
            p[0] = static_cast<uint8>(x * 15);
            p[1] = static_cast<uint8>(y * 15);
            p[2] = 100;
        }
    }

    TiffImage image;
    image.width = Size;
    image.height = Size;
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.compression = COMPRESSION_JPEG;
    image.rowsPerStrip = Size;
    image.raw = rgb;
    const auto close = [&rgb](const Decoded& decoded) noexcept {
        if (!decoded.ok || decoded.result.image.format != TextureSourceFormat::RGBA8 ||
            decoded.result.image.pixels.Size() != Size * Size * 4) {
            return false;
        }

        // 손실 부호화라 값은 원본 근처다(부드러운 기울기의 오차 한도).
        constexpr int Tolerance = 24;
        for (uint32 texel = 0; texel < Size * Size; ++texel) {
            for (uint32 c = 0; c < 3; ++c) {
                const int difference =
                    static_cast<int>(decoded.result.image.pixels[texel * 4 + c]) -
                    static_cast<int>(rgb[texel * 3 + c]);
                if (difference > Tolerance || difference < -Tolerance) {
                    return false;
                }
            }
        }

        return true;
    };
    const Array<uint8> rgbFile = WriteTiff(allocator, image);
    CHECK(close(Decode(allocator, rgbFile.View())));
    image.photometric = PHOTOMETRIC_YCBCR;
    image.jpegColorModeRgb = true;
    const Array<uint8> yccFile = WriteTiff(allocator, image);
    CHECK(close(Decode(allocator, yccFile.View())));

    // 할당 실패 전수: 모든 시도가 OutOfMemory거나 같은 결과로 성공한다.
    const Decoded reference = Decode(allocator, rgbFile.View());
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        ImageDecodeResult result(failing);
        auto decoded = DecodeImage(failing, rgbFile.View(), ImageFileFormat::Tiff, {}, result);
        const bool exhausted = failing.Attempts() < attempt;
        failing.DisableFailure();
        if (exhausted) {
            CHECK(decoded);
            break;
        }

        if (decoded) {
            CHECK(platform::MemoryEquals(result.image.pixels.Data(),
                                         reference.result.image.pixels.Data(),
                                         reference.result.image.pixels.Size()));
            continue;
        }

        CHECK(decoded.Error() == ErrorCode::OutOfMemory &&
              result.failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures > 0);
}

namespace {

// 살아 있는 블록 수를 세는 Allocator다(디코딩이 실패 경로에서도 받은 블록을 전부 돌려주는지 본다).
class LiveCountingAllocator final : public Allocator {
public:
    explicit LiveCountingAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto allocated = inner_.Allocate(bytes, alignment, tag);
        if (allocated) {
            ++live_;
        }

        return allocated;
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address != nullptr) {
            --live_;
        }

        return inner_.Deallocate(address);
    }

    void TrimMemory() noexcept override {
        inner_.TrimMemory();
    }

    void MarkThreadIdle() noexcept override {
        inner_.MarkThreadIdle();
    }

    void MarkThreadActive() noexcept override {
        inner_.MarkThreadActive();
    }

    int64 Live() const noexcept {
        return live_;
    }

private:
    Allocator& inner_;
    int64 live_ = 0;
};

struct ThreadJob {
    Span<const uint8> file;
    test::NFailAllocator* allocator = nullptr;
    bool ok = false;
};

void DecodeOnThread(void* context) noexcept {
    auto* job = static_cast<ThreadJob*>(context);
    ImageDecodeResult result(*job->allocator);
    job->ok = DecodeImage(*job->allocator, job->file, ImageFileFormat::Tiff, {}, result).IsOk();
}

} // namespace

// 할당 실패(모든 시도가 OutOfMemory거나 같은 결과로 성공)와 스레드별 묶음(할당 횟수가 단독 실행과
// 같다).
TEST(TiffDecode_AllocationAndThreads) {
    SystemAllocator allocator;
    const uint8 rgb[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    TiffImage image;
    image.width = 2;
    image.height = 2;
    image.samples = 3;
    image.photometric = PHOTOMETRIC_RGB;
    image.compression = COMPRESSION_LZW;
    image.raw = rgb;
    const Array<uint8> file = WriteTiff(allocator, image);
    const Decoded reference = Decode(allocator, file.View());
    CHECK(reference.ok);
    uint32 failures = 0;
    uint64 firstLeak = 0;
    for (uint64 attempt = 1;; ++attempt) {
        LiveCountingAllocator counted(allocator);
        test::NFailAllocator failing(counted);
        failing.FailOnAttempt(attempt);
        bool exhausted = false;
        bool ok = false;
        bool same = true;
        bool outOfMemory = true;
        {
            ImageDecodeResult result(failing);
            auto decoded = DecodeImage(failing, file.View(), ImageFileFormat::Tiff, {}, result);
            exhausted = failing.Attempts() < attempt;
            failing.DisableFailure();
            ok = decoded.IsOk();
            if (ok && !exhausted) {
                same = platform::MemoryEquals(result.image.pixels.Data(),
                                              reference.result.image.pixels.Data(),
                                              reference.result.image.pixels.Size());
            } else if (!ok) {
                outOfMemory = decoded.Error() == ErrorCode::OutOfMemory &&
                              result.failure.code == AssetDiagnosticCode::None;
            }
        }

        // 결과를 버리면 받은 블록이 모두 돌아와 있다(실패 경로도).
        if (counted.Live() != 0 && firstLeak == 0) {
            firstLeak = attempt;
        }

        if (exhausted) {
            CHECK(ok);
            break;
        }

        CHECK(same && outOfMemory);
        if (!ok) {
            ++failures;
        }
    }

    CHECK(firstLeak == 0);
    CHECK(failures > 0);

    // 단독 실행의 할당 횟수와, 두 스레드가 각자 Allocator로 동시에 디코딩한 횟수가 같다.
    TiffImage other = image;
    other.compression = COMPRESSION_ADOBE_DEFLATE;
    const Array<uint8> second = WriteTiff(allocator, other);
    // 스레드마다 따로 센다(같은 시각에 두 스레드가 한 세기를 바꾸지 않게).
    LiveCountingAllocator firstCount(allocator);
    LiveCountingAllocator secondCount(allocator);
    test::NFailAllocator soloFirst(firstCount);
    test::NFailAllocator soloSecond(secondCount);
    ThreadJob solo[2] = {{file.View(), &soloFirst}, {second.View(), &soloSecond}};
    DecodeOnThread(&solo[0]);
    DecodeOnThread(&solo[1]);
    test::NFailAllocator sharedFirst(firstCount);
    test::NFailAllocator sharedSecond(secondCount);
    ThreadJob jobs[2] = {{file.View(), &sharedFirst}, {second.View(), &sharedSecond}};
    platform::Thread threads[2];
    for (uint32 index = 0; index < 2; ++index) {
        CHECK(threads[index].Start(DecodeOnThread, &jobs[index]) ==
              platform::ThreadStatus::Success);
    }

    for (platform::Thread& thread : threads) {
        CHECK(thread.Join() == platform::ThreadStatus::Success);
    }

    CHECK(solo[0].ok && solo[1].ok && jobs[0].ok && jobs[1].ok);
    CHECK(sharedFirst.Attempts() == soloFirst.Attempts() &&
          sharedSecond.Attempts() == soloSecond.Attempts());
    CHECK(firstCount.Live() == 0 && secondCount.Live() == 0);
}

} // namespace tiff_decode_tests_file
