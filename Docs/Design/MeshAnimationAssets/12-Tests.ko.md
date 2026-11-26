## 12. 시험

에셋 시스템 시험 원칙(변조·실패 주입·취소·동시성·퍼징까지 넓게 시험한다)을 따른다.

- **원본 메시**: 위상·속성 불변식별 위반 fixture(번호 범위, 중복 변, 둘레 3 미만, 삼각형 수 불일치, 비유한 위치, offset 비단조, 희소 번호 중복·역순·범위 밖, Name 길이 넘침, 대소문자만 다른 속성 이름, boneIndex 범위 밖). 코너 공유·퇴화 삼각형이 있는 정상 메시. 저장·로드 왕복의 비트 일치. bulk 길이 변조·잘림·SHA 불일치(`CorruptPackage`). 무작위 위상·속성 퍼징(검증이 오류를 내거나 통과하고, 통과한 것은 빌드가 범위 검사 단언에 걸리지 않고 끝남). morph 희소 저장의 `-0.0` 보존.
- **빌드**: 결정성(같은 입력 두 번 — 스레드는 쓰지 않으므로 "스레드 수 바꿔서"는 5.4절 13대로 대상이 없다). 용접(속성 하나만 다른 코너는 합치지 않음). 퇴화 삼각형 제거 on/off와 UE 판정식(일직선 위 세 점은 남김). 슬롯 대응·대체 경고. `buildScale` 음수 성분(winding 유지, 경고). MikkTSpace 참조 구현과의 대조(공개 시험 메시, 패치 전후 차이 상한).
- **스켈레톤·스킨**: 계층 불변식, 병합 성공·부모 사슬 실패, 영향 줄이기 결정성(가중치 동률), 정규화 잔차. "병합 뒤 클립 재쿠킹(너겟 무효화)"은 MA6b 시험으로 넘긴다(클립 처리기·쿠커가 필요하다 — 스킨 메시는 스켈레톤 Build 의존이 없다, 6.6절 7·6.7.2절 3). "palette 분할 경계"는 섹션을 본 수로 나누지 않으므로(RenderBase R18 — 6.3절) 대상이 없다.
- **클립**: 상수 트랙 접기의 비트 보존, 재샘플링(원본 비율 = 목표 비율이면 키 그대로), 상한 초과, 스켈레톤에 없는 트랙, 커브 왕복, 이벤트 정렬.

### 12.1 요구 대조표(MA1 — 원본 메시)

시험은 EngineTests의 `MeshSource_*`(`Engine/Source/Runtime/MeshSource/Tests`, 52개)와 AssetRuntime의 `AssetPackage_MemoryBulkSource*`(3개)다. 패키지 왕복은 fixture 에셋 `TestMeshAsset`(AssetTestFixtures `MeshAssets.h` — `MeshSourceData` 필드·배열·EditorOnly 필드)으로 실제 패키지를 쓰고 다른 Registry에 로드한 뒤 `MeshSource::Load`를 부른다. 불변식 위반 시험은 진단 코드와 세부 규칙(`MeshIssue`)·위치를 함께 대조한다.

