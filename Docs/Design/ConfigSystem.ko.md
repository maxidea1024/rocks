# 설정 계층과 콘솔 설정 설계

기준일: 2026-11-19, 개정 10(봉인 뒤 독립 콘솔 설정 직접 쓰기 BootSealed, 5.1·8절 정리). 엔진·프로젝트·플랫폼·기기·사용자·세션·콘솔에서 오는 설정 값을 하나의 우선순위로 합치는 **설정 계층**과, 실행 중 이름으로 읽고 바꾸는 **콘솔 설정**(변수·명령)을 정한다.

[렌더 기반](RenderBase.ko.md)의 선행 작업 CP8(Config 계층)·CP4(콘솔 설정)이며, [엔진 구현 계획](EngineImplementationPlan.ko.md)의 다음 요구를 구현 스펙으로 옮긴다.
- 24.1절 표의 "Device Profile / Scalability / CVar" 행
- 24.2절 Config 우선순위: engine default → platform → project → device profile → user → session override, 런타임 변경 가능·재시작 필요 구분, 값 출처 표시, 품질 조합 검증

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론)의 현재 기본 경로이고, 경로는 `Engine/Source/Runtime/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 레거시·deprecated 경로는 보지 않았다.

상태: **설계 확정**(결정 D1–D9). 구현 단계는 9절이다. 구현: CF1·CF2(2026-11-17, Core 모듈 `Config` — `Core/Source/Runtime/Config`), 엔진 연결(2026-11-18, 5.4절 — `HostConfig` 모듈·엔진 sync 지점·`--set`·`config-dump` 등록), CF3(2026-11-18, 8절 — 사용자 층 저장·관대한 읽기·재시작 대기·hot reload), CF4(2026-11-18, 6절 — 기기 프로필·품질 조합 검증 함수. 품질 묶음 정의·펼침은 CVR5). 5절 콘솔 설정의 상세 스펙은 [CVar·치트 명령 설계](CVarSystem.ko.md)로 옮겼다(개정 4). 그 단계 CVR1–CVR8의 착수 시점은 다른 세션 작업을 본 뒤 사용자가 정하고, 그 전까지는 CF2 구현이 현재 동작이다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 파일 형식 | JSON. config 파일을 읽을 때만 `//` 줄 주석을 허용한다. 쿠킹 결과와 저장 파일은 주석 없는 결정적 JSON이다(2026-11-17 사용자 결정) | 리플렉션 스키마로 엄격히 읽어 모르는 키·타입 불일치를 줄·열 진단으로 거부한다. UE ini는 오타 키를 조용히 받는다. Rocks는 TargetProfile·CookSettings·셰이더 형식 정의가 이미 JSON이고, Core JSON은 결정적 출력과 위치 진단을 가진다(`Core/Source/Runtime/Core/Public/Core/Text/Json.h:10–21, 104–109`) |
| 우선순위 | config 층과 콘솔 변수 출처를 **하나의 순서**로 합친다. 값마다 출처별 값 스택을 둔다(사용자 결정) | UE는 config 층 순서(`Core/Public/Misc/ConfigHierarchy.h:9–46`)와 콘솔 변수 SetBy 16단계(`Core/Public/HAL/IConsoleManager.h:152–210`)가 따로 있고 겹친다 |
| 배열 병합 | 기본은 통째 교체이고, 추가·제거·비우기 연산을 둔다(사용자 결정) | UE ini `+ . - ! ^` 연산(`Core/Public/Misc/ConfigCacheIni.h:132–155`) |
| 기기 프로필 | 설정 값 묶음 전체(콘솔 변수와 설정 struct 필드)를 덮어쓰는 한 층이다. 부모 상속과 메모리 등급별 묶음을 둔다. 쿠킹에 영향을 주는 텍스처 LOD 그룹은 TargetProfile에 둔다(사용자 결정) | UE 기기 프로필은 콘솔 변수 묶음이고 텍스처 LOD 그룹을 가진다(`Engine/Classes/DeviceProfiles/DeviceProfile.h:20–71`) |
| 값 저장 단위 | 설정 struct(리플렉션) 하나가 한 섹션이다. 콘솔 변수와 설정 필드는 같은 값 저장소를 공유한다(이름 하나에 값 하나) | UE `UDeveloperSettings`의 `ConsoleVariable` 메타가 둘을 잇는 방식(`DeveloperSettings/Public/Engine/DeveloperSettings.h:31–35, 69–72`) |
| 적용 시점 | 값의 소유자는 게임 lane이다. 변경은 어느 스레드에서나 큐에 넣고, 프레임 시작 sync 지점에서만 적용하고 알림을 부른다. 다른 스레드는 게시된 불변 snapshot만 읽는다(렌더 lane은 프레임 packet이 든 것 — 5.3절, CVar 10.1절) | UE 전역 sink는 정해진 지점에서만 불린다(`Core/Private/HAL/ConsoleManager.cpp:1092–1109`). 렌더 스레드 사본을 주소에 쓰는 전파(`RenderCore/Private/RenderingThread.cpp:493–513`)는 따르지 않는다(RenderBase 5.3·12절) |
| 쿠킹 | TargetProfile마다 층을 미리 합쳐 결정적 JSON으로 쓴다. 출시에 넣을 필드는 **허용 목록**으로 고른다 | UE는 `BinaryConfig.ini`로 미리 합치고, 비공개 키는 거부 목록으로 뺀다(`Engine/Config/BaseGame.ini:141–166`). 허용 목록이 더 안전하다 |
| 시험 | 전역 싱글턴 대신 설정 문맥 객체를 둔다. 헤드리스·시험 기본은 사용자 층·자동 기기 선택이 없는 봉인 모드다 | 결정적 시험 원칙 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 설정 계층 `ConfigStack` | `FConfigCacheIni`·`FConfigBranch` | 층을 합친 값과 출처의 집합 |
| 설정 층 `ConfigLayer` | config layer·SetBy | 값이 오는 곳 하나(엔진·프로젝트·사용자 등) |
| 설정 struct | `UCLASS(config=)`·`UDeveloperSettings` | 리플렉션 struct 하나 = 섹션 하나 |
| 콘솔 설정 `CVar<T>`·`BootCVar<T>` | console variable(`TAutoConsoleVariable`) | 이름으로 읽고 바꾸는 타입 있는 값(CVar 문서) |
| 콘솔 명령 `ConsoleCommand` | console command | 이름으로 부르는 동작 |
| 품질 묶음 `CVarGroup` | scalability group(`sg.*`) | 등급 하나가 여러 값을 정하는 묶음 |
| 범위 `CVarScope` | (없음) | World·플레이어별 값을 두는 범위(CVar 8절) |
| 기기 프로필 `DeviceProfile` | device profile | 플랫폼·기기별 값 묶음 |
| 설정 snapshot `ConfigSnapshot` | 렌더 스레드 사본 | 프레임 packet에 담는 불변 값 블록 |
| 설정 문맥 `ConfigContext` | (전역 `GConfig`) | 한 실행·시험의 설정 계층 소유자 |

## 3. 층과 우선순위

### 3.1 층 순서

약한 것부터 강한 것 순서다. 값마다 출처별 값을 스택으로 보관하고, 가장 강한 출처의 값이 현재 값이다.

