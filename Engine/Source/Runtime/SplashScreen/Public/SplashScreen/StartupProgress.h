#pragma once

#include <Core/Containers/Span.h>
#include <Progress/ProgressCounter.h>
#include <SplashScreen/SplashTypes.h>

namespace rocks {
class CatalogSnapshot;
class ProgressBinding;
class ProgressScope;
class ProgressTaskHandle;
class Text;
} // namespace rocks

// 시작 진행이다(Docs/Design/SplashScreen 7·8절, UE EditorInit의 FScopedSlowTask(100) 대응).
// 호스트가 시작 작업 "Starting <앱>"(ProgressHub 최상위 작업, Hidden)을 열고 호스트 스레드를
// Primary로 묶은 뒤 가장 바깥 구간을 단계(StartupStep)로 나눈다. 시작 경로의 라이브러리
// 코드(Engine::Initialize· ModuleLoader::StartPhase)가 여는 구간이 현재 단계를 채우고, 다른
// 스레드의 일은 단계 계수로 보고한다. 스플래시는 MakeSplashSource가 만든 상태 원천으로 중계
// 스냅샷을 당긴다(스플래시 스레드).
//
// 사용 예(호스트 스레드):
//   const StartupStep steps[] = {StartupStep::HostServices, StartupStep::Engine,
//                                StartupStep::FirstFrame};
//   StartupProgressDesc desc;
//   desc.appTitle = &sandboxTitle;
//   desc.steps = Span<const StartupStep>(steps);
//   auto startup = StartupProgress::Begin(allocator, desc);
//   auto source = startup.Value().MakeSplashSource(allocator, nullptr);
//   splashDesc.source = source.Value();
//   auto splash = SplashScreen::Show(allocator, splashDesc);
//   startup.Value().Enter(StartupStep::HostServices);
//   modules.StartPhase(LoadingPhase::PostConfigInit);
//   startup.Value().Enter(StartupStep::Engine);
//   engine.Initialize(config);
//   ...                                  // 주 창 첫 프레임 뒤(9.4절)
//   startup.Value().Finish();
//   splash.Value().Hide();
//
// 스레드: Begin·Enter·MakeStepCounter·Finish·소멸은 Begin한 호스트 스레드에서 한다(Primary 바인딩과
// 구간을 품는다). 단계 계수의 Add와 상태 원천의 poll은 아무 스레드다. ProgressHub는 Begin 전에 켜져
// 있어야 하고 이 객체보다 오래 켜져 있어야 한다(먼저 닫히면 보고는 아무 일도 하지 않는다).
namespace rocks::splash {

// 시작 단계다(8.1절). 호스트가 밖에서 경계를 정할 수 있는 단위만 둔다.
enum class StartupStep : uint8 {
    // StartPhase(PostConfigInit), trace 세션·콘솔 명령
    HostServices,
    // Engine::Initialize 전체
    Engine,
    // 에셋 마운트·색인·너겟 준비(엔진 밖에서 하는 부분)
    Assets,
    // 셰이더 라이브러리·전역 셰이더
    Shaders,
    // 게임: GameInstance·기본 맵, Studio: 에디터 모듈·UI·기본 레벨
    Content,
    // 주 창 첫 프레임 렌더·표시
    FirstFrame,
    Count
};

inline constexpr uint32 StartupStepCount = static_cast<uint32>(StartupStep::Count);

// 단계 기본 가중치다(8.1절 — 합 100. SP5 측정 보정이 지난 실행 시간으로 바꾼다).
inline constexpr double DefaultStartupStepWeights[StartupStepCount] = {10.0, 40.0, 20.0,
                                                                       15.0, 10.0, 5.0};

// 단계 이름이다(로그·프로필 키, 예: "HostServices"). 범위 밖은 "Unknown".
ROCKS_SPLASHSCREEN_API const char* StartupStepName(StartupStep step) noexcept;

struct StartupProgressDesc {
    // 필수다. 작업 제목 "Starting <앱>"에 들어간다(Text::Format — 지역화 이력 유지).
    const Text* appTitle = nullptr;
    // 이 호스트가 들어갈 단계다(가중치 합에 들어간다). 들어가지 않을 단계를 빼야 막대가 끝에서
    // 뛰지 않는다. 비면 모든 단계다. 같은 단계를 두 번 넣으면 InvalidArgument.
    Span<const StartupStep> steps;
    // 단계 시간 측정 시계(나노초)다. null이면 단조 시계다.
    uint64 (*nowNanoseconds)(void* context) noexcept = nullptr;
    void* clockContext = nullptr;
};

// 단계마다 걸린 시간(초)이다. 들어가지 않은 단계는 0이다.
struct StartupStepTimes {
    double seconds[StartupStepCount]{};
};

class ROCKS_SPLASHSCREEN_API StartupProgress {
public:
    // 이동은 같은 호스트 스레드 안에서만 한다(바인딩·구간 상태는 따로 할당돼 주소가 바뀌지 않는다).
    StartupProgress(StartupProgress&& other) noexcept;
    StartupProgress& operator=(StartupProgress&& other) noexcept;
    StartupProgress(const StartupProgress&) = delete;
    StartupProgress& operator=(const StartupProgress&) = delete;
    // Finish 없이 소멸하면 시작 작업이 Abandoned로 끝난다(ProgressHub Warning).
    ~StartupProgress() noexcept;