| 요구 | 시험 |
|---|---|
| 3.1 번호 범위(코너·변·다각형·삼각형) | `MeshSource_ValidRejectsIndexRanges`, `MeshSource_LoadRejectsTopologyBytes`(로드 중 CorruptPackage) |
| 3.1 코너 공유·퇴화 삼각형·같은 코너 반복 정상 | `MeshSource_ValidAcceptsSharedAndDegenerate`, `MeshSource_TopologyQueries`, `MeshSource_TopologyRepeatedCorners` |
| 3.1 변: 같은 정점·중복(순서만 다른 것 포함)·작은 번호 먼저 | `MeshSource_ValidRejectsBadEdges`, `MeshSource_AddElementsKeepsShape`, `MeshSource_LoadRejectsTopologyBytes`(EdgeOrder) |
| 3.1 둘레 ≥ 3·둘레 변 존재·퇴화 변 건너뜀 | `MeshSource_ValidRejectsBadPolygons`, `MeshSource_ValidAcceptsSharedAndDegenerate` |
| 3.1 다각형별 삼각형 수·둘레 소속 | `MeshSource_ValidRejectsBadTriangles` |
| 3.1 위치 유한값(MA-D9) | `MeshSource_ValidRejectsNonFinitePosition` |
| 3.1 요소 수 ≤ `MeshMaxElements`·묶음 수 ≤ `MeshMaxPolygonGroups`(경계 포함, 여섯 종류 모두 큰 할당 없이 — 3.7절 12) | `MeshSource_LoadRejectsFormatFields`, `MeshSource_LoadBoundsElementAllocations` |
| 3.2 엔진 이름 속성 형태·대소문자·접두 | `MeshSource_ValidRejectsEngineAttributeShapes`, `MeshSource_LoadRejectsCaseOnlyDuplicateNames` |
| 3.2 기본값(Color 흰색)·속성 추가 규칙 | `MeshSource_AttributeDefaults`, `MeshSource_AddAttributeRejectsBadArguments`, `MeshSource_AddAttributeNamesAreUniquePerElement` |
| 3.2 스킨 가중치 상한·같은 본·boneIndex 범위 | `MeshSource_ValidRejectsAttributeValues`, `MeshSource_LoadRejectsInfluenceBytes` |
| 3.2 정규화 잔차·동률 | `MeshSource_NormalizeWeightsResidual`, `MeshSource_SanitizeNormalizesWeights` |
| 3.2 morph 희소 저장, `-0.0` 보존 | `MeshSource_SparseKeepsNegativeZero`, `MeshSource_StoreLoadRoundTrip`, `MeshSource_LoadRejectsSparseBytes`(+0.0 항목) |
| 3.3 값 정리와 경고 수, 실패 시 무변경 | `MeshSource_SanitizeFixesValues`, `MeshSource_SanitizeFailureKeepsMesh`, `MeshSource_SanitizeWarningAllocation` |
| 3.4 Load·Store 왕복(쿠킹·에디터·Zstd)·비트 일치 | `MeshSource_StoreLoadRoundTrip`, `MeshSource_StoreIsDeterministicAndResaveIdentical`, `MeshSource_SeveralSourcesShareBulk`, `MeshSource_EmptyMeshRoundTrip` |
| 3.4 포맷 필드 하나씩 변조 | `MeshSource_LoadRejectsFormatFields`, `MeshSource_LoadRecordNameTokens` |
| 3.4 bulk 길이 변조·잘림·SHA 불일치 | `MeshSource_LoadRejectsRefMismatch`, `MeshSource_PackageBulkTamperIsCorrupt`, `MeshSource_LoadRejectsFixedValueBytes`, `MeshSource_LoadRejectsNameValueBytes` |
| 3.4.1 값 타입별 배치·길이·단조·범위 | `MeshSource_LoadRejectsFixedValueBytes`, `MeshSource_LoadRejectsNameValueBytes`, `MeshSource_LoadRejectsInfluenceBytes`, `MeshSource_LoadRejectsSparseBytes` |
| 3.5 이름 유일(대소문자만 다른 이름)·Name 값 내용 규칙 | `MeshSource_LoadRejectsCaseOnlyDuplicateNames`, `MeshSource_ValidRejectsAttributeValues` |
| 3.7 2 `MemoryBulkSource` | `AssetPackage_MemoryBulkSourceEntries`, `AssetPackage_MemoryBulkSourceFailures`, `AssetPackage_MemoryBulkSourceWrites` |
| 3.7 9 가변 길이 값 덮어쓰기·별칭 | `MeshSource_NameValuesOverwriteAndAlias`, `MeshSource_InfluencesOverwriteAndAlias` |
| 3.7 11 위상 조회 | `MeshSource_TopologyQueries`, `MeshSource_TopologyRepeatedCorners`, `MeshSource_TopologyEmpty` |
| 9.1 위상 blob 배치 | `MeshSource_LoadRejectsTopologyBytes`, `MeshSource_EmptyMeshRoundTrip` |
| 9.2 결정적 정렬 | `MeshSource_StoreIsDeterministicAndResaveIdentical`, `MeshSource_StoreLoadRoundTrip`, `MeshSource_LoadRejectsFormatFields`(AttributeOrder) |
| 11 진단 코드 등록·이름·4.6절 변환 | `MeshSource_DiagnosticNames`, `AssetPackage_DiagnosticNames`, `MeshSource_ValidReportsAttributeIndex` |
| 12 퍼징(고정 seed) | `MeshSource_FuzzRandomMeshes`(600개 — 통과분은 위상 조회 전체·일부 패키지 왕복), `MeshSource_FuzzStoredBytes`(160개 bulk 변조) |
| 할당 실패 주입(강한 보장) | `MeshSource_EditFailuresKeepState`, `MeshSource_EditFailuresFromEmpty`, `MeshSource_FirstAttributeFailureAndRegistration`, `MeshSource_ValidScratchFailures`, `MeshSource_StoreFailures`, `MeshSource_LoadFailures`, `MeshSource_TopologyBuildFailures`, `MeshSource_SanitizeWarningAllocation` |

Coverage(2026-11-20, clang source-based — Rocks.Build `Coverage` 구성, Swift 6.3.3 툴체인의 llvm-profdata·llvm-cov, 측정 방법은 `Build/MeasureCoverage.ps1`과 같고 출력만 이 작업 전용 디렉터리에 두었다): 시험 `MeshSource`(52개)·`AssetPackage_MemoryBulkSource`(3개)·`AssetPackage_DiagnosticNames`(1개) 모두 통과. 대상은 MeshSource Private·Public 14개 파일과 AssetRuntime `MemoryBulkSource.h/.cpp`다.

| 파일 | 줄 | 분기 |
|---|---|---|
| MeshSource.cpp | 472/488 | 265/307 |
| MeshSourceStorage.cpp | 681/684 | 335/342 |
| MeshSourceValidation.cpp | 263/263 | 150/151 |
| MeshSourceSanitize.cpp | 178/178 | 104/108 |
| MeshSourceTypes.cpp | 231/231 | 173/174 |
| MeshTopologyIndex.cpp | 250/250 | 86/90 |
| MemoryBulkSource.cpp | 26/28 | 11/13 |
| 그 밖(MeshSourceModule.cpp·MeshSourceInternal.h·공개 헤더 5개·MemoryBulkSource.h) | 87/87 | 2/2 |
| 합계 | 2188/2209(99.05%) | 1126/1187(94.86%), 함수 157/157 |