| 순위 | 층 | 위치·출처 | 비고 |
|---|---|---|---|
| 0 | 코드 기본값 | 설정 struct의 기본값, 콘솔 설정 등록값 | |
| 1 | 엔진 | `Engine/Config/<Branch>.json` | |
| 2 | 엔진 플랫폼 | `Engine/Config/<Platform>/<Branch>.json` | 플랫폼 부모 사슬을 따른다(3.3절) |
| 3 | 프로젝트 | `<Project>/Config/<Branch>.json` | |
| 4 | 프로젝트 플랫폼 | `<Project>/Config/<Platform>/<Branch>.json` | |
| 5 | 기능 묶음 | 플러그인·게임 기능이 더하는 층(동적) | 묶음마다 우선순위·태그가 있고 해제하면 되돌린다. UE 동적 층(`ConfigCacheIni.h:1103–1118`)에 대응 |
| 6 | 품질 묶음 | 고른 품질 등급이 펼친 값(`CVarGroup` — 6.1절) | 프로젝트 파일보다 강하고 기기 프로필보다 약하다. UE는 프로젝트 ini가 품질 묶음을 이긴다(`Engine/Private/Scalability.cpp:267–353`). Rocks는 고른 등급이 프로젝트 기본값을 이기게 한다(CVar VD4) |
| 7 | 기기 프로필 | 선택된 프로필과 부모 사슬 | 6.2절 |
| 8 | 사용자 | `<UserData>/<Project>/Config/<Branch>.json` | "사용자 설정 가능"(`UserSetting()`·`UserConfig`) 값만 쓸 수 있다 |
| 9 | 핫픽스 | 라이브 운영 핫픽스 | UE `ECVF_SetByHotfix`. `ClearLayer(Hotfix)`로 전체를 되돌린다 |
| 10 | 세션 | 명령줄 `--set=<경로>=<값>`, 시험 픽스처 | 출시 빌드는 서버 대상만 허용한다(UE와 같다, `Core/Public/Misc/ConfigCacheIni.h:57`) |
| 11 | 코드 | 코드의 `Set()` | UE `ECVF_SetByCode`. 출처는 호출 위치다 |
| 12 | 범위 임시 | `cvar::ScopedOverride`·시험 | UE `ECVF_SetByTemp`처럼 콘솔보다 약하다 |
| 13 | 콘솔 | 실행 중 콘솔 입력 | 개발 빌드. 출시 빌드는 콘솔 입력 자체가 없다 |
| 14 | 서버 | 서버가 복제한 값(`Replicated()` 변수) | 가장 강하다. 클라이언트의 콘솔·코드·임시 변경이 복제 값을 이기지 못한다(CVar 13.2절) |

- 9·11·12·14층은 개정 4에서 CVar 문서(6.1절)를 따라 더했다. World·플레이어별 값은 층이 아니라 범위(CVar 8절)이고, 범위 값은 Global 유효값보다 강하다.

**규칙.**
- 현재 출처보다 약한 출처의 쓰기는 값을 바꾸지 않는다. 다만 출처 스택에는 기록하고 진단을 남긴다. 그래서 강한 출처를 지우면 약한 출처 값이 드러난다. UE에서 약한 쓰기를 거부하는 규칙(`Core/Private/HAL/ConsoleManager.cpp:275–312`)과 같다.
- 출처 지우기 `Unset(층, 태그)`는 그 출처의 값을 스택에서 빼고 남은 최강 출처 값으로 돌아간다. 기능 묶음 해제, 품질 등급 변경, 시험 정리에 쓴다. UE `Unset`(`:1191–1230`)에 대응한다.
- 기능 묶음·품질 묶음·기기 프로필 층은 태그(묶음 이름)별로 여러 값을 쌓는다. UE "배열 우선순위"(`:760–771`)와 같다.
- "값의 출처"는 스택의 최상단이다. 진단·도구 UI가 이를 표시한다(계획 24.2절).
- 범위 임시 변경(시험·코드)은 범위 임시 층(12)에 고유 태그로 쓰고 범위가 끝나면 그 태그를 지운다(`cvar::ScopedOverride` — CVar 6.3절). UE `TGuardConsoleVariable`(`IConsoleManager.h:1411`)과 같다.

### 3.2 branch와 섹션

- **branch**는 파일 단위다: `Engine`·`Game`·`Input`·`Render`·`Quality`·`DeviceProfiles`·`Editor`. 목록은 이름 있는 상수 표로 두고, 플러그인은 자기 branch를 등록할 수 있다.
- **섹션**은 branch 파일 안의 최상위 키이고 설정 struct 하나에 대응한다(예: `"RenderGraph": { ... }`). 콘솔 설정만 따로 두는 섹션은 `"Settings": { "render.graph.immediate": true }`처럼 이름 → 값이다.
  - 섹션 키는 필드 키처럼 ASCII 대소문자를 무시한다. 대소문자만 다른 섹션 키 두 개는 오류다. 그 branch에 등록된 설정 struct가 없는 섹션 키는 오류(`UnknownSection`)다.
  - 대입(세션 `--set`·시험 픽스처)의 대상은 `<Branch>.<Section>.<필드 경로>`이고 값은 리플렉션 명령행 표기다(ReflectionExtensions 10.4절). 콘솔 설정 이름(소문자 점 경로)도 대상이 된다(5.1절).

### 3.3 플랫폼 부모 사슬

플랫폼 층(2·4)은 플랫폼 정의가 선언한 부모 사슬을 따라 약한 것부터 적용한다(예: `Android` → `Mobile`). 사슬은 플랫폼 정의 파일에 둔다. 기본은 사슬 없음이다. UE `IniParentChain`(`Core/Private/Misc/ConfigContext.cpp:969–988`)에 대응한다.

## 4. 파일 형식과 병합

### 4.1 JSON과 주석

- Core JSON reader에 config 전용 읽기 옵션 "줄 주석 허용"을 더한다. `//`부터 줄 끝까지를 무시하고, 문자열 안의 `//`는 그대로 둔다. 다른 용도(쿠킹 manifest 등)는 지금처럼 엄격하다.
- 블록 주석·끝 쉼표·중복 키는 계속 오류다.
- 쿠킹 결과와 사용자 저장 파일은 주석 없는 결정적 JSON(Core canonical writer)으로 쓴다.

### 4.2 struct 병합과 배열 연산

층은 리플렉션 `ImportJson`(`Core/Source/Runtime/ReflectionText/Public/ReflectionText/TextJson.h:44–82`)으로 struct 값에 차례로 병합한다.
- **struct**는 재귀 병합이다(적힌 필드만 바뀐다).
- **Map**은 키별 병합이다.
- **배열·Set**은 기본이 통째 교체다. 배열 필드 자리에 연산 객체를 쓰면 이전 층 값에 연산을 적용한다.

| 연산 | 표기 | 뜻 | UE ini 대응 |
|---|---|---|---|
| 교체 | `"Items": [ ... ]` | 통째로 바꾼다 | `Key=` 여러 줄 |
| 추가(중복 없음) | `"Items": { "$add": [ ... ] }` | 없는 원소만 뒤에 더한다 | `+` |
| 추가(중복 허용) | `"Items": { "$append": [ ... ] }` | 그대로 뒤에 더한다 | `.` |
| 제거 | `"Items": { "$remove": [ ... ] }` | 같은 값의 원소를 모두 뺀다 | `-` |
| 비우기 | `"Items": { "$clear": true }` | 빈 배열로 만든다 | `!`·`^` |

- 한 연산 객체에 여러 연산을 함께 쓸 수 있다. 적용 순서는 `$clear` → `$remove` → `$add` → `$append`다(객체 안 키 순서와 무관).
- 원소 비교는 리플렉션 값 동등성을 쓴다. 구현은 리플렉션 정규 JSON 표기(`ExportJson`)의 동등성이다: 이전 값은 현재 값을 export한 배열이고, 연산 목록은 기본값 struct에 그 목록만 import했다가 다시 export해 같은 표기로 만든다. 그래서 enum 도구 이름의 대소문자, 실수 `1`과 `1.0`, struct 원소의 멤버 순서와 빠진 멤버(기본값)는 같은 값으로 본다. Name은 표시 문자열 그대로 비교한다. 키가 있는 struct 배열(UE `@`·`*`)은 Map 필드로 표현하고 따로 두지 않는다.
- 연산 객체는 배열·Set 필드에만 허용한다. 다른 자리의 `$` 키(struct 멤버·Map 키)는 오류다. Set에는 `$append`를 쓸 수 없다(중복이 없으므로 `$add`). 정적 배열은 길이가 고정이라 연산을 받지 않는다.
- 연산은 섹션 루트에서 struct 멤버로만 닿는 필드에 쓴다. 배열 원소·Map 값·Optional 안의 배열 필드는 층마다 이전 값이 정해지지 않으므로 연산 객체를 오류(`UnsupportedOperation`)로 거부한다.
- 구현 위치: Config 모듈이 ReflectionText `ImportJson` 앞 단계에서 처리한다(ReflectionText는 고치지 않는다). 섹션을 스키마와 함께 걸으며 연산 객체를 이전 층까지의 값에 연산을 적용한 배열로 바꿔 쓰고, 다시 쓴 JSON의 노드마다 원문 위치를 기록해 import 진단의 줄·열을 원문 위치로 되돌린다(연산 결과 원소는 그 원소가 온 목록 원소 또는 연산 객체 위치). 다시 쓸 것이 없으면 원문 문서를 그대로 import한다.

### 4.3 경로 치환

