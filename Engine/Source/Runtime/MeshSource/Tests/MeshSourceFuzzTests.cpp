#include "MeshSourceTestSupport.h"

#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>

// 고정 seed 퍼징이다(Docs/Design/MeshAnimationAssets.ko.md 12절 "무작위 위상·속성 퍼징"): 무작위
// 메시는 검증이 오류(정해진 코드·세부 규칙)를 내거나 통과하고, 통과한 것은 위상 조회 전체를 돌아도
// 범위 단언에 걸리지 않으며 저장·로드 왕복이 같은 메시다. 저장 byte를 무작위로 바꾼 패키지는
// Load가 손상(CorruptPackage)으로 거부하거나, 받아들이면 검증을 통과한 메시다.
using namespace rocks;
namespace mesh_source_fuzz_tests_file {

using namespace rocks::mesh_source_test;

namespace {

constexpr uint64 FuzzSeed = 0x4d41314d45534821ull;
constexpr uint32 FuzzMeshCount = 600;
constexpr uint32 FuzzBulkCount = 160;

// 검증을 통과한 메시의 위상 조회를 모두 돌며 결과가 범위 안인지 센다(어긋난 수를 돌려준다). 범위
// 밖 조회는 표 안의 단언에 걸려 시험 프로세스가 멈추므로, 끝까지 오면 단언도 지킨 것이다.
uint32 WalkTopology(const MeshSource& mesh, const MeshTopologyIndex& index) noexcept {
    uint32 bad = 0;
    const uint32 vertices = mesh.Count(MeshElement::Vertex);
    const uint32 corners = mesh.Count(MeshElement::Corner);
    const uint32 edges = mesh.Count(MeshElement::Edge);
    const uint32 polygons = mesh.Count(MeshElement::Polygon);
    const uint32 triangles = mesh.Count(MeshElement::Triangle);
    auto within = [&](Span<const uint32> row, uint32 limit) {
        for (uint32 item : row) {
            bad += item < limit ? 0 : 1;
        }
    };
    for (uint32 polygon = 0; polygon < polygons; ++polygon) {
        within(index.PolygonTriangles(polygon), triangles);
        const uint32 sides = static_cast<uint32>(mesh.PolygonCorners(polygon).Size());
        for (uint32 side = 0; side < sides; ++side) {
            const uint32 edge = index.PolygonSideEdge(polygon, side);
            bad += edge == MeshNoIndex || edge < edges ? 0 : 1;
        }
    }

    for (uint32 group = 0; group < mesh.Count(MeshElement::PolygonGroup); ++group) {
        within(index.GroupPolygons(group), polygons);
    }

    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        within(index.VertexCorners(vertex), corners);
        within(index.VertexEdges(vertex), edges);
        for (uint32 other = 0; other < vertices; ++other) {
            const uint32 edge = index.FindEdge(vertex, other);
            bad += edge == MeshNoIndex || edge < edges ? 0 : 1;
        }
    }

    for (uint32 corner = 0; corner < corners; ++corner) {
        within(index.CornerTriangles(corner), triangles);
        within(index.CornerPolygons(corner), polygons);
    }

    for (uint32 edge = 0; edge < edges; ++edge) {
        within(index.EdgePolygons(edge), polygons);
    }

    return bad;
}

bool IsValidationCode(const MeshDiagnostic& diagnostic) noexcept {
    return (diagnostic.code == AssetDiagnosticCode::InvalidMeshData ||
            diagnostic.code == AssetDiagnosticCode::TooManyInfluences) &&
           diagnostic.issue != MeshIssue::None && diagnostic.error == ErrorCode::InvalidArgument;
}

} // namespace

