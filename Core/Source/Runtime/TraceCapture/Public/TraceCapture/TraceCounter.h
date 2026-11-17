#pragma once

#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <TraceCapture/TraceChannel.h>

namespace rocks {

// 표준 채널 "counter"다. 세션이 Start 때 등록하며 처음에는 꺼져 있다.
extern TraceChannel TraceCounterChannel;

enum class TraceCounterKind : uint8 {
    Integer = 0,
    Float = 1,
};

// 표시 단위다(분석기·viewer가 값을 읽는 방법).
enum class TraceCounterUnit : uint8 {
    Count = 0,
    Bytes = 1,
    Percent = 2,
};

enum class TraceCounterMode : uint8 {
    // 마지막으로 낸 값과 다를 때만 낸다.
    OnChange = 0,
    // 바꿀 때마다 낸다.
    Always = 1,
};

class TraceCounter;

namespace detail {
struct TraceCounterAccess;

void EmitTraceCounter(TraceCounter& counter, uint64 bits) noexcept;
} // namespace detail

// 시간에 따라 바뀌는 수치다(UE TRACE_DECLARE_*_COUNTER 대응, Docs/Design/TraceSystem.ko.md 6.2절).
//
//   TraceCounter LoadedAssets{"asset.loaded", TraceCounterKind::Integer};
//   LoadedAssets.Add(1);
//   TraceCounter FrameMilliseconds{"frame.ms", TraceCounterKind::Float, TraceCounterUnit::Count,
//                                  TraceCounterMode::Always};
//   FrameMilliseconds.SetFloat(16.6);
//
// - 현재 값은 채널·세션과 무관하게 객체가 가진다(Add는 원자적이다). 채널 "counter"가 켜져 있을 때만
//   값 기록(Trace.CounterInteger·Trace.CounterFloat, 순서 번호)을 낸다.
// - 정의 기록(id·이름·종류·단위·방식)은 세션마다 첫 값 기록 바로 전에 한 번 낸다. 그래서
//   채널이 나중에 켜져도 첫 값보다 정의가 먼저 나간다.
// - OnChange 방식은 여러 스레드가 동시에 바꾸면 같은 값을 한 번 더 낼 수 있다(빠뜨리지는 않는다).
// 인자 오류: 정수 카운터에 SetFloat·AddFloat, 실수 카운터에 Set·Add를 부르면 Fatal이다.
// 수명: 정적 저장소에 두고 이름은 정적 수명 문자열이다(constexpr 생성자, 상수 초기화).
class TraceCounter {
public:
    constexpr TraceCounter(const char* name, TraceCounterKind kind,
                           TraceCounterUnit unit = TraceCounterUnit::Count,
                           TraceCounterMode mode = TraceCounterMode::OnChange) noexcept
        : name_(name), kind_(kind), unit_(unit), mode_(mode) {
    }

    TraceCounter(const TraceCounter&) = delete;
    TraceCounter& operator=(const TraceCounter&) = delete;

    void Set(int64 value) noexcept;
    void Add(int64 delta) noexcept;
    void SetFloat(double value) noexcept;
    void AddFloat(double delta) noexcept;

    int64 Value() const noexcept;
    double FloatValue() const noexcept;

    const char* Name() const noexcept {
        return name_;
    }

    TraceCounterKind Kind() const noexcept {
        return kind_;
    }

    TraceCounterUnit Unit() const noexcept {
        return unit_;
    }

    TraceCounterMode Mode() const noexcept {
        return mode_;
    }

private:
    friend struct detail::TraceCounterAccess;

    const char* name_;
    TraceCounterKind kind_;
    TraceCounterUnit unit_;
    TraceCounterMode mode_;
    // 현재 값의 bit(정수는 int64, 실수는 double의 bit)다.
    uint64 value_ = 0;
    // (세션 serial 하위 32비트 << 32) | 카운터 id.
    uint64 cache_ = 0;
    // OnChange 방식이 마지막으로 낸 값과 그 세션 serial이다.
    uint64 lastBits_ = 0;
    uint64 lastSerial_ = 0;
};

} // namespace rocks