문자열 필드 값에서 `{EngineDir}`·`{ProjectDir}`·`{UserDataDir}`·`{SavedDir}`를 읽을 때 펼친다. UE `%GAMEDIR%` 등(`Core/Private/Misc/ConfigCacheIni.cpp:212–300`)에 대응한다. 목록은 이름 있는 상수 표다. 표에 없는 `{...}`는 오류다.
- 이름은 `{` + `[A-Za-z0-9_]+` + `}`다. 이 모양이 아닌 `{`(`{x`·`{}`)는 글자 그대로다. 값은 설정 문맥 옵션의 디렉터리이고, 표에 있지만 그 문맥에서 값이 비어 있는 이름도 오류다.
- 문자열 필드·문자열 원소·Map 문자열 값·Optional 문자열과 배열 연산 목록에서 펼친다. 대입(세션 `--set`·콘솔 쓰기)의 값 텍스트는 필드 종류를 보기 전에 펼친다(struct 기본값 `{}`는 이름 모양이 아니라 그대로다).

## 5. 콘솔 설정과 명령

이 절의 상세 스펙은 [CVar·치트 명령 설계](CVarSystem.ko.md)(이하 CVar)다. 이 절에는 Config 쪽에 남는 규칙과 CVar 문서의 위치만 적는다. 지금 구현(CF2 — `ConsoleSettingDefinition`·`RegisterConsoleSetting`·이름 snapshot·변수별 `Subscribe`)은 CVar 단계 CVR1–CVR8에서 바뀌고, 쓰는 곳은 CVR7에서 한꺼번에 옮긴다. 하위 호환 경로는 두지 않는다(CVar 21절). CVR 착수 전까지는 CF2 구현이 현재 동작이다.

### 5.1 콘솔 설정

- **정의**: 타입 있는 핸들 `CVar<T>`(실행 중 변경)·`BootCVar<T>`(이번 실행 고정)·`CVarGroup`(프리셋)을 `cvar::Var`·`cvar::Boot`·`cvar::Group` 빌더 한 문장으로 정의한다(CVar 4절). 이름·기본값·도움말이 필수이고, 빠뜨리거나 어긋난 것(이름 문법·범위 밖 기본값·타입에 맞지 않는 빌더 호출)은 컴파일 오류다. 읽기는 `.Get()`이다.
- **타입**: bool·int32·int64·float·double·String·Name·enum. enum 이름은 리플렉션 enum의 열거자 이름을 쓰고, 리플렉션 enum이 아니면 ADL `CVarEnumNames` 표를 쓴다(CVar 4.4절).
- **등록**:
  - 모듈 등록 함수에서 `ConfigContext::Register(핸들…)`로 명시적으로 등록한다. 전역 생성자는 금지다(UE 지침 `IConsoleManager.h:39–58`과 같다). 등록 누락은 Policy `cvar-registration`이 빌드에서 잡는다(CVar 5절, VD1).
  - 이름이 실행 중에 정해지는 변수(Sparks 스크립트·플러그인 데이터)는 `RegisterDynamic`으로 등록한다.
  - 등록 전에 층에서 들어온 값은 "대기 값"으로 보관했다가 등록 때 넘긴다. UE `CreatedFromIni`(`Core/Private/Misc/ConfigUtilities.cpp:419–422`)와 같다. `CompleteStartup` 뒤에도 남은 대기 이름은 `ReportUnresolved`가 가까운 이름 제안과 함께 보고한다(CVar 12절).
- **이름 문법**: 소문자 점 구분 경로다(`[a-z][a-z0-9_]*`를 `.`으로 이은 마디 둘 이상 — D7, CVar VD2). UE식 `r.ShadowDistance`는 쓰지 않는다. 첫 단어는 영역 이름 공간이다. 기본 표는 `render`·`graph`·`stream`·`shader`·`quality`·`net`·`physics`·`log`·`game`·`input`·`audio`·`ui`·`cvar`·`cheat`·`obj`이고 `RegisterConsoleRegion`으로 더한다. 표에 없는 영역은 등록 오류다. CVar(설정 struct 필드의 `ConsoleName` 포함)와 콘솔 명령이 한 이름 공간이다.
- **값 표기**: 콘솔·`--set`·JSON의 값 해석은 ReflectionText 명령행 단일 값 표기 하나다(CVar 전용 파서는 없다 — CVar 14.1절). 설정 struct 필드의 `ConsoleName`은 `Register`가 같은 번호 체계로 함께 올리고, 그 필드와 값 저장소를 공유한다(출처 스택도 필드 경로의 것). 대기 값은 등록 때 바로 검사·적용하고, 형식·범위·층 허용에 맞지 않는 대기 값은 그 입력의 위치로 진단을 남기고 건너뛴다(등록은 성공한다).
- **값의 JSON 모양**: `Settings` 섹션의 값은 bool·수·문자열이고 종류와 맞아야 한다(bool 변수는 JSON bool, 정수 변수는 정수, 실수 변수는 수, 문자열·Name·enum 변수는 문자열). 맞지 않으면 그 위치의 `InvalidSetting`이다.
- **성질**: CF2의 플래그는 빌더 메서드와 타입으로 옮긴다(CVar 21절).

| 빌더·타입 | 뜻 | UE 대응 | CF2 플래그 |
|---|---|---|---|
| `ConsoleReadOnly()` | 콘솔 층에서 못 바꾼다(다른 층은 가능) | `ECVF_ReadOnly` | `ConsoleReadOnly` |
| `Cheat()` | 출시 빌드에서는 등록되지 않고 `Get()`이 기본값이다. 개발 빌드에서는 치트 허용 상태 `cheat.enabled`가 꺼져 있으면 콘솔 쓰기를 거부한다 | `ECVF_Cheat` | `DevelopmentOnly` |
| `Replicated()` | 클라이언트에서 콘솔 쓰기를 거부하고 서버가 준 값(Server 층)을 따른다 | — | — |
| `UserSetting()` | 사용자 층에 쓸 수 있다 | — | `UserConfig` |
| `Range`·`Clamp`·`Validate` | 범위 거부·범위 보정·검증 함수 | — | 범위·허용 값 |
| `BootCVar<T>` | `CompleteStartup` 뒤 이번 실행 값이 고정된다. `UserSetting()`이면 다음 실행용 사용자 값만 저장한다 | — | `StartupOnly`·`RestartRequired` |
| (자동) 프리셋 항목 | 어떤 `CVarGroup`의 항목인 변수만 품질 묶음 층 쓰기를 받는다 | `ECVF_Scalability` | `QualityMember` |
| (자동) snapshot | 모든 CVar가 snapshot에 든다 | `ECVF_RenderThreadSafe` | `RenderRead` |

- **현재 구현(CF2·CF3)의 플래그 동작**(CVR 전까지): 콘솔 읽기 전용은 콘솔 층만 거부한다(`ConsoleReadOnly`). 품질 묶음 구성원 플래그가 없는 콘솔 설정은 콘솔 이름으로 오는 품질 층 쓰기를 거부한다(`NotQualityMember` — CVR5에서 프리셋 항목 자동 판정으로 바뀐다). 개발 전용은 출시 규칙에서 등록은 되지만 콘솔 조회·자동완성에 보이지 않고 어떤 층도 쓸 수 없어 코드 기본값으로 고정된다. 시작 뒤 고정(`StartupOnly`)과 재시작 필요(`RestartRequired`)는 `CompleteStartup`(봉인) 뒤 같은 "부팅 값" 규칙을 따른다(개정 8·9, CVar 9절): 독립 콘솔 설정(`BootCVar` 포함)에 바로 쓰는 층(콘솔·코드·범위 임시 변경)의 쓰기는 넣을 때 Error `BootSealed`로 거부한다(봉인 뒤 아직 등록되지 않은 이름이라 받은 쓰기는 그 이름을 등록할 때 받지 않고 `BootSealed`를 남긴다). 그 밖의 쓰기는 거부하지 않고 Sync가 지금 값과 그 출처를 유지하며, 새 값의 출처가 파일 층이면 재시작 대기로 기록하고(`GetPendingRestarts`, Note `RestartRequired`), 파생·휘발 층이면 대기 없이 Warning `BootValueIgnored`를 같은 이름·층·출처에 한 번 낸다(8절). CVar의 `BootCVar` + `UserSetting()`(VD15)은 사용자 층 대기를 그대로 쓴다. 파일 층(엔진·프로젝트 hot reload)의 시작 뒤 변경을 대기로 보이는 것은 VD15의 "이번 실행 값은 바뀌지 않고 다음 실행에 그 파일이 적용된다"와 같은 뜻이다. 설정 struct 필드의 `RestartRequired`·`StartupOnly` 지정자는 콘솔 이름으로 써도 `BootValueIgnored` 규칙 그대로 남는다(7절 — 콘솔 직접 쓰기 거부를 걸지는 CVar CVR6 결정 항목).
- 거부 코드와 진단은 CVar 6.2·12·13절이다. 권한(치트·복제·부팅 봉인)은 쓰기를 받을 때 한 번, Sync 적용 때 한 번 검사한다.
- 출시 쿠킹에서 빼는 값은 설정 struct 필드 `ShippingExcluded`와 `Cheat()` 변수다(8절 쿠킹 허용 목록).
- 셰이더·쿠킹 결과에 영향을 주는 값은 CVar로 두지 않고 셰이더 형식 정의·TargetProfile에 둔다. 쿠킹 키에 들어가야 하기 때문이다(RenderBase 12절). UE의 셰이더 변경 플래그(`IConsoleManager.h:1488–1496`)는 두지 않는다.