// 무작위 메시: 검증은 정해진 오류이거나 통과이고, 통과한 것은 위상 조회 전체가 범위 안이며 일부는
// 패키지 왕복도 같은 메시다. 통과·거부가 둘 다 충분히 나와야 퍼징이 의미 있다.
TEST(MeshSource_FuzzRandomMeshes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    RandomStream random(FuzzSeed);
    uint32 accepted = 0;
    uint32 rejected = 0;
    uint32 roundTrips = 0;
    for (uint32 iteration = 0; iteration < FuzzMeshCount; ++iteration) {
        MeshSource mesh(env.allocator);
        REQUIRE(RandomFuzzMesh(random, mesh));
        REQUIRE(RandomFuzzAttributes(random, mesh));
        MeshDiagnostic diagnostic;
        if (!ValidateMeshSource(mesh, {}, &diagnostic)) {
            ++rejected;
            CHECK(IsValidationCode(diagnostic));
            continue;
        }

        ++accepted;
        MeshTopologyIndex index(env.allocator);
        REQUIRE(index.Build(mesh));
        CHECK(WalkTopology(mesh, index) == 0);
        if (iteration % 4 == 0) {
            MeshSource loaded(env.allocator);
            REQUIRE(RoundTripMesh(env, 10000 + iteration, PackageKind::Editor, mesh, loaded));
            CHECK(SameMesh(mesh, loaded));
            ++roundTrips;
        }
    }

    CHECK(accepted >= FuzzMeshCount / 8);
    CHECK(rejected >= FuzzMeshCount / 8);
    CHECK(roundTrips != 0);
}

// 저장 byte 퍼징: 리치 메시의 위상·속성 bulk 하나를 골라 byte를 바꾸거나 자르거나 늘린 패키지를
// 로드한다. Load는 CorruptPackage(배치·내용)로 거부하거나, 받아들이면 그 메시는 검증을 통과하고
// 위상 조회가 범위 안이다.
TEST(MeshSource_FuzzStoredBytes) {
    asset_test::AssetEnv env;
    REQUIRE(env.Ready());
    MeshSource mesh(env.allocator);
    REQUIRE(BuildRichMesh(mesh));
    RandomStream random(FuzzSeed + 1);
    uint32 accepted = 0;
    uint32 rejected = 0;
    for (uint32 iteration = 0; iteration < FuzzBulkCount; ++iteration) {
        MeshSource out(env.allocator);
        MeshDiagnostic diagnostic;
        auto loaded = LoadEdited(
            env, 20000 + iteration, mesh,
            [&](MeshSourceData& data, MemoryBulkSource& bulk) {
                const uint32 target =
                    random.NextUInt32Below(static_cast<uint32>(data.attributes.Size()) + 1);
                const BulkDataRef ref =
                    target == data.attributes.Size() ? data.topology : data.attributes[target].data;
                Array<uint8> bytes = BulkCopy(env.allocator, bulk, ref);
                const uint32 mode = random.NextUInt32Below(4);
                if (mode == 0 && !bytes.IsEmpty()) {
                    (void)bytes.TryResize(
                        random.NextUInt32Below(static_cast<uint32>(bytes.Size())));
                } else if (mode == 1) {
                    if (!bytes.TryPushBack(static_cast<uint8>(random.NextUInt32()))) {
                        return false;
                    }
                } else if (!bytes.IsEmpty()) {
                    const uint32 flips = 1 + random.NextUInt32Below(3);
                    for (uint32 flip = 0; flip < flips; ++flip) {
                        bytes[random.NextUInt32Below(static_cast<uint32>(bytes.Size()))] ^=
                            static_cast<uint8>(1u << random.NextUInt32Below(8));
                    }
                }

                auto replaced = bulk.Add(Move(bytes));
                if (!replaced) {
                    return false;
                }

                if (target == data.attributes.Size()) {
                    data.topology = replaced.Value();
                } else {
                    data.attributes[target].data = replaced.Value();
                }

                return true;
            },
            out, diagnostic);
        REQUIRE(loaded || loaded.Error() != ErrorCode::NotFound);
        if (loaded) {
            ++accepted;
            CHECK(ValidateMeshSource(out).IsOk());
            MeshTopologyIndex index(env.allocator);
            REQUIRE(index.Build(out));
            CHECK(WalkTopology(out, index) == 0);
        } else {
            ++rejected;
            CHECK(diagnostic.code == AssetDiagnosticCode::CorruptPackage);
            CHECK(diagnostic.issue != MeshIssue::None);
        }
    }

    CHECK(rejected >= FuzzBulkCount / 4);
    CHECK(accepted != 0);
}

} // namespace mesh_source_fuzz_tests_file
