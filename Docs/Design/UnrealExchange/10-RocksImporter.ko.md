## 10. Rocks 쪽 임포터 `UnrealExchange`

### 10.1 플러그인과 모듈

| 항목 | 내용 |
|---|---|
| 위치 | `Engine/Plugins/Importers/UnrealExchange/` — Rocks 엔진 플러그인(빌드 규칙 glob `Engine/Plugins/**`, PluginCatalog). 엔진 핵심 계층(`Engine/Source/Runtime`)에 두지 않는다 |
| 모듈 | `UnrealExchange`(Developer 종류) |
| 책임 | 묶음 읽기·검증(3절), 종류별 대응(4–8절), 머티리얼 번역 표, 경로·ID·순서(9절), 임포트 서비스 호출 |
| 의존 | AssetDatabase·TextureTools·MeshTools·AnimationTools·MaterialCompiler(그래프 검증)·Material·Core(JSON·SHA-256). UX1a 시점은 Core·AssetRuntime·MeshSource이고 나머지는 쓰는 단계에서 더한다(10.4절 1번) |

UE 쪽 지식(클래스 이름·프로퍼티 이름·enum 값)은 이 모듈에만 둔다. 에셋 모듈과 도구 모듈은 UE를 모른다. 플러그인을 켜지 않은 프로젝트·Shipping 빌드에는 들어가지 않는다.

### 10.2 명령

```text
asset-import-unreal <묶음 디렉터리> [--root=/Game/Unreal] [--only=<objectPath>...]
                    [--mesh-normals=unreal|rocks] [--dry-run]
```

- `--only`는 지정한 에셋과 그 Hard 닫힘을 가져온다(경로 대응은 9.1절이라 결과가 전체 임포트와 같다).
- `--dry-run`은 검증·대응·경로 계산만 하고 결과 표(만들 에셋, 진단)를 출력한다.
- 결과 보고: 에셋별 성공·실패·경고 수, 머티리얼 번역 결과(1·2·3번 비율), 번역 못 한 노드 클래스 빈도 표. 번역 표를 채울 우선순위를 정하는 데 쓴다.

### 10.3 진단 코드

AssetSystem 4.6절 진단 표에 등록한다(요구 UXR3).

| 코드 | 수준 | 상황 |
|---|---|---|
| `CorruptBundle` | 오류 | blob 크기·SHA-256 불일치, JSON 구문 오류, 필수 키 누락, 배열 길이 불일치, 배치 파싱 실패 |
| `UnsupportedBundleVersion` | 오류 | 모르는 `formatVersion` |
| `UnknownField` | 경고 | 모르는 JSON 키 |
| `UnsupportedAsset` | 경고 | 범위 밖 클래스(2절) |
| `UnsupportedSourceLayout` | 오류 | UDIM·다층·위도경도 큐브 |
| `GammaMismatch` | 경고 | 4.2절 |
| `GeneratedLodBaked` / `BakedColorQuantized` | 정보 | 5.1·6.2절 |
| `UnrealPathRenamed` | 정보 | 9.1절 |
| `UnrealPathTooLong` / `UnrealPathCollision` | 오류 | 9.1절 |
| `MissingUnrealDependency` | 경고 | 9.3절 |
| `UntranslatedMaterialNode` / `MaterialBakedFallback` | 경고 | 8.5절 |
| `ParameterDefaultConflict` / `ParameterResolveMismatch` | 경고 | 8.5절 |

메시·애니메이션 데이터 검증 진단은 MeshAnimationAssets 11절, 텍스처는 AssetSystem 4.6절 코드를 그대로 쓴다.

오류 값(2026-11-27 — AssetSystem 19장 `CorruptData` 이전): 묶음은 도구(RocksExport)가 쓰고 도구가 읽는 파일이므로 `CorruptBundle` 진단의 형식 위반(JSON 구문·필수 키·종류·값·정렬·blob 크기·SHA-256·가변 배치·일반 파일 아님·목록 대조)은 `CorruptData`다. 상한은 `CapacityExceeded`, 판은 `Unsupported`, 파일 IO는 그 파일 오류 그대로다. 종류별 단계도 같다 — 텍스처 원본 형식 위반(4.6절 2번 — `CorruptBundle`과 클래스 slice 규칙 `InvalidSource`)과 설정 덤프 번역·`ImportJson` 거부(10.6절 8번)는 `CorruptData`다. `InvalidArgument`로 두는 것: 명령 인자·`--root`·`--only`(사람의 입력), 호출 계약 위반(선택이 Hard 닫힘이 아님 등), 임포터 선택·옵션 대입(`--importer`·`objectPath` 불일치·쓰지 않는 blob 원본 — 호출자가 준 원본 목록), 플러그인 error 진단(`InvalidSource` — 손상이 아니라 내보내기가 실패를 알린 것).

### 10.4 구현에서 정한 점(UX1a)

UX1a 구현(플러그인 모듈 `UnrealExchange`의 묶음 리더·검증, 경로 대응, 임포트 순서) 중 3·9·10.3절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다. 에셋 DB에 쓰는 부분(임포트 서비스 연결, 기존 에셋 찾기, `--only` 닫힘, 명령 등록)은 UX1b다.

