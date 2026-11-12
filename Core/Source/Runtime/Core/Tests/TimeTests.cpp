#include <Core/Math/RandomStream.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Time/DateTime.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

using namespace rocks;

namespace {

bool Same(StringView actual, const char* expected) noexcept {
    const usize bytes = strlen(expected);
    return actual.ByteCount() == bytes && memcmp(actual.Data(), expected, bytes) == 0;
}

// "{}" 서식 결과가 기대값과 같은지 본다. 다르면 실제 값을 출력한다.
template <typename T>
bool FormatsAs(const T& value, const char* expected) noexcept {
    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    if (!text.TryAppendFormat("{}", value) || !Same(text.View(), expected)) {
        printf("  time format: expected \"%s\" actual \"%.*s\"\n", expected,
               static_cast<int>(text.ByteCount()), text.CString());
        return false;
    }

    return true;
}

DateTime Date(int32 year, int32 month, int32 day, int32 hour = 0, int32 minute = 0,
              int32 second = 0) noexcept {
    return DateTime::TryFromDate(year, month, day, hour, minute, second).Value();
}

} // namespace

TEST(TimeSpan_FactoriesComponentsAndOverflow) {
    const TimeSpan span = TimeSpan::TryFromMinutes(90).Value();
    CHECK(span.Ticks() == 90 * TimeSpan::TicksPerMinute && span.Hours() == 1 &&
          span.Minutes() == 30);
    CHECK(TimeSpan::TryFromDays(2).Value().TotalHours() == 48.0);
    CHECK(TimeSpan::TryFromMilliseconds(1500).Value().TotalSeconds() == 1.5);
    CHECK(TimeSpan::TryFromDays(0x7fffffffffffffffll).Error() == ErrorCode::Overflow);
    CHECK(TimeSpan::TryFromSeconds(-0x7fffffffffffffffll).Error() == ErrorCode::Overflow);

    // 음수 간격의 구성 요소는 모두 0 이하다.
    const TimeSpan negative =
        TimeSpan::FromTicks(-(TimeSpan::TicksPerDay + 15 * TimeSpan::TicksPerMillisecond * 100));
    CHECK(negative.Days() == -1 && negative.Seconds() == -1 && negative.Milliseconds() == -500);

    // 실수 초는 가장 가까운 tick으로, 반 tick은 0에서 먼 쪽으로 반올림한다.
    CHECK(TimeSpan::TryFromTotalSeconds(1.5).Value().Ticks() == 15000000);
    CHECK(TimeSpan::TryFromTotalSeconds(0.00000005).Value().Ticks() == 1);
    CHECK(TimeSpan::TryFromTotalSeconds(-0.00000005).Value().Ticks() == -1);
    const double nan = __builtin_nan("");
    CHECK(TimeSpan::TryFromTotalSeconds(nan).Error() == ErrorCode::InvalidArgument);
    CHECK(TimeSpan::TryFromTotalSeconds(1e12).Error() == ErrorCode::Overflow);
    CHECK(TimeSpan::TryFromTotalSeconds(-1e300).Error() == ErrorCode::Overflow);

    // 산술과 비교.
    CHECK(TimeSpan::MaxValue().TryAdd(TimeSpan::FromTicks(1)).Error() == ErrorCode::Overflow);
    CHECK(TimeSpan::MinValue().TrySubtract(TimeSpan::FromTicks(1)).Error() == ErrorCode::Overflow);
    TimeSpan total = TimeSpan::FromTicks(10);
    total += TimeSpan::FromTicks(5);
    total -= TimeSpan::FromTicks(20);
    CHECK(total.Ticks() == -5 && total.Duration().Ticks() == 5 && (-total).Ticks() == 5);
    CHECK(TimeSpan::FromTicks(1) < TimeSpan::FromTicks(2) &&
          TimeSpan::FromTicks(2) >= TimeSpan::FromTicks(2));
}

