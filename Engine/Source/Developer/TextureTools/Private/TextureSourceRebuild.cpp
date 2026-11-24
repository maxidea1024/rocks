#include <AssetDatabase/ImportRecord.h>
#include <Base/NumericLimits.h>
#include <ImageCodec/ImageDecode.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureImport.h>

// 텍스처 원본 묶음 재구성이다(Docs/Design/AssetSystem 12.14절 A5j2a — UE
// UTexture2DArray·UTextureCubeArray::UpdateSourceFromSourceTextures, UVolumeTexture::
// UpdateSourceFromSourceTexture·SetDefaultSource2DTileSize). 묶음 원소의 mip 0 byte를 이어 자기
// 원본으로 복사한다(쿠킹은 자기 원본만 읽는다 — Build 의존 없음).
namespace rocks {
namespace {

constexpr StringView SourceTexturesField = "sourceTextures";
constexpr StringView Source2DTextureField = "source2DTexture";
constexpr StringView TileSizeXField = "source2DTileSizeX";
constexpr StringView TileSizeYField = "source2DTileSizeY";
// 처음 만들기가 원소에서 가져오는 칸이다(대입되었으면 대입 값을 둔다).
constexpr StringView CompressionSettingsField = "compressionSettings";
constexpr StringView MipGenSettingsField = "mipGenSettings";
constexpr StringView PowerOfTwoModeField = "powerOfTwoMode";
constexpr StringView BundleLodGroupField = "lodGroup";
constexpr StringView NeverStreamField = "neverStream";

// 원소 수 상한(임포트와 같다 — 12.12절)과 결과 byte 한도(디코더 한도 — 12.9절)다.
constexpr uint32 BundleMaxArraySlices = static_cast<uint32>(MaxImportSources);
constexpr uint32 BundleMaxCubes = static_cast<uint32>(MaxImportSources) / TextureCubeFaces;
constexpr uint64 BundleMaxSourceBytes = image::ImageDecodeLimits::DefaultMaxPixelBytes;
// UE MAX_VOLUME_TEXTURE_DEPTH(VolumeTexture.cpp 46)다.
constexpr uint32 MaxVolumeTextureDepth = 512;
// asset-new VolumeTexture의 팩토리 기본 원본 texel이다(UE VolumeTextureFactory.cpp 63–68 — BGRA8
// (0, 0, 0, 255)를 Rocks 정규 RGBA8로).
constexpr uint8 VolumePlaceholderTexel[4] = {0, 0, 0, 255};

bool HasField(Span<const StringView> fields, StringView name) noexcept {
    for (StringView field : fields) {
        if (EqualsIgnoringAsciiCase(field, name)) {
            return true;
        }
    }

    return false;
}

bool HasSource(const TextureSource& source) noexcept {
    return source.format != TextureSourceFormat::Invalid && !source.bulk.IsEmpty() &&
           source.sizeX != 0 && source.sizeY != 0 && source.numSlices != 0;
}

// UE GetGammaSpace(0) == sRGB다(Texture.cpp 3189–3213 — 8비트 정수 형식·SRGB on·legacy gamma off).
bool SourceIsSrgb(const Texture& texture) noexcept {
    const TextureSourceFormat format = texture.source.format;
    const bool eightBit = format == TextureSourceFormat::G8 || format == TextureSourceFormat::GA8 ||
                          format == TextureSourceFormat::RGBA8;
    return texture.srgb != 0 && texture.useLegacyGamma == 0 && eightBit;
}

// x × y × slices × texel byte다(넘치거나 한도를 넘으면 false).
bool SourceBytes(TextureSourceFormat format, uint64 x, uint64 y, uint64 slices,
                 uint64& out) noexcept {
    const uint64 texel = TextureSourceTexelBytes(format);
    if (texel == 0 || x == 0 || y == 0 || slices == 0) {
        return false;
    }

    const uint64 max = NumericLimits<uint64>::Max();
    if (x > max / y || x * y > max / slices || x * y * slices > max / texel) {
        return false;
    }

    out = x * y * slices * texel;
    return out <= BundleMaxSourceBytes;
}

// 원소 하나의 값 사본이다(Borrow를 오래 쥐지 않는다).
struct ElementFacts {
    TextureSource source;
    bool srgb = false;
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
    TextureGroup lodGroup = TextureGroup::World;
};

Result<ElementFacts> ReadElement(const AssetTypeHookContext& context,
                                 ObjectHandle element) noexcept {
    auto borrowed = context.registry.Resolve<Texture>(context.access, element);
    if (!borrowed) {
        return Result<ElementFacts>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<ElementFacts>::Failure(value.Error());
    }

    const Texture& texture = *value.Value();
    ElementFacts facts;
    facts.source = texture.source;
    facts.srgb = SourceIsSrgb(texture);
    facts.compression = texture.compressionSettings;
    facts.lodGroup = texture.lodGroup;
    return Result<ElementFacts>::Success(facts);
}

// 원소 원본의 mip 0 byte(bytes)를 out 끝에 붙인다(bulk가 짧으면 InvalidSource).
Result<void> AppendMip0(AssetSourceRebuildSink& sink, ObjectHandle element,
                        const TextureSource& source, uint64 bytes, uint64 index,
                        Array<uint8>& out) noexcept {
    auto read = sink.ReadBulk(element, source.bulk);
    if (!read) {
        return sink.Fail(AssetDiagnosticCode::InvalidSource, read.Error(), index);
    }

    if (read.Value().Size() < bytes) {
        return sink.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, index);
    }

