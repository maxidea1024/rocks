#pragma once

#include <AssetRuntime/MemoryBulkSource.h>
#include <AssetRuntime/Package.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Vector2.h>
#include <Core/Math/Vector3.h>
#include <Core/Math/Vector4.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/String.h>
#include <MeshSource/MeshSourceData.h>
#include <MeshSource/MeshSourceTypes.h>
#include <Object/ModuleRegistration.h>

namespace rocks {

// 변 하나의 두 정점이다. 저장 형태는 작은 번호 먼저다(first < second — ValidateMeshSource가 본다).
struct MeshEdgeVertices {
    uint32 first = 0;
    uint32 second = 0;
};

// 삼각형 하나의 세 코너다(winding은 원본 순서 그대로 — 좌표·winding은 UE와 같다).
struct MeshTriangleCorners {
    uint32 corners[3] = {};
};

// 속성 하나의 정의다. name은 MeshSource가 가진 원문을 빌린 view이며 속성을 더하거나 MeshSource를
// 옮기면 무효가 된다.
struct MeshAttributeInfo {
    MeshElement element = MeshElement::Invalid;
    MeshValueType type = MeshValueType::Invalid;
    uint8 channels = 0;
    StringView name{};
};

// 원본 메시의 편집·빌드용 메모리 형태다(Docs/Design/MeshAnimationAssets.ko.md 3절, UE
// FMeshDescription 대응). 리플렉션 타입이 아니며 영속 형태 MeshSourceData와 Load·Store로 오간다.
//
// 모델: 정점(위치)·코너(정점 번호)·변(정점 쌍)·다각형(둘레 코너 목록 + 묶음 번호)·삼각형(코너 셋
// + 다각형 번호)·다각형 묶음의 여섯 요소와, 요소마다 이름 있는 속성(값 타입·채널 수)을 갖는다.
// 이웃한 면이 같은 코너를 공유할 수 있고, 퇴화 삼각형도 원본에 남는다(3.1절).
//
// 상태 규칙: 이 객체는 배열 모양(요소 수와 각 배열 길이, 속성 값 배열 길이, 영향·희소 저장의
// 범위)을 늘 맞게 유지한다. 내용 불변식(번호 범위·변·다각형·삼각형 관계·위치 유한·속성 이름·엔진
// 속성 형태·값)은 Add*·Set*가 보지 않으며 ValidateMeshSource(MeshSource/MeshSourceValidation.h)가
// 본다. 빌드·위상 조회는 검증을 통과한 메시만 받는다.
//
// 사용 예(사각형 하나 — 코너 넷, 다각형 하나, 삼각형 둘):
//   MeshSource mesh(allocator);
//   const uint32 group = mesh.AddPolygonGroup().Value();
//   uint32 corners[4];
//   for (uint32 i = 0; i < 4; ++i) {
//       corners[i] = mesh.AddCorner(mesh.AddVertex(points[i]).Value()).Value();
//   }
//   for (uint32 i = 0; i < 4; ++i) {
//       mesh.AddEdge(i, (i + 1) % 4);
//   }
//   const uint32 polygon = mesh.AddPolygon(corners, group).Value();
//   mesh.AddTriangle(corners[0], corners[1], corners[2], polygon);
//   mesh.AddTriangle(corners[0], corners[2], corners[3], polygon);
//   const uint32 uv = mesh.AddAttribute(MeshElement::Corner, MeshAttributeName::TexCoord,
//                                       MeshValueType::Vec2, 1).Value();
//   mesh.SetVec2(uv, 0, corners[1], Vec2(1.0f, 0.0f));
//
// 오류: 모든 Try 성격 함수(Add*·SetNameValue·SetInfluences·SetSparseValue·Load·Store)는 실패하면
// 이 객체를 호출 전 상태로 둔다(강한 보장). 인자의 번호·타입이 계약과 다른 호출(없는 속성 번호,
// 값 타입과 다른 접근 함수, 범위 밖 요소·채널)은 프로그래밍 오류이며 ROCKS_ASSERT로 멈춘다.
//
// 수명: allocator는 이 객체보다 오래 살아야 한다. 반환 span·view는 같은 종류의 배열을 늘리는 호출
// (Add*·AddAttribute·Set*·Load)까지 유효하다. 이동 전용이다.
// 스레드: 동기화하지 않는다. 한 스레드가 고치는 동안 다른 스레드가 읽지 않는다(읽기끼리는 된다).
class MeshSource {
public:
    explicit MeshSource(Allocator& allocator) noexcept;

    MeshSource(MeshSource&&) noexcept = default;
    MeshSource& operator=(MeshSource&&) noexcept = default;
    MeshSource(const MeshSource&) = delete;
    MeshSource& operator=(const MeshSource&) = delete;

