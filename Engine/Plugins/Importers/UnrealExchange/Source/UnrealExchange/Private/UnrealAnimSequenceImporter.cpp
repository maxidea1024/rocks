#include "ExchangeImporterCommon.h"

#include <Animation/AnimationClip.generated.h>
#include <AnimationTools/AnimationClipImport.h>
#include <UnrealExchange/ExchangeAnimationClip.h>
#include <UnrealExchange/UnrealExchangeFormat.h>
#include <UnrealExchange/UnrealExchangeImportOptions.generated.h>
#include <UnrealExchange/UnrealAnimSequenceImporter.h>

// 교환 애니메이션 클립 A2f 임포터다(Docs/Design/UnrealExchange 7.2절 7번, 10.6절 5–7번).

namespace rocks {

namespace unreal_anim_sequence_import {

// 임포터 id·버전이다(7.2절 7번).
constexpr Guid ImporterId{0x8305f84f40d6430cull, 0x974be2e8377143e5ull};
constexpr uint32 ImporterVersion = 1;
constexpr StringView AssetRoles[] = {UnrealExchangeAssetRole};
constexpr StringView BlobRolePrefixes[] = {UnrealExchangeBlobRolePrefix};

} // namespace unreal_anim_sequence_import

UnrealAnimSequenceImporter::UnrealAnimSequenceImporter() noexcept {
    using namespace unreal_anim_sequence_import;
    info_.id = ImporterId;
    info_.name = UnrealAnimSequenceImporterName;
    info_.version = ImporterVersion;
    info_.outputType = TypeIdOf<AnimationClip>();
    info_.roles = Span<const StringView>(AssetRoles);
    info_.rolePrefixes = Span<const StringView>(BlobRolePrefixes);
    info_.tagPrefix = UnrealExchangeTagPrefix;
    info_.optionsType = TypeIdOf<UnrealExchangeImportOptions>();
}

Result<void> UnrealAnimSequenceImporter::Import(AssetImportContext& context) noexcept {
    Allocator& allocator = context.Registry().ObjectAllocator();

    // 1–3. 문서·클래스·옵션 objectPath.
    ExchangeAsset asset(allocator);
    const UnrealExchangeImportOptions* options = nullptr;
    if (auto read = exchange_detail::ReadImportDocument(context, ExchangeClass::AnimSequence, asset,
                                                        options);
        !read) {
        return read;
    }

    // 4·5. blob 원본.
    Array<Span<const uint8>> blobs(allocator);
    if (auto collected = exchange_detail::CollectImportBlobs(context, asset, blobs); !collected) {
        return collected;
    }

    // 명세(7.2절 1–6번).
    ExchangeAnimationClipInput input;
    input.asset = &asset;
    input.blobs = blobs.View();
    input.references = &options->references;
    input.registry = &context.Registry();
    input.access = context.Access();
    ExchangeAnimationClip clip(allocator);
    ExchangeMeshFailure failure;
    if (auto built = clip.Build(input, failure); !built) {
        return failure.code == AssetDiagnosticCode::None
                   ? Result<void>::Failure(built.Error())
                   : context.Fail(failure.code, built.Error(), failure.detail);
    }

    for (const ExchangeSettingWarning& warning : clip.Warnings()) {
        if (auto warned = context.Warn(warning.code, warning.detail); !warned) {
            return warned;
        }
    }

    auto output = context.Output({}, info_.outputType);
    if (!output) {
        return Result<void>::Failure(output.Error());
    }

    if (auto imported = ImportAnimationClip(context, output.Value(), clip.Spec()); !imported) {
        return imported;
    }

    return exchange_detail::AddExchangeTags(context, *options);
}

} // namespace rocks
