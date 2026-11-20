#pragma once

#include <Animation/BoneHierarchy.h>
#include <Core/Math/Matrix4.h>
#include <MeshTools/StaticMeshBuilder.h>

// 스킨 메시 LOD 빌드다(Docs/Design/MeshAnimationAssets.ko.md 6.3·6.5절, UE BuildSkeletalMesh·
// FSkeletalMeshBuilder 대응).
namespace rocks {

// 쿠킹 정점당 영향 칸 수다(6.3절 — 렌더 정점 영향 수 최댓값이 4 이하면 Low, 아니면 High. UE
// MAX_INFLUENCES_PER_STREAM·EXTRA_BONE_INFLUENCES).
inline constexpr uint32 MeshCookedInfluencesLow = 4;
inline constexpr uint32 MeshCookedInfluencesHigh = 8;

// 8비트 쿠킹 가중치의 합이다(UByte4Unorm — 6.3절 잔차 규칙).
inline constexpr uint16 SkinWeightTotal8 = 255;

// 법선을 추적할 때 morph 법선 변화량 길이²가 이 값보다 크면 위치 변화가 작아도 그 렌더 정점을
// 담는다(UE ComputeMorphDeltas의 0.01f).
inline constexpr float MorphNormalDeltaThresholdSquared = 0.01f;

// LOD 하나의 스킨 빌드 입력이다. 포인터·span이 가리키는 것은 BuildSkinnedMeshLod가 끝날 때까지
// 살아 있어야 한다.
struct SkinnedMeshBuildInput {
    // 원본 메시다(필수). 정점 SkinWeights·SkinWeights-<profile>, 정점 Morph-<이름>, 코너
    // MorphNormal-<이름>을 쓴다(3.2절).
    const MeshSource* source = nullptr;
    // 메시 본 계층이다(필수 — 6.1절, 원본 boneIndex가 이 번호다).
    const BoneHierarchy* bones = nullptr;
    SkinnedMeshBuildSettings settings;
    // 슬롯 이름·섹션 정보·bounds 확장·임시 할당기는 StaticMeshBuildInput과 같다.
    Span<const StringView> slotNames{};
    Span<const MeshBuildSectionInfo> sectionInfo{};
    Vec3d positiveBoundsExtension{};
    Vec3d negativeBoundsExtension{};
    Allocator* scratch = nullptr;
};

// 가중치 프로필 하나의 렌더 정점별 영향이다(6.5절 9). influences는 SkinnedMeshBuildResult::
// influences와 같은 배치다.
struct SkinWeightProfileBuild {
    explicit SkinWeightProfileBuild(Allocator& allocator) noexcept : influences(allocator) {
    }

    // 원본의 SkinWeights-<profile> 속성 번호다(이름은 원본에서 얻는다).
    uint32 sourceAttribute = MeshNoAttribute;
    Array<BoneInfluence> influences;
};

// morph target 변화량 하나다(렌더 정점 번호 오름차순).
struct MorphTargetDelta {
    uint32 vertex = 0;
    Vec3 positionDelta{};
    Vec3 normalDelta{};
};

// morph target 하나다(6.5절 13). 두 속성 번호 가운데 없는 쪽은 MeshNoAttribute다(이름은 있는 쪽
// 원본 속성에서 접두를 뺀 것).
struct MorphTargetBuild {
    explicit MorphTargetBuild(Allocator& allocator) noexcept : deltas(allocator) {
    }

    // 원본의 정점 Morph-<이름> 속성 번호다.
    uint32 positionAttribute = MeshNoAttribute;
    // 원본의 코너 MorphNormal-<이름> 속성 번호다.
    uint32 normalAttribute = MeshNoAttribute;
    Array<MorphTargetDelta> deltas;
};

// 스킨 메시 LOD 하나의 메모리 빌드 결과다(쿠킹 패킹 전 — SkinnedRenderLod 패킹은 MA5b).
//
// 수명: 할당기는 이 객체보다 오래 살아야 한다. 스레드: 동기화하지 않는다.
struct SkinnedMeshBuildResult {
    explicit SkinnedMeshBuildResult(Allocator& allocator) noexcept
        : mesh(allocator), influences(allocator), profiles(allocator), boneMap(allocator),
          requiredBones(allocator), inverseBindMatrices(allocator), morphTargets(allocator) {
    }

    SkinnedMeshBuildResult(SkinnedMeshBuildResult&&) noexcept = default;
    SkinnedMeshBuildResult& operator=(SkinnedMeshBuildResult&&) noexcept = default;
    SkinnedMeshBuildResult(const SkinnedMeshBuildResult&) = delete;
    SkinnedMeshBuildResult& operator=(const SkinnedMeshBuildResult&) = delete;

