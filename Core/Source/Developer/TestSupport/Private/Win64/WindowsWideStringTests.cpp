#include <Core/Move.h>
#include <Platform/Win64/WideString.h>
#include <TestSupport/Test.h>

#include <Windows.h>
#include <string.h>

// Platform/Win64/WideString.h 시험이다(UTF-8·UTF-16 변환, 할당 판과 고정 버퍼 판). Windows 전용
// 시험이라 TestSupport/Private/Win64에 둔다.
using namespace rocks;
using namespace rocks::platform;
using namespace rocks::platform::windows;

namespace {

// "가"(U+AC00)는 UTF-8 3 byte, UTF-16 1단위다.
constexpr char Hangul[] = "\xEA\xB0\x80";
// "😀"(U+1F600)는 UTF-8 4 byte, UTF-16 서로게이트 쌍 2단위다.
constexpr char Emoji[] = "\xF0\x9F\x98\x80";
constexpr wchar_t EmojiWide[] = {0xD83D, 0xDE00, 0};
// 두 처리 방식 모두 같은 거부 규칙인지 본다.
constexpr TextFit Fits[] = {TextFit::Exact, TextFit::Truncate};

bool SameWide(const wchar_t* left, const wchar_t* right) noexcept {
    usize index = 0;
    while (left[index] == right[index]) {
        if (left[index] == L'\0') {
            return true;
        }

        ++index;
    }

    return false;
}

bool IsRejected(const FileStatus& status) noexcept {
    return status.code == FileCode::InvalidArgument &&
           status.nativeError == ERROR_NO_UNICODE_TRANSLATION;
}

} // namespace

TEST(PlatformWindowsWideStringRoundTripsAsciiHangulAndSurrogatePairs) {
    // "a가😀z"
    const char text[] = "a\xEA\xB0\x80\xF0\x9F\x98\x80z";
    const wchar_t expected[] = {L'a', 0xAC00, 0xD83D, 0xDE00, L'z', 0};
    WideString wide;
    REQUIRE(wide.AssignUtf8(text, strlen(text)));
    CHECK(wide.Units() == 5 && SameWide(wide.Value(), expected));

    Utf8String narrow;
    REQUIRE(narrow.AssignWide(wide.Value(), wide.Units()));
    CHECK(narrow.Bytes() == strlen(text) && strcmp(narrow.Value(), text) == 0);

    // 이동하면 값이 옮겨 가고 원래 쪽은 빈다.
    WideString movedWide(Move(wide));
    CHECK(wide.Value() == nullptr && wide.Units() == 0 && movedWide.Units() == 5);
    Utf8String movedNarrow;
    movedNarrow = Move(narrow);
    CHECK(narrow.Value() == nullptr && movedNarrow.Bytes() == strlen(text));
}

TEST(PlatformWindowsWideStringEmptyInputIsEmptyString) {
    WideString wide;
    REQUIRE(wide.AssignUtf8("", 0));
    CHECK(wide.Value() != nullptr && wide.Value()[0] == L'\0' && wide.Units() == 0);
    REQUIRE(wide.AssignUtf8(nullptr, 0));
    CHECK(wide.Value() != nullptr && wide.Units() == 0);

    Utf8String narrow;
    REQUIRE(narrow.AssignWide(L"", 0));
    CHECK(narrow.Value() != nullptr && narrow.Value()[0] == '\0' && narrow.Bytes() == 0);

    wchar_t wideBuffer[4] = {L'x', L'x', L'x', L'x'};
    const TextConversion toWide = ConvertUtf8ToWide("", 0, wideBuffer, 4, TextFit::Exact);
    CHECK(toWide && toWide.length == 0 && !toWide.truncated && wideBuffer[0] == L'\0');

    char narrowBuffer[4] = {'x', 'x', 'x', 'x'};
    const TextConversion toUtf8 = ConvertWideToUtf8(L"", 0, narrowBuffer, 4, TextFit::Exact);
    CHECK(toUtf8 && toUtf8.length == 0 && !toUtf8.truncated && narrowBuffer[0] == '\0');
}

