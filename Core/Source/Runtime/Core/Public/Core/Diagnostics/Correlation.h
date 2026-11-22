#pragma once

#include <Base/Types.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 상관 id다(Docs/Design/LoggingSystem.ko.md 14.1.4, L14). 로그와 trace가 같은 값을
// 쓴다(계획 29.4절). 스레드별 고정 깊이 스택이고 할당·lock이 없다. 로그 기록은 맨 위 값을 캡처한다.
// trace는 확장 칸과 관찰자로 자기 상태(정의 캐시·기록 여부)를 덧붙인다. Core는 확장 칸의 뜻을
// 모른다.
//
// 사용 예:
//   CorrelationScope request(CorrelationIds{.request = requestId});
//   Log::Info(LogNet, "Request accepted");         // 기록에 request=... 가 실린다
//   const CorrelationToken saved = CaptureCorrelation();   // 다른 스레드로 넘길 때
//   ...                                                    // 그 스레드에서
//   CorrelationScope restored(saved);
namespace rocks {

// 스레드별 스택 깊이 상한이다. 넘친 쌓기는 스택에 들어가지 않고 세기만 한다.
inline constexpr usize MaxCorrelationDepth = 16;

// 상위 계층용 확장 칸 수다(Core는 뜻을 모르고 옮기기만 한다).
inline constexpr usize CorrelationExtensionCount = 2;
// 확장 칸의 쓰임 규칙이다. 사본 칸(0)은 사본·복원 때 그대로 옮겨지고, 쌓기 칸(1)은 쌓기·복원마다
// 0에서 시작한다(사본에 실려 다니지 않는다 — 다른 쌓기의 상태가 섞이지 않게).
inline constexpr usize CorrelationCarriedExtension = 0;
inline constexpr usize CorrelationPushExtension = 1;

// 상관 id 묶음이다. 0은 "없음"이다.
struct CorrelationIds {
    uint64 session = 0;
    uint64 task = 0;
    uint64 asset = 0;
    uint64 entity = 0;
    uint64 request = 0;
};

// 상관 id 칸 수와 출력 이름·순서다(레이아웃 {Correlation}, JSON lines corr, trace, log view가 같이
// 쓴다).
inline constexpr usize CorrelationFieldCount = 5;
inline constexpr StringView CorrelationFieldNames[CorrelationFieldCount] = {
    "session", "task", "asset", "entity", "request"};

// ids를 CorrelationFieldNames 순서의 값 배열로 옮긴다.
constexpr void CorrelationFieldValues(const CorrelationIds& ids,
                                      uint64 (&values)[CorrelationFieldCount]) noexcept {
    values[0] = ids.session;
    values[1] = ids.task;
    values[2] = ids.asset;
    values[3] = ids.entity;
    values[4] = ids.request;
}

// 모든 칸이 0인지다.
constexpr bool IsEmptyCorrelation(const CorrelationIds& ids) noexcept {
    return (ids.session | ids.task | ids.asset | ids.entity | ids.request) == 0;
}

// 스택 칸 하나다. ext는 상위 계층(trace)이 쓰는 칸이다(CorrelationCarriedExtension·
// CorrelationPushExtension 규칙).
struct CorrelationEntry {
    CorrelationIds ids;
    uint64 ext[CorrelationExtensionCount]{};
};

// 저장한 문맥(맨 위 칸의 사본)이다. 스택이 비어 있었으면 valid가 false이고 복원은 아무것도 하지
// 않는다.
struct CorrelationToken {
    CorrelationEntry entry;
    bool valid = false;
};

// 쌓기·내리기 관찰자다. 쌓기·내리기 때만 불린다(로그 기록마다 드는 비용은 없다). 함수는
// noexcept이고 같은 스레드에서 불린다. onPush는 칸을 고칠 수 있다(ext). restored는 사본 복원인지다.
struct CorrelationObserver {
    void (*onPush)(CorrelationEntry& entry, bool restored) noexcept = nullptr;
    void (*onPop)(const CorrelationEntry& entry) noexcept = nullptr;
};

// 쌓기 결과다(CorrelationScope와, 쌓기를 조건부로 하는 상위 계층 scope가 쓴다).
enum class CorrelationPushState : uint8 {
    // 쌓지 않음(빈 사본)
    None,
    // 스택에 쌓음
    Pushed,
    // 깊이 상한으로 세기만 함
    Counted
};

namespace detail {

// 칸 하나를 쌓는다(쌓기 칸 확장을 0으로 하고 관찰자를 부른다). restored는 사본 복원인지다.
// CorrelationScope의 느린 경로이고, 쌓기 여부를 직접 정하는 scope(TraceTaskScope)도 쓴다. 결과가
// None이 아니면 같은 스레드에서 LIFO 순서로 PopCorrelation을 불러야 한다.
ROCKS_CORE_API CorrelationPushState PushCorrelation(const CorrelationEntry& entry,
                                                    bool restored) noexcept;

// PushCorrelation이 쌓은 칸을 내린다(state는 None이 아니어야 한다). 순서가 어긋나면 Fatal이다.
ROCKS_CORE_API void PopCorrelation(CorrelationPushState state) noexcept;

} // namespace detail

// 이 스레드의 스택에 상관 id를 쌓고 소멸 때 내린다. 만든 스레드에서 LIFO 순서로 소멸해야 한다(스택
// 객체로 쓴다). 순서가 어긋나면 Fatal이다. 사본 복원의 빈 경로(스택이 비어 있던 사본)는 인라인
// 분기 하나다(task·lane이 실행마다 거친다).
class CorrelationScope {
public:
    explicit CorrelationScope(const CorrelationIds& ids) noexcept
        : pushed_(detail::PushCorrelation(CorrelationEntry{ids, {}}, false)) {
    }

