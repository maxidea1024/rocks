#include "MeshSourceTestSupport.h"

#include <Base/NumericLimits.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>

// ValidateMeshSource 시험이다(Docs/Design/MeshAnimationAssets 3.1·3.5·3.7절, 12절 "위상·속성
// 불변식별 위반 fixture"): 정상 메시(코너 공유·퇴화 삼각형·같은 코너 반복)는 통과하고, 불변식마다
// 하나씩 어긴 메시는 정확히 그 규칙(issue)과 위치로 거부된다. 검증은 메시를 바꾸지 않는다.
using namespace rocks;
namespace mesh_source_validation_tests_file {

using namespace rocks::mesh_source_test;

namespace {

// mesh 검증이 code·issue·element·index로 실패하는지 본다.
bool Rejects(const MeshSource& mesh, MeshIssue issue, MeshElement element, uint32 index,
             const MeshValidateOptions& options = {},
             AssetDiagnosticCode code = AssetDiagnosticCode::InvalidMeshData) noexcept {
    MeshDiagnostic diagnostic;
    auto valid = ValidateMeshSource(mesh, options, &diagnostic);
    return !valid && valid.Error() == ErrorCode::InvalidArgument &&
           diagnostic.error == ErrorCode::InvalidArgument && HasIssue(diagnostic, code, issue) &&
           diagnostic.element == element && diagnostic.index == index;
}

// 둘레 변 하나(0–3)가 빠진 사각형이다.
bool BuildQuadWithoutEdge(MeshSource& mesh) noexcept {
    for (uint32 index = 0; index < 4; ++index) {
        if (!mesh.AddVertex(Vec3(static_cast<float>(index), 0, 0)) || !mesh.AddCorner(index)) {
            return false;
        }
    }

    const uint32 corners[] = {0, 1, 2, 3};
    return mesh.AddEdge(0, 1) && mesh.AddEdge(1, 2) && mesh.AddEdge(2, 3) &&
           mesh.AddPolygonGroup() && mesh.AddPolygon(corners, 0) && mesh.AddTriangle(0, 1, 2, 0) &&
           mesh.AddTriangle(0, 2, 3, 0);
}

} // namespace

// 정상: 빈 메시, 사각형, 코너 공유·퇴화 삼각형·같은 코너 반복 메시, 모든 엔진 속성을 단 메시.
TEST(MeshSource_ValidAcceptsSharedAndDegenerate) {
    SystemAllocator allocator;
    MeshSource empty(allocator);
    CHECK(ValidateMeshSource(empty).IsOk());
    MeshSource quad(allocator);
    REQUIRE(BuildQuad(quad));
    CHECK(ValidateMeshSource(quad).IsOk());
    MeshSource shared(allocator);
    REQUIRE(BuildSharedMesh(shared));
    MeshDiagnostic untouched;
    untouched.index = 1234;
    CHECK(ValidateMeshSource(shared, {}, &untouched).IsOk());
    CHECK(untouched.index == 1234 && untouched.code == AssetDiagnosticCode::None);
    MeshSource rich(allocator);
    REQUIRE(BuildRichMesh(rich));
    MeshValidateOptions options;
    options.boneCount = 7;
    CHECK(ValidateMeshSource(rich, options).IsOk());
    // 정점 하나뿐인 메시(위상 없음)도 정상이다.
    MeshSource point(allocator);
    REQUIRE(point.AddVertex(Vec3(1, 2, 3)));
    CHECK(ValidateMeshSource(point).IsOk());
}

// 위치 비유한값은 오류다(UE는 0으로 고치지만 Rocks는 받지 않는다 — MA-D9).
TEST(MeshSource_ValidRejectsNonFinitePosition) {
    SystemAllocator allocator;
    const float bad[] = {TestQuietNaN(), TestInfinity(), -TestInfinity()};
    for (uint32 component = 0; component < 3; ++component) {
        for (float value : bad) {
            MeshSource mesh(allocator);
            REQUIRE(BuildQuad(mesh));
            float* target = &mesh.Positions()[2].x + component;
            *target = value;
            CHECK(Rejects(mesh, MeshIssue::NonFinitePosition, MeshElement::Vertex, 2));
        }
    }
}

// 번호 범위: 코너의 정점, 변의 정점, 다각형의 묶음·코너, 삼각형의 코너·다각형.
TEST(MeshSource_ValidRejectsIndexRanges) {
    SystemAllocator allocator;
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddCorner(4));
        CHECK(Rejects(mesh, MeshIssue::CornerVertexRange, MeshElement::Corner, 4));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddEdge(1, 4));
        CHECK(Rejects(mesh, MeshIssue::EdgeVertexRange, MeshElement::Edge, 4));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddEdge(9, 0));
        CHECK(Rejects(mesh, MeshIssue::EdgeVertexRange, MeshElement::Edge, 4));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 corners[] = {0, 1, 2};
        REQUIRE(mesh.AddPolygon(corners, 1));
        CHECK(Rejects(mesh, MeshIssue::PolygonGroupRange, MeshElement::Polygon, 1));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 corners[] = {0, 1, 4};
        REQUIRE(mesh.AddPolygon(corners, 0));
        CHECK(Rejects(mesh, MeshIssue::PolygonCornerRange, MeshElement::Polygon, 1));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddTriangle(0, 1, 4, 0));
        CHECK(Rejects(mesh, MeshIssue::TriangleCornerRange, MeshElement::Triangle, 2));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddTriangle(4, 1, 2, 0));
        CHECK(Rejects(mesh, MeshIssue::TriangleCornerRange, MeshElement::Triangle, 2));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddTriangle(0, 1, 2, 1));
        CHECK(Rejects(mesh, MeshIssue::TrianglePolygonRange, MeshElement::Triangle, 2));
    }
}

