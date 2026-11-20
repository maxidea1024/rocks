#include "MeshToolsTestSupport.h"

#include <mikktspace.h>
#include <stdio.h>

// MikkTSpace 패치(결정적 cos·acos, allocator 훅 — 결정 MA-D5)와 패치 전 원본의 대조다
// (Docs/Design/MeshAnimationAssets.ko.md 12절 "MikkTSpace 참조 구현과의 대조"). 원본 동작은
// ThirdParty MikkTSpaceReference.c(같은 원본을 CRT cos·acos·malloc으로 다시 빌드한 것)가 준다.
// 법선을 원본 속성으로 주고 법선 재계산을 끈 빌드는 MikkTSpace에 같은 위치·법선·UV0을 넘기므로,
// 코너마다 tangent 차이는 acos 결과의 ulp 차이와 출력 정규화뿐이다. 상한
// MikkReferenceTolerance(성분마다 1e-6)를 넘으면 실패하고 쌍법선 부호는 같아야 한다.
extern "C" tbool RocksMikkTSpaceReferenceGenTangSpaceDefault(const SMikkTSpaceContext* context);

using namespace rocks;
namespace static_mesh_builder_mikk_tests_file {

using namespace rocks::mesh_tools_test;

// 패치 전후 tangent 성분 차이의 상한이다.
constexpr float MikkReferenceTolerance = 1.0e-6f;

// 참조 실행의 메시 자료와 코너마다의 결과(마지막 삼각형이 쓴 값 — 빌드 어댑터와 같은 규칙).
struct ReferenceData {
    const MeshSource* mesh = nullptr;
    uint32 normal = MeshNoAttribute;
    uint32 uv = MeshNoAttribute;
    Array<Vec3>* tangents = nullptr;
    Array<float>* signs = nullptr;
};

ReferenceData& Data(const SMikkTSpaceContext* context) noexcept {
    return *static_cast<ReferenceData*>(context->m_pUserData);
}

uint32 ReferenceCorner(const SMikkTSpaceContext* context, int face, int vertex) noexcept {
    return Data(context).mesh->TriangleCorners()[static_cast<uint32>(face)].corners[vertex];
}

int ReferenceFaces(const SMikkTSpaceContext* context) noexcept {
    return static_cast<int>(Data(context).mesh->Count(MeshElement::Triangle));
}

int ReferenceVertices(const SMikkTSpaceContext*, const int) noexcept {
    return 3;
}

void ReferencePosition(const SMikkTSpaceContext* context, float out[], const int face,
                       const int vertex) noexcept {
    const MeshSource& mesh = *Data(context).mesh;
    const Vec3 p = mesh.Positions()[mesh.CornerVertices()[ReferenceCorner(context, face, vertex)]];
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

void ReferenceNormal(const SMikkTSpaceContext* context, float out[], const int face,
                     const int vertex) noexcept {
    const Vec3 n = Data(context).mesh->GetVec3(Data(context).normal, 0,
                                               ReferenceCorner(context, face, vertex));
    out[0] = n.x;
    out[1] = n.y;
    out[2] = n.z;
}

void ReferenceTexCoord(const SMikkTSpaceContext* context, float out[], const int face,
                       const int vertex) noexcept {
    const Vec2 uv =
        Data(context).mesh->GetVec2(Data(context).uv, 0, ReferenceCorner(context, face, vertex));
    out[0] = uv.x;
    out[1] = uv.y;
}

void ReferenceSet(const SMikkTSpaceContext* context, const float tangent[], const float sign,
                  const int face, const int vertex) noexcept {
    const uint32 corner = ReferenceCorner(context, face, vertex);
    (*Data(context).tangents)[corner] = Vec3(tangent[0], tangent[1], tangent[2]);
    (*Data(context).signs)[corner] = sign < 0.0f ? 1.0f : -1.0f;
}

// 원본 MikkTSpace로 코너 tangent·부호를 만든다.
bool RunReference(const MeshSource& mesh, Array<Vec3>& tangents, Array<float>& signs) noexcept {
    if (!tangents.TryResize(mesh.Count(MeshElement::Corner)) ||
        !signs.TryResize(mesh.Count(MeshElement::Corner))) {
        return false;
    }

    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = ReferenceFaces;
    callbacks.m_getNumVerticesOfFace = ReferenceVertices;
    callbacks.m_getPosition = ReferencePosition;
    callbacks.m_getNormal = ReferenceNormal;
    callbacks.m_getTexCoord = ReferenceTexCoord;
    callbacks.m_setTSpaceBasic = ReferenceSet;
    ReferenceData data;
    data.mesh = &mesh;
    data.normal = mesh.FindAttribute(MeshElement::Corner, MeshAttributeName::Normal);
    data.uv = mesh.FindAttribute(MeshElement::Corner, MeshAttributeName::TexCoord);
    data.tangents = &tangents;
    data.signs = &signs;
    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &data;
    return RocksMikkTSpaceReferenceGenTangSpaceDefault(&context) != 0;
}

// 정점 법선을 원본 속성으로 준다: normalOf(정점 위치, 코너)가 돌려준 방향을 정규화해 쓴다.
template <class NormalFn>
bool AssignNormals(MeshSource& mesh, NormalFn&& normalOf) noexcept {
    const uint32 normal = CornerAttribute(mesh, MeshAttributeName::Normal, MeshValueType::Vec3);
    if (normal == MeshNoAttribute) {
        return false;
    }

    for (uint32 corner = 0; corner < mesh.Count(MeshElement::Corner); ++corner) {
        const Vec3 position = mesh.Positions()[mesh.CornerVertices()[corner]];
        const Vec3 direction = normalOf(position, corner);
        const float length = math::Sqrt(direction.x * direction.x + direction.y * direction.y +
                                        direction.z * direction.z);
        mesh.SetVec3(normal, 0, corner, direction / length);
    }

    return true;
}

// 메시 하나를 빌드하고 원본과 대조한다. 단일 묶음·퇴화 없는 메시라 wedge w의 렌더 정점은
// indices[w]다. 돌려주는 값은 최대 성분 차이다(실패하면 음수).
float CompareWithReference(const MeshSource& mesh, const char* label) noexcept {
    SystemAllocator allocator;
    StaticMeshBuildInput input = InputFor(mesh);
    input.settings.recomputeNormals = false;
    StaticMeshBuildResult built(allocator);
    Array<Vec3> tangents(allocator);
    Array<float> signs(allocator);
    if (!BuildStaticMeshLod(input, built) || !RunReference(mesh, tangents, signs) ||
        built.indices.Size() != mesh.Count(MeshElement::Triangle) * 3) {
        return -1.0f;
    }

    float worst = 0.0f;
    uint32 signMismatches = 0;
    for (uint32 wedge = 0; wedge < built.indices.Size(); ++wedge) {
        const uint32 corner = mesh.TriangleCorners()[wedge / 3].corners[wedge % 3];
        const uint32 vertex = built.indices[wedge];
        const Vec3 difference = built.tangents[vertex] - tangents[corner];
        worst = math::Max(worst,
                          math::Max(math::Abs(difference.x),
                                    math::Max(math::Abs(difference.y), math::Abs(difference.z))));
        signMismatches += built.binormalSigns[vertex] == signs[corner] ? 0 : 1;
    }

    printf("MikkTSpace reference %s: %u wedges, worst tangent difference %.3g (bound %.3g), sign "
           "mismatches %u\n",
           label, static_cast<uint32>(built.indices.Size()), static_cast<double>(worst),
           static_cast<double>(MikkReferenceTolerance), signMismatches);
    return signMismatches == 0 ? worst : -1.0f;
}

// 정육면체(면마다 코너, 면 법선), 위도·경도 구(정점 공유 코너, 위치 방향 법선, 이음매·극), UV를
// 거울로 뒤집은 띠(부호 반전), 기울어진 법선의 울퉁불퉁한 격자에서 패치 전후 tangent가 상한 안이고
// 부호가 같다.
TEST(StaticMeshBuild_MikkTSpaceMatchesReference) {
    SystemAllocator allocator;
    {
        MeshSource cube(allocator);
        REQUIRE(BuildCube(cube, 100.0f));
        REQUIRE(AssignNormals(cube, [&](const Vec3&, uint32 corner) noexcept {
            const uint32 face = corner / 4;
            const Vec3 normals[6] = {Vec3(0, 0, -1), Vec3(0, 0, 1), Vec3(0, -1, 0),
                                     Vec3(1, 0, 0),  Vec3(0, 1, 0), Vec3(-1, 0, 0)};
            return normals[face];
        }));
        const float worst = CompareWithReference(cube, "cube");
        CHECK(worst >= 0.0f && worst <= MikkReferenceTolerance);
    }

    {
        MeshSource sphere(allocator);
        REQUIRE(BuildUvSphere(sphere, 50.0f, 16, 8));
        REQUIRE(
            AssignNormals(sphere, [](const Vec3& position, uint32) noexcept { return position; }));
        const float worst = CompareWithReference(sphere, "sphere");
        CHECK(worst >= 0.0f && worst <= MikkReferenceTolerance);
    }

    {
        // 사각형 셋을 이은 띠: 가운데 사각형은 U를 뒤집어(거울) 쌍법선 부호가 바뀐다.
        const Vec3 points[] = {Vec3(0, 0, 0),     Vec3(100, 0, 0),  Vec3(200, 0, 0),
                               Vec3(300, 0, 0),   Vec3(0, 100, 0),  Vec3(100, 100, 0),
                               Vec3(200, 100, 0), Vec3(300, 100, 0)};
        const uint32 q0[] = {0, 1, 5, 4};
        const uint32 q1[] = {1, 2, 6, 5};
        const uint32 q2[] = {2, 3, 7, 6};
        const PolygonSpec polygons[] = {{q0, 0}, {q1, 0}, {q2, 0}};
        MeshSource strip(allocator);
        REQUIRE(BuildPolygonMesh(strip, points, polygons, 1, false));
        const uint32 uv = PlanarUvs(strip, 100.0f);
        REQUIRE(uv != MeshNoAttribute);
        for (uint32 corner = 4; corner < 8; ++corner) {
            const Vec2 value = strip.GetVec2(uv, 0, corner);
            strip.SetVec2(uv, 0, corner, Vec2(3.0f - value.x, value.y));
        }

        REQUIRE(AssignNormals(strip, [](const Vec3&, uint32) noexcept { return Vec3(0, 0, -1); }));
        const float worst = CompareWithReference(strip, "mirrored strip");
        CHECK(worst >= 0.0f && worst <= MikkReferenceTolerance);
    }

    {
        // 9 × 9 정점 격자, 높이와 법선을 정수 패턴으로 흔든다(생성 메시 — 입력 값만 정한다).
        constexpr uint32 Side = 9;
        Vec3 points[Side * Side];
        for (uint32 y = 0; y < Side; ++y) {
            for (uint32 x = 0; x < Side; ++x) {
                const float height = static_cast<float>((x * 7 + y * 13) % 11) * 3.0f;
                points[y * Side + x] =
                    Vec3(static_cast<float>(x) * 25.0f, static_cast<float>(y) * 25.0f, height);
            }
        }

        uint32 quads[(Side - 1) * (Side - 1)][4];
        PolygonSpec polygons[(Side - 1) * (Side - 1)];
        for (uint32 y = 0; y + 1 < Side; ++y) {
            for (uint32 x = 0; x + 1 < Side; ++x) {
                const uint32 cell = y * (Side - 1) + x;
                const uint32 a = y * Side + x;
                quads[cell][0] = a;
                quads[cell][1] = a + 1;
                quads[cell][2] = a + Side + 1;
                quads[cell][3] = a + Side;
                polygons[cell] = {Span<const uint32>(quads[cell], 4), 0};
            }
        }

        MeshSource grid(allocator);
        REQUIRE(BuildPolygonMesh(grid, points, polygons, 1, true));
        REQUIRE(PlanarUvs(grid, 200.0f) != MeshNoAttribute);
        REQUIRE(AssignNormals(grid, [](const Vec3& position, uint32) noexcept {
            const int32 x = static_cast<int32>(position.x) / 25;
            const int32 y = static_cast<int32>(position.y) / 25;
            return Vec3(static_cast<float>(x % 3 - 1) * 0.2f, static_cast<float>(y % 5 - 2) * 0.1f,
                        -1.0f);
        }));
        const float worst = CompareWithReference(grid, "bumpy grid");
        CHECK(worst >= 0.0f && worst <= MikkReferenceTolerance);
    }
}

} // namespace static_mesh_builder_mikk_tests_file
