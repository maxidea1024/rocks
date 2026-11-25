#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Base/NumericLimits.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/StringView.h>
#include <MeshSource/MeshSourceData.h>

// 원본 메시의 상수·값 타입·진단이다(Docs/Design/MeshAnimationAssets 3절·11절).
namespace rocks {

// 다각형 둘레의 최소 코너 수다(3.1절 — 삼각형 미만은 면이 아니다).
inline constexpr uint32 MeshMinPolygonCorners = 3;

// 요소 종류마다의 최대 개수다(2^31 − 1, 3.1절). 번호 계산(합·차·+1)이 uint32에서 넘치지 않게 int32
// 최댓값으로 둔다.
inline constexpr uint32 MeshMaxElements = static_cast<uint32>(NumericLimits<int32>::Max());

// 다각형 묶음 수의 상한이다(3.1절·3.7절 12). 묶음은 머티리얼 섹션 하나라 다른 요소처럼 데이터(위상·
// 위치 bulk)에 묶인 개수가 아니다 — 속성 없는 묶음 수만 크게 쓴 원본이 빌드에서 묶음·섹션·위상 표를
// 그 수만큼 만들지 않게 이름 있는 상한으로 묶는다. 65535는 섹션·슬롯 번호를 16비트로 담는 값이다(UE
// 고전 정적 메시에는 섹션 수 상한이 없고 Nanite는 64다 — NaniteBuilder.h MaxSectionArraySize).
inline constexpr uint32 MeshMaxPolygonGroups = 65535;

// 코너 TexCoord 속성의 최대 채널 수다(UE MAX_MESH_TEXTURE_COORDS_MD와 같다, 3.2절).
inline constexpr uint32 MeshMaxTexCoords = 8;

// UV 성분 절댓값이 이 값을 넘으면 UvOutOfRange 경고다(값은 바꾸지 않는다, 3.3절).
inline constexpr float MeshUvRangeWarning = 1024.0f;

// 정점 영향 가중치의 정규화 합이다(UE FBoneWeight의 16비트 정밀도와 같다 — 가중치 1.0 = 65535).
inline constexpr uint16 BoneWeightTotal = NumericLimits<uint16>::Max();

// 원본 정점 하나의 최대 영향 수다(UE MAX_TOTAL_INFLUENCES와 같다, 3.2절). 쿠킹 상한은
// 따로다(6.3절).
inline constexpr uint32 MeshMaxSourceInfluences = 12;

// 요소 종류 수다(MeshElement에서 Invalid를 뺀 Vertex–PolygonGroup).
inline constexpr uint32 MeshElementKindCount = 6;

// "속성 없음" 표지다(FindAttribute가 찾지 못함, 진단이 속성을 가리키지 않음).
inline constexpr uint32 MeshNoAttribute = NumericLimits<uint32>::Max();

// "요소 번호 없음" 표지다(MeshTopologyIndex::FindEdge가 찾지 못함, 진단이 요소를 가리키지 않음).
inline constexpr uint32 MeshNoIndex = NumericLimits<uint32>::Max();

// "본 수 모름" 표지다(MeshValidateOptions::boneCount 기본값 — boneIndex 범위를 보지 않는다).
inline constexpr uint32 MeshUnknownBoneCount = NumericLimits<uint32>::Max();

// 엔진이 이름을 정한 속성이다(3.2절 표, 3.7절 1·5). 이름 비교는 Name 규칙(ASCII 대소문자 무시)이고
// 접두는 ASCII 대소문자를 무시하고 맞춘다. 접두 이름은 접두 뒤가 비어 있으면 안 된다.
struct MeshAttributeName {
    // 정점 위치(Vec3, 1채널, 필수). MeshSource는 Positions()로 따로 갖는다.
    static constexpr StringView Position{"Position"};
    // 코너 법선·tangent(Vec3, 1채널), binormal 부호(Float, 1채널), 선형 색(Vec4, 1채널).
    static constexpr StringView Normal{"Normal"};
    static constexpr StringView Tangent{"Tangent"};
    static constexpr StringView BinormalSign{"BinormalSign"};
    static constexpr StringView Color{"Color"};
    // 코너 UV(Vec2, 1–MeshMaxTexCoords 채널).
    static constexpr StringView TexCoord{"TexCoord"};
    // 변 hard edge(Bool, 1채널).
    static constexpr StringView IsHard{"IsHard"};
    // 다각형 부분 이름(Name, 1채널).
    static constexpr StringView PartName{"PartName"};
    // 다각형 묶음의 머티리얼 슬롯 키(Name, 1채널).
    static constexpr StringView MaterialSlotName{"MaterialSlotName"};
    // 정점 스킨 가중치(BoneInfluences, 1채널). 프로필은 `SkinWeights-<profile>`이다.
    static constexpr StringView SkinWeights{"SkinWeights"};
    static constexpr StringView SkinWeightsPrefix{"SkinWeights-"};
    // 정점 morph 위치 변화량(SparseVec3, 1채널) `Morph-<이름>`, 코너 morph 법선 변화량
    // `MorphNormal-<이름>`.
    static constexpr StringView MorphPrefix{"Morph-"};
    static constexpr StringView MorphNormalPrefix{"MorphNormal-"};
};

// 정점 영향 하나다(UE FBoneWeight 대응, 3.2절). boneIndex는 메시 본 계층의 원본 번호, weight는
// 0–BoneWeightTotal 정규화 값이다.
struct BoneInfluence {
    uint16 boneIndex = 0;
    uint16 weight = 0;

