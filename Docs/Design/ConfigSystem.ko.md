# 설정 계층과 콘솔 설정 설계

기준일: 2026-11-17, 개정 1(초안). 엔진·프로젝트·플랫폼·기기·사용자·세션·콘솔에서 오는 설정 값을 하나의 우선순위로 합치는 **설정 계층**과, 실행 중 이름으로 읽고 바꾸는 **콘솔 설정**(변수·명령)을 정한다.

[렌더 기반](RenderBase.ko.md)의 선행 작업 CP8(Config 계층)·CP4(콘솔 설정)이며, [엔진 구현 계획](EngineImplementationPlan.ko.md)의 다음 요구를 구현 스펙으로 옮긴다.
- 24.1절 표의 "Device Profile / Scalability / CVar" 행
- 24.2절 Config 우선순위: engine default → platform → project → device profile → user → session override, 런타임 변경 가능·재시작 필요 구분, 값 출처 표시, 품질 조합 검증

UE 근거는 UE 5.8.4(`release`, commit `f45729c0b6f097038e72b52d366a62baeca3275b`, 로컬 클론)의 현재 기본 경로이고, 경로는 `Engine/Source/Runtime/` 기준이다. UE 이름은 대응표와 근거에만 적는다. 레거시·deprecated 경로는 보지 않았다.

상태: **설계 확정**(결정 D1–D9). 구현 단계는 9절이다.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 파일 형식 | JSON. config 파일을 읽을 때만 `//` 줄 주석을 허용한다. 쿠킹 결과와 저장 파일은 주석 없는 결정적 JSON이다(2026-11-17 사용자 결정) | 리플렉션 스키마로 엄격히 읽어 모르는 키·타입 불일치를 줄·열 진단으로 거부한다. UE ini는 오타 키를 조용히 받는다. Rocks는 TargetProfile·CookSettings·셰이더 형식 정의가 이미 JSON이고, Core JSON은 결정적 출력과 위치 진단을 가진다(`Core/Source/Runtime/Core/Public/Core/Text/Json.h:10–21, 104–109`) |
| 우선순위 | config 층과 콘솔 변수 출처를 **하나의 순서**로 합친다. 값마다 출처별 값 스택을 둔다(사용자 결정) | UE는 config 층 순서(`Core/Public/Misc/ConfigHierarchy.h:9–46`)와 콘솔 변수 SetBy 16단계(`Core/Public/HAL/IConsoleManager.h:152–210`)가 따로 있고 겹친다 |
| 배열 병합 | 기본은 통째 교체이고, 추가·제거·비우기 연산을 둔다(사용자 결정) | UE ini `+ . - ! ^` 연산(`Core/Public/Misc/ConfigCacheIni.h:132–155`) |
| 기기 프로필 | 설정 값 묶음 전체(콘솔 변수와 설정 struct 필드)를 덮어쓰는 한 층이다. 부모 상속과 메모리 등급별 묶음을 둔다. 쿠킹에 영향을 주는 텍스처 LOD 그룹은 TargetProfile에 둔다(사용자 결정) | UE 기기 프로필은 콘솔 변수 묶음이고 텍스처 LOD 그룹을 가진다(`Engine/Classes/DeviceProfiles/DeviceProfile.h:20–71`) |
| 값 저장 단위 | 설정 struct(리플렉션) 하나가 한 섹션이다. 콘솔 변수와 설정 필드는 같은 값 저장소를 공유한다(이름 하나에 값 하나) | UE `UDeveloperSettings`의 `ConsoleVariable` 메타가 둘을 잇는 방식(`DeveloperSettings/Public/Engine/DeveloperSettings.h:31–35, 69–72`) |
| 적용 시점 | 값의 소유자는 게임 lane이다. 변경은 큐에 쌓았다가 프레임 시작 sync 지점에서만 적용하고 콜백을 부른다. 렌더 lane은 프레임 packet 안의 불변 snapshot만 읽는다 | UE 전역 sink는 정해진 지점에서만 불린다(`Core/Private/HAL/ConsoleManager.cpp:1092–1109`). 렌더 스레드 사본을 주소에 쓰는 전파(`RenderCore/Private/RenderingThread.cpp:493–513`)는 따르지 않는다(RenderBase 5.3·12절) |
| 쿠킹 | TargetProfile마다 층을 미리 합쳐 결정적 JSON으로 쓴다. 출시에 넣을 필드는 **허용 목록**으로 고른다 | UE는 `BinaryConfig.ini`로 미리 합치고, 비공개 키는 거부 목록으로 뺀다(`Engine/Config/BaseGame.ini:141–166`). 허용 목록이 더 안전하다 |
| 시험 | 전역 싱글턴 대신 설정 문맥 객체를 둔다. 헤드리스·시험 기본은 사용자 층·자동 기기 선택이 없는 봉인 모드다 | 결정적 시험 원칙 |

