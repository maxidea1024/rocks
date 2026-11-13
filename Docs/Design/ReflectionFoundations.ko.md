# 리플렉션·빌드 보강(A0-B) 설계

기준일: 2026-11-13, 초안 3(교차검증 3건 — 빌드 실현성·런타임 값 모델·소비자 적합성 — 과 최종 검증 반영). [에셋 시스템 설계](AssetSystem.ko.md) 1절 A0-B 단계와 5.6절의 상세다. 현재 구현은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)·[직렬화](../Implementation/Serialization.ko.md)에 있다. 패키지 형식(A1)·텍스처 설정(12.4절)·graph v2(A0-O)·World 객체·UI·Sparks가 이 기반을 쓴다.

원칙:

- UE 5.6 대응: UHT → MetaCompiler, `FProperty` → `FieldSchema` + 생성 typed thunk, `UENUM`(`UMETA(DisplayName)` 포함)·`USTRUCT`·`TArray`·`TSoftObjectPtr`·상속되는 클래스 플래그. UE의 클래스명과 메모리 offset 기반 property 접근은 복제하지 않는다 — 필드 접근은 생성 코드의 멤버 포인터 thunk만 쓴다(raw offset·주소 공개 없음).
- 실패 원자성(읽기 실패는 출력 불변, 쓰기 실패는 미게시, 로드는 전부 공개 또는 전부 rollback), 결정성(같은 입력 → 같은 byte), 모든 길이·개수·작업량에 한도.
- 하위호환을 두지 않는다(초기 단계). `FieldSchema`·graph 필드 인코딩·생성 코드 형식을 바로 바꾸고 시험을 고친다.

## 0. 현재 상태와 바꿀 점

| 항목 | 현재 | A0-B 뒤 |
|---|---|---|
| 메타데이터 소유 | 타깃(`TargetRules.Metadata`·`IdentityManifest`), 타깃 선언 순서로 헤더마다 실행 | 모듈(`NativeModuleInput.Metadata`), 모듈 의존 순서, 모듈 하나에 실행 한 번 |
| 생성 단위 사이 참조 | base·참조 대상은 같은 단위만("not reflected in this unit") | descriptor `--import`로 의존 모듈의 타입 사용 |
| 값 종류 | 스칼라·Guid·String·ObjectRef | + Enum·Struct·Array·AssetId·SoftObjectReference·Digest256, ObjectRefArray |
| 지정자 | CLASS `Abstract`, PROPERTY `Transient`·`ReadOnly`·`Category`·`DisplayName` | + CLASS·STRUCT `Version`, CLASS `WorldOnly`, STRUCT `Binds`, ENUM `Flags`·`ToolPrefix`, PROPERTY `EditorOnly`·`Required`·`Name`·`MaxElements`, 열거자 `Name`·`DisplayName`·`Hidden` |
| 비리플렉션 멤버 | 금지(모든 비정적 필드에 PROPERTY) | `ROCKS_NATIVE`(비영속, 강한 참조 재귀 금지) |
| 스키마 버전 | 1 고정 | `Version=N` |
| 필드 직렬화 | GraphValues의 `FieldValue` switch | 필드별 생성 codec thunk(encode·두 단계 decode) + 관찰 hook |
| 엔진 World 타입 | `ROCKS_NATIVE_TYPE` 수동 등록(B2에서 `ROCKS_NATIVE_CLASS`를 바꿈) | `ROCKS_CLASS(WorldOnly)`(B8) |
| 빌드 주입 상수 | 없음 | 툴체인 지문·도구 빌드 ID |

## 1. 모듈 소유 메타데이터(B1)

### 1.1 선언

- `NativeModuleInput`에 `Metadata`(헤더 include 철자 목록), `ModuleDirectory`(프로젝트 상대 경로), `ImportedMetadata`(SDK 모듈 전용 — 미리 생성된 descriptor·명세 경로, 1.8절)를 더한다. 규칙은 `Configure()`에서 설정한다(RuleLoader는 `Configure`만 부른다). `SourceModuleRules`는 기존 `{Layer}/Source/{Group}/{Name}`를 `ModuleDirectory`로 넘기고, `ModuleRules`를 직접 상속하는 게임 모듈은 명시한다. Metadata가 있으면 `ModuleDirectory`는 필수다(루트 기본값을 두지 않는다 — 실수로 루트 명세를 쓰지 않게). `Metadata`와 `ImportedMetadata`는 함께 쓸 수 없다.
- 생성 include 디렉터리 추가·명세 경로 결정·검증은 Rocks.Build의 **공용 함수 하나**(`MetadataLayout.Apply(modules, outputDirectory)`)가 한다. RuleLoader(와 SDK 모듈 복원) 뒤, ActionGraph 앞에서 부르고 Program.cs·InstalledSdk·GameInputSnapshot·IntegrationTests가 모두 이 결과를 본다(같은 목록을 따로 계산하지 않는다).
- Metadata의 철자는 모듈의 **public 또는 private include 디렉터리** 기준이다. public 헤더의 타입은 의존 모듈이 import할 수 있고, private 헤더의 타입은 그 모듈 안에서만 쓴다(UE도 Private 헤더의 UCLASS를 허용한다). 검증(존재·루트 안·symlink 정규화·같은 헤더의 모듈 사이 이중 소유 금지)은 지금의 `MetadataRunner` 규칙을 모듈마다 적용한다.
- 생성 헤더는 원본 헤더를 같은 철자(`#include <철자>`)로 포함한다. 그 철자가 **그 헤더를 쓰는 모든 모듈**의 include 검색 순서에서 같은 파일로 풀려야 한다 — MetadataLayout이 생성 모듈과, public 헤더면 그 모듈에 의존하는 타깃 내 모든 모듈의 검색 순서로 확인한다(가리는 같은 철자 헤더가 있으면 RMC1001).
- identity 명세는 `<ModuleDirectory>/<ModuleName>.rocksids.json`로 고정한다. Metadata가 있는 모듈은 명세가 있어야 하고(RMC2100), Metadata가 없는 모듈에 **활성 항목이 있는** 명세가 있으면 RMC2102다(은퇴 항목만 남은 명세는 ID 예약이므로 허용한다). SDK 모듈(`ImportedMetadata`)은 이 위치 규칙과 RMC2102에서 빠진다 — 명세 위치는 SDK manifest가 정하고 읽기 전용이다.
- `TargetRules.Metadata`·`IdentityManifest`와 `MetadataUnit`은 지운다.

### 1.2 생성 위치와 include

- 모듈 M의 생성 헤더는 원본 철자를 그대로 따른다: public 헤더 `ReflectionTestFixtures/ReflectionFixture.h` → `<out>/Generated/<M>/Public/ReflectionTestFixtures/ReflectionFixture.generated.h`, private 헤더는 `.../Private/...`. `<out>/Generated/<M>/Public`을 **M의 public include 디렉터리로**, `.../Private`를 private include 디렉터리로 자동 추가한다(ActionGraph의 public 전파). 사용 철자는 원본 철자의 `.h`를 `.generated.h`로 바꾼 것이다(`#include <ReflectionTestFixtures/ReflectionFixture.generated.h>`). 생성 철자의 유일성은 저절로 보장되지 않으므로(빌드는 public 헤더 철자의 모듈 사이 유일성을 강제하지 않는다) MetadataLayout이 타깃에서 도달하는 모듈의 생성 철자를 대소문자 무시로 비교해 겹치면 RMC2005, 원본 철자나 생성 철자(`.generated.h`·집계 헤더)를 생성 루트보다 앞에서 가리는 파일은 RMC1001로 거부한다.
- 모듈 집계 헤더 `<M>/<M>.module.generated.h`를 public 생성 루트에 둔다(원본 이름이 `.module.h`·`.generated.h`로 끝나는 헤더는 RMC2005). B1에서는 모듈의 생성 헤더를 모두 include하고 클래스를 base 먼저 등록하는 `RegisterModuleTypes`(3.5절)를 담는다 — B3가 enum·struct 등록을 더한다.
- 생성 디렉터리는 **메타 생성 전에** 만든다(MetaCompiler의 `--include-dir`가 존재를 요구하고, ActionGraph도 없는 include 디렉터리를 거부한다). stage·백업·검증 임시 파일·descriptor는 `<out>/Generated/<M>` **밖**에 둔다(`<out>/Metadata/`·`<out>/.metadata-stage/<tag>/`) — 안에 두면 헤더 검사·증분 스캔·SDK 복사에 걸린다.
- 규칙마다 손으로 넣던 `out/Generated` private include(FoundationTests·Game·Program.cs)와 Program.cs의 생성 헤더 별도 검사 루프는 지운다(public include의 `*.h`를 도는 기존 헤더 검사가 생성 헤더도 덮는다 — 두 번 등록하면 "Duplicate source"다).

### 1.3 실행 단위와 순서

- 타깃에서 도달하는 모듈(플러그인 모듈 포함 — native 빌드와 같은 root 집합) 중 Metadata가 있는 모듈을 의존 순서(위상 정렬)로 실행한다. 모듈 하나에 MetaCompiler 실행 한 번이며 입력 헤더 전부를 `#include`하는 합성 번역 단위를 Clang에 준다. 같은 모듈 안 헤더 사이의 base·참조는 이 실행 안에서 풀린다.
- 명령: `Rocks.MetaCompiler --module M --input <h> --include <철자> [--private]… --output-root <stage>/<M> --descriptor <stage>/M.rocksmeta.json --identity-manifest <명세> --identity-inventory <임시> --import <의존 descriptor>… [--check]`.
- clang 인자는 지금처럼 소유 모듈의 compile 프로필·public+private define/include와 의존 모듈의 public define/include다. 단 **자기 모듈을 포함한 모든 생성 include 루트는 게시본 대신 이번 실행의 stage 루트**로 바꿔 넘긴다(생성 헤더가 같은 모듈·의존 모듈의 생성 헤더를 include하므로 — 4.2절 — 검증 컴파일이 이전 빌드의 낡은 게시본이 아니라 이번 산출물을 봐야 한다).
- 검증 컴파일은 생성 헤더마다 따로 한다(소비자가 헤더를 하나씩 include하므로). `--check`는 모든 생성 헤더·집계 헤더·descriptor와 남은 잉여 생성 파일까지 본다.

### 1.4 descriptor(`<M>.rocksmeta.json`)

의존 모듈이 상속 필드를 펼치고 참조·값 형식을 해석하는 데 필요한 것을 모두 담는다(설계 5.6절의 "타입 ID·base·필드 ID/종류/플래그"만으로는 상속 필드 thunk를 만들 수 없다).

```json
{ "schema": 1, "module": "ReflectionTestFixtures",
  "types": [
    { "kind": "class", "name": "rocks::reflection_fixture::Base", "id": "<32 hex>", "version": 1,
      "flags": ["Abstract"], "base": "rocks::Object",
      "header": "ReflectionTestFixtures/ReflectionFixture.h", "visibility": "public",
      "fields": [ { "owner": "rocks::reflection_fixture::Base", "name": "count", "id": "<hex>",
                    "fieldKind": "Value", "value": { "kind": "Int32" }, "element": null,
                    "flags": [], "toolName": "count", "category": "", "displayName": "", "maxElements": 0 } ] },
    { "kind": "struct", "name": "…", "id": "<32 hex>", "version": 1, "flags": [], "binds": "",
      "header": "…", "visibility": "public", "fields": [ … ] },
    { "kind": "enum", "underlying": "UInt8", "flags": ["Flags"], "toolPrefix": "TC_",
      "values": [ { "name": "Default", "value": 0, "toolName": "Default", "displayName": "", "flags": ["Hidden"] } ] } ] }
```

- 클래스 `fields`는 상속 포함 전체(정렬 순서 그대로)이고 각 필드에 선언 owner를 적는다. 생성 thunk는 `&::<owner>::<name>`을 쓰며 private 기반 필드는 owner의 `ROCKS_GENERATED_BODY`(모든 `GeneratedType` 특수화에 friend)로 접근한다 — 모듈 경계와 무관하다.
- struct의 `flags`는 `ContainsStrongReference`·`RequiresWorldOnlyOwner`(생성 형질, 4.6절)·`ContainsReferenceArray`(B5, 5.6절), `binds`는 Binds 실제 타입의 Clang 정규 철자(없으면 빈 문자열)다.
- 읽기는 모르는 속성을 거부하고 schema 1만 받는다. 출력은 결정적이다(이름 순, 고정 서식, LF).

### 1.5 import와 파서의 수집 범위

- 파서는 **이 모듈의 입력 헤더에 물리적으로 있는 선언만** 리플렉션 대상으로 모은다. 비교는 디스크 철자로 정규화한 정규 경로로 한다(macOS의 대소문자 무시 볼륨에서 철자만 다른 경로로 거짓 오류가 나지 않게). 필터는 **identity 조회보다 앞에** 둔다(지금은 선언을 모으는 시점에 ID를 찾으므로, 의존 모듈의 리플렉션 헤더를 include하면 그 타입의 ID를 이 모듈 명세에서 찾다가 실패한다).
- 입력 밖의 `ROCKS_CLASS`·`ROCKS_STRUCT`·`ROCKS_ENUM` 선언은 import한 descriptor에 같은 정규 이름으로, **같은 헤더 철자·같은 물리 파일**로 있어야 한다(이름만 같은 이중 정의를 막는다). 없으면 RMC2003. private 타입을 다른 모듈이 쓰면 RMC2006.
- base·참조·값 형식 해석은 이 모듈 타입 → import 타입 순으로 정규 이름을 찾는다(`SchemaValidator`·`CppEmitter`에 import 조회를 더한다). 같은 이름이 둘 이상이면 RMC2004.
- import는 직접·간접 의존 모듈 전부의 descriptor다(빌드가 닫힌 집합을 넘긴다).

### 1.6 identity 검증과 이력

- 모듈마다 inventory를 만들고 `ids verify --manifest <m> --inventory <i>`를 명세별로 실행한다(누락·고아·중복 규칙은 지금과 같다).
- 명세 사이 ID·이름 중복: `ids verify-set --manifest <m>…`가 타깃에서 도달하는 모듈의 모든 명세(은퇴 전용 예약 명세·SDK 동봉 명세 포함)에서 활성·은퇴 항목을 모아 같은 ID나 같은 종류의 같은 이름·별칭이 두 파일에 나오면 RMC2101이다.
- 명세의 `kind`는 그대로 `type`·`field`다. enum·struct 타입은 `type`, struct 필드는 `field`다. enumerator는 정수 값으로 저장되므로 ID를 받지 않는다.
- **이력·병합 검사 개정(필수)**: `CheckIdentityMerges`·`StableIdMergeValidator`는 지금 경로별로 비교하므로 명세를 옮기면 옛 경로가 "disappeared"로 반드시 실패한다. 리비전마다 저장소의 모든 명세를 **ID 기준으로 합친 뷰**(한 ID·한 이름은 한 파일에만)를 만들어 보존 규칙(ID 삭제·재사용 금지, 이름 재사용 금지)을 검사한다. 이름(별칭 포함)도 합친 뷰에서 유일해야 한다 — 빌드의 `ids verify-set`과 같은 규칙이라 로컬 빌드와 CI 병합 검사가 같은 결과를 낸다(초안의 "이름 충돌은 명세별"을 B1 구현에서 바꿨다). 새 명령 `ids move --from <m> --to <m> --type <name>`이 타입과 그 필드를 함께 옮긴다(필드의 owner 타입이 같은 명세에 있어야 하는 규칙 유지). `BuildEvidence`는 접미사로 찾아 해시만 하므로 그대로다.

### 1.7 게시 트랜잭션

- 모든 모듈의 생성 헤더·집계 헤더·descriptor를 stage에 만든다. 의존 모듈은 stage의 descriptor와 생성 루트를 쓴다(1.3절).
- 모든 inventory 검증과 명세 집합 검증이 끝난 뒤 한 번에 게시한다. `MetadataOutputTransaction.Publish`는 명세 스냅샷 **목록**을 받아 각 명세가 실행 동안 바뀌지 않았는지 확인하고, 출력 전부를 백업·교체·실패 시 복구한다. 항목에 **삭제**(이번 실행의 모듈에서 입력이 빠진 헤더의 생성물과 그 모듈 생성 루트의 잉여 파일)를 더하고 삭제도 백업·복구한다. 이번 실행에 없는 모듈의 생성 디렉터리는 지우지 않는다(`--output`을 여러 타깃이 함께 쓸 수 있다 — 그 디렉터리는 include 경로에 들어가지 않으므로 해가 없다). 임시 정리는 stage 디렉터리까지 지운다.

### 1.8 SDK·게임 템플릿·입력 스냅샷

- `InstalledSdk`: 생성 public 루트가 모듈의 public include 디렉터리에 들어가므로 기존 export가 `Headers/<M>/…`로 복사한다(별도 복사를 두지 않는다). descriptor·명세는 `Metadata/<M>/`로 내보내고 `rocks-sdk.json` 모듈 항목에 `descriptor`·`identityManifest`를 더한다(manifest 버전 3, 모르는 속성 거부 유지). SDK 모듈을 `NativeModuleInput`으로 되살릴 때(InstalledSdk의 모듈 복원 지점) 이 둘을 `ImportedMetadata`로 넘긴다 — SDK 모듈은 다시 생성하지 않고, 게임 빌드는 SDK descriptor를 import하고 SDK 명세를 명세 집합 검증에 넣는다. ThirdParty manifest 형식을 바꾸는 다른 세션의 InstalledSdk 변경과 겹치므로 착수 전에 조율한다.
- `GameInputSnapshot`: 선언 입력에 모듈의 Metadata 헤더와 명세를 넣는다.
- `Templates/Game`: SampleGame 모듈은 `Source`를 public include 디렉터리로 두고(`GameObject.h` 철자) `Metadata`와 `ModuleDirectory = "."`를 선언한다. 명세는 `ids move`로 `SampleGame.rocksids.json`이 된다. `Main.cpp`는 생성 철자를 쓰고 README를 고친다.
- `Program.cs`: ToolTests `meta` 실행 조건은 "타깃에 Metadata 모듈이 있음"이다.

### 1.9 fixture 이전과 시험

- Foundation 리플렉션 fixture를 `Core/Source/Developer/ReflectionTestFixtures` 모듈(Public/`ReflectionTestFixtures/`)로 옮기고 명세를 `ids move`로 `ReflectionTestFixtures.rocksids.json`에 옮긴다(ID는 그대로). FoundationTests·EngineTests는 이 모듈에 의존한다(두 타깃 이중 owner가 없어진다).
- 시험: Rocks.Build.IntegrationTests의 타깃 소유 전제를 모듈 소유 사례로 바꾸고, 모듈 사이 상속·참조(import), private 타입 import 거부, 고아 명세(활성·은퇴만), 명세 사이 ID 중복, 입력 밖 리플렉션 선언(RMC2003), 가리는 include 철자(RMC1001), stage import 후 실패 시 미게시·삭제 복구, SDK descriptor import, 명세 이동 PR의 병합 검사 통과를 더한다. ToolTests(`IdentityCompilerTests`·`MetaTests`·`IdentityInventoryTests`·`MetadataOutputTransactionTests`·`IdentityGitMergeTests`)를 새 CLI·다중 명세·합친 뷰로 고친다.
- 문서: `Build/README.md`, `ReflectionMetadata.ko.md`(타깃 소유 서술).

## 2. 지정자와 플래그(B2)

### 2.1 문법

지정자 값은 따옴표 문자열 또는 **부호 없는 10진 정수**(앞자리 0 금지, 1–2³¹−1)다. 나머지 문법(이름 목록·쉼표·중복 금지·JSON escape)은 그대로다. 지정자 어휘는 MetaCompiler의 표 하나에 두고 모르는 지정자는 오류다. UI 문서의 확장 문법(`|`·구간·실수 값)과 버전 있는 어휘 manifest는 UI의 메타데이터 단계(CU1)가 이 표를 넓혀 도입한다.

### 2.2 타입 지정자

| 지정자 | 대상 | 의미 |
|---|---|---|
| `Abstract` | CLASS | 지금과 같다 |
| `Version=N` | CLASS·STRUCT | `version`·Record schema version. 없으면 1 |
| `WorldOnly` | CLASS | `TypeFlags::WorldOnly`(2). **상속된다**(UE의 상속 클래스 플래그처럼 base가 WorldOnly면 파생도 자동으로 WorldOnly, 파생에 다시 쓰지 않아도 된다). 에셋 객체 Registry가 생성을 거부한다(정책은 A0-O). TypeRegistry는 WorldOnly base의 파생이 플래그를 갖는지 검사한다(수동 등록 대비) |
| `Flags` | ENUM | 플래그 enum(3.4절) |
| `ToolPrefix="…"` | ENUM | 도구 이름의 선택 접두(예 `TC_`) |
| `Binds="…"` | STRUCT | 미러 선언이 결합하는 실제 타입 철자(4.5절) |

버전 규칙: 버전은 그 타입 Record의 해석 규칙이다. 영속 필드의 의미·형식이 바뀌면 올리고 마이그레이션을 등록한다. base의 버전은 파생 Record에 기록되지 않으므로 base의 영속 배치를 바꾸면 파생 타입들도 버전을 올린다(MetaCompiler는 강제하지 않는다 — 배치 지문 비교는 후속 후보). 필드 추가만 한 경우의 누락 허용은 패키지 쪽 정책(`allowMissingFields`, A1)이 정한다.

### 2.3 필드 지정자

| 지정자 | 플래그 | 의미·검증 |
|---|---|---|
| `Transient`·`ReadOnly`·`Category`·`DisplayName` | 1·2·— | 지금과 같다 |
| `EditorOnly` | 4 | 항상 컴파일되고 플래그만 단다. graph v2(A0-O)가 쿠킹 데이터에서 뺀다. 중첩 struct·배열 안의 값에도 효력이 전파된다(6.1절 유효 플래그). 쿠킹 데이터 로드에서 EditorOnly 필드 누락은 `NotFound`가 아니다(A0-O). A0-B의 graph v1은 보통 필드처럼 저장한다 |
| `Required` | 8 | ObjectRef·SoftObjectRef 필드와 그 배열(`ObjectRefArray`·`Array<SoftObjectRef>` — 원소마다 non-null, 5.6절)에만 쓸 수 있다(그 밖은 RMC2202). **패키지 저장·쿠킹에서** null이면 거부한다 — graph 저장 옵션 `rejectMissingRequired`(패키지 저장·쿠킹이 켠다, 스냅샷·복제 같은 다른 저장은 끈다)이 켜져 있으면 InvalidArgument, Soft 대상의 존재 확인은 패키지 저장·쿠킹(A1). **로드는 거부하지 않고** 진단(`RequiredMissing`)만 낸다 — 기존 필드에 `Required`를 붙여도 null을 가진 에셋을 열어 고칠 수 있어야 한다 |
| `Name="…"` | — | 도구·명령행 이름(`toolName`). ASCII 식별자(`[A-Za-z_][A-Za-z0-9_]*`), 없으면 C++ 필드 이름. 상속 포함 대소문자 무시로 유일해야 한다(RMC2203). 직렬화는 필드 ID라 이름과 무관하다 |
| `MaxElements=N` | — | 배열 길이 상한(5.5절 — 쓰기·저장에서 건다). 1..1,048,576(밖은 RMC2000), 없으면 65,536. 배열이 아닌 필드는 RMC2202 |

계약 설정(기본값이 아니면 `UnsupportedSetting`, 12.4절)과 필드 기본값 조회(빌드 키의 "기본값 포함")는 클래스 기본 객체가 필요하므로 텍스처 설정 단계(A5)에서 정한다(11절).

### 2.4 `ROCKS_NATIVE`

- `ROCKS_NATIVE()`는 리플렉션 클래스·구조체 안의 **비영속 런타임 멤버**다(GPU 핸들·캐시·World 쪽 `AssetLease`). 모든 비정적 필드는 `ROCKS_PROPERTY` 또는 `ROCKS_NATIVE` 중 하나다(둘 다 없으면 지금처럼 오류, 둘 다면 RMC2204).
- **강한 참조 재귀 금지**(GC 구멍 방지): MetaCompiler가 Clang AST로 NATIVE 멤버의 타입을 재귀 검사한다 — 값으로 담는 필드·base 클래스·이름 없는 struct/union 멤버·배열 원소·템플릿 인자와 템플릿 패턴의 필드를 따라가며 `ObjectRef`·`ObjectRefArray`·`RootRef`가 있거나 `Object` 파생 타입을 가리키는 raw 포인터·참조(EIP 525행)가 있으면 RMC2205다. 포인터·참조는 대상이 `Object` 파생이거나 대상이 다시 강한 참조인지만 보고, 빌린 포인터가 가리키는 비-Object 레코드의 필드는 펼치지 않는다(B2 구현에서 정함 — `World*` 같은 NATIVE 멤버를 허용한다). `Object` 파생 타입은 값이나 템플릿 인자로 닿아도 펼치지 않는다. 리플렉션 struct는 생성 형질 `ContainsStrongReference`로 판정한다. 생성 코드는 직접 타입에 대해 같은 조건을 `static_assert`로 한 번 더 둔다. `WeakRef`·`SoftObjectRef`·`ObjectHandle`은 허용한다(추적하지 않는 참조 — 대상을 담지 않으므로 템플릿 인자도 따라가지 않는다).
- WorldOnly 전용 멤버: 타입이 `static constexpr bool RocksRequiresWorldOnlyOwner = true;`를 선언하면(A1의 `AssetLease`가 선언) WorldOnly가 아닌 클래스의 `ROCKS_NATIVE` 멤버로 둘 수 없다(RMC2206, 같은 재귀 검사로 담는 타입까지 본다). 리플렉션 struct가 그런 NATIVE 멤버를 (중첩까지) 담으면 struct 자체가 생성 형질 `RequiresWorldOnlyOwner`를 갖고, 그 struct를 필드로 둔 WorldOnly 아닌 클래스도 RMC2206이다. 이름 문자열이 아니라 형질로 판정하므로 `AssetLease`가 생기기 전에도 규칙과 시험을 둘 수 있다.
- 수동 등록 매크로 `ROCKS_NATIVE_CLASS`는 이름이 겹치므로 `ROCKS_NATIVE_TYPE`으로 바꾼다(EngineRuntime 80행 서술도 고친다).

### 2.5 `completeFieldLayout`

뜻을 "모든 **영속** 필드와 **모든 강한 간선**이 typed 메타데이터에 있음"으로 바꾼다. `ROCKS_NATIVE` 멤버는 영속 필드가 아니고 강한 참조를 담을 수 없으므로(2.4절) 생성 타입은 계속 true다. GraphSave·TypeRegistry 계약 주석과 시험을 고친다.

### 2.6 스키마·Registry 변경

- `FieldFlags`: `EditorOnly = 4`, `Required = 8`. `TypeFlags`: `WorldOnly = 2`. TypeRegistry의 허용 마스크를 넓히고, `Required`는 참조 종류 필드에만, `MaxElements`는 배열에만 허용한다(수동 등록도 같은 검사).
- 상속 필드 일치 비교에 새 서술자(3.5절 `ValueType`·`toolName`·`maxElements`)를 더한다.
- `toolName`은 등록 때 깊게 복사하고(단일 할당 원자성 유지) 대소문자 무시 유일성도 등록 때 검사한다.

## 3. 단일 값 종류(B3)

### 3.1 `AssetId`

- `rocks::AssetId { Guid value; }`를 ObjectRuntime(`Object/AssetId.h`)에 둔다(AS 4.1절: 값 타입은 ObjectRuntime). 0은 null. 문자열 표기는 36자 소문자 하이픈 형식(AS 4.2절). 기존 `Engine/.../AssetRuntime/AssetTypes.h`의 `asset::AssetId`는 구 설계 코드 재구성 때 이 타입으로 바꾼다.
- `ValueKind::AssetId`, wire `Guid`(7). `FieldValue`는 `guid` 칸을 쓴다.

### 3.2 `Sha256Digest`

- Core의 `Sha256Digest`(`Core/Encoding/Sha256.h`, 32 byte)를 쓴다. `ValueKind::Digest256`, wire `Bytes`(9), 길이 정확히 32(아니면 InvalidArgument). `FieldValue`에 `digest` 칸을 더한다.

### 3.3 `SoftObjectRef<T>`

- `rocks::SoftObjectRef<T> { AssetId asset; String objectPath; }`를 ObjectRuntime에 둔다. GC 간선이 아니며(`FieldKind::Value`) 이동 전용이고 복제는 `TryClone`(실패 가능 — Core `String`이 복사를 지원하지 않는다)이다. `T`는 기대 타입(`value.type`, 0 = Object)이다. null은 `asset`이 0이고 `objectPath`가 빈 것이다(asset이 0인데 경로가 있으면 InvalidArgument). 객체 경로는 패키지 안 Outer·Name 경로(A0-O, 비면 대표 에셋), UTF-8 4 KiB 상한(AS 5.1절).
- `ValueKind::SoftObjectReference`, wire `SoftObjectReference = 12`(새 wire tag, payload = Guid 16 byte + String). `FieldValue`는 `guid`·`string`을 쓴다.
- Sparks 문서의 `Soft<T>`("AssetId만")와 UI 문서의 `AssetRef<T>`는 이 타입으로 맞춘다(11절). 클래스 참조(`TSoftClassPtr`·`TSubclassOf` 대응)는 이 단계 밖이다.

### 3.4 `ROCKS_ENUM`

```cpp
ROCKS_ENUM(ToolPrefix = "TEXTUREGROUP_")
enum class TextureGroup : uint8 {
    World = 0,
    EightBitData ROCKS_ENUMERATOR(Name = "8BitData", DisplayName = "8비트 데이터") = 7,
    Reserved13 ROCKS_ENUMERATOR(Hidden) = 13,
};
```

- 대상은 `enum class`이고 underlying을 명시해야 한다: `uint8`·`uint16`·`uint32`·`int32`(기존 `ValueKind`가 있는 폭. `int8`·`int16`·64비트는 받지 않는다 — RMC2401). 열거자 값은 서로 달라야 한다(별칭 금지).
- 열거자 지정자 `ROCKS_ENUMERATOR(Name="…", DisplayName="…", Hidden)`(UE `UMETA(DisplayName)` 대응): `Name`은 도구 이름(없으면 C++ 이름)이며 `[A-Za-z0-9_]+` — 접두를 떼면 숫자로 시작하는 UE 이름(`TEXTUREGROUP_8BitData`)을 받기 위해 숫자 시작을 허용한다. `DisplayName`은 표시용 UTF-8, `Hidden`은 저장 값으로는 유효하지만 도구가 설정 값으로 받지 않는 열거자다(텍스처 설정의 숨김 값 → `InvalidSetting`, 판정은 설정 계층). 도구 이름 조회는 대소문자 무시이고 `ToolPrefix`가 있으면 접두가 있든 없든 받는다. 접두 유무를 합친 이름 집합이 대소문자 무시로 유일해야 한다(RMC2203).
- 값 검증: 일반 enum은 선언된 값이어야 하고, `Flags` enum은 선언 값들의 OR(`(v & ~mask) == 0`)이어야 한다. 생성 헤더가 constexpr 검증기 `GeneratedType<E>::IsDeclared(int64)`를 내고 쓰기·decode thunk가 그것을 쓴다(TypeRegistry에 접근하지 않는 staged 쓰기·decode에서도 검증된다). `FieldValue`는 `signedInteger`와 enum ID(`type`)를 싣고 thunk가 `type == GeneratedType<E>::Id()`를 확인한다(다르면 TypeMismatch).
- 미선언 값: 쓰기는 InvalidArgument. 로드는 decode 정책(6.1·6.4절)을 따른다 — 기본 거부(InvalidArgument), `ReportAndDefault`면 일반 enum은 기본값(값 0이 선언되어 있으면 0, 아니면 첫 열거자), Flags enum은 선언되지 않은 비트만 지운 값으로 바꾸고 `UndeclaredEnumValue` 진단을 낸다(Sparks 1117행의 "기본값으로 바꾸고 보고"). 새 빌드가 쓴 데이터를 `CorruptPackage`로 오진하지 않도록 A1이 정책을 고른다.
- 스키마: `EnumSchema { TypeId id; name; ValueKind underlying; uint32 flags(Flags=1); Span<EnumeratorSchema> values; toolPrefix }`, `EnumeratorSchema { name; toolName; displayName; int64 value; uint32 flags(Hidden=1) }`. wire `I64`(4).

### 3.5 스키마 표현과 등록

```cpp
struct ValueType {
    ValueKind kind = ValueKind::Opaque;
    TypeId type{};   // Enum·Struct schema ID, ObjectReference·SoftObjectReference 대상(0 = Object)
};

struct FieldSchema {
    Guid id;
    FieldKind kind;            // ObjectReference = 강한 GC 간선(ObjectRef·ObjectRefArray)
    uint32 flags;
    Span<const char> name;     // C++ 이름
    Span<const uint8> attributes;
    ValueType value;           // Array면 kind = Array
    Span<const char> category;
    Span<const char> displayName;
    Span<const char> toolName; // Name="…", 비면 name
    ValueType element;         // Array의 원소 형식, 그 밖은 Opaque
    uint32 maxElements;        // Array만, 그 밖은 0
};
```

(멤버 순서는 기존 집합 초기화 순서를 유지하려고 새 멤버를 끝에 붙인 것이다 — B3a 구현.)

- `ValueKind` 끝에 `Enum`·`Struct`·`Array`·`AssetId`·`SoftObjectReference`·`Digest256`를 붙이고 TypeRegistry의 상한 검사를 바꾼다.
- 규칙(등록 때 검사): `kind == ObjectReference` ⇔ (`value.kind == ObjectReference`) 또는 (`value.kind == Array`이고 `element.kind == ObjectReference`). `value.type`은 Enum·Struct·ObjectReference·SoftObjectReference에서만 쓴다. Array 원소는 Array일 수 없다.
- **TypeRegistry 종류 구분**: 레코드에 종류(Class·Struct·Enum)를 둔다. 기존 `Acquire(TypeId)`는 **클래스만** 돌려준다(다른 종류는 TypeMismatch — ObjectRegistry·GraphSave·GraphLoad의 기존 호출자는 모두 클래스를 가정한다). struct·enum은 `AcquireValueType(TypeId, TypeKind)`으로 연다. 등록은 `RegisterStruct(const StructSchema&, RuntimeStructOps)`·`RegisterEnum(const EnumSchema&, RuntimeEnumOps)`(`RuntimeEnumOps{nativeType, module}` — B3b 구현).
- **의존 계수**: 클래스(또는 struct)가 가리키는 enum·struct가 등록 때 이미 있어야 하고(없으면 NotFound), 참조는 lease가 아니라 base와 같은 `dependents` 계수로 표시한다 — 가리키는 레코드 포인터 목록을 등록 단일 할당 안에 두고 `DestroyRecord`에서 각각 줄인다(lease로 쥐면 Shutdown이 영구 Busy가 된다). 의존자가 남은 레코드의 Unregister는 Busy다.
- **native 타입 대조**: struct 필드의 생성 ops는 정적 타입 `S`의 `NativeTypeKey<S>()`를 싣고, 등록이 그 필드가 가리키는 struct 레코드의 native 키와 같은지 확인한다(schema로 해석한 경로와 정적 thunk가 어긋나 엉뚱한 멤버에 쓰지 않게). enum도 같다.
- **모듈 등록**(B1에서 구현, 클래스만): 집계 헤더의 `RegisterModuleTypes(ModuleRegistration&, const ModuleLease& = {}) -> Result<void>`가 먼저 `registration.EnterModule(모듈 키)`로 이 registration에 이미 들어온 모듈이면 바로 성공한다(의존 그래프의 여러 경로로 불려도 모듈마다 한 번 — 호출 수가 경로 수만큼 늘지 않는다). 이어서 **리플렉션 타입을 가진 의존 모듈의 `RegisterModuleTypes`를 부르고**(import한 descriptor의 모듈 집계 헤더를 include한다), 자기 모듈의 enum → struct(중첩 의존 위상 순) → class(base 먼저) 순으로 등록한다(B3가 enum·struct를 더한다). 같은 ID·같은 native 키로 이미 등록된 타입은 새로 등록하지 않고 그 타입의 lease를 붙잡는다 — 여러 registration이 같은 의존을 공유해도 한 번만 등록되고, 쓰는 쪽이 남아 있는 동안 그 타입은 해제되지 않는다(다른 native 키면 AlreadyExists). 중간 실패면 이 호출의 기록을 역순으로 되돌린 뒤 오류를 돌려준다. `ModuleRegistration`은 모듈·등록 토큰·lease 기록이며 역순으로 되돌린다(객체나 다른 기록의 lease가 남아 있으면 그 해제는 Busy다).

### 3.6 `FieldValue`와 thunk

