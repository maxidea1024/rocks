## 6. 스켈레톤과 스킨 메시

### 6.1 본 계층 `BoneHierarchy`

```cpp
inline constexpr int32 NoParentBone = -1;

ROCKS_STRUCT()
struct BoneInfo {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name name;                         // 본의 안정 식별자(UE와 같이 이름으로 식별)
    ROCKS_PROPERTY() int32 parentIndex = NoParentBone;  // 루트만 NoParentBone
    ROCKS_PROPERTY(EditorOnly) String exportName;       // 대소문자를 보존한 원래 이름(UE ExportName)
};

ROCKS_STRUCT()
struct BoneHierarchy {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Array<BoneInfo> bones;
    ROCKS_PROPERTY() Array<Transform> referencePose;    // 부모 기준 local, double(UE FTransform)
};
```

- 불변식: 루트는 하나이고 번호 0이다. 나머지 본의 부모 번호는 자기 번호보다 작다. UE도 기본(`bOnlyOneRootAllowed = true`)에서 같은 검사를 한다(E/Public/ReferenceSkeleton.h:101, 214-220). 이름은 Core `Name` 규칙(ASCII 대소문자 무시)으로 유일하다. 기준 pose의 회전은 단위 quaternion이며 저장 전에 정규화한다(UE도 Add 때 정규화한다).
- 본 수 상한은 `uint16` boneIndex(3.2절) 때문에 `MeshMaxBones`(= 65535)다. UE `FBoneIndexType`도 uint16이다(RT/AnimationCore/Public/BoneIndices.h:7).
- 계획 15절의 "stable bone ID"는 본 이름이다. 메시·클립·스켈레톤 사이 대응은 이름으로 하고 번호는 각 에셋 안의 지역 번호다(UE와 같다).
- 계층은 원본 본만 담는다. 가상 본을 덧붙인 전체 계층은 로드 때 계산하며 저장하지 않는다(UE raw·final 구분 — ReferenceSkeleton.h:107-116, E/Private/Animation/ReferenceSkeleton.cpp:942-948 직렬화, 960-965 로드 때 final 구성).
- 계획 15절의 inverse-bind pose는 저장하지 않고 빌드가 기준 pose에서 계산해 쿠킹 데이터(6.3절)에 넣는다.

### 6.2 스켈레톤 `Skeleton`

```text
Skeleton : Asset
  bones               : BoneHierarchy
  retargetModes       : Array<BoneTranslationRetarget>    // bones와 같은 길이. { Animation, Skeleton, AnimationScaled, AnimationRelative, OrientAndScale }
  virtualBones        : Array<VirtualBone { sourceBone Name, targetBone Name, name Name }>   // 이름 접두사 "VB "(UE Skeleton.cpp:109)
  sockets             : Array<BoneSocket { name, boneName, relativeLocation, relativeRotation, relativeScale, forceAlwaysAnimated }>
  curves              : Array<SkeletonCurveInfo { name, isMaterial, isMorphTarget, linkedBones Array<Name>, maxLod uint8 }>
  slotGroups          : Array<AnimSlotGroup { name, slots Array<Name> }>
  retargetSources     : Array<NamedReferencePose { name, pose Array<Transform> }>
  compatibleSkeletons : Array<SoftObjectRef<Skeleton>>
  useRetargetModesFromCompatibleSkeleton : bool
```

- `retargetModes`는 **구현**(애니메이션 평가 단계에서 쓴다)이고, 슬롯 묶음·리타깃 원본·호환 스켈레톤은 **보존**(montage·리타깃 단계)이다.
- blend profile(UE `BlendProfiles`, Skeleton.h:526)은 blend 단계에서 더한다.
- 본 병합: 스킨 메시를 임포트할 때 메시 본이 스켈레톤에 없으면 스켈레톤에 더한다. 부모 사슬이 맞지 않으면(중간 본이 빠짐) `SkeletonMismatch`로 실패한다(UE `MergeBonesToBoneTree`·`DoesParentChainMatch`, Skeleton.h:786-797, 912-923). 병합은 스켈레톤 패키지를 바꾸므로 그 스켈레톤에 Build 의존을 건 클립이 다시 쿠킹된다(7.1절).

### 6.4 구현에서 정한 점(MA4a)