    return out.TryAppend(read.Value().Slice(0, static_cast<usize>(bytes)));
}

// 자기 텍스처를 쓰기 Borrow로 fn에 준다.
template <class Fn>
Result<void> WriteSelf(const AssetTypeHookContext& context, Fn&& fn) noexcept {
    auto borrowed = context.registry.ResolveForWrite<Texture>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    fn(*value.Value());
    return Result<void>::Success();
}

// 2DArray·CubeArray 공통이다(12.14절 2DArray·CubeArray 규칙 1–6번).
Result<void> RebuildArray(const AssetTypeHookContext& context,
                          Span<const StringView> assignedFields, AssetSourceRebuildSink& sink,
                          bool cube) noexcept {
    Allocator& allocator = context.registry.ObjectAllocator();
    Array<ObjectHandle> elements(allocator);
    bool selfEmpty = false;
    {
        auto borrowed = context.registry.Resolve<Texture>(context.access, context.object);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        const Texture& self = *value.Value();
        selfEmpty = !HasSource(self.source);
        usize count = 0;
        if (cube) {
            const auto& list = static_cast<const TextureCubeArray&>(self).sourceTextures;
            count = list.Size();
            if (auto reserved = elements.TryReserve(count); !reserved) {
                return reserved;
            }

            for (usize index = 0; index < count; ++index) {
                (void)elements.TryPushBack(list[index]);
            }
        } else {
            const auto& list = static_cast<const Texture2DArray&>(self).sourceTextures;
            count = list.Size();
            if (auto reserved = elements.TryReserve(count); !reserved) {
                return reserved;
            }

            for (usize index = 0; index < count; ++index) {
                (void)elements.TryPushBack(list[index]);
            }
        }
    }

    // 부른 까닭(Edit·New는 대입 필드, Propagate는 그 원소를 가리킬 때).
    if (sink.Cause() == AssetSourceRebuildCause::Propagate) {
        bool references = false;
        for (ObjectHandle element : elements) {
            references = references || element == sink.ChangedSource();
        }

        if (!references) {
            return Result<void>::Success();
        }
    } else if (!HasField(assignedFields, SourceTexturesField)) {
        return Result<void>::Success();
    }

    // 1. 빈 목록 → 빈 원본(UE InvalidateTextureSource). 파일로 임포트한 배열(ImportRecord)은 묶음이
    // 원본을 정하지 않으므로 그대로 둔다(되돌리기로 빈 목록을 두는 것은 받는다).
    if (elements.IsEmpty()) {
        if (sink.HasImportRecord()) {
            return Result<void>::Success();
        }

        return WriteSelf(context, [](Texture& self) noexcept { self.source = TextureSource{}; });
    }

    // 임포트한 배열 보호(UE bSourceGeneratedFromSourceTexturesArray).
    if (sink.HasImportRecord()) {
        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument, 0);
    }