- `FieldValue`에 `digest`(Sha256Digest)와 `type`(Enum 값의 enum ID)을 더한다. AssetId·SoftObjectReference는 `guid`·`string`을 쓴다.
- 단일 값 종류 필드는 지금처럼 `read`·`write`·`writeStaged` thunk를 가진다. Enum은 3.4절 검증, SoftObjectReference는 null 규칙·경로 상한을 검증하고 String처럼 새 값을 만든 뒤 Swap으로 게시한다. Struct·Array 필드는 `FieldValue`로 표현할 수 없으므로 `read`·`write`·`writeStaged`가 null이고(통째 쓰기는 Unsupported — 멤버·원소 단위 쓰기는 B6 경로 접근) Transient가 아니면 `encode`·`decode`가 필수다(TypeRegistry·GraphValues의 "모든 typed 필드에 read·write·writeStaged" 검사를 종류별로 바꾼다 — 4.6·5.6절).
- `FieldWriteOperation`의 바인딩은 `ValueType`과 읽기 전용 여부를 보관한다(B3c 구현 — `detail::FieldWriteBinding`). ReadOnly·대상 타입·enum·상한 검증을 **한 곳**(`detail::ValidateFieldWrite`)에 모아 FieldWriteOperation과 경로 쓰기(7절)가 같이 쓴다.

## 4. `ROCKS_STRUCT`(B4)

### 4.1 선언

```cpp
ROCKS_STRUCT(Version = 1)
struct BulkDataRef {
    ROCKS_PROPERTY() uint32 bulkId = 0;
    ROCKS_PROPERTY() uint64 rawBytes = 0;
    ROCKS_PROPERTY() Sha256Digest digest;
};
```

- base 없는 `struct`·`class`(상속은 v1에서 금지 — RMC2301), 템플릿 금지(UI의 `Sides<float>`는 비템플릿 구체 타입으로 감싼다). 모든 비정적 필드는 PROPERTY 또는 NATIVE.
- 생성: `explicit T(Allocator&) noexcept` 또는 `T() noexcept`, noexcept 소멸자. 배열 원소가 되려면 noexcept 이동 가능해야 한다(`Array`의 static_assert가 이미 강제한다).
- 중첩: struct 필드는 다른 struct를 담을 수 있다. 타입 그래프에 순환이 있으면(배열을 거친 자기 참조 포함) RMC2302, 중첩 깊이는 16 이하(RMC2303 — EIP 635행의 재귀 깊이 제한).

### 4.2 스키마·등록·생성 코드

- struct 스키마는 클래스와 같은 `TypeSchema`다(base 없음, 클래스 전용 플래그 없음). struct 필드도 identity 명세의 `field`다. 레지스트리는 enum처럼 기존 레코드 칸(schema·ops)을 쓴다(4.6절) — `TypeOf<S>()`·필드 배열·B6 경로 해석이 클래스와 같은 형태로 동작한다.
- 생성 코드: `GeneratedType<S>`가 schema, constexpr 형질(`ContainsStrongReference`·`RequiresWorldOnlyOwner`), 멤버 codec thunk(6.1절)·Trace를 낸다. 다른 헤더(같은 모듈 포함)의 struct를 쓰는 생성 헤더는 **그 struct의 생성 헤더를 include한다**(같은 특수화를 두 번 정의하지 않는다 — ODR). 그래서 생성 중 의존 모듈의 stage 생성 루트를 넘긴다(1.3절).
- `RuntimeStructOps`: 크기·정렬, native 키, 임시 값 생성·소멸(struct hook), Trace, 선택 마이그레이션, 멤버 codec 함수표(4.6절). 경로 접근 thunk는 B6에서 더한다(7절).

### 4.3 참조를 담는 struct

- struct가 `ObjectRef<T>`(직접 또는 중첩 struct 안 — `ObjectRefArray<T>` 멤버는 B5)를 담을 수 있다("참조 struct", `ContainsStrongReference` = true). `ObjectRefArray`·`StructRefArray`·`Array`를 담으면 struct가 `explicit S(Allocator&) noexcept`로 allocator를 전달해야 한다. 소유 클래스의 생성 Trace가 그 경로를 따라 방문한다. 참조 쓰기는 소유 객체 저장소 안의 주소이므로 기존 `SetReference`·배열 소유자 검사의 범위 검사를 통과한다.
- 참조 struct의 목록은 **B5에서 지원한다**(2026-11-13 사용자 결정 — UE `UStaticMesh::StaticMaterials`·머티리얼 인스턴스 파라미터 배열 대응). `Array<S>`의 버퍼는 소유 객체 밖이고 `ObjectRef`는 복사·이동할 수 없으므로(`ObjectRefs.h`) `Array<S>`가 아니라 `ObjectRefArray`처럼 Registry가 관리하는 별도 컨테이너 **`StructRefArray<S>`**(원소마다 블록 — 주소 고정, 5.6절)로 하고(`Array`의 nothrow-move static_assert는 유지), 그 원소 안 참조 쓰기는 SetEdge 범위 검사를 컨테이너 기준으로 넓힌다. `ObjectRefArray`·`StructRefArray`를 담은 struct는 클래스 필드나 인라인 struct 멤버로만 둘 수 있고 `StructRefArray`의 원소가 될 수 없다(B5 — 12절 결정 15). B4는 이를 막지 않는다: struct thunk는 (소유 객체 handle, struct 주소)로 일하고 참조 멤버 쓰기를 `detail::SetStructMemberReference` 하나로 모은다.
- 참조 struct 값은 통째로 복사·대입할 수 없으므로 도구의 struct 단위 쓰기는 경로 접근(7절)으로 멤버마다 하고, **struct 단위 원자성은 없다**(멤버 하나의 쓰기만 원자적이다).

### 4.4 직렬화와 버전

- struct 값은 중첩 Record(wire `Record` = 10)다. 헤더 schema = struct ID, schema version = struct 버전, 필드는 struct 필드 ID TLV다. 중첩 TLV도 그래프의 필드 누계(`maxTotalFields`)에 센다. Binds Core 값 타입을 포함한 **모든 struct가 태그형 중첩 Record**다 — 고정 배치는 쓰지 않는다(2026-11-13 사용자 결정: 용량이 늘어도 유연한 구조. 중첩 값만 떼어도 온전한 Record라 도구·클립보드·경로 접근이 같은 Record 기능을 쓴다).
- 로드: struct ID가 다르면 TypeMismatch. 버전이 낮으면 마이그레이션 — 로드 옵션 override(`ObjectGraphLoadOptions.migrateStruct`, 시험·도구용)를 먼저 묻고(Unsupported를 내면 다음으로), 타입이 등록한 마이그레이션(`RuntimeStructOps.migrate` — UE가 타입별로 옛 버전을 올리는 것과 같은 자리)을 쓰며, 둘 다 없으면 Unsupported. 결과 Record는 정확히 현재 버전이어야 하고(아니면 Unsupported — 객체와 같다) 다시 마이그레이션하지 않는다. 버전이 높으면 decode 정책(6.1·6.4절)을 따른다 — 기본 거부(Unsupported), `LoadNewerSkippingUnknown`이면 현재 schema로 읽어 모르는 필드를 건너뛰고 `NewerVersion` 진단을 낸다(AS 5.3절의 "높으면 읽기 전용 열기"는 A1이 이 정책과 저장 금지를 함께 건다). 패키지 Summary의 타입 버전 표(AS 5.2절 Summary)에 struct 타입도 넣는다(A1).
- 모르는 필드는 건너뛰고 `UnknownField` 진단을 내며, 누락은 객체와 같은 `allowMissingFields` 정책을 따른다.

### 4.5 Core 값 타입 결합(`Binds`)

- 색·벡터 설정(`PaddingColor`·`ChromaKeyColor`는 UE FColor(8비트), `AlphaCoverageThresholds`는 FVector4(LWC double), 12.4절)은 Core 수학 타입을 쓴다. Core는 MetaSchema·ObjectRuntime에 의존하지 않으므로 Core 헤더에 annotation을 달 수 없다.
- 별도 Metadata 모듈 **`CoreTypeBindings`**(Core/Source/Runtime, 소스 없음, ObjectRuntime 의존)에 미러 선언 헤더 `CoreTypeBindings/CoreValueTypes.h`를 두고 `ROCKS_STRUCT(Binds = "<실제 타입 철자>", Version = 1)`로 같은 공개 멤버를 가진 미러 선언을 리플렉션한다(UE NoExportTypes 방식). ObjectRuntime을 Metadata 모듈로 만들지 않는다(그것에 닿는 모든 타깃에 meta 단계가 붙지 않게) — Core 값 타입 필드를 쓰는 모듈만 의존한다. 생성 코드는 미러가 아니라 실제 타입의 멤버 포인터를 쓰고, 미러와 실제 타입의 멤버 이름·형식·개수·순서가 같은지 static_assert로 확인한다(어긋나면 컴파일 실패, 4.6절).
- 대상(Core 헤더 확인 결과): `rocks::Vector2<float|double>`·`Vector3<float|double>`·`Vector4<float|double>`, `Quaternion<float|double>`, `Rotator<float|double>`(yaw·pitch·roll 라디안 — UE FRotator는 pitch·yaw·roll 도라 UE 데이터 반입 때 변환), `Color`(uint8 RGBA), `LinearColor`(float RGBA), `IntVec2`·`IntVec3`(int32), `Transform`(비템플릿 — `Vec3d translation`·`Quat rotation`·`Vec3 scale`, 다른 Binds struct에 의존). ID는 `CoreTypeBindings` 명세에 등록한다.

### 4.6 B4 구현 API(확정안)

4.1–4.5절을 구현하는 구체 형태다. 설계 교차검증(2026-11-13 — 1차: 구현 적합성·반례·MetaCompiler와 UE 5.6 대조, 2차 최종: 런타임·MetaCompiler/Binds)의 지적과 사용자 결정(4.3·4.4절)을 반영했다. 세 커밋으로 나눈다: **B4a** 런타임, **B4b** MetaCompiler, **B4c** Binds.

**상수·스키마**

- `MaxStructDepth = 16`(MetaSchema) — 깊이 = 1 + 멤버 struct 깊이의 최댓값(멤버 struct가 없으면 1). MetaCompiler(RMC2303)·Registry·생성 static_assert가 같은 상수를 쓴다. 문맥 프레임은 최대 1(최상위 필드) + 16 = 17개다. B5 배열 원소의 index는 **같은 프레임**에 둔다(원소마다 새 프레임을 만들지 않는다) — `BuildPath`의 33 안이다.
- `ValueKind::Struct` 필드: `value.type` = struct ID, `FieldKind::Value`.

**레지스트리(B4a)**

```cpp
using StructFieldEncodeFunction = Result<uint32> (*)(const RuntimeStructFieldOps& self,
    const void* structValue, ArchiveWriter& out, FieldEncodeContext&) noexcept;
using StructFieldDecodeFunction = Result<void> (*)(const RuntimeStructFieldOps& self,
    ConstructionContext* construction, ObjectHandle owner, void* structValue,
    const FieldView& field, FieldDecodeContext&) noexcept;
struct RuntimeStructFieldOps {
    Guid fieldId{};
    StructFieldEncodeFunction encode = nullptr;
    StructFieldDecodeFunction decode = nullptr;
    const void* valueNativeType = nullptr;   // Enum·Struct 멤버의 NativeTypeKey
};
using StructMigration = Result<Array<uint8>> (*)(Allocator&, TypeId, uint32 sourceVersion,
    uint32 targetVersion, Span<const uint8> sourceRecord) noexcept;
struct RuntimeStructOps {
    usize size = 0;
    usize alignment = 0;
    const void* nativeType = nullptr;
    void (*construct)(void*, Allocator&) noexcept = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    void (*trace)(const void*, TraceVisitor&) noexcept = nullptr;   // 참조 struct만
    StructMigration migrate = nullptr;                                // 선택
    bool requiresWorldOnlyOwner = false;                              // 생성 형질
    ModuleLease module;
    Span<const RuntimeStructFieldOps> fields;
};
Result<TypeRegistrationToken> TypeRegistry::RegisterStruct(const TypeSchema&, RuntimeStructOps) noexcept;
```

- decode의 세 방식: **검증**(construction·structValue null — 멤버 주소를 만들지 않는다), **관찰**(construction null, structValue = scratch 임시 값 — 직접 쓴다), **적용**(둘 다 non-null — 소유 객체 안 주소).
- 레코드는 `RegisterEnum`처럼 기존 칸을 채운다: `record->schema`(id·version·name·fields), `record->ops`(size·alignment·nativeType·module — `ModuleRegistration::Retain`의 native 대조가 그대로 동작). 추가 칸: 스키마 순서로 정렬해 깊게 복사한 멤버 함수표, 필드 ID 정렬 색인, construct·destroy·trace·migrate, 계산 값 `depth`·`containsStrongReference`(Transient 참조 포함 — GC는 추적한다)·`requiresWorldOnlyOwner`(입력 OR 멤버 struct)·`containsRequired`(Required 후손이 있는지 — 누락 진단 가지치기).
- 필드 검증은 `Register`와 공유하는 함수로 뽑는다(4096 필드 상한·플래그·값 종류·도구 이름·Required 규칙). Struct 필드를 위해 지원 값 종류·typed 판정·`valueNativeType` 대조(Enum|Struct)·의존 탐색과 `fieldTypes`를 일반화한다. `FieldEnum`은 의존 레코드의 종류를 확인한다.
- `RegisterStruct` 규칙: version ≥ 1, base 없음·클래스 플래그 없음, Transient가 아닌 모든 필드에 encode·decode 둘 다(Transient는 둘 다 없거나 둘 다), 멤버 함수표는 스키마 필드와 1:1(같은 ID 둘·없는 ID는 InvalidArgument), size·alignment·nativeType·construct·destroy 필수, `trace != null ⇔ containsStrongReference`, depth > `MaxStructDepth`는 InvalidArgument, 같은 nativeType의 다른 ID는 AlreadyExists, 멤버 struct·enum이 미등록이면 NotFound·종류가 다르면 TypeMismatch·native 키가 다르면 TypeMismatch. 순환은 의존이 먼저 등록돼야 하므로 레지스트리에서는 생기지 않는다.
- 클래스의 Struct 필드: `read`·`writeStaged`는 null, `write`는 Struct 종류로 묶어 호출하면 **Unsupported**(3.6절 — 멤버 단위 쓰기는 B6 경로 접근), encode·decode 필수, `valueNativeType` = struct native 키. 클래스가 WorldOnly가 아닌데 struct의 `requiresWorldOnlyOwner`가 참이면 InvalidArgument(MetaCompiler RMC2206이 먼저 막는다).
- `TypeLease::FieldStruct(usize) const&`(`const&& = delete`) → `StructView`. struct 레코드 lease는 `AcquireValueType(id, TypeKind::Struct)`로 열고 `Schema()`·`Ops()`·`StructOps()`.
- `StructView`(비소유 — 얻어 온 lease가 사는 동안 유효하다. 의존 사슬이 대상 레코드를 살린다): `Schema()`, `StructOps()`, `FieldOps(i)`(스키마 순서), `FieldIndex(Guid)`(정렬 색인 이진 탐색), `FieldEnum(i)`, `FieldStruct(i)`, `Depth()`, `ContainsStrongReference()`, `ContainsRequired()`, `IsEmpty()`.

**Serialization(B4a)**

- `RecordReader::OpenRecordField(Allocator&, const FieldView&, Guid schema, uint32 minVersion, uint32 maxVersion, uint32 maxFields) const` — 6.1절의 새 API. 이 Record의 TLV인지(범위) 검사하고 공유 예산을 쓰되 **헤더(44)·TLV 색인(29n)·정렬(2n·log n)만 청구한다** — payload 바이트는 바깥 Open이 이미 청구했으므로 다시 청구하지 않는다(중첩 깊이만큼 작업이 불어나지 않는다). 크기 한도는 바깥 reader의 한도. schemaVersion 0은 InvalidArgument, 버전 범위 밖은 Unsupported, schema가 다르면 TypeMismatch. 구현은 색인 전용 내부 파싱(범위 검사와 위치 이동만)이다.
- `RecordWriter`(Begin·BeginField·EndField·End) — 바깥 `ArchiveWriter` 위에 Record를 제자리로 쓴다(header·TLV header 자리를 먼저 쓰고 값은 바깥 writer에 바로 쓴 뒤 wire·길이·개수를 채운다). 값 byte를 한 번만 쓰므로 중첩 깊이만큼 복사·작업이 늘지 않고, 같은 필드를 같은 순서로 쓰면 `RecordBuilder::Build`와 같은 byte다(구현 교차검증 반영 — 처음 안의 `RecordBuilder::AppendTo`는 단계마다 복사가 남아 바꿨다).

**문맥(B4a, `Object/FieldCodec.h`)**

- `FieldCodecFrame += StructView structure`, `FieldScope(context, field, declaringType, enumeration = nullptr, structure = {}, index = NoIndex)`.
- 공통: `virtual Result<void> AddFields(usize) noexcept` — 저장·1단계는 그래프 `maxTotalFields` 누계, 2단계 no-op. `ReportUnknownField(Guid)`는 프레임이 있으면 그 path(감싼 struct 필드까지)를 넣는다.
- 저장 문맥: `NestedRecordLimits()`(그래프 옵션에서 — 중첩 Record 전체 크기와 값 하나 모두 필드 값 한도 안).
- **복원 Record 스택**: 문맥이 `MaxStructDepth + 1`개 reader 자리를 소유한다. `RecordScope`는 (문맥, 깊이)만 갖는 RAII다(`[[nodiscard]]`, 이동 가능 — 이동된 원본은 pop하지 않는다, LIFO가 아니면 Fatal). `CurrentRecord()`는 맨 위 reader(non-const — `Next()` 순회)다. `OpenValue`는 스택이 비면 지금 경로(1단계 노드 Record, 2단계 보관 FieldView), 아니면 맨 위 reader로 연다 — 마이그레이션 결과 안의 멤버도 올바르게 열린다.
- `OpenRecord(const FieldView&, TypeId, uint32 minVersion, uint32 maxVersion) -> Result<RecordScope>` — wire가 Record(10)인지 먼저 확인한다(TypeMismatch). 1단계: 맨 위(또는 노드) Record의 `OpenRecordField`, `maxFields` = 남은 `maxTotalFields`, `AddFields(fieldCount)`. 2단계: 스택이 비면 정적 `RecordReader::Open`(자체 예산 — 같은 크기 한도·버전 범위, 무제한 operation·work, `maxFields` = `maxTotalFields`), 아니면 맨 위 reader의 `OpenRecordField`를 같은 설정으로. B5 RecordList 항목 reader도 쌓을 수 있게 스택에 reader를 넣는 내부 경로를 FieldView 전용으로 닫지 않는다.
- `OpenMigratedRecord(Span<const uint8>, TypeId, uint32 version) -> Result<RecordScope>` — 마이그레이션 결과를 정확히 현재 버전으로만 연다(아니면 Unsupported). 원본 scope는 마이그레이션 전에 닫는다(스택 깊이가 불어나지 않게).
- `MigrateStruct(TypeId, uint32 from, uint32 to, Span<const uint8>) -> Result<Span<const uint8>>` — 1단계: override → (Unsupported면) 타입 migrate → 둘 다 없으면 Unsupported. 결과는 `maxMigrationBytes` 누계, 작업량은 원본 + 결과 바이트를 문맥의 `Charge`로(reader 작업 Δ 청구와 이중이 되지 않게), (TypeId, from, 소유 버퍼)를 노드 목록(`StoredStructMigration`)에 **발생 순서대로** 보관하고 view를 돌려준다(view는 `Array` 이동이 heap 블록을 유지하므로 유효하다). 2단계: 노드별 커서(`GraphApplyContext` 소유)로 다음 항목을 꺼내 (TypeId, from)을 대조한다 — 어긋남·부족·노드 끝에 남은 항목은 InvalidArgument(계약 위반 — 오류 반환·rollback, Fatal 아님 — 6.4절 2단계 규칙).
- **struct hook**(6.4절): `FieldCodecHooks`에 기본 no-op 가상 함수 — `ObservedStructs() -> Span<const TypeId>`, `ObserveStruct(const FieldCodecFrame&, TypeId, const void* nativeKey, const void* value)`, `SubstituteStruct(const FieldCodecFrame&, TypeId, const void* nativeKey, const void* in) -> Result<const void*>`. 계약은 6.4절(한 번의 해석, 반환 수명, 참조 struct 제외).

**struct 값 codec(B4a, `Object/GeneratedFieldCodec.h`)**

- `EncodeStructRecord(StructView, const void* value, ArchiveWriter& out, FieldEncodeContext&)`: 참조 struct가 아니면 `SubstituteStruct` → `RecordWriter::Begin(out, id, version, NestedRecordLimits())` → 스키마 순서로 멤버마다 `FieldScope(멤버, struct ID, FieldEnum(i), FieldStruct(i))`, 유효 플래그가 제외면 건너뜀, `BeginField` → thunk가 out에 바로 쓰고 wire 재확인 → `EndField(wire)` → `End`. `AddFields(쓴 TLV 수)`, 작업량은 out의 청구(호출자가 out의 작업을 청구한다).
- `DecodeStructRecord(StructView, ConstructionContext*, ObjectHandle owner, void* value, FieldDecodeContext&)`: `CurrentRecord()`를 순회한다 — `FieldIndex`, 모르는 필드 → unknownField 정책(path 포함), 저장된 Transient → Unsupported, 멤버 thunk, 누락 → `IsMissingAllowed`가 아니면 NotFound, 허용된 누락 중 Required는 진단, 누락된 Struct 멤버는 `ContainsRequired()`일 때만 스키마를 재귀로 돌며 Required 후손마다 전체 path로 진단(방문마다 `Spend(1)`, 2단계는 생략). present는 스택 bitset(4096 bit).
- `DecodeStructField(StructView, ConstructionContext*, ObjectHandle, void* value, const FieldView&, FieldDecodeContext&)`: 1단계에서 관찰 대상이면 scratch에 `construct`(모든 경로에서 RAII `destroy`)하고 value = scratch로 이후를 **한 번에** 해석한다 → `OpenRecord(view, id, 1, UINT32_MAX)` → 버전이 같으면 그대로, 낮으면 원본 Record 바이트로 `MigrateStruct` → 원본 scope를 닫고 `OpenMigratedRecord`, 높으면 newerVersion 정책(`LoadNewerSkippingUnknown`이면 `NewerVersion` 진단 — path = 이 struct 필드, 현재 스키마로 해석, 모르는 필드 Reject가 우선) → `DecodeStructRecord` → 관찰이면 `ObserveStruct`.
- 정책 판정(모르는 필드·Transient·누락·NewerVersion)은 그래프 `DecodeNode`와 같은 술어 함수(`detail::RecordFieldRules`)를 쓴다. 청구는 호출자 몫이다 — 객체 루프는 1배(2단계가 보관 결과를 재생한다), struct는 `Spend`의 2배(2단계가 다시 해석한다). `OpenRecordField`의 작업은 그래프 `DecodeNode`의 reader 작업 Δ가 2배로 청구하므로 codec이 따로 `Spend`하지 않는다.
- 멤버 thunk 도우미(생성 코드·수동 시험 공용) `template <class S, auto Member> RuntimeStructFieldOps StructFieldOps(Guid)`: 단일 값 멤버는 `EncodeFieldWire`·`ParseFieldValue`(검증 방식은 value에 손대지 않는다), Enum 멤버는 정적 `GeneratedType<E>::IsDeclared`도 확인(관찰 방식의 ReportAndDefault는 대체 값을 scratch에), 참조 멤버 적용은 `detail::SetStructMemberReference`(지금은 `SetReference` — B5에서 컨테이너 기준 검사로 넓힌다), String 멤버는 `TryAssign`(적용은 새 객체 — 실패하면 객체째 rollback), Struct 멤버는 `EncodeStructRecord`·`DecodeStructField` 재귀, `MatchesSchema`(Struct 멤버는 `value.type == GeneratedType<S>::Id()`).
- 클래스의 struct 필드 thunk: encode는 owner Borrow → `EncodeStructRecord(context.Current()->structure, &(owner->*member), …)` → 수명 재확인, decode 1단계는 `DecodeStructField(view, nullptr, handle, nullptr, …)`, 2단계는 `construction->Get<Owner>(handle)`의 멤버 주소. `FieldWireType(Struct)` = Record(10).

**그래프(B4a, ObjectSerialization)**

- `ObjectGraphLoadOptions.migrateStruct`(override, `ObjectGraphMigration` 형태 — type 인자가 struct ID, `migrationUser` 공유).
- `maxTotalFields` 기본값 1,048,576(5.5절). Record별 `maxFields`는 남은 누계로 연다(최상위·중첩 모두 — 큰 입력이 누계 검사 전에 색인을 할당하지 않게). `node.fields` 예약은 min(FieldCount, 스키마 필드 수).
- `LoadNode += Array<StoredStructMigration>`, 2단계 소비 커서는 `GraphApplyContext`가 노드별로 갖는다(`ConstructGraph`의 const 순회와 충돌하지 않게).
- `ValidateLayout`: Struct 필드는 `FieldStruct(i)`가 비어 있지 않아야 한다. `FieldScope` 호출 네 곳에 `FieldStruct(i)`를 넘긴다.
- 저장 청구 정정: `SaveNodeFields`가 필드마다 누적 payload 바이트를 청구하던 과대 청구(B3 이전부터)를 RecordBuilder의 실제 작업(성장할 때만 복사) 하나로 바꾼다(선청구와 겹치지 않게).

**Trace**

- 모든 struct에 생성 `GeneratedType<S>::Trace(const S&, TraceVisitor&) noexcept`를 낸다(비참조 struct는 빈 본문 — 생성 클래스 Trace는 비템플릿 람다라 `if constexpr`의 버려진 분기도 검사되기 때문). 참조 struct는 직접 `ObjectRef` 멤버를 Visit하고 멤버 struct의 Trace를 부른다. 클래스 생성 Trace는 struct 필드마다 그 Trace를 부른다. `RuntimeStructOps.trace`는 참조 struct만 같은 함수를 `const void*`로 감싼 것이다(B5 컨테이너·도구용).

**MetaCompiler(B4b)**

- 마커: `Annotations.h`에 `rocks.struct.marker`, `BindPrefixMarkers`·`PrefixSourceVerifier`에 세 번째 모드, 한 레코드에 CLASS·STRUCT 마커가 함께 있으면 오류, `Specifiers.Struct`(Version·Binds).
- 검사: base가 하나라도 있으면 RMC2301, 템플릿은 기존 RMC2000, 모든 비정적 데이터 멤버는 PROPERTY 또는 NATIVE, 생성자·소멸자 noexcept static_assert. struct는 base·`ObjectRef`·`SoftObjectRef` 대상에서 거부(`Resolve`), Base 해석·`Order()`는 struct를 건너뛰되 필드 목록 구성·RMC2203 검사·"선언 없음" 검사·IdentityInventory에는 넣는다. `KindHasTargetType`에 Struct, descriptor 빈 ID 규칙, RMC2006(public 타입의 private struct 필드).
- 순환 RMC2302: native 철자로 푼 필드 간선 DFS를 Order·깊이 계산·생성보다 먼저 한다(Binds 미러는 C++에서 순환을 만들 수 있다 — 미러가 자기가 묶는 실제 타입을 필드로 가지는 경우). import 쪽은 `LoadImports`에서 같은 DFS·깊이 계산(descriptor에 깊이 키는 없다). 깊이 RMC2303, 생성 코드에 `static_assert(depth <= ::rocks::MaxStructDepth)`.
- 형질: `ContainsStrongReference`(ObjectReference 필드 — Transient 포함 — 또는 그런 struct). `RequiresWorldOnlyOwner`: struct의 NATIVE 멤버가 RMC2206 형질을 가져도 struct 자체는 오류가 아니라 형질을 전파한다. 생성 struct는 `static constexpr bool RequiresWorldOnlyOwner`와 모델 값이 직접 멤버의 형질을 포함하는지 확인하는 static_assert를 내고, WorldOnly가 아닌 클래스는 struct 필드마다 `static_assert(!GeneratedType<S>::RequiresWorldOnlyOwner)`를 낸다(import 플래그를 믿지 않는다). NATIVE 멤버의 직접 타입이 리플렉션 struct면 그 생성 헤더를 include하고 `static_assert(!GeneratedType<S>::ContainsStrongReference)`(RMC2205의 생성 대조) — `NativeMemberTraits`에 `GeneratedType` 탐지 특수화를 두지 않는다(생성 헤더가 없으면 조용히 false가 되는 ODR 위반).
- 생성: `GeneratedType<S>`(Id·Schema·Ops·형질·Trace·Register), struct가 `static Result<Array<uint8>> Migrate(Allocator&, TypeId, uint32, uint32, Span<const uint8>) noexcept`를 가지면 `Ops().migrate`에 연결, 클래스 struct 필드의 read·writeStaged는 nullptr·write는 Struct 종류, 헤더 안 특수화는 위상 순서, 다른 헤더(같은 모듈 포함)·import base의 상속 struct 필드가 쓰는 struct의 생성 헤더 include(header≠include 규칙), import 필드 Reference ID 채우기 확장.
- descriptor struct 항목 키(1.4절): kind·name·id·version·header·visibility·flags·binds·fields.
- 모듈 등록: `ModuleRegistration::RegisterStructIfAbsent`, 집계 순서 enum → struct(의존 위상 순서, 같은 깊이는 이름 순) → 클래스.

**Binds(B4c)**

- 모듈 `CoreTypeBindings`: ReflectionTestFixtures와 같은 형태(`CoreModuleRules`, 소스 없음, `RequiredModules = ["ObjectRuntime"]`, Group Runtime, `CoreTypeBindings.rocksids.json`)이고 SDK 타깃에서 도달하게 한다. Binds는 이 모듈에서만 허용한다(다른 모듈의 Binds는 RMC2004 — 무관한 두 모듈이 같은 타입을 묶으면 ODR 위반·런타임 AlreadyExists가 되므로). 소비 모듈이 의존하지 않고 Core 값 타입 필드를 쓰면 "CoreTypeBindings에 의존하라"는 힌트를 진단에 붙인다.
- 철자: Binds가 있으면 MetaCompiler가 같은 합성 TU에 `using rocksBinds_N = <철자>; static_assert(sizeof(rocksBinds_N) > 0);`를 붙여 Clang을 한 번 더 돌리고(`-ast-dump-filter`로 출력을 줄인다) 기존 `TypeSpelling`의 정규 철자를 쓴다(문자열 비교 금지). 이 실행의 오류는 미러 선언 위치로 보고한다. `ClassModel.NativeName`(Binds 정규 철자, 없으면 Name)을 생성 코드 전체가 쓰고(`::` + 이름, `&::{owner}::{name}`), schema 이름은 미러 이름이다. descriptor `binds` 검증기는 템플릿 철자를 받고 대상이 리플렉션 타입·미러면 거부한다. import와 자기 모듈의 native 철자 색인을 두고(Transform 미러가 같은 모듈의 Vector·Quaternion 미러를 쓴다), 미러 타입을 필드 형식으로 직접 쓰면 거부한다.
- 대조(생성 static_assert): 멤버마다 `__is_same(decltype(Real::m), T)`, 개수·순서는 비템플릿 inline 함수의 구조적 바인딩 `[[maybe_unused]] const auto& [m0, …] = value;`와 위치별 `__is_same(__remove_cv(decltype(mi)), T)`(private 데이터·union·base 분할도 컴파일 실패), noexcept 생성·소멸은 실제 타입에 건다. sizeof·offset 대조는 쓰지 않는다(Transform은 데이터 52 B·sizeof 56 — 꼬리 패딩에 숨은 멤버를 놓친다).

**시험(11절 B4 행 + 교차검증 추가분)**

- 레지스트리: 공유 필드 검증·trace⇔참조·깊이 17 거부·native 대조·같은 native 다른 ID·의존 Busy·FieldEnum 종류 검사·클래스 struct 필드 write Unsupported·WorldOnly 형질 전파.
- codec·그래프: 3단 중첩·참조 struct 왕복과 재저장 byte 동일, 낮은 버전(타입 migrate·override와 Unsupported 이어 넘김·결과가 현재 버전 아님·버전 0·마이그레이션 결과 안 멤버의 OpenValue·2단계가 보관 결과를 재사용 — callback 1회·커서 어긋남 오류), 높은 버전(거부·건너뛰기·Reject 우선), 모르는 필드 path, 누락 struct 안 Required 진단과 가지치기, struct 안 EditorOnly 제외·ReadOnly 전파, struct 안 미선언 enum(path 2단)·정적 enum 확인, 모든 작업 한도 실패가 생성자 전, 깊이 16에서 로드·저장 작업이 깊이 배수로 불어나지 않음, struct hook(한 번의 해석·중첩 치환 수명·참조 struct 거부·미등록 관찰 ID), Trace가 struct 안 참조를 방문(GC 생존).
- MetaCompiler: RMC2301·2302(Binds 미러 C++ fixture·조작 descriptor)·2303·2006·2205·2206 전파, 생성 코드·descriptor·import·include·등록 순서.
- Binds: alias 정규화(`rocks::Vec3` → `rocks::Vector3<float>`), 다른 모듈 Binds RMC2004, 미러 타입 필드 거부, 의존 누락 힌트, 대조 실패(이름·형식·개수·순서) 컴파일 실패 probe.

## 5. 배열(B5)

### 5.1 `Array<T>`

- 원소: 스칼라·`String`·`Guid`·`AssetId`·`Sha256Digest`·enum·참조 없는 struct(Binds Core 값 타입 포함)·`SoftObjectRef<U>`. 참조 목록은 `ObjectRefArray<T>`, 참조 struct 목록은 Registry가 관리하는 `StructRefArray<S>`(4.3·5.6절)다. `Array<ObjectRef<…>>`·`Array<참조 struct>`는 `Array`의 nothrow-move static_assert에서 Clang 단계가 먼저 실패하므로 MetaCompiler가 그 진단을 RMC2304로 바꿔 보고하고, 중첩 배열은 RMC2304다(5.6절).
- 필드는 소유 타입 생성자가 allocator로 초기화한다. struct 원소가 `explicit T(Allocator&) noexcept`면 그 allocator로 만든다.
- `ValueKind::Array`, `element`가 원소 형식이다.

### 5.2 `ObjectRefArray<T>`

- `FieldKind::ObjectReference`, `value.kind = Array`, `element = {ObjectReference, T의 ID}`. 생성 Trace가 `field.Trace(visitor)`를 부른다. inline struct 안에도 둘 수 있다(4.3절).
- **원소 null을 허용한다**(2026-11-13 사용자 결정 — UE `TArray<TObjectPtr>`와 같고, `OverrideMaterials`처럼 "index = 슬롯, null = 재정의 없음"인 배열을 그대로 표현한다). null 원소는 간선이 아니다. `Required`를 붙이면 원소마다 non-null이어야 한다(단일 참조와 같은 규칙 — 저장·쿠킹에서 거부, 로드는 진단, 2.3절).
- 변경 API: 기존 `AddReference`·`RemoveReference`·`RemoveReferenceAt`·`ClearReferences`(null을 받게 바꾼다)에 도구 편집용 `InsertReferenceAt(index)`·`SetReferenceAt(index)`를 더한다(non-null 대상은 삽입 장벽, 실패 시 내용 불변).

### 5.3 staged 변경 API

- 그래프 로드용 `ConstructionContext::AssignReferences(handle, ObjectRefArray<T>&, Span<const ObjectHandle>)`를 더한다(원소별 호출 대신 한 번 — 원소마다 slot 탐색·성장을 반복하지 않고 예산에 청구한다). `StructRefArray`는 `AssignElements`·원소 참조 staged 쓰기를 더한다(5.6절).
- Registry 쪽 변경: 소유자 검사를 Borrow 대신 **같은 트랜잭션의 Reserved**로 판정한다(Reserved 객체는 Borrow를 얻을 수 없다). 대상은 null, Alive 또는 같은 트랜잭션의 Reserved를 받는다(그래프 순환). shade는 SetEdge처럼 대상이 Alive일 때만 한다(Marking 중 Reserved 대상에 StaleHandle을 내지 않게). 잠금 밖 버퍼 성장 동안 `processing_`을 세운다(CreatePending과 같이 — allocator 재진입 Rollback이 use-after-free가 아니라 Fatal이 된다). Commit의 staged 간선 검증은 Trace로 원소도 방문한다. 생성 중에는 GC Begin·Step이 Busy라 수집과 겹치지 않는다.

### 5.4 wire

