#include "MeshToolsTestSupport.h"

#include <MeshSource/Tests/MeshSourceTestSupport.h>

// 정적 메시 빌드의 기하 시험이다(Docs/Design/MeshAnimationAssets 5.2절 3·5·8·12단계, 12절
// "빌드"): 기본 사각형의 정점·index·기저 부호(UE 관례), 0 벡터 코너만 재계산, 가중 법선, hard edge,
// n-gon 대각선, 빈 메시, buildScale(음수 성분·비균일 역전치), bounds와 확장, UV 없는 tangent.
using namespace rocks;
namespace static_mesh_builder_tests_file {

using namespace rocks::mesh_tools_test;

constexpr float DirectionTolerance = 1.0e-5f;

// 사각형(코너 공유 넷, 삼각형 둘)과 평면 UV(x/100, y/100).
bool BuildUvQuad(MeshSource& mesh) noexcept {
    return mesh_source_test::BuildQuad(mesh) && PlanarUvs(mesh, 100.0f) != MeshNoAttribute;
}

// 원본 정점 vertex 위치(크기 적용)에 있는 렌더 정점 번호다(없으면 MeshNoIndex).
uint32 RenderVertexAt(const StaticMeshBuildResult& built, const Vec3& position) noexcept {
    for (uint32 vertex = 0; vertex < built.VertexCount(); ++vertex) {
        if (built.positions[vertex] == position) {
            return vertex;
        }
    }

    return MeshNoIndex;
}

Vec3 Unit(double x, double y, double z) noexcept {
    const double length = math::Sqrt(x * x + y * y + z * z);
    return Vec3(static_cast<float>(x / length), static_cast<float>(y / length),
                static_cast<float>(z / length));
}

// 기본 설정(재계산·MikkTSpace)의 사각형: 정점 넷, index 원본 순서, 법선은 UE 식(Cross(P2 − P0, P1 −
// P0))대로 −Z, tangent +X, 쌍법선 부호 −1(쌍법선 = cross(N, T) × 부호 = +Y = dP/dv — MikkTSpace
// 부호를 뒤집는 UE 관례), 섹션 하나·슬롯 대체 경고 하나.
TEST(StaticMeshBuild_QuadBasics) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildUvQuad(mesh));
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    REQUIRE(built.VertexCount() == 4);
    const uint32 expected[] = {0, 1, 2, 0, 2, 3};
    REQUIRE(built.indices.Size() == 6);
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(built.indices[index] == expected[index]);
    }

    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        CHECK(NearVector(built.normals[vertex], Vec3(0.0f, 0.0f, -1.0f), DirectionTolerance));
        CHECK(NearVector(built.tangents[vertex], Vec3(1.0f, 0.0f, 0.0f), DirectionTolerance));
        CHECK(built.binormalSigns[vertex] == -1.0f);
    }

    CHECK(built.texCoordChannels == 1 && built.texCoords.Size() == 4);
    CHECK(built.texCoords[2].x == 1.0f && built.texCoords[2].y == 1.0f);
    CHECK(built.colors.IsEmpty());
    REQUIRE(built.sections.Size() == 1);
    CHECK(built.sections[0].firstIndex == 0 && built.sections[0].triangleCount == 2);
    CHECK(built.sections[0].minVertex == 0 && built.sections[0].maxVertex == 3);
    CHECK(built.sections[0].materialIndex == 0 && built.sections[0].castShadow);
    REQUIRE(built.warnings.Size() == 1);
    CHECK(built.warnings[0].code == AssetDiagnosticCode::MaterialSlotFallback);
    CHECK(built.warnings[0].index == 0 && built.warnings[0].element == MeshElement::PolygonGroup);
    CHECK(built.bounds.boxMin == Vec3d(0.0, 0.0, 0.0));
    CHECK(built.bounds.boxMax == Vec3d(100.0, 100.0, 0.0));
    // 구 중심 = 상자 중심(50, 50, 0), 반지름 = 꼭짓점까지 거리 50√2.
    CHECK(math::Abs(built.bounds.sphereRadius - 70.71067811865476) < 1e-9);
}

// UV 평균 경로(MikkTSpace 끔)도 같은 기저와 부호를 낸다(UE 비 MikkTSpace 경로의 행렬식 부호).
TEST(StaticMeshBuild_AveragedTangentsMatchConvention) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildUvQuad(mesh));
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.useMikkTSpace = false;
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(input, built));
    REQUIRE(built.VertexCount() == 4);
    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        CHECK(NearVector(built.tangents[vertex], Vec3(1.0f, 0.0f, 0.0f), DirectionTolerance));
        CHECK(built.binormalSigns[vertex] == -1.0f);
    }
}

