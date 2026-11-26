#include "ImageFuzz.h"

#include <Core/Math/Float16.h>
#include <Core/Memory/SystemAllocator.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// DDS 디코더 시험이다(Docs/Design/AssetSystem 12.10절 A5f). 시험 안의 생성기가 DX10·레거시 헤더
// 파일을 만든다: 형식 표 전부(값 보존 대응·UE 비정확 대응 포함), encoding(UE ImportDDS 규칙),
// 차원(1D·2D·Cube· 2DArray·CubeArray·Volume), mip 0만 쓰기(slice마다 mip 사슬
// 건너뛰기)·IgnoredSourceMips, UE Validate 규칙, 블록 압축 LossySource·SNORM
// UnsupportedSourceFormat, float 정리, 한도, 절단 전수.
using namespace rocks;
using namespace rocks::image;
namespace dds_decode_tests_file {

namespace {

constexpr uint32 DdsdMipmapCount = 0x20000;
constexpr uint32 DdpfAlpha = 0x2;
constexpr uint32 DdpfFourCc = 0x4;
constexpr uint32 DdpfRgb = 0x40;
constexpr uint32 DdpfLuminance = 0x20000;
constexpr uint32 DdsCapsMipmap = 0x400000;
constexpr uint32 DdsCaps2Cubemap = 0x200;
constexpr uint32 DdsCaps2AllFaces = 0xFC00;
constexpr uint32 DdsCaps2PositiveX = 0x400;
constexpr uint32 DdsCaps2Volume = 0x200000;
constexpr uint32 Texture1D = 2;
constexpr uint32 Texture2D = 3;
constexpr uint32 Texture3D = 4;
constexpr uint32 MiscTextureCube = 0x4;

constexpr uint32 FourCc(char a, char b, char c, char d) noexcept {
    return static_cast<uint32>(static_cast<uint8>(a)) |
           (static_cast<uint32>(static_cast<uint8>(b)) << 8) |
           (static_cast<uint32>(static_cast<uint8>(c)) << 16) |
           (static_cast<uint32>(static_cast<uint8>(d)) << 24);
}

// DDS 파일 설명이다. dxgi가 0이 아니면 DX10 헤더, 아니면 레거시 픽셀
// 형식(pixelFlags·fourCc·마스크).
struct DdsSpec {
    uint32 width = 1;
    uint32 height = 1;
    uint32 depth = 0;
    uint32 mipCount = 1;
    bool mipFlag = false;
    uint32 caps2 = 0;
    uint32 dxgi = 0;
    uint32 resourceDimension = Texture2D;
    uint32 miscFlag = 0;
    uint32 arraySize = 1;
    uint32 pixelFlags = 0;
    uint32 fourCc = 0;
    uint32 bitCount = 0;
    uint32 masks[4] = {};
    Span<const uint8> data;
};

Array<uint8> Dds(Allocator& allocator, const DdsSpec& spec) noexcept {
    Array<uint8> file(allocator);
    const auto le32 = [&file](uint32 value) noexcept {
        for (uint32 shift = 0; shift < 32; shift += 8) {
            CHECK(file.TryPushBack(static_cast<uint8>(value >> shift)));
        }
    };
    const bool dx10 = spec.dxgi != 0;
    le32(FourCc('D', 'D', 'S', ' '));
    le32(124);
    le32(spec.mipFlag ? DdsdMipmapCount : 0);
    le32(spec.height);
    le32(spec.width);
    le32(0);
    le32(spec.depth);
    le32(spec.mipCount);
    for (uint32 index = 0; index < 11; ++index) {
        le32(0);
    }

    le32(32);
    le32(dx10 ? DdpfFourCc : spec.pixelFlags);
    le32(dx10 ? FourCc('D', 'X', '1', '0') : spec.fourCc);
    le32(spec.bitCount);
    for (uint32 mask : spec.masks) {
        le32(mask);
    }

    le32(spec.mipFlag ? DdsCapsMipmap : 0);
    le32(spec.caps2);
    le32(0);
    le32(0);
    le32(0);
    if (dx10) {
        le32(spec.dxgi);
        le32(spec.resourceDimension);
        le32(spec.miscFlag);
        le32(spec.arraySize);
        le32(0);
    }

    CHECK(file.TryAppend(spec.data));
    return file;
}

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file,
               ImageDecodeLimits limits = {}) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, ImageFileFormat::Unknown, limits, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

bool SamePixels(const Decoded& decoded, Span<const uint8> expected) noexcept {
    const DecodedImage& image = decoded.result.image;
    if (!decoded.ok || image.pixels.Size() != expected.Size()) {
        return false;
    }

    return platform::MemoryEquals(image.pixels.Data(), expected.Data(), expected.Size());
}

bool HasWarning(const Decoded& decoded, AssetDiagnosticCode code, uint32 detail) noexcept {
    for (const ImageDecodeWarning& warning : decoded.result.warnings) {
        if (warning.code == code && warning.detail == detail) {
            return true;
        }
    }

    return false;
}

// DXGI 형식 한 칸: 원본 texel byte → 기대 결과(형식·byte·encoding).
struct DxgiCase {
    uint32 dxgi;
    Span<const uint8> source;
    TextureSourceFormat format;
    Span<const uint8> expected;
    ImageColorEncoding encoding;
};

} // namespace