닿지 않은 분기 61개의 사유:
- 47개는 불변식 검사 `ROCKS_ASSERT`의 실패 쪽이다(용량을 미리 잡은 뒤의 실패할 수 없는 추가, 공개 접근 함수의 계약 위반 — 매크로 전개 안이라 `-show-expansions`로만 보인다).
- 10개는 메모리 상한이라 시험으로 만들 수 없다: 요소 종류마다 `MeshMaxElements`(2^31 − 1)개에 닿은 뒤의 `Add*` 거부 7개(`AddPolygon`의 둘레 전체 길이 포함), 가변 길이 값 저장소의 uint32 offset 넘침(Name·영향 쓰기, Name 로드) 3개 — 모두 수 GiB–수십 GiB를 실제로 할당해야 한다. `MemoryBulkSource::Add`의 bulkId 소진(2^32 − 2 entry)도 같은 이유다(1개).
- 1개는 모듈 내부 열거 `EngineName`의 열거 밖 값(switch의 암묵 default)으로, 모듈 안 호출자가 만들지 않는다.
- 2개는 `BoneInfluence`의 defaulted `operator==` 거짓 쪽이다. `MeshSource_NormalizeWeightsResidual`이 본이 다른 경우와 가중치만 다른 경우를 런타임 값으로 모두 부르는데도 기록이 0으로 남는다(헤더 constexpr 기본 비교의 계측 기록 방식으로 보이며 원인은 확인하지 못했다).

입력 변조·실패 주입으로 닿을 수 있는 오류 분기는 모두 시험으로 닿게 했다. 측정 중 닿을 수 없음을 확인한 분기는 구조를 고쳤다: 저장 후 정렬 비교의 빈 이름 경로(레코드 이름은 비지 않는다), 64비트에서 늘 거짓인 크기 검사, 저장 경로의 `Invalid` 값 타입 case, 미리 잡은 변 표 삽입의 실패(단언으로), 엔진 규칙의 최소 채널(늘 1 이상), 속성 이름 내용 검사(만드는 두 길이 모두 지킨다 — 3.7절 4).

개정 13(리뷰 반영) 뒤 다시 잰 값: `MeshSource.cpp` 줄 474/488·분기 266/307(`AddPolygonGroup`의 상한 거부가 `MeshMaxPolygonGroups`로 닿게 되어 위 메모리 상한 10개 가운데 하나가 빠졌다), `MeshSourceStorage.cpp` 줄 688/691·분기 339/346(Name 값 bulk 최소 길이 판정 포함, 남은 미달은 위와 같다). MeshTools `MeshOverlappingCorners.cpp`는 줄 76/76·분기 38/38, Animation `SkeletonMerge.cpp`는 줄 133/133·분기 64/64, `ClipPlayback.cpp`는 줄 320/320·분기 154/155(상수 접기 단언)다.

### 12.2 요구 대조표(MA2a — 정적 메시 빌드 핵심)

