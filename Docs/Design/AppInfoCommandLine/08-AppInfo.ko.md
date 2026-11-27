# 앱 정보와 명령줄 — 8. 앱 정보 `AppInfo`

[목차](README.ko.md)

## 8. 앱 정보 `AppInfo`

### 8.1 레코드

```cpp
namespace rocks {

// 실행 종류다. 빌드 대상 종류(build::TargetType)와 다르다. Program 대상 하나가 commandlet으로도 도구로도 돈다.
enum class HostKind : uint8 { Game, Commandlet, Tool, TestRunner };

// 실행 성격이다. 명령줄과 빌드·호스트 기본값에서만 정하고(10절) 실행 중 바뀌지 않는다.
// 뜻과 기본값은 UE 5.8 FApp·FAppTime과 같다(10.2절 표).
struct RunTraits {
    bool unattended = false;        // 사람이 보지 않는다: 대화상자·입력 대기 금지
    bool unattendedInput = false;   // 무인이어도 입력 장치를 연다
    bool benchmark = false;         // 비출시 빌드만. 고정 step·고정 시드를 함께 뜻한다
    bool fixedSeed = false;         // 무작위 시드를 상수로
    bool useFixedTimeStep = false;  // 엔진 프레임 delta를 fixedDeltaSeconds로 고정한다
    double fixedDeltaSeconds = 1.0 / 30.0; // 고정 step 길이(초). UE FixedDeltaTime 기본값과 같다
    bool headless = false;          // 창·그래픽 장치를 만들지 않는다
    bool audioOutput = true;        // 소리 장치를 만든다. 부트가 Server 대상·commandlet(--allow-commandlet-audio가 없으면)·benchmark·--no-sound면 false로 정한다(UE FApp::CanEverRenderAudio)
    bool audioMuted = false;        // 장치·믹서는 돌리고 마지막 출력만 끈다(--mute-audio, UE FApp::IsAudioMuted)

    // 엔진이 실제로 고정 step을 쓰는지다(UE UpdateTimeAndHandleMaxTickRate와 같은 판정).
    bool UsesFixedTimeStep() const noexcept { return benchmark || useFixedTimeStep; }
};

// 프로세스 정보다. 호스트 부트가 한 번 만들고 AppInfoScope로 설치한 뒤 바꾸지 않는다.
// 문자열 view는 AppInfo가 소유한 블록을 가리키며 AppInfo 수명 동안 유효하다.
class AppInfo {
public:
    // 설치된 AppInfo다. 전제: AppInfoScope가 살아 있다(Debug·Development는 assert).
    static const AppInfo& Get() noexcept;
    // 설치 여부다. 부트 전 경로(Fatal 처리 등)가 확인할 때만 쓴다.
    static bool IsInstalled() noexcept;

    StringView ProjectName() const noexcept;       // 없으면 빈 값(도구)
    StringView ProjectDirectory() const noexcept;
    StringView ExecutableName() const noexcept;    // 확장자를 뗀 실행 파일 이름
    HostKind Host() const noexcept;
    const Guid& InstanceId() const noexcept;
    const Optional<Guid>& SessionId() const noexcept; // 없으면 단독 실행
    StringView SessionName() const noexcept;
    const RunTraits& Traits() const noexcept;
    const build::BuildDescription& Build() const noexcept; // build::Description()과 같다
};

} // namespace rocks
```

- 만드는 길은 `AppInfo::Create(const AppInfoDescription&, Allocator&) → Result<AppInfo>` 하나다. 입력 문자열을 한 블록으로 복사한다. 길이 상한은 이름 붙은 상수다(`MaxProjectNameBytes` 등). 넘으면 `CapacityExceeded`다. UE처럼 64자에서 조용히 자르지 않는다.
- 문자열은 UTF-8이고, 프로젝트 이름은 파일 이름에 쓸 수 있는 문자만 허용한다. 문법은 Config 사용자 층 경로가 쓰는 규칙을 따른다.

### 8.2 수명과 쓰기 규칙

- **한 번 채우고 불변이다.** setter가 없다. 바꿔야 하는 값이라면 AppInfo에 두지 않는다(8.6절).
- **설치는 범위 객체로 한다.** `AppInfoScope installed(appInfo);`가 프로세스 설치 포인터를 release 저장으로 바꾸고, 소멸이 되돌린다. `Get()`은 acquire 읽기다.
  - 이미 설치돼 있으면 생성자가 Fatal이다(두 번 설치는 부트 순서 버그다). 이 점이 `LogInstallScope`와 다르다. `LogInstallScope`는 이전 값을 보관해 중첩을 허용한다(`LogSystem.h:368–377`).
  - 시험은 시험마다 scope를 만든다. 시험 실행기(`HostKind::TestRunner`)는 기본 AppInfo 하나를 설치하고, 다른 값이 필요한 시험은 TestSupport의 `ReplaceAppInfoForTest` scope로 잠시 바꾼다. 이 scope만 이전 값을 보관하고 되돌린다. 그동안 다른 스레드가 읽지 않는 시험만 쓴다(문서화한 전제).