MA4a 구현(모듈 `Engine/Source/Runtime/Animation`) 중 6.1·6.2절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **모듈과 파일.** `Animation`은 AssetRuntime(`Asset`·진단)과 CoreTypeBindings(`Transform`·`Rotator<double>`·`Vec3d` 미러 — 모두 있어 새 미러는 없다)에 의존한다. 8절의 Curves 의존은 클립(MA6)이 들어올 때 더한다. 리플렉션 헤더는 `Animation/BoneHierarchy.h`(`BoneInfo`·`BoneHierarchy`)와 `Animation/Skeleton.h`(`BoneTranslationRetarget`·`VirtualBone`·`BoneSocket`·`SkeletonCurveInfo`·`AnimSlotGroup`·`NamedReferencePose`·`Skeleton`)이고, 병합은 `Animation/SkeletonMerge.h`, 진단은 `Animation/AnimationDiagnostic.h`다. 모든 리플렉션 class·struct에 `ROCKS_GENERATED_BODY()`를 둔다.
2. **계층 불변식(`ValidateBoneHierarchy`).** 차례대로 본 수 ≤ `MeshMaxBones`(65535, 넘으면 `CapacityExceeded`), pose 길이 = 본 수, 본 번호 순으로 (본 0의 부모 = `NoParentBone`, 본 i ≥ 1의 부모 ∈ [0, i) — 두 번째 루트·자기·뒤 본·−2 이하는 모두 `ParentOrder`, 이름은 구조가 맞는 Normal 토큰 — None 거부, pose 성분 유한, 회전 |길이² − 1| ≤ `BoneRotationNormalizedTolerance`(0.01, UE `THRESH_QUAT_NORMALIZED`)), 마지막에 이름 중복(같은 이름의 두 번째 본 번호가 가장 작은 쌍, related = 앞 본)이다. 이름 비교는 Name 토큰 비교(같은 pool, ASCII 대소문자 무시)라 pool 없이 한다. 빈 계층은 정상이다. `exportName`은 검사하지 않는다(원래 이름의 대소문자 보존용 EditorOnly 표시값이라 이름과 다를 수 있다 — UE도 검사하지 않는다).
3. **pose 정규화(`NormalizeBoneReferencePose`).** 6.1절 "저장 전에 정규화"를 함수로 둔다: 길이² = 1이 아닌 유한 회전을 1/√길이²로 나누고, 길이²가 `BoneRotationMinimumLengthSquared`(1e-8, UE `FQuat::Normalize`의 `UE_SMALL_NUMBER`) 미만이면 단위 회전으로 바꾼다. 비유한 회전은 그대로 둔다(검증이 거부한다). 저장 경로(임포트·병합 저장)가 부르는 것은 MA4b다.
4. **스켈레톤 불변식(`ValidateSkeleton`).** 계층 다음에 `retargetModes` 길이 = 본 수·값이 열거 안, 가상 본(이름은 "VB " 접두사로 시작하고 접두사보다 길다 — ASCII 대소문자 무시, UE `CheckVirtualBonePrefix`의 `StartsWith` 기본과 같다, 이름이 본·앞 가상 본과 겹치지 않음, (원본, 대상) 쌍이 앞 가상 본과 같지 않음 — UE `AddNewVirtualBone`, 원본·대상은 본 또는 **앞** 가상 본 — 사슬은 되지만 순환은 막는다), 소켓(이름 유일, 본은 원본 본 또는 가상 본, 상대 변환 유한), 커브(이름 유일, `linkedBones`는 원본 본만 — UE `FCurveMetaData::LinkedBones`가 raw 본 참조다), 슬롯 묶음(이름 유일, 슬롯 이름은 모든 묶음을 통틀어 유일 — UE에서 슬롯은 묶음 하나에만 속한다), 리타깃 원본(이름 유일, pose 유한 — 보존 필드라 길이는 보지 않는다) 순이다. `compatibleSkeletons`는 soft 참조라 보지 않는다. 가상 본 접두사는 원문이 필요해 `ValidateSkeleton`만 `NamePool`을 받고, 원문을 읽지 못하면(다른 pool·stale 토큰) 그 pool 오류다.
5. **진단.** 6.1·6.2절 불변식 위반은 새 코드 `InvalidSkeletonData`(오류 — 11절 표에 더했다)이고, 병합의 부모 사슬 실패는 `SkeletonMismatch`다. 둘을 `AssetDiagnosticCode`에 등록했다. 결과는 `SkeletonDiagnostic{code, error, issue(SkeletonIssue), role(Skeleton·Mesh), index, related}`이고 `ToAssetDiagnostic`이 4.6절 형식(detail = index)으로 옮긴다. 할당 실패는 진단 코드 없이 그 오류다. 버린 대안: 계층 위반도 `SkeletonMismatch`(병합 실패와 입력 손상을 가를 수 없다), `InvalidMeshData`(메시가 아니다).
6. **병합(`MergeBoneHierarchy`).** UE `MergeBonesToBoneTree`의 기본 경로(부모 사슬 검사 켬 — `a.AllowIncompatibleSkeletalMeshMerge` 끔)를 순수 함수로 옮긴다. 두 입력을 검증하고(진단의 role로 어느 쪽인지), 메시 본이 없으면 UE처럼 거부한다(`SkeletonMismatch`·`EmptyMeshHierarchy` — UE `IsCompatibleMesh`는 맞는 본이 하나도 없으면 거짓이고 `CreateReferenceSkeletonFromMesh`도 본이 없으면 만들지 않는다, 리뷰 반영 개정 13). 스켈레톤이 비었으면 결과는 메시 그대로(UE `CreateReferenceSkeletonFromMesh`)다. 아니면 메시 본마다 UE `IsCompatibleMesh`대로 같은 이름 본 또는 스켈레톤에 있는 가장 가까운 메시 조상에서 `DoesParentChainMatch`(부모 이름을 차례로 비교하고 한쪽이 루트에 닿으면 다른 쪽도 루트, 시작 본이 스켈레톤 루트면 두 루트 이름만 비교)를 한다. 실패는 `ParentChainMismatch`(index = 메시 본, related = 대조한 스켈레톤 본), 조상이 하나도 없으면 `MissingAncestor`다. 통과하면 스켈레톤 본을 번호 그대로 두고 없는 메시 본을 메시 순서로 뒤에 더한다(부모 = 메시 부모의 결과 번호 — 부모가 앞이라 늘 있다, pose = 메시 pose). 이미 있는 본은 이름이 대소문자만 다르면(같은 토큰 id, 다른 표시) UE `Rename`처럼 메시 표시와 `exportName`으로 바꾼다(`renamedBones`). UE의 둘째 단계(부모가 바뀐 본 옮기기)는 사슬 검사를 켠 경로에서 일어나지 않으므로 두지 않는다. 결과 본 수가 `MeshMaxBones`를 넘으면 `InvalidSkeletonData`·`TooManyBones`·`CapacityExceeded`다. 결과는 계층 + 메시 본 → 결과 본 대응표(`meshToMerged`)이고, 실패하면 out은 그대로다. `retargetModes` 등 스켈레톤의 본 길이 배열을 늘리는 것과 저장은 MA4b다(새 본의 리타깃 방식은 UE 기본 `Animation`).
7. **결정성.** 검증은 첫 위반 규칙과 위치가 입력만으로 정해지고(이름 중복은 정렬 뒤 가장 작은 두 번째 번호), 병합은 메시 순서를 따르므로 같은 입력이면 같은 결과다. 부동소수점은 정규화의 √ 하나뿐이다(IEEE 정확 반올림). 스레드를 쓰지 않는다.
8. **시험 환경.** 패키지 왕복은 AssetRuntime 시험의 `AssetEnv`에 `RegisterAnimationTypes`를 더 불러 실제 `Skeleton` 클래스로 한다(새 fixture 모듈을 두지 않는다 — 같은 의존 모듈 타입은 lease로 공유된다).

### 6.3 스킨 메시 `SkinnedAsset`·`SkeletalMesh`

```text
SkinnedAsset : StreamableRenderAsset (추상)
SkeletalMesh : SkinnedAsset
  skeleton            : ObjectRef<Skeleton>        // Hard
  bones               : BoneHierarchy              // 메시 자체 계층(스켈레톤의 부분집합, 순서가 달라도 됨)
  EditorOnly:
    sourceLods        : Array<SkeletalMeshSourceLod {
                          hasSource bool, source MeshSourceData, bakedSource MeshSourceData,
                          build SkinnedMeshBuildSettings, reduction MeshReductionSettings,
                          screenSize(플랫폼별), lodHysteresis float, lodMaterialMap Array<int32>,
                          bonesToRemove Array<Name>, bonesToPrioritize Array<Name>, weightOfPrioritization float }>
  materialSlots       : StructRefArray<MeshMaterialSlot>
  sockets             : Array<BoneSocket>          // 메시 전용 소켓(스켈레톤 소켓과 합쳐 쓴다)
  skinWeightProfiles  : Array<SkinWeightProfileInfo { name, defaultProfile(플랫폼별 bool), defaultProfileFromLod(플랫폼별 int32) }>
  positiveBoundsExtension, negativeBoundsExtension, minLod(플랫폼별)
  쿠킹 데이터:
    renderData        : SkeletalMeshRenderData
```

- `SkinnedMeshBuildSettings` = `MeshBuildSettings` + `useHighPrecisionSkinWeights`, `thresholdPosition`, `thresholdTangentNormal`, `thresholdUV`, `morphThresholdPosition`, `boneInfluenceLimit`(UE `FSkeletalMeshBuildSettings`, EngineTypes.h:3096-3164). 이 여섯은 **구현**이다. `bOptimizeForInstancing`은 넣지 않는다(UE 전용 렌더 최적화 플래그).
- `skinWeightProfiles`의 두 기본값 필드는 UE에서 플랫폼별 값이다(E/Public/Rendering/SkinWeightProfile.h:46, 50).
- 메시 본 ↔ 스켈레톤 본 대응 표(UE `FSkeletonToMeshLinkup`, Skeleton.h:47-65)는 로드 때 이름으로 만든다. 저장하지 않는다. 대응이 없으면 `NoParentBone`과 같은 표지(-1)다.
- physics asset(UE `PhysicsAsset`)은 물리 단계에서 별도 에셋 타입으로 더한다.
- 축소 LOD·bone 제거·bake pose는 **보존**이다.

