#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <SplashScreen/SplashLayout.h>
#include <SplashScreen/SplashTypes.h>

// 시작 화면(스플래시) 창이다(Docs/Design/SplashScreen 9·11절, UE FPlatformSplash 대응). 호스트가
// 설정·로그 준비 직후 Show하고, 주 창이 첫 프레임을 보인 뒤 Hide한다. Windows는 전용 스레드가 창을
// 소유해 호스트 스레드가 막혀도 다시 그린다. Windows 밖 플랫폼은 아직 구현이 없다(macOS는 SP3).
//
// 사용 예:
//   splash::SplashTextValue texts[] = {{splash::SplashTextSlot::Title, "Rocks Sandbox"}};
//   splash::SplashShowDesc desc;
//   desc.kind = splash::SplashKind::Game;
//   desc.engineDirectory = engineDir;
//   desc.texts = texts;
//   auto shown = splash::SplashScreen::Show(allocator, desc);
//   if (!shown) { Log::Warning(...); }          // 시작은 계속한다
//   ...                                         // 엔진 시작, 주 창 첫 프레임
//   shown.Value().Hide();
namespace rocks::splash {

// 최소 표시 시간의 기본값과 상한(초)이다(Docs/Design/SplashScreen 9.7절 — 설정
// ui.splash.min_seconds).
inline constexpr double DefaultSplashMinimumDisplaySeconds = 2.0;
inline constexpr double MaxSplashMinimumDisplaySeconds = 10.0;

// 처음 칸 글자 하나다(UTF-8, Show가 복사한다).
struct SplashTextValue {
    SplashTextSlot slot = SplashTextSlot::Title;
    StringView text;
};

// Show 설명이다. 문자열은 Show 안에서 복사한다.
struct SplashShowDesc {
    SplashKind kind = SplashKind::Game;
    // 이미지 찾기 입력이다(SplashImage.h — 설정 경로·프로젝트·엔진 폴더). engineDirectory나
    // configuredImage 중 하나는 있어야 한다.
    StringView configuredImage;
    StringView projectDirectory;
    StringView engineDirectory;
    // 처음 칸 글자다.
    Span<const SplashTextValue> texts;
    // 칸 표시다(Game만 본다 — Studio는 모두 보인다, 6.3절). 글자가 빈 칸은 그려지지 않는다.
    bool showTitle = true;
    bool showVersion = false;
    bool showProgressText = false;
    bool showCopyright = true;
    bool showProgressBar = true;
    // 배치 덮어쓰기(6.5절)다. 칸마다 하나이고 Custom 칸은 여기 있는 칸만 그린다. Show가 검사하고
    // 복사한다(범위 밖·중복 칸은 InvalidArgument).
    Span<const SplashTextPlacement> placements;
    // 막대 배치 덮어쓰기다. nullptr이면 기본(6.2절)이다. Show가 복사한다.
    const SplashBarPlacement* bar = nullptr;
    // 진행 원천이다. 비어도 된다(막대는 불확정 표시). Show가 소유를 넘겨받는다(성공·실패 모두 —
    // release는 Hide·소멸 때, Show가 스레드를 띄우기 전에 실패하면 Show 안에서 부른다).
    SplashStatusSource source;
    // 최소 표시 시간(초)이다. 창이 뜬 뒤 이 시간이 지나야 CanHide가 참이다(9.7절). 0이면 기다리지
    // 않는다. 음수·NaN·무한이면 Show가 InvalidArgument, MaxSplashMinimumDisplaySeconds를 넘으면 그
    // 값으로 줄인다.
    double minimumDisplaySeconds = DefaultSplashMinimumDisplaySeconds;
    // 시계다(나노초). nullptr이면 단조 시계다. 시험이 주입한다.
    uint64 (*nowNanoseconds)(void* context) noexcept = nullptr;
    void* clockContext = nullptr;
};

class SplashScreenTestAccess;
struct SplashPresenterFactory;

class ROCKS_SPLASHSCREEN_API SplashScreen {
public:
    // 스플래시를 띄운다. 판정(DecideSplashVisibility)은 하지 않는다 — 호스트 몫이다. 호스트
    // 스레드에서는 설명 검사·이미지 찾기(파일 존재 확인)만 하고 스플래시 스레드를 띄운 뒤 바로
    // 돌아온다. 디코딩·창 생성 실패는 스플래시 스레드가 Warning으로 남기고 State()가 Failed가 된다.
    // 오류: engineDirectory·configuredImage가 모두 비었으면 InvalidArgument, 이미지가 없으면
    // NotFound, 이미 떠 있는 스플래시가 있으면 AlreadyExists(프로세스에 하나), 구현이 없는 플랫폼은
    // Unsupported, 할당·스레드 실패는 그 오류다.
    static Result<SplashScreen> Show(Allocator& allocator, const SplashShowDesc& desc) noexcept;

    SplashScreen(SplashScreen&& other) noexcept;
    SplashScreen& operator=(SplashScreen&& other) noexcept;
    SplashScreen(const SplashScreen&) = delete;
    SplashScreen& operator=(const SplashScreen&) = delete;
    // Hide한다.
    ~SplashScreen();

    // 칸 글자를 바꾼다. 아무 스레드에서 부른다(작은 잠금 + 갱신 알림). 길이 초과는 잘라 쓴다.
    void SetText(SplashTextSlot slot, StringView utf8) noexcept;

    // 상태다. 호스트 스레드에서 부른다.
    SplashState State() const noexcept;

    // 최소 표시 시간이 지나 닫아도 되는지다(9.7절): 창이 뜬 뒤(Shown) 최소 시간이 지났거나, 창을
    // 만들지 못했거나(Failed) 이미 닫혔으면(Hidden) 참이고, 아직 만드는 중(Starting)이면 거짓이다.
    // 호스트가 주 창으로 넘기기 전에 프레임마다 본다. Hide 자체는 이 값과 무관하게 바로 닫는다.
    // 호스트 스레드에서 부른다.
    //
    // 사용 예(프레임 끝):
    //   if (firstFrameDone && splash.CanHide()) { window.ShowAndActivate(); hideNextFrame = true; }
    bool CanHide() const noexcept;

    // 창을 잠시 숨기고 다시 보인다(호스트가 띄우는 모달 UI 앞뒤 — 9.5절). 갱신은 계속한다. 호스트
    // 스레드에서 부른다.
    void Suspend() noexcept;
    void Resume() noexcept;

    // 창을 닫고 스플래시 스레드를 끝낸 뒤 진행 원천의 release를 부른다. 창 생성이 끝나기 전이면
    // 끝날 때까지 기다린다. 멱등이다. 호스트 스레드에서 부른다.
    void Hide() noexcept;

private:
    friend class SplashScreenTestAccess;
    struct Shared;

    explicit SplashScreen(Shared* shared) noexcept;

    // Show의 본체다. factory가 presenter를 만든다(Show는 플랫폼 presenter, 시험은 가짜).
    static Result<SplashScreen> ShowWith(Allocator& allocator, const SplashShowDesc& desc,
                                         const SplashPresenterFactory& factory) noexcept;

    Shared* shared_ = nullptr;
};

} // namespace rocks::splash