    // 시작 작업을 열고 이 스레드를 Primary로 묶는다. 오류: appTitle 없음·단계 중복·범위 밖
    // InvalidArgument, ProgressHub Begin·Bind의 오류(NotReady·ShuttingDown 등), 할당 실패는 그
    // 오류.
    static Result<StartupProgress> Begin(Allocator& allocator,
                                         const StartupProgressDesc& desc) noexcept;

    // 다음 단계로 넘어간다. 한 단계는 한 번만, desc.steps에 든 단계만 들어간다(어기면 Warning 뒤
    // 무시). 순서는 호출 순서다.
    void Enter(StartupStep step) noexcept;

    // 현재 단계 구간을 채우는 계수다(다른 스레드의 일 — 셰이더 job·에셋 색인 worker). 단계에
    // 들어가기 전이면 빈 계수다. 오류: 살아 있는 구간 계수가 이미 있으면 AlreadyExists.
    Result<ProgressCounter> MakeStepCounter(uint64 total) noexcept;

    // 스플래시 상태 원천을 만든다(7.1절). 원천은 시작 작업 id와 자기 allocator·사건 cursor만
    // 가지므로 이 객체보다 오래 살아도 된다(작업이 끝나면 "끝남"을 보고한다). catalog가 있으면
    // 문구를 그 문화로 풀고 원천이 사본을 쥔다(없으면 원문). allocator는 원천이 release될 때까지
    // 살아 있어야 하고 다른 스레드의 할당·해제를 지원해야 한다. 오류: 할당·Clone 실패는 그 오류.
    Result<SplashStatusSource> MakeSplashSource(Allocator& allocator,
                                                const CatalogSnapshot* catalog) noexcept;

    // 시작을 끝낸다: 가장 바깥 구간을 닫고 바인딩을 풀고 시작 작업을 outcome으로 끝낸다.
    // Succeeded면 단계 시간을 Info 로그로 남긴다. 주 창으로 넘기기 전에 시작이 멈췄으면(창
    // 닫기·엔진 실패) 호스트가 Abandoned·Failed로 부른다(소멸에 맡기면 ProgressHub Warning이
    // 남는다). 두 번째부터는 아무 일도 하지 않는다.
    void Finish(ProgressOutcome outcome = ProgressOutcome::Succeeded) noexcept;

    // 지금까지의 단계 시간이다(Finish 뒤면 확정 값).
    StartupStepTimes StepTimes() const noexcept;

    ProgressTaskId TaskId() const noexcept;

private:
    struct State;

    explicit StartupProgress(State* state) noexcept : state_(state) {
    }

    void Release() noexcept;

    State* state_ = nullptr;
};

} // namespace rocks::splash