// UV가 없으면 MikkTSpace를 요청해도 법선 하나로 만든 Duff·Frisvad 기저다(UE WithNoUVs 경로): 법선
// −Z면 tangent +X, 기저 행렬식 +1.
TEST(StaticMeshBuild_TangentsWithoutUvs) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    REQUIRE(built.VertexCount() == 4);
    CHECK(built.texCoordChannels == 0 && built.texCoords.IsEmpty());
    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        CHECK(NearVector(built.normals[vertex], Vec3(0.0f, 0.0f, -1.0f), DirectionTolerance));
        CHECK(NearVector(built.tangents[vertex], Vec3(1.0f, 0.0f, 0.0f), DirectionTolerance));
        CHECK(built.binormalSigns[vertex] == 1.0f);
    }
}

// 재계산 플래그를 끄면 유효한 법선·tangent를 가진 코너는 bit 그대로이고, 0 벡터(값 정리가 NaN을
// 0으로 바꾼 것 포함) 코너만 계산된다(5.2절 5단계, UE ComputeTangentsAndNormals). 플래그를 켜면
// 모두 계산한다.
TEST(StaticMeshBuild_RecomputesOnlyZeroCorners) {
    SystemAllocator allocator;
    for (uint32 variant = 0; variant < 3; ++variant) {
        MeshSource mesh(allocator);
        REQUIRE(BuildUvQuad(mesh));
        const uint32 normal = CornerAttribute(mesh, MeshAttributeName::Normal, MeshValueType::Vec3);
        const uint32 tangent =
            CornerAttribute(mesh, MeshAttributeName::Tangent, MeshValueType::Vec3);
        const uint32 sign =
            CornerAttribute(mesh, MeshAttributeName::BinormalSign, MeshValueType::Float);
        // 원본 법선은 계산값(−Z)과 반대인 +Z로 두어 유지 여부가 드러나게 한다.
        for (uint32 corner = 0; corner < 4; ++corner) {
            mesh.SetVec3(normal, 0, corner, Vec3(0.0f, 0.0f, 1.0f));
            mesh.SetVec3(tangent, 0, corner, Vec3(1.0f, 0.0f, 0.0f));
            mesh.SetFloat(sign, 0, corner, 1.0f);
        }

        const float broken = variant == 1 ? mesh_source_test::TestQuietNaN() : 0.0f;
        mesh.SetVec3(normal, 0, 3, Vec3(broken, 0.0f, 0.0f));
        StaticMeshBuildInput input = InputFor(mesh);
        input.settings.recomputeNormals = variant == 2;
        input.settings.recomputeTangents = variant == 2;
        StaticMeshBuildResult built(allocator);
        REQUIRE(BuildStaticMeshLod(input, built));
        REQUIRE(built.VertexCount() == 4);
        for (uint32 vertex = 0; vertex < 3; ++vertex) {
            // 유지된 코너는 bit 그대로(허용 오차 0)다.
            const Vec3 expected = variant == 2 ? Vec3(0.0f, 0.0f, -1.0f) : Vec3(0.0f, 0.0f, 1.0f);
            CHECK(NearVector(built.normals[vertex], expected,
                             variant == 2 ? DirectionTolerance : 0.0f));
        }

        CHECK(NearVector(built.normals[3], Vec3(0.0f, 0.0f, -1.0f), DirectionTolerance));
        CHECK(NearVector(built.tangents[3], Vec3(1.0f, 0.0f, 0.0f), DirectionTolerance));
    }
}

// 가중 법선: 정점 V를 쓰는 두 삼각형(면적 5000·500, V의 각 모두 90°, 법선 −Z·−X)의 평균은 가중하면
// 면적 × 각에 비례하고(−0.0995, 0, −0.995), 끄면 단순 평균(−0.707, 0, −0.707)이다.
TEST(StaticMeshBuild_WeightedNormals) {
    SystemAllocator allocator;
    const Vec3 points[] = {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(0, 100, 0), Vec3(0, 0, 10)};
    const uint32 first[] = {0, 1, 2};
    const uint32 second[] = {0, 2, 3};
    const PolygonSpec polygons[] = {{first, 0}, {second, 0}};
    for (uint32 weighted = 0; weighted < 2; ++weighted) {
        MeshSource mesh(allocator);
        REQUIRE(BuildPolygonMesh(mesh, points, polygons, 1, true));
        StaticMeshBuildInput input = InputFor(mesh);
        input.settings.computeWeightedNormals = weighted != 0;
        StaticMeshBuildResult built(allocator);
        REQUIRE(BuildStaticMeshLod(input, built));
        const uint32 vertex = RenderVertexAt(built, points[0]);
        REQUIRE(vertex != MeshNoIndex);
        const Vec3 expected = weighted != 0 ? Unit(-500.0, 0.0, -5000.0) : Unit(-1.0, 0.0, -1.0);
        CHECK(NearVector(built.normals[vertex], expected, DirectionTolerance));
    }
}