시험은 EngineTests의 `StaticMeshBuild_*`(`Engine/Source/Developer/MeshTools/Tests`, 26개)와 Core의 `DeterministicMath_AccuracyAgainstReferenceTables`(Acos 표)·`DeterministicMath_AcosSpecialValuesAndDigest`다. 진단 이름표는 `AssetPackage_DiagnosticNames`·`MeshSource_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 5.2 1단계 원본 검증(실패는 검증 진단 그대로) | `StaticMeshBuild_RejectsInvalidInputs`, `StaticMeshBuild_FuzzSourceMeshes` |
| 5.2 2단계 값 정리(사본에만, 경고는 원본 속성 번호) | `StaticMeshBuild_SanitizeWarningsUseSourceAttributes`, `StaticMeshBuild_RecomputesOnlyZeroCorners`(NaN 법선) |
| 5.2 3단계 삼각형 기저(UV 경로·UV 없는 Duff·Frisvad 경로) | `StaticMeshBuild_QuadBasics`, `StaticMeshBuild_TangentsWithoutUvs`, `StaticMeshBuild_AveragedTangentsMatchConvention` |
| 5.2 4단계 겹치는 코너(위치 임계값 경계 안·밖, 임계값 0, UE 가중합 정렬 키 — 평면 격자 비교 횟수, 5.4절 16) | `StaticMeshBuild_WeldPositionThreshold`, `StaticMeshBuild_OverlapKeySpreadsFlatMeshes` |
| 5.2 5단계 0 벡터 코너만 재계산·가중 법선·hard edge·대각선 soft·MikkTSpace | `StaticMeshBuild_RecomputesOnlyZeroCorners`, `StaticMeshBuild_WeightedNormals`, `StaticMeshBuild_HardEdgeSplitsNormals`, `StaticMeshBuild_NgonDiagonalsAreSoft`, `StaticMeshBuild_MikkTSpaceMatchesReference` |
| 5.2 6·7단계 보존 설정 경고 | `StaticMeshBuild_PreservedSettingsWarn` |
| 5.2 8단계 buildScale(음수 성분 winding 유지·경고 홀짝, 비균일 역전치, 잘못된 값) | `StaticMeshBuild_NegativeBuildScale`, `StaticMeshBuild_NonUniformScaleNormals`, `StaticMeshBuild_RejectsInvalidInputs` |
| 5.2 9단계 퇴화 on/off·UE 판정식(일직선 세 점 남김)·용접(속성 하나만 다르면 안 합침) | `StaticMeshBuild_DegenerateTriangles`, `StaticMeshBuild_WeldPositionThreshold`, `StaticMeshBuild_WeldRequiresEveryAttribute` |
| 5.2 10단계 섹션(묶음 순서·빈 묶음)·슬롯 대응·대체 경고·섹션 정보 | `StaticMeshBuild_SectionsFollowGroups`, `StaticMeshBuild_SlotMatchingAndSectionInfo` |
| 5.2 11단계 원본 순서 | `StaticMeshBuild_QuadBasics`, `StaticMeshBuild_NgonDiagonalsAreSoft`, `StaticMeshBuild_SectionsFollowGroups` |
| 5.2 12단계 bounds·확장 | `StaticMeshBuild_BoundsAndExtension`, `StaticMeshBuild_QuadBasics` |
| 5.2 결정성(두 번, 호출 전 FTZ, 비정규 중간값) — 스레드 없음(5.4절 13) | `StaticMeshBuild_DeterministicBytes`, `StaticMeshBuild_FuzzSourceMeshes` |
| n-gon·코너 공유·빈 메시·UV 채널·색 | `StaticMeshBuild_NgonDiagonalsAreSoft`, `StaticMeshBuild_EmptyMeshes`, `StaticMeshBuild_UvChannelsAndColors` |
| 11 `MaterialSlotFallback`·`NegativeBuildScale`·`UnsupportedSetting`·`InvalidSetting` | `StaticMeshBuild_SlotMatchingAndSectionInfo`, `StaticMeshBuild_NegativeBuildScale`, `StaticMeshBuild_PreservedSettingsWarn`, `StaticMeshBuild_RejectsInvalidInputs` |
| 4.1 구현 필드 `recomputeNormals`·`recomputeTangents` | `StaticMeshBuild_RecomputesOnlyZeroCorners`, `StaticMeshBuild_DeterministicBytes` |
| 4.1 `useMikkTSpace`·`computeWeightedNormals`·`removeDegenerates` | `StaticMeshBuild_AveragedTangentsMatchConvention`, `StaticMeshBuild_MikkTSpaceMatchesReference`, `StaticMeshBuild_WeightedNormals`, `StaticMeshBuild_DegenerateTriangles` |
| 4.1 `useHighPrecisionTangentBasis`·`useFullPrecisionUVs`(패킹은 MA2b — 여기서는 저장·경고 없음) | `StaticMeshBuild_SettingFieldTable`, `StaticMeshBuild_PreservedSettingsWarn` |
| 4.1 `buildScale` | `StaticMeshBuild_NegativeBuildScale`, `StaticMeshBuild_NonUniformScaleNormals`, `StaticMeshBuild_BoundsAndExtension` |
| 4.1 필드 표·UE 기본값·리플렉션 필드 이름 | `StaticMeshBuild_SettingFieldTable` |
| MikkTSpace 패치 전 원본 대조(정육면체·구·거울 UV 띠·울퉁불퉁한 격자, 상한 성분 1e-6, 부호 일치) | `StaticMeshBuild_MikkTSpaceMatchesReference` |
| 할당 실패 주입(결과 그대로, MikkTSpace 느린 경로는 같은 결과) | `StaticMeshBuild_AllocationFailures` |
| 고정 seed 퍼징(MA1 생성기 400개) | `StaticMeshBuild_FuzzSourceMeshes` |
| 아주 큰 값(좌표·UV·원본 법선 넘침)·아주 작은 삼각형 — 결과 유한, byte 결정적 | `StaticMeshBuild_ExtremeValuesStayFinite` |
| Core `Acos`(기준값 1 ulp·특수값·digest) | `DeterministicMath_AccuracyAgainstReferenceTables`, `DeterministicMath_AcosSpecialValuesAndDigest` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`과 같은 방법, Swift 6.3.3 툴체인의 llvm-profdata·llvm-cov): 시험 `StaticMeshBuild`(26개)·`DeterministicMath`(7개) 모두 통과. 메인 트리가 다른 세션의 리플렉션 작업 중이라 빌드되지 않아 검증은 `origin/feature/engine-runtime`(90972993)에 이 작업을 얹은 임시 worktree에서 했다.

| 파일 | 줄 | 분기 |
|---|---|---|
| StaticMeshBuilder.cpp | 517/521 | 261/264 |
| MeshTangentBasis.cpp | 442/442 | 217/220 |
| MeshOverlappingCorners.cpp | 70/70 | 38/38 |
| MeshMikkTSpace.cpp | 79/79 | 10/11 |
| MeshBuildInternal.h | 78/78 | 37/40 |
| MeshBuildSettings.cpp·MeshModule.cpp·StaticMeshBuilder.h | 121/121 | 114/114 |
| 합계(Mesh·MeshTools) | 1307/1311(99.7%) | 677/687(98.5%), 함수 전부 |