- 단일 값 원소 배열(스칼라·String·Guid·AssetId·Digest·enum·SoftObjectRef): Serialization에 **`ValueArray`**(wire `ValueArray = 13`)를 더한다. payload = u32 원소 wire tag, u32 count, count개의 원소 payload(그 wire tag의 Archive 표현을 이어 붙인다 — Bool 1, U32 4, U64·I64·F64 8, F32 4, Guid 16, String·Bytes u64 길이 + byte, SoftObjectReference Guid + String). 원소마다 Record envelope(최소 52 byte)를 쓰는 RecordList로는 배열 원소 한도가 그래프 바이트 한도를 넘기 때문이다.
- reader 검증: 원소 tag는 schema 원소 종류의 wire와 같아야 하고 Record·RecordList·ValueArray tag는 원소로 받지 않는다. count는 할당 전에 **schema 원소 종류의 최소 크기**(Digest 40, String·Bytes 8, SoftObjectReference 24, U64 8 …)로 남은 byte와 비교하고 count만큼 work를 먼저 청구한다. Record 트리 예산을 공유한다(codec은 필드 payload를 문맥의 `OpenValue`로 열어 `ValueArrayReader::Open`에 넘긴다 — 5.6절).
- 원소 폭은 단일 값과 같다(UInt8·UInt16·UInt32 → U32, Int32·Int64 → I64 …) — 정수 폭을 넓히는 스키마 변경이 wire 호환으로 남는다(결정 10의 유연성 우선, 12절 결정 14). 그래서 `Array<uint8>`은 원소당 4 byte이고 대용량 byte·수치 데이터는 bulk data(A1)가 맡는다.
- struct 원소 배열(`Array<S>`·`StructRefArray<S>`): `RecordList`(11), 항목 Record schema = struct ID·버전. 로드 reader의 `RecordListLimits.maxItems`는 `MaxArrayElements`다(기본 65,536에 기대지 않는다 — 1·2단계가 같은 함수로 정한다, 5.6절).
- `ObjectRefArray`: `ValueArray` of 참조 원소(참조 wire는 문맥 매개변수 — 6.1절, graph v1은 U64 local ID, 0 = null).

### 5.5 한도

- 필드별 길이 상한 `MaxElements`: 기본 65,536, 최대 1,048,576(`MaxArrayElements`). 인코딩된 필드 값은 그래프 `maxValueBytes`(4 MiB)도 지켜야 한다(넘으면 CapacityExceeded — 예를 들어 U64 원소는 약 52만 개, U32 원소는 약 104만 개가 한계다).
- **강제 지점**: 필드별 `MaxElements`는 리플렉션 쓰기(경로 접근 7절)·저장(encode)에서 건다. **로드(decode)는 필드별 `MaxElements`를 걸지 않고** 배열마다 `MaxArrayElements`와 그래프 한도(`maxValueBytes`·`maxTotalElements`·work)로만 막는다 — `MaxElements`를 낮춰도 기존 에셋이 열리고, 넘친 데이터는 다시 저장할 때 경로가 붙은 CapacityExceeded로 드러난다(UE도 로드에 필드별 상한이 없다, 12절 결정 13). Registry 변경 API(`AddReference` 등)와 C++ 직접 사용에는 걸지 않는다(Level의 actor 목록 같은 런타임 목록을 막지 않는다 — 넘친 목록은 저장할 때 오류다).
- 그래프 전체: B4가 중첩 struct TLV를 `maxTotalFields`에 세기 시작하며 그 기본값을 4,096에서 A0-O 목표치인 1,048,576으로 올리고(필드 3개짜리 struct 배열이 원소 약 1,366개에서 막히지 않게 — Record별 `maxFields`는 남은 누계로 연다, 4.6절), B5가 `ObjectGraphLimits`에 `maxTotalElements`(기본 4,194,304)를 더한다. 바이트·작업량 한도 확장(64 MiB·8 × (필드 + 원소))은 A0-O의 graph v2가 정한다.

### 5.6 B5 구현 API(확정안)

5.1–5.5절과 4.3절의 사용자 결정(참조 struct 목록 지원)을 구현하는 구체 형태다. 설계 교차검증(2026-11-13 — 구현 적합성·반례·MetaCompiler와 UE 5.6 대조)의 지적과 사용자 결정(ObjectRefArray 원소 null 허용, 5.2절)을 반영했다. B4의 규칙을 그대로 따른다: 2단계는 입력 때문에 실패하지 않고(원소 형식·정적 enum·참조 기대는 1단계), 2단계 몫은 1단계가 한 번 선청구하며, 값 byte는 바깥 writer에 제자리로 한 번 쓰고, Record·목록 한도는 남은 누계로 연다. 생성 static_assert는 전체 특수화 안에만 두고(`if constexpr`는 라이브러리 템플릿에만), requires 탐지 형질은 쓰지 않는다. 세 커밋으로 나눈다: **B5a** Serialization과 Registry 컨테이너, **B5b** 레지스트리·문맥·codec·그래프·Trace(손으로 쓴 특수화 시험), **B5c** MetaCompiler·fixture·SDK.

**상수·스키마**

- `DefaultMaxElements = 65536`, `MaxArrayElements = 1048576`(MetaSchema `TypeSchema.h`, MetaCompiler 규칙과 같은 값).
- 배열 필드: `value = {Array, 0}`, `element` = 원소 형식, `maxElements ∈ [1, MaxArrayElements]`(MetaCompiler가 기본값을 채운다 — 스키마의 0은 "배열 아님" 하나뿐). 배열이 아닌 필드는 `element = {Opaque, 0}`, `maxElements = 0`.

| C++ 형식 | FieldKind | 원소 | wire |
|---|---|---|---|
| `Array<T>`(스칼라·Guid·String·AssetId·Digest·enum·SoftObjectRef) | Value | T의 종류 | ValueArray(13) |
| `Array<S>`(참조 없는 struct, Binds 포함) | Value | Struct | RecordList(11) |
| `ObjectRefArray<T>`(원소 null 허용) | ObjectReference | ObjectReference | ValueArray(원소 tag = `ReferenceWireType()`) |
| `StructRefArray<S>`(참조 struct) | Value | Struct | RecordList(11) |

- `FieldKind::ObjectReference ⇔ value가 ObjectReference이거나 (Array이고 element가 ObjectReference)`. `Array<S>`와 `StructRefArray<S>`는 wire가 같고 원소 struct의 `ContainsStrongReference`로만 구별한다.
- `WireType::ValueArray = 13`.
- 깊이: struct 원소 배열 멤버는 원소 깊이 + 1이다(RMC2303·Registry·생성 static_assert 공통).
- 원소 index는 배열 필드 프레임의 index 칸에 둔다(원소마다 새 프레임을 만들지 않는다). 프레임은 최대 17개라 `BuildPath`의 33 안이고 Record 스택(최대 16)도 `MaxRecordDepth` 17 안이다.

**Registry 컨테이너(B5a)**

`ObjectRefArray<T>`:

- 기존 `AddReference`·`RemoveReference`·`RemoveReferenceAt`·`ClearReferences`가 null을 받는다. null(`IsNull()` — registry 0 또는 generation 0)은 `ObjectHandle{}`로 정규화해 장벽·대상 검증 없이 넣는다(Add·Insert·SetAt·AssignReferences·SetElementReference 모두 — 비교·Required 검사·encode가 정규 null만 보게). `RemoveReference(null)`은 첫 null 원소를 지운다. Trace는 지금처럼 모든 원소를 `Visit`하고 방문자가 null을 무시한다(단일 `ObjectRef`와 같다). null 원소는 간선 관찰·패키지 간선 다중도(AS 6.1절)에 세지 않는다.
- `ObjectRegistry::InsertReferenceAt(access, owner, ObjectRefArray<T>&, index, target)`(index == size면 끝), `SetReferenceAt(…)`(할당 없음). 검사는 `AddReference`와 같다 — owner Alive·Borrow, 배열이 owner 저장소 안, index 범위 밖 InvalidArgument, non-null 대상은 Alive·`IsCutoff`가 아님(StaleHandle)·`IsNativeA`(TypeMismatch) 확인 뒤 shade.
- **성장 재진입 정정(현행 결함 포함)**: 지금 `AppendArrayEdge`는 잠금 밖 `Allocate`가 돌아온 뒤 그 시점의 size만큼 복사하고 재검증은 owner·대상만 본다 — allocator가 같은 배열에 재진입해 원소를 늘리면 새 버퍼를 넘쳐 쓴다. live API(Append·Insert)는 잠금 밖에서 **할당만** 하고, 재잠금 뒤 owner·대상을 다시 검증하며 **현재** size·index로 다시 판단한다 — 새 버퍼가 현재 size + 1을 담지 못하거나 index > size면 새 버퍼를 잠금 밖에서 반환하고 Busy(내용 불변), 그 사이 용량이 충분해졌으면 새 버퍼를 반환하고 기존 버퍼에 넣는다. 복사·삽입·교체·게시는 잠금 안에서 하고 옛 버퍼는 잠금 밖에서 반환한다. `processing_`(레지스트리 전체 플래그 — 다른 스레드의 Borrow 획득·`RootRef` 복제까지 Busy가 된다)은 live 성장에 세우지 않는다(흔한 런타임 목록 변경이 다른 스레드를 막지 않게). 실패하면 내용이 바뀌지 않는다.
- `ConstructionContext::AssignReferences(owner, ObjectRefArray<T>&, Span<const ObjectHandle>)`(staged 전체 교체): owner는 같은 트랜잭션의 Reserved(Borrow 불요), 배열은 owner 저장소 안, non-null 대상은 Alive이거나 같은 트랜잭션의 Reserved가 아니면 StaleHandle(`IsCutoff` 포함), `IsNativeA`가 아니면 TypeMismatch. 잠금 안에서 전부 검증 → 용량이 모자라면 `processing_`을 세우고 잠금 밖에서 count 버퍼를 할당(staged API는 생성 트랜잭션 안이라 `CreatePending`과 같은 규칙 — allocator 재진입은 Busy, Rollback 재진입은 Fatal) → 재잠금 뒤 Alive 대상만 shade(`ShadeLocked`는 Reserved에 StaleHandle을 내므로 Reserved 대상은 Commit이 shade한다) → 복사·버퍼 교체·size 게시 → 옛 버퍼는 `processing_` 아래 잠금 밖에서 반환. 실패하면 불변. Commit의 staged 간선 검증은 owner Trace로 원소를 방문한다. 컨테이너의 friend는 `ObjectRegistry` 하나로 두고 `ConstructionContext`의 staged API는 ObjectRegistry의 private 템플릿을 거친다.
- MaxElements는 Registry 변경 API에 걸지 않는다(5.5절).

`StructRefArray<S>`(`Object/StructRefArray.h`, `ObjectRegistry.h`가 include):

- 내부: `detail::StructElementOps{size, alignment, construct(void*, Allocator&), destroy}`(원소 타입별 정적 `StructElementOpsOf<S>`), `detail::StructRefArrayStorage{allocator, element, void** blocks, size, capacity}`, `detail::StructElementLocation{storage, index}`. trace는 `StructElementOps`에 넣지 않는다(owner 생성자에서 `GeneratedType<S>::Trace`가 인스턴스화되어 생성 헤더 없는 곳에서 컴파일이 깨진다). codec이 위치를 만들 수 있게 `detail::StructRefArrayAccess`가 storage에 닿는다.
- `class StructRefArray<S>`: `explicit StructRefArray(Allocator&) noexcept`, 복사·이동 금지, 소멸자가 역순 소멸·반환, `Size()`·`IsEmpty()`·`operator[]`(범위 밖 Fatal — 비참조 멤버는 owner Borrow 안에서 직접 쓴다), `Trace(TraceVisitor&)`(멤버 템플릿 — 생성 코드에서만 인스턴스화되며 원소마다 `GeneratedType<S>::Trace`).
- 저장 모델: **원소마다 블록 하나 + 포인터 색인**. `ObjectRef`는 이동할 수 없고(ObjectRefs.h) Registry가 참조 주소를 범위로 검사하므로 원소 주소를 고정한다. 삽입·삭제·성장은 색인만 옮기고, 원소 주소는 Remove·Clear·Assign·owner 소멸 때만 무효다. `Array`의 nothrow-move static_assert는 그대로다. UE는 연속 `TArray`지만 이 이유로 다르게 둔다(원소 수에 비례한 할당 수는 성능 시험으로 잰다).
- 원소 S 조건: 리플렉션 참조 struct이고 `ObjectRefArray`·`StructRefArray`를 (중첩까지) 담지 않는다(`ContainsReferenceArray` = false — MetaCompiler RMC2304, Registry InvalidArgument, 생성 static_assert). `Array<참조 없는 struct>`·String·Soft 멤버는 된다.
- API(`ObjectRegistry`): `AddElement(access, owner, StructRefArray<S>&) -> Result<usize>`, `InsertElementAt(…, index) -> Result<void>`, `RemoveElementAt(…, index)`, `ClearElements(…)`, `SetElementReference(access, owner, StructRefArray<S>&, index, ObjectRef<T>& field, target)`. staged(`ConstructionContext`): `AssignElements(owner, StructRefArray<S>&, count)`, `SetElementReference(owner, const detail::StructElementLocation&, ObjectRef<T>&, target)`과 (owner, 배열, index, …) 오버로드.
- Add·Insert: owner Alive·Borrow, 컨테이너가 owner 저장소 안. 잠금 밖에서 블록 할당·원소 기본 생성(참조는 null이라 장벽이 필요 없다)·필요하면 새 색인 할당 → 재잠금 뒤 owner를 다시 검증하고 **현재** size·index로 다시 판단해(새 색인이 모자라거나 index > size면 원소를 소멸·반환하고 Busy) 잠금 안에서 색인 복사·삽입·게시, 옛 색인은 잠금 밖에서 반환(ObjectRefArray live 성장과 같은 규칙 — `processing_`을 세우지 않는다). 실패하면 원소를 소멸·반환하고 불변. index > size는 InvalidArgument, size가 uint32 상한이면 Overflow.
- Remove·Clear: owner Alive 또는 DestroyRequested·Borrow, 잠금 안에서 색인에서 떼고 잠금 밖에서 소멸·반환(삭제 장벽은 필요 없다 — 삽입 장벽과 Sweeping cutoff가 지킨다). 떼어 낸 원소는 이미 컨테이너에 없으므로 소멸 중 재진입이 그 원소를 다시 건드릴 수 없다.
- `SetElementReference`: `SetReference`의 원소판이며 null을 받는다. 원소 모드 검사는 `CheckContainerOwnerLocked(owner, storage, storageBytes, transaction, allowDestroyRequested)`를 거친다 — live(transaction 0)는 Borrow 필수, staged는 같은 트랜잭션의 Reserved.
- `AssignElements`(staged): owner는 같은 트랜잭션의 Reserved, 전체를 count개의 기본 원소로 바꾼다. 블록 할당·S 생성·옛 원소 소멸은 `AssignReferences`처럼 `processing_` 아래 잠금 밖에서 한다. 하나라도 실패하면 전부 되돌리고, 옛 원소는 게시 뒤 소멸한다.
- SetEdge 일반화: `SetEdge(access, owner, ObjectHandle& field, target, transaction, native, const detail::StructElementLocation* element = nullptr)`. element가 없으면 지금처럼 owner 저장소 범위, 있으면 (1) 컨테이너 저장소가 owner 저장소 안 (2) index < size (3) `&field ∈ [blocks[index], blocks[index] + storage->element->size − sizeof(ObjectHandle)]`를 O(1)로 확인한다. `CheckArrayOwnerLocked`는 `CheckContainerOwnerLocked`로 일반화한다.
- 원소 참조 쓰기 단일 지점: `detail::SetStructMemberReference(ConstructionContext&, FieldDecodeContext&, owner, ObjectRef<T>&, ObjectHandle)` — 문맥에 원소 위치(`StructElement()`)가 있으면 `SetElementReference`, 없으면 `SetReference`.
- `detail::NativeStrongReference<StructRefArray<S>>` 특수화와 새 `detail::RegistryContainer<T>`(ObjectRefArray·StructRefArray면 참) 형질(NativeMemberTraits.h). 주석 정정: ObjectRefs.h의 컨테이너 주석, ObjectRefArray.h의 "null은 담지 않는다"와 변경 API 목록, ObjectRegistry.h의 배열 API "null 불가" 주석, RuntimeTypeOps.h의 `allowNull` 주석.

**encode 고정(B5a)**: String·Soft 경로·`Array<T>`·컨테이너 원소는 owner 밖 버퍼에 있고, encode 도중 allocator 콜백이 그 owner를 바꾸면 순회하던 버퍼가 해제된다(원소 String을 쓰다 writer가 커질 때 `RemoveElementAt`, `WriteReference`가 노드 목록을 키울 때 `AddReference`, B6 경로 쓰기의 `TryInsert`·String 대입). 1차 방어는 지금의 콜백 계약(저장 중 콜백은 Registry·입력을 바꾸지 않는다 — ObjectGraph.h)이고, 고정은 그 계약 위반을 메모리 오류 대신 Busy로 만드는 보조 장치다. `ObjectRegistry::PinForEncode(access, owner) -> Result<EncodePin>`(RAII, slot별 계수 — `ObjectStats`·진단에 보이고 남아 있으면 Shutdown이 Busy, 고정 중에는 회수하지 않는다)을 두고, 고정된 owner의 Registry 컨테이너·참조 변경 API, 등록된 필드 쓰기(생성 `WriteGeneratedField`·`FieldWriteOperation`의 값·String·Soft·참조 쓰기 — `ObjectRegistry::ResolveForWrite`로 쓰기용 Borrow를 얻는다)와 B6 경로 쓰기는 모두 Busy다(고정 Busy는 owner 밖 컨테이너 같은 호출 오류보다 뒤에 판정한다). **encode thunk를 부르는 쪽이 owner를 고정한다** — GraphSave는 노드의 필드를 encode하는 동안 노드마다 한 번 쥔다(필드마다 잠금을 잡지 않는다). 다른 encode 호출자(A0-O 복제 등)도 같은 계약이다.

**레지스트리(B5b)**

- `ValidateFieldList`: Array 허용. Array 필드는 `value.type` 0, `element.kind` ∈ 스칼라·Guid·Utf8String·ObjectReference·Enum·Struct·AssetId·SoftObjectReference·Digest256(Opaque·Array 거부), `element.type`은 Enum·Struct 필수·두 참조는 선택·그 밖 0, `maxElements ∈ [1, MaxArrayElements]`, `FieldKind::ObjectReference ⇔ element ObjectReference`, Required는 원소가 ObjectReference·SoftObjectReference일 때만. 위반은 InvalidArgument.
- 값 형식 해석을 대상 형식(배열이면 element, 아니면 value)으로 일반화한다: `ResolveValueTypes`·`fieldTypes`, `TypeLease::FieldEnum`·`FieldStruct`와 `StructView`의 같은 함수(배열이면 원소의 enum·struct), `valueNativeType`(Enum·Struct 원소의 native 키), `CarriesValueNativeType`은 필드를 받는다.
- 함수표: Array 필드는 read·write·writeStaged가 null이고 Transient가 아니면 encode·decode 필수. `FieldWriteOperation`의 Struct 분기를 Struct·Array로 넓힌다(문맥·대상 검사 뒤 Unsupported — 배열 통째 쓰기는 B6 뒤에도 Unsupported).
- `RegisterStruct` 계산 값: depth(원소 깊이 + 1), `containsStrongReference`(ObjectReference 원소 배열·강한 참조 원소 struct 배열 포함), `requiresWorldOnlyOwner`(원소 struct 전파), `containsRequired`(배열 멤버 제외 — 누락 struct 안 배열은 생성자 값이다), 새 `containsReferenceArray`(ObjectRefArray·StructRefArray 멤버이거나 그런 struct 멤버, `StructView::ContainsReferenceArray()`).
- 원소 struct 규칙: Array 필드의 원소 struct가 강한 참조를 담으면 그 struct의 `containsReferenceArray`는 거짓이어야 한다(InvalidArgument). WorldOnly 형질 원소 struct는 WorldOnly가 아닌 클래스에서 InvalidArgument. 컨테이너 구별(Array<S>와 StructRefArray<S>)은 정적 thunk와 생성 static_assert가 맡는다.
- `ValidateFieldWrite` 원소 모드: `FieldWriteTarget += bool element`(참이면 actual은 Array, binding을 `actual->element`와 대조, enum은 원소 enum). ObjectRefArray 원소도 null을 받으므로 null을 막는 곳이 없어진다 — `FieldWriteTarget::allowNull`과 6.4절 오류 순서의 "null 금지" 단계를 지운다. `detail::ValidateArrayLength(const FieldSchema&, usize newLength)`(Array가 아니면 TypeMismatch, 넘으면 CapacityExceeded). B5가 시험하고 B6이 쓴다.

**Serialization(B5a)**

`Serialization/ValueArray.h` — payload = u32 원소 tag + u32 count + 원소들:

- `ValueArrayShape{minElementBytes, exact}`(Digest는 40 exact), `ValueArrayElementShape(WireType)`(Bool 1·U32/F32 4·U64/I64/F64 8·Guid 16 고정, String·Bytes 8·SoftObjectReference 24 최소, 그 밖 InvalidArgument). 가변 wire에 exact를 주면 원소마다 길이 prefix가 정확히 그 크기를 만드는지까지 본다(Open이 남은 byte를 크기 간격으로 훑는다 — 할당 없음, 작업은 `Spend(count)`가 덮는다). 모순 조합(고정 폭 wire에 다른 폭, exact인데 wire 최소보다 작은 크기)은 reader·writer 공통으로 InvalidArgument다.
- `ValueArrayReader::Open(ArchiveReader&&, elementWire, maxCount, shape)`·`OpenField(const RecordReader&, const FieldView&, elementWire, maxCount, shape)`·`Count()`·`Elements()`·`Finish()`. 검사 순서(모두 할당 전): 저장 tag가 Record·RecordList·ValueArray·모르는 값이면 InvalidArgument → 기대 tag와 다르면 TypeMismatch → count > maxCount면 CapacityExceeded → count × max(wire 최소, schema 최소) > 남은 byte면 InvalidArgument(고정 폭·exact는 남은 byte가 정확히 count × 크기가 아니면 InvalidArgument) → `Spend(count)`. 원소는 `Elements()`의 `Read*`로 읽는다(원소마다 op 1 + byte). reader는 넘겨받은 `ArchiveReader`의 예산을 쓴다(1단계 공유, 2단계 `OpenValue`의 무제한) — 라우팅은 `OpenValue` 하나다. `ArchiveReader`의 `Spend`·`Charge`와 `RecordReader`의 `archive_`·`Contains`에 닿도록 friend를 더한다.
- `ValueArrayWriter::Begin(ArchiveWriter& out, elementWire, count, maxBytes, maxCount = UINT32_MAX, shape = {})`·`Check()`·`End()`: header에 최종 count를 쓰고, Begin은 기대 tag·shape가 잘못되면 InvalidArgument, count > maxCount(reader와 같은 값)이거나 8(header) + count × max(wire 최소, shape 최소) > maxBytes면 CapacityExceeded, 원소는 out에 바로 쓰며, End는 전체 ≤ maxBytes(CapacityExceeded)와 원소 byte ≥ count × 최소(고정 폭·exact는 정확히 같음, 아니면 InvalidArgument)를 확인한다. 가변 원소는 byte 합계의 하한만 잡는다. 원소 tag 인자는 `uint32`와 `WireType` 오버로드를 둔다.

`Serialization/RecordList.h`:

- `RecordListWriter::Begin(out, count, RecordListLimits)`(count > maxItems 또는 4(count) + count × 52 > maxPayloadBytes면 CapacityExceeded)·`BeginItem()`(**u64 길이 자리만** 쓴다 — 항목 Record는 호출자가 `RecordWriter::Begin`으로 쓴다)·`EndItem()`(out이 길이 자리보다 짧거나 항목이 Record envelope가 아니면 InvalidArgument, 항목 > maxItemBytes 또는 지금까지의 payload > maxPayloadBytes면 CapacityExceeded, 길이를 채운다)·`End()`(항목 수 == count, 전체 ≤ maxPayloadBytes). byte는 `RecordListBuilder::Finish` payload와 같다(시험). `ArchiveWriter::Patch`에 닿도록 friend를 더한다.
- `RecordListReader::OpenItem(index, maxFields)`, `ItemBytes(index)`(마이그레이션 입력). 기존 `OpenItem(index)` = `OpenItem(index, limits_.maxFields)`.
- **청구 정정**: 지금 `OpenField`는 항목을 `ReadRaw`로 다시 청구하고 `OpenItem`은 indexOnly 없이 연다 — 중첩 깊이만큼 작업이 불어난다. `OpenField`는 색인 전용(`OpenRecordField`처럼 항목마다 `Charge(1)`), `OpenItem`은 항상 indexOnly로 바꾼다. 단독 `Open`은 지금처럼 항목 byte를 한 번 청구한다. 작업 수는 같고 work만 줄어 기존 상대값 시험은 대부분 그대로이며, **깊이 비증폭을 확인하는 새 시험**을 완료 기준으로 둔다.

**문맥(B5b, `Object/FieldCodec.h`)**

- `ElementIndexScope`(`[[nodiscard]]`, 문맥의 friend): 만들 때 현재 프레임이 배열 필드이고 index가 `NoIndex`인지 확인하고(아니면 Fatal) 그 프레임의 non-const 포인터를 잡는다. `Set(index)`는 현재 프레임이 그 프레임인지만 확인하고(아니면 Fatal) 쓰며, 소멸 때 `NoIndex`로 되돌린다. `FieldCodecContext::current_`를 non-const `FieldCodecFrame*`로 바꾸고(`Current()`는 const를 돌려준다), `FieldCodecFrame::parent`가 const라 `FieldScope`는 이전 `current_`를 따로 보관해 되돌린다. 경로는 [배열[i], 멤버], 유효 플래그는 배열 필드의 것을 원소·멤버가 공유한다.
- 공통 `virtual Result<void> AddElements(usize) noexcept = 0` — 저장·1단계는 `maxTotalElements` 누계, 2단계 no-op. header 해석 직후·원소 읽기와 할당 전에 부른다. 시험 문맥(FieldCodecContextTests)도 새 순수 가상을 구현한다.
- 저장: 새 가상 없음. 배열 payload 상한은 `NestedRecordLimits().archive.maxTotalBytes`(= maxValueBytes) → `ValueArrayWriter`의 maxBytes, `RecordListLimits{MaxArrayElements, 그 값, 그 값 − 12}`.
- 복원: 비가상 `OpenValueArray(field, elementWire, maxCount, shape)`(wire 확인 → `OpenValue` → `ValueArrayReader::Open` — codec은 maxCount에 MetaSchema 상수 `MaxArrayElements`를 넘긴다), 가상 `OpenRecordList(field, itemSchema)`(항목 버전 범위 (1, UINT32_MAX)로 연다 — 버전 판정은 codec. 1단계: 맨 위 Record 또는 객체 Record의 `RecordListReader::OpenField` — 공유 예산 색인 전용, 맨 위가 없으면 2단계 단독 Open 몫인 payload byte를 선청구. 2단계: 스택이 비면 단독 Open(같은 크기 한도·무제한·maxFields = maxTotalFields), 있으면 맨 위의 `OpenField`), 가상 `OpenListItem(list, index)`(1단계 `OpenItem(index, 남은 maxTotalFields)` + `AddFields`, 2단계 `OpenItem(index, maxTotalFields)` — 오류는 `OpenRecord`와 같다), `StructElement()`(2단계 codec이 원소마다 RAII `StructElementScope`로 세운다, 겹치면 Fatal). 목록 한도는 두 그래프 문맥이 `OpenRecordList` 안에서 ObjectSerialization의 공용 함수 `graph_detail::ArrayLoadLimits(const ObjectGraphLimits&)` 하나로 얻는다(`{MaxArrayElements, maxValueBytes, maxValueBytes − 12}` — 저장 한도와 같다. codec은 ObjectRuntime에 있어 `ObjectGraphLimits`를 모른다).
- **관찰 임시 값의 allocator**: 관찰 대상 struct가 배열 멤버를 가지면 원소 버퍼가 `maxValueBytes`(BudgetAllocator의 단일 할당 한도 — wire 값 기준)를 넘을 수 있다(예: String 20만 개). 공통 가상 `ObservationScratch()`(바이트를 work로 청구하되 단일 할당 한도가 없다 — work 예산이 상한, 2단계는 쓰지 않는다)를 더하고, B4 `DecodeStructField`의 관찰 임시 값 할당·생성을 `Scratch()`에서 이것으로 바꾼다. 그래프 두 문맥과 시험 문맥이 구현한다.
- 누락: `CheckMissingField`는 배열 필드에서 Required 진단도 struct 후손 순회도 하지 않는다. `ReportMissingRequired`는 `value.kind == Struct`일 때만 내려가고, 그 required 판정은 `HasFlag(Required) && value.kind != Array`다(누락 struct 안의 Required 배열 멤버를 진단하지 않는다). 누락된 배열 필드는 생성자 값을 유지한다(단일 값과 같다).
- Required 배열의 뜻: 원소마다 non-null. `ObjectRefArray`·`Array<SoftObjectRef<T>>`의 null 원소는 저장에서 `rejectMissingRequired`면 진단 + InvalidArgument, 로드는 진단만 낸다(경로 [f[i]]). "비어 있지 않아야"라는 뜻은 없다.

**codec(B5b, `Object/GeneratedFieldCodec.h`)**

- 값 wire 읽기·쓰기를 `ValueType` 인자로 분리한다: `WriteValueWire(out, ctx, ValueType, const EnumSchema*, FieldValue, text, staticNative)`, `ReadValueWire(reader, ctx, ValueType, const EnumSchema*, const EnumValueSet*, staticNative, ParsedValue&)`(검증만 — 진단·청구 없음, `ParsedValue`는 할당 없는 view), `ReportParsedValue(ctx, ValueType, const ParsedValue&)`(미선언 enum·Required null·Soft 수집). `EncodeFieldWire`·`ParseFieldValue`는 이것을 감싸고 단일 값 동작은 바뀌지 않는다.
- 필드 단위 `FieldWireType(const FieldSchema&, referenceWire)`: Array는 원소가 Struct면 RecordList, 아니면 ValueArray. `EncodeStructRecord`의 wire 재확인·GraphSave·`ValidateLayout`이 쓴다.
- enum 원소 비용: 배열마다 한 번 `EnumValueSet::Build(EnumSchema, ctx)` — Flags는 mask O(v), 일반 enum은 16개 이하면 선형, 넘으면 Scratch 정렬 사본(v·log v 청구). 원소마다 O(log v)이고 `Spend(count × (BitWidth(v) + 1))`를 원소 읽기 전에 한다. 1단계는 원소마다 등록 enum과 정적 `IsDeclared`를 모두 확인한다.
- 컨테이너 형질: 부분 특수화(requires 탐지 없음) `ArrayField<Array<E>>`(원소 E), `ArrayField<ObjectRefArray<T>>`(원소 `ObjectRef<T>`), `ArrayField<StructRefArray<S>>`(원소 S). `FieldValueKind`는 셋 다 Array. `MatchesSchema<Member>`: value Array·원소 종류 같음·Enum·Struct면 `element.type == GeneratedType<E>::Id()`, struct 원소는 `MatchesStructView`도.
- 배열 codec(struct codec과 서로 부른다 — 템플릿 앞선 선언): `EncodeArrayField(const 컨테이너&, out, ctx)`, `DecodeArrayField(construction, owner, 컨테이너* target, field, ctx)`.
- **decode의 세 방식**(B4 struct와 같은 표, 6.4절의 "construction이 null이면 1단계"를 유지한다):

| 방식 | construction | target | 동작 |
|---|---|---|---|
| 검증 | null | null | 파싱·검증만(문자열은 view, 원소 할당 없음) |
| 관찰 | null | scratch 임시 값 | `ObservationScratch()` 원소로 채운다. 원소 struct의 관찰은 그 원소 주소에서 한다. 관찰 대상은 참조 없는 struct라 ObjectRefArray·StructRefArray는 오지 않는다 |
| 적용 | non-null | staged 객체 안 | 2단계 채움(아래) |

- encode 순서: `MatchesSchema` → Size ≤ maxElements(CapacityExceeded) → `AddElements(Size)` → writer Begin → 원소마다 `ElementIndexScope::Set(i)` 후 원소 → End. 원소마다 다시 검증한다: enum 선언 값, Soft null 규칙·경로, Required null(`rejectMissingRequired`), 참조 원소는 `WriteReference(out, h, {element.type, NativeTypeKey<T>()})`(null은 0). struct 원소는 `RecordListWriter::BeginItem` → `EncodeStructRecord(원소 view, &e, out, ctx)` → `EndItem`(SubstituteStruct·AddFields·제외는 B4 그대로, 치환 hook은 index 프레임에서). 가변 원소마다 `Check()`. 작업량은 out의 청구다.
- decode 순서(1·2단계가 같은 reader 호출 순서): wire 확인(TypeMismatch) → `MatchesSchema` → `OpenValueArray`(maxCount = `MaxArrayElements`)/`OpenRecordList` → `AddElements(count)` → 원소 → `Finish`(trailing InvalidArgument) → `ReserveApply` 한 번. 원소 진단은 원소 검증 직후 [f[i]]로 낸다(필드가 끝내 실패하면 그 호출의 진단은 무효다 — 6.4절).
- 단일 값 원소의 적용: `target->Clear()` → `TryReserve(count)` → `TryEmplaceBack`. String·Soft·struct 원소는 `target->GetAllocator()`로 만든 뒤 `TryAssign`한다. struct 원소 생성은 `MakeRuntimeStructOps`처럼 `__is_constructible(S, Allocator&)`로 생성자를 고른다. 로드는 생성자가 넣은 원소를 통째로 바꾼다.
- `ObjectRefArray`: 원소 tag는 `ReferenceWireType()`. 1단계는 원소마다 `ReadReference` — null은 token을 남기지 않고 Required면 [f[i]] 진단, non-null은 (token, 필드 기대 형식)을 남긴다. 2단계는 handle(null 포함)을 Scratch `Array<ObjectHandle>`(count 예약)에 모아 `construction->AssignReferences`를 한 번 부른다.
- struct 원소(`Array<S>`·`StructRefArray<S>`): 원소마다 `ElementIndexScope::Set(i)` 후 `DecodeStructValue(원소 view, construction, owner, value, StructRecordSource{&list, i}, ctx)` — B4 `DecodeStructField`를 출처(FieldView 또는 목록 항목)를 받는 `DecodeStructValue`로 일반화한다(`DecodeStructField`는 감싸기). `OpenRecord(…, 1, UINT32_MAX)`/`OpenListItem(list, i)`(목록은 항목 버전 (1, UINT32_MAX)로 열려 있다) → 버전이 같으면 그대로, 낮으면 `MigrateStruct(id, from, to, field.payload 또는 list.ItemBytes(i))` → 원본 scope를 닫고 `OpenMigratedRecord`, 높으면 newerVersion 정책(`NewerVersion` path [f[i]]) → `DecodeStructRecord`. 1단계에서 원소 struct가 관찰 대상이면 원소마다 scratch에 한 번의 해석(RAII destroy) + `ObserveStruct`(index = i). 마이그레이션 보관과 2단계 커서는 원소마다 방문 순서다. `Array<S>` 적용: `Clear` → `TryReserve(count)` → 원소를 `TryEmplaceBack(allocator)`한 직후 그 원소 주소에 적용(예약했으므로 주소가 고정된다, Registry를 거치지 않는다). `StructRefArray<S>` 적용: `construction->AssignElements(owner, *target, count)` 뒤 원소마다 `StructElementScope` 안에서 `&(*target)[i]`에 적용(참조 멤버는 `SetStructMemberReference`가 `SetElementReference`로 보낸다).
- **2단계 몫의 선청구(`ReserveApply`)**: 원소 메모리 `count × sizeof(E)`(StructRefArray는 원소 블록 + 색인 포인터, ObjectRefArray는 Scratch 사본 `count × sizeof(ObjectHandle)` 포함 — BudgetAllocator처럼 바이트를 work로 센다) + 원소 수 + 1 + 2 × Σ(문자열·경로 byte), ObjectRefArray는 여기에 참조 수를 더한다. 큰 NATIVE 멤버를 가진 원소 struct가 작은 wire로 큰 할당을 끌어내지 못하게 한다.
- 클래스 필드 thunk: `if constexpr (ArrayField<Member>::Supported)` 분기 — encode는 owner Borrow(호출자가 쥔 encode 고정 아래) → `EncodeArrayField(owner->*member)` → 수명 재확인, decode 1단계는 `DecodeArrayField(nullptr, handle, nullptr, …)`, 2단계는 `construction->Get<Owner>(handle)`의 멤버 주소. struct 멤버 thunk도 같은 분기(검증은 target null, 관찰·적용은 `&(target->*Member)`). `StructFieldOps`의 `valueNativeType`은 Enum·Struct 원소 배열이면 원소 native 키다. ObjectRefArray·StructRefArray 멤버는 인라인 struct(owner 저장소 안)에서만 둘 수 있다.
- **2단계 불변식**: 1단계가 wire·header·count·trailing, 원소 형식·폭·UTF-8·digest·Soft, enum(등록 + 정적), 참조 token 범위·도달성·기대 타입(스키마와 `staticNative` — `AssignReferences`가 검사하는 키와 같다), struct 원소 버전·마이그레이션·관찰을 모두 검증하므로 2단계 실패는 할당과 Registry 상태뿐이다.

**그래프(B5b)**

