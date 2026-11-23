#pragma once

#include <Base/Types.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <TraceCapture/TraceChannel.h>

// 로그 기록을 trace에 남기는 sink다(Docs/Design/LoggingSystem.ko.md 14.1.5, LG15). 표준 채널
// "log"가 켜져 있을 때만 쓴다. 꺼져 있으면 Write는 분기 하나로 돌아온다.
//
// 기록(TraceSystem.ko.md 6.6절):
// - Trace.LogMessageSpec(정의): spec id(호출 지점 = EventId·카테고리·위치 문자열 해시)·EventId·
//   레벨·줄, 가변 카테고리·템플릿·파일·함수. 세션마다 호출 지점당 한 번이다(고정 중복 표). 표가
//   상한까지 차거나, spec이 정의 기록 상한을 넘거나, 정의 쓰기가 실패하면 그 기록에 카테고리·
//   템플릿을 싣고 센다(그 기록은 위치를 잃는다).
// - Trace.LogMessage(순서 번호): 캡처 tick(변환 없음)·생산자 OS 스레드 id·프레임·EventId·레벨·
//   플래그·상관 id, 가변 렌더된 메시지와 동봉 템플릿(각각 MaxTraceLogMessageBytes까지, 넘으면 잘림
//   비트).
// sink는 로그 출력 스레드에서 불리므로 생산자 스레드·시각·상관 id를 기록에 명시한다.
//
// 재귀 차단: 스레드별 재진입 표시가 있고, 카테고리 "Trace"(와 그 하위)의 기록은 trace로 보내지
// 않는다(trace 쓰기 실패가 남기는 Warning이 다시 trace로 돌지 않게). 세션이 없으면 보내지 않는다.
// 손실: 기록 버퍼가 없으면 다른 기록처럼 기다리지 않고 버리며 Lost()로 센다.
//
// 사용 예(엔진 호스트, trace 세션 시작 뒤·정지 전):
//   TraceLogSink traceLog;
//   (void)logs.AddSink(traceLog);
//   ...
//   (void)logs.RemoveSink(traceLog);   // trace.Stop() 전에
namespace rocks {

// 표준 채널 "log"다. 세션이 Start 때 등록하며 처음에는 꺼져 있다.
ROCKS_TRACECAPTURE_API extern TraceChannel TraceLogChannel;

// Trace.LogMessage에 싣는 렌더된 메시지(와 동봉 템플릿)의 최대 byte 수다.
inline constexpr usize MaxTraceLogMessageBytes = 1024;

// spec 중복 표의 칸 수(2의 거듭제곱)와 채우는 상한(3/4, 열린 주소 탐색이 길어지지 않게)이다.
inline constexpr usize TraceLogSpecTableSlots = 4096;
inline constexpr usize TraceLogSpecTableLimit = TraceLogSpecTableSlots / 4 * 3;

namespace detail {

// 이 스레드가 TraceLogSink::Write 안에 있는지다. LogSystem을 거친 호출은 LogSystem의 sink 표시가
// 이미 재귀를 막지만, sink를 직접 부르는 호스트·시험 경로와 trace 쓰기 안에서 남는 로그를 다시 받지
// 않도록 sink 자신도 막는다(두 표시는 지키는 범위가 다르다). 표시는 TraceCapture .cpp의
// thread_local이다 (공개 헤더에 thread_local을 두지 않는다 — Docs/Design/BuildTargets 3.8절
// BD29).
ROCKS_TRACECAPTURE_API bool InTraceLogWrite() noexcept;

// 재진입 표시를 켜고 소멸 때 이전 값으로 되돌린다. Write가 쓰고, 시험이 "Write 안" 상태를 만들 때도
// 쓴다.
class ROCKS_TRACECAPTURE_API TraceLogWriteScope {
public:
    TraceLogWriteScope() noexcept;
    ~TraceLogWriteScope() noexcept;

    TraceLogWriteScope(const TraceLogWriteScope&) = delete;
    TraceLogWriteScope& operator=(const TraceLogWriteScope&) = delete;

private:
    bool previous_;
};

} // namespace detail

class ROCKS_TRACECAPTURE_API TraceLogSink final : public RuntimeLogSink {
public:
    TraceLogSink() noexcept = default;
    TraceLogSink(const TraceLogSink&) = delete;
    TraceLogSink& operator=(const TraceLogSink&) = delete;

    Result<void> Write(const LogEvent& event) noexcept override;
    Result<void> Flush() noexcept override;

    // 기록 버퍼가 없어 버린 Trace.LogMessage 수다.
    uint64 Lost() const noexcept;

    // 중복 표가 차서 spec 정의 대신 기록에 카테고리·템플릿을 실은 횟수다.
    uint64 SpecTableOverflows() const noexcept;

    // spec 정의를 쓰지 못해(정의 기록 상한 초과·정의 버퍼 부족) 기록에 카테고리·템플릿을 실은
    // 횟수다. 세션의 "버린 정의" 수에는 넣지 않는다.
    uint64 SpecWriteFailures() const noexcept;

private:
    // spec 표에서 찾은 결과다.
    enum class SpecState : uint8 {
        // 이 세션에 정의를 썼다
        Known,
        // 처음 본다(표에 자리가 있다)
        Missing,
        // 처음 보지만 표가 상한까지 찼다
        Full
    };

    SpecState FindSpec(uint64 serial, uint64 specId) noexcept;
    void RememberSpec(uint64 specId) noexcept;

    uint64 specSerial_ = 0;
    uint64 specIds_[TraceLogSpecTableSlots]{};
    usize specCount_ = 0;
    uint64 lost_ = 0;
    uint64 specOverflows_ = 0;
    uint64 specFailures_ = 0;
};

} // namespace rocks