닿지 않은 분기 10개의 사유:
- 3개는 메모리 상한이다: 삼각형 수 상한(`MeshMikkTSpaceMaxTriangles`·index 수 uint32 — 수억 개 삼각형) 둘, 원본 사본의 `AddPolygonGroup` 실패(묶음은 속성이 없으면 할당하지 않아 `MeshMaxElements`에서만 실패) 하나.
- 3개는 UE 식을 그대로 옮긴 판정의 닿지 않는 쪽이다(`BasisWithUv`): `CreateOrthonormalBasis`는 tangent가 있으면 쌍법선을 외적으로 채우고 법선은 단위 벡터로 끝나므로 "tangent는 있는데 쌍법선이 0"과 "직교화 뒤 법선이 0"이 생기지 않는다.
- 3개는 성분별 단락의 뒤쪽 성분이다(`IsFiniteVector`·`ContainsNaN`의 y·z 성분만 비유한) — 시험의 넘침 입력은 x 성분부터 넘친다.
- 1개는 `ROCKS_ASSERT`의 실패 쪽(MikkTSpace 실패가 할당 실패뿐이라는 단언)이다.

`DeterministicMath.cpp`의 `Acos`·`AsinTail`·`SqrtApproximation`은 줄·분기 모두 닿았다(그 파일의 나머지 미달은 기존 함수의 특수값 분기로 이 작업 범위 밖이다). 측정 중 닿을 수 없음을 확인한 분기는 구조를 고쳤다: MikkTSpace 해제 훅의 null 검사(원본은 할당에 성공한 포인터만 넘긴다), 할당 오류의 "처음 것만" 조건, 지역 진단 null 검사, `GroupByUv`의 빈 묶음, `HasAllTangents`의 NaN·x·z 판정(입력이 유한하고 x·z가 0이면 y도 0), `SqrtApproximation`의 양수 지수 쪽(z < 1). 할당 실패 주입이 찾은 MikkTSpace 원본 버그는 5.4절 8에 적었다.

### 12.3 요구 대조표(MA4a — 본 계층·스켈레톤·병합 판정)