TEST(PlatformWindowsWideStringRejectsInvalidUtf8) {
    // 잘린 sequence, continuation byte로 시작, overlong '/', UTF-8로 부호화한 surrogate(U+D800),
    // U+10FFFF 초과(F4 90 80 80).
    const char* invalid[] = {"\xEA\xB0", "\x80", "\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80"};
    for (const char* text : invalid) {
        WideString wide;
        CHECK(IsRejected(wide.AssignUtf8(text, strlen(text))));
        CHECK(wide.Value() == nullptr);

        wchar_t buffer[8] = {L'x'};
        for (const TextFit fit : Fits) {
            const TextConversion converted = ConvertUtf8ToWide(text, strlen(text), buffer, 8, fit);
            CHECK(IsRejected(converted.status) && converted.length == 0 && buffer[0] == L'\0');
        }
    }

    // 잘릴 앞부분이 유효해도 입력 전체를 검사한다.
    const char tail[] = "abcdef\xFF";
    wchar_t small[4] = {};
    CHECK(IsRejected(ConvertUtf8ToWide(tail, strlen(tail), small, 4, TextFit::Truncate).status));

    // null text에 길이가 있으면 인자 오류다.
    WideString wide;
    const FileStatus nullText = wide.AssignUtf8(nullptr, 3);
    CHECK(nullText.code == FileCode::InvalidArgument && nullText.nativeError == 0);
}

TEST(PlatformWindowsWideStringRejectsLoneSurrogates) {
    const wchar_t loneHigh[] = {L'a', 0xD83D, L'b', 0};
    const wchar_t loneLow[] = {0xDE00, 0};
    const wchar_t trailingHigh[] = {L'a', 0xD83D, 0};
    const wchar_t* const lones[] = {loneHigh, loneLow, trailingHigh};
    for (const wchar_t* text : lones) {
        const usize units = WideLength(text, 8);
        Utf8String narrow;
        CHECK(IsRejected(narrow.AssignWide(text, units)));
        CHECK(narrow.Value() == nullptr);

        char buffer[16] = {'x'};
        for (const TextFit fit : Fits) {
            const TextConversion converted = ConvertWideToUtf8(text, units, buffer, 16, fit);
            CHECK(IsRejected(converted.status) && converted.length == 0 && buffer[0] == '\0');
        }
    }
}

TEST(PlatformWindowsWideStringExactReportsRequiredLength) {
    // "a가" = UTF-16 2단위, UTF-8 4 byte.
    const char text[] = "a\xEA\xB0\x80";
    wchar_t wide[2] = {L'x', L'x'};
    TextConversion converted = ConvertUtf8ToWide(text, strlen(text), wide, 2, TextFit::Exact);
    CHECK(converted.status.code == FileCode::Overflow && converted.required == 2 &&
          converted.length == 0 && wide[0] == L'\0');
    wchar_t fits[3] = {};
    converted = ConvertUtf8ToWide(text, strlen(text), fits, 3, TextFit::Exact);
    CHECK(converted && converted.length == 2 && fits[1] == 0xAC00 && fits[2] == L'\0');

    const wchar_t source[] = {L'a', 0xAC00, 0};
    char narrow[4] = {'x'};
    converted = ConvertWideToUtf8(source, 2, narrow, 4, TextFit::Exact);
    CHECK(converted.status.code == FileCode::Overflow && converted.required == 4 &&
          narrow[0] == '\0');
    char narrowFits[5] = {};
    converted = ConvertWideToUtf8(source, 2, narrowFits, 5, TextFit::Exact);
    CHECK(converted && converted.length == 4 && strcmp(narrowFits, text) == 0);

    // capacity 0은 쓰지 않고 필요한 길이만 알린다.
    converted = ConvertWideToUtf8(source, 2, nullptr, 0, TextFit::Exact);
    CHECK(converted.status.code == FileCode::Overflow && converted.required == 4);
    converted = ConvertUtf8ToWide(text, strlen(text), nullptr, 0, TextFit::Truncate);
    CHECK(converted.status.code == FileCode::Overflow && converted.required == 2);
}

