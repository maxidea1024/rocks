#include "ImageCodecInternal.h"

// DDS 디코더다(Docs/Design/AssetSystem 12.10절 — UE ImageCore
// DDSFile.cpp·UTextureFactory::ImportDDS와 같은 받는 범위): DX10 헤더와 레거시 헤더(FourCC·비트
// 마스크), 1D·2D·Cube·2DArray·CubeArray·Volume, 비압축 형식 중 값의 의미가 보존되는 것. mip은 0번만
// 쓰고(slice마다 나머지 mip 사슬을 건너뛴다) 나머지는 IgnoredSourceMips 경고다. 블록 압축은
// LossySource, SNORM·SINT 등은 UnsupportedSourceFormat이다. 헤더의 pitch·linear size는 믿지 않고
// 형식에서 계산한다.
namespace rocks::image::detail {
namespace {

constexpr usize DdsMagicBytes = 4;
constexpr usize DdsHeaderBytes = 124;
constexpr usize DdsDx10HeaderBytes = 20;
constexpr usize DdsDataOffset = DdsMagicBytes + DdsHeaderBytes;
constexpr usize DdsDx10DataOffset = DdsDataOffset + DdsDx10HeaderBytes;

// 파일 안 위치다(magic 4 byte 포함).
constexpr usize DdsHeightOffset = 12;
constexpr usize DdsWidthOffset = 16;
constexpr usize DdsDepthOffset = 24;
constexpr usize DdsMipCountOffset = 28;
constexpr usize DdsPixelFormatOffset = 76;
constexpr usize DdsPixelFlagsOffset = 80;
constexpr usize DdsFourCcOffset = 84;
constexpr usize DdsBitCountOffset = 88;
constexpr usize DdsRedMaskOffset = 92;
constexpr usize DdsGreenMaskOffset = 96;
constexpr usize DdsBlueMaskOffset = 100;
constexpr usize DdsAlphaMaskOffset = 104;
constexpr usize DdsCapsOffset = 108;
constexpr usize DdsCaps2Offset = 112;
constexpr usize DdsDxgiFormatOffset = 128;
constexpr usize DdsResourceDimensionOffset = 132;
constexpr usize DdsMiscFlagOffset = 136;
constexpr usize DdsArraySizeOffset = 140;

constexpr uint32 DdpfAlpha = 0x2;
constexpr uint32 DdpfFourCc = 0x4;
constexpr uint32 DdpfRgb = 0x40;
constexpr uint32 DdpfLuminance = 0x20000;
constexpr uint32 DdpfBumpDuDv = 0x80000;
constexpr uint32 DdsCapsMipmap = 0x400000;
constexpr uint32 DdsCaps2Cubemap = 0x200;
constexpr uint32 DdsCaps2AllFaces = 0xFC00;
constexpr uint32 DdsCaps2Volume = 0x200000;
constexpr uint32 DdsResourceDimensionTexture1D = 2;
constexpr uint32 DdsResourceDimensionTexture3D = 4;
constexpr uint32 DdsMiscTextureCube = 0x4;
constexpr uint32 DdsCubeFaces = 6;

// UE FDDSFile::MAX_MIPS_SUPPORTED(20)와 그에서 나오는 한 변 상한 2^20 − 1이다.
constexpr uint32 DdsMaxMips = 20;
constexpr uint32 DdsMaxDimension = (1u << DdsMaxMips) - 1u;

constexpr uint32 MakeFourCc(char a, char b, char c, char d) noexcept {
    return static_cast<uint32>(static_cast<uint8>(a)) |
           (static_cast<uint32>(static_cast<uint8>(b)) << 8) |
           (static_cast<uint32>(static_cast<uint8>(c)) << 16) |
           (static_cast<uint32>(static_cast<uint8>(d)) << 24);
}

// 원본 texel의 배치다. Lossy는 블록 압축, None은 받지 않는 형식이다.
enum class DdsKind : uint8 {
    None,
    Lossy,
    Gray8,
    Rg8,
    Rgba8,
    Bgra8,
    Bgrx8,
    Rgbx8,
    Rgb8,
    Bgr8,
    Gray16,
    Rgba16,
    R16F,
    Rgba16F,
    R32F,
    Rgba32F
};

// 형식 하나다. encoding은 DX10 헤더일 때의 8비트 색 부호화다(UE가 ConvertRGBXtoRGBA·
// ConvertChannelOrder로 바꾼 뒤의 형식에 _UNORM·_SRGB 쌍이 있으면 표시대로, 없으면 Srgb).
struct DdsFormat {
    DdsKind kind = DdsKind::None;
    ImageColorEncoding encoding = ImageColorEncoding::Linear;
};

// DXGI_FORMAT 값이다(dxgiformat.h).
enum DxgiFormat : uint32 {
    DxgiR32G32B32A32Float = 2,
    DxgiR16G16B16A16Float = 10,
    DxgiR16G16B16A16Unorm = 11,
    DxgiR16G16B16A16Uint = 12,
    DxgiR8G8B8A8Typeless = 27,
    DxgiR8G8B8A8Unorm = 28,
    DxgiR8G8B8A8UnormSrgb = 29,
    DxgiR8G8B8A8Uint = 30,
    DxgiR32Float = 41,
    DxgiR8G8Typeless = 48,
    DxgiR8G8Unorm = 49,
    DxgiR8G8Uint = 50,
    DxgiR16Typeless = 53,
    DxgiR16Float = 54,
    DxgiR16Unorm = 56,
    DxgiR16Uint = 57,
    DxgiR8Typeless = 60,
    DxgiR8Unorm = 61,
    DxgiR8Uint = 62,
    DxgiA8Unorm = 65,
    DxgiBc1Typeless = 70,
    DxgiBc5Snorm = 84,
    DxgiB8G8R8A8Unorm = 87,
    DxgiB8G8R8X8Unorm = 88,
    DxgiB8G8R8A8Typeless = 90,
    DxgiB8G8R8A8UnormSrgb = 91,
    DxgiB8G8R8X8Typeless = 92,
    DxgiB8G8R8X8UnormSrgb = 93,
    DxgiBc6hTypeless = 94,
    DxgiBc7UnormSrgb = 99
};

DdsFormat DdsFromDxgi(uint32 format) noexcept {
    constexpr ImageColorEncoding Srgb = ImageColorEncoding::Srgb;
    constexpr ImageColorEncoding Linear = ImageColorEncoding::Linear;
    if ((format >= DxgiBc1Typeless && format <= DxgiBc5Snorm) ||
        (format >= DxgiBc6hTypeless && format <= DxgiBc7UnormSrgb)) {
        return {DdsKind::Lossy, Linear};
    }

    switch (format) {
    case DxgiR32G32B32A32Float:
        return {DdsKind::Rgba32F, Linear};
    case DxgiR16G16B16A16Float:
        return {DdsKind::Rgba16F, Linear};
    case DxgiR16G16B16A16Unorm:
    case DxgiR16G16B16A16Uint:
        return {DdsKind::Rgba16, Linear};
    case DxgiR8G8B8A8Unorm:
        // UE는 B8G8R8A8_UNORM으로 바꾼다 — 쌍이 있어 선형이다.
        return {DdsKind::Rgba8, Linear};
    case DxgiR8G8B8A8Typeless:
    case DxgiR8G8B8A8UnormSrgb:
    case DxgiR8G8B8A8Uint:
        return {DdsKind::Rgba8, Srgb};
    case DxgiR32Float:
        return {DdsKind::R32F, Linear};
    case DxgiR8G8Typeless:
    case DxgiR8G8Unorm:
    case DxgiR8G8Uint:
        return {DdsKind::Rg8, Srgb};
    case DxgiR16Typeless:
    case DxgiR16Unorm:
    case DxgiR16Uint:
        return {DdsKind::Gray16, Linear};
    case DxgiR16Float:
        return {DdsKind::R16F, Linear};
    case DxgiR8Typeless:
    case DxgiR8Unorm:
    case DxgiR8Uint:
    case DxgiA8Unorm:
        return {DdsKind::Gray8, Srgb};
    case DxgiB8G8R8A8Unorm:
        return {DdsKind::Bgra8, Linear};
    case DxgiB8G8R8X8Unorm:
        // UE ConvertRGBXtoRGBA가 B8G8R8A8_UNORM으로 바꾼다 — 선형이다.
        return {DdsKind::Bgrx8, Linear};
    case DxgiB8G8R8A8Typeless:
    case DxgiB8G8R8A8UnormSrgb:
        return {DdsKind::Bgra8, Srgb};
    case DxgiB8G8R8X8Typeless:
    case DxgiB8G8R8X8UnormSrgb:
        return {DdsKind::Bgrx8, Srgb};
    default:
        return {};
    }
}

// 레거시(D3D9) 헤더의 형식이다(UE FOURCCToDXGITable·BitmaskToDXGITable 중 UE 대응표에 있는 것).
DdsKind DdsFromLegacy(Span<const uint8> file) noexcept {
    const uint32 flags = LoadLe32(file.Data() + DdsPixelFlagsOffset);
    if ((flags & DdpfFourCc) != 0) {
        constexpr uint32 FourCcA16B16G16R16 = 36;
        constexpr uint32 FourCcR16F = 111;
        constexpr uint32 FourCcA16B16G16R16F = 113;
        constexpr uint32 FourCcR32F = 114;
        constexpr uint32 FourCcA32B32G32R32F = 116;
        const uint32 fourCc = LoadLe32(file.Data() + DdsFourCcOffset);
        const uint32 blockFourCcs[] = {
            MakeFourCc('D', 'X', 'T', '1'), MakeFourCc('D', 'X', 'T', '2'),
            MakeFourCc('D', 'X', 'T', '3'), MakeFourCc('D', 'X', 'T', '4'),
            MakeFourCc('D', 'X', 'T', '5'), MakeFourCc('B', 'C', '4', 'U'),
            MakeFourCc('B', 'C', '4', 'S'), MakeFourCc('A', 'T', 'I', '1'),
            MakeFourCc('B', 'C', '5', 'U'), MakeFourCc('B', 'C', '5', 'S'),
            MakeFourCc('A', 'T', 'I', '2'), MakeFourCc('B', 'C', '6', 'H'),
            MakeFourCc('B', 'C', '7', 'L'), MakeFourCc('B', 'C', '7', '\0')};
        for (uint32 block : blockFourCcs) {
            if (fourCc == block) {
                return DdsKind::Lossy;
            }
        }

        switch (fourCc) {
        case FourCcA16B16G16R16:
            return DdsKind::Rgba16;
        case FourCcR16F:
            return DdsKind::R16F;
        case FourCcA16B16G16R16F:
            return DdsKind::Rgba16F;
        case FourCcR32F:
            return DdsKind::R32F;
        case FourCcA32B32G32R32F:
            return DdsKind::Rgba32F;
        default:
            return DdsKind::None;
        }
    }

    struct MaskFormat {
        uint32 flags;
        uint32 bits;
        uint32 red;
        uint32 green;
        uint32 blue;
        uint32 alpha;
        DdsKind kind;
    };

    // UE BitmaskToDXGITable 순서 그대로다(같은 마스크가 둘이면 앞이 이긴다). R10G10B10A2·R16G16·
    // 5551·565·4444·BUMPDUDV는 UE 원본 대응표에 없어 받지 않는다.
    constexpr MaskFormat masks[] = {
        {DdpfRgb, 32, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0xff000000u, DdsKind::Rgba8},
        {DdpfRgb, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u, DdsKind::Bgra8},
        {DdpfRgb, 32, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00000000u, DdsKind::Bgrx8},
        {DdpfRgb, 32, NumericLimits<uint32>::Max(), 0x00000000u, 0x00000000u, 0x00000000u,
         DdsKind::R32F},
        {DdpfLuminance, 8, 0xffu, 0x00u, 0x00u, 0x00u, DdsKind::Gray8},
        {DdpfLuminance, 16, 0xffffu, 0x0000u, 0x0000u, 0x0000u, DdsKind::Gray16},
        {DdpfLuminance, 16, 0x00ffu, 0x0000u, 0x0000u, 0xff00u, DdsKind::Rg8},
        {DdpfLuminance, 8, 0xffu, 0x00u, 0x00u, 0xff00u, DdsKind::Rg8},
        {DdpfAlpha, 8, 0x00u, 0x00u, 0x00u, 0xffu, DdsKind::Gray8},
        {DdpfRgb, 32, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0x00000000u, DdsKind::Rgbx8},
        {DdpfRgb, 24, 0x000000ffu, 0x0000ff00u, 0x00ff0000u, 0x00000000u, DdsKind::Rgb8},
        {DdpfRgb, 24, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0x00000000u, DdsKind::Bgr8}};
    const uint32 typeFlags = flags & (DdpfRgb | DdpfLuminance | DdpfAlpha | DdpfBumpDuDv);
    const uint32 bits = LoadLe32(file.Data() + DdsBitCountOffset);
    const uint32 red = LoadLe32(file.Data() + DdsRedMaskOffset);
    const uint32 green = LoadLe32(file.Data() + DdsGreenMaskOffset);
    const uint32 blue = LoadLe32(file.Data() + DdsBlueMaskOffset);
    const uint32 alpha = LoadLe32(file.Data() + DdsAlphaMaskOffset);
    for (const MaskFormat& mask : masks) {
        if (typeFlags == mask.flags && bits == mask.bits && red == mask.red &&
            green == mask.green && blue == mask.blue && alpha == mask.alpha) {
            return mask.kind;
        }
    }

    return DdsKind::None;
}

uint32 DdsTexelBytes(DdsKind kind) noexcept {
    switch (kind) {
    case DdsKind::None:
    case DdsKind::Lossy:
        return 0;
    case DdsKind::Gray8:
        return 1;
    case DdsKind::Rg8:
    case DdsKind::Gray16:
    case DdsKind::R16F:
        return 2;
    case DdsKind::Rgb8:
    case DdsKind::Bgr8:
        return 3;
    case DdsKind::Rgba8:
    case DdsKind::Bgra8:
    case DdsKind::Bgrx8:
    case DdsKind::Rgbx8:
    case DdsKind::R32F:
        return 4;
    case DdsKind::Rgba16:
    case DdsKind::Rgba16F:
        return 8;
    case DdsKind::Rgba32F:
        return 16;
    }

    return 0;
}

TextureSourceFormat DdsOutputFormat(DdsKind kind) noexcept {
    switch (kind) {
    case DdsKind::Gray8:
        return TextureSourceFormat::G8;
    case DdsKind::Gray16:
        return TextureSourceFormat::G16;
    case DdsKind::Rgba16:
        return TextureSourceFormat::RGBA16;
    case DdsKind::R16F:
        return TextureSourceFormat::R16F;
    case DdsKind::Rgba16F:
        return TextureSourceFormat::RGBA16F;
    case DdsKind::R32F:
        return TextureSourceFormat::R32F;
    case DdsKind::Rgba32F:
        return TextureSourceFormat::RGBA32F;
    case DdsKind::Rg8:
    case DdsKind::Rgba8:
    case DdsKind::Bgra8:
    case DdsKind::Bgrx8:
    case DdsKind::Rgbx8:
    case DdsKind::Rgb8:
    case DdsKind::Bgr8:
        return TextureSourceFormat::RGBA8;
    case DdsKind::None:
    case DdsKind::Lossy:
        return TextureSourceFormat::Invalid;
    }

    return TextureSourceFormat::Invalid;
}

bool DdsIsEightBit(DdsKind kind) noexcept {
    return DdsOutputFormat(kind) == TextureSourceFormat::RGBA8 || kind == DdsKind::Gray8;
}

// texel 하나를 결과 형식으로 옮긴다.
void DdsConvertTexel(DdsKind kind, const uint8* s, uint8* d) noexcept {
    switch (kind) {
    case DdsKind::Gray8:
        d[0] = s[0];
        return;
    case DdsKind::Gray16:
        d[0] = s[0];
        d[1] = s[1];
        return;
    case DdsKind::Rgba16:
        for (uint32 index = 0; index < 8; ++index) {
            d[index] = s[index];
        }

        return;
    case DdsKind::R16F:
        StoreLe16(d, SanitizeHalfBits(LoadLe16(s)));
        return;
    case DdsKind::Rgba16F:
        for (uint32 c = 0; c < 4; ++c) {
            StoreLe16(d + c * 2, SanitizeHalfBits(LoadLe16(s + c * 2)));
        }

        return;
    case DdsKind::R32F:
        StoreSanitizedFloat(d, platform::BitCast<float>(LoadLe32(s)));
        return;
    case DdsKind::Rgba32F:
        for (uint32 c = 0; c < 4; ++c) {
            StoreSanitizedFloat(d + c * 4, platform::BitCast<float>(LoadLe32(s + c * 4)));
        }

        return;
    case DdsKind::Rg8:
        // TextureSource에 2채널 색이 없다 — UE와 같은 (r, g, 0, 255).
        d[0] = s[0];
        d[1] = s[1];
        d[2] = 0;
        d[3] = 255;
        return;
    case DdsKind::Rgba8:
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = s[3];
        return;
    case DdsKind::Bgra8:
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = s[3];
        return;
    case DdsKind::Bgrx8:
    case DdsKind::Bgr8:
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
        return;
    case DdsKind::Rgbx8:
    case DdsKind::Rgb8:
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
        return;
    case DdsKind::None:
    case DdsKind::Lossy:
        return;
    }
}

uint32 DdsMipDimension(uint32 dimension, uint32 level) noexcept {
    const uint32 value = dimension >> level;
    return value != 0 ? value : 1;
}

} // namespace

Result<void> DecodeDds(Allocator& allocator, Span<const uint8> file,
                       const ImageDecodeLimits& limits, ImageDecodeResult& out) noexcept {
    (void)allocator;
    if (file.Size() < DdsDataOffset) {
        return Invalid(out, file.Size());
    }

    if (LoadLe32(file.Data()) != MakeFourCc('D', 'D', 'S', ' ')) {
        return Invalid(out, 0);
    }

    // 형식과 차원(UE ParseHeader 667–722).
    const uint32 pixelFlags = LoadLe32(file.Data() + DdsPixelFlagsOffset);
    const bool dx10 = (pixelFlags & DdpfFourCc) != 0 &&
                      LoadLe32(file.Data() + DdsFourCcOffset) == MakeFourCc('D', 'X', '1', '0');
    const uint32 caps2 = LoadLe32(file.Data() + DdsCaps2Offset);
    uint32 dimension = 0;
    DdsFormat format;
    bool cube = false;
    uint32 arraySize = 1;
    usize dataOffset = DdsDataOffset;
    if (dx10) {
        if (file.Size() < DdsDx10DataOffset) {
            return Invalid(out, file.Size());
        }

        const uint32 resourceDimension = LoadLe32(file.Data() + DdsResourceDimensionOffset);
        if (resourceDimension < DdsResourceDimensionTexture1D ||
            resourceDimension > DdsResourceDimensionTexture3D) {
            return Invalid(out, DdsResourceDimensionOffset);
        }

        dimension = resourceDimension - DdsResourceDimensionTexture1D + 1;
        format = DdsFromDxgi(LoadLe32(file.Data() + DdsDxgiFormatOffset));
        cube = (LoadLe32(file.Data() + DdsMiscFlagOffset) & DdsMiscTextureCube) != 0;
        arraySize = LoadLe32(file.Data() + DdsArraySizeOffset);
        dataOffset = DdsDx10DataOffset;
    } else {
        dimension = (caps2 & DdsCaps2Volume) != 0 ? 3 : 2;
        format.kind = DdsFromLegacy(file);
        // D3D9 헤더에는 sRGB 표시가 없다 — UE는 sRGB로 가정한다.
        format.encoding = ImageColorEncoding::Srgb;
        cube = (caps2 & DdsCaps2Cubemap) != 0;
        // 면 일부만 있는 D3D9 cube는 받지 않는다(UE는 면 비트를 보지 않고 6면을 읽는다).
        if (cube && (caps2 & DdsCaps2AllFaces) != DdsCaps2AllFaces) {
            return Unsupported(out, DdsCaps2Offset);
        }
    }

    const usize formatOffset = dx10 ? DdsDxgiFormatOffset : DdsPixelFormatOffset;
    if (format.kind == DdsKind::Lossy) {
        return Fail(out, AssetDiagnosticCode::LossySource, ErrorCode::Unsupported, formatOffset);
    }

    if (format.kind == DdsKind::None) {
        return Unsupported(out, formatOffset);
    }

    // 크기·mip 검증(UE FDDSFile::Validate 393–505).
    const uint32 width = LoadLe32(file.Data() + DdsWidthOffset);
    const uint32 height = LoadLe32(file.Data() + DdsHeightOffset);
    const uint32 depth = dimension == 3 ? LoadLe32(file.Data() + DdsDepthOffset) : 1;
    const uint32 mipCount = (LoadLe32(file.Data() + DdsCapsOffset) & DdsCapsMipmap) != 0
                                ? LoadLe32(file.Data() + DdsMipCountOffset)
                                : 1;
    if (cube) {
        if (arraySize > NumericLimits<uint32>::Max() / DdsCubeFaces) {
            return Invalid(out, DdsArraySizeOffset);
        }

        arraySize *= DdsCubeFaces;
    }

    if ((dimension == 1 && height != 1) || (dimension == 3 && arraySize != 1)) {
        return Invalid(out, DdsHeightOffset);
    }

    if (width == 0 || height == 0 || depth == 0 || mipCount == 0 || arraySize == 0) {
        return Invalid(out, DdsHeightOffset);
    }

    if (width > DdsMaxDimension || height > DdsMaxDimension || depth > DdsMaxDimension ||
        arraySize > DdsMaxDimension || mipCount > DdsMaxMips) {
        return Invalid(out, DdsMipCountOffset);
    }

    const uint32 finalMip = mipCount - 1;
    if ((width >> finalMip) == 0 && (height >> finalMip) == 0 && (depth >> finalMip) == 0) {
        return Invalid(out, DdsMipCountOffset);
    }

    if (cube && (dimension != 2 || width != height)) {
        return Invalid(out, DdsWidthOffset);
    }

    // Rocks 한도: 깊이도 한 변 한도, slice 수는 깊이 또는 배열 크기다.
    if (depth > limits.maxDimension) {
        return OverLimit(out, DdsDepthOffset);
    }

    const uint32 numSlices = dimension == 3 ? depth : arraySize;
    const TextureSourceFormat outputFormat = DdsOutputFormat(format.kind);
    if (auto allocated = AllocateImage(limits, outputFormat, width, height, out, numSlices);
        !allocated) {
        return allocated;
    }

    // 데이터 배치: 배열 원소마다 mip 사슬 전체, 그다음 원소(UE EditorFactories.cpp 3418–3421).
    // 모든 mip이 있어야 한다(UE ReadPayload).
    const uint32 texelBytes = DdsTexelBytes(format.kind);
    uint64 chainBytes = 0;
    for (uint32 level = 0; level < mipCount; ++level) {
        chainBytes += uint64{DdsMipDimension(width, level)} * DdsMipDimension(height, level) *
                      DdsMipDimension(depth, level) * texelBytes;
    }

    const uint64 available = file.Size() - dataOffset;
    if (chainBytes > available || uint64{arraySize} > available / chainBytes) {
        return Invalid(out, file.Size());
    }

    const uint64 mip0Texels = uint64{width} * height * depth;
    const uint32 outputBytes = TextureSourceTexelBytes(outputFormat);
    uint8* destination = out.image.pixels.Data();
    for (uint32 element = 0; element < arraySize; ++element) {
        const uint8* source = file.Data() + dataOffset + element * chainBytes;
        for (uint64 texel = 0; texel < mip0Texels; ++texel) {
            DdsConvertTexel(format.kind, source + texel * texelBytes, destination);
            destination += outputBytes;
        }
    }

    if (dimension == 3) {
        out.image.dimension = TextureDimension::Volume;
    } else if (cube) {
        out.image.dimension = arraySize == DdsCubeFaces ? TextureDimension::TextureCube
                                                        : TextureDimension::TextureCubeArray;
    } else {
        out.image.dimension =
            arraySize > 1 ? TextureDimension::Texture2DArray : TextureDimension::Texture2D;
    }

    // 8비트 형식만 sRGB 표시가 뜻이 있다(UE GetFormatNeedsGammaSpace) — 16비트·float는 선형이다.
    out.image.encoding = DdsIsEightBit(format.kind) ? format.encoding : ImageColorEncoding::Linear;
    out.image.sourceMips = mipCount;
    if (mipCount > 1) {
        return Warn(out, AssetDiagnosticCode::IgnoredSourceMips, mipCount - 1);
    }

    return Result<void>::Success();
}

} // namespace rocks::image::detail