    Allocator& GetAllocator() const noexcept {
        return *allocator_;
    }

    // element 요소의 수다. Invalid는 0이다.
    uint32 Count(MeshElement element) const noexcept;

    // 요소를 끝에 더하고 그 번호를 돌려준다. 같은 요소의 모든 속성 값도 기본값(3.7절 8 — 0, 엔진
    // Color는 (1, 1, 1, 1), Name은 빈 문자열, 영향은 없음)으로 한 칸 늘어난다. 번호의 범위·관계는
    // 보지 않는다(ValidateMeshSource가 본다).
    //
    // 오류: 그 요소 수가 이미 MeshMaxElements(다각형 묶음은 MeshMaxPolygonGroups)이면
    // CapacityExceeded, 할당 실패는 allocator 오류다.
    Result<uint32> AddVertex(const Vec3& position) noexcept;
    Result<uint32> AddCorner(uint32 vertex) noexcept;
    // 두 정점을 작은 번호 먼저로 담는다(같은 정점이면 그대로 — 검증이 거부한다).
    Result<uint32> AddEdge(uint32 vertexA, uint32 vertexB) noexcept;
    Result<uint32> AddPolygonGroup() noexcept;
    // corners는 둘레 순서의 코너 번호다. 둘레 전체 길이도 MeshMaxElements 이하여야 한다
    // (넘으면 CapacityExceeded). corners가 이 메시의 배열(PolygonCorners 등)을 가리키면 안 된다
    // (늘리는 재할당이 그 byte를 옮긴다 — 지역 배열에 복사해 넘긴다).
    Result<uint32> AddPolygon(Span<const uint32> corners, uint32 group) noexcept;
    Result<uint32> AddTriangle(uint32 corner0, uint32 corner1, uint32 corner2,
                               uint32 polygon) noexcept;

    // 위상 배열이다(9.1절 순서와 같은 뜻). 다각형 p의 둘레는 PolygonCornerList()의
    // [p == 0 ? 0 : ends[p − 1], ends[p])이다(ends = PolygonCornerEnds() — 다각형마다 끝 위치, 저장
    // 배치의 시작 표에서 첫 값 0을 뺀 것).
    Span<const Vec3> Positions() const noexcept {
        return positions_.View();
    }

    // 위치는 제자리에서 고쳐도 된다(모양이 바뀌지 않는다).
    Span<Vec3> Positions() noexcept {
        return positions_.View();
    }

    Span<const uint32> CornerVertices() const noexcept {
        return cornerVertices_.View();
    }

    Span<const MeshEdgeVertices> Edges() const noexcept {
        return edges_.View();
    }

    Span<const uint32> PolygonCornerEnds() const noexcept {
        return polygonEnds_.View();
    }

    Span<const uint32> PolygonCornerList() const noexcept {
        return polygonCorners_.View();
    }

    Span<const uint32> PolygonGroups() const noexcept {
        return polygonGroups_.View();
    }

    Span<const MeshTriangleCorners> TriangleCorners() const noexcept {
        return triangleCorners_.View();
    }

    Span<const uint32> TrianglePolygons() const noexcept {
        return trianglePolygons_.View();
    }

    // 다각형 polygon의 둘레 코너다(polygon < 다각형 수).
    Span<const uint32> PolygonCorners(uint32 polygon) const noexcept;

    // 속성 수와 정의다(번호는 0..AttributeCount() − 1, 더한 순서 — Load 뒤는 9.2절 정렬 순서).
    // 정점 위치는 속성 목록에 없다(Positions()).
    uint32 AttributeCount() const noexcept {
        return static_cast<uint32>(attributes_.Size());
    }

    MeshAttributeInfo AttributeInfo(uint32 attribute) const noexcept;

    // (element, name)인 속성의 번호다(Name 비교 규칙 — ASCII 대소문자 무시). 없으면
    // MeshNoAttribute다.
    uint32 FindAttribute(MeshElement element, StringView name) const noexcept;

    // 속성을 더하고 번호를 돌려준다. 값은 지금 요소 수만큼 기본값으로 채운다. 엔진 이름 속성의
    // 형태(3.2절 표)는 보지 않는다(ValidateMeshSource가 본다).
    //
    // 오류(InvalidArgument): element·type이 Invalid이거나 범위 밖, channels가 0, BoneInfluences·
    // SparseVec3인데 channels가 1이 아님, name이 None이거나 Name 내용 규칙(ValidateNameFieldText)
    // 위반. 같은 (element, name)이 있거나 정점 `Position`(위치가 쓰는 이름)이면 AlreadyExists, 할당
    // 실패는 allocator 오류다.
    Result<uint32> AddAttribute(MeshElement element, StringView name, MeshValueType type,
                                uint8 channels) noexcept;