1. **파일과 의존.** 공개 헤더는 `UnrealExchange/UnrealExchangeFormat.h`(상수), `ExchangeDiagnostic.h`(진단), `ExchangeBundle.h`(리더·메모리 형태·blob 읽기), `ExchangePaths.h`(경로 대응), `ExchangeImportOrder.h`(임포트 순서)다. 모듈 공개 의존은 Core·AssetRuntime(`AssetDiagnosticCode`)·MeshSource(가변 배치 상한 `MeshMaxSourceInfluences`, Name 길이 상한 `MaxNameFieldBytes`)다. 10.1절 표의 나머지 의존은 그것을 쓰는 단계(UX1b–UX8)에서 더한다. 파일 접근은 `platform::FileSystem`을 인자로 받는다(시험은 메모리 파일 시스템을 쓴다).
2. **실패 범위.** 묶음 수준 오류(`bundle.json`을 읽지 못함·구문 오류·필수 키 누락·`formatVersion` 불일치·목록 정렬 위반)는 읽기 전체를 실패시킨다. 에셋 수준 오류(에셋 JSON 손상, blob 크기·SHA-256·배치 불일치, 목록과 다른 `objectPath`·`class`)는 그 에셋만 `Failed` 상태로 두고 나머지를 계속 읽는다(9.3절 "부분 결과" 원칙). 실패한 에셋을 Hard로 참조하는 에셋은 순서 계산(15번)에서 `MissingDependency`로 실패한다. 파일을 열거나 읽지 못하면(없음 포함) `CorruptBundle`(세부 규칙 `ReadFailed`)이고 원래 파일 오류를 `error`에 그대로 남긴다. 할당 실패는 진단 없이 그 오류 그대로 전체 실패다.
3. **진단 형태.** `ExchangeDiagnostic`은 AssetSystem 4.6절 코드, 수준(오류·경고·정보 — 코드마다 고정, 10.3절 표), 원본 `ErrorCode`, 세부 규칙 `ExchangeIssue`, 에셋 번호, 관련 번호(의존 대상 에셋·의존 순번), 파일 종류(`bundle.json`·에셋 JSON·blob), JSON 위치(줄·열·byte), 짧은 텍스트(키 이름·objectPath·blob hex)를 담는다. 묶음 하나의 진단은 `UnrealExchangeMaxDiagnostics`(= 4096)개까지 담고 넘는 것은 개수만 센다(조작된 묶음이 모르는 키 수백만 개로 메모리를 쓰지 못하게). 이 단계에서 등록하는 코드는 리더·경로·순서가 내는 8개(`CorruptBundle`·`UnsupportedBundleVersion`·`UnknownField`·`UnsupportedAsset`·`UnrealPathRenamed`·`UnrealPathTooLong`·`UnrealPathCollision`·`MissingUnrealDependency`)다. `UnsupportedCycle`·`MissingDependency`·`InvalidPath`는 기존 코드를 쓴다. 나머지 10.3절 코드는 그것을 내는 단계에서 등록한다. 원본 `ErrorCode`는 손상 `CorruptData`(상한 초과는 `CapacityExceeded` — 10.3절 오류 값), 버전 `Unsupported`, `UnrealPathTooLong` `CapacityExceeded`, `UnrealPathCollision` `AlreadyExists`, `UnsupportedCycle` `CycleDetected`, `MissingDependency` `NotFound`, `InvalidPath` `InvalidArgument`이고 경고·정보는 `None`이다.
4. **JSON 읽기.** Core JSON Strict 읽기에 상한 `UnrealExchangeMaxJsonBytes`(= 64 MiB, 파일 하나)를 준다. 깊이·문자열·노드 상한은 Core 기본값이다. BOM은 Core가 경고로 건너뛰지만 3.2절이 BOM 없음을 정하므로 `CorruptBundle`이다. 중복 키는 Core가 구문 오류로 거부한다. 정수 키(`formatVersion`·`bytes`·`components`·`count`)는 JSON 정수여야 한다(`1.0`·지수 표기는 타입 오류). 실수 값은 숫자 또는 문자열 `"nan"`·`"inf"`·`"-inf"`만 받는 공용 함수(`ReadExchangeNumber`)로 읽는다. 종류별 단계가 `settings`·`data`를 읽을 때 쓴다.
5. **필수 키와 모르는 키.** 필수: `bundle.json`의 `formatVersion`·`exporter`(문자열 4개)·`projectSettings`(bool 2개)·`assets`·`redirects`, 에셋 JSON의 7개 키 전부, 목록·의존·진단·blob 참조 항목의 모든 키. 선택: `environment`(정보, 객체, 내용은 보지 않는다)·`createdUtc`(정보, 문자열). `formatVersion`을 가장 먼저 보므로 다른 버전의 묶음은 스키마 차이와 무관하게 `UnsupportedBundleVersion`이다. 모르는 키 검사(`UnknownField` 경고)는 이 절의 고정 스키마 객체에만 한다. `settings`·`data`·`environment`는 종류별 단계가 본다.
6. **정렬과 유일성.** `assets`는 `objectPath`의 UTF-8 byte 순으로 엄격 증가(같은 경로 둘은 손상), `redirects`는 `from` 순 엄격 증가, 에셋의 `dependencies`는 `objectPath` 순 엄격 증가다(같은 객체를 두 번 적지 않는다 — Hard·Soft로 모두 참조하면 Hard 하나). 내보내는 쪽이 결정적으로 쓰는 규칙(3.2절)을 리더가 확인해 같은 묶음의 해석이 하나로 정해지게 한다. 3.4·3.5절 표에 반영했다.
7. **objectPath 문법.** `/<Mount>/<경로…>/<패키지>.<객체>`: 앞 `/`, 패키지 경로 구성 요소 2개 이상(마운트 + 패키지), 빈 구성 요소 없음, `.`은 객체 이름 앞 하나뿐, `:`(하위 객체)·`\`·제어 문자(U+0000–U+001F, U+007F) 없음, 전체 ≤ `UnrealObjectPathMaxBytes`(= 1024, UE `NAME_SIZE`). 어기면 `CorruptBundle`이다. 객체 이름과 패키지 이름이 같을 필요는 없다(경로 대응은 패키지 경로를 쓴다 — 파일 위치가 진실의 원천, AssetSystem 4.2절).
8. **파일 이름.** 에셋 목록의 `file`은 `assets` + 패키지 경로 + `.json`과 정확히 같아야 한다. blob 파일은 `blobs/<앞 2자>/<64자>.bin`으로 리더가 만든다. 그래서 묶음 밖을 가리키는 경로(`..`·절대 경로)가 생기지 않는다. 목록의 `objectPath`·`class`와 에셋 JSON의 값이 다르면 그 에셋은 `CorruptBundle`이다.
9. **클래스.** 교환 클래스 이름 14개(2절 UE 클래스에서 `U`를 뺀 것)를 열거로 둔다. 목록에 그 밖의 클래스가 있으면 `UnsupportedAsset` 경고를 내고 그 에셋 JSON은 읽지 않는다(`Unsupported` 상태, 순서 계산에서 빠짐). 의존 항목에는 대상 클래스 `class`를 둔다(3.5절에 더함). 묶음에 없는 대상을 9.3절대로 머티리얼·텍스처와 그 밖으로 가르려면 대상 클래스가 필요하기 때문이다. 범위 밖 클래스를 가리키는 의존은 순서와 누락 판정에서 뺀다(그 참조를 받는 필드는 종류별 단계가 처리한다).
10. **redirect.** `from`은 묶음의 에셋 경로와 겹칠 수 없고 `to`는 다른 항목의 `from`일 수 없다(사슬 없음 — 플러그인이 끝까지 푼다). 어기면 묶음 수준 `CorruptBundle`이다. 의존의 `objectPath`가 `from`과 같으면 `to`로 바꿔 찾는다.
11. **blob 참조 인식.** `settings`·`data` 안에서 `blob` 키를 가진 객체를 blob 참조로 본다(종류별 스키마 없이 리더가 모든 blob을 검증할 수 있게). 키는 정확히 3.3절 5개이고(빠지면 손상, 더 있으면 `UnknownField`), `blob`은 소문자 64자 hex, `bytes` ≤ `UnrealExchangeMaxBlobBytes`(= 4 GiB), `components` 1 이상(`boneInfluences`·`sparseVec3`는 1), 고정 배치는 `bytes` = 원소 크기 × components × count(곱이 넘치면 손상)다. 같은 blob hex를 다른 `bytes`로 적으면 손상이다(내용 주소).
12. **blob 검증.** 읽기 옵션 `verifyBlobs`(기본 켬)이면 서로 다른 (hex, 배치, components, count) 참조마다 파일을 한 번 열어 크기를 먼저 보고(`bytes`와 다르면 큰 할당 없이 손상), `UnrealExchangeBlobChunkBytes`(= 64 KiB) 조각으로 읽으며 SHA-256과 가변 배치 파싱을 함께 한다(파일 전체를 메모리에 올리지 않는다). 가변 배치 규칙(MeshAnimationAssets 3.4.1절): `names`는 count × components개의 (`uint32` 길이 ≤ `MaxNameFieldBytes` + byte)를 끝까지 정확히 소비, `boneInfluences`는 시작 표 첫 값 0·단조 증가·연속 차 ≤ `MeshMaxSourceInfluences`·크기 = 4 × (count + 1) + 4 × 마지막 값, `sparseVec3`는 K ≤ count·번호 엄격 증가·번호 < count·크기 = 4 + 16 × K다. 이름의 UTF-8·NFC, 0 항목, 가중치 같은 내용 규칙은 MeshSource 검증(UX3)이 본다. `LoadExchangeBlob`은 임포트 때 blob 하나를 메모리로 읽고 같은 검사를 다시 한다(읽기와 임포트 사이에 파일이 바뀌어도 잡는다).
13. **경로 대응 세부(9.1절).** 구성 요소마다 먼저 NFC로 정규화한다(정규화만의 차이는 바꿈으로 치지 않는다). 그다음 허용 문자(L*·N*·M*·`_`·`-`, Default_Ignorable 제외)가 아닌 code point를 하나씩 `_`로 바꾸고, 바꾼 뒤의 첫 문자가 L*·N*가 아니거나(L*이어도 Default_Ignorable인 U+115F 같은 글자는 `_`가 되므로 접두가 붙는다 — 퍼징이 찾은 경우) Windows 예약 이름이면(판정은 Core `ValidateNameComponent`) 앞에 `U`를 붙이고 다시 NFC로 정규화한다(첫 문자가 결합 문자면 `U`와 합성될 수 있다). 바뀌었거나 64 byte를 넘으면 code point 경계에서 55 byte 이하로 자르고 `-` + 해시 8자를 붙인다. 해시 입력은 원래 UE 패키지 경로에서 그 구성 요소까지의 접두다(예: `/Game/My Dir`). 결과는 언제나 `ValidateNameComponent`를 통과한다(퍼징 시험). `--root`도 같은 규칙으로 검사하고 어기면 `InvalidPath` 오류다. 겹침 판정 키는 대응 경로 전체의 `TryMakeCaselessKey`이고 겹치는 에셋 모두를 `UnrealPathCollision`으로 실패시킨다(어느 쪽을 살릴지 집합에 따라 달라지지 않게).
14. **임포트 순서(9.3절).** 묶음 안 대상을 가리키는 Hard 의존이 간선이다(Soft는 순서에 쓰지 않는다). 간선이 Build인지는 클래스 쌍으로 정한다. 머티리얼 계열(`Material`·`MaterialInstanceConstant`·`MaterialFunction`·`MaterialFunctionInstance`)에서 머티리얼 계열·`MaterialParameterCollection`으로 가는 Hard 의존이 Build다(MaterialSystem 4.4절 — 부모·함수 호출·레이어·컬렉션). Build 관계는 Rocks 개념이라 UE 플러그인이 쓰지 않는다(UX-D2와 같은 이유). SCC는 Core 공용 반복 Tarjan(`algo::ForEachStrongComponent`, 개정 17 — 전에는 같은 알고리즘의 사본)으로 objectPath 순 시작점에서 구하고(단위가 아닌 에셋 — 고르지 않았거나 지원하지 않는 에셋 — 은 간선이 없어 혼자 묶음이 되므로 건너뛴다), 단위 깊이 = 단위 밖 의존 단위 깊이의 최댓값 + 1(의존 없으면 0)이다. 순서는 (깊이, 단위의 가장 작은 objectPath)다. Build 간선이 단위 안에 있으면(자기 자신 Build 참조 포함) `UnsupportedCycle`, 깊이가 `UnrealImportMaxDepth`를 넘는 단위도 `UnsupportedCycle`(세부 규칙 `DepthLimit`)이다. Hard만의 자기 참조는 무시한다.
15. **실패 전파와 누락 의존.** 단위의 구성원 하나가 실패(읽기 실패·경로 오류·순환)하면 단위 전체가 실패하고, 실패한 단위를 Hard로 참조하는 단위는 `MissingDependency`(관련 = 처음 걸린 의존 대상)로 실패한다. 묶음에 없는 대상은 선택 인자 `ExchangeExistingAssets`(UX1b가 AssetIndex 태그 조회로 구현)가 있다고 하면 잇고, 없으면 대상 클래스가 텍스처 5종·`Material`·`MaterialInstanceConstant`일 때 `MissingUnrealDependency` 경고, 그 밖은 `MissingDependency` 오류(단위 실패)다.
16. **상한 상수.** `UnrealExchangeMaxAssets`(= 1,048,576), `UnrealExchangeMaxRedirects`(= 1,048,576), `UnrealExchangeMaxDependencies`(= 65,536, 에셋 하나), 위 4·7·11·12번 상수. 넘으면 그 수준의 `CorruptBundle`이다(세부 규칙 `LimitExceeded`).
17. **스레드와 수명.** 읽기·순서 계산은 호출한 스레드에서 끝난다. 읽은 `ExchangeBundle`은 바뀌지 않으므로 여러 스레드가 const로 함께 읽어도 된다(`LoadExchangeBlob` 포함 — 넘긴 FileSystem이 스레드 안전할 때). 에셋의 문자열·`settings`·`data`는 그 에셋이 가진 JSON 문서를 가리키므로 묶음보다 오래 쓸 수 없다.

**시험과 coverage(UX1a, 개정 7).** 에셋 시험 기준(AssetSystem — 분기 100% 목표, 입력 변조·실패 주입으로 닿는 오류 분기는 시험으로 닿게)을 적용해 플러그인 Private 소스(`ExchangeBlob`·`ExchangeBundle`·`ExchangeDiagnostic`·`ExchangeImportOrder`·`ExchangePaths`, `ExchangeInternal.h`)는 줄·분기·함수 모두 100%다. 닿지 않던 분기는 두 갈래로 정리했다.

- 시험으로 닿게 한 것: 진단 목록 할당 실패(진단을 내는 자리마다 n번째 할당 실패를 되풀이), 긴 묶음 디렉터리의 경로 조립 할당 실패, 넓은 `settings`·`data`의 탐색 stack 할당 실패, 결합 문자가 든 `--root`(이름 검사에 할당이 든다), 64 KiB 조각 뒤의 읽기 실패(시험 파일 시스템의 늦은 읽기 실패), 목록 상한(에셋·redirect 1,048,577개), 정수 키의 `uint64` 큰 값, 빠진 필수 키 전부, blob 사용 조합(같은 blob의 다른 components·중복·count만 다름, 충돌로 이미 실패한 에셋의 뒤 blob 실패), 순서 경계(읽기 실패 에셋의 경로 초과·겹침, 실패한 의존 둘, Tarjan의 늦은 역간선, SCC 구성원의 누락 의존).
- 구조를 바꿔 없앤 것(실패할 수 없는 연산의 결과 검사): blob 원천의 읽기는 할당이 없으므로 `Result` 대신 `bool`을 돌려준다. 미리 잡은 용량 안의 경로 붙이기는 연쇄 붙이기(`Append`) 뒤 `Status` 한 번으로 본다. 비어 있지 않은 배열 꺼내기(`TryPopBack`), JSON 숫자의 double 읽기, NFC 결과의 code point 순회는 실패할 수 없어 `.Value()`로 받는다. 예약해 둔 배열에 넣기는 예약을 없애 넣기 자체가 할당 실패 지점이 되게 했다(할당 실패 시험이 닿는다). blob 사용 정렬은 에셋 번호까지 같은 두 사용이 없으므로 마지막 비교를 에셋 번호로 끝낸다.
- 사유를 남기고 닿지 않는 분기: 없음.

18. **묶음 JSON 합 상한(개정 15).** 리더는 bundle.json과 모든 에셋 JSON 문서를 메모리에 둔다. 그래서 파일 하나 상한(`UnrealExchangeMaxJsonBytes`, 64 MiB)에 더해 읽은 JSON 파일 크기 합 상한 `UnrealExchangeMaxBundleJsonBytes`(1 GiB, `ExchangeReadOptions::maxBundleJsonBytes` — 시험이 줄여 쓴다)를 둔다. 넘는 파일부터는 읽지 않는다: bundle.json이면 묶음 수준, 에셋 JSON이면 그 에셋이 `LimitExceeded`(`CorruptBundle`, `CapacityExceeded`)로 실패한다. 상한은 파싱 전 원문 크기 기준이다.
19. **일반 파일만 읽는다(개정 15).** JSON과 blob을 열기 전에 Core `FileSystem::Stat`으로 link가 아닌 일반 파일인지 본다(장치·FIFO는 열기·읽기가 막힐 수 있고 link는 묶음 밖을 가리킬 수 있다). 아니면 새 세부 규칙 `NotRegularFile`(`CorruptBundle`, `CorruptData`)이고 Stat 실패는 `ReadFailed`(파일 오류 보존)다. Stat과 Open 사이의 교체는 막지 않는다(로컬 작업 디렉터리 전제).
20. **플러그인 error 진단(개정 15).** 에셋 JSON `diagnostics`에 `severity: error`가 하나라도 있으면 그 에셋은 Ready가 아니다: 첫 error 진단의 `code`를 text로 `InvalidSource`(새 세부 규칙 `ExporterError`, `InvalidArgument`)를 내고 Failed로 둔다. 기존 진단 코드 `InvalidSource`(임포트 원본이 쓸 수 없음 — AssetSystem 4.6절)를 그대로 써 AssetRuntime 진단 표를 늘리지 않는다. Hard 의존자는 임포트 순서 계산이 `FailedDependency`로 실패시킨다(15번).
21. **blob 참조 수집(개정 15).** 한 에셋의 blob 사용 목록(처음 나온 순서, 같은 사용은 한 번, 같은 hex의 다른 bytes는 `BlobConflict`)을 만들 때 이미 모은 것과의 비교를 digest 앞 8 byte 키의 사슬로 한다(키별 마지막 번호 표 + 같은 키의 앞 번호). 전에는 참조마다 목록 전체를 훑어 참조 수의 제곱이었다. 결과 순서·진단은 같다.
22. **의존 class 대조 범위(개정 15).** 의존 항목의 `class`는 대상이 묶음 안에 있을 때만 대상 에셋의 `class`와 대조한다(9번 — 다르면 손상). 묶음 밖(범위 밖 클래스·닫힘 밖) 대상은 대조할 것이 없어 그대로 둔다. 플러그인은 옛 경로(redirector) 참조를 대상 경로·클래스로 바꿔 의존에 적으므로(11.7절 21번) 묶음 안 대상의 대조를 늦추지 않는다(권장안 — 대조를 완화하면 실제 손상을 놓친다).

### 10.5 임포트 계획과 명령(UX1c)

UX1c는 A2f 임포트 프레임워크를 고치지 않고 할 수 있는 Rocks 쪽 연결이다(사용자 결정 2026-11-23 "나눠 진행"). 묶음을 읽어 무엇을 어디에 만들거나 다시 임포트할지 정하고(계획), 종류별 임포터를 순서대로 부르는 틀과 명령을 둔다. 실제 에셋을 만드는 종류별 임포터는 UX2b 이후이고, 그 전제인 프레임워크 보강 네 가지는 13절 UXR8–UXR11이다. 모두 권장안으로 정했다.

1. **모듈 의존.** 공개 의존에 AssetDatabase(색인 태그 조회 `AssetIndex::FindByTag`, `AssetService`)와 AssetCommandlets(명령 세션 `EditSession`·`OpenSession`)를 더한다. 둘 다 Developer라 플러그인 종류(Developer)와 맞는다.
2. **`--only` 닫힘(`SelectExchangeClosure`).** 값마다 묶음 redirect를 풀고(`from`이면 `to`) 묶음 에셋을 찾는다. 없으면 명령 실패(`NotFound`, 그 값)다. 고른 에셋에서 묶음 안 Hard 대상(`target`이 있는 의존)을 끝까지 따라가 고른다. Soft는 따라가지 않는다. 결과는 에셋마다 선택 표시(0·1)이고 `--only`가 없으면 모두 고른다. 범위 밖 클래스 에셋도 직접 고르면 선택에 들지만 계획에는 들지 않는다(건너뜀으로 보고).
3. **선택과 결정성.** 경로 대응·겹침 판정과 기존 에셋 조회는 선택과 무관하게 묶음 전체에 한다. 그래서 같은 에셋의 대응 경로·겹침 실패·재임포트 대상은 `--only` 유무와 상관없이 같다(12.1절 "`--only` 결과와 전체 임포트의 경로·내용 일치"). 진단은 고른 에셋에 대해서만 낸다(겹침 진단의 관련 에셋은 고르지 않은 것일 수 있다). 단위·간선·Tarjan은 고른 에셋만 본다. 선택이 Hard 닫힘이므로 고른 에셋의 묶음 안 Hard 대상은 모두 고른 것이다.
4. **기존 에셋 조회(`ExchangeExistingAssets`, 9.2절).** 조회 키는 에셋의 objectPath와 그 objectPath로 이어지는 redirect `from` 목록이다(redirect는 `to` 기준 색인을 한 번 만들어 찾는다). 두 단계로 찾는다: 먼저 `UnrealExchange.ObjectPath` = objectPath, 없으면 `UnrealExchange.ObjectPath` ∈ 옛 경로 또는 `UnrealExchange.FormerPaths` ∈ {objectPath, 옛 경로}(둘째 단계로 찾았다는 표시 `former`를 돌려준다). 처음 찾은 단계에서 서로 다른 색인 항목이 하나이고 그 id가 색인에서 유일하면 그 에셋(색인의 지금 경로와 함께), 항목이 둘 이상이거나 같은 AssetId의 다른 파일이 있으면(태그가 한쪽에만 있어도 — 재임포트가 어느 파일인지 정할 수 없다) 모호함이다. 인터페이스는 `Result<ExchangeExistingMatch> Find(objectPath, formerPaths)`와 경로 점유 `Result<bool> PathTaken(논리 경로)`다(조회 할당 실패를 삼키지 않게 — UX1a의 `bool Contains`를 대신한다). 색인 구현은 `ExchangeIndexLookup`(`AssetIndex` 위)이고, 처음 조회할 때 두 태그의 (키, 값) 정렬 표를 한 번 만들어 이진 검색한다(묶음 에셋마다 색인 전체를 훑으면 묶음 크기 × 색인 크기다 — 리뷰 권장).
5. **재임포트 대상과 경로.** 기존 에셋을 찾으면 대상 경로는 그 에셋의 지금 경로다(9.1절 "재임포트에서는 기존 에셋 위치가 우선"). 판정은 모두 선택과 무관하게 묶음 전체에 한다.
   - 같은 기존 에셋: 묶음 에셋 둘 이상이 같은 AssetId를 찾으면 첫 단계로 찾은 하나만 그 에셋에 다시 임포트하고, 둘째 단계로 찾은 나머지는 새 에셋이다. UE에서 B를 A로 바꾼 뒤 redirector를 지우고 새 B를 만든 경우로, 9.2절 "redirector를 지운 뒤의 이름 변경은 새 에셋"이다. 첫 단계로 찾은 것이 없거나 둘 이상이면 모두 모호함이다. 이것을 막지 않으면 두 묶음 에셋이 한 Rocks 에셋을 덮어쓴다(리뷰 필수 1).
   - 모호함: 그 에셋은 `UnrealPathCollision`(세부 규칙 `AmbiguousExisting`, `AlreadyExists`, text = objectPath)으로 실패하고 단위 실패 전파(10.4절 15번)를 따른다. 묶음에 없는 Hard 대상의 조회가 모호하면 같은 진단을 내고 대상 클래스와 무관하게 `MissingDependency`로 단위를 실패시킨다(어느 쪽에 이을지 고를 수 없다).
   - 경로 판정: 기존 에셋에 다시 임포트하는 에셋은 대응 경로의 길이 초과로 실패하지 않는다(쓰지 않는 경로). 겹침 키 집합에는 새 에셋의 대응 경로와 기존 에셋의 지금 경로를 함께 넣고, 겹치는 새 에셋만 `UnrealPathCollision`(`PathCollision`, related = 겹친 기존 에셋 쪽 묶음 에셋)으로 실패시킨다. 남은 새 에셋의 대응 경로를 색인의 다른 에셋이 쓰면 `UnrealPathCollision`(새 세부 규칙 `PathTaken`, text = 경로)이다 — dry run이 plan-create로 보고한 뒤 실행이 덮어쓰거나 실패하지 않게 한다(리뷰 권장 4).
6. **계획 결과.** `ExchangeImportPlan`은 에셋마다 기존 에셋(`Existing(asset)` — id·경로, 없으면 null id)과 대상 경로(`TargetPath(asset)` — 기존 경로 또는 대응 경로)를 더 가진다. `--dry-run`은 계획까지만 하고 쓰지 않는다(잠금도 잡지 않는다).
7. **종류별 임포터 표(`ExchangeKindTable`).** 교환 클래스마다 임포터 `ExchangeKindImporter` 하나를 등록한다(같은 클래스 둘은 `AlreadyExists`). 임포터는 요청(묶음, 에셋 번호, 대상 경로, 기존 AssetId, 명령 옵션, 이번 실행에서 이미 임포트한 에셋의 AssetId 표, 서비스·잠금)을 받아 만든 에셋의 AssetId를 돌려주고 진단을 묶음 진단에 더한다. 표는 명령 문맥의 등록 hook(`ExchangeKindSetup` — A2f의 `AssetImporterSetup`과 같은 모양)이 채운다. 제품 Commandlet은 지금 등록하는 것이 없다(UX2b부터 텍스처·메시·스켈레톤·클립·머티리얼 순으로 더한다). 시험은 서비스에 쓰지 않는 시험 임포터를 등록한다. 요청 형태는 첫 실제 임포터(UX2b)가 UXR8–UXR11과 함께 다시 본다.
8. **실행 순서와 전파(`RunExchangeImport`).** 계획의 단위 순서(깊이, 가장 작은 objectPath)대로 Ready 단위의 구성원을 부른다. 에셋마다 결과는 고르지 않음·범위 밖·실패·건너뜀·임포트(새로 만듦 또는 재임포트)다.
   - 실패한 단위(계획)는 부르지 않는다.
   - 구성원이 둘 이상인 단위(Hard만의 순환 — 9.3절이 허용)는 다중 에셋 원자 저장(UXR11)이 없으므로 계획이 구성원 모두를 `UnsupportedCycle`(세부 규칙 `HardCycle`, `CycleDetected`)로 실패시키고 의존자에게 전파한다(실행이 아니라 계획에서 정해 dry run 결과가 실행과 같다 — 리뷰 필수 2). 같은 단위에 의존 실패가 있으면 그쪽(`MissingDependency`)이 먼저다(원인에 가깝다). 그래서 Ready 단위는 구성원이 하나다.
   - 클래스에 등록된 임포터가 없으면 `UnsupportedAsset` 경고(세부 규칙 `NoKindImporter`, text = 교환 클래스 이름)를 내고 건너뛴다.
   - 묶음 안 Hard 대상이 이번 실행에서 실패했으면 대상 클래스·기존 에셋과 무관하게 `MissingDependency`(세부 규칙 `FailedDependency`, related = 대상)로 실패한다 — 9.3절의 단위 실패 전파와 같고, 옛 버전의 기존 에셋에 조용히 잇지 않는다(리뷰 필수 2). 대상을 건너뛰었으면(임포터 없음) 기존 에셋이 있으면 그 id로 잇고, 없으면 묶음에 없는 대상과 같이 텍스처·`Material`·`MaterialInstanceConstant`는 `MissingUnrealDependency` 경고 뒤 계속, 그 밖은 `MissingDependency`다.
   - 임포터 계약: 새 에셋은 null이 아닌 id, 재임포트는 기존 id 그대로다. 어기면 그 에셋의 실패(진단 코드 없음, `InvalidArgument`)다.
   - 임포터 오류는 그 에셋의 실패이고 다음 에셋으로 넘어간다(9.3절 부분 결과). 할당 실패만 실행 전체를 멈춘다.
9. **명령 `asset-import-unreal`.** 문법은 10.2절에 공통 옵션 `--project=<경로>`·`--wait=<초>`를 더한 것이다. 같은 옵션 둘(`--only` 제외), 빈 값, 모르는 옵션(이름은 대소문자 구분), `--mesh-normals`의 값이 `unreal`·`rocks`가 아님, `--wait`가 다른 에셋 명령의 규칙(10진 숫자, 하루 이하)을 어김, 위치 인자 수가 하나가 아님은 사용법 오류(종료 코드 2)다. `--dry-run`은 잠금을 잡지 않는다(다른 쪽이 잠금을 쥐어도 된다). `--mesh-normals`는 요청 옵션으로 임포터에 넘긴다(쓰는 것은 UX3b). 손실 원본 옵션 `--allow-lossy-origin`은 두지 않는다(사용자 결정 2026-11-23 — JPEG 원본은 옵션 없이 받는다, 4.3절). 출력:
   - 표준 출력, 에셋마다 계획·실행 순서로: `asset-import-unreal: <동작> <objectPath> <대상 경로>[ <AssetId>]`. 동작은 `--dry-run`이면 `plan-create`(id 없음)·`plan-reimport`, 실행이면 `created`·`reimported`다. 실패는 `failed <objectPath> <진단 코드>`(코드가 없으면 원본 오류 이름), 건너뜀은 `skipped <objectPath> <unsupported|no-importer>`다.
   - 표준 오류: 묶음 진단마다 `asset-import-unreal: <코드>: <objectPath 또는 bundle>: <error|warning|info> <세부 규칙>[ <text>]`(진단 순서, 상한을 넘어 버린 수는 `dropped <수> diagnostics` 한 줄), 그 뒤 서비스 경고마다 `asset-import-unreal: <코드>: <텍스트>: warning`, 색인 캐시를 쓰지 못하면 `<오류>: <Intermediate 경로>: warning CacheNotWritten`.
   - 끝 줄(표준 출력): 실행이면 `asset-import-unreal: <만듦> created, <재임포트> reimported, <실패> failed, <건너뜀> skipped`, `--dry-run`이면 `<수> to create, <수> to reimport, <실패> failed, <건너뜀> skipped`. 종료 코드는 실패한 에셋이 없으면 0, 있으면 1이다. 묶음 읽기 전체 실패·`--only` 값이 묶음에 없음·계획 실패(잘못된 root)·종류별 임포터 등록 실패·세션 열기 실패(잠금 Busy 포함)는 그 실패 줄을 쓰고 1이다.
10. **명령 등록.** Commandlet 실행 파일이 UnrealExchange 플러그인 모듈에 의존하고(BuildTargets BD39 — Program은 켠 플러그인 모듈에 의존할 수 있다) 호스트가 `asset-import-unreal`을 등록한다. 명령 함수는 플러그인의 `RunAssetImportUnrealCommand`(문맥 `UnrealExchangeCommandContext` — 에셋 명령 문맥과 종류별 임포터 등록 hook)이고, EngineTests가 시험 임포터로 프로세스 안에서 부른다. 모듈 시작이 명령을 스스로 등록하는 길(UE commandlet 클래스 탐색 대응)은 모듈 로더 쪽 기능이 없어 두지 않았다.
11. **새 세부 규칙.** `AmbiguousExisting`·`PathTaken`(5번), `HardCycle`(8번), `NoKindImporter`(8번). 진단 코드는 기존 것(`UnrealPathCollision`·`UnsupportedCycle`·`UnsupportedAsset`·`MissingDependency`·`MissingUnrealDependency`)을 쓴다.
12. **시험과 coverage(UX1c).** 시험은 다음과 같다.
   - `ExchangeSelectionTests`: 닫힘, 선택 계획, 기존 에셋 충돌·경로 점유, `--only`와 전체 계획 대조.
   - `ExchangeImportRunTests`: 임포터 표, 순서, 실행 중 전파, 임포터 계약, dry run.
   - `ExchangeIndexLookupTests`: 실제 색인 위 두 단계, 중복 id, 경로 점유.
   - `ExchangeCommandTests`: 명령 줄 전부, 잠금, 캐시 쓰기 실패, 진단 버림, 쓰기 하나하나의 실패.
   - 제품 실행 파일의 C# 확인(`Rocks.Commandlet.Tests`).
   - 할당 실패는 계획·조회·실행의 모든 지점을 n번째 실패로 돈다.
   
   플러그인 Private coverage는 줄 99.4%·분기 99.1%·함수 100%다. 계획·실행·리더 파일은 모두 100%다. 닿지 않는 것은 사유를 남긴다.
   - `ExchangeCommand.cpp`의 할당 실패 경로(인자 목록·출력 줄 조립·단계 실패·임포터 등록·실행·캐시 쓰기의 OutOfMemory)는 닿지 않는다. 명령 하나가 세션 열기(색인 만들기)에서 수천 번 할당해 n번째 실패 반복이 너무 길다. 다른 에셋 명령 시험도 명령 단위 할당 실패를 돌지 않는다. 그 아래 함수(계획·조회·실행)의 할당 실패는 시험이 닿는다.
   - `ExchangeIndexLookup::PathTaken`의 같은 경로 키 둘(`AlreadyExists`)은 대소문자를 무시하는 파일 시스템(시험 플랫폼 Windows)에서 만들 수 없어 닿지 않는다.

### 10.6 종류별 임포트 공통(UX2b1)

설계일 2026-11-26(사용자 지시 "순서대로" — MeshAnimationAssets MA7 다음). 첫 실제 종류별 임포트(텍스처, 4.6절)가 쓰는 틀이고 UX3b 이후도 같은 틀을 쓴다. 10.5절 7번 "요청 형태는 첫 실제 임포터가 다시 본다"를 여기서 닫는다. 모두 권장안으로 정했다.

1. **두 층.** 종류별 임포터(`ExchangeKindImporter`)는 에셋 서비스 임포트를 부르는 얇은 층이다. 에셋 내용은 A2f 임포터(`AssetImporter`, 이름 `unreal_<종류>`)가 원본 byte(에셋 JSON·blob)에서 만든다. 그래서 ImportRecord가 9.2절대로 원본(역할 `asset`·`blob:<sha256>`)과 옵션을 담고, 묶음 파일이 그 자리에 있으면 일반 `asset-reimport`도 같은 결과를 낸다(UE `FReimportHandler`가 원본 파일을 다시 읽는 것과 같다). 버린 대안: 종류별 임포터가 출력 객체를 직접 채우기 — ImportRecord·재임포트 길이 없고 A2f 저장·되돌리기 규칙을 다시 만들어야 한다.
2. **원본 입력.** 역할 `asset` = `<묶음>/<asset.file>`, 역할 `blob:<64자 hex>` = `<묶음>/blobs/<앞 2자>/<hex>.bin`(3.1절)이고 blob은 `asset.blobs` 순이다. 명령의 상대 묶음 경로는 작업 디렉터리 기준 절대 경로로 바꿔 읽는다(`asset-import` 원본과 같은 규칙 — 서비스는 절대 원본 경로를 받는다). 원본 루트는 파일 임포트와 같다: 묶음 디렉터리가 프로젝트 원본 루트(AssetSystem `AssetSources.json`) 아래면 상대 경로로 기록하고, 아니면 절대 경로로 기록하고 서비스 경고 `AbsoluteSourcePath`를 낸다. 새 에셋은 `AssetService::Import`(임포터 이름), 기존 에셋은 `AssetService::Reimport(replaceSources = 참)`(원본 목록 전체 교체 — UE에서 고쳐 blob 집합이 바뀌어도 된다)다. 서비스 실패 진단은 묶음 진단(asset = 이 에셋)으로 옮기고(코드·오류 그대로 — 코드가 없으면 `InvalidSource`, 세부 규칙 `ServiceFailure`, text = 대상 경로), 서비스 경고는 `request.warnings`에 모인다(10.5절 9번 출력).
3. **임포트 옵션 struct `UnrealExchangeImportOptions`**(UnrealExchange 모듈의 `ROCKS_STRUCT` — A2f 임포터 `optionsType`):
   - `objectPath`(String) — 이 에셋의 objectPath.
   - `formerPaths`(Array<String>) — 묶음 `redirects` 가운데 `to`가 이 objectPath인 `from`(byte 순).
   - `references`(OrderedMap<String, AssetId>) — 참조로 걸 대상: 이 에셋 의존마다 묶음 안이면 `request.resolved[target]`, 묶음 밖이면 계획이 찾은 기존 에셋(4번). 못 찾은 대상(null id)은 넣지 않는다. 키는 의존의 objectPath(redirect를 풀기 전 원문 — 설정 덤프의 참조 값과 같다), 의존 순(objectPath byte 순)이다.
   옵션은 서비스 대입 텍스트로 넘긴다(`objectPath=<값>`·`formerPaths=<목록>`·`references=<k:v 목록>` — ReflectionText 목록 escape 표로 `,`·`;`·키의 `:`를 escape한다). ImportRecord options에 남아 일반 재임포트가 그대로 쓴다. 필드 이름은 출력 타입의 노출 속성과 겹치지 않아야 한다(A2f 등록 규칙 — 텍스처 클래스에는 없다). 종류별 옵션(`--mesh-normals` 등)은 그 단계가 필드를 더한다.
4. **계획의 묶음 밖 대상 id.** 계획이 찾은 묶음 밖 Hard 대상의 기존 에셋(`Found`)을 의존마다 남긴다: `ExchangeImportPlan::ExternalTarget(asset, dependency)`(dependency는 `asset.dependencies` 번호 — 묶음 안 대상·못 찾음·모호함·Soft는 null id). 계획은 이미 그 조회를 하므로(10.5절 5번) 실행이 다시 찾지 않는다(dry run과 같은 결과). Soft 참조 연결은 후속이다(텍스처 설정에는 없다).
5. **에셋 문서 다시 읽기.** A2f 임포터는 원본 byte를 믿지 않는다(묶음 읽기와 임포트 사이, 또는 일반 재임포트 때 파일이 바뀌었을 수 있다).
   - `ReadExchangeAssetDocument(bytes, allocator, out, diagnostics)`: 묶음 리더의 에셋 JSON 검사(10.4절 5–9·11번 — 키·`objectPath`·`class`·`ueClass`·의존·플러그인 진단·`settings`·`data` 객체·blob 참조 수집·플러그인 오류 진단)를 byte 입력으로 한다. bundle.json 대조(`ManifestMismatch`)만 빠지고, `className`·`exchangeClass`·`objectPath`는 문서에서 채운다. 의존의 `target`은 `ExchangeNoIndex`다. 묶음 리더도 파일을 읽은 뒤 이 검사를 부른다(동작 불변 — 같은 진단).
   - `VerifyExchangeBlob(ref, bytes)`: 크기·SHA-256·가변 배치 검사(`LoadExchangeBlob`이 파일을 읽은 뒤 하는 것과 같은 검사 단계 `FinishBlobCheck`를 쓴다). blob 파일 경로는 `AppendExchangeBlobPath(directory, digest)`(묶음 리더와 공용)다.
6. **A2f 임포터 공통 검사 순서**(실패는 `context.Fail` — 코드 / 오류 / detail):
   1. 원본 `asset`을 문서로 읽는다: 실패 코드는 문서 읽기의 마지막 error 진단 코드(구문·형식은 `CorruptBundle`, 플러그인 error 진단은 `InvalidSource` — 없으면 `CorruptBundle`) / 그 오류(진단의 세부 규칙은 버린다 — 한계: 서비스 진단은 코드·detail만 담는다).
   2. 문서 교환 클래스가 임포터의 클래스와 같다: 아니면 `InvalidSource` / `InvalidArgument`.
   3. 옵션 `objectPath`가 문서 objectPath와 같다: 아니면 `InvalidImportOption` / `InvalidArgument`.
   4. 문서 blob 참조마다 역할 `blob:<hex>` 원본이 있다: 없으면 `MissingSource` / `NotFound`(detail = blob 번호). 참조하지 않는 blob 역할 원본이 있으면 `InvalidImportOption` / `InvalidArgument`(detail = 원본 번호).
   5. blob마다 `VerifyExchangeBlob`: 실패 `CorruptBundle` / `CorruptData`(detail = blob 번호).
7. **태그(UXR8 — A2f3a `AddTag`).** 교환 임포터는 모두 태그 접두 `UnrealExchange`(AssetSystem 7.1절 21 A2f5 `tagPrefix` — 임포터 이름과 다른 접두)를 쓴다. `UnrealExchange.ObjectPath` = 옵션 objectPath. `UnrealExchange.FormerPaths` = 옵션 formerPaths ∪ 옛 기록의 FormerPaths ∪ 옛 기록의 ObjectPath에서 지금 objectPath를 뺀 것(이름을 되돌린 경우 — 지금 이름은 옛 경로가 아니다) — 값마다 한 쌍, 같은 값은 한 번(문맥이 byte 순으로 쌓는다). 그래서 UE에서 두 번 이름을 바꾸고 redirector를 하나 지워도 앞 경로로 다시 찾는다(9.2절). 태그 수가 `MaxImportTags`를 넘으면 `AddTag` 오류로 실패한다(옛 경로가 255개 쌓이는 경우 — 한계).
8. **설정 번역.** UE 설정 덤프(`settings`)를 Rocks 리플렉션 JSON(ReflectionText `ImportJson` 표기)으로 바꿔 출력 객체에 `ImportJson`하고 게시한다. 클래스마다 행 표(UE 키 → 처리)를 두고(텍스처는 4.6절 3번), 키 짝맞추기는 `ImportJson`의 대소문자 무시를 그대로 쓴다. 처리 종류:
   - `Same`: 키의 bool 접두 `b`(뒤가 대문자)만 떼고 값 그대로(`bFlipGreenChannel` → `FlipGreenChannel` → 필드 `flipGreenChannel`).
   - `Enum`: UE 값 이름의 접두(첫 `_`까지, 접두가 대문자·숫자만일 때 — `TC_HDR_Compressed` → `HDR_Compressed`, `TEXTUREGROUP_World` → `World`)를 떼고, 접두가 없으면 그대로(`Default`·`GPU`). enum에 없는 값이 정수로 온 것(11.7절 13번)은 그대로 넘겨 `ImportJson`이 거부한다.
   - `Guid`: `{A,B,C,D}`(int32 넷 — UE FGuid)를 UE `DigitsWithHyphens` 순서(`A-B상위-B하위-C상위-C하위D`, 소문자)의 36자 문자열로 바꾼다.
   - `PerPlatformFloat`: `Default` → `defaultValue`. `PerPlatform`이 비어 있지 않으면 버리고 `UnsupportedSetting` 경고(UE 플랫폼 이름과 TargetProfile 이름의 대응은 후속).
   - `Reference`·`ReferenceArray`: `{ "objectPath" }`를 resolver 텍스트(objectPath 그대로)로 바꾼다. resolver는 옵션 `references`에서 id를 찾아 `context.OpenAsset(id)`로 Hard 참조를 건다(대상 타입은 codec이 필드 타입으로 확인). `references`에 없는 objectPath(묶음에도 기존 에셋에도 없는 대상 — 계획이 `MissingUnrealDependency`를 이미 냈다)는 null로 둔다. 배열은 null 원소를 뺀다.
   - `Struct`: 멤버 행 표로 재귀한다(값 struct — `SourceColorSettings`의 enum 멤버 접두 떼기).
   - `Ignore`: 설정이 아닌 UE 멤버(`LightingGuid`·`AssetImportData` 등 — 버림).
   - `Unsupported`: Rocks 필드가 없는 UE 설정이다. 값이 UE 기본값과 다르면 `UnsupportedSetting` 경고를 내고 버린다.
   표에 없는 키는 `UnsupportedSetting` 경고(detail = `ExchangeUnknownSettingDetail`)로 버린다(UE 버전이 늘린 프로퍼티). `UnsupportedSetting` 경고의 detail은 그 클래스 표의 행 번호다(공개 표 — 도구가 UE 이름으로 바꿔 보여 줄 수 있다). `ImportJson` 진단(값 형식·범위·모르는 키)은 `InvalidSource` / `CorruptData` 실패다(덤프 내용이 필드와 맞지 않음 — 표가 맞는지는 시험이 엔진 콘텐츠 묶음으로 막는다). 진단이 없는 `ImportJson` 오류는 그대로 전한다.
9. **모듈 의존.** UnrealExchange가 ReflectionText(`ImportJson`), Texture(출력 타입)에 공개 의존한다. 제품 Commandlet은 A2f 임포터 등록(`AssetImporterSetup`)과 종류별 임포터 등록(`ExchangeKindSetup`)을 `ProductAssetSetup`(MeshAnimationAssets 13.1절 1)에 더한다.
10. **새 진단.** 코드 `UnsupportedSourceLayout`(오류)·`GammaMismatch`(경고)를 AssetRuntime 진단 표에 더한다(AssetSystem 4.6절 표에 이미 있다 — 10.3절). 세부 규칙 `ServiceFailure`(2번)를 더한다.