## 2. 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 설정 계층 `ConfigStack` | `FConfigCacheIni`·`FConfigBranch` | 층을 합친 값과 출처의 집합 |
| 설정 층 `ConfigLayer` | config layer·SetBy | 값이 오는 곳 하나(엔진·프로젝트·사용자 등) |
| 설정 struct | `UCLASS(config=)`·`UDeveloperSettings` | 리플렉션 struct 하나 = 섹션 하나 |
| 콘솔 설정 `ConsoleSetting` | console variable | 이름으로 읽고 바꾸는 값 |
| 콘솔 명령 `ConsoleCommand` | console command | 이름으로 부르는 동작 |
| 품질 묶음 `QualityGroup` | scalability group(`sg.*`) | 등급 하나가 여러 값을 정하는 묶음 |
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
| 6 | 품질 묶음 | 고른 품질 등급이 펼친 값 | UE처럼 프로젝트·기기 프로필보다 약하다(`Engine/Private/Scalability.cpp:267–353`) |
| 7 | 기기 프로필 | 선택된 프로필과 부모 사슬 | 4절 |
| 8 | 사용자 | `<UserData>/<Project>/Config/<Branch>.json` | "사용자 설정 가능" 필드만 쓸 수 있다 |
| 9 | 세션 | 명령줄 `--set=<경로>=<값>`, 시험 픽스처 | 출시 빌드는 서버 대상만 허용한다(UE와 같다, `Core/Public/Misc/ConfigCacheIni.h:57`) |
| 10 | 콘솔 | 실행 중 콘솔 입력, 코드의 범위 임시 변경 | 개발 빌드. 출시 빌드는 콘솔 입력 자체가 없다 |

**규칙.**
- 현재 출처보다 약한 출처의 쓰기는 값을 바꾸지 않는다. 다만 출처 스택에는 기록하고 진단을 남긴다. 그래서 강한 출처를 지우면 약한 출처 값이 드러난다. UE에서 약한 쓰기를 거부하는 규칙(`Core/Private/HAL/ConsoleManager.cpp:275–312`)과 같다.
- 출처 지우기 `Unset(층, 태그)`는 그 출처의 값을 스택에서 빼고 남은 최강 출처 값으로 돌아간다. 기능 묶음 해제, 품질 등급 변경, 시험 정리에 쓴다. UE `Unset`(`:1191–1230`)에 대응한다.
- 기능 묶음·품질 묶음·기기 프로필 층은 태그(묶음 이름)별로 여러 값을 쌓는다. UE "배열 우선순위"(`:760–771`)와 같다.
- "값의 출처"는 스택의 최상단이다. 진단·도구 UI가 이를 표시한다(계획 24.2절).
- 범위 임시 변경(시험·코드)은 콘솔 층에 태그를 붙여 쓰고 범위가 끝나면 그 태그를 지운다. UE `TGuardConsoleVariable`(`IConsoleManager.h:1411`)과 같다.

### 3.2 branch와 섹션

- **branch**는 파일 단위다: `Engine`·`Game`·`Input`·`Render`·`Quality`·`DeviceProfiles`·`Editor`. 목록은 이름 있는 상수 표로 두고, 플러그인은 자기 branch를 등록할 수 있다.
- **섹션**은 branch 파일 안의 최상위 키이고 설정 struct 하나에 대응한다(예: `"RenderGraph": { ... }`). 콘솔 설정만 따로 두는 섹션은 `"Settings": { "render.graph.immediate": true }`처럼 이름 → 값이다.

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

