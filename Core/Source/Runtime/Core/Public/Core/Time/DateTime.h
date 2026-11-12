#pragma once

#include <Core/Time/TimeSpan.h>

// UTC 시각(DateTime)이다. 저장 파일의 시각, 로그 시각, 서버와 주고받는 시각처럼 "언제인가"를
// 나타낸다.
//
// 사용 예:
//   DateTime now = DateTime::UtcNow();
//   DateTime launch = DateTime::TryFromDate(2026, 11, 12, 9, 30).Value();
//   TimeSpan remaining = launch - now;                                  // 두 시각의 차이
//   auto saved = TryFormat(allocator, "{}", now);                       //
//   "2026-11-12T00:30:05.123Z" auto parsed = TryParseIso8601("2026-11-12T09:30:00+09:00"); //
//   2026-11-12T00:30:00Z int32 year = 0, month = 0, day = 0; now.GetDate(year, month, day);
//
// 주의:
// - Rocks DateTime은 항상 UTC다. 시간대·지역 시각(UE FDateTime::Now)·서머타임은 아직 다루지 않는다.
//   사용자에게 보일 지역 시각은 후속 시간대 기능으로 바꾼다.
// - 윤초는 표현하지 않는다(OS 시각과 같이 하루는 항상 86400초다).
// - 경과 시간·timeout에는 단조 시계(platform::MonotonicNanoseconds)를 쓴다. UtcNow는 벽시계라
// NTP·사용자
//   조정으로 뒤로 갈 수 있다.
namespace rocks {

// 요일이다(UE5.6 EDayOfWeek와 같이 월요일이 0).
enum class DayOfWeek : uint8 {
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
    Sunday
};

// 0001-01-01 00:00:00 UTC부터의 100ns tick으로 나타낸 시각이다(UE5.6 FDateTime 대응).
// - 범위는 0001-01-01 00:00:00부터 9999-12-31 23:59:59.9999999까지, 달력은 역산
// 그레고리력이다(1582년
//   이전에도 그레고리력 규칙을 그대로 적용한다).
// - 연산자(+, -)는 범위를 넘으면 Fatal이다. 외부 데이터는 같은 일을 하는 Try* 함수로 다룬다.
// - tick 값은 PlatformBase의 파일 시각(FileInfo::modifiedTicks)과 같은 단위·기준이다.
class DateTime {
public:
    // 9999-12-31 23:59:59.9999999의 tick이다.
    static constexpr int64 MaxTicks = 3155378975999999999;

    // 1970-01-01 00:00:00 UTC(Unix epoch)의 tick이다.
    static constexpr int64 UnixEpochTicks = 621355968000000000;

    // 0001-01-01 00:00:00이다.
    constexpr DateTime() noexcept = default;

    static constexpr DateTime MinValue() noexcept {
        return DateTime();
    }

    static constexpr DateTime MaxValue() noexcept {
        DateTime value;
        value.ticks_ = MaxTicks;
        return value;
    }

    // tick 값으로 만든다. 0–MaxTicks 밖이면 Overflow다.
    static Result<DateTime> TryFromTicks(int64 ticks) noexcept {
        if (ticks < 0 || ticks > MaxTicks) {
            return Result<DateTime>::Failure(ErrorCode::Overflow);
        }

        DateTime value;
        value.ticks_ = ticks;
        return Result<DateTime>::Success(value);
    }

    // 날짜와 시각으로 만든다. 예: TryFromDate(2024, 2, 29, 13, 5)는 2024-02-29 13:05:00.
    // 범위: 연 1–9999, 월 1–12, 일 1–그 달의 마지막 날, 시 0–23, 분·초 0–59, 밀리초 0–999.
    // 존재하지 않는 날짜·시각(13월, 평년 2월 29일, 24시, 60초 등)은 InvalidArgument다.
    static Result<DateTime> TryFromDate(int32 year, int32 month, int32 day, int32 hour = 0,
                                        int32 minute = 0, int32 second = 0,
                                        int32 millisecond = 0) noexcept;

    // Unix 시각(1970-01-01 00:00:00 UTC부터의 초, 그 이전은 음수)으로 만든다. 범위 밖은 Overflow다.
    static Result<DateTime> TryFromUnixSeconds(int64 seconds) noexcept;

    // 현재 UTC 시각이다(platform::UtcTicks). OS 시계를 읽지 못하면 Fatal이다.
    static DateTime UtcNow() noexcept;

    // 윤년(4로 나누어지고 100으로 나누어지지 않거나, 400으로 나누어지는 해)인지다.
    static constexpr bool IsLeapYear(int32 year) noexcept {
        return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    }

    // year년 month월의 날 수(28–31)다. 연 1–9999, 월 1–12 밖은 InvalidArgument다.
    static Result<int32> TryDaysInMonth(int32 year, int32 month) noexcept;

    // 0001-01-01 00:00:00부터의 tick 수다.
    constexpr int64 Ticks() const noexcept {
        return ticks_;
    }

    // 연·월·일을 한 번에 구한다(Year·Month·Day를 따로 부르면 같은 날짜 계산을 세 번 한다).
    void GetDate(int32& year, int32& month, int32& day) const noexcept;

    int32 Year() const noexcept;
    int32 Month() const noexcept;
    int32 Day() const noexcept;

    // 시(0–23)다.
    constexpr int32 Hour() const noexcept {
        return static_cast<int32>(ticks_ / TimeSpan::TicksPerHour % 24);
    }

    // 분(0–59)이다.
    constexpr int32 Minute() const noexcept {
        return static_cast<int32>(ticks_ / TimeSpan::TicksPerMinute % 60);
    }

