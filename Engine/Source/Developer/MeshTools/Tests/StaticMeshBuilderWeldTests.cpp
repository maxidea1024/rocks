#include "MeshToolsTestSupport.h"

#include "../Private/MeshBuildInternal.h"

#include <Core/Math/Color.h>
#include <MeshSource/Tests/MeshSourceTestSupport.h>
#include <stdio.h>

// 렌더 정점 용접·퇴화 제거·섹션·슬롯 대응·색과 UV 채널·값 정리 경고 시험이다(Docs/Design/
// Docs/Design/MeshAnimationAssets 5.2절 9·10단계, 4.3절, 12절 "빌드"). 임계값은 UE 원본 값이고 비교는
// 성분마다 |a − b| ≤ 임계값이다(경계 안·밖을 함께 본다).
using namespace rocks;
namespace static_mesh_builder_weld_tests_file {

using namespace rocks::mesh_tools_test;

// 삼각형 A (0,0,0)(100,0,0)(0,100,0)과 B (d,0,0)(0,−100,0)(100,−100,0). 정점은 따로이고 A의 0번과
// B의 0번(코너 0과 3)만 위치가 겹친다. UV 채널 둘(채널 0 = 평면 매핑, 채널 1 = 채널 0 + 1 — 채널
// 1은 tangent에 쓰이지 않아 UV 비교만 따로 바꿀 수 있다). 두 면 모두 법선 −Z, tangent +X다.
bool BuildTwinTriangles(MeshSource& mesh, float d) noexcept {
    const Vec3 points[] = {Vec3(0, 0, 0), Vec3(100, 0, 0),  Vec3(0, 100, 0),
                           Vec3(d, 0, 0), Vec3(0, -100, 0), Vec3(100, -100, 0)};
    const uint32 a[] = {0, 1, 2};
    const uint32 b[] = {3, 4, 5};
    const PolygonSpec polygons[] = {{a, 0}, {b, 0}};
    return BuildPolygonMesh(mesh, points, polygons, 1, false) &&
           PlanarUvs(mesh, 100.0f, 2) != MeshNoAttribute;
}

uint32 BuildVertexCount(const MeshSource& mesh, bool removeDegenerates = true) noexcept {
    SystemAllocator allocator;
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.removeDegenerates = removeDegenerates;
    StaticMeshBuildResult built(allocator);
    return BuildStaticMeshLod(input, built) ? built.VertexCount() : MeshNoIndex;
}

float NextUp(float value) noexcept {
    return platform::BitCast<float>(platform::BitCast<uint32>(value) + 1u);
}

// 위치 임계값 경계: 겹치는 정점 사이 거리가 MeshPointsSameThreshold면 합치고(5 정점) 한 ulp 더 멀면
// 합치지 않는다(6). removeDegenerates를 끄면 임계값이 0이라 정확히 같은 위치만 합친다.
TEST(StaticMeshBuild_WeldPositionThreshold) {
    SystemAllocator allocator;
    struct Case {
        float distance;
        bool removeDegenerates;
        uint32 vertices;
    };
    const Case cases[] = {{0.0f, true, 5},
                          {MeshPointsSameThreshold, true, 5},
                          {NextUp(MeshPointsSameThreshold), true, 6},
                          {0.0f, false, 5},
                          {MeshPointsSameThreshold, false, 6}};
    for (const Case& item : cases) {
        MeshSource mesh(allocator);
        REQUIRE(BuildTwinTriangles(mesh, item.distance));
        CHECK(BuildVertexCount(mesh, item.removeDegenerates) == item.vertices);
    }

    // 겹침은 크기 적용 전 위치로 찾고 용접 비교는 크기 적용 뒤 위치로 한다(UE와 같다): 경계 거리를
    // 두 배로 늘리면 겹치는 후보였어도 합치지 않는다.
    MeshSource mesh(allocator);
    REQUIRE(BuildTwinTriangles(mesh, MeshPointsSameThreshold));
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.buildScale = Vec3d(2.0, 1.0, 1.0);
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(input, built));
    CHECK(built.VertexCount() == 6);
}