시험은 EngineTests의 `Skeleton_*`(`Engine/Source/Runtime/Animation/Tests`, 15개)다. 진단 이름표는 `AssetPackage_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 6.1 계층 불변식(정상: 빈·루트 하나·가지, 이름 찾기 대소문자 무시) | `Skeleton_HierarchyAcceptsValid` |
| 6.1 규칙마다 하나씩 어기기(pose 길이·루트 부모·두 번째 루트·자기·뒤·−2 부모·None·잘못된 토큰·대소문자만 다른 중복·NaN 이동·크기·회전·정규화 안 된 회전, 허용 경계 안쪽) | `Skeleton_HierarchyRejectsEachRule` |
| 6.1 본 수 상한 `MeshMaxBones`(경계 포함) | `Skeleton_HierarchyBoneLimit`, `Skeleton_MergeRejectsInvalidInputs`(병합 결과) |
| 6.1 기준 pose 정규화 | `Skeleton_NormalizeReferencePose` |
| 6.2 스켈레톤 필드 하나씩 변조 거부(리타깃 방식·가상 본·소켓·커브·슬롯 묶음·리타깃 원본, 다른 pool 이름) | `Skeleton_ValidateRejectsEachField` |
| 6.2 저장 → 로드 → 재저장 byte 일치, 두 번 쓴 byte 일치(결정성) | `Skeleton_PackageRoundTripResaveIdentical` |
| 6.2 병합 거부(본 없는 메시 — `EmptyMeshHierarchy`)·병합 성공(빈 스켈레톤, 없는 본 더하기·대응표·pose, 대소문자 이름 바꾸기, 결정성) | `Skeleton_MergeIntoEmptySkeleton`, `Skeleton_MergeAddsMissingBones`, `Skeleton_MergeCaseOnlyRename` |
| 6.2 부모 사슬 실패(`SkeletonMismatch` — 중간 본 빠짐·조상 사슬 다름·루트 다름) | `Skeleton_MergeRejectsParentChain` |
| 병합 입력 검증(role)과 결과 상한 | `Skeleton_MergeRejectsInvalidInputs` |
| 할당 실패 주입(결과 그대로·같은 결과) | `Skeleton_MergeAllocationFailures`, `Skeleton_ValidateAllocationFailures` |
| 고정 seed 퍼징(무작위 계층 500개 — 검증·스켈레톤에서 만든 메시와 병합·무관한 계층 병합) | `Skeleton_FuzzHierarchiesAndMerges` |
| 11 `InvalidSkeletonData`·`SkeletonMismatch` 등록·이름·4.6절 변환 | `Skeleton_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`): 시험 `Skeleton_`(15개) 모두 통과. Animation Private·Public 8개 파일 줄 627/627(100%), 분기 333/334(99.70%), 함수 47/47. 닿지 않은 분기 1개는 이름 표 이진 탐색(`FindNamed`)의 "다른 pool 이름" 비교 쪽이다 — 병합·검증의 이름 조회는 같은 pool이라는 전제(6.4절 6) 안에서만 부르므로 domain이 다른 키가 오지 않는다(정렬 쪽 domain 비교는 다른 pool 이름을 섞은 계층 시험이 닿는다). 측정 중 닿을 수 없음을 확인한 구조는 고쳤다: 중복 이름 찾기의 "묶음 첫 원소까지 거슬러 오르기"(묶음 안이 번호 순이라 두 번째 원소의 바로 앞이 첫 원소다), 검증 단계 사이에 이름 표를 다시 써 용량이 남아 할당이 일어나지 않던 경로(단계마다 새 표).

### 12.4 요구 대조표(MA5a — 스킨 빌드 핵심)

시험은 EngineTests의 `SkinnedMeshBuild_*`(`Engine/Source/Developer/MeshTools/Tests`, 16개)다. 정적 빌드 시험(`StaticMeshBuild_*` 26개)은 단계 함수를 옮긴 뒤에도 그대로 통과한다(결과 byte 불변). 진단 이름표는 `MeshSource_DiagnosticNames`·`AssetPackage_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 6.3 `SkinnedMeshBuildSettings` 여섯 필드·UE 기본값·리플렉션 필드(상속 펼침)·보존 판정(buildScale 포함) | `SkinnedMeshBuild_SettingFieldTable`, `SkinnedMeshBuild_PreservedSettingsWarn`, `StaticMeshBuild_SettingFieldTable` |
| 6.5절 3 스킨 빌드의 buildScale 미적용·`UnsupportedSetting`·`NegativeBuildScale` 없음 | `SkinnedMeshBuild_PreservedSettingsWarn` |
| 6.5절 4·5 설정·본 계층·원본 입력 오류(임계값 NaN·무한·음수, 상한 음수, 본 없음·계층 위반·boneIndex 범위·합 0, 실패 시 결과 그대로) | `SkinnedMeshBuild_RejectsInvalidInputs` |
| 6.5절 5 값 정리 경고의 원본 속성 번호(기본·프로필) | `SkinnedMeshBuild_RejectsInvalidInputs` |
| 6.5절 6 스킨 임계값(위치·UV·법선 경계 안·밖, 위치 0)·번호가 겹친 삼각형 제외(세 쌍)·removeDegenerates와 정적 빌드의 차이 | `SkinnedMeshBuild_ThresholdsAndCollapsedTriangles` |
| 6.5절 7 영향 줄이기(가중치 0 제외, 동률은 본 번호, 잔차는 가장 큰 영향, 상한 1·4·8·100, 12개 원본, 루트 대체) | `SkinnedMeshBuild_ReduceInfluences`, `SkinnedMeshBuild_InfluencesAndBoneMap` |
| 6.5절 7 `MissingInfluences`·`InfluencesTrimmed`(정점 수·첫 정점·속성 번호·순서) | `SkinnedMeshBuild_InfluencesAndBoneMap`, `SkinnedMeshBuild_WeightProfiles` |
| 6.5절 8 용접의 스킨 항목(기본·프로필 영향 값과 개수, morph 위치 변화량, MorphNormal, 비유한 morph를 0으로) | `SkinnedMeshBuild_WeldComparesSkinData` |
| 6.5절 9 가중치 프로필(원본 순서, 프로필 본이 bone map에, 빠진 정점은 루트) | `SkinnedMeshBuild_WeightProfiles` |
| 6.5절 10 칸 수 4/8, 8비트 잔차(양·음)·0이 된 영향 유지, 16비트 그대로, 빈 칸 (0, 0) | `SkinnedMeshBuild_QuantizeWeights8`, `SkinnedMeshBuild_InfluencesAndBoneMap` |
| 6.5절 11 bone map(오름차순·map 위치 번호)·필요 본(메시 본 전체) | `SkinnedMeshBuild_InfluencesAndBoneMap`, `SkinnedMeshBuild_BindPose` |
| 6.5절 12 inverse-bind(독립 계산과 bit 일치, 역 × component ≈ 단위)·`SingularBindPose` | `SkinnedMeshBuild_BindPose`, `SkinnedMeshBuild_RejectsInvalidInputs` |
| 6.5절 13 morph 법선 재계산(옮긴 메시를 처음부터 빌드한 법선과 bit 일치, 가중 법선, 위치 문턱으로 법선만 담기) | `SkinnedMeshBuild_MorphTargets` |
| 6.5절 13 MorphNormal 짝(대소문자 무시)·짝 없는 MorphNormal·문턱 경계(같으면 뺌)·법선 추적 여부·빈 target·비유한 항목 경고 | `SkinnedMeshBuild_MorphAttributesAndThreshold` |
| 6.5절 15 정적 빌드와 렌더 부분 일치(정육면체·구·공유 메시, 설정 8가지) | `SkinnedMeshBuild_RenderPartMatchesStaticBuild` |
| 6.5절 17 결정성(두 번, 호출 전 FTZ, 임시 할당기 따로) | `SkinnedMeshBuild_DeterministicBytes`, `SkinnedMeshBuild_FuzzSourceMeshes` |
| 할당 실패 주입(결과 그대로·같은 결과, 경고를 처음 할당하는 단계 7가지) | `SkinnedMeshBuild_AllocationFailures` |
| 고정 seed 퍼징(MA1 생성기 400개 — 원본 진단 일치, 영향·bone map·morph 순서 불변식, 두 번 빌드 byte 일치) | `SkinnedMeshBuild_FuzzSourceMeshes` |
| 11 `InfluencesTrimmed`·`MissingInfluences` 등록·이름, 접두 판정 `MatchesMeshAttributePrefix` | `MeshSource_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`, 필터 `SkinnedMeshBuild`·`StaticMeshBuild`): 시험 42개 모두 통과. 대상은 Mesh·MeshTools Private·Public이다.

