#include "MeshSourceTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>

// MeshSource 편집 API 시험이다(Docs/Design/MeshAnimationAssets 3.1·3.2·3.7절): 요소 추가와
// 배열 모양, 속성 추가 규칙(인자·이름·중복·예약 이름), 기본값, 값 접근, 가변 길이 값의 덮어쓰기·
// 별칭, 희소 값의 +0.0/-0.0, 할당 실패 주입의 강한 보장.
using namespace rocks;
namespace mesh_source_edit_tests_file {

using namespace rocks::mesh_source_test;

// 요소 추가: 번호가 차례로 매겨지고 배열 모양이 요소 수와 맞으며, 변은 작은 번호 먼저로 담긴다.
TEST(MeshSource_AddElementsKeepsShape) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    CHECK(mesh.Count(MeshElement::Invalid) == 0);
    CHECK(mesh.Count(static_cast<MeshElement>(7)) == 0);
    for (uint32 kind = 1; kind <= MeshElementKindCount; ++kind) {
        CHECK(mesh.Count(static_cast<MeshElement>(kind)) == 0);
    }

    REQUIRE(BuildQuad(mesh));
    CHECK(mesh.Count(MeshElement::Vertex) == 4);
    CHECK(mesh.Count(MeshElement::Corner) == 4);
    CHECK(mesh.Count(MeshElement::Edge) == 4);
    CHECK(mesh.Count(MeshElement::Polygon) == 1);
    CHECK(mesh.Count(MeshElement::Triangle) == 2);
    CHECK(mesh.Count(MeshElement::PolygonGroup) == 1);
    CHECK(mesh.Positions().Size() == 4);
    CHECK(mesh.PolygonCornerEnds().Size() == 1 && mesh.PolygonCornerEnds()[0] == 4);
    CHECK(mesh.PolygonCorners(0).Size() == 4);
    CHECK(mesh.PolygonGroups()[0] == 0);
    CHECK(mesh.TrianglePolygons()[1] == 0);
    CHECK(mesh.TriangleCorners()[1].corners[2] == 3);
    // AddEdge(3, 0)은 (0, 3)으로 담긴다.
    CHECK(mesh.Edges()[3].first == 0 && mesh.Edges()[3].second == 3);
    CHECK(ValidateMeshSource(mesh).IsOk());

    // 위치는 제자리에서 고칠 수 있다.
    mesh.Positions()[2] = Vec3(5.0f, 6.0f, 7.0f);
    const MeshSource& view = mesh;
    CHECK(view.Positions()[2].z == 7.0f);
}

// 둘째 다각형의 둘레는 앞 다각형 끝에서 시작한다.
TEST(MeshSource_PolygonCornersFollowEnds) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildSharedMesh(mesh));
    CHECK(mesh.PolygonCorners(0).Size() == 4);
    CHECK(mesh.PolygonCorners(1)[0] == 1 && mesh.PolygonCorners(1)[3] == 2);
    CHECK(mesh.PolygonCorners(2).Size() == 3);
    CHECK(mesh.PolygonCornerList().Size() == 14);
    CHECK(ValidateMeshSource(mesh).IsOk());
}

// 속성 추가의 인자 규칙: 요소·값 타입 Invalid·범위 밖, 0채널, 가변 길이 타입의 다채널, None·잘못된
// 이름은 InvalidArgument다. 실패는 속성 수를 바꾸지 않는다.
TEST(MeshSource_AddAttributeRejectsBadArguments) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    struct Case {
        MeshElement element;
        StringView name;
        MeshValueType type;
        uint8 channels;
    };

    const Case cases[] = {
        {MeshElement::Invalid, "A", MeshValueType::Float, 1},
        {static_cast<MeshElement>(7), "A", MeshValueType::Float, 1},
        {MeshElement::Corner, "A", MeshValueType::Invalid, 1},
        {MeshElement::Corner, "A", static_cast<MeshValueType>(10), 1},
        {MeshElement::Corner, "A", MeshValueType::Float, 0},
        {MeshElement::Vertex, "A", MeshValueType::BoneInfluences, 2},
        {MeshElement::Vertex, "A", MeshValueType::SparseVec3, 2},
        {MeshElement::Corner, "", MeshValueType::Float, 1},
        {MeshElement::Corner, "none", MeshValueType::Float, 1},
        {MeshElement::Corner, StringView("A\0B", 3), MeshValueType::Float, 1},
        {MeshElement::Corner, "\xff", MeshValueType::Float, 1},
        // NFD(e + U+0301)는 Name 내용 규칙(NFC)에 걸린다.
        {MeshElement::Corner, "e\xCC\x81", MeshValueType::Float, 1},
    };
    for (const Case& entry : cases) {
        auto added = mesh.AddAttribute(entry.element, entry.name, entry.type, entry.channels);
        CHECK(!added && added.Error() == ErrorCode::InvalidArgument);
    }

    CHECK(mesh.AttributeCount() == 0);
}