    explicit CorrelationScope(const CorrelationToken& token) noexcept
        : pushed_(token.valid ? detail::PushCorrelation(token.entry, true)
                              : CorrelationPushState::None) {
    }

    ~CorrelationScope() noexcept {
        if (pushed_ != CorrelationPushState::None) {
            detail::PopCorrelation(pushed_);
        }
    }

    CorrelationScope(const CorrelationScope&) = delete;
    CorrelationScope& operator=(const CorrelationScope&) = delete;

private:
    CorrelationPushState pushed_ = CorrelationPushState::None;
};

// 이 스레드 스택 맨 위 칸의 사본이다. 비어 있으면 valid가 false다.
ROCKS_CORE_API CorrelationToken CaptureCorrelation() noexcept;

// 이 스레드 스택 맨 위의 id를 out에 쓴다. 비어 있으면 false이고 out은 바뀌지 않는다.
bool CurrentCorrelation(CorrelationIds& out) noexcept;

// 이 스레드 스택의 깊이다(넘쳐 세기만 한 쌓기는 빼고). 비어 있는지를 분기 하나로 볼 수 있다.
// 깊이는 Core .cpp의 thread_local이라 호출 하나를 거친다(공개 헤더에 thread_local을 두지 않는다 —
// Docs/Design/BuildTargets.ko.md 3.8절 BD29).
ROCKS_CORE_API usize CorrelationDepth() noexcept;

// 쌓기·내리기 관찰자를 정한다(원자 포인터 하나). 같은 관찰자를 다시 주면 아무것도 바꾸지 않고
// 성공하고(trace는 세션 Start마다 다시 건다), 다른 관찰자가 이미 있으면 AlreadyExists다. nullptr은
// 해제이며, 다른 스레드가 쌓기·내리기를 하지 않는 종료 단계에서만 부른다(그래서 해제 뒤에는 불리지
// 않는다). observer와 그 함수는 해제될 때까지 살아 있어야 한다(보통 정적 객체로 두고 해제하지
// 않는다).
ROCKS_CORE_API Result<void> SetCorrelationObserver(const CorrelationObserver* observer) noexcept;

namespace detail {

// 맨 위 칸이다(상위 계층이 사본을 만들기 전에 ext를 채울 때). 비어 있으면 nullptr이다.
ROCKS_CORE_API CorrelationEntry* TopCorrelationEntry() noexcept;

// 지금 걸린 관찰자다(시험이 잠시 바꿨다가 되돌릴 때).
const CorrelationObserver* CurrentCorrelationObserver() noexcept;

} // namespace detail

} // namespace rocks
