# 시작 화면(스플래시) — 11. API 초안

[목차](README.ko.md)

## 11. API 초안

SP1에서 구현한 부분(`SplashTypes.h`·`SplashImage.h`·`SplashLayout.h`·`SplashModel.h`·`SplashScreen.h`)은 헤더가 정본이다. 아래와 다른 점: 판정 입력은 `HostKind`·`RunTraits` 대신 같은 뜻의 bool 칸(`interactiveHost`·`headless`·`unattended`)이고 플랫폼 값은 `MakeSplashVisibilityInput()`이 채운다(AppInfo 구현 전), 칸 표시는 `SplashShowDesc`의 `show*` bool, `SplashState`·`SetText`·`Suspend`·`Resume`·`Hide`는 아래와 같다. `StartupProgress`는 SP2다. 개정 4에서 `SplashShowDesc::minimumDisplaySeconds`와 `SplashScreen::CanHide()`를 더했다(9.7절).

```cpp
namespace rocks::splash {

enum class SplashKind : uint8 { Game, Studio };
enum class SplashSkipReason : uint8 {
    None, UnsupportedPlatform, NotInteractiveHost, NoMainWindow, DisabledByCommandLine, Headless, Unattended,
    DisabledBySettings, NoDesktopSession, NoMainThreadHost, ImageNotFound, ImageInvalid, WindowFailed,
};
enum class SplashState : uint8 { Starting, Shown, Failed, Hidden };
enum class SplashTextSlot : uint8 { Title, Version, Progress, Copyright, Custom1, Custom2, Custom3, Custom4, Custom5, Custom6 };

struct SplashVisibilityInput {
    HostKind host = HostKind::Game;
    RunTraits traits;
    bool opensMainWindow = false;              // 4절 호스트별 표
    bool noSplashOption = false;
    bool settingsEnabled = true;
    platform::DesktopSessionState desktop = platform::DesktopSessionState::Unknown;
    bool mainThreadHost = false;               // macOS만 본다
};
ROCKS_SPLASHSCREEN_API SplashSkipReason DecideSplashVisibility(const SplashVisibilityInput&) noexcept;

struct SplashStatus {                           // poll이 채운다
    bool determinate = false;                  // false면 막대는 불확정 표시
    double fraction = 0.0;                     // 0..1, determinate일 때
    bool finished = false;
    bool messageChanged = false;               // true면 아래 문구로 바꾼다(빈 문구 = 비우기)
    char message[kMaxSplashLineBytes]{};       // UTF-8, NUL 없음
    uint32 messageBytes = 0;
};
struct SplashStatusSource {                     // SplashScreen이 소유한다(7.1절)
    bool (*poll)(void* context, SplashStatus& out) noexcept = nullptr;   // false면 바뀐 것 없음
    void (*release)(void* context) noexcept = nullptr;                  // Hide 뒤 한 번
    void* context = nullptr;
};

struct SplashShowDesc {
    SplashKind kind = SplashKind::Game;
    const SplashSettings* settings = nullptr;  // 필수, Show 안에서 복사한다
    StringView projectDirectory;               // 비면 프로젝트 없음
    StringView engineDirectory;                // 필수
    Span<const SplashTextValue> texts;         // 처음 글자(Title·Version·Copyright 등, UTF-8, 복사)
    Span<const SplashCustomText> customTexts;  // Custom 칸 배치 + 글자, 최대 kMaxCustomSplashTexts
    SplashStatusSource source;                 // 비어도 된다
    uint64 (*nowNanoseconds)(void*) = nullptr; // 시험 주입, nullptr이면 단조 시계
    void* clockContext = nullptr;
};

class ROCKS_SPLASHSCREEN_API SplashScreen {     // 이동만(구현은 힙 상태 하나를 가리킨다)
public:
    // 판정은 하지 않는다(4절은 호스트 몫). Windows는 스레드만 띄우고 돌아오며, 창 실패는 스플래시
    // 스레드가 로그로 남기고 State()가 Failed가 된다. 실패해도 원천의 release는 Hide·소멸 때 불린다.
    static Result<SplashScreen> Show(Allocator&, const SplashShowDesc&) noexcept;
    void SetText(SplashTextSlot, StringView utf8) noexcept;   // 아무 스레드, 길이 초과는 잘라 쓴다
    SplashState State() const noexcept;
    void Suspend() noexcept;
    void Resume() noexcept;
    void Hide() noexcept;                       // 멱등
    ~SplashScreen();                            // Hide
};

struct StartupProgressDesc {
    const Text* appTitle = nullptr;            // 필수 — 작업 제목 "Starting <앱>"(Text::Format)에 들어간다
    Span<const StartupStep> steps;             // 들어갈 단계(가중치 합) — 비면 전부, 중복은 InvalidArgument
    uint64 (*nowNanoseconds)(void*) = nullptr; // nullptr이면 단조 시계
    void* clockContext = nullptr;
    StringView profilePath;                    // 보정 프로필(8.2절) — 비면 끔
    StringView profileTarget;                  // 프로필 안의 대상 이름(profilePath가 있으면 필수)
};

class ROCKS_SPLASHSCREEN_API StartupProgress {  // 이동만(같은 호스트 스레드) — 바인딩·구간은 따로 할당
public:
    static Result<StartupProgress> Begin(Allocator&, const StartupProgressDesc&) noexcept;
    void Enter(StartupStep step) noexcept;      // 한 단계 한 번, 순서는 호출 순서
    Result<ProgressCounter> MakeStepCounter(uint64 total) noexcept;   // 현재 단계 구간 계수(아무 스레드 Add)
    Result<SplashStatusSource> MakeSplashSource(Allocator&, const CatalogSnapshot*) noexcept;
    void Finish(ProgressOutcome = ProgressOutcome::Succeeded) noexcept;   // 구간·바인딩 닫기, 작업 결과, Succeeded면 단계 시간 Info 로그(SP5: 프로필 저장)
    void MarkReady() noexcept;                  // 실제 시작 일 끝(첫 프레임 뒤) — 그 뒤 대기는 단계 시간·프로필에 넣지 않음(SP5)
    StartupStepTimes StepTimes() const noexcept;
    ProgressTaskId TaskId() const noexcept;
    ~StartupProgress();                         // Finish 없이 소멸하면 Abandoned, 프로필은 쓰지 않음
};

} // namespace rocks::splash
```

- `Result<T>`는 T의 noexcept 이동을 요구한다(`Core/Result.h`). `StartupProgress`는 바인딩·구간 상태를 따로 할당해 쥐므로(주소가 바뀌지 않는다) 이동할 수 있게 하고 `Result<StartupProgress>`로 돌려준다(개정 7 — 처음 안은 `UniquePtr`).
- 오류 코드는 `Core/Error.h`의 기존 값만 쓴다: 잘못된 설명(`settings`·`engineDirectory`·`appTitle` 없음, Custom 칸 초과) `InvalidArgument`, 이미 떠 있는 스플래시가 있는데 Show `AlreadyExists`(프로세스에 하나), 이미지를 못 찾음 `NotFound`(사유 `ImageNotFound`를 로그로), Windows·macOS가 아닌 플랫폼의 Show `Unsupported`.
- 공개 헤더에 쓰기 가능한 정적 저장소·`thread_local`을 두지 않는다(BuildTargets BS8c). 프로세스 하나 제약은 cpp 안 원자 표시로 한다.
