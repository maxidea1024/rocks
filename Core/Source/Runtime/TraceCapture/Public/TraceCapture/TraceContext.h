#pragma once

#include <Base/Types.h>
#include <Core/Diagnostics/Correlation.h>
#include <Core/Text/StringView.h>
#include <TraceCapture/TraceChannel.h>

namespace rocks {

// 표준 채널 "context"다. 세션이 Start 때 등록하며 처음에는 꺼져 있다. 꺼져 있어도 문맥 스택 자체는
// 동작한다(로그가 같은 상관 id를 쓴다).
extern TraceChannel TraceContextChannel;

// 상관 id 묶음과 저장한 문맥(사본)이다(Docs/Design/TraceSystem.ko.md 6.4절). 스택은 Core
// CorrelationScope 하나이고(로그와 trace가 같은 값을 쓴다, 계획 29.4절), trace는 칸의 확장 칸에
// 정의 캐시(ext[0] = 세션 serial 하위 32비트 << 32 | 문맥 id)와 쌓기 기록 serial(ext[1])을 둔다.
// 사본은 값과 정의 캐시를 함께 가지므로 세션이 바뀌어도 복원할 수 있고, 같은 세션이면 복원 때
// 정의를 다시 쓰지 않고 id만 기록한다(UE metadata trace의 Save·Restore 대응).
using TraceContextValues = CorrelationIds;
using TraceContextToken = CorrelationToken;

// 이 스레드의 문맥 스택에 문맥을 쌓고, 소멸 때 내린다(Core CorrelationScope와 같다. 공개
// CorrelationScope로 쌓아도 같은 기록이 남는다).
//
//   TraceContextScope request({.request = requestId});
//   const TraceContextToken saved = TraceCaptureContext();   // task 만들 때
//   ...                                                      // 실행 스레드에서
//   TraceContextScope restored(saved);
//
// - 채널 "context"가 켜져 있고 세션이 있으면 쌓기·복원 때 Trace.ContextPush(tick·문맥 id)를,
//   내릴 때 Trace.ContextPop을 기록한다(Core 스택의 관찰자, 세션 Start가 건다). 문맥 정의
//   (Trace.ContextDefinition: id·값)는 정의 기록으로 처음 한 번 나간다. 내림 기록은 쌓기를 기록한
//   경우에만 낸다.
// - 스택 깊이 상한(MaxCorrelationDepth)을 넘은 쌓기는 스택에 들어가지 않고 기록도 하지 않는다(짝이
//   되는 내림도 아무것도 하지 않는다).
// 스레드: 만든 스레드에서 LIFO 순서로 소멸해야 한다(스택 객체로 쓴다).
class TraceContextScope {
public:
    explicit TraceContextScope(const TraceContextValues& values) noexcept : scope_(values) {
    }

    explicit TraceContextScope(const TraceContextToken& token) noexcept : scope_(token) {
    }

    TraceContextScope(const TraceContextScope&) = delete;
    TraceContextScope& operator=(const TraceContextScope&) = delete;

private:
    CorrelationScope scope_;
};

// 이 스레드 문맥 스택 맨 위의 사본이다. 채널이 켜져 있고 세션이 있으면 문맥 정의를 먼저 써서, 같은
// 세션에서의 복원이 id만으로 끝나게 한다. 스택이 비어 있으면 valid가 false다.
TraceContextToken TraceCaptureContext() noexcept;

// 이 스레드 문맥 스택 맨 위의 값을 out에 쓴다. 스택이 비어 있으면 false이고 out은 바뀌지 않는다.
inline bool TraceCurrentContext(TraceContextValues& out) noexcept {
    return CurrentCorrelation(out);
}

namespace detail {

// Core 상관 id 스택에 trace 관찰자를 건다(세션 Start가 부른다, 프로세스 수명 동안 유지).
void InstallTraceContextObserver() noexcept;

} // namespace detail

// 이 스레드의 trace 정보(이름·묶음·정렬 순위)를 정한다(Docs/Design/TraceSystem.ko.md 6.5절). 장수
// 스레드(게임 lane·렌더 lane·worker·IO·writer·오디오)는 시작 때 부른다. 값은 스레드에 보관되어 이후
// 어느 세션에 등록되든 스레드 정보 기록(Trace.ThreadInfo, 정의 기록)에 들어간다. 지금 세션이 있으면
// 바로 등록해 정보를 낸다. 이름·묶음은 TraceStandardFormat::MaxThreadNameBytes를 넘으면 앞부분만
// 쓴다.
void TraceSetThreadInfo(StringView name, StringView group = {}, int32 sortOrder = 0) noexcept;

} // namespace rocks