TEST(TimeSpan_FormatAndParseRoundTrip) {
    CHECK(FormatsAs(TimeSpan::Zero(), "00:00:00"));
    CHECK(FormatsAs(TimeSpan::TryFromMinutes(90).Value(), "01:30:00"));
    CHECK(FormatsAs(-(TimeSpan::TryFromHours(51).Value() + TimeSpan::FromTicks(5000000)),
                    "-2.03:00:00.5000000"));
    CHECK(FormatsAs(TimeSpan::MaxValue(), "10675199.02:48:05.4775807"));
    CHECK(FormatsAs(TimeSpan::MinValue(), "-10675199.02:48:05.4775808"));

    CHECK(TryParseTimeSpan("01:30:00").Value() == TimeSpan::TryFromMinutes(90).Value());
    CHECK(TryParseTimeSpan("-2.03:00:00.5").Value().Ticks() ==
          -(51 * TimeSpan::TicksPerHour + 5000000));
    CHECK(TryParseTimeSpan("00:00:00.0000001").Value().Ticks() == 1);
    CHECK(TryParseTimeSpan("-10675199.02:48:05.4775808").Value() == TimeSpan::MinValue());
    CHECK(TryParseTimeSpan("10675199.02:48:05.4775807").Value() == TimeSpan::MaxValue());
    CHECK(TryParseTimeSpan("10675199.02:48:05.4775808").Error() == ErrorCode::Overflow);
    CHECK(TryParseTimeSpan("10675201.00:00:00").Error() == ErrorCode::Overflow);
    const char* invalid[] = {"",          "-",         "1:00:00",         "24:00:00",
                             "00:60:00",  "00:00:60",  "00:00:00.",       "00:00:00.12345678",
                             "--1",       " 00:00:00", "00:00:00 ",       "1.",
                             "1.2:00:00", "00-00-00",  "123456789:00:00", "00:00"};
    for (const char* text : invalid) {
        CHECK(TryParseTimeSpan(text).Error() == ErrorCode::InvalidArgument);
    }

    // 무작위 간격은 서식화 후 다시 읽어도 같다.
    RandomStream random(test::CurrentSeed() ^ 0x7153ull);
    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const uint64 bits = (static_cast<uint64>(random.NextUInt32()) << 32) | random.NextUInt32();
        const TimeSpan span = TimeSpan::FromTicks(static_cast<int64>(bits));
        text.Clear();
        REQUIRE(text.TryAppendFormat("{}", span));
        auto parsed = TryParseTimeSpan(text.View());
        REQUIRE(parsed);
        CHECK(parsed.Value() == span);
    }

    CHECK(text.TryAppendFormat("{:x}", TimeSpan::Zero()).Error() == ErrorCode::InvalidArgument);
}

TEST(DateTime_KnownDatesAndValidation) {
    CHECK(Date(1, 1, 1).Ticks() == 0);
    CHECK(Date(1970, 1, 1).Ticks() == DateTime::UnixEpochTicks);
    CHECK(Date(2000, 1, 1).Ticks() == 630822816000000000ll);
    CHECK(Date(2026, 11, 12, 0, 30).Ticks() == 639300402000000000ll);
    CHECK(DateTime::TryFromDate(9999, 12, 31, 23, 59, 59, 999).Value().Ticks() ==
          DateTime::MaxTicks - 9999);

    // 요일·연중 날짜(기대값은 Python datetime으로 확인했다).
    CHECK(Date(1, 1, 1).GetDayOfWeek() == DayOfWeek::Monday);
    CHECK(Date(2026, 11, 12).GetDayOfWeek() == DayOfWeek::Thursday);
    CHECK(Date(2000, 2, 29).GetDayOfWeek() == DayOfWeek::Tuesday);
    CHECK(Date(2024, 12, 31).DayOfYear() == 366 && Date(2023, 12, 31).DayOfYear() == 365);
    CHECK(Date(2024, 3, 1).DayOfYear() == 61);

    // 윤년과 달의 날 수.
    CHECK(DateTime::IsLeapYear(2000) && DateTime::IsLeapYear(2024) && !DateTime::IsLeapYear(1900) &&
          !DateTime::IsLeapYear(2023));
    CHECK(DateTime::TryDaysInMonth(2024, 2).Value() == 29 &&
          DateTime::TryDaysInMonth(1900, 2).Value() == 28);
    CHECK(DateTime::TryDaysInMonth(2024, 4).Value() == 30 &&
          DateTime::TryDaysInMonth(2024, 12).Value() == 31);
    CHECK(DateTime::TryDaysInMonth(2024, 13).Error() == ErrorCode::InvalidArgument);

    // 존재하지 않는 날짜·시각.
    const int32 invalid[][7] = {
        {2023, 2, 29, 0, 0, 0, 0}, {1900, 2, 29, 0, 0, 0, 0}, {2000, 2, 30, 0, 0, 0, 0},
        {0, 1, 1, 0, 0, 0, 0},     {10000, 1, 1, 0, 0, 0, 0}, {2024, 13, 1, 0, 0, 0, 0},
        {2024, 4, 31, 0, 0, 0, 0}, {2024, 1, 0, 0, 0, 0, 0},  {2024, 1, 1, 24, 0, 0, 0},
        {2024, 1, 1, 0, 60, 0, 0}, {2024, 1, 1, 0, 0, 60, 0}, {2024, 1, 1, 0, 0, 0, 1000},
        {2024, 1, 1, -1, 0, 0, 0}};
    for (const auto& value : invalid) {
        CHECK(DateTime::TryFromDate(value[0], value[1], value[2], value[3], value[4], value[5],
                                    value[6])
                  .Error() == ErrorCode::InvalidArgument);
    }

    CHECK(DateTime::TryFromTicks(-1).Error() == ErrorCode::Overflow);
    CHECK(DateTime::TryFromTicks(DateTime::MaxTicks + 1).Error() == ErrorCode::Overflow);
}

