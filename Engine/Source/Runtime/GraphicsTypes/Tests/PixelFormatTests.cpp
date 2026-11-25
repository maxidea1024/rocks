#include <GraphicsTypes/PixelFormat.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace pixel_format_tests_file {

using namespace rocks::graphics;

namespace {

// Docs/Design/PixelFormat 5.2절 번호표 그대로다(번호·이름·종류·블록·바이트·채널 수). 블록
// 크기·바이트는 UE 5.8 GPixelFormats(Core/Private/Misc/PixelFormat.cpp:31–150)와 같다(깊이·
// 스텐실·비디오는 Rocks 정의).
struct Row {
    uint32 number;
    const char* name;
    PixelFormatKind kind;
    uint32 blockWidth;
    uint32 blockHeight;
    uint32 blockBytes;
    uint32 channelCount;
};

using K = PixelFormatKind;

constexpr Row Table[] = {
    {1, "R8Unorm", K::Color, 1, 1, 1, 1},
    {2, "R8UnormSrgb", K::Color, 1, 1, 1, 1},
    {3, "R8Uint", K::Color, 1, 1, 1, 1},
    {4, "R8Sint", K::Color, 1, 1, 1, 1},
    {5, "A8Unorm", K::Color, 1, 1, 1, 1},
    {6, "RG8Unorm", K::Color, 1, 1, 2, 2},
    {7, "RG8UnormSrgb", K::Color, 1, 1, 2, 2},
    {8, "RG8Snorm", K::Color, 1, 1, 2, 2},
    {9, "RG8Uint", K::Color, 1, 1, 2, 2},
    {10, "RGB8Unorm", K::Color, 1, 1, 3, 3},
    {11, "RGB8UnormSrgb", K::Color, 1, 1, 3, 3},
    {12, "RGBA8Unorm", K::Color, 1, 1, 4, 4},
    {13, "RGBA8UnormSrgb", K::Color, 1, 1, 4, 4},
    {14, "RGBA8Snorm", K::Color, 1, 1, 4, 4},
    {15, "RGBA8Uint", K::Color, 1, 1, 4, 4},
    {16, "BGRA8Unorm", K::Color, 1, 1, 4, 4},
    {17, "BGRA8UnormSrgb", K::Color, 1, 1, 4, 4},
    {18, "R16Unorm", K::Color, 1, 1, 2, 1},
    {19, "R16Uint", K::Color, 1, 1, 2, 1},
    {20, "R16Sint", K::Color, 1, 1, 2, 1},
    {21, "R16Float", K::Color, 1, 1, 2, 1},
    {22, "RG16Unorm", K::Color, 1, 1, 4, 2},
    {23, "RG16Snorm", K::Color, 1, 1, 4, 2},
    {24, "RG16Uint", K::Color, 1, 1, 4, 2},
    {25, "RG16Sint", K::Color, 1, 1, 4, 2},
    {26, "RG16Float", K::Color, 1, 1, 4, 2},
    {27, "RGBA16Unorm", K::Color, 1, 1, 8, 4},
    {28, "RGBA16Snorm", K::Color, 1, 1, 8, 4},
    {29, "RGBA16Uint", K::Color, 1, 1, 8, 4},
    {30, "RGBA16Sint", K::Color, 1, 1, 8, 4},
    {31, "RGBA16Float", K::Color, 1, 1, 8, 4},
    {32, "R32Uint", K::Color, 1, 1, 4, 1},
    {33, "R32Sint", K::Color, 1, 1, 4, 1},
    {34, "R32Float", K::Color, 1, 1, 4, 1},
    {35, "RG32Uint", K::Color, 1, 1, 8, 2},
    {36, "RG32Float", K::Color, 1, 1, 8, 2},
    {37, "RGB32Uint", K::Color, 1, 1, 12, 3},
    {38, "RGB32Sint", K::Color, 1, 1, 12, 3},
    {39, "RGB32Float", K::Color, 1, 1, 12, 3},
    {40, "RGBA32Uint", K::Color, 1, 1, 16, 4},
    {41, "RGBA32Float", K::Color, 1, 1, 16, 4},
    {42, "R64Uint", K::Color, 1, 1, 8, 1},
    {43, "R5G6B5Unorm", K::Color, 1, 1, 2, 3},
    {44, "B5G5R5A1Unorm", K::Color, 1, 1, 2, 4},
    {45, "RGB10A2Unorm", K::Color, 1, 1, 4, 4},
    {46, "RG11B10Ufloat", K::Color, 1, 1, 4, 3},
    {47, "RGB9E5Ufloat", K::Color, 1, 1, 4, 3},
    {64, "D16Unorm", K::Depth, 1, 1, 2, 1},
    {65, "D24Unorm", K::Depth, 1, 1, 4, 1},
    {66, "D32Float", K::Depth, 1, 1, 4, 1},
    {67, "D24UnormS8Uint", K::DepthStencil, 1, 1, 0, 2},
    {68, "D32FloatS8Uint", K::DepthStencil, 1, 1, 0, 2},
    {80, "BC1Unorm", K::Compressed, 4, 4, 8, 4},
    {81, "BC1UnormSrgb", K::Compressed, 4, 4, 8, 4},
    {82, "BC2Unorm", K::Compressed, 4, 4, 16, 4},
    {83, "BC2UnormSrgb", K::Compressed, 4, 4, 16, 4},
    {84, "BC3Unorm", K::Compressed, 4, 4, 16, 4},
    {85, "BC3UnormSrgb", K::Compressed, 4, 4, 16, 4},
    {86, "BC4Unorm", K::Compressed, 4, 4, 8, 1},
    {87, "BC5Unorm", K::Compressed, 4, 4, 16, 2},
    {88, "BC6HUfloat", K::Compressed, 4, 4, 16, 3},
    {89, "BC7Unorm", K::Compressed, 4, 4, 16, 4},
    {90, "BC7UnormSrgb", K::Compressed, 4, 4, 16, 4},
    {96, "ETC2RGB8Unorm", K::Compressed, 4, 4, 8, 3},
    {97, "ETC2RGB8UnormSrgb", K::Compressed, 4, 4, 8, 3},
    {98, "ETC2RGBA8Unorm", K::Compressed, 4, 4, 16, 4},
    {99, "ETC2RGBA8UnormSrgb", K::Compressed, 4, 4, 16, 4},
    {100, "EACR11Unorm", K::Compressed, 4, 4, 8, 1},
    {101, "EACRG11Unorm", K::Compressed, 4, 4, 16, 2},
    {112, "ASTC4x4Unorm", K::Compressed, 4, 4, 16, 4},
    {113, "ASTC4x4UnormSrgb", K::Compressed, 4, 4, 16, 4},
    {114, "ASTC4x4Hdr", K::Compressed, 4, 4, 16, 4},
    {115, "ASTC6x6Unorm", K::Compressed, 6, 6, 16, 4},
    {116, "ASTC6x6UnormSrgb", K::Compressed, 6, 6, 16, 4},
    {117, "ASTC6x6Hdr", K::Compressed, 6, 6, 16, 4},
    {118, "ASTC8x8Unorm", K::Compressed, 8, 8, 16, 4},
    {119, "ASTC8x8UnormSrgb", K::Compressed, 8, 8, 16, 4},
    {120, "ASTC8x8Hdr", K::Compressed, 8, 8, 16, 4},
    {121, "ASTC10x10Unorm", K::Compressed, 10, 10, 16, 4},
    {122, "ASTC10x10UnormSrgb", K::Compressed, 10, 10, 16, 4},
    {123, "ASTC10x10Hdr", K::Compressed, 10, 10, 16, 4},
    {124, "ASTC12x12Unorm", K::Compressed, 12, 12, 16, 4},
    {125, "ASTC12x12UnormSrgb", K::Compressed, 12, 12, 16, 4},
    {126, "ASTC12x12Hdr", K::Compressed, 12, 12, 16, 4},
    {160, "NV12", K::Planar, 1, 1, 0, 3},
    {161, "P010", K::Planar, 1, 1, 0, 3},
};

static_assert(sizeof(Table) / sizeof(Table[0]) == PixelFormatCount);

uint32 ChannelCount(uint8 channels) noexcept {
    uint32 count = 0;
    for (uint32 bit = 0; bit < 8; ++bit) {
        count += (channels >> bit) & 1u;
    }

    return count;
}

} // namespace