// 속성 하나만 다른 코너는 합치지 않는다: UV(채널 1, 1/1024 경계 안·밖), 8비트 sRGB가 다른 색, 법선,
// 쌍법선 부호. 같은 8비트 sRGB로 가는 색 차이는 합치고 먼저 나온 코너의 선형 색이 남는다.
TEST(StaticMeshBuild_WeldRequiresEveryAttribute) {
    SystemAllocator allocator;
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildTwinTriangles(mesh, 0.0f));
        const uint32 uv = mesh.FindAttribute(MeshElement::Corner, MeshAttributeName::TexCoord);
        const Vec2 base = mesh.GetVec2(uv, 1, 3);
        mesh.SetVec2(uv, 1, 3, Vec2(base.x + MeshUvsSameThreshold, base.y));
        CHECK(BuildVertexCount(mesh) == 5);
        mesh.SetVec2(uv, 1, 3, Vec2(base.x, NextUp(base.y + MeshUvsSameThreshold)));
        CHECK(BuildVertexCount(mesh) == 6);
    }

    {
        MeshSource mesh(allocator);
        REQUIRE(BuildTwinTriangles(mesh, 0.0f));
        const uint32 color = CornerAttribute(mesh, MeshAttributeName::Color, MeshValueType::Vec4);
        for (uint32 corner = 0; corner < 6; ++corner) {
            mesh.SetVec4(color, 0, corner, Vec4(0.5f, 0.25f, 0.125f, 1.0f));
        }

        // 0.5f와 그 다음 float는 같은 sRGB byte다.
        mesh.SetVec4(color, 0, 3, Vec4(NextUp(0.5f), 0.25f, 0.125f, 1.0f));
        CHECK(LinearColor(NextUp(0.5f), 0.25f, 0.125f, 1.0f).ToSRGB() ==
              LinearColor(0.5f, 0.25f, 0.125f, 1.0f).ToSRGB());
        StaticMeshBuildResult built(allocator);
        REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
        CHECK(built.VertexCount() == 5);
        REQUIRE(built.colors.Size() == 5);
        CHECK(built.colors[built.indices[3]].x == 0.5f);
        mesh.SetVec4(color, 0, 3, Vec4(0.5f, 0.25f, 0.125f, 0.5f));
        CHECK(BuildVertexCount(mesh) == 6);
    }

    {
        // 법선·tangent·부호를 주고 재계산을 끈다(모든 코너가 유효해 그대로 남는다).
        MeshSource mesh(allocator);
        REQUIRE(BuildTwinTriangles(mesh, 0.0f));
        const uint32 normal = CornerAttribute(mesh, MeshAttributeName::Normal, MeshValueType::Vec3);
        const uint32 tangent =
            CornerAttribute(mesh, MeshAttributeName::Tangent, MeshValueType::Vec3);
        const uint32 sign =
            CornerAttribute(mesh, MeshAttributeName::BinormalSign, MeshValueType::Float);
        for (uint32 corner = 0; corner < 6; ++corner) {
            mesh.SetVec3(normal, 0, corner, Vec3(0.0f, 0.0f, -1.0f));
            mesh.SetVec3(tangent, 0, corner, Vec3(1.0f, 0.0f, 0.0f));
            mesh.SetFloat(sign, 0, corner, -1.0f);
        }

        StaticMeshBuildInput input = InputFor(mesh);
        input.settings.recomputeNormals = false;
        input.settings.recomputeTangents = false;
        SystemAllocator local;
        StaticMeshBuildResult built(local);
        REQUIRE(BuildStaticMeshLod(input, built));
        CHECK(built.VertexCount() == 5);
        mesh.SetFloat(sign, 0, 3, 1.0f);
        REQUIRE(BuildStaticMeshLod(input, built));
        CHECK(built.VertexCount() == 6);
        mesh.SetFloat(sign, 0, 3, -1.0f);
        // 법선 x 성분이 임계값을 넘게 다르다(정규화 뒤 차이 ≈ 2.5e-5 > 2e-5).
        mesh.SetVec3(normal, 0, 3, Vec3(2.5e-5f, 0.0f, -1.0f));
        REQUIRE(BuildStaticMeshLod(input, built));
        CHECK(built.VertexCount() == 6);
        mesh.SetVec3(normal, 0, 3, Vec3(1.0e-5f, 0.0f, -1.0f));
        REQUIRE(BuildStaticMeshLod(input, built));
        CHECK(built.VertexCount() == 5);
    }
}

