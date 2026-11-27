#include "ExchangeImporterCommon.h"

#include <Mesh/SkeletalMesh/SkeletalMesh.generated.h>
#include <MeshTools/SkeletalMeshImport.h>
#include <UnrealExchange/ExchangeSkeletalMesh.h>
#include <UnrealExchange/UnrealExchangeFormat.h>
#include <UnrealExchange/UnrealExchangeImportOptions.generated.h>
#include <UnrealExchange/UnrealSkeletalMeshImporter.h>

// 교환 스킨 메시 A2f 임포터다(Docs/Design/UnrealExchange 6.4절 8번, 10.6절 5–7번).

namespace rocks {

namespace unreal_skeletal_mesh_import {

// 임포터 id·버전이다(6.4절 8번).
constexpr Guid ImporterId{0x61d142d7c91a4414ull, 0x907d86734331e21bull};
constexpr uint32 ImporterVersion = 1;
constexpr StringView AssetRoles[] = {UnrealExchangeAssetRole};
constexpr StringView BlobRolePrefixes[] = {UnrealExchangeBlobRolePrefix};

} // namespace unreal_skeletal_mesh_import

UnrealSkeletalMeshImporter::UnrealSkeletalMeshImporter() noexcept {
    using namespace unreal_skeletal_mesh_import;
    info_.id = ImporterId;
    info_.name = UnrealSkeletalMeshImporterName;
    info_.version = ImporterVersion;
    info_.outputType = TypeIdOf<SkeletalMesh>();
    info_.roles = Span<const StringView>(AssetRoles);
    info_.rolePrefixes = Span<const StringView>(BlobRolePrefixes);
    info_.tagPrefix = UnrealExchangeTagPrefix;
    info_.optionsType = TypeIdOf<UnrealExchangeImportOptions>();
}

Result<void> UnrealSkeletalMeshImporter::Import(AssetImportContext& context) noexcept {
    Allocator& allocator = context.Registry().ObjectAllocator();

    // 1–3. 문서·클래스·옵션 objectPath.
    ExchangeAsset asset(allocator);
    const UnrealExchangeImportOptions* options = nullptr;
    if (auto read = exchange_detail::ReadImportDocument(context, ExchangeClass::SkeletalMesh, asset,
                                                        options);
        !read) {
        return read;
    }

    // 4·5. blob 원본.
    Array<Span<const uint8>> blobs(allocator);
    if (auto collected = exchange_detail::CollectImportBlobs(context, asset, blobs); !collected) {
        return collected;
    }

    // 명세(6.4절 2–6번 — 원본 모드는 늘 순수 원본이라 meshNormals를 쓰지 않는다).
    ExchangeSkeletalMeshInput input;
    input.asset = &asset;
    input.blobs = blobs.View();
    input.references = &options->references;
    input.registry = &context.Registry();
    input.access = context.Access();
    ExchangeSkeletalMesh mesh(allocator);
    ExchangeMeshFailure failure;
    if (auto built = mesh.Build(input, failure); !built) {
        return failure.code == AssetDiagnosticCode::None
                   ? Result<void>::Failure(built.Error())
                   : context.Fail(failure.code, built.Error(), failure.detail);
    }

    for (const ExchangeSettingWarning& warning : mesh.Warnings()) {
        if (auto warned = context.Warn(warning.code, warning.detail); !warned) {
            return warned;
        }
    }

    auto output = context.Output({}, info_.outputType);
    if (!output) {
        return Result<void>::Failure(output.Error());
    }

    if (auto imported = ImportSkeletalMesh(context, output.Value(), mesh.Spec()); !imported) {
        return imported;
    }

    return exchange_detail::AddExchangeTags(context, *options);
}

} // namespace rocks
