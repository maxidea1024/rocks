#include <Compression/Compression.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <ImageDecoders/ImageDecode.h>
#include <Platform/Memory.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 원본 디코더 시험이다(Docs/Design/AssetSystem 12.2·12.9·15절 A5e). 시험 안의 생성기가 파일 byte를
// 결정적으로 만든다: BMP·TGA·PNG·PSD의 변형별 결과 형식·픽셀, UE와 같은 알파 규칙, 손상(절단 전수·
// CRC·RLE 넘침), 받지 않는 변형, 판별, 한도, 경고, 할당 실패.
using namespace rocks;
using namespace rocks::image;
namespace image_decode_tests_file {

namespace {

// 파일 byte 생성기다.
struct Bytes {
    explicit Bytes(Allocator& allocator) noexcept : data(allocator) {
    }

    void U8(uint32 value) noexcept {
        CHECK(data.TryPushBack(static_cast<uint8>(value)));
    }

    void Le16(uint32 value) noexcept {
        U8(value);
        U8(value >> 8);
    }

    void Le32(uint32 value) noexcept {
        Le16(value);
        Le16(value >> 16);
    }

    void Be16(uint32 value) noexcept {
        U8(value >> 8);
        U8(value);
    }

    void Be32(uint32 value) noexcept {
        Be16(value >> 16);
        Be16(value);
    }

    void Text(const char* text) noexcept {
        for (; *text != '\0'; ++text) {
            U8(static_cast<uint8>(*text));
        }
    }

    void Append(Span<const uint8> bytes) noexcept {
        CHECK(data.TryAppend(bytes));
    }

    Array<uint8> data;
};

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file,
               ImageFileFormat format = ImageFileFormat::Unknown,
               ImageDecodeLimits limits = {}) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, format, limits, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

bool Pixels(const Decoded& decoded, TextureSourceFormat format, uint32 width, uint32 height,
            Span<const uint8> expected) noexcept {
    const DecodedImage& image = decoded.result.image;
    if (!decoded.ok || image.format != format || image.width != width || image.height != height ||
        image.pixels.Size() != expected.Size()) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (image.pixels[index] != expected[index]) {
            return false;
        }
    }

    return true;
}

// 모든 앞부분(길이 0 … 전체 − 1)은 실패하고 넘침 없이 끝난다(이 시험은 Asan 없이도 결과 실패를
// 본다).
bool EveryPrefixFails(Allocator& allocator, Span<const uint8> file,
                      ImageFileFormat format) noexcept {
    for (usize length = 0; length < file.Size(); ++length) {
        const Decoded decoded = Decode(allocator, file.Slice(0, length), format);
        if (decoded.ok) {
            return false;
        }
    }

    return true;
}

// ---- BMP ----

// 40 byte(또는 headerSize) 헤더 BMP다. masks는 헤더 안(52 이상) 또는 뒤(40 + bitfields).
Array<uint8> Bmp(Allocator& allocator, uint32 headerSize, int32 width, int32 height,
                 uint32 bitCount, uint32 compression, Span<const uint32> masks,
                 Span<const uint8> palette, Span<const uint8> pixels) noexcept {
    Bytes body(allocator);
    body.Le32(headerSize);
    body.Le32(static_cast<uint32>(width));
    body.Le32(static_cast<uint32>(height));
    body.Le16(1);
    body.Le16(bitCount);
    body.Le32(compression);
    body.Le32(static_cast<uint32>(pixels.Size()));
    body.Le32(2835);
    body.Le32(2835);
    body.Le32(static_cast<uint32>(palette.Size() / 4));
    body.Le32(0);
    if (headerSize > 40) {
        for (uint32 index = 0; index < 4; ++index) {
            if (40 + index * 4 + 4 <= headerSize) {
                body.Le32(index < masks.Size() ? masks[index] : 0);
            }
        }

        while (body.data.Size() < headerSize) {
            body.U8(0);
        }
    } else {
        for (uint32 mask : masks) {
            body.Le32(mask);
        }
    }

    body.Append(palette);
    Bytes file(allocator);
    file.Text("BM");
    file.Le32(static_cast<uint32>(14 + body.data.Size() + pixels.Size()));
    file.Le32(0);
    file.Le32(static_cast<uint32>(14 + body.data.Size()));
    file.Append(body.data.View());
    file.Append(pixels);
    return Move(file.data);
}

} // namespace