// 퇴화 삼각형: 두 코너가 같은 정점인 삼각형(정확히 같은 위치)은 설정과 무관하게 빠지고(UE 임계값 0
// 비교도 같다고 본다), 두 꼭짓점이 임계값 안인 삼각형은 removeDegenerates일 때만 빠진다. 일직선
// 위의 서로 다른 세 점(면적 0)은 남는다(UE 판정식 — 면적 판정이 아니다, MA-D8).
TEST(StaticMeshBuild_DegenerateTriangles) {
    SystemAllocator allocator;
    for (uint32 remove = 0; remove < 2; ++remove) {
        MeshSource shared(allocator);
        REQUIRE(mesh_source_test::BuildSharedMesh(shared));
        StaticMeshBuildInput input = InputFor(shared);
        input.settings.removeDegenerates = remove != 0;
        StaticMeshBuildResult built(allocator);
        REQUIRE(BuildStaticMeshLod(input, built));
        // 사각형 둘(삼각형 넷)만 남는다. 묶음 B(퇴화 삼각형만)는 빈 섹션이다.
        CHECK(built.indices.Size() == 12);
        REQUIRE(built.sections.Size() == 2);
        CHECK(built.sections[0].triangleCount == 4 && built.sections[1].triangleCount == 0);
        CHECK(built.VertexCount() == 6);

        const Vec3 near[] = {Vec3(0, 0, 0), Vec3(1.0e-5f, 0, 0), Vec3(0, 100, 0)};
        const Vec3 line[] = {Vec3(0, 0, 0), Vec3(50, 0, 0), Vec3(100, 0, 0)};
        const uint32 triangle[] = {0, 1, 2};
        const PolygonSpec polygons[] = {{triangle, 0}};
        MeshSource sliver(allocator);
        REQUIRE(BuildPolygonMesh(sliver, near, polygons, 1, true));
        input.source = &sliver;
        REQUIRE(BuildStaticMeshLod(input, built));
        CHECK(built.indices.Size() == (remove != 0 ? 0u : 3u));

        MeshSource collinear(allocator);
        REQUIRE(BuildPolygonMesh(collinear, line, polygons, 1, true));
        input.source = &collinear;
        REQUIRE(BuildStaticMeshLod(input, built));
        REQUIRE(built.indices.Size() == 3);
        // 면 법선이 없어 코너 법선은 0 벡터로 남는다.
        CHECK(built.normals[0] == Vec3(0.0f, 0.0f, 0.0f));
    }
}

