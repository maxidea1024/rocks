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

// 확률 percent%로 참이다.
bool Chance(RandomStream& random, uint32 percent) noexcept {
    return random.NextUInt32Below(100) < percent;
}

float RandomCoordinate(RandomStream& random) noexcept {
    if (Chance(random, 2)) {
        return TestQuietNaN();
    }

    return random.NextFloatInRange(-500.0f, 500.0f);
}

// 대부분 정상인 메시를 만들고 확률로 불변식 하나를 어긴다(위상은 다각형마다 새 코너 + 둘레 변 +
// 부채꼴 삼각 분할, 코너는 가끔 이웃 다각형의 것을 공유한다).
bool RandomMesh(RandomStream& random, MeshSource& mesh) noexcept {
    const uint32 vertices = random.NextUInt32Below(10);
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        if (!mesh.AddVertex(Vec3(RandomCoordinate(random), RandomCoordinate(random),
                                 RandomCoordinate(random)))) {
            return false;
        }
    }

    const uint32 groups = 1 + random.NextUInt32Below(3);
    for (uint32 group = 0; group < groups; ++group) {
        if (!mesh.AddPolygonGroup()) {
            return false;
        }
    }

    const uint32 polygons = vertices >= 3 ? random.NextUInt32Below(5) : 0;
    for (uint32 polygon = 0; polygon < polygons; ++polygon) {
        uint32 sides = 3 + random.NextUInt32Below(vertices - 2);
        if (Chance(random, 3)) {
            sides = random.NextUInt32Below(3);
        }

        uint32 corners[16] = {};
        sides = sides > 16 ? 16 : sides;
        for (uint32 side = 0; side < sides; ++side) {
            const bool share = mesh.Count(MeshElement::Corner) != 0 && Chance(random, 15);
            if (share) {
                corners[side] = random.NextUInt32Below(mesh.Count(MeshElement::Corner));
            } else {
                auto corner = mesh.AddCorner(random.NextUInt32Below(vertices));
                if (!corner) {
                    return false;
                }

                corners[side] = corner.Value();
            }
        }

        const uint32 group =
            Chance(random, 3) ? groups + random.NextUInt32Below(2) : random.NextUInt32Below(groups);
        auto added = mesh.AddPolygon(Span<const uint32>(corners, sides), group);
        if (!added) {
            return false;
        }

        // 둘레 변: 없는 것만 더한다(같은 정점 쌍은 건너뜀). 가끔 하나를 빠뜨린다.
        for (uint32 side = 0; side < sides; ++side) {
            const uint32 a = mesh.CornerVertices()[corners[side]];
            const uint32 b = mesh.CornerVertices()[corners[(side + 1) % sides]];
            bool known = a == b;
            for (const MeshEdgeVertices& edge : mesh.Edges()) {
                known = known || (edge.first == (a < b ? a : b) && edge.second == (a < b ? b : a));
            }

            if (!known && !Chance(random, 2) && !mesh.AddEdge(a, b)) {
                return false;
            }
        }

        // 부채꼴 삼각 분할(가끔 하나를 빼거나 더한다).
        for (uint32 fan = 1; sides >= 3 && fan + 1 < sides; ++fan) {
            if (!Chance(random, 2) &&
                !mesh.AddTriangle(corners[0], corners[fan], corners[fan + 1], added.Value())) {
                return false;
            }
        }

        if (Chance(random, 2) && sides != 0 &&
            !mesh.AddTriangle(corners[0], corners[sides - 1], corners[0], added.Value())) {
            return false;
        }
    }

    // 잡음: 범위 밖 코너·변·삼각형, 같은 정점 변.
    if (Chance(random, 3) && !mesh.AddCorner(vertices + random.NextUInt32Below(3))) {
        return false;
    }

    if (Chance(random, 3) &&
        !mesh.AddEdge(random.NextUInt32Below(vertices + 2), random.NextUInt32Below(vertices + 2))) {
        return false;
    }

    if (Chance(random, 2) &&
        !mesh.AddTriangle(random.NextUInt32Below(4), 0, 1, random.NextUInt32Below(polygons + 1))) {
        return false;
    }

    return true;
}

