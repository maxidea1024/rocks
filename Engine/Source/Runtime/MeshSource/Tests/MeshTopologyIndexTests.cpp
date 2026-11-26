#include "MeshSourceTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>

// 위상 조회 시험이다(Docs/Design/MeshAnimationAssets 3.7절 11): 다각형 → 삼각형, 묶음 →
// 다각형, 정점 → 코너·변, 코너 → 삼각형·다각형, 변 → 다각형, 변 찾기, 둘레 변(퇴화 변 건너뜀), 할당
// 실패 시 이전 표 유지.
using namespace rocks;
namespace mesh_topology_index_tests_file {

using namespace rocks::mesh_source_test;

namespace {

bool RowIs(Span<const uint32> row, Span<const uint32> expected) noexcept {
    return SameSpan(row, expected);
}

} // namespace

// 공유 메시(BuildSharedMesh)의 조회 결과를 손으로 센 값과 대조한다.
TEST(MeshSource_TopologyQueries) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildSharedMesh(mesh));
    REQUIRE(ValidateMeshSource(mesh));
    MeshTopologyIndex index(allocator);
    REQUIRE(index.Build(mesh));
    const uint32 p0[] = {0, 1};
    const uint32 p2[] = {4};
    const uint32 p3[] = {5};
    CHECK(RowIs(index.PolygonTriangles(0), p0));
    CHECK(RowIs(index.PolygonTriangles(2), p2));
    CHECK(RowIs(index.PolygonTriangles(3), p3));
    const uint32 groupA[] = {0, 1};
    const uint32 groupB[] = {2, 3};
    CHECK(RowIs(index.GroupPolygons(0), groupA));
    CHECK(RowIs(index.GroupPolygons(1), groupB));
    // 정점 0은 코너 0과 6(퇴화 삼각형용)이 가리킨다.
    const uint32 vertex0Corners[] = {0, 6};
    CHECK(RowIs(index.VertexCorners(0), vertex0Corners));
    // 변: 0(0,1) 1(1,2) 2(2,3) 3(0,3) 4(1,4) 5(4,5) 6(2,5).
    const uint32 vertex1Edges[] = {0, 1, 4};
    const uint32 vertex2Edges[] = {1, 2, 6};
    CHECK(RowIs(index.VertexEdges(1), vertex1Edges));
    CHECK(RowIs(index.VertexEdges(2), vertex2Edges));
    // 코너 1은 삼각형 0, 2, 3, 4, 5(같은 코너를 두 번 쓰는 삼각형 5는 한 번만).
    const uint32 corner1Triangles[] = {0, 2, 3, 4, 5};
    CHECK(RowIs(index.CornerTriangles(1), corner1Triangles));
    const uint32 corner1Polygons[] = {0, 1, 2, 3};
    CHECK(RowIs(index.CornerPolygons(1), corner1Polygons));
    const uint32 corner6Polygons[] = {2};
    CHECK(RowIs(index.CornerPolygons(6), corner6Polygons));
    // 변 (0,1)은 다각형 0과 퇴화 다각형 2(둘레 0→1, 1→0 두 번이지만 한 번), 변 (1,2)는 0·1 공유.
    const uint32 edge0Polygons[] = {0, 2};
    const uint32 edge1Polygons[] = {0, 1};
    const uint32 edge4Polygons[] = {1, 3};
    CHECK(RowIs(index.EdgePolygons(0), edge0Polygons));
    CHECK(RowIs(index.EdgePolygons(1), edge1Polygons));
    CHECK(RowIs(index.EdgePolygons(4), edge4Polygons));
    CHECK(index.FindEdge(2, 1) == 1 && index.FindEdge(1, 2) == 1);
    CHECK(index.FindEdge(5, 4) == 5);
    CHECK(index.FindEdge(0, 4) == MeshNoIndex);
    CHECK(index.FindEdge(3, 3) == MeshNoIndex);
    // 퇴화 다각형 2의 둘레: 코너 0→6은 같은 정점(퇴화 변), 6→1과 1→0은 변 0.
    CHECK(index.PolygonSideEdge(2, 0) == MeshNoIndex);
    CHECK(index.PolygonSideEdge(2, 1) == 0 && index.PolygonSideEdge(2, 2) == 0);
    // 다각형 1 둘레 {1, 4, 5, 2}: 변 2는 5→2(변 6), 변 3은 2→1(변 1, 다각형 0과 공유).
    CHECK(index.PolygonSideEdge(1, 2) == 6 && index.PolygonSideEdge(1, 3) == 1);
}

