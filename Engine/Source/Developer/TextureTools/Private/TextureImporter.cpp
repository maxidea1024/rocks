#include "TextureImportInternal.h"

#include <Base/NumericLimits.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>
#include <Texture/Texture.generated.h>
#include <Texture/TextureImportOptions.generated.h>
#include <TextureTools/TextureImport.h>

// 텍스처 임포터 구현이다(Docs/Design/AssetSystem 12.12절 — 임포터 표·구성 규칙·원본 저장·새 임포트
// 기본값·재임포트 규칙).
namespace rocks {
namespace {

// 임포터 id·버전이다(기록 importerId — kind 순서, 바꾸지 않는다).
constexpr Guid TextureImporterIds[TextureImportKindCount] = {
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a01ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a02ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a03ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a04ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a05ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a06ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a07ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a08ull},
    Guid{0x7c1f4b2e9a3d4e51ull, 0x8b6a0d2c4f1e7a09ull}};
constexpr uint32 TextureImporterVersion = 1;

// `texture`의 기본 확장자다(12.12절 표 — ImageCodec이 받는 형식).
constexpr StringView TextureExtensions[] = {"png", "bmp", "tga", "icb",  "vda",
                                            "vst", "psd", "dds", "tif",  "tiff",
                                            "hdr", "exr", "jpg", "jpeg", "webp"};
constexpr StringView SlicePrefixes[] = {TextureSliceRolePrefix};
constexpr StringView CubePrefixes[] = {TextureCubeRolePrefix};

// 새 임포트의 알파 커버리지 기본 임계다(UE SetModernSettingsForNewOrChangedTexture).
constexpr double ModernAlphaCoverageThreshold = 0.75;
// UE 규칙의 "4의 배수가 아님" 판정 마스크다(블록 압축 단위).
constexpr uint32 BlockAlignMask = 3;
// 경고 detail의 원본 번호 자리(상위 32비트 — 12.12절)와 역할 번호의 기수다.
constexpr uint32 WarningSourceShift = 32;
constexpr uint64 RoleIndexBase = 10;

bool IsPowerOfTwo(uint32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

bool IsFloatFormat(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::R16F || format == TextureSourceFormat::RGBA16F ||
           format == TextureSourceFormat::R32F || format == TextureSourceFormat::RGBA32F;
}

bool IsEightBitFormat(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::G8 || format == TextureSourceFormat::GA8 ||
           format == TextureSourceFormat::RGBA8;
}

TypeId OutputTypeOf(TextureImportKind kind) noexcept {
    switch (kind) {
    case TextureImportKind::Texture2D:
        return TypeIdOf<Texture2D>();
    case TextureImportKind::Cube:
    case TextureImportKind::CubeFile:
        return TypeIdOf<TextureCube>();
    case TextureImportKind::Array:
    case TextureImportKind::ArrayFile:
        return TypeIdOf<Texture2DArray>();
    case TextureImportKind::CubeArray:
    case TextureImportKind::CubeArrayFile:
        return TypeIdOf<TextureCubeArray>();
    case TextureImportKind::Volume:
    case TextureImportKind::VolumeFile:
        return TypeIdOf<VolumeTexture>();
    }

    return TypeId{};
}

TypeId OptionsTypeOf(TextureImportKind kind) noexcept {
    switch (kind) {
    case TextureImportKind::CubeFile:
        return TypeIdOf<TextureCubeFileOptions>();
    case TextureImportKind::VolumeFile:
        return TypeIdOf<TextureVolumeFileOptions>();
    case TextureImportKind::ArrayFile:
    case TextureImportKind::CubeArrayFile:
        return TypeId{};
    default:
        return TypeIdOf<TextureImportOptions>();
    }
}

bool IsMultiSource(TextureImportKind kind) noexcept {
    return kind == TextureImportKind::Cube || kind == TextureImportKind::Array ||
           kind == TextureImportKind::CubeArray || kind == TextureImportKind::Volume;
}

bool IsVariableOnly(TextureImportKind kind) noexcept {
    return kind == TextureImportKind::Array || kind == TextureImportKind::CubeArray ||
           kind == TextureImportKind::Volume;
}

// 앞 0 없는 10진 번호를 푼다(`0`·`12` — `01`·빈 값·넘침은 거짓).
bool ParseIndex(StringView text, uint32& out) noexcept {
    if (text.IsEmpty() || (text.ByteCount() > 1 && text.Data()[0] == '0')) {
        return false;
    }

    uint64 value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }

        value = value * RoleIndexBase + static_cast<uint64>(digit - '0');
        if (value > NumericLimits<uint32>::Max()) {
            return false;
        }
    }

