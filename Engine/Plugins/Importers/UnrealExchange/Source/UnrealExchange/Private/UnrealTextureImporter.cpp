#include "ExchangeImporterCommon.h"

#include <Core/Text/StringBuilder.h>
#include <ReflectionText/TextJson.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>
#include <UnrealExchange/ExchangeSettings.h>
#include <UnrealExchange/ExchangeTextureSettings.h>
#include <UnrealExchange/ExchangeTextureSource.h>
#include <UnrealExchange/UnrealExchangeFormat.h>
#include <UnrealExchange/UnrealExchangeImportOptions.generated.h>
#include <UnrealExchange/UnrealExchangeImportOptions.h>
#include <UnrealExchange/UnrealTextureImporter.h>

// 교환 텍스처 A2f 임포터다(Docs/Design/UnrealExchange 4.6절, 10.6절 5–8번).

namespace rocks {

namespace unreal_texture_import {

using exchange_detail::FailImport;
using reflection_text::TextReferenceKind;

// 임포터 정보 하나다(4.6절 1번 표).
struct ImporterEntry {
    ExchangeClass type;
    StringView name;
    Guid id;
};

constexpr ImporterEntry Importers[] = {
    {ExchangeClass::Texture2D, "unreal_texture_2d",
     Guid{0x337d13db1df44802ull, 0xba4755da35a5b39dull}},
    {ExchangeClass::TextureCube, "unreal_texture_cube",
     Guid{0x93f7e0e111764305ull, 0xa63e6497472c9260ull}},
    {ExchangeClass::Texture2DArray, "unreal_texture_2d_array",
     Guid{0x12ee1fb95e5241a5ull, 0x9405dfe8d9406ca1ull}},
    {ExchangeClass::TextureCubeArray, "unreal_texture_cube_array",
     Guid{0xa49f013e97bc4fe4ull, 0xb46dfb6b0f126a37ull}},
    {ExchangeClass::VolumeTexture, "unreal_volume_texture",
     Guid{0xbabb45b27cbe49c3ull, 0xac8a9bad95195d23ull}},
};

// 임포터 버전이다(4.6절 1번).
constexpr uint32 ImporterVersion = 1;
constexpr StringView AssetRoles[] = {UnrealExchangeAssetRole};
constexpr StringView BlobRolePrefixes[] = {UnrealExchangeBlobRolePrefix};

const ImporterEntry* FindEntry(ExchangeClass type) noexcept {
    for (const ImporterEntry& entry : Importers) {
        if (entry.type == type) {
            return &entry;
        }
    }

    return nullptr;
}

TypeId OutputType(ExchangeClass type) noexcept {
    switch (type) {
    case ExchangeClass::Texture2D:
        return TypeIdOf<Texture2D>();
    case ExchangeClass::TextureCube:
        return TypeIdOf<TextureCube>();
    case ExchangeClass::Texture2DArray:
        return TypeIdOf<Texture2DArray>();
    case ExchangeClass::TextureCubeArray:
        return TypeIdOf<TextureCubeArray>();
    case ExchangeClass::VolumeTexture:
        return TypeIdOf<VolumeTexture>();
    default:
        return TypeId{};
    }
}

// 설정의 참조 텍스트(UE objectPath)를 옵션 references의 AssetId로 열어 Hard로 건다(10.6절 8번).
class ReferenceResolver final : public reflection_text::TextReferenceResolver {
public:
    ReferenceResolver(AssetImportContext& context,
                      const OrderedMap<String, AssetId>& references) noexcept
        : context_(context), references_(references) {
    }

    Result<void> Resolve(StringView text, TypeId expected, TextReferenceKind kind,
                         FieldValue& out) noexcept override {
        const AssetId* id = references_.Find(text);
        if (kind != TextReferenceKind::Hard || id == nullptr) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        auto opened = context_.OpenAsset(*id);
        if (!opened) {
            return Result<void>::Failure(opened.Error());
        }

        if (!(expected == TypeId{})) {
            auto lease = context_.Registry().AcquireType(context_.Access(), opened.Value());
            if (!lease) {
                return Result<void>::Failure(lease.Error());
            }

            if (!lease.Value().IsA(expected)) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }

        out.reference = opened.Value();
        return Result<void>::Success();
    }

    Result<void> Format(TextReferenceKind, const FieldValue&,
                        StringBuilderBase&) noexcept override {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

private:
    AssetImportContext& context_;
    const OrderedMap<String, AssetId>& references_;
};

// 설정을 번역해 출력에 ImportJson·게시하고 경고를 남긴다(10.6절 8번, 4.6절 3번).
Result<void> ApplySettings(AssetImportContext& context, const ExchangeAsset& asset,
                           const UnrealExchangeImportOptions& options, ObjectHandle output,
                           Allocator& allocator) noexcept {
    StringBuilder<> text(allocator);
    JsonWriter writer(text);
    Array<ExchangeSettingWarning> warnings(allocator);
    ExchangeSettingInput input;
    input.settings = asset.settings;
    input.rows = ExchangeTextureSettingRows();
    input.output = asset.exchangeClass;
    input.references = &options.references;
    // 번역의 CorruptData만 덤프 내용이다. 행 표 결함·writer 오류는 진단 코드 없이 그대로다
    // (UnrealExchange 5.5절 4번 — 정적 메시와 같은 분담).
    if (auto translated = TranslateExchangeSettings(input, writer, warnings); !translated) {
        return translated.Error() == ErrorCode::CorruptData
                   ? context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData)
                   : Result<void>::Failure(translated.Error());
    }

    if (auto finished = writer.Finish(); !finished) {
        return Result<void>::Failure(finished.Error());
    }