- `ObjectGraphLimits.maxTotalElements = 4,194,304`, `Budget`에 원소 누계와 `AddElements`(`AddFields`와 같은 모양).
- 두 문맥의 `OpenRecordList`·`OpenListItem`은 `OpenRecord`와 같은 구조다. 1단계 최상위 목록은 `Charge(field.payload.Size())`(2단계 단독 Open 몫), 항목은 색인 전용 — `DecodeNode`의 작업 Δ 2배가 덮는다.
- 기대 형식 보관: 지금은 "직전 값과 같으면 재사용"이라 서로 다른 참조 멤버를 가진 struct 배열에서 2n개로 불어난다. `BeginField`가 필드의 기대 시작 위치를 기록하고 [시작, 끝)을 선형으로 찾아 재사용한다(비교 수를 청구한다). 한 필드 안 서로 다른 기대의 수는 스키마가 정한다.
- 도달성 (기대, 실제) 캐시와 기대 타입 lease 목록을 선형 탐색에서 `HashMap`으로 바꾸고 조회마다 `Spend(1)`.
- 저장 참조 비용: `AddHandle`의 노드 선형 탐색과 `WriteReferenceWire`의 참조마다 `AcquireType`·`AcquireRegisteredType`을 `GraphEncodeContext`의 handle → local ID `HashMap`(키는 domain을 포함한 handle 전체 — 캐시 적중이 domain 검사를 건너뛰지 않게), 노드별 `TypeLease`(발견 때 한 번 열고 `SaveNodeFields`가 재사용), 기대 TypeId → lease 캐시로 바꾼다. 조회·삽입마다 `Spend(1)`. BFS 순서와 byte는 그대로다.
- `ValidateLayout`: 필드 단위 `FieldWireType`, Array의 `value.type` 0, 원소 Enum은 `FieldEnum` non-null, 원소 Struct는 `FieldStruct`가 비어 있지 않음, FieldKind 규칙. GraphSave의 wire 재확인도 필드 단위다.
- token은 non-null 원소마다 하나이고 `maxTotalElements`가 상한이다. 1단계 끝 선청구(노드 + 1 + staged 간선)는 원소 참조를 포함한다.

**Trace**

- 클래스와 참조 struct의 생성 Trace는 **모든 배열 필드·멤버**에 `detail::TraceArrayField(typed.*(&::Owner::f), visitor)`를 부른다. 오버로드 집합이 `Array<E>`는 아무것도 하지 않고 ObjectRefArray·StructRefArray는 `.Trace`를 부른다 — 실제 C++ 타입이 호출을 고르므로 descriptor에서 컨테이너를 추론하다 틀려도 Trace가 빠지지 않는다(`if constexpr`·requires 탐지 아님). `RuntimeStructOps.trace`는 이 멤버를 포함한 struct Trace를 감싼다. 생성 타입의 `parallelTraceSafe`는 그대로다.

**MetaCompiler(B5c)**

- 해석: desugar 철자에서 `rocks::Array<…>`·`rocks::ObjectRefArray<…>`·`rocks::StructRefArray<…>`를 꺾쇠 깊이로 나누고, 원소는 단일 필드와 같은 해석(`ResolveValue` → (kind, reference, id))을 쓴다. `ObjectRefArray<>`(기본 인자 생략 출력)는 `rocks::Object`다. cv 원소(`Array<const T>`)는 RMC2000. `FieldModel += ElementKind·MaxElements`, 배열 Kind는 "Array". 배열의 `value.type`은 항상 0이므로 **원소 대상(enum·struct·참조 대상)은 기존 `Reference`·`ReferenceId`에 담는다**(`ElementReference*`를 따로 두지 않는다 — `Find(field.Reference!)`를 쓰는 지점이 배열에서 null 역참조나 조용한 건너뜀이 되지 않게). 스키마·descriptor를 쓸 때 배열이면 그 대상을 `element.type`으로, `value.type`은 0으로 낸다.
- `Container`는 저장하지 않는 파생 속성이다: 원소가 ObjectReference면 ObjectRefArray, 원소가 Struct이고 그 struct가 `ContainsStrongReference`면 StructRefArray, 그 밖은 Array. 이 모듈 필드는 C++ 철자의 컨테이너가 이 값과 다르면 RMC2304다(import 필드는 이 규칙으로 복원한다).
- **판정 지점 일괄 정정**: 기존 `IsReference`·`IsStruct`·`HasTargetType`·`KindRequiresTargetId`·`AcceptsRequired`는 배열 필드에서 거짓이 되므로, `FieldModel`에 파생 속성 `TargetKind`(배열이면 원소 종류)·`StructTarget`(Struct이거나 원소가 Struct)·`IsObjectReferenceFieldKind`(ObjectReference이거나 원소가 ObjectReference — StructRefArray는 거짓)를 두고 쓰는 곳을 한꺼번에 바꾼다 — ModuleDescriptor(fieldKind·type 쓰기·읽기), CppEmitter(valueHeaders·스키마·클래스 Ops·클래스 Trace·struct 생성), SchemaValidator(깊이·순위·형질·RMC2206·RMC2006, `LinkImported`의 import 필드 대상 연결), CompilerDriver(같은 모듈 private 원소의 RMC2006, 첫 Clang 실행 실패 분기 — RMC2304 변환). 빠지면 컴파일은 되면서 GC 구멍이 되는 지점(클래스 Trace)이 있으므로 시험으로 덮는다.
- RMC2304: `Array<T>`의 T가 배열, `ObjectRefArray`·`StructRefArray`의 인자가 배열(RMC2003보다 먼저 판정), `StructRefArray<S>`의 S가 리플렉션 struct가 아니거나 참조 없는 struct(→ `Array<S>`)이거나 Registry 컨테이너를 (중첩까지) 담음. `Array<ObjectRef<T>>`·`Array<참조 struct>`·`Array<ObjectRefArray를 담은 struct>`는 멤버 선언에서 `Array`가 인스턴스화되며 nothrow-move static_assert로 Clang이 먼저 실패한다(오류의 주 위치는 `Array.h`이고 멤버 선언은 `requested here` note에만 나온다). CompilerDriver의 첫 Clang 실행이 실패하고 stderr에 `rocks::Array<` 인스턴스화의 static_assert 실패가 있으면, 딸린 `requested here` note 중 이 모듈 입력 헤더 안의 첫 위치를 RMC2304 위치로 쓴다(clang `file:line:col`과 clang-cl `file(line,col)` 형식 모두 해석, 지역화 문구에 기대지 않는다). 실패해도 stdout의 AST가 나오므로 그 위치의 PROPERTY 필드를 찾으면 필드 이름과 원소 철자를 메시지에 넣고, NATIVE 멤버면 RMC2205, 찾지 못하면 RMC3000을 유지한다. 안내("참조는 ObjectRefArray, 참조 struct는 StructRefArray")는 RMC2304 힌트에 두고 Core의 `Array.h` 메시지는 중립으로 둔다(Core가 ObjectRuntime 타입 이름을 알지 않게). 사용자가 noexcept 이동을 직접 정의해 Clang 단계를 지나는 `Array<참조 struct>`는 모델 단계에서 "`Array<S>`의 S가 `ContainsStrongReference`면 RMC2304"로 막는다. 지원하지 않는 원소 형식은 RMC2000(안내에 원소 철자), `ObjectRefArray<T>`의 T가 클래스가 아니면 RMC2003.
- 지정자: `MaxElements`는 배열 전용(그 밖 RMC2202 — 종류를 먼저 판정한다), 그 뒤 범위를 본다 — 0과 2³¹ 이상은 이미 지정자 문법이 RMC2000으로 막고 1,048,576 초과는 validator가 RMC2000으로 막는다. 없으면 65,536. Required는 원소가 ObjectRef·SoftObjectRef인 배열만(그 밖 RMC2202).
- 순환·깊이·순위: `ComputeDepths`·`ComputeRanks`·import DFS의 간선에 struct 원소 배열을 넣는다. 배열을 거친 자기 참조는 RMC2302.
- 형질: `ContainsStrongReference`에 ObjectRefArray·StructRefArray 필드, `RequiresWorldOnlyOwner` 원소 struct 전파, 새 `ContainsReferenceArray`(생성 `static constexpr`. descriptor struct 항목의 `flags` 배열에 값 `ContainsReferenceArray`를 더하고(`KnownFlag`, 쓰기 순서 ContainsStrongReference → RequiresWorldOnlyOwner → ContainsReferenceArray) 필드·멤버 flag와 대조하며, "배열 필드의 원소 struct가 참조 struct면(= StructRefArray) 그 struct의 ContainsReferenceArray는 거짓" 규칙을 RMC1000으로 — import 순회 순서에 의존하지 않는다). RMC2206은 원소 struct까지, RMC2205의 `StrongReferenceTemplates`에 `rocks::StructRefArray`, RMC2006은 private 원소 enum·struct까지.
- 생성: 스키마 `{id, FieldKind::X, flags, name, {}, {ValueKind::Array, Guid{}}, category, displayName, toolName, {ValueKind::E, elemId}, maxElements}`. 클래스 Ops는 Array 필드에 `nullptr, {}, nullptr,` + codec, 원소 Enum·Struct면 `NativeTypeKey<::E>()`. static_assert(전체 특수화 안): `Array<S>` → `!GeneratedType<S>::ContainsStrongReference`, `StructRefArray<S>` → `ContainsStrongReference && !ContainsReferenceArray`, struct 원소 struct의 `Depth < Depth`·형질 포함, WorldOnly가 아닌 클래스의 원소 struct `!RequiresWorldOnlyOwner`, struct 특수화의 PROPERTY 멤버마다 `ContainsStrongReference || !::rocks::IsNativeStrongReference<decltype(S::m)>`와 `ContainsReferenceArray || !::rocks::detail::RegistryContainer<decltype(S::m)>::value`(ObjectRefArray·StructRefArray면 참인 새 형질), struct 멤버마다 `ContainsReferenceArray || !GeneratedType<M>::ContainsReferenceArray` — 모델 형질을 C++ 타입과 직접 대조한다(encode 고정이 `ContainsReferenceArray`에 기대므로 틀린 모델 값이 고정 누락이 되지 않게). include: struct 특수화를 내는 생성 헤더는 `<Object/NativeMemberTraits.h>`를, valueHeaders에는 원소 enum·struct 생성 헤더(상속 import 필드의 원소까지)를 넣는다.
- descriptor: 쓰기 `"value": {"kind": "Array"}`, `"element": {"kind", "type"}`(type은 대상 종류일 때만), `"maxElements"`(배열이 아니면 0). schema 1 유지, 컨테이너 키는 두지 않는다(원소 종류와 원소 struct 형질로 정해진다). 엄격 읽기: 배열이 아니면 `element`는 null·`maxElements`는 0, element는 Array 필드에만·지원 종류(Array 제외)·종류별 키가 정확히 맞음, `element.type`은 Enum·Struct면 0이 아닌 hex·참조면 ""(Object) 허용, maxElements 1..1,048,576, fieldKind는 원소 ObjectReference와·Required는 참조 원소와 대응(아니면 RMC1000). `IsSupportedValueKind`에 Array. `LinkImported`는 원소 대상 연결·종류 대조(RMC1000)·private(RMC2006)를 하고, import `FieldModel`은 MaxElements와 원소 정보를 보존한다(파생 클래스 스키마가 상속 maxElements를 그대로 내야 Registry 상속 대조를 통과한다).
- 모듈 등록 순서: 원소 struct가 순위 간선이다.
- 명시적 인스턴스화·`extern template`이 있으면 Clang이 작성 철자를 출력할 수 있다 — `rocks::Array`·`ObjectRefArray`·`StructRefArray`의 명시적 인스턴스화·`extern template` 선언을 Policy 검사(소스 정규식, `Policy.RepositoryRuntime`)로 금지한다(원소 미러 조회 실패는 RMC2000 안내).

**B6 호환**

- `PathStep{fieldId, index, indexed}`는 그대로이고 원소 index는 배열 필드 단계에 둔다([slots[2], material]). B6이 쓰는 것: Registry 컨테이너 API, `ValidateFieldWrite` 원소 모드·`ValidateArrayLength`, 원소 struct 멤버 단위 쓰기(참조 struct 단위 원자성 없음), `Array<T>` 삽입은 원소와 새 버퍼를 먼저 만든 뒤 무할당으로 옮겨 게시(7.1절). B6의 `ops.path` thunk는 `RuntimeFieldOps`·`RuntimeStructFieldOps` 끝에 더한다. 배열 원소 접근 템플릿은 생성 헤더 안에서만 인스턴스화한다. 원소 안정 ID는 열린 문제다(B5 wire는 순서 기반).

**시험**

- Serialization: ValueArray wire별 왕복·byte 배치·빈 배열·count > max·할당 전 최소 크기(계수 allocator 할당 0)·exact 불일치·tag 오류·잘림·trailing·UTF-8·bool·`OpenField` 공유 예산. `RecordListWriter` byte = `RecordListBuilder`, count·항목 크기 위반. 색인 전용 청구(목록 안 struct 안 목록의 작업이 깊이 배수가 아님), `OpenItem(maxFields)`, `ItemBytes`.
- ObjectRuntime: Insert·SetAt(범위·null·타입·`IsCutoff`·성장 실패 불변·Marking shade), **성장 중 allocator 재진입(같은 배열 Add·Remove)을 현재 상태로 다시 판단 — 새 버퍼가 모자라면 Busy·내용 불변, 넘침 없음, 현행 Append 포함**, live 성장 중 다른 스레드의 Borrow 획득이 Busy가 되지 않음, AssignReferences(Alive owner StaleHandle·다른 트랜잭션·null 원소·타입·같은 트랜잭션 Reserved 대상 허용·성장 실패 불변·재진입 Busy·slice 사이 Marking에서 Alive shade 후 Commit·Commit 원소 방문·rollback 버퍼 반환), StructRefArray(Add·Insert·Remove·Clear·주소 고정·범위 밖 field·index InvalidArgument·DestroyRequested 정리·원소 참조 GC 생존·제거 후 회수·생성자·allocator 재진입을 현재 상태로 다시 판단·모든 할당 실패 불변·owner 소멸 반환·live SetElementReference Borrow 필수·AssignElements 실패 전부 되돌림), 비정규 null(`generation 0`) 정규화, encode 고정 중 컨테이너 변경·경로 쓰기 Busy, 원소 1만 개 StructRefArray 로드의 할당 수(성능).
- 레지스트리: 배열 필드 규칙 전부, 원소 enum·struct 의존(Busy)·NotFound·TypeMismatch, `FieldEnum`·`FieldStruct` 원소, 배열 경유 깊이 17, 형질 계산(`containsReferenceArray` 포함), StructRefArray 원소 컨테이너 거부, WorldOnly 원소, 접근기를 가진 배열 거부, `FieldWriteOperation` Unsupported(오류 순서), 원소 모드·`ValidateArrayLength`, 상속 maxElements 불일치.
- codec·그래프: 모든 원소 종류 왕복·재저장 byte 동일(Binds Vec3·Flags enum·null/non-null Soft·Digest·String·uint8·null 원소 섞인 ObjectRefArray), struct 안 배열·배열 원소 struct 안 배열·인라인 struct 안 ObjectRefArray·StructRefArray, 순환 참조 배열, Required ObjectRefArray null 원소(저장 거부·로드 진단 [f[i]]), 대상 타입 불일치 TypeMismatch·도달성, 진단 경로 [f[i]]·[f[i], m], 누락 struct 안 Required 배열 멤버 무진단, struct 원소 마이그레이션(원소당 1회·보관본 재사용·커서 어긋남 오류), 관찰 struct 안 배열 멤버가 채워져 관찰됨(maxValueBytes보다 큰 원소 버퍼 포함)·치환 hook index·치환 수명, MaxElements(저장 거부·**로드는 허용**)·`MaxArrayElements`·`maxTotalElements`·maxValueBytes(U64 약 52만 개)·struct 원소 maxTotalFields, 큰 NATIVE 멤버 원소 struct의 선청구 한도 실패가 생성자 전, 모든 한도 스윕이 생성자 전 실패, 1단계 사용량과 같은 예산은 성공·1 작으면 생성자 전 실패, 모든 할당 실패 스윕, Marking 중 staged 배열 로드, 기대 보관 수 = 참조 멤버 종류 수, 저장 참조 비용 선형(65,536 원소 → 1,000 노드가 기본 예산 안), 배열 경유 깊이 16 작업이 깊이 배수 아님.
- MetaCompiler: 컨테이너·원소별 해석·생성 컴파일·등록, `ObjectRefArray<>` 기본 인자, cv 원소 RMC2000, MaxElements 기본·명시·범위(RMC2000)·비배열(RMC2202), Required(RMC2202), RMC2304 각 사례(Clang static_assert 경로의 변환 — 위치가 멤버 선언·clang과 clang-cl 형식·NATIVE 멤버는 RMC2205, noexcept 이동을 직접 정의한 `Array<참조 struct>`의 모델 단계 거부, 철자 컨테이너와 파생 `Container` 불일치), RMC2003(`ObjectRefArray<Struct>`), RMC2302·2303 배열 경유, RMC2206·2205·2006, descriptor 왕복·조작(RMC1000 — 원소 키·type·maxElements·ContainsReferenceArray flag), 원소 struct 헤더 include, Binds 원소, static_assert probe(형질과 C++ 타입 대조 — `ContainsStrongReference`·`ContainsReferenceArray`), Policy의 컨테이너 명시적 인스턴스화 거부, 클래스 Trace가 import base의 배열 필드를 방문, **세 모듈 구성**(원소 struct 모듈 → base 모듈의 `StructRefArray<SA>`·`Array<EA>` → 파생 모듈이 컴파일·등록), import struct 멤버가 다른 모듈 struct의 배열.
- SDK: reflection 게임에 `Array<int32>`·`ObjectRefArray`·`Array<rocks::Vec3>`·`StructRefArray` 필드를 두고 SDK 빌드·저장·로드.

## 6. 필드 codec(B3–B5 공통)

### 6.1 생성 codec thunk와 문맥

`RuntimeFieldOps`에 두 thunk를 더하고, GraphSave·GraphLoad는 모든 필드를 이 thunk로 인코딩·디코딩한다(지금의 `FieldValue` switch를 대신한다 — 단일 값 종류도 같은 경로).

- `encode`: 필드 값을 wire tag와 payload로 만든다(정확한 서명은 6.4절). C++에서 직접 쓴 값은 thunk 검증을 거치지 않았으므로 **encode가 다시 검증한다**(enum 선언 값, SoftObjectRef null 규칙·경로 상한, MaxElements, `rejectMissingRequired`가 켜졌으면 Required null).
- `decode`: 두 단계로 부른다(6.2절, 정확한 서명은 6.4절).
- 문맥(`FieldEncodeContext`·`FieldDecodeContext`)이 제공하는 것 — A0-O·A1이 생성 코드를 다시 고치지 않도록 B3에서 고정한다:
  - **참조 기록·해석기**: `WriteReference(ArchiveWriter&, ObjectHandle, expectation)`·`ReadReference(ArchiveReader&, expectation) -> DecodedReference`(null 여부와 2단계 handle — 6.4절). 참조 wire 형식이 문맥 매개변수다(graph v1은 U64 local ID, graph v2는 AS 5.1절의 u32 + import 비트). struct·배열 안의 참조도 같은 경로이며 기대 타입을 함께 넘긴다.
  - **Soft 참조 수집기**: (AssetId, 객체 경로, 기대 타입, 유효 플래그) — A1 Dependencies(Soft·EditorOnly, `expectedTypeId`·`required`)와 읽기 전용 열기의 부분집합 대조가 struct·배열 안까지 모은다.
  - **유효 플래그**: 바깥 필드의 EditorOnly·Transient·ReadOnly가 중첩 struct·배열 원소로 전파된 값(`PropagateFieldFlags`, 6.4절). graph v2의 EditorOnly 제외와 B6의 읽기 전용 판정이 이것을 쓴다. Required는 전파하지 않는다 — 그 필드 자신의 규칙이며 배열 원소의 null은 그 배열 필드의 Required(원소마다 non-null, 5.6절)로 다룬다.
  - **struct 관찰·치환 hook**: encode 직전(struct ID별 값 치환 — bulkId 재매핑, A0-O가 채운다)과 decode 뒤(struct ID별 관찰 — BulkDataRef 전수 대조, A1).
  - **진단 싱크**: `UnknownField`·`RequiredMissing`·`UndeclaredEnumValue`·`NewerVersion`(필드 경로와 함께).
  - **decode 정책**: enum 미선언 값(Reject·ReportAndDefault), 높은 struct 버전(Reject·LoadNewerSkippingUnknown). 기본은 모두 거부.
- 값 해석은 Record 트리 예산을 공유한다: 필드 payload는 `RecordReader::OpenValue`, 중첩 struct는 새 `RecordReader::OpenRecordField`(OpenValue와 같은 범위 검사·예산 공유), 값 배열은 `OpenValue`로 연 payload를 `ValueArrayReader::Open`에, struct 원소 목록은 `RecordListReader::OpenField`로 연다(1단계는 필드마다 `ArchiveReader::Create`로 새 예산을 열지 않는다 — 2단계는 6.2절의 별도 예산).

### 6.2 두 단계 decode

- **1단계(검증)**: construction = nullptr. 파싱·형식·한도·enum·버전을 검증하고 참조 token을 기대 타입과 함께 수집기로 보고한다(도달성·범위·타입 검증용 — 지금 GraphLoad가 최상위 필드의 `value.type`만 보는 것을 중첩까지 넓힌다). 1단계 산출물 — 노드의 RecordReader 색인(FieldView 목록)과 객체·struct 마이그레이션 결과 Record — 을 `LoadNode`에 보관한다. **2단계의 작업량도 1단계에서 미리 청구한다**(같은 양).
- **2단계(적용)**: 모든 객체를 만든 뒤 staged 객체에 쓴다. 보관한 색인·마이그레이션 결과를 다시 쓰므로 record를 다시 열지 않고(필드 누계 이중 청구 없음) 마이그레이션도 다시 실행하지 않는다. 2단계의 값 읽기(`OpenValue`·중첩 Record·배열 reader)는 보관한 색인에서 **1단계와 같은 크기 한도, 무제한 operation·work의 별도 예산**으로 연다(같은 양을 1단계가 실제 예산에 미리 청구했으므로 이중 청구가 아니다). 그래서 2단계는 예산으로 실패하지 않는다 — "잘못된 파일은 생성자를 실행하지 않는다" 불변식이 유지된다(2단계 실패는 할당 실패뿐이며 그래프 로드 전체 rollback이다).
- 참조 대상 타입 확인(`Acquire`)은 (기대 타입, 실제 타입) 쌍으로 캐시한다(간선마다 lease를 다시 열지 않는다).

### 6.3 A0-O와의 경계

- A0-B는 graph envelope(format 1)를 그대로 두고 필드 인코딩·문맥·한도(원소 누계)만 넓힌다. 저장 집합·외부 참조·export 기준 노드 타입·EditorOnly 제외·값 치환 hook 구현·Commit 다중도 보고·바이트·작업량 한도 확장은 graph v2(A0-O)다.
- A0-O가 A0-B에서 받는 것: 문맥의 참조 wire 매개변수, Soft 수집기·유효 플래그·struct hook·진단·decode 정책 자리, 두 단계 decode의 산출물 보관 구조.

### 6.4 B3c 구현 API(확정안)

6.1–6.3절을 단일 값 종류(스칼라·String·Guid·AssetId·Digest256·SoftObjectReference·Enum·ObjectReference)에 대해 구현하는 구체 형태다. 설계 교차검증(2026-11-13, API 적합성·정확성 두 관점)의 지적을 반영했다. struct·배열 codec은 B4·B5가 같은 문맥에 더하며 thunk 서명은 바뀌지 않는다.

**모듈과 헤더**

- ObjectRuntime이 Serialization에 의존한다(Serialization은 Core에만 의존하므로 순환이 없다 — UE 5.6의 FArchive(Core)와 `FProperty::SerializeItem`(CoreUObject) 관계와 같다). `RuntimeTypeOps.h`는 `ArchiveWriter`·`FieldView`·두 문맥을 전방 선언만 하고, Serialization 헤더는 `Object/FieldCodec.h`(문맥·정책·hook)와 `Object/GeneratedFieldCodec.h`(생성 codec 템플릿)만 include한다. 이 둘은 생성 헤더와 직렬화기만 include하므로 Engine·Sparks VM·Inspector 번역 단위에 Archive가 퍼지지 않는다.

**위치(프레임)와 플래그 전파**

- `FieldCodecFrame { const FieldSchema* field; TypeId declaringType; uint32 effectiveFlags; const EnumSchema* enumeration; uint32 index; const FieldCodecFrame* parent; }` — 지금 처리하는 필드(struct 멤버면 멤버), 그 필드를 선언한 클래스·struct(최상위 필드는 객체 타입), 전파된 플래그, Enum 필드의 등록된 enum 스키마, 배열 원소 index(아니면 `NoIndex`), 바깥 프레임. `FieldScope`는 `[[nodiscard]]`이고 역순으로 끝나지 않으면 Fatal이다. 문맥 기반 클래스가 프레임 스택을 갖고, 직렬화기와 B4·B5 codec은 RAII `FieldScope`로 들어가고 나온다. 기반 클래스는 `Current()`, `ObjectType()`, `ObjectIndex()`(그래프 local ID − 1 = v2 export index)와 줄임 `Field()`·`EffectiveFlags()`를 준다.
- 전파 규칙은 MetaSchema의 constexpr 하나로 고정한다: `PropagateFieldFlags(parent, own) = (parent & (EditorOnly | Transient | ReadOnly)) | own`. B6의 ReadOnly 전파도 이것을 쓴다. 최상위 필드의 유효 플래그는 필드 플래그다.
- 경로 표현은 7절의 `PathStep { Guid fieldId; uint32 index; bool indexed; }`(MetaSchema)이며, 진단 경로는 프레임 사슬로 고정 크기 버퍼(struct 깊이 16 + 배열 index — 최대 33 step)에 만들어 할당하지 않는다.

**문맥·정책·hook(`Object/FieldCodec.h`)**

- `FieldEncodePolicy { bool rejectMissingRequired = false; uint32 excludedFlags = 0; }` — 패키지 저장·쿠킹이 Required null 거부를 켜고, 쿠킹은 `excludedFlags = EditorOnly`를 준다(복제의 복사 정책도 이것으로 표현한다).
- `FieldDecodePolicy { UndeclaredEnumPolicy undeclaredEnum = Reject; NewerVersionPolicy newerVersion = Reject; UnknownFieldPolicy unknownField = SkipAndReport; bool allowMissingFields = false; uint32 allowMissingFlags = 0; }` — `ReportAndDefault`, `LoadNewerSkippingUnknown`, `Reject`(쿠킹 데이터는 정확히 일치해야 한다). `allowMissingFields`는 `ObjectGraphLoadOptions`에서 이리로 옮기고, `allowMissingFlags`(쿠킹 로드 = EditorOnly)는 그 플래그의 누락을 허용한다.
- `FieldDiagnostic { FieldDiagnosticKind kind; TypeId objectType; uint32 objectIndex; Span<const PathStep> path; Guid field; TypeId valueType; int64 value; }` — 종류는 `UnknownField`(field = 모르는 ID)·`RequiredMissing`·`UndeclaredEnumValue`(value = 저장 값, valueType = enum ID)·`NewerVersion`(value = 저장 버전). path는 호출 동안만 유효하다.
- `SoftReferenceInfo { AssetId asset; StringView objectPath; TypeId expectedType; uint32 effectiveFlags; uint32 objectIndex; Span<const PathStep> path; }` — view는 호출 동안만 유효하다. null은 보고하지 않는다.
- `FieldCodecHooks`(기본 no-op 가상 함수 묶음): `Report(const FieldDiagnostic&) noexcept -> Result<void>`, `CollectSoftReference(const SoftReferenceInfo&) noexcept -> Result<void>`. hook이 실패하면 그 저장·복원이 실패한다(진단 유실로 읽기 전용 판정을 놓치지 않게). B4가 struct 관찰·치환을, A0-O가 외부 참조 보고를 같은 묶음에 더한다 — 옵션 구조체는 다시 바뀌지 않는다. hook은 소유 객체를 Borrow한 상태에서 불리므로 Registry·입력을 바꾸지 않는다(`ObjectGraph.h`의 콜백 계약). 저장·복원이 실패하면 그 호출에서 받은 진단·수집은 무효이고 호출자가 버린다. `ReportAndDefault`·`LoadNewerSkippingUnknown`을 고르고 hook이 없으면 InvalidArgument다(조용한 기본값 대체 금지).
- `FieldEncodeContext`·`FieldDecodeContext`(가상 클래스, 프레임 기반 공유): `Policy()`, `ReferenceWireType()`(graph v1 = U64, v2 = U32 — 참조 필드의 encode는 이 값을 돌려주고 decode는 `field.wireType`을 이것과 비교한다. B5의 ObjectRefArray 원소 tag와 원소 최소 크기도 이것에서 얻는다), `IsExcluded(effectiveFlags)`(Transient는 항상), `Report(kind, value, valueType)`(현재 프레임으로 진단을 만든다), `CollectSoftReference(asset, path, expectedType)`(플래그·위치는 문맥이 채운다), `Spend(work)`·`ReserveApply(work)`(아래 예산).
  - encode 전용: `WriteReference(ArchiveWriter&, ObjectHandle, const ReferenceExpectation&) -> Result<void>` — `ReferenceExpectation { TypeId schemaType; const void* staticNative; }`(스키마의 기대 타입과 정적 thunk의 `NativeTypeKey<T>`). 대상이 둘 다를 상속 규칙(`IsNativeA`)으로 만족하는지 검사하고(TypeMismatch) wire를 쓴다. null이고 현재 프레임이 Required이며 `rejectMissingRequired`면 `RequiredMissing`을 보고하고 InvalidArgument다.
  - decode 전용: `IsMissingAllowed(effectiveFlags)`, `OpenValue(const FieldView&) -> Result<ArchiveReader>`(1단계는 그래프 공유 예산, 2단계는 같은 크기 한도·무제한 operation·work), `ReadReference(ArchiveReader&, const ReferenceExpectation&) -> Result<DecodedReference>` — `DecodedReference { bool isNull; ObjectHandle handle; }`. 1단계는 token 범위를 검사하고 (token, 기대 형식) 기록을 남기며 isNull을 정확히 채우고 handle은 비운다. 2단계는 만든 객체 handle을 채운다. Required null 진단은 codec이 그 필드의 검증(trailing byte 포함)을 마친 뒤 `ReportIfRequiredNull(isNull)`로 보고한다(1단계에서만, 거부하지 않는다 — 소프트 참조도 같다).
- 단계 정보는 한 곳에서만 얻는다: decode의 `construction`이 nullptr이면 1단계다. 문맥은 2단계에서 `Report`·`CollectSoftReference`·`Spend`·`ReserveApply`를 아무것도 하지 않는다(진단·수집은 1단계에서 한 번만).

**thunk와 등록**

- `RuntimeFieldOps`: `encode(const RuntimeFieldOps& self, ObjectRegistry&, const ObjectAccessContext&, ObjectHandle, ArchiveWriter& payload, FieldEncodeContext&) -> Result<uint32 wireType>`, `decode(const RuntimeFieldOps& self, ConstructionContext* construction, ObjectHandle, const FieldView&, FieldDecodeContext&) -> Result<void>`. `self`로 같은 함수표의 read·writeStaged를 쓸 수 있어, 라이브러리의 `detail::EncodeThroughFieldValue`·`DecodeThroughFieldValue`가 단일 값 종류를 read·writeStaged로 구현한다 — 수동 함수표와 실행 중 정의되는 필드(Sparks `@save`, UI 동적 속성)가 같은 경로·같은 Soft 수집을 쓴다.
- 등록: encode·decode는 둘 다 있거나 둘 다 없다. `completeFieldLayout = true`면 Transient가 아닌 모든 필드에 둘 다 있어야 한다(2.5절의 뜻을 영속 필드의 codec까지 넓힌다). 그래프 저장·복원은 영속 필드마다 둘을 요구한다. struct·배열(B4·B5)은 Transient가 아니면 등록에서 필수다. 생성 코드는 단일 값 필드에 다섯 thunk(read·write·writeStaged·encode·decode)를, struct·배열 필드에 encode·decode를 낸다(read·write·writeStaged는 null — 3.6절).
- 생성 codec(`detail::EncodeGeneratedField<Owner>`·`DecodeGeneratedField<Owner>`): 시작할 때 `Field().value`가 정적 멤버 형식과 같은지(kind, enum은 `GeneratedType<E>::Id()`) 확인하고 다르면 TypeMismatch다(별명·수동 스키마와 정적 thunk의 어긋남이 조용한 오인코딩이 되지 않게). 직렬화기는 encode가 돌려준 wire가 필드 단위 `FieldWireType(field, ReferenceWireType())`(5.6절 — 배열은 원소에 따라 ValueArray·RecordList)인지 다시 확인한다.
  - encode: 멤버를 직접 읽어 쓴다(String을 복사하지 않는다 — ArchiveWriter가 성장 전에 크기 한도를 검사한다). SoftObjectRef는 asset을 지역으로 복사한 뒤 (asset, path view)로 검증해 쓰고, payload를 쓴 뒤 owner의 논리 수명을 다시 확인하기 전에 수집한다. 다시 검증하는 것: enum 선언 값, SoftObjectRef null 규칙·경로 상한, Required null(정책이 켜졌을 때). 쓰기 뒤 owner의 논리 수명을 다시 확인한다.
  - decode: 1·2단계가 같은 reader 호출 순서를 실행한다(문자열·경로는 양 단계 모두 `ReadStringView`). 값 검증(wire 종류 TypeMismatch, 정수 폭, enum, digest 길이, Soft 규칙, trailing byte InvalidArgument)은 한 함수로 두어 1단계에서 실행하고, 2단계 staged 쓰기(`WriteGeneratedStagedField`)의 검사는 그 부분집합이다. 필드 진단은 그 필드의 검증(trailing byte 포함)이 끝난 뒤 보고한다(배열 원소 진단은 원소 검증 직후 — 필드가 끝내 실패하면 그 호출의 진단은 무효다, 5.6절). 2단계에서만 staged 쓰기로 게시한다.
- 미선언 enum 값: `Reject`면 InvalidArgument. `ReportAndDefault`면 1단계가 `UndeclaredEnumValue`를 보고하고 2단계가 바꾼 값을 쓴다 — 일반 enum은 값 0이 선언되어 있으면 0, 아니면 첫 열거자, Flags enum은 underlying 범위 안이면 선언되지 않은 비트만 지운 값(`v & 선언 mask` — 알려진 비트를 잃지 않는다), 범위 밖·음수면 0이다(`EnumReplacementValue`). 이 정책은 진단을 받을 곳이 있어야 하며(`CanReport()`), 공용 codec도 그것이 없으면 InvalidArgument다. 생성 decode는 등록된 enum 스키마로 판정한 뒤 정적 enum(`GeneratedType<E>::IsDeclared`)으로도 1단계에서 확인한다(등록 enum이 정적 enum보다 넓어도 2단계가 실패하지 않게). 3.4절의 기본값 서술도 이 규칙으로 맞춘다.
- 객체 Record의 높은 버전(AS 5.3 타입 데이터 축): 파일 버전이 현재보다 높으면 migrate를 부르지 않는다. `Reject`면 Unsupported, `LoadNewerSkippingUnknown`이면 그 버전으로 Record를 열어 아는 필드만 해석하고 모르는 필드는 건너뛰며 `NewerVersion`(path 없음)을 보고한다(모르는 필드 정책이 `Reject`면 그것이 우선한다). 낮은 버전만 migrate한다.

**그래프 저장·복원**