**쿠킹 렌더 데이터.** 정적 메시의 `MeshRenderLod`에 다음을 더한 `SkinnedRenderLod`다.
- `boneInfluences`: 정점당 영향 `MeshCookedInfluencesLow`(= 4) 또는 `MeshCookedInfluencesHigh`(= 8)개(빌드 설정 `boneInfluenceLimit`에 따라 올림). 번호는 아래 LOD 통합 bone map의 위치이고, 통합 본이 `MeshBoneIndex8Limit`(= 256)개 이하면 `UByte4`, 넘으면 `UShort4`다. 가중치 `UByte4Unorm`/`UShort4Unorm`(`useHighPrecisionSkinWeights`).
- LOD 통합 bone map(`Array<uint16>`, 메시 본 번호의 정렬 목록) — LOD의 모든 섹션이 같이 쓴다. 섹션을 본 수로 나누지 않는다(RenderBase 결정 R18, UE 5.8과 같다 — 섹션당 최대 본 설정이 5.8에서 deprecated). 본 행렬은 렌더러가 typed buffer로 넘긴다.
- LOD별 필요 본 목록(`requiredBones`, 부모 포함 정렬)과 메시 본별 inverse-bind 행렬(기준 pose의 component 공간 역).
- morph target: 이름별 희소 (렌더 정점 번호, 위치 변화량 `Half4`, 법선 변화량 `Half4`) 목록. `morphThresholdPosition` 미만의 변화량은 뺀다.

영향 개수 줄이기: 원본 영향(최대 12)을 가중치 내림차순(같으면 본 번호 오름차순)으로 정렬해 쿠킹 상한(최대 8)까지 남기고 다시 정규화한다. 버린 영향이 있으면 정점 수를 세어 `InfluencesTrimmed` 정보 진단을 낸다. 8비트 가중치는 비례 배분 후 반올림하고 잔차(255와의 차)를 가장 큰 영향에 더한다. 모두 결정적이다.

### 6.5 구현에서 정한 점(MA5a)

MA5a 구현(`Mesh`의 `SkinnedMeshBuildSettings`, `MeshTools`의 스킨 빌드) 중 6.3절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. UE 근거는 Dev/MeshUtilities/Private/MeshUtilities.cpp(`GenerateSkeletalRenderMesh`), SkeletalMeshTools.cpp(`BuildSkeletalMeshChunks`·`AreSkelMeshVerticesEqual`), Dev/SkeletalMeshUtilitiesCommon/Private/LODUtilities.cpp(morph 변화량·대체 가중치), RT/AnimationCore/Public/BoneWeights.h, E/Private/SkeletalMesh.cpp(`CalculateInvRefMatrices`·`CalculateRequiredBones`)다.

