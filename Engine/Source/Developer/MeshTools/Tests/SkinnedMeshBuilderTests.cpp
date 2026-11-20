#include "SkinnedMeshTestSupport.h"

// 스킨 메시 빌드 시험이다(Docs/Design/MeshAnimationAssets.ko.md 6.3·6.5절, 12절 "스켈레톤·스킨"):
// 영향 줄이기·8비트 부호화의 동률과 잔차, bone map, 용접의 스킨 항목, 스킨 임계값, inverse-bind,
// 가중치 프로필, morph target.
using namespace rocks;
namespace skinned_mesh_builder_tests_file {

using namespace rocks::skinned_mesh_test;

// 빌드와 같은 연산 순서의 내적이다(문턱 판정을 bit 그대로 재현한다).
float Dot(const Vec3& a, const Vec3& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

uint32 WeightSum(Span<const BoneInfluence> influences) noexcept {
    uint32 sum = 0;
    for (const BoneInfluence& influence : influences) {
        sum += influence.weight;
    }

    return sum;
}

// 6.5절 7: 가중치 0을 빼고 내림차순(같으면 본 번호 오름차순)으로 정렬해 상한까지 남기고, 버린 것이
// 있으면 합을 65535로 다시 맞춘다. 영향이 없으면 루트 본 0.
TEST(SkinnedMeshBuild_ReduceInfluences) {
    BoneInfluence out[MeshCookedInfluencesHigh] = {};
    bool trimmed = true;
    bool missing = true;
    const BoneInfluence mixed[] = {{3, 100}, {1, 100}, {2, 0}, {5, 65335}};
    CHECK(ReduceBoneInfluences(mixed, 8, out, trimmed, missing) == 3);
    CHECK(!trimmed && !missing);
    CHECK(out[0] == (BoneInfluence{5, 65335}) && out[1] == (BoneInfluence{1, 100}) &&
          out[2] == (BoneInfluence{3, 100}));
    CHECK(ReduceBoneInfluences(mixed, 2, out, trimmed, missing) == 2);
    CHECK(trimmed && !missing && out[0].boneIndex == 5 && out[1].boneIndex == 1);
    CHECK(WeightSum(Span<const BoneInfluence>(out, 2)) == BoneWeightTotal);
    // 동률: 같은 가중치는 본 번호 오름차순으로 남는다(4번 본이 버려진다).
    const BoneInfluence ties[] = {{7, 16384}, {2, 16384}, {9, 16384}, {4, 16383}};
    CHECK(ReduceBoneInfluences(ties, 3, out, trimmed, missing) == 3);
    CHECK(trimmed && out[0] == (BoneInfluence{2, 21845}) && out[1] == (BoneInfluence{7, 21845}) &&
          out[2] == (BoneInfluence{9, 21845}));
    CHECK(ReduceBoneInfluences(ties, 1, out, trimmed, missing) == 1);
    CHECK(trimmed && out[0] == (BoneInfluence{2, BoneWeightTotal}));
    // 잔차는 가장 큰 영향(앞)이 받는다: 40000·20000·5535에서 5535를 버리면 내림 합이 65534.
    const BoneInfluence residual[] = {{1, 20000}, {0, 5535}, {6, 40000}};
    CHECK(ReduceBoneInfluences(residual, 2, out, trimmed, missing) == 2);
    CHECK(out[0].boneIndex == 6 && out[1] == (BoneInfluence{1, 21845}) &&
          out[0].weight == BoneWeightTotal - 21845);
    // 영향 없음·모두 0은 루트 본 하나다.
    CHECK(ReduceBoneInfluences(Span<const BoneInfluence>(), 8, out, trimmed, missing) == 1);
    CHECK(missing && !trimmed && out[0] == (BoneInfluence{0, BoneWeightTotal}));
    const BoneInfluence zeros[] = {{4, 0}, {5, 0}};
    CHECK(ReduceBoneInfluences(zeros, 8, out, trimmed, missing) == 1);
    CHECK(missing && out[0] == (BoneInfluence{0, BoneWeightTotal}));
    // 원본 상한 12개 → 8개.
    BoneInfluence twelve[MeshMaxSourceInfluences] = {};
    for (uint16 at = 0; at < MeshMaxSourceInfluences; ++at) {
        twelve[at] = BoneInfluence{at, static_cast<uint16>(at == 0 ? 5535 : 5455)};
    }

    CHECK(ReduceBoneInfluences(twelve, 8, out, trimmed, missing) == 8);
    CHECK(trimmed && WeightSum(Span<const BoneInfluence>(out, 8)) == BoneWeightTotal);
    for (uint32 at = 1; at < 8; ++at) {
        CHECK(out[at].weight <= out[at - 1].weight);
    }
}

// 6.5절 10: 8비트는 반올림 뒤 잔차(255 − 합, 음수도 된다)를 첫 영향에 더하고 다시 정렬한다.
TEST(SkinnedMeshBuild_QuantizeWeights8) {
    BoneInfluence single[] = {{3, BoneWeightTotal}};
    QuantizeSkinWeights8(single);
    CHECK(single[0] == (BoneInfluence{3, SkinWeightTotal8}));
    // 넷 다 64로 반올림되어 합 256 → 첫 영향이 63이 되고 맨 뒤로 간다.
    BoneInfluence over[] = {{0, 16384}, {1, 16384}, {2, 16384}, {3, 16383}};
    QuantizeSkinWeights8(over);
    CHECK(over[0] == (BoneInfluence{1, 64}) && over[1] == (BoneInfluence{2, 64}) &&
          over[2] == (BoneInfluence{3, 64}) && over[3] == (BoneInfluence{0, 63}));
    // 여섯은 42로 내려가고 3은 0이 되어 합 252 → 첫 영향이 45, 0이 된 영향도 칸을 유지한다.
    BoneInfluence under[] = {{0, 10922}, {1, 10922}, {2, 10922}, {3, 10922},
                             {4, 10922}, {5, 10922}, {6, 3}};
    QuantizeSkinWeights8(under);
    CHECK(under[0] == (BoneInfluence{0, 45}) && under[1] == (BoneInfluence{1, 42}) &&
          under[6] == (BoneInfluence{6, 0}));
    CHECK(WeightSum(under) == SkinWeightTotal8);
    QuantizeSkinWeights8(Span<BoneInfluence>());
}

// 쿠킹 영향: 칸 수(실제 최댓값의 올림), bone map(쓰는 본 오름차순), 본 번호 → map 위치, 합
// 255/65535, 빈 칸 (0, 0), MissingInfluences·InfluencesTrimmed(정점 수·첫 정점·원본 속성 번호).
TEST(SkinnedMeshBuild_InfluencesAndBoneMap) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 12, bones));
    MeshSource mesh(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    // 앞에 다른 속성을 두어 원본 번호와 사본 번호가 다르게 한다.
    REQUIRE(mesh.AddAttribute(MeshElement::Vertex, "Heat", MeshValueType::Float, 1));
    const uint32 weights = AddWeights(mesh, MeshAttributeName::SkinWeights);
    REQUIRE(weights == 1);
    const BoneInfluence v0[] = {{5, BoneWeightTotal}};
    const BoneInfluence v1[] = {{2, 30000}, {5, 35535}};
    BoneInfluence v3[9] = {};
    for (uint16 at = 0; at < 9; ++at) {
        v3[at] =
            BoneInfluence{static_cast<uint16>(at + 1), static_cast<uint16>(at == 0 ? 7 : 8191)};
    }

    REQUIRE(SetWeights(mesh, weights, 0, v0) && SetWeights(mesh, weights, 1, v1) &&
            SetWeights(mesh, weights, 3, v3));
    SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
    SkinnedMeshBuildResult built(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(input, built));
    REQUIRE(built.mesh.VertexCount() == 4);
    CHECK(built.influencesPerVertex == MeshCookedInfluencesHigh &&
          built.weightTotal == SkinWeightTotal8);
    // v3은 본 1(가중치 7)을 버리고 2–9를 남긴다. v2는 루트 0.
    const uint16 expectedMap[] = {0, 2, 3, 4, 5, 6, 7, 8, 9};
    REQUIRE(built.boneMap.Size() == 9);
    for (uint32 at = 0; at < 9; ++at) {
        CHECK(built.boneMap[at] == expectedMap[at]);
    }

    CHECK(built.requiredBones.Size() == 12 && built.inverseBindMatrices.Size() == 12);
    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        CHECK(InfluencesWellFormed(built, VertexInfluences(built, built.influences, vertex)));
    }

    const Span<const BoneInfluence> first = VertexInfluences(built, built.influences, 0);
    CHECK(first[0] == (BoneInfluence{4, SkinWeightTotal8}) && first[1] == (BoneInfluence{0, 0}));
    const Span<const BoneInfluence> second = VertexInfluences(built, built.influences, 1);
    CHECK(second[0].boneIndex == 4 && second[1].boneIndex == 1 &&
          second[0].weight + second[1].weight == SkinWeightTotal8);
    CHECK(VertexInfluences(built, built.influences, 2)[0] == (BoneInfluence{0, SkinWeightTotal8}));
    const MeshDiagnostic* missing = FindWarning(built, AssetDiagnosticCode::MissingInfluences);
    const MeshDiagnostic* trimmed = FindWarning(built, AssetDiagnosticCode::InfluencesTrimmed);
    REQUIRE(missing != nullptr && trimmed != nullptr);
    CHECK(missing->issue == MeshIssue::MissingInfluences && missing->attribute == weights &&
          missing->index == 2 && missing->count == 1 && missing->element == MeshElement::Vertex);
    CHECK(trimmed->issue == MeshIssue::InfluencesTrimmed && trimmed->attribute == weights &&
          trimmed->index == 3 && trimmed->count == 1);
    CHECK(missing < trimmed);

    // 상한 4: 칸 4, v3은 2–5만 남는다. 16비트면 v1 가중치가 그대로다.
    input.settings.boneInfluenceLimit = 4;
    input.settings.useHighPrecisionSkinWeights = true;
    REQUIRE(BuildSkinnedMeshLod(input, built));
    CHECK(built.influencesPerVertex == MeshCookedInfluencesLow &&
          built.weightTotal == BoneWeightTotal);
    CHECK(built.boneMap.Size() == 5);
    const Span<const BoneInfluence> high = VertexInfluences(built, built.influences, 1);
    CHECK(high[0].weight == 35535 && high[1].weight == 30000);
    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        CHECK(InfluencesWellFormed(built, VertexInfluences(built, built.influences, vertex)));
    }

    // 상한 1·상한 100(8로 본다).
    input.settings.boneInfluenceLimit = 1;
    REQUIRE(BuildSkinnedMeshLod(input, built));
    CHECK(built.influencesPerVertex == MeshCookedInfluencesLow);
    CHECK(VertexInfluences(built, built.influences, 1)[1] == (BoneInfluence{0, 0}));
    input.settings.boneInfluenceLimit = 100;
    REQUIRE(BuildSkinnedMeshLod(input, built));
    CHECK(built.influencesPerVertex == MeshCookedInfluencesHigh && built.boneMap.Size() == 9);
    // 가중치 속성이 없으면 모든 정점이 루트 본이고 경고 속성은 MeshNoAttribute다.
    MeshSource rigid(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(rigid));
    SkinnedMeshBuildResult root(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(SkinInputFor(rigid, bones), root));
    CHECK(root.boneMap.Size() == 1 && root.boneMap[0] == 0);
    const MeshDiagnostic* all = FindWarning(root, AssetDiagnosticCode::MissingInfluences);
    REQUIRE(all != nullptr);
    CHECK(all->attribute == MeshNoAttribute && all->index == 0 && all->count == 4);
}