- `SaveObjectGraph(Allocator&, ObjectRegistry&, const ObjectAccessContext&, ObjectHandle root, ObjectGraphSaveOptions = {})`, `ObjectGraphSaveOptions { limits; FieldEncodePolicy policy; FieldCodecHooks* hooks = nullptr; }`. 영속(제외되지 않은) 필드마다 프레임에 들어가 encode가 payload를 쓰고 Record에 더한다. graph v1의 `WriteReference`는 지금의 대상 타입 검사와 BFS local ID 부여다(문맥은 `nodes` 재할당을 고려해 원소 참조를 보관하지 않는다).
- `ObjectGraphLoadOptions { limits; FieldDecodePolicy policy; migrate; migrationUser; FieldCodecHooks* hooks = nullptr; }`.
- **예산**: 그래프 `Budget`이 `ArchiveBudget` 하나를 모든 Record 트리와 공유한다(Serialization에 `RecordReader::Open(…, SharedPtr<ArchiveBudget>)` 공개 오버로드를 더한다). 스냅샷 편차가 없어지고 그래프 청구와 reader 청구가 같은 누계다.
- **1단계**(노드마다, 생성 전): 타입 기반 생성 거부(Abstract·construct 없음 — A0-O의 WorldOnly 정책도 여기)를 판정하고, Record 필드를 기록 순서로 돌며 스키마에 없으면 `UnknownField`(Reject 정책이면 InvalidArgument), Transient 필드가 있으면 Unsupported다. 아는 필드는 프레임에 들어가 `decode(self, nullptr, …)`로 검증하고, 필드마다 (스키마 위치, 등록 함수표 `const RuntimeFieldOps*`, FieldView)를 `LoadNode`에 보관한다. decode 직후 그 필드의 2단계 몫을 바로 청구한다: `Δ(공유 예산 사용량) + Σ ReserveApply`(`Spend(w)`는 양 단계에서 하는 일이라 1단계가 2w를 청구하고, `ReserveApply(w)`는 2단계에만 있는 일 — staged 쓰기 1 + 문자열·경로 복사 byte×2 + 참조 수 — 이라 1단계가 w를 청구한다). 누락은 Transient가 아니고 `IsMissingAllowed`가 아닌 스키마 필드 중 Record에 없는 것이다(허용되지 않으면 NotFound). 허용된 누락 중 Required는 `RequiredMissing`(파일에 없음)을 보고한다(배열 필드 제외 — 누락 배열은 생성자 값을 유지한다, 5.6절) — 생성자 기본값이 non-null인 SoftObjectRef도 보고 대상이다. 참조 token은 `{uint32 target; uint32 expectation}`으로 두고 기대 형식은 필드마다 한 번만 보관한다(B5에서 원소마다 복제하지 않는다).
- **도달성·참조 타입**: 모든 노드의 1단계 뒤 BFS가 도달한 노드의 token에 대해 검사한다(자기·전방 참조 포함). (기대 TypeId, 대상 노드 TypeId) 쌍을 캐시하고 기대 타입 lease는 캐시 항목이 로드 끝까지 쥔다. `TypeId{}`는 조회 없이 통과하고, 조회·삽입마다 청구한다. 기대 형식은 필드 안에서 같은 값이 반복되면 한 번만, 다르면(B4 struct의 서로 다른 참조 멤버) 따로 보관한다. 정적 기대(`staticNative`)도 같이 검사한다. 1단계 끝에 `노드 수 + 1(Commit) + staged 간선 수`를 청구한다.
- **2단계**: 그래프 `Budget`에 접근하지 않는다. 모든 객체를 만든 뒤 보관한 함수표·FieldView로 `decode(self, &transaction, …)`를 부른다. 입력 내용 때문에 실패하지 않는다 — 실패는 할당 실패와 Registry 상태(ShuttingDown·Busy·슬롯 상한·접근 문맥)뿐이며 전체 rollback이다. 그 밖의 2단계 실패는 계약 위반(1단계 검증이 2단계 검사를 포함하지 않음)으로 보고 오류 반환과 rollback으로 처리한다. 원본과 마이그레이션 결과는 호출 끝까지 불변이며 모든 콜백이 이를 지킨다.

**`detail::ValidateFieldWrite`**

- `ValidateFieldWrite(const FieldWriteBinding&, const FieldWriteTarget&, const FieldValue&) -> Result<void>`는 lease를 얻지 않는 순수 검사다. `FieldWriteTarget { const FieldSchema* actual; uint32 effectiveFlags; const EnumSchema* actualEnum; const void* sourceExpectedNative; const void* targetExpectedNative; const TypeLease* referenced; bool allowNull; }`(B5에서 `allowNull`과 아래 "null 금지" 단계를 지우고 원소 모드 `element`를 더한다 — 5.6절). 소유 타입·양쪽 기대 타입(·B6의 경로 struct 레코드) lease는 호출자가 얻어 callback 완료까지 쥔다(재진입 재등록으로 검증과 게시의 기준이 달라지지 않게). enum은 실제 객체 쪽 `FieldEnum`으로 검사한다. 오류 순서는 값 종류·대상 타입·enum ID(TypeMismatch) → null 금지(InvalidArgument) → 읽기 전용(Unsupported) → 값 내용(Overflow·InvalidArgument). `FieldWriteOperation`은 양쪽 기대 타입의 native 불일치를 참조 값 조회(StaleHandle) 전에 TypeMismatch로 거부한다. unbound 원본 함수표 경로와 callback null 검사는 `FieldWriteOperation`에 남는다. 그래프 decode(staged)는 이 검사를 거치지 않는다(ReadOnly는 로드를 막지 않는다).

**B4·B5가 같은 문맥에 더하는 것(계약만 지금 고정)**

- 두 문맥의 `Scratch()`(예산을 청구하는 할당기)·`ObservationScratch()`(관찰 임시 값 — 단일 할당 한도 없음, B5), decode의 `OpenRecord`·`OpenValueArray`·`OpenRecordList`·`OpenListItem`·`MigrateStruct`, `AddFields(n)`·`AddElements(n)`(2단계 no-op). B4의 구체 형태는 4.6절이다.
- 1·2단계는 같은 값을 같은 순서로 방문한다. 중첩 struct 마이그레이션 결과는 1단계 방문 순번으로 `LoadNode`에 (struct ID, 원본 버전, 소유 버퍼)로 보관하고 2단계가 같은 순번으로 받아 대조한다(어긋나면 계약 위반 — 오류 반환·rollback). 중첩 색인은 2단계 전용 open(같은 크기 한도·버전 범위, 무제한 work, 그래프 필드 누계에 세지 않음)으로 다시 연다.
- struct hook(B4): 관찰은 1단계에서만, 관찰 대상으로 등록된 struct ID(로드 시작 때 정렬·검사 — 미등록 NotFound, 참조 struct InvalidArgument)를 **검증과 같은 한 번의 해석으로** scratch 임시 값에 채워 `ObserveStruct(frame, TypeId, nativeKey, const void* value)`를 부른다(따로 두 번 해석하면 중첩 마이그레이션·진단·필드 누계가 두 번 일어난다. 실패하면 생성 전에 로드 실패 — BulkDataRef 대조의 `CorruptPackage`). 치환은 encode에서 `SubstituteStruct(frame, TypeId, nativeKey, const void* in) -> Result<const void*>`(hook이 같은 native 타입의 사본을 돌려준다 — 저장은 객체를 바꾸지 않는다. 반환값은 **그 struct 값의 encode가 끝날 때까지** 유효해야 한다 — 안쪽 struct의 치환이 바깥 결과를 무효로 만들지 않게. null은 InvalidArgument). 참조를 담는 struct는 치환·관찰하지 않는다. 깊이·원소 여부와 무관하게 모든 struct 값에 적용하고 encode 순서는 결정적이다(AS 5.5).
- 로드는 필드별 MaxElements를 걸지 않으므로(5.5절) MaxElements를 바꿔도 Version을 올릴 필요가 없다 — 옛 빌드도 `MaxArrayElements` 안의 새 데이터를 읽는다(B5 확정안에서 바꿨다).
- decode는 staged 객체 전용이다. live 객체의 속성 단위 변경은 B6 경로 연산이 맡고, Undo 기록은 CU4가 B6 연산의 옛 값 출력으로 만든다(비우기·struct 원소 제거는 연산 전 `PathRead`로 — 7.1절).

## 7. 경로 접근(B6)

- **해석 결과는 FieldId 체인 + 원소 index**(`PropertyPath { Span<const PathStep> }`, `PathStep { Guid fieldId; uint32 index; bool indexed }`)다. UI 문서의 Transaction(CU4)도 같은 표현을 쓴다. 문자열 경로(`toolName('.' toolName | '[' index ']')*`, 대소문자 무시, 모르는 이름은 `UnknownProperty` — AS 4.6절)는 schema로 이 체인을 만드는 앞단이다.
- 연산: 단일 값 읽기·쓰기(`FieldValue`), 배열 길이·삽입·삭제·원소 교체, 참조 struct의 멤버 쓰기, `ObjectRefArray`의 삽입·교체(5.2절 API), `StructRefArray`의 원소 삽입·삭제와 원소 멤버 쓰기(5.6절 API). 실제 접근은 생성 thunk(`ops.path`)가 정적 타입으로 한다. 검증은 3.6절의 단일 검증 지점을 지난다(ReadOnly·대상 타입·enum·MaxElements).
- 원자성: 소유 객체 Borrow 안에서 하고, cursor가 직접 할당하는 멤버·원소 끝 지점은 할당부터 게시까지 encode 고정을 쥐어 재진입 변경을 막으며, 할당 뒤 게시 직전에 `Borrow.Get()`으로 소유자 생존을 다시 확인한다(고정은 RequestDestroy를 막지 않는다 — 7.1절). encode 고정된 소유자(5.6절)에는 Busy다. 배열 삽입은 원소와 새 버퍼를 먼저 만든 뒤 무할당으로 옮겨 게시한다(살아 있는 배열의 내부 성장은 새 버퍼를 게시하기 전에 옛 버퍼를 반환하므로 그 해제 콜백이 해제된 버퍼를 본다 — 쓰지 않는다). 실패하면 미게시.
- 명령행 값 표기(쉼표 목록·`none`·경로 또는 AssetId, AS 13절)는 도구 계층이 이 연산 위에서 해석한다. B6은 A0-B 순서대로 지금 구현한다(7.1절 — 처음에는 A6까지 미룰 수 있다고 했다).

### 7.1 B6 구현 API(확정안)

7절을 구현하는 구체 형태다. 설계 교차검증(2026-11-14 — 구현 적합성, 반례와 UE 5.6 대조)의 지적을 반영했다. B5의 규칙을 그대로 따른다: 경로 템플릿은 생성 헤더 안에서만 인스턴스화하고, requires 탐지 형질은 쓰지 않으며, 할당은 게시 전에 끝내고 게시 단계는 할당·반환을 하지 않는다(옛 값·옛 버퍼는 게시 뒤에 반환). 두 커밋으로 나눈다: **B6a** 런타임·문자열 앞단·손으로 쓴 특수화 시험(+ 현행 결함 정정), **B6b** MetaCompiler·fixture·생성 시험.

**표현(MetaSchema `TypeSchema.h`)**

- `inline constexpr uint32 MaxPropertyPathSteps = MaxStructDepth + 1;`(17 — codec 프레임 수와 같다), `struct PropertyPath { Span<const PathStep> steps; };`. `PathStep{fieldId, index, indexed}`는 그대로이고 원소 index는 배열 필드 단계에 둔다(`[slots[2], material]`). **정규형**: `indexed == false`면 `index == 0`(아니면 InvalidArgument) — CU4가 경로 동등성으로 병합할 수 있게.
- 경로는 **객체 하나 안에서 끝난다**(참조를 따라 다른 객체로 들어가지 않는다 — 객체 지정은 CU4의 안정 ID가 맡는다. UE PropertyPathHelpers는 객체·함수 세그먼트를 지나지만 Rocks는 하지 않는다).
- `TypeSchema.h`로 옮기는 공용 도우미: `EffectiveToolName(const FieldSchema&)`·ASCII 대소문자 무시 비교(TypeRegistry.cpp 익명 namespace에서 — `detail::EnumNameEquals`와 같은 구현을 공유).

**문자열 앞단(`Object/PropertyPath.h` — Archive 비의존, 스레드 무관)**

```cpp
enum class PropertyPathError : uint8 {
    None, Syntax, TooDeep, UnknownProperty, UnknownField, NotArray, NotStruct, IndexRequired, IndexNotAllowed,
    IndexOutOfRange, InvalidValue, ReadOnly, AggregateValue, NoPathAccess, TooManyElements, StaticTypeMismatch };
struct PropertyPathFailure { PropertyPathError reason = PropertyPathError::None; usize step = 0; usize offset = 0; };
struct PropertyPathBuffer { PathStep steps[MaxPropertyPathSteps]; usize count = 0; PropertyPath View() const noexcept; };
Result<void> ParsePropertyPath(const TypeLease& type, StringView text, PropertyPathBuffer& out,
                               PropertyPathFailure* failure = nullptr) noexcept;              // 할당 없음
Result<void> FormatPropertyPath(const TypeLease& type, PropertyPath path, String& out) noexcept;   // String에 할당
```

- 문법 `name ('.' name | '[' index ']')*`. 이름은 ASCII 식별자이고 실효 도구 이름(toolName, 없으면 C++ 이름)과 대소문자 무시로 비교한다 — toolName이 있으면 C++ 이름은 받지 않는다(AS 13절의 "Name으로 노출한 이름"). index는 정규 10진(앞자리 0은 `0` 하나만, 부호·공백·uint32 초과 거부).
- 해석은 클래스·struct lease 스키마로 한다(파생 스키마가 상속 필드를 같은 ID로 담는다). struct 멤버는 `FieldStruct(i)`, 원소 struct는 원소 규칙으로 내려간다. Opaque 필드는 이름 조회에서 건너뛰고, enum lease를 넘기면 InvalidArgument.
- 실패: 문법 InvalidArgument(Syntax), 17단계 초과 InvalidArgument(TooDeep), 모르는 이름 **NotFound + `UnknownProperty`**(step·byte offset — ErrorCode에 새 값을 더하지 않는다. 도구 계층이 AS 4.6절 `UnknownProperty` 진단으로 바꾼다), 배열 아닌 필드의 `[` TypeMismatch(NotArray), struct가 아닌 값(index 없는 배열 포함)의 `.` TypeMismatch(NotStruct).

**연산(`Object/PropertyPath.h`, 소유 스레드 전용)**

```cpp
Result<void>  PathRead(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle, PropertyPath, FieldValue& out,
                       PropertyPathFailure* failure = nullptr) noexcept;
Result<void>  PathWrite(…, PropertyPath, const FieldValue& value, FieldValue* previous = nullptr,
                        PropertyPathFailure* failure = nullptr) noexcept;           // 원소 교체 포함
Result<usize> PathArrayLength(…, PropertyPath, PropertyPathFailure* = nullptr) noexcept;
Result<void>  PathInsertElement(…, PropertyPath /*마지막 단계 indexed: 삽입 위치, size면 끝*/,
                                const FieldValue* value, PropertyPathFailure* = nullptr) noexcept;
Result<void>  PathRemoveElement(…, PropertyPath /*마지막 단계 indexed*/, FieldValue* removed = nullptr,
                                PropertyPathFailure* = nullptr) noexcept;
Result<void>  PathClearArray(…, PropertyPath, PropertyPathFailure* = nullptr) noexcept;
```

- 모든 연산이 `PropertyPathFailure`로 실패 단계와 사유를 돌려준다 — 도구가 모르는 속성·범위 오류·값 오류를 나눠 보고한다. 사유와 코드: UnknownProperty(문자열 앞단의 모르는 이름, NotFound), UnknownField(FieldId 체인의 없는 필드 — 스키마가 바뀐 옛 경로, NotFound), NotArray·NotStruct(모양 불일치, TypeMismatch), IndexRequired(index가 있어야 하는 단계에 없음, InvalidArgument), IndexNotAllowed(index가 없어야 하는 단계에 있음, InvalidArgument), IndexOutOfRange(InvalidArgument), InvalidValue(값 내용, InvalidArgument), ReadOnly(Unsupported), AggregateValue(통째 struct·배열, Unsupported), NoPathAccess(path thunk 없음, Unsupported), TooManyElements(MaxElements, CapacityExceeded), StaticTypeMismatch(정적 타입과 스키마 불일치, TypeMismatch). 사유가 없는 오류(문맥·수명·Busy·OutOfMemory·Overflow)는 None이고 step만 채운다.
- **옛 값 출력(CU4용)**: `PathWrite`의 `previous`는 쓰기 전 값이고, `PathRemoveElement`의 `removed`는 단일 값 원소의 값이다(struct 원소에 `removed`를 주면 InvalidArgument). String·Soft 경로는 `previous`·`removed`의 FieldValue allocator와 원본 allocator가 같을 때만 Swap·이동으로 옮기고(추가 할당 없음), 다르면 고정 아래 게시 전에 그 FieldValue의 allocator로 복사한다(CU4 Undo 기록이 객체 allocator의 버퍼를 쥐지 않게). `previous`가 쥐던 옛 버퍼는 고정을 푼 뒤 반환하고, 연산이 실패하면 `previous`·`removed`는 바뀌지 않는다. 최상위 단일 값 필드는 write 직전에 등록된 read로 `previous`를 얻는다(read와 write 사이의 재진입 변경은 계약 밖). 비우기와 struct 원소 제거의 옛 값은 CU4가 연산 전 `PathRead`로 만들고, struct 원소 제거의 복원은 "기본 원소 삽입 + 잎 단위 쓰기" 재생으로 충분함을 시험으로 고정한다(제거 → 재생 → encode byte 동일). 캡처한 참조 handle은 약한 참조이므로 CU4가 RootRef로 붙잡는다.
- 끝 지점별 동작:

| 끝 지점 | Read·Write | Insert·Remove·Clear |
|---|---|---|
| 최상위 단일 값 필드 | 등록된 `read`·`write`(FieldWriteOperation — 수동 함수표·CU6 동적 필드도 같다) | — |
| struct 멤버 단일 값(String·Soft·enum 포함) | 멤버 path thunk | — |
| 참조 멤버(인라인 struct) | `SetReference` | — |
| 참조 멤버(StructRefArray 원소 안) | 새 live `ObjectRegistry::SetElementReference(access, owner, const detail::StructElementLocation&, ObjectRef<T>&, target)`(SetEdge가 이미 원소 위치를 받는다), 단일 지점 `detail::SetPathMemberReference` | — |
| `Array<E>` 단일 값 원소 | 값 읽기·교체 | 삽입은 값 필수, 제거·비우기 |
| `Array<S>`(참조 없는 struct) 원소 | 멤버 단위만 | 삽입은 값 없이 기본 원소(`S(Allocator&)` 또는 `S()` — `__is_constructible`로 고른다) |
| `ObjectRefArray` 원소 | `SetReferenceAt`(null 허용) | `InsertReferenceAt`·`RemoveReferenceAt`·`ClearReferences` |
| `StructRefArray` 원소 | 멤버 단위만 | `InsertElementAt`·`RemoveElementAt`·`ClearElements`(값 없음) |

- struct·배열을 **통째로 읽거나 쓰면 Unsupported**(AggregateValue — FieldValue로 표현할 수 없다, Record 복사는 CU4·A6). struct 원소 삽입에 값을 주면 Unsupported, 단일 값 원소 삽입에 값이 없으면 InvalidArgument. 모양: `PathInsertElement`·`PathRemoveElement`의 마지막 단계가 index가 없으면 InvalidArgument(IndexRequired), `PathArrayLength`·`PathClearArray`의 마지막 단계에 index가 있으면 InvalidArgument(IndexNotAllowed). 경로 연산은 `ResolveForWrite`를 쓰므로 DestroyRequested 소유자에서는 Destroyed다 — 정리 중 제거·비우기는 Registry API를 직접 쓴다(그 API는 DestroyRequested를 허용한다). Resize·원소 이동·복제는 두지 않는다. 참조 struct는 struct 단위 원자성이 없다(4.3절).
- 비정규 null(`IsNull()`인 non-zero handle)은 모든 쓰기에서 `ObjectHandle{}`로 정규화한다(B5 컨테이너·`SetReference`와 하나로). B3 쪽 세 곳을 함께 바꾼다: `FieldWriteOperation`의 StaleHandle 거부, `ValidateFieldWrite`의 null 판정(`value.reference != ObjectHandle{}` → `IsNull()`), 콜백에 넘기는 값(reference를 `ObjectHandle{}`로 바꾼 사본 — 할당 없음, 수동 함수표가 비정규 handle을 저장하지 않게). `ReferenceSchemaReviewTests`의 StaleHandle 기대(`{0, 7, 1}`)는 성공·null 저장으로 바꾼다.

**thunk(`RuntimeTypeOps.h`, 생성 템플릿 `Object/GeneratedPathAccess.h`)**

```cpp
namespace detail { class PathCursor; }   // 선언·인터페이스는 공개 detail 헤더 Object/PathCursor.h, 구현은 Private/PropertyPath.cpp
using FieldPathFunction = Result<void> (*)(const RuntimeFieldOps& self, detail::PathCursor&) noexcept;
using StructFieldPathFunction = Result<void> (*)(const RuntimeStructFieldOps& self, detail::PathCursor&, void* structValue) noexcept;
// RuntimeFieldOps 끝:       FieldPathFunction path = nullptr;
// RuntimeStructFieldOps 끝: StructFieldPathFunction path = nullptr;
template <class Owner, ValueKind Target, class MemberOwner, class Member>
Result<void> PathClassField(detail::PathCursor&, Member MemberOwner::* member) noexcept;
template <auto MemberPointer, ValueKind Target>
Result<void> PathStructMember(const RuntimeStructFieldOps&, detail::PathCursor&, void* structValue) noexcept;
```

- `PathCursor`(스택 전용)는 소유 타입 lease, 해석 프레임(최대 17 — `{field, schemaIndex, effectiveFlags, enumeration, StructView target}`), 참조 lease, `Borrow`, 연산·입출력, StructRefArray 원소 위치를 담는다. 템플릿이 멤버를 부르므로 클래스 선언은 공개 detail 헤더에 둔다.
- **필드 하나에 thunk 하나**: thunk는 자기 단계(멤버 주소·원소 index)만 처리하고, 끝이 아니면 `cursor.Descend(void* structValue)`를 부른다 — 지금 프레임의 StructView에서 다음 프레임 `schemaIndex`의 멤버 함수표(스키마 순서로 복사돼 있다)를 찾아 그 `path`를 부른다(재귀 ≤ 17). 소유자 캐스트는 `static_cast<Owner*>`(함수표가 실제 타입 레코드에서 온다). `void*`는 생성 thunk와 라이브러리 내부 사이에서만 오가고 공개 API는 handle + PropertyPath뿐이다. `path`가 null인 필드·멤버에 닿으면 Unsupported.
- **정적 타입 대조**: Descend 직전에 `NativeTypeKey<S>() == frame.target.StructOps().nativeType`(아니면 TypeMismatch — `MatchesStructView`와 같은 형태), enum은 `GeneratedType<E>::Id() == FieldTargetType(field).type`, 그리고 `FieldTargetType(*frame.field).kind == Target`. 수동 함수표의 정적 타입과 스키마가 어긋나도 `void*` 타입 혼동이 생기지 않게 한다.
- 클래스 최상위 필드의 함수표 찾기: 클래스 레코드에 스키마 위치 → ops 위치 색인을 두고 `TypeLease::FieldOps(schemaIndex) -> const RuntimeFieldOps*`(Opaque는 null)와 필드 ID 정렬 색인 `TypeLease::FieldIndex(Guid)`를 더한다(지금 클래스 `ops.fields`는 입력 순서이고 ID 조회는 선형이다 — struct 레코드에만 정렬 색인이 있다).
- 등록 규칙: 클래스 단일 값 필드의 `path`는 null이어야 한다(InvalidArgument — 최상위 단일 값 쓰기 경로는 read·write 하나). Struct·Array 필드와 struct 멤버의 `path`는 선택.
- ODR: `ReflectedEnumField`·`ReflectedStructField`(requires 탐지)를 B6 템플릿은 부르지 않는다. MetaCompiler가 대상 종류를 `ValueKind Target`으로 명시하고(필드 `TargetKind`), 컨테이너는 부분 특수화 `ArrayField`, enum은 `GeneratedType<E>`를 직접 쓴다(헤더가 없으면 조용히 거짓이 아니라 컴파일 오류). 단일 값 복사·쓰기는 Target을 받는 도우미(`CopyTargetValue<Target>`·`WriteTargetValue<Target>`). `StructMemberPointer`는 `GeneratedFieldCodec.h`에서 `GeneratedFieldAccess.h`로 옮겨 경로 헤더가 Serialization에 의존하지 않게 한다.

**검증(단일 지점)**

- 프레임마다 `PropagateFieldFlags`로 유효 플래그를 누적한다 — 바깥 ReadOnly가 멤버·원소·삽입·삭제·비우기를 모두 막고(ReadOnly), EditorOnly·Transient는 경로 접근을 막지 않는다.
- 값 쓰기·단일 값 원소 삽입은 `ValidateFieldWrite`(binding = 해석된 대상 형식·readOnly false, target = `{actual, effectiveFlags, actualEnum = frame.enumeration, element = 배열 원소인지, targetExpectedNative, referenced}`).
- 구조 변경은 새 `detail::ValidateArrayEdit(const FieldWriteTarget&, ArrayEdit{Insert, Remove, Clear})`(배열이 아니면 TypeMismatch, ReadOnly면 Unsupported — CU1의 `EditFixedSize` 같은 UI 정책을 나중에 이 한 곳에 넣는다).
- 실행 시: index(읽기·쓰기·삭제는 ≥ size, 삽입은 > size) InvalidArgument(IndexOutOfRange), 삽입 뒤 `ValidateArrayLength(field, size + 1)` CapacityExceeded(TooManyElements), 정수 폭 Overflow. Registry 컨테이너는 MaxElements를 검사한 뒤 자체 재판단 규칙으로 할당하므로 재진입이 겹친 수만큼 상한을 넘길 수 있다 — 넘친 배열은 저장에서 `ArrayTooLong`으로 드러난다(5.5절, 허용).
- 참조 값 lease 획득 순서는 `FieldWriteOperation`과 같다(기대 타입 `AcquireRegisteredType`, non-null 대상 `AcquireType`).

**원자성·수명·스레드(재탐색 대신 고정)**

- 소유 타입 lease와 참조 lease는 완료까지 쥔다. StructView는 의존 계수로 유효하다.
- **쓰기**: `ResolveForWrite<Object>`로 Borrow(고정 중이면 Busy)를 얻는다. **cursor가 직접 할당하는 멤버·원소 thunk 끝 지점(struct 멤버·배열 원소의 String·Soft 쓰기, `Array<E>` 삽입)만 `PinForEncode`를 쥐고 할당부터 게시까지 한다** — 고정 중 재진입 쓰기(ResolveForWrite)·Registry 컨테이너 변경·SetEdge는 Busy이므로 할당 콜백이 경로의 대상(원소·struct·버퍼)을 바꾸거나 없앨 수 없다. 주소 재계산은 하지 않는다(비템플릿 cursor는 멤버 포인터·원소 크기를 모르고, 주소 비교는 같은 크기 블록을 다시 받는 ABA를 놓친다). **고정은 RequestDestroy를 막지 않으므로** 할당이 끝나면 게시 직전에 `Borrow.Get()`으로 소유자 생존을 다시 확인한다(Destroyed면 미게시). 게시 뒤 고정을 풀고 옛 값·옛 버퍼를 소멸·반환한다. 같은 배열을 C++ 코드로 직접 바꾸는 콜백은 계약 밖이다(`Array.h`의 재진입 계약).
- **고정 없이 하는 끝 지점**: 최상위 단일 값 필드(등록된 write — 그 thunk가 `ResolveForWrite`로 Borrow를 얻으므로 고정을 쥐면 자기 자신 때문에 Busy가 된다. 할당 뒤 재확인은 그 thunk가 한다), `SetReference`·`SetElementReference`, Registry 컨테이너 연산(ObjectRefArray·StructRefArray의 삽입·삭제·비우기·교체 — 고정과 함께 쓸 수 없고 자체 재판단 규칙이 있다, 5.6절).
- **`Array<E>` 삽입**: (고정 아래) 원소를 `array.GetAllocator()`로 만들고, 용량이 모자라면 `Array<E> temp(live.GetAllocator())`(이동 대입이 temp의 allocator를 가져가므로 같은 allocator로)에 `TryReserve(ArrayGrowthCapacity(…))`(Core에 성장 규칙을 공개 함수로 더한다 — 지금 `GrowthCapacity`는 private) → 옛 원소를 temp에 `TryEmplaceBack(Move(e))`(예약했으므로 무할당)로 옮기며 index 위치에 새 원소 → **`Array<E> old(Move(live)); live = Move(temp);`**(이동 생성은 `live`를 allocator를 유지한 빈 상태로 두고, 빈 배열로의 이동 대입은 할당·반환이 없다 — `live = Move(temp)`를 바로 쓰면 이동 대입이 옛 버퍼를 먼저 반환해 그 해제 콜백 동안 `live`가 해제된 버퍼를 가리킨다) → 소유자 재확인 → 고정 해제 → `old` 소멸. 용량이 충분하면 `TryEmplaceAt`의 용량 내 경로(무할당)를 쓴다.
- **삭제·비우기**: 원소는 `E e(Move(live[i])); live.TryRemoveAt(i);`, 비우기는 `Array<E> old(Move(live));`로 지역으로 옮겨 뺀 뒤(무할당) 게시하고, 소멸은 게시 뒤에 한다(`TryRemoveAt`·`Clear`는 제자리에서 소멸해 소멸 콜백 중 불일치 상태가 보인다). 경로의 비우기는 용량까지 반환한다(`Array::Clear`는 용량을 남긴다). **moved-from 원소의 소멸자는 allocator를 부르지 않는다**는 계약(NATIVE 멤버 포함)을 리플렉션 struct·배열 원소의 일반 규칙으로 적는다 — 삭제뿐 아니라 `TryEmplaceAt` 용량 내 경로와 `TryRemoveAt`의 원소 이동도 이것에 기댄다.
- **읽기**: `Resolve` + `PinForEncode`로 복사하고, out을 게시하기 전에 `Borrow.Get()`으로 소유자를 다시 확인한다(String·Soft 복사는 할당한 뒤 원본을 읽으므로 그 사이 재진입 쓰기가 원본을 해제하지 못하게). `ops.read`(수동 함수표·CU6 동적 필드) 호출도 같은 고정으로 감싼다 — read thunk는 쓰기 Borrow를 얻지 않으므로 자기 고정에 막히지 않는다.
- **현행 결함 정정(B6a)**: `ReadGeneratedField`는 고정 없는 `Resolve` 뒤 `CopyFieldValue` → `TryAssign`(할당 뒤 원본 복사)을 해, 할당 콜백이 같은 필드를 `WriteGeneratedField`로 바꾸면 해제된 버퍼를 읽는다(Soft 경로도 같다). 읽기를 encode 고정으로 감싸 고친다(시험 포함).
- 스레드: 연산은 소유 스레드 전용(WrongThread), Parse·Format은 TypeLease만 있으면 어느 스레드나, staged 객체(ConstructionContext)에는 쓰지 않는다(decode의 몫).

**오류 순서**

1. 입력 형식(단계 0개·17 초과·정규형 위반·value가 Opaque) InvalidArgument → 2. 문맥·수명(AcquireType 오류) → 3. 해석(단계 순서 — ID 없음 NotFound, 모양 불일치 TypeMismatch) → 4. 통째 struct·배열·thunk 없음 Unsupported → 5. 참조 lease NotFound·StaleHandle → 6. `ValidateFieldWrite`·`ValidateArrayEdit`(TypeMismatch → Unsupported(ReadOnly) → InvalidArgument(값 내용)) → 7. Borrow·고정 Busy·Destroyed → 8. index InvalidArgument → 길이 CapacityExceeded → 정수 폭 Overflow(StructRefArray uint32 상한도 Overflow) → 9. Registry 대상 검사 StaleHandle·TypeMismatch → 10. 할당 OutOfMemory → 11. 게시 직전 소유자 재확인 Destroyed. `PropertyPathFailure`의 사유가 같은 코드의 원인을 구별한다(문자열 앞단의 UnknownProperty와 스키마가 바뀐 옛 FieldId 경로의 UnknownField 등).

**B6이 하지 않는 것**

Transaction·Undo 기록·병합(CU4 — B6은 옛 값 출력까지), 변경 통지·setter 호출(CU5·UD7 — 다만 규칙은 지금 정한다: 하위 경로 쓰기는 `steps[0]` 필드의 setter·통지 hook을 거친다. UE의 `CallParentSetters`·`PostEditChangeProperty` 대응이며, hook 칸은 그 단계에서 `RuntimeFieldOps` 끝에 더하고 `FieldPathFunction`은 바꾸지 않는다), 동적 속성 내부 경로(CU6), 명령행 값 표기(A6), 통째 Record 복사, 원소 안정 ID(prefab — UE 5.6 `FPropertyPathName`은 연관 컨테이너를 key가 아니라 원소 ID로 가리킨다, map 도입 때 참고), map key, Resize·이동·복제, 다른 객체 역참조, 해석 결과 캐시(UE `FCachedPropertyPath` — UI 바인딩·애니메이션 트랙이 매 프레임 쓰게 되면 해석과 연산을 분리해 lease를 쥔 동안 재사용하는 API를 더한다), 로드 MaxElements(5.5절 그대로).

**MetaCompiler(B6b)**

- 클래스 Struct·Array 필드: 함수표의 `valueNativeType` 자리에 없으면 `nullptr`를 명시하고 뒤에 `+[](const ::rocks::RuntimeFieldOps&, ::rocks::detail::PathCursor& cursor) noexcept -> ::rocks::Result<void> { return ::rocks::detail::PathClassField<{native}, ::rocks::ValueKind::{TargetKind}>(cursor, &::{Owner}::{name}); }`를 낸다. 단일 값 필드에는 내지 않는다.
- struct 멤버: `detail::StructFieldOps<&{native}::m>(id, &detail::PathStructMember<&{native}::m, ValueKind::{TargetKind}>)`(둘째 인자 기본값 nullptr — 기존 수동 시험은 그대로 컴파일된다). Binds 미러는 실제 타입 `NativeName`의 멤버 포인터를 쓴다. 상속 import 필드는 `type.Fields`로 덮이고, struct 단계는 등록 StructView로 내려가므로 include를 더하지 않는다. 생성 헤더에 `<Object/GeneratedPathAccess.h>`를 include한다. descriptor는 바꾸지 않는다.

**시험**

- 앞단: 문법 표(빈 문자열·`.`·끝 `.`·`[]`·`[01]`·`[4294967296]`·공백·비ASCII), 대소문자 무시, toolName이 있을 때 C++ 이름 거부, 상속 필드 이름, UnknownProperty step·offset, NotArray·NotStruct, 18단계, Format 왕복, base lease로 해석한 경로를 파생 객체에.
- 레지스트리 단위(손 특수화, 타입마다 TU 하나): 종류별 읽기·쓰기·옛 값, 통째 Unsupported, ReadOnly 전파(struct·배열 필드 → 멤버·원소·삽입·삭제·비우기), enum 미선언·enum ID 불일치·정적 타입 대조, Soft 규칙, Overflow, MaxElements 경계(상한까지 성공·+1 CapacityExceeded), index·모양·정규형 오류, ObjectRefArray null·비정규 null·타입·stale·Marking shade, StructRefArray 삽입·삭제·비우기·원소 멤버 쓰기·원소 참조 쓰기(위치 오버로드·블록 범위), 인라인 참조 struct, `Array<S>` 기본 원소(allocator 생성자)와 `arr[i].inner.name`, `Array<E>` 삽입의 버퍼 교체 순서(해제 콜백 중 재진입이 옛 버퍼를 보지 않음), 고정 중 모든 쓰기 Busy, **할당 콜백 재진입**(같은 배열·원소 삭제 후 같은 크기 재삽입(ABA)·배열 축소·소유자 파괴 → Busy 또는 오류, 미게시, ASan 무결), 읽기 중 재진입 쓰기 Busy(현행 `ReadGeneratedField` 결함 회귀 포함), 모든 할당 실패 스윕 내용 불변, lease 보유 중 Unregister Busy, null path thunk Unsupported, struct 원소 제거 → 기본 원소 삽입 + 잎 쓰기 재생 → encode byte 동일, `PropertyPathFailure` 사유.
- 생성: StructFixture·ArrayFixture·CoreValueFixture 모든 종류, Binds `transform.translation.x`, 경로 쓰기 뒤 그래프 저장·로드 왕복.
- MetaCompiler: Struct·Array 필드와 모든 멤버에 path가 나오고 단일 값 필드에는 없음, Target 종류, Binds 실제 타입, 세 모듈 상속 import 필드, Target 불일치 probe 컴파일 실패.

## 8. 빌드 주입 상수(B7)

- **툴체인 지문**: SHA-256(정규 텍스트: 컴파일러 `--version`에서 `InstalledDir:` 등 호스트 경로 줄을 뺀 식별·버전, `Target:` 줄의 대상 triple, 실제 `CompileFlags`에서 가져온 부동소수점 모델 인자). 빌드 구성(Debug·Release)과 호스트 경로·환경은 넣지 않는다. 지문은 너겟 키에만 들어가고 쿠킹 byte에는 들어가지 않는다(AS 10절의 호스트 무관 요구와 충돌하지 않는다 — 다른 툴체인의 너겟 항목을 재사용하지 않을 뿐이다).
- **도구 빌드 ID**: SHA-256(정렬된 입력 목록 + 정규화된 compile 인자 + 툴체인 지문). 입력은 실행 파일 모듈 액션 그래프의 1차 입력(소스·규칙)과 `NativeBuildEngine.InputSnapshot`의 헤더 디렉터리 inventory(그 모듈과 의존 모듈의 include 디렉터리 안 헤더 전부)이고, 생성물과 BuildInfo 자신은 뺀다. depfile은 쓰지 않는다(Rocks.Build에 depfile 추적이 없고, 컴파일 뒤에야 생기는 정보라 clean 빌드와 증분 빌드의 ID가 달라진다). 경로는 `src:`·`out:`·`sdk:` 접두를 붙인 상대 경로(출력 디렉터리 이름의 preset·구성이 섞이지 않게)이며 내용 해시는 **텍스트 파일만** CRLF를 LF로 정규화해 계산한다(prebuilt 바이너리는 그대로 — 저장소는 이미 `* text=auto eol=lf`). Summary·AssetIndex 캐시 키의 진단용이다(AS 10절, 병합 조건 아님).
- 주입: NativeBuildEngine이 Core에 도달하는 실행 파일 모듈마다 `<out>/BuildInfo/<Module>.BuildInfo.cpp`를 만들어(계획 두 단계 — 파일을 만든 뒤 액션 그래프를 세운다) 그 모듈 소스에 더한다(Program.cs와 InstalledSdk 경로가 함께 쓴다). Core에 선언 헤더(`Core/Build/BuildInfo.h`: `ToolchainFingerprint()`·`ToolBuildId()`가 `Sha256Digest`를 돌려준다)를 둔다. 생성 TU 하나만 다시 컴파일된다.
- 시험: 지문이 구성과 무관하고 컴파일러 버전·대상에 따라 바뀜, 줄 끝 정규화, 생성 TU만 재컴파일. AS 시나리오 13·25에 **구성 사이 쿠킹 byte 비교**를 더한다(C3은 수학 digest만 확인했다).
- 문서 정정(반영함): llvm-nm 외부 심볼 검사·fmadd·`long double` 정책은 C3에서 이미 모든 구성에 구현되었다(CDF 2절). AS 5.6절의 "A0-B에서 추가"·"Development 구성"·`-fno-math-errno`(clang-cl이 거부해 쓰지 않기로 함, CDF 2.1절) 서술을 고쳤다.
- 위 서술 중 여러 곳은 8.1절 확정안이 바꾼다: 대상은 `Target:` 줄이 아니라 effective triple, 컴파일러 식별은 차단 목록("`InstalledDir:` 등")이 아니라 허용 목록 + 컴파일러·링커 바이너리 해시, "규칙" 입력은 규칙 원본이 아니라 그 결과인 액션, "생성물 제외"는 액션 그래프 출력 제외(MetaCompiler 생성 헤더는 입력이다), 주입 대상은 "Core" 이름 비교가 아니라 `ProvidesBuildInfo` 표지, 도구 빌드 ID는 패키지 Summary·AssetIndex 캐시 키에 넣지 않고 쿠킹 보고서·캐시 header에 진단용으로만 둔다(12절 결정 17), 시험의 구성 사이 쿠킹 byte 비교는 A4로 옮긴다.