| 파일 | 줄 | 분기 |
|---|---|---|
| SkinnedMeshBuilder.cpp | 664/666 | 320/328 |
| MeshLodBuild.cpp | 432/434 | 249/250 |
| StaticMeshBuilder.cpp | 157/159 | 62/64 |
| MeshTangentBasis.cpp | 442/442 | 217/220 |
| MeshBuildInternal.h | 81/81 | 34/36 |
| MeshMikkTSpace.cpp·MeshOverlappingCorners.cpp | 149/149 | 48/49 |
| MeshBuildSettings.cpp·MeshModule.cpp·공개 헤더 둘 | 176/176 | 168/168 |
| 합계(Mesh·MeshTools) | 2101/2107(99.72%) | 1098/1115(98.48%), 함수 125/125 |

닿지 않은 분기 17개의 사유:
- 6개는 `ROCKS_ASSERT`의 실패 쪽이다(`ReduceBoneInfluences`의 인자 계약 넷, `QuantizeSkinWeights8`의 잔차 범위 둘 — 합이 65535이고 영향이 8개 이하면 첫 영향은 32 이상이고 잔차는 ±4 안이다).
- 5개는 메모리 상한이다: 두 빌드의 삼각형 수 상한(index 수 uint32·`MeshMikkTSpaceMaxTriangles`) 넷, 원본 사본의 `AddPolygonGroup` 실패 하나(12.2절과 같다).
- 6개는 12.2절에 적은 MA2a 그대로다(`BasisWithUv`의 UE 식 셋, `IsFiniteVector`의 y·z 성분 단락 둘, MikkTSpace 단언 하나).

측정 중 닿을 수 없음을 확인한 구조는 고쳤다: morph 재계산 판정의 "위치 속성 있음" 조건(MorphNormal이 없는 target은 늘 위치 속성이 있다), 원본 사본의 가중치 접두 속성 요소 검사(원본 검증이 보장한다).

### 12.5 요구 대조표(MA6a — 애니메이션 클립 핵심)

시험은 EngineTests의 `Clip_*`(`Engine/Source/Runtime/Animation/Tests`, 17개)다. 진단 이름표는 `AssetPackage_DiagnosticNames`가 새 코드까지 본다. `FrameRate` 미러는 클립 패키지 왕복(원본·쿠킹 비율, 덮어쓰기 표)으로 확인한다.

| 요구 | 시험 |
|---|---|
| 7.1 `AnimationAsset`·`AnimationClip` 저장 → 로드 → 재저장 byte 일치(에디터·쿠킹), 두 번 쓴 byte 일치, 쿠킹 패키지의 EditorOnly 제외 | `Clip_PackageRoundTripResaveIdentical` |
| 7.2 원본 상한(비율·프레임·트랙 수)·트랙 규칙(이름·원소 수·유한·회전 정규화 경계·대소문자만 다른 중복) | `Clip_ValidateSourceRules` |
| 7.2·7.5절 6 상수 트랙 접기의 bit 보존·이름 순 정렬(접은 byte, 접두·숫자)·본 트랙 bulk 왕복 | `Clip_PackageRoundTripResaveIdentical`, `Clip_StoreOrdersAndFolds` |
| 7.5절 6 원본 bulk 변조(원소 크기·개수, 순서·중복, 비유한·정규화 안 된 키, 빈 ref 셋, 프레임·트랙 수 상한, 다른 pool 이름) | `Clip_LoadRejectsSourceBytes`, `Clip_StoreRejectsInvalidSource` |
| 7.2 커브 왕복(TangentCurve 키 bit), 변환 커브 채널 9개 | `Clip_PackageRoundTripResaveIdentical`, `Clip_ValidateClipRules` |
| 7.3 이벤트·동기 마커 규칙(시간·길이·NaN·확률·track·정렬)과 정렬 도우미(안정 정렬) | `Clip_ValidateClipRules` |
| 7.5절 4 플랫폼별 목표 비율 | `Clip_PlatformFrameRate`, `Clip_ValidateClipRules` |
| 7.5절 8 샘플링 규칙(0 이하·NaN·끝 이후, 선형·nlerp 최단 경로, α 문턱, Step, 상수·1키, 0 길이 회전) | `Clip_SampleSourceRules`, `Clip_SamplePlaybackRules` |
| 7.5절 9 쿠킹 변환: 원본 비율 = 목표 비율이면 키 그대로(bit), 상수 접기, 트랙 대응·가상 본·`UnknownBoneTrack`, 커브 복사·변환 커브 수, 쿠킹 샘플링 = 원본 샘플링 | `Clip_BuildSameRateKeepsKeys` |
| 7.5절 9 재샘플링(30 → 60 짝수 표본 = 원본 키, 홀수 = 중간, Step, NTSC, 정수가 아닌 표본 수·상한 초과 `IncompatibleTargetFrameRate`, 0 프레임) | `Clip_BuildResamples` |
| 7.5절 10 쿠킹 bulk 변조(원소 수, 짧은 bulk, 성분별 비유한, trackToBone 길이, sampleCount 0(`NoSamples`)·상한(`TooManyFrames`), 비율 상한(`FrameRateTooHigh`), 빈 ref, 트랙 수 상한)와 저장 인자 오류 | `Clip_LoadRejectsPlaybackBytes` |
| 7.5절 13 결정성(두 번, 호출 전 FTZ, 비정규 값, 저장 SHA-256) | `Clip_DeterministicBuild`, `Clip_FuzzSources` |
| 할당 실패 주입(쿠킹 변환·원본·쿠킹 저장·검증·트랙 찾기·원본·쿠킹 로드 — 결과 그대로·같은 결과) | `Clip_AllocationFailures`, `Clip_LoadAllocationFailures` |
| 고정 seed 퍼징(무작위 원본 300개 — 검증, 쿠킹 불변식, 두 번 변환 일치) | `Clip_FuzzSources` |
| 11 `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped` 등록·이름·4.6절 변환 | `Clip_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`, 필터 `Clip_`·`Skeleton_`): 시험 32개 모두 통과. Animation Private·Public 전체는 줄 1618/1618(100%), 분기 819/826(99.15%), 함수 126/126이다. 클립 파일만 보면 다음과 같다.