    const uint32 limit = cube ? BundleMaxCubes : BundleMaxArraySlices;
    if (elements.Size() > limit) {
        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument, limit);
    }

    // 2. 원소 판정(첫 원소 기준).
    const uint32 slicesPerElement = cube ? TextureCubeFaces : 1u;
    Array<ElementFacts> facts(allocator);
    if (auto reserved = facts.TryReserve(elements.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < elements.Size(); ++index) {
        if (elements[index].IsNull()) {
            return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                             index);
        }

        auto read = ReadElement(context, elements[index]);
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        const ElementFacts& element = read.Value();
        if (!HasSource(element.source)) {
            return sink.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, index);
        }

        if (element.source.numLayers != 1) {
            return sink.Fail(AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported,
                             index);
        }

        const ElementFacts* first = facts.IsEmpty() ? &element : &facts[0];
        if (element.source.numSlices != slicesPerElement ||
            element.source.sizeX != first->source.sizeX ||
            element.source.sizeY != first->source.sizeY ||
            element.source.format != first->source.format || element.srgb != first->srgb) {
            return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                             index);
        }

        (void)facts.TryPushBack(element);
    }

    // 3. 새 원본(원소마다 mip 0 byte를 원소 순으로 잇는다).
    const ElementFacts& first = facts[0];
    const uint64 slices = uint64{slicesPerElement} * elements.Size();
    uint64 total = 0;
    uint64 perElement = 0;
    if (!SourceBytes(first.source.format, first.source.sizeX, first.source.sizeY, slices, total) ||
        !SourceBytes(first.source.format, first.source.sizeX, first.source.sizeY, slicesPerElement,
                     perElement)) {
        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::CapacityExceeded, 0);
    }

    Array<uint8> bytes(allocator);
    if (auto reserved = bytes.TryReserve(static_cast<usize>(total)); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < elements.Size(); ++index) {
        if (auto appended =
                AppendMip0(sink, elements[index], facts[index].source, perElement, index, bytes);
            !appended) {
            return appended;
        }
    }

    auto added = sink.AddBulk(Move(bytes));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    const BulkDataRef ref = added.Value();
    // 5. 처음 만들기 설정(대입한 칸은 둔다).
    const bool creating = selfEmpty;
    return WriteSelf(context, [&](Texture& self) noexcept {
        TextureSource source;
        source.format = first.source.format;
        source.sizeX = first.source.sizeX;
        source.sizeY = first.source.sizeY;
        source.numSlices = static_cast<uint32>(slices);
        source.numMips = 1;
        source.numLayers = 1;
        source.bulk = ref;
        self.source = source;
        // 4. 배열 SRGB = 첫 원소의 sRGB 여부.
        self.srgb = first.srgb ? 1 : 0;
        if (creating) {
            if (!HasField(assignedFields, CompressionSettingsField)) {
                self.compressionSettings = first.compression;
            }

            if (!HasField(assignedFields, MipGenSettingsField)) {
                self.mipGenSettings = TextureMipGenSettings::NoMipmaps;
            }

            if (!HasField(assignedFields, PowerOfTwoModeField)) {
                self.powerOfTwoMode = TexturePowerOfTwoSetting::None;
            }

            if (!HasField(assignedFields, BundleLodGroupField)) {
                self.lodGroup = first.lodGroup;
            }

            if (!HasField(assignedFields, NeverStreamField)) {
                self.neverStream = 1;
            }
        }
    });
}

Result<void> RebuildTexture2DArray(const AssetTypeHookContext& context,
                                   Span<const StringView> assignedFields,
                                   AssetSourceRebuildSink& sink) noexcept {
    return RebuildArray(context, assignedFields, sink, false);
}

Result<void> RebuildTextureCubeArray(const AssetTypeHookContext& context,
                                     Span<const StringView> assignedFields,
                                     AssetSourceRebuildSink& sink) noexcept {
    return RebuildArray(context, assignedFields, sink, true);
}

// round(value^(1/3))이다(정수 — (2t − 1)³ ≤ 8N < (2t + 1)³, 반올림은 UE RoundToInt처럼 위로).
uint64 RoundCubeRoot(uint64 value) noexcept {
    uint64 root = 0;
    while ((2 * root + 1) * (2 * root + 1) * (2 * root + 1) <= 8 * value) {
        ++root;
    }

    return root;
}

// round(sqrt(value))이다(정수 — (2s − 1)² ≤ 4P < (2s + 1)²).
uint64 RoundSquareRoot(uint64 value) noexcept {
    uint64 root = 0;
    while ((2 * root + 1) * (2 * root + 1) <= 4 * value) {
        ++root;
    }

    return root;
}