// DX10 형식 표(UE DXGIRawFormatMap 중 값 보존 대응)와 encoding(UE ImportDDS 3434–3460).
TEST(DdsDecode_Dx10Formats) {
    SystemAllocator allocator;
    const uint8 gray[] = {77};
    const uint8 rg[] = {10, 20};
    const uint8 rgOut[] = {10, 20, 0, 255};
    const uint8 rgba[] = {1, 2, 3, 4};
    const uint8 bgra[] = {3, 2, 1, 4};
    const uint8 bgrxOut[] = {1, 2, 3, 255};
    const uint8 g16[] = {0x34, 0x12};
    const uint8 rgba16[] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8 r16f[] = {0x00, 0x3C};
    const uint8 rgba16f[] = {0x00, 0x3C, 0x00, 0x40, 0x00, 0x42, 0x00, 0x44};
    const uint8 r32f[] = {0x00, 0x00, 0x80, 0x3F};
    const uint8 rgba32f[] = {0, 0, 0x80, 0x3F, 0, 0, 0, 0x40, 0, 0, 0x40, 0x40, 0, 0, 0x80, 0x40};
    constexpr ImageColorEncoding Srgb = ImageColorEncoding::Srgb;
    constexpr ImageColorEncoding Linear = ImageColorEncoding::Linear;
    const DxgiCase cases[] = {
        {61, gray, TextureSourceFormat::G8, gray, Srgb},         // R8_UNORM(쌍 없음 → Srgb)
        {60, gray, TextureSourceFormat::G8, gray, Srgb},         // R8_TYPELESS
        {62, gray, TextureSourceFormat::G8, gray, Srgb},         // R8_UINT
        {65, gray, TextureSourceFormat::G8, gray, Srgb},         // A8_UNORM(UE 비정확 — 값 보존)
        {49, rg, TextureSourceFormat::RGBA8, rgOut, Srgb},       // R8G8_UNORM → (r, g, 0, 255)
        {48, rg, TextureSourceFormat::RGBA8, rgOut, Srgb},       // R8G8_TYPELESS
        {50, rg, TextureSourceFormat::RGBA8, rgOut, Srgb},       // R8G8_UINT
        {28, rgba, TextureSourceFormat::RGBA8, rgba, Linear},    // R8G8B8A8_UNORM(쌍 → Linear)
        {29, rgba, TextureSourceFormat::RGBA8, rgba, Srgb},      // R8G8B8A8_UNORM_SRGB
        {27, rgba, TextureSourceFormat::RGBA8, rgba, Srgb},      // R8G8B8A8_TYPELESS(쌍 없음)
        {30, rgba, TextureSourceFormat::RGBA8, rgba, Srgb},      // R8G8B8A8_UINT(UE는 R·B가 바뀐다)
        {87, bgra, TextureSourceFormat::RGBA8, rgba, Linear},    // B8G8R8A8_UNORM
        {91, bgra, TextureSourceFormat::RGBA8, rgba, Srgb},      // B8G8R8A8_UNORM_SRGB
        {90, bgra, TextureSourceFormat::RGBA8, rgba, Srgb},      // B8G8R8A8_TYPELESS
        {88, bgra, TextureSourceFormat::RGBA8, bgrxOut, Linear}, // B8G8R8X8_UNORM(X → 255)
        {93, bgra, TextureSourceFormat::RGBA8, bgrxOut, Srgb},   // B8G8R8X8_UNORM_SRGB
        {92, bgra, TextureSourceFormat::RGBA8, bgrxOut, Srgb},   // B8G8R8X8_TYPELESS
        {56, g16, TextureSourceFormat::G16, g16, Linear},        // R16_UNORM
        {53, g16, TextureSourceFormat::G16, g16, Linear},        // R16_TYPELESS
        {57, g16, TextureSourceFormat::G16, g16, Linear},        // R16_UINT
        {11, rgba16, TextureSourceFormat::RGBA16, rgba16, Linear}, // R16G16B16A16_UNORM
        {12, rgba16, TextureSourceFormat::RGBA16, rgba16, Linear}, // R16G16B16A16_UINT
        {54, r16f, TextureSourceFormat::R16F, r16f, Linear},
        {10, rgba16f, TextureSourceFormat::RGBA16F, rgba16f, Linear},
        {41, r32f, TextureSourceFormat::R32F, r32f, Linear},
        {2, rgba32f, TextureSourceFormat::RGBA32F, rgba32f, Linear}};
    for (const DxgiCase& item : cases) {
        DdsSpec spec;
        spec.dxgi = item.dxgi;
        spec.data = item.source;
        const Array<uint8> file = Dds(allocator, spec);
        const Decoded decoded = Decode(allocator, file.View());
        CHECK(decoded.ok && decoded.result.image.format == item.format &&
              decoded.result.image.encoding == item.encoding &&
              decoded.result.image.dimension == TextureDimension::Texture2D);
        CHECK(SamePixels(decoded, item.expected));
        CHECK(decoded.result.warnings.Size() == 0);
    }

    // X 채널은 모든 texel이 255다(UE ConvertRGBXtoRGBA는 첫 texel만 255로 쓴다 — UE와 다르다).
    const uint8 bgrxTexels[] = {3, 2, 1, 7, 6, 5, 4, 9, 9, 8, 7, 11};
    const uint8 bgrxTexelsOut[] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255};
    DdsSpec xSpec;
    xSpec.width = 3;
    xSpec.dxgi = 88;
    xSpec.data = bgrxTexels;
    CHECK(SamePixels(Decode(allocator, Dds(allocator, xSpec).View()), bgrxTexelsOut));

    // 블록 압축(BC1·BC5·BC6H·BC7)은 LossySource, SNORM·SINT·R32_UINT·모르는 값은
    // UnsupportedSourceFormat.
    const uint32 lossy[] = {71, 72, 83, 95, 98, 99};
    for (uint32 dxgi : lossy) {
        DdsSpec spec;
        spec.dxgi = dxgi;
        spec.data = rgba;
        CHECK(Fails(Decode(allocator, Dds(allocator, spec).View()),
                    AssetDiagnosticCode::LossySource));
    }

    const uint32 unsupported[] = {31, 32, 13, 14, 42, 43, 51, 58, 63, 24, 9, 200};
    for (uint32 dxgi : unsupported) {
        DdsSpec spec;
        spec.dxgi = dxgi;
        spec.data = rgba16;
        CHECK(Fails(Decode(allocator, Dds(allocator, spec).View()),
                    AssetDiagnosticCode::UnsupportedSourceFormat));
    }
}