// 같은 (element, 이름)은 Name 비교 규칙(ASCII 대소문자 무시)으로 AlreadyExists다. 요소가 다르면
// 같은 이름도 된다. 정점 Position은 위치가 쓰는 이름이라 AlreadyExists다(다른 요소의 Position은
// 추가는 되고 검증이 엔진 이름 형태로 거부한다).
TEST(MeshSource_AddAttributeNamesAreUniquePerElement) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    REQUIRE(mesh.AddAttribute(MeshElement::Corner, "Weight", MeshValueType::Float, 1));
    auto again = mesh.AddAttribute(MeshElement::Corner, "WEIGHT", MeshValueType::Vec2, 1);
    CHECK(!again && again.Error() == ErrorCode::AlreadyExists);
    CHECK(mesh.AddAttribute(MeshElement::Vertex, "weight", MeshValueType::Float, 1));
    // 번호 접미가 다르면 다른 이름이다.
    CHECK(mesh.AddAttribute(MeshElement::Corner, "Weight_1", MeshValueType::Float, 1));
    auto position = mesh.AddAttribute(MeshElement::Vertex, "position", MeshValueType::Vec3, 1);
    CHECK(!position && position.Error() == ErrorCode::AlreadyExists);
    CHECK(mesh.FindAttribute(MeshElement::Corner, "weight") == 0);
    CHECK(mesh.FindAttribute(MeshElement::Vertex, "Weight") == 1);
    CHECK(mesh.FindAttribute(MeshElement::Edge, "Weight") == MeshNoAttribute);
    CHECK(mesh.AttributeInfo(2).name == StringView("Weight_1"));
    CHECK(mesh.AttributeCount() == 3);
}