// 슬롯 대응(4.3절): 묶음 MaterialSlotName과 슬롯 importedSlotName을 Name 비교 규칙(ASCII 대소문자
// 무시)으로 맞추고 첫 일치를 쓴다. 없으면 묶음 번호 슬롯 + MaterialSlotFallback(index = 묶음). 빈
// 이름(None)은 빈 슬롯 이름과 맞는다. 섹션 정보는 머티리얼 번호·castShadow를 덮어쓴다(덮어쓴 섹션은
// 대체 경고가 없다).
TEST(StaticMeshBuild_SlotMatchingAndSectionInfo) {
    SystemAllocator allocator;
    const Vec3 points[] = {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(0, 100, 0)};
    const uint32 triangle[] = {0, 1, 2};
    const PolygonSpec polygons[] = {
        {triangle, 0}, {triangle, 1}, {triangle, 2}, {triangle, 3}, {triangle, 4}};
    MeshSource mesh(allocator);
    REQUIRE(BuildPolygonMesh(mesh, points, polygons, 5, false));
    const StringView groupNames[] = {"Paint", "Glass", "Chrome", "", "Chrome"};
    REQUIRE(SlotNames(mesh, groupNames));
    const StringView slots[] = {"glass", "PAINT", "", "paint"};
    const MeshBuildSectionInfo info[] = {
        {MeshNoIndex, false}, {7, true}, {MeshNoIndex, true}, {MeshNoIndex, true}, {2, false}};
    StaticMeshBuildInput input = InputFor(mesh);
    input.slotNames = slots;
    input.sectionInfo = Span<const MeshBuildSectionInfo>(info, 4);
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(input, built));
    REQUIRE(built.sections.Size() == 5);
    CHECK(built.sections[0].materialIndex == 1 && !built.sections[0].castShadow);
    CHECK(built.sections[1].materialIndex == 7 && built.sections[1].castShadow);
    CHECK(built.sections[2].materialIndex == 2 && built.sections[2].castShadow);
    CHECK(built.sections[3].materialIndex == 2);
    // sectionInfo가 짧아 묶음 4는 기본값(대체 + 그림자 켬)이다.
    CHECK(built.sections[4].materialIndex == 4 && built.sections[4].castShadow);
    REQUIRE(CountWarnings(built, AssetDiagnosticCode::MaterialSlotFallback) == 2);
    uint32 seen = 0;
    for (const MeshDiagnostic& warning : built.warnings) {
        if (warning.code == AssetDiagnosticCode::MaterialSlotFallback) {
            CHECK(warning.issue == MeshIssue::MaterialSlotFallback);
            CHECK(warning.element == MeshElement::PolygonGroup);
            CHECK(warning.index == (seen == 0 ? 2u : 4u));
            ++seen;
        }
    }

    // 섹션 정보가 묶음 4의 번호를 덮어쓰면 그 경고가 없다.
    input.sectionInfo = info;
    REQUIRE(BuildStaticMeshLod(input, built));
    CHECK(built.sections[4].materialIndex == 2 && !built.sections[4].castShadow);
    CHECK(CountWarnings(built, AssetDiagnosticCode::MaterialSlotFallback) == 1);

    // MaterialSlotName 속성이 없으면 모든 묶음이 None이라 빈 슬롯 이름과 맞는다.
    MeshSource unnamed(allocator);
    REQUIRE(BuildPolygonMesh(unnamed, points, polygons, 5, false));
    const StringView none[] = {"Body", ""};
    StaticMeshBuildInput plain = InputFor(unnamed);
    plain.slotNames = none;
    REQUIRE(BuildStaticMeshLod(plain, built));
    for (const MeshBuildSection& section : built.sections) {
        CHECK(section.materialIndex == 1);
    }

    CHECK(CountWarnings(built, AssetDiagnosticCode::MaterialSlotFallback) == 0);
}

