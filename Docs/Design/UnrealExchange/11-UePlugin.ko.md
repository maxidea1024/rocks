## 11. UE 플러그인 `RocksExport`

### 11.1 위치와 빌드

```text
Extras/UnrealExport/RocksExport/
  RocksExport.uplugin
  Source/RocksExport/RocksExport.Build.cs       (UE ModuleRules)
  Source/RocksExport/Public|Private/…
  README.md                                     설치(UE 프로젝트 Plugins/에 복사 또는 링크)·사용법
```

- `Extras/`는 Rocks 빌드 규칙 glob(Build/Rules/Rocks.Engine.Rules.csproj:13-16), 정책 검사 루트(Programs/Rocks.Tooling.Checks/PolicyChecks.cs:61-64), C# 서식 루트(FormatCheck.cs:26) 어디에도 들지 않는다. Rocks 빌드와 검사는 이 코드를 보지 않는다.
- **경계 정책 검사 `foreign-engine-boundary`**(요구 UXR5, 구현 2026-11-20 — `PolicyChecks.ForeignEngineBoundary`). 두 엔진의 빌드 파일이 서로의 자리에 들어가는 실수를 막는다. Rocks 모듈을 `Extras/`에 두면 Rocks 빌드가 조용히 빼먹고, UE 플러그인을 Rocks 규칙 경로에 두면 Rocks 규칙 프로젝트가 UE `ModuleRules`를 컴파일하다 원인이 흐린 오류로 멈추기 때문이다.
  - `Extras/` 아래: Rocks 서술자(`.rocksproject`·`.rocksplugin`)가 있으면 실패. 규칙 파일(`*.Build.cs`·`*.Target.cs`)이 Rocks 빌드 네임스페이스(`Rocks.Build`)를 참조하면 그 줄에서 실패.
  - Rocks 규칙 경로(엔진 규칙 프로젝트가 모으는 `Core/Source`·`Engine/Source`·`Engine/Plugins`·`Programs`(C# 도구 `Programs/Rocks.*` 제외)와 템플릿 규칙 프로젝트가 모으는 `Templates`) 아래: UE 서술자(`.uplugin`·`.uproject`)가 있으면 실패. 규칙 파일이 UE 빌드 네임스페이스(`UnrealBuildTool`)를 참조하면 그 줄에서 실패.
  - 네임스페이스 판정은 주석·문자열을 지운 텍스트로 한다(설명 주석에서 상대 엔진 이름을 언급하는 것은 허용). 서술자 확장자는 대소문자를 무시한다. `Extras/`가 없으면 그쪽 검사는 할 일이 없다.
- 코드 규약은 UE 코딩 표준(F·U 접두사, UE 컨테이너)을 따른다. 손으로 쓴 주석은 한국어, 로그·진단 메시지는 영어다(Rocks 규약과 같은 언어 경계).
- 지원 UE 버전은 5.8 하나다. `.uplugin`의 `EngineVersion`을 5.8로 두고, 코드는 `ENGINE_MAJOR_VERSION`·`ENGINE_MINOR_VERSION`이 다르면 `#error`로 멈춘다(메시지 영어).
- 모듈: `Type: "Editor"`, `LoadingPhase: "Default"`. `Editor` 타입은 `GIsEditor`일 때 로드되고 commandlet도 `GIsEditor`를 켜므로 commandlet에서 로드된다(RT/Projects/Private/ModuleDescriptor.cpp:724-742, RT/Launch/Private/LaunchEngineLoop.cpp:2223). 의존 모듈: UnrealEd, AssetRegistry, ToolMenus, ContentBrowser, MeshDescription, StaticMeshDescription, SkeletalMeshDescription, MeshBuilder, MaterialEditor, MaterialBaking, Json. 각 모듈은 그것을 쓰는 단계에서 더한다(UX1b 목록과 JsonUtilities 제외 — 11.7절 1번).

### 11.2 진입점

| 진입점 | 동작 |
|---|---|
| 콘텐츠 브라우저 우클릭 "Export to Rocks…" | `UToolMenus`로 `ContentBrowser.AssetContextMenu.<클래스>` 메뉴를 확장한다(Engine/Source/Editor/ContentBrowser/Private/AssetContextMenu.cpp:407-435). 상위 클래스 메뉴(`Texture`·`StaticMesh`·`SkeletalMesh`·`Skeleton`·`AnimSequence`·`MaterialInterface`·`MaterialFunctionInterface`·`MaterialParameterCollection`)에 한 번씩 등록한다. 선택은 `UContentBrowserAssetContextMenuContext::GetSelectedAssetsOfType`으로 얻는다. 출력 디렉터리와 옵션 대화상자를 띄운다 |
| commandlet `-run=RocksExport` | `URocksExportCommandlet::Main`(E/Classes/Commandlets/Commandlet.h:103). 인자 `-Assets=<경로 또는 /Game/폴더/...>` `-Out=<묶음 디렉터리>` `-Closure`(기본 켬)/`-NoClosure` `-BakeMaterials` `-BakeSize=<n>`. 굽기에는 `-AllowCommandletRendering`도 필요하다. 시작 때 `IAssetRegistry::SearchAllAssets(true)`로 스캔을 끝낸다(IAssetRegistry.h:809) |

UE `UExporter` 계열(콘텐츠 브라우저 Export 대화상자, E/Classes/Exporters/Exporter.h)은 쓰지 않는다(결정 UX-D7).

### 11.3 SHA-256

UE Core의 `FGenericPlatformMisc::GetSHA256Signature`는 구현이 없다(`checkf(false, …)` — RT/Core/Private/GenericPlatform/GenericPlatformMisc.cpp:2019-2023). 공개 SHA-256 API는 Experimental 플러그인 PlatformCrypto(OpenSSL)뿐이고, 그 밖에는 모듈 내부의 OpenSSL 직접 호출과 자체 구현만 있다. Experimental 플러그인이나 ThirdParty OpenSSL 링크에 기대지 않도록 플러그인 안에 FIPS 180-4 SHA-256을 직접 둔다(200줄 안팎). NIST 시험 벡터로 자기 시험을 한다.

### 11.4 숫자 서식

float는 9자리, double은 17자리 유효숫자(`%.9g`·`%.17g`)로 쓴다. 다시 읽었을 때 같은 비트가 된다(최단 표기는 아니다). 같은 상태는 같은 bytes가 된다. Rocks 리더는 Core의 정확한 숫자 파서로 읽는다(A0-C).

### 11.5 UPROPERTY 덤프

노드·notify·설정 덤프는 자체 writer(`FRocksPropertyDumper`)로 한다. `FJsonObjectConverter`를 쓰지 않는 이유는 둘이다. 기본으로 키 첫 글자를 바꾸고(`StandardizeCase` — RT/JsonUtilities/Private/JsonObjectConverter.cpp:449-452), 숫자를 UE writer 서식으로 내보낸다. 규칙:
- 키는 UE 프로퍼티 이름 그대로.
- transient·deprecated 프로퍼티는 건너뛴다.
- `FExpressionInput`은 8.1절 `inputs`로 옮기고 `properties`에서 뺀다.
- 객체 참조는 `{ "objectPath": … }`. Outer가 덤프 대상인 하위 객체만 값으로 펼친다.
- `TMap`·`TSet`은 키 문자열 순 정렬.
- 숫자는 11.4절.

### 11.6 클래스 구성

| 클래스 | 책임 |
|---|---|
| `FRocksExportModule` | 메뉴 등록(`UToolMenus::RegisterStartupCallback`) |
| `URocksExportCommandlet` | 명령행 진입점 |
| `FRocksBundleWriter` | 디렉터리·`bundle.json`·에셋 JSON 쓰기, blob 저장(내용 주소, 이미 있으면 건너뜀), 숫자 서식 |
| `FRocksSha256` | 11.3절 |
| `FRocksTextureExtractor` / `FRocksStaticMeshExtractor` / `FRocksSkeletonExtractor` / `FRocksSkeletalMeshExtractor` / `FRocksAnimSequenceExtractor` / `FRocksMaterialExtractor` | 종류별 추출(4–8절) |
| `FRocksMeshDescriptionWriter` | `FMeshDescription` → 교환 형식 `MeshSource`(정적·스킨 공용) |
| `FRocksPropertyDumper` | 11.5절 |
| `FRocksExportClosure` | AssetRegistry 의존 닫힘·redirector 풀기 |

### 11.7 구현에서 정한 점(UX1b)

UX1b 구현(`Extras/UnrealExport/RocksExport/` — 플러그인 골격·commandlet·메뉴·writer·SHA-256·숫자 서식·UPROPERTY 덤프·닫힘·redirector·에셋 JSON 공통부) 중 3·9·11절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **모듈 의존은 쓰는 단계에서 더한다.** UX1b는 AssetRegistry·ContentBrowser·Core·CoreUObject·DesktopPlatform(디렉터리 대화상자)·Engine·Json(자기 시험이 결과를 UE 파서로 다시 읽는다)·Projects(플러그인 버전)·Slate·SlateCore·ToolMenus·UnrealEd다. 11.1절 목록의 메시·머티리얼·굽기 모듈은 UX2 이후 그것을 쓰는 단계에서 더한다. JsonUtilities는 쓰지 않는다(`FJsonObjectConverter`를 쓰지 않으므로 — 11.5절).
2. **클래스 구성 추가.** 11.6절 표에 더해 `FRocksJsonWriter`(JSON 텍스트·숫자 서식 — writer와 분리해 덤프·정렬 키도 같은 서식을 쓴다)와 `FRocksExporter`(commandlet과 메뉴가 함께 쓰는 내보내기 흐름)를 둔다. 자기 시험은 `RocksExportSelfTest`(함수 묶음)와 그 시험 타입(`FRocksSelfTestStruct`)이다.
3. **UX1b는 에셋을 로드하지 않는다.** `class`·`ueClass`는 AssetRegistry의 클래스 경로로 정하고 `settings`·`data`는 빈 객체다. 종류별 본문(4–8절)이 로드를 요구하는 단계(UX2 이후)에서 로드한다. 교환 클래스는 실제 클래스부터 상위로 올라가며 처음 만나는 2절 클래스 이름이다(파생 클래스는 `ueClass`로 구분 — 3.5절). 2절 클래스를 상속해도 2절이 범위 밖으로 둔 IES `TextureLightProfile`은 범위 밖이다. 클래스를 찾지 못하면(로드되지 않은 Blueprint 클래스) 클래스 경로의 이름을 쓰고 범위 밖이다.
4. **의존 목록의 출처.** 3.5절은 `dependencies`를 "에셋 JSON 안에서 참조한 객체"로 정한다. 본문이 빈 UX1b에서는 AssetRegistry 패키지 의존(9.3절 — Hard·Soft 질의)을 쓴다. 의존 패키지의 에셋 하나하나가 항목이고(objectPath·교환 클래스·kind), `/Script/`·`/Memory/` 패키지와 자기 자신은 뺀다. 본문을 쓰는 단계는 덤프가 모은 참조(`FRocksPropertyDumper::GetReferences`)를 합친다(합집합, 둘 다면 Hard). 패키지 의존은 저장된 참조 전체라 본문 참조를 포함하므로 Rocks 임포트 순서(10.4절 14번)의 간선이 지금부터 맞다.
5. **닫힘과 범위 밖 선택.** 닫힘(9.3절)은 범위 안 에셋에서만 Hard 의존을 따라가고, 범위 안 대상만 더한다. 범위 밖 대상은 의존 항목으로만 남는다(Rocks 리더가 순서·누락 판정에서 뺀다 — 10.4절 9번). 사용자가 직접 고른 범위 밖 에셋(레벨·Blueprint 등)은 `assets`에 그 클래스 이름으로 넣고 에셋 JSON 공통부(의존 포함)와 `UnsupportedAsset` 경고 진단을 쓴다. Rocks 리더는 그 JSON을 읽지 않고 `UnsupportedAsset` 경고를 낸다. 범위 밖 에셋에서는 닫힘을 따라가지 않는다.
6. **선택 문법.** `-Assets=`는 쉼표·세미콜론으로 나눈 목록이다. 항목은 객체 경로(`/Game/A.A`), 패키지 경로(`/Game/A` — 그 패키지의 에셋 전부), 폴더 재귀(`/Game/Dir/...`)다. 맞는 에셋이 없으면 실패다. AssetRegistry 조회는 기본 필터를 따른다(에디터에서 Blueprint 생성 클래스 `_C` 항목을 뺀다 — 그러지 않으면 Blueprint 패키지가 에셋 둘로 보인다). 에디터 메뉴는 선택한 에셋 전부를 넘긴다(11.2절의 `GetSelectedAssetsOfType` 대신 `SelectedAssets` — 범위 밖 선택도 위 5번 규칙으로 다룬다).
7. **redirector.** 대상은 AssetRegistry의 redirector 태그 `DestinationObject`로 읽고, 태그가 없으면 redirector를 로드해 `DestinationObject`를 본다. 사슬은 끝까지 따라가고 `RedirectMaxChain`(= 64) 단계를 넘거나 순환하면 끊어진 사슬이다. 지나간 redirector마다 `{ from: 그 경로, to: 사슬 끝 }`을 적는다(사슬 없음 — 10.4절 10번). 의존 항목의 `objectPath`는 사슬 끝을 쓴다(리더는 `from`도 풀므로 어느 쪽이든 되지만 하나로 정한다). 선택한 redirector가 끊어졌으면 내보내기 실패, 의존 쪽이면 그 의존을 빼고 `BrokenRedirector` 경고 진단이다.
8. **에셋 여럿인 패키지.** 형식 v1은 에셋 파일 이름(10.4절 8번)과 Rocks 경로(9.1절)를 패키지 경로에서 만들므로 한 패키지에 에셋이 둘 이상이면 담을 수 없다. 그런 패키지의 에셋은 내보내지 않고(경고 로그), 그것을 Hard로 참조한 에셋에 `MultiAssetPackage` 경고 진단을 단다. 의존 항목은 남으므로 Rocks 임포트는 9.3절 누락 규칙(머티리얼·텍스처는 경고, 그 밖은 오류)을 따른다. UE 5.8 엔진 콘텐츠에도 있다(`/Engine/BasicShapes/M_Cube_WorldGridMaterial_8D67460644F5AEE976EC80B26230FFEB` — 머티리얼과 텍스처). 버린 대안: 파일·경로를 objectPath로 만드는 형식 변경 — 9.1절 대응과 리더(10.4절 8번)를 함께 바꿔야 하고, UE 5 콘텐츠는 패키지당 에셋 하나가 원칙이라 실제 콘텐츠가 요구할 때 `formatVersion`과 함께 정한다.
9. **플러그인 진단 코드.** 에셋 JSON `diagnostics`의 `code`는 `UnsupportedAsset`(범위 밖 선택), `MissingUnrealPackage`(의존 패키지가 AssetRegistry에 없음), `BrokenRedirector`, `InvalidObjectPath`(의존 경로가 10.4절 7번 문법을 어겨 적지 않음), `MultiAssetPackage`이고 모두 `warning`이다. 종류별 추출 코드는 각 장이 정한다(텍스처 `NoSourceData`·`UnsupportedSourceFormat`·`SourceReadFailed` error — 4.5절 1번, Custom 노드 `CustomIncludeSkipped` info — 8.6절 2번, 굽기 `BakeSkipped` info·`BakeFailed` warning — 8.9절 1번). 묶음에 들 에셋 자신의 경로가 문법을 어기면 내보내기 실패다. Rocks 리더는 `code` 문자열을 해석하지 않는다(형식만 본다).
10. **`bundle.json` 값.** `engineVersion`은 `FEngineVersion`의 주.부.패치(`5.8.4`)다. `engineCommit`은 설치판(Launcher) 엔진에 git commit이 없으므로 Perforce changelist 번호(10진, 5.8.4 = `58902052`)를 쓰고, 0이면(소스 빌드) `unknown`이다. `substrate`는 `r.Substrate` 값, `retainJpegFormat`은 Editor ini `[TextureImporter] RetainJpegFormat`(없으면 UE 기본 True)이다. `environment`는 굽기(UX6) 전이라 쓰지 않는다. `createdUtc`는 초 단위 ISO 8601 UTC(`2026-11-22T02:59:17Z`)다.
11. **JSON 텍스트.** 들여쓰기 두 칸, 줄바꿈 LF, 끝에 LF 하나, 빈 객체·배열은 `{}`·`[]`다. 문자열은 따옴표·역슬래시·제어 문자(U+0000–U+001F — 짧은 escape 5종, 나머지 `\u00XX`)만 escape하고 나머지는 UTF-8 그대로다. 짝 없는 UTF-16 surrogate는 U+FFFD로 바꾼다(UE 문자열은 UTF-16이라 생길 수 있고, Rocks 리더는 잘못된 UTF-8을 받지 않는다). 정렬(3.2절·10.4절 6번)은 code point 비교로 한다(UTF-8 byte 순과 같다).
12. **숫자 서식 구현.** `std::to_chars(…, std::chars_format::general, 9 | 17)`이다. C 표준이 `printf`의 `%.9g`·`%.17g`와 같은 결과를 정하고 로캘을 보지 않는다(UE `FString::Printf`·CRT `printf`는 프로세스 로캘의 소수점을 따를 수 있다). 정수는 `std::to_chars` 10진이다.
13. **UPROPERTY 덤프 세부(11.5절).** 키 순서는 `TFieldIterator` 순(그 클래스 선언부터 상위 쪽), 고정 배열(`ArrayDim` > 1)은 배열이다. enum은 이름(`GetNameStringByValue`), enum에 없는 값은 정수다. `FText`는 원문(source string — 표시 문자열은 언어마다 다르다)이다. `FSoftObjectPath` 구조체와 soft 참조는 `{ "objectPath" }`다. 하위 객체(덤프 루트를 Outer 사슬에 둠)는 처음 만날 때만 `{ "subobject": 루트 기준 경로, "class": 클래스 경로, "properties": {…} }`로 펼치고 다시 만나면 `{ "subobject" }`만 쓴다(공유·순환 참조). `TSet`은 배열, `TMap`은 `{ "key", "value" }` 객체의 배열이다. 둘 다 원소(키)의 compact JSON 표기의 UTF-8 byte 순으로 정렬한다(키가 문자열이 아닐 수 있어 같은 규칙을 모든 키 타입에 쓴다). delegate·field path·optional 같은 그 밖의 타입은 UE `ExportText` 문자열이다. 참조 수집은 `/Script/`·`/Memory/`·transient 패키지를 뺀다.
14. **원자적 쓰기.** 모든 파일을 `<출력>.staging`에 쓰고, `bundle.json`을 마지막에 쓴 뒤 기존 출력을 `<출력>.previous`로 옮기고 staging을 출력 이름으로 바꾼 다음 옛것을 지운다. 출력 이름에는 언제나 완성된 묶음만 있다. 기존 출력이 묶음(`bundle.json` 있음)도 빈 디렉터리도 아니면 덮지 않고 실패한다(오타로 다른 디렉터리를 지우지 않게). 실패·중단이면 staging을 지우고 출력은 그대로다. blob은 한 번의 내보내기 안에서 같은 hex를 한 번만 쓰고, 고정 배치는 `bytes` = 원소 크기 × components × count를 쓰기 전에 확인한다(10.4절 11번과 같은 규칙).
15. **commandlet 인자.** `-Closure`와 `-NoClosure`를 함께 주면 실패다. `-BakeMaterials`·`-BakeSize`는 굽기 단계(UX6) 전이라 실패로 거부한다(조용히 무시하지 않는다). `-SelfTest`는 자기 시험만 돌린다. 그 밖의 스위치는 엔진 스위치(`-unattended` 등)라 보지 않는다. 종료 코드는 성공 0, 실패 1이다(`UseCommandletResultAsExitCode`).
16. **에디터 메뉴.** 2절 상위 클래스 메뉴 8개에 동적 항목 "Export to Rocks..."를 단다. 옵션 대화상자는 지금 출력 디렉터리 선택(`IDesktopPlatform::OpenDirectoryDialog`)뿐이고 닫힘은 켠다. 굽기 같은 옵션은 그 단계에서 더한다. AssetRegistry가 검색 중이면 끝날 때까지 기다린다. 결과는 메시지 대화상자로 알린다.
17. **자기 시험(12.2절).** commandlet `-SelfTest`와 UE Automation `RocksExport.*` 6개(`Sha256`·`NumberFormat`·`JsonWriter`·`PropertyDump`·`BundleWriter`·`ExportDeterminism`)가 같은 함수를 부른다. SHA-256은 FIPS 180-4 예제 4개와 백만 `a`, 1 byte씩·997 byte 조각·블록 경계 길이(55·56·63·64·65·119·120)의 분할 일치다. 숫자 서식은 표기 16개(printf 기대값), 비유한값, 결정적 비트 패턴 2만 개의 왕복(`std::from_chars`로 다시 읽어 비트 비교), 독일어 로캘에서의 같은 표기다. 덤프는 시험 구조체의 기대 JSON 전체 비교·참조 수집과 엔진 에셋(정육면체 메시·기본 머티리얼) 두 번 덤프의 일치·UE 파서 읽기다. writer는 blob 경로·중복·크기 검사, 교체·중단·묶음 아닌 디렉터리 보호다. 결정성은 `/Engine/BasicShapes/Cube`를 두 번 내보내 `createdUtc` 줄 말고 bytes가 같은지와 형식(키·정렬·파일 대응)이다. `prepared` 추출이 원본 설정을 바꾸지 않는지는 UX3a에서 더한다.
18. **Rocks 쪽 왕복 확인.** EngineTests에 선택 시험 `UnrealExchange_ExternalBundle`(`Tests/ExchangeExternalBundleTests.cpp`)을 둔다. 환경 변수 `ROCKS_UNREAL_EXCHANGE_BUNDLE`이 묶음 디렉터리를 가리키면 리더로 읽고 임포트 순서를 계산해 결과를 출력하며, 묶음 수준 오류 없음·오류 진단 없음·범위 안 에셋 Ready·범위 밖 Unsupported·실패한 저장 단위 없음을 확인한다. 변수가 없으면 SKIP을 출력하고 통과한다(기본 CI 불변). 환경 변수 읽기는 TestSupport `test::ReadEnvironmentVariable`(Win64 `GetEnvironmentVariableW`, POSIX `getenv`)이다.
19. **빌드 환경.** `RunUAT BuildPlugin -TargetPlatforms=Win64`는 경고 없이 빌드된다. 확인한 PC에서 UBT는 VS 2026의 MSVC(폴더 14.50.35717, 컴파일러 14.50.35740 — `Windows_SDK.json`의 금지 구간 14.50.0–14.50.35722 밖, 선호 범위 안)와 Windows SDK 10.0.22621.0을 골랐다. 사용자 전역 UBT 설정(`%APPDATA%\Unreal Engine\UnrealBuildTool\BuildConfiguration.xml`)에 UE 5.8이 모르는 값(`VisualStudio2019`)이 있으면 UBT가 설정 읽기에서 멈춘다. 그 PC에서는 빌드 프로세스에만 `USERPROFILE`·`APPDATA`·`LOCALAPPDATA`를 작업 폴더로 바꿔 그 파일을 보지 않게 했다(파일은 다른 UE 버전용이라 고치지 않았다).

20. **golden 묶음 생성 명령(개정 15).** 12.3절. 생성 스크립트는 `Extras/UnrealExport/RocksExport/Scripts/Golden/`에 있다.
21. **옛 경로 참조(개정 15).** 덤프가 모은 참조 경로가 닫힘이 푼 redirect의 `from`이면 의존에는 그 `to`(대상 경로)와 대상의 교환 클래스를 적는다. 본문(`settings`·`data`)의 `{ "objectPath" }`는 덤프한 값 그대로다(UE가 로드 때 soft 경로를 고쳤으면 새 경로, 아니면 옛 경로 — 옛 경로는 bundle.json `redirects`가 잇는다). 전에는 옛 경로를 그대로 의존에 적어 클래스가 `ObjectRedirector`가 되고, 대상이 묶음 안에 있으면 리더가 클래스 불일치로 참조 에셋을 손상 처리했다. redirect 표에 없는 redirector(닫힘이 만나지 못한 참조)는 warning `UnresolvedRedirector`로 알리고 의존에 넣지 않는다. 자기 자신 참조는 의존에 넣지 않는다(4번 — Rocks 시험의 참조 검사도 받는다).
22. **weak·lazy 참조(개정 15).** `TWeakObjectPtr`·`TLazyObjectPtr` 프로퍼티는 UE가 대상을 함께 로드하지 않으므로 의존에 Soft로 모은다. 값은 로드된 객체의 경로이고, lazy 참조의 대상이 로드되지 않았으면 `null`이다(v1 한계).
23. **예약 키(개정 15).** 3.3절. 덤프가 키로 쓰는 프로퍼티 이름이 `blob`·`objectPath`·`subobject`와 같으면 `@`를 붙인다.
24. **문자열 변형·optional(개정 15).** `FUtf8String`·`FAnsiString` 프로퍼티는 문자열, `TOptional<T>`는 값이 있으면 그 값(같은 규칙 — float도 11.4절 서식), 없으면 `null`이다. 그 밖의 드문 프로퍼티(delegate·field path·Verse 문자열 등)는 UE ExportText 문자열이며 서식이 11.4절을 따르지 않는다(v1 한계 — 만나면 대응을 더한다).
25. **옛 staging·previous 디렉터리(개정 15).** `<출력>.staging`·`<출력>.previous`가 이미 있으면 맨 위 항목이 묶음 이름(`bundle.json`·`assets`·`blobs`)뿐일 때만 지운다. 그 밖의 파일이 있으면 사용자 디렉터리일 수 있으므로 지우지 않고 내보내기를 거부한다.
26. **자기 시험 추가(개정 15).** `RocksExport.MeshWriterEdgeCases`(commandlet `-SelfTest`에도 포함)가 메시 설명을 직접 만들어 writer 경계 사례를 본다: 퇴화 다각형(자기 변을 뺀 변 수, 변 속성 길이, 복원한 둘레 코너, `DegenerateEdgesDropped`), 정점 영향 14개(상위 12개 본·합 65535·`TooManyInfluences`), 정점 0개 메시, 다채널 Name 속성(채널 우선 배치). `RocksExport.PropertyDump`는 예약 키 `@blob`, weak 참조(Soft), optional(값·`null`), UTF-8 문자열을, `RocksExport.BundleWriter`는 남의 파일이 든 staging 디렉터리를 지우지 않는지 본다.