// 새 속성과 새 요소의 기본값: 0, 엔진 Color는 (1, 1, 1, 1)(UE 기본 흰색), Name은 빈 문자열, 영향은
// 없음, 희소는 +0. 다른 요소의 Color는 흰색이 아니다.
TEST(MeshSource_AttributeDefaults) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    const uint32 color =
        mesh.AddAttribute(MeshElement::Corner, "color", MeshValueType::Vec4, 1).Value();
    const uint32 vertexColor =
        mesh.AddAttribute(MeshElement::Vertex, "Color", MeshValueType::Vec4, 1).Value();
    const uint32 count =
        mesh.AddAttribute(MeshElement::Corner, "Count", MeshValueType::UInt32, 2).Value();
    const uint32 flag =
        mesh.AddAttribute(MeshElement::Edge, "Flag", MeshValueType::Bool, 1).Value();
    const uint32 label =
        mesh.AddAttribute(MeshElement::Polygon, "Label", MeshValueType::Name, 1).Value();
    const uint32 skin = mesh.AddAttribute(MeshElement::Vertex, MeshAttributeName::SkinWeights,
                                          MeshValueType::BoneInfluences, 1)
                            .Value();
    const uint32 morph =
        mesh.AddAttribute(MeshElement::Vertex, "Morph-A", MeshValueType::SparseVec3, 1).Value();
    const uint32 scalar =
        mesh.AddAttribute(MeshElement::Triangle, "Scalar", MeshValueType::Float, 1).Value();
    const uint32 group =
        mesh.AddAttribute(MeshElement::PolygonGroup, "Group", MeshValueType::Vec2, 1).Value();
    CHECK(mesh.GetVec4(color, 0, 3).x == 1.0f && mesh.GetVec4(color, 0, 3).w == 1.0f);
    CHECK(mesh.GetVec4(vertexColor, 0, 0).x == 0.0f);
    CHECK(mesh.UInt32Values(count, 1).Size() == 4 && mesh.UInt32Values(count, 1)[3] == 0);
    CHECK(mesh.BoolValues(flag, 0).Size() == 4);
    CHECK(mesh.NameValue(label, 0, 0).IsEmpty());
    CHECK(mesh.Influences(skin, 2).IsEmpty());
    CHECK(mesh.SparseValues(morph).IsEmpty());
    CHECK(mesh.GetFloat(scalar, 0, 1) == 0.0f);
    CHECK(mesh.GetVec2(group, 0, 0).y == 0.0f);

    // 요소를 더하면 그 요소의 모든 속성이 한 칸 늘고 기본값이 들어간다.
    REQUIRE(mesh.AddCorner(0));
    CHECK(mesh.GetVec4(color, 0, 4).y == 1.0f);
    CHECK(mesh.UInt32Values(count, 0).Size() == 5);
    REQUIRE(mesh.AddEdge(0, 2));
    CHECK(mesh.BoolValues(flag, 0)[4] == 0);
    const uint32 corners[] = {0, 2, 4};
    REQUIRE(mesh.AddPolygon(corners, 0));
    CHECK(mesh.NameValue(label, 0, 1).IsEmpty());
    REQUIRE(mesh.AddVertex(Vec3(1, 2, 3)));
    CHECK(mesh.Influences(skin, 4).IsEmpty());
    CHECK(mesh.GetVec4(vertexColor, 0, 4).w == 0.0f);
    REQUIRE(mesh.AddTriangle(0, 2, 4, 1));
    CHECK(mesh.GetFloat(scalar, 0, 2) == 0.0f);
    REQUIRE(mesh.AddPolygonGroup());
    CHECK(mesh.GetVec2(group, 0, 1).x == 0.0f);
    CHECK(mesh.FloatValues(group, 0).Size() == 4);
}

// 값 접근: 벡터 쓰기·읽기, 채널별 배열, const 접근.
TEST(MeshSource_VectorAccess) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    const uint32 uv =
        mesh.AddAttribute(MeshElement::Corner, "TexCoord", MeshValueType::Vec2, 3).Value();
    const uint32 normal =
        mesh.AddAttribute(MeshElement::Corner, "Normal", MeshValueType::Vec3, 1).Value();
    const uint32 sign =
        mesh.AddAttribute(MeshElement::Corner, "BinormalSign", MeshValueType::Float, 1).Value();
    mesh.SetVec2(uv, 2, 3, Vec2(0.5f, 0.25f));
    mesh.SetVec3(normal, 0, 1, Vec3(0.0f, 1.0f, 0.0f));
    mesh.SetFloat(sign, 0, 2, -1.0f);
    mesh.FloatValues(uv, 1)[0] = 9.0f;
    const MeshSource& view = mesh;
    CHECK(view.GetVec2(uv, 2, 3).y == 0.25f);
    CHECK(view.FloatValues(uv, 2)[6] == 0.5f);
    CHECK(view.FloatValues(uv, 1)[0] == 9.0f);
    CHECK(view.FloatValues(uv, 0)[0] == 0.0f);
    CHECK(view.GetVec3(normal, 0, 1).y == 1.0f);
    CHECK(view.GetFloat(sign, 0, 2) == -1.0f);
    const uint32 words =
        mesh.AddAttribute(MeshElement::Corner, "Words", MeshValueType::UInt32, 1).Value();
    const uint32 bools =
        mesh.AddAttribute(MeshElement::Corner, "Bools", MeshValueType::Bool, 1).Value();
    mesh.UInt32Values(words, 0)[2] = 77;
    mesh.BoolValues(bools, 0)[1] = 1;
    CHECK(view.UInt32Values(words, 0)[2] == 77);
    CHECK(view.BoolValues(bools, 0)[1] == 1);
    const MeshAttributeInfo info = view.AttributeInfo(uv);
    CHECK(info.element == MeshElement::Corner && info.type == MeshValueType::Vec2 &&
          info.channels == 3 && info.name == StringView("TexCoord"));
}

