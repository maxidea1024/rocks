## 5. 결정 사항

| 번호 | 결정 | 상태 |
|---|---|---|
| BD1 | `TargetType`을 Game·Editor·Client·Server·Program 다섯 값으로 넓힌다. 타깃 종류별 플랫폼은 3.1절 표다 | 권장안 |
| BD2 | 매크로 `ROCKS_WITH_EDITOR`·`ROCKS_WITH_EDITOR_DATA`·`ROCKS_WITH_SERVER_CODE`·`ROCKS_WITH_CLIENT_CODE`를 모든 모듈에 0·1로 정의한다. 타깃 안에서 같은 값을 강제하고, 기본값 없이 `Base/Compiler.h`가 검사한다. `ROCKS_WITH_EDITOR=1`이면 `ROCKS_WITH_EDITOR_DATA=1`이다. 서버·클라이언트 코드 값은 타깃 종류로만 정한다 | 권장안 |
| BD3 | 타깃 종류 매크로(UE `UE_GAME`·`UE_EDITOR`·`IS_PROGRAM` 등)는 두지 않는다. 실행 중 분기는 `build::Description().targetType`, 컴파일 분기는 BD2 매크로로 한다 | 권장안 |
| BD4 | 모듈 type은 Runtime·Developer·Editor·ServerOnly·ClientOnly다. 게임·플러그인 모듈은 기술자 type으로 거르고, 엔진 모듈은 `SourceModuleRules.ModuleType`(기본은 Group 폴더)으로 도달 검사만 한다. SDK manifest에 type을 싣는다 | 권장안 |
| BD5 | 설치 SDK는 매크로 조합마다 엔진 라이브러리 변형을 싣는다(key는 조합). 1차는 Game 조합만이며 게임의 Game 타깃과 기본 Program 타깃을 빌드할 수 있다. 없는 조합은 오류이고, 변형을 더하면 `ManifestVersion`을 올린다. Editor 변형은 key에 링크 방식(`Modular`)을 더하고 Rocks Studio 실행 파일과 엔진 모듈 공유 라이브러리를 싣는다(3.8절). 빌드 구성(Debug·Development·Release)은 이 변형 축과 직교하는 별도 축이다: SDK 하나가 여러 구성을 `Configurations/<구성>/`에 싣고(manifest 14, [Visual Studio 프로젝트 파일](../ProjectFiles.ko.md) 10.3절), 변형이 생기면 `Configurations/<구성>/<변형>/`으로 넓힌다 | 권장안 |
| BD6 | 리플렉션 선언은 전처리 조건 블록 밖에만 쓴다. 예외는 `#if ROCKS_WITH_EDITOR_DATA` 안의 `ROCKS_PROPERTY`와 `#if ROCKS_WITH_EDITOR` 안의 `ROCKS_FUNCTION`뿐이고, 둘 다 `EditorOnly`와 양방향으로 일치해야 한다. 타입 선언은 블록 안에 둘 수 없다. 판정은 메타 컴파일러의 어휘 스캔(정확한 조건 철자 두 개만 허용)이다. 에디터·게임 두 조합의 AST 비교를 보강 검사로 두고, descriptor·안정 ID는 합집합, 생성 코드는 조건부다. `ROCKS_WITH_EDITOR_DATA=0` 빌드는 `EditorOnly` FieldId 목록으로 에디터 패키지의 그 값을 건너뛴다 | 권장안 |
| BD7 | `Dependencies`·`RequiredModules`를 `PublicDependencies`·`PrivateDependencies`로 바꾼다(호환 이름 없음) | 권장안 |
| BD8 | 메타 컴파일러의 모듈 범위(import·include 순서·생성 헤더 해석·전파 define)는 컴파일 환경에 보이는 모듈과 같다 | 권장안 |
| BD9 | 공개 헤더 검사는 모듈마다 그 모듈의 공개 환경만으로 컴파일한다 | 권장안 |
| BD10 | include-only 의존(UE `*IncludePathModuleNames`)은 두지 않는다 | 권장안 |
| BD11 | include 소유 모듈 정책 검사(depfile 기반, UB0 파서 공유), 두 목록 중복 금지, 쓰지 않는 공개 의존 진단 | 권장안 |
| BD12 | 프로젝트 플러그인 참조에 `targets`·`platforms`·`configurations` 허용 목록을 둔다. 조건 참조는 그 플러그인의 상태를 정한다(문맥이 맞으면 켬, 아니면 끔 — UE 의미론). 값 검사는 문맥과 무관하게 한다 | 권장안 |
| BD13 | `.rocksplugin`에 지원 플랫폼 `platforms`를 둔다. 기본 활성만으로 켜질 플러그인은 미지원 플랫폼에서 조용히 빼고, 명시·의존으로 켜지면 오류다 | 권장안 |
| BD14 | 거부 목록(UE `*DenyList`)은 두지 않는다. 같은 축에 허용·거부 두 목록이 있으면 충돌 규칙이 필요하다. 템플릿의 `TargetDenyList: ["Server"]`는 `targets: ["Game", "Client", "Editor", "Program"]`로 적는다. 대가로 타깃 종류가 늘면 기존 허용 목록에 자동으로 들어가지 않는다. 타깃 종류 추가는 드물고, 그때 기존 목록을 검토하는 편이 명시적이다 | 권장안 |
| BD15 | 플러그인 참조 `optional`(없으면 조용히 무시)은 두지 않는다. 저장소에 플러그인이 있는지에 따라 빌드 결과가 조용히 달라지기 때문이다 | 권장안 |
| BD16 | 모듈 기술자의 `AdditionalDependencies`(UE 에디터 클래스 추가 마법사가 Build.cs 대신 기술자에 의존을 덧붙이는 칸 — `UEBuildTarget.cs:6784-6790`)는 두지 않는다. 의존의 단일 출처는 모듈 규칙(Build.cs)이고, 에디터 마법사가 생기면 Build.cs를 고친다 | 권장안 |
| BD17 | 타깃 규칙 속성 `LinkType { Monolithic, Modular }`. 기본값은 Editor가 `Modular`, 나머지가 `Monolithic`이다(UE `TargetRules.cs:2686-2689`). 타깃이 바꿀 수 있고 Android는 `Modular`를 거부한다. 매크로 `ROCKS_MONOLITHIC`, 파일 이름은 UE 규칙(3.8절) | **확정 — 사용자 결정(2026-11-19, UE 방식)** |
| BD18 | 전용 서버는 필수다(사용자 요구 2026-11-19 — 네트워크가 구성되면 서버로 돌 수 있어야 하며, 스펙은 지금 잡고 구현은 네트워크 단계). Server 타깃은 Windows·macOS·Linux에서 빌드하고 Android에서는 거부하며, 그래픽 backend 모듈(type ClientOnly)을 링크하지 않는다. `NativePlatform`에 Linux(x64)를 더하고 Linux에서는 Server·Program 타깃만 빌드한다. 게임 템플릿은 Server 타깃 규칙을 싣는다 | **확정(요구)** |
| BD19 | Program 타깃의 기본 매크로 조합은 Game과 같다(`ROCKS_WITH_EDITOR_DATA=0`). `WithEditorData`·`CompileWithEditor` 속성으로 켠다 | 권장안 |
| BD20 | Release 배제 정책을 Game·Client·Server로 넓히고, type Editor 모듈이 Game·Client·Server에 닿는 것을 구성과 무관하게 막는다 | 권장안 |
| BD21 | 서드파티 모듈은 include·define을 public으로 내고, 소비자는 `PrivateDependencies`로 쓴다. `Consumer*` 우회를 없앤다 | 권장안 |
| BD22 | 시험 소스는 시험 대상 모듈의 컴파일 환경 전체와 그 모듈의 `Private/`만 include할 수 있다. 개정 27(사용자 결정): 시험 실행 파일의 컴파일 환경은 대상 모듈 환경의 합집합 하나이고, `NativeModuleInput.SourceGroups`(폴더, 보이는 모듈)가 BD11 검사의 보이는 모듈을 묶음마다 정한다. 유니티 묶음 TU는 소스 묶음 경계를 넘지 않는다. 버린 안: 묶음별 컴파일 환경 | 권장안·사용자 결정 |
| BD23 | 플러그인 의존 참조(`.rocksplugin` `plugins`)에는 조건을 두지 않는다 | 권장안 |
| BD24 | 게임 빌드의 플러그인 처리는 문맥 무관 검증(규칙 로드 전)과 문맥별 해석(규칙 로드 뒤, 타깃 종류 사용)으로 나눈다 | 권장안 |
| BD25 | 모듈 A는 들어갈 수 있는 (타깃 종류, 구성) 집합이 자기 집합을 포함하는 모듈에만 의존한다(3.3절 표). 위반은 규칙 로드 때 오류다 | 권장안 |
| BD26 | `ROCKS_WITH_EDITOR_DATA=0` 빌드는 에디터 패키지(쿠킹하지 않은 패키지)를 저장하지 않는다(Unsupported). 건너뛴 `EditorOnly` 값을 잃지 않기 위해서다 | 권장안 |
| BD27 | 빌드가 모듈마다 `ROCKS_<MODULE>_API`를 정의한다. `Monolithic`이면 빈 값, `Modular`이면 자기 모듈은 export·다른 모듈은 import다(UE `<MODULE>_API`). 가시성·rpath 옵션은 UE와 같다. 규칙 파일은 이 매크로를 정의할 수 없고, 다른 모듈의 API 매크로를 붙이면 정책 오류다. 다른 모듈이 상속하는 클래스는 클래스 단위 export가 필요하다(메타 컴파일러 진단) | 권장안 |
| BD28 | 메타 컴파일러는 생성 `.cpp`(UE `.gen.cpp` — 리플렉션 헤더마다 하나, 모듈마다 등록용 하나: [리플렉션 생성 소스](../ReflectionGeneratedSources.ko.md) GSD2)를 내고, 타입 키·타입 정보·등록 함수를 거기 정의해 항상 export한다. `NativeTypeKey`는 그 키 저장소 주소다. UE `MinimalAPI` 지정자는 두지 않는다 | 권장안 |
| BD29 | 공개 헤더에 쓰기 가능한 정적 저장소(const 아닌 inline 변수·inline/템플릿 함수의 static 지역 변수·헤더 정의 정적 데이터 멤버), `thread_local` 변수, 주소를 식별자로 쓰는 정적 객체를 두지 않는다(정책 `public-header-static-storage`, 링크 방식과 무관하게 검사). 서드파티는 UE와 같이 소비 모듈마다 링크하고, `HasGlobalState` 서드파티만 직접 소비 모듈을 하나로 제한한다. Monolithic 시험 실행 파일(`GlobalStateTestConsumer`)은 세지 않는다(3.8절 — 개정 38) | 권장안 |
| BD30 | 모듈 수명 객체·로더는 별도 스펙 [모듈 로더](../ModuleLoader/README.ko.md)로 정한다(ML0–ML4는 BS5 전, ML5는 BS5와 함께). Modular 실행 파일은 부트 모듈만 import한다(MD21). 요구 계약은 3.8절 네 항목이다. 게임의 Editor 타깃은 설치 SDK의 엔진 모듈 공유 라이브러리를 공유하며 엔진 컴파일 환경을 바꿀 수 없다 | 권장안 |
| BD31 | Editor보다 먼저 `Modular` Program 시험 빌드(FoundationTests — 타깃 `FoundationModular`)를 검사 체인에 넣어 export 누락과 공유 상태 위반을 잡는다(Windows·macOS). 개정 22(사용자 결정): 시험은 시험 실행 파일 모듈(호스트) 하나에 두고 시험이 부르는 모듈 내부 선언에 그 모듈의 API 매크로를 붙인다(비공개 헤더는 정책 허용 목록). 처음 안(모듈 `Private/` 시험 묶음을 그 모듈 공유 라이브러리에 링크 — UE `Private/Tests`)은 6개 모듈의 시험이 그 모듈에 의존하는 fixture·모듈을 써서 순환이 생겨 버렸다. 시험 등록 표는 호스트에 한 벌이다 | 확정(사용자 결정) |
| BD32 | 엔진 저장소의 규칙 평가는 `Engine/Plugins`를 늘 찾고, 끈 플러그인까지 기술자 검증·규칙 대조를 한다(3.9절) | 권장안 |
| BD33 | 타깃 규칙 `Plugins`(프로젝트 `plugins[]`와 같은 형식), 타깃 참조가 프로젝트 참조를 이긴다. 버린 안: 문자열 목록 둘(형식이 둘이 됨), 플러그인 쪽 지원 프로그램 목록(UE `SupportedPrograms` — 이중 등록) | 권장안 |
| BD34 | `enabledByDefault`는 Program 타깃에 적용하지 않는다(명시만). Game에는 적용한다 | 권장안 |
| BD35 | 플러그인 모듈 포함: Runtime은 모두, Developer는 Game Release만 뺌, Editor는 BS2 전까지 뺌(3.9절 표). 버린 안: 지금 규칙(Program Release에서 Developer가 빠짐) | 권장안 |
| BD36 | 켠 플러그인 모듈은 주 실행 파일에 링크하고 기술자 단계의 보통 모듈 행으로 싣는다. 게임 빌드도 같다. 버린 안: 참조된 것만 링크(정적 의존이 필요해 끄고 켜는 의미가 없어짐), `DynamicModules` 재사용(요청 시 적재 행이라 단계로 시작하지 않음), 모든 실행 파일에 링크 | 권장안 |
| BD37 | 플러그인 모듈 규칙 기반 클래스 `PluginModuleRules`, 위치·종류·단계의 단일 출처는 기술자. 버린 안: 규칙에 경로 직접 기입(중복), `Source/Developer` 흉내 | 권장안 |
| BD38 | `NativeModuleInput.Type`을 두고, Release 배제는 경로 대신 이 칸으로 판정한다 | 권장안 |
| BD39 | 엔진 모듈 → 플러그인 모듈 의존 금지(동적 포함), 끈 플러그인 모듈 의존은 오류(UE는 경고), 플러그인 사이는 `plugins` 선언 필요 | 권장안 |
| BD40 | 플러그인 시험은 `<플러그인>/Source/<모듈>/Tests/`에 두고 EngineTests가 `PluginTestModules`로 모은다. 버린 안: 플러그인마다 시험 실행 파일 | 권장안 |
| BD41 | SDK export 타깃의 엔진 플러그인 켜기 거부. `Modular` 타깃의 플러그인은 ML5a(모듈 로더 MD23 — 플러그인 binaries 폴더·manifest)부터 허용(개정 26, 그 전에는 거부) | 권장안 |
| BD42 | 정책 경로에 `Engine/Plugins` 모양 추가(`crt-number-conversion`·시험 소스·fixture 판정), 새 검사 `plugin-module-rules-base` | 권장안 |
| BD43 | 쿠커(Commandlet)의 모듈 집합은 쿠킹 대상 출시 타깃(Game·Client, Release)의 모듈 집합과 같아야 한다: 엔진 몫은 Commandlet이 엔진 저장소 출시 타깃(지금 Sandbox) 폐포의 Runtime·ClientOnly 엔진 모듈을 링크(정책 `cooker-module-set`), 프로젝트·플러그인 몫은 ML5b 뒤 모듈 로더로 적재, 쿠커가 이름으로 대조(3.10절, 렌더 세션 요청 — 셰이더 시스템 SSD3). 버린 안: 대상 타깃마다 쿠커 링크, 모듈 없이 셰이더 타입 manifest로 쿠킹 | 권장안 |
| BD44 | 타깃 속성 `PrerequisiteTargets`(`PrerequisiteTarget` — 이름과 조건 칸 `Platforms`·`Configurations`): `build`·`test`가 주 타깃 메타 생성 전에 선행 타깃을 자기 규칙으로 같은 구성·도구 체인·빌드 방식에서 따로 빌드한다(깊이 우선 후위, 중복 한 번, 출력은 자기 기본 출력·`--output`이면 형제). 선행 타깃은 Program이고, 주 타깃이 Developer 모듈을 빼는 구성(출시 타깃 Release)에서는 건너뛰며 그 구성을 명시하면 오류다. 없는 이름·자기 자신·중복·순환·선행 타깃을 가진 동반 타깃은 규칙 로드 오류다(3.11절, UE `PreBuildTargets`). 버린 안: Game 타깃 동반 허용(매크로 조합·빌드 정보 TU 공유가 남음), Game 매크로 워커 타깃 추가, `TestPrograms` 재사용, `Rocks.proj`에서 두 번 부르기 | 권장안 |
| BD45 | ISPC 도구 고정(3.12절): `Build/Toolchain.json` `ispc`(버전·호스트별 url·sha256), `InstallIspc.ps1`, `ROCKS_ISPC` → 기본 위치, 버전 대조 실패는 빌드 오류, ISPC 원본이 대상에 있을 때만 요구 |
| BD46 | ISPC 원본 선언(3.12절): `NativeModuleInput.IspcSources`·manifest `ispcUnits`(명시 목록만), Android 대상은 Unsupported |
| BD47 | action `Ispc`(3.12절): 원본마다 object·`_ispc.h`·`.deps`, 아키텍처당 타깃 하나(X64 `sse4-i32x4`, Arm64 `neon-i32x4`), `--opt=disable-fma`, fast-math·다중 타깃 금지(모듈 인자로 못 바꿈) |
| BD48 | ISPC 생성 헤더(3.12절): `<out>/Ispc/<모듈>/`(계획 때 만듦, Private include, 입력 스냅샷·목록 제외), 같은 모듈 C++ compile이 의존하고 DeclaredInputs로 받음 |
| BD49 | ISPC 증분 key(3.12절): Identity에 버전·실행 파일 sha256(찾을 때 한 번)·타깃·인자, `.isph`는 depfile(`-MMM`)·헤더 확장자 목록 |
| BD50 | ISPC 빌드 ID·인코더 신원(3.12절): 도구 빌드 ID가 Ispc action을 원본 키로 셈, `ToolchainFingerprint`는 그대로(실행 파일 간 대조 계약), ISPC 신원은 생성 정의 `ROCKS_ISPC_IDENTITY`로 인코더 너겟 키에 |