### 8.1 B7 구현 API(확정안)

8절을 구현하는 구체 형태다. 설계 교차검증(2026-11-14 — 구현 적합성·반례, 반례는 이 기기의 clang-cl 21.1.6으로 실측)의 지적을 반영했다. B7은 다른 단계와 독립이다. 경로는 `Programs/` 기준이다.

**툴체인 신원과 빌드 환경 위생(`Rocks.Build/Toolchain.cs`·`Rocks.Tooling.Common/NativeEnvironment.cs`)**

- `NativeToolchain`에 discovery가 채우는 `CompilerVersion`·`TargetTriple`·`CompilerDigest`·`LinkerDigest`·`OwnedRoots`·`AbiFields`·`RemovedEnvironment`(init, 기본 빈 값)를 더한다. BuildInfo 대상 실행 파일이 있는데 `CompilerVersion`·`TargetTriple`·`CompilerDigest`·`LinkerDigest` 중 빈 값이 있으면(수동으로 만든 툴체인) ArgumentException이다 — 빈 지문을 조용히 만들지 않는다.
  - `CompilerVersion`: `--version` 출력의 처음 비어 있지 않은 줄(`clang version`이 없으면 discovery 오류). 그 줄의 괄호 묶음 중 경로나 URL(`/`·`\`·`:` 포함)을 담은 것은 안의 7–40자 hex 토큰만 남기고(없으면 묶음째) 지운다 — 자가 빌드 LLVM이 넣는 로컬 저장소 경로를 없애고 commit hash만 남긴다. 경로가 없는 괄호(NDK 첫 줄의 `Android (…, based on r…)`)는 건드리지 않는다.
  - `TargetTriple`: `<compiler> <CompileArguments> -print-effective-triple`의 한 줄(clang-cl은 `/clang:` 접두 — 이미 쓰는 `-print-resource-dir`와 같다). Android는 `--target`이 CompileArguments에 있어야 실제 대상이 나오므로 프로브는 그 인자가 정해진 뒤 실행한다. 출력의 CR은 자르고, 종료 실패·빈 출력·여러 줄·`[A-Za-z0-9._+-]` 밖 문자는 discovery 오류. `--version`의 `Target:` 줄은 쓰지 않는다(Apple clang은 호스트 커널 버전, NDK clang은 호스트 triple을 낸다).
  - `CompilerDigest`·`LinkerDigest`: 컴파일러 드라이버와 최종 링크·ThinLTO 코드 생성을 하는 바이너리 내용의 SHA-256. Windows는 컴파일러와 `LinkerPath`(lld-link), Android는 NDK clang과 `<NDK bin>/ld.lld`, macOS는 컴파일러가 xcrun shim(`/usr/bin`의 `clang`·`clang++`·`cc`·`c++` — Xcode 버전을 구별하지 못한다)일 때만 `xcrun --find clang++`로 찾은 실제 컴파일러를, 그 밖에는 주어진 경로를 해시하고, 링크는 ld(clang driver 탐색 순서대로 컴파일러 옆 — 호출 경로, 그다음 링크를 따라간 실제 경로 — 에 있으면 그것, 없으면 `xcrun --find ld`)와 실제 컴파일러 기준 `../lib/libLTO.dylib`의 digest를 이은 텍스트(`<ld hex>\n<libLTO hex>\n`)를 해시한다(macOS·Android의 `LinkerPath`는 지금 null이다). 같은 버전 줄을 내는 다른 벤더 빌드(이 저장소의 swiftlang 포크도 `clang version 21.1.6`)를 구별한다. 같은 설치본이면 경로와 무관하게 같다. discovery에서 한 번 계산한다(실측 clang-cl 119 MB·lld-link 73 MB에 약 0.2초 — 캐시를 두지 않는다).
  - `OwnedRoots`: 이름 붙은 툴체인 소유 디렉터리(`resource`·`runtime`·`sysroot`·`ndk` 등 — resource dir, RuntimeFiles의 디렉터리, macOS SDK 경로, NDK 루트). `FingerprintPaths`에서 추론하지 않는다(시험 툴체인의 FingerprintPaths는 픽스처 루트 안의 파일이라 추론하면 루트 전체가 툴체인 소유가 된다). 소유 루트끼리 겹치면(`runtime` ⊂ `resource`) 가장 긴 루트부터 적용한다. src·out·sdk **안**의 소유 루트는 허용하고(`<root>/out/toolchains/…` 설치 관례 — 가장 긴 루트가 `toolchain:` 키를 고른다), 소유 루트가 src·out·sdk를 **포함**하거나 같으면 ArgumentException(그 아래 입력이 모두 빠진다).
  - `AbiFields`: 정렬된 `key=value` 목록(CompilerVersion·SDK 버전·배포 대상·NDK revision·VCToolsVersion — 경로와 `--version`의 `Target:` 줄은 넣지 않는다). 기존 `AbiIdentity`(SHA-256 hex라 줄을 골라낼 수 없다)는 `--version`의 `Target:` 줄 대신 MSVC 접미를 지운 `TargetTriple`로 계산한다(형식 이름 `rocks-native-abi-v2`, 첫 줄 대신 정규화한 `CompilerVersion`, `InstalledDir:`·`Target:` 줄 제외, `target=<MSVC 접미를 지운 triple>`) — macOS를 업데이트하면 Apple clang의 `Target:` 커널 버전이 바뀌어 `InstalledSdk.Verify`가 SDK를 거부하던 기존 결함도 함께 고친다(SDK `ManifestVersion` 4와 함께).
- **빌드 환경 위생(허용 목록)**: `Rocks.Tooling.Common/NativeEnvironment.CreateReportedAsync`(discovery·MetaCompiler·ToolTests 공용, Windows는 VsDevCmd 출력을 합친 뒤)와 Android discovery는 프로세스 환경을 통째로 물려주지 않고 허용 목록의 변수만 목록의 정규 철자로 담은 새 환경을 만들어 자식 프로세스에 넘긴다(Windows의 `Path`·`PATH` 차이도 없앤다). 목록은 둘이다 — **Keyed**(증분 key에 든다): Windows `INCLUDE`·`EXTERNAL_INCLUDE`·`LIB`·`LIBPATH`·`VCToolsInstallDir`·`VCToolsVersion`·`VCINSTALLDIR`·`VSINSTALLDIR`·`WindowsSdkDir`·`WindowsSDKVersion`·`WindowsSDKLibVersion`·`UniversalCRTSdkDir`·`UCRTVersion`·`SOURCE_DATE_EPOCH`, macOS `DEVELOPER_DIR`·`SDKROOT`·`TOOLCHAINS`·`MACOSX_DEPLOYMENT_TARGET`·`ZERO_AR_DATE`·`SOURCE_DATE_EPOCH`, Android `SOURCE_DATE_EPOCH`. **Runtime**(넘기되 key에서 뺀다 — 도구는 절대 경로와 바이너리 digest로 고정된다): Windows `PATH`·`PATHEXT`·`SystemRoot`·`SystemDrive`·`windir`·`ComSpec`·`TEMP`·`TMP`·`USERPROFILE`·`LOCALAPPDATA`·`APPDATA`·`ProgramData`·`ProgramFiles`·`ProgramFiles(x86)`·`ProgramW6432`·`NUMBER_OF_PROCESSORS`·`PROCESSOR_ARCHITECTURE`·`ROCKS_CLANG_FORMAT`, macOS `PATH`·`HOME`·`TMPDIR`·`USER`·`LANG`·`LC_ALL`·`ROCKS_CLANG_FORMAT`, Android는 호스트 목록(`APPDATA`·`ROCKS_CLANG_FORMAT`은 같은 환경으로 실행하는 dotnet·MetaCompiler·ToolTests용이다). `Fingerprints.Toolchain`은 대상의 Keyed 집합만 정규 철자·ordinal 순으로 해시한다 — 실행마다 바뀌는 `DOTNET_CLI_TELEMETRY_SESSIONID`(dotnet msbuild)·`VSCODE_PID`·`WT_SESSION` 같은 변수가 매 빌드를 전체 재빌드로 만들던 기존 결함을 함께 고친다. 컴파일러·링커 동작을 바꾸는 것으로 알려진 변수(`CL`·`_CL_`·`LINK`·`_LINK_`·`CCC_OVERRIDE_OPTIONS`·`COMPILER_PATH`·`CPATH`·`C_INCLUDE_PATH`·`CPLUS_INCLUDE_PATH`·`OBJC_INCLUDE_PATH`·`LIBRARY_PATH`)는 허용 목록 밖이라 자연히 빠지며, 프로세스 환경에 있었으면 이름을 `NativeToolchain.RemovedEnvironment`로 남겨 빌드 출력에 한 줄로 알린다(실측: `CL=/fp:fast`가 clang-cl에 fast-math를 켠다). compile·archive·link 프로세스는 모두 `toolchain.Environment`로 실행되므로(Executor) 빌드 액션 전체에 효과가 있다. 기본 config 파일(Homebrew LLVM의 `etc/clang/*.cfg`)은 `--no-default-config`로 끈다: discovery가 `--no-default-config --version`으로 그 인자를 받는다고 확인한 컴파일러일 때만 모든 드라이버의 CompileArguments·LinkArguments·프로브(NativeEnvironment의 Windows resource-dir 프로브 포함 — 받지 않으면 인자 없이 다시 묻고, 그래도 실패하면 오류)에 **접두 없이** 넣는다 — clang-cl도 CL 옵션으로 직접 받는다(실측: `/clang:--no-default-config`는 config 로드 뒤에 해석되어 unused 경고가 `/WX`에서 오류가 되므로 쓰지 않는다). 받지 않는 컴파일러면 인자 없이 `--version`을 다시 보고 `Configuration file:` 줄이 있을 때 discovery 오류다. MetaCompiler는 인자 허용 목록(Windows·POSIX)에 `--no-default-config`를 더하고 고정 인자에는 넣지 않는다 — 네이티브 프로필(`--clang-arg`)이 discovery 결과대로 실어 온다.

**FP 모델 인자(`Rocks.Build/FloatingPointArguments.cs`, public static)**

- `IsModelArgument(string)`: Windows에서는 CL 철자 표(`/fp:`·`/arch:`·`/clang:`)를 `/`·`-` 두 접두 모두로 대소문자를 구분해 대조하고(`-fp:fast`·`-clang:-ffast-math`도 잡는다 — GCC형 `-f`·`-m` 인자는 접두를 바꾸지 않는다), `/clang:`·`-clang:` 접두를 뗀 뒤 판정한다 — `-ffp-*`, `-f[no-]fast-math`, `-f[no-]` + `unsafe-math-optimizations`·`associative-math`·`reciprocal-math`·`signed-zeros`·`trapping-math`·`finite-math-only`·`honor-*`·`math-errno`·`approx-func`·`rounding-math`·`protect-parens`·`strict-float-cast-overflow`·`cx-limited-range`·`cx-fortran-rules`, `-fcomplex-arithmetic=`, `-fdenormal-fp-math*`, `-fexcess-precision*`, `-ffloat16-excess-precision*`·`-fbfloat16-excess-precision*`, `-mfpmath*`, `-mrecip*`, `-mdaz-ftz`, `-mlong-double-*`, `-fveclib=`, ISA 선택(`-march=`·`-mcpu=`·`-mtune=`·`-m[no-]<기능>` — `-mavx2`·`-mfma`·`-mno-sse*`·`-m80387`·`-msoft-float` 등. 배포 대상 인자 `-m<os>-version-min=`은 target triple에 들어가므로 제외하고, 이름 뒤에 `:`가 먼저 오는 링커·CL 철자 `-machine:x64`도 제외), `/fp:*`, `/arch:*`.
- `InputValidation`은 모듈의 `AdditionalCompileArguments`와 `AdditionalLinkArguments`에서 모델 인자, `-mllvm`·`/mllvm:`·`-Xlinker`·`-Ofast`, `-Wl,` 조각 중 앞의 `-`·`/`를 모두 뗀 뒤 `mllvm`·`plugin-opt`·`lto`·`thinlto`·`opt:lldlto`·`lldlto`로 시작하는 것(`-Wl,--mllvm=`·`-Wl,--plugin-opt=` 포함), `-Xclang`·`-Xclang=…`·`-Xpreprocessor`, `--config`·`--config=`·`--config-system-dir=`·`--config-user-dir=`, `-fuse-ld=`·`--ld-path=`, `--target=`·`-target`·`-arch`·`-fms-compatibility-version=`·`-fmsc-version=`(owned)을 거부하고(링크 줄의 `-ffast-math`·`-mdaz-ftz`는 crtfastmath를 붙인다), `-m*`는 컴파일러 인자에서 허용 목록 방식이다(지금 빈 목록 — ISA·FP 하드웨어 선택이 FP 결과를 바꾼다. 링크 인자에는 적용하지 않아 링커 철자를 오인하지 않는다). Windows(clang-cl) 대상의 기존 CL 옵션 거부(`/Fo`·`/Fe`·`/Fd`·`/FI`·`/EH`·`/GR`·`/wd`·`/external:`·`/std:` 등)는 소문자 접두 비교 대신 CL 철자 표를 `/`·`-` 두 접두로 대소문자를 구분해 대조한다(지금 방식대로 `-`를 `/`로 바꾸면 clang-cl이 받는 `-fdiagnostics-color`·`-fdebug-compilation-dir=.`가 `/fd` 접두로 오인된다). CL 철자 표는 POSIX 대상에 적용하지 않는다(`-Fdeps/…`는 framework 경로다). 지금 `Additional*Arguments`를 쓰는 규칙이 없어 기존 빌드는 깨지지 않는다(확인함).

**툴체인 지문(`Rocks.Build/ToolchainFingerprint.cs`, public static)**

- `CanonicalText(NativeToolchain) -> string`, `Compute(NativeToolchain) -> byte[32]` = SHA-256(UTF-8(정규 텍스트)). 프로세스를 실행하지 않는 순수 함수다.
- 정규 텍스트(LF, 마지막 줄도 LF, 줄 끝 공백 없음, 줄 순서 고정):
  ```
  rocks-toolchain-fingerprint 1
  compiler: clang version 21.1.6
  compiler-sha256: <64 hex>
  linker-sha256: <64 hex>
  target: x86_64-pc-windows-msvc
  fp: /clang:-ffp-contract=off
  ```
- `target`은 `TargetTriple`에서 Windows MSVC 환경 버전 접미(`msvc19.29.30159`의 `19.29.30159` — 설치된 VC 도구 버전, 호스트 쪽)를 지운 값이다. macOS 배포 대상·Android API 수준은 compile 인자가 정한 대상이므로 남긴다(실측: 이 기기는 VS 2022가 있는데도 `msvc19.29.30159`를 낸다).
- `fp`는 `NativeCompileProfile.Arguments(toolchain, configuration, optimization, language)`의 모델 인자를 원래 순서로 쓴다. 모든 구성 × `CodeOptimization` × `NativeLanguage` 조합(지금 4 × 3 × 2)의 목록이 같아야 하고 다르면 InvalidOperationException(결정 7을 구조로 강제한다).
- 지문은 에셋 너겟 키에만 들어가고 쿠킹 byte에는 들어가지 않는다.

**도구 빌드 ID(`Rocks.Build/BuildInfo.cs` — internal `BuildInfoGenerator`·`PortablePaths`·`FileDigests`)**

- 대상: 계획의 `Executable` 모듈 중 소스가 있고 도달 집합에 **`ProvidesBuildInfo` 표지 모듈**이 있는 것. `NativeModuleInput.ProvidesBuildInfo`를 Core 규칙이 켜고 InstalledSdk의 SdkModule이 함께 나른다(이름 "Core" 비교는 쓰지 않는다 — 엔진 밖 프로젝트의 `Core` 모듈이 오인되지 않게). 정적 라이브러리·`PublicHeaderChecks`는 대상이 아니다.
- `NativeBuildRequest.SdkDirectory`(string?, InstalledSdk 게임 빌드만), `NativePlan.Closures`(ActionGraph가 이미 계산하는 모듈별 도달 집합). `RootDirectory`·`OutputDirectory`·`SdkDirectory`는 절대 경로여야 한다(상대 경로는 InputValidation이 거부 — 기준이 작업 디렉터리인지 소스 루트인지 단계마다 갈리지 않게).
- ID = SHA-256(UTF-8(manifest)). manifest는 BuildInfo를 더하기 **전** 1단계 계획에서 그 실행 파일 도달 집합의 Compile·Archive·Link 액션으로 만든다(자기 TU·object가 없으므로 자기 참조가 없다):
  ```
  rocks-tool-build-id 1
  executable: FoundationTests
  toolchain-fingerprint: <64 hex>
  toolchain-target: x86_64-pc-windows-msvc19.29.30159
  toolchain-abi: <AbiFields — 정렬된 key=value 목록을 ;로 이음>
  host-dependent: no
  action: Compile src:Core/Source/Runtime/Core/Private/Encoding/Sha256.cpp
  arg: /nologo
  …
  input: src:Core/Source/Runtime/Core/Public/Core/Encoding/Sha256.h <64 hex>
  ```
  액션은 (종류, Compile이면 소스 키·나머지는 모듈 이름)의 ordinal 순, 인자는 원래 순서, 입력은 키 ordinal 순·중복 제거. ID는 진단·캐시 무효화용이며 호스트에 따라 달라질 수 있는 `toolchain-target`·`toolchain-abi`를 일부러 담는다(호환·병합 판정에 쓰지 않는다).
- 입력: 1단계 액션의 `Inputs`, `DeclaredInputs`(액션 key와 같이 `headersOnly:false`·제외 없음으로 열되 `<out>/BuildInfo`는 명시적으로 뺀다), `HeaderDirectories`를 `Fingerprints.TreeFiles(dir, headersOnly:true, …)`로 연 파일(액션 key·InputSnapshot과 같은 열거). 액션 출력·`<out>/BuildInfo`·`OwnedRoots` 아래 파일은 뺀다. MetaCompiler 생성 헤더(`out:Generated/…`)는 넣는다(액션 그래프보다 앞 단계의 출력이므로 입력이다 — identity 명세 ID 재배정과 MetaCompiler 변경이 ID에 보인다). 규칙 원본(`*.Build.cs`)은 해시하지 않는다(규칙의 영향은 모두 액션의 소스 목록·인자·include 순서·라이브러리로 나타나고, Program.cs는 컴파일된 규칙 어셈블리만 갖는다). depfile은 쓰지 않는다.
- **경로 키**(`PortablePaths`): 판정은 `Path.GetRelativePath`(결과가 rooted이거나 `..`로 시작하면 그 루트 밖)로 한다 — `subst` 드라이브 루트(`R:\`), 8.3 짧은 경로, junction에서도 맞게. 가장 긴 루트부터 `out:` → `sdk:` → `src:` 상대 경로(구분자 `/`, 루트 자체는 `src:`), `OwnedRoots` 아래는 `toolchain:<루트 이름>/<상대 경로>`(입력에서는 빠지고 지문과 `toolchain-abi`가 대신한다 — 지문은 SDK·NDK sysroot의 내용을 담지 않는다). 그 밖의 루트 밖 경로는 빌드를 막지 않는다 — include 디렉터리는 `ext<i>:<그 디렉터리 기준 상대 경로>`(i는 그 실행 파일의 외부 include 루트 순번), prebuilt 라이브러리는 `ext:<파일 이름>`과 내용 해시로 쓰고 `host-dependent: yes`로 표시한다. 철자는 열거된 그대로(대소문자 접지 않음) NFC 후 UTF-8, C0·C1 제어 문자(U+0000–001F·U+007F–009F)·U+2028·U+2029·`%`는 UTF-8 byte 단위 percent-encoding(짝 없는 surrogate는 `%uXXXX`), 키를 만들 때 한 번만 인코딩한다(인자 안의 키를 다시 인코딩하지 않는다). NFC 충돌은 원래 byte 순으로 둘 다 남긴다.
- **인자 정규화**: (1) Compile 인자의 `toolchain.CompileArguments` 부분열(위치 = `module.AdditionalCompileArguments.Count`)과 Link의 `LinkArguments` 부분열을 뺀다(macOS `-isysroot <Xcode 경로>` 같은 호스트 경로 — SDK·배포 대상은 `toolchain-abi`가 대신한다). (2) Compile 출력 object는 `obj:<소스 키>`(object 이름이 절대 소스 경로의 해시다). (3) 인자 전체가 절대 경로이거나 `/I`·`-I`·`/WHOLEARCHIVE:` 뒤가 절대 경로면 경로 키로 바꾼다. (4) 정규화 뒤 src·out·sdk 루트 문자열(Windows는 `/`·`\` 두 구분자, 대소문자 무시)이 경로로 남으면 — 바로 뒤가 구분자이거나 끝이고 바로 앞이 시작·`= : , ; " '`·공백 또는 `-I`·`/I`·`-L` 접두 바로 뒤일 때(POSIX의 짧은 루트 `/src`·`/c`가 다른 낱말의 일부로 오탐되지 않게) — InvalidOperationException. ActionGraph가 새 경로 인자 형태를 더하면 시험이 잡는다. 모듈 인자 안의 절대 경로(`/LIBPATH:`·`-L`·`-Wl,-rpath,`·`-fmacro-prefix-map=`·`-iquote`·경로를 담은 define 값)는 `InputValidation`이 거부한다(외부 경로는 선언 입력으로만). `{OUTPUTn}` 자리표시와 구성 인자(`/MDd`·`-O2`·`ROCKS_BUILD_*`·`-flto=thin`)는 남긴다 — ID는 바이너리의 신원이다.
- **내용 해시**(`FileDigests`): 확장자가 `HeaderExtensions` ∪ `.cpp .cc .cxx .c++ .m .mm`이면 `\r\n`만 `\n`으로 바꾼 byte(단독 CR·BOM은 그대로, 스트리밍 버퍼 경계의 CR은 이월), prebuilt `.lib`·`.a`와 그 밖은 원본 byte. 판정은 확장자로만 한다. 한 빌드 안에서 경로별 digest를 메모해 실행 파일끼리 공유하고, mtime·크기 캐시는 두지 않는다(증분이 내용 해시만 믿는 계약과 같다).

**계획 두 단계·주입·증분(`NativeBuildEngine.BuildAsync`·`ActionGraph`)**

- `BuildInfoGenerator.Prepare(request)`를 출력 잠금(`.native-build.lock`) 뒤, `ActionGraph.Create` 앞에서 부른다: 1단계 계획 → 지문 빌드당 1회 → 실행 파일마다 manifest·ID → `AtomicFile.WriteIfChanged`로 `<out>/BuildInfo/<Module>.BuildInfo.cpp`·`<Module>.BuildInfo.txt`(manifest 원문, `sha256(txt) == ID`)·`ToolchainFingerprint.txt` → 해당 모듈 `Sources`에 cpp를 더한 요청. 2단계는 기존 흐름이다. 잠금 아래에서 `<out>/BuildInfo/*.tmp`를 지우고(지우지 못하면 경고만 하고 빌드는 계속한다 — 임시 파일은 입력이 아니다), 이번 실행에 없는 실행 파일의 BuildInfo는 지우지 않는다. 쓰기 순서는 실행 파일마다 cpp → txt, 마지막에 지문 텍스트이며, 중간에 실패해도 각 파일은 통째로 교체되고 다음 Prepare가 같은 내용을 다시 계산해 맞춘다(생성 TU의 key가 cpp 내용이라 낡은 ID가 남지 않는다).
- **안정성**: Prepare는 같은 읽기에서 원본 byte digest(InputSnapshot의 `FileHash`와 같은 값)·줄 끝 정규화 digest·디렉터리 열거 결과를 함께 메모하고, 첫 `InputSnapshot`을 이 메모로 만든다. 첫 InputSnapshot은 2단계 `ActionGraph.Create` **뒤**에 계산하고 메모에 없는 파일(BuildInfo 대상이 아닌 모듈의 입력, ASan 라이브러리 같은 `OwnedRoots` 아래 Link 입력)은 거기서 처음 읽는다 — Create는 파일 내용을 읽지 않으므로 덮는 구간이 같아 정확성에 영향이 없고, 2단계 계획 기준이라 마지막 snapshot과 집합이 확실히 일치한다. InputSnapshot은 처음·마지막 모두 `<out>/BuildInfo` 아래 파일을 뺀다(Prepare만 잠금 아래에서 쓰고 그 내용은 생성 TU의 액션 key가 담는다 — 선언 입력이 출력 루트를 담아도 Prepare의 쓰기가 불안정으로 보이지 않게). 그래서 기존의 마지막 InputSnapshot 비교가 Prepare 읽기부터 빌드 끝까지를 덮고 따로 재검사(`IsCurrent`)를 두지 않는다(finally의 `stable` 계산이 IOException만 잡으므로 새 예외 경로를 만들지 않는다). 입력이 바뀐 빌드는 `ToolBuildIds`를 비운다. Prepare 중 입력을 읽지 못하면 출력을 바꾸지 않고 `InvalidOperationException("BuildInfo: cannot read <key>")`.
- `ActionGraph`는 생성 TU 디렉터리(`<out>/BuildInfo`)를 header 디렉터리 목록에 넣지 않는다(공용 디렉터리의 header 확장자 파일 하나가 모든 실행 파일 TU를 재컴파일하게 만드는 취약성을 없앤다).
- 증분: 생성 TU의 key는 cpp 내용이다(ID가 같으면 적중, 바뀌면 그 TU와 link만). B7 도입 첫 빌드는 실행 파일 모듈의 TU를 한 번 모두 다시 컴파일한다(compile identity가 모듈 소스 목록을 담는다). **Core에 닿는 실행 파일은 입력 byte가 하나만 바뀌어도(주석 포함) 생성 TU와 link가 다시 돈다** — 지금의 "object byte가 같으면 link 생략"보다 일이 늘어나는 동작 변화다.
- `NativeBuildResult += ToolchainFingerprint`(hex, 대상이 없으면 null)·`ToolBuildIds`(모듈 → hex — **link가 성공한 실행 파일만**). action-report.json에 실린다.
- 호출자: Program.cs는 BuildInfo를 직접 다루지 않는다(환경 알림 한 줄과, 빌드 전에 실패 상태 action-report.json을 먼저 써서 예외 경로에서도 이전 PASS 보고서가 남지 않게 하는 것만 더한다). InstalledSdk는 `SdkDirectory`를 넘기고, SDK export는 기존 public include 복사로 `Core/Build/BuildInfo.h`를 담으며 `ManifestVersion`을 3에서 4로 올린다(B7 이전 SDK로 게임을 빌드하면 생성 TU의 include가 깨지는 대신 버전 불일치로 거부한다).
- 증거·잠금: `out`은 SourceSnapshot·GameInputSnapshot에서 빠지므로 생성 파일 쓰기가 sourceStable을 깨지 않는다. 쓰기는 `.native-build.lock`과 상위 `OutputInvocationLock` 안에서만 한다.

**입력 열거 정리(`Fingerprints.TreeFiles`·FoundationTests 규칙)**

- `TreeFiles`의 **headersOnly 열거**(include 디렉터리 inventory)에만 다음을 더 뺀다: 점으로 시작하는 디렉터리(`.claude`·`.vs`·`.idea` — 실측: `.claude/worktrees` 아래 헤더 143개가 FoundationTests 액션 key에 들어가 다른 작업 사본의 편집이 이 빌드를 "inputs changed"로 깨뜨린다), `.git`(파일 또는 디렉터리 — worktree의 `.git`은 파일이다)을 가진 하위 디렉터리(뺄 때 경로마다 한 번 빌드 출력에 알린다), `.native-build.lock`·`.rocks-invocation.lock`을 가진 디렉터리(깊이와 무관한 모든 Rocks 출력). 열거 루트 자신과 out 아래를 직접 선언한 루트(`Generated` 등)는 빼지 않는다. `DeclaredInputs`(headersOnly:false)와 `Fingerprints.Toolchain`의 열거는 바꾸지 않는다. 액션 key도 같은 열거를 쓰므로 한 번 전부 무효화된다(하위호환 없음 원칙).
- FoundationTests의 저장소 루트 include를 없앤다: include 경로를 `Core/Source/Runtime`으로 바꾸고 `<Core/Source/Runtime/…/Private/…>` 철자의 시험 include(12곳)를 `<Compression/Private/…>`처럼 고친다(gitignore된 `/Build/Cache`·`/Intermediate`·`/Saved`·`/Binaries`와 루트 안 사용자 출력 디렉터리가 들어가지 않게). `PublicHeaderChecks`의 루트 private include도 지운다.

**생성 TU(ASCII, LF, 결정적)와 Core 헤더**

```cpp
// Generated by Rocks.Build BuildInfo 1 for module SampleGame. Do not edit.
#include <Core/Build/BuildInfo.h>

static_assert(rocks::Sha256Digest::Bytes == 32);

namespace rocks::build {

Sha256Digest ToolchainFingerprint() noexcept {
    return Sha256Digest{{0x3a, 0x7f, /* 32 byte, 8개씩 4줄, 소문자 0x%02x */}};
}