// 변: 같은 정점을 잇는 변, 같은 정점 쌍의 중복(순서만 다른 것 포함).
TEST(MeshSource_ValidRejectsBadEdges) {
    SystemAllocator allocator;
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddEdge(2, 2));
        CHECK(Rejects(mesh, MeshIssue::EdgeSameVertex, MeshElement::Edge, 4));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddEdge(1, 0));
        CHECK(Rejects(mesh, MeshIssue::DuplicateEdge, MeshElement::Edge, 4));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddEdge(0, 2));
        REQUIRE(mesh.AddEdge(2, 0));
        CHECK(Rejects(mesh, MeshIssue::DuplicateEdge, MeshElement::Edge, 5));
    }
}

// 다각형: 둘레 3 미만, 둘레 변이 변 목록에 없음.
TEST(MeshSource_ValidRejectsBadPolygons) {
    SystemAllocator allocator;
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 corners[] = {0, 1};
        REQUIRE(mesh.AddPolygon(corners, 0));
        CHECK(Rejects(mesh, MeshIssue::PolygonTooFewCorners, MeshElement::Polygon, 1));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddPolygon(Span<const uint32>(), 0));
        CHECK(Rejects(mesh, MeshIssue::PolygonTooFewCorners, MeshElement::Polygon, 1));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuadWithoutEdge(mesh));
        CHECK(Rejects(mesh, MeshIssue::PerimeterEdgeMissing, MeshElement::Polygon, 0));
    }
}

