#pragma once

#include <Core/Diagnostics/Log.h>
#include <Localization/Text.h>
#include <Platform/Atomics.h>
#include <Platform/Sync.h>
#include <Progress/ProgressScope.h>
#include <Progress/ProgressSnapshot.h>

// Progress 모듈 안의 공유 상태다(Docs/Design/ProgressReporting 5–7장). 공개 헤더에 쓰기 가능한 정적
// 저장소·thread_local을 두지 않는다(BuildTargets BS8c) — hub 저장소와 스레드별 바인딩 스택은 이
// 모듈 cpp에만 있다.
namespace rocks::detail {

inline constexpr LogCategory LogProgress{"Progress"};

// hub 켜짐 상태다.
enum class ProgressHubPhase : uint32 {
    Uninitialized,
    Running,
    ShutDown
};

// 처음 한 번만 남기는 Warning 종류다(hub 켜짐마다 다시 센다). Abandoned·ShutdownAbandoned는
// 매번이다.
enum class ProgressWarning : uint32 {
    InvalidWork,
    StepOverflow,
    ScopeDepth,
    MessageClone,
    Abandoned,
    ShutdownAbandoned,
    Count
};

inline constexpr uint32 ProgressWarningCount = static_cast<uint32>(ProgressWarning::Count);

// Primary 스택이 발행하는 깊이별 문구다(6.2절). stepText는 stepTextSerial이 그 깊이의 현재 구간
// 번호와 같을 때만 쓰이고, 아니면 defaultText(범위 기본 문구)다.
struct ProgressDepthMessage {
    Text defaultText;
    Text stepText;
    uint64 stepTextSerial = 0;
};

// 작업 기록이다(5.3절). 참조 계수로 산다: hub의 살아 있는 목록, handle, 계수, 바인딩이 붙잡는다.
// "hub 잠금" 칸은 hub 잠금 안에서만 읽고 쓴다. 원자 칸은 잠금 없이 보고 경로가 쓴다.
struct ProgressTaskRecord {
    Allocator* allocator = nullptr;
    platform::Atomic<uint32> references{1};

    // Begin이 정하고 바꾸지 않는다.
    ProgressTaskId id = NoProgressTask;
    ProgressTaskId parentId = NoProgressTask;
    uint64 epoch = 0;
    Text title;
    ProgressPresentation presentation = ProgressPresentation::Background;
    ProgressCancel cancel = ProgressCancel::NotCancellable;
    double weightInParent = 1.0;
    double selfWeight = 1.0;
    double modalDelaySeconds = DefaultModalProgressDelay;
    uint64 startNanoseconds = 0;
    CancelSource source;
    CancelToken token;

    // hub 잠금. parent는 이 기록이 살아 있는 목록에 있는 동안만 읽는다(부모는 자식보다 먼저 끝나지
    // 않는다 — 부모 Finish가 자식을 먼저 닫는다).
    ProgressTaskRecord* parent = nullptr;
    bool live = true;
    ProgressOutcome outcome = ProgressOutcome::Abandoned;
    double visible = 0.0;
    double finishedChildWeight = 0.0;
    uint32 childCount = 0;
    double scratchSum = 0.0;
    double scratchWeight = 0.0;
    double lastFraction = 0.0;
    uint64 lastDone = 0;
    uint64 lastProgressNanoseconds = 0;

    // 끝났는지다(hub 잠금 안에서 쓰고 보고 경로가 잠금 없이 읽는다).
    platform::Atomic<uint32> finished{0};

    // 바인딩.
    platform::Atomic<uint32> primaryBound{0};
    // 자기 구간·계수를 보고한 적이 있는지다(5.5절 3의 w₀).
    platform::Atomic<uint32> selfReported{0};
    // Primary 구간이 발행한 적이 있는지다(비율 모양).
    platform::Atomic<uint32> scopePublished{0};

    // 자기 진행 발행 seqlock이다(Primary 스레드만 쓴다). sequence가 홀수면 쓰는 중이다.
    platform::Atomic<uint32> sequence{0};
    platform::Atomic<uint64> stepSerial{0};
    platform::Atomic<uint64> baseBits{0};
    platform::Atomic<uint64> widthBits{0};
    // 구간 번호 발급기다(작업마다 1부터, 다시 쓰지 않는다).
    platform::Atomic<uint64> nextStepSerial{1};

    // 구간 계수(6.4절).
    platform::Atomic<uint32> scopeCounterLive{0};
    platform::Atomic<uint64> scopeCounterSerial{0};
    platform::Atomic<uint64> scopeDone{0};
    platform::Atomic<uint64> scopeTotal{0};

    // 작업 수준 계수.
    platform::Atomic<uint32> taskCounterClaimed{0};
    platform::Atomic<uint32> hasTaskCounter{0};
    platform::Atomic<uint64> taskDone{0};
    platform::Atomic<uint64> taskTotal{0};