TEST(PixelFormat_TableNumbersNamesAndBlocks) {
    for (const Row& row : Table) {
        auto format = PixelFormatFromNumber(row.number);
        REQUIRE(format);
        CHECK(PixelFormatNumber(format.Value()) == row.number);
        CHECK(IsValidPixelFormat(format.Value()));
        const PixelFormatInfo& info = GetPixelFormatInfo(format.Value());
        CHECK(info.format == format.Value());
        CHECK(info.kind == row.kind);
        CHECK(info.blockWidth == row.blockWidth);
        CHECK(info.blockHeight == row.blockHeight);
        CHECK(info.blockBytes == row.blockBytes);
        CHECK(ChannelCount(info.channels) == row.channelCount);
        CHECK(platform::CStringCompare(PixelFormatName(format.Value()), row.name) == 0);
        CHECK(platform::CStringCompare(info.name, row.name) == 0);
        auto parsed = ParsePixelFormatName(StringView(row.name, platform::CStringLength(row.name)));
        REQUIRE(parsed);
        CHECK(parsed.Value() == format.Value());
    }
}

TEST(PixelFormat_NumberTableHasNoOtherValues) {
    uint32 valid = 0;
    for (uint32 number = 0; number < 256; ++number) {
        const auto format = static_cast<PixelFormat>(number);
        if (IsValidPixelFormat(format)) {
            ++valid;
            CHECK(PixelFormatFromNumber(number).IsOk());
        } else {
            CHECK(PixelFormatFromNumber(number).Error() == ErrorCode::InvalidArgument);
            CHECK(platform::CStringCompare(PixelFormatName(format), "Unknown") == 0);
            CHECK(GetPixelFormatInfo(format).format == PixelFormat::Unknown);
            CHECK(PixelFormatImageBytes(format, 4, 4) == 0);
        }
    }
    CHECK(valid == PixelFormatCount);
    CHECK(!IsValidPixelFormat(PixelFormat::Unknown));
    // 구간 사이의 빈 번호다.
    CHECK(PixelFormatFromNumber(48).Error() == ErrorCode::InvalidArgument);
    CHECK(PixelFormatFromNumber(69).Error() == ErrorCode::InvalidArgument);
    CHECK(PixelFormatFromNumber(91).Error() == ErrorCode::InvalidArgument);
    CHECK(PixelFormatFromNumber(102).Error() == ErrorCode::InvalidArgument);
    CHECK(PixelFormatFromNumber(127).Error() == ErrorCode::InvalidArgument);
    CHECK(PixelFormatFromNumber(MaxPixelFormatNumber + 1).Error() == ErrorCode::InvalidArgument);
}

