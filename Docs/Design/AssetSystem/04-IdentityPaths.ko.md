## 4. 식별과 경로

### 4.1 Asset ID와 객체 이름

- `AssetId`(128비트 Guid, 무작위 v4)는 대표 에셋과 그 패키지의 영속 ID다(패키지당 대표 에셋 하나). 생성 때 발급, 저장·이동·이름 변경·재임포트 후 유지, 복제 시 새로 발급. `AssetId`·`SoftObjectRef<T>` 값 타입은 Core(ObjectRuntime)에 둔다(리플렉션이 필드 타입으로 인식).
- 객체 이름: 대표 에셋은 빈 이름, 하위 객체는 같은 Outer 안에서 유일한 이름(계획 §5.2)이며 규칙은 경로 구성 요소(4.2절)와 같다(`.`·`/` 불가). 패키지 안 객체 경로는 Outer 체인 이름을 `.`로 이은 것이다(예: 대표 에셋 아래 `Body` 아래 `Socket` = `Body.Socket`). 하위 객체는 `<AssetId, 객체 경로>`로 식별한다. 하위 객체 이름 변경 안정성은 SubobjectGuid(계획 §5.3, 후속).
- 중복 ID는 덮어쓰지 않고 `DuplicateAssetId`로 보고하며 그 ID 조회는 오류를 반환한다. 해결은 `asset-fix-duplicate-id <path>`(지정한 쪽에 새 ID)로만 한다.

### 4.2 논리 경로