    // 문구(messageMutex가 지킨다). depthStepSerial·publishedDepth는 Primary 스레드가 잠금 없이
    // 쓴다.
    platform::Mutex messageMutex;
    Text taskMessage;
    ProgressDepthMessage depthMessages[MaxReportedProgressDepth];
    platform::Atomic<uint64> depthStepSerial[MaxReportedProgressDepth]{};
    platform::Atomic<uint32> publishedDepth{0};
};

void RetainRecord(ProgressTaskRecord* record) noexcept;
void ReleaseRecord(ProgressTaskRecord* record) noexcept;

// 바인딩 스택 칸이다. record가 null이면 ProgressBindingSuspend 칸이다.
struct ProgressBindingFrame {
    ProgressTaskRecord* record = nullptr;
    ProgressScope* topScope = nullptr;
    uint32 scopeDepth = 0;
    bool primary = false;
};

inline constexpr uint32 MaxProgressBindingFrames =
    MaxProgressBindingDepth + MaxProgressBindingSuspendDepth;

// 스레드별 진행 상태다.
struct ProgressThreadState {
    ProgressBindingFrame frames[MaxProgressBindingFrames];
    uint32 depth = 0;
    uint32 bindingCount = 0;
    uint32 suspendCount = 0;
    // 이 스레드가 UI 스레드로 등록된 UI 세대다(0이면 아님).
    uint64 uiGeneration = 0;
    bool inPump = false;
};

extern constinit thread_local ProgressThreadState ProgressThread;

// hub 저장소다(7장).
struct ProgressHubState {
    platform::Mutex mutex;
    platform::Atomic<uint32> phase{static_cast<uint32>(ProgressHubPhase::Uninitialized)};
    platform::Atomic<uint64> epoch{0};
    Allocator* allocator = nullptr;
    ProgressClock clock = nullptr;
    void* clockContext = nullptr;

    // 살아 있는 작업(시작 순서 — 부모가 자식보다 앞).
    ProgressTaskRecord** live = nullptr;
    uint32 liveCount = 0;

    // 사건 고리. 번호는 프로세스 전체에서 늘고, 이번 켜짐의 첫 번호가 ringFirstSerial이다.
    ProgressEvent* ring = nullptr;
    uint32 ringCapacity = 0;
    uint64 nextEventSerial = 1;
    uint64 ringFirstSerial = 1;

    // 작업 id 발급기(프로세스 전체, 다시 쓰지 않는다).
    platform::Atomic<uint64> nextTaskId{1};

    // UI 펌프. uiGeneration이 0이면 펌프가 없다. pump·uiGenerationSource는 hub 잠금이 지킨다(UI
    // 스레드도 잠금 안에서 복사해 부른다 — Shutdown·Initialize가 다른 스레드에서 지우므로).
    // lastPumpNanoseconds는 간격 판정을 잠금 없이 하려고 원자다.
    platform::Atomic<uint64> uiGeneration{0};
    uint64 uiGenerationSource = 0;
    ProgressUiPump pump;
    platform::Atomic<uint64> lastPumpNanoseconds{0};

    // Warning 기록.
    platform::Atomic<uint32> warnedOnce{0};
    platform::Atomic<uint32> warningCounts[ProgressWarningCount]{};

    // RequestCancelAllTopLevel을 받은 뒤다(hub 잠금). 그 뒤 시작하는 취소 가능한 최상위 작업도 바로
    // 취소된다 — 취소(cancel 줄·EOF·Ctrl-C)가 명령의 첫 Begin보다 먼저 와도 사라지지 않게. 켜짐마다
    // 지운다.
    bool cancelAllPending = false;
};

extern constinit ProgressHubState ProgressHubGlobal;

// hub 시계(설정 시계 또는 단조 시계)다.
uint64 ProgressNow() noexcept;

// 그 기록이 지금 켜진 hub에 속하는지다(아니면 handle·계수 조작은 ShuttingDown).
inline bool IsCurrentEpoch(const ProgressTaskRecord& record) noexcept {
    return ProgressHubGlobal.phase.Load<platform::MemoryOrder::Acquire>() ==
               static_cast<uint32>(ProgressHubPhase::Running) &&
           ProgressHubGlobal.epoch.Load<platform::MemoryOrder::Relaxed>() == record.epoch;
}

// Warning을 세고 남긴다. once면 hub 켜짐마다 처음 한 번만 남긴다.
void ProgressWarn(ProgressWarning kind, const char* message) noexcept;

// UI 펌프(6.3절). 이 스레드가 등록된 UI 스레드일 때만 out-of-line 본체를 부른다.
void PumpUiSlow() noexcept;

inline void MaybePumpUi() noexcept {
    const uint64 generation = ProgressHubGlobal.uiGeneration.Load<platform::MemoryOrder::Relaxed>();
    if (generation != 0 && ProgressThread.uiGeneration == generation) {
        PumpUiSlow();
    }
}

// seqlock 쓰기(Primary 스레드만)와 읽기다.
void PublishSelfProgress(ProgressTaskRecord& record, uint64 serial, double base,
                         double width) noexcept;

struct ProgressSelfStep {
    uint64 serial = 0;
    double base = 0.0;
    double width = 0.0;
};

ProgressSelfStep ReadSelfProgress(const ProgressTaskRecord& record) noexcept;

// 문구의 원문(source culture)이다(9.2절 — invalid CatalogSnapshot으로 fallback 원문을 얻는다).
Result<ResolvedText> ProgressSourceText(Allocator& allocator, const Text& text) noexcept;

} // namespace rocks::detail
