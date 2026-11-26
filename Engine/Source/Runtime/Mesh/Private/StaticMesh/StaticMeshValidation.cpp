#include <Mesh/StaticMesh/StaticMeshValidation.h>

#include "../Common/MeshRenderLodChecks.h"

// 정적 메시 로드 검증과 확장 bounds다(Docs/Design/MeshAnimationAssets 5.5.1절 9). LOD 검사는 스킨
// 메시와 함께 쓰는 MeshRenderLodChecks.h다.
namespace rocks {

Result<void> ValidateStaticMeshRenderData(const StaticMeshRenderData& renderData,
                                          MeshDiagnostic* failure) noexcept {
    if (renderData.lods.Size() > StaticMeshMaxLods) {
        return mesh_render_checks::Corrupt(failure, MeshIssue::RenderLodCount, MeshNoIndex);
    }

    if (auto bounds =
            mesh_render_checks::ValidateBounds(renderData.box, renderData.sphereRadius, failure);
        !bounds) {
        return bounds;
    }

    for (usize lodIndex = 0; lodIndex < renderData.lods.Size(); ++lodIndex) {
        const StaticMeshRenderLod& lod = renderData.lods[lodIndex];
        const auto index = static_cast<uint32>(lodIndex);
        const bool streams = !lod.data.IsEmpty();
        Result<void> streamed =
            streams ? mesh_render_checks::ValidateStreams(lod, index, failure,
                                                          mesh_render_checks::NoExtraStreams{})
                    : mesh_render_checks::ValidateStreamlessLod(lod, index, failure);
        if (!streamed) {
            return streamed;
        }

        if (auto sections = mesh_render_checks::ValidateSections(lod, index, streams, failure);
            !sections) {
            return sections;
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateStaticMeshIndices(const StaticMeshRenderLod& lod, uint32 lodIndex,
                                       Span<const uint8> data, MeshDiagnostic* failure) noexcept {
    return mesh_render_checks::ValidateIndexValues(lod, lodIndex, data, failure);
}

Result<void> ValidateStaticMesh(const StaticMesh& mesh, const Package& package, Allocator& scratch,
                                MeshDiagnostic* meshFailure,
                                CollisionDiagnostic* collisionFailure) noexcept {
    if (auto collision = ValidateSimpleCollision(mesh.simpleCollision, collisionFailure);
        !collision) {
        return collision;
    }

    if (auto bulk =
            ValidateSimpleCollisionBulk(mesh.simpleCollision, package, scratch, collisionFailure);
        !bulk) {
        return bulk;
    }

    return ValidateStaticMeshRenderData(mesh.renderData, meshFailure);
}

MeshBounds StaticMeshExtendedBounds(const StaticMesh& mesh) noexcept {
    return mesh_render_checks::ExtendBounds(mesh.renderData.box, mesh.renderData.sphereRadius,
                                            mesh.positiveBoundsExtension,
                                            mesh.negativeBoundsExtension);
}

} // namespace rocks