    // 초(0–59)다.
    constexpr int32 Second() const noexcept {
        return static_cast<int32>(ticks_ / TimeSpan::TicksPerSecond % 60);
    }

    // 밀리초(0–999)다.
    constexpr int32 Millisecond() const noexcept {
        return static_cast<int32>(ticks_ / TimeSpan::TicksPerMillisecond % 1000);
    }

    // 초 아래 tick(0–9999999)이다.
    constexpr int32 FractionTicks() const noexcept {
        return static_cast<int32>(ticks_ % TimeSpan::TicksPerSecond);
    }

    // 요일이다. 0001-01-01은 월요일이다.
    constexpr DayOfWeek GetDayOfWeek() const noexcept {
        return static_cast<DayOfWeek>(ticks_ / TimeSpan::TicksPerDay % 7);
    }

    // 그 해의 몇째 날인지(1월 1일이 1, 최대 366)다.
    int32 DayOfYear() const noexcept;

    // 같은 날 00:00:00이다.
    constexpr DateTime Date() const noexcept {
        DateTime value;
        value.ticks_ = ticks_ - ticks_ % TimeSpan::TicksPerDay;
        return value;
    }

    // 그날 00:00:00부터 지난 시간이다.
    constexpr TimeSpan TimeOfDay() const noexcept {
        return TimeSpan::FromTicks(ticks_ % TimeSpan::TicksPerDay);
    }

    // Unix 시각(초)이다. 1970년 이전은 음수이며 초 아래는 음의 무한대 쪽으로 버린다(-0.5초는 -1).
    constexpr int64 ToUnixSeconds() const noexcept {
        const int64 offset = ticks_ - UnixEpochTicks;
        const int64 seconds = offset / TimeSpan::TicksPerSecond;
        return offset % TimeSpan::TicksPerSecond < 0 ? seconds - 1 : seconds;
    }

    // span만큼 옮긴 시각이다. 범위를 넘으면 Overflow다(이 값은 바뀌지 않는다).
    Result<DateTime> TryAdd(TimeSpan span) const noexcept {
        int64 sum = 0;
        if (__builtin_add_overflow(ticks_, span.Ticks(), &sum)) {
            return Result<DateTime>::Failure(ErrorCode::Overflow);
        }

        return TryFromTicks(sum);
    }

    // 연산자는 범위를 넘으면 Fatal이다.
    friend DateTime operator+(DateTime time, TimeSpan span) noexcept {
        auto result = time.TryAdd(span);
        if (!result) {
            platform::Fatal("DateTime arithmetic is outside the supported range");
        }

        return result.Value();
    }

    friend DateTime operator-(DateTime time, TimeSpan span) noexcept {
        return time + (-span);
    }

    // 두 시각의 차이다. 범위가 약 1만 년이라 항상 TimeSpan 안이다.
    friend constexpr TimeSpan operator-(DateTime left, DateTime right) noexcept {
        return TimeSpan::FromTicks(left.ticks_ - right.ticks_);
    }

    friend constexpr bool operator==(DateTime, DateTime) noexcept = default;

    // 순서 비교다(STL <compare>를 쓰지 않으려고 직접 정의한다).
    friend constexpr bool operator<(DateTime left, DateTime right) noexcept {
        return left.ticks_ < right.ticks_;
    }

    friend constexpr bool operator<=(DateTime left, DateTime right) noexcept {
        return left.ticks_ <= right.ticks_;
    }

    friend constexpr bool operator>(DateTime left, DateTime right) noexcept {
        return left.ticks_ > right.ticks_;
    }

    friend constexpr bool operator>=(DateTime left, DateTime right) noexcept {
        return left.ticks_ >= right.ticks_;
    }

private:
    int64 ticks_ = 0;
};

// ISO 8601 확장 형식 문자열을 UTC 시각으로 읽는다(UE FDateTime::ParseIso8601 대응).
// 받는 형식:
//   YYYY-MM-DD                                   예: "2026-11-12" → 그날 00:00:00Z
//   YYYY-MM-DD(T|t|공백)hh:mm[:ss[(.|,)소수부]][시간대]
//   시간대: Z·z(UTC) 또는 (+|-)hh:mm·(+|-)hhmm      예: "2026-11-12T09:30:00+09:00" → 00:30:00Z
// - 시간대 표기가 없으면 UTC로 본다(지역 시각으로 해석하지 않는다).
// - 소수부는 1자리 이상이며 8자리부터는 버린다(7자리, 100ns까지 쓴다).
// - 연도는 4자리(0001–9999), 시 0–23, 분·초 0–59, 오프셋 시 0–23·분 0–59다. 24:00과 윤초(60초)는
// 받지 않는다. 실패: 문법 오류와 존재하지 않는 날짜·시각은 InvalidArgument, 오프셋을 적용한 결과가
// 범위 밖이면 Overflow다.
Result<DateTime> TryParseIso8601(StringView text) noexcept;

// "{}"로 ISO 8601 UTC 형식 "YYYY-MM-DDThh:mm:ss[.소수부]Z"를 쓴다.
//   2026-11-12 09:30:00 → "2026-11-12T09:30:00Z",  0.25초 → "…:00.25Z"(소수부는 0이 아닐 때만, 끝의
//   0은 뺀다)
// TryParseIso8601로 다시 읽으면 같은 값이다. spec("{:...}")은 받지 않는다(InvalidArgument).
template <>
struct Formatter<DateTime> {
    static Result<void> Format(StringBuilderBase& out, const DateTime& time,
                               StringView spec) noexcept;
};

} // namespace rocks