Result<void> RebuildVolumeTexture(const AssetTypeHookContext& context,
                                  Span<const StringView> assignedFields,
                                  AssetSourceRebuildSink& sink) noexcept {
    Allocator& allocator = context.registry.ObjectAllocator();
    ObjectHandle source2D{};
    int32 tileX = 0;
    int32 tileY = 0;
    bool selfEmpty = false;
    {
        auto borrowed = context.registry.Resolve<Texture>(context.access, context.object);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        const auto& self = static_cast<const VolumeTexture&>(*value.Value());
        source2D = self.source2DTexture.Handle();
        tileX = self.source2DTileSizeX;
        tileY = self.source2DTileSizeY;
        selfEmpty = !HasSource(self.source);
    }

    const AssetSourceRebuildCause cause = sink.Cause();
    const bool textureAssigned = HasField(assignedFields, Source2DTextureField);
    const bool tileAssigned =
        HasField(assignedFields, TileSizeXField) || HasField(assignedFields, TileSizeYField);
    if (cause == AssetSourceRebuildCause::Propagate) {
        if (source2D.IsNull() || source2D != sink.ChangedSource()) {
            return Result<void>::Success();
        }
    } else if (!textureAssigned && !tileAssigned) {
        // 5. asset-new로 원본 2D 없이 만든 VolumeTexture는 팩토리 기본 원본을 둔다.
        if (cause != AssetSourceRebuildCause::New || !source2D.IsNull() || !selfEmpty) {
            return Result<void>::Success();
        }

        Array<uint8> texel(allocator);
        if (auto appended = texel.TryAppend(Span<const uint8>(VolumePlaceholderTexel)); !appended) {
            return appended;
        }

        auto added = sink.AddBulk(Move(texel));
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        const BulkDataRef ref = added.Value();
        return WriteSelf(context, [&](Texture& self) noexcept {
            TextureSource placeholder;
            placeholder.format = TextureSourceFormat::RGBA8;
            placeholder.sizeX = 1;
            placeholder.sizeY = 1;
            placeholder.numSlices = 1;
            placeholder.bulk = ref;
            self.source = placeholder;
        });
    }

    // 파일로 임포트한 Volume(ImportRecord)은 원본 2D가 없으면 묶음이 원본을 정하지 않는다(타일만
    // 고쳐도 원본을 지우지 않는다), 원본 2D를 대입하면 거부한다.
    if (sink.HasImportRecord()) {
        if (source2D.IsNull()) {
            return Result<void>::Success();
        }

        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument, 0);
    }

    ElementFacts element;
    bool valid = false;
    if (!source2D.IsNull()) {
        auto read = ReadElement(context, source2D);
        if (!read) {
            return Result<void>::Failure(read.Error());
        }

        element = read.Value();
        valid = HasSource(element.source);
    }

    // 1. 원본 2D를 대입하고 타일을 대입하지 않았으면 기본 타일(UE SetDefaultSource2DTileSize).
    if (cause != AssetSourceRebuildCause::Propagate && textureAssigned && !tileAssigned) {
        tileX = 0;
        tileY = 0;
        if (valid) {
            const uint64 x = element.source.sizeX;
            const uint64 y = element.source.sizeY;
            uint64 tile = RoundCubeRoot(x * y);
            tile = tile == 0 ? 1 : tile;
            uint64 side = RoundSquareRoot((x / tile) * (y / tile));
            side = side == 0 ? 1 : side;
            tileX = static_cast<int32>(x / side);
            tileY = static_cast<int32>(y / side);
        }
    }

    // 2. 타일을 떼어 깊이로 잇는다(왼쪽 → 오른쪽, 위 → 아래, 나머지 버림, 512 상한).
    uint64 depth = 0;
    if (valid && tileX > 0 && tileY > 0) {
        const uint64 columns = element.source.sizeX / static_cast<uint64>(tileX);
        const uint64 rows = element.source.sizeY / static_cast<uint64>(tileY);
        depth = columns * rows;
        depth = depth > MaxVolumeTextureDepth ? MaxVolumeTextureDepth : depth;
    }

    if (depth == 0) {
        // 3. 원본을 만들 수 없으면 비운다(UE와 같다 — 경고를 더한다).
        if (auto written = WriteSelf(context,
                                     [&](Texture& self) noexcept {
                                         auto& volume = static_cast<VolumeTexture&>(self);
                                         volume.source = TextureSource{};
                                         volume.source2DTileSizeX = tileX;
                                         volume.source2DTileSizeY = tileY;
                                     });
            !written) {
            return written;
        }

        return sink.Warn(AssetDiagnosticCode::SourceCleared, 0);
    }

    if (element.source.numLayers != 1) {
        return sink.Fail(AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported, 0);
    }

    const uint64 texel = TextureSourceTexelBytes(element.source.format);
    uint64 total = 0;
    uint64 plane = 0;
    if (!SourceBytes(element.source.format, static_cast<uint64>(tileX), static_cast<uint64>(tileY),
                     depth, total) ||
        !SourceBytes(element.source.format, element.source.sizeX, element.source.sizeY,
                     element.source.numSlices, plane)) {
        return sink.Fail(AssetDiagnosticCode::InvalidSetting, ErrorCode::CapacityExceeded, 0);
    }

    auto read = sink.ReadBulk(source2D, element.source.bulk);
    if (!read) {
        return sink.Fail(AssetDiagnosticCode::InvalidSource, read.Error(), 0);
    }

    // 원본 2D의 첫 slice만 쓴다(UE GetMipData(0)의 앞 — Texture2D는 slice 1).
    const uint64 rowBytes = uint64{element.source.sizeX} * texel;
    if (read.Value().Size() < uint64{element.source.sizeY} * rowBytes) {
        return sink.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, 0);
    }

    Array<uint8> bytes(allocator);
    if (auto resized = bytes.TryResize(static_cast<usize>(total)); !resized) {
        return resized;
    }

    const uint64 columns = element.source.sizeX / static_cast<uint64>(tileX);
    const uint64 tileRowBytes = static_cast<uint64>(tileX) * texel;
    uint8* cursor = bytes.Data();
    const uint8* origin = read.Value().Data();
    for (uint64 z = 0; z < depth; ++z) {
        const uint64 baseX = (z % columns) * static_cast<uint64>(tileX);
        const uint64 baseY = (z / columns) * static_cast<uint64>(tileY);
        for (uint64 row = 0; row < static_cast<uint64>(tileY); ++row) {
            platform::MemoryCopy(cursor, origin + (baseY + row) * rowBytes + baseX * texel,
                                 static_cast<usize>(tileRowBytes));
            cursor += tileRowBytes;
        }
    }

    auto added = sink.AddBulk(Move(bytes));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    const BulkDataRef ref = added.Value();
    const TextureSourceFormat format = element.source.format;
    return WriteSelf(context, [&](Texture& self) noexcept {
        auto& volume = static_cast<VolumeTexture&>(self);
        TextureSource source;
        source.format = format;
        source.sizeX = static_cast<uint32>(tileX);
        source.sizeY = static_cast<uint32>(tileY);
        source.numSlices = static_cast<uint32>(depth);
        source.bulk = ref;
        volume.source = source;
        volume.source2DTileSizeX = tileX;
        volume.source2DTileSizeY = tileY;
    });
}

} // namespace

namespace texture_import {

Result<void> RegisterTextureSourceRebuildHooks(AssetTypeHookTable& table,
                                               Array<AssetTypeHookRegistration>& tokens) noexcept {
    struct Row {
        TypeId type;
        AssetSourceRebuildHook hook;
    };
    const Row rows[] = {{TypeIdOf<Texture2DArray>(), &RebuildTexture2DArray},
                        {TypeIdOf<TextureCubeArray>(), &RebuildTextureCubeArray},
                        {TypeIdOf<VolumeTexture>(), &RebuildVolumeTexture}};
    if (auto reserved = tokens.TryReserve(tokens.Size() + sizeof(rows) / sizeof(rows[0]));
        !reserved) {
        return reserved;
    }

    for (const Row& row : rows) {
        AssetTypeHooks hooks;
        hooks.rebuildSource = row.hook;
        auto registered = table.Register(row.type, hooks);
        if (!registered) {
            return Result<void>::Failure(registered.Error());
        }

        (void)tokens.TryPushBack(Move(registered.Value()));
    }

    return Result<void>::Success();
}

} // namespace texture_import

} // namespace rocks