### 5.2 콘솔 명령

- 정의는 `cvar::Command` 빌더 한 문장(`ConsoleCommand` 핸들)이고 `Register`로 올린다. 치트 명령은 함수 리플렉션(UiToolkit CU2) 뒤 `CheatSet` 객체의 `ROCKS_FUNCTION(Cheat)` 함수가 기본형이다(CVar 14.3절, VD12). 실행은 게임 lane의 sync 지점에서 한다.
- 콘솔 명령은 실행 중 엔진 안의 동작이다. Commandlet(`Core/Public/Core/Commandlet/Commandlet.h:36–57`)은 프로세스 단위 일괄 작업이라 registry를 합치지 않는다. 같은 이름 문법만 공유한다(D9).
- 콘솔 입력 문법은 CVar 15절 표다: `<이름>`(값·출처), `<이름> ?`(Explain), `<이름> <값>`(콘솔 층 쓰기), `<이름> reset`(콘솔 층 값 지우기), `cvar.find`·`cvar.explain`·`cvar.dump`, `<명령> <인자…>`. 자동완성은 이름 접두로 찾는다.
- 명령 인자는 명령 함수 매개변수를 ReflectionText 표기로 읽는다(CVar 14.1절). CF2의 인자 struct `<경로>=<값>` 방식은 CVR6에서 바뀐다. 큐에 든 명령은 Sync의 값 적용·snapshot 게시 뒤, `OnSynced` 전에 실행하고 출력은 입력 때 받은 sink로 쓴다.
- 출시 빌드에는 콘솔 입력이 없다(UE `ALLOW_CONSOLE_IN_SHIPPING` 기본 0, `Core/Public/Misc/Build.h:214–215`).

### 5.3 sync 지점과 설정 snapshot

- 가변 상태(층 스택·범위 값·이름 표·대기 값·묶음 상태)의 소유자는 게임 lane이다. 변경 요청(코드 `Set`·콘솔 입력·사용자 설정 변경·hot reload·기능 묶음 추가)은 **어느 스레드에서나** 문맥의 쓰기 큐(다중 생산자·단일 소비자)에 넣는다. 검증은 넣을 때 하고, 적용은 sync 지점에서만 한다(CVar 6.2절, VD8).
- 엔진 프레임 3단계(프레임 시작) 전에 `Sync`가 큐를 적용한다. 순서는 CVar 10절이다: 큐 적용 → 묶음 해결 반복(유효값 계산과 `CVarGroup` 펼침) → 세대 갱신 → snapshot 게시 → 큐에 든 명령 실행 → `OnSynced`. 다른 시점에는 값이 바뀌지 않는다. 예외는 `Register`가 새 칸만 더한 snapshot을 즉시 게시하는 것이다(기존 값은 바꾸지 않는다 — CVar 5절, VD17).
- **snapshot**: 모든 CVar 값을 번호 순서의 조각(`ChunkSlotCount` = 256칸)에 담은 불변 블록이다. 바뀐 조각만 복사하고(copy-on-write) 나머지는 참조 계수로 공유한다. 바뀐 것이 없으면 이전 snapshot을 재사용한다. 설정 struct 필드의 `RenderRead` 블록은 같은 snapshot 안에 그대로 둔다(CVar 7.2절).
- **얻기**: `ConfigContext::Acquire()`는 어느 스레드에서나 부를 수 있고 참조 계수를 올린 `SharedPtr<const ConfigSnapshot>`을 돌려준다. CF2의 `Snapshot()`은 소유 스레드 전용이므로 `Acquire()`로 바꾼다.
- **읽기**: `Get()`은 스레드에 묶인 snapshot을 잠금·할당·문자열 연산 없이 읽는다(CVar 7.1절, VD7). 게임 lane은 Sync가 게시할 때 다시 묶고, 렌더 lane은 프레임 packet이 든 snapshot을 묶고(RenderBase RB1), worker 작업은 만든 lane의 snapshot 참조를 쥐고 넘겨받는다. 한 프레임 동안 같은 스레드는 같은 snapshot을 본다. 묶음 없는 `Get()`은 Fatal이다.
- 프레임 packet은 snapshot을 참조한다. snapshot 수명은 마지막 참조가 놓일 때까지다.
- 렌더 lane은 registry를 읽지 않는다(RenderBase 5.3·12절).
- **변경 알림**: 변수별 `Watcher`(세대 비교)와 Sync마다 한 번 부르는 `OnSynced`(바뀐 번호 목록과 바뀐 설정 struct 목록)다. CF2의 변수별 `Subscribe` 콜백은 없앤다(CVar 7.3절, VD9). 콜백은 게임 lane에서 문맥 잠금 없이 부르고, 콜백 안의 쓰기는 다음 Sync에 적용되며, 콜백 안에서 `Sync`를 부르면 `Busy`다.
- 스레드 안전성(공유 상태별 소유·동기화·수명)은 CVar 10.1절이다.

### 5.4 엔진 연결(구현 2026-11-18)

- **문맥 하나.** 프로세스의 설정 문맥은 호스트(게임·에디터 main, Sandbox, RocksCommandlet)가 하나만 만든다. Core 모듈 `HostConfig`의 `HostSettings`가 문맥과 그 문맥이 쓰는 타입 등록·모듈 등록·객체 Registry를 소유한다(로그가 엔진보다 먼저 준비돼야 하므로 엔진 Registry와 따로 둔다). 엔진은 `EngineConfig::settings`로 빌려 쓰고 만들거나 파괴하지 않는다.
- **부트 순서**(`HostSettings`):
  1. 등록: 모듈 타입(TraceControl + 호스트 콜백) → 설정(`log.*` — LogHost, `trace.*`·`TracePresets`·`Trace` — TraceControl, 호스트 콜백). 파일을 읽기 전이다(모르는 섹션·설정은 오류이기 때문이다).
  2. `LoadConfigFiles`: 엔진·프로젝트(·플랫폼·사용자) 층 파일. 엔진 폴더는 호스트가 정한다. Sandbox·RocksCommandlet은 실행 파일 위쪽에서 `Engine/Config`를 가진 첫 폴더를 찾는다(`FindEngineDirectory`).
  3. 호스트의 추가 파일(Sandbox `--config` — Project 층).
  4. 명령줄 `--set=<target>=<value>`(`AddCommandLineAssignments` — Session 층, 태그 `--set`, 값은 첫 `=` 뒤 전부)와 `--device-profile=<이름>`(기억만 한다).
  5. Sync → 기기 프로필 적용(`ApplyDeviceProfile` — 6.2절) → Sync → `CompleteStartup`(StartupOnly 값 고정 — `log.mode`·`log.file.*` 등). 프로필 정의 오류는 기록하고 시작은 계속한다. 기기 프로필이 정한 StartupOnly 값도 시작에 들어간다. 품질 묶음 펼침(CVR5)은 이 단계의 기기 프로필 뒤에 들어간다.
  6. 그다음 LogHost 설치 → trace 세션·콘솔 명령·`--trace` → 엔진 루프.