- 한 연산 객체에 여러 연산을 함께 쓸 수 있다. 적용 순서는 `$clear` → `$remove` → `$add`·`$append`다.
- 원소 비교는 리플렉션 값 동등성을 쓴다. 키가 있는 struct 배열(UE `@`·`*`)은 Map 필드로 표현하고 따로 두지 않는다.
- 연산 객체는 배열·Set 필드에만 허용한다. 다른 자리의 `$` 키는 오류다.
- 이 연산은 ReflectionText의 병합 기능으로 넣는다(9절 CF1).

### 4.3 경로 치환

문자열 필드 값에서 `{EngineDir}`·`{ProjectDir}`·`{UserDataDir}`·`{SavedDir}`를 읽을 때 펼친다. UE `%GAMEDIR%` 등(`Core/Private/Misc/ConfigCacheIni.cpp:212–300`)에 대응한다. 목록은 이름 있는 상수 표다. 표에 없는 `{...}`는 오류다.

## 5. 콘솔 설정과 명령

### 5.1 콘솔 설정

- **타입**: bool·int32·int64·float·double·String·Name·리플렉션 enum.
- **등록**: 이름·타입·기본값·도움말(필수)·플래그·검증(범위·허용 값)을 받는다.
  - 모듈 등록 함수에서 명시적으로 등록한다. 전역 생성자는 금지다(UE 지침 `IConsoleManager.h:39–58`과 같다).
  - 등록 전에 층에서 들어온 값은 "대기 값"으로 보관했다가 등록 때 넘긴다. UE `CreatedFromIni`(`Core/Private/Misc/ConfigUtilities.cpp:419–422`)와 같다.
- **이름 문법**: 소문자 점 구분 경로다. 첫 단어는 영역 이름 공간이다(`render.`·`graph.`·`stream.`·`shader.`·`quality.`·`net.`·`physics.`·`log.` 등). 영역 목록은 등록 표로 두고, 표에 없는 영역은 등록 오류다.
- **플래그**:

| 플래그 | 뜻 | UE 대응 |
|---|---|---|
| 콘솔 읽기 전용 | 콘솔 층에서 못 바꾼다(다른 층은 가능) | `ECVF_ReadOnly` |
| 개발 전용 | 출시 빌드에서는 등록되지 않고 값은 코드 기본값으로 고정된다 | `ECVF_Cheat` |
| 렌더 읽기 | 설정 snapshot에 넣는다(5.3절) | `ECVF_RenderThreadSafe` |
| 즉시 적용 / 재시작 필요 / 시작 뒤 고정 | 바뀐 값이 언제 효력을 갖는지(계획 24.2절) | — |
| 사용자 설정 가능 | 사용자 층에 쓸 수 있다 | — |
| 품질 묶음 구성원 | 품질 묶음 층이 바꿀 수 있다. 다른 변수는 품질 묶음 층 쓰기를 거부한다 | `ECVF_Scalability` |
| 출시 제외 | 쿠킹 결과에 넣지 않는다 | (거부 목록) |

- 셰이더·쿠킹 결과에 영향을 주는 값은 콘솔 설정으로 두지 않고 셰이더 형식 정의·TargetProfile에 둔다. 쿠킹 키에 들어가야 하기 때문이다(RenderBase 12절). UE의 셰이더 변경 플래그(`IConsoleManager.h:1488–1496`)는 두지 않는다.

### 5.2 콘솔 명령

- 이름·도움말·인자 스키마(리플렉션 struct)·실행 함수를 등록한다. 실행은 게임 lane의 sync 지점에서 한다.
- 콘솔 명령은 실행 중 엔진 안의 동작이다. Commandlet(`Core/Public/Core/Commandlet/Commandlet.h:36–57`)은 프로세스 단위 일괄 작업이라 registry를 합치지 않는다. 같은 이름 문법만 공유한다.
- 콘솔 입력 문법:
  - `<이름>`: 값과 출처 스택을 보인다.
  - `<이름> <값>`: 콘솔 층에 쓴다.
  - `<이름> ?`: 도움말.
  - `<명령> <인자…>`: 명령 실행.