// 삼각형: 다각형의 삼각형 수 != 둘레 − 2(모자람·넘침), 둘레에 없는 코너.
TEST(MeshSource_ValidRejectsBadTriangles) {
    SystemAllocator allocator;
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddTriangle(1, 2, 3, 0));
        CHECK(Rejects(mesh, MeshIssue::TriangleCountMismatch, MeshElement::Polygon, 0));
    }
    {
        // 둘째 다각형에 삼각형이 하나도 없다(둘레 변 (0, 2)는 더해 둔다).
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddEdge(0, 2));
        const uint32 corners[] = {0, 1, 2};
        REQUIRE(mesh.AddPolygon(corners, 0));
        CHECK(Rejects(mesh, MeshIssue::TriangleCountMismatch, MeshElement::Polygon, 1));
    }
    {
        // 사각형의 둘째 삼각형이 둘레 밖 코너(같은 정점을 가리키는 새 코너 4)를 쓴다.
        MeshSource mesh(allocator);
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(mesh.AddVertex(Vec3(static_cast<float>(index), 0, 0)));
            REQUIRE(mesh.AddCorner(index));
        }

        REQUIRE(mesh.AddCorner(3));
        for (uint32 index = 0; index < 4; ++index) {
            REQUIRE(mesh.AddEdge(index, (index + 1) % 4));
        }

        const uint32 corners[] = {0, 1, 2, 3};
        REQUIRE(mesh.AddPolygonGroup());
        REQUIRE(mesh.AddPolygon(corners, 0));
        REQUIRE(mesh.AddTriangle(0, 1, 2, 0));
        REQUIRE(mesh.AddTriangle(0, 2, 4, 0));
        CHECK(Rejects(mesh, MeshIssue::TriangleCornerNotOnPerimeter, MeshElement::Triangle, 1));
    }
    {
        // 코너를 공유하는 두 다각형: 다각형 1의 삼각형이 다각형 0의 코너(공유되지 않은 것)를 쓴다.
        MeshSource mesh(allocator);
        REQUIRE(BuildSharedMesh(mesh));
        const uint32 copy[] = {1, 4, 5, 2};
        REQUIRE(mesh.AddPolygon(copy, 0));
        REQUIRE(mesh.AddTriangle(1, 4, 5, 4));
        REQUIRE(mesh.AddTriangle(1, 5, 3, 4));
        CHECK(Rejects(mesh, MeshIssue::TriangleCornerNotOnPerimeter, MeshElement::Triangle, 7));
    }
}

// 엔진 이름 속성 형태: 값 타입·채널·요소가 3.2절 표와 다르거나 접두 뒤가 빈 이름은 거부한다.
// 대소문자만 다른 엔진 이름도 엔진 이름이다.
TEST(MeshSource_ValidRejectsEngineAttributeShapes) {
    SystemAllocator allocator;
    struct Case {
        MeshElement element;
        StringView name;
        MeshValueType type;
        uint8 channels;
    };

    const Case bad[] = {
        {MeshElement::Corner, "Normal", MeshValueType::Vec4, 1},
        {MeshElement::Corner, "normal", MeshValueType::Vec3, 2},
        {MeshElement::Vertex, "Normal", MeshValueType::Vec3, 1},
        {MeshElement::Corner, "Tangent", MeshValueType::Vec2, 1},
        {MeshElement::Corner, "BinormalSign", MeshValueType::Vec2, 1},
        {MeshElement::Corner, "Color", MeshValueType::Vec3, 1},
        {MeshElement::Corner, "TexCoord", MeshValueType::Vec2, 9},
        {MeshElement::Corner, "TexCoord", MeshValueType::Vec3, 1},
        {MeshElement::Edge, "IsHard", MeshValueType::UInt32, 1},
        {MeshElement::Polygon, "PartName", MeshValueType::Name, 2},
        {MeshElement::PolygonGroup, "MaterialSlotName", MeshValueType::UInt32, 1},
        {MeshElement::Polygon, "MaterialSlotName", MeshValueType::Name, 1},
        {MeshElement::Corner, "SkinWeights", MeshValueType::BoneInfluences, 1},
        {MeshElement::Vertex, "skinweights-cloth", MeshValueType::Vec4, 1},
        {MeshElement::Vertex, "SkinWeights-", MeshValueType::BoneInfluences, 1},
        {MeshElement::Vertex, "Morph-", MeshValueType::SparseVec3, 1},
        {MeshElement::Corner, "Morph-Smile", MeshValueType::SparseVec3, 1},
        {MeshElement::Vertex, "MORPH-Smile", MeshValueType::Vec3, 1},
        {MeshElement::Corner, "MorphNormal-", MeshValueType::SparseVec3, 1},
        {MeshElement::Vertex, "MorphNormal-Smile", MeshValueType::SparseVec3, 1},
        {MeshElement::Corner, "Position", MeshValueType::Vec3, 1},
    };
    for (const Case& entry : bad) {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddAttribute(entry.element, entry.name, entry.type, entry.channels));
        CHECK(Rejects(mesh, MeshIssue::EngineAttributeMismatch, entry.element, MeshNoIndex));
    }

    // 접두와 닮았지만 접두가 아닌 이름, 접두 + 이름(번호 접미 포함)은 사용자·엔진 이름으로 받는다.
    const Case good[] = {
        {MeshElement::Corner, "Morph", MeshValueType::Vec4, 1},
        {MeshElement::Vertex, "MorphX-Smile", MeshValueType::Vec2, 1},
        {MeshElement::Corner, "Normals", MeshValueType::Vec2, 1},
        {MeshElement::Vertex, "Morph-Smile_3", MeshValueType::SparseVec3, 1},
        {MeshElement::Vertex, "skinweights-LOD1", MeshValueType::BoneInfluences, 1},
        {MeshElement::Corner, "texcoord", MeshValueType::Vec2, 8},
        {MeshElement::Corner, "Normal_1", MeshValueType::Vec4, 1},
    };
    for (const Case& entry : good) {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        REQUIRE(mesh.AddAttribute(entry.element, entry.name, entry.type, entry.channels));
        CHECK(ValidateMeshSource(mesh).IsOk());
    }
}