Sha256Digest ToolBuildId() noexcept {
    return Sha256Digest{{/* 같은 형식 */}};
}

} // namespace rocks::build
```

- 자유 텍스트(컴파일러 줄·경로)는 넣지 않는다 — 원문은 `.txt`에 있다. STL·CRT를 쓰지 않는다.
- `Core/Source/Runtime/Core/Public/Core/Build/BuildInfo.h`: `namespace rocks::build { Sha256Digest ToolchainFingerprint() noexcept; Sha256Digest ToolBuildId() noexcept; }` — 정의는 생성 TU에 있고 정적 라이브러리는 정의를 갖지 않는다(주입되지 않은 링크에서 부르면 링크 오류). 주석에 각 값의 용도(지문 = 에셋 너겟 키, ID = 진단·캐시 무효화 — 패키지에 쓰지 않음)를 적는다.

**시험**

- Rocks.Build.Tests `BuildInfoTests.cs`(그룹 등록): 지문 정규 텍스트 golden, 구성·최적화·언어 조합 동일, 버전·triple·바이너리 해시가 바뀌면 달라짐, MSVC 접미 제거, 자가 빌드 버전 줄의 저장소 부분 제거, 실제 discovery 텍스트에 컴파일러 디렉터리 경로 없음, 환경 위생(`CL=/fp:fast` 등이 지워져 compile 인자에 영향 없음), FP 인자 거부(`-ffast-math`·`/fp:fast`·`-fp:fast`·`-clang:-ffast-math`·`-ffp-contract=fast`·`-fno-math-errno`·`-mllvm`·링크 `-Ofast`, 기존 CL 옵션의 `-` 형태), 실제 빌드(가짜 `Core` — `ProvidesBuildInfo`·`Core/Build/BuildInfo.h`·`Bytes`와 `bytes[32]`를 가진 최소 `Sha256Digest` ← Engine 라이브러리 ← Game 실행 파일 TU 2개, 출력한 digest = C# `Compute`·`sha256(Game.BuildInfo.txt)`·`ToolBuildIds`), 이름이 `Core`지만 표지가 없는 모듈에는 주입하지 않음, 증분(no-op 0 액션·cpp 재작성 없음, Engine 소스 편집은 Engine compile·archive·생성 TU·link만, Core에 닿지 않는 기존 픽스처 액션 수 불변), 줄 끝(소스 CRLF 전환은 ID 불변·생성 TU 미실행, `.a`·`.lib` CR 변경은 `FileDigests` 내부 API로 ID 변경), 호스트 무관(이름이 다른 두 루트(한글·공백)·다른 출력 이름·`subst` 드라이브 루트·8.3 경로·junction에서 ID·cpp byte 동일), 버전 줄 괄호 규칙(공식 LLVM·NDK·자가 빌드 형태), `--no-default-config`가 clang-cl `/WX`에서 통과하고 MetaCompiler meta 단계가 통과, `LINK`·`COMPILER_PATH`도 지워짐과 `RemovedEnvironment` 출력, 수동 툴체인의 빈 신원 필드 거부, 구성(Debug·Release·Asan 지문 같고 ID 다름), 외부 include·prebuilt의 `ext` 키와 `host-dependent: yes`(외부 루트 여럿의 순번·겹친 루트는 가장 긴 것), 점 디렉터리·중첩 작업 사본·`.rocks-invocation.lock` 디렉터리 제외, 실패한 link는 `ToolBuildIds`에 없음. 리뷰 반영으로 더한 것: 허용 목록 환경(목록 밖 무작위 변수는 넘기지 않고 discovery 두 번의 `Fingerprints.Toolchain`이 같고 no-op 0 액션, PATH 변경은 key 불변, `INCLUDE`·`SDKROOT` 변경은 key 변경, 정규 철자), 우회 인자 거부(`-Xclang=`·`--config*`·`-Wl,--mllvm=`·`-Wl,--plugin-opt=`·`-fuse-ld=`·`--ld-path=`·`--target=`·`-target`·`-arch`·`-fms-compatibility-version=`), 오탐 없음(POSIX 대상 `-Fdeps/…`, 링크 `-machine:x64`), 남은 호스트 경로 검사의 경계(POSIX 짧은 루트·Windows 대소문자·드라이브 루트), percent-encoding 한 번(`%` 디렉터리·파일, C1·U+2028/2029·NFC), src·out 안의 소유 루트, 루트 대소문자 차이(`c:\` 대 `C:\`)의 ID 동일, 상대 루트 거부, 소스 없는 실행 파일, 툴체인 인자 부분열 불일치.
- Rocks.Build.IntegrationTests: 게임 소스 편집의 기대 액션 수 2 → 3(Main.cpp·생성 TU·link), no-op 0, `SampleGame.BuildInfo.txt`에 `sdk:` 키가 있고 SDK·프로젝트 절대 경로는 없음, B7 이전 버전 SDK 거부, `dotnet msbuild Rocks.proj /t:Build`(Engine) 연속 두 번의 두 번째 0 액션(실행마다 다른 세션 변수).
- FoundationTests `Core/Source/Runtime/Core/Tests/BuildInfoTests.cpp`: 두 값이 0이 아니고 서로 다르고 호출마다 같음.
- AS 시나리오 13·25의 구성 사이 쿠킹 byte 비교는 쿠커가 생기는 **A4로 미룬다**(Commandlet을 Debug·Release로 빌드해 쿠킹 트리를 byte 비교). B7은 그 전제인 "구성 사이 지문 동일"만 확인한다.
- 문서: `Rocks.Build/README.ko.md`, `Build/README.md`, AS 5.2·5.6·7·10절, ShaderSystem 지문 문구, 11절 상태.

## 9. 엔진 World 타입 리플렉션(B8)

- `Level`·`Actor`·`Component`를 `ROCKS_NATIVE_TYPE` 수동 등록에서 `ROCKS_CLASS(WorldOnly)`로 옮긴다. 컨테이너는 `ObjectRefArray`(actor·component 목록)·`Array`, raw 포인터·캐시는 `ROCKS_NATIVE`로 바꾼다(EngineRuntime 68·80·105행의 수동 등록 이유가 B2·B5로 해소된다). WorldOnly가 상속되므로 Actor 파생에 다시 쓰지 않는다.
- 소비자: AS 6.3절의 World 객체(SoftObjectRef 필드, `AssetLease` 멤버). World 모듈을 다루는 세션과 착수 전에 조율한다.

### 9.1 B8 구현 API(확정안)

9절을 구현하는 구체 형태다. 설계 교차검증(2026-11-14 — 구현 적합성, 반례와 UE 5.6 대조)의 지적을 반영했다. 두 커밋으로 나눈다: **B8a** ObjectRuntime·MetaCompiler에서 리플렉션 base를 둔 native 파생 지원, **B8b** World 이전·시험·문서. B8b는 World·Engine 공개 API(`RegisterWorldTypes`·`EngineConfig`·종료 순서)를 바꾸므로 착수 전에 조율한다 — 2026-11-14에 열린 세션 둘(셰이더·프로세스 API, 에셋 너겟 서버 설계)이 World·Engine 담당이 아니고 진행 중인 변경·이견이 없음을 확인했다(World·Engine을 만든 [Engine] 세션은 열려 있지 않다).

**막는 점(B8a가 푼다)**: `RegisterNativeClass`의 base는 Object·native만 받고, native 등록은 WorldOnly를 달지 않으며(WorldOnly base의 파생에 플래그가 없으면 TypeRegistry가 InvalidArgument), 파생 스키마는 base 필드를 같은 ID로 담아야 하고, `TraceNativeChain`은 손 Trace만 불러 생성 base의 간선이 빠지며(GC 구멍), `NativeTypeIdOf`는 `NativeClassType` 전용이다. World 시험의 native 파생 7종(.cpp 안)은 Metadata 입력이 될 수 없으므로 이것이 필요하다.

**B8a — 리플렉션 base를 둔 native 파생**

- CppEmitter: 클래스 `GeneratedType<T>`에 `static void Trace(const T&, TraceVisitor&) noexcept`(지금 `ops.trace` 람다 본문 — 상속 필드 포함, 본문이 비면 매개변수 이름 없이)를 내고 `ops.trace`는 이것을 감싼다.
- `NativeClass.h`: `ObjectTypeIdOf<T>()` — `NativeClassType`(T 자신의 매크로 멤버로 판정, TU마다 결과가 같다)이면 `T::StaticTypeId`, 아니면 기존 `TypeIdOf<T>()`(안내 메시지 있는 static_assert — `ROCKS_NATIVE_TYPE`을 빠뜨리면 "incomplete type" 대신 안내). `detail::ReflectedAncestor<T>`는 Super 사슬을 올라가 처음 만나는 리플렉션 조상 R(생성 헤더가 없으면 static_assert가 그 include를 안내한다). `RegisterNativeClass`가 리플렉션 base를 받는다: `schema.fields = GeneratedType<R>::Schema().fields`, `ops.fields = GeneratedType<R>::Ops().fields`(R의 thunk는 `Resolve<R>`·`construction.Get<R>`의 IsNativeA 판정이라 파생 객체에 그대로 맞다), `flags |= R의 WorldOnly`, trace는 사슬을 따라가다 R에서 `GeneratedType<R>::Trace`를 부르고 멈춘다(생성 Trace가 상속 필드를 모두 방문하고 ROCKS_CLASS는 손 Trace·NATIVE 강한 참조를 금지하므로 이중 방문·누락이 없다), `completeFieldLayout = false`(파생의 NATIVE 상태를 모르므로 그래프 저장은 Unsupported — GC는 trace만 쓴다), `parallelTraceSafe = false`. 빌린 thunk의 수명은 base 레코드의 dependents가 지킨다. 주석 정정(NativeClass.h 머리·`NativeTypeIdOf`·WorldOnly).
- policy: `ROCKS_NATIVE_TYPE` 클래스 안, 또는 어느 모듈의 Metadata 목록에도 없는 파일의 `ROCKS_PROPERTY`·`ROCKS_NATIVE`를 거부한다(MetaCompiler 없이 attribute로만 남아 추적·저장되지 않는 조용한 GC 구멍을 막는다).
- 시험(Foundation, 기존 `WorldFixture`의 native 파생): 등록·WorldOnly 상속, base 필드 읽기·쓰기·경로 접근, 손 Trace와 생성 Trace 모두 방문(GC 생존), 두 단계 native 사슬(native U : native T : R), 리플렉션 두 단계 상속(native : SceneComponent형 — struct 의존 필드 복사), 저장 Unsupported, 플래그 누락 수동 등록은 InvalidArgument, policy 거부.

**B8b — World 이전**

- 규칙·명세: `World.Build.cs`에 `RequiredModules = ["ObjectRuntime", "CoreTypeBindings"]`, `Metadata = ["World/Level.h", "World/Actor.h", "World/Component.h", "World/SceneComponent.h"]`. ids 도구로 4 타입·7 필드(아래 표)를 명세에 등록한다(기존 수동 TypeId는 어디에도 영속되지 않았으므로 새로 발급해도 되지만, 바꾸지 않으려면 기존 ID를 명세에 그대로 쓴다 — 구현이 택하고 보고).
- 멤버 분류:

| 타입 | PROPERTY(영속, 모두 ReadOnly) | NATIVE(런타임) |
|---|---|---|
| Level | `actors_` `ObjectRefArray<Actor>`(Required, Name="actors") · `name_` String(Name="name") | `world_`(World*) · `persistent_` · `pendingDestroy_` |
| Actor | `components_` `ObjectRefArray<Component>`(Required, Name="components") · `rootComponent_` **`ObjectRef<SceneComponent>`**(Name="rootComponent") · `enabled_` bool(Name="enabled") | `world_` · `level_`(actors 포함 관계에서 유도) · `tickSettings_`·`tickEnabled_` |
| Component | `enabled_` bool(Name="enabled") | `world_` · `owner_` · tick·수명 상태(`state_`·`registered_`·`awake_`·`enabledApplied_`·`startQueued_`·`ticks_[3]`) |
| SceneComponent | `relative_` Transform(CoreTypeBindings 미러 경유, Name="relativeTransform") | `worldMatrix_`(지금 `world_` — `Component::world_`를 가리므로 개명) · `worldDirty_` · `parent_` ObjectHandle · `children_` `Array<ObjectHandle>`(유도값) |

  - **rootComponent는 영속 강한 참조**(UE `AActor::RootComponent`가 `UPROPERTY TObjectPtr`). root는 늘 `components_` 안에 있으므로 새 생존 간선이 생기지 않고 그래프가 다른 객체를 끌어오지 않는다. 지금의 handle 대입(Actor.cpp·World.cpp의 root 설정)을 registry `SetReference`로 바꾸고, 우회 파괴 분기에서도 root를 비운다(좀비 방지).
  - **parent·children은 NATIVE 유지**(UE는 AttachParent를 영속 강한 참조로 두지만 Rocks GC는 파괴된 객체를 가리키는 강한 참조를 null로 만들지 않고 저장소를 남기며 owner 역간선이 없어, 다른 Actor의 Component에 붙은 parent가 좀비를 남기고 그래프로 남의 Component를 끌어온다 — UE는 Outer·레벨 패키지·garbage elimination으로 푼다). 계층 영속은 추적하지 않는 참조 `WeakObjectRef<T>`(12절 열린 문제)가 생긴 뒤 레벨·prefab 저장 전에 한다. children은 UE처럼 Transient 유도값이다.
  - **Tick 설정은 NATIVE**(UE `PrimaryActorTick`은 EditDefaultsOnly로 archetype 차분 직렬화 — Rocks에는 CDO가 없어 인스턴스 저장값이 클래스 기본값 변경을 가린다. CDO가 생기는 A5에서 ROCKS_STRUCT·ROCKS_ENUM으로 옮길 수 있게 `TickSettings`를 단순 값 구조로 둔다).
  - ReadOnly는 경로·리플렉션 쓰기가 World 장부(ActorRecord·hook·MarkDirty·Tick 등록)를 우회하지 않게 한다(로드의 staged 쓰기는 허용). 그래서 CU5 setter hook 전에는 에디터에서 relativeTransform을 경로로 편집할 수 없다(UE의 EditAnywhere와 다름 — 제약으로 적는다).
- 헤더: Level·Actor·Component는 `ROCKS_CLASS(WorldOnly)`, SceneComponent는 `ROCKS_CLASS()`(WorldOnly 상속), `ROCKS_GENERATED_BODY()`, 손 Trace 삭제. 생성 헤더 단독 컴파일 검사(MetaCompiler 검증 TU·Sandbox 공개 헤더 검사)를 위해 Level.h가 `<World/Actor.h>`를, Actor.h가 `<World/Component.h>`를 include한다(역방향 include가 없어 순환이 없다). `NativeTypeIdOf` → `ObjectTypeIdOf`. **World.h는 생성 헤더를 include하지 않는다** — native T의 `SpawnActor<T>`·`AddComponent<T>`는 `T::StaticTypeId`만 쓰고, `GeneratedType<R>`이 필요한 곳(World.cpp, `RegisterNativeClass<T>`, 리플렉션 World 타입을 직접 Spawn·AddComponent하는 TU)은 그 생성 헤더나 집계 헤더 `<World/World.module.generated.h>`를 include한다(World.h 주석에 규약 — Engine.h 소비자 전체로 컴파일 비용이 번지지 않게).
- 등록: `Result<void> RegisterWorldTypes(ModuleRegistration&) noexcept`가 `generated::World::RegisterModuleTypes`를 부른다(CoreTypeBindings struct → base 먼저 클래스, 같은 ID·native 키 재등록은 lease를 붙잡고 성공). Engine이 ModuleRegistration을 소유하고, 등록 순서는 World → `EngineConfig.registerModuleTypes(ModuleRegistration&)`(새 선택 항목, 같은 ModuleRegistration) → `EngineConfig.registerTypes(TypeRegistry&)`(native 타입 — 리플렉션 타입을 base로 둘 수 있으므로 마지막).
- **종료**: Engine은 World 타입을 `Unregister`하지 않는다 — 게임이 토큰을 버린 native 파생이 Actor 레코드의 dependents를 남겨 Unregister가 Busy로 멈추기 때문이다. ObjectRegistry 정리 → ModuleRegistration 파괴(붙잡은 lease 해제) → `TypeRegistry::Shutdown`(lease만 검사, dependent는 순서대로 반납) 순서이고, ModuleRegistration 멤버는 TypeRegistry 멤버 **뒤에** 선언해 먼저 소멸시킨다. 초기화 실패 경로의 Teardown도 같은 순서다. WorldTests·EngineTests의 고정물도 ModuleRegistration 수명을 같은 순서로 관리하고, native 토큰은 그보다 먼저 해제한다.
- **null 순회**: World는 컨테이너에 null을 넣지 않는 불변식을 지키지만, 로드된 배열에는 null이 올 수 있으므로(Required는 로드에서 진단만) `ComponentAt`의 null을 끝 표지로 쓰는 모든 순회(FindComponent·FinishSpawning·DestroyActor·AdvanceComponents)를 개수 기반 + null 건너뛰기로 바꾼다.
- **장부 밖 객체**: `world_`가 null인 Actor·Component(그래프 로드 등 World 밖에서 만든 객체)의 계층·World API는 Fatal 대신 InvalidArgument를 돌려준다. 레벨 편입 계약(레벨 패키지 단계 — ActorRecord 생성, world_·level_·owner_ 연결, parent에서 children 재구성, null 원소 정리, relativeTransform 검증)과 World registry의 외부 생성 거부 정책 자리(A0-O 에셋 Registry 정책의 거울)를 EngineRuntime 문서에 고정한다.
- 생성·소멸 불변(`T(Allocator&)`). GC 간선은 지금과 같다(생성 Trace → `TraceArrayField` → `ObjectRefArray::Trace`, root 추가 방문은 이미 components로 도달하는 객체). 생성 타입은 `parallelTraceSafe = true`(World는 scheduler 없이 Step하므로 지금은 차이 없음).
- 저장·로드: graph v1로 Level 루트를 저장할 수 있다(그래프의 모든 타입이 생성 타입일 때 — native 파생 Actor가 하나라도 있으면 Unsupported. Actor별 저장 제외는 레벨 패키지 단계). **B8의 Level 그래프 저장은 구조 시험용이다** — 계층(parent·children)은 저장되지 않는다(root는 저장된다). 저장 안전 지점은 Tick 밖·PendingDestroy flush 뒤다(flush 전에는 PendingDestroy Actor가 `actors_`에 남아 함께 저장된다). 로드는 같은 TypeRegistry를 쓰는 별도 ObjectRegistry에 하고 로드된 객체는 World 장부 밖이다(편입은 레벨 패키지 단계).
- 경로 접근: B6이 그대로 적용된다(`components[0]`·`rootComponent`·`relativeTransform.translation.x` 읽기, World 필드 쓰기는 ReadOnly로 Unsupported). World의 `Access()` 문맥·게임 스레드 전용.
- fixture 모듈 `Engine/Source/Developer/WorldTestFixtures`(헤더 Metadata, World 의존, 명세, EngineTests가 의존): `SaveActor : Actor`(int32·String·`SoftObjectRef<>` 필드 + `RocksRequiresWorldOnlyOwner` 형질 NATIVE 멤버 — AS 6.3 AssetLease 자리), `SaveComponent : SceneComponent`(형제 `ObjectRef<SaveComponent>`·`Array<int32>`).

**수동으로 남는 것**: `ROCKS_NATIVE_TYPE`(.cpp 시험 타입·영속 상태가 없는 런타임 전용 파생 — 저장 불가), NATIVE handle(parent·children·owner·level), Tick·수명 기록, World(비Object)·Subsystem·TickScheduler.

**시험**: 회귀(WorldTests·TickSchedulerTests·EngineTests — Debug·Release·Asan·Android, Sandbox 빌드), native 파생을 등록한 채 Engine Shutdown·초기화 실패 Teardown 성공, 등록 순서, MetaCompiler(World 생성·WorldOnly 상속·손 Trace 거부·클래스 static Trace·모듈 이름과 클래스 이름 겹침·다른 모듈 base의 상속 ObjectRefArray Trace·RMC2206 통과·생성 헤더 단독 컴파일), Registry(네 타입 WorldOnly·native 파생 7종 등록), GC(Spawn 생존·제거 회수·native 파생 components 생존·Marking 중 AddComponent 장벽·root 참조), 저장(Level 저장 → 별도 Registry 로드 → 필드·간선·root·Soft 확인 → 재저장 byte 동일, NATIVE 기본값, native 파생 있으면 Unsupported, 계층은 비저장임을 확인), null 원소 순회, 장부 밖 객체 API InvalidArgument, 경로(읽기·World 필드 쓰기 Unsupported·fixture 쓰기와 옛 값), 성능(Actor 1만·Component 3만 Collect, Engine 초기화, 컴파일 시간 비교).

## 10. 진단 코드

| 코드 | 판정 | 뜻 |
|---|---|---|
| RMC1001 | MetadataLayout | 생성 헤더의 원본 include 철자, 또는 생성 철자를 소비 모듈의 다른 헤더가 가림 |
| RMC2003 | MetaCompiler | 리플렉션 선언이 이 모듈 입력에도 import에도 없음(헤더·물리 파일까지 대조) |
| RMC2004 | MetaCompiler | import 사이 같은 정규 이름·같은 모듈 두 번·자기 descriptor import·import 타입 재정의 |
| RMC2005 | MetadataLayout | 생성 철자 충돌(대소문자만 다름·예약 `.module.h`·`.generated.h`) |
| RMC2006 | MetaCompiler | 다른 모듈의 private 타입 사용 |
| RMC2101 | ids verify-set | 명세 사이 ID·이름 중복 |
| RMC2102 | MetadataLayout | Metadata 없는 모듈의 활성 항목 명세 |
| RMC2202 | MetaCompiler | 지정자를 그 필드 종류에 쓸 수 없음(Required는 참조·참조 원소 배열만, MaxElements는 배열만 — 값 범위 오류는 RMC2000) |
| RMC2203 | MetaCompiler | 도구 이름(상속·접두 포함) 대소문자 무시 중복·형식 오류 |
| RMC2204 | MetaCompiler | 필드에 PROPERTY와 NATIVE가 함께 있음 |
| RMC2205 | MetaCompiler | NATIVE 멤버가 강한 참조·Object raw 포인터를 (재귀적으로) 담음 |
| RMC2206 | MetaCompiler | WorldOnly 전용 멤버를 WorldOnly 아닌 클래스에 둠 |
| RMC2301 | MetaCompiler | struct 상속 |
| RMC2302 | MetaCompiler | struct 타입 순환 |
| RMC2303 | MetaCompiler | struct 중첩 깊이 초과 |
| RMC2304 | MetaCompiler | 배열 원소로 쓸 수 없는 형식 — `Array<T>`의 ObjectRef·참조 struct(Clang의 `Array` static_assert 진단을 바꿔 보고)·배열, 컨테이너 인자의 배열, `StructRefArray<S>`의 S가 리플렉션 struct가 아님·참조 없음·Registry 컨테이너를 담음(5.6절) |
| RMC2401 | MetaCompiler | enum underlying 미지정·지원하지 않는 폭·값 별칭 |

기존 코드(RMC1000·2000·2001·2100·3000·3100·4000)는 그대로다. RMC2005 중 한 모듈 안의 충돌(`.module.h`·`.generated.h`·대소문자)은 MetaCompiler도 판정한다.

## 11. 단계와 완료 기준

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| B1 | 모듈 소유 메타데이터·descriptor·import·stage 의존 해석·명세 집합 검증·합친 뷰 병합 검사·`ids move`·다중 명세 게시(삭제 포함)·SDK·템플릿·fixture 모듈 | 기존 리플렉션 시험 전부 + 1.9절 통합 시험 |
| B2 | 지정자 문법(정수)·`Version`·`WorldOnly`(상속)·`EditorOnly`·`Required`·`Name`·`MaxElements`·`ROCKS_NATIVE`(재귀 강한 참조 검사·WorldOnly 형질)·`completeFieldLayout` 뜻·`ROCKS_NATIVE_TYPE` | MetaTests 진단 사례, Registry 마스크·유일성·WorldOnly 상속 시험, 버전이 Record에 기록됨 |
| B3 | `AssetId`·`Sha256Digest`·`SoftObjectRef<T>`·`ROCKS_ENUM`(열거자 지정자·constexpr 검증기)·`ValueType`·TypeRegistry 종류·의존 계수·native 대조·`RegisterModuleTypes`·codec thunk와 문맥 전체·두 단계 decode·decode 정책·진단 | 종류별 읽기·쓰기·그래프 왕복, 잘못된 값 거부·정책별 진단, Shutdown이 Busy로 남지 않음, 모든 할당 실패 rollback |
| B4 | `ROCKS_STRUCT`·`RegisterStruct`·참조 struct Trace·중첩 Record·struct 마이그레이션·버전 정책·`Binds` Core 값 타입 | 중첩·참조 struct 왕복, 버전 낮음·높음 처리, Binds static_assert 실패 사례, 깊이·순환 진단 |
| B5 | `Array<T>`·`ObjectRefArray<T>`(null 원소·Insert·SetAt·성장 재진입 정정)·`StructRefArray<S>`·staged `AssignReferences`·`AssignElements`·encode 고정·`ValueArray` wire·RecordList 청구 정정·`MaxElements`·`maxTotalElements` | 5.6절 시험 — 원소 종류별 왕복, 순환 참조 배열 로드, Marking 중 staged 배열, 길이·바이트 상한, 잘림·count 사전 검사, 깊이 비증폭, 재진입 Busy, 모든 할당 실패 |
| B6 | 경로 접근(FieldId 체인)·이름 해석 앞단·옛 값 출력·현행 필드 읽기 결함 정정 | 7.1절 시험 — 경로 읽기·쓰기·배열 연산(삽입·삭제·비우기)·UnknownProperty·실패 시 미게시·할당 콜백 재진입 |
| B7 | 툴체인 지문·도구 빌드 ID 생성·주입·빌드 환경 위생·FP 인자 거부·입력 열거 정리, AS 문서 정정 | 8.1절 시험(구성 사이 쿠킹 byte 비교는 A4) (다른 단계와 독립 — 병렬 가능) |
| B8 | B8a 리플렉션 base를 둔 native 파생, B8b Level·Actor·Component·SceneComponent의 `ROCKS_CLASS(WorldOnly)` 이전(rootComponent 영속, Engine 종료 순서) | 9.1절 시험 — 기존 World 시험 전부 + 리플렉션 Trace·저장·native 파생을 둔 채 Engine 종료 |

상태: B1은 2026-11-13에 구현했다(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 "모듈 메타데이터"·"생성물과 등록"). B1의 `RegisterModuleTypes`는 클래스만 등록하며 B3가 enum·struct와 TypeRegistry 종류를 더한다. B2는 2026-11-13에 구현했다: 정수 지정자 값, `Version`·상속되는 `WorldOnly`, `EditorOnly`·`Required`(참조 필드만, RMC2202)·`Name`(도구 이름, RMC2203)·`MaxElements`(배열 전용 — 배열이 생기기 전까지 RMC2202), `ROCKS_NATIVE`(RMC2204·2205·2206, 생성 static_assert), `ROCKS_NATIVE_TYPE`, `completeFieldLayout`의 새 뜻, TypeRegistry 마스크·WorldOnly 상속·도구 이름 검사. 리플렉션 struct의 `ContainsStrongReference`·`RequiresWorldOnlyOwner` 형질은 struct가 생기는 B4에서 더한다. B3는 세 커밋으로 나눈다 — B3a(`ValueType`·`AssetId`·`Sha256Digest`·`SoftObjectRef<T>`, 3.1–3.3·3.5절의 스키마 표현), B3b(`ROCKS_ENUM`·TypeRegistry 종류·의존 계수·native 대조·enum 먼저 등록), B3c(codec thunk와 문맥·두 단계 decode·decode 정책·진단·`ValidateFieldWrite`). B3a는 2026-11-13에 구현했다: `FieldSchema::value`(`ValueType`)·`element`·`maxElements`(배열 전까지 Opaque·0만 등록), `ValueKind` 끝의 여섯 종류(Enum·Struct·Array는 B3b–B5까지 등록 거부), `FieldValue::digest`·`type`, wire `SoftObjectReference = 12`, 소프트 참조의 기대 타입과 `Required` 허용, 쓰기·저장·로드의 소프트 참조 규칙 검사(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 형식 표). B3a에서 정한 것: `AssetId::TryParse`는 정규 표기(36자 소문자)만 받는다. FieldValue의 String은 UTF-8 불변식을 지키므로 소프트 참조의 UTF-8 검사는 `SoftObjectRef::TryAssign(StringView)`와 Archive 해석에서 걸린다. `SoftObjectRef`는 이동하면 원본이 null이 되고(경로만 비우면 같은 에셋의 대표 객체를 가리키게 된다), 받는 쪽 allocator를 유지하는 복사 `TryAssign(other)`와 allocator를 받는 `TryClone(allocator)`를 둔다. 등록된 쓰기의 오류 순서는 전달 값 종류 → 읽기 전용 → 값 내용(정수 폭·소프트 참조 규칙)이다. 소프트 참조의 기대 타입은 쓰기·저장·로드에서 검사하지 않는다(대상을 찾는 A0-O·A1의 일). descriptor import는 MetaCompiler가 만드는 값 종류만 받고 Required와 종류의 대응도 다시 검사한다(RMC1000). 상속 필드 대조에 `element`·`maxElements`를 포함한다. B3b는 2026-11-13에 구현했다: `ROCKS_ENUM`·`ROCKS_ENUMERATOR`(3.4절 문법·RMC2401·RMC2203), `EnumSchema`·`EnumeratorSchema`와 실행 시 검증기 `IsDeclaredEnumValue`·도구 이름 조회 `FindEnumerator`, TypeRegistry 종류(`TypeKind`)·`RegisterEnum`·`AcquireValueType`·enum 의존 계수·native 키 대조, `ModuleRegistration::RegisterEnumIfAbsent`와 enum 먼저 등록, 생성 `GeneratedType<E>`(constexpr `IsDeclared`), enum 필드 읽기·쓰기·staged 쓰기 검증, 그래프 I64 저장·복원(선언되지 않은 값 거부), descriptor의 enum 항목과 import. B3b 구현에서 정한 것: enum 레코드는 클래스와 같은 목록·ID 공간을 쓰고 `schema.id`·`name`과 `ops.nativeType`·`module`만 채운다. Enum 필드의 enum은 클래스 레코드의 필드별 의존 배열(`fieldTypes`)로 붙잡고 클래스 lease의 `FieldEnum(index)`로 연다(그래프 저장·복원이 이것으로 검증한다). 생성 접근기는 C++ enum의 native 키를 `RuntimeFieldOps::valueNativeType`으로 싣고, enum 등록 정보는 `RuntimeEnumOps{nativeType, module}`이다. Clang은 underlying을 생략한 `enum class`에도 int를 주므로 명시 여부는 원문(이름 뒤 `:`)으로 판정한다. 빈 enum·전방 선언·Flags enum의 음수 값·비ASCII 열거자 이름·4,096개 초과는 RMC2401이다. 클래스 안 중첩 enum도 리플렉션하되 public 구역이어야 한다. 생성 코드는 열거자 값마다 `static_assert`로 AST에서 읽은 값을 대조한다. 같은 C++ enum을 다른 ID로 다시 등록하지 않는다(AlreadyExists — 생성 thunk는 생성 ID 하나만 받는다). 같은 모듈의 public 클래스는 private enum을 필드로 쓸 수 없다(RMC2006). import한 base의 enum 필드는 import enum으로 해석한다. MetaCompiler는 enum을 클래스와 같은 타입 모델(`IsEnum`)로 두어 descriptor·import·inventory 경로를 공유한다. 미선언 값의 decode 정책(`ReportAndDefault`)은 B3c다. B3c는 2026-11-13에 구현했다: `Object/FieldCodec.h`(정책·진단·hooks·frame stack 문맥)와 `Object/GeneratedFieldCodec.h`(생성 codec·`EncodeThroughFieldValue`/`DecodeThroughFieldValue`), `RuntimeFieldOps::encode`/`decode`와 등록 규칙(둘 다 있거나 둘 다 없음, `completeFieldLayout`이면 Transient가 아닌 필드 전부), graph v1의 두 단계 decode(1단계 검증·예산 선청구, 2단계 예산 무접촉), `ObjectGraphSaveOptions`/`ObjectGraphLoadOptions`의 정책·hooks, `RecordReader::Open`의 공유 `ArchiveBudget`, `PropagateFieldFlags`·`EnumReplacementValue`, `detail::ValidateFieldWrite`(사용 계약은 [직렬화](../Implementation/Serialization.ko.md)의 "필드 codec과 문맥"). B3c 구현에서 정한 것: 진단을 보낼 hooks가 없으면 `ReportAndDefault`·`LoadNewerSkippingUnknown`은 InvalidArgument다. 더 새 객체 Record 버전은 migrate하지 않는다. 모르는 필드 `Reject`는 `LoadNewerSkippingUnknown`보다 우선한다. Meta 검사(`Rocks.Build.Cli`의 meta 단계와 ToolTests meta)는 구성의 `ROCKS_BUILD_*` 매크로를 넘긴다(생성 codec이 포함하는 Serialization·Core 헤더를 거쳐 `Base/Compiler.h`가 구성 매크로 정확히 하나를 요구하므로). B4는 세 커밋으로 나눈다 — B4a(런타임: 등록·문맥·struct codec·그래프·Trace), B4b(MetaCompiler), B4c(`CoreTypeBindings`의 Binds). 구현 형태는 4.6절이다. B4a는 2026-11-13에 구현했다: `MaxStructDepth`, `RecordReader::OpenRecordField`·`RecordWriter`(schema version 0은 모든 Open에서 InvalidArgument), `RuntimeStructOps`·`RuntimeStructFieldOps`·`TypeRegistry::RegisterStruct`·`StructView`·`TypeLease::FieldStruct`, 클래스·struct 공용 필드 검증과 Enum·Struct 의존 해석, 문맥의 Record 스택·`OpenRecord`·`OpenMigratedRecord`·`MigrateStruct`·`AddFields`·struct hook, struct 값 codec(`EncodeStructRecord`·`DecodeStructRecord`·`DecodeStructField`·`StructFieldOps<&S::m>`), 그래프의 `migrateStruct`·`maxTotalFields` 1,048,576·Record별 남은 누계·저장 청구 정정(사용 계약은 [직렬화](../Implementation/Serialization.ko.md)의 "리플렉션 struct 값"). 생성 코드(B4b) 전에는 `GeneratedType<S>` 특수화를 같은 모양으로 손으로 쓴 시험이 계약을 검증한다. B4b는 2026-11-13에 구현했다: `ROCKS_STRUCT`(RMC2301·2302·2303, CLASS·STRUCT 마커 배타), struct 형질·깊이와 RMC2206 전파·RMC2006, 생성 `GeneratedType<S>`(형질·`Depth`·Id·Schema·Trace·Ops·Register)와 클래스 struct 필드, descriptor struct 항목과 import(순환·깊이 DFS), `ModuleRegistration::RegisterStructIfAbsent`와 enum → struct → 클래스 집계 등록(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 "리플렉션 struct"). B4b 구현에서 정한 것: 생성 형질과 `Depth`는 MetaCompiler 모델 값이고 직접 멤버 struct의 생성 값을 포함하는지 `static_assert`로 대조한다. `Migrate`는 MetaCompiler가 AST에서 그 이름의 멤버(메서드·함수 템플릿, 접근 무관)를 보면 생성 `Ops()`가 `ops.migrate = &S::Migrate;`로 연결한다 — requires 식 탐지는 friend가 아닌 곳에서 private·오버로드를 조용히 놓치므로 쓰지 않으며, 시그니처·noexcept가 다르면 컴파일 오류, private이면 `ROCKS_GENERATED_BODY`가 필요하다(RMC2000). 생성·등록 순서는 깊이(PROPERTY 멤버만 — RMC2303)가 아니라 NATIVE 리플렉션 struct 멤버까지 센 순위 순이다(생성 static_assert가 NATIVE 멤버 struct의 형질도 쓴다). 클래스 안에 중첩한 리플렉션 struct·클래스는 public이어야 한다(RMC2000). import 필드의 대상 ID가 다른 종류의 타입(Struct 필드 → enum, 참조 → enum·struct)이면 RMC1000, 다른 모듈의 private 타입을 쓰면 private 타입이어도 RMC2006이다. 클래스·참조 struct의 Trace는 모든 struct 멤버의 Trace를 부른다(import flags를 믿지 않는다). import struct의 flags는 필드·멤버 struct와 대조하고(어긋나면 RMC1000) struct 항목 필드의 owner는 그 struct여야 한다. NATIVE 멤버의 직접 타입이 리플렉션 struct면 WorldOnly 아닌 소유자는 `RequiresWorldOnlyOwner`도 생성 `static_assert`로 확인한다. B4c는 2026-11-13에 구현했다: Metadata 모듈 `CoreTypeBindings`(Core/Source/Runtime, 헤더만, Engine SDK 타깃 포함)의 `CoreTypeBindings/CoreValueTypes.h` 미러 15개(Vector2·3·4<float|double>, Quaternion·Rotator<float|double>, Color, LinearColor, IntVec2·3, Transform), Binds 정규 철자(두 번째 Clang 실행 — 별칭 `using rocksBinds_N`·`-ast-dump-filter`), `ClassModel.NativeName`을 쓰는 생성 코드와 실제 타입 대조 static_assert(멤버별 `__is_same(decltype(Real::m), T)`와 `decltype(&Real::m)`의 소유 클래스, 구조적 바인딩 개수·위치별 형식, standard layout과 `ROCKS_OFFSET_OF` 증가 사슬로 선언 순서), 실제 타입 철자 색인으로 필드 형식 해석(이 모듈·import), descriptor `binds`와 import 검사, Core 값 타입 필드의 의존 안내(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 "Core 값 타입"). B4c 구현에서 정한 것: 미러 이름은 `rocks::core_type_bindings::<Name>Binding`이다. Binds 위반 중 다른 모듈의 Binds·같은 실제 타입의 두 미러·리플렉션 타입 결합은 RMC2004, 철자 문법·미러의 NATIVE 멤버·`Migrate`·미러 타입을 필드로 직접 쓰기는 RMC2000, 실제 타입이 완전한 class가 아니면 미러 위치의 RMC3000이다. Binds 철자는 전역 기준 완전 한정 이름으로 해석하고(앞에 `::`가 없으면 붙인다), 정규 철자는 별칭 선언의 desugar 철자에서 cv·elaborated 키워드를 뗀 것이며 descriptor `binds` 문법 밖이면(음수·문자 템플릿 인자, 포인터 인자 등) 생성 모듈에서 RMC2000이다. 실제 타입은 standard layout이어야 한다(순서 대조의 offset 사슬). Engine 타깃이 `CoreTypeBindings`를 포함하므로 Engine 빌드도 그 모듈의 메타데이터 단계를 실행한다. ReflectionTestFixtures는 `CoreTypeBindings`에 의존한다(Core 값 타입 필드 fixture `CoreValueFields`). B5는 세 커밋으로 나눈다 — B5a(Serialization `ValueArray`·`RecordListWriter`·RecordList 청구 정정, Registry 컨테이너 `ObjectRefArray` null 원소·Insert·SetAt·성장 재진입 정정·`AssignReferences`·`StructRefArray`·encode 고정), B5b(레지스트리 배열 필드·문맥·codec·그래프·Trace), B5c(MetaCompiler·fixture·SDK). 구현 형태는 5.6절이다. B5a는 2026-11-13에 구현했다: wire `ValueArray = 13`과 `Serialization/ValueArray.h`(`ValueArrayShape`·`ValueArrayElementShape`·`ValueArrayReader`·`ValueArrayWriter`), `RecordListWriter`·`RecordListReader::OpenItem(index, maxFields)`·`ItemBytes`와 RecordList 청구 정정(`OpenField` 색인 전용, `OpenItem` 언제나 색인만), `ObjectRefArray` null 원소(정규화)·`InsertReferenceAt`·`SetReferenceAt`·live 성장 재진입 정정(현행 Append의 넘침 포함), `ConstructionContext::AssignReferences`, `StructRefArray<S>`(`Object/StructRefArray.h`)와 Registry·생성 트랜잭션 API, SetEdge 원소 모드·`CheckContainerOwnerLocked`, `NativeStrongReference<StructRefArray<S>>`·`detail::RegistryContainer<T>`, `ObjectRegistry::PinForEncode`·`EncodePin`과 GraphSave의 노드별 고정(사용 계약은 [직렬화](../Implementation/Serialization.ko.md)의 "ValueArray"·"RecordList"). B5a 구현에서 정한 것: `ValueArrayReader::Open`은 기대 원소 tag 인자 자체가 원소 tag가 아니거나 shape와 모순이면(고정 폭 wire에 다른 폭, exact인데 wire 최소보다 작은 크기) 먼저 InvalidArgument다(writer도 같은 검사). 가변 wire의 exact shape는 원소마다 길이 prefix를 확인한다(합계만 맞는 Digest 길이 0·64를 거부한다). `ValueArrayWriter`의 maxBytes는 header 8 byte를 포함한 payload 전체이고, Begin은 reader와 같은 maxCount·shape를 받으며, `End`는 원소 byte가 count × 최소 크기보다 작으면(고정 폭·exact는 정확히 같지 않으면) InvalidArgument다(원소 수를 잘못 쓴 호출자 오류). 이동된 `ValueArrayReader`는 원소 0개다. `RecordListWriter::Begin`은 count 자리 4 byte를 포함해 4 + count × 52를 maxPayloadBytes와 비교하고(빈 목록도 builder처럼 4 byte가 필요하다), `EndItem`은 out이 길이 자리보다 짧으면 버퍼를 읽지 않고 InvalidArgument, 항목의 Record envelope(magic·길이 일관성)를 확인하며, 지금까지의 payload가 maxPayloadBytes를 넘으면 그 자리에서 CapacityExceeded다. 단일 참조 쓰기(`SetReference`·staged 포함)도 비정규 null을 `ObjectHandle{}`로 정규화한다(그래프 저장이 비정규 null을 StaleHandle로 거부하므로). `ClearElements`는 떼어 낸 원소를 담으려고 할당하지 않도록 색인째 떼어 반환한다(용량 0). `AddElement`는 게시 시점의 index를 돌려준다. 끝에 넣기는 내부에서 별도 플래그로 넘긴다(공개 index의 어떤 값도 "끝"으로 해석하지 않는다 — index > size는 언제나 InvalidArgument). 컨테이너 owner 검사 순서는 상태 → Borrow → 위치 → encode 고정이다. encode 고정은 `BackendLease`처럼 회수를 막는다(고정을 풀 때 slot을 찾을 수 있게 — 도달하지 않게 된 고정 객체는 retire되고 고정이 풀린 뒤 회수된다). 고정 계수는 `ObjectStats::encodePins`·`ObjectDiagnosticRecord::encodePins`에 보이고 남아 있으면 Shutdown이 Busy다. GraphSave는 노드마다 `AcquireType`보다 먼저 고정한다(생존 판정이 같다). 원소 참조 쓰기 단일 지점 `detail::SetStructMemberReference`의 원소 경로는 문맥의 `StructElement()`가 생기는 B5b에서 잇는다(B5a의 staged 원소 쓰기는 `ConstructionContext::SetElementReference` 두 오버로드다). 등록된 필드 쓰기도 고정 중이면 Busy다: 생성 `WriteGeneratedField`가 쓰기용 Borrow `ObjectRegistry::ResolveForWrite`(고정이면 Busy)를 얻는다. B6 경로 쓰기도 같은 Borrow를 쓴다. B5b는 2026-11-14에 구현했다: MetaSchema `DefaultMaxElements`·`MaxArrayElements`와 대상 형식 함수 `FieldTargetType`, TypeRegistry의 배열 필드 규칙(원소 종류·대상·`maxElements` 범위·FieldKind ⇔ 원소 참조·Required는 참조 원소만·read·write·writeStaged 없음과 codec 필수)·원소 enum·struct 의존·원소 `FieldEnum`·`FieldStruct`·계산 형질(배열 경유 깊이·`containsReferenceArray`·배열 제외 `containsRequired`)·StructRefArray 원소 컨테이너 거부·WorldOnly 원소·`FieldWriteOperation` Struct·Array Unsupported·`ValidateFieldWrite` 원소 모드(`allowNull`과 null 금지 단계 제거)·`ValidateArrayLength`, 문맥의 `ElementIndexScope`·`AddElements`·`ObservationScratch`·`OpenValueArray`·`OpenRecordList`·`OpenListItem`·`StructElement`·`StructElementScope`와 배열 누락 규칙, codec의 `WriteValueWire`·`ReadValueWire`·`ReportParsedValue`·필드 단위 `FieldWireType`·`EnumValueSet`·`detail::ArrayField`·`EncodeArrayField`·`DecodeArrayField`(세 방식, ObjectRefArray null 원소·`AssignReferences` 한 번, struct 원소의 `DecodeStructValue`·`StructRecordSource`, StructRefArray `AssignElements`·`SetElementReference`, `ReserveApply` 선청구)·`TraceArrayField`, 그래프의 `maxTotalElements`·목록 한도 `graph_detail::ArrayLoadLimits`·기대 형식 [시작, 끝) 재사용·도달성 캐시와 기대 lease 해시 표·저장 handle → local ID 해시 표와 노드 lease·기대 lease 캐시·필드 단위 `ValidateLayout`(사용 계약은 [직렬화](../Implementation/Serialization.ko.md)의 "리플렉션 배열 값"). 생성 코드(B5c) 전에는 `GeneratedType` 특수화를 같은 모양으로 손으로 쓴 시험이 계약을 검증한다. B5b 구현에서 정한 것: `WriteValueWire`는 선택 인자 `const EnumValueSet*`를 받아 저장도 enum 원소를 배열마다 한 집합으로 판정한다(원소 수 × 조회 작업을 원소 쓰기 전에 청구 — 원소마다 열거자 수를 청구하지 않는다). `EnumValueSet::Build`는 한 번 훑기 몫 v를, 정렬 사본(열거자 16개 넘는 일반 enum)은 v × BitWidth(v)를 더 청구하고, 선언되지 않은 원소의 대체 값 계산은 조회 하나만큼 더 청구한다. `ObservationScratch`는 공통 가상이라 저장 문맥도 구현한다(단일 할당 한도 없는 예산 할당기 — 저장은 부르지 않는다). 로드 1단계의 `ObservationScratch`는 바이트를 그 문맥의 `Charge`로 청구한다 — 관찰은 1단계에만 있는 일이므로 `DecodeNode`의 reader 작업 Δ 2배 청구에 들어가지 않고 한 번만 센다. 관찰 방식 struct 멤버의 문자열 사본(`FieldValue`)도 `ObservationScratch`로 할당한다(Scratch는 두 단계 몫을 센다). 빈 enum 배열은 `EnumValueSet`을 만들지도 조회 작업을 청구하지도 않는다(저장·로드 두 단계 같은 조건 — `detail::PrepareEnumElements`). 저장에서 필드 `maxElements`를 넘는 배열은 새 진단 종류 `FieldDiagnosticKind::ArrayTooLong`(path = 배열 필드, value = 원소 수)을 보고한 뒤 CapacityExceeded다(5.5절의 "경로가 붙은 CapacityExceeded" — 로드는 필드별 상한을 걸지 않아 내지 않는다). 로드 1단계의 `OpenRecordList`는 항목 수 한도를 min(`MaxArrayElements`, 남은 `maxTotalElements`)로 열어 항목 색인을 할당하기 전에 같은 CapacityExceeded를 낸다(2단계는 `MaxArrayElements`). struct 원소 목록 한도는 저장 codec과 로드가 같은 함수 `detail::ArrayRecordListLimits(값 한도)`로 만든다. `ValidateFieldWrite`는 Struct·Array 종류(원소 모드의 struct 원소 포함)의 통째 쓰기를 종류 대조 뒤 Unsupported로 거부한다(경로 쓰기가 이 함수만 거쳐도 통과하지 않게). struct 원소 배열의 선청구는 원소 메모리 + 원소 수 + 1이고 원소 안 문자열 byte는 멤버 thunk가 청구한다(이중 청구하지 않는다). StructRefArray 원소 메모리는 count × (sizeof(S) + sizeof(void*)), ObjectRefArray는 count × 2 × sizeof(ObjectHandle)(staged 버퍼 + Scratch 사본)다. 관찰 방식(construction 없음·target 있음)이 ObjectRefArray·StructRefArray에 오면(관찰 struct는 참조가 없어 생기지 않는다) 검증으로 다룬다. 저장 쪽은 이미 발견한 노드를 가리키는 참조를 그 노드 lease로 타입 검사하고 lease를 다시 열지 않되, 참조마다 lease 없는 O(1) 생존 조회(`InspectLifecycle`)를 해 b704a6d의 참조마다 `AcquireType`과 같은 판정을 유지한다(무효·재사용 handle StaleHandle, 논리 파괴·cutoff Destroyed). 노드 추가는 노드를 먼저 넣고 handle 맵 삽입이 실패하면 되돌린다(맵의 local ID ⊆ 노드). 1·2단계 문맥의 `OpenRecordList`는 wire를 먼저 확인한다(TypeMismatch — 1단계는 청구 전에). 시험 문맥(FieldCodecContextTests)의 `OpenRecordList`·`OpenListItem`은 Unsupported다(struct 원소 배열은 그래프 시험이 다룬다). 2단계 마이그레이션 커서 어긋남은 2단계에서 원소를 덜 방문하는 계약 위반 thunk로 시험한다(노드 끝에 남은 보관본 InvalidArgument·rollback). 기대 보관 수 시험은 로드의 가장 큰 단일 할당으로 원소마다 기대를 보관하지 않음을 확인한다. B5c는 2026-11-14에 구현했다: MetaCompiler의 배열 필드 해석(desugar 철자의 `rocks::Array`·`ObjectRefArray`·`StructRefArray`를 꺾쇠 깊이로 나누고 원소는 단일 필드와 같은 해석, `ObjectRefArray<>`는 Object, 원소 대상은 기존 `Reference`·`ReferenceId`)과 `FieldModel`의 `ElementKind`·`MaxElements`·파생 속성(`TargetKind`·`StructTarget`·`IsObjectReferenceFieldKind`·`ContainerOf`)으로 판정 지점 일괄 정정, 철자 컨테이너와 파생 컨테이너 대조·중첩 배열·StructRefArray 원소 규칙(RMC2304, 컨테이너 인자의 배열은 RMC2003보다 먼저)·cv 원소(RMC2000), 첫 Clang 실행의 `Array` static_assert 실패를 멤버 위치의 RMC2304(NATIVE는 RMC2205)로 바꾸는 변환, `MaxElements`(기본 65,536·1,048,576 초과 RMC2000·비배열 RMC2202)와 참조 원소 배열의 `Required`, 배열 경유 순환·깊이·순위, 형질 `ContainsReferenceArray`와 원소 struct까지의 RMC2206·2205·2006, 생성 스키마(`element`·`maxElements`)·함수표(원소 Enum·Struct의 native 키)·Trace(모든 배열 필드·멤버의 `detail::TraceArrayField`)·형질 static_assert, descriptor의 `element`·`maxElements`·struct flag `ContainsReferenceArray`와 엄격한 읽기·import 원소 연결, Policy `reflection-container-instantiation`, fixture `ReflectionTestFixtures/ArrayFixture.h`와 생성 그래프 시험(`GeneratedArrayGraphTests`), SDK 게임의 배열 저장·로드(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 "리플렉션 배열"). B5c 구현에서 정한 것: `Array` static_assert 변환은 진단 문구가 아니라 위치와 심각도만 본다 — 첫 error가 Core `Containers/Array.h`에 있을 때 그 error에 딸린 note(다른 진단이 끼기 전) 중 이 모듈 입력 헤더 안의 첫 위치(Clang이 보고하는 필드 이름 위치)를 실패한 실행의 AST에서 이름 위치가 같은 PROPERTY·NATIVE 필드와 맞추고, 같은 AST의 레코드 색인으로 NATIVE 재귀 검사와 같은 규칙을 원소(NATIVE는 필드 형식 전체)에 적용해 강한 참조 경로가 있을 때만 바꾼다(경로를 메시지에 넣는다 — 래퍼 템플릿 원소도 같다). 경로가 없으면(참조 없는 원소의 이동·소멸 noexcept 위반 등) 또는 조회가 어떤 예외로든 실패하면 Clang 원문을 담은 RMC3000이다. PROPERTY·NATIVE가 함께 붙은 필드면 RMC2204, 원소가 맨 위 cv 한정이면 RMC2000이다. cv 원소는 세 컨테이너 모두 RMC2000이고 포인터 대상의 const(`Array<const T*>`)는 지원하지 않는 원소의 RMC2000이다. `StructRefArray<S>`의 S가 enum·클래스·스칼라 등 리플렉션 struct가 아니면 RMC2304다. Registry 컨테이너를 담는 참조 struct 원소는 철자 컨테이너 불일치보다 먼저 그 사유로 RMC2304다(StructRefArray로 바꾸라는 안내가 다음 실행에서 다시 실패하지 않게). 생성 클래스·참조 struct Trace는 `Array<E>`(아무것도 하지 않는다)를 포함해 모든 배열 필드·멤버에 `TraceArrayField`를 부르고, 참조 없는 struct의 Trace는 빈 본문으로 둔다(그 struct에 강한 참조·Registry 컨테이너 멤버가 없음을 멤버 static_assert가 C++ 타입으로 확인한다). 멤버 형질 static_assert는 Binds 미러에서 생략한다 — `VerifyBinds`가 실제 타입의 멤버마다 미러 필드 형식과 `__is_same`으로 대조하고 모델 형질은 그 형식으로만 계산되므로 같은 보장이다. 클래스의 원소 struct static_assert는 상속 import 필드까지 파생 컨테이너로 낸다. 명시적 인스턴스화 Policy는 세 컨테이너와 `ObjectRef`·`SoftObjectRef`를 대상으로, 주석·문자열을 지운 텍스트에서 여러 줄 선언까지 찾으며 STL 검사와 달리 시험·Developer·Templates 소스까지 본다(리플렉션 헤더가 그곳에 있을 수 있다). SDK 시험 타깃 `ReflectionFixtures`는 게임의 그래프 저장·로드를 위해 `ObjectSerialization`을 담는다. 저장소 안 그래프 시험의 상속 배열 필드는 같은 모듈 base(`ArrayChild : ArrayOwner`)이고, 다른 모듈 base의 배열 필드(생성 Trace·codec)는 MetaCompiler 세 모듈 시험과 SDK 게임(GameCore에서 상속한 배열 필드의 저장·로드·수집)이 덮는다. B6는 두 커밋으로 나눈다 — B6a(런타임·문자열 앞단·손으로 쓴 특수화 시험·현행 결함 정정), B6b(MetaCompiler 생성 path thunk·fixture·생성 시험). 구현 형태는 7.1절이다. B6a는 2026-11-14에 구현했다: MetaSchema `MaxPropertyPathSteps`·`PropertyPath`·정규형 검사 `IsCanonicalPathStep`·경로 동등성 `SamePropertyPath`와 공용 `EffectiveToolName(FieldSchema)`·`EqualsIgnoringAsciiCase`(`detail::EnumNameEquals`와 구현 공유 — TypeRegistry.cpp 익명 정의 삭제), 문자열 앞단 `Object/PropertyPath.h`(`PropertyPathError`·`PropertyPathFailure`·`PropertyPathBuffer`·`ParsePropertyPath`·`FormatPropertyPath`), 경로 연산 `PathRead`·`PathWrite`(previous)·`PathArrayLength`·`PathInsertElement`·`PathRemoveElement`(removed)·`PathClearArray`, `RuntimeFieldOps::path`·`RuntimeStructFieldOps::path`와 `StructFieldOps`의 둘째 인자, 공개 detail 헤더 `Object/PathCursor.h`(`detail::PathCursor`·`PathFrame`·`PathOperation`, 구현 `Private/PropertyPath.cpp`), 생성 템플릿 `Object/GeneratedPathAccess.h`(`PathClassField`·`PathStructMember`·`CopyTargetValue`·`WriteTargetValue`·`SetPathMemberReference`), `StructMemberPointer`의 `GeneratedFieldAccess.h` 이전, `TypeLease::FieldOps(schemaIndex)`·`FieldIndex(Guid)`와 클래스 레코드 색인, 클래스 단일 값 필드의 path 금지(등록 InvalidArgument), live 원소 위치 `ObjectRegistry::SetElementReference(access, owner, const detail::StructElementLocation&, ObjectRef<T>&, target)`(private, `detail::SetPathMemberReference`만 friend), `detail::ValidateArrayEdit`·`ArrayEdit`, Core 공개 `ArrayGrowthCapacity`, 비정규 null 정규화(`FieldWriteOperation`·`ValidateFieldWrite`·callback 사본 — `ReferenceSchemaReviewTests` 기대 갱신), `ReadGeneratedField`의 encode 고정과 최상위 생성 String·Soft write의 할당 중 고정(현행 결함 정정 — 할당 콜백의 같은 필드 쓰기가 Busy)과 회귀 시험(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 "경로 접근"). B6a 구현에서 정한 것: `Array<E>` 삽입의 게시 직전 소유자 재확인은 임시 배열 예약 뒤·원소 이동 전에 한다(7.1절 서술의 "교체 → 재확인"과 순서가 다르다 — 원소를 옮긴 뒤 Destroyed를 알면 live에 이동된 원소가 남으므로, 이동과 교체는 무할당이라 그 사이 재진입이 없다). 고정은 할당하는 끝 지점에서 cursor가 쥐고(삭제의 removed는 다른 allocator로 복사할 때만), 게시 또는 실패 기록 직후 곧바로 풀며(cursor의 실패 기록이 고정을 푼다) 옛 값·옛 버퍼와 실패한 연산의 임시 버퍼(새 값·옛 값 사본·새 원소·새 배열 버퍼)는 그 뒤에 반환한다. 최상위 String·Soft 필드의 생성 write(`WriteGeneratedField`)도 ResolveForWrite 뒤 새 값 할당부터 Swap까지 같은 고정을 쥔다(현행 결함 정정 — 할당 콜백의 재진입 생성 write가 성공하고 바깥 Swap이 덮어써 갱신이 유실되던 것) — 수동 함수표의 write callback에는 이 고정이 없다. 최상위 단일 값 쓰기도 참조 lease·`ValidateFieldWrite`(5·6단계)를 previous read·할당보다 먼저 거치고(오류 순서상 ValidateFieldWrite가 OutOfMemory보다 먼저 — 등록된 write는 같은 검증을 다시 한다), previous read는 쓰기용 Borrow로 고정 중이면 read 전에 Busy를 판정한 뒤 고정 아래 등록된 read로 얻고 고정을 푼 뒤 write를 부른다. 실패 사유: `ValidateFieldWrite`의 값 종류·enum ID 불일치(TypeMismatch)와 값 내용(InvalidArgument)은 모두 `InvalidValue`, Opaque 값·단일 값 원소 삽입의 값 누락도 `InvalidValue`, Unsupported는 유효 플래그에 ReadOnly가 있을 때만 `ReadOnly`(수동 callback의 그 밖 Unsupported는 `None`), struct 원소 제거의 removed는 InvalidArgument + `AggregateValue`, 단계 0개·정규형 위반은 InvalidArgument + `None`, Opaque 필드로 끝나는 읽기·쓰기와 previous를 요청했는데 최상위 read가 없으면 `NoPathAccess`, `WriteTargetValue`의 정적 enum ID 불일치는 `StaticTypeMismatch`, Registry API(대상 검사·Busy 등)와 문맥·수명·Borrow·게시 직전 재확인 오류는 `None`이다. 연산의 failure.step은 문맥·수명(AcquireType)과 Borrow 획득 오류면 0, 17단계 초과면 17이고, 고정·게시 직전 재확인·Registry 호출 오류는 그 연산을 하던 단계다. thunk 유무는 Borrow 전에 모든 단계를 미리 확인하고(Unsupported가 Busy보다 먼저), 정적 대조는 Borrow 뒤 thunk 안에서 index보다 먼저 한다(오류 순서: Borrow·고정 → 정적 대조 → index → 길이 → 정수 폭). struct 원소 배열(`Array<S>`·StructRefArray)은 연산과 무관하게(길이·비우기·삭제 포함) 그 단계 맨 앞에서 원소 struct를 대조한다. 클래스 thunk는 실제 객체 타입이 Owner의 파생인지(`IsNativeA`), struct 멤버 thunk는 받은 struct 값의 등록 타입이 S인지도 대조한다. Registry 컨테이너 끝 지점도 Registry를 부르기 전에 index(`IndexOutOfRange`)와 MaxElements(`TooManyElements`)를 먼저 본다. `PathArrayLength`는 할당하지 않으므로 고정 없이 Resolve만 한다. 원소 위치로 고르는 live `SetElementReference`는 위조한 위치를 공개 API로 받지 않도록 ObjectRegistry의 private이고 `detail::SetPathMemberReference`만 friend다. previous·removed의 참조 handle은 약한 참조다 — 간선을 지운 뒤에는 대상을 살리는 것이 없으므로 CU4가 RootRef로 붙잡기 전까지(이 연산의 해제 콜백 안 GC 포함) 옛 대상이 회수될 수 있다. 단일 값 변환 규칙은 `CopyTargetValue`·`WriteTargetValue` 한 벌이다(`Object/GeneratedFieldAccess.h` — 기존 `CopyFieldValue`·`WriteScalarField`가 `FieldValueKind<Member>()`로 위임한다). 문자열 앞단: `TooDeep`은 17단계 뒤의 `.`에서 NotStruct보다 먼저 판정하고(등록 스키마는 그보다 깊을 수 없어 NotStruct에 가려지므로), 끝 `.`은 struct 뒤면 이름이 없어 Syntax, struct가 아닌 값 뒤면 NotStruct이며, 문법 오류의 step은 해석 중이던 단계(index 안의 오류는 그 단계, 단계 사이의 오류는 다음 단계)다. 실패하면 out은 바뀌지 않는다. Format은 Opaque 필드 단계를 Unsupported로 거부한다. `TypeLease::FieldOps`는 클래스 lease 전용(그 밖 Fatal), `FieldIndex`는 클래스·struct lease(enum은 NoField)다. `ValidateArrayEdit`는 알 수 없는 edit 값을 InvalidArgument로 거부한다. 이동된 원소의 소멸자가 allocator를 부르지 않는다는 계약은 `MakeRuntimeStructOps` 주석과 리플렉션 메타데이터 문서에 적었다. B7은 2026-11-14에 구현했다: `NativeToolchain`의 `CompilerVersion`·`TargetTriple`·`CompilerDigest`·`LinkerDigest`·`OwnedRoots`·`AbiFields`·`RemovedEnvironment`와 discovery probe(Windows·macOS·Android), `NativeEnvironment`의 허용 목록 빌드 환경(`CreateReportedAsync`·`Restrict`, Keyed·Runtime 목록)과 Keyed 변수만 담는 `Fingerprints.Toolchain`, 받는 컴파일러에만 넣는 접두 없는 `--no-default-config`(드라이버 CompileArguments·LinkArguments·probe, MetaCompiler 허용 목록), `FloatingPointArguments.IsModelArgument`와 `InputValidation`의 FP 모델·LLVM·링커·config·대상 선택·CL 철자 표(Windows 대상만)·절대 경로 거부와 절대 루트 요구, `ToolchainFingerprint`(`CanonicalText`·`Compute`), `Rocks.Build/BuildInfo.cs`의 `BuildInfoGenerator`·`PortablePaths`·`FileDigests`, `NativeBuildEngine`의 두 단계 계획·메모로 만든 첫 InputSnapshot·`NativeBuildResult.ToolchainFingerprint`·`ToolBuildIds`, `NativePlan.Closures`, `NativeBuildRequest.SdkDirectory`, `NativeModuleInput.ProvidesBuildInfo`(Core 규칙, SDK manifest 버전 4), `TreeFiles` headersOnly 열거 제외, FoundationTests·PublicHeaderChecks의 저장소 루트 include 제거, `Core/Build/BuildInfo.h`(사용 계약은 [Rocks.Build](../../Programs/Rocks.Build/README.ko.md)의 "빌드 환경 위생"·"빌드 주입 상수(BuildInfo)"). 구성 사이 쿠킹 byte 비교는 A4로 미뤘고 B7은 구성 사이 지문 동일만 확인한다. B7 구현에서 정한 것: 빌드 환경은 차단 목록이 아니라 허용 목록이다(8.1절 빌드 환경 위생 — 실행마다 바뀌는 `DOTNET_CLI_TELEMETRY_SESSIONID` 때문에 `dotnet msbuild` 경유 빌드가 매번 전체 재빌드되던 기존 결함을 함께 고쳤다). Runtime 목록에는 같은 환경으로 실행하는 빌드 도구를 위해 `ROCKS_CLANG_FORMAT`(양쪽)과 `APPDATA`(Windows)를 더했다. `--no-default-config`는 discovery가 `--no-default-config --version`으로 받는지 확인한 컴파일러에만 넣고(받지 않으면 인자 없이 `--version`을 다시 보고 `Configuration file:` 줄이 있을 때만 오류), NativeEnvironment의 Windows resource-dir probe도 같은 조건이며 실패하면 오류다. MetaCompiler는 그 인자를 고정 인자에 넣지 않고 네이티브 프로필(`--clang-arg`)이 실어 올 때만 쓴다. `AbiIdentity`는 형식 이름 `rocks-native-abi-v2`로 첫 줄 대신 정규화한 `CompilerVersion`(자가 빌드 경로 제거), `InstalledDir:`·`Target:` 줄 제외, `target=<MSVC 접미를 지운 effective triple>`를 담는다. `AbiFields` 키는 `compiler=`, Windows `VCToolsVersion=`·`WindowsSDKVersion=`, macOS `sdk-version=`·`sdk-build-version=`·`deployment=`, Android `ndk=`(API 수준은 triple에 있다)이며 `;`·줄바꿈을 담으면 BuildInfo가 ArgumentException이다. macOS `LinkerDigest`는 SHA-256(UTF-8(`<ld hex>\n<libLTO hex>\n`))이고, 컴파일러가 xcrun shim(`/usr/bin`의 clang·clang++·cc·c++)일 때만 `xcrun --find clang++`를 쓰며 ld는 컴파일러 옆에 있으면 그것이다. `OwnedRoots` 이름은 `resource`(Windows·macOS)·`runtime`(Windows ASan DLL 디렉터리)·`sysroot`(macOS SDK)·`ndk`(Android 루트)이고 ASCII 영숫자·`-`·`_`만 받으며, src·out·sdk 안에 있어도 되지만 그 루트를 포함하면 거부한다. 경로 키의 percent-encoding은 C0·C1 제어 문자·U+2028·U+2029·`%`를 UTF-8 byte 단위로, 짝 없는 surrogate를 `%uXXXX`로 바꾸고 키를 만들 때 한 번만 한다. 남은 호스트 경로 검사는 루트 문자열이 경로로 나타날 때만(뒤가 구분자·끝, 앞이 시작·`= : , ; " '`·공백·`-I`/`/I`/`-L` 접두 뒤) 걸린다. 외부 include 순번(`ext<i>`)은 도달 집합 순서(실행 파일, 그다음 의존 순서)와 Compile 액션 header 디렉터리 순서의 첫 등장 순이고, 루트 밖의 include 아닌 파일(prebuilt·선언 입력·소스)은 `ext:<파일 이름>`이다. 같은 키의 입력은 경로 단위로 중복을 없애고 원래 경로의 UTF-8 byte 순으로 둘 다 남긴다. 첫 InputSnapshot은 Prepare 메모로 만들되 2단계 `ActionGraph.Create` 뒤에 계산하고, InputSnapshot은 처음·마지막 모두 `<out>/BuildInfo` 아래 파일을 뺀다. 입력이 바뀐 빌드는 `ToolBuildIds`를 비운다. 모듈 인자 검사는 CL 출력 옵션 `/Fa`·`/Fi`·`/Fp`, 경고 수준 `/W1`–`/W3`, `/out:`·`/pdb:`·`/implib:`의 `-` 형태도 거부하고(Windows 대상만), clang-cl의 `/clang:`·`-clang:` 안 인자는 link 인자에서도 GCC형 규칙으로 본다. `-m*` 허용 목록은 컴파일러 인자에만, FP 모델 판정은 이름 뒤 `:`가 먼저 오는 링커 철자(`-machine:x64`)를 제외한다. 절대 경로 판정은 Windows 드라이브·UNC는 어느 대상에서나, `/`로 시작하는 조각은 POSIX 대상에서만이다. 설치 SDK 내보내기는 `ProvidesBuildInfo` 모듈의 public include에 `Core/Build/BuildInfo.h`가 없으면 거부하고, SDK manifest 버전 불일치는 별도 진단(`SDK manifest version mismatch`)이다. 환경 위생 알림 한 줄은 `NativeToolchains.EnvironmentNotice`가 만들고 `build`·`test`·`sdk`·`game` 명령이 discovery 직후 출력한다. Program.cs는 빌드 전에 실패 상태 action-report.json을 먼저 써 예외 경로에서도 이전 PASS 보고서가 남지 않게 한다(게임 빌드와 같은 `WritePendingReport`). 중첩 작업 사본을 include inventory에서 뺄 때 경로마다 한 번 빌드 출력에 알리고, 생성 파일 임시본(`*.tmp`)을 지우지 못하면 경고만 한다. 생성 TU의 digest 배열은 `Sha256Digest{{` 다음 줄부터 8개씩 4줄(8칸 들여쓰기)이다. B1 구현에서 정한 것: 메타데이터 배치·검증은 타깃에서 도달하는 모듈에만 적용한다. `ModuleRegistration`은 이미 등록된 공유 타입의 lease를 붙잡는다. `.generated.h` 철자를 예약한다(RMC2005). 명세 사이 이름도 유일해야 한다(1.6절).

