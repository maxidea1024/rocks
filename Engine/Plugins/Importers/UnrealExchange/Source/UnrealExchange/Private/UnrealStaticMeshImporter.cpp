#include "ExchangeImporterCommon.h"

#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <MeshTools/StaticMeshImport.h>
#include <UnrealExchange/ExchangeStaticMesh.h>
#include <UnrealExchange/UnrealExchangeFormat.h>
#include <UnrealExchange/UnrealExchangeImportOptions.generated.h>
#include <UnrealExchange/UnrealStaticMeshImporter.h>

// 교환 정적 메시 A2f 임포터다(Docs/Design/UnrealExchange 5.5절 10번, 10.6절 5–7번).

namespace rocks {

namespace unreal_static_mesh_import {

// 임포터 id·버전이다(5.5절 10번).
constexpr Guid ImporterId{0x880ace5284284e4dull, 0x82d0924e1c041e86ull};
constexpr uint32 ImporterVersion = 1;
constexpr StringView AssetRoles[] = {UnrealExchangeAssetRole};
constexpr StringView BlobRolePrefixes[] = {UnrealExchangeBlobRolePrefix};

} // namespace unreal_static_mesh_import

UnrealStaticMeshImporter::UnrealStaticMeshImporter() noexcept {
    using namespace unreal_static_mesh_import;
    info_.id = ImporterId;
    info_.name = UnrealStaticMeshImporterName;
    info_.version = ImporterVersion;
    info_.outputType = TypeIdOf<StaticMesh>();
    info_.roles = Span<const StringView>(AssetRoles);
    info_.rolePrefixes = Span<const StringView>(BlobRolePrefixes);
    info_.tagPrefix = UnrealExchangeTagPrefix;
    info_.optionsType = TypeIdOf<UnrealExchangeImportOptions>();
}

Result<void> UnrealStaticMeshImporter::Import(AssetImportContext& context) noexcept {
    Allocator& allocator = context.Registry().ObjectAllocator();

    // 1–3. 문서·클래스·옵션 objectPath.
    ExchangeAsset asset(allocator);
    const UnrealExchangeImportOptions* options = nullptr;
    if (auto read =
            exchange_detail::ReadImportDocument(context, ExchangeClass::StaticMesh, asset, options);
        !read) {
        return read;
    }

    // 4·5. blob 원본.
    Array<Span<const uint8>> blobs(allocator);
    if (auto collected = exchange_detail::CollectImportBlobs(context, asset, blobs); !collected) {
        return collected;
    }

    // 명세(5.5절 1–9번).
    ExchangeStaticMeshInput input;
    input.asset = &asset;
    input.blobs = blobs.View();
    input.normals = options->meshNormals;
    input.references = &options->references;
    input.registry = &context.Registry();
    input.access = context.Access();
    ExchangeStaticMesh mesh(allocator);
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

    if (auto imported = ImportStaticMesh(context, output.Value(), mesh.Spec()); !imported) {
        return imported;
    }

    return exchange_detail::AddExchangeTags(context, *options);
}

} // namespace rocks