- **설정 오류의 출력.** 1–5단계의 거부는 `HostSettings`가 진단(파일·줄·열·코드·경로·메시지)과 함께 한 번 기록한다. LogHost 설치 전이므로 비상 경로(stderr, Warning 이상)로 나간다. 호출자는 다시 기록하지 않는다.
- **sync 지점.** 엔진 `RunFrame`은 2단계(시간 갱신) 뒤, 3단계(프레임 시작 — hooks.BeginFrame·Subsystem.OnFrameBegin·client.OnEngineTick) 전에 `Sync`를 부른다. 그래서 프레임 사이 변경은 다음 프레임 시작부터 보이고, 프레임 안에서 넣은 변경은 그 프레임 동안 값을 바꾸지 않는다. Sync 실패는 그 프레임의 첫 오류로 돌려주고 프레임은 계속한다(연속 실패는 첫 번째만 Error 로그).
- **종료 순서.** 엔진 정리 → trace 세션 정지 → 로그 설치 범위 해제 → `LogHost::Stop`(구독 해제) → 설정 문맥 파괴. 문맥은 LogHost·trace 제어기·엔진보다 오래 산다.
- **config-dump.** RocksCommandlet이 같은 부트(1·2·5단계)로 문맥을 만들어 `config-dump`를 등록한다. 엔진 폴더를 못 찾으면 엔진 층 없이 코드 기본값을 낸다.
- **기기 특성.** `HostSettings::Initialize`가 플랫폼 특성(플랫폼 이름 — 옵션 `platform`, 비면 `HostPlatformName()`, 시스템 메모리)을 채운다. 플랫폼 이름은 문맥의 플랫폼(플랫폼 층 폴더)이기도 하다. 그래픽 장치를 만든 호스트는 `DeviceTraits()`에 GPU 특성(`DeviceCaps` — Sandbox `AppendGraphicsDeviceTraits`)을 더하고 `ApplyDeviceTraits()`로 기기 프로필을 다시 적용한다(다음 Sync, 시작 뒤라 StartupOnly 값은 바뀌지 않는다).
- **프로젝트 폴더·사용자 층(게임·에디터 실행 파일 — 2026-11-18 결정, 권장안).** 프로젝트 폴더는 실행 파일 폴더부터 위로 올라가며 프로젝트 기술자(`*.rocksproject`)를 가진 첫 폴더이고 프로젝트 이름은 그 기술자 파일 이름이다(`FindProjectDirectory`·`FindProjectFrom` — 여럿이면 이름 byte 순서로 첫 것). 사용자 층은 Standard 모드에서 `<UserData>/<프로젝트 이름>/Config`다(`UserDataDirectory` — Windows Roaming AppData, macOS `~/Library/Application Support`, Android는 등록한 앱 data 폴더). 프로젝트가 없는 도구(Sandbox·RocksCommandlet)는 봉인 모드다. hot reload는 `StartHotReload`(개발 빌드)다.
- **남은 일.** 게임·에디터 실행 파일을 HostSettings 부트로 바꾸는 일은 대상이 없어 하지 않았다(2026-11-18 확인): 저장소의 실행 파일은 Sandbox·RocksCommandlet·시험·벤치 프로그램과 Foundation 게임 템플릿(`Templates/Game/Foundation` — 엔진 루프 없이 Object·리플렉션만 쓰는 예제)뿐이고 에디터는 없다. 템플릿을 바꾸려면 설치 SDK(`Engine` 타깃 — ObjectSerialization·Localization·CoreTypeBindings)에 HostConfig와 그 의존(LogHost → Crypto → 외부 라이브러리 Sodium)을 넣어야 하는데 SDK 내보내기는 외부 라이브러리 모듈을 아직 거부한다. 엔진 루프를 쓰는 게임 템플릿·에디터가 생길 때 위 결정대로 main을 짠다(Sandbox `RunEngineHost`가 본보기다). 실제 그래픽 장치의 특성 연결도 장치를 만드는 엔진 호스트가 생길 때 한다(지금 Sandbox 엔진 호스트는 headless다).
- 스펙과 다른 점: 문맥·부트를 엔진 안이 아니라 호스트 모듈(`HostConfig`)에 둔다(로그 설정이 엔진 초기화보다 먼저 필요하다). 엔진은 문맥을 빌린다. 엔진 폴더는 실행 파일 위치에서 찾는다(설치 경로 규칙은 패키징 단계에서 정한다).

## 6. 품질 묶음과 기기 프로필

### 6.1 품질 묶음

- 묶음(예: `quality.shadow`·`quality.texture`·`quality.effects`)은 등급(Low·Medium·High·Epic·Cinematic 등, 묶음별 정의)을 가진다. 등급 하나가 값 묶음을 정한다.
- 묶음과 등급별 항목은 코드의 `CVarGroup`으로 정의한다(CVar 11절). 묶음 자체가 int32 변수이고, 항목 타입 불일치·같은 등급 중복은 컴파일 오류다. 항목인 변수만 품질 묶음 층 쓰기를 받는다(구성원 자동 — CVar VD14).
- `Engine/Config/Quality.json`(branch `Quality`)의 `"<묶음>": { "<등급>": { 값들 } }`은 코드가 정의한 항목의 **값만** 바꾼다. 항목 추가는 코드에서 한다. UE `Scalability.ini`의 `[<Group>@<Level>]`(`Engine/Private/Scalability.cpp:267–353`)에 대응한다.
- 등급을 바꾸면 그 묶음 태그의 품질 묶음 층 값을 `Unset`하고 새 등급 값을 쓴다.
- 사용자가 고른 등급은 사용자 층에 저장한다.
- 품질 조합 검증(계획 24.2절): 등급 조합이 장치 능력(`DeviceCaps`)으로 불가능하면 정해진 순서로 낮추고, 낮춘 결과와 사유를 진단에 남긴다.
- 구현 메모(CF4, `Config/QualityValidation.h`): 묶음 정의와 Quality 층 펼침은 CVR5(`CVarGroup`)의 일이고, CF4는 그 앞의 **순수 함수** `ResolveQualityLevels`만 둔다. 입력은 "묶음 이름 → 요청 등급(묶음 변수 = `CVar<int32>`의 값)"·묶음 규칙(정의된 등급·우선순위·등급별 기기 조건 `requirements`와 예산 비용)·예산(한도 = 수 특성 × `fraction`)·기기 특성이고, 출력은 "묶음 이름 → 펼칠 등급 + 사유"다. 층에 아무것도 쓰지 않는다. 낮춤 순서: ① 등급 요구가 거짓이면 정의된 바로 아래 등급으로(가장 낮은 등급도 거짓이면 그대로 두고 unmet) ② 예산을 넘으면 우선순위가 낮은 묶음부터(같으면 이름 byte 순서) 그 예산 비용을 줄이는 한 칸씩(더 낮출 수 없으면 사유를 남기고 멈춘다). 낮춘 등급은 묶음 변수에 쓰지 않고 펼칠 때만 쓴다(CVar 11절). 표기는 `FormatQualityResolution` — `quality.shadow@1 (requested 3, lowered: <사유>)`.
- CVR5로 넘기는 것: 등급 요구·비용을 어디서 정하는지(`CVarGroup` 빌더 또는 `Quality.json` 값), 펼침 전에 이 함수를 부르는 것, 낮춤의 Explain·진단(`QualityLowered` 등) 기록, 품질 묶음이 실제로 있어야 하는 시험(등급 변경 `Unset`, 낮춘 등급의 펼침, 장치 변경 뒤 원래 선택 복귀).

### 6.2 기기 프로필