1. **API.** `MeshTools/SkinnedMeshBuilder.h`의 순수 함수 `BuildSkinnedMeshLod(const SkinnedMeshBuildInput&, SkinnedMeshBuildResult&, MeshDiagnostic*)`다. 입력은 원본 메시, 메시 본 계층(`BoneHierarchy` — 6.1절), `SkinnedMeshBuildSettings`, 슬롯 이름·섹션 정보·bounds 확장(5.4절 1과 같다), 임시 할당기다. 결과는 정적 메시와 같은 `StaticMeshBuildResult`(`mesh` — 렌더 정점 속성·index·섹션·bounds·경고)에 정점당 영향 칸 수, 가중치 합(255 또는 65535), 영향 배열(정점 × 칸, 본 번호는 bone map 위치), 가중치 프로필별 영향, LOD 통합 bone map, 필요 본 목록, 메시 본별 inverse-bind 행렬(`Mat4d`), morph target별 희소 변화량을 더한다. 프로필·morph target은 원본 속성 번호로 가리킨다(이름 원문은 원본에서 얻는다). 실패하면 결과는 그대로다. `MeshTools`는 `Animation`에 public으로 의존한다(본 계층이 공개 입력이다).
2. **설정 struct.** `SkinnedMeshBuildSettings`는 `MeshBuildSettings`의 리플렉션 파생 struct다(struct 상속 — ReflectionExtensions 12절). 여섯 필드의 기본값은 UE와 같다(`thresholdPosition` 0.00002, `thresholdTangentNormal` 0.00002, `thresholdUV` 1/1024, `morphThresholdPosition` 0.015, `boneInfluenceLimit` 0, `useHighPrecisionSkinWeights` 거짓 — EngineTypes.h:3167-3183). 진단용 필드 번호는 `MeshBuildSettingField` 뒤에 이어 붙인다(`SkinnedMeshBuildSettingFieldCount` = 23). 버린 대안: 공통 설정을 멤버로 담기(필드 경로가 한 단 깊어지고 공통 필드 판정 함수를 다시 써야 한다), UE처럼 따로 된 struct(공통 필드를 두 번 정의한다).
3. **스킨 빌드의 `buildScale`은 보존이다.** UE `FSkeletalMeshBuildSettings`에는 BuildScale이 없다. 정점만 키우면 본 기준 pose와 맞지 않아 inverse-bind가 틀어지므로 크기는 임포터가 메시와 본에 함께 적용할 일이다. 스킨 빌드는 크기 1로 빌드하고 `buildScale`이 기본값이 아니면 다른 보존 필드처럼 `UnsupportedSetting`을 낸다(`NegativeBuildScale`은 내지 않는다).
4. **설정 검사.** 네 임계값은 유한한 0 이상, `boneInfluenceLimit`은 0 이상이어야 한다. 어기면 `InvalidSetting`(issue `InvalidSettingValue`, index = 필드 번호)·InvalidArgument다. `boneInfluenceLimit` 0은 기본 상한이다: UE는 0이면 프로젝트 기본값을 쓰고 그것도 0이면 `MAX_TOTAL_INFLUENCES`인데 unlimited influence 모드가 없으면 `EXTRA_BONE_INFLUENCES`(8)로 자른다(GPUSkinVertexFactory.cpp `GetBoneInfluenceLimitForAsset`, MeshUtilities.cpp `GenerateSkeletalRenderMesh`). Rocks는 프로젝트 설정·unlimited 모드가 없으므로 0과 8을 넘는 값은 모두 `MeshCookedInfluencesHigh`(8)다.
5. **원본 사본과 검증.** 5.4절 3의 사본에 정점 `SkinWeights`·`SkinWeights-<profile>`도 담아 값 정리가 가중치를 정규화하게 한다(`NonNormalizedWeights` 경고, 합 0은 `InvalidMeshData`·`ZeroWeightSum` 실패 — 진단의 속성 번호는 원본 번호로 바꾼다). 원본 검증은 `boneCount` = 본 수로 해서 범위 밖 boneIndex를 거부한다. 본 계층은 `ValidateBoneHierarchy`로 보고, 어기면 `InvalidSkeletonData`(issue `InvalidBoneHierarchy`, index = 그 진단의 본 번호 — 세부 규칙은 `ValidateBoneHierarchy`를 다시 불러 얻는다), 본이 없으면 `InvalidSkeletonData`·`EmptyBoneHierarchy`다(영향 없는 정점이 루트 본을 쓰므로 루트가 있어야 한다).
6. **임계값.** 렌더 정점 용접은 위치 `thresholdPosition`, 법선·tangent·쌍법선 `thresholdTangentNormal`, UV `thresholdUV`로 비교한다(UE `AreSkelMeshVerticesEqual`). UE 스킨 빌드는 용접 위치 임계값을 `bRemoveDegenerates`와 무관하게 쓰므로 Rocks도 그렇게 한다. 퇴화 삼각형 판정과 삼각형 기저의 위치 임계값은 5.4절 4와 같은 규칙에 스킨 임계값을 넣는다(`removeDegenerates`면 `thresholdPosition`, 아니면 0). 그에 더해 용접 뒤 세 렌더 정점 번호가 서로 다르지 않은 삼각형도 뺀다(UE `BuildSkeletalMeshChunks` — 그 삼각형이 만든 렌더 정점은 남는다, UE와 같다). 법선·tangent 계산은 5.2절 3–5단계 그대로다. UE의 스킨 경로는 옛 import data 기반의 다른 tangent 구현이라 비트 일치는 원래 보장하지 않는다(5.2절).
7. **영향 줄이기(원본 정점마다 한 번).** 가중치 0인 영향을 빼고(UE `FBoneWeights`의 기본 문턱), 가중치 내림차순·같으면 본 번호 오름차순으로 정렬해 상한까지 남기고, 버린 것이 있으면 MA1 정규화(`NormalizeBoneWeights` — 내림 비례 배분 + 잔차를 가장 큰 영향에)로 다시 합을 65535로 맞춘 뒤 다시 정렬한다. 영향이 없는 정점(속성이 없거나 빈 목록)은 루트 본 0에 65535다(UE "Missing influence … weighting to root"). 그런 정점이 있으면 가중치 속성마다 새 경고 `MissingInfluences`(index = 첫 정점, count = 정점 수 — 속성이 없으면 attribute = `MeshNoAttribute`), 버린 정점이 있으면 `InfluencesTrimmed`(정보, 같은 형식)다. 둘 다 삼각형이 쓰지 않는 정점까지 원본 정점 전체를 센다.
8. **용접 비교의 스킨 항목.** 같은 원본 정점의 두 코너는 영향·morph 위치 변화량이 늘 같으므로 `MorphNormal-` 값만 비교한다. 다른 원본 정점이면 줄인 16비트 영향(기본·프로필 모두)과 모든 morph target의 위치 변화량(비트 단위), `MorphNormal-` 값을 비교한다. UE는 영향만 비교해 위치가 겹치는 서로 다른 정점의 morph 변화량이 다르면 하나를 잃는다 — Rocks는 손실을 막는다. 아래 10의 재계산 법선 변화량은 용접 뒤에 정해지므로 비교하지 않는다(렌더 정점을 만든 코너의 값을 쓴다).
9. **가중치 프로필도 빌드한다.** `SkinWeights-<profile>`마다 같은 규칙으로 줄여 렌더 정점별 영향을 만든다. UE는 대체 가중치를 렌더 정점의 원본 점에서 가져오고 용접에는 넣지 않는다(LODUtilities.cpp `GenerateImportedSkinWeightProfileData`). bone map에 프로필 본을 넣는 것은 UE와 같다(`AlternateBoneIDs`).
10. **쿠킹 칸과 가중치 부호화.** 정점당 칸 수는 렌더 정점의 (기본·프로필) 영향 수 최댓값이 4 이하면 `MeshCookedInfluencesLow`(4), 아니면 `MeshCookedInfluencesHigh`(8)다(UE 섹션 `MaxBoneInfluences`의 extra influence 판정과 같다 — 상한은 자르는 값이고 칸 수는 실제 최댓값의 올림). 빈 칸은 (0, 0)이다. `useHighPrecisionSkinWeights`면 16비트(합 65535) 그대로, 아니면 8비트로 `(w × 255 + 32767) / 65535`(반올림) 후 잔차(255 − 합, 음수일 수 있다)를 첫 영향(가장 큰 것, 같으면 본 번호가 작은 것)에 더하고 다시 정렬한다. UE는 8비트를 `w >> 8`로 잘라 합이 255보다 작아질 수 있다(SkinWeightVertexBuffer.cpp `SetBoneWeight`) — Rocks는 6.3절대로 합을 맞춘다. 반올림으로 0이 된 영향도 칸과 본을 유지한다.
11. **bone map과 필요 본.** bone map은 렌더 정점이 쓰는 (기본·프로필) 영향 본의 정렬된 중복 없는 목록이다(빠진 삼각형만 쓰던 정점은 넣지 않는다). 필요 본 목록은 UE `CalculateRequiredBones`(제거 본이 없을 때)와 같이 메시 본 전체다 — 본 제거(`bonesToRemove`)는 보존 설정이라 MA5b 이후에 뺀다. 버린 대안: bone map + 조상(스킨에 안 쓰는 소켓·IK 본이 평가되지 않아 UE와 달라진다).
12. **inverse-bind.** 본 번호 순으로 component[b] = local[b].ToMatrix() × component[부모](`Mat4d`, UE `CalculateInvRefMatrices`와 같은 곱 순서)를 만들고 `Mat4d::TryInverse`로 뒤집는다. UE는 float(`FMatrix44f`)로 저장하는데 그 변환은 쿠킹 패킹(MA5b) 몫이다. 행렬식이 0이라 역이 없으면(크기 0 축) `InvalidSkeletonData`·`SingularBindPose`(index = 본) 실패다 — UE는 경고만 남기고 쓸 수 없는 행렬을 둔다.
13. **morph target.** `Morph-<이름>` 속성마다 하나(원본 속성 순서)이고 같은 이름(Name 비교 규칙)의 `MorphNormal-<이름>`을 짝으로 쓴다. 짝이 없는 `MorphNormal-`도 그 뒤에 위치 변화량 0인 target으로 둔다. 렌더 정점 번호 오름차순으로 Δp = 그 정점을 만든 코너의 원본 정점 변화량, Δn = 같은 코너의 `MorphNormal-` 값이다. `MorphNormal-`이 없으면 UE처럼 법선을 다시 계산한다: Δp가 0이 아닌 정점을 옮긴 위치로, 그 정점을 쓰는 삼각형의 세 정점에 붙은 코너 법선만 0으로 두고 5.2절 3·5단계(법선만, 같은 가중 설정)를 다시 돌려 Δn = (새 법선의 렌더 정규화) − (렌더 법선)이다. UE는 한 고리 더 넓게 0으로 두고(바뀌지 않은 면만 쓰는 코너까지) 원본 점 하나의 법선을 모든 렌더 정점에 쓰는데, Rocks는 실제로 바뀌는 코너만 다시 계산하고 렌더 정점마다 자기 코너 값을 쓴다. 담는 조건은 UE `ComputeMorphDeltas`와 같다: |Δp|² > `morphThresholdPosition`² 이거나, 법선을 추적하는 경우(`MorphNormal-`이 있거나 `recomputeNormals`) |Δn|² > `MorphNormalDeltaThresholdSquared`(0.01). 변화량이 하나도 없는 target도 빈 목록으로 남는다. morph 속성 값은 3.3절 정리 대상이 아니므로 비유한 항목은 0으로 보고 속성마다 `NonFiniteAttribute`(issue `NonFiniteMorph`, count = 항목 수) 경고를 낸다.
14. **경고 순서.** 값 정리(NonFiniteAttribute·UvOutOfRange·NonNormalizedWeights) → `UnsupportedSetting`(필드 순) → 가중치 속성 순(기본 먼저, 프로필은 원본 순서)으로 `MissingInfluences`·`InfluencesTrimmed` → morph target 순으로 `NonFiniteAttribute`(morph) → `MaterialSlotFallback`(묶음 순)이다.
15. **정적 빌드와 공유.** 5.2절 단계 함수(원본 사본·값 정리 경고·설정 경고·렌더 정점·섹션·bounds)를 MeshTools 내부 공용 파일로 옮기고 렌더 정점 구성에 용접 임계값·삼각형 번호 붕괴 제외·추가 비교 함수·렌더 정점을 만든 코너 목록을 매개변수로 더했다. 정적 빌드 결과 byte는 그대로다(기존 시험).
16. **진단 등록.** `AssetDiagnosticCode`에 `InfluencesTrimmed`(11절에 있던 이름)와 `MissingInfluences`(새 경고 — 11절 표에 더했다)를, `MeshIssue`에 `InfluencesTrimmed`·`MissingInfluences`·`NonFiniteMorph`·`InvalidSettingValue`·`InvalidBoneHierarchy`·`EmptyBoneHierarchy`·`SingularBindPose`를 더했다. MeshSource에 접두 판정 `MatchesMeshAttributePrefix`(3.7절 5와 같은 ASCII 대소문자 무시, 접두 뒤가 비면 거짓)를 공개해 빌드가 이름 판정을 다시 구현하지 않게 했다.
17. **결정성.** 빌드 전체가 `RunWithDeterministicFloatingPoint` 안이고 스레드를 쓰지 않는다. 정수 규칙(정렬·정규화·부호화)은 입력만으로 정해지고, morph 법선 재계산은 기본 빌드와 같은 결정적 수학을 쓴다.