// 레거시 헤더: UE BitmaskToDXGITable·FOURCCToDXGITable 중 대응표에 있는 것, D3D9은 8비트면 Srgb.
TEST(DdsDecode_LegacyFormats) {
    SystemAllocator allocator;
    struct MaskCase {
        uint32 flags;
        uint32 bits;
        uint32 masks[4];
        Span<const uint8> source;
        TextureSourceFormat format;
        Span<const uint8> expected;
    };

    const uint8 abgr[] = {1, 2, 3, 4};
    const uint8 argb[] = {3, 2, 1, 4};
    const uint8 xrgbOut[] = {1, 2, 3, 255};
    const uint8 rgb24[] = {1, 2, 3};
    const uint8 bgr24[] = {3, 2, 1};
    const uint8 luminance[] = {99};
    const uint8 l16[] = {0x01, 0x80};
    const uint8 l8a8[] = {40, 50};
    const uint8 l8a8Out[] = {40, 50, 0, 255};
    const uint8 r32[] = {0, 0, 0x80, 0x3F};
    const MaskCase masks[] = {
        {DdpfRgb,
         32,
         {0xff, 0xff00, 0xff0000, 0xff000000u},
         abgr,
         TextureSourceFormat::RGBA8,
         abgr},
        {DdpfRgb,
         32,
         {0xff0000, 0xff00, 0xff, 0xff000000u},
         argb,
         TextureSourceFormat::RGBA8,
         abgr},
        {DdpfRgb, 32, {0xff0000, 0xff00, 0xff, 0}, argb, TextureSourceFormat::RGBA8, xrgbOut},
        {DdpfRgb, 32, {0xff, 0xff00, 0xff0000, 0}, abgr, TextureSourceFormat::RGBA8, xrgbOut},
        {DdpfRgb, 32, {NumericLimits<uint32>::Max(), 0, 0, 0}, r32, TextureSourceFormat::R32F, r32},
        {DdpfRgb, 24, {0xff, 0xff00, 0xff0000, 0}, rgb24, TextureSourceFormat::RGBA8, xrgbOut},
        {DdpfRgb, 24, {0xff0000, 0xff00, 0xff, 0}, bgr24, TextureSourceFormat::RGBA8, xrgbOut},
        {DdpfLuminance, 8, {0xff, 0, 0, 0}, luminance, TextureSourceFormat::G8, luminance},
        {DdpfLuminance, 16, {0xffff, 0, 0, 0}, l16, TextureSourceFormat::G16, l16},
        {DdpfLuminance, 16, {0xff, 0, 0, 0xff00}, l8a8, TextureSourceFormat::RGBA8, l8a8Out},
        {DdpfLuminance, 8, {0xff, 0, 0, 0xff00}, l8a8, TextureSourceFormat::RGBA8, l8a8Out},
        {DdpfAlpha, 8, {0, 0, 0, 0xff}, luminance, TextureSourceFormat::G8, luminance}};
    for (const MaskCase& item : masks) {
        DdsSpec spec;
        spec.pixelFlags = item.flags;
        spec.bitCount = item.bits;
        for (uint32 index = 0; index < 4; ++index) {
            spec.masks[index] = item.masks[index];
        }

        spec.data = item.source;
        const Decoded decoded = Decode(allocator, Dds(allocator, spec).View());
        CHECK(decoded.ok && decoded.result.image.format == item.format);
        CHECK(SamePixels(decoded, item.expected));
        const bool eightBit =
            item.format == TextureSourceFormat::RGBA8 || item.format == TextureSourceFormat::G8;
        CHECK(decoded.result.image.encoding ==
              (eightBit ? ImageColorEncoding::Srgb : ImageColorEncoding::Linear));
    }

    // UE 대응표에 없는 마스크(565·5551·4444·R10G10B10A2·R16G16·BUMPDUDV)는 받지 않는다.
    const uint32 rejected[][6] = {{DdpfRgb, 16, 0xf800, 0x07e0, 0x001f, 0},
                                  {DdpfRgb, 16, 0x7c00, 0x03e0, 0x001f, 0x8000},
                                  {DdpfRgb, 16, 0x0f00, 0x00f0, 0x000f, 0xf000},
                                  {DdpfRgb, 32, 0x3ff00000, 0x000ffc00, 0x000003ff, 0xc0000000u},
                                  {DdpfRgb, 32, 0x0000ffff, 0xffff0000u, 0, 0},
                                  {0x80000, 32, 0xff, 0xff00, 0xff0000, 0xff000000u}};
    for (const auto& item : rejected) {
        DdsSpec spec;
        spec.pixelFlags = item[0];
        spec.bitCount = item[1];
        for (uint32 index = 0; index < 4; ++index) {
            spec.masks[index] = item[2 + index];
        }

        spec.data = abgr;
        CHECK(Fails(Decode(allocator, Dds(allocator, spec).View()),
                    AssetDiagnosticCode::UnsupportedSourceFormat));
    }

    // FourCC: 36·111·113·114·116은 받고, DXT·BC·ATI는 LossySource, 110·112·115·모르는 값은 받지
    // 않는다.
    const uint8 rgba16[] = {1, 0, 2, 0, 3, 0, 4, 0};
    struct FourCcCase {
        uint32 fourCc;
        TextureSourceFormat format;
    };

    const FourCcCase accepted[] = {{36, TextureSourceFormat::RGBA16},
                                   {111, TextureSourceFormat::R16F},
                                   {113, TextureSourceFormat::RGBA16F},
                                   {114, TextureSourceFormat::R32F},
                                   {116, TextureSourceFormat::RGBA32F}};
    const uint8 floats[16] = {0, 0, 0x80, 0x3F, 0, 0, 0x80, 0x3F,
                              0, 0, 0x80, 0x3F, 0, 0, 0x80, 0x3F};
    for (const FourCcCase& item : accepted) {
        DdsSpec spec;
        spec.pixelFlags = DdpfFourCc;
        spec.fourCc = item.fourCc;
        spec.data = item.fourCc == 116 ? Span<const uint8>(floats) : Span<const uint8>(rgba16);
        const Decoded decoded = Decode(allocator, Dds(allocator, spec).View());
        CHECK(decoded.ok && decoded.result.image.format == item.format &&
              decoded.result.image.encoding == ImageColorEncoding::Linear);
    }

    const uint32 block[] = {FourCc('D', 'X', 'T', '1'), FourCc('D', 'X', 'T', '5'),
                            FourCc('A', 'T', 'I', '2'), FourCc('B', 'C', '4', 'U'),
                            FourCc('B', 'C', '7', 'L')};
    for (uint32 fourCc : block) {
        DdsSpec spec;
        spec.pixelFlags = DdpfFourCc;
        spec.fourCc = fourCc;
        spec.data = rgba16;
        CHECK(Fails(Decode(allocator, Dds(allocator, spec).View()),
                    AssetDiagnosticCode::LossySource));
    }

    const uint32 other[] = {110, 112, 115, FourCc('A', 'B', 'C', 'D')};
    for (uint32 fourCc : other) {
        DdsSpec spec;
        spec.pixelFlags = DdpfFourCc;
        spec.fourCc = fourCc;
        spec.data = floats;
        CHECK(Fails(Decode(allocator, Dds(allocator, spec).View()),
                    AssetDiagnosticCode::UnsupportedSourceFormat));
    }
}