    JsonDocument document(allocator);
    const JsonError parsed = document.Parse(text.View());
    if (parsed.code == JsonErrorCode::OutOfMemory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (!parsed.Succeeded()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ReferenceResolver resolver(context, options.references);
    reflection_text::TextCodecOptions codec;
    codec.resolver = &resolver;
    reflection_text::TextDiagnostics diagnostics(allocator);
    auto prepared = reflection_text::ImportJson(
        document.Root(),
        reflection_text::AssignmentTarget::Live(context.Registry(), context.Access(), output),
        codec, diagnostics);
    if (!prepared) {
        // 진단을 남긴 거부(값 형식·범위·모르는 키)는 덤프 내용의 문제다(10.3절 오류 값). 진단이
        // 없는 InvalidArgument는 호출 오류라 그대로다.
        const bool content =
            prepared.Error() == ErrorCode::InvalidArgument && diagnostics.Count() != 0;
        return content ? context.Fail(AssetDiagnosticCode::InvalidSource, ErrorCode::CorruptData)
                       : Result<void>::Failure(prepared.Error());
    }

    if (auto published = prepared.Value().Publish(); !published) {
        return Result<void>::Failure(published.Error());
    }

    for (const ExchangeSettingWarning& warning : warnings) {
        if (auto warned = context.Warn(warning.code, warning.detail); !warned) {
            return warned;
        }
    }

    return Result<void>::Success();
}

// 층 감마와 SRGB·bUseLegacyGamma 설정이 맞는지다(4.6절 4번).
bool GammaMatches(ExchangeGammaSpace gamma, const Texture& texture) noexcept {
    switch (gamma) {
    case ExchangeGammaSpace::Linear:
        return texture.srgb == 0;
    case ExchangeGammaSpace::Pow22:
        return texture.srgb != 0 && texture.useLegacyGamma != 0;
    case ExchangeGammaSpace::Srgb:
        return texture.srgb != 0 && texture.useLegacyGamma == 0;
    }

    return false;
}

// 원본을 쓰고 감마를 대조한다.
Result<void> WriteSource(AssetImportContext& context, ObjectHandle output,
                         ExchangeTextureSource& converted) noexcept {
    const uint32 sizeX = converted.sizeX;
    const uint32 sizeY = converted.sizeY;
    const uint32 slices = converted.numSlices;
    auto bulk = context.AddBulk(output, rocks::Move(converted.pixels));
    if (!bulk) {
        return Result<void>::Failure(bulk.Error());
    }

    auto borrowed = context.Registry().ResolveForWrite<Texture>(context.Access(), output);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    Texture& texture = *value.Value();
    texture.source.format = converted.format;
    texture.source.sizeX = sizeX;
    texture.source.sizeY = sizeY;
    texture.source.numSlices = slices;
    texture.source.numMips = 1;
    texture.source.numLayers = 1;
    texture.source.bulk = bulk.Value();
    if (!GammaMatches(converted.gamma, texture)) {
        return context.Warn(AssetDiagnosticCode::GammaMismatch,
                            static_cast<uint64>(converted.gamma));
    }

    return Result<void>::Success();
}

} // namespace unreal_texture_import

StringView UnrealTextureImporterName(ExchangeClass type) noexcept {
    using namespace unreal_texture_import;
    const ImporterEntry* entry = FindEntry(type);
    return entry != nullptr ? entry->name : StringView();
}

UnrealTextureImporter::UnrealTextureImporter(ExchangeClass type) noexcept : type_(type) {
    using namespace unreal_texture_import;
    const ImporterEntry* entry = FindEntry(type);
    if (entry != nullptr) {
        info_.id = entry->id;
        info_.name = entry->name;
    }

    info_.version = ImporterVersion;
    info_.outputType = OutputType(type);
    info_.roles = Span<const StringView>(AssetRoles);
    info_.rolePrefixes = Span<const StringView>(BlobRolePrefixes);
    info_.tagPrefix = UnrealExchangeTagPrefix;
    info_.optionsType = TypeIdOf<UnrealExchangeImportOptions>();
}

Result<void> UnrealTextureImporter::Import(AssetImportContext& context) noexcept {
    using namespace unreal_texture_import;
    Allocator& allocator = context.Registry().ObjectAllocator();

    // 1–3. 문서·클래스·옵션 objectPath.
    ExchangeAsset asset(allocator);
    const UnrealExchangeImportOptions* options = nullptr;
    if (auto read = exchange_detail::ReadImportDocument(context, type_, asset, options); !read) {
        return read;
    }

    // 4·5. blob 원본.
    Array<Span<const uint8>> blobs(allocator);
    if (auto collected = exchange_detail::CollectImportBlobs(context, asset, blobs); !collected) {
        return collected;
    }

    // 원본 변환(4.6절 2번).
    ExchangeTextureSource converted(allocator);
    ExchangeTextureFailure failure;
    if (auto made = ConvertExchangeTextureSource(asset, blobs.View(), converted, failure); !made) {
        return failure.code == AssetDiagnosticCode::None ? Result<void>::Failure(made.Error())
                                                         : context.Fail(failure.code, made.Error());
    }

    auto output = context.Output({}, info_.outputType);
    if (!output) {
        return Result<void>::Failure(output.Error());
    }

    if (auto applied = ApplySettings(context, asset, *options, output.Value(), allocator);
        !applied) {
        return applied;
    }

    if (auto written = WriteSource(context, output.Value(), converted); !written) {
        return written;
    }

    return exchange_detail::AddExchangeTags(context, *options);
}

} // namespace rocks