// 섹션은 묶음 순서이고 index는 묶음마다 삼각형 순서로 이어 붙인다. 빈 묶음은 (0, 0) 섹션이고
// min·max 정점은 그 섹션 index의 범위다.
TEST(StaticMeshBuild_SectionsFollowGroups) {
    SystemAllocator allocator;
    const Vec3 points[] = {Vec3(0, 0, 0),    Vec3(10, 0, 0),   Vec3(0, 10, 0),  Vec3(100, 0, 0),
                           Vec3(110, 0, 0),  Vec3(100, 10, 0), Vec3(200, 0, 0), Vec3(210, 0, 0),
                           Vec3(200, 10, 0), Vec3(300, 0, 0),  Vec3(310, 0, 0), Vec3(300, 10, 0)};
    const uint32 t0[] = {0, 1, 2};
    const uint32 t1[] = {3, 4, 5};
    const uint32 t2[] = {6, 7, 8};
    const uint32 t3[] = {9, 10, 11};
    const PolygonSpec polygons[] = {{t0, 2}, {t1, 0}, {t2, 2}, {t3, 0}};
    MeshSource mesh(allocator);
    REQUIRE(BuildPolygonMesh(mesh, points, polygons, 3, true));
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    const uint32 expected[] = {3, 4, 5, 9, 10, 11, 0, 1, 2, 6, 7, 8};
    REQUIRE(built.indices.Size() == 12);
    for (uint32 index = 0; index < 12; ++index) {
        CHECK(built.indices[index] == expected[index]);
    }

    REQUIRE(built.sections.Size() == 3);
    CHECK(built.sections[0].firstIndex == 0 && built.sections[0].triangleCount == 2);
    CHECK(built.sections[0].minVertex == 3 && built.sections[0].maxVertex == 11);
    CHECK(built.sections[1].firstIndex == 0 && built.sections[1].triangleCount == 0);
    CHECK(built.sections[1].minVertex == 0 && built.sections[1].maxVertex == 0);
    CHECK(built.sections[2].firstIndex == 6 && built.sections[2].triangleCount == 2);
    CHECK(built.sections[2].minVertex == 0 && built.sections[2].maxVertex == 8);
}

// UV 채널 여럿은 채널 우선 배치(texCoords[c × 정점 수 + v])이고 색은 선형 float 그대로 남는다.
TEST(StaticMeshBuild_UvChannelsAndColors) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    REQUIRE(PlanarUvs(mesh, 100.0f, 3) != MeshNoAttribute);
    const uint32 color = CornerAttribute(mesh, MeshAttributeName::Color, MeshValueType::Vec4);
    for (uint32 corner = 0; corner < 4; ++corner) {
        mesh.SetVec4(color, 0, corner, Vec4(0.1f * corner, 2.0f, -0.5f, 0.75f));
    }

    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    REQUIRE(built.VertexCount() == 4 && built.texCoordChannels == 3);
    REQUIRE(built.texCoords.Size() == 12 && built.colors.Size() == 4);
    for (uint32 channel = 0; channel < 3; ++channel) {
        const Vec2 uv = built.texCoords[channel * 4 + 2];
        CHECK(uv.x == 1.0f + static_cast<float>(channel) && uv.y == 1.0f);
    }

    CHECK(built.colors[3].x == 0.1f * 3 && built.colors[3].y == 2.0f && built.colors[3].z == -0.5f);
}

// 값 정리 경고(5.2절 2단계)는 결과 경고 앞에 붙고 속성 번호는 원본 메시 번호다(사본 번호가 아니다).
TEST(StaticMeshBuild_SanitizeWarningsUseSourceAttributes) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    REQUIRE(mesh.AddAttribute(MeshElement::Corner, "Tint", MeshValueType::Vec4, 1));
    const uint32 color = CornerAttribute(mesh, MeshAttributeName::Color, MeshValueType::Vec4);
    const uint32 uv = PlanarUvs(mesh, 100.0f);
    mesh.SetVec2(uv, 0, 1, Vec2(mesh_source_test::TestQuietNaN(), 0.0f));
    mesh.SetVec2(uv, 0, 2, Vec2(2000.0f, 0.0f));
    mesh.SetVec4(color, 0, 0, Vec4(mesh_source_test::TestInfinity(), 0.0f, 0.0f, 1.0f));
    StaticMeshBuildResult built(allocator);
    REQUIRE(BuildStaticMeshLod(InputFor(mesh), built));
    uint32 found = 0;
    for (const MeshDiagnostic& warning : built.warnings) {
        if (warning.issue == MeshIssue::NonFiniteTexCoord ||
            warning.issue == MeshIssue::UvOutOfRange) {
            CHECK(warning.attribute == uv && warning.count == 1);
            ++found;
        }

        if (warning.issue == MeshIssue::NonFiniteColor) {
            CHECK(warning.attribute == color && warning.count == 1);
            ++found;
        }
    }

    CHECK(found == 3);
    // 정리된 값이 빌드에 쓰인다(NaN UV → 0, 비유한 색 → 흰색).
    REQUIRE(built.colors.Size() == built.VertexCount());
    CHECK(built.colors[built.indices[0]].x == 1.0f);
    // 원본은 그대로다.
    CHECK(mesh.GetVec2(uv, 0, 1).x != mesh.GetVec2(uv, 0, 1).x);
}

