#pragma once

#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Fatal.h>

// 시간 간격(TimeSpan)이다. 두 시각의 차이, 타이머 길이, 쿨다운처럼 "얼마나 긴가"를 나타낸다.
// 시각 자체는 DateTime(Core/Time/DateTime.h)이 나타낸다.
//
// 사용 예:
//   TimeSpan cooldown = TimeSpan::TryFromSeconds(30).Value();       // 30초
//   TimeSpan elapsed = DateTime::UtcNow() - start;                  // 두 시각의 차이
//   if (elapsed >= cooldown) { ... }
//   double seconds = elapsed.TotalSeconds();                        // 실수 초
//   auto text = TryFormat(allocator, "{}", elapsed);                // "00:01:05.2500000"
//
// 주의:
// - 경과 시간 측정·deadline에는 단조 시계(platform::MonotonicNanoseconds)를 쓴다. DateTime 차이는
// 벽시계
//   조정(NTP·사용자 변경)에 영향을 받는다.
// - 프레임 시간처럼 실수 초가 필요한 계산은 TotalSeconds()로 바꿔 쓴다. tick은 정수이므로 누적해도
// 오차가
//   생기지 않는다.
namespace rocks {

// 100ns tick 단위의 부호 있는 시간 간격이다(UE5.6 FTimespan 대응). 범위는 int64 tick 전체(약
// ±29,227년)다.
// - 연산자(+, -, 단항 -, +=, -=, Duration)는 overflow를 Fatal로 끝낸다. 외부 데이터에서 온 값은
// 같은 일을
//   하는 Try* 함수로 다루고 Overflow를 처리한다.
// - 구성 요소(Days·Hours·Minutes·Seconds·Milliseconds·FractionTicks)는 0 쪽으로 자른 값이다. 음수
// 간격의
//   구성 요소는 모두 0 이하다(-1.5초는 Seconds() -1, Milliseconds() -500).
class TimeSpan {
public:
    static constexpr int64 TicksPerMicrosecond = 10;
    static constexpr int64 TicksPerMillisecond = 10000;
    static constexpr int64 TicksPerSecond = 10000000;
    static constexpr int64 TicksPerMinute = 600000000;
    static constexpr int64 TicksPerHour = 36000000000;
    static constexpr int64 TicksPerDay = 864000000000;

    // 0 간격이다.
    constexpr TimeSpan() noexcept = default;

    // tick(100ns) 수로 만든다. 모든 int64 값이 유효하다.
    static constexpr TimeSpan FromTicks(int64 ticks) noexcept {
        TimeSpan span;
        span.ticks_ = ticks;
        return span;
    }

    static constexpr TimeSpan Zero() noexcept {
        return TimeSpan();
    }

    static constexpr TimeSpan MinValue() noexcept {
        return FromTicks(-0x7fffffffffffffffll - 1);
    }

    static constexpr TimeSpan MaxValue() noexcept {
        return FromTicks(0x7fffffffffffffffll);
    }

    // 정수 단위 값으로 만든다. 예: TryFromMinutes(90)은 1시간 30분. 범위를 넘으면 Overflow다.
    static Result<TimeSpan> TryFromDays(int64 days) noexcept {
        return TryScale(days, TicksPerDay);
    }

    static Result<TimeSpan> TryFromHours(int64 hours) noexcept {
        return TryScale(hours, TicksPerHour);
    }

    static Result<TimeSpan> TryFromMinutes(int64 minutes) noexcept {
        return TryScale(minutes, TicksPerMinute);
    }

    static Result<TimeSpan> TryFromSeconds(int64 seconds) noexcept {
        return TryScale(seconds, TicksPerSecond);
    }

    static Result<TimeSpan> TryFromMilliseconds(int64 milliseconds) noexcept {
        return TryScale(milliseconds, TicksPerMillisecond);
    }

    // 실수 초(예: 프레임 delta 0.016)에서 가장 가까운 tick으로 만든다. 정확히 반인 경우는 0에서 먼
    // 쪽으로 반올림한다. NaN은 InvalidArgument, 무한·범위 밖은 Overflow다.
    static Result<TimeSpan> TryFromTotalSeconds(double seconds) noexcept;

    // 전체 tick 수다.
    constexpr int64 Ticks() const noexcept {
        return ticks_;
    }

    // 전체 일 수(0 쪽으로 자름)다.
    constexpr int64 Days() const noexcept {
        return ticks_ / TicksPerDay;
    }

    // 일을 뺀 나머지의 시(-23–23)다.
    constexpr int32 Hours() const noexcept {
        return static_cast<int32>(ticks_ / TicksPerHour % 24);
    }

    // 시를 뺀 나머지의 분(-59–59)이다.
    constexpr int32 Minutes() const noexcept {
        return static_cast<int32>(ticks_ / TicksPerMinute % 60);
    }

    // 분을 뺀 나머지의 초(-59–59)다.
    constexpr int32 Seconds() const noexcept {
        return static_cast<int32>(ticks_ / TicksPerSecond % 60);
    }

