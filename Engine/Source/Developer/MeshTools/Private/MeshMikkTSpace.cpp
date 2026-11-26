#include "MeshBuildInternal.h"

#include <Core/Assert.h>
#include <DeterministicMath/DeterministicMath.h>
#include <mikktspace.h>

// MikkTSpace 어댑터와 Rocks.patch 훅 정의다(Docs/Design/MeshAnimationAssets 5.2절, 결정
// MA-D5, UE MeshDescriptionMikktSpaceInterface). 훅은 C 연결 이름으로 정의하며 MikkTSpace 정적
// 라이브러리를 이 모듈에 링크해야 풀린다(MikkTSpace를 쓰는 곳은 이 모듈뿐이다).
namespace rocks::mesh_build {

namespace {

// MikkTSpace 내부 배열의 정렬이다(malloc과 같은 최대 기본 정렬 — 원본은 double·구조체 배열을
// 담는다).
constexpr usize MikkAllocationAlignment = 16;

// MikkTSpace 사용자 자료다(m_pUserData). 콜백이 메시·기저를 읽고 쓰고, 할당 훅이 allocator를 쓴다.
struct MikkUserData {
    const MeshSource* mesh = nullptr;
    CornerBasis* basis = nullptr;
    Allocator* allocator = nullptr;
    // 마지막으로 실패한 할당의 오류다(원본은 일부 할당 실패를 느린 경로로 넘긴다 — 그때는 결과가
    // 같다).
    ErrorCode allocationError = ErrorCode::None;
};

MikkUserData& User(const SMikkTSpaceContext* context) noexcept {
    return *static_cast<MikkUserData*>(context->m_pUserData);
}

uint32 CornerOf(const SMikkTSpaceContext* context, int face, int vertex) noexcept {
    return User(context)
        .mesh->TriangleCorners()[static_cast<uint32>(face)]
        .corners[static_cast<uint32>(vertex)];
}

int MikkFaceCount(const SMikkTSpaceContext* context) noexcept {
    return static_cast<int>(User(context).mesh->Count(MeshElement::Triangle));
}

int MikkVerticesOfFace(const SMikkTSpaceContext*, const int) noexcept {
    return 3;
}

void MikkPosition(const SMikkTSpaceContext* context, float out[], const int face,
                  const int vertex) noexcept {
    const MeshSource& mesh = *User(context).mesh;
    const Vec3 position = mesh.Positions()[mesh.CornerVertices()[CornerOf(context, face, vertex)]];
    out[0] = position.x;
    out[1] = position.y;
    out[2] = position.z;
}

void MikkNormal(const SMikkTSpaceContext* context, float out[], const int face,
                const int vertex) noexcept {
    const Vec3 normal = User(context).basis->Normal(CornerOf(context, face, vertex));
    out[0] = normal.x;
    out[1] = normal.y;
    out[2] = normal.z;
}

void MikkTexCoord(const SMikkTSpaceContext* context, float out[], const int face,
                  const int vertex) noexcept {
    const Vec2 uv = User(context).basis->Uv(CornerOf(context, face, vertex));
    out[0] = uv.x;
    out[1] = uv.y;
}

// UE MikkSetTSpaceBasic: tangent를 쓰고 쌍법선 부호는 MikkTSpace 부호의 반대다(UE 좌표계 관례).
void MikkSetBasic(const SMikkTSpaceContext* context, const float tangent[], const float sign,
                  const int face, const int vertex) noexcept {
    const uint32 corner = CornerOf(context, face, vertex);
    CornerBasis& basis = *User(context).basis;
    basis.SetTangent(corner, FiniteOrZero(Vec3(tangent[0], tangent[1], tangent[2])));
    basis.signs[corner] = sign < 0.0f ? 1.0f : -1.0f;
}

} // namespace

Result<void> ComputeMikkTSpaceTangents(const MeshSource& mesh, Allocator& scratch,
                                       CornerBasis& basis) noexcept {
    SMikkTSpaceInterface callbacks{};
    callbacks.m_getNumFaces = MikkFaceCount;
    callbacks.m_getNumVerticesOfFace = MikkVerticesOfFace;
    callbacks.m_getPosition = MikkPosition;
    callbacks.m_getNormal = MikkNormal;
    callbacks.m_getTexCoord = MikkTexCoord;
    callbacks.m_setTSpaceBasic = MikkSetBasic;
    callbacks.m_setTSpace = nullptr;
    MikkUserData user;
    user.mesh = &mesh;
    user.basis = &basis;
    user.allocator = &scratch;
    SMikkTSpaceContext context{};
    context.m_pInterface = &callbacks;
    context.m_pUserData = &user;
    if (genTangSpaceDefault(&context) == 0) {
        // 삼각형이 있고 콜백이 모두 있으므로 실패는 필수 배열 할당 실패뿐이다.
        ROCKS_ASSERT(user.allocationError != ErrorCode::None);
        return Result<void>::Failure(user.allocationError);
    }

    return Result<void>::Success();
}

} // namespace rocks::mesh_build

// Rocks.patch 훅(C 연결): 초월 함수는 Core 결정적 수학, 할당은 사용자 자료의 allocator다.
extern "C" double RocksMikkTSpaceCosPi(double x) noexcept {
    return rocks::deterministic::CosPi(x);
}

extern "C" double RocksMikkTSpaceAcos(double x) noexcept {
    return rocks::deterministic::Acos(x);
}

extern "C" void* RocksMikkTSpaceAllocate(const SMikkTSpaceContext* context, size_t bytes) noexcept {
    auto& user = rocks::mesh_build::User(context);
    auto allocated = user.allocator->Allocate(bytes == 0 ? 1 : bytes,
                                              rocks::mesh_build::MikkAllocationAlignment);
    if (!allocated) {
        user.allocationError = allocated.Error();
        return nullptr;
    }

    return allocated.Value();
}

// 원본은 할당에 성공한 포인터만 넘긴다(null이면 부르지 않는다).
extern "C" void RocksMikkTSpaceFree(const SMikkTSpaceContext* context, void* memory) noexcept {
    (void)rocks::mesh_build::User(context).allocator->Deallocate(memory);
}