// 차원과 mip: slice마다 mip 사슬을 건너뛰고 mip 0만 쓴다(UE 3418–3421), Volume mip은 깊이도
// 절반이다.
TEST(DdsDecode_DimensionsAndMips) {
    SystemAllocator allocator;
    // 2DArray 2 × (2×2 mip 0 + 1×1 mip 1), R8_UNORM: slice 0 = 1 2 3 4 | 5, slice 1 = 6 7 8 9 | 10.
    const uint8 array[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    DdsSpec spec;
    spec.width = 2;
    spec.height = 2;
    spec.mipFlag = true;
    spec.mipCount = 2;
    spec.dxgi = 61;
    spec.arraySize = 2;
    spec.data = array;
    Decoded decoded = Decode(allocator, Dds(allocator, spec).View());
    const uint8 arrayOut[] = {1, 2, 3, 4, 6, 7, 8, 9};
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::Texture2DArray &&
          decoded.result.image.numSlices == 2 && decoded.result.image.sourceMips == 2);
    CHECK(SamePixels(decoded, arrayOut));
    CHECK(HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceMips, 1));

    // DX10 cube(arraySize 1 → 면 6), cube array(arraySize 2 → 12).
    uint8 faces[12] = {};
    for (uint32 index = 0; index < 12; ++index) {
        faces[index] = static_cast<uint8>(index + 1);
    }

    spec = DdsSpec{};
    spec.dxgi = 61;
    spec.miscFlag = MiscTextureCube;
    spec.data = Span<const uint8>(faces, 6);
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::TextureCube &&
          decoded.result.image.numSlices == 6);
    CHECK(SamePixels(decoded, Span<const uint8>(faces, 6)));
    spec.arraySize = 2;
    spec.data = faces;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::TextureCubeArray &&
          decoded.result.image.numSlices == 12);
    CHECK(SamePixels(decoded, faces));

    // 레거시 cube: 면 비트가 모두 있어야 한다(일부면 UnsupportedSourceFormat — UE는 비트를 보지
    // 않는다).
    spec = DdsSpec{};
    spec.pixelFlags = DdpfLuminance;
    spec.bitCount = 8;
    spec.masks[0] = 0xff;
    spec.caps2 = DdsCaps2Cubemap | DdsCaps2AllFaces;
    spec.data = Span<const uint8>(faces, 6);
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::TextureCube);
    spec.caps2 = DdsCaps2Cubemap | DdsCaps2PositiveX;
    CHECK(Fails(Decode(allocator, Dds(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));

    // Volume 2×2×2 + mip 1(1×1×1): 깊이 slice 2개.
    const uint8 volume[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    spec = DdsSpec{};
    spec.width = 2;
    spec.height = 2;
    spec.depth = 2;
    spec.mipFlag = true;
    spec.mipCount = 2;
    spec.dxgi = 61;
    spec.resourceDimension = Texture3D;
    spec.data = volume;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::Volume &&
          decoded.result.image.numSlices == 2);
    CHECK(SamePixels(decoded, Span<const uint8>(volume, 8)));
    CHECK(HasWarning(decoded, AssetDiagnosticCode::IgnoredSourceMips, 1));
    // 레거시 Volume(caps2 VOLUME).
    spec.dxgi = 0;
    spec.pixelFlags = DdpfLuminance;
    spec.bitCount = 8;
    spec.masks[0] = 0xff;
    spec.caps2 = DdsCaps2Volume;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::Volume);
    // mip 하나가 모자라면(데이터 끝) 손상이다(UE ReadPayload).
    spec.data = Span<const uint8>(volume, 8);
    CHECK(
        Fails(Decode(allocator, Dds(allocator, spec).View()), AssetDiagnosticCode::InvalidSource));

    // 1D는 높이 1의 Texture2D다(높이가 1이 아니면 손상).
    const uint8 line[] = {5, 6, 7};
    spec = DdsSpec{};
    spec.width = 3;
    spec.dxgi = 61;
    spec.resourceDimension = Texture1D;
    spec.data = line;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.dimension == TextureDimension::Texture2D &&
          decoded.result.image.width == 3 && decoded.result.image.height == 1);
    spec.height = 2;
    CHECK(
        Fails(Decode(allocator, Dds(allocator, spec).View()), AssetDiagnosticCode::InvalidSource));

    // DDSCAPS_MIPMAP이 없으면 mip 수 칸을 무시한다(mip 1).
    spec = DdsSpec{};
    spec.dxgi = 61;
    spec.mipCount = 5;
    spec.data = line;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok && decoded.result.image.sourceMips == 1 &&
          decoded.result.warnings.Size() == 0);
}