TEST(PixelFormat_ParseRejectsUnknownNames) {
    CHECK(ParsePixelFormatName(StringView("Unknown", 7)).Error() == ErrorCode::NotFound);
    CHECK(ParsePixelFormatName(StringView("rgba8unorm", 10)).Error() == ErrorCode::NotFound);
    CHECK(ParsePixelFormatName(StringView("RGBA8", 5)).Error() == ErrorCode::NotFound);
    CHECK(ParsePixelFormatName(StringView("", 0)).Error() == ErrorCode::NotFound);
}

TEST(PixelFormat_InfoIsConsistent) {
    for (const Row& row : Table) {
        const auto format = static_cast<PixelFormat>(row.number);
        const PixelFormatInfo& info = GetPixelFormatInfo(format);
        // 블록이 1×1보다 큰 것은 압축 형식뿐이고 압축 형식은 계열을 가진다.
        const bool block = info.blockWidth > 1 || info.blockHeight > 1;
        CHECK(block == (info.kind == PixelFormatKind::Compressed));
        CHECK((info.compression != PixelCompression::None) ==
              (info.kind == PixelFormatKind::Compressed));
        CHECK(info.topLevelBlockAligned == (info.compression == PixelCompression::BC));
        CHECK(info.componentType != PixelComponentType::None);
        CHECK(info.shaderType != PixelShaderType::None);
        // sRGB 짝은 서로를 가리키고 저장 크기가 같으며 한쪽만 sRGB다.
        if (info.srgbPair != PixelFormat::Unknown) {
            const PixelFormatInfo& pair = GetPixelFormatInfo(info.srgbPair);
            CHECK(pair.srgbPair == format);
            CHECK(pair.srgb != info.srgb);
            CHECK(pair.blockBytes == info.blockBytes);
        } else {
            CHECK(!info.srgb);
        }
        switch (info.kind) {
        case PixelFormatKind::Depth:
            CHECK(info.depthBits > 0 && info.stencilBits == 0 && info.planeCount == 1);
            break;
        case PixelFormatKind::DepthStencil:
            CHECK(info.depthBits > 0 && info.stencilBits == 8 && info.planeCount == 2);
            CHECK(info.planes[1].format == PixelFormat::R8Uint);
            break;
        case PixelFormatKind::Planar:
            CHECK(info.planeCount == 2 && info.planes[1].widthShift == 1 &&
                  info.planes[1].heightShift == 1);
            break;
        default:
            CHECK(info.planeCount == 1 && info.depthBits == 0 && info.stencilBits == 0);
            break;
        }
    }
    // sRGB 형식은 16개다(결정 PX1).
    uint32 srgb = 0;
    for (const Row& row : Table) {
        srgb += GetPixelFormatInfo(static_cast<PixelFormat>(row.number)).srgb ? 1u : 0u;
    }
    CHECK(srgb == 16);
    CHECK(GetPixelFormatInfo(PixelFormat::R64Uint).shaderType == PixelShaderType::Uint64);
    CHECK(GetPixelFormatInfo(PixelFormat::R16Sint).shaderType == PixelShaderType::Int);
    CHECK(GetPixelFormatInfo(PixelFormat::RGBA8Uint).shaderType == PixelShaderType::Uint);
    CHECK(GetPixelFormatInfo(PixelFormat::BC6HUfloat).componentType == PixelComponentType::Ufloat);
}