- 기기 프로필은 이름·부모·적용 대상(플랫폼)·선택 규칙·값 묶음을 가진다. 값 묶음은 branch·섹션별 설정 struct 필드와 콘솔 설정 값이다.
- **상속**: 자신부터 부모 사슬을 따라 올라가며, 아직 정해지지 않은 값만 채운다. 그래서 자식이 이긴다(UE와 같다, `Engine/Private/DeviceProfiles/DeviceProfileManager.cpp:459–537`).
- **메모리 등급별 묶음**: 기기 메모리 등급에 따라 추가로 적용할 값 묶음이다. 등급 판정은 장치 정보 입력으로 한다(UE `CVars_<Bucket>`, `:471–487`).
- **선택 순서**: 명령줄 지정 → 프로젝트 지정 → 선택 규칙(장치 정보를 인자로 받는 순수 함수) → 플랫폼 기본. UE 선택 순서(`:1631–1655`)에 대응한다. 봉인 모드는 선택 규칙을 쓰지 않고 id를 명시한다.
- **쿠킹 경계**: 쿠킹에 영향을 주는 것(텍스처 LOD 그룹·압축 형식 등)은 TargetProfile에 두고 기기 프로필에는 두지 않는다.
- 구현(CF4, `Config/DeviceProfiles.h`·`Config/DeviceProfileSettings.h`·`Config/DeviceTraits.h`):
  - 정의는 설정 struct `DeviceProfileSettings`(branch `DeviceProfiles`, 섹션 `DeviceProfiles`): `profiles`(이름 → `parent`·`platforms`·`match`(기기 조건)·`matchPriority`·`values`(대상 → 명령행 값 표기)·`memoryValues`(등급 → 값)), 프로젝트 지정 `profile`, `platformDefaults`(플랫폼 → 프로필, 없으면 플랫폼 이름과 같은 프로필), `memoryBuckets`(큰 것부터 `name`·`minimumMiB`). 대상은 콘솔 이름(품질 묶음 변수 `quality.shadow` 포함) 또는 `<Branch>.<Section>.<경로>`다. 엔진 기본은 `Engine/Config/DeviceProfiles.json`(메모리 등급과 Windows·Mac·Android 기본 프로필 — 값은 아직 없다)이다.
  - `ApplyDeviceProfile(context, 명령줄 이름, traits, out)`이 선택 순서(명령줄 `--device-profile=` → 프로젝트 지정 → 규칙(이 플랫폼의 후보 중 `matchPriority`가 크고 이름이 앞선 것) → 플랫폼 기본)로 고른다. 정의에 없는 이름은 Error `InvalidDeviceProfile` 뒤 다음 단계이고, 봉인 모드는 규칙을 건너뛴다. 자식부터 부모로 "같은 프로필의 메모리 등급 값 → 일반 값"을 아직 없는 대상만 모아(UE `CVars_<Bucket>` 먼저와 같다) 기기 프로필 층(태그 `DeviceProfile`)을 `Unset` 뒤 다시 쓴다. 모르는 부모·순환은 진단 후 거기서 멈춘다. 거부된 값은 그 값만 건너뛴다.
  - 기기 정보 입력: Config는 Graphics에 의존하지 않는다. `DeviceTraits`(이름 → 수·문자열, 표준 이름 `DeviceTraitNames` — `platform`·`memory.system_mib`·`gpu.memory_mib`·`gpu.texture_size_2d` 등)를 호스트가 채운다. 플랫폼 특성은 `HostConfig`의 `CollectPlatformDeviceTraits`(새 `platform::QueryPhysicalMemory`), GPU 특성은 장치를 만든 호스트가 `DeviceCaps`에서 옮긴다(Sandbox `AppendGraphicsDeviceTraits`). 기기 조건 `DeviceCondition`은 `trait`·`equals`·`contains`(ASCII 대소문자 무시)·`atLeast`·`below`다.
- 이름 충돌 주의: 입력 계획의 "device profile"(`Docs/Design/CoreObjectInputDetailedPlan.ko.md:994`)은 다른 개념이므로 그쪽 이름을 바꾸는 개정을 10절에 올린다.

## 7. 리플렉션 연결

- **struct 지정자 `Settings`**를 더한다: `ROCKS_STRUCT(Settings(Branch="Render", Section="RenderGraph"))`. 설정 struct는 객체가 아니라 값이다.
- **필드 지정자**:

| 지정자 | 뜻 | UE 대응 |
|---|---|---|
| `UserConfig` | 사용자 층에 쓸 수 있다 | `UPROPERTY(config, GlobalUserConfig)` 계열 |
| `ConsoleName="render.graph.immediate"` | 이 필드를 콘솔 설정으로 노출한다(값 저장소 공유) | `UPROPERTY(meta=(ConsoleVariable=...))` |
| `RestartRequired`·`StartupOnly` | 효력 시점 | `ConfigRestartRequired` |
| `RenderRead` | 설정 snapshot에 넣는다 | — |
| `ShippingExcluded` | 쿠킹 결과에서 뺀다 | (거부 목록) |
| `Cheat` | 출시 빌드에서 고정, 개발 빌드는 치트 허용 상태 검사(CVar 13.1절) | `Cheat` |

- 개정 4: `DevelopmentOnly`를 `Cheat`로 바꿨다(CVar VD5). `RestartRequired`·`StartupOnly`·`RenderRead`는 설정 struct 필드에만 남는다. CVar는 `BootCVar<T>`가 부팅 고정을 맡고, 모든 CVar가 snapshot에 든다(CVar VD6·VD15).
- 설정 struct 값 읽기는 설정 문맥에서 타입으로 한다(`ConfigContext::Get<T>()`). 반환은 sync 지점 사이에 바뀌지 않는 불변 참조다.
- 출처는 병합 묶음의 영향 경로(Touches)로 필드 경로별로 기록한다. 구현은 게시한 묶음의 override 경로(`OverridePaths` — 컨테이너 필드는 그 필드 경로)를 도구 이름 경로 텍스트로 기록한다.
- 지정자 구현: `Settings(...)`와 필드 지정자는 `FieldFlags`가 아니라 생성 `GeneratedType<S>::Settings()`의 `SettingsSchema`(`Meta/SettingsSchema.h`)에 실린다. 설정 struct는 강한 참조를 담을 수 없다(RMC2210). `ShippingExcluded`는 쿠킹(CF5) 때 더한다.

## 8. 저장·실행 중 변경·쿠킹·시험

**저장**
- 사용자 층만 저장한다. 그 아래 층을 합친 값과의 차이만 쓴다(UE `ConfigCacheIni.h:1161–1170`과 같다).
- 쓰기는 임시 파일 → Flush → 원자적 `Replace`(`Core/Source/Runtime/PlatformBase/Public/Platform/File.h:177–196`)다.
- 사용자 파일에는 스키마 버전을 둔다. 사용자 기기의 오래된 파일에 든 모르는 키는 경고 후 버린다. 엔진·프로젝트 파일의 모르는 키는 오류로 실패한다.
- 구현(CF3, `Config/UserConfig.h`):
  - 사용자 설정 변경은 `SetUserValue(context, target, value)`(사용자 층 대입, 태그 `user`)다. 저장은 `SaveUserConfig(context, files)`이고 branch마다 `WriteUserConfig`가 "사용자 층까지 합친 값"과 "그 아래 층(Engine … DeviceProfile)만 합친 값"을 필드 정규 JSON으로 비교해 다른 UserConfig 필드만 쓴다. 사용자 층보다 강한 층(세션·콘솔 등) 값은 쓰지 않는다. 독립 콘솔 설정(UserConfig 플래그)은 branch가 없으므로 `Engine` branch 파일의 `Settings`에 쓴다. 값이 없는 branch의 옛 파일은 지운다(UE `SaveBranch`와 같다). 쓰기는 `<Branch>.json.tmp` → Flush → `Replace`다.
  - 파일 형식은 Core canonical JSON이고 첫 키가 `"$version": 1`(`UserConfigSchemaVersion`)이다. 섹션은 이름 byte 순서, 필드는 스키마 순서다.
  - 읽기는 `AddUserFileText`(LoadConfigFiles가 사용자 층에 쓴다)다. 최상위 키·섹션 멤버·`Settings` 이름마다 따로 검사해 받을 수 없는 조각만 버리고 원문 위치의 Warning 진단을 남긴다(JSON 문법 오류는 파일 전체). 버전이 없거나 다르면 Warning `UserFileVersion`이다(키 검사로 처리하므로 버전 변환 규칙은 아직 없다). 등록되지 않은 콘솔 이름은 대기 값으로 받지 않고 `UnknownSetting`으로 버린다(사용자 파일은 모든 등록 뒤에 읽는다). 진단 심각도에 Warning을 더했다(`ErrorCount`에 들지 않는다).