// BMP 24비트 아래→위·위→아래, 32비트 BI_RGB 알파(있음·모두 0 → 255 — UE와 같다), 16비트 555,
// 8비트 색표(범위 밖 index → 불투명 검정), RLE8, BITFIELDS 565, V4 알파 마스크, 40 byte
// ALPHABITFIELDS의 알파 무시, 받지 않는 변형(OS/2·BI_PNG), 절단.
TEST(ImageDecode_Bmp) {
    SystemAllocator allocator;
    // 2×2 24비트 아래→위(행 4 byte 정렬 — 6 + 2 패딩). 아래 행이 먼저다.
    const uint8 rows24[] = {0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0};
    const Array<uint8> bmp24 = Bmp(allocator, 40, 2, 2, 24, 0, {}, {}, rows24);
    // 위 행(저장 둘째 행): 파랑·청록, 아래 행(저장 첫 행): 빨강·초록.
    const uint8 expect24[] = {0, 0, 255, 255, 0, 255, 255, 255, 255, 0, 0, 255, 0, 255, 0, 255};
    CHECK(Pixels(Decode(allocator, bmp24.View()), TextureSourceFormat::RGBA8, 2, 2, expect24));
    CHECK(EveryPrefixFails(allocator, bmp24.View(), ImageFileFormat::Bmp));

    // 위→아래(음수 높이).
    const Array<uint8> topDown = Bmp(allocator, 40, 2, -2, 24, 0, {}, {}, rows24);
    const uint8 expectTop[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 255, 255, 255};
    CHECK(Pixels(Decode(allocator, topDown.View()), TextureSourceFormat::RGBA8, 2, 2, expectTop));

    // 32비트 BI_RGB: 넷째 byte는 알파, 모두 0이면 255.
    const uint8 argb[] = {1, 2, 3, 40};
    const Array<uint8> alpha32 = Bmp(allocator, 40, 1, 1, 32, 0, {}, {}, argb);
    const uint8 expectAlpha[] = {3, 2, 1, 40};
    CHECK(Pixels(Decode(allocator, alpha32.View()), TextureSourceFormat::RGBA8, 1, 1, expectAlpha));
    const uint8 xrgb[] = {1, 2, 3, 0};
    const Array<uint8> zero32 = Bmp(allocator, 40, 1, 1, 32, 0, {}, {}, xrgb);
    const uint8 expectOpaque[] = {3, 2, 1, 255};
    CHECK(Pixels(Decode(allocator, zero32.View()), TextureSourceFormat::RGBA8, 1, 1, expectOpaque));

    // 16비트 555(최상위 비트 무시): 0x7C00 = 빨강 최대.
    const uint8 rgb555[] = {0x00, 0xFC, 0, 0};
    const Array<uint8> bmp16 = Bmp(allocator, 40, 1, 1, 16, 0, {}, {}, rgb555);
    const uint8 expectRed[] = {255, 0, 0, 255};
    CHECK(Pixels(Decode(allocator, bmp16.View()), TextureSourceFormat::RGBA8, 1, 1, expectRed));

    // 8비트 색표 2개, index 5는 범위 밖 → 불투명 검정.
    const uint8 palette[] = {10, 20, 30, 0, 40, 50, 60, 0};
    const uint8 indices[] = {1, 5, 0, 0};
    const Array<uint8> bmp8 = Bmp(allocator, 40, 2, 1, 8, 0, {}, palette, indices);
    const uint8 expectIndexed[] = {60, 50, 40, 255, 0, 0, 0, 255};
    CHECK(Pixels(Decode(allocator, bmp8.View()), TextureSourceFormat::RGBA8, 2, 1, expectIndexed));

    // RLE8: 반복 2개(index 1) + 행 끝 + 이미지 끝(2×1).
    const uint8 rle[] = {2, 1, 0, 0, 0, 1};
    const Array<uint8> bmpRle = Bmp(allocator, 40, 2, 1, 8, 1, {}, palette, rle);
    const uint8 expectRle[] = {60, 50, 40, 255, 60, 50, 40, 255};
    CHECK(Pixels(Decode(allocator, bmpRle.View()), TextureSourceFormat::RGBA8, 2, 1, expectRle));
    // RLE 위→아래는 InvalidSource.
    CHECK(Fails(Decode(allocator, Bmp(allocator, 40, 2, -1, 8, 1, {}, palette, rle).View()),
                AssetDiagnosticCode::InvalidSource));

    // BITFIELDS 565(40 byte 헤더 뒤 마스크 셋 — Adobe 변형).
    const uint32 masks565[] = {0xF800, 0x07E0, 0x001F};
    const uint8 green565[] = {0xE0, 0x07, 0, 0};
    const Array<uint8> bmp565 = Bmp(allocator, 40, 1, 1, 16, 3, masks565, {}, green565);
    const uint8 expectGreen[] = {0, 255, 0, 255};
    CHECK(Pixels(Decode(allocator, bmp565.View()), TextureSourceFormat::RGBA8, 1, 1, expectGreen));

    // 32비트 마스크 + 알파: V4(108)는 알파를 쓰고 40 byte ALPHABITFIELDS는 무시한다(UE와 같다).
    const uint32 masks8888[] = {0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000};
    const uint8 bgra[] = {10, 20, 30, 77};
    const uint8 expectV4[] = {30, 20, 10, 77};
    const uint8 expectNoAlpha[] = {30, 20, 10, 255};
    CHECK(Pixels(Decode(allocator, Bmp(allocator, 108, 1, 1, 32, 3, masks8888, {}, bgra).View()),
                 TextureSourceFormat::RGBA8, 1, 1, expectV4));
    CHECK(Pixels(Decode(allocator, Bmp(allocator, 40, 1, 1, 32, 6, masks8888, {}, bgra).View()),
                 TextureSourceFormat::RGBA8, 1, 1, expectNoAlpha));
    CHECK(Pixels(Decode(allocator, Bmp(allocator, 56, 1, 1, 32, 3, masks8888, {}, bgra).View()),
                 TextureSourceFormat::RGBA8, 1, 1, expectNoAlpha));
    // 겹치는 마스크는 InvalidSource.
    const uint32 overlap[] = {0xFF00, 0x0FF0, 0x000F};
    CHECK(Fails(Decode(allocator, Bmp(allocator, 40, 1, 1, 16, 3, overlap, {}, green565).View()),
                AssetDiagnosticCode::InvalidSource));

    // 받지 않는 변형: OS/2 12 byte 헤더, BI_PNG.
    Array<uint8> os2 = Bmp(allocator, 40, 1, 1, 24, 0, {}, {}, rows24);
    os2[14] = 12;
    CHECK(Fails(Decode(allocator, os2.View()), AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(Fails(Decode(allocator, Bmp(allocator, 40, 1, 1, 24, 5, {}, {}, rows24).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

namespace {

// ---- TGA ----

Array<uint8> Tga(Allocator& allocator, uint32 type, uint32 width, uint32 height, uint32 depth,
                 uint32 descriptor, Span<const uint8> palette, uint32 paletteDepth,
                 Span<const uint8> pixels, int32 attributes = -1) noexcept {
    Bytes file(allocator);
    file.U8(0);
    file.U8(palette.IsEmpty() ? 0 : 1);
    file.U8(type);
    file.Le16(0);
    file.Le16(palette.IsEmpty() ? 0
                                : static_cast<uint32>(palette.Size() / ((paletteDepth + 7) / 8)));
    file.U8(palette.IsEmpty() ? 0 : paletteDepth);
    file.Le16(0);
    file.Le16(0);
    file.Le16(width);
    file.Le16(height);
    file.U8(depth);
    file.U8(descriptor);
    file.Append(palette);
    file.Append(pixels);
    if (attributes >= 0) {
        // 확장 영역(495 byte — attributes type은 494번째).
        const uint32 extension = static_cast<uint32>(file.data.Size());
        file.Le16(495);
        while (file.data.Size() < extension + 494) {
            file.U8(0);
        }

        file.U8(static_cast<uint32>(attributes));
        file.Le32(extension);
        file.Le32(0);
        file.Text("TRUEVISION-XFILE.");
        file.U8(0);
    }

    return Move(file.data);
}

} // namespace

// TGA 형식 2(아래→위)·3(회색 G8)·1(색표 적용)·10(RLE가 행을 넘음), 알파 비트 0이면 알파 무시,
// 확장 영역 attributes 4(premultiplied → straight)·2(무시), 16비트 회색 받지 않음, 판별(footer),
// 절단.
TEST(ImageDecode_Tga) {
    SystemAllocator allocator;
    // 2×1 32비트, 알파 비트 8, 아래→위 기본(1행이라 같다).
    const uint8 bgra[] = {10, 20, 30, 40, 50, 60, 70, 80};
    const Array<uint8> truecolor = Tga(allocator, 2, 2, 1, 32, 8, {}, 0, bgra);
    const uint8 expect[] = {30, 20, 10, 40, 70, 60, 50, 80};
    CHECK(Pixels(Decode(allocator, truecolor.View(), ImageFileFormat::Tga),
                 TextureSourceFormat::RGBA8, 2, 1, expect));
    CHECK(EveryPrefixFails(allocator, truecolor.View(), ImageFileFormat::Tga));
    // 알파 비트 0 → 알파 무시.
    const uint8 expectOpaque[] = {30, 20, 10, 255, 70, 60, 50, 255};
    CHECK(Pixels(
        Decode(allocator, Tga(allocator, 2, 2, 1, 32, 0, {}, 0, bgra).View(), ImageFileFormat::Tga),
        TextureSourceFormat::RGBA8, 2, 1, expectOpaque));
    // 2행 아래→위: 첫 저장 행이 아래.
    const uint8 gray[] = {1, 2, 3, 4};
    const uint8 expectGray[] = {3, 4, 1, 2};
    CHECK(Pixels(
        Decode(allocator, Tga(allocator, 3, 2, 2, 8, 0, {}, 0, gray).View(), ImageFileFormat::Tga),
        TextureSourceFormat::G8, 2, 2, expectGray));
    // 위→아래 원점 비트(0x20).
    CHECK(Pixels(Decode(allocator, Tga(allocator, 3, 2, 2, 8, 0x20, {}, 0, gray).View(),
                        ImageFileFormat::Tga),
                 TextureSourceFormat::G8, 2, 2, gray));
    // 색표(24비트 항목) 적용.
    const uint8 palette[] = {0, 0, 255, 0, 255, 0};
    const uint8 indices[] = {1, 0};
    const uint8 expectMapped[] = {0, 255, 0, 255, 255, 0, 0, 255};
    CHECK(Pixels(Decode(allocator, Tga(allocator, 1, 2, 1, 8, 0, palette, 24, indices).View(),
                        ImageFileFormat::Tga),
                 TextureSourceFormat::RGBA8, 2, 1, expectMapped));
    // RLE(형식 11 회색): 반복 3개 + 그대로 1개가 2×2 행을 넘는다.
    const uint8 rle[] = {0x82, 9, 0x00, 7};
    const uint8 expectRle[] = {9, 7, 9, 9};
    CHECK(Pixels(
        Decode(allocator, Tga(allocator, 11, 2, 2, 8, 0, {}, 0, rle).View(), ImageFileFormat::Tga),
        TextureSourceFormat::G8, 2, 2, expectRle));
    // RLE 넘침은 InvalidSource.
    const uint8 overflow[] = {0x85, 9};
    CHECK(Fails(Decode(allocator, Tga(allocator, 11, 2, 2, 8, 0, {}, 0, overflow).View(),
                       ImageFileFormat::Tga),
                AssetDiagnosticCode::InvalidSource));
    // premultiplied(attributes 4): (60·255/120)=128 근처로 되돌린다, 알파 0은 색 0.
    const uint8 premultiplied[] = {0, 30, 60, 120, 9, 9, 9, 0};
    const Array<uint8> pre = Tga(allocator, 2, 2, 1, 32, 8, {}, 0, premultiplied, 4);
    CHECK(DetectImageFileFormat(pre.View()) == ImageFileFormat::Tga);
    const uint8 expectStraight[] = {128, 64, 0, 120, 0, 0, 0, 0};
    CHECK(Pixels(Decode(allocator, pre.View()), TextureSourceFormat::RGBA8, 2, 1, expectStraight));
    // attributes 2 → 알파 무시.
    const uint8 expectIgnored[] = {60, 30, 0, 255, 9, 9, 9, 255};
    CHECK(Pixels(Decode(allocator, Tga(allocator, 2, 2, 1, 32, 8, {}, 0, premultiplied, 2).View()),
                 TextureSourceFormat::RGBA8, 2, 1, expectIgnored));
    // 16비트 회색은 받지 않는다.
    const uint8 gray16[] = {1, 2};
    CHECK(Fails(Decode(allocator, Tga(allocator, 3, 1, 1, 16, 0, {}, 0, gray16).View(),
                       ImageFileFormat::Tga),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

namespace {

// ---- PNG ----

void Chunk(Bytes& file, const char* type, Span<const uint8> data) noexcept {
    file.Be32(static_cast<uint32>(data.Size()));
    const usize start = file.data.Size();
    file.Text(type);
    file.Append(data);
    file.Be32(Crc32(file.data.View().Slice(start, 4 + data.Size())));
}

struct PngSpec {
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 8;
    uint32 colorType = 0;
    uint32 interlace = 0;
    // 필터 byte를 포함한 행 데이터(해제 전).
    Span<const uint8> raw{};
    Span<const uint8> palette{};
    Span<const uint8> transparency{};
    // 추가 chunk(IHDR 뒤에 넣는다).
    const char* extraType = nullptr;
    Span<const uint8> extra{};
};

Array<uint8> Png(Allocator& allocator, const PngSpec& spec) noexcept {
    Bytes file(allocator);
    const uint8 signature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    file.Append(signature);
    Bytes ihdr(allocator);
    ihdr.Be32(spec.width);
    ihdr.Be32(spec.height);
    ihdr.U8(spec.depth);
    ihdr.U8(spec.colorType);
    ihdr.U8(0);
    ihdr.U8(0);
    ihdr.U8(spec.interlace);
    Chunk(file, "IHDR", ihdr.data.View());
    if (spec.extraType != nullptr) {
        Chunk(file, spec.extraType, spec.extra);
    }

    if (!spec.palette.IsEmpty()) {
        Chunk(file, "PLTE", spec.palette);
    }

    if (!spec.transparency.IsEmpty()) {
        Chunk(file, "tRNS", spec.transparency);
    }

    Array<uint8> compressed(allocator);
    auto settings = CompressionSettings::TryDefault(CompressionFormat::Zlib);
    REQUIRE(settings);
    REQUIRE(TryCompressAppend(settings.Value(), spec.raw, compressed));
    Chunk(file, "IDAT", compressed.View());
    Chunk(file, "IEND", {});
    return Move(file.data);
}

} // namespace

// PNG 색 형식·비트 깊이별 결과 형식(12.2절 표 — 회색 1·2·4비트 확장, 16비트 little-endian 저장,
// tRNS 회색·RGB·팔레트), 필터 1–4, Adam7 = 같은 그림의 비interlace, CRC 손상·모르는 필수 chunk·해제
// 크기 불일치, 보조 chunk 건너뜀, NonSrgbSource 판정(sRGB 우선·iCCP·gAMA), 절단.
TEST(ImageDecode_Png) {
    SystemAllocator allocator;
    // 회색 8비트 2×1, 필터 0.
    const uint8 gray8[] = {0, 10, 200};
    PngSpec spec;
    spec.width = 2;
    spec.raw = gray8;
    const Array<uint8> simple = Png(allocator, spec);
    const uint8 expectGray8[] = {10, 200};
    CHECK(Pixels(Decode(allocator, simple.View()), TextureSourceFormat::G8, 2, 1, expectGray8));
    CHECK(EveryPrefixFails(allocator, simple.View(), ImageFileFormat::Png));
    // 회색 1비트 8×1: 10110000 → 255,0,255,255,0,0,0,0.
    const uint8 gray1[] = {0, 0xB0};
    spec.width = 8;
    spec.depth = 1;
    spec.raw = gray1;
    const uint8 expectGray1[] = {255, 0, 255, 255, 0, 0, 0, 0};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::G8, 8, 1,
                 expectGray1));
    // 회색 2비트: 값 1 → 85.
    const uint8 gray2[] = {0, 0x40};
    spec.width = 1;
    spec.depth = 2;
    spec.raw = gray2;
    const uint8 expectGray2[] = {85};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::G8, 1, 1,
                 expectGray2));
    // 회색 16비트(big-endian 저장 → little-endian 결과).
    const uint8 gray16[] = {0, 0x12, 0x34};
    spec.depth = 16;
    spec.raw = gray16;
    const uint8 expectGray16[] = {0x34, 0x12};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::G16, 1, 1,
                 expectGray16));
    // 회색 8비트 + tRNS(값 10) → GA8.
    const uint8 key[] = {0, 10};
    spec.width = 2;
    spec.depth = 8;
    spec.raw = gray8;
    spec.transparency = key;
    const uint8 expectKeyed[] = {10, 0, 200, 255};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::GA8, 2, 1,
                 expectKeyed));
    spec.transparency = {};
    // RGB 16비트 → RGBA16(알파 65535).
    const uint8 rgb16[] = {0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    spec.width = 1;
    spec.depth = 16;
    spec.colorType = 2;
    spec.raw = rgb16;
    const uint8 expectRgb16[] = {0x02, 0x01, 0x04, 0x03, 0x06, 0x05, 0xFF, 0xFF};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::RGBA16, 1, 1,
                 expectRgb16));
    // 팔레트 + tRNS(항목 0만 알파 7).
    const uint8 palette[] = {1, 2, 3, 4, 5, 6};
    const uint8 paletteAlpha[] = {7};
    const uint8 indexed[] = {0, 1, 0};
    spec = PngSpec{};
    spec.width = 2;
    spec.colorType = 3;
    spec.raw = indexed;
    spec.palette = palette;
    spec.transparency = paletteAlpha;
    const uint8 expectIndexed[] = {4, 5, 6, 255, 1, 2, 3, 7};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::RGBA8, 2, 1,
                 expectIndexed));
    // 팔레트 밖 index는 InvalidSource.
    const uint8 outside[] = {0, 5, 0};
    spec.raw = outside;
    CHECK(
        Fails(Decode(allocator, Png(allocator, spec).View()), AssetDiagnosticCode::InvalidSource));
    // RGBA 8비트, 필터 1(Sub)·2(Up)·3(Average)·4(Paeth) — 2×2.
    spec = PngSpec{};
    spec.width = 2;
    spec.height = 2;
    spec.colorType = 6;
    // 원래 그림: (10,20,30,40)(11,21,31,41) / (12,22,32,42)(13,23,33,43).
    const uint8 subUp[] = {1, 10, 20, 30, 40, 1, 1, 1, 1,  // Sub: 두 번째 = 첫째 + 1
                           2, 2,  2,  2,  2,  2, 2, 2, 2}; // Up: 위 행 + 2
    spec.raw = subUp;
    const uint8 expectRgba[] = {10, 20, 30, 40, 11, 21, 31, 41, 12, 22, 32, 42, 13, 23, 33, 43};
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::RGBA8, 2, 2,
                 expectRgba));
    // 필터 5는 InvalidSource.
    const uint8 badFilter[] = {5, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 3, 4, 5, 6, 7, 8};
    spec.raw = badFilter;
    CHECK(
        Fails(Decode(allocator, Png(allocator, spec).View()), AssetDiagnosticCode::InvalidSource));
    // Adam7 2×2(패스 1: (0,0), 패스 6: (1,0), 패스 7: 아래 행 둘) = 비interlace와 같은 그림.
    const uint8 adam[] = {0, 10, 20, 30, 40, 0, 11, 21, 31, 41, 0, 12, 22, 32, 42, 13, 23, 33, 43};
    spec.raw = adam;
    spec.interlace = 1;
    CHECK(Pixels(Decode(allocator, Png(allocator, spec).View()), TextureSourceFormat::RGBA8, 2, 2,
                 expectRgba));
    // 해제 크기 불일치(행 하나 모자람)는 InvalidSource.
    spec.interlace = 0;
    spec.raw = Span<const uint8>(subUp, 9);
    CHECK(
        Fails(Decode(allocator, Png(allocator, spec).View()), AssetDiagnosticCode::InvalidSource));

    // CRC 손상·모르는 필수 chunk·보조 chunk 건너뜀.
    Array<uint8> corrupt = Png(allocator, PngSpec{1, 1, 8, 0, 0, Span<const uint8>(gray8, 2)});
    corrupt[29] ^= 1;
    CHECK(Fails(Decode(allocator, corrupt.View()), AssetDiagnosticCode::InvalidSource));
    PngSpec withChunk{1, 1, 8, 0, 0, Span<const uint8>(gray8, 2)};
    withChunk.extraType = "ABCD";
    CHECK(Fails(Decode(allocator, Png(allocator, withChunk).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    withChunk.extraType = "abCd";
    CHECK(Decode(allocator, Png(allocator, withChunk).View()).ok);

    // NonSrgbSource: gAMA 1.0(100000)은 경고, sRGB chunk가 있으면 경고 없음, iCCP는 경고.
    const uint8 linearGamma[] = {0, 1, 0x86, 0xA0};
    withChunk.extraType = "gAMA";
    withChunk.extra = linearGamma;
    const Decoded warned = Decode(allocator, Png(allocator, withChunk).View());
    REQUIRE(warned.ok);
    REQUIRE(warned.result.warnings.Size() == 1);
    CHECK(warned.result.warnings[0].code == AssetDiagnosticCode::NonSrgbSource);
    const uint8 srgbGamma[] = {0, 0, 0xB1, 0x8F};
    withChunk.extra = srgbGamma;
    CHECK(Decode(allocator, Png(allocator, withChunk).View()).result.warnings.IsEmpty());
    const uint8 icc[] = {'p', 0, 0};
    withChunk.extraType = "iCCP";
    withChunk.extra = icc;
    CHECK(Decode(allocator, Png(allocator, withChunk).View()).result.warnings.Size() == 1);
}

namespace {

// ---- PSD ----

struct PsdSpec {
    uint32 version = 1;
    uint32 channels = 3;
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 8;
    uint32 mode = 3;
    uint32 compression = 0;
    // 평면(채널 순서, 채널마다 height × rowBytes) — raw만. RLE는 행마다 그대로 패킷(literal)으로.
    Span<const uint8> planes{};
    Span<const uint8> colorData{};
    // 리소스 0x0421 hasRealMergedData(-1이면 리소스 없음).
    int32 merged = -1;
};

Array<uint8> Psd(Allocator& allocator, const PsdSpec& spec) noexcept {
    Bytes file(allocator);
    file.Text("8BPS");
    file.Be16(spec.version);
    for (uint32 index = 0; index < 6; ++index) {
        file.U8(0);
    }

    file.Be16(spec.channels);
    file.Be32(spec.height);
    file.Be32(spec.width);
    file.Be16(spec.depth);
    file.Be16(spec.mode);
    file.Be32(static_cast<uint32>(spec.colorData.Size()));
    file.Append(spec.colorData);
    if (spec.merged >= 0) {
        file.Be32(4 + 2 + 2 + 4 + 6);
        file.Text("8BIM");
        file.Be16(0x0421);
        file.Be16(0);
        file.Be32(5);
        file.Be32(1);
        file.U8(static_cast<uint32>(spec.merged));
        file.U8(0);
    } else {
        file.Be32(0);
    }

    if (spec.version == 2) {
        file.Be32(0);
        file.Be32(0);
    } else {
        file.Be32(0);
    }

    file.Be16(spec.compression);
    const usize rowBytes = (static_cast<usize>(spec.width) * spec.depth + 7) / 8;
    if (spec.compression == 1) {
        // 행마다 literal 패킷 하나(머리 1 + 행) — byte 수 표 먼저.
        const uint32 rows = spec.channels * spec.height;
        for (uint32 row = 0; row < rows; ++row) {
            if (spec.version == 2) {
                file.Be32(static_cast<uint32>(rowBytes + 1));
            } else {
                file.Be16(static_cast<uint32>(rowBytes + 1));
            }
        }

        for (uint32 row = 0; row < rows; ++row) {
            file.U8(static_cast<uint32>(rowBytes - 1));
            file.Append(spec.planes.Slice(row * rowBytes, rowBytes));
        }
    } else {
        file.Append(spec.planes);
    }

    return Move(file.data);
}

} // namespace

// PSD RGB 8비트 3채널(알파 255)·4채널(넷째 = 알파, UE와 같다)·5채널(IgnoredSourceChannels),
// Grayscale 16비트 2채널(GA16), PSB, RLE, Bitmap(0 → 흰색), Indexed, 32비트 float(NaN → 0),
// hasRealMergedData false·ZIP·CMYK 받지 않음, 절단.
TEST(ImageDecode_Psd) {
    SystemAllocator allocator;
    const uint8 rgb[] = {10, 20, 30};
    PsdSpec spec;
    spec.planes = rgb;
    const Array<uint8> plain = Psd(allocator, spec);
    const uint8 expectRgb[] = {10, 20, 30, 255};
    CHECK(Pixels(Decode(allocator, plain.View()), TextureSourceFormat::RGBA8, 1, 1, expectRgb));
    CHECK(EveryPrefixFails(allocator, plain.View(), ImageFileFormat::Psd));
    // 4채널: 넷째가 알파.
    const uint8 rgba[] = {10, 20, 30, 40};
    spec.channels = 4;
    spec.planes = rgba;
    const uint8 expectRgba[] = {10, 20, 30, 40};
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::RGBA8, 1, 1,
                 expectRgba));
    // 5채널: 다섯째는 버리고 경고(detail 1).
    const uint8 five[] = {10, 20, 30, 40, 50};
    spec.channels = 5;
    spec.planes = five;
    const Decoded extra = Decode(allocator, Psd(allocator, spec).View());
    CHECK(Pixels(extra, TextureSourceFormat::RGBA8, 1, 1, expectRgba));
    REQUIRE(extra.result.warnings.Size() == 1);
    CHECK(extra.result.warnings[0].code == AssetDiagnosticCode::IgnoredSourceChannels &&
          extra.result.warnings[0].detail == 1);
    // RLE·PSB도 같은 결과.
    spec.channels = 4;
    spec.planes = rgba;
    spec.compression = 1;
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::RGBA8, 1, 1,
                 expectRgba));
    spec.version = 2;
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::RGBA8, 1, 1,
                 expectRgba));
    // Grayscale 16비트 2채널 → GA16(big-endian → little-endian).
    spec = PsdSpec{};
    spec.mode = 1;
    spec.channels = 2;
    spec.depth = 16;
    const uint8 grayAlpha[] = {0x12, 0x34, 0xAB, 0xCD};
    spec.planes = grayAlpha;
    const uint8 expectGa16[] = {0x34, 0x12, 0xCD, 0xAB};
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::GA16, 1, 1,
                 expectGa16));
    // Bitmap 8×1: 1 → 검정 0, 0 → 흰색 255.
    spec = PsdSpec{};
    spec.mode = 0;
    spec.channels = 1;
    spec.depth = 1;
    spec.width = 8;
    const uint8 bits[] = {0xF0};
    spec.planes = bits;
    const uint8 expectBits[] = {0, 0, 0, 0, 255, 255, 255, 255};
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::G8, 8, 1,
                 expectBits));
    // Indexed: 팔레트 평면(R 256·G 256·B 256).
    uint8 colors[768] = {};
    colors[2] = 9;
    colors[256 + 2] = 8;
    colors[512 + 2] = 7;
    spec = PsdSpec{};
    spec.mode = 2;
    spec.channels = 1;
    spec.colorData = colors;
    const uint8 index[] = {2};
    spec.planes = index;
    const uint8 expectIndexed[] = {9, 8, 7, 255};
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::RGBA8, 1, 1,
                 expectIndexed));
    // 32비트 float 회색 1채널: NaN → 0.
    spec = PsdSpec{};
    spec.mode = 1;
    spec.channels = 1;
    spec.depth = 32;
    const uint8 nan[] = {0x7F, 0xC0, 0x00, 0x00};
    spec.planes = nan;
    const uint8 expectZero[] = {0, 0, 0, 0};
    CHECK(Pixels(Decode(allocator, Psd(allocator, spec).View()), TextureSourceFormat::R32F, 1, 1,
                 expectZero));
    // 받지 않는 변형.
    spec = PsdSpec{};
    spec.planes = rgb;
    spec.merged = 0;
    CHECK(Fails(Decode(allocator, Psd(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    spec.merged = 1;
    CHECK(Decode(allocator, Psd(allocator, spec).View()).ok);
    spec.merged = -1;
    spec.compression = 2;
    CHECK(Fails(Decode(allocator, Psd(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    spec.compression = 0;
    spec.mode = 4;
    CHECK(Fails(Decode(allocator, Psd(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

// 판별(서명·TGA 확장자), A5f·A5g 형식은 아직 UnsupportedSourceFormat, WebP 손실 LossySource·무손실
// UnsupportedSourceFormat·VP8X는 뒤 chunk로, 모르는 형식, 한도(한 변) CapacityExceeded, 할당 실패.
TEST(ImageDecode_DetectionLimitsAndFailures) {
    SystemAllocator allocator;
    const uint8 dds[] = {'D', 'D', 'S', ' ', 0};
    CHECK(DetectImageFileFormat(dds) == ImageFileFormat::Dds);
    CHECK(Fails(Decode(allocator, dds), AssetDiagnosticCode::UnsupportedSourceFormat));
    const uint8 jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0};
    CHECK(DetectImageFileFormat(jpeg) == ImageFileFormat::Jpeg);
    const uint8 unknown[] = {1, 2, 3, 4};
    CHECK(DetectImageFileFormat(unknown) == ImageFileFormat::Unknown);
    CHECK(DetectImageFileFormat(unknown, "Sky.TGA") == ImageFileFormat::Tga);
    CHECK(Fails(Decode(allocator, unknown), AssetDiagnosticCode::UnsupportedSourceFormat));

    const auto webp = [&](const char* chunk) noexcept {
        Bytes file(allocator);
        file.Text("RIFF");
        file.Le32(20);
        file.Text("WEBP");
        if (StringView(chunk) == "VP8X") {
            file.Text("VP8X");
            file.Le32(10);
            for (uint32 index = 0; index < 10; ++index) {
                file.U8(0);
            }

            chunk = "VP8L";
        }

        file.Text(chunk);
        file.Le32(2);
        file.U8(0);
        file.U8(0);
        return Move(file.data);
    };
    CHECK(Fails(Decode(allocator, webp("VP8 ").View()), AssetDiagnosticCode::LossySource));
    CHECK(Fails(Decode(allocator, webp("VP8L").View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    CHECK(Fails(Decode(allocator, webp("VP8X").View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));

    // 한도: 한 변 1보다 큰 그림.
    const uint8 rows[] = {0, 0, 255, 0, 255, 0, 0, 0};
    const Array<uint8> wide = Bmp(allocator, 40, 2, 1, 24, 0, {}, {}, rows);
    ImageDecodeLimits tight;
    tight.maxDimension = 1;
    const Decoded limited = Decode(allocator, wide.View(), ImageFileFormat::Unknown, tight);
    CHECK(!limited.ok && limited.result.failure.code == AssetDiagnosticCode::InvalidSource &&
          limited.result.failure.error == ErrorCode::CapacityExceeded);

    // 할당 실패: 모든 시도가 OutOfMemory(진단 None)이고 마지막은 성공한다(PNG — 압축 해제 포함).
    const uint8 gray[] = {0, 10, 200};
    PngSpec spec;
    spec.width = 2;
    spec.raw = gray;
    const Array<uint8> png = Png(allocator, spec);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        ImageDecodeResult result(failing);
        auto decoded = DecodeImage(failing, png.View(), ImageFileFormat::Png, {}, result);
        const bool exhausted = failing.Attempts() < attempt;
        failing.DisableFailure();
        if (exhausted) {
            CHECK(decoded);
            break;
        }

        CHECK(!decoded && decoded.Error() == ErrorCode::OutOfMemory &&
              result.failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures > 0);
}

} // namespace image_decode_tests_file