    // 초를 뺀 나머지의 밀리초(-999–999)다.
    constexpr int32 Milliseconds() const noexcept {
        return static_cast<int32>(ticks_ / TicksPerMillisecond % 1000);
    }

    // 초를 뺀 나머지의 tick(-9999999–9999999)이다.
    constexpr int32 FractionTicks() const noexcept {
        return static_cast<int32>(ticks_ % TicksPerSecond);
    }

    // 전체 길이를 실수 단위로 나타낸다. 매우 긴 간격은 double 정밀도만큼 반올림된다.
    double TotalDays() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(TicksPerDay);
    }

    double TotalHours() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(TicksPerHour);
    }

    double TotalMinutes() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(TicksPerMinute);
    }

    double TotalSeconds() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(TicksPerSecond);
    }

    double TotalMilliseconds() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(TicksPerMillisecond);
    }

    // 더하거나 뺀다. 결과가 범위를 넘으면 Overflow다(값은 바뀌지 않는다).
    Result<TimeSpan> TryAdd(TimeSpan other) const noexcept {
        int64 sum = 0;
        if (__builtin_add_overflow(ticks_, other.ticks_, &sum)) {
            return Result<TimeSpan>::Failure(ErrorCode::Overflow);
        }

        return Result<TimeSpan>::Success(FromTicks(sum));
    }

    Result<TimeSpan> TrySubtract(TimeSpan other) const noexcept {
        int64 difference = 0;
        if (__builtin_sub_overflow(ticks_, other.ticks_, &difference)) {
            return Result<TimeSpan>::Failure(ErrorCode::Overflow);
        }

        return Result<TimeSpan>::Success(FromTicks(difference));
    }

    // 연산자는 overflow를 Fatal로 끝낸다(범위가 ±29,227년이라 정상 코드에서는 일어나지 않는다).
    friend TimeSpan operator+(TimeSpan left, TimeSpan right) noexcept {
        return Checked(left.TryAdd(right));
    }

    friend TimeSpan operator-(TimeSpan left, TimeSpan right) noexcept {
        return Checked(left.TrySubtract(right));
    }

    TimeSpan operator-() const noexcept {
        return Checked(Zero().TrySubtract(*this));
    }

    TimeSpan& operator+=(TimeSpan other) noexcept {
        return *this = *this + other;
    }

    TimeSpan& operator-=(TimeSpan other) noexcept {
        return *this = *this - other;
    }

    // 절댓값이다. MinValue는 양수로 표현할 수 없어 Fatal이다.
    TimeSpan Duration() const noexcept {
        return ticks_ < 0 ? -*this : *this;
    }

    friend constexpr bool operator==(TimeSpan, TimeSpan) noexcept = default;

    // 순서 비교다(STL <compare>를 쓰지 않으려고 직접 정의한다).
    friend constexpr bool operator<(TimeSpan left, TimeSpan right) noexcept {
        return left.ticks_ < right.ticks_;
    }

    friend constexpr bool operator<=(TimeSpan left, TimeSpan right) noexcept {
        return left.ticks_ <= right.ticks_;
    }

    friend constexpr bool operator>(TimeSpan left, TimeSpan right) noexcept {
        return left.ticks_ > right.ticks_;
    }

    friend constexpr bool operator>=(TimeSpan left, TimeSpan right) noexcept {
        return left.ticks_ >= right.ticks_;
    }

private:
    static Result<TimeSpan> TryScale(int64 value, int64 unit) noexcept {
        int64 ticks = 0;
        if (__builtin_mul_overflow(value, unit, &ticks)) {
            return Result<TimeSpan>::Failure(ErrorCode::Overflow);
        }

        return Result<TimeSpan>::Success(FromTicks(ticks));
    }

    static TimeSpan Checked(const Result<TimeSpan>& result) noexcept {
        if (!result) {
            platform::Fatal("TimeSpan arithmetic overflow");
        }

        return result.Value();
    }

    int64 ticks_ = 0;
};

// 문자열을 TimeSpan으로 읽는다. 형식은 서식 출력과 같다: "[-][일.]시:분:초[.소수부]".
//   "01:30:00" → 1시간 30분,  "-2.03:00:00.5" → -(2일 3시간 0.5초),  "00:00:00.25" → 0.25초
// 시는 0–23, 분·초는 0–59, 소수부는 1–7자리(10^-7초 단위, 짧으면 뒤를 0으로 본다)다. 공백은 받지
// 않는다. 실패: 문법·범위 오류는 InvalidArgument, 전체 값이 TimeSpan 범위를 넘으면 Overflow다.
Result<TimeSpan> TryParseTimeSpan(StringView text) noexcept;

// "{}"로 서식화한다: "[-][일.]시:분:초[.소수부 7자리]". 일은 0이 아닐 때만, 소수부는 0이 아닐 때만
// 쓴다.
//   1시간 30분 → "01:30:00",  -2일 3시간 0.5초 → "-2.03:00:00.5000000"
// spec("{:...}")은 받지 않는다(InvalidArgument).
template <>
struct Formatter<TimeSpan> {
    static Result<void> Format(StringBuilderBase& out, const TimeSpan& span,
                               StringView spec) noexcept;
};

} // namespace rocks