// 속성 값: Bool 0·1 밖, Name 값 내용 규칙(UTF-8·NUL·NFC·길이), 영향 개수 상한(TooManyInfluences
// 코드)·같은 본·본 번호 범위.
TEST(MeshSource_ValidRejectsAttributeValues) {
    SystemAllocator allocator;
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 hard =
            mesh.AddAttribute(MeshElement::Edge, "IsHard", MeshValueType::Bool, 1).Value();
        const uint32 flags =
            mesh.AddAttribute(MeshElement::Corner, "Flags", MeshValueType::Bool, 2).Value();
        mesh.BoolValues(hard, 0)[3] = 1;
        CHECK(ValidateMeshSource(mesh).IsOk());
        mesh.BoolValues(flags, 1)[2] = 2;
        CHECK(Rejects(mesh, MeshIssue::BoolValue, MeshElement::Corner, 2));
    }

    // MaxNameFieldBytes를 넘는 이름과 base가 UTF-16 1,023단위를 넘는 이름.
    Array<char> tooLong(allocator);
    REQUIRE(tooLong.TryResize(MaxNameFieldBytes + 1));
    for (char& value : tooLong) {
        value = 'a';
    }

    Array<char> tooManyUnits(allocator);
    REQUIRE(tooManyUnits.TryResize(MaxNameBaseUtf16Units + 1));
    for (char& value : tooManyUnits) {
        value = 'b';
    }

    const StringView badNames[] = {"\xc3", StringView("A\0B", 3), "e\xCC\x81",
                                   StringView(tooLong.Data(), tooLong.Size()),
                                   StringView(tooManyUnits.Data(), tooManyUnits.Size())};
    for (StringView name : badNames) {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 part =
            mesh.AddAttribute(MeshElement::Polygon, "PartName", MeshValueType::Name, 1).Value();
        REQUIRE(mesh.SetNameValue(part, 0, 0, name));
        CHECK(Rejects(mesh, MeshIssue::NameValue, MeshElement::Polygon, 0));
    }

    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 skin =
            mesh.AddAttribute(MeshElement::Vertex, "SkinWeights", MeshValueType::BoneInfluences, 1)
                .Value();
        BoneInfluence many[MeshMaxSourceInfluences + 1];
        for (uint16 index = 0; index < MeshMaxSourceInfluences + 1; ++index) {
            many[index] = BoneInfluence{index, 5000};
        }

        REQUIRE(
            mesh.SetInfluences(skin, 1, Span<const BoneInfluence>(many, MeshMaxSourceInfluences)));
        CHECK(ValidateMeshSource(mesh).IsOk());
        REQUIRE(mesh.SetInfluences(skin, 3, many));
        CHECK(Rejects(mesh, MeshIssue::TooManyInfluences, MeshElement::Vertex, 3, {},
                      AssetDiagnosticCode::TooManyInfluences));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 skin =
            mesh.AddAttribute(MeshElement::Vertex, "SkinWeights", MeshValueType::BoneInfluences, 1)
                .Value();
        const BoneInfluence twice[] = {{4, 30000}, {2, 5535}, {4, 30000}};
        REQUIRE(mesh.SetInfluences(skin, 2, twice));
        CHECK(Rejects(mesh, MeshIssue::DuplicateInfluenceBone, MeshElement::Vertex, 2));
    }
    {
        MeshSource mesh(allocator);
        REQUIRE(BuildQuad(mesh));
        const uint32 skin =
            mesh.AddAttribute(MeshElement::Vertex, "SkinWeights", MeshValueType::BoneInfluences, 1)
                .Value();
        const BoneInfluence bones[] = {{0, 30000}, {5, 35535}};
        REQUIRE(mesh.SetInfluences(skin, 1, bones));
        // 본 수를 모르면 범위를 보지 않는다. 본 수 6이면 통과, 5면 거부.
        CHECK(ValidateMeshSource(mesh).IsOk());
        MeshValidateOptions options;
        options.boneCount = 6;
        CHECK(ValidateMeshSource(mesh, options).IsOk());
        options.boneCount = 5;
        CHECK(Rejects(mesh, MeshIssue::BoneIndexRange, MeshElement::Vertex, 1, options));
    }
}