    // Float·Vec2·Vec3·Vec4 속성 channel의 성분 배열이다(요소 수 × 성분 수, 요소마다 성분이
    // 이어진다). 제자리에서 고쳐도 된다.
    Span<float> FloatValues(uint32 attribute, uint32 channel) noexcept;
    Span<const float> FloatValues(uint32 attribute, uint32 channel) const noexcept;

    // UInt32 속성 channel의 값 배열이다.
    Span<uint32> UInt32Values(uint32 attribute, uint32 channel) noexcept;
    Span<const uint32> UInt32Values(uint32 attribute, uint32 channel) const noexcept;

    // Bool 속성 channel의 값 배열이다(0·1 — 다른 값은 검증이 거부한다).
    Span<uint8> BoolValues(uint32 attribute, uint32 channel) noexcept;
    Span<const uint8> BoolValues(uint32 attribute, uint32 channel) const noexcept;

    // 요소 index 하나의 벡터 값 읽기·쓰기다(속성 값 타입이 그 벡터여야 한다).
    float GetFloat(uint32 attribute, uint32 channel, uint32 index) const noexcept;
    void SetFloat(uint32 attribute, uint32 channel, uint32 index, float value) noexcept;
    Vec2 GetVec2(uint32 attribute, uint32 channel, uint32 index) const noexcept;
    void SetVec2(uint32 attribute, uint32 channel, uint32 index, const Vec2& value) noexcept;
    Vec3 GetVec3(uint32 attribute, uint32 channel, uint32 index) const noexcept;
    void SetVec3(uint32 attribute, uint32 channel, uint32 index, const Vec3& value) noexcept;
    Vec4 GetVec4(uint32 attribute, uint32 channel, uint32 index) const noexcept;
    void SetVec4(uint32 attribute, uint32 channel, uint32 index, const Vec4& value) noexcept;

    // Name 속성 값이다(빈 문자열 = None). 반환 view는 같은 속성의 다음 SetNameValue까지 유효하다.
    StringView NameValue(uint32 attribute, uint32 channel, uint32 index) const noexcept;

    // Name 속성 값을 쓴다. 내용 규칙(UTF-8·NFC·길이)은 검증이 본다(3.7절 9 — 더 길면 저장소 끝에
    // 덧붙인다).
    //
    // 오류: 그 채널 저장소가 uint32 byte를 넘으면 CapacityExceeded, 할당 실패는 allocator 오류다.
    Result<void> SetNameValue(uint32 attribute, uint32 channel, uint32 index,
                              StringView value) noexcept;

    // BoneInfluences 속성의 정점 vertex 영향이다(저장 순서 그대로).
    Span<const BoneInfluence> Influences(uint32 attribute, uint32 vertex) const noexcept;

    // 정점 vertex의 영향을 바꾼다. 개수·boneIndex·가중치 합은 검증·값 정리가 본다(개수 상한을
    // 넘는 값도 담는다 — ValidateMeshSource가 TooManyInfluences로 거부한다).
    //
    // 오류: 저장소가 uint32 원소를 넘으면 CapacityExceeded, 할당 실패는 allocator 오류다.
    Result<void> SetInfluences(uint32 attribute, uint32 vertex,
                               Span<const BoneInfluence> influences) noexcept;

    // SparseVec3 속성의 0이 아닌 항목이다(index 엄격 증가).
    Span<const MeshSparseVec3> SparseValues(uint32 attribute) const noexcept;

    // 요소 index의 희소 값이다(항목이 없으면 +0 벡터).
    Vec3 SparseValue(uint32 attribute, uint32 index) const noexcept;

    // 요소 index의 희소 값을 쓴다. 세 성분이 모두 비트 단위 +0.0이면 항목을 지우고, 그 밖(-0.0
    // 포함)은 항목으로 담는다(3.2절 — 손실 없음). index는 그 요소 수 미만이어야 한다.
    //
    // 오류: 할당 실패는 allocator 오류다.
    Result<void> SetSparseValue(uint32 attribute, uint32 index, const Vec3& value) noexcept;

    // data와 package의 bulk로 이 객체를 다시 만든다(3.4절). 3.7절 4의 배치 판정 뒤
    // ValidateMeshSource(본 수 모름)를 부른다. 성공하면 기존 내용을 바꾸고 속성 순서는 레코드
    // 순서다.
    //
    // 오류: 배치·내용 위반은 failure->code CorruptPackage(formatVersion만 UnsupportedVersion)와
    // ErrorCode InvalidArgument(버전은 Unsupported, bulk를 못 읽으면 Package::BulkBytes의 오류)이고
    // 세부 규칙은 failure->issue다. 레코드 이름 pool 오류(ShuttingDown·StaleHandle 등)와 할당
    // 실패는 진단 코드 없이(None) 그 오류다. 실패하면 이 객체는 그대로다.
    //
    // 인자: names는 data를 담은 Registry의 Name pool(RegistryOptions::names)이다.
    // 스레드: package를 읽으므로 그 Registry 소유 스레드에서 부른다.
    Result<void> Load(const MeshSourceData& data, const Package& package, NamePool& names,
                      MeshDiagnostic* failure = nullptr) noexcept;

