#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

namespace rocks {

// 스레드별 로그 문맥 스택의 최대 깊이다. 넘는 범위는 버리고 LogEventFlags::ContextTruncated를
// 세운다.
inline constexpr usize MaxLogContextDepth = 8;

namespace detail {

// 문맥 항목 하나다. 이름은 리터럴(정적 저장소), 값은 범위가 끝날 때까지 빌린다.
struct LogContextEntry {
    const char* name = nullptr;
    usize nameBytes = 0;
    StringView value;
};

// 이 스레드의 문맥(바깥 범위부터)이다. truncated는 깊이를 넘어 버린 범위가 지금 열려 있는지다.
// 돌려준 view는 범위가 바뀌기 전까지만 유효하다(캡처가 곧바로 복사한다).
Span<const LogContextEntry> CurrentLogContext(bool& truncated) noexcept;

void PushLogContext(const char* name, usize nameBytes, StringView value) noexcept;
void PopLogContext() noexcept;

} // namespace detail

// 범위 동안 이 스레드의 모든 로그 기록에 "이름=값" 문맥을 붙인다(설계 6.5절, 육하원칙의
// "누가·어디서"). 출력의 {Context}는 바깥 범위부터 "World=Main Stage=BeginPlay"처럼 쓴다.
//
// - 이름: 문자열 리터럴(정적 저장소). 레이아웃·JSON 키로 그대로 쓴다.
// - 값: 범위가 끝날 때까지 살아 있어야 한다. 로그를 남길 때 기록으로 복사하므로 범위가 끝난 뒤의
//   출력(비동기 출력 스레드)은 안전하다.
// - 스레드별 고정 깊이(MaxLogContextDepth) 스택이다. 깊이를 넘는 안쪽 범위는 기록에 들어가지
//   않고, 그 범위가 열린 동안의 기록은 ContextTruncated로 표시된다.
// - 같은 스레드에서 만든 역순으로 끝나야 한다(스택). 힙을 쓰지 않는다.
//
// 사용 예:
//   LogContextScope world("World", world.Name());
//   LogContextScope stage("Stage", "BeginPlay");
//   Log::Info(LogEngine, "Spawned {Count} actors", count); // ... World=Main Stage=BeginPlay
class LogContextScope {
public:
    template <usize Count>
    LogContextScope(const char (&name)[Count], StringView value) noexcept {
        detail::PushLogContext(name, Count - 1, value);
    }

    ~LogContextScope() noexcept {
        detail::PopLogContext();
    }

    LogContextScope(const LogContextScope&) = delete;
    LogContextScope& operator=(const LogContextScope&) = delete;
};

} // namespace rocks