| 파일 | 줄 | 분기 |
|---|---|---|
| ClipSource.cpp | 418/418 | 170/174 |
| ClipPlayback.cpp | 315/315 | 154/155 |
| AnimationClip.cpp | 215/215 | 160/161 |
| AnimationInternal.h·AnimationClip.h·ClipSource.h | 49/49 | 6/6 |

닿지 않은 분기 7개 가운데 6개는 `ROCKS_ASSERT`의 실패 쪽이다(용량을 미리 잡은 뒤의 추가 넷 — 트랙 이름 표·정렬 표·저장 트랙·로드 트랙, 커브 이름 표 하나, 상수 접기의 줄이는 크기 조정 하나). 나머지 하나는 12.3절에 적은 MA4a의 `FindNamed` 다른 pool 비교다. 측정 중 닿을 수 없음을 확인한 구조는 고쳤다: 커브 키 재검증(TangentCurve API·검증 hook이 이미 지킨다 — 7.5절 7), 샘플링의 `TryAsFrameTime` 실패·음수 프레임(양수 시간만 그 경로로 간다), 유한 판정의 성분별 단락(v − v 합 한 번), 쿠킹 로드의 원소별 유한 단락(개수 세기), 상수 접기의 빈 배열.

### 12.6 요구 대조표(MA3a — 단순 충돌 값·검증)

시험은 EngineTests의 `Collision_*`(`Engine/Source/Runtime/Mesh/Tests`, 8개)다. 패키지 왕복은 fixture 에셋 `TestCollisionAsset`(AssetTestFixtures `MeshAssets.h` — `SimpleCollision` 필드·배열·EditorOnly 사본, `CollisionComplexity`)으로 한다. 진단 이름표는 `AssetPackage_DiagnosticNames`가 새 코드까지 본다.

| 요구 | 시험 |
|---|---|
| 4.6·4.8절 1 형상 struct·공통 필드의 UE 기본값, 열거 번호 | `Collision_Defaults` |
| 4.8절 2 형상 규칙마다 하나씩(위치·회전·restOffset 비유한, 크기 0·음수·비유한과 성분 번호, 길이 음수·0 허용, 실린더 길이 0) | `Collision_ValidateRules` |
| 4.8절 3 볼록 transform(비유한·정규화·크기 0, 허용 경계)과 점·index 규칙(점 수 하한·상한 경계, 비유한 점, index 수·범위, 삼각형 수 2V − 4, index 없음 허용) | `Collision_ValidateRules`, `Collision_ConvexGeometryRules` |
| 4.8절 4 형상 수 상한(경계 포함) | `Collision_ValidateRules` |
| 저장 → 로드 → 재저장 byte 일치(에디터·쿠킹), 두 번 쓴 byte 일치, 볼록 bulk(점 double bit·index) 왕복, EditorOnly 제외 | `Collision_PackageRoundTripResaveIdentical` |
| 4.8절 3·5 볼록 bulk 변조(길이·빈 ref·점 수·index 범위 — CorruptPackage), bulk 검증의 형상 번호, 저장 거부 | `Collision_LoadRejectsConvexBytes` |
| 할당 실패 주입(저장·로드·bulk 검증 — 결과 그대로·같은 결과) | `Collision_AllocationFailures` |
| 고정 seed 퍼징(형상 값·볼록 점 400개 — 같은 진단, 저장 byte 결정성) | `Collision_FuzzValues` |
| 11 `InvalidCollisionData`·`UnsupportedCollisionShape` 등록·이름, 받지 않는 형상 경고 형식(4.8절 6), 4.6절 변환 | `Collision_DiagnosticNames`, `AssetPackage_DiagnosticNames` |

Coverage(2026-11-21, clang source-based — `Build/MeasureCoverage.ps1`, 필터 `Collision_`): 시험 8개 모두 통과. `SimpleCollision.cpp` 줄 375/375(100%), 분기 182/182(100%), 함수 27/27, `SimpleCollision.h` 줄 9/9. 측정 중 형상 종류별 템플릿 인스턴스마다 위치 판정 분기가 따로 세어져(구 등 일부 종류에서 회전 비유한을 만들 수 없는 조합) 위치·크기·길이 판정을 템플릿 밖 함수 하나(`CheckSized`)로 모았고, 점·index bulk 추가를 같은 자리에서 실패를 처리하는 반복으로 바꿨다(두 번째 entry 추가 실패는 bulk 원본의 용량이 미리 잡혀 있어 따로 닿지 않는다).