// 겹치는 코너 정렬 키(5.4절 16): z가 모두 같은 평면 격자(정점 100 × 100, 간격 10·7 — 키가 같은 다른
// 격자점이 드물게, 정점마다 코너 하나 공유)도 UE 가중합 키 0.30X + 0.33Y + 0.37Z로 퍼져 훑기의 비교
// 횟수가 wedge 수에 비례한다(z만으로 정렬하면 같은 z 묶음 전체를 훑어 수십억 번이 된다). 겹침 표는
// 정확히 같은 정점을 쓰는 다른 wedge들이다.
TEST(StaticMeshBuild_OverlapKeySpreadsFlatMeshes) {
    SystemAllocator allocator;
    constexpr uint32 side = 100;
    Array<Vec3> positions(allocator);
    for (uint32 y = 0; y < side; ++y) {
        for (uint32 x = 0; x < side; ++x) {
            REQUIRE(positions.TryPushBack(Vec3(10.0f * x, 7.0f * y, 5.0f)));
        }
    }

    Array<uint32> quads(allocator);
    REQUIRE(quads.TryResize(static_cast<usize>(side - 1) * (side - 1) * 4));
    Array<PolygonSpec> polygons(allocator);
    for (uint32 y = 0; y + 1 < side; ++y) {
        for (uint32 x = 0; x + 1 < side; ++x) {
            uint32* quad = quads.Data() + (static_cast<usize>(y) * (side - 1) + x) * 4;
            quad[0] = y * side + x;
            quad[1] = y * side + x + 1;
            quad[2] = (y + 1) * side + x + 1;
            quad[3] = (y + 1) * side + x;
            REQUIRE(polygons.TryPushBack(PolygonSpec{Span<const uint32>(quad, 4), 0}));
        }
    }

    MeshSource mesh(allocator);
    REQUIRE(BuildPolygonMesh(mesh, positions.View(), polygons.View(), 1, true));
    mesh_build::OverlappingWedges overlaps(allocator);
    REQUIRE(mesh_build::FindOverlappingWedges(mesh, MeshPointsSameThreshold, allocator, overlaps));
    const uint32 wedges = 3 * mesh.Count(MeshElement::Triangle);
    // 정점마다 그 정점을 쓰는 wedge 수(최대 6)다.
    Array<uint32> uses(allocator);
    REQUIRE(uses.TryResize(mesh.Count(MeshElement::Vertex)));
    for (uint32 wedge = 0; wedge < wedges; ++wedge) {
        ++uses[mesh.CornerVertices()[mesh.TriangleCorners()[wedge / 3].corners[wedge % 3]]];
    }

    for (uint32 wedge = 0; wedge < wedges; ++wedge) {
        const uint32 vertex =
            mesh.CornerVertices()[mesh.TriangleCorners()[wedge / 3].corners[wedge % 3]];
        CHECK(overlaps.Row(wedge).Size() == uses[vertex] - 1);
    }

    CHECK(overlaps.comparisons <= 8ull * wedges);
    printf("StaticMeshBuild_OverlapKeySpreadsFlatMeshes: %u wedges, %llu comparisons\n", wedges,
           static_cast<unsigned long long>(overlaps.comparisons));
}

} // namespace static_mesh_builder_weld_tests_file
