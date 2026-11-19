#include <GraphicsTypes/PixelFormat.h>
#include <Platform/Memory.h>

namespace rocks::graphics {

namespace {

using PF = PixelFormat;
using CT = PixelComponentType;

constexpr uint8 ChR = static_cast<uint8>(PixelChannel::R);
constexpr uint8 ChG = static_cast<uint8>(PixelChannel::G);
constexpr uint8 ChB = static_cast<uint8>(PixelChannel::B);
constexpr uint8 ChA = static_cast<uint8>(PixelChannel::A);
constexpr uint8 ChD = static_cast<uint8>(PixelChannel::Depth);
constexpr uint8 ChS = static_cast<uint8>(PixelChannel::Stencil);
constexpr uint8 ChRG = ChR | ChG;
constexpr uint8 ChRGB = ChR | ChG | ChB;
constexpr uint8 ChRGBA = ChR | ChG | ChB | ChA;

// 성분 타입에서 셰이더 읽기 계열을 정한다(정수는 Int·Uint, 나머지는 Float).
constexpr PixelShaderType ShaderTypeOf(PixelComponentType type) noexcept {
    switch (type) {
    case CT::Uint:
        return PixelShaderType::Uint;
    case CT::Sint:
        return PixelShaderType::Int;
    case CT::None:
        return PixelShaderType::None;
    default:
        return PixelShaderType::Float;
    }
}

// 비압축 색 형식 행이다.
constexpr PixelFormatInfo Color(PF format, const char* name, uint8 bytes, uint8 channels,
                                PixelComponentType type, PF pair = PF::Unknown,
                                bool srgb = false) noexcept {
    PixelFormatInfo info;
    info.format = format;
    info.name = name;
    info.kind = PixelFormatKind::Color;
    info.blockWidth = 1;
    info.blockHeight = 1;
    info.blockBytes = bytes;
    info.channels = channels;
    info.componentType = type;
    info.shaderType = ShaderTypeOf(type);
    info.srgb = srgb;
    info.srgbPair = pair;
    info.planeCount = 1;
    return info;
}

// 블록 압축 형식 행이다. BC만 최상위 크기가 블록 배수여야 한다.
constexpr PixelFormatInfo Compressed(PF format, const char* name, PixelCompression compression,
                                     uint8 blockWidth, uint8 blockHeight, uint8 bytes,
                                     uint8 channels, PixelComponentType type, PF pair = PF::Unknown,
                                     bool srgb = false) noexcept {
    PixelFormatInfo info = Color(format, name, bytes, channels, type, pair, srgb);
    info.kind = PixelFormatKind::Compressed;
    info.compression = compression;
    info.blockWidth = blockWidth;
    info.blockHeight = blockHeight;
    info.topLevelBlockAligned = compression == PixelCompression::BC;
    return info;
}

// 깊이 전용 형식 행이다. blockBytes는 깊이 texel 하나의 바이트다(readback 크기).
constexpr PixelFormatInfo Depth(PF format, const char* name, uint8 bytes, uint8 depthBits,
                                PixelComponentType type) noexcept {
    PixelFormatInfo info = Color(format, name, bytes, ChD, type);
    info.kind = PixelFormatKind::Depth;
    info.depthBits = depthBits;
    return info;
}

// 깊이 + 스텐실 형식 행이다. plane 0은 깊이(depthPlane 형식), plane 1은 스텐실(R8Uint)이다.
constexpr PixelFormatInfo DepthStencil(PF format, const char* name, uint8 depthBits,
                                       PixelComponentType type, PF depthPlane) noexcept {
    PixelFormatInfo info = Color(format, name, 0, ChD | ChS, type);
    info.kind = PixelFormatKind::DepthStencil;
    info.depthBits = depthBits;
    info.stencilBits = 8;
    info.planeCount = 2;
    info.planes[0] = {depthPlane, 0, 0};
    info.planes[1] = {PF::R8Uint, 0, 0};
    return info;
}

// 4:2:0 2-plane 비디오 형식 행이다.
constexpr PixelFormatInfo Planar(PF format, const char* name, PF luma, PF chroma) noexcept {
    PixelFormatInfo info = Color(format, name, 0, ChRGB, CT::Unorm);
    info.kind = PixelFormatKind::Planar;
    info.planeCount = 2;
    info.planes[0] = {luma, 0, 0};
    info.planes[1] = {chroma, 1, 1};
    return info;
}

// 번호표다. 행 0은 Unknown이고 나머지는 번호 오름차순이다(아래 정적 검사).
constexpr PixelFormatInfo Infos[] = {
    PixelFormatInfo{},

    Color(PF::R8Unorm, "R8Unorm", 1, ChR, CT::Unorm, PF::R8UnormSrgb),
    Color(PF::R8UnormSrgb, "R8UnormSrgb", 1, ChR, CT::Unorm, PF::R8Unorm, true),
    Color(PF::R8Uint, "R8Uint", 1, ChR, CT::Uint),
    Color(PF::R8Sint, "R8Sint", 1, ChR, CT::Sint),
    Color(PF::A8Unorm, "A8Unorm", 1, ChA, CT::Unorm),
    Color(PF::RG8Unorm, "RG8Unorm", 2, ChRG, CT::Unorm, PF::RG8UnormSrgb),
    Color(PF::RG8UnormSrgb, "RG8UnormSrgb", 2, ChRG, CT::Unorm, PF::RG8Unorm, true),
    Color(PF::RG8Snorm, "RG8Snorm", 2, ChRG, CT::Snorm),
    Color(PF::RG8Uint, "RG8Uint", 2, ChRG, CT::Uint),
    Color(PF::RGB8Unorm, "RGB8Unorm", 3, ChRGB, CT::Unorm, PF::RGB8UnormSrgb),
    Color(PF::RGB8UnormSrgb, "RGB8UnormSrgb", 3, ChRGB, CT::Unorm, PF::RGB8Unorm, true),
    Color(PF::RGBA8Unorm, "RGBA8Unorm", 4, ChRGBA, CT::Unorm, PF::RGBA8UnormSrgb),
    Color(PF::RGBA8UnormSrgb, "RGBA8UnormSrgb", 4, ChRGBA, CT::Unorm, PF::RGBA8Unorm, true),
    Color(PF::RGBA8Snorm, "RGBA8Snorm", 4, ChRGBA, CT::Snorm),
    Color(PF::RGBA8Uint, "RGBA8Uint", 4, ChRGBA, CT::Uint),
    Color(PF::BGRA8Unorm, "BGRA8Unorm", 4, ChRGBA, CT::Unorm, PF::BGRA8UnormSrgb),
    Color(PF::BGRA8UnormSrgb, "BGRA8UnormSrgb", 4, ChRGBA, CT::Unorm, PF::BGRA8Unorm, true),
    Color(PF::R16Unorm, "R16Unorm", 2, ChR, CT::Unorm),
    Color(PF::R16Uint, "R16Uint", 2, ChR, CT::Uint),
    Color(PF::R16Sint, "R16Sint", 2, ChR, CT::Sint),
    Color(PF::R16Float, "R16Float", 2, ChR, CT::Float),
    Color(PF::RG16Unorm, "RG16Unorm", 4, ChRG, CT::Unorm),
    Color(PF::RG16Snorm, "RG16Snorm", 4, ChRG, CT::Snorm),
    Color(PF::RG16Uint, "RG16Uint", 4, ChRG, CT::Uint),
    Color(PF::RG16Sint, "RG16Sint", 4, ChRG, CT::Sint),
    Color(PF::RG16Float, "RG16Float", 4, ChRG, CT::Float),
    Color(PF::RGBA16Unorm, "RGBA16Unorm", 8, ChRGBA, CT::Unorm),
    Color(PF::RGBA16Snorm, "RGBA16Snorm", 8, ChRGBA, CT::Snorm),
    Color(PF::RGBA16Uint, "RGBA16Uint", 8, ChRGBA, CT::Uint),
    Color(PF::RGBA16Sint, "RGBA16Sint", 8, ChRGBA, CT::Sint),
    Color(PF::RGBA16Float, "RGBA16Float", 8, ChRGBA, CT::Float),
    Color(PF::R32Uint, "R32Uint", 4, ChR, CT::Uint),
    Color(PF::R32Sint, "R32Sint", 4, ChR, CT::Sint),
    Color(PF::R32Float, "R32Float", 4, ChR, CT::Float),
    Color(PF::RG32Uint, "RG32Uint", 8, ChRG, CT::Uint),
    Color(PF::RG32Float, "RG32Float", 8, ChRG, CT::Float),
    Color(PF::RGB32Uint, "RGB32Uint", 12, ChRGB, CT::Uint),
    Color(PF::RGB32Sint, "RGB32Sint", 12, ChRGB, CT::Sint),
    Color(PF::RGB32Float, "RGB32Float", 12, ChRGB, CT::Float),
    Color(PF::RGBA32Uint, "RGBA32Uint", 16, ChRGBA, CT::Uint),
    Color(PF::RGBA32Float, "RGBA32Float", 16, ChRGBA, CT::Float),
    [] {
        PixelFormatInfo info = Color(PF::R64Uint, "R64Uint", 8, ChR, CT::Uint);
        info.shaderType = PixelShaderType::Uint64;
        return info;
    }(),
    Color(PF::R5G6B5Unorm, "R5G6B5Unorm", 2, ChRGB, CT::Unorm),
    Color(PF::B5G5R5A1Unorm, "B5G5R5A1Unorm", 2, ChRGBA, CT::Unorm),
    Color(PF::RGB10A2Unorm, "RGB10A2Unorm", 4, ChRGBA, CT::Unorm),
    Color(PF::RG11B10Ufloat, "RG11B10Ufloat", 4, ChRGB, CT::Ufloat),
    Color(PF::RGB9E5Ufloat, "RGB9E5Ufloat", 4, ChRGB, CT::Ufloat),

    Depth(PF::D16Unorm, "D16Unorm", 2, 16, CT::Unorm),
    Depth(PF::D24Unorm, "D24Unorm", 4, 24, CT::Unorm),
    Depth(PF::D32Float, "D32Float", 4, 32, CT::Float),
    DepthStencil(PF::D24UnormS8Uint, "D24UnormS8Uint", 24, CT::Unorm, PF::D24Unorm),
    DepthStencil(PF::D32FloatS8Uint, "D32FloatS8Uint", 32, CT::Float, PF::D32Float),

    Compressed(PF::BC1Unorm, "BC1Unorm", PixelCompression::BC, 4, 4, 8, ChRGBA, CT::Unorm,
               PF::BC1UnormSrgb),
    Compressed(PF::BC1UnormSrgb, "BC1UnormSrgb", PixelCompression::BC, 4, 4, 8, ChRGBA, CT::Unorm,
               PF::BC1Unorm, true),
    Compressed(PF::BC2Unorm, "BC2Unorm", PixelCompression::BC, 4, 4, 16, ChRGBA, CT::Unorm,
               PF::BC2UnormSrgb),
    Compressed(PF::BC2UnormSrgb, "BC2UnormSrgb", PixelCompression::BC, 4, 4, 16, ChRGBA, CT::Unorm,
               PF::BC2Unorm, true),
    Compressed(PF::BC3Unorm, "BC3Unorm", PixelCompression::BC, 4, 4, 16, ChRGBA, CT::Unorm,
               PF::BC3UnormSrgb),
    Compressed(PF::BC3UnormSrgb, "BC3UnormSrgb", PixelCompression::BC, 4, 4, 16, ChRGBA, CT::Unorm,
               PF::BC3Unorm, true),
    Compressed(PF::BC4Unorm, "BC4Unorm", PixelCompression::BC, 4, 4, 8, ChR, CT::Unorm),
    Compressed(PF::BC5Unorm, "BC5Unorm", PixelCompression::BC, 4, 4, 16, ChRG, CT::Unorm),
    Compressed(PF::BC6HUfloat, "BC6HUfloat", PixelCompression::BC, 4, 4, 16, ChRGB, CT::Ufloat),
    Compressed(PF::BC7Unorm, "BC7Unorm", PixelCompression::BC, 4, 4, 16, ChRGBA, CT::Unorm,
               PF::BC7UnormSrgb),
    Compressed(PF::BC7UnormSrgb, "BC7UnormSrgb", PixelCompression::BC, 4, 4, 16, ChRGBA, CT::Unorm,
               PF::BC7Unorm, true),

    Compressed(PF::ETC2RGB8Unorm, "ETC2RGB8Unorm", PixelCompression::ETC2, 4, 4, 8, ChRGB,
               CT::Unorm, PF::ETC2RGB8UnormSrgb),
    Compressed(PF::ETC2RGB8UnormSrgb, "ETC2RGB8UnormSrgb", PixelCompression::ETC2, 4, 4, 8, ChRGB,
               CT::Unorm, PF::ETC2RGB8Unorm, true),
    Compressed(PF::ETC2RGBA8Unorm, "ETC2RGBA8Unorm", PixelCompression::ETC2, 4, 4, 16, ChRGBA,
               CT::Unorm, PF::ETC2RGBA8UnormSrgb),
    Compressed(PF::ETC2RGBA8UnormSrgb, "ETC2RGBA8UnormSrgb", PixelCompression::ETC2, 4, 4, 16,
               ChRGBA, CT::Unorm, PF::ETC2RGBA8Unorm, true),
    Compressed(PF::EACR11Unorm, "EACR11Unorm", PixelCompression::ETC2, 4, 4, 8, ChR, CT::Unorm),
    Compressed(PF::EACRG11Unorm, "EACRG11Unorm", PixelCompression::ETC2, 4, 4, 16, ChRG, CT::Unorm),

    Compressed(PF::ASTC4x4Unorm, "ASTC4x4Unorm", PixelCompression::ASTC, 4, 4, 16, ChRGBA,
               CT::Unorm, PF::ASTC4x4UnormSrgb),
    Compressed(PF::ASTC4x4UnormSrgb, "ASTC4x4UnormSrgb", PixelCompression::ASTC, 4, 4, 16, ChRGBA,
               CT::Unorm, PF::ASTC4x4Unorm, true),
    Compressed(PF::ASTC4x4Hdr, "ASTC4x4Hdr", PixelCompression::ASTC, 4, 4, 16, ChRGBA, CT::Float),
    Compressed(PF::ASTC6x6Unorm, "ASTC6x6Unorm", PixelCompression::ASTC, 6, 6, 16, ChRGBA,
               CT::Unorm, PF::ASTC6x6UnormSrgb),
    Compressed(PF::ASTC6x6UnormSrgb, "ASTC6x6UnormSrgb", PixelCompression::ASTC, 6, 6, 16, ChRGBA,
               CT::Unorm, PF::ASTC6x6Unorm, true),
    Compressed(PF::ASTC6x6Hdr, "ASTC6x6Hdr", PixelCompression::ASTC, 6, 6, 16, ChRGBA, CT::Float),
    Compressed(PF::ASTC8x8Unorm, "ASTC8x8Unorm", PixelCompression::ASTC, 8, 8, 16, ChRGBA,
               CT::Unorm, PF::ASTC8x8UnormSrgb),
    Compressed(PF::ASTC8x8UnormSrgb, "ASTC8x8UnormSrgb", PixelCompression::ASTC, 8, 8, 16, ChRGBA,
               CT::Unorm, PF::ASTC8x8Unorm, true),
    Compressed(PF::ASTC8x8Hdr, "ASTC8x8Hdr", PixelCompression::ASTC, 8, 8, 16, ChRGBA, CT::Float),
    Compressed(PF::ASTC10x10Unorm, "ASTC10x10Unorm", PixelCompression::ASTC, 10, 10, 16, ChRGBA,
               CT::Unorm, PF::ASTC10x10UnormSrgb),
    Compressed(PF::ASTC10x10UnormSrgb, "ASTC10x10UnormSrgb", PixelCompression::ASTC, 10, 10, 16,
               ChRGBA, CT::Unorm, PF::ASTC10x10Unorm, true),
    Compressed(PF::ASTC10x10Hdr, "ASTC10x10Hdr", PixelCompression::ASTC, 10, 10, 16, ChRGBA,
               CT::Float),
    Compressed(PF::ASTC12x12Unorm, "ASTC12x12Unorm", PixelCompression::ASTC, 12, 12, 16, ChRGBA,
               CT::Unorm, PF::ASTC12x12UnormSrgb),
    Compressed(PF::ASTC12x12UnormSrgb, "ASTC12x12UnormSrgb", PixelCompression::ASTC, 12, 12, 16,
               ChRGBA, CT::Unorm, PF::ASTC12x12Unorm, true),
    Compressed(PF::ASTC12x12Hdr, "ASTC12x12Hdr", PixelCompression::ASTC, 12, 12, 16, ChRGBA,
               CT::Float),

    Planar(PF::NV12, "NV12", PF::R8Unorm, PF::RG8Unorm),
    Planar(PF::P010, "P010", PF::R16Unorm, PF::RG16Unorm),
};

constexpr uint32 InfoCount = sizeof(Infos) / sizeof(Infos[0]);
static_assert(InfoCount == PixelFormatCount + 1, "Pixel format table row count mismatch");

// 번호 → 표 행 위치다(0은 없음).
struct NumberIndex {
    uint8 row[MaxPixelFormatNumber + 1] = {};
};

constexpr NumberIndex BuildNumberIndex() noexcept {
    NumberIndex index;
    for (uint32 row = 1; row < InfoCount; ++row) {
        index.row[PixelFormatNumber(Infos[row].format)] = static_cast<uint8>(row);
    }

    return index;
}

constexpr NumberIndex Index = BuildNumberIndex();

// 행이 번호 오름차순이고 번호가 범위 안인지 확인한다.
constexpr bool RowsAscending() noexcept {
    uint32 previous = 0;
    for (uint32 row = 1; row < InfoCount; ++row) {
        const uint32 number = PixelFormatNumber(Infos[row].format);
        if (number <= previous || number > MaxPixelFormatNumber) {
            return false;
        }
        previous = number;
    }

    return previous == MaxPixelFormatNumber;
}

static_assert(RowsAscending(), "Pixel format rows must be in ascending number order");

constexpr const PixelFormatInfo& RowOf(PixelFormat format) noexcept {
    const uint32 number = PixelFormatNumber(format);
    return Infos[number <= MaxPixelFormatNumber ? Index.row[number] : 0];
}

// sRGB 짝이 서로를 가리키고 정확히 한쪽만 sRGB이며 저장 크기가 같은지 확인한다.
constexpr bool SrgbPairsConsistent() noexcept {
    for (uint32 row = 1; row < InfoCount; ++row) {
        const PixelFormatInfo& info = Infos[row];
        if (info.srgbPair == PF::Unknown) {
            if (info.srgb) {
                return false;
            }
            continue;
        }
        const PixelFormatInfo& pair = RowOf(info.srgbPair);
        if (pair.srgbPair != info.format || pair.srgb == info.srgb ||
            pair.blockBytes != info.blockBytes || pair.blockWidth != info.blockWidth ||
            pair.blockHeight != info.blockHeight) {
            return false;
        }
    }

    return true;
}

static_assert(SrgbPairsConsistent(), "sRGB pairs must be symmetric and share storage");

// plane 하나의 (texel 바이트, 폭 축소, 높이 축소)다. 단일 plane 형식은 plane 0만 있고 블록 단위다.
struct PlaneShape {
    uint32 blockWidth = 0;
    uint32 blockHeight = 0;
    uint32 blockBytes = 0;
    uint32 widthShift = 0;
    uint32 heightShift = 0;
};

PlaneShape ShapeOf(const PixelFormatInfo& info, uint32 plane) noexcept {
    PlaneShape shape;
    switch (info.kind) {
    case PixelFormatKind::Color:
    case PixelFormatKind::Compressed:
    case PixelFormatKind::Depth:
        if (plane == 0) {
            shape.blockWidth = info.blockWidth;
            shape.blockHeight = info.blockHeight;
            shape.blockBytes = info.blockBytes;
        }
        break;
    case PixelFormatKind::DepthStencil:
    case PixelFormatKind::Planar:
        if (plane < info.planeCount) {
            const PixelFormatPlane& layout = info.planes[plane];
            shape.blockWidth = 1;
            shape.blockHeight = 1;
            shape.blockBytes = RowOf(layout.format).blockBytes;
            shape.widthShift = layout.widthShift;
            shape.heightShift = layout.heightShift;
        }
        break;
    case PixelFormatKind::Unknown:
        break;
    }

    return shape;
}

} // namespace

const PixelFormatInfo& GetPixelFormatInfo(PixelFormat format) noexcept {
    return RowOf(format);
}

bool IsValidPixelFormat(PixelFormat format) noexcept {
    return RowOf(format).format != PixelFormat::Unknown;
}

Result<PixelFormat> PixelFormatFromNumber(uint32 number) noexcept {
    if (number == 0 || number > MaxPixelFormatNumber || Index.row[number] == 0) {
        return Result<PixelFormat>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<PixelFormat>::Success(static_cast<PixelFormat>(number));
}

const char* PixelFormatName(PixelFormat format) noexcept {
    return RowOf(format).name;
}

Result<PixelFormat> ParsePixelFormatName(StringView name) noexcept {
    for (uint32 row = 1; row < InfoCount; ++row) {
        const usize bytes = platform::CStringLength(Infos[row].name);
        if (name.ByteCount() == bytes &&
            platform::MemoryCompare(name.Data(), Infos[row].name, bytes) == 0) {
            return Result<PixelFormat>::Success(Infos[row].format);
        }
    }

    return Result<PixelFormat>::Failure(ErrorCode::NotFound);
}

uint32 PixelFormatBlockColumns(PixelFormat format, uint32 width) noexcept {
    const PixelFormatInfo& info = RowOf(format);
    if (info.blockWidth == 0) {
        return 0;
    }

    return (width + info.blockWidth - 1u) / info.blockWidth;
}

uint32 PixelFormatBlockRows(PixelFormat format, uint32 height) noexcept {
    const PixelFormatInfo& info = RowOf(format);
    if (info.blockHeight == 0) {
        return 0;
    }

    return (height + info.blockHeight - 1u) / info.blockHeight;
}

uint32 PixelFormatPlaneCount(PixelFormat format) noexcept {
    return RowOf(format).planeCount;
}

uint64 PixelFormatPlaneRowBytes(PixelFormat format, uint32 plane, uint32 width) noexcept {
    const PlaneShape shape = ShapeOf(RowOf(format), plane);
    if (shape.blockBytes == 0 || width == 0 || width % (1u << shape.widthShift) != 0) {
        return 0;
    }
    const uint32 planeWidth = width >> shape.widthShift;
    const uint64 columns = (planeWidth + shape.blockWidth - 1u) / shape.blockWidth;
    return columns * shape.blockBytes;
}

uint32 PixelFormatPlaneRows(PixelFormat format, uint32 plane, uint32 height) noexcept {
    const PlaneShape shape = ShapeOf(RowOf(format), plane);
    if (shape.blockBytes == 0 || height == 0 || height % (1u << shape.heightShift) != 0) {
        return 0;
    }
    const uint32 planeHeight = height >> shape.heightShift;
    return (planeHeight + shape.blockHeight - 1u) / shape.blockHeight;
}

uint64 PixelFormatPlaneImageBytes(PixelFormat format, uint32 plane, uint32 width,
                                  uint32 height) noexcept {
    return PixelFormatPlaneRowBytes(format, plane, width) *
           PixelFormatPlaneRows(format, plane, height);
}

uint64 PixelFormatRowBytes(PixelFormat format, uint32 width) noexcept {
    if (RowOf(format).planeCount != 1) {
        return 0;
    }

    return PixelFormatPlaneRowBytes(format, 0, width);
}

uint64 PixelFormatImageBytes(PixelFormat format, uint32 width, uint32 height) noexcept {
    const uint32 planes = RowOf(format).planeCount;
    uint64 total = 0;
    for (uint32 plane = 0; plane < planes; ++plane) {
        const uint64 bytes = PixelFormatPlaneImageBytes(format, plane, width, height);
        if (bytes == 0) {
            return 0;
        }
        total += bytes;
    }

    return total;
}

uint64 MipChainBytes(PixelFormat format, uint32 width, uint32 height, uint32 depth,
                     uint32 mipLevels) noexcept {
    if (width == 0 || height == 0 || depth == 0 || mipLevels == 0) {
        return 0;
    }
    const uint32 planar = width > height ? width : height;
    if (mipLevels > FullMipCount(planar, depth)) {
        return 0;
    }
    uint64 total = 0;
    for (uint32 level = 0; level < mipLevels; ++level) {
        const uint64 image =
            PixelFormatImageBytes(format, MipExtent(width, level), MipExtent(height, level));
        if (image == 0) {
            return 0;
        }
        total += image * MipExtent(depth, level);
    }

    return total;
}

} // namespace rocks::graphics