    out = static_cast<uint32>(value);
    return true;
}

// 원본 하나의 디코딩 결과다.
struct DecodedSource {
    explicit DecodedSource(Allocator& allocator) noexcept : result(allocator) {
    }

    image::ImageDecodeResult result;
    image::ImageFileFormat file = image::ImageFileFormat::Unknown;
    // context.Sources()의 번호(진단 detail)다.
    usize source = 0;
    bool tgaColorMapped8 = false;
    bool tgaGray8 = false;
};

// 구성 결과다(원본 저장과 기본값의 근거).
struct ComposedSource {
    explicit ComposedSource(Allocator& allocator) noexcept : pixels(allocator) {
    }

    TextureSourceFormat format = TextureSourceFormat::Invalid;
    uint32 sizeX = 0;
    uint32 sizeY = 0;
    uint32 numSlices = 0;
    image::ImageColorEncoding encoding = image::ImageColorEncoding::Unknown;
    bool allTgaGray8 = false;
    bool allTgaColorMapped8 = false;
    Array<uint8> pixels;
};

// UE GetDefaultSRGB다(12.12절 기본값 6번 — Default·EditorIcon의 8비트는 encoding(Unknown은 sRGB),
// 16비트 정수·float는 on, 그 밖 종류는 off).
bool DefaultSrgb(TextureCompressionSettings compression, TextureSourceFormat format,
                 image::ImageColorEncoding encoding) noexcept {
    if (compression != TextureCompressionSettings::Default &&
        compression != TextureCompressionSettings::EditorIcon) {
        return false;
    }

    return IsEightBitFormat(format) ? encoding != image::ImageColorEncoding::Linear : true;
}

// 새 임포트 기본값이다(12.12절 1·4–7번 — 2·3번은 구성 때 했다).
void ApplyNewImportDefaults(Texture& texture, const ComposedSource& composed,
                            bool isTexture2D) noexcept {
    // 1. UE SetModernSettingsForNewOrChangedTexture.
    if (texture.compositeTexture.Handle().IsNull()) {
        texture.compositeTextureMode = TextureCompositeMode::Disabled;
    }

    const Vec4d& thresholds = texture.alphaCoverageThresholds;
    if (!texture.doScaleMipsForAlphaCoverage && thresholds.x == 0.0 && thresholds.y == 0.0 &&
        thresholds.z == 0.0 && thresholds.w == 0.0) {
        texture.alphaCoverageThresholds = Vec4d{0.0, 0.0, 0.0, ModernAlphaCoverageThreshold};
    }

    texture.normalizeNormals = 1;
    texture.useNewMipFilter = true;

    // 4·5. 원본 형식에서 정하는 압축 설정(float → HDR → HDR_Compressed, TGA 8비트 회색).
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
    if (IsFloatFormat(composed.format)) {
        compression = TextureCompressionSettings::HDR_Compressed;
    } else if (composed.allTgaGray8 ||
               (composed.allTgaColorMapped8 && composed.format == TextureSourceFormat::G8)) {
        compression = TextureCompressionSettings::Grayscale;
    }

    // 6. SRGB(UE는 HDR·Grayscale을 정한 뒤 GetDefaultSRGB를 부른다 — 둘 다 off).
    texture.srgb = DefaultSrgb(compression, composed.format, composed.encoding) ? 1 : 0;

    // 7. 2의 거듭제곱이 아닌 원본(UE bDoAutomaticTextureSettingsForNonPow2Textures — XY만 본다).
    if (!IsPowerOfTwo(composed.sizeX) || !IsPowerOfTwo(composed.sizeY)) {
        if (isTexture2D && texture.mipGenSettings == TextureMipGenSettings::FromTextureGroup) {
            texture.mipGenSettings = TextureMipGenSettings::NoMipmaps;
        }

        if (((composed.sizeX & BlockAlignMask) != 0 || (composed.sizeY & BlockAlignMask) != 0) &&
            compression == TextureCompressionSettings::Default) {
            compression = TextureCompressionSettings::EditorIcon;
        }
    }

    texture.compressionSettings = compression;
}