TEST(DateTime_EveryDayMatchesIndependentCalendar) {
    // 0001-01-01부터 9999-12-31까지 하루씩 진행하며 직접 센 달력과 GetDate·TryFromDate·요일을
    // 대조한다.
    int32 year = 1;
    int32 month = 1;
    int32 day = 1;
    int64 count = 0;
    bool same = true;
    while (same) {
        const DateTime value = DateTime::TryFromTicks(count * TimeSpan::TicksPerDay).Value();
        int32 y = 0;
        int32 m = 0;
        int32 d = 0;
        value.GetDate(y, m, d);
        same = y == year && m == month && d == day &&
               static_cast<int64>(value.GetDayOfWeek()) == count % 7;
        // 생성 방향은 매달 1일·마지막 날과 무작위 날 대신 매 7일마다 확인한다(시간 절약).
        if (count % 7 == 0) {
            same = same && DateTime::TryFromDate(year, month, day).Value() == value;
        }

        const int32 days = DateTime::TryDaysInMonth(year, month).Value();
        if (++day > days) {
            day = 1;
            if (++month > 12) {
                month = 1;
                if (++year > 9999) {
                    break;
                }
            }
        }

        ++count;
    }

    CHECK(same);
    CHECK(count == DateTime::MaxTicks / TimeSpan::TicksPerDay);
}

TEST(DateTime_UnixTimeArithmeticAndUtcNow) {
    CHECK(DateTime::TryFromUnixSeconds(0).Value().Ticks() == DateTime::UnixEpochTicks);
    CHECK(DateTime::TryFromUnixSeconds(-1).Value().Ticks() == 621355967990000000ll);
    CHECK(DateTime::TryFromUnixSeconds(-62135596801ll).Error() == ErrorCode::Overflow);
    CHECK(DateTime::TryFromUnixSeconds(253402300800ll).Error() == ErrorCode::Overflow);
    CHECK(DateTime::TryFromTicks(DateTime::UnixEpochTicks - 1).Value().ToUnixSeconds() == -1);
    CHECK(Date(2026, 11, 12, 0, 30).ToUnixSeconds() == 1794443400ll);

    const DateTime start = Date(2024, 2, 28, 23);
    const DateTime later = start + TimeSpan::TryFromHours(2).Value();
    int32 y = 0;
    int32 m = 0;
    int32 d = 0;
    later.GetDate(y, m, d);
    CHECK(y == 2024 && m == 2 && d == 29 && later.Hour() == 1);
    CHECK((later - start) == TimeSpan::TryFromHours(2).Value() &&
          later.Date() == Date(2024, 2, 29));
    CHECK(later.TimeOfDay() == TimeSpan::TryFromHours(1).Value());
    CHECK(DateTime::MaxValue().TryAdd(TimeSpan::FromTicks(1)).Error() == ErrorCode::Overflow);
    CHECK(DateTime::MinValue().TryAdd(TimeSpan::FromTicks(-1)).Error() == ErrorCode::Overflow);
    CHECK(start < later && later > start && start <= start);

    // 현재 시각은 C runtime의 time()과 몇 초 안에서 일치해야 한다(플랫폼 hook 독립 확인).
    const int64 before = static_cast<int64>(time(nullptr));
    const DateTime now = DateTime::UtcNow();
    const int64 after = static_cast<int64>(time(nullptr));
    CHECK(now.ToUnixSeconds() >= before - 2 && now.ToUnixSeconds() <= after + 2);
    CHECK(now.Year() >= 2024);
}

