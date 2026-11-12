#include <Core/Time/DateTime.h>
#include <Platform/Clock.h>

#include <math.h>

namespace rocks {
namespace {

// 0001-01-01부터 1970-01-01까지의 날 수다.
constexpr int64 DaysToUnixEpoch = 719162;

// TimeSpan이 표현하는 최대 일 수(약 29,227년 → 10,675,199일)다.
constexpr uint64 MaxTimeSpanDays = 10675199;

// 역산 그레고리력 날짜의 0001-01-01부터의 날 수다(Howard Hinnant의 공개 도메인 civil 날짜
// 알고리즘을 0001-01-01 기준으로 옮겼다). month·day는 호출자가 검증한다.
int64 DaysFromCivil(int64 year, int32 month, int32 day) noexcept {
    year -= month <= 2 ? 1 : 0;
    const int64 era = (year >= 0 ? year : year - 399) / 400;
    const auto yearOfEra = static_cast<uint32>(year - era * 400);
    const auto monthIndex = static_cast<uint32>(month > 2 ? month - 3 : month + 9);
    const uint32 dayOfYear = (153 * monthIndex + 2) / 5 + static_cast<uint32>(day) - 1;
    const uint32 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    // 알고리즘의 기준(1970-01-01)을 0001-01-01로 옮긴다.
    return era * 146097 + static_cast<int64>(dayOfEra) - 719468 + DaysToUnixEpoch;
}

// DaysFromCivil의 역이다. days는 0001-01-01부터의 날 수(0 이상)다.
void CivilFromDays(int64 days, int32& year, int32& month, int32& day) noexcept {
    const int64 shifted = days - DaysToUnixEpoch + 719468;
    const int64 era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const auto dayOfEra = static_cast<uint32>(shifted - era * 146097);
    const uint32 yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const uint32 dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const uint32 monthIndex = (5 * dayOfYear + 2) / 153;
    day = static_cast<int32>(dayOfYear - (153 * monthIndex + 2) / 5 + 1);
    month = static_cast<int32>(monthIndex < 10 ? monthIndex + 3 : monthIndex - 9);
    year = static_cast<int32>(static_cast<int64>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0));
}

int32 DaysInMonth(int32 year, int32 month) noexcept {
    constexpr int32 days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && DateTime::IsLeapYear(year) ? 29 : days[month - 1];
}

Result<void> Invalid() noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<DateTime> InvalidDate() noexcept {
    return Result<DateTime>::Failure(ErrorCode::InvalidArgument);
}

// ---- 문자열 해석 도우미 ----

// text[index]부터 숫자 정확히 count자리를 읽는다.
bool ReadFixedDigits(StringView text, usize& index, usize count, int32& value) noexcept {
    if (index + count > text.ByteCount()) {
        return false;
    }

    value = 0;
    for (usize offset = 0; offset < count; ++offset) {
        const char character = text.Data()[index + offset];
        if (character < '0' || character > '9') {
            return false;
        }

        value = value * 10 + (character - '0');
    }

    index += count;
    return true;
}

bool ReadCharacter(StringView text, usize& index, char expected) noexcept {
    if (index < text.ByteCount() && text.Data()[index] == expected) {
        ++index;
        return true;
    }

    return false;
}

bool IsDigitAt(StringView text, usize index) noexcept {
    return index < text.ByteCount() && text.Data()[index] >= '0' && text.Data()[index] <= '9';
}

// 소수부 숫자열(1자리 이상)을 tick(7자리)으로 읽는다. 8자리부터는 truncate가 true면 버리고, 아니면
// 오류다.
bool ReadFraction(StringView text, usize& index, bool truncate, int64& ticks) noexcept {
    usize digits = 0;
    ticks = 0;
    while (IsDigitAt(text, index)) {
        if (digits < 7) {
            ticks = ticks * 10 + (text.Data()[index] - '0');
        } else if (!truncate) {
            return false;
        }

        ++digits;
        ++index;
    }

    for (usize padding = digits; padding < 7; ++padding) {
        ticks *= 10;
    }

    return digits != 0;
}

} // namespace