// UE FDDSFile::Validate 규칙과 손상(모두 InvalidSource), 한도, float 정리, 절단 전수.
TEST(DdsDecode_ValidationAndLimits) {
    SystemAllocator allocator;
    const uint8 data[64] = {};
    const auto make = [&](auto&& edit) noexcept {
        DdsSpec spec;
        spec.width = 2;
        spec.height = 2;
        spec.dxgi = 61;
        spec.data = data;
        edit(spec);
        return Dds(allocator, spec);
    };
    // mip 수 0, 과다 mip(2×2에 mip 3), arraySize 0, 정사각형이 아닌 cube, 1D 높이, 3D 배열, 차원
    // 값.
    CHECK(Fails(Decode(allocator, make([](DdsSpec& s) noexcept {
                                      s.mipFlag = true;
                                      s.mipCount = 0;
                                  }).View()),
                AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(Decode(allocator, make([](DdsSpec& s) noexcept {
                                      s.mipFlag = true;
                                      s.mipCount = 3;
                                  }).View()),
                AssetDiagnosticCode::InvalidSource));
    CHECK(Decode(allocator, make([](DdsSpec& s) noexcept {
                                s.mipFlag = true;
                                s.mipCount = 2;
                            }).View())
              .ok);
    CHECK(Fails(Decode(allocator, make([](DdsSpec& s) noexcept { s.arraySize = 0; }).View()),
                AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(Decode(allocator, make([](DdsSpec& s) noexcept {
                                      s.width = 4;
                                      s.miscFlag = MiscTextureCube;
                                  }).View()),
                AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(Decode(allocator, make([](DdsSpec& s) noexcept {
                                      s.resourceDimension = Texture3D;
                                      s.depth = 1;
                                      s.arraySize = 2;
                                  }).View()),
                AssetDiagnosticCode::InvalidSource));
    CHECK(
        Fails(Decode(allocator, make([](DdsSpec& s) noexcept { s.resourceDimension = 5; }).View()),
              AssetDiagnosticCode::InvalidSource));
    CHECK(Fails(Decode(allocator, make([](DdsSpec& s) noexcept { s.width = 0; }).View()),
                AssetDiagnosticCode::InvalidSource));

    // 한도: slice 수, 깊이, slice를 곱한 결과 byte.
    ImageDecodeLimits limits;
    limits.maxSlices = 1;
    Decoded decoded =
        Decode(allocator, make([](DdsSpec& s) noexcept { s.arraySize = 2; }).View(), limits);
    CHECK(!decoded.ok && decoded.result.failure.code == AssetDiagnosticCode::InvalidSource &&
          decoded.result.failure.error == ErrorCode::CapacityExceeded);
    limits = ImageDecodeLimits{};
    limits.maxPixelBytes = 7;
    decoded = Decode(allocator, make([](DdsSpec& s) noexcept { s.arraySize = 2; }).View(), limits);
    CHECK(!decoded.ok && decoded.result.failure.error == ErrorCode::CapacityExceeded);
    limits.maxPixelBytes = 8;
    CHECK(Decode(allocator, make([](DdsSpec& s) noexcept { s.arraySize = 2; }).View(), limits).ok);
    limits = ImageDecodeLimits{};
    limits.maxDimension = 1;
    decoded = Decode(allocator, make([](DdsSpec& s) noexcept {
                                    s.width = 1;
                                    s.height = 1;
                                    s.resourceDimension = Texture3D;
                                    s.depth = 2;
                                }).View(),
                     limits);
    CHECK(!decoded.ok && decoded.result.failure.error == ErrorCode::CapacityExceeded);

    // float 정리: R32F NaN → 0, +Inf → float 최대, R16F −Inf → −65504.
    const uint8 nanInf[] = {0x00, 0x00, 0xC0, 0x7F, 0x00, 0x00, 0x80, 0x7F};
    DdsSpec spec;
    spec.width = 2;
    spec.dxgi = 41;
    spec.data = nanInf;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    CHECK(decoded.ok);
    if (decoded.ok) {
        const uint8* pixels = decoded.result.image.pixels.Data();
        CHECK(platform::BitCast<float>(
                  static_cast<uint32>(pixels[0] | pixels[1] << 8 | pixels[2] << 16 |
                                      static_cast<uint32>(pixels[3]) << 24)) == 0.0f);
        CHECK(platform::BitCast<float>(static_cast<uint32>(
                  pixels[4] | pixels[5] << 8 | pixels[6] << 16 |
                  static_cast<uint32>(pixels[7]) << 24)) == NumericLimits<float>::Max());
    }

    const uint8 negativeInf[] = {0x00, 0xFC};
    spec.width = 1;
    spec.dxgi = 54;
    spec.data = negativeInf;
    decoded = Decode(allocator, Dds(allocator, spec).View());
    const uint8 negativeMax[] = {0xFF, 0xFB};
    CHECK(SamePixels(decoded, negativeMax));

    // 절단 전수: 모든 앞부분은 실패한다(DX10·레거시).
    const Array<uint8> dx10 = make([](DdsSpec&) noexcept {});
    for (usize length = 0; length + 60 < dx10.Size(); ++length) {
        CHECK(!Decode(allocator, dx10.View().Slice(0, length)).ok);
    }

    spec = DdsSpec{};
    spec.width = 2;
    spec.height = 2;
    spec.pixelFlags = DdpfLuminance;
    spec.bitCount = 8;
    spec.masks[0] = 0xff;
    spec.data = Span<const uint8>(data, 4);
    const Array<uint8> legacy = Dds(allocator, spec);
    for (usize length = 0; length < legacy.Size(); ++length) {
        CHECK(!Decode(allocator, legacy.View().Slice(0, length)).ok);
    }
}

using image_codec_test::FuzzDecode;
using image_codec_test::FuzzStats;

// 고정 seed 변조 fuzz(Docs/Design/AssetSystem 14.1.119 후속 3번 — ImageFuzz.h): DDS(DX10
// RGBA8·RGBA16F, 옛 머리 BGRA mask, Cube, 2DArray, mip 사슬) 씨앗. 변조한 파일은 결과가 서로 맞는
// 성공이거나 다섯 분류의 실패이고, 할당 블록을 다 돌려준다.
TEST(DdsDecode_FixedSeedMutationFuzz) {
    SystemAllocator allocator;
    const uint8 texels[64] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                              17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
                              33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
                              49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64};
    DdsSpec rgba;
    rgba.width = 2;
    rgba.height = 2;
    rgba.dxgi = 28;
    rgba.data = Span<const uint8>(texels, 16);
    const Array<uint8> dx10 = Dds(allocator, rgba);
    DdsSpec half = rgba;
    half.dxgi = 10;
    half.data = Span<const uint8>(texels, 32);
    const Array<uint8> half16 = Dds(allocator, half);
    DdsSpec legacy;
    legacy.width = 2;
    legacy.height = 2;
    legacy.pixelFlags = 0x41;
    legacy.bitCount = 32;
    legacy.masks[0] = 0x00FF0000u;
    legacy.masks[1] = 0x0000FF00u;
    legacy.masks[2] = 0x000000FFu;
    legacy.masks[3] = 0xFF000000u;
    legacy.data = Span<const uint8>(texels, 16);
    const Array<uint8> bgra = Dds(allocator, legacy);
    DdsSpec array = rgba;
    array.width = 1;
    array.height = 1;
    array.arraySize = 4;
    const Array<uint8> slices = Dds(allocator, array);
    DdsSpec mips = rgba;
    mips.width = 4;
    mips.height = 2;
    mips.mipCount = 3;
    mips.mipFlag = true;
    mips.data = Span<const uint8>(texels, 44);
    const Array<uint8> chain = Dds(allocator, mips);
    const Span<const uint8> seeds[] = {dx10.View(), half16.View(), bgra.View(), slices.View(),
                                       chain.View()};
    FuzzStats stats;
    CHECK(FuzzDecode(allocator, seeds, ImageFileFormat::Dds, 0x5EED0DD5ull, 4000, stats));
    CHECK(stats.Clean() && stats.rejected > 0);
}

} // namespace dds_decode_tests_file
