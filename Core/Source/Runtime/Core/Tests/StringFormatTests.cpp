#include <Core/Math/RandomStream.h>
#include <Core/Memory/StackAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringSplit.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

using namespace rocks;

namespace {

struct Point {
    int x;
    int y;
};

struct Broken {};

bool Same(StringView actual, const char* expected) noexcept {
    const usize bytes = strlen(expected);
    return actual.ByteCount() == bytes && memcmp(actual.Data(), expected, bytes) == 0;
}

// 서식 결과가 기대와 같은지 본다. 실패하면 빈 결과로 다룬다.
template <typename... Args>
bool Formats(const char* expected, StringView format, const Args&... args) noexcept {
    SystemAllocator allocator;
    StringBuilder<16> builder(allocator);
    if (!builder.TryAppendFormat(format, args...)) {
        printf("  format failed: expected \"%s\"\n", expected);
        return false;
    }
    if (!Same(builder.View(), expected)) {
        printf("  format mismatch: expected \"%s\" actual \"%.*s\"\n", expected,
               static_cast<int>(builder.ByteCount()), builder.View().Data());
        return false;
    }
    return true;
}

template <typename... Args>
ErrorCode FormatError(StringView format, const Args&... args) noexcept {
    SystemAllocator allocator;
    StringBuilder<> builder(allocator);
    if (!builder.TryAppend("keep")) {
        return ErrorCode::Busy;
    }
    auto result = builder.TryAppendFormat(format, args...);
    // 실패한 서식은 내용을 바꾸지 않는다.
    if (!Same(builder.View(), "keep")) {
        return ErrorCode::Busy;
    }
    return result ? ErrorCode::None : result.Error();
}

} // namespace