TEST(PixelFormat_BlockSizeReferenceValues) {
    // BC1 5×3: 2×1 블록 × 8바이트.
    CHECK(PixelFormatBlockColumns(PixelFormat::BC1Unorm, 5) == 2);
    CHECK(PixelFormatBlockRows(PixelFormat::BC1Unorm, 3) == 1);
    CHECK(PixelFormatImageBytes(PixelFormat::BC1Unorm, 5, 3) == 16);
    // ASTC 6×6 13×13: 3×3 블록 × 16바이트.
    CHECK(PixelFormatImageBytes(PixelFormat::ASTC6x6Unorm, 13, 13) == 144);
    // 블록보다 작은 이미지도 블록 하나다.
    CHECK(PixelFormatImageBytes(PixelFormat::ASTC12x12Hdr, 1, 1) == 16);
    CHECK(PixelFormatBlockColumns(PixelFormat::ASTC10x10Unorm, 21) == 3);
    CHECK(PixelFormatRowBytes(PixelFormat::BC7Unorm, 9) == 48);
    // 비압축.
    CHECK(PixelFormatImageBytes(PixelFormat::RGBA8Unorm, 4, 4) == 64);
    CHECK(PixelFormatImageBytes(PixelFormat::RGB32Float, 3, 1) == 36);
    CHECK(PixelFormatRowBytes(PixelFormat::RGB8Unorm, 5) == 15);
    // 비디오: plane 0 원래 크기, plane 1 반 크기 2채널.
    CHECK(PixelFormatImageBytes(PixelFormat::NV12, 4, 2) == 8 + 4);
    CHECK(PixelFormatImageBytes(PixelFormat::P010, 4, 2) == 16 + 8);
    CHECK(PixelFormatPlaneImageBytes(PixelFormat::NV12, 1, 4, 2) == 4);
    CHECK(PixelFormatImageBytes(PixelFormat::NV12, 3, 2) == 0);
    CHECK(PixelFormatPlaneImageBytes(PixelFormat::NV12, 2, 4, 2) == 0);
    // 깊이·스텐실은 plane 크기의 합이다(깊이 4바이트 + 스텐실 1바이트).
    CHECK(PixelFormatImageBytes(PixelFormat::D24UnormS8Uint, 4, 4) == 64 + 16);
    CHECK(PixelFormatPlaneCount(PixelFormat::D24UnormS8Uint) == 2);
    CHECK(PixelFormatPlaneRowBytes(PixelFormat::D32FloatS8Uint, 1, 4) == 4);
    CHECK(PixelFormatPlaneRows(PixelFormat::NV12, 1, 6) == 3);
    CHECK(PixelFormatPlaneRows(PixelFormat::NV12, 1, 5) == 0);
    CHECK(PixelFormatPlaneRows(PixelFormat::BC1Unorm, 0, 9) == 3);
    CHECK(PixelFormatPlaneImageBytes(PixelFormat::D24UnormS8Uint, 0, 4, 4) == 64);
    CHECK(PixelFormatPlaneImageBytes(PixelFormat::D32FloatS8Uint, 1, 4, 4) == 16);
    CHECK(PixelFormatPlaneImageBytes(PixelFormat::D16Unorm, 0, 2, 2) == 8);
    CHECK(PixelFormatPlaneImageBytes(PixelFormat::D16Unorm, 1, 2, 2) == 0);
    CHECK(PixelFormatRowBytes(PixelFormat::D32Float, 4) == 16);
    CHECK(PixelFormatImageBytes(PixelFormat::D16Unorm, 2, 2) == 8);
    CHECK(PixelFormatRowBytes(PixelFormat::NV12, 4) == 0);
    // 크기 0.
    CHECK(PixelFormatImageBytes(PixelFormat::RGBA8Unorm, 0, 4) == 0);
}