// hard edge: 90° 접힌 두 사각형(공유 변 1–2, 다각형마다 코너)은 soft면 공유 정점의 두 면 코너가
// 같은 평균 법선을, hard면 각 면 법선을 갖는다(5.2절 5단계 — 법선이 갈린다). 평균은 UE와 같이
// 삼각형마다 더하므로 정점 1은 바닥 삼각형 하나(−Z)와 벽 삼각형 둘(−X)의 평균 (−2, 0, −1)/√5다.
TEST(StaticMeshBuild_HardEdgeSplitsNormals) {
    SystemAllocator allocator;
    const Vec3 points[] = {Vec3(0, 0, 0),   Vec3(100, 0, 0),    Vec3(100, 100, 0),
                           Vec3(0, 100, 0), Vec3(100, 0, -100), Vec3(100, 100, -100)};
    const uint32 floor[] = {0, 1, 2, 3};
    const uint32 wall[] = {1, 4, 5, 2};
    const PolygonSpec polygons[] = {{floor, 0}, {wall, 0}};
    for (uint32 hard = 0; hard < 2; ++hard) {
        MeshSource mesh(allocator);
        REQUIRE(BuildPolygonMesh(mesh, points, polygons, 1, false));
        auto isHard =
            mesh.AddAttribute(MeshElement::Edge, MeshAttributeName::IsHard, MeshValueType::Bool, 1);
        REQUIRE(isHard);
        for (uint32 edge = 0; edge < mesh.Count(MeshElement::Edge); ++edge) {
            const MeshEdgeVertices pair = mesh.Edges()[edge];
            if (pair.first == 1 && pair.second == 2) {
                mesh.BoolValues(isHard.Value(), 0)[edge] = static_cast<uint8>(hard);
            }
        }

        StaticMeshBuildResult built(allocator);
        REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
        REQUIRE(built.indices.Size() == 12);
        // 바닥 삼각형 0의 칸 1(정점 1)과 벽 삼각형 2의 칸 0(정점 1).
        const Vec3 floorNormal = built.normals[built.indices[1]];
        const Vec3 wallNormal = built.normals[built.indices[6]];
        if (hard != 0) {
            CHECK(NearVector(floorNormal, Vec3(0.0f, 0.0f, -1.0f), DirectionTolerance));
            CHECK(NearVector(wallNormal, Vec3(-1.0f, 0.0f, 0.0f), DirectionTolerance));
        } else {
            CHECK(NearVector(floorNormal, Unit(-2.0, 0.0, -1.0), DirectionTolerance));
            CHECK(NearVector(wallNormal, Unit(-2.0, 0.0, -1.0), DirectionTolerance));
        }

        // 바닥 안쪽 정점 0은 접힘과 무관하게 −Z다.
        CHECK(NearVector(built.normals[built.indices[0]], Vec3(0.0f, 0.0f, -1.0f),
                         DirectionTolerance));
    }
}