// 위치가 같은 서로 다른 원본 정점 두 쌍(삼각형 둘이 변을 공유하지 않고 정점을 따로 가진다).
const Vec3 SplitPositions[] = {Vec3(0, 0, 0),   Vec3(100, 0, 0),   Vec3(0, 100, 0),
                               Vec3(100, 0, 0), Vec3(100, 100, 0), Vec3(0, 100, 0)};
const uint32 SplitFirst[] = {0, 1, 2};
const uint32 SplitSecond[] = {3, 4, 5};

bool BuildSplitMesh(MeshSource& mesh, Span<const Vec3> positions = SplitPositions) noexcept {
    const PolygonSpec polygons[] = {{SplitFirst, 0}, {SplitSecond, 0}};
    return BuildPolygonMesh(mesh, positions, polygons, 1, false) &&
           RigidWeights(mesh, 0) != MeshNoAttribute;
}

// 6.5절 8: 위치가 겹치는 다른 원본 정점은 영향(기본·프로필)·morph 위치 변화량·MorphNormal 값까지
// 같아야 합친다.
TEST(SkinnedMeshBuild_WeldComparesSkinData) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 3, bones));
    for (uint32 variant = 0; variant < 8; ++variant) {
        MeshSource mesh(fixture.allocator);
        REQUIRE(BuildSplitMesh(mesh));
        const uint32 weights = mesh.FindAttribute(MeshElement::Vertex, "SkinWeights");
        const BoneInfluence other{1, BoneWeightTotal};
        const uint32 profile = AddWeights(mesh, "SkinWeights-Cloth");
        const uint32 morph = AddSparse(mesh, MeshElement::Vertex, "Morph-Smile");
        const uint32 normal = AddSparse(mesh, MeshElement::Corner, "MorphNormal-Smile");
        REQUIRE(profile != MeshNoAttribute && morph != MeshNoAttribute &&
                normal != MeshNoAttribute);
        uint32 expected = 4;
        switch (variant) {
        case 1: // 기본 영향이 다르다.
            REQUIRE(SetWeights(mesh, weights, 3, Span<const BoneInfluence>(&other, 1)));
            expected = 5;
            break;
        case 2: // 프로필 영향이 다르다.
            REQUIRE(SetWeights(mesh, profile, 5, Span<const BoneInfluence>(&other, 1)));
            expected = 5;
            break;
        case 3: // morph 위치 변화량이 다르다.
            REQUIRE(mesh.SetSparseValue(morph, 1, Vec3(0, 0, 1)));
            expected = 5;
            break;
        case 4: // 같은 morph 변화량은 합친다.
            REQUIRE(mesh.SetSparseValue(morph, 1, Vec3(0, 0, 1)) &&
                    mesh.SetSparseValue(morph, 3, Vec3(0, 0, 1)));
            break;
        case 5: // 겹치는 코너의 MorphNormal 값이 다르다(코너 1과 3).
            REQUIRE(mesh.SetSparseValue(normal, 3, Vec3(0, 0.5f, 0)));
            expected = 5;
            break;
        case 7: { // 기본 영향 개수가 다르다.
            const BoneInfluence two[] = {{0, 30000}, {1, 35535}};
            REQUIRE(SetWeights(mesh, weights, 3, two));
            expected = 5;
            break;
        }
        case 6: // morph 비유한 항목은 0으로 본다(경고).
            REQUIRE(mesh.SetSparseValue(morph, 1, Vec3(mesh_source_test::TestQuietNaN(), 0, 0)));
            break;
        default:
            break;
        }

        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(SkinInputFor(mesh, bones), built));
        CHECK(built.mesh.VertexCount() == expected);
        CHECK(built.mesh.indices.Size() == 6);
        CHECK(built.profiles.Size() == 1 && built.morphTargets.Size() == 1);
        if (variant == 6) {
            const MeshDiagnostic* nan = FindWarning(built, AssetDiagnosticCode::NonFiniteAttribute);
            REQUIRE(nan != nullptr);
            CHECK(nan->issue == MeshIssue::NonFiniteMorph && nan->attribute == morph &&
                  nan->element == MeshElement::Vertex && nan->index == 1 && nan->count == 1);
        }
    }
}