TEST(DateTime_Iso8601FormatAndParse) {
    CHECK(FormatsAs(Date(2026, 11, 12, 0, 30), "2026-11-12T00:30:00Z"));
    CHECK(FormatsAs(DateTime::MinValue(), "0001-01-01T00:00:00Z"));
    CHECK(FormatsAs(DateTime::MaxValue(), "9999-12-31T23:59:59.9999999Z"));
    CHECK(FormatsAs(Date(2024, 2, 29) + TimeSpan::FromTicks(2500000), "2024-02-29T00:00:00.25Z"));

    const DateTime expected = Date(2026, 11, 12, 0, 30);
    const char* same[] = {"2026-11-12T00:30:00Z",      "2026-11-12T00:30:00z",
                          "2026-11-12t00:30:00Z",      "2026-11-12 00:30:00Z",
                          "2026-11-12T00:30:00",       "2026-11-12T00:30",
                          "2026-11-12T09:30:00+09:00", "2026-11-12T09:30:00+0900",
                          "2026-11-11T19:00:00-05:30", "2026-11-12T00:30:00.0Z"};
    for (const char* text : same) {
        auto parsed = TryParseIso8601(text);
        CHECK(parsed && parsed.Value() == expected);
    }

    CHECK(TryParseIso8601("2026-11-12").Value() == Date(2026, 11, 12));
    CHECK(TryParseIso8601("2026-11-12T00:30:00,5Z").Value().FractionTicks() == 5000000);
    CHECK(TryParseIso8601("2026-11-12T00:30:00.123456789Z").Value().FractionTicks() == 1234567);

    const char* invalid[] = {"",
                             "2026-11-12T",
                             "2026-11-12T24:00:00Z",
                             "2026-02-30",
                             "26-11-12",
                             "2026-11-12T00:30:00Zjunk",
                             "2026-11-12Z",
                             "2026-11-12T00:30:60Z",
                             "2026-11-12T00:30:00+24:00",
                             "2026-11-12T00:30:00+09:60",
                             "2026-11-12T00:30:00+9:00",
                             "2026-11-12T00:30:00.Z",
                             "2026/11/12",
                             "0000-01-01",
                             "2026-11-12X00:30:00",
                             " 2026-11-12"};
    for (const char* text : invalid) {
        CHECK(TryParseIso8601(text).Error() == ErrorCode::InvalidArgument);
    }

    CHECK(TryParseIso8601("0001-01-01T00:00:00+01:00").Error() == ErrorCode::Overflow);
    CHECK(TryParseIso8601("9999-12-31T23:59:59-01:00").Error() == ErrorCode::Overflow);

    // 무작위 시각은 서식화 후 다시 읽어도 같다.
    RandomStream random(test::CurrentSeed() ^ 0xda7eull);
    SystemAllocator allocator;
    StringBuilder<64> text(allocator);
    for (int iteration = 0; iteration < 2000; ++iteration) {
        const uint64 bits = (static_cast<uint64>(random.NextUInt32()) << 32) | random.NextUInt32();
        const DateTime value =
            DateTime::TryFromTicks(
                static_cast<int64>(bits % static_cast<uint64>(DateTime::MaxTicks + 1)))
                .Value();
        text.Clear();
        REQUIRE(text.TryAppendFormat("{}", value));
        auto parsed = TryParseIso8601(text.View());
        REQUIRE(parsed);
        CHECK(parsed.Value() == value);
    }
}
