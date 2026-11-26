#include <MeshTools/StaticMeshPacking.h>

#include "MeshLodPacking.h"

// 정적 메시 쿠킹 LOD 패킹이다(Docs/Design/MeshAnimationAssets 5.5.2절 1). 단계는 스킨 패킹과 함께
// 쓰는 MeshLodPacking.h다.
namespace rocks {

Result<void> PackStaticMeshLod(const StaticMeshBuildResult& built,
                               const MeshBuildSettings& settings,
                               Span<const MeshSectionInfo> sections, MeshColorFormat colorFormat,
                               Array<uint8>& bytes, StaticMeshRenderLod& lod) noexcept {
    if (built.indices.Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    bytes.Clear();
    mesh_packing::PackWriter writer(bytes);
    lod.sections.Clear();
    if (auto streams = mesh_packing::PackVertexStreams(built, settings, colorFormat, writer, lod);
        !streams) {
        return streams;
    }

    return mesh_packing::PackIndicesAndSections(built, sections, writer, lod);
}

} // namespace rocks