// 6.5절 6: 용접은 thresholdPosition·thresholdUV·thresholdTangentNormal을 쓰고, removeDegenerates를
// 꺼도 용접으로 번호가 겹친 삼각형은 index에서 빠진다(정적 빌드는 남긴다).
TEST(SkinnedMeshBuild_ThresholdsAndCollapsedTriangles) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 2, bones));
    struct PositionCase {
        float offset;
        float threshold;
        uint32 vertices;
    };

    const PositionCase positions[] = {
        {0.0005f, 0.001f, 4}, {0.002f, 0.001f, 6}, {0.0f, 0.0f, 4}, {0.00001f, 0.0f, 6}};
    for (const PositionCase& item : positions) {
        Vec3 moved[6];
        for (uint32 at = 0; at < 6; ++at) {
            moved[at] = SplitPositions[at];
        }

        moved[3].x += item.offset;
        moved[5].y += item.offset;
        MeshSource mesh(fixture.allocator);
        REQUIRE(BuildSplitMesh(mesh, moved));
        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        input.settings.thresholdPosition = item.threshold;
        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, built));
        CHECK(built.mesh.VertexCount() == item.vertices);
    }

    // 원본 법선·tangent를 쓰고(재계산 끔) UV·법선 하나만 조금 다르게 한다.
    for (uint32 variant = 0; variant < 4; ++variant) {
        MeshSource mesh(fixture.allocator);
        REQUIRE(BuildSplitMesh(mesh));
        const uint32 normal = CornerAttribute(mesh, MeshAttributeName::Normal, MeshValueType::Vec3);
        const uint32 tangent =
            CornerAttribute(mesh, MeshAttributeName::Tangent, MeshValueType::Vec3);
        const uint32 sign =
            CornerAttribute(mesh, MeshAttributeName::BinormalSign, MeshValueType::Float);
        const uint32 uv = PlanarUvs(mesh, 100.0f);
        REQUIRE(uv != MeshNoAttribute);
        for (uint32 corner = 0; corner < 6; ++corner) {
            mesh.SetVec3(normal, 0, corner, Vec3(0, 0, 1));
            mesh.SetVec3(tangent, 0, corner, Vec3(1, 0, 0));
            mesh.SetFloat(sign, 0, corner, 1.0f);
        }

        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        input.settings.recomputeNormals = false;
        input.settings.recomputeTangents = false;
        uint32 expected = 4;
        if (variant < 2) {
            mesh.SetVec2(uv, 0, 3, mesh.GetVec2(uv, 0, 3) + Vec2(0.0005f, 0.0f));
            input.settings.thresholdUV = variant == 0 ? 1.0f / 1024.0f : 0.0001f;
            expected = variant == 0 ? 4 : 5;
        } else {
            mesh.SetVec3(normal, 0, 3, Vec3(0, 0.0005f, 1));
            input.settings.thresholdTangentNormal = variant == 2 ? 0.00002f : 0.001f;
            expected = variant == 2 ? 5 : 4;
        }

        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, built));
        CHECK(built.mesh.VertexCount() == expected);
    }

    // 가는 삼각형: 두 꼭짓점이 0.0005 떨어져 있다(가까운 쌍을 0·1, 0·2로 바꾼 것도 같다).
    const Vec3 slivers[3][3] = {{Vec3(100, 0, 0), Vec3(100, 0.0005f, 0), Vec3(0, 0, 0)},
                                {Vec3(100, 0, 0), Vec3(0, 0, 0), Vec3(100, 0.0005f, 0)},
                                {Vec3(0, 0, 0), Vec3(100, 0, 0), Vec3(100, 0.0005f, 0)}};
    const uint32 triangle[] = {0, 1, 2};
    const PolygonSpec polygons[] = {{triangle, 0}};
    for (uint32 pair = 0; pair < 2; ++pair) {
        MeshSource rotated(fixture.allocator);
        REQUIRE(BuildPolygonMesh(rotated, slivers[pair], polygons, 1, false));
        REQUIRE(RigidWeights(rotated, 1) != MeshNoAttribute);
        SkinnedMeshBuildInput input = SkinInputFor(rotated, bones);
        input.settings.thresholdPosition = 0.001f;
        input.settings.removeDegenerates = false;
        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, built));
        CHECK(built.mesh.VertexCount() == 2 && built.mesh.indices.IsEmpty());
    }

    MeshSource mesh(fixture.allocator);
    REQUIRE(BuildPolygonMesh(mesh, slivers[2], polygons, 1, false));
    REQUIRE(RigidWeights(mesh, 1) != MeshNoAttribute);
    SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
    input.settings.thresholdPosition = 0.001f;
    input.settings.removeDegenerates = false;
    SkinnedMeshBuildResult built(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(input, built));
    CHECK(built.mesh.VertexCount() == 2 && built.mesh.indices.IsEmpty());
    CHECK(built.mesh.sections.Size() == 1 && built.mesh.sections[0].triangleCount == 0);
    // 렌더 정점은 남으므로 bone map은 그 정점의 본을 담는다.
    CHECK(built.boneMap.Size() == 1 && built.boneMap[0] == 1);
    input.settings.removeDegenerates = true;
    REQUIRE(BuildSkinnedMeshLod(input, built));
    CHECK(built.mesh.VertexCount() == 0 && built.mesh.indices.IsEmpty() &&
          built.boneMap.IsEmpty() && built.influencesPerVertex == MeshCookedInfluencesLow);
    StaticMeshBuildInput staticInput = InputFor(mesh);
    staticInput.settings.removeDegenerates = false;
    StaticMeshBuildResult staticBuilt(fixture.allocator);
    REQUIRE(BuildStaticMeshLod(staticInput, staticBuilt));
    CHECK(staticBuilt.VertexCount() == 3 && staticBuilt.indices.Size() == 3);
}