// ---- TimeSpan ----

Result<TimeSpan> TimeSpan::TryFromTotalSeconds(double seconds) noexcept {
    if (isnan(seconds)) {
        return Result<TimeSpan>::Failure(ErrorCode::InvalidArgument);
    }

    // 0.5 tick은 0에서 먼 쪽으로 반올림한다.
    const double ticks = seconds * static_cast<double>(TicksPerSecond);
    const double rounded = ticks >= 0.0 ? floor(ticks + 0.5) : ceil(ticks - 0.5);
    // int64 범위는 [-2^63, 2^63)이다. 두 경계는 double로 정확히 표현된다.
    if (!(rounded >= -9223372036854775808.0 && rounded < 9223372036854775808.0)) {
        return Result<TimeSpan>::Failure(ErrorCode::Overflow);
    }

    return Result<TimeSpan>::Success(FromTicks(static_cast<int64>(rounded)));
}

Result<TimeSpan> TryParseTimeSpan(StringView text) noexcept {
    usize index = 0;
    const bool negative = ReadCharacter(text, index, '-');

    // 처음 숫자열 뒤에 '.'이 오면 그 숫자열은 일이고, 아니면 시(2자리)부터 다시 읽는다.
    const usize start = index;
    while (IsDigitAt(text, index)) {
        ++index;
    }

    if (index == start) {
        return Result<TimeSpan>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 days = 0;
    if (index < text.ByteCount() && text.Data()[index] == '.') {
        for (usize digit = start; digit < index; ++digit) {
            days = days * 10 + static_cast<uint64>(text.Data()[digit] - '0');
            if (days > MaxTimeSpanDays + 1) {
                return Result<TimeSpan>::Failure(ErrorCode::Overflow);
            }
        }

        ++index; // '.'
    } else {
        index = start;
    }

    int32 hours = 0;
    int32 minutes = 0;
    int32 seconds = 0;
    if (!ReadFixedDigits(text, index, 2, hours) || !ReadCharacter(text, index, ':') ||
        !ReadFixedDigits(text, index, 2, minutes) || !ReadCharacter(text, index, ':') ||
        !ReadFixedDigits(text, index, 2, seconds) || hours > 23 || minutes > 59 || seconds > 59) {
        return Result<TimeSpan>::Failure(ErrorCode::InvalidArgument);
    }

    int64 fraction = 0;
    if (ReadCharacter(text, index, '.') && !ReadFraction(text, index, false, fraction)) {
        return Result<TimeSpan>::Failure(ErrorCode::InvalidArgument);
    }

    if (index != text.ByteCount()) {
        return Result<TimeSpan>::Failure(ErrorCode::InvalidArgument);
    }

    // 크기를 부호 없이 모은 뒤 부호를 붙인다(MinValue의 크기 2^63도 표현한다).
    const uint64 magnitude = days * static_cast<uint64>(TimeSpan::TicksPerDay) +
                             static_cast<uint64>(hours) * TimeSpan::TicksPerHour +
                             static_cast<uint64>(minutes) * TimeSpan::TicksPerMinute +
                             static_cast<uint64>(seconds) * TimeSpan::TicksPerSecond +
                             static_cast<uint64>(fraction);
    constexpr uint64 Limit = 0x8000000000000000ull;
    if (magnitude > (negative ? Limit : Limit - 1)) {
        return Result<TimeSpan>::Failure(ErrorCode::Overflow);
    }

    const int64 ticks = negative
                            ? (magnitude == Limit ? static_cast<int64>(-0x7fffffffffffffffll - 1)
                                                  : -static_cast<int64>(magnitude))
                            : static_cast<int64>(magnitude);
    return Result<TimeSpan>::Success(TimeSpan::FromTicks(ticks));
}

Result<void> Formatter<TimeSpan>::Format(StringBuilderBase& out, const TimeSpan& span,
                                         StringView spec) noexcept {
    if (!spec.IsEmpty()) {
        return Invalid();
    }

    const int64 ticks = span.Ticks();
    // MinValue도 다루려고 부호 없는 크기로 바꾼다.
    const uint64 magnitude =
        ticks < 0 ? static_cast<uint64>(-(ticks + 1)) + 1u : static_cast<uint64>(ticks);
    const uint64 days = magnitude / TimeSpan::TicksPerDay;
    const uint64 hours = magnitude / TimeSpan::TicksPerHour % 24;
    const uint64 minutes = magnitude / TimeSpan::TicksPerMinute % 60;
    const uint64 seconds = magnitude / TimeSpan::TicksPerSecond % 60;
    const uint64 fraction = magnitude % TimeSpan::TicksPerSecond;

    Result<void> result = Result<void>::Success();
    if (ticks < 0) {
        result = out.TryAppendAscii('-');
    }

    if (result && days != 0) {
        result = out.TryAppendFormat("{}.", days);
    }

    if (result) {
        result = out.TryAppendFormat("{:02}:{:02}:{:02}", hours, minutes, seconds);
    }

    if (result && fraction != 0) {
        result = out.TryAppendFormat(".{:07}", fraction);
    }

    return result;
}

// ---- DateTime ----

Result<DateTime> DateTime::TryFromDate(int32 year, int32 month, int32 day, int32 hour, int32 minute,
                                       int32 second, int32 millisecond) noexcept {
    if (year < 1 || year > 9999 || month < 1 || month > 12 || day < 1 ||
        day > DaysInMonth(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59 || millisecond < 0 || millisecond > 999) {
        return Result<DateTime>::Failure(ErrorCode::InvalidArgument);
    }

    const int64 ticks = DaysFromCivil(year, month, day) * TimeSpan::TicksPerDay +
                        hour * TimeSpan::TicksPerHour + minute * TimeSpan::TicksPerMinute +
                        second * TimeSpan::TicksPerSecond +
                        millisecond * TimeSpan::TicksPerMillisecond;
    return TryFromTicks(ticks);
}

Result<DateTime> DateTime::TryFromUnixSeconds(int64 seconds) noexcept {
    constexpr int64 Minimum = -UnixEpochTicks / TimeSpan::TicksPerSecond;
    constexpr int64 Maximum = (MaxTicks - UnixEpochTicks) / TimeSpan::TicksPerSecond;
    if (seconds < Minimum || seconds > Maximum) {
        return Result<DateTime>::Failure(ErrorCode::Overflow);
    }

    return TryFromTicks(seconds * TimeSpan::TicksPerSecond + UnixEpochTicks);
}

DateTime DateTime::UtcNow() noexcept {
    // platform::UtcTicks는 범위를 검사하고 실패하면 Fatal이므로 여기서는 항상 성공한다.
    return TryFromTicks(platform::UtcTicks()).Value();
}

Result<int32> DateTime::TryDaysInMonth(int32 year, int32 month) noexcept {
    if (year < 1 || year > 9999 || month < 1 || month > 12) {
        return Result<int32>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<int32>::Success(DaysInMonth(year, month));
}

void DateTime::GetDate(int32& year, int32& month, int32& day) const noexcept {
    CivilFromDays(ticks_ / TimeSpan::TicksPerDay, year, month, day);
}

int32 DateTime::Year() const noexcept {
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    GetDate(year, month, day);
    return year;
}

int32 DateTime::Month() const noexcept {
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    GetDate(year, month, day);
    return month;
}

int32 DateTime::Day() const noexcept {
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    GetDate(year, month, day);
    return day;
}

int32 DateTime::DayOfYear() const noexcept {
    const int64 days = ticks_ / TimeSpan::TicksPerDay;
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    CivilFromDays(days, year, month, day);
    return static_cast<int32>(days - DaysFromCivil(year, 1, 1)) + 1;
}

Result<DateTime> TryParseIso8601(StringView text) noexcept {
    usize index = 0;

    // 날짜: YYYY-MM-DD
    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    if (!ReadFixedDigits(text, index, 4, year) || !ReadCharacter(text, index, '-') ||
        !ReadFixedDigits(text, index, 2, month) || !ReadCharacter(text, index, '-') ||
        !ReadFixedDigits(text, index, 2, day)) {
        return InvalidDate();
    }

    int32 hour = 0;
    int32 minute = 0;
    int32 second = 0;
    int64 fraction = 0;
    int64 offsetTicks = 0;
    if (index != text.ByteCount()) {
        // 날짜와 시각 구분자: T, t, 공백
        const char separator = text.Data()[index++];
        if (separator != 'T' && separator != 't' && separator != ' ') {
            return InvalidDate();
        }

        // 시각: hh:mm[:ss[(.|,)소수부]]
        if (!ReadFixedDigits(text, index, 2, hour) || !ReadCharacter(text, index, ':') ||
            !ReadFixedDigits(text, index, 2, minute)) {
            return InvalidDate();
        }

        if (ReadCharacter(text, index, ':')) {
            if (!ReadFixedDigits(text, index, 2, second)) {
                return InvalidDate();
            }

            if ((ReadCharacter(text, index, '.') || ReadCharacter(text, index, ',')) &&
                !ReadFraction(text, index, true, fraction)) {
                return InvalidDate();
            }
        }

        // 시간대: Z, z, (+|-)hh[:]mm. 없으면 UTC로 본다.
        if (ReadCharacter(text, index, 'Z') || ReadCharacter(text, index, 'z')) {
            offsetTicks = 0;
        } else if (index < text.ByteCount() &&
                   (text.Data()[index] == '+' || text.Data()[index] == '-')) {
            const bool behind = text.Data()[index++] == '-';
            int32 offsetHours = 0;
            int32 offsetMinutes = 0;
            if (!ReadFixedDigits(text, index, 2, offsetHours)) {
                return InvalidDate();
            }

            ReadCharacter(text, index, ':');
            if (!ReadFixedDigits(text, index, 2, offsetMinutes) || offsetHours > 23 ||
                offsetMinutes > 59) {
                return InvalidDate();
            }

            offsetTicks =
                offsetHours * TimeSpan::TicksPerHour + offsetMinutes * TimeSpan::TicksPerMinute;
            if (behind) {
                offsetTicks = -offsetTicks;
            }
        }
    }

    if (index != text.ByteCount()) {
        return InvalidDate();
    }

    auto local = DateTime::TryFromDate(year, month, day, hour, minute, second);
    if (!local) {
        return InvalidDate();
    }

    // 지역 시각 = UTC + 오프셋이므로 UTC = 지역 시각 - 오프셋이다.
    const int64 utc = local.Value().Ticks() + fraction - offsetTicks;
    return DateTime::TryFromTicks(utc);
}

Result<void> Formatter<DateTime>::Format(StringBuilderBase& out, const DateTime& time,
                                         StringView spec) noexcept {
    if (!spec.IsEmpty()) {
        return Invalid();
    }

    int32 year = 0;
    int32 month = 0;
    int32 day = 0;
    time.GetDate(year, month, day);
    auto result = out.TryAppendFormat("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", year, month, day,
                                      time.Hour(), time.Minute(), time.Second());

    // 소수부는 0이 아닐 때만, 끝의 0을 빼고 쓴다.
    int32 fraction = time.FractionTicks();
    if (result && fraction != 0) {
        char digits[8] = {'.', '0', '0', '0', '0', '0', '0', '0'};
        for (usize position = 7; position != 0; --position) {
            digits[position] = static_cast<char>('0' + fraction % 10);
            fraction /= 10;
        }

        usize length = 8;
        while (digits[length - 1] == '0') {
            --length;
        }

        result = out.TryAppend(StringView(digits, length));
    }

    if (result) {
        result = out.TryAppendAscii('Z');
    }

    return result;
}

} // namespace rocks