TEST(PixelFormat_MipChainBytes) {
    // RGBA8 4×4 3단: 64 + 16 + 4.
    CHECK(MipChainBytes(PixelFormat::RGBA8Unorm, 4, 4, 1, 3) == 84);
    // BC1 8×8 4단: 블록 4개 + 1 + 1 + 1(작은 mip도 블록 하나).
    CHECK(MipChainBytes(PixelFormat::BC1Unorm, 8, 8, 1, 4) == 32 + 8 + 8 + 8);
    // 3D RGBA8 4×4×4 3단: 깊이도 반씩 준다.
    CHECK(MipChainBytes(PixelFormat::RGBA8Unorm, 4, 4, 4, 3) == 64 * 4 + 16 * 2 + 4);
    // 깊이가 가장 긴 축이면 mip 수가 늘어난다.
    CHECK(MipChainBytes(PixelFormat::R8Unorm, 1, 1, 4, 3) == 4 + 2 + 1);
    // mip 수가 전체 mip 수를 넘거나 0이면 0이다.
    CHECK(MipChainBytes(PixelFormat::RGBA8Unorm, 4, 4, 1, 4) == 0);
    CHECK(MipChainBytes(PixelFormat::RGBA8Unorm, 4, 4, 1, 0) == 0);
    // 깊이 형식은 texel 크기로, 다중 plane은 mip마다 plane 합으로 센다. 무효 형식은 0이다.
    CHECK(MipChainBytes(PixelFormat::D32Float, 4, 4, 1, 1) == 64);
    CHECK(MipChainBytes(PixelFormat::NV12, 4, 4, 1, 2) == (16 + 8) + (4 + 2));
    CHECK(MipChainBytes(PixelFormat::Unknown, 4, 4, 1, 1) == 0);
    CHECK(FullMipCount(16, 1) == 5);
    CHECK(MipExtent(5, 1) == 2 && MipExtent(5, 3) == 1);
}

TEST(PixelFormat_CapsFlags) {
    const PixelFormatCaps caps =
        PixelFormatCaps::Sampled | PixelFormatCaps::Filterable | PixelFormatCaps::RenderTarget;
    CHECK(HasPixelFormatCaps(caps, PixelFormatCaps::Sampled | PixelFormatCaps::RenderTarget));
    CHECK(!HasPixelFormatCaps(caps, PixelFormatCaps::Sampled | PixelFormatCaps::DepthStencil));
    CHECK(HasPixelFormatCaps(caps, PixelFormatCaps::None));
    CHECK((caps & PixelFormatCaps::Filterable) == PixelFormatCaps::Filterable);
}
} // namespace pixel_format_tests_file