    // 이 객체를 data(필드 전체를 덮어쓴다)와 bulk entry로 쓴다(3.4절, 9.1절 위상 배치, 9.2절 속성
    // 정렬, 가변 길이 값 압축). 먼저 ValidateMeshSource(본 수 모름)로 검증한다. 같은 내용이면 같은
    // byte다(결정적). 위상 entry 하나와 레코드마다 entry 하나를 bulk에 더한다.
    //
    // 오류: 검증 실패는 failure의 진단(InvalidMeshData·TooManyInfluences)과 그 오류다. 이름 intern
    // 실패(pool 상한·종료)·할당 실패는 진단 코드 없이 그 오류다. 실패하면 data는 그대로이고
    // bulk에는 이번 호출이 더한 entry가 남을 수 있다(참조하는 필드가 없어 저장되지 않는다).
    //
    // 인자: names는 data를 담을 Registry의 Name pool이다.
    Result<void> Store(MemoryBulkSource& bulk, NamePool& names, MeshSourceData& data,
                       MeshDiagnostic* failure = nullptr) const noexcept;

    // 값 저장소 하나다(구현 세부 — 속성 채널 하나). 공개 API가 아니다.
    struct ValueRange {
        uint32 offset = 0;
        uint32 count = 0;
    };

    struct Channel {
        explicit Channel(Allocator& allocator) noexcept
            : floats(allocator), words(allocator), bytes(allocator), ranges(allocator),
              influences(allocator), sparse(allocator) {
        }

        // Float·Vec2·Vec3·Vec4 성분이다(요소 수 × 성분 수).
        Array<float> floats;
        // UInt32 값이다.
        Array<uint32> words;
        // Bool 값이거나 Name 원문 저장소다.
        Array<uint8> bytes;
        // Name·BoneInfluences 요소별 범위다(bytes·influences 안).
        Array<ValueRange> ranges;
        // BoneInfluences 저장소다.
        Array<BoneInfluence> influences;
        // SparseVec3 항목이다(index 엄격 증가).
        Array<MeshSparseVec3> sparse;
    };

    struct Attribute {
        explicit Attribute(Allocator& allocator) noexcept : name(allocator), channels(allocator) {
        }

        String name;
        MeshElement element = MeshElement::Invalid;
        MeshValueType type = MeshValueType::Invalid;
        bool engineColor = false;
        Array<Channel> channels;
    };

private:
    friend struct MeshSourceAccess;

    // element 요소를 하나 더할 자리를 그 요소의 모든 속성 채널에 잡는다(실패하면 아무것도 늘지
    // 않은 것과 같다 — 늘어난 용량은 내용이 아니다).
    Result<void> ReserveAttributeSlot(MeshElement element) noexcept;
    // 잡아 둔 자리에 element 요소 하나의 기본값을 넣는다(실패하지 않는다).
    void AppendAttributeDefaults(MeshElement element) noexcept;

    const Attribute& CheckedAttribute(uint32 attribute, MeshValueType type) const noexcept;
    const Channel& CheckedChannel(uint32 attribute, uint32 channel,
                                  MeshValueType type) const noexcept;
    Channel& MutableChannel(uint32 attribute, uint32 channel, MeshValueType type) noexcept;

    Allocator* allocator_;
    uint32 vertexCount_ = 0;
    uint32 cornerCount_ = 0;
    uint32 edgeCount_ = 0;
    uint32 polygonCount_ = 0;
    uint32 triangleCount_ = 0;
    uint32 polygonGroupCount_ = 0;
    Array<Vec3> positions_;
    Array<uint32> cornerVertices_;
    Array<MeshEdgeVertices> edges_;
    Array<uint32> polygonEnds_;
    Array<uint32> polygonCorners_;
    Array<uint32> polygonGroups_;
    Array<MeshTriangleCorners> triangleCorners_;
    Array<uint32> trianglePolygons_;
    Array<Attribute> attributes_;
};

// MeshSource 리플렉션 타입(MeshElement·MeshValueType·MeshAttributeRecord·MeshSourceData)을 등록한다
// (생성 집계 함수 generated::MeshSource::RegisterModuleTypes를 부른다 — 의존 모듈 AssetRuntime
// 타입도 함께). 도구·시험은 직접, 엔진은 메시 에셋 모듈이 부른다.
Result<void> RegisterMeshSourceTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
