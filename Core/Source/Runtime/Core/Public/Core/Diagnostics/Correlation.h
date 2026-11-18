#pragma once

#include <Base/Types.h>
#include <Core/Result.h>

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

// 상관 id 묶음이다. 0은 "없음"이다.
struct CorrelationIds {
    uint64 session = 0;
    uint64 task = 0;
    uint64 asset = 0;
    uint64 entity = 0;
    uint64 request = 0;
};

// 스택 칸 하나다. ext는 상위 계층(trace)이 쓰는 칸이다. 새로 쌓으면 0, 복원하면 사본의 값이다.
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

// 이 스레드의 스택에 상관 id를 쌓고 소멸 때 내린다. 만든 스레드에서 LIFO 순서로 소멸해야 한다(스택
// 객체로 쓴다). 순서가 어긋나면 Fatal이다.
class CorrelationScope {
public:
    explicit CorrelationScope(const CorrelationIds& ids) noexcept;
    explicit CorrelationScope(const CorrelationToken& token) noexcept;
    ~CorrelationScope() noexcept;

    CorrelationScope(const CorrelationScope&) = delete;
    CorrelationScope& operator=(const CorrelationScope&) = delete;

private:
    // 0 = 쌓지 않음(빈 사본), 1 = 스택에 쌓음, 2 = 깊이 상한으로 세기만 함.
    uint8 pushed_ = 0;
};

// 이 스레드 스택 맨 위 칸의 사본이다. 비어 있으면 valid가 false다.
CorrelationToken CaptureCorrelation() noexcept;

// 이 스레드 스택 맨 위의 id를 out에 쓴다. 비어 있으면 false이고 out은 바뀌지 않는다.
bool CurrentCorrelation(CorrelationIds& out) noexcept;

// 이 스레드 스택의 깊이다(넘쳐 세기만 한 쌓기는 빼고). 비어 있는지를 분기 하나로 볼 때 쓴다.
usize CorrelationDepth() noexcept;

// 쌓기·내리기 관찰자를 정한다(원자 포인터 하나). 프로세스 시작 단계(다른 스레드가 쌓기 전)에
// 부른다. 같은 관찰자를 다시 주면 성공하고, 다른 관찰자가 이미 있으면 AlreadyExists다. nullptr은
// 해제이며, 다른 스레드가 쌓기·내리기를 하지 않는 종료 단계에서만 부른다(그래서 해제 뒤에는 불리지
// 않는다). observer는 해제될 때까지 살아 있어야 한다.
Result<void> SetCorrelationObserver(const CorrelationObserver* observer) noexcept;

namespace detail {

// 맨 위 칸이다(상위 계층이 사본을 만들기 전에 ext를 채울 때). 비어 있으면 nullptr이다.
CorrelationEntry* TopCorrelationEntry() noexcept;

// 지금 걸린 관찰자다(시험이 잠시 바꿨다가 되돌릴 때).
const CorrelationObserver* CurrentCorrelationObserver() noexcept;

} // namespace detail

} // namespace rocks
