#pragma once

#include <Core/CancelToken.h>

// 작업 진행 보고의 공용 타입·한도 상수다(Docs/Design/ProgressReporting 5·6·7·11장). 작업(task)은
// 사용자에게 보이는 일 하나이고, 구간(ProgressScope)은 바인딩된 작업 아래의 중첩 가중 범위다(UE
// FScopedSlowTask 모델). 보고하는 코드는 받는 쪽을 모르고, 받는 쪽은 ProgressHub::Snapshot으로
// 당긴다.
namespace rocks {

class Text;

// 작업 식별자다. 0은 "없음"이고 1부터 늘며 재사용하지 않는다(프로세스 수명 동안).
using ProgressTaskId = uint64;
inline constexpr ProgressTaskId NoProgressTask = 0;

// 받는 쪽이 작업을 보이는 방식이다. Blocking은 modalDelaySeconds 뒤 진행 창, Background는 알림,
// Hidden은 화면에 보이지 않고 로그(시작·끝)와 하위 작업 합성에만 쓰인다(9장).
enum class ProgressPresentation : uint8 {
    Blocking,
    Background,
    Hidden
};

// 사용자(받는 쪽)가 취소를 요청할 수 있는지다. UserCancellable 작업만 hub가 취소 표지를 쥔다.
enum class ProgressCancel : uint8 {
    NotCancellable,
    UserCancellable
};

// 작업이 끝난 결과다. Abandoned는 Finish 없이 handle이 사라졌거나, 부모가 먼저 끝났거나, hub가
// 닫힐 때 살아 있던 작업이다.
enum class ProgressOutcome : uint8 {
    Succeeded,
    Failed,
    Cancelled,
    Abandoned
};

// 스냅샷 때 정하는 진행 모양이다(5.4절).
enum class ProgressShape : uint8 {
    Indeterminate,
    Fraction,
    Count
};

// 바인딩의 역할이다(6.1절). Primary 스택의 구간만 진행과 문구를 발행하고, Secondary 스택의 구간은
// 취소 조회만 한다.
enum class ProgressBindingRole : uint8 {
    Primary,
    Secondary
};

// ---- 한도 상수(11장) ----

// Blocking 작업의 진행 창이 뜨기 전 지연이다(UE Engine/Source 지연 창 호출의 최빈값).
inline constexpr double DefaultModalProgressDelay = 1.0;
// UI 펌프 hook을 부르는 최소 간격(초)이다(UE FSlowTask MinUpdateTimeS와 같음).
inline constexpr double ProgressUiPumpInterval = 0.2;
// 동시에 살아 있을 수 있는 작업 수다(셰이더 종류별·패키지별 하위 작업 여유).
inline constexpr uint32 MaxLiveProgressTasks = 4096;
// 스레드 하나의 바인딩 스택 깊이다(중첩 작업).
inline constexpr uint32 MaxProgressBindingDepth = 16;
// 바인딩 스택에 ProgressBindingSuspend가 더 얹을 수 있는 칸 수다. 넘으면 코드 결함으로 Fatal이다.
inline constexpr uint32 MaxProgressBindingSuspendDepth = 16;
// 스레드 하나의 구간 깊이다(UE 실사용 깊이보다 넉넉). 넘는 구간은 아무 일도 하지 않는다.
inline constexpr uint32 MaxProgressScopeDepth = 64;
// 스냅샷에 담는 구간 문구 깊이다(진행 창 줄 수).
inline constexpr uint32 MaxReportedProgressDepth = 4;
// trace span으로 남기는 Primary 구간 깊이다(UE RegionId와 같은 깊이).
inline constexpr uint32 MaxTracedProgressDepth = 2;
// 시작·끝 사건 고리의 기본 용량이다(소비자 Pump 간격 동안의 시작·끝).
inline constexpr uint32 DefaultProgressEventLogCapacity = 1024;
// 사건 고리 용량의 상한이다(설정 검사).
inline constexpr uint32 MaxProgressEventLogCapacity = 1u << 20;

// 이번 구간 작업량이 남은 양을 넘는지 볼 때 허용하는 상대 오차다(0.1 + 0.2 같은 부동소수 합으로
// 생기는 경고를 막는다).
inline constexpr double ProgressWorkTolerance = 1e-9;

// hub 시계다. 단조 나노초를 돌려준다(시험은 가짜 시계를 주입해 직접 전진한다).
using ProgressClock = uint64 (*)(void* context) noexcept;

// ProgressHub::Initialize 설정이다(7.1절).
struct ProgressHubSettings {
    // null이면 platform::MonotonicNanoseconds다.
    ProgressClock clock = nullptr;
    void* clockContext = nullptr;
    // 시작·끝 사건 고리 용량이다. 1 이상 MaxProgressEventLogCapacity 이하.
    uint32 eventLogCapacity = DefaultProgressEventLogCapacity;
};

// 작업 하나를 시작하는 설명이다(5.1절).
struct ProgressTaskDesc {
    // 필수다. 유효하고 원문이 비어 있지 않아야 한다. Begin이 Clone하므로 호출 뒤 버려도 된다.
    const Text* title = nullptr;
    // 부모 작업이다(NoProgressTask면 최상위). 부모는 살아 있어야 한다.
    ProgressTaskId parent = NoProgressTask;
    ProgressPresentation presentation = ProgressPresentation::Background;
    ProgressCancel cancel = ProgressCancel::NotCancellable;
    // 호출자의 취소 token이다. 최상위 작업에만 쓴다(하위 작업은 부모 token에 묶인다 — 함께 주면
    // InvalidArgument, 5.2절). UserCancellable이면 hub source가 이 token에 연결되고, 아니면 작업
    // token이 이 token 그대로다.
    CancelToken externalCancel;
    // 부모 안 몫이다. 유한한 양수.
    double weightInParent = 1.0;
    // 하위 작업과 섞일 때 자기 구간·계수의 몫이다. 유한한 0 이상.
    double selfWeight = 1.0;
    // Blocking만 쓴다. 유한한 0 이상(초).
    double modalDelaySeconds = DefaultModalProgressDelay;
};

// hub 진단 계수다(이번 Initialize 뒤). 입력 검사 Warning은 켜짐마다 처음 한 번만 남기므로 그 계수는
// 0 또는 1이고, Abandoned 계수는 Finish 없이 사라진 handle 수, shutdownAbandoned는 살아 있는 작업을
// 남긴 채 닫힌 횟수다.
struct ProgressHubStats {
    uint32 invalidWorkWarnings = 0;
    uint32 stepOverflowWarnings = 0;
    uint32 scopeDepthWarnings = 0;
    uint32 messageCloneWarnings = 0;
    uint32 abandonedTasks = 0;
    uint32 shutdownAbandoned = 0;
};

// UI 펌프 hook이다(6.3절). pump는 UI 스레드에서만, 펌프 안 재진입 없이 불린다.
struct ProgressUiPump {
    void (*pump)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

namespace detail {
struct ProgressTaskRecord;
} // namespace detail

} // namespace rocks