// Name 값: 같은 길이 이하는 제자리, 더 길면 저장소 끝에 덧붙인다. 같은 저장소의 다른 값을 넘겨도
// (별칭) 바르게 복사된다.
TEST(MeshSource_NameValuesOverwriteAndAlias) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    const uint32 tags =
        mesh.AddAttribute(MeshElement::Corner, "Tags", MeshValueType::Name, 2).Value();
    REQUIRE(mesh.SetNameValue(tags, 0, 0, "Alpha"));
    REQUIRE(mesh.SetNameValue(tags, 0, 1, "Beta"));
    REQUIRE(mesh.SetNameValue(tags, 1, 3, "Gamma"));
    CHECK(mesh.NameValue(tags, 0, 0) == StringView("Alpha"));
    REQUIRE(mesh.SetNameValue(tags, 0, 0, "Al"));
    CHECK(mesh.NameValue(tags, 0, 0) == StringView("Al"));
    REQUIRE(mesh.SetNameValue(tags, 0, 0, "LongerName"));
    CHECK(mesh.NameValue(tags, 0, 0) == StringView("LongerName"));
    CHECK(mesh.NameValue(tags, 0, 1) == StringView("Beta"));
    // 별칭: 저장소 안 값을 다른 칸(더 긴 쪽 — 덧붙임)과 같은 칸(제자리)에 쓴다.
    REQUIRE(mesh.SetNameValue(tags, 0, 2, mesh.NameValue(tags, 0, 0)));
    CHECK(mesh.NameValue(tags, 0, 2) == StringView("LongerName"));
    REQUIRE(mesh.SetNameValue(tags, 0, 1, mesh.NameValue(tags, 0, 1)));
    CHECK(mesh.NameValue(tags, 0, 1) == StringView("Beta"));
    REQUIRE(mesh.SetNameValue(tags, 0, 1, ""));
    CHECK(mesh.NameValue(tags, 0, 1).IsEmpty());
    CHECK(mesh.NameValue(tags, 1, 3) == StringView("Gamma"));
    CHECK(mesh.NameValue(tags, 1, 0).IsEmpty());
}

// 영향: 같거나 적으면 제자리, 많으면 덧붙임. 별칭 입력도 된다. 상한을 넘는 개수도 담는다(검증이
// 거부한다).
TEST(MeshSource_InfluencesOverwriteAndAlias) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    const uint32 skin =
        mesh.AddAttribute(MeshElement::Vertex, "SkinWeights", MeshValueType::BoneInfluences, 1)
            .Value();
    const BoneInfluence two[] = {{1, 30000}, {2, 35535}};
    const BoneInfluence one[] = {{7, BoneWeightTotal}};
    REQUIRE(mesh.SetInfluences(skin, 0, two));
    REQUIRE(mesh.SetInfluences(skin, 1, one));
    REQUIRE(mesh.SetInfluences(skin, 0, one));
    CHECK(mesh.Influences(skin, 0).Size() == 1 && mesh.Influences(skin, 0)[0].boneIndex == 7);
    REQUIRE(mesh.SetInfluences(skin, 1, two));
    CHECK(mesh.Influences(skin, 1).Size() == 2 && mesh.Influences(skin, 1)[1].weight == 35535);
    // 별칭: 저장소 안 값을 더 긴 칸으로.
    REQUIRE(mesh.SetInfluences(skin, 2, mesh.Influences(skin, 1)));
    CHECK(mesh.Influences(skin, 2).Size() == 2 && mesh.Influences(skin, 2)[0].boneIndex == 1);
    REQUIRE(mesh.SetInfluences(skin, 2, mesh.Influences(skin, 2)));
    CHECK(mesh.Influences(skin, 2)[1].boneIndex == 2);
    REQUIRE(mesh.SetInfluences(skin, 2, Span<const BoneInfluence>()));
    CHECK(mesh.Influences(skin, 2).IsEmpty());
    BoneInfluence many[MeshMaxSourceInfluences + 1];
    for (uint16 index = 0; index < MeshMaxSourceInfluences + 1; ++index) {
        many[index] = BoneInfluence{index, 1};
    }

    REQUIRE(mesh.SetInfluences(skin, 3, many));
    CHECK(mesh.Influences(skin, 3).Size() == MeshMaxSourceInfluences + 1);
}