namespace rocks {
template <>
struct Formatter<Point> {
    static Result<void> Format(StringBuilderBase& out, const Point& point,
                               StringView spec) noexcept {
        if (!spec.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        return out.TryAppendFormat("({}, {})", point.x, point.y);
    }
};

template <>
struct Formatter<Broken> {
    static Result<void> Format(StringBuilderBase& out, const Broken&, StringView) noexcept {
        (void)out.TryAppend("partial");
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
};
} // namespace rocks

TEST(StringBuilder_InlineThenHeapAndNulTermination) {
    SystemAllocator allocator;
    {
        StringBuilder<8> builder(allocator);
        CHECK(builder.IsEmpty() && builder.Capacity() == 7 && builder.CString()[0] == '\0');
        REQUIRE(builder.TryAppend("1234567"));
        CHECK(allocator.Stats().totalAllocations == 0 && builder.Capacity() == 7);
        REQUIRE(builder.TryAppendAscii('8'));
        CHECK(allocator.Stats().liveAllocations == 1 && builder.Capacity() >= 8);
        CHECK(Same(builder.View(), "12345678") && builder.CString()[8] == '\0');
        // embedded NUL도 내용이다.
        REQUIRE(builder.TryAppend(StringView("a\0b", 3)));
        CHECK(builder.ByteCount() == 11 && builder.View().Data()[9] == '\0');
        builder.Clear();
        CHECK(builder.IsEmpty() && builder.CString()[0] == '\0');
        REQUIRE(builder.TryAppendRepeated('-', 3));
        REQUIRE(builder.TryAppendCodePoint(0x20ac)); // €
        REQUIRE(builder.TryAppendCodePoint(0x1f600));
        CHECK(Same(builder.View(), "---\xe2\x82\xac\xf0\x9f\x98\x80"));
        CHECK(builder.TryAppendCodePoint(0xd800).Error() == ErrorCode::InvalidArgument);
        CHECK(builder.TryAppendCodePoint(0x110000).Error() == ErrorCode::InvalidArgument);
        CHECK(builder.TryAppendAscii(static_cast<char>(0xc3)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(builder.TryAppend(StringView("\xc3", 1)).Error() == ErrorCode::InvalidArgument);
        CHECK(builder.ByteCount() == 10);
        auto copied = builder.TryToString(allocator);
        REQUIRE(copied);
        CHECK(Same(copied.Value().View(), "---\xe2\x82\xac\xf0\x9f\x98\x80"));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(StringBuilder_SelfAliasedAppendJoinAndFormat) {
    SystemAllocator allocator;
    StringBuilder<8> builder(allocator);
    REQUIRE(builder.TryAppend("abcdef"));
    // 성장하면서 자기 내용을 읽는다.
    REQUIRE(builder.TryAppend(builder.View()));
    CHECK(Same(builder.View(), "abcdefabcdef"));
    const StringView parts[] = {builder.View(), StringView("x")};
    REQUIRE(builder.TryAppendJoin(Span<const StringView>(parts, 2), builder.View()));
    CHECK(Same(builder.View(), "abcdefabcdefabcdefabcdefabcdefabcdefx"));
    builder.Clear();
    REQUIRE(builder.TryAppend("{}-{}"));
    // format 문자열과 인자가 모두 builder 자신을 가리킨다.
    REQUIRE(builder.TryAppendFormat(builder.View(), builder.View(), 7));
    CHECK(Same(builder.View(), "{}-{}{}-{}-7"));
}

TEST(StringBuilder_ChainedAppendRemembersFirstFailure) {
    SystemAllocator allocator;
    StringBuilder<> builder(allocator);
    builder << "frame " << 42 << ' ' << 1.5;
    REQUIRE(builder.Status());
    CHECK(Same(builder.View(), "frame 42 1.5"));
    builder.Append(StringView("\xff", 1)).Append("ignored") << 7;
    CHECK(builder.HasFailed() && builder.Status().Error() == ErrorCode::InvalidArgument);
    CHECK(Same(builder.View(), "frame 42 1.5"));
    builder.Clear();
    CHECK(!builder.HasFailed());
    builder.AppendFormat("{}+{}={}", 1, 2, 3);
    CHECK(builder.Status() && Same(builder.View(), "1+2=3"));
}

TEST(StringBuilder_GrowthFailurePreservesContent) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        StringBuilder<8> builder(failing);
        REQUIRE(builder.TryAppend("short"));
        failing.FailOnAttempt(1);
        CHECK(builder.TryAppend("this does not fit").Error() == ErrorCode::OutOfMemory);
        CHECK(Same(builder.View(), "short") && builder.Capacity() == 7);
        failing.FailOnAttempt(1);
        CHECK(builder.TryAppendFormat("{:>40}", 1).Error() == ErrorCode::OutOfMemory);
        CHECK(Same(builder.View(), "short"));
        failing.DisableFailure();
        REQUIRE(builder.TryAppend(" and now it grows"));
        CHECK(Same(builder.View(), "short and now it grows"));
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(Format_FieldsEscapesAndErrors) {
    CHECK(Formats("a 1 b 2", "a {} b {}", 1, 2));
    CHECK(Formats("2 1 2", "{1} {0} {1}", 1, 2));
    CHECK(Formats("{x}", "{{x}}"));
    CHECK(Formats("no fields", "no fields"));
    CHECK(Formats("unused ok", "unused ok", 1, 2));
    CHECK(FormatError("{} {0}", 1) == ErrorCode::InvalidArgument); // 자동·수동 혼합
    CHECK(FormatError("{0} {}", 1) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{} {}", 1) == ErrorCode::InvalidArgument); // 인자 부족
    CHECK(FormatError("{2}", 1, 2) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{", 1) == ErrorCode::InvalidArgument);
    CHECK(FormatError("}", 1) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{:", 1) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{01}", 1, 2) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{:q}", 1) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{:.2}", 1) == ErrorCode::InvalidArgument);    // 정수에 정밀도
    CHECK(FormatError("{:99999}", 1) == ErrorCode::InvalidArgument); // 폭 한도
    CHECK(FormatError(StringView("\xff{}", 3), 1) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{}", static_cast<const char*>(nullptr)) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{}", StringView("\xc3", 1)) == ErrorCode::InvalidArgument);
}

TEST(Format_Integers) {
    CHECK(Formats("-9223372036854775808", "{}", static_cast<int64>(-0x7fffffffffffffffll - 1)));
    CHECK(Formats("18446744073709551615", "{}", ~0ull));
    CHECK(Formats("255 ff FF 11111111 377", "{0} {0:x} {0:X} {0:b} {0:o}", 255));
    CHECK(Formats("0xff 0XFF 0b101 017 0", "{:#x} {:#X} {:#b} {:#o} {:#o}", 255, 255, 5, 15, 0));
    CHECK(Formats("+5 -5  5 5", "{:+} {:+} {: } {:-}", 5, -5, 5, 5));
    CHECK(Formats("00042|-0042|0x002a", "{:05}|{:05}|{:#06x}", 42, -42, 42));
    CHECK(Formats("   42|42   | 42 |**42**", "{:5}|{:<5}|{:^4}|{:*^6}", 42, 42, 42, 42));
    CHECK(Formats("-128 255 65535", "{} {} {}", static_cast<signed char>(-128),
                  static_cast<unsigned char>(255), static_cast<unsigned short>(65535)));
    CHECK(Formats("true false 1 0", "{} {} {:d} {:d}", true, false, true, false));
    CHECK(Formats("A 65 41", "{} {:d} {:x}", 'A', 'A', 'A'));
    CHECK(FormatError("{}", static_cast<char>(0xc3)) == ErrorCode::InvalidArgument);
    CHECK(Formats("195", "{:d}", static_cast<char>(0xc3))); // char는 부호 없는 값으로 읽는다
}

TEST(Format_FloatingPoint) {
    CHECK(Formats("0.1 1 100 1e+20 0.0001 1e-05 -0 2.5", "{} {} {} {} {} {} {} {}", 0.1, 1.0, 100.0,
                  1e20, 0.0001, 0.00001, -0.0, 2.5));
    CHECK(Formats("0.1 3.14159 16777216", "{} {} {}", 0.1f, 3.14159f, 16777216.0f));
    CHECK(Formats("0.30000000000000004", "{}", 0.1 + 0.2));
    CHECK(Formats("3.142 3.141590 3.1 1.234500e+04 1.23E+04", "{:.3f} {:f} {:.2} {:e} {:.2E}",
                  3.14159, 3.14159, 3.14159, 12345.0, 12345.0));
    CHECK(Formats("-0003.14|+2.50|  1.5", "{:08.2f}|{:+.2f}|{:>5}", -3.14159, 2.5, 1.5));
    const double infinity = 1e308 * 10.0;
    const double nan = infinity - infinity;
    CHECK(Formats("inf -inf INF nan", "{} {} {:F} {}", infinity, -infinity, infinity,
                  nan < 0 ? -nan : nan));
    CHECK(Formats("   inf", "{:06}", infinity)); // inf·nan은 0으로 채우지 않는다
    CHECK(FormatError("{:x}", 1.0) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{:#}", 1.0) == ErrorCode::InvalidArgument);
    // 큰 고정 소수점은 heap scratch를 쓴다.
    SystemAllocator allocator;
    StringBuilder<> builder(allocator);
    REQUIRE(builder.TryAppendFormat("{:.200f}", 1e300));
    CHECK(builder.ByteCount() == 301 + 1 + 200);
    CHECK(builder.View().Data()[301] == '.');
}

TEST(Format_ShortestRoundTripsRandomDoubles) {
    RandomStream random(test::CurrentSeed() ^ 0xd0b1eull);
    SystemAllocator allocator;
    StringBuilder<64> builder(allocator);
    usize checked = 0;
    for (int iteration = 0; iteration < 3000; ++iteration) {
        const uint64 high = random.NextUInt32();
        const uint64 low = random.NextUInt32();
        const uint64 bits = (high << 32) | low;
        double value = 0;
        memcpy(&value, &bits, sizeof(value));
        if (value != value || value - value != 0.0) {
            continue; // NaN·무한 제외
        }
        builder.Clear();
        REQUIRE(builder.TryAppendFormat("{}", value));
        auto parsed = TryParseDouble(builder.View());
        REQUIRE(parsed);
        uint64 back = 0;
        const double reparsed = parsed.Value();
        memcpy(&back, &reparsed, sizeof(back));
        CHECK(back == bits);
        ++checked;
        const float single = static_cast<float>(random.NextUInt32()) / 4096.0f;
        builder.Clear();
        REQUIRE(builder.TryAppendFormat("{}", single));
        auto singleParsed = TryParseFloat(builder.View());
        CHECK(singleParsed && singleParsed.Value() == single);
    }
    CHECK(checked > 2500);
}

TEST(Format_TextPointersAndCustom) {
    CHECK(Formats("    ab|ab    |  ab  |ab", "{:>6}|{:<6}|{:^6}|{:s}", "ab", "ab", "ab", "ab"));
    // 폭과 정밀도는 code point 단위다.
    CHECK(Formats("한글|한   |", "{:.2}|{:<4}|", "한글텍스트", "한"));
    SystemAllocator allocator;
    String owned(allocator);
    REQUIRE(owned.TryAssign("owned"));
    const char buffer[] = "array";
    char mutableText[] = "mutable";
    CHECK(Formats("owned array mutable view", "{} {} {} {}", owned, buffer, mutableText,
                  StringView("view")));
    CHECK(FormatError("{:+}", "x") == ErrorCode::InvalidArgument);
    CHECK(FormatError("{:05}", "x") == ErrorCode::InvalidArgument);
    int object = 0;
    const void* address = &object;
    SystemAllocator scratch;
    StringBuilder<> builder(scratch);
    REQUIRE(builder.TryAppendFormat("{}", address));
    CHECK(builder.View().StartsWith("0x") && builder.ByteCount() > 2);
    CHECK(Formats("0x0", "{}", nullptr));
    CHECK(Formats("(1, 2) [(3, -4)]", "{} [{}]", Point{1, 2}, Point{3, -4}));
    CHECK(FormatError("{:x}", Point{1, 2}) == ErrorCode::InvalidArgument);
    CHECK(FormatError("a{}b", Broken{}) == ErrorCode::Unsupported); // 부분 출력도 되돌린다
}

TEST(Format_IgnoresCLocaleDecimalSeparator) {
    const char* candidates[] = {"de_DE.UTF-8", "de_DE", "German", "fr_FR.UTF-8"};
    const char* applied = nullptr;
    for (const char* name : candidates) {
        if (setlocale(LC_NUMERIC, name) != nullptr && localeconv()->decimal_point[0] == ',') {
            applied = name;
            break;
        }
    }
    if (applied == nullptr) {
        (void)setlocale(LC_NUMERIC, "C");
        printf("[SKIP] no comma-decimal locale on this host\n");
        return;
    }
    const bool formatted = Formats("1.5 0.25 3.000", "{} {} {:.3f}", 1.5, 0.25, 3.0);
    auto parsed = TryParseDouble("2.75");
    auto comma = TryParseDouble("2,75");
    (void)setlocale(LC_NUMERIC, "C");
    CHECK(formatted);
    CHECK(parsed && parsed.Value() == 2.75);
    CHECK(!comma && comma.Error() == ErrorCode::InvalidArgument);
}

TEST(Format_ReviewRegressions) {
    // NUL 없는 char 배열은 배열 끝에서 멈춘다(뒤 메모리를 읽지 않는다).
    struct {
        char head[4];
        char tail[8];
    } packed{};
    memcpy(packed.head, "abcd", 4);
    memcpy(packed.tail, "XYZ", 4);
    CHECK(Formats("[abcd]", "[{}]", packed.head));

    // std::format처럼 bool·char에는 정밀도를 받지 않는다.
    CHECK(FormatError("{:.2}", true) == ErrorCode::InvalidArgument);
    CHECK(FormatError("{:.0}", 'A') == ErrorCode::InvalidArgument);

    // 최단 표기는 찾은 유효 숫자로 조립한다(이진 값을 다시 전개하지 않는다).
    CHECK(Formats("100000000000 1000000000000000 1e+16", "{} {} {}", 1e11f, 1e15f, 1e16f));
    // 2의 거듭제곱 경계: 반올림 값이 아니라 그 이웃이 한 자리 짧게 왕복한다.
    double boundary = 1.0;
    for (int i = 0; i < 1017; ++i) {
        boundary /= 2.0;
    }

    CHECK(Formats("7.120236347223045e-307", "{}", boundary));
    float singleBoundary = 1.0f;
    for (int i = 0; i < 96; ++i) {
        singleBoundary /= 2.0f;
    }

    CHECK(Formats("1.2621775e-29", "{}", singleBoundary));

    // 정밀도 상한(1074)까지는 heap 없이 쓴다. 넘으면 InvalidArgument다.
    SystemAllocator allocator;
    StringBuilder<> builder(allocator);
    REQUIRE(builder.TryAppendFormat("{:.1074f}", 1.0));
    CHECK(builder.ByteCount() == 1 + 1 + 1074);
    CHECK(FormatError("{:.1075f}", 1.0) == ErrorCode::InvalidArgument);
}

TEST(Format_WorksWithLifoAllocatorAfterReserve) {
    // 역순 반환만 받는 allocator에서도 미리 확보하면 서식이 별도 할당을 하지 않는다.
    SystemAllocator backing;
    StackAllocator stack(backing, 16384);
    {
        StringBuilder<16> builder(stack);
        REQUIRE(builder.TryReserve(4096));
        REQUIRE(builder.TryAppendFormat("{:300}{:300}", 1, 2));
        REQUIRE(builder.TryAppendFormat("{:.300f}", 1.0));
        CHECK(builder.ByteCount() == 600 + 302);
    }
}

TEST(NumberText_StrictIntegers) {
    CHECK(TryParseInt64("123").Value() == 123 && TryParseInt64("-123").Value() == -123);
    CHECK(TryParseInt64("+7").Value() == 7 && TryParseInt64("0").Value() == 0);
    CHECK(TryParseInt64("9223372036854775807").Value() == 0x7fffffffffffffffll);
    CHECK(TryParseInt64("-9223372036854775808").Value() == -0x7fffffffffffffffll - 1);
    CHECK(TryParseInt64("9223372036854775808").Error() == ErrorCode::Overflow);
    CHECK(TryParseInt64("-9223372036854775809").Error() == ErrorCode::Overflow);
    CHECK(TryParseInt64("99999999999999999999999x").Error() == ErrorCode::InvalidArgument);
    const char* invalid[] = {"", "-", "+", " 1", "1 ", "12a", "0x10", "1_000", "--1"};
    for (const char* text : invalid) {
        CHECK(TryParseInt64(text).Error() == ErrorCode::InvalidArgument);
    }
    CHECK(TryParseUInt64("18446744073709551615").Value() == ~0ull);
    CHECK(TryParseUInt64("18446744073709551616").Error() == ErrorCode::Overflow);
    CHECK(TryParseUInt64("-1").Error() == ErrorCode::InvalidArgument);
    CHECK(TryParseUInt64("ff", 16).Value() == 255 && TryParseUInt64("FF", 16).Value() == 255);
    CHECK(TryParseInt64("-zz", 36).Value() == -1295);
    CHECK(TryParseUInt64("102", 2).Error() == ErrorCode::InvalidArgument);
    CHECK(TryParseUInt64("1", 1).Error() == ErrorCode::InvalidArgument);
    CHECK(TryParseUInt64("1", 37).Error() == ErrorCode::InvalidArgument);
    CHECK(TryParseInt32("2147483647").Value() == 2147483647);
    CHECK(TryParseInt32("-2147483648").Value() == -2147483647 - 1);
    CHECK(TryParseInt32("2147483648").Error() == ErrorCode::Overflow);
    CHECK(TryParseUInt32("4294967295").Value() == 4294967295u);
    CHECK(TryParseUInt32("4294967296").Error() == ErrorCode::Overflow);
}

TEST(NumberText_StrictFloatingPoint) {
    CHECK(TryParseDouble("1.5").Value() == 1.5 && TryParseDouble("-0.25").Value() == -0.25);
    CHECK(TryParseDouble(".5").Value() == 0.5 && TryParseDouble("5.").Value() == 5.0);
    CHECK(TryParseDouble("1e3").Value() == 1000.0 && TryParseDouble("1E-3").Value() == 0.001);
    CHECK(TryParseDouble("+2").Value() == 2.0);
    auto infinity = TryParseDouble("-Infinity");
    CHECK(infinity && infinity.Value() < -1e308);
    auto nan = TryParseDouble("NaN");
    CHECK(nan && nan.Value() != nan.Value());
    const char* invalid[] = {"",   ".",   "e3",   "1e",  "1e+",     "0x1p3", " 1",
                             "1 ", "1,5", "1.5f", "--1", "infinit", "1.2.3"};
    for (const char* text : invalid) {
        CHECK(TryParseDouble(text).Error() == ErrorCode::InvalidArgument);
    }
    CHECK(TryParseDouble("1e400").Error() == ErrorCode::Overflow);
    auto tiny = TryParseDouble("1e-400");
    CHECK(tiny && tiny.Value() == 0.0);
    CHECK(TryParseFloat("3.4e39").Error() == ErrorCode::Overflow);
    CHECK(TryParseFloat("0.1").Value() == 0.1f);
    // 길이 제한이 없다: 1100자리 정수는 범위를 넘고(Overflow), 300자리는 정확히 반올림된다.
    char longText[1100];
    memset(longText, '1', sizeof(longText));
    CHECK(TryParseDouble(StringView(longText, sizeof(longText))).Error() == ErrorCode::Overflow);
    auto threeHundred = TryParseDouble(StringView(longText, 300));
    CHECK(threeHundred && threeHundred.Value() > 1.11e299 && threeHundred.Value() < 1.12e299);
}

TEST(StringSplit_IntoArrayAndJoin) {
    SystemAllocator allocator;
    Array<StringView> parts(allocator);
    auto count = TrySplitIntoArray("a,b,,c,", ",", parts);
    REQUIRE(count);
    CHECK(count.Value() == 3 && Same(parts[0], "a") && Same(parts[2], "c"));
    parts.Clear();
    SplitOptions keep;
    keep.cullEmpty = false;
    count = TrySplitIntoArray("a,b,,c,", ",", parts, keep);
    REQUIRE(count);
    CHECK(count.Value() == 5 && parts[2].IsEmpty() && parts[4].IsEmpty());
    parts.Clear();
    SplitOptions trimmed;
    trimmed.trimAsciiWhitespace = true;
    count = TrySplitIntoArray(" x :: y ::  :: z", "::", parts, trimmed);
    REQUIRE(count);
    CHECK(count.Value() == 3 && Same(parts[0], "x") && Same(parts[1], "y") && Same(parts[2], "z"));
    parts.Clear();
    SplitOptions folded;
    folded.comparison = StringComparison::IgnoreAsciiCase;
    count = TrySplitIntoArray("aANDbandc", "and", parts, folded);
    REQUIRE(count);
    CHECK(count.Value() == 3 && Same(parts[1], "b"));
    count = TrySplitIntoArray("whole", ",", parts);
    CHECK(count && count.Value() == 1 && Same(parts[3], "whole")); // out 뒤에 붙인다
    CHECK(TrySplitIntoArray("a", "", parts).Error() == ErrorCode::InvalidArgument);
    parts.Clear();
    count = TrySplitIntoArray("", ",", parts, keep);
    CHECK(count && count.Value() == 1 && parts[0].IsEmpty());

    const StringView words[] = {"alpha", "beta", "gamma"};
    auto joined = TryJoin(allocator, Span<const StringView>(words, 3), ", ");
    REQUIRE(joined);
    CHECK(Same(joined.Value().View(), "alpha, beta, gamma"));
    auto empty = TryJoin(allocator, Span<const StringView>(), ",");
    CHECK(empty && empty.Value().ByteCount() == 0);
    const StringView bad[] = {"ok", StringView("\xff", 1)};
    CHECK(TryJoin(allocator, Span<const StringView>(bad, 2), ",").Error() ==
          ErrorCode::InvalidArgument);
}

TEST(StringSplit_GrowthFailureRestoresArray) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    Array<StringView> parts(failing);
    REQUIRE(parts.TryPushBack("existing"));
    failing.FailOnAttempt(1);
    auto failed = TrySplitIntoArray("a,b,c,d,e,f,g,h", ",", parts);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(parts.Size() == 1 && Same(parts[0], "existing"));
    failing.DisableFailure();
}