**실행 중 변경**
- 재시작이 필요한 값은 "다음 시작 때 적용 예정"으로 기록하고 진단·UI에 보인다.
  - 구현(CF3, 개정 8에서 StartupOnly도 같은 규칙): `CompleteStartup` 뒤 Sync가 부팅 값(RestartRequired·StartupOnly 필드·콘솔 설정)의 새 합친 값이 지금 값과 다르면 지금 값(과 그 출처 스택)을 유지한다. 새 값의 가장 강한 출처가 **파일 층**(엔진·엔진 플랫폼·프로젝트·프로젝트 플랫폼·사용자 — hot reload 포함, 파일이 지워져 코드 기본값으로 돌아간 경우도)이면 `GetPendingRestarts`에 (이름, 대기 값, 지금 값)으로 남긴다. 다음 실행에도 같은 입력이 다시 들어오기 때문이다. 그 밖의 층(기기 프로필·품질·기능 묶음처럼 시작 뒤 다시 계산되는 파생 층, 세션·콘솔·코드처럼 저장되지 않는 휘발 층)은 대기를 만들지 않고 Warning `BootValueIgnored`("boot value ignored after startup")를 같은 이름·층·출처에 한 번만 낸다(개정 7). 그렇지 않으면 예를 들어 시작 뒤 GPU 특성으로 골라진 기기 프로필이 정한 값은 다음 실행에도 부팅 때는 GPU 특성이 없어 다시 대기가 되는 "영원한 대기"가 되고, 콘솔 값은 저장되지 않는데 "다음 시작에 적용"으로 보인다. 예외로, 봉인 뒤 독립 콘솔 설정(CVar `BootCVar` 포함)의 StartupOnly·RestartRequired 값에 바로 쓰는 층(콘솔·코드·범위 임시 변경)의 쓰기는 넣을 때 Error `BootSealed`로 거부한다(개정 9, CVar 9절 — 부른 쪽이 바로 효력을 기대하므로 결과로 알린다. 파생·휘발 층·파일 층은 아래 규칙 그대로라 기기 프로필 재적용 문제는 다시 생기지 않는다). 설정 struct 필드는 콘솔 이름으로 써도 이 규칙(BootValueIgnored)을 유지한다. 파생·휘발 층에 한해 StartupOnly도 같은 까닭으로 쓰기 단계의 Error 거부 대신 이 규칙을 따른다(개정 8 — 시작 뒤 기기 프로필 재적용이 StartupOnly 값을 정할 때마다 거부 오류가 나던 것을 없앴다. 파일 층의 StartupOnly 변경은 그 파일이 다음 실행에 읽히므로 대기다). 새로 대기가 된 것은 Note `RestartRequired`(CVar VD15 진단 이름)와 Info 로그다. 변경 콜백·snapshot은 바뀌지 않는다. 사용자 층 값이면 저장되어 다음 실행에 적용되고, 되돌리면 대기가 없어진다. UE `ConfigRestartRequired`는 값을 바로 바꾸고 알림만 띄우지만, Rocks는 효력 시점을 문맥이 지킨다(CVar 9절 "세 번째 종류를 두지 않는다"와 같은 뜻).
- hot reload는 에디터·개발 빌드에서 `DirectoryWatcher`(`Platform/DirectoryWatch.h:48–93`)로 `Config/`를 감시한다. 바뀐 층만 다시 읽어 출처 스택을 갱신하고, sync 지점에서 적용한다. 출시 빌드는 감시하지 않는다.
  - 구현(CF3, `Config/ConfigReload.h`): `ConfigFileReloader`가 엔진·프로젝트·사용자 `Config/` 폴더를 감시하고 문맥의 sync hook(`ConfigContext::AddSyncHook` — Sync가 큐를 보기 전에 부른다)에서 감시 이벤트를 모아 바뀐 파일만 다시 읽는다. 이벤트는 표시일 뿐이라 값은 그 sync 지점에서만 바뀐다. 파일 하나는 `ReplaceLayerText`(검사에 통과해야 그 앞에 `Unset`을 넣는다 — 잘못 고친 파일은 옛 값을 지우지 않는다)로, 지운 파일은 `Unset`으로, 사용자 파일은 관대한 읽기로 바꾼다. Overflow는 그 루트의 모든 branch 파일을 다시 읽는다. 출시 규칙이면 `Start`가 Unsupported다. `HostSettings::StartHotReload`가 호스트에서 켠다(Sandbox는 LogHost 설치 뒤). CVar 10절의 쓰기 큐가 생기면 hook은 그 큐에 넣는 쪽으로 옮긴다(CVR2).

**쿠킹**
- TargetProfile마다 층 1–4와 그 대상 기기 프로필 정의·품질 묶음 정의를 합친다. 허용 목록 밖의 값(출시 제외 필드·`Cheat` 값·에디터 전용)은 빼고 결정적 JSON으로 쓴다.
- 결과 SHA-256은 쿠킹 manifest에 들어간다.
- 실행 파일은 쿠킹된 설정을 먼저 읽고, 그 위에 사용자·세션 층을 얹는다.
- UE 미리 합친 config(`BinaryConfig.ini`, `Core/Private/Misc/ConfigCacheIni.cpp:6543–6558`)에 대응한다.

**시험**
- 설정 문맥 객체를 명시적으로 만든다. 층 목록은 메모리 문자열로 넘길 수 있다.
- 헤드리스·시험 기본은 봉인 모드다. 사용자 층·환경 변수·자동 기기 선택을 쓰지 않고, 기기 프로필 id와 장치 정보는 인자로 받는다.
- 값은 sync 지점에서만 바뀌므로 시간·경쟁에 기대지 않고 단언할 수 있다.
- `config-dump` Commandlet은 값과 출처를 결정적 JSON으로 낸다. golden 파일과 비교하는 시험에 쓴다.

## 9. 단계와 완료 기준

| 단계 | 내용 | 선행 | 완료 기준 |
|---|---|---|---|
| CF1 | 설정 문맥·층 로더(JSON 줄 주석 옵션, branch·섹션, 경로 치환, 플랫폼 부모 사슬)·출처 스택·`Unset`, 설정 struct `Settings` 지정자와 병합, 배열 연산(Config 모듈의 import 앞 단계 — 4.2절), 봉인 모드, `config-dump` | ReflectionText(구현) | 층 순서·약한 쓰기 기록·`Unset` 복귀·배열 연산 각각과 적용 순서·모르는 키 오류(위치)·치환 오류·사슬 순서의 시험, golden dump |
| CF2 | 콘솔 설정 registry(타입·검증·플래그·영역 이름 공간·대기 값)·콘솔 명령·sync 지점·콜백·설정 snapshot(세대) | CF1 | 등록 전 값 이관, 플래그별 거부(읽기 전용·품질 구성원·개발 전용), sync 지점 밖 값 불변, snapshot 세대·재사용, 콘솔 입력 문법 시험 |
| CF3 | 사용자 층 저장(차이·원자적)·재시작 필요 표시·hot reload(개발 빌드) | CF2 | 저장 왕복과 차이만 기록, 오래된 사용자 파일 처리, 감시 이벤트를 gate로 막아 sync 지점 적용 단언 |
| CF4 | 품질 묶음·기기 프로필(상속·메모리 등급·선택 규칙)·품질 조합 검증 | CF2, GX1(장치 능력) | 상속 우선순위, 등급 변경 `Unset`, 선택 순서, 불가능 조합 낮춤과 진단 |
| CF5 | 쿠킹(허용 목록·결정적 출력·SHA)·실행 파일 읽기 순서 | CF4, AssetSystem A4(쿠커) | 같은 입력 같은 바이트, 출시 제외 필드 부재, 쿠킹 설정 + 사용자 층 순서 |

- 진척: CF4 구현(2026-11-18 — 시험 `DeviceProfileTests.cpp`: 기기 조건, 선택 순서·봉인 모드, 부모 사슬·메모리 등급 우선순위·순환·프로필 바꿈의 `Unset`. `QualityValidationTests.cpp`: 요구·예산 낮춤과 순서, unmet, 표기. `HostSettingsTests`: 시작 완료의 기기 프로필 적용, 프로젝트 폴더 찾기). 품질 묶음이 있어야 하는 완료 기준(등급 변경 `Unset`, 낮춘 등급의 펼침)은 CVR5로 넘긴다. 남은 단계: CF5.
- 진척: CF1·CF2 구현(2026-11-17), CF3 구현(2026-11-18 — 시험 `UserConfigTests.cpp`·`ConfigReloadTests.cpp`: 저장 왕복 golden과 차이만 기록, 오래된 사용자 파일의 키별 Warning, 재시작 대기와 다음 실행 적용, 감시 이벤트(시험 gate `NotifyChanged`)가 sync 지점에서만 적용됨과 실제 감시기 이벤트).
- CF1·CF2는 미리 할 수 있다(Core 단독). RenderBase RB1의 설정 snapshot은 CF2 뒤에 잇는다.
- CF2의 콘솔 설정 부분(registry·플래그·snapshot·구독)은 CVar 단계 CVR1–CVR8이 대체한다. CF4의 품질 묶음은 CVR5의 `CVarGroup`을 쓴다. CVR 착수 시점은 사용자가 정한다.
- CF4의 장치 능력 연결은 GX1 뒤다. CF5는 쿠커(A4) 뒤다.

## 10. 다른 문서 개정

