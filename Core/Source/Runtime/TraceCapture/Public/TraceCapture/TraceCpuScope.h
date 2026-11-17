#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <TraceCapture/TraceChannel.h>

namespace rocks {

// 표준 채널 "cpu"다. 세션이 Start 때 등록하며 처음에는 꺼져 있다(켜기: SetChannelEnabled).
extern TraceChannel TraceCpuChannel;

namespace detail {
struct TraceScopeNameAccess;
} // namespace detail

// CPU 구간의 정적 이름이다(호출 지점마다 하나). 세션 이름 표의 id를 객체에 캐시해, 같은 세션에서는
// 두 번째 구간부터 이름 표를 보지 않는다. 여러 스레드가 처음 동시에 들어와도 이름 표가 같은
// 문자열에 같은 id를 돌려주므로 같은 id로 수렴한다(결정 T4).
//
// 객체는 정적 저장소(함수 안 static 포함)에 둔다. 생성자가 constexpr이라 상수 초기화된다. 보통
// ROCKS_TRACE_CPU_SCOPE가 만든다.
// 수명: name 문자열은 객체와 같이 정적 수명이어야 한다. 이름 표는 등록 때 복사한다.
class TraceScopeName {
public:
    constexpr explicit TraceScopeName(const char* name) noexcept : name_(name) {
    }

    TraceScopeName(const TraceScopeName&) = delete;
    TraceScopeName& operator=(const TraceScopeName&) = delete;

    const char* Name() const noexcept {
        return name_;
    }

private:
    friend struct detail::TraceScopeNameAccess;

    const char* name_;
    // (세션 serial 하위 32비트 << 32) | 이름 id. 다른 세션의 값이면 다시 찾는다.
    uint64 cache_ = 0;
};

namespace detail {

// CpuScope의 느린 경로다. 기록하면 세션 serial을, 기록하지 않았으면 0을 돌려준다.
uint64 BeginCpuScope(TraceScopeName& name) noexcept;
uint64 BeginCpuScope(StringView name) noexcept;
void EndCpuScope(uint64 serial) noexcept;

} // namespace detail

// 이름 있는 스레드 안 시간 구간이다(UE TRACE_CPUPROFILER_EVENT_SCOPE 대응,
// Docs/Design/TraceSystem.ko.md 6.1절). 생성 때 시작을, 소멸 때 끝을 이 스레드의 구간 배치에
// 쌓는다.
//
//   void Simulate() {
//       ROCKS_TRACE_CPU_SCOPE("Game.Simulate");          // 정적 이름(권장)
//       ...
//       CpuScope entity(StringView(entityName));          // 동적 이름(세션 이름 표에서 찾는다)
//   }
//
// - 채널 "cpu"가 꺼져 있으면 relaxed 읽기와 분기 하나로 끝난다. 채널 상태는 생성 때 한 번만 읽고,
//   끝은 시작을 기록한 경우에만 낸다(중간에 채널을 꺼도 쌍이 깨지지 않는다).
// - 배치(256 B)는 스레드 전용 메모리라 시작·끝마다 잠금이나 세션 접근이 없다. 배치가 차거나 깊이가
//   0으로 돌아오면 이벤트 하나(Trace.CpuScopeBatch)로 낸다. 그때 블록을 받지 못하면 그 배치의
//   시작·끝은 손실로 센다(분석기는 끝을 잃은 구간을 "끝 없음"으로 표시한다).
// - 세션 Stop 때 아직 내지 않은 배치(열려 있는 바깥 구간 등)는 버려진다. 스레드 종료 때는 낸다.
// 스레드: 만든 스레드에서 소멸해야 한다(스택 객체로 쓴다).
class CpuScope {
public:
    explicit CpuScope(TraceScopeName& name) noexcept {
        if (TraceCpuChannel.IsEnabled()) {
            serial_ = detail::BeginCpuScope(name);
        }
    }

    // 동적 이름이다. 정적 이름보다 비싸다(세션 접근과 스레드 이름 캐시 조회). 이름은 호출 동안만
    // 빌리고, 처음 보는 이름은 세션 이름 표에 복사된다. 1024 byte를 넘으면 앞부분만 쓰고 이름
    // 정의에 잘림을 표시한다.
    explicit CpuScope(StringView name) noexcept {
        if (TraceCpuChannel.IsEnabled()) {
            serial_ = detail::BeginCpuScope(name);
        }
    }

    ~CpuScope() noexcept {
        if (serial_ != 0) {
            detail::EndCpuScope(serial_);
        }
    }

    CpuScope(const CpuScope&) = delete;
    CpuScope& operator=(const CpuScope&) = delete;

private:
    uint64 serial_ = 0;
};

} // namespace rocks

#define ROCKS_TRACE_CONCAT_INNER(left, right) left##right
#define ROCKS_TRACE_CONCAT(left, right) ROCKS_TRACE_CONCAT_INNER(left, right)

// 정적 이름 CPU 구간을 현재 블록 끝까지 연다. name은 문자열 리터럴이다.
#define ROCKS_TRACE_CPU_SCOPE(name)                                                                \
    static ::rocks::TraceScopeName ROCKS_TRACE_CONCAT(rocksTraceScopeName, __LINE__){name};        \
    ::rocks::CpuScope ROCKS_TRACE_CONCAT(rocksTraceScope, __LINE__) {                              \
        ROCKS_TRACE_CONCAT(rocksTraceScopeName, __LINE__)                                          \
    }