TEST(PlatformWindowsWideStringTruncateKeepsUtf8CharacterBoundaries) {
    // "a가😀" = 1 + 3 + 4 byte. 칸(NUL 제외)이 byte 경계마다 어디서 끊기는지 본다.
    const wchar_t source[] = {L'a', 0xAC00, 0xD83D, 0xDE00, 0};
    struct Case {
        usize capacity;
        usize bytes;
    };

    const Case cases[] = {{1, 0}, {2, 1}, {3, 1}, {4, 1}, {5, 4}, {6, 4}, {7, 4}, {8, 4}};
    for (const Case& item : cases) {
        char buffer[16];
        memset(buffer, 'x', sizeof(buffer));
        const TextConversion converted =
            ConvertWideToUtf8(source, 4, buffer, item.capacity, TextFit::Truncate);
        CHECK(converted && converted.truncated && converted.length == item.bytes &&
              buffer[item.bytes] == '\0');
        CHECK(memcmp(buffer, "a\xEA\xB0\x80", item.bytes) == 0);
    }

    char whole[9];
    const TextConversion converted = ConvertWideToUtf8(source, 4, whole, 9, TextFit::Truncate);
    CHECK(converted && !converted.truncated && converted.length == 8);
    CHECK(memcmp(whole + 4, Emoji, 4) == 0 && whole[8] == '\0');
}

TEST(PlatformWindowsWideStringTruncateKeepsSurrogatePairs) {
    // "가😀b" = UTF-16 1 + 2 + 1단위. 서로게이트 쌍을 둘로 나누지 않는다.
    const char text[] = "\xEA\xB0\x80\xF0\x9F\x98\x80"
                        "b";
    struct Case {
        usize capacity;
        usize units;
    };

    const Case cases[] = {{1, 0}, {2, 1}, {3, 1}, {4, 3}};
    for (const Case& item : cases) {
        wchar_t buffer[8];
        for (wchar_t& unit : buffer) {
            unit = L'x';
        }

        const TextConversion converted =
            ConvertUtf8ToWide(text, strlen(text), buffer, item.capacity, TextFit::Truncate);
        CHECK(converted && converted.truncated && converted.length == item.units &&
              buffer[item.units] == L'\0');
    }

    wchar_t pair[4];
    TextConversion converted = ConvertUtf8ToWide(text, strlen(text), pair, 4, TextFit::Truncate);
    CHECK(converted && pair[0] == 0xAC00 && pair[1] == EmojiWide[0] && pair[2] == EmojiWide[1]);

    wchar_t whole[5];
    converted = ConvertUtf8ToWide(text, strlen(text), whole, 5, TextFit::Truncate);
    CHECK(converted && !converted.truncated && converted.length == 4 && whole[3] == L'b');

    // 한글만으로 된 이름은 UTF-16 칸 수만큼 들어간다.
    wchar_t hangul[2];
    converted = ConvertUtf8ToWide(Hangul, strlen(Hangul), hangul, 2, TextFit::Truncate);
    CHECK(converted && !converted.truncated && converted.length == 1 && hangul[0] == 0xAC00);
}

TEST(PlatformWindowsWideStringWideLengthStopsAtNulOrLimit) {
    const wchar_t terminated[8] = {L'a', L'b', 0, L'c'};
    CHECK(WideLength(terminated) == 2);
    const wchar_t full[3] = {L'a', L'b', L'c'};
    CHECK(WideLength(full) == 3);
    CHECK(WideLength(full, 2) == 2);
    CHECK(WideLength(EmojiWide) == 2);
}