// 6.5절 11·12: 필요 본은 메시 본 전체, inverse-bind는 component 공간 기준 pose(local × 부모)의
// 역이다.
TEST(SkinnedMeshBuild_BindPose) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    const Transform poses[] = {
        Transform(Quatd::Identity(), Vec3d(0.0, 0.0, 10.0), Vec3d(1.0, 1.0, 1.0)),
        Transform(Quatd(0.0, 0.0, 0.70710678118654752, 0.70710678118654752), Vec3d(20.0, 0.0, 0.0),
                  Vec3d(2.0, 2.0, 2.0)),
        Transform(Quatd(0.5, 0.5, 0.5, 0.5), Vec3d(0.0, -5.0, 3.0), Vec3d(1.0, 0.5, 3.0)),
        Transform(Quatd::Identity(), Vec3d(1.0, 2.0, 3.0), Vec3d(1.0, 1.0, 1.0)),
    };
    const int32 parents[] = {NoParentBone, 0, 1, 0};
    for (uint32 bone = 0; bone < 4; ++bone) {
        REQUIRE(AddBone(fixture.names, bones, parents[bone], poses[bone]));
    }

    MeshSource mesh(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    REQUIRE(RigidWeights(mesh, 2) != MeshNoAttribute);
    SkinnedMeshBuildResult built(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(SkinInputFor(mesh, bones), built));
    REQUIRE(built.requiredBones.Size() == 4 && built.inverseBindMatrices.Size() == 4);
    Mat4d component[4];
    for (uint32 bone = 0; bone < 4; ++bone) {
        CHECK(built.requiredBones[bone] == bone);
        const Mat4d local = poses[bone].ToMatrix();
        component[bone] =
            parents[bone] < 0 ? local : local * component[static_cast<uint32>(parents[bone])];
        const Mat4d expected = component[bone].TryInverse().Value();
        CHECK(platform::MemoryEquals(&expected, &built.inverseBindMatrices[bone], sizeof(Mat4d)));
        const Mat4d identity = built.inverseBindMatrices[bone] * component[bone];
        for (uint32 at = 0; at < 16; ++at) {
            const double want = at % 5 == 0 ? 1.0 : 0.0;
            CHECK(math::Abs(identity.m[at] - want) < 1.0e-9);
        }
    }

    CHECK(built.boneMap.Size() == 1 && built.boneMap[0] == 2);
}

