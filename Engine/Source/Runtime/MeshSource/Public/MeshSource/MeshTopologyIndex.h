#pragma once

#include <MeshSource/MeshSource.h>

namespace rocks {

// 검증을 통과한 원본 메시의 이웃 조회 표다(Docs/Design/MeshAnimationAssets.ko.md 3.7절 11, UE
// FMeshDescription의 Get*Connected*·GetVertexPairEdge 대응). 빌드(법선·tangent·용접·hard edge)가
// 쓰는 기본 조회를 한 번 만든 CSR 표로 준다. 표는 Build 때의 mesh 내용을 담으므로 mesh를 고치면
// 다시 만든다. 조회 결과의 번호 순서는 오름차순이다(결정적). FindEdge·PolygonSideEdge는 mesh의
// 위상을 직접 읽으므로 mesh가 이 표보다 오래 살고 바뀌지 않아야 한다.
//
// 사용 예:
//   MeshTopologyIndex index(allocator);
//   if (auto built = index.Build(mesh); !built) { return built; }
//   for (uint32 triangle : index.PolygonTriangles(polygon)) { ... }
//   const uint32 edge = index.FindEdge(a, b);    // 없으면 MeshNoIndex
//
// 계약: Build의 mesh는 ValidateMeshSource를 통과한 것이어야 한다(통과하지 않은 메시는 범위 단언으로
// 멈출 수 있다). 조회 인자는 그 요소 수 미만이어야 한다(ROCKS_ASSERT).
// 수명: 반환 span은 다음 Build·파괴까지 유효하다. 스레드: Build 뒤 조회는 여러 스레드가 함께 해도
// 된다(읽기만 한다).
class MeshTopologyIndex {
public:
    explicit MeshTopologyIndex(Allocator& allocator) noexcept;

    MeshTopologyIndex(MeshTopologyIndex&&) noexcept = default;
    MeshTopologyIndex& operator=(MeshTopologyIndex&&) noexcept = default;
    MeshTopologyIndex(const MeshTopologyIndex&) = delete;
    MeshTopologyIndex& operator=(const MeshTopologyIndex&) = delete;

    // mesh의 표를 만든다. 오류: 할당 실패는 allocator 오류다(실패하면 이전 표가 그대로다).
    Result<void> Build(const MeshSource& mesh) noexcept;

    // 다각형 polygon의 삼각형이다.
    Span<const uint32> PolygonTriangles(uint32 polygon) const noexcept;
    // 다각형 묶음 group의 다각형이다.
    Span<const uint32> GroupPolygons(uint32 group) const noexcept;
    // 정점 vertex를 가리키는 코너다.
    Span<const uint32> VertexCorners(uint32 vertex) const noexcept;
    // 정점 vertex에 닿는 변이다.
    Span<const uint32> VertexEdges(uint32 vertex) const noexcept;
    // 코너 corner를 쓰는 삼각형이다.
    Span<const uint32> CornerTriangles(uint32 corner) const noexcept;
    // 코너 corner가 둘레에 있는 다각형이다(한 다각형 둘레에 두 번 있으면 한 번만).
    Span<const uint32> CornerPolygons(uint32 corner) const noexcept;
    // 변 edge가 둘레에 있는 다각형이다(한 다각형 둘레에 두 번 있으면 한 번만).
    Span<const uint32> EdgePolygons(uint32 edge) const noexcept;

    // 정점 a·b를 잇는 변의 번호다(순서 무관). 없으면 MeshNoIndex다.
    uint32 FindEdge(uint32 vertexA, uint32 vertexB) const noexcept;

    // 다각형 polygon 둘레의 side번째 변(코너 side → side + 1)의 번호다. 두 코너가 같은 정점이면
    // MeshNoIndex다(퇴화 변 — 3.1절).
    uint32 PolygonSideEdge(uint32 polygon, uint32 side) const noexcept;

    // CSR 표 하나다(구현 세부).
    struct Table {
        explicit Table(Allocator& allocator) noexcept : starts(allocator), items(allocator) {
        }

        Array<uint32> starts;
        Array<uint32> items;

        Span<const uint32> Row(uint32 row) const noexcept;
    };

private:
    Allocator* allocator_;
    // Build에 넘긴 메시다(FindEdge·PolygonSideEdge가 위상을 읽는다).
    const MeshSource* mesh_ = nullptr;
    Table polygonTriangles_;
    Table groupPolygons_;
    Table vertexCorners_;
    Table vertexEdges_;
    Table cornerTriangles_;
    Table cornerPolygons_;
    Table edgePolygons_;
};

} // namespace rocks