- 형식 `/<Mount>/<Dir>/.../<Name>`. `/Game` = 프로젝트 `Content/`, `/Engine` = 엔진 콘텐츠(후속). 하위 객체 경로는 `<PackagePath>.<객체 경로>`(예: `/Game/Hero.Body.Socket`).
- **진실의 원천은 파일 위치 하나다.** 에디터 패키지는 경로를 저장하지 않고 대표 에셋 이름은 파일 이름에서 유도한다. 쿠킹 매니페스트(10절)는 경로 힌트를 담으며 런타임의 경로 조회(`FindByPath`)는 이 힌트로 AssetId를 찾는 편의 기능이다(영속 참조는 항상 ID).
- 정규화: 구분자 `/`만(입력의 `\`는 거부), 앞 `/` 필수, 끝 `/`·빈 구성 요소·`.`·`..` 금지. 문자열은 NFC로 정규화하며 비교 키는 `TryMakeCaselessKey` = NFC(simple case folding(NFD(x)))다(Unicode §3.13 정준 caseless 일치의 simple folding 판, 버전은 A0-C). 파일 열거 이름도 NFC로 바꿔 비교한다(macOS NFD 대응). NTFS는 자체 대문자 표($UpCase)를 쓰므로 키가 다른데 NTFS가 같게 보는 드문 이름 쌍은 저장 시 `CaseCollision` 진단으로 잡는다.
- 구성 요소: 1–64 byte UTF-8(NFC). 허용: General_Category L*·N*·M*(단, 첫 문자는 L*·N*), `_`, `-`. 거부: Default_Ignorable_Code_Point(보이지 않는 문자 — L*·M*이어도), 그 밖의 문자(공백·제어·구두점·기호 포함 — 따라서 `.`·`,`·`;`·`\`도 없다), Windows 예약 이름(대소문자 무시 `CON PRN AUX NUL COM0-9 LPT0-9`, `COM¹²³ LPT¹²³`).
- 길이: 논리 경로 ≤ 256 byte. `ProjectLayout`이 OS 경로(임시 접미사 포함) 길이를 검사한다. Windows 긴 경로는 PlatformBase가 지원한다(A0-P).
- 정렬: 이름·경로의 결정적 정렬은 NFC UTF-8 byte 순, Guid는 (high, low) 부호 없는 수치 순이다(5·10절의 모든 정렬).
- 파일: 에디터 `<Project>/Content/<path>.rasset`, 쿠킹 `<쿠킹 루트>/Packages/<a>/<A>-<h>.rpkg`. A = AssetId의 36자 소문자 표기(Core Guid `{}` 서식, RFC 9562), a = A의 앞 2자, h = 패키지 SHA-256의 앞 16자(소문자 hex). 내용 주소 이름이라 같은 이름은 같은 내용이다(10절 게시).

### 4.3 경로 별칭

- 사람이 쓴 텍스트 경로는 별칭 표 "옛 경로 → AssetId"로 풀린다(계획 §9.1). 이동·이름 변경 때 추가하고 풀릴 때마다 `AliasResolved` 경고. 위치: `Config/AssetAliases.json`(버전 관리 대상, 팀 공유, 원자적 기록, 항목 정렬로 병합 충돌 최소화). 패키지 재검색으로는 복구되지 않는 사용자 데이터이므로 버전 관리가 백업이다(계획 문서의 "재검색으로 복원되는 캐시" 표현은 17절에서 고친다).
- 만료: 항목은 생성 UTC와 만료 일수(기본 90, 설정)를 가진다. `asset-alias-prune`이 만료 항목을 지우며, 지우기 전 텍스트 참조 검색(`Config/`·스크립트)에서 쓰인 곳을 보고한다. `asset-alias-list`·`asset-alias-remove` 제공.

### 4.4 작업

| 작업 | 방법 | 상태 |
|---|---|---|
| 이동·이름 변경(디렉터리 포함) | 잠금 → 대상 없음 확인 → `Move(no-replace)`(디렉터리면 디렉터리 한 번, 대소문자만 바꾸는 이름 변경 허용) → 별칭 추가(디렉터리면 안의 모든 패키지) → 인덱스 갱신. ID·참조 무변경 | 구현 |
| 복제 | 새 경로에 새 ID로 기록(하위 객체·내부 참조 재매핑, 계획 §5.3) | 구현 |
| 삭제 | 참조자가 있으면 거부, `--force`면 삭제 후 참조자에 `MissingDependency` | 구현 |
| 중복 ID 해결 | `asset-fix-duplicate-id` | 구현 |

읽기 전용으로 연 패키지(5.3절)는 저장·쿠킹·복제·ID 수정을 모두 `NewerVersion`으로 거부한다(모르는 내용을 잃는다).

### 4.5 원본 경로

원본은 이름 있는 원본 루트 기준 상대 경로(`{Project}/SourceArt/Checker.png`)로 기록한다. 루트 목록은 `Config/AssetSources.json`(이름 → 경로, 기본 `Project`). 프로젝트 밖 원본은 절대 경로로 기록하고 `AbsoluteSourcePath` 경고. 재임포트는 기록한 경로를 현재 루트로 풀고, 없으면 `MissingSource`로 실패·기존 보존. 재지정은 `--source=`(원본 하나) 또는 `--source.<역할>=`(원본 여러 개, 역할은 ImportRecord `sources[].role`)로 한다(13절).

### 4.6 진단

| 진단 | 뜻 |
|---|---|
| `DuplicateAssetId`, `DuplicatePath`, `CaseCollision`, `InvalidPath`, `MissingFile`, `UnregisteredFile` | 식별·경로 |
| `TypeMismatch`, `MissingDependency`(Hard·Soft), `UnsupportedCycle`, `UnpackagedReference`(패키지에 속하지 않은 객체를 저장 그래프가 참조) | 참조 |
| `CorruptPackage`, `UnsupportedVersion`, `NewerVersion`, `ExternalModification`, `NotDurable`(경고 — 교체는 됐고 디렉터리 동기화만 실패, 5.5절) | 패키지·저장 |
| `MissingSource`, `SourceChanged`, `InvalidSource`(원본 해석 실패), `UnsupportedSourceFormat`(지원하지 않는 원본 변형, 예: PSD CMYK), `LossySource`, `NonSrgbSource`, `IgnoredSourceChannels`(경고), `IgnoredSourceChunks`(경고 — 오디오 임포터, [오디오 시스템](../AudioSystem/03-Design/03.10-Import.ko.md) 3.10.2절), `AbsoluteSourcePath` | 원본 |
| `ImporterOutdated`, `InvalidImportOption`, `OrphanedOutput`, `UnknownProperty`(모르는 속성 이름), `UnsupportedSetting`(계약 설정의 기본값이 아닌 값 — MA2a 메시 빌드가 보존 설정에 처음 내며 `AssetDiagnosticCode`에 등록), `InvalidSetting`(모순된 조합·숨김 값·쓸 수 없는 값 — 같이 등록, 메시 `buildScale`의 0·비유한 성분, 스킨 빌드 임계값의 음수·비유한 값과 음수 `boneInfluenceLimit`), `SrgbIgnored`(경고 — 선형 설정·float 출력의 SRGB), `PreserveBorderWithWrap`(경고), `NuggetConflict`(경고 — 쿠킹 너겟 Put의 같은 키 다른 값, 12.14절), `SourceCleared`(경고 — 묶음 원본 재구성이 원본을 비움, 12.14절 A5j2) | 임포트·설정 |
| `CorruptEntry`(에셋 너겟 항목·게시 패키지 파일 손상), `IncompatibleCook`(부분 쿠킹 병합 불가·런타임 프로파일 불일치), `NeverCookReferenced` | 에셋 너겟·쿠킹 |
| `AliasResolved`, `RootLost`(감시 루트 소실) | 그 밖 |
| `InvalidMeshData`, `NonFiniteAttribute`, `UvOutOfRange`, `NonNormalizedWeights`, `TooManyInfluences`, `InfluencesTrimmed`, `MissingInfluences`(경고), `MaterialSlotFallback`, `NegativeBuildScale`, `UnsupportedCollisionShape`, `InvalidCollisionData`, `InvalidSkeletonData`, `SkeletonMismatch`, `UnknownBoneTrack`, `InvalidClipData`, `EventPropertiesDropped` | 메시·애니메이션([MeshAnimationAssets](../MeshAnimationAssets.ko.md) 11절 — 앞 다섯 코드는 MA1이, `MaterialSlotFallback`·`NegativeBuildScale`은 MA2a가, `InvalidSkeletonData`·`SkeletonMismatch`는 MA4a가, `InfluencesTrimmed`(정보)·`MissingInfluences`는 MA5a가, `InvalidCollisionData`·`UnsupportedCollisionShape`(임포터 경고)는 MA3a가, `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped`는 MA6a가 `AssetDiagnosticCode`에 등록, 나머지는 설계) |
| `DuplicateLevelScript`(`levelScript` 말고 또 있는 LevelScript 파생 Actor), `LevelScriptNotInLevel`(`levelScript`가 `actors` 원소가 아님), `CrossLevelReference`(레벨에 저장하는 참조가 다른 Level, 또는 영속 Entity 참조 전 다른 셀의 객체를 가리킴), `NotPlaceableClass`(편집기 배치 명령이 `NotPlaceable` 클래스를 받음), `LevelScriptCollider`(LevelScript의 Collider component) | 레벨(설계 — [레벨 스크립트](../LevelScript.ko.md) 4.9절. 저장·쿡은 InvalidArgument, cooked 로드는 CorruptPackage) |
| `CorruptBundle`, `UnsupportedBundleVersion`, `UnknownField`, `UnsupportedAsset`, `UnsupportedSourceLayout`, `GammaMismatch`, `GeneratedLodBaked`, `BakedColorQuantized`, `UnrealPathRenamed`, `UnrealPathTooLong`, `UnrealPathCollision`, `MissingUnrealDependency`, `UntranslatedMaterialNode`, `MaterialBakedFallback`, `ParameterDefaultConflict`, `ParameterResolveMismatch` | Unreal 교환 임포트([Unreal 교환 형식](../UnrealExchange.ko.md) 10.3절). `CorruptBundle`·`UnsupportedBundleVersion`·`UnknownField`·`UnsupportedAsset`·`UnrealPathRenamed`·`UnrealPathTooLong`·`UnrealPathCollision`·`MissingUnrealDependency`는 UX1a 리더가 내며 `AssetDiagnosticCode`에 등록(2026-11-21, 10.4절), 나머지는 설계 |