// 같은 코너를 두 번 쓰는 삼각형은 어느 자리에 겹쳐도(0·2, 1·2) 코너 행에 한 번만 든다.
TEST(MeshSource_TopologyRepeatedCorners) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildSharedMesh(mesh));
    const uint32 repeat[] = {1, 1, 4};
    auto first = mesh.AddPolygon(repeat, 1);
    auto second = mesh.AddPolygon(repeat, 1);
    REQUIRE(first && second);
    REQUIRE(mesh.AddTriangle(1, 4, 1, first.Value()));
    REQUIRE(mesh.AddTriangle(4, 1, 1, second.Value()));
    REQUIRE(ValidateMeshSource(mesh));
    MeshTopologyIndex index(allocator);
    REQUIRE(index.Build(mesh));
    const uint32 corner1[] = {0, 2, 3, 4, 5, 6, 7};
    const uint32 corner4[] = {2, 5, 6, 7};
    CHECK(RowIs(index.CornerTriangles(1), corner1));
    CHECK(RowIs(index.CornerTriangles(4), corner4));
    const uint32 group1[] = {2, 3, 4, 5};
    CHECK(RowIs(index.GroupPolygons(1), group1));
}

// 빈 메시와 위상 없는 정점만 있는 메시도 표를 만든다(행이 비어 있다).
TEST(MeshSource_TopologyEmpty) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    MeshTopologyIndex index(allocator);
    REQUIRE(index.Build(mesh));
    REQUIRE(mesh.AddVertex(Vec3()));
    REQUIRE(mesh.AddVertex(Vec3(1, 0, 0)));
    REQUIRE(index.Build(mesh));
    CHECK(index.VertexCorners(1).IsEmpty() && index.VertexEdges(0).IsEmpty());
    CHECK(index.FindEdge(0, 1) == MeshNoIndex);
}

// Build의 모든 할당 지점에서 실패하면 그 오류이고 이전 표가 그대로다.
TEST(MeshSource_TopologyBuildFailures) {
    SystemAllocator allocator;
    MeshSource quad(allocator);
    REQUIRE(BuildQuad(quad));
    MeshSource shared(allocator);
    REQUIRE(BuildSharedMesh(shared));
    // 대각선 변을 더해 변 수(8)가 코너 수(7)보다 많게 한다(변 표식 배열이 다시 할당된다).
    REQUIRE(shared.AddEdge(0, 2));
    bool done = false;
    uint64 failures = 0;
    for (uint64 attempt = 1; attempt <= 128 && !done; ++attempt) {
        test::NFailAllocator failing(allocator);
        MeshTopologyIndex index(failing);
        REQUIRE(index.Build(quad));
        failing.FailOnAttempt(attempt);
        auto built = index.Build(shared);
        failing.DisableFailure();
        if (built) {
            done = true;
            CHECK(index.GroupPolygons(1).Size() == 2);
        } else {
            ++failures;
            CHECK(built.Error() == ErrorCode::OutOfMemory);
            // 이전 표(사각형)가 그대로다.
            CHECK(index.PolygonTriangles(0).Size() == 2);
            CHECK(index.FindEdge(0, 3) == 3);
        }
    }

    CHECK(done);
    CHECK(failures >= 14);
}

} // namespace mesh_topology_index_tests_file