// 6.5절 9: 프로필은 같은 규칙으로 줄이고, bone map에 프로필 본이 들어가며, 빠진 정점은 루트 본이다.
TEST(SkinnedMeshBuild_WeightProfiles) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 10, bones));
    MeshSource mesh(fixture.allocator);
    REQUIRE(mesh_source_test::BuildQuad(mesh));
    const uint32 cloth = AddWeights(mesh, "SkinWeights-Cloth");
    const uint32 base = RigidWeights(mesh, 3);
    const uint32 muscle = AddWeights(mesh, "skinweights-Muscle");
    REQUIRE(cloth != MeshNoAttribute && base != MeshNoAttribute && muscle != MeshNoAttribute);
    const BoneInfluence clothWeights[] = {{7, 40000}, {3, 25535}};
    const BoneInfluence wide[] = {{1, 10000}, {2, 10000}, {4, 10000},
                                  {5, 10000}, {6, 10000}, {8, 15535}};
    for (uint32 vertex = 0; vertex < 3; ++vertex) {
        REQUIRE(SetWeights(mesh, cloth, vertex, clothWeights));
    }

    for (uint32 vertex = 0; vertex < 4; ++vertex) {
        REQUIRE(SetWeights(mesh, muscle, vertex, wide));
    }

    SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
    input.settings.useHighPrecisionSkinWeights = true;
    SkinnedMeshBuildResult built(fixture.allocator);
    REQUIRE(BuildSkinnedMeshLod(input, built));
    REQUIRE(built.profiles.Size() == 2);
    // 프로필은 원본 순서다(Cloth가 기본보다 앞에 있어도 기본은 influences, 프로필은 Cloth·Muscle).
    CHECK(built.profiles[0].sourceAttribute == cloth &&
          built.profiles[1].sourceAttribute == muscle);
    // 6칸(Muscle)이 있어 8칸이다. bone map: 0(Cloth 빠진 정점), 1–8.
    CHECK(built.influencesPerVertex == MeshCookedInfluencesHigh);
    const uint16 expectedMap[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
    REQUIRE(built.boneMap.Size() == 9);
    for (uint32 at = 0; at < 9; ++at) {
        CHECK(built.boneMap[at] == expectedMap[at]);
    }

    const Span<const BoneInfluence> clothFirst =
        VertexInfluences(built, built.profiles[0].influences, 0);
    CHECK(clothFirst[0] == (BoneInfluence{7, 40000}) && clothFirst[1] == (BoneInfluence{3, 25535}));
    CHECK(VertexInfluences(built, built.profiles[0].influences, 3)[0] ==
          (BoneInfluence{0, BoneWeightTotal}));
    CHECK(VertexInfluences(built, built.influences, 2)[0] == (BoneInfluence{3, BoneWeightTotal}));
    for (const SkinWeightProfileBuild& profile : built.profiles) {
        for (uint32 vertex = 0; vertex < 4; ++vertex) {
            CHECK(InfluencesWellFormed(built, VertexInfluences(built, profile.influences, vertex)));
        }
    }

    // 경고 순서: 기본(경고 없음), Cloth의 MissingInfluences(정점 3), Muscle(경고 없음).
    const MeshDiagnostic* missing = FindWarning(built, AssetDiagnosticCode::MissingInfluences);
    REQUIRE(missing != nullptr);
    CHECK(missing->attribute == cloth && missing->index == 3 && missing->count == 1);
    // 상한 4: Muscle이 2개를 버린다.
    input.settings.boneInfluenceLimit = 4;
    REQUIRE(BuildSkinnedMeshLod(input, built));
    const MeshDiagnostic* trimmed = FindWarning(built, AssetDiagnosticCode::InfluencesTrimmed);
    REQUIRE(trimmed != nullptr);
    CHECK(trimmed->attribute == muscle && trimmed->count == 4 && trimmed->index == 0);
    CHECK(built.influencesPerVertex == MeshCookedInfluencesLow);
}