// 희소 값: 0이 아닌 것만 항목이다. 비트 단위 +0.0 셋은 지우고, -0.0은 0이 아닌 값으로 보존한다.
TEST(MeshSource_SparseKeepsNegativeZero) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildQuad(mesh));
    const uint32 morph =
        mesh.AddAttribute(MeshElement::Vertex, "Morph-Blink", MeshValueType::SparseVec3, 1).Value();
    REQUIRE(mesh.SetSparseValue(morph, 2, Vec3(0.0f, 0.0f, 0.0f)));
    CHECK(mesh.SparseValues(morph).IsEmpty());
    // 항목이 없으면 +0이다.
    CHECK(platform::BitCast<uint32>(mesh.SparseValue(morph, 2).x) == 0);
    REQUIRE(mesh.SetSparseValue(morph, 3, Vec3(-0.0f, 0.0f, 0.0f)));
    REQUIRE(mesh.SetSparseValue(morph, 1, Vec3(1.0f, 2.0f, 3.0f)));
    REQUIRE(mesh.SparseValues(morph).Size() == 2);
    CHECK(mesh.SparseValues(morph)[0].index == 1 && mesh.SparseValues(morph)[1].index == 3);
    CHECK(platform::BitCast<uint32>(mesh.SparseValue(morph, 3).x) == 0x80000000u);
    CHECK(mesh.SparseValue(morph, 0).x == 0.0f);
    CHECK(mesh.SparseValue(morph, 1).z == 3.0f);
    // 덮어쓰기와 +0.0으로 지우기.
    REQUIRE(mesh.SetSparseValue(morph, 1, Vec3(4.0f, 0.0f, 0.0f)));
    CHECK(mesh.SparseValue(morph, 1).x == 4.0f);
    REQUIRE(mesh.SetSparseValue(morph, 1, Vec3()));
    CHECK(mesh.SparseValues(morph).Size() == 1 && mesh.SparseValues(morph)[0].index == 3);
    REQUIRE(mesh.SetSparseValue(morph, 3, Vec3(0.0f, -0.0f, 0.0f)));
    CHECK(platform::BitCast<uint32>(mesh.SparseValue(morph, 3).y) == 0x80000000u);
    CHECK(platform::BitCast<uint32>(mesh.SparseValue(morph, 3).x) == 0);
    REQUIRE(mesh.SetSparseValue(morph, 0, Vec3(0.0f, 0.0f, -0.0f)));
    CHECK(mesh.SparseValues(morph).Size() == 2);
    CHECK(platform::BitCast<uint32>(mesh.SparseValue(morph, 0).z) == 0x80000000u);
}

// 이동: 옮긴 메시가 내용을 그대로 갖는다.
TEST(MeshSource_MoveKeepsContent) {
    SystemAllocator allocator;
    MeshSource mesh(allocator);
    REQUIRE(BuildRichMesh(mesh));
    MeshSource copy(allocator);
    REQUIRE(BuildRichMesh(copy));
    MeshSource moved(Move(mesh));
    CHECK(SameMesh(moved, copy));
    MeshSource assigned(allocator);
    assigned = Move(moved);
    CHECK(SameMesh(assigned, copy));
    CHECK(&assigned.GetAllocator() == &allocator);
}