| 문서 | 위치 | 개정 |
|---|---|---|
| EngineImplementationPlan | 24.2절 Config 문단 | 이 문서 참조, 층 순서 표(3.1절) |
| RenderBase | 4.2절 CP4·CP8 행, 12절 | 이 문서 참조, 상태 갱신 |
| UnrealCoreFeatureAudit | UE-C21·UE-C29 행 | 이 문서 단계(CF1–CF5) |
| ReflectionFoundations·ReflectionExtensions | 지정자 표 | `Settings` struct 지정자, 필드 지정자 6종 |
| CoreObjectInputDetailedPlan | :994 "device profile" | 입력 쪽 이름을 다른 이름으로(이름 충돌) |
| Core JSON 구현 문서 | 읽기 옵션 | config 전용 줄 주석 허용 |

## 11. 결정 사항

결정 필요(개정 7, 사용자): **다음 실행 부팅 때의 GPU 특성.** 기기 프로필 규칙이 GPU 특성(`gpu.*`)으로 RestartRequired·StartupOnly 값을 정하려면 부팅(시작 완료 전)에 그 특성을 알아야 한다. 지금은 장치를 만든 뒤에야 알므로 그런 값은 `BootValueIgnored`로 무시된다. 후보: (가) 지난 실행에서 잰 장치 특성을 사용자 데이터에 캐시해 다음 부팅의 입력으로 쓴다(장치·드라이버가 바뀌면 한 번 어긋난다), (나) UE처럼 RHI 초기화 전에 플랫폼 API로 GPU를 질의한다(UE `FWindowsPlatformMisc::GetGPUDriverInfo` — `Core/Public/Windows/WindowsPlatformMisc.h:259`, `FAndroidMisc::GetGPUFamily` — `Core/Public/Android/AndroidPlatformMisc.h:203`. 플랫폼마다 질의 경로가 따로 필요하다). 구현하지 않았다.


| ID | 결정 | 상태 |
|---|---|---|
| D1 | 파일 형식 JSON + config 읽기 줄 주석 | 결정됨(2026-11-17) |
| D2 | config 층과 콘솔 설정 출처의 단일 순서·출처 스택 | 결정됨(2026-11-17) |
| D3 | 배열 연산 `$add`·`$append`·`$remove`·`$clear` | 결정됨(2026-11-17, 표기는 이 문서) |
| D4 | 기기 프로필은 설정 값 묶음 전체, LOD 그룹은 TargetProfile | 결정됨(2026-11-17) |
| D5 | 출시 빌드의 세션 층(명령줄)은 서버 대상만 | 결정됨(2026-11-17, 권장안) |
| D6 | 플랫폼 부모 사슬은 플랫폼 정의가 선언, 기본 없음 | 결정됨(2026-11-17, 권장안) |
| D7 | 이름 문법: 소문자 점 경로, 영역 이름 공간 등록 표 | 결정됨(2026-11-17, 권장안) |
| D8 | 파일 단위: branch별 파일, 섹션 = 설정 struct | 결정됨(2026-11-17, 권장안) |
| D9 | 콘솔 명령과 Commandlet registry는 따로, 이름 문법만 공유 | 결정됨(2026-11-17, 권장안) |

콘솔 설정·치트 명령의 결정은 CVar 문서 17절(VD1–VD18)이다.

## 12. 개정 이력

- 개정 10(2026-11-19): CVar CVR4 리뷰 반영. 5.1절 플래그 동작 문장을 개정 9 규칙으로 고쳤다(쓰기 거부 없음 → 바로 쓰는 층은 `BootSealed`, 늦은 등록도 같다). 8절 개정 8 문장을 파생·휘발 층으로 한정했다.
- 개정 9(2026-11-19): CVar CVR4. 봉인 뒤 독립 콘솔 설정(`BootCVar` 포함)의 부팅 값에 콘솔·코드·범위 임시 변경 층으로 바로 쓰면 넣을 때 `BootSealed`로 거부한다(CVar 9절). 파생·휘발·파일 층 규칙과 설정 struct 필드는 그대로다. 8절.
- 개정 8(2026-11-18): 시작 뒤 고정(`StartupOnly`) 값도 재시작 필요와 같은 부팅 값 규칙을 따른다(사용자 결정). 시작 뒤 파생 층·휘발 층의 변경은 쓰기 단계 Error 거부 대신 지금 값 유지와 Warning `BootValueIgnored`(같은 이름·층·출처에 한 번), 파일 층의 변경은 재시작 대기다(권장안 — VD15와 같은 뜻). 5.1·8절.

- 개정 7(2026-11-18): 재시작 대기를 파일 층에서 온 변경으로 한정했다(5.1·8절). 시작 뒤 파생 층(기기 프로필·품질·기능 묶음)과 휘발 층(세션·콘솔 등)의 RestartRequired 변경은 대기 없이 Warning `BootValueIgnored`를 한 번 낸다. 다음 실행 부팅 때 GPU 특성을 아는 방법을 결정 필요 항목으로 11절에 적었다.

- 개정 6(2026-11-18): CF4 구현을 반영했다. 기기 프로필(`DeviceProfileSettings` — 선택 순서·부모 사슬·메모리 등급, 기기 프로필 층), 기기 특성 입력(`DeviceTraits` — Config는 Graphics를 모른다), 품질 조합 검증 순수 함수(`ResolveQualityLevels` — 품질 묶음 정의·펼침은 CVar CVR5), 호스트 부트의 기기 프로필 단계·기기 특성·프로젝트 폴더와 사용자 층 결정, 게임·에디터 main의 남은 일(5.4·6·9절).

- 개정 5(2026-11-18): CF3 구현을 반영했다(개정 4 위에 다시 붙임). 사용자 층 저장(아래 층과의 차이, 독립 콘솔 설정은 Engine branch, 빈 branch의 옛 파일 삭제, 원자적 교체)과 파일 형식(`$version`), 오래된 사용자 파일의 키별 관대한 읽기(Warning 심각도), 재시작 필요 값의 대기(지금 값 유지 — CVar VD15가 그대로 쓰는 모양, 진단 이름 `RestartRequired`), hot reload(sync hook·`ReplaceLayerText`)를 5.1·8·9절에 적었다.

- 개정 4(2026-11-18): [CVar·치트 명령 설계](CVarSystem.ko.md) 개정 1을 반영했다. 5절을 그 문서로 대체하고 Config 쪽 규칙만 남겼다. 반영한 내용은 다음과 같다.
  - 3.1절: 층 넷(핫픽스·코드·범위 임시·서버)을 더하고, 품질 묶음 비고를 프로젝트보다 강한 실제 순서로 고쳤다. 범위 임시 변경은 범위 임시 층으로 옮겼다.
  - 5.1절: 플래그를 빌더 메서드·타입으로 옮겼다. 콘솔 이름 영역에 `cvar`·`cheat`·`obj`를 더했다.
  - 5.3절: 어느 스레드에서나 쓰기 큐를 쓰고, `Acquire()`·번호 조각 snapshot·`Watcher`·`OnSynced`를 쓴다.
  - 6.1절: 품질 묶음 정의는 코드 `CVarGroup`이고 `Quality.json`은 값만 바꾼다.
  - 7절: 지정자 `DevelopmentOnly`를 `Cheat`로 바꿨다.
  - 콘솔 이름 문법(D7)은 CVar VD2로 다시 확인했다.

- 개정 3(2026-11-18): 엔진 연결을 반영했다(5.4절). 호스트가 문맥 하나를 만드는 `HostConfig` 모듈과 부트 순서(등록 → 층 파일 → 추가 파일 → `--set` → `CompleteStartup` → LogHost → 엔진 루프), 엔진의 프레임 sync 지점, `AddCommandLineAssignments`, RocksCommandlet의 `config-dump` 등록, 실행 파일 위쪽에서 엔진 폴더를 찾는 규칙을 적었다.
- 개정 2(2026-11-17): CF1·CF2 구현을 반영했다. 배열 연산은 ReflectionText 확장이 아니라 Config 모듈의 import 앞 단계(4.2절), 경로 이름 문법(4.3절), 섹션 키 대소문자·대입 대상 문법(3.2절), 콘솔 이름 영역 표·값 모양·플래그 동작·명령 인자·snapshot·콜백 구현(5절), 지정자 실림 위치(7절).
- 개정 1(2026-11-17): 초안. UE 5.8.4 config 계층·ini 문법·콘솔 변수·SetBy 우선순위·렌더 스레드 전파·Scalability·Device Profile 조사와 Rocks 현황(JSON·ReflectionText·Commandlet) 조사를 바탕으로 썼다. 사용자 결정 D1–D4를 반영했고, D5–D9는 권장안대로 확정했다.