// n-gon과 코너 공유: 다각형 안 대각선(변 목록에 없음)은 soft라, 둘레 변이 모두 hard여도 부채꼴
// 삼각형이 정점에서 한 묶음으로 평균된다. 공유 코너는 렌더 정점 하나다. index는 부채꼴 순서다.
TEST(StaticMeshBuild_NgonDiagonalsAreSoft) {
    SystemAllocator allocator;
    // 오각형(0–4)을 정점 0–2 대각선에서 접는다: 삼각형 (0,1,2)는 z = 0, (0,2,3)·(0,3,4)는 기울어짐.
    const Vec3 points[] = {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(100, 100, 0), Vec3(50, 150, 50),
                           Vec3(0, 100, 50)};
    const uint32 pentagon[] = {0, 1, 2, 3, 4};
    const PolygonSpec polygons[] = {{pentagon, 0}};
    MeshSource mesh(allocator);
    REQUIRE(BuildPolygonMesh(mesh, points, polygons, 1, true));
    auto isHard =
        mesh.AddAttribute(MeshElement::Edge, MeshAttributeName::IsHard, MeshValueType::Bool, 1);
    REQUIRE(isHard);
    for (uint8& value : mesh.BoolValues(isHard.Value(), 0)) {
        value = 1;
    }

    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    CHECK(built.VertexCount() == 5);
    const uint32 expected[] = {0, 1, 2, 0, 2, 3, 0, 3, 4};
    REQUIRE(built.indices.Size() == 9);
    for (uint32 index = 0; index < 9; ++index) {
        CHECK(built.indices[index] == expected[index]);
    }

    // 정점 2는 삼각형 0(−Z)과 삼각형 1(기울어진 면)을 대각선 0–2로 이어 평균한다 — 어느 한 면
    // 법선이 아니다.
    const Vec3 normal = built.normals[2];
    CHECK(!NearVector(normal, Vec3(0.0f, 0.0f, -1.0f), 1.0e-3f));
    CHECK(normal.z < 0.0f);
    CHECK(Near(normal.Length(), 1.0f, DirectionTolerance));
}

// 빈 메시와 삼각형 없는 메시: 성공하고 렌더 정점·index가 없으며 bounds는 0이다. 묶음만 있으면 빈
// 섹션과 슬롯 대체 경고가 남는다.
TEST(StaticMeshBuild_EmptyMeshes) {
    SystemAllocator allocator;
    MeshSource empty(allocator);
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(empty), built));
    CHECK(built.VertexCount() == 0 && built.indices.IsEmpty() && built.sections.IsEmpty());
    CHECK(built.warnings.IsEmpty() && built.bounds.sphereRadius == 0.0);
    CHECK(built.bounds.boxMin == Vec3d(0.0, 0.0, 0.0) &&
          built.bounds.boxMax == Vec3d(0.0, 0.0, 0.0));

    MeshSource loose(allocator);
    REQUIRE(loose.AddVertex(Vec3(5, 6, 7)));
    REQUIRE(loose.AddPolygonGroup());
    REQUIRE(loose.AddPolygonGroup());
    REQUIRE(PlanarUvs(loose, 1.0f) != MeshNoAttribute);
    REQUIRE(BuildStaticMeshLod(InputFor(loose), built));
    CHECK(built.VertexCount() == 0 && built.indices.IsEmpty());
    REQUIRE(built.sections.Size() == 2);
    CHECK(built.sections[1].materialIndex == 1 && built.sections[1].triangleCount == 0);
    CHECK(built.sections[1].firstIndex == 0 && built.sections[1].maxVertex == 0);
    CHECK(CountWarnings(built, AssetDiagnosticCode::MaterialSlotFallback) == 2);
    CHECK(built.texCoordChannels == 1 && built.texCoords.IsEmpty());
}

// buildScale 음수 성분: winding(index 순서)은 그대로이고 위치는 곱해지며, 음수 성분이 홀수 개면
// NegativeBuildScale 경고(count = 음수 성분 수), 짝수 개면 경고가 없다. 법선은 크기로 나눠(역전치)
// Z 거울이면 +Z가 된다.
TEST(StaticMeshBuild_NegativeBuildScale) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildUvQuad(mesh));
    struct Case {
        Vec3d scale;
        uint32 warnings;
        uint64 count;
    };
    const Case cases[] = {{Vec3d(1.0, 1.0, -1.0), 1, 1},
                          {Vec3d(-1.0, -1.0, 1.0), 0, 0},
                          {Vec3d(-1.0, -1.0, -1.0), 1, 3},
                          {Vec3d(-2.0, 1.0, 1.0), 1, 1}};
    for (const Case& item : cases) {
        StaticMeshBuildInput input = InputFor(mesh);
        input.settings.buildScale = item.scale;
        StaticMeshBuildResult built(allocator);
        REQUIRE(BuildStaticMeshLod(input, built));
        const uint32 expected[] = {0, 1, 2, 0, 2, 3};
        REQUIRE(built.indices.Size() == 6);
        for (uint32 index = 0; index < 6; ++index) {
            CHECK(built.indices[index] == expected[index]);
        }

        CHECK(built.positions[2] == Vec3(static_cast<float>(100.0 * item.scale.x),
                                         static_cast<float>(100.0 * item.scale.y), 0.0f));
        CHECK(CountWarnings(built, AssetDiagnosticCode::NegativeBuildScale) == item.warnings);
        for (const MeshDiagnostic& warning : built.warnings) {
            if (warning.code == AssetDiagnosticCode::NegativeBuildScale) {
                CHECK(warning.issue == MeshIssue::NegativeBuildScale &&
                      warning.count == item.count);
            }
        }

        const float z = item.scale.z < 0.0 ? 1.0f : -1.0f;
        CHECK(NearVector(built.normals[0], Vec3(0.0f, 0.0f, z), DirectionTolerance));
        const float x = item.scale.x < 0.0 ? -1.0f : 1.0f;
        CHECK(NearVector(built.tangents[0], Vec3(x, 0.0f, 0.0f), DirectionTolerance));
    }
}