// 격자의 정점 하나를 옮긴 메시를 처음부터 빌드한 렌더 법선이다(법선 재계산의 기준값).
bool MovedNormals(SkinFixture& fixture, const BoneHierarchy& bones,
                  const SkinnedMeshBuildSettings& settings, uint32 moved, const Vec3& delta,
                  Array<Vec3>& out) noexcept {
    MeshSource mesh(fixture.allocator);
    if (!BuildGrid(mesh, 4, 100.0f) || RigidWeights(mesh, 0) == MeshNoAttribute) {
        return false;
    }

    mesh.Positions()[moved] = mesh.Positions()[moved] + delta;
    SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
    input.settings = settings;
    SkinnedMeshBuildResult built(fixture.allocator);
    if (!BuildSkinnedMeshLod(input, built)) {
        return false;
    }

    out.Clear();
    return out.TryAppend(built.mesh.normals.View()).IsOk();
}

// 6.5절 13: morph target 변화량. MorphNormal이 없으면 법선을 다시 계산하며(옮긴 메시를 처음부터
// 빌드한 법선과 bit가 같다), 담는 조건은 |Δp|² > t² 또는 (추적할 때) |Δn|² > 0.01이다.
TEST(SkinnedMeshBuild_MorphTargets) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 2, bones));
    const Vec3 raise(0.0f, 0.0f, 50.0f);
    for (uint32 variant = 0; variant < 3; ++variant) {
        SkinnedMeshBuildSettings settings;
        settings.computeWeightedNormals = variant == 1;
        settings.morphThresholdPosition = variant == 2 ? 100.0f : 0.015f;
        MeshSource mesh(fixture.allocator);
        REQUIRE(BuildGrid(mesh, 4, 100.0f));
        REQUIRE(RigidWeights(mesh, 0) != MeshNoAttribute);
        const uint32 morph = AddSparse(mesh, MeshElement::Vertex, "Morph-Raise");
        REQUIRE(morph != MeshNoAttribute && mesh.SetSparseValue(morph, 5, raise));
        // -0.0 항목은 옮기지 않는다(법선을 다시 계산하지 않는다).
        REQUIRE(mesh.SetSparseValue(morph, 15, Vec3(-0.0f, 0.0f, 0.0f)));
        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        input.settings = settings;
        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, built));
        REQUIRE(built.mesh.VertexCount() == 16 && built.morphTargets.Size() == 1);
        Array<Vec3> moved(fixture.allocator);
        REQUIRE(MovedNormals(fixture, bones, settings, 5, raise, moved));
        const MorphTargetBuild& target = built.morphTargets[0];
        CHECK(target.positionAttribute == morph && target.normalAttribute == MeshNoAttribute);
        usize next = 0;
        uint32 normalOnly = 0;
        const uint32 raised = GridRenderVertex(built.mesh, 4, 100.0f, 5);
        REQUIRE(raised != MeshNoIndex);
        for (uint32 vertex = 0; vertex < 16; ++vertex) {
            const Vec3 position = vertex == raised ? raise : Vec3(0, 0, 0);
            const Vec3 normal = moved[vertex] - built.mesh.normals[vertex];
            const float limit = settings.morphThresholdPosition * settings.morphThresholdPosition;
            const bool included = Dot(position, position) > limit ||
                                  Dot(normal, normal) > MorphNormalDeltaThresholdSquared;
            if (!included) {
                continue;
            }

            REQUIRE(next < target.deltas.Size());
            const MorphTargetDelta& delta = target.deltas[next++];
            CHECK(delta.vertex == vertex);
            CHECK(platform::MemoryEquals(&delta.positionDelta, &position, sizeof(Vec3)));
            CHECK(platform::MemoryEquals(&delta.normalDelta, &normal, sizeof(Vec3)));
            normalOnly += vertex == raised ? 0 : 1;
        }

        CHECK(next == target.deltas.Size());
        CHECK(normalOnly > 0);
    }
}