- 자동완성은 이름 접두로 찾는다.
- 출시 빌드에는 콘솔 입력이 없다(UE `ALLOW_CONSOLE_IN_SHIPPING` 기본 0, `Core/Public/Misc/Build.h:214–215`).

### 5.3 sync 지점과 설정 snapshot

- 값의 소유자는 게임 lane이다. 콘솔 입력·사용자 설정 변경·hot reload·기능 묶음 추가 같은 변경 요청은 큐에 쌓인다.
- 엔진 프레임 3단계(프레임 시작) 전에 큐를 적용한다. 그 직후 바뀐 값의 콜백을 등록 순서대로 부른다. 다른 시점에는 값이 바뀌지 않는다.
- 같은 sync 지점에서 "렌더 읽기" 변수의 불변 snapshot을 세대 번호와 함께 만든다. 바뀐 것이 없으면 이전 snapshot을 재사용한다.
- 프레임 packet은 snapshot을 참조한다. snapshot 수명은 그 프레임 slot이 끝날 때까지다.
- 렌더 lane은 registry를 읽지 않는다(RenderBase 5.3·12절).
- worker 작업은 시작 때 받은 snapshot이나 값 복사본을 쓴다. 게임 lane 밖에서 값을 바꾸는 API는 없다(UE는 주 스레드 밖 변경이 `check` 실패, `ConsoleManager.cpp:465–508`).

## 6. 품질 묶음과 기기 프로필

### 6.1 품질 묶음

- 묶음(예: `quality.shadow`·`quality.texture`·`quality.effects`)은 등급(Low·Medium·High·Epic·Cinematic 등, 묶음별 정의)을 가진다. 등급 하나가 값 묶음을 정한다.
- 정의는 `Engine/Config/Quality.json`(branch `Quality`)의 `"<묶음>": { "<등급>": { 값들 } }`이다. UE `Scalability.ini`의 `[<Group>@<Level>]`(`Engine/Private/Scalability.cpp:267–353`)에 대응한다.
- 등급을 바꾸면 그 묶음 태그의 품질 묶음 층 값을 `Unset`하고 새 등급 값을 쓴다.
- 사용자가 고른 등급은 사용자 층에 저장한다.
- 품질 조합 검증(계획 24.2절): 등급 조합이 장치 능력(`DeviceCaps`)으로 불가능하면 정해진 순서로 낮추고, 낮춘 결과와 사유를 진단에 남긴다.

### 6.2 기기 프로필

- 기기 프로필은 이름·부모·적용 대상(플랫폼)·선택 규칙·값 묶음을 가진다. 값 묶음은 branch·섹션별 설정 struct 필드와 콘솔 설정 값이다.
- **상속**: 자신부터 부모 사슬을 따라 올라가며, 아직 정해지지 않은 값만 채운다. 그래서 자식이 이긴다(UE와 같다, `Engine/Private/DeviceProfiles/DeviceProfileManager.cpp:459–537`).
- **메모리 등급별 묶음**: 기기 메모리 등급에 따라 추가로 적용할 값 묶음이다. 등급 판정은 장치 정보 입력으로 한다(UE `CVars_<Bucket>`, `:471–487`).
- **선택 순서**: 명령줄 지정 → 프로젝트 지정 → 선택 규칙(장치 정보를 인자로 받는 순수 함수) → 플랫폼 기본. UE 선택 순서(`:1631–1655`)에 대응한다. 봉인 모드는 선택 규칙을 쓰지 않고 id를 명시한다.
- **쿠킹 경계**: 쿠킹에 영향을 주는 것(텍스처 LOD 그룹·압축 형식 등)은 TargetProfile에 두고 기기 프로필에는 두지 않는다.
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
| `DevelopmentOnly` | 출시 빌드에서 고정 | `Cheat` |