- **왜 전역인가(D3).** 로그·trace·크래시 처리·Fatal 대화상자 정책은 Core 깊은 곳에서 프로젝트 이름·instance id·unattended를 읽어야 한다. 이것을 모두 인자로 전하면 Core API가 앱 정보를 줄줄이 받아야 한다. 값이 불변이고 설치·해제가 범위로 묶이므로 UE 전역의 스레드·수명 문제(동기화 없는 쓰기, 첫 호출 시점에 굳는 static)가 없다.
  - AppInfo를 설치하지 않는 실행(일부 도구·시험)이 있으므로, Core 깊은 곳의 읽는 쪽은 `IsInstalled()`를 먼저 본다.

### 8.3 부트 순서

Config 5.4절 부트 순서 앞뒤에 다음을 끼운다.

| 순서 | 일 | 실패 처리 |
|---|---|---|
| 0 | Launch 진입점(`RunApplication`)이 `ProcessCommandLine`을 캡처한다(Windows는 CRT의 넓은 문자 argv에서 — 12절) | 종료 코드 `ApplicationDescription::commandLineFailureExitCode`(기본 1, RocksCommandlet 2), stderr 한 줄 |
| 1 | 호스트가 공용 옵션 struct(`HostCommandLineOptions`)와 자기 옵션 struct를 파싱한다(14절) | 사용법 오류: 진단 + 짧은 사용법, 종료 2. 로그가 없으므로 stderr에 직접 쓴다. 게임 호스트 Release의 알 수 없는 인자는 오류가 아니라 9단계 경고로 미룬다(D29) |
| 2 | `RunTraits`를 정하고 Fatal 대화상자 정책을 PlatformBase에 바로 적용한다(unattended면 대화상자 없음) | — |
| 3 | `AppInfo::Create` → `AppInfoScope` 설치. 입력은 0–2단계 결과와 `build::Description()`뿐이다 | 종료 코드 1 |
| 4 | `HostSettings::Initialize` — `projectName`·`projectDir`를 AppInfo에서 채운다. 설정 등록 코드는 여기서 `AppInfo::Get().Traits()`를 읽을 수 있다(10.1절) | 기존과 같다 |
| 5–8 | 층 파일 → `--config` 파일 → `--set` 값 → `CompleteStartup`(기존 2–5단계). `--set` 값은 파싱한 배열을 그대로 받는다(14.3절) | 기존과 같다 |
| 9 | LogHost 설치 → 시작 로그 묶음(D14) → 1단계에서 미룬 경고 | 기존과 같다 |
| 10 | trace 세션 → `--trace` 목록 적용 → 엔진 루프 | 기존과 같다 |

- 2단계의 대화상자 정책은 AppInfo를 거치지 않고 PlatformBase 값 하나로 둔다. 0–2단계의 Fatal도 같은 값을 본다.
- Commandlet 호스트도 같은 순서다. 그래서 commandlet에서도 `--set`·`--trace`가 동작한다. 지금은 쓸 수 없다(5.2절).

### 8.4 스레드

| 대상 | 쓰는 쪽 | 읽는 쪽 | 동기화 |
|---|---|---|---|
| AppInfo 내용 | 만들 때 한 번(게임 lane, 부트) | 모든 스레드 | 설치 포인터의 release·acquire. 내용은 불변 |
| 설치 포인터 | `AppInfoScope` 생성·소멸(부트 3단계·종료) | `Get()` | 원자 포인터. 로그 출력 스레드·worker·렌더 lane은 설치 뒤에 시작하고 해제 전에 끝난다 |
| `RunTraits`가 PlatformBase에 넣은 대화상자 정책 | 부트 2단계 한 번 | Fatal 경로(아무 스레드) | relaxed 원자 값 하나(정책 하나라 순서 문제가 없다) |

### 8.5 빌드 정보 조회

`AppInfo::Build()`는 `build::Description()`(11절)과 같은 레코드를 가리킨다. 상수라 스레드 안전하다. AppInfo를 설치하기 전에도 `build::Description()`을 직접 부를 수 있다.

### 8.6 AppInfo에 두지 않는 것

| UE 값 | Rocks에서 맡는 곳 | 이유 |
|---|---|---|
| 포커스(`HasFocus`) | PlatformWindow 이벤트를 받은 엔진이 프레임 상태로 공개한다: `Engine::IsForeground()`(게임 lane). 다른 lane이 필요하면 프레임 packet에 넣는다. benchmark면 늘 true(UE `APPC` 347–350과 같다) | 실행 중 바뀐다. 원천은 `ui::Platform`·PlatformWindow다 |
| 볼륨 배수·포커스 잃었을 때 배수 | 오디오 모듈의 `audio.*` CVar(`audio.master.volume`·`audio.unfocused.volume` — [오디오 시스템](../AudioSystem/03-Design/03.08-EngineAndWorld.ko.md) 3.8.5절) | 설정 값이다. UE도 config에서 읽는다(`APPC` 316) |
| 그래픽 API 이름 | Graphics 장치 정보 | 장치를 만든 뒤에야 안다 |
| 게임 중인지(`IsGame`) | World 종류 | World마다 다르다 |
| 세션 이름 바꾸기 | 없음 | 실행 중 바꿀 이유가 없다 |