의존: B1 → B3(집계 헤더·import), B2 → B3(Required·EditorOnly 검증), B3 → B4 → B5(struct 원소 배열), B3·B5 → B6(경로는 struct·배열을 지난다), B2·B5 → B8. B7은 독립이다. 각 단계는 구현 뒤 다수 서브에이전트 교차검증, Debug·Release·Asan·format·policy·Android 빌드를 거쳐 커밋한다.

## 12. 결정 기록·열린 문제·다른 문서 정정

결정:

1. 참조 struct의 목록은 `Array<T>` 원소가 아니라 B5의 Registry 관리 컨테이너 `StructRefArray<S>`로 지원한다(4.3·5.6절). 처음에는 금지였으나 2026-11-13 사용자 결정으로 바꿨다(UE `StaticMaterials`·머티리얼 파라미터 배열 대응). Registry 간선 모델은 컨테이너 기준 범위 검사로 넓힌다.
2. 단일 값 배열은 `ValueArray`로 압축한다(5.4절) — 원소 한도와 바이트 한도를 함께 만족한다.
3. 플래그 enum을 지원한다(3.4절) — 미선언 값 거부를 "선언 값들의 OR"로 정의한다.
4. struct 상속은 v1에서 금지(4.1절). DataTable 행 같은 base struct 요구가 생기면 base 필드를 펼치는 방식으로 연다.
5. Core 수학 타입은 `Binds` 미러 선언으로 리플렉션한다(4.5절) — Core의 의존 방향을 지킨다.
6. 필드 codec은 생성 thunk의 두 단계 파싱이며 1단계 산출물을 보관하고 2단계 작업량을 1단계에서 청구한다(6.2절).
7. 툴체인 지문에 빌드 구성을 넣지 않는다(8절). B7은 구성 사이 지문 동일을 시험하고, 구성 사이 쿠킹 byte 비교 시험은 쿠커가 생기는 A4에서 한다.
8. WorldOnly는 상속되는 플래그다(2.2절). `Required`는 저장·쿠킹에서만 거부한다(2.3절).
9. 미선언 enum 값·높은 struct 버전은 기본 거부이고, 정책으로 보고 후 계속을 고를 수 있다(6.1절) — 새 빌드가 쓴 데이터를 손상으로 오진하지 않는다.
10. struct 값은 용량보다 유연성을 택해 Binds Core 값 타입까지 모두 태그형 중첩 Record로 저장한다(4.4절, 2026-11-13 사용자 결정 — 고정 배치 없음).
11. struct 마이그레이션은 타입이 등록하고(`RuntimeStructOps.migrate`, UE의 타입별 버전 처리 대응) 로드 옵션은 override다(4.4·4.6절).
12. `ObjectRefArray` 원소는 null을 허용한다(5.2절, 2026-11-13 사용자 결정 — UE `TArray<TObjectPtr>`와 같고 슬롯 배열을 표현한다). Required는 원소마다 non-null이다.
13. 로드는 필드별 `MaxElements`를 걸지 않고 `MaxArrayElements`와 그래프 한도로만 막는다(5.5절) — 상한을 낮춰도 기존 에셋이 열리고 넘친 데이터는 다시 저장할 때 드러난다. MaxElements 변경에 Version을 올릴 필요가 없다.
14. 정수 배열 원소의 wire 폭은 단일 값과 같다(5.4절 — UInt8·UInt16도 U32). UE는 native 폭으로 쓰지만, 결정 10처럼 용량보다 정수 폭 확장의 wire 호환을 택한다. 대용량 byte·수치 데이터는 bulk data(A1)다.
15. `StructRefArray`의 원소 struct는 `ObjectRefArray`·`StructRefArray`를 담지 않는다(B5, RMC2304). 허용하려면 원소 블록 header에 (컨테이너, 소유 블록)을 두고 Registry가 소유 사슬을 검증하며 다단 경로·staged API가 필요하다. UE에는 실례가 있으므로(`ULandscapeGrassType::GrassVarieties`의 `FGrassVariety` 안 `TArray<TObjectPtr>`, 머티리얼·메시) 랜드스케이프·폴리지·World(B8) 소비자가 요구할 때 연다(권고안으로 진행, 2026-11-13).
16. struct 원소 배열도 결정 10의 태그형 항목 Record다. `Array<rocks::Vec3>`는 원소당 약 168 B라 필드 하나가 기본 `maxValueBytes`(4 MiB)에 약 2.5만 개다 — UE는 inner struct 타입을 배열당 한 번 쓰고 native Serialize struct는 원소당 24 B다(의도적 차이). 정점 같은 대량 수치는 bulk data(A1), 한도 확장(64 MiB)은 A0-O graph v2가 맡는다(권고안으로 진행, 2026-11-13).
17. 도구 빌드 ID는 패키지 Summary(에디터·쿠킹 모두)에 넣지 않는다(8.1절). ID는 소스 편집·구성·호스트 도구 버전으로 바뀌므로 Summary에 있으면 쿠킹 byte의 구성 사이 동일(AS 시나리오 13·25)이 깨지고, 쿠킹 패키지 이름(내용 주소)이 엔진 빌드마다 바뀌며, 에디터 패키지는 다시 저장할 때마다 VCS 변경과 재쿠킹을 낳는다(Rockpile도 같은 이유로 Record 밖에 둔다). 쿠킹 보고서와 AssetIndex 캐시 header에 진단용으로만 남기고, AssetIndex 캐시 키는 형식·Unicode·스키마 버전으로 한다(2026-11-14, 기존 결정성 요구에 맞춘 정리).
18. 툴체인 지문의 대상은 effective triple(MSVC 환경 버전 접미 제거), 컴파일러 식별은 버전 줄 허용 목록 + 컴파일러·링커 바이너리 해시, 빌드 환경은 컴파일러 동작을 바꾸는 환경 변수·기본 config 파일을 지운다(8.1절).
19. 도구 빌드 ID의 "규칙" 입력은 규칙이 만든 액션으로 대신하고, MetaCompiler 생성 헤더는 입력에 넣는다(8.1절).
20. B6 경로 접근(7.1절): 경로는 객체 하나 안에서 끝나고, struct·배열 통째 접근은 B6 밖이며, 최상위 단일 값은 등록된 read·write 한 경로다. 모르는 이름은 ErrorCode를 늘리지 않고 NotFound + `PropertyPathFailure` 사유로 알린다. 할당 중 재진입은 주소 재계산이 아니라 encode 고정으로 막고(cursor가 직접 할당하는 끝 지점만), 고정이 막지 못하는 소유자 파괴는 게시 직전 생존 재확인으로 막는다. 쓰기·제거는 CU4 Undo가 쓸 옛 값을 돌려주고, 하위 경로 쓰기는 앞으로 `steps[0]` 필드의 setter·통지 hook을 거친다(CU5·UD7). B6은 A6까지 미루지 않고 A0-B 순서대로 지금 구현한다(권고안으로 진행, 2026-11-14).
21. B8의 World 타입 이전 — rootComponent는 영속 강한 참조(UE와 같음), 부착 계층(parent·children)과 Tick 설정은 NATIVE(계층은 WeakObjectRef, Tick은 CDO 뒤), World 필드는 ReadOnly(CU5 setter 전), Engine은 World 타입을 Unregister하지 않고 TypeRegistry::Shutdown에 맡긴다, 리플렉션 base를 둔 native 파생을 지원한다(저장 불가). 에셋 Registry의 WorldOnly 거부는 A0-O(권고안으로 진행, 2026-11-14).

열린 문제(이 단계 밖에서 정한다):

- 계약 설정·필드 기본값 조회(클래스 기본 객체) — 텍스처 설정(A5).
- 배열 원소 안정 ID(prefab override, EIP 1702행·Sparks 1119행) — prefab 단계.
- 객체 단위 EditorOnly(Exports 표 플래그, AS 5.2절 Exports)와 Dependencies 표 로드 대조 키(AS 5.2절 Dependencies): 6.1절 수집기·유효 플래그가 담는 정보로 표현되도록 A1에서 정한다(코드의 `Required`·EditorOnly·기대 타입 변경만으로 정상 파일이 `CorruptPackage`가 되지 않는 범위).
- `TMap`·`TSet` 대응(Sparks 287·482행, UI CU4 map key, 계약 설정의 플랫폼별 값): 결정적 순서 규칙과 함께 후속.
- 텍스트(FText 대응)·클래스 참조(`TSoftClassPtr`·`TSubclassOf` 대응 — RuntimeClassMetadata)·Sparks `ScriptName`과 `Name`의 관계 — 후속.
- 버전 강제(영속 배치 지문 비교) — 후속 후보.
- 고정 배치 Binds struct의 `ValueArray` 원소 형식(원소 tag 예약 — 스플라인 점·커브 키·인스턴스 변환이 bulk로 가기 전 4 MiB에 걸리는지 소비자의 예상 원소 수로 판단, 결정 16).
- UE에 있고 B5에 없는 배열 기능: 정적 배열(`ArrayDim` — `T x[N]`은 지금 RMC2000), `EditFixedSize`·`ArrayClamp`(UI CU1), WeakRef·클래스 참조·FName·FText 원소(단일 값 종류부터), Instanced 하위 객체 배열(A0-O·A1), overridable 배열 직렬화(원소 안정 ID와 같은 축).

다른 문서 정정(이 설계를 반영할 때 함께):

- AS 1절 A0-B 행·리플렉션 한계 행·5.6절(이 문서로 연결, `-fno-math-errno`·llvm-nm 서술 정정)·14절 시나리오 13·25(구성 사이 쿠킹 byte 비교): 이 초안과 함께 반영했다.
- `ReflectionMetadata.ko.md`: 타깃 소유 서술은 B1에서 모듈 소유로 바꿨다. 버전 비노출·모든 비정적 필드 리플렉션 필수 서술은 B2 결과로 바꾼다.
- `SparksLanguage.ko.md`의 `Soft<T>`(AssetId만)·`UiToolkit.ko.md`의 `AssetRef<T>` → `SoftObjectRef<T>`(AssetId + 객체 경로). `UiToolkit.ko.md`의 `Sides<float>`(비템플릿 구체 타입으로)·지정자 확장 문법(CU1이 2.1절 표를 넓힌다).
- `EngineImplementationPlan.ko.md` 7.1절 예시 매크로(`ROCKS_REFLECT`·`ROCKS_FIELD(FieldId=…)`)와 `GcArray` 용어 → 현재 문법·`ObjectRefArray`.
- `EngineRuntime.ko.md` 68·80·105행: `ROCKS_NATIVE_CLASS` → `ROCKS_NATIVE_TYPE`, B8 이전 계획.
