#include "AssetDatabaseTestSupport.h"

#include <AssetDatabase/LogicalPath.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <string.h>

// 논리 경로 시험이다(Docs/Design/AssetSystem.ko.md 4.2절, 7.1절 10 — A2a): 규칙마다 양성·음성, 판정
// 순서(겹친 위반), NFC 정규화·caseless 키, 접근자, 할당 실패.
using namespace rocks;

namespace logical_path_tests_file {

namespace {

struct ValidCase {
    const char* input;
    const char* text;
    const char* key;
    const char* name;
    const char* directory;
};

struct InvalidCase {
    const char* input;
    usize inputBytes; // 0이면 strlen(NUL이 든 입력만 길이를 준다)
    LogicalPathError error;
    uint32 component;
    NameComponentError componentError;
};

StringView InputOf(const InvalidCase& c) noexcept {
    return StringView(c.input, c.inputBytes != 0 ? c.inputBytes : strlen(c.input));
}

// 같은 문자 count개의 구성 요소를 '/'로 이은 "/Game/..." 경로를 out에 쓴다.
void RepeatedPath(char* out, usize capacity, usize components, usize componentBytes,
                  char letter) noexcept {
    usize at = 0;
    const char* mount = "/Game";
    for (const char* c = mount; *c != 0 && at + 1 < capacity; ++c) {
        out[at++] = *c;
    }

    for (usize index = 0; index < components; ++index) {
        out[at++] = '/';
        for (usize byte = 0; byte < componentBytes && at + 1 < capacity; ++byte) {
            out[at++] = letter;
        }
    }

    out[at] = 0;
}

} // namespace

// 양성(4.2절): 마운트 철자 정규화(대소문자 무시), 숫자 첫 문자, '_'·'-', 비ASCII 문자(L*), NFD
// 입력의 NFC 정규화, 접근자(Text·Key·Name·Directory·Relative·Mount).
TEST(LogicalPath_ValidPaths) {
    SystemAllocator allocator;
    const ValidCase cases[] = {
        {"/Game/Hero", "/Game/Hero", "game/hero", "Hero", "/Game"},
        {"/game/Characters/Hero", "/Game/Characters/Hero", "game/characters/hero", "Hero",
         "/Game/Characters"},
        {"/GAME/a_b-1/Z9", "/Game/a_b-1/Z9", "game/a_b-1/z9", "Z9", "/Game/a_b-1"},
        {"/Game/9Lives", "/Game/9Lives", "game/9lives", "9Lives", "/Game"},
        // 비ASCII 숫자(N* — U+0663 ARABIC-INDIC DIGIT THREE)가 첫 문자.
        {"/Game/\xd9\xa3"
         "abc",
         "/Game/\xd9\xa3"
         "abc",
         "game/\xd9\xa3"
         "abc",
         "\xd9\xa3"
         "abc",
         "/Game"},
        // 한글(L*) 구성 요소.
        {"/Game/\xed\x95\x9c\xea\xb8\x80/\xec\x98\x81\xec\x9b\x85",
         "/Game/\xed\x95\x9c\xea\xb8\x80/\xec\x98\x81\xec\x9b\x85",
         "game/\xed\x95\x9c\xea\xb8\x80/\xec\x98\x81\xec\x9b\x85", "\xec\x98\x81\xec\x9b\x85",
         "/Game/\xed\x95\x9c\xea\xb8\x80"},
        // NFD "e + U+0301"은 NFC "U+00E9"가 된다.
        {"/Game/Cafe\xcc\x81", "/Game/Caf\xc3\xa9", "game/caf\xc3\xa9", "Caf\xc3\xa9", "/Game"},
        // 대문자 비ASCII는 simple case folding 키로 접힌다.
        {"/Game/\xc3\x84Rger", "/Game/\xc3\x84Rger", "game/\xc3\xa4rger", "\xc3\x84Rger", "/Game"},
    };
    for (const ValidCase& c : cases) {
        LogicalPathFailure failure;
        failure.error = LogicalPathError::TooLong;
        auto parsed = ParseLogicalPath(allocator, c.input, &failure);
        REQUIRE(parsed);
        CHECK(parsed.Value().Text() == c.text);
        CHECK(parsed.Value().Key() == c.key);
        CHECK(parsed.Value().Name() == c.name);
        CHECK(parsed.Value().Directory() == c.directory);
        CHECK(parsed.Value().Mount() == "/Game");
        CHECK(parsed.Value().Relative() == StringView(c.text + 6));
        CHECK(failure.error == LogicalPathError::None);
    }
}

// 비교 키(4.2절): 대소문자·정규화 형태만 다른 경로는 같은 키(같은 경로)이고, simple case
// folding이라 'ß'와 "ss"는 다르다. 접기 뒤에도 다른 경로는 다른 키다.
TEST(LogicalPath_CaselessKeys) {
    SystemAllocator allocator;
    auto a = ParseLogicalPath(allocator, "/Game/Hero/Sword");
    auto b = ParseLogicalPath(allocator, "/game/HERO/sword");
    auto nfd = ParseLogicalPath(allocator, "/Game/Cafe\xcc\x81");
    auto nfc = ParseLogicalPath(allocator, "/Game/CAF\xc3\x89");
    auto sharp = ParseLogicalPath(allocator, "/Game/Stra\xc3\x9f"
                                             "e");
    auto doubled = ParseLogicalPath(allocator, "/Game/Strasse");
    auto other = ParseLogicalPath(allocator, "/Game/Hero/Swords");
    REQUIRE(a && b && nfd && nfc && sharp && doubled && other);
    CHECK(a.Value() == b.Value());
    CHECK(a.Value().Text() != b.Value().Text());
    CHECK(nfd.Value() == nfc.Value());
    CHECK(!(sharp.Value() == doubled.Value()));
    CHECK(!(a.Value() == other.Value()));
}

// 음성(4.2절, 판정 순서): 규칙마다 거부 사유와 위치. 겹친 위반은 앞 규칙이 이긴다.
TEST(LogicalPath_InvalidPaths) {
    SystemAllocator allocator;
    char longComponent[128];
    RepeatedPath(longComponent, sizeof(longComponent), 1, MaxNameComponentBytes + 1, 'a');
    char longTotal[512];
    // "/Game"(5) + 4 × (1 + 64) = 265 byte(구성 요소는 모두 64 byte — 각자는 규칙을 지킨다).
    RepeatedPath(longTotal, sizeof(longTotal), 4, MaxNameComponentBytes, 'b');
    const char nul[] = {'/', 'G', 'a', 'm', 'e', '/', 'A', 0, 'B'};
    // "/Game" + 4 × (1 + 64) + "/_x" = 268 byte이고 마지막 구성 요소가 InvalidFirst다.
    char tooLongAndInvalid[512];
    RepeatedPath(tooLongAndInvalid, sizeof(tooLongAndInvalid), 4, MaxNameComponentBytes, 'h');
    const usize invalidAt = strlen(tooLongAndInvalid);
    tooLongAndInvalid[invalidAt] = '/';
    tooLongAndInvalid[invalidAt + 1] = '_';
    tooLongAndInvalid[invalidAt + 2] = 'x';
    tooLongAndInvalid[invalidAt + 3] = 0;
    const InvalidCase cases[] = {
        {"", 0, LogicalPathError::Empty, 0, NameComponentError::None},
        {"Game/Hero", 0, LogicalPathError::MissingLeadingSlash, 0, NameComponentError::None},
        {"/Game\\Hero", 0, LogicalPathError::Backslash, 0, NameComponentError::None},
        {"/", 0, LogicalPathError::EmptyComponent, 0, NameComponentError::None},
        {"/Game//Hero", 0, LogicalPathError::EmptyComponent, 1, NameComponentError::None},
        {"/Game/Hero/", 0, LogicalPathError::EmptyComponent, 2, NameComponentError::None},
        {"/Game", 0, LogicalPathError::MissingName, 0, NameComponentError::None},
        {"/Engine/Hero", 0, LogicalPathError::UnknownMount, 0, NameComponentError::None},
        {"/Gam\xff/Hero", 0, LogicalPathError::UnknownMount, 0, NameComponentError::None},
        {"/Game/.", 0, LogicalPathError::InvalidComponent, 1, NameComponentError::InvalidFirst},
        {"/Game/..", 0, LogicalPathError::InvalidComponent, 1, NameComponentError::InvalidFirst},
        {"/Game/_Hero", 0, LogicalPathError::InvalidComponent, 1, NameComponentError::InvalidFirst},
        {"/Game/Hero/He ro", 0, LogicalPathError::InvalidComponent, 2,
         NameComponentError::InvalidCharacter},
        {"/Game/He.ro", 0, LogicalPathError::InvalidComponent, 1,
         NameComponentError::InvalidCharacter},
        {"/Game/CON", 0, LogicalPathError::InvalidComponent, 1,
         NameComponentError::ReservedDeviceName},
        {"/Game/com1", 0, LogicalPathError::InvalidComponent, 1,
         NameComponentError::ReservedDeviceName},
        {"/Game/LPT\xc2\xb9", 0, LogicalPathError::InvalidComponent, 1,
         NameComponentError::ReservedDeviceName},
        {"/Game/A\xe2\x80\x8b"
         "B",
         0, LogicalPathError::InvalidComponent, 1, NameComponentError::DefaultIgnorable},
        {"/Game/\xc3", 0, LogicalPathError::InvalidComponent, 1,
         NameComponentError::InvalidCharacter},
        {nul, sizeof(nul), LogicalPathError::InvalidComponent, 1,
         NameComponentError::InvalidCharacter},
        {longComponent, 0, LogicalPathError::InvalidComponent, 1, NameComponentError::TooLong},
        {longTotal, 0, LogicalPathError::TooLong, 0, NameComponentError::None},
        // 첫 문자가 결합 문자(M* — U+0301)면 InvalidFirst다(NFC로도 합쳐질 앞 글자가 없다).
        {"/Game/\xcc\x81"
         "A",
         0, LogicalPathError::InvalidComponent, 1, NameComponentError::InvalidFirst},
        // 겹친 위반: 앞 규칙이 이긴다.
        {"Game\\Hero", 0, LogicalPathError::MissingLeadingSlash, 0, NameComponentError::None},
        {"/Engine//Hero", 0, LogicalPathError::EmptyComponent, 1, NameComponentError::None},
        {"/Engine", 0, LogicalPathError::MissingName, 0, NameComponentError::None},
        {"/Engine/CON", 0, LogicalPathError::UnknownMount, 0, NameComponentError::None},
        {"/Game/CON/_x", 0, LogicalPathError::InvalidComponent, 1,
         NameComponentError::ReservedDeviceName},
        {"/Game\\/", 0, LogicalPathError::Backslash, 0, NameComponentError::None},
        // 규칙 4가 5보다 앞선다: 전체가 256을 넘어도 구성 요소 위반이면 InvalidComponent다.
        {tooLongAndInvalid, 0, LogicalPathError::InvalidComponent, 5,
         NameComponentError::InvalidFirst},
    };
    for (const InvalidCase& c : cases) {
        LogicalPathFailure failure;
        auto parsed = ParseLogicalPath(allocator, InputOf(c), &failure);
        const bool same = !parsed && parsed.Error() == ErrorCode::InvalidArgument &&
                          failure.error == c.error && failure.component == c.component &&
                          failure.componentError == c.componentError;
        if (!same) {
            printf("  input '%s': error %s component %u reason %u\n", c.input,
                   LogicalPathErrorName(failure.error), failure.component,
                   static_cast<unsigned>(failure.componentError));
        }

        CHECK(same);
    }

    // 위반 위치는 NFC로 정규화한 구성 요소 안의 byte offset이다(입력이 NFD면 입력의 위치와 다를 수
    // 있다 — 진단은 정규화한 이름 기준이다).
    LogicalPathFailure failure;
    CHECK(!ParseLogicalPath(allocator, "/Game/Hero/He ro", &failure));
    CHECK(failure.offset == 2);
    CHECK(!ParseLogicalPath(allocator, "/Game/He.ro", &failure));
    CHECK(failure.offset == 2);
    // NFD "e + U+0301"(3 byte)가 NFC "U+00E9"(2 byte)로 줄어 공백 위치가 앞당겨진다.
    CHECK(!ParseLogicalPath(allocator, "/Game/Cafe\xcc\x81 x", &failure));
    CHECK(failure.componentError == NameComponentError::InvalidCharacter && failure.offset == 5);
}

// 경계(4.2절): 구성 요소 64 byte·전체 256 byte는 받고 하나 넘으면 거부한다. 정규화 뒤 길이로 센다
// (NFD 입력이 NFC로 짧아져 상한 안에 들어오면 받는다).
TEST(LogicalPath_LengthBoundaries) {
    SystemAllocator allocator;
    char component64[128];
    RepeatedPath(component64, sizeof(component64), 1, MaxNameComponentBytes, 'c');
    CHECK(ParseLogicalPath(allocator, component64));
    // "/Game"(5) + 3 × (1 + 64) + 1 + 55 = 256.
    char exact[512];
    RepeatedPath(exact, sizeof(exact), 3, MaxNameComponentBytes, 'd');
    usize at = strlen(exact);
    exact[at++] = '/';
    for (usize index = 0; index < 55; ++index) {
        exact[at++] = 'e';
    }

    exact[at] = 0;
    REQUIRE(strlen(exact) == MaxLogicalPathBytes);
    CHECK(ParseLogicalPath(allocator, exact));
    exact[at++] = 'e';
    exact[at] = 0;
    LogicalPathFailure failure;
    CHECK(!ParseLogicalPath(allocator, exact, &failure));
    CHECK(failure.error == LogicalPathError::TooLong);
    // NFD 입력(e + U+0301, 3 byte)이 NFC(U+00E9, 2 byte)로 줄어 상한 안에 든다. 구성 요소 하나는
    // NFD 63 byte → NFC 42 byte다.
    char nfd[512] = "/Game/";
    usize nfdAt = strlen(nfd);
    for (usize index = 0; index < 21; ++index) {
        nfd[nfdAt++] = 'e';
        nfd[nfdAt++] = static_cast<char>(0xcc);
        nfd[nfdAt++] = static_cast<char>(0x81);
    }

    nfd[nfdAt] = 0;
    auto parsed = ParseLogicalPath(allocator, nfd);
    REQUIRE(parsed);
    CHECK(parsed.Value().Text().ByteCount() == 6 + 42);
    // 조합 제외 문자 U+0958(3 byte)은 NFC에서 U+0915 U+093C(6 byte)로 길어진다. 입력 63 byte 구성
    // 요소가 NFC 126 byte가 되어 구성 요소 TooLong이다.
    char grow[512] = "/Game/";
    usize growAt = strlen(grow);
    for (usize index = 0; index < 21; ++index) {
        grow[growAt++] = static_cast<char>(0xe0);
        grow[growAt++] = static_cast<char>(0xa5);
        grow[growAt++] = static_cast<char>(0x98);
    }

    grow[growAt] = 0;
    LogicalPathFailure growFailure;
    CHECK(!ParseLogicalPath(allocator, grow, &growFailure));
    CHECK(growFailure.error == LogicalPathError::InvalidComponent &&
          growFailure.componentError == NameComponentError::TooLong);
    // 구성 요소 다섯(각 10 × U+0958 = 입력 30 byte → NFC 60 byte): 입력 160 byte가 NFC 310 byte로
    // 전체 TooLong이다. 넷이면 NFC 249 byte로 받는다.
    char total[512] = "/Game";
    usize totalAt = strlen(total);
    for (usize component = 0; component < 5; ++component) {
        total[totalAt++] = '/';
        for (usize index = 0; index < 10; ++index) {
            total[totalAt++] = static_cast<char>(0xe0);
            total[totalAt++] = static_cast<char>(0xa5);
            total[totalAt++] = static_cast<char>(0x98);
        }

        total[totalAt] = 0;
        LogicalPathFailure totalFailure;
        auto grown = ParseLogicalPath(allocator, total, &totalFailure);
        if (component < 4) {
            CHECK(grown);
        } else {
            CHECK(!grown && totalFailure.error == LogicalPathError::TooLong);
        }
    }
}

// 접근자·복사·이름(7.1절 10): TryClone은 같은 경로, 사유 이름은 모두 고유하고 범위 밖은 "Unknown".
TEST(LogicalPath_CloneAndNames) {
    SystemAllocator allocator;
    auto parsed = ParseLogicalPath(allocator, "/Game/A/B/C");
    REQUIRE(parsed);
    auto copy = parsed.Value().TryClone();
    REQUIRE(copy);
    CHECK(copy.Value().Text() == "/Game/A/B/C" && copy.Value() == parsed.Value());
    CHECK(copy.Value().Directory() == "/Game/A/B" && copy.Value().Relative() == "A/B/C");
    constexpr uint32 Last = static_cast<uint32>(LogicalPathError::NotNfcOnDisk);
    for (uint32 value = 0; value <= Last; ++value) {
        const StringView name(LogicalPathErrorName(static_cast<LogicalPathError>(value)));
        CHECK(name != "Unknown");
        for (uint32 other = 0; other < value; ++other) {
            CHECK(name != StringView(LogicalPathErrorName(static_cast<LogicalPathError>(other))));
        }
    }

    CHECK(StringView(LogicalPathErrorName(static_cast<LogicalPathError>(Last + 1))) == "Unknown");
    // 옮겨 간 값은 비어 있고 접근자는 빈 view를 준다(범위 밖을 읽지 않는다).
    LogicalPath moved = Move(copy.Value());
    CHECK(moved.Text() == "/Game/A/B/C");
    CHECK(copy.Value().Text().IsEmpty() && copy.Value().Key().IsEmpty());
    CHECK(copy.Value().Name().IsEmpty() && copy.Value().Relative().IsEmpty());
    CHECK(copy.Value().Directory().IsEmpty() && copy.Value().Mount().IsEmpty());
    // 옮겨 간 값의 복사도 빈 값이다.
    auto emptyCopy = copy.Value().TryClone();
    REQUIRE(emptyCopy);
    CHECK(emptyCopy.Value().Text().IsEmpty() && emptyCopy.Value().Mount().IsEmpty());
}

namespace {

// text를 모든 할당 지점에서 한 번씩 실패시키며 파싱한다. 실패하면 OutOfMemory·사유 None이고, 실패가
// 없는 마지막 실행은 기준 결과(expectedError가 None이면 성공 — 같은 Text·Key, 아니면 그 사유)와
// 같다. 실패 주입 횟수를 돌려준다.
uint32 SweepParse(const char* text, LogicalPathError expectedError) noexcept {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    auto expected = ParseLogicalPath(system, text);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        failing.FailOnAttempt(attempt);
        LogicalPathFailure failure;
        failure.error = LogicalPathError::Empty;
        auto parsed = ParseLogicalPath(failing, text, &failure);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!failed) {
            if (expectedError == LogicalPathError::None) {
                CHECK(parsed && expected);
                CHECK(parsed.Value().Text() == expected.Value().Text());
                CHECK(parsed.Value().Key() == expected.Value().Key());
                if (parsed) {
                    // 복사의 모든 할당 지점도 실패시킨다(복사는 원본의 allocator를 쓴다).
                    for (uint64 copyAttempt = 1;; ++copyAttempt) {
                        failing.FailOnAttempt(copyAttempt);
                        auto copy = parsed.Value().TryClone();
                        const bool copyFailed = failing.Attempts() >= copyAttempt;
                        failing.DisableFailure();
                        if (!copyFailed) {
                            CHECK(copy && copy.Value() == parsed.Value());
                            break;
                        }

                        CHECK(!copy && copy.Error() == ErrorCode::OutOfMemory);
                        ++failures;
                    }
                }
            } else {
                CHECK(!parsed && failure.error == expectedError);
            }

            break;
        }