### 6.6 MA4b — 스켈레톤 임포트 서비스와 병합 저장

설계일 2026-11-25(사용자 지시 "SkeletalMesh도 처리해" — MA5b의 선행). MA2b3(5.5.3절)과 같은 모양이다.

1. **모듈.** 새 Developer 모듈 `AnimationTools`(`Engine/Source/Developer/AnimationTools`, 8절)다. 공개 의존은 Animation·AssetDatabase(`AssetImportContext`)다. 클립 처리기·클립 임포트는 MA6b가 더한다.
2. **API.** `AnimationTools/SkeletonImport.h`의 `ImportSkeleton(context, output, spec, failure)`가 임포터(교환 형식 UX·glTF·FBX)의 `AssetImporter::Import` 안에서 `Output()`으로 받은 `Skeleton`을 채운다. `SkeletonImportSpec`은 10절 입력 그대로다: 본(`SkeletonImportBone{name, parentIndex, referencePose}` — 본 번호 순), `retargetModes`(비면 모두 `Animation` — UE 새 본 기본, 아니면 본 수와 같은 길이), 가상 본(이름 셋), 소켓(`BoneSocket` 필드 — 이름은 문자열), 커브(`SkeletonCurveInfo` 필드 — `linkedBones`는 문자열 목록), `replaceOnReimport`(아래 4). 포인터·span은 호출 동안 산다. `failure`(선택)는 스켈레톤 진단 세부(`SkeletonDiagnostic`)다.
3. **새 임포트.** 본이 없으면 `SkeletonMismatch`(issue `EmptyMeshHierarchy` — UE `CreateReferenceSkeletonFromMesh`도 본 없는 스켈레톤을 만들지 않는다, 재임포트 병합 판정과 같은 진단 — 리뷰 반영), `retargetModes` 길이가 0도 본 수도 아니면 `InvalidSetting`(detail = 길이)이고, 그다음 이름을 intern한다(실패는 `InvalidSetting`, detail = 그 목록 안 위치, 원래 오류). 본 계층을 만들고 기준 pose를 정규화한다(`NormalizeBoneReferencePose` — 6.4절 3 "저장 경로가 부른다"). 필드를 쓰고 `ValidateSkeleton`으로 본다. 위반은 그 진단(`InvalidSkeletonData`·`CapacityExceeded` 등, detail = 진단 index)으로 `context.Fail`이고 서비스가 출력을 버린다(AssetImporter 계약 — 저장하지 않으므로 기존 에셋은 그대로다). 보존 필드(슬롯 묶음·리타깃 원본·호환 스켈레톤·`useRetargetModesFromCompatibleSkeleton`)는 비운다.
4. **재임포트(기존 스켈레톤 — 병합).** UE는 스켈레톤 본을 지우지 않고 메시 본을 더하기만 한다(`USkeleton::MergeAllBonesToBoneTree`). 그래서:
   - 본: 정규화한 명세 계층을 메시 쪽으로 `MergeBonesIntoSkeleton`(아래 5)한다. 기존 본은 번호·pose를 지키고, 없는 본을 명세 순서로 뒤에 더하고, 대소문자만 다른 이름은 명세 표시로 바꾼다. 사슬 불일치는 `SkeletonMismatch`다(6.4절 6).
   - `retargetModes`: 기존 본은 그대로, 더한 본은 명세 값(명세가 비면 `Animation`)이다.
   - 소켓·가상 본·커브: 기본은 사용자 편집 보존 — 기존 항목을 두고 명세의 새 항목만 뒤에 더한다. 소켓은 이름(Name 비교), 가상 본은 이름이나 (원본, 대상) 쌍이 같으면 있는 것으로 본다(UE `AddNewVirtualBone`은 같은 쌍을 거부한다 — 사용자가 이름을 바꾼 가상 본을 다시 더하지 않는다), 커브는 이름이 같으면 머티리얼·morph 표시만 OR로 합치고 연결 본·`maxLod` 편집은 둔다(UE `AccumulateCurveMetaData` — 표시는 더하기만 한다). `replaceOnReimport`면 명세 값으로 바꾸고 명세에 있는 본의 `retargetModes`도 명세 값(명세가 비면 `Animation`)으로 바꾼다(교환 경로처럼 원본 쪽이 정본일 때 — 정적 메시 `replaceSettingsOnReimport`와 같은 역할). 본은 이때도 지우지 않는다(클립·메시의 본 번호가 깨지지 않게).
   - 보존 필드는 그대로 둔다(명세에 없다).
   - 그 뒤 `ValidateSkeleton`(3과 같다).
5. **병합 적용 도우미.** `AnimationTools/SkeletonMergeApply.h`의 `MergeBonesIntoSkeleton(skeleton, meshBones, newModes, result, failure)`가 `MergeBoneHierarchy`(순수 판정 — 6.4절 6) 결과를 스켈레톤에 쓴다: `bones` 교체, `retargetModes`를 결과 본 수로 늘려 더한 본에 `newModes[메시 번호]`(비면 `Animation`)를 둔다. 계층 검증 다음에 `retargetModes` 길이를 본다(`ValidateSkeleton`과 같은 순서·같은 진단 — index 없음). 실패하면 스켈레톤은 그대로다(할당을 먼저 마친다). 스킨 메시 임포트(MA5b3)도 같은 함수를 쓴다.
6. **로드 경로.** 정적 메시·텍스처와 같이 클래스 PostLoad는 쓰지 않는다(5.5.1절 9, AssetSystem 12.15절 2). 스켈레톤을 쓰는 쪽 — 스킨 메시 쿠킹(MA5b2)·클립 쿠킹(MA6b)·애니메이션 평가 — 이 쓰기 전에 `ValidateSkeleton`을 부른다. 13절 MA4b 행의 "로드 경로에서 ValidateSkeleton"은 이 규칙으로 바꾼다.
7. **너겟 무효화.** 병합 저장은 스켈레톤 패키지를 바꾸므로 콘텐츠 해시가 바뀐다. 스켈레톤을 Build로 의존하는 처리기(클립 MA6b — `trackToBone`이 스켈레톤 본 번호다. 저장 때 타입 hook `DependencyDeclarer`로 Build 간선을 선언한다, AssetSystem 16.2절)의 build key에 그 해시가 들어가 다시 쿠킹된다(AssetSystem 10.2절 5). 스킨 메시는 메시 자체 본만으로 쿠킹되어 Build 의존이 없다(6.7.2절 3). 시험은 클립 처리기가 생기는 MA6b에서 한다.
8. **시험.** `AnimationTools/Tests/SkeletonImportTests.cpp`: 새 임포트 필드·pose 정규화·`retargetModes` 기본, 이름·길이 실패(detail), 계층·소켓 검증 실패(진단 index), 재임포트 병합(기존 본 번호·pose 유지, 새 본 추가, 대소문자 이름 바꿈, 사슬 불일치), 편집 보존과 `replaceOnReimport`, 보존 필드 유지, `MergeBonesIntoSkeleton` 실패 시 불변.

### 6.7 MA5b — 스킨 메시 타입·쿠킹·임포트