    friend constexpr bool operator==(const BoneInfluence&, const BoneInfluence&) noexcept = default;
};

// 희소 Vec3 속성(morph)의 항목 하나다. index는 그 요소 번호다.
struct MeshSparseVec3 {
    uint32 index = 0;
    Vec3 value{};
};

// 진단이 가리키는 세부 규칙이다(3.7절 10). 하나의 AssetDiagnosticCode(예: InvalidMeshData) 아래
// 어느 규칙을 어겼는지 가른다. 값은 저장 형식이 아니다(순서를 바꿔도 된다).
enum class MeshIssue : uint8 {
    None,
    // 로드 배치 판정(MeshSource::Load — 모두 CorruptPackage).
    FormatVersion,        // formatVersion이 MeshSourceFormatVersion이 아님(UnsupportedVersion)
    ElementCountTooLarge, // 요소 수 > MeshMaxElements(다각형 묶음은 MeshMaxPolygonGroups)
    BulkUnreadable,       // 빈 ref·패키지에 없는 bulkId·크기·SHA-256 불일치
    TopologyLength,       // 위상 blob 길이가 9.1절 배치와 다름
    PolygonStarts,        // 다각형 둘레 시작 표가 0에서 시작하지 않거나 줄어듦
    AttributeRecordKind,  // 레코드 element·값 타입·채널 수가 범위 밖
    AttributeRecordName,  // 레코드 이름을 읽을 수 없음(None·내용 규칙 위반)
    AttributeOrder,       // 레코드가 (element, name byte) 엄격 증가 순이 아님
    MissingPosition,      // 정점 Position 레코드가 없거나 형태가 다름
    AttributeLength,      // 값 bulk 길이가 3.4.1절 배치와 다름
    NameValueLength,      // Name 값 길이가 MaxNameFieldBytes를 넘거나 bulk를 넘음
    InfluenceStarts,      // 영향 시작 표가 0에서 시작하지 않거나 줄어들거나 차가 상한을 넘음
    SparseIndexOrder,     // 희소 번호가 엄격 증가가 아님
    SparseIndexRange,     // 희소 번호 >= 요소 수
    SparseZeroEntry,      // 희소 항목 값이 비트 단위 +0.0 셋
    // 내용 판정(ValidateMeshSource — InvalidMeshData, 영향 개수만 TooManyInfluences).
    NonFinitePosition,
    CornerVertexRange,
    EdgeVertexRange,
    EdgeSameVertex,
    EdgeOrder, // 변의 정점 쌍이 작은 번호 먼저가 아님
    DuplicateEdge,
    PolygonTooFewCorners,
    PolygonCornerRange,
    PolygonGroupRange,
    PerimeterEdgeMissing, // 둘레를 잇는 변이 변 목록에 없음
    TriangleCornerRange,
    TrianglePolygonRange,
    TriangleCountMismatch, // 다각형의 삼각형 수 != 둘레 코너 수 − 2
    TriangleCornerNotOnPerimeter,
    DuplicateAttributeName,  // (element, 이름)이 Name 비교 규칙으로 겹침
    EngineAttributeMismatch, // 엔진 이름 속성의 요소·값 타입·채널이 3.2절 표와 다름
    BoolValue,               // Bool 값이 0·1이 아님
    NameValue,               // Name 값이 Name 내용 규칙 위반
    TooManyInfluences,       // 정점 영향 > MeshMaxSourceInfluences
    DuplicateInfluenceBone,  // 한 정점 안 같은 boneIndex
    BoneIndexRange,          // boneIndex >= MeshValidateOptions::boneCount
    ZeroWeightSum,           // 영향이 있는데 가중치 합이 0(정규화 불가 — SanitizeMeshSource)
    // 경고(SanitizeMeshSource).
    NonFiniteTexCoord,    // NonFiniteAttribute: UV 비유한 성분을 0으로 고침
    NonFiniteColor,       // NonFiniteAttribute: 색 비유한값을 (1, 1, 1, 1)로 고침
    UvOutOfRange,         // UvOutOfRange: |UV 성분| > MeshUvRangeWarning
    NonNormalizedWeights, // NonNormalizedWeights: 가중치 합 != BoneWeightTotal을 정규화함
    // 빌드(MeshTools BuildStaticMeshLod — Docs/Design/MeshAnimationAssets 5.2·5.4절). 진단 code는 같은
    // 이름이고 InvalidBuildScale만 InvalidSetting이다. index는 차례로 묶음·없음·필드·축이다.
    MaterialSlotFallback, // 묶음의 슬롯 이름과 맞는 슬롯이 없음
    NegativeBuildScale,   // buildScale 음수 성분이 홀수 개(count = 그 수)
    UnsupportedSetting,   // 보존 설정(MeshBuildSettingField)이 기본값이 아님
    InvalidBuildScale,    // buildScale 축이 0·비유한이거나 역수가 비유한
    // 스킨 빌드(MeshTools BuildSkinnedMeshLod — 6.5절). 경고 셋은 같은 이름의 코드(NonFiniteMorph는
    // NonFiniteAttribute), InvalidSettingValue는 InvalidSetting, 나머지는 InvalidSkeletonData다.
    InfluencesTrimmed,    // 쿠킹 상한으로 영향을 버린 정점이 있음(count = 정점 수)
    MissingInfluences,    // 영향이 없어 루트 본에 붙인 정점이 있음(count = 정점 수)
    NonFiniteMorph,       // morph 변화량 비유한 항목을 0으로 봄(count = 항목 수)
    InvalidSettingValue,  // 스킨 임계값이 음수·비유한이거나 boneInfluenceLimit이 음수(index = 필드)
    InvalidBoneHierarchy, // 메시 본 계층이 ValidateBoneHierarchy를 통과하지 못함(index = 본)
    EmptyBoneHierarchy,   // 메시 본 계층에 본이 없음
    SingularBindPose      // 본의 component 공간 기준 pose 행렬에 역이 없음(index = 본)
};

// name이 prefix(엔진 접두 이름 — MeshAttributeName::SkinWeightsPrefix·MorphPrefix·
// MorphNormalPrefix)로 시작하고 접두 뒤가 비어 있지 않은지다. 접두 비교는 ASCII 대소문자를
// 무시한다(3.7절 5 — ValidateMeshSource의 판정과 같다). 빌드가 프로필·morph 속성을 고를 때 쓴다.
ROCKS_MESHSOURCE_API bool MatchesMeshAttributePrefix(StringView name, StringView prefix) noexcept;

// 진단 세부 규칙의 이름이다(예: DuplicateEdge → "DuplicateEdge"). 모르는 값은 "Unknown"이다. 반환
// 문자열은 정적 수명이다(로그·명령행 진단 줄이 쓴다).
ROCKS_MESHSOURCE_API const char* MeshIssueName(MeshIssue issue) noexcept;

// 원본 메시의 진단 하나다(3.7절 10). 실패한 호출은 out 인자로 하나를 채우고(성공하면 건드리지
// 않는다), 경고는 SanitizeMeshSource가 배열로 돌려준다.
struct MeshDiagnostic {
    // AssetSystem 4.6절 진단 이름이다(InvalidMeshData·CorruptPackage·NonFiniteAttribute 등).
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 실패의 ErrorCode다(호출이 돌려준 것과 같다). 경고는 None이다.
    ErrorCode error = ErrorCode::None;
    MeshIssue issue = MeshIssue::None;
    // 관련 요소 종류다. 위상 전체·레코드 목록이면 Invalid다.
    MeshElement element = MeshElement::Invalid;
    // 관련 속성 번호다(MeshSource 속성 번호. 로드 배치 판정은 MeshSourceData::attributes 번호).
    // 속성과 무관하면 MeshNoAttribute다.
    uint32 attribute = MeshNoAttribute;
    // 처음 걸린 요소 번호다(속성 값이면 그 요소 번호). 없으면 MeshNoIndex다.
    uint32 index = MeshNoIndex;
    // 경고가 센 값 수다(고친 값·범위 밖 값·정규화한 정점). 오류는 0이다.
    uint64 count = 0;
};

// diagnostic을 AssetSystem 4.6절 형식으로 옮긴다: code·error는 그대로, asset은 인자, section은 0,
// detail은 index(MeshNoIndex면 count)다.
ROCKS_MESHSOURCE_API AssetDiagnostic ToAssetDiagnostic(const MeshDiagnostic& diagnostic,
                                                       AssetId asset) noexcept;

} // namespace rocks
