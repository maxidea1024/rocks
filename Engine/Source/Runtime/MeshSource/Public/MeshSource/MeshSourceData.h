#pragma once

#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/Name.h>
#include <Meta/Annotations.h>

// 원본 메시의 영속 형태다(Docs/Design/MeshAnimationAssets.ko.md 3.4절). 요소 수·위상 blob·속성
// 레코드 목록만 리플렉션 필드로 두고, 위상 배열과 속성 값은 Bulk entry에 둔다(속성 하나 = Bulk
// entry 하나, 결정 MA-D2). 편집·빌드는 메모리 형태 MeshSource(MeshSource/MeshSource.h)로 하고 둘
// 사이 변환은 MeshSource::Load·Store가 한다.
namespace rocks {

// 원본 메시 요소의 종류다. 번호는 저장 값이라 바꾸지 않는다(3.4절). UE 대응: Vertex /
// VertexInstance (Rocks의 코너) / Edge / Polygon / Triangle / PolygonGroup.
ROCKS_ENUM()
enum class MeshElement : uint8 {
    Invalid = 0,
    Vertex = 1,
    Corner = 2,
    Edge = 3,
    Polygon = 4,
    Triangle = 5,
    PolygonGroup = 6
};

// 속성 값 타입이다. 번호는 저장 값이라 바꾸지 않는다. bulk 배치는 3.4.1절이다(Float·Vec*는 float32
// 성분, UInt32는 uint32, Bool은 byte 0·1, Name은 uint32 길이 + NFC UTF-8, BoneInfluences는 시작
// 위치 표 + 영향 배열, SparseVec3는 (번호, Vec3) 쌍).
ROCKS_ENUM()
enum class MeshValueType : uint8 {
    Invalid = 0,
    Float = 1,
    Vec2 = 2,
    Vec3 = 3,
    Vec4 = 4,
    UInt32 = 5,
    Bool = 6,
    Name = 7,
    BoneInfluences = 8,
    SparseVec3 = 9
};

// 위상·값 배치의 버전이다(3.4.2절). 다른 버전은 읽지 않는다(초기 단계 원칙 — 하위호환 읽기 없음).
inline constexpr uint32 MeshSourceFormatVersion = 1;

// 속성 하나의 저장 레코드다. (element, name)이 메시 안에서 유일하고(Name 비교 규칙 — 대소문자만
// 다른 두 이름은 같다), data는 값 타입별 배치(3.4.1절)의 bulk다.
ROCKS_STRUCT()
struct MeshAttributeRecord {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;
    ROCKS_PROPERTY() MeshElement element = MeshElement::Invalid;
    ROCKS_PROPERTY() MeshValueType type = MeshValueType::Invalid;
    ROCKS_PROPERTY() uint8 channels = 1;
    ROCKS_PROPERTY() BulkDataRef data;
};

// 원본 메시 하나의 영속 형태다. 정적·스킨 메시의 LOD 원본이 이 값을 필드로 갖는다. 요소 수 0인
// 원본은 정상적인 빈 메시이고 "원본 없음"은 LOD 쪽 플래그로 나타낸다(3.4.2절). 위치는 정점 속성
// `Position` 레코드다(3.7절 1). attributes는 (element, name의 UTF-8 byte) 엄격 증가 순이다(9.2절).
//
// 사용 예(저장과 로드):
//   MemoryBulkSource bulk(allocator);
//   auto stored = mesh.Store(bulk, names, asset->source);        // 필드 asset->source를 채운다
//   ...
//   MeshSource loaded(allocator);
//   auto read = loaded.Load(asset->source, *package, names);    // 로드한 패키지의 bulk로 읽는다
ROCKS_STRUCT()
struct MeshSourceData {
    ROCKS_GENERATED_BODY()

    explicit MeshSourceData(Allocator& allocator) noexcept : attributes(allocator) {
    }

    ROCKS_PROPERTY() uint32 formatVersion = MeshSourceFormatVersion;
    ROCKS_PROPERTY() uint32 vertexCount = 0;
    ROCKS_PROPERTY() uint32 cornerCount = 0;
    ROCKS_PROPERTY() uint32 edgeCount = 0;
    ROCKS_PROPERTY() uint32 polygonCount = 0;
    ROCKS_PROPERTY() uint32 triangleCount = 0;
    ROCKS_PROPERTY() uint32 polygonGroupCount = 0;
    // 위상 배열들이다(9.1절 고정 배치).
    ROCKS_PROPERTY() BulkDataRef topology;
    ROCKS_PROPERTY() Array<MeshAttributeRecord> attributes;
};

} // namespace rocks