// 비균일 크기의 법선은 역전치(법선 / 크기 후 정규화)라 크기를 먼저 적용한 기하에서 계산한 법선과
// 같고, tangent는 크기를 곱한 뒤 정규화한다(UE ScaleStaticMeshVertex).
TEST(StaticMeshBuild_NonUniformScaleNormals) {
    SystemAllocator allocator;
    const Vec3 tilted[] = {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(100, 100, 100), Vec3(0, 100, 100)};
    const Vec3 stretched[] = {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(100, 200, 100),
                              Vec3(0, 200, 100)};
    const uint32 quad[] = {0, 1, 2, 3};
    const PolygonSpec polygons[] = {{quad, 0}};
    MeshSource mesh(allocator);
    REQUIRE(BuildPolygonMesh(mesh, tilted, polygons, 1, true));
    REQUIRE(PlanarUvs(mesh, 100.0f) != MeshNoAttribute);
    MeshSource reference(allocator);
    REQUIRE(BuildPolygonMesh(reference, stretched, polygons, 1, true));
    REQUIRE(PlanarUvs(reference, 100.0f) != MeshNoAttribute);
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.buildScale = Vec3d(1.0, 2.0, 1.0);
    StaticMeshBuildResult scaled(allocator);
    StaticMeshBuildResult direct(allocator);
    REQUIRE(BuildStaticMeshLod(input, scaled));
    REQUIRE(BuildStaticMeshLod(InputFor(reference), direct));
    REQUIRE(scaled.VertexCount() == 4 && direct.VertexCount() == 4);
    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        CHECK(scaled.positions[vertex] == direct.positions[vertex]);
        CHECK(NearVector(scaled.normals[vertex], Unit(0.0, 1.0, -2.0), DirectionTolerance));
        CHECK(NearVector(scaled.normals[vertex], direct.normals[vertex], DirectionTolerance));
        CHECK(NearVector(scaled.tangents[vertex], Vec3(1.0f, 0.0f, 0.0f), DirectionTolerance));
    }
}

// bounds: 렌더 정점 위치의 상자, 상자 중심 구, 확장이 있으면 상자를 늘리고 반지름 = 반대각선(UE
// CalculateExtendedBounds).
TEST(StaticMeshBuild_BoundsAndExtension) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildUvQuad(mesh));
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.buildScale = Vec3d(1.0, 0.5, 1.0);
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(input, built));
    CHECK(built.bounds.boxMin == Vec3d(0.0, 0.0, 0.0) &&
          built.bounds.boxMax == Vec3d(100.0, 50.0, 0.0));
    CHECK(math::Abs(built.bounds.sphereRadius - math::Sqrt(50.0 * 50.0 + 25.0 * 25.0)) < 1e-9);

    input.positiveBoundsExtension = Vec3d(10.0, 0.0, 5.0);
    input.negativeBoundsExtension = Vec3d(0.0, 20.0, 5.0);
    REQUIRE(BuildStaticMeshLod(input, built));
    CHECK(built.bounds.boxMin == Vec3d(0.0, -20.0, -5.0));
    CHECK(built.bounds.boxMax == Vec3d(110.0, 50.0, 5.0));
    CHECK(math::Abs(built.bounds.sphereRadius - math::Sqrt(55.0 * 55.0 + 35.0 * 35.0 + 5.0 * 5.0)) <
          1e-9);

    // 한쪽만 0이 아니어도 확장이다.
    input.positiveBoundsExtension = Vec3d(0.0, 0.0, 0.0);
    input.negativeBoundsExtension = Vec3d(0.0, 0.0, 1.0);
    REQUIRE(BuildStaticMeshLod(input, built));
    CHECK(built.bounds.boxMin == Vec3d(0.0, 0.0, -1.0));
    CHECK(math::Abs(built.bounds.sphereRadius - math::Sqrt(50.0 * 50.0 + 25.0 * 25.0 + 0.25)) <
          1e-9);
}

} // namespace static_mesh_builder_tests_file