// 할당 실패 주입: 요소 추가·속성 추가·값 쓰기의 모든 할당 지점에서 실패해도 메시는 호출 전과 같다
// (강한 보장).
TEST(MeshSource_EditFailuresKeepState) {
    SystemAllocator base;
    // 각 연산: 실패 주입 allocator 위에 리치 메시를 만들고(주입 끔), 주입을 켜고 연산을 한 번 한다.
    struct Operation {
        const char* name;
        bool (*run)(MeshSource& mesh);
    };

    const Operation operations[] = {
        {"AddVertex", [](MeshSource& mesh) { return mesh.AddVertex(Vec3(1, 1, 1)).IsOk(); }},
        {"AddCorner", [](MeshSource& mesh) { return mesh.AddCorner(0).IsOk(); }},
        {"AddEdge", [](MeshSource& mesh) { return mesh.AddEdge(0, 5).IsOk(); }},
        {"AddPolygonGroup", [](MeshSource& mesh) { return mesh.AddPolygonGroup().IsOk(); }},
        {"AddPolygon",
         [](MeshSource& mesh) {
             const uint32 corners[] = {0, 1, 4};
             return mesh.AddPolygon(corners, 0).IsOk();
         }},
        {"AddTriangle", [](MeshSource& mesh) { return mesh.AddTriangle(0, 1, 4, 0).IsOk(); }},
        {"AddAttribute",
         [](MeshSource& mesh) {
             return mesh.AddAttribute(MeshElement::Corner, "Extra", MeshValueType::Vec4, 3).IsOk();
         }},
        {"AddNameAttribute",
         [](MeshSource& mesh) {
             return mesh.AddAttribute(MeshElement::Polygon, "Extra", MeshValueType::Name, 2).IsOk();
         }},
        {"SetNameValue",
         [](MeshSource& mesh) {
             const uint32 tags = mesh.FindAttribute(MeshElement::Corner, "Tags");
             return mesh.SetNameValue(tags, 1, 2, "AMuchLongerNameThanBefore").IsOk();
         }},
        {"SetInfluences",
         [](MeshSource& mesh) {
             const uint32 skin = mesh.FindAttribute(MeshElement::Vertex, "SkinWeights");
             const BoneInfluence three[] = {{1, 1}, {2, 2}, {3, 3}};
             return mesh.SetInfluences(skin, 2, three).IsOk();
         }},
        {"SetSparseValue",
         [](MeshSource& mesh) {
             const uint32 morph = mesh.FindAttribute(MeshElement::Vertex, "Morph-Smile");
             return mesh.SetSparseValue(morph, 0, Vec3(2.0f, 0.0f, 0.0f)).IsOk();
         }},
    };
    for (const Operation& operation : operations) {
        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 256 && !succeeded; ++attempt) {
            test::NFailAllocator failing(base);
            MeshSource mesh(failing);
            REQUIRE(BuildRichMesh(mesh));
            MeshSource expected(base);
            REQUIRE(BuildRichMesh(expected));
            failing.FailOnAttempt(attempt);
            succeeded = operation.run(mesh);
            failing.DisableFailure();
            if (!succeeded) {
                if (!SameMesh(mesh, expected)) {
                    test::RecordFailure(operation.name, __FILE__, __LINE__);
                }

                CHECK(ValidateMeshSource(mesh).IsOk());
            }
        }

        if (!succeeded) {
            test::RecordFailure(operation.name, __FILE__, __LINE__);
        }
    }
}