설계일 2026-11-25(사용자 지시 "SkeletalMesh도 처리해"). MA2b(5.5절)처럼 셋으로 나눈다. MA5b1 = 타입과 로드 검증, MA5b2 = 쿠킹 패킹·처리기, MA5b3 = 임포트 서비스. 정적 메시에서 정한 규칙(5.5절)은 다르게 적지 않으면 그대로 쓴다.

#### 6.7.1 MA5b1 — 타입과 로드 검증

1. **파일 배치.** `Mesh` 모듈에 `Mesh/SkeletalMesh/` 폴더를 둔다: `SkinnedAsset.h`, `SkeletalMesh.h`, `SkeletalMeshSourceLod.h`, `SkinWeightProfileInfo.h`, `SkinnedRenderLod.h`, `SkinnedMorphTarget.h`, `SkinWeightProfileRenderLod.h`, `SkeletalMeshRenderData.h`, `SkeletalMeshValidation.h`(한 파일 한 타입 — 검증 함수는 그 파일). `Mesh`는 `Animation`에 공개 의존한다(`Skeleton`·`BoneHierarchy`·`BoneSocket` — 8절). AssetRuntime에 `PerPlatformBool`(`AssetRuntime/PerPlatformBool.h`, UE `FPerPlatformBool` — 맵 EditorOnly, `PerPlatformInt32`와 같은 모양)을 더한다.
2. **`SkinnedAsset`** = `ROCKS_CLASS(Abstract)` : `StreamableRenderAsset`(UE `USkinnedAsset` — 고유 저장 필드 없음).
3. **`SkeletalMesh` 필드와 기본값**(6.3절, UE `USkeletalMesh` 생성자 — `E/Private/SkeletalMesh.cpp:424`): `skeleton` `ObjectRef<Skeleton>`(Hard), `bones` `BoneHierarchy`(UE `RefSkeleton`), `sourceLods` `Array<SkeletalMeshSourceLod>`(EditorOnly — UE `SourceModels`·`LODInfo`는 쿠킹에 남지만 Rocks는 런타임 값(화면 크기·LOD 전환 여유·섹션 머티리얼)을 쿠킹 LOD로 굽는다), `sectionInfo` `Array<MeshSectionInfoEntry>`(EditorOnly — UE `FSkeletalMeshLODModel::UserSectionsData`, 정적과 같은 (lod, section) 표와 대체 규칙. UE `bRecomputeTangent`·`RecomputeTangentsVertexMaskChannel`·`bDisabled`는 skin cache·섹션 끄기 체계 때 더한다), `virtualGeometry` `VirtualGeometrySettings`(EditorOnly·보존 — UE `NaniteSettings`), `materialSlots` `StructRefArray<MeshMaterialSlot>`(UE `FSkeletalMaterial`과 같은 필드 — `MaterialSlotName`·`ImportedMaterialSlotName`·`MaterialInterface`·`OverlayMaterialInterface`·`UVChannelData`), `sockets` `Array<BoneSocket>`(메시 전용 — 스켈레톤 소켓과 합쳐 쓴다), `skinWeightProfiles` `Array<SkinWeightProfileInfo>`, `positiveBoundsExtension`·`negativeBoundsExtension` `Vec3d`, `minLod` `PerPlatformInt32`(0), `renderData` `SkeletalMeshRenderData`. UE의 `MinQualityLevelLOD`·`DisableBelowMinLodStripping`·LOD 스트리밍 덮어쓰기·`LODSettings`·ray tracing 필드는 그 체계(품질 묶음·스트리밍·RT) 때 더한다. 같이 미루는 것(리뷰 확인): `OverlayMaterial`·`OverlayMaterialMaxDrawDistance`(정적 메시와 함께 — 컴포넌트 쪽 overlay는 D2b에 있다), `ForwardAxis`, `ShadowPhysicsAsset`·`bEnablePerPolyCollision`(물리), `MeshClothingAssets`(옷감), `DefaultMeshDeformer`·`TargetMeshDeformers`(deformer), `SamplingInfo`·`NodeMappingData`·`AssetUserData`, `PostProcessAnimBlueprint`·`PostProcessAnimBPLODThreshold`(Blueprint 제외 — Sparks 대응은 애니메이션 단계). 정적 메시의 `lodGroup`·`numStreamedLods`에 대응하는 UE 스킨 필드는 `LODSettings` 에셋이라 같이 미룬다.
4. **원본 LOD `SkeletalMeshSourceLod`**(UE `FSkeletalMeshLODInfo` + `FSkeletalMeshSourceModel` — `E/Classes/Engine/SkinnedAssetCommon.h`): `hasSource`·`hasBakedSource`·`source`·`bakedSource`(정적과 같다 — 5.5.1절 6), `build` `SkinnedMeshBuildSettings`(MA5a), `reduction` `SkeletalMeshReductionSettings`(UE `FSkeletalMeshOptimizationSettings` — 정적과 다른 struct, 필드·기본값은 UE 생성자: 비율 0.5·절대 삼각형 4·정점 6·용접 0.1·법선 60·정점당 본 4 등. UE는 LOD를 만들 때 기본 LOD 설정 에셋으로 덮어쓰므로(`SetLODSettingsToMesh`) 임포트 서비스 MA5b3이 LOD 설정을 정한다), `screenSize` `PerPlatformFloat`(기본 1.0 — UE `ScreenSize{1.0}`), `lodHysteresis` 0, `lodMaterialMap` `Array<int32>`(섹션 → 슬롯 덮어쓰기, −1 = 그대로), `bonesToRemove`·`bonesToPrioritize` `Array<Name>`, `weightOfPrioritization` 1.0. 생성 LOD 판정 `IsSkeletalMeshLodGenerated(lod, lodIndex)`는 UE `USkeletalMesh::IsReductionActive`(SkeletalMesh.cpp:623)다: 원본이 없으면 참, inline 축소(`reduction.baseLod` == LOD 번호)면 원본 정점·삼각형 수로, 아니면 수를 모르는 것(uint32 최댓값)으로 스킨 축소기 식(SkeletalMeshReductionPlugin.cpp:69 — 비율 기준은 비율·비율 상한, 절대 기준은 수 > 상한)을 쓴다. UE `bHasBeenSimplified`(편집기 축소 캐시)는 없다. 같이 미루는 LOD 정보: `SectionsToPrioritize`·`BakePoseOverride`·`SkinCacheUsage`·`MorphTargetPositionErrorTolerance`·`bAllowCPUAccess`·`bAllowMeshDeformer`·`bBuildHalfEdgeBuffers`·`bSupportUniformlyDistributedSampling`·`VertexAttributes`·`bHasPerLODVertexColors`. 축소·본 제거·bake pose는 **보존**(6.3절) — bake pose(UE `BakePose` 애니메이션 참조)는 클립이 생기는 MA6b 뒤에 더한다.
5. **`SkinWeightProfileInfo`**(UE `FSkinWeightProfileInfo` — `E/Public/Rendering/SkinWeightProfile.h:41`): `name` `Name`, `defaultProfile` `PerPlatformBool`(거짓), `defaultProfileFromLod` `PerPlatformInt32`(0 — 0 이상은 임포트 서비스 MA5b3이 본다). UE `PerLODSourceFiles`는 편집기 원본 경로라 넣지 않는다.
6. **쿠킹 렌더 데이터.** `SkinnedRenderLod`는 `StaticMeshRenderLod`의 필드(`screenSize`·`vertexCount`·`data`·stream·`indexFormat`·`sections`)에 다음을 더한다(RenderBase 10절 skinned 행):
   - `lodHysteresis` float(UE `LODHysteresis`).
   - `influencesPerVertex`(4 = `MeshCookedInfluencesLow`, 8 = `High`), `boneIndices` stream(`UByte4` — bone map 256개 이하, 넘으면 `UShort4`; 정점마다 influencesPerVertex / 4개 원소를 잇는다), `boneWeights` stream(`UByte4Unorm` — 합 255, `useHighPrecisionSkinWeights`면 `UShort4Unorm` — 합 65535, 같은 배치).
   - `boneMap` `Array<uint16>`(LOD 통합 bone map — 결정 R18), `requiredBones` `Array<uint16>`.
   - `morphTargets` `Array<SkinnedMorphTarget{name, deltaCount, deltas stream}>` — 항목마다 (렌더 정점 번호 uint32, 위치 변화량 `Half4`, 법선 변화량 `Half4`) 20 byte(w = 0). 정점 번호 오름차순이다.
   - `skinWeightProfiles` `Array<SkinWeightProfileRenderLod{name, boneIndices stream, boneWeights stream}>` — 기본과 같은 배치·같은 bone map.
   - 섹션 materialIndex는 `lodMaterialMap`을 적용한 값이다(쿠킹이 굽는다 — 런타임은 맵을 갖지 않는다).
   `SkeletalMeshRenderData` = `lods` `Array<SkinnedRenderLod>`, `box`·`sphereRadius`(LOD 0, 확장 없음 — 정적과 같다), `inverseBindMatrices` `BulkDataRef`(메시 본마다 float 16개 — `Mat4d`를 float로 바꾼 행 벡터 규약 행 우선, UE `RefBasesInvMatrix`(`FMatrix44f`)와 같은 값). UE는 로드 때 기준 pose로 다시 계산하는데 Rocks는 쿠킹이 한 번 만든다(결정적 double 계산 — 6.5절 12. 렌더러가 로드 뒤 행렬 계산을 하지 않게).