- 설정 struct 값 읽기는 설정 문맥에서 타입으로 한다(`ConfigContext::Get<T>()`). 반환은 sync 지점 사이에 바뀌지 않는 불변 참조다.
- 출처는 병합 묶음의 영향 경로(Touches)로 필드 경로별로 기록한다.

## 8. 저장·실행 중 변경·쿠킹·시험

**저장**
- 사용자 층만 저장한다. 그 아래 층을 합친 값과의 차이만 쓴다(UE `ConfigCacheIni.h:1161–1170`과 같다).
- 쓰기는 임시 파일 → Flush → 원자적 `Replace`(`Core/Source/Runtime/PlatformBase/Public/Platform/File.h:177–196`)다.
- 사용자 파일에는 스키마 버전을 둔다. 사용자 기기의 오래된 파일에 든 모르는 키는 경고 후 버린다. 엔진·프로젝트 파일의 모르는 키는 오류로 실패한다.

**실행 중 변경**
- 재시작이 필요한 값은 "다음 시작 때 적용 예정"으로 기록하고 진단·UI에 보인다.
- hot reload는 에디터·개발 빌드에서 `DirectoryWatcher`(`Platform/DirectoryWatch.h:48–93`)로 `Config/`를 감시한다. 바뀐 층만 다시 읽어 출처 스택을 갱신하고, sync 지점에서 적용한다. 출시 빌드는 감시하지 않는다.

**쿠킹**
- TargetProfile마다 층 1–4와 그 대상 기기 프로필 정의·품질 묶음 정의를 합친다. 허용 목록 밖의 필드(출시 제외·개발 전용·에디터 전용)는 빼고 결정적 JSON으로 쓴다.
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
| CF1 | 설정 문맥·층 로더(JSON 줄 주석 옵션, branch·섹션, 경로 치환, 플랫폼 부모 사슬)·출처 스택·`Unset`, 설정 struct `Settings` 지정자와 병합, 배열 연산(ReflectionText 확장), 봉인 모드, `config-dump` | ReflectionText(구현) | 층 순서·약한 쓰기 기록·`Unset` 복귀·배열 연산 각각과 적용 순서·모르는 키 오류(위치)·치환 오류·사슬 순서의 시험, golden dump |
| CF2 | 콘솔 설정 registry(타입·검증·플래그·영역 이름 공간·대기 값)·콘솔 명령·sync 지점·콜백·설정 snapshot(세대) | CF1 | 등록 전 값 이관, 플래그별 거부(읽기 전용·품질 구성원·개발 전용), sync 지점 밖 값 불변, snapshot 세대·재사용, 콘솔 입력 문법 시험 |
| CF3 | 사용자 층 저장(차이·원자적)·재시작 필요 표시·hot reload(개발 빌드) | CF2 | 저장 왕복과 차이만 기록, 오래된 사용자 파일 처리, 감시 이벤트를 gate로 막아 sync 지점 적용 단언 |
| CF4 | 품질 묶음·기기 프로필(상속·메모리 등급·선택 규칙)·품질 조합 검증 | CF2, GX1(장치 능력) | 상속 우선순위, 등급 변경 `Unset`, 선택 순서, 불가능 조합 낮춤과 진단 |
| CF5 | 쿠킹(허용 목록·결정적 출력·SHA)·실행 파일 읽기 순서 | CF4, AssetSystem A4(쿠커) | 같은 입력 같은 바이트, 출시 제외 필드 부재, 쿠킹 설정 + 사용자 층 순서 |

- CF1·CF2는 미리 할 수 있다(Core 단독). RenderBase RB1의 설정 snapshot은 CF2 뒤에 잇는다.
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

## 12. 개정 이력

- 개정 1(2026-11-17): 초안. UE 5.8.4 config 계층·ini 문법·콘솔 변수·SetBy 우선순위·렌더 스레드 전파·Scalability·Device Profile 조사와 Rocks 현황(JSON·ReflectionText·Commandlet) 조사를 바탕으로 썼다. 사용자 결정 D1–D4를 반영했고, D5–D9는 권장안대로 확정했다.
