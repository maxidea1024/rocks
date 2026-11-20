#pragma once

#include <MeshSource/MeshSource.h>

// MeshSource 모듈 안에서만 쓰는 판정·접근 도우미다(유니티 빌드에서 소스끼리 겹치지 않게 모두 이
// namespace에 두고 다른 파일에서는 완전 한정으로 부른다).
namespace rocks::mesh_source_detail {

// 엔진 이름 속성의 종류다(3.2절 표, 3.7절 5). Malformed는 접두만 있고 뒤가 빈 이름(`Morph-`)이다.
enum class EngineName : uint8 {
    None,
    Position,
    Normal,
    Tangent,
    BinormalSign,
    Color,
    TexCoord,
    IsHard,
    PartName,
    MaterialSlotName,
    SkinWeights,
    Morph,
    MorphNormal,
    Malformed
};

// 엔진 이름 속성이 가져야 하는 형태다(채널은 1–maxChannels — 속성의 채널은 늘 1 이상이다).
struct EngineRule {
    MeshElement element = MeshElement::Invalid;
    MeshValueType type = MeshValueType::Invalid;
    uint8 maxChannels = 1;
};

// name이 어느 엔진 이름인지다(Name 비교 규칙, 접두는 ASCII 대소문자 무시).
EngineName ClassifyAttributeName(StringView name) noexcept;

// kind의 형태다(None·Malformed는 빈 규칙 — element Invalid).
EngineRule EngineRuleFor(EngineName kind) noexcept;

// element·type이 정의된 값인지다(Invalid 제외).
bool IsValidElement(MeshElement element) noexcept;
bool IsValidValueType(MeshValueType type) noexcept;

// Float·Vec2·Vec3·Vec4의 float 성분 수다. 그 밖은 0이다.
uint32 FloatComponents(MeshValueType type) noexcept;

// 채널을 하나만 갖는 값 타입(가변 길이 BoneInfluences·SparseVec3)인지다.
bool IsSingleChannelType(MeshValueType type) noexcept;

// pointer가 [begin, begin + bytes) 안을 가리키는지다(값 쓰기의 별칭 판정 — 주소 차를 부호 없는 수로
// 한 번 비교한다. begin이 null이면 bytes도 0이라 거짓이다).
inline bool PointsInto(const void* pointer, const void* begin, usize bytes) noexcept {
    return reinterpret_cast<uptr>(pointer) - reinterpret_cast<uptr>(begin) < bytes;
}

// 세 성분이 모두 비트 단위 +0.0인지다(희소 저장의 "0" — -0.0은 0이 아니다).
bool IsPositiveZero(const Vec3& value) noexcept;

// failure를 채우고 error로 실패를 돌려준다.
Result<void> Fail(MeshDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                  MeshIssue issue, MeshElement element = MeshElement::Invalid,
                  uint32 attribute = MeshNoAttribute, uint32 index = MeshNoIndex) noexcept;

} // namespace rocks::mesh_source_detail

namespace rocks {

// MeshSource의 비공개 상태를 Load가 직접 채우는 통로다(배치 판정이 끝난 배열을 옮겨 담는다).
struct MeshSourceAccess {
    static uint32& CountRef(MeshSource& mesh, MeshElement element) noexcept;

    static Array<Vec3>& Positions(MeshSource& mesh) noexcept {
        return mesh.positions_;
    }

    static Array<uint32>& CornerVertices(MeshSource& mesh) noexcept {
        return mesh.cornerVertices_;
    }

    static Array<MeshEdgeVertices>& Edges(MeshSource& mesh) noexcept {
        return mesh.edges_;
    }

    static Array<uint32>& PolygonEnds(MeshSource& mesh) noexcept {
        return mesh.polygonEnds_;
    }

    static Array<uint32>& PolygonCorners(MeshSource& mesh) noexcept {
        return mesh.polygonCorners_;
    }

    static Array<uint32>& PolygonGroups(MeshSource& mesh) noexcept {
        return mesh.polygonGroups_;
    }

    static Array<MeshTriangleCorners>& TriangleCorners(MeshSource& mesh) noexcept {
        return mesh.triangleCorners_;
    }

    static Array<uint32>& TrianglePolygons(MeshSource& mesh) noexcept {
        return mesh.trianglePolygons_;
    }

    static Array<MeshSource::Attribute>& Attributes(MeshSource& mesh) noexcept {
        return mesh.attributes_;
    }
};

} // namespace rocks