7. **로드 검증**(5.5.1절 9와 같은 규칙 — 위반은 `CorruptPackage`, PostLoad 없음, 소비자가 부른다. 정적과 같은 검사는 Mesh 내부 공용 `MeshRenderLodChecks.h` 템플릿이다): `ValidateSkeletalMeshRenderData(renderData, boneCount, failure)` — 정적의 LOD·stream·섹션·bounds 규칙에 더해 `influencesPerVertex` ∈ {4, 8}, 번호 형식이 bone map 크기에 맞음(256개 이하 `UByte4`, 넘으면 `UShort4`), 가중치 형식 두 가지 중 하나, 두 stream byte = 정점 수 × (영향 / 4) × 원소 크기, bone map·필요 본이 엄격 증가하고 < `boneCount`, morph `deltas` byte = 개수 × 20, 프로필 stream이 기본과 같은 형식·크기, 배치 순서(위치 → tangent 기저 → UV → 색 → 영향 번호 → 가중치 → 프로필 번호·가중치 → morph target(항목 0개도 구간 자리는 앞 끝 이상) → index). data가 빈 LOD(장치 없는 프로파일)는 `data.rawBytes` 0, 스킨·프로필·morph stream이 비고 `influencesPerVertex`·morph `deltaCount`가 0이다(bone map·필요 본·이름은 남는다). `ValidateSkinnedRenderLodBulk(lod, lodIndex, bytes, failure)` — 영향 번호 < bone map 크기(가중치 0인 빈 칸 (0, 0)은 된다), morph 정점 번호 < 정점 수·엄격 증가, 가중치 합(합 255 또는 65535), index 값 < 정점 수(정적 함수 재사용). 진단 attribute는 프로필·morph target 번호다(기본 영향은 `MeshNoAttribute`). data가 빈 LOD는 빈 byte만 받는다. `ValidateSkeletalMesh(mesh, scratch, failure)` — `ValidateBoneHierarchy(bones)`(위반은 issue `InvalidBoneHierarchy`·`CorruptPackage`), `inverseBindMatrices`(쿠킹 LOD가 있으면 bulk가 있고 본 수 × 64 byte, 에디터 패키지는 빈 ref), 렌더 데이터. 확장 bounds는 `SkeletalMeshExtendedBounds` — UE `USkeletalMesh::CalculateExtendedBounds`(SkeletalMesh.cpp:548)처럼 확장이 0이어도 반지름을 늘 상자 반 대각선으로 다시 만든다(정적과 다르다). 결과 타입은 공용 `MeshBounds`(Mesh/Common — 정적 `StaticMeshBounds`를 개명). 새 `MeshIssue` 여덟: `RenderInfluenceLayout`·`RenderBoneMap`·`RenderMorphTarget`·`RenderSkinWeightProfile`·`RenderBoneIndexValue`·`RenderSkinWeightSum`·`RenderMorphVertex`·`RenderInverseBind`. 영향 칸 상수 `MeshCookedInfluencesLow`·`High`와 `SkinWeightTotal8`은 `Mesh/SkeletalMesh/SkinnedRenderLod.h`로 옮겨 MeshTools가 쓴다(런타임 데이터의 뜻이다).

#### 6.7.2 MA5b2 — 쿠킹 패킹과 쿠킹 처리기