// 할당 실패 주입(빈 배열에서 시작): 위상 배열·속성 슬롯·속성 목록·영향 저장소가 처음 할당되는
// 지점에서 실패해도 메시는 그대로다. 요소마다 속성 하나씩 단 빈 메시에 차례로 더한다.
TEST(MeshSource_EditFailuresFromEmpty) {
    SystemAllocator base;
    struct Step {
        const char* name;
        bool (*run)(MeshSource& mesh);
    };

    const Step steps[] = {
        {"AddVertex", [](MeshSource& mesh) { return mesh.AddVertex(Vec3(1, 2, 3)).IsOk(); }},
        {"AddCorner", [](MeshSource& mesh) { return mesh.AddCorner(0).IsOk(); }},
        {"AddEdge", [](MeshSource& mesh) { return mesh.AddEdge(0, 1).IsOk(); }},
        {"AddPolygonGroup", [](MeshSource& mesh) { return mesh.AddPolygonGroup().IsOk(); }},
        {"AddPolygon",
         [](MeshSource& mesh) {
             const uint32 corners[] = {0, 0, 0};
             return mesh.AddPolygon(corners, 0).IsOk();
         }},
        {"AddTriangle", [](MeshSource& mesh) { return mesh.AddTriangle(0, 0, 0, 0).IsOk(); }},
        {"SetInfluences",
         [](MeshSource& mesh) {
             const BoneInfluence one[] = {{1, BoneWeightTotal}};
             return mesh
                 .SetInfluences(mesh.FindAttribute(MeshElement::Vertex, "SkinWeights"), 0, one)
                 .IsOk();
         }},
        {"AddAttribute",
         [](MeshSource& mesh) {
             return mesh.AddAttribute(MeshElement::Edge, "Late", MeshValueType::UInt32, 1).IsOk();
         }},
    };
    const MeshElement elements[] = {MeshElement::Vertex,   MeshElement::Corner,
                                    MeshElement::Edge,     MeshElement::Polygon,
                                    MeshElement::Triangle, MeshElement::PolygonGroup};
    for (usize step = 0; step < sizeof(steps) / sizeof(steps[0]); ++step) {
        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 64 && !succeeded; ++attempt) {
            test::NFailAllocator failing(base);
            MeshSource mesh(failing);
            // 요소마다 속성 하나(정점은 영향 속성), 그리고 앞 단계들을 성공시켜 둔다.
            REQUIRE(mesh.AddAttribute(MeshElement::Vertex, "SkinWeights",
                                      MeshValueType::BoneInfluences, 1));
            for (uint32 kind = 1; kind < MeshElementKindCount; ++kind) {
                REQUIRE(mesh.AddAttribute(elements[kind], "Value", MeshValueType::Float, 1));
            }

            for (usize before = 0; before < step; ++before) {
                REQUIRE(steps[before].run(mesh));
            }

            if (step == 2) {
                // 변의 두 번째 정점.
                REQUIRE(mesh.AddVertex(Vec3()));
            }

            const uint32 counts[] = {
                mesh.Count(MeshElement::Vertex),   mesh.Count(MeshElement::Corner),
                mesh.Count(MeshElement::Edge),     mesh.Count(MeshElement::Polygon),
                mesh.Count(MeshElement::Triangle), mesh.AttributeCount()};
            failing.FailOnAttempt(attempt);
            succeeded = steps[step].run(mesh);
            failing.DisableFailure();
            if (!succeeded) {
                const bool same = counts[0] == mesh.Count(MeshElement::Vertex) &&
                                  counts[1] == mesh.Count(MeshElement::Corner) &&
                                  counts[2] == mesh.Count(MeshElement::Edge) &&
                                  counts[3] == mesh.Count(MeshElement::Polygon) &&
                                  counts[4] == mesh.Count(MeshElement::Triangle) &&
                                  counts[5] == mesh.AttributeCount();
                if (!same) {
                    test::RecordFailure(steps[step].name, __FILE__, __LINE__);
                }
            }
        }

        if (!succeeded) {
            test::RecordFailure(steps[step].name, __FILE__, __LINE__);
        }
    }
}

// 속성 목록이 처음 할당되는 지점(빈 메시의 첫 AddAttribute)과 모듈 타입 등록.
TEST(MeshSource_FirstAttributeFailureAndRegistration) {
    SystemAllocator base;
    bool succeeded = false;
    for (uint64 attempt = 1; attempt <= 16 && !succeeded; ++attempt) {
        test::NFailAllocator failing(base);
        MeshSource mesh(failing);
        failing.FailOnAttempt(attempt);
        succeeded = mesh.AddAttribute(MeshElement::Vertex, "A", MeshValueType::Float, 1).IsOk();
        failing.DisableFailure();
        CHECK(mesh.AttributeCount() == (succeeded ? 1u : 0u));
    }

    CHECK(succeeded);
    TypeRegistry types(base);
    ModuleRegistration modules(types, base);
    CHECK(RegisterMeshSourceTypes(modules));
    CHECK(modules.Unregister());
}

} // namespace mesh_source_edit_tests_file
