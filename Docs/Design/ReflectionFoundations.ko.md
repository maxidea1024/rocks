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
    { "kind": "struct", "containsStrongReference": false, … },
    { "kind": "enum", "underlying": "UInt8", "flags": ["Flags"], "toolPrefix": "TC_",
      "values": [ { "name": "Default", "value": 0, "toolName": "Default", "displayName": "", "flags": ["Hidden"] } ] } ] }
```

- 클래스 `fields`는 상속 포함 전체(정렬 순서 그대로)이고 각 필드에 선언 owner를 적는다. 생성 thunk는 `&::<owner>::<name>`을 쓰며 private 기반 필드는 owner의 `ROCKS_GENERATED_BODY`(모든 `GeneratedType` 특수화에 friend)로 접근한다 — 모듈 경계와 무관하다.
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
| `Binds="…"` | STRUCT | 거울 선언이 결합하는 실제 타입 철자(4.5절) |

버전 규칙: 버전은 그 타입 Record의 해석 규칙이다. 영속 필드의 의미·형식이 바뀌면 올리고 마이그레이션을 등록한다. base의 버전은 파생 Record에 기록되지 않으므로 base의 영속 배치를 바꾸면 파생 타입들도 버전을 올린다(MetaCompiler는 강제하지 않는다 — 배치 지문 비교는 후속 후보). 필드 추가만 한 경우의 누락 허용은 패키지 쪽 정책(`allowMissingFields`, A1)이 정한다.

### 2.3 필드 지정자

| 지정자 | 플래그 | 의미·검증 |
|---|---|---|
| `Transient`·`ReadOnly`·`Category`·`DisplayName` | 1·2·— | 지금과 같다 |
| `EditorOnly` | 4 | 항상 컴파일되고 플래그만 단다. graph v2(A0-O)가 쿠킹 데이터에서 뺀다. 중첩 struct·배열 안의 값에도 효력이 전파된다(6.1절 유효 플래그). 쿠킹 데이터 로드에서 EditorOnly 필드 누락은 `NotFound`가 아니다(A0-O). A0-B의 graph v1은 보통 필드처럼 저장한다 |
| `Required` | 8 | ObjectRef·SoftObjectRef 필드(와 그 배열)에만 쓸 수 있다(그 밖은 RMC2202). **패키지 저장·쿠킹에서** null이면 거부한다 — graph 저장 옵션 `rejectMissingRequired`(패키지 저장·쿠킹이 켠다, 스냅샷·복제 같은 다른 저장은 끈다)이 켜져 있으면 InvalidArgument, Soft 대상의 존재 확인은 패키지 저장·쿠킹(A1). **로드는 거부하지 않고** 진단(`RequiredMissing`)만 낸다 — 기존 필드에 `Required`를 붙여도 null을 가진 에셋을 열어 고칠 수 있어야 한다 |
| `Name="…"` | — | 도구·명령행 이름(`toolName`). ASCII 식별자(`[A-Za-z_][A-Za-z0-9_]*`), 없으면 C++ 필드 이름. 상속 포함 대소문자 무시로 유일해야 한다(RMC2203). 직렬화는 필드 ID라 이름과 무관하다 |
| `MaxElements=N` | — | 배열 길이 상한(5.5절). 배열이 아닌 필드는 RMC2202 |

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
- 단일 값 종류 필드는 지금처럼 `read`·`write`·`writeStaged` thunk를 가진다. Enum은 3.4절 검증, SoftObjectReference는 null 규칙·경로 상한을 검증하고 String처럼 새 값을 만든 뒤 Swap으로 게시한다. Struct·Array 필드는 `FieldValue`로 표현할 수 없으므로 `read`·`write`가 선택이고(없으면 Unsupported) `encode`·`decode`가 필수다(TypeRegistry·GraphValues의 "모든 typed 필드에 read·write·writeStaged" 검사를 종류별로 바꾼다).
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

- `StructSchema { TypeId id; uint32 version; name; Span<FieldSchema> fields; uint32 flags }`. struct 필드도 identity 명세의 `field`다.
- 생성 코드: `GeneratedType<S>`가 schema, constexpr 형질(`ContainsStrongReference`), codec 함수(6.1절)를 낸다. 다른 모듈의 struct를 쓰는 클래스의 생성 헤더는 **그 struct의 생성 헤더를 include한다**(같은 특수화를 두 번 정의하지 않는다 — ODR). 그래서 생성 중 의존 모듈의 stage 생성 루트를 넘긴다(1.3절).
- `RuntimeStructOps`: 크기·정렬, native 키, 경로 접근 thunk(7절).

### 4.3 참조를 담는 struct

- struct가 `ObjectRef<T>`나 `ObjectRefArray<T>`(직접 또는 중첩 struct 안)를 담을 수 있다("참조 struct", `ContainsStrongReference` = true). `ObjectRefArray`를 담으면 struct가 `explicit S(Allocator&) noexcept`로 allocator를 전달해야 한다. 소유 클래스의 생성 Trace가 그 경로를 따라 방문한다. 참조 쓰기는 소유 객체 저장소 안의 주소이므로 기존 `SetReference`·배열 소유자 검사의 범위 검사를 통과한다.
- 참조 struct는 **배열 원소가 될 수 없다**(RMC2304 — 배열 버퍼는 소유 객체 밖이라 위치 검사를 통과하지 못하고, `ObjectRef`는 복사·이동 금지라 재배치할 수 없다. `Array`의 nothrow-move static_assert가 이중으로 막는다). 객체 참조 목록은 `ObjectRefArray<T>`를 쓴다. 텍스처의 `BulkDataRef`·`TexturePlatformData` 등은 참조를 담지 않는다.
- 참조 struct 값은 통째로 복사·대입할 수 없으므로 도구의 struct 단위 쓰기는 경로 접근(7절)으로 멤버마다 하고, **struct 단위 원자성은 없다**(멤버 하나의 쓰기만 원자적이다).

### 4.4 직렬화와 버전

- struct 값은 중첩 Record(wire `Record` = 10)다. 헤더 schema = struct ID, schema version = struct 버전, 필드는 struct 필드 ID TLV다. 중첩 TLV도 그래프의 필드 누계(`maxTotalFields`)에 센다.
- 로드: struct ID가 다르면 TypeMismatch. 버전이 낮으면 등록된 struct 마이그레이션(`ObjectGraphLoadOptions.migrateStruct`, 객체 마이그레이션과 같은 형태, 결과 Record 재검증)을 적용하고 없으면 Unsupported. 버전이 높으면 decode 정책(6.1·6.4절)을 따른다 — 기본 거부(Unsupported), `LoadNewerSkippingUnknown`이면 현재 schema로 읽어 모르는 필드를 건너뛰고 `NewerVersion` 진단을 낸다(AS 5.3절의 "높으면 읽기 전용 열기"는 A1이 이 정책과 저장 금지를 함께 건다). 패키지 Summary의 타입 버전 표(AS 5.2절 Summary)에 struct 타입도 넣는다(A1).
- 모르는 필드는 건너뛰고 `UnknownField` 진단을 내며, 누락은 객체와 같은 `allowMissingFields` 정책을 따른다.

### 4.5 Core 값 타입 결합(`Binds`)

- 색·벡터 설정(`PaddingColor`·`ChromaKeyColor`는 UE FColor(8비트), `AlphaCoverageThresholds`는 FVector4(LWC double), 12.4절)은 Core 수학 타입을 쓴다. Core는 MetaSchema·ObjectRuntime에 의존하지 않으므로 Core 헤더에 annotation을 달 수 없다.
- ObjectRuntime에 결합 헤더(`Object/CoreValueTypes.h`)를 두고 `ROCKS_STRUCT(Binds = "<실제 타입 철자>", Version = 1)`로 같은 공개 멤버를 가진 거울 선언을 리플렉션한다(UE NoExportTypes 방식). 생성 코드는 거울이 아니라 실제 타입의 멤버 포인터를 쓰고, 거울과 실제 타입의 멤버 이름·타입·개수가 같은지 static_assert로 확인한다(어긋나면 컴파일 실패).
- 대상(구체 인스턴스 철자): `rocks::Vector2<float>`·`Vector2<double>`, `Vector3<float>`·`Vector3<double>`, `Vector4<float>`·`Vector4<double>`, `IntVec2`·`IntVec3`, `Quaternion<float>`·`Quaternion<double>`, `Rotator<float>`·`Rotator<double>`, `Color`(uint8 RGBA)·`LinearColor`(float RGBA), `Transform`(템플릿이면 구체 인스턴스). 실제 멤버 구성은 착수 때 헤더에서 확인한다. ID는 ObjectRuntime 명세에 등록한다.

## 5. 배열(B5)

### 5.1 `Array<T>`

- 원소: 스칼라·`String`·`Guid`·`AssetId`·`Sha256Digest`·enum·참조 없는 struct·`SoftObjectRef<U>`. `Array<ObjectRef<…>>`·참조 struct·중첩 배열은 RMC2304(`ObjectRefArray`를 쓴다).
- 필드는 소유 타입 생성자가 allocator로 초기화한다. struct 원소가 `explicit T(Allocator&) noexcept`면 그 allocator로 만든다.
- `ValueKind::Array`, `element`가 원소 형식이다.

### 5.2 `ObjectRefArray<T>`

- `FieldKind::ObjectReference`, `value.kind = Array`, `element = {ObjectReference, T의 ID}`. 생성 Trace가 `field.Trace(visitor)`를 부른다. inline struct 안에도 둘 수 있다(4.3절).
- 변경 API: 기존 `AddReference`·`RemoveReference`·`RemoveReferenceAt`·`ClearReferences`에 도구 편집용 `InsertReferenceAt(index)`·`SetReferenceAt(index)`를 더한다(삽입 장벽 포함, null 금지, 실패 시 내용 불변).

### 5.3 staged 변경 API

- 그래프 로드용 `ConstructionContext::AssignReferences(handle, ObjectRefArray<T>&, Span<const ObjectHandle>)`를 더한다(원소별 호출 대신 한 번 — 원소마다 slot 탐색·성장을 반복하지 않고 예산에 청구한다).
- Registry 쪽 변경: 소유자 검사를 Borrow 대신 **같은 트랜잭션의 Reserved**로 판정한다(Reserved 객체는 Borrow를 얻을 수 없다). 대상은 Alive 또는 같은 트랜잭션의 Reserved를 받는다(그래프 순환). shade는 SetEdge처럼 대상이 Alive일 때만 한다(Marking 중 Reserved 대상에 StaleHandle을 내지 않게). 잠금 밖 버퍼 성장 동안 `processing_`을 세운다(CreatePending과 같이 — allocator 재진입 Rollback이 use-after-free가 아니라 Fatal이 된다). Commit의 staged 간선 검증은 Trace로 원소도 방문한다. 생성 중에는 GC Begin·Step이 Busy라 수집과 겹치지 않는다.

### 5.4 wire

- 단일 값 원소 배열(스칼라·String·Guid·AssetId·Digest·enum·SoftObjectRef): Serialization에 **`ValueArray`**(wire `ValueArray = 13`)를 더한다. payload = u32 원소 wire tag, u32 count, count개의 원소 payload(그 wire tag의 Archive 표현을 이어 붙인다 — Bool 1, U32 4, U64·I64·F64 8, F32 4, Guid 16, String·Bytes u64 길이 + byte, SoftObjectReference Guid + String). 원소마다 Record envelope(최소 52 byte)를 쓰는 RecordList로는 배열 원소 한도가 그래프 바이트 한도를 넘기 때문이다.
- reader 검증: 원소 tag는 schema 원소 종류의 wire와 같아야 하고 Record·RecordList·ValueArray tag는 원소로 받지 않는다. count는 할당 전에 **schema 원소 종류의 최소 크기**(Digest 40, String·Bytes 8, SoftObjectReference 24, U64 8 …)로 남은 byte와 비교하고 count만큼 work를 먼저 청구한다. Record 트리 예산을 공유한다(`ValueArrayReader::OpenField`).
- struct 원소 배열: `RecordList`(11), 항목 Record schema = struct ID·버전. reader에 필드의 `maxElements`를 `RecordListLimits.maxItems`로 넘긴다(기본 65,536에 기대지 않는다).
- `ObjectRefArray`: `ValueArray` of 참조 원소(참조 wire는 문맥 매개변수 — 6.1절, graph v1은 U64 local ID, 0 금지).

### 5.5 한도

- 필드별 길이 상한 `MaxElements`: 기본 65,536, 최대 1,048,576. 인코딩된 필드 값은 그래프 `maxValueBytes`(4 MiB)도 지켜야 한다(넘으면 CapacityExceeded — 예를 들어 U64 원소는 약 52만 개가 한계다).
- **강제 지점**: 리플렉션 쓰기(경로 접근 7절)·저장(encode)·로드(decode). Registry 변경 API(`AddReference` 등)와 C++ 직접 사용에는 걸지 않는다(Level의 actor 목록 같은 런타임 목록을 막지 않는다 — 넘친 목록은 저장할 때 오류다).
- 그래프 전체: A0-B에서 `ObjectGraphLimits`에 `maxTotalElements`(기본 4,194,304)를 더하고, 중첩 struct TLV를 `maxTotalFields`에 세면서 그 기본값을 4,096에서 A0-O 목표치인 1,048,576으로 올린다(필드 3개짜리 struct 배열이 원소 약 1,366개에서 막히지 않게). 바이트·작업량 한도 확장(64 MiB·8 × (필드 + 원소))은 A0-O의 graph v2가 정한다.

## 6. 필드 codec(B3–B5 공통)

### 6.1 생성 codec thunk와 문맥

`RuntimeFieldOps`에 두 thunk를 더하고, GraphSave·GraphLoad는 모든 필드를 이 thunk로 인코딩·디코딩한다(지금의 `FieldValue` switch를 대신한다 — 단일 값 종류도 같은 경로).

- `encode`: 필드 값을 wire tag와 payload로 만든다(정확한 서명은 6.4절). C++에서 직접 쓴 값은 thunk 검증을 거치지 않았으므로 **encode가 다시 검증한다**(enum 선언 값, SoftObjectRef null 규칙·경로 상한, MaxElements, `rejectMissingRequired`가 켜졌으면 Required null).
- `decode`: 두 단계로 부른다(6.2절, 정확한 서명은 6.4절).
- 문맥(`FieldEncodeContext`·`FieldDecodeContext`)이 제공하는 것 — A0-O·A1이 생성 코드를 다시 고치지 않도록 B3에서 고정한다:
  - **참조 기록·해석기**: `WriteReference(ArchiveWriter&, ObjectHandle, expectation)`·`ReadReference(ArchiveReader&, expectation) -> DecodedReference`(null 여부와 2단계 handle — 6.4절). 참조 wire 형식이 문맥 매개변수다(graph v1은 U64 local ID, graph v2는 AS 5.1절의 u32 + import 비트). struct·배열 안의 참조도 같은 경로이며 기대 타입을 함께 넘긴다.
  - **Soft 참조 수집기**: (AssetId, 객체 경로, 기대 타입, 유효 플래그) — A1 Dependencies(Soft·EditorOnly, `expectedTypeId`·`required`)와 읽기 전용 열기의 부분집합 대조가 struct·배열 안까지 모은다.
  - **유효 플래그**: 바깥 필드의 EditorOnly·Transient·ReadOnly가 중첩 struct·배열 원소로 전파된 값(`PropagateFieldFlags`, 6.4절). graph v2의 EditorOnly 제외와 B6의 읽기 전용 판정이 이것을 쓴다. Required는 전파하지 않는다 — 그 필드 자신의 규칙이며 배열 원소의 null은 원소 규칙(null 금지)으로 다룬다.
  - **struct 관찰·치환 hook**: encode 직전(struct ID별 값 치환 — bulkId 재매핑, A0-O가 채운다)과 decode 뒤(struct ID별 관찰 — BulkDataRef 전수 대조, A1).
  - **진단 싱크**: `UnknownField`·`RequiredMissing`·`UndeclaredEnumValue`·`NewerVersion`(필드 경로와 함께).
  - **decode 정책**: enum 미선언 값(Reject·ReportAndDefault), 높은 struct 버전(Reject·LoadNewerSkippingUnknown). 기본은 모두 거부.
- 값 해석은 Record 트리 예산을 공유한다: 필드 payload는 `RecordReader::OpenValue`, 중첩 struct는 새 `RecordReader::OpenRecordField`(OpenValue와 같은 범위 검사·예산 공유), 배열은 `ValueArrayReader::OpenField`·`RecordListReader::OpenField`로 연다(1단계는 필드마다 `ArchiveReader::Create`로 새 예산을 열지 않는다 — 2단계는 6.2절의 별도 예산).

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
- 등록: encode·decode는 둘 다 있거나 둘 다 없다. `completeFieldLayout = true`면 Transient가 아닌 모든 필드에 둘 다 있어야 한다(2.5절의 뜻을 영속 필드의 codec까지 넓힌다). 그래프 저장·복원은 영속 필드마다 둘을 요구한다. struct·배열(B4·B5)은 등록에서 항상 필수다. 생성 코드는 모든 필드에 다섯 thunk(read·write·writeStaged·encode·decode)를 낸다.
- 생성 codec(`detail::EncodeGeneratedField<Owner>`·`DecodeGeneratedField<Owner>`): 시작할 때 `Field().value`가 정적 멤버 형식과 같은지(kind, enum은 `GeneratedType<E>::Id()`) 확인하고 다르면 TypeMismatch다(별명·수동 스키마와 정적 thunk의 어긋남이 조용한 오인코딩이 되지 않게). 직렬화기는 encode가 돌려준 wire가 `FieldWireType(kind)`(참조는 `ReferenceWireType()`)인지 다시 확인한다.
  - encode: 멤버를 직접 읽어 쓴다(String을 복사하지 않는다 — ArchiveWriter가 성장 전에 크기 한도를 검사한다). SoftObjectRef는 asset을 지역으로 복사한 뒤 (asset, path view)로 검증해 쓰고, payload를 쓴 뒤 owner의 논리 수명을 다시 확인하기 전에 수집한다. 다시 검증하는 것: enum 선언 값, SoftObjectRef null 규칙·경로 상한, Required null(정책이 켜졌을 때). 쓰기 뒤 owner의 논리 수명을 다시 확인한다.
  - decode: 1·2단계가 같은 reader 호출 순서를 실행한다(문자열·경로는 양 단계 모두 `ReadStringView`). 값 검증(wire 종류 TypeMismatch, 정수 폭, enum, digest 길이, Soft 규칙, trailing byte InvalidArgument)은 한 함수로 두어 1단계에서 실행하고, 2단계 staged 쓰기(`WriteGeneratedStagedField`)의 검사는 그 부분집합이다. 필드 진단은 그 필드의 검증(trailing byte 포함)이 끝난 뒤 보고한다. 2단계에서만 staged 쓰기로 게시한다.
- 미선언 enum 값: `Reject`면 InvalidArgument. `ReportAndDefault`면 1단계가 `UndeclaredEnumValue`를 보고하고 2단계가 바꾼 값을 쓴다 — 일반 enum은 값 0이 선언되어 있으면 0, 아니면 첫 열거자, Flags enum은 underlying 범위 안이면 선언되지 않은 비트만 지운 값(`v & 선언 mask` — 알려진 비트를 잃지 않는다), 범위 밖·음수면 0이다(`EnumReplacementValue`). 이 정책은 진단을 받을 곳이 있어야 하며(`CanReport()`), 공용 codec도 그것이 없으면 InvalidArgument다. 생성 decode는 등록된 enum 스키마로 판정한 뒤 정적 enum(`GeneratedType<E>::IsDeclared`)으로도 1단계에서 확인한다(등록 enum이 정적 enum보다 넓어도 2단계가 실패하지 않게). 3.4절의 기본값 서술도 이 규칙으로 맞춘다.
- 객체 Record의 높은 버전(AS 5.3 타입 데이터 축): 파일 버전이 현재보다 높으면 migrate를 부르지 않는다. `Reject`면 Unsupported, `LoadNewerSkippingUnknown`이면 그 버전으로 Record를 열어 아는 필드만 해석하고 모르는 필드는 건너뛰며 `NewerVersion`(path 없음)을 보고한다(모르는 필드 정책이 `Reject`면 그것이 우선한다). 낮은 버전만 migrate한다.

**그래프 저장·복원**

- `SaveObjectGraph(Allocator&, ObjectRegistry&, const ObjectAccessContext&, ObjectHandle root, ObjectGraphSaveOptions = {})`, `ObjectGraphSaveOptions { limits; FieldEncodePolicy policy; FieldCodecHooks* hooks = nullptr; }`. 영속(제외되지 않은) 필드마다 프레임에 들어가 encode가 payload를 쓰고 Record에 더한다. graph v1의 `WriteReference`는 지금의 대상 타입 검사와 BFS local ID 부여다(문맥은 `nodes` 재할당을 고려해 원소 참조를 보관하지 않는다).
- `ObjectGraphLoadOptions { limits; FieldDecodePolicy policy; migrate; migrationUser; FieldCodecHooks* hooks = nullptr; }`.
- **예산**: 그래프 `Budget`이 `ArchiveBudget` 하나를 모든 Record 트리와 공유한다(Serialization에 `RecordReader::Open(…, SharedPtr<ArchiveBudget>)` 공개 오버로드를 더한다). 스냅샷 편차가 없어지고 그래프 청구와 reader 청구가 같은 누계다.
- **1단계**(노드마다, 생성 전): 타입 기반 생성 거부(Abstract·construct 없음 — A0-O의 WorldOnly 정책도 여기)를 판정하고, Record 필드를 기록 순서로 돌며 스키마에 없으면 `UnknownField`(Reject 정책이면 InvalidArgument), Transient 필드가 있으면 Unsupported다. 아는 필드는 프레임에 들어가 `decode(self, nullptr, …)`로 검증하고, 필드마다 (스키마 위치, 등록 함수표 `const RuntimeFieldOps*`, FieldView)를 `LoadNode`에 보관한다. decode 직후 그 필드의 2단계 몫을 바로 청구한다: `Δ(공유 예산 사용량) + Σ ReserveApply`(`Spend(w)`는 양 단계에서 하는 일이라 1단계가 2w를 청구하고, `ReserveApply(w)`는 2단계에만 있는 일 — staged 쓰기 1 + 문자열·경로 복사 byte×2 + 참조 수 — 이라 1단계가 w를 청구한다). 누락은 Transient가 아니고 `IsMissingAllowed`가 아닌 스키마 필드 중 Record에 없는 것이다(허용되지 않으면 NotFound). 허용된 누락 중 Required는 `RequiredMissing`(파일에 없음)을 보고한다 — 생성자 기본값이 non-null인 SoftObjectRef도 보고 대상이다. 참조 token은 `{uint32 target; uint32 expectation}`으로 두고 기대 형식은 필드마다 한 번만 보관한다(B5에서 원소마다 복제하지 않는다).
- **도달성·참조 타입**: 모든 노드의 1단계 뒤 BFS가 도달한 노드의 token에 대해 검사한다(자기·전방 참조 포함). (기대 TypeId, 대상 노드 TypeId) 쌍을 캐시하고 기대 타입 lease는 캐시 항목이 로드 끝까지 쥔다. `TypeId{}`는 조회 없이 통과하고, 조회·삽입마다 청구한다. 기대 형식은 필드 안에서 같은 값이 반복되면 한 번만, 다르면(B4 struct의 서로 다른 참조 멤버) 따로 보관한다. 정적 기대(`staticNative`)도 같이 검사한다. 1단계 끝에 `노드 수 + 1(Commit) + staged 간선 수`를 청구한다.
- **2단계**: 그래프 `Budget`에 접근하지 않는다. 모든 객체를 만든 뒤 보관한 함수표·FieldView로 `decode(self, &transaction, …)`를 부른다. 입력 내용 때문에 실패하지 않는다 — 실패는 할당 실패와 Registry 상태(ShuttingDown·Busy·슬롯 상한·접근 문맥)뿐이며 전체 rollback이다. 그 밖의 2단계 실패는 계약 위반(1단계 검증이 2단계 검사를 포함하지 않음)으로 보고 오류 반환과 rollback으로 처리한다. 원본과 마이그레이션 결과는 호출 끝까지 불변이며 모든 콜백이 이를 지킨다.

**`detail::ValidateFieldWrite`**

- `ValidateFieldWrite(const FieldWriteBinding&, const FieldWriteTarget&, const FieldValue&) -> Result<void>`는 lease를 얻지 않는 순수 검사다. `FieldWriteTarget { const FieldSchema* actual; uint32 effectiveFlags; const EnumSchema* actualEnum; const void* sourceExpectedNative; const void* targetExpectedNative; const TypeLease* referenced; bool allowNull; }`. 소유 타입·양쪽 기대 타입(·B6의 경로 struct 레코드) lease는 호출자가 얻어 callback 완료까지 쥔다(재진입 재등록으로 검증과 게시의 기준이 달라지지 않게). enum은 실제 객체 쪽 `FieldEnum`으로 검사한다. 오류 순서는 값 종류·대상 타입·enum ID(TypeMismatch) → null 금지(InvalidArgument) → 읽기 전용(Unsupported) → 값 내용(Overflow·InvalidArgument). `FieldWriteOperation`은 양쪽 기대 타입의 native 불일치를 참조 값 조회(StaleHandle) 전에 TypeMismatch로 거부한다. unbound 원본 함수표 경로와 callback null 검사는 `FieldWriteOperation`에 남는다. 그래프 decode(staged)는 이 검사를 거치지 않는다(ReadOnly는 로드를 막지 않는다).

**B4·B5가 같은 문맥에 더하는 것(계약만 지금 고정)**

- 두 문맥의 `Scratch()`(예산을 청구하는 할당기), decode의 `OpenRecord`·`OpenValueArray`·`OpenRecordList`·`MigrateStruct`, `AddFields(n)`·`AddElements(n)`(2단계 no-op).
- 1·2단계는 같은 값을 같은 순서로 방문한다. 중첩 struct 마이그레이션 결과는 1단계 방문 순번으로 `LoadNode`(`Array<Array<uint8>>`)에 보관하고 2단계가 같은 순번으로 받는다. 중첩 색인은 1단계에서 보관하거나 2단계 전용 open(같은 크기 한도, 무제한 work, 그래프 필드 누계에 세지 않음)으로 다시 연다.
- struct hook(B4): 관찰은 1단계에서만, 관찰 대상으로 등록된 struct ID를 scratch 임시 값으로 해석해 `ObserveStruct(frame, TypeId, nativeKey, const void* value)`를 부른다(실패하면 생성 전에 로드 실패 — BulkDataRef 대조의 `CorruptPackage`). 치환은 encode에서 `SubstituteStruct(frame, TypeId, nativeKey, const void* in) -> Result<const void*>`(hook이 scratch에 같은 native 타입의 사본을 돌려준다 — 저장은 객체를 바꾸지 않는다). 참조를 담는 struct는 치환하지 않는다. 깊이·원소 여부와 무관하게 모든 struct 값에 적용하고 encode 순서는 결정적이다(AS 5.5).
- MaxElements를 올리면 그 타입의 Version도 올린다(옛 빌드가 새 데이터를 `CorruptPackage`로 오진하지 않게 — B5).
- decode는 staged 객체 전용이다. live 객체의 속성 단위 변경·Undo는 B6 경로 쓰기가 맡는다.

## 7. 경로 접근(B6)

- **해석 결과는 FieldId 체인 + 원소 index**(`PropertyPath { Span<const PathStep> }`, `PathStep { Guid fieldId; uint32 index; bool indexed }`)다. UI 문서의 Transaction(CU4)도 같은 표현을 쓴다. 문자열 경로(`toolName('.' toolName | '[' index ']')*`, 대소문자 무시, 모르는 이름은 `UnknownProperty` — AS 4.6절)는 schema로 이 체인을 만드는 앞단이다.
- 연산: 단일 값 읽기·쓰기(`FieldValue`), 배열 길이·삽입·삭제·원소 교체, 참조 struct의 멤버 쓰기, `ObjectRefArray`의 삽입·교체(5.2절 API). 실제 접근은 생성 thunk(`ops.path`)가 정적 타입으로 한다. 검증은 3.6절의 단일 검증 지점을 지난다(ReadOnly·대상 타입·enum·MaxElements).
- 원자성: 소유 객체 Borrow 안에서 하고, 할당 뒤에는 `Borrow.Get()`으로 소유자를 다시 확인한다(WriteGeneratedValue 패턴). 배열 삽입은 원소를 먼저 만든 뒤 `TryInsert`(원자적)로 넣는다(`TryResize`는 `T()`가 필요해 allocator 생성자만 있는 원소에 쓸 수 없다). 실패하면 미게시.
- 명령행 값 표기(쉼표 목록·`none`·경로 또는 AssetId, AS 13절)는 도구 계층이 이 연산 위에서 해석한다. B6은 도구 명령(A6) 전까지 미룰 수 있다.

## 8. 빌드 주입 상수(B7)

- **툴체인 지문**: SHA-256(정규 텍스트: 컴파일러 `--version`에서 `InstalledDir:` 등 호스트 경로 줄을 뺀 식별·버전, `Target:` 줄의 대상 triple, 실제 `CompileFlags`에서 가져온 부동소수점 모델 인자). 빌드 구성(Debug·Release)과 호스트 경로·환경은 넣지 않는다. 지문은 너겟 키에만 들어가고 쿠킹 byte에는 들어가지 않는다(AS 10절의 호스트 무관 요구와 충돌하지 않는다 — 다른 툴체인의 캐시 항목을 재사용하지 않을 뿐이다).
- **도구 빌드 ID**: SHA-256(정렬된 입력 목록 + 정규화된 compile 인자 + 툴체인 지문). 입력은 실행 파일 모듈 액션 그래프의 1차 입력(소스·규칙)과 `NativeBuildEngine.InputSnapshot`의 헤더 디렉터리 inventory(그 모듈과 의존 모듈의 include 디렉터리 안 헤더 전부)이고, 생성물과 BuildInfo 자신은 뺀다. depfile은 쓰지 않는다(Rocks.Build에 depfile 추적이 없고, 컴파일 뒤에야 생기는 정보라 clean 빌드와 증분 빌드의 ID가 달라진다). 경로는 `src:`·`out:`·`sdk:` 접두를 붙인 상대 경로(출력 디렉터리 이름의 preset·구성이 섞이지 않게)이며 내용 해시는 **텍스트 파일만** CRLF를 LF로 정규화해 계산한다(prebuilt 바이너리는 그대로 — 저장소는 이미 `* text=auto eol=lf`). Summary·AssetIndex 캐시 키의 진단용이다(AS 10절, 병합 조건 아님).
- 주입: NativeBuildEngine이 Core에 도달하는 실행 파일 모듈마다 `<out>/BuildInfo/<Module>.BuildInfo.cpp`를 만들어(계획 두 단계 — 파일을 만든 뒤 액션 그래프를 세운다) 그 모듈 소스에 더한다(Program.cs와 InstalledSdk 경로가 함께 쓴다). Core에 선언 헤더(`Core/Build/BuildInfo.h`: `ToolchainFingerprint()`·`ToolBuildId()`가 `Sha256Digest`를 돌려준다)를 둔다. 생성 TU 하나만 다시 컴파일된다.
- 시험: 지문이 구성과 무관하고 컴파일러 버전·대상에 따라 바뀜, 줄 끝 정규화, 생성 TU만 재컴파일. AS 시나리오 13·25에 **구성 사이 쿠킹 byte 비교**를 더한다(C3은 수학 digest만 확인했다).
- 문서 정정(반영함): llvm-nm 외부 심볼 검사·fmadd·`long double` 정책은 C3에서 이미 모든 구성에 구현되었다(CDF 2절). AS 5.6절의 "A0-B에서 추가"·"Development 구성"·`-fno-math-errno`(clang-cl이 거부해 쓰지 않기로 함, CDF 2.1절) 서술을 고쳤다.

## 9. 엔진 World 타입 리플렉션(B8)

- `Level`·`Actor`·`Component`를 `ROCKS_NATIVE_TYPE` 수동 등록에서 `ROCKS_CLASS(WorldOnly)`로 옮긴다. 컨테이너는 `ObjectRefArray`(actor·component 목록)·`Array`, raw 포인터·캐시는 `ROCKS_NATIVE`로 바꾼다(EngineRuntime 68·80·105행의 수동 등록 이유가 B2·B5로 해소된다). WorldOnly가 상속되므로 Actor 파생에 다시 쓰지 않는다.
- 소비자: AS 6.3절의 World 객체(SoftObjectRef 필드, `AssetLease` 멤버). World 모듈을 다루는 세션과 착수 전에 조율한다.

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
| RMC2202 | MetaCompiler | 지정자를 그 필드 종류에 쓸 수 없음(Required·MaxElements) |
| RMC2203 | MetaCompiler | 도구 이름(상속·접두 포함) 대소문자 무시 중복·형식 오류 |
| RMC2204 | MetaCompiler | 필드에 PROPERTY와 NATIVE가 함께 있음 |
| RMC2205 | MetaCompiler | NATIVE 멤버가 강한 참조·Object raw 포인터를 (재귀적으로) 담음 |
| RMC2206 | MetaCompiler | WorldOnly 전용 멤버를 WorldOnly 아닌 클래스에 둠 |
| RMC2301 | MetaCompiler | struct 상속 |
| RMC2302 | MetaCompiler | struct 타입 순환 |
| RMC2303 | MetaCompiler | struct 중첩 깊이 초과 |
| RMC2304 | MetaCompiler | 배열 원소로 쓸 수 없는 형식(ObjectRef·참조 struct·배열) |
| RMC2401 | MetaCompiler | enum underlying 미지정·지원하지 않는 폭·값 별칭 |

기존 코드(RMC1000·2000·2001·2100·3000·3100·4000)는 그대로다. RMC2005 중 한 모듈 안의 충돌(`.module.h`·`.generated.h`·대소문자)은 MetaCompiler도 판정한다.

## 11. 단계와 완료 기준

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| B1 | 모듈 소유 메타데이터·descriptor·import·stage 의존 해석·명세 집합 검증·합친 뷰 병합 검사·`ids move`·다중 명세 게시(삭제 포함)·SDK·템플릿·fixture 모듈 | 기존 리플렉션 시험 전부 + 1.9절 통합 시험 |
| B2 | 지정자 문법(정수)·`Version`·`WorldOnly`(상속)·`EditorOnly`·`Required`·`Name`·`MaxElements`·`ROCKS_NATIVE`(재귀 강한 참조 검사·WorldOnly 형질)·`completeFieldLayout` 뜻·`ROCKS_NATIVE_TYPE` | MetaTests 진단 사례, Registry 마스크·유일성·WorldOnly 상속 시험, 버전이 Record에 기록됨 |
| B3 | `AssetId`·`Sha256Digest`·`SoftObjectRef<T>`·`ROCKS_ENUM`(열거자 지정자·constexpr 검증기)·`ValueType`·TypeRegistry 종류·의존 계수·native 대조·`RegisterModuleTypes`·codec thunk와 문맥 전체·두 단계 decode·decode 정책·진단 | 종류별 읽기·쓰기·그래프 왕복, 잘못된 값 거부·정책별 진단, Shutdown이 Busy로 남지 않음, 모든 할당 실패 rollback |
| B4 | `ROCKS_STRUCT`·`RegisterStruct`·참조 struct Trace·중첩 Record·struct 마이그레이션·버전 정책·`Binds` Core 값 타입 | 중첩·참조 struct 왕복, 버전 낮음·높음 처리, Binds static_assert 실패 사례, 깊이·순환 진단 |
| B5 | `Array<T>`·`ObjectRefArray<T>`(Insert·SetAt)·staged `AssignReferences`·`ValueArray` wire·`MaxElements`·`maxTotalElements` | 원소 종류별 왕복, 순환 참조 배열 로드, Marking 중 staged 배열, 길이·바이트 상한, 잘림·count 사전 검사, 모든 할당 실패 |
| B6 | 경로 접근(FieldId 체인)·이름 해석 앞단 | 경로 읽기·쓰기·배열 연산·UnknownProperty·실패 시 미게시 (A6 전까지 미룰 수 있음) |
| B7 | 툴체인 지문·도구 빌드 ID 생성·주입, AS 문서 정정 | 8절 시험 (다른 단계와 독립 — 병렬 가능) |
| B8 | Level·Actor·Component의 `ROCKS_CLASS(WorldOnly)` 이전 | 기존 World 시험 전부 + 리플렉션 Trace·저장 |

상태: B1은 2026-11-13에 구현했다(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 "모듈 메타데이터"·"생성물과 등록"). B1의 `RegisterModuleTypes`는 클래스만 등록하며 B3가 enum·struct와 TypeRegistry 종류를 더한다. B2는 2026-11-13에 구현했다: 정수 지정자 값, `Version`·상속되는 `WorldOnly`, `EditorOnly`·`Required`(참조 필드만, RMC2202)·`Name`(도구 이름, RMC2203)·`MaxElements`(배열 전용 — 배열이 생기기 전까지 RMC2202), `ROCKS_NATIVE`(RMC2204·2205·2206, 생성 static_assert), `ROCKS_NATIVE_TYPE`, `completeFieldLayout`의 새 뜻, TypeRegistry 마스크·WorldOnly 상속·도구 이름 검사. 리플렉션 struct의 `ContainsStrongReference`·`RequiresWorldOnlyOwner` 형질은 struct가 생기는 B4에서 더한다. B3는 세 커밋으로 나눈다 — B3a(`ValueType`·`AssetId`·`Sha256Digest`·`SoftObjectRef<T>`, 3.1–3.3·3.5절의 스키마 표현), B3b(`ROCKS_ENUM`·TypeRegistry 종류·의존 계수·native 대조·enum 먼저 등록), B3c(codec thunk와 문맥·두 단계 decode·decode 정책·진단·`ValidateFieldWrite`). B3a는 2026-11-13에 구현했다: `FieldSchema::value`(`ValueType`)·`element`·`maxElements`(배열 전까지 Opaque·0만 등록), `ValueKind` 끝의 여섯 종류(Enum·Struct·Array는 B3b–B5까지 등록 거부), `FieldValue::digest`·`type`, wire `SoftObjectReference = 12`, 소프트 참조의 기대 타입과 `Required` 허용, 쓰기·저장·로드의 소프트 참조 규칙 검사(사용 계약은 [리플렉션 메타데이터](../Implementation/ReflectionMetadata.ko.md)의 형식 표). B3a에서 정한 것: `AssetId::TryParse`는 정규 표기(36자 소문자)만 받는다. FieldValue의 String은 UTF-8 불변식을 지키므로 소프트 참조의 UTF-8 검사는 `SoftObjectRef::TryAssign(StringView)`와 Archive 해석에서 걸린다. `SoftObjectRef`는 이동하면 원본이 null이 되고(경로만 비우면 같은 에셋의 대표 객체를 가리키게 된다), 받는 쪽 allocator를 유지하는 복사 `TryAssign(other)`와 allocator를 받는 `TryClone(allocator)`를 둔다. 등록된 쓰기의 오류 순서는 전달 값 종류 → 읽기 전용 → 값 내용(정수 폭·소프트 참조 규칙)이다. 소프트 참조의 기대 타입은 쓰기·저장·로드에서 검사하지 않는다(대상을 찾는 A0-O·A1의 일). descriptor import는 MetaCompiler가 만드는 값 종류만 받고 Required와 종류의 대응도 다시 검사한다(RMC1000). 상속 필드 대조에 `element`·`maxElements`를 포함한다. B3b는 2026-11-13에 구현했다: `ROCKS_ENUM`·`ROCKS_ENUMERATOR`(3.4절 문법·RMC2401·RMC2203), `EnumSchema`·`EnumeratorSchema`와 실행 시 검증기 `IsDeclaredEnumValue`·도구 이름 조회 `FindEnumerator`, TypeRegistry 종류(`TypeKind`)·`RegisterEnum`·`AcquireValueType`·enum 의존 계수·native 키 대조, `ModuleRegistration::RegisterEnumIfAbsent`와 enum 먼저 등록, 생성 `GeneratedType<E>`(constexpr `IsDeclared`), enum 필드 읽기·쓰기·staged 쓰기 검증, 그래프 I64 저장·복원(선언되지 않은 값 거부), descriptor의 enum 항목과 import. B3b 구현에서 정한 것: enum 레코드는 클래스와 같은 목록·ID 공간을 쓰고 `schema.id`·`name`과 `ops.nativeType`·`module`만 채운다. Enum 필드의 enum은 클래스 레코드의 필드별 의존 배열(`fieldTypes`)로 붙잡고 클래스 lease의 `FieldEnum(index)`로 연다(그래프 저장·복원이 이것으로 검증한다). 생성 접근기는 C++ enum의 native 키를 `RuntimeFieldOps::valueNativeType`으로 싣고, enum 등록 정보는 `RuntimeEnumOps{nativeType, module}`이다. Clang은 underlying을 생략한 `enum class`에도 int를 주므로 명시 여부는 원문(이름 뒤 `:`)으로 판정한다. 빈 enum·전방 선언·Flags enum의 음수 값·비ASCII 열거자 이름·4,096개 초과는 RMC2401이다. 클래스 안 중첩 enum도 리플렉션하되 public 구역이어야 한다. 생성 코드는 열거자 값마다 `static_assert`로 AST에서 읽은 값을 대조한다. 같은 C++ enum을 다른 ID로 다시 등록하지 않는다(AlreadyExists — 생성 thunk는 생성 ID 하나만 받는다). 같은 모듈의 public 클래스는 private enum을 필드로 쓸 수 없다(RMC2006). import한 base의 enum 필드는 import enum으로 해석한다. MetaCompiler는 enum을 클래스와 같은 타입 모델(`IsEnum`)로 두어 descriptor·import·inventory 경로를 공유한다. 미선언 값의 decode 정책(`ReportAndDefault`)은 B3c다. B3c는 2026-11-13에 구현했다: `Object/FieldCodec.h`(정책·진단·hooks·frame stack 문맥)와 `Object/GeneratedFieldCodec.h`(생성 codec·`EncodeThroughFieldValue`/`DecodeThroughFieldValue`), `RuntimeFieldOps::encode`/`decode`와 등록 규칙(둘 다 있거나 둘 다 없음, `completeFieldLayout`이면 Transient가 아닌 필드 전부), graph v1의 두 단계 decode(1단계 검증·예산 선청구, 2단계 예산 무접촉), `ObjectGraphSaveOptions`/`ObjectGraphLoadOptions`의 정책·hooks, `RecordReader::Open`의 공유 `ArchiveBudget`, `PropagateFieldFlags`·`EnumReplacementValue`, `detail::ValidateFieldWrite`(사용 계약은 [직렬화](../Implementation/Serialization.ko.md)의 "필드 codec과 문맥"). B3c 구현에서 정한 것: 진단을 보낼 hooks가 없으면 `ReportAndDefault`·`LoadNewerSkippingUnknown`은 InvalidArgument다. 더 새 객체 Record 버전은 migrate하지 않는다. 모르는 필드 `Reject`는 `LoadNewerSkippingUnknown`보다 우선한다. Meta 검사(`Rocks.Build.Cli`의 meta 단계와 ToolTests meta)는 구성의 `ROCKS_BUILD_*` 매크로를 넘긴다(생성 codec이 포함하는 Serialization·Core 헤더를 거쳐 `Base/Compiler.h`가 구성 매크로 정확히 하나를 요구하므로). B1 구현에서 정한 것: 메타데이터 배치·검증은 타깃에서 도달하는 모듈에만 적용한다. `ModuleRegistration`은 이미 등록된 공유 타입의 lease를 붙잡는다. `.generated.h` 철자를 예약한다(RMC2005). 명세 사이 이름도 유일해야 한다(1.6절).

의존: B1 → B3(집계 헤더·import), B2 → B3(Required·EditorOnly 검증), B3 → B4 → B5(struct 원소 배열), B3·B5 → B6(경로는 struct·배열을 지난다), B2·B5 → B8. B7은 독립이다. 각 단계는 구현 뒤 다수 서브에이전트 교차검증, Debug·Release·Asan·format·policy·Android 빌드를 거쳐 커밋한다.

## 12. 결정 기록·열린 문제·다른 문서 정정

결정:

1. 참조 struct는 배열 원소가 될 수 없다(4.3절) — Registry 간선 모델(소유 객체 저장소 안의 간선)을 유지한다.
2. 단일 값 배열은 `ValueArray`로 압축한다(5.4절) — 원소 한도와 바이트 한도를 함께 만족한다.
3. 플래그 enum을 지원한다(3.4절) — 미선언 값 거부를 "선언 값들의 OR"로 정의한다.
4. struct 상속은 v1에서 금지(4.1절). DataTable 행 같은 base struct 요구가 생기면 base 필드를 펼치는 방식으로 연다.
5. Core 수학 타입은 `Binds` 거울 선언으로 리플렉션한다(4.5절) — Core의 의존 방향을 지킨다.
6. 필드 codec은 생성 thunk의 두 단계 파싱이며 1단계 산출물을 보관하고 2단계 작업량을 1단계에서 청구한다(6.2절).
7. 툴체인 지문에 빌드 구성을 넣지 않는다(8절). 구성 사이 쿠킹 byte 비교 시험으로 확인한다.
8. WorldOnly는 상속되는 플래그다(2.2절). `Required`는 저장·쿠킹에서만 거부한다(2.3절).
9. 미선언 enum 값·높은 struct 버전은 기본 거부이고, 정책으로 보고 후 계속을 고를 수 있다(6.1절) — 새 빌드가 쓴 데이터를 손상으로 오진하지 않는다.

열린 문제(이 단계 밖에서 정한다):

- 계약 설정·필드 기본값 조회(클래스 기본 객체) — 텍스처 설정(A5).
- 배열 원소 안정 ID(prefab override, EIP 1702행·Sparks 1119행) — prefab 단계.
- 객체 단위 EditorOnly(Exports 표 플래그, AS 5.2절 Exports)와 Dependencies 표 로드 대조 키(AS 5.2절 Dependencies): 6.1절 수집기·유효 플래그가 담는 정보로 표현되도록 A1에서 정한다(코드의 `Required`·EditorOnly·기대 타입 변경만으로 정상 파일이 `CorruptPackage`가 되지 않는 범위).
- `TMap`·`TSet` 대응(Sparks 287·482행, UI CU4 map key, 계약 설정의 플랫폼별 값): 결정적 순서 규칙과 함께 후속.
- 텍스트(FText 대응)·클래스 참조(`TSoftClassPtr`·`TSubclassOf` 대응 — RuntimeClassMetadata)·Sparks `ScriptName`과 `Name`의 관계 — 후속.
- 버전 강제(영속 배치 지문 비교) — 후속 후보.

다른 문서 정정(이 설계를 반영할 때 함께):

- AS 1절 A0-B 행·리플렉션 한계 행·5.6절(이 문서로 연결, `-fno-math-errno`·llvm-nm 서술 정정)·14절 시나리오 13·25(구성 사이 쿠킹 byte 비교): 이 초안과 함께 반영했다.
- `ReflectionMetadata.ko.md`: 타깃 소유 서술은 B1에서 모듈 소유로 바꿨다. 버전 비노출·모든 비정적 필드 리플렉션 필수 서술은 B2 결과로 바꾼다.
- `SparksLanguage.ko.md`의 `Soft<T>`(AssetId만)·`UiToolkit.ko.md`의 `AssetRef<T>` → `SoftObjectRef<T>`(AssetId + 객체 경로). `UiToolkit.ko.md`의 `Sides<float>`(비템플릿 구체 타입으로)·지정자 확장 문법(CU1이 2.1절 표를 넓힌다).
- `EngineImplementationPlan.ko.md` 7.1절 예시 매크로(`ROCKS_REFLECT`·`ROCKS_FIELD(FieldId=…)`)와 `GcArray` 용어 → 현재 문법·`ObjectRefArray`.
- `EngineRuntime.ko.md` 68·80·105행: `ROCKS_NATIVE_CLASS` → `ROCKS_NATIVE_TYPE`, B8 이전 계획.
