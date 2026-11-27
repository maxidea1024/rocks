#include "ExchangeImporterCommon.h"

#include <Animation/Skeleton.generated.h>
#include <AnimationTools/SkeletonImport.h>
#include <UnrealExchange/ExchangeSkeleton.h>
#include <UnrealExchange/UnrealExchangeFormat.h>
#include <UnrealExchange/UnrealExchangeImportOptions.generated.h>
#include <UnrealExchange/UnrealSkeletonImporter.h>

// 교환 스켈레톤 A2f 임포터다(Docs/Design/UnrealExchange 6.4절 8번, 10.6절 5–7번).

namespace rocks {

namespace unreal_skeleton_import {

// 임포터 id·버전이다(6.4절 8번).
constexpr Guid ImporterId{0x932f987c2f7a4160ull, 0x97fc9c9a2bb4767full};
constexpr uint32 ImporterVersion = 1;
constexpr StringView AssetRoles[] = {UnrealExchangeAssetRole};
constexpr StringView BlobRolePrefixes[] = {UnrealExchangeBlobRolePrefix};

} // namespace unreal_skeleton_import

UnrealSkeletonImporter::UnrealSkeletonImporter() noexcept {
    using namespace unreal_skeleton_import;
    info_.id = ImporterId;
    info_.name = UnrealSkeletonImporterName;
    info_.version = ImporterVersion;
    info_.outputType = TypeIdOf<Skeleton>();
    info_.roles = Span<const StringView>(AssetRoles);
    info_.rolePrefixes = Span<const StringView>(BlobRolePrefixes);
    info_.tagPrefix = UnrealExchangeTagPrefix;
    info_.optionsType = TypeIdOf<UnrealExchangeImportOptions>();
}

Result<void> UnrealSkeletonImporter::Import(AssetImportContext& context) noexcept {
    Allocator& allocator = context.Registry().ObjectAllocator();

    // 1–3. 문서·클래스·옵션 objectPath.
    ExchangeAsset asset(allocator);
    const UnrealExchangeImportOptions* options = nullptr;
    if (auto read =
            exchange_detail::ReadImportDocument(context, ExchangeClass::Skeleton, asset, options);
        !read) {
        return read;
    }

    // 4·5. blob 원본(스켈레톤 문서는 blob이 없다 — 쓰지 않는 blob 원본을 거부하려고 모은다).
    Array<Span<const uint8>> blobs(allocator);
    if (auto collected = exchange_detail::CollectImportBlobs(context, asset, blobs); !collected) {
        return collected;
    }

    ExchangeSkeletonInput input;
    input.asset = &asset;
    input.references = &options->references;
    input.registry = &context.Registry();
    input.access = context.Access();
    ExchangeSkeleton skeleton(allocator);
    ExchangeMeshFailure failure;
    if (auto built = skeleton.Build(input, failure); !built) {
        return failure.code == AssetDiagnosticCode::None
                   ? Result<void>::Failure(built.Error())
                   : context.Fail(failure.code, built.Error(), failure.detail);
    }

    for (const ExchangeSettingWarning& warning : skeleton.Warnings()) {
        if (auto warned = context.Warn(warning.code, warning.detail); !warned) {
            return warned;
        }
    }

    auto output = context.Output({}, info_.outputType);
    if (!output) {
        return Result<void>::Failure(output.Error());
    }

    if (auto imported = ImportSkeleton(context, output.Value(), skeleton.Spec()); !imported) {
        return imported;
    }

    return exchange_detail::AddExchangeTags(context, *options);
}

} // namespace rocks