// MorphNormal 짝(이름은 대소문자 무시), 짝 없는 MorphNormal, 위치 문턱 경계(같으면 뺀다), 법선을
// 추적하지 않을 때(원본 법선 + MorphNormal 없음)는 법선만 바뀐 정점을 뺀다, 빈 target.
TEST(SkinnedMeshBuild_MorphAttributesAndThreshold) {
    SkinFixture fixture;
    REQUIRE(fixture.ready);
    BoneHierarchy bones(fixture.allocator);
    REQUIRE(ChainBones(fixture.names, 2, bones));
    MeshSource mesh(fixture.allocator);
    REQUIRE(BuildGrid(mesh, 3, 100.0f));
    REQUIRE(RigidWeights(mesh, 1) != MeshNoAttribute);
    const uint32 frown = AddSparse(mesh, MeshElement::Corner, "MorphNormal-Frown");
    const uint32 smile = AddSparse(mesh, MeshElement::Vertex, "Morph-Smile");
    const uint32 smileNormal = AddSparse(mesh, MeshElement::Corner, "morphnormal-SMILE");
    const uint32 lift = AddSparse(mesh, MeshElement::Vertex, "Morph-Lift");
    const uint32 empty = AddSparse(mesh, MeshElement::Vertex, "Morph-Empty");
    REQUIRE(frown != MeshNoAttribute && smile != MeshNoAttribute &&
            smileNormal != MeshNoAttribute && lift != MeshNoAttribute && empty != MeshNoAttribute);
    // Smile: 정점 1은 문턱과 같아 빠지고, 정점 2는 넘는다. 코너 4는 법선만 바뀐다(0.04 > 0.01),
    // 코너 6은 법선 변화가 작아 빠진다.
    REQUIRE(mesh.SetSparseValue(smile, 1, Vec3(0.015f, 0, 0)) &&
            mesh.SetSparseValue(smile, 2, Vec3(0.016f, 0, 0)) &&
            mesh.SetSparseValue(smileNormal, 4, Vec3(0, 0.2f, 0)) &&
            mesh.SetSparseValue(smileNormal, 6, Vec3(0, 0.05f, 0)));
    REQUIRE(mesh.SetSparseValue(frown, 8, Vec3(0.3f, 0, 0)));
    // Lift: 정점 4를 올리고 정점 0은 문턱 아래로 조금(y만) 옮긴다.
    REQUIRE(mesh.SetSparseValue(lift, 4, Vec3(0, 0, 30)) &&
            mesh.SetSparseValue(lift, 0, Vec3(0, 0.001f, 0)));
    // Empty에는 비유한 항목 둘뿐이다(0으로 본다 — 경고 하나, 첫 항목 번호, 항목 수).
    REQUIRE(mesh.SetSparseValue(empty, 0, Vec3(mesh_source_test::TestInfinity(), 0, 0)) &&
            mesh.SetSparseValue(empty, 3, Vec3(0, mesh_source_test::TestQuietNaN(), 0)));
    // 원본 법선을 쓴다.
    const uint32 normal = CornerAttribute(mesh, MeshAttributeName::Normal, MeshValueType::Vec3);
    for (uint32 corner = 0; corner < mesh.Count(MeshElement::Corner); ++corner) {
        mesh.SetVec3(normal, 0, corner, Vec3(0, 0, 1));
    }

    for (uint32 variant = 0; variant < 2; ++variant) {
        SkinnedMeshBuildInput input = SkinInputFor(mesh, bones);
        input.settings.recomputeNormals = variant == 1;
        SkinnedMeshBuildResult built(fixture.allocator);
        REQUIRE(BuildSkinnedMeshLod(input, built));
        REQUIRE(built.mesh.VertexCount() == 9 && built.morphTargets.Size() == 4);
        auto render = [&](uint32 source) noexcept {
            return GridRenderVertex(built.mesh, 3, 100.0f, source);
        };
        // 렌더 정점 순서: 원본 2(0.016)가 원본 4(법선만)보다 앞인지에 따라 목록 순서가 정해진다.
        const bool twoFirst = render(2) < render(4);
        const MorphTargetBuild& smileTarget = built.morphTargets[0];
        CHECK(smileTarget.positionAttribute == smile && smileTarget.normalAttribute == smileNormal);
        REQUIRE(smileTarget.deltas.Size() == 2);
        const MorphTargetDelta& moved = smileTarget.deltas[twoFirst ? 0 : 1];
        const MorphTargetDelta& turned = smileTarget.deltas[twoFirst ? 1 : 0];
        CHECK(moved.vertex == render(2) && moved.positionDelta == Vec3(0.016f, 0, 0));
        CHECK(turned.vertex == render(4) && turned.normalDelta == Vec3(0, 0.2f, 0) &&
              turned.positionDelta == Vec3(0, 0, 0));
        const MorphTargetBuild& liftTarget = built.morphTargets[1];
        CHECK(liftTarget.positionAttribute == lift &&
              liftTarget.normalAttribute == MeshNoAttribute);
        // 법선을 추적하지 않으면(원본 법선) 정점 4만, 추적하면 이웃의 법선 변화도 담는다.
        if (variant == 0) {
            REQUIRE(liftTarget.deltas.Size() == 1);
            CHECK(liftTarget.deltas[0].vertex == render(4) &&
                  liftTarget.deltas[0].positionDelta == Vec3(0, 0, 30));
        } else {
            CHECK(liftTarget.deltas.Size() > 1);
        }

        CHECK(built.morphTargets[2].positionAttribute == empty &&
              built.morphTargets[2].deltas.IsEmpty());
        const MorphTargetBuild& frownTarget = built.morphTargets[3];
        CHECK(frownTarget.positionAttribute == MeshNoAttribute &&
              frownTarget.normalAttribute == frown);
        REQUIRE(frownTarget.deltas.Size() == 1);
        CHECK(frownTarget.deltas[0].vertex == render(8) &&
              frownTarget.deltas[0].normalDelta == Vec3(0.3f, 0, 0));
        const MeshDiagnostic* nan = FindWarning(built, AssetDiagnosticCode::NonFiniteAttribute);
        REQUIRE(nan != nullptr);
        CHECK(nan->issue == MeshIssue::NonFiniteMorph && nan->attribute == empty &&
              nan->index == 0 && nan->count == 2);
    }
}

} // namespace skinned_mesh_builder_tests_file