// 진단이 속성 번호를 가리킨다.
TEST(MeshSource_ValidReportsAttributeIndex) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    REQUIRE(mesh.AddAttribute(MeshElement::Corner, "Fine", MeshValueType::Float, 1));
    REQUIRE(mesh.AddAttribute(MeshElement::Corner, "Normal", MeshValueType::Float, 1));
    MeshDiagnostic diagnostic;
    CHECK(!ValidateMeshSource(mesh, {}, &diagnostic));
    CHECK(diagnostic.attribute == 1 && diagnostic.count == 0);
    const AssetDiagnostic asset = ToAssetDiagnostic(diagnostic, asset_test::FixedId(9));
    CHECK(asset.code == AssetDiagnosticCode::InvalidMeshData &&
          asset.error == ErrorCode::InvalidArgument && asset.asset == asset_test::FixedId(9) &&
          asset.section == 0 && asset.detail == 0);
    // 실패해도 진단 없이 부를 수 있다.
    CHECK(!ValidateMeshSource(mesh));
}

// 검증의 임시 할당(변 표·삼각형 표·코너 표식·Name NFC 판정)이 실패하면 진단 코드 없이 그 오류다.
// 모든 지점을 차례로 실패시키고, 마지막에는 통과한다.
TEST(MeshSource_ValidScratchFailures) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildRichMesh(mesh));
    // NFC 판정이 할당하는(빠른 검사가 Maybe인) 결합 문자 이름을 둔다.
    const uint32 tags = mesh.FindAttribute(MeshElement::Corner, "Tags");
    // (q + U+0301은 합성형이 없어 NFC다.)
    REQUIRE(mesh.SetNameValue(tags, 0, 0, "q\xCC\x81"));
    bool passed = false;
    uint64 failures = 0;
    for (uint64 attempt = 1; attempt <= 64 && !passed; ++attempt) {
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        MeshValidateOptions options;
        options.scratch = &failing;
        MeshDiagnostic diagnostic;
        auto valid = ValidateMeshSource(mesh, options, &diagnostic);
        if (valid) {
            passed = true;
        } else {
            ++failures;
            CHECK(valid.Error() == ErrorCode::OutOfMemory);
            CHECK(diagnostic.code == AssetDiagnosticCode::None);
        }
    }

    CHECK(passed);
    CHECK(failures >= 4);
}

} // namespace mesh_source_validation_tests_file