        CHECK(!parsed && parsed.Error() == ErrorCode::OutOfMemory);
        CHECK(failure.error == LogicalPathError::None);
        ++failures;
    }

    return failures;
}

} // namespace

// 할당 실패(7.1절 10): 모든 할당 지점에서 한 번 실패하면 그 오류이고 사유는 None이며, 실패가 없으면
// 같은 결과다. 입력은 셋이다: 비ASCII 긴 경로(키·정규화 저장소가 인라인 크기를 넘어 자란다), 상한을
// 넘는 긴 경로(TooLong 판정 전에 붙이기 저장소가 자란다), NFC 뒤에도 빠른 검사가 애매한 결합 문자
// (q + U+0301 — 이름 규칙 검사가 정규화 임시 저장소를 쓴다).
TEST(LogicalPath_AllocationFailures) {
    char text[512] = "/Game/";
    usize at = strlen(text);
    for (usize part = 0; part < 4; ++part) {
        for (usize index = 0; index < 20; ++index) {
            text[at++] = static_cast<char>(0xc3);
            text[at++] = static_cast<char>(0x84);
        }

        text[at++] = '/';
    }

    text[at++] = 'Z';
    text[at] = 0;
    CHECK(SweepParse(text, LogicalPathError::None) > 0);
    char tooLong[1024];
    RepeatedPath(tooLong, sizeof(tooLong), 9, MaxNameComponentBytes, 'f');
    CHECK(SweepParse(tooLong, LogicalPathError::TooLong) > 0);
    CHECK(SweepParse("/Game/Aq\xcc\x81", LogicalPathError::None) > 0);
}

} // namespace logical_path_tests_file