    // 렌더 정점 속성·index·섹션·bounds·경고다(정적 메시와 같은 형태 — 크기는 늘 1, 6.5절 3).
    // 경고에는 정보 진단 InfluencesTrimmed도 들어 있다(6.5절 14 순서).
    StaticMeshBuildResult mesh;
    // 정점당 영향 칸 수(MeshCookedInfluencesLow 또는 High)와 가중치 합(SkinWeightTotal8 또는
    // BoneWeightTotal — useHighPrecisionSkinWeights)이다.
    uint32 influencesPerVertex = MeshCookedInfluencesLow;
    uint16 weightTotal = SkinWeightTotal8;
    // 렌더 정점 v의 영향 k는 influences[v × influencesPerVertex + k]다. boneIndex는 boneMap 안
    // 위치, 가중치 내림차순(같으면 본 번호 오름차순)이고 빈 칸은 (0, 0)이다. 칸의 가중치 합은
    // weightTotal이다.
    Array<BoneInfluence> influences;
    // 가중치 프로필(원본 SkinWeights-<profile> 순서)이다.
    Array<SkinWeightProfileBuild> profiles;
    // LOD 통합 bone map이다: 렌더 정점 영향(기본·프로필)이 쓰는 메시 본 번호의 오름차순 목록.
    Array<uint16> boneMap;
    // 필요 본 목록이다(메시 본 번호 오름차순 — 지금은 메시 본 전체, 6.5절 11).
    Array<uint16> requiredBones;
    // 메시 본마다 component 공간 기준 pose의 역행렬이다(행 벡터 규약, double).
    Array<Mat4d> inverseBindMatrices;
    // morph target이다(Morph- 속성 원본 순서, 그다음 짝 없는 MorphNormal- 속성).
    Array<MorphTargetBuild> morphTargets;
};

// 원본 정점 하나의 영향을 쿠킹 상한까지 줄인다(6.5절 7): 가중치 0을 빼고, 가중치 내림차순·같으면
// 본 번호 오름차순으로 정렬해 limit(1–MeshCookedInfluencesHigh)개까지 out에 남긴다. 버린 것이
// 있으면 NormalizeBoneWeights로 합을 BoneWeightTotal로 맞추고 다시 정렬한다. 영향이 없으면 (본 0,
// BoneWeightTotal) 하나다. source는 합이 BoneWeightTotal로 정규화된 값이어야 한다(값 정리 뒤).
//
// 반환: 남긴 개수(1–limit)다. trimmed는 버린 영향이 있으면 true, missing은 영향이 없었으면 true다.
// out은 MeshCookedInfluencesHigh칸 이상이어야 한다.
uint32 ReduceBoneInfluences(Span<const BoneInfluence> source, uint32 limit, Span<BoneInfluence> out,
                            bool& trimmed, bool& missing) noexcept;

// 합이 BoneWeightTotal인 영향(내림차순 정렬)을 8비트 합 SkinWeightTotal8로 바꾼다(6.5절 10):
// (w × 255 + 32767) / 65535로 반올림한 뒤 잔차(255 − 합)를 첫 영향에 더하고 다시 정렬한다. 비어
// 있으면 그대로다.
void QuantizeSkinWeights8(Span<BoneInfluence> influences) noexcept;

// 원본 메시 LOD 하나를 스킨 렌더 데이터로 빌드한다(6.3·6.5절). 렌더 정점·index·섹션·bounds는
// 5.2절 단계(스킨 임계값, 크기 1)이고, 거기에 영향·bone map·필요 본·inverse-bind·morph target을
// 더한다.
//
// 순서: 설정 검사(임계값·boneInfluenceLimit) → 본 계층(비었는지, ValidateBoneHierarchy) → 원본
// 검증(boneCount = 본 수) → 사본(코너 속성 + 가중치 속성) 값 정리 → 보존 설정 경고(buildScale 포함)
// → 원본 정점마다 영향 줄이기(MissingInfluences·InfluencesTrimmed) → morph 속성 찾기(비유한 항목
// 경고) → 삼각형·코너 기저 → 겹치는 코너 → 렌더 정점(영향·morph 비교를 더한 용접) → 섹션 → bounds
// → 영향 부호화·bone map·필요 본·inverse-bind → morph 변화량. 모두
// RunWithDeterministicFloatingPoint 안이고 스레드를 쓰지 않는다. 같은 입력이면 결과 bit가 같다.
//
// 사용 예:
//   SkinnedMeshBuildInput input;
//   input.source = &mesh;
//   input.bones = &meshBones;
//   input.settings.boneInfluenceLimit = 4;
//   SkinnedMeshBuildResult built(allocator);
//   MeshDiagnostic failure;
//   if (auto result = BuildSkinnedMeshLod(input, built, &failure); !result) {
//       Report(AssetDiagnosticName(failure.code), MeshIssueName(failure.issue), failure.index);
//   }
//
// 오류: source·bones가 null이면 InvalidArgument(진단 없음). 임계값이 음수·비유한이거나
// boneInfluenceLimit이 음수면 InvalidSetting(issue InvalidSettingValue, index =
// MeshBuildSettingField)·InvalidArgument다. 본이 없으면 InvalidSkeletonData(EmptyBoneHierarchy)·
// InvalidArgument, 계층 위반은 InvalidSkeletonData(InvalidBoneHierarchy, index = 본)와
// ValidateBoneHierarchy의 오류, 역이 없는 기준 pose는 InvalidSkeletonData(SingularBindPose, index =
// 본)·InvalidArgument다. 원본 검증·값 정리 실패는 그 진단(InvalidMeshData·TooManyInfluences —
// 속성 번호는 원본 번호)이다. 삼각형 수 상한은 CapacityExceeded, 할당 실패는 그 오류(진단 없음)다.
// 실패하면 out은 그대로다.
//
// 인자: out은 성공하면 통째로 바뀐다. failure는 실패하면 채운다(성공하면 건드리지 않는다).
// 스레드: input.source·input.bones를 읽기만 한다.
Result<void> BuildSkinnedMeshLod(const SkinnedMeshBuildInput& input, SkinnedMeshBuildResult& out,
                                 MeshDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