1. **패킹.** `MeshTools/SkinnedMeshPacking.h`의 `PackSkinnedMeshLod(built, settings, sectionInfo, lodMaterialMap, colorFormat, bytes, lod)`가 `SkinnedMeshBuildResult`(6.5절 1)를 6.7.1절 6 배치로 바꾼다. 위치 → tangent 기저 → UV 쌍 → 색 → 영향 번호 → 영향 가중치 → 프로필(번호·가중치 쌍, 원본 순서) → morph target(원본 순서) → index이고 각 시작은 16 byte 정렬이다. 정적 패킹 단계(5.5.2절 1)를 공용으로 쓴다(MeshTools 내부 공용 함수로 옮긴다 — 정적 결과 byte는 그대로). 변화량 half는 Core `Float16` RNE다.
2. **처리기.** `SkeletalMeshCookProcessor`(TypeIdOf<SkeletalMesh>())가 정적 처리기(5.5.2절 4–11)와 같은 흐름이다. LOD 입력·생성 LOD·baked 규칙·색 형식 프로파일·장치 없는 프로파일(stream·`data` 비움, 섹션·bounds·본 목록·`inverseBindMatrices`는 남긴다 — 서버의 소켓·본 계산)·화면 크기(자동 계산 없음 — UE 스킨 메시는 `bAutoComputeLODScreenSize`가 없고 LOD 정보 값을 쓴다)·`minLod` 풀기와 자르기·경고는 같다. 빌드 입력은 `BuildSkinnedMeshLod`(메시 `bones`, LOD의 `build`, 슬롯 이름·섹션 정보 — 정적과 같은 대체 규칙)다. 쿠킹 전에 `ValidateBoneHierarchy(bones)`를 부르고 위반은 `InvalidSkeletonData`다.
3. **스켈레톤 의존.** 쿠킹 결과는 메시 자체 본(`bones`)만으로 정해진다: bone map·필요 본·inverse-bind는 메시 본 번호이고, 메시 본 ↔ 스켈레톤 본 대응은 로드 때 이름으로 만든다(6.3절). 그래서 스킨 메시는 스켈레톤에 **Build 의존을 걸지 않는다**(Hard 참조만 — 쿠킹 폐포에 들어간다). 9.3절의 "스켈레톤 build key"는 빼고(UE도 스킨 메시 파생 데이터 키에 스켈레톤을 넣지 않는다), 스켈레톤 병합 뒤 재쿠킹 시험은 클립(MA6b — `trackToBone`이 스켈레톤 본 번호다)으로 옮긴다. 6.6절 7을 함께 고친다.
4. **너겟.** bucket `mesh`, 지역 키는 정적 키(5.5.2절 10)의 항목에 `SkinnedMeshBuildVersion`, 스킨 설정 여섯(6.3절), 메시 본 계층(본마다 이름 원문 UTF-8·부모·기준 pose 비트), `lodMaterialMap`을 더한다. `inverseBindMatrices`는 본 계층만의 함수라 너겟 밖(쿠킹 때마다 계산 — 본 수 × 행렬 곱 하나라 싸다).
5. **bulk.** LOD마다 덩어리 하나(정적과 같이 4 KiB 정렬), `inverseBindMatrices` 하나(16 byte 정렬).
6. **시험.** 패킹(영향 4·8칸, 8·16비트 번호 경계 256·257개 본, 8·16비트 가중치, morph·프로필 배치·정렬), 처리기(너겟 적중·verify, 장치 없는 프로파일, 생성 LOD·baked, minLod 풀기, `lodMaterialMap` 굽기, 본 계층 위반 실패, 스켈레톤 변경이 너겟 키에 들지 않음), 로드 검증 왕복.
7. **구현에서 정한 점(착수 전 — 2026-11-25).**
   - 공용화(동작 불변): 정적 패킹의 PackWriter·stream 단계(위치 → tangent 기저 → UV → 색, index·섹션)를 MeshTools 내부 `MeshLodPacking.h`(`PackWriter`는 `MeshPackWriter.h`)로, 정적 처리기의 진단·섹션 선택·LOD 번호 옮기기·머리 stream 읽기·쓰기를 `MeshCookCommon.h`로 옮긴다. 경고 struct `StaticMeshCookWarning`은 `MeshCookWarning`(`MeshTools/MeshCookWarning.h`)으로 개명해 둘이 같이 쓴다.
   - 패킹 이름: `PackSkinnedMeshLod(built, settings, sections, lodMaterialMap, colorFormat, profileNames, morphNames, bytes, lod)` — 프로필·morph 이름은 원본 속성 이름에서 접두(`SkinWeights-`·`Morph-`, 짝 없는 법선 target은 `MorphNormal-`)를 뗀 Name이다(처리기가 원본에서 만든다 — 패킹은 순수 함수). 영향 칸은 `built.influencesPerVertex`, 번호는 bone map 위치(`UByte4` 256개 이하·아니면 `UShort4` — 정점마다 칸 순서로 원소 1·2개), 가중치는 `built.weightTotal`이 255면 `UByte4Unorm`, 아니면 `UShort4Unorm`. morph 항목은 (정점 uint32, Δp xyz·0 `Half4`, Δn xyz·0 `Half4`) — Core `Float16` RNE. 섹션 materialIndex는 섹션 번호 i < `lodMaterialMap` 길이이고 값 ≠ −1이면 그 값(UE `LODMaterialMap` — 섹션 번호 기준)이다.
   - 처리기: 입력 선택은 정적과 같은 규칙이고 생성 판정은 `IsSkeletalMeshLodGenerated(lod, 원본 번호)`다. 원본이 없으면 그 LOD를 빼고 `UnsupportedSetting` 경고, 생성인데 baked가 없으면 축소 없이 원본으로 빌드하고 같은 경고다. 화면 크기는 LOD의 `screenSize`를 프로파일로 푼 값(자동 계산 없음), `lodHysteresis`는 그대로, `minLod`는 프로파일로 풀어 쿠킹 LOD 번호로 옮기고 자른다. 빌드 전 `ValidateBoneHierarchy(bones)` 위반은 `InvalidSkeletonData`(detail = 본). inverse-bind는 쿠킹 LOD 0의 빌드 결과(본 계층만의 함수라 LOD마다 같다 — 6.5절 12)를 float로 바꿔 쓴다. 적중이면 원본을 읽지 않으므로 inverse-bind도 너겟 값에 둔다(part 0 머리 다음 값 — 비지 않는 메시는 늘 있다). 장치 없는 프로파일도 inverse-bind를 쓴다(서버의 본 계산).
   - 너겟 값: part 0 = 머리(형식 버전 `SkeletalMeshNuggetHeaderVersion`, 정적 머리 항목 + LOD마다 `lodHysteresis`는 빼고(너겟 밖 값) 영향 칸·영향 stream 둘·bone map·필요 본·프로필(이름 원문·stream 둘)·morph(이름 원문·항목 수·stream)), part 1 = inverse-bind byte, part 2.. = LOD 덩어리(장치가 있을 때). 이름은 원문 UTF-8로 두고 읽을 때 intern한다. 적중 검증은 머리 읽기 + `ValidateSkeletalMeshRenderData` + LOD마다 `ValidateSkinnedRenderLodBulk`다.
   - 너겟 키: 정적 키 항목(유효 빌드 설정은 `SkinnedMeshBuildSettings` 리플렉션 값) + `SkinnedMeshBuildVersion` + 메시 본(본마다 이름 원문·부모·pose 비트) + LOD마다 `lodMaterialMap`. 스켈레톤은 넣지 않는다(3).

#### 6.7.3 MA5b3 — 임포트 서비스

1. **API.** `MeshTools/SkeletalMeshImport.h`의 `ImportSkeletalMesh(context, output, spec, failure)`. 명세는 정적 메시 명세(5.5.3절 2)의 LOD·슬롯·섹션 정보·소켓(본 이름 포함 — `BoneSocket`)·bounds 확장·`minLod`에 메시 본(`SkeletonImportBone` 목록 — 6.6절), LOD별 스킨 설정·`lodHysteresis`·`lodMaterialMap`, 가중치 프로필 정보, 스켈레톤(아래 2)을 더한다. 검증 순서·저장·재임포트 보존 규칙은 5.5.3절 3·4·6을 따른다(스킨 원본 검증은 `boneCount` = 메시 본 수).
2. **스켈레톤.** 명세의 `skeleton`은 셋 중 하나다: (a) 기존 스켈레톤 `AssetId`, (b) 이번 묶음 임포트의 다른 구성원(`MemberAsset` — 교환 경로가 스켈레톤과 메시를 함께 임포트할 때), (c) 없음 — 서비스가 보조 출력 `Skeleton`(`Output("Skeleton", TypeIdOf<Skeleton>())`)을 만들어 메시 본으로 채운다(UE "스켈레톤을 만들며 임포트"). (a)·(b)는 `MergeBoneHierarchy`로 판정해 메시 본이 모두 이미 있으면 그대로 참조한다.
3. **열린 결정 — 기존 스켈레톤에 본 더하기.** 메시 본이 기존 스켈레톤에 없으면 UE는 스켈레톤을 고쳐 저장한다(`MergeAllBonesToBoneTree`). 지금 임포트 계약(`AssetImportContext::OpenAsset`)은 다른 에셋을 Hard 참조로 열 뿐 고쳐 저장하지 못한다. 정공법은 계약에 "다른 에셋을 고쳐 출력과 함께 원자적으로 저장"(가칭 `EditAsset`)을 더하는 것이다(AssetSystem 7.1절 19의 임포트 실행·저장 단계 변경 — 에셋 세션 영역). MA5b3 착수 때 사용자에게 확인한다. 그 전까지의 동작으로 두는 것: 본이 모자라면 `SkeletonMismatch`(issue `MissingSkeletonBones` — 새 진단 세부, detail = 첫 메시 본)로 실패하고 스켈레톤 재임포트(6.6절)로 먼저 합치게 한다.
4. **시험.** 정적 메시 임포트 시험(5.5.3절)과 같은 범위 + 메시 본 검증·스켈레톤 세 경우·본 모자람 실패.