// 재임포트 규칙이다(UE ApplyDefaultsForNewlyImportedTextures(bIsReimport) — 2의 거듭제곱이 된
// NoMipmaps Texture2D를 기본으로 되돌린다).
void ApplyReimportRule(Texture& texture, const ComposedSource& composed,
                       bool isTexture2D) noexcept {
    const TextureCompressionSettings compression = texture.compressionSettings;
    if (!isTexture2D || texture.mipGenSettings != TextureMipGenSettings::NoMipmaps ||
        (compression != TextureCompressionSettings::EditorIcon &&
         compression != TextureCompressionSettings::Default) ||
        texture.lodGroup == TextureGroup::UI) {
        return;
    }

    const bool powerOfTwo = (IsPowerOfTwo(composed.sizeX) && IsPowerOfTwo(composed.sizeY)) ||
                            texture.powerOfTwoMode != TexturePowerOfTwoSetting::None;
    if (powerOfTwo) {
        texture.mipGenSettings = TextureMipGenSettings::FromTextureGroup;
        texture.compressionSettings = TextureCompressionSettings::Default;
        texture.neverStream = 0;
    }
}

} // namespace

TextureImporter::TextureImporter(TextureImportKind kind) noexcept : kind_(kind) {
    const usize index = static_cast<usize>(kind);
    info_.id = TextureImporterIds[index];
    info_.name = TextureImporterNames[index];
    info_.version = TextureImporterVersion;
    info_.outputType = OutputTypeOf(kind);
    info_.optionsType = OptionsTypeOf(kind);
    if (kind == TextureImportKind::Texture2D) {
        info_.extensions = Span<const StringView>(TextureExtensions);
    }

    if (kind == TextureImportKind::Cube) {
        info_.roles = Span<const StringView>(TextureCubeFaceRoles);
    }

    if (kind == TextureImportKind::Array || kind == TextureImportKind::Volume) {
        info_.rolePrefixes = Span<const StringView>(SlicePrefixes);
    }

    if (kind == TextureImportKind::CubeArray) {
        info_.rolePrefixes = Span<const StringView>(CubePrefixes);
    }

    info_.variableSourcesOnly = IsVariableOnly(kind);
}