// 무작위 속성을 더한다(엔진 이름·사용자 이름, 정상 값과 가끔 잘못된 값).
bool RandomAttributes(RandomStream& random, MeshSource& mesh) noexcept {
    struct Template {
        MeshElement element;
        StringView name;
        MeshValueType type;
        uint8 channels;
    };

    const Template templates[] = {
        {MeshElement::Corner, "Normal", MeshValueType::Vec3, 1},
        {MeshElement::Corner, "TexCoord", MeshValueType::Vec2, 2},
        {MeshElement::Corner, "Color", MeshValueType::Vec4, 1},
        {MeshElement::Edge, "IsHard", MeshValueType::Bool, 1},
        {MeshElement::Polygon, "PartName", MeshValueType::Name, 1},
        {MeshElement::PolygonGroup, "MaterialSlotName", MeshValueType::Name, 1},
        {MeshElement::Vertex, "SkinWeights", MeshValueType::BoneInfluences, 1},
        {MeshElement::Vertex, "Morph-Fuzz", MeshValueType::SparseVec3, 1},
        {MeshElement::Corner, "MorphNormal-Fuzz", MeshValueType::SparseVec3, 1},
        {MeshElement::Triangle, "Ids", MeshValueType::UInt32, 2},
        {MeshElement::Corner, "Normal", MeshValueType::Vec2, 1},
        {MeshElement::Vertex, "Morph-", MeshValueType::SparseVec3, 1},
    };
    const uint32 count = random.NextUInt32Below(5);
    for (uint32 pick = 0; pick < count; ++pick) {
        const Template& entry = templates[random.NextUInt32Below(12)];
        auto added = mesh.AddAttribute(entry.element, entry.name, entry.type, entry.channels);
        if (!added) {
            continue;
        }

        const uint32 attribute = added.Value();
        const uint32 elements = mesh.Count(entry.element);
        for (uint32 index = 0; index < elements; ++index) {
            switch (entry.type) {
            case MeshValueType::Bool:
                mesh.BoolValues(attribute, 0)[index] =
                    static_cast<uint8>(random.NextUInt32Below(Chance(random, 3) ? 4 : 2));
                break;
            case MeshValueType::Name: {
                const StringView names[] = {"", "Body", "e\xCC\x81", "\xff", "Visor_2"};
                if (!mesh.SetNameValue(attribute, 0, index,
                                       names[Chance(random, 4) ? random.NextUInt32Below(5)
                                                               : random.NextUInt32Below(2)])) {
                    return false;
                }

                break;
            }
            case MeshValueType::BoneInfluences: {
                BoneInfluence influences[MeshMaxSourceInfluences + 1];
                const uint32 used = random.NextUInt32Below(Chance(random, 3) ? 14 : 5);
                for (uint32 slot = 0; slot < used; ++slot) {
                    influences[slot].boneIndex = static_cast<uint16>(
                        Chance(random, 5) ? random.NextUInt32Below(4) : slot * 3);
                    influences[slot].weight = static_cast<uint16>(random.NextUInt32Below(30000));
                }

                if (!mesh.SetInfluences(attribute, index,
                                        Span<const BoneInfluence>(influences, used))) {
                    return false;
                }

                break;
            }
            case MeshValueType::SparseVec3:
                if (Chance(random, 30) &&
                    !mesh.SetSparseValue(attribute, index,
                                         Vec3(Chance(random, 30) ? -0.0f : 1.0f, 0.0f, 0.0f))) {
                    return false;
                }

                break;
            case MeshValueType::UInt32:
                mesh.UInt32Values(attribute, 1)[index] = random.NextUInt32();
                break;
            default:
                for (float& value : mesh.FloatValues(attribute, 0)) {
                    value = random.NextFloatInRange(-2.0f, 2.0f);
                }

                break;
            }
        }
    }

    return true;
}

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
        REQUIRE(RandomMesh(random, mesh));
        REQUIRE(RandomAttributes(random, mesh));
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