Result<void> TextureImporter::Import(AssetImportContext& context) noexcept {
    Allocator& allocator = context.Registry().ObjectAllocator();
    const Span<const AssetImportSource> sources = context.Sources();
    const auto invalid = [&context](uint64 detail) noexcept {
        return context.Fail(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                            detail);
    };

    // 옵션(없는 임포터는 기본값).
    TextureImportPngInfill infill = TextureImportPngInfill::OnlyOnBinaryTransparency;
    TextureCubeLayout layout = TextureCubeLayout::None;
    uint32 tileX = 0;
    uint32 tileY = 0;
    if (const void* options = context.Options(); options != nullptr) {
        if (kind_ == TextureImportKind::CubeFile) {
            const auto& cube = *static_cast<const TextureCubeFileOptions*>(options);
            layout = cube.cubeLayout;
            infill = cube.pngInfill;
        } else if (kind_ == TextureImportKind::VolumeFile) {
            const auto& volume = *static_cast<const TextureVolumeFileOptions*>(options);
            tileX = volume.volumeTileX;
            tileY = volume.volumeTileY;
            infill = volume.pngInfill;
        } else {
            infill = static_cast<const TextureImportOptions*>(options)->pngInfill;
        }
    }

    if ((tileX == 0) != (tileY == 0)) {
        return invalid(0);
    }

    // slice 순서(가변 역할은 번호를 수로 푼다 — 프레임워크는 byte 순으로 준다).
    Array<usize> order(allocator);
    if (auto sized = order.TryResize(sources.Size()); !sized) {
        return sized;
    }

    if (kind_ == TextureImportKind::Array || kind_ == TextureImportKind::Volume ||
        kind_ == TextureImportKind::CubeArray) {
        Array<bool> filled(allocator);
        if (auto sized = filled.TryResize(sources.Size()); !sized) {
            return sized;
        }

        for (usize index = 0; index < sources.Size(); ++index) {
            filled[index] = false;
        }

        // CubeArray는 cube마다 면 6개가 있어야 한다.
        if (kind_ == TextureImportKind::CubeArray && sources.Size() % TextureCubeFaces != 0) {
            return invalid(sources.Size());
        }

        for (usize index = 0; index < sources.Size(); ++index) {
            const StringView role = sources[index].role;
            uint32 slot = 0;
            if (kind_ == TextureImportKind::CubeArray) {
                const StringView rest =
                    role.SliceBytes(TextureCubeRolePrefix.ByteCount(),
                                    role.ByteCount() - TextureCubeRolePrefix.ByteCount());
                usize dot = IndexNone;
                for (usize at = 0; at < rest.ByteCount() && dot == IndexNone; ++at) {
                    dot = rest.Data()[at] == '.' ? at : IndexNone;
                }

                uint32 cube = 0;
                usize face = TextureCubeFaces;
                if (dot != IndexNone) {
                    const StringView faceName =
                        rest.SliceBytes(dot + 1, rest.ByteCount() - dot - 1);
                    for (usize at = 0; at < TextureCubeFaces; ++at) {
                        face = faceName == TextureCubeFaceRoles[at] ? at : face;
                    }
                }

                if (dot == IndexNone || face == TextureCubeFaces ||
                    !ParseIndex(rest.SliceBytes(0, dot), cube) ||
                    uint64{cube} * TextureCubeFaces + face >= sources.Size()) {
                    return invalid(index);
                }

                slot = cube * TextureCubeFaces + static_cast<uint32>(face);
            } else {
                const StringView number =
                    role.SliceBytes(TextureSliceRolePrefix.ByteCount(),
                                    role.ByteCount() - TextureSliceRolePrefix.ByteCount());
                if (!ParseIndex(number, slot) || slot >= sources.Size()) {
                    return invalid(index);
                }
            }

            if (filled[slot]) {
                return invalid(index);
            }

            filled[slot] = true;
            order[slot] = index;
        }
    } else {
        for (usize index = 0; index < sources.Size(); ++index) {
            order[index] = index;
        }
    }

    // 원본마다 디코딩한다(경고 detail = 원본 번호 << 32 | 디코더 detail).
    Array<DecodedSource> decoded(allocator);
    if (auto reserved = decoded.TryReserve(order.Size()); !reserved) {
        return reserved;
    }

    for (usize slot = 0; slot < order.Size(); ++slot) {
        const AssetImportSource& source = sources[order[slot]];
        DecodedSource one(allocator);
        one.source = order[slot];
        one.file = image::DetectImageFileFormat(source.bytes, source.path);
        one.tgaColorMapped8 = one.file == image::ImageFileFormat::Tga &&
                              texture_import::IsTgaColorMapped8(source.bytes);
        one.tgaGray8 =
            one.file == image::ImageFileFormat::Tga && texture_import::IsTgaGray8(source.bytes);
        if (auto done = image::DecodeImage(allocator, source.bytes, one.file,
                                           image::ImageDecodeLimits{}, one.result, source.path);
            !done) {
            return context.Fail(one.result.failure.code, one.result.failure.error, one.source);
        }

        for (const image::ImageDecodeWarning& warning : one.result.warnings) {
            if (auto warned = context.Warn(
                    warning.code, (uint64{one.source} << WarningSourceShift) | warning.detail);
                !warned) {
                return warned;
            }
        }

        (void)decoded.TryPushBack(rocks::Move(one));
    }

    // 차원 검사(12.12절 구성 규칙).
    const auto dimensionOf = [&decoded](usize slot) noexcept {
        return decoded[slot].result.image.dimension;
    };
    TextureDimension expected = TextureDimension::Texture2D;
    switch (kind_) {
    case TextureImportKind::CubeFile:
        expected = layout == TextureCubeLayout::None ? TextureDimension::TextureCube
                                                     : TextureDimension::Texture2D;
        break;
    case TextureImportKind::ArrayFile:
        expected = TextureDimension::Texture2DArray;
        break;
    case TextureImportKind::CubeArrayFile:
        expected = TextureDimension::TextureCubeArray;
        break;
    case TextureImportKind::VolumeFile:
        expected = tileX == 0 ? TextureDimension::Volume : TextureDimension::Texture2D;
        break;
    default:
        break;
    }

    for (usize slot = 0; slot < decoded.Size(); ++slot) {
        if (dimensionOf(slot) != expected) {
            // 한 파일은 결과 차원 값, 여러 원본은 원본 번호다.
            return invalid(IsMultiSource(kind_) ? decoded[slot].source
                                                : static_cast<uint64>(dimensionOf(slot)));
        }
    }

    // 2. 회색 자동 감지(PSD·DDS 밖 — 대상 형식인 원본이 모두 회색일 때만 모두 바꾼다). 감지하지
    // 않는 PSD·DDS 원본이 RGBA8·GA8이면 그 원본은 그대로 남으므로 바꾸지 않는다(일부만 G8이 되어
    // 형식 일치에 걸리지 않게).
    bool convert = false;
    bool blocked = false;
    for (const DecodedSource& one : decoded) {
        const TextureSourceFormat format = one.result.image.format;
        if (format != TextureSourceFormat::RGBA8 && format != TextureSourceFormat::GA8) {
            continue;
        }

        if (one.file == image::ImageFileFormat::Psd || one.file == image::ImageFileFormat::Dds) {
            blocked = true;
        } else if (texture_import::IsGrayConvertible(one.result.image)) {
            convert = true;
        } else {
            blocked = true;
        }
    }

    for (DecodedSource& one : decoded) {
        const TextureSourceFormat format = one.result.image.format;
        if (convert && !blocked && one.file != image::ImageFileFormat::Psd &&
            one.file != image::ImageFileFormat::Dds &&
            (format == TextureSourceFormat::RGBA8 || format == TextureSourceFormat::GA8)) {
            texture_import::ConvertToG8(one.result.image);
        }

        // 3. PNG infill(원본마다).
        if (one.file == image::ImageFileFormat::Png) {
            if (auto filled =
                    texture_import::FillTransparentTexels(one.result.image, infill, allocator);
                !filled) {
                return filled;
            }
        }
    }

    // 구성.
    ComposedSource composed(allocator);
    const image::DecodedImage& first = decoded[0].result.image;
    composed.format = first.format;
    composed.encoding = first.encoding;
    composed.allTgaGray8 = true;
    composed.allTgaColorMapped8 = true;
    for (const DecodedSource& one : decoded) {
        composed.allTgaGray8 = composed.allTgaGray8 && one.tgaGray8;
        composed.allTgaColorMapped8 = composed.allTgaColorMapped8 && one.tgaColorMapped8;
    }

    if (IsMultiSource(kind_)) {
        const bool square =
            kind_ == TextureImportKind::Cube || kind_ == TextureImportKind::CubeArray;
        usize total = 0;
        for (const DecodedSource& one : decoded) {
            const image::DecodedImage& image = one.result.image;
            if (image.width != first.width || image.height != first.height ||
                image.format != first.format || (square && image.width != image.height)) {
                return invalid(one.source);
            }

            total += image.pixels.Size();
        }

        if (auto sized = composed.pixels.TryResize(total); !sized) {
            return sized;
        }

        usize offset = 0;
        for (const DecodedSource& one : decoded) {
            const Array<uint8>& pixels = one.result.image.pixels;
            platform::MemoryCopy(composed.pixels.Data() + offset, pixels.Data(), pixels.Size());
            offset += pixels.Size();
        }

        composed.sizeX = first.width;
        composed.sizeY = first.height;
        composed.numSlices = static_cast<uint32>(decoded.Size());
    } else if (kind_ == TextureImportKind::CubeFile && layout != TextureCubeLayout::None) {
        uint32 face = 0;
        auto extracted = texture_import::ExtractCubeLayout(first, layout, composed.pixels, face);
        if (!extracted) {
            return Result<void>::Failure(extracted.Error());
        }

        if (!extracted.Value()) {
            return invalid(0);
        }

        composed.sizeX = face;
        composed.sizeY = face;
        composed.numSlices = TextureCubeFaces;
    } else if (kind_ == TextureImportKind::VolumeFile && tileX != 0) {
        uint32 slices = 0;
        auto extracted = texture_import::ExtractVolumeTiles(
            first, tileX, tileY, image::ImageDecodeLimits::DefaultMaxSlices, composed.pixels,
            slices);
        if (!extracted) {
            return Result<void>::Failure(extracted.Error());
        }

        if (!extracted.Value()) {
            return invalid(0);
        }

        composed.sizeX = tileX;
        composed.sizeY = tileY;
        composed.numSlices = slices;
    } else {
        composed.pixels = rocks::Move(decoded[0].result.image.pixels);
        composed.sizeX = first.width;
        composed.sizeY = first.height;
        composed.numSlices = first.numSlices;
    }

    // 원본 저장.
    auto made = context.Output({}, info_.outputType);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    const ObjectHandle main = made.Value();
    const uint32 sizeX = composed.sizeX;
    const uint32 sizeY = composed.sizeY;
    const uint32 numSlices = composed.numSlices;
    auto bulk = context.AddBulk(main, rocks::Move(composed.pixels));
    if (!bulk) {
        return Result<void>::Failure(bulk.Error());
    }

    auto borrowed = context.Registry().ResolveForWrite<Texture>(context.Access(), main);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    Texture& texture = *value.Value();
    texture.source.format = composed.format;
    texture.source.sizeX = sizeX;
    texture.source.sizeY = sizeY;
    texture.source.numSlices = numSlices;
    texture.source.numMips = 1;
    texture.source.numLayers = 1;
    texture.source.bulk = bulk.Value();
    const bool isTexture2D = kind_ == TextureImportKind::Texture2D;
    if (context.IsReimport()) {
        ApplyReimportRule(texture, composed, isTexture2D);
    } else {
        ApplyNewImportDefaults(texture, composed, isTexture2D);
    }

    return Result<void>::Success();
}

TextureImporterSet::TextureImporterSet() noexcept
    : importers_{TextureImporter(TextureImportKind::Texture2D),
                 TextureImporter(TextureImportKind::Cube),
                 TextureImporter(TextureImportKind::CubeFile),
                 TextureImporter(TextureImportKind::Array),
                 TextureImporter(TextureImportKind::ArrayFile),
                 TextureImporter(TextureImportKind::CubeArray),
                 TextureImporter(TextureImportKind::CubeArrayFile),
                 TextureImporter(TextureImportKind::Volume),
                 TextureImporter(TextureImportKind::VolumeFile)} {
}

Result<void> RegisterTextureImporters(void* state, AssetImporterTable& table,
                                      Array<AssetImporterRegistration>& tokens) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    TextureImporterSet& set = *static_cast<TextureImporterSet*>(state);
    if (auto reserved = tokens.TryReserve(tokens.Size() + TextureImportKindCount); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < TextureImportKindCount; ++index) {
        auto registered = table.Register(set.At(static_cast<TextureImportKind>(index)));
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        (void)tokens.TryPushBack(rocks::Move(registered.Value()));
    }

    return Result<void>::Success();
}

} // namespace rocks
