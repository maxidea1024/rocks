#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NameComponent.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Unicode.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

// 이름 구성 요소 규칙(Docs/Design/AssetSystem.ko.md 4.2절, Docs/Design/ObjectFoundations.ko.md 1.1·
// 1.2절)과 비교 키의 ASCII 빠른 경로를 검증한다.
using namespace rocks;

namespace {

struct RejectCase {
    const char* text;
    usize bytes;
    NameComponentError reason;
    usize offset;
};

StringView View(const char* text) noexcept {
    usize bytes = 0;
    while (text[bytes] != '\0') {
        ++bytes;
    }

    return StringView(text, bytes);
}

} // namespace

TEST(NameComponent_AcceptsLettersNumbersMarksUnderscoreAndHyphen) {
    SystemAllocator allocator;
    const char* accepted[] = {"a",
                              "Body",
                              "1abc",
                              "a_b-c",
                              "\xED\x95\x9C\xEA\xB8\x80", // 한글
                              "q\xCC\x87",                // q + U+0307(합성형이 없는 결합 문자)
                              "Straße",
                              "COM10",
                              "CONX",
                              "LPT",
                              "COM\xC2\xB9x", // COM¹x(5 byte를 넘어 예약 이름이 아니다)
                              "\xD9\xA3x"};   // U+0663(Nd) 시작
    for (const char* text : accepted) {
        NameComponentError reason = NameComponentError::Empty;
        usize offset = 99;
        CHECK(ValidateNameComponent(View(text), allocator, &reason, &offset));
        CHECK(reason == NameComponentError::None);
        CHECK(offset == 0);
    }

    // 64 byte 경계(ASCII와 3 byte 문자 21개 + 1 byte).
    char longest[MaxNameComponentBytes];
    for (char& character : longest) {
        character = 'a';
    }

    CHECK(ValidateNameComponent(StringView(longest, sizeof(longest)), allocator));
    char wide[MaxNameComponentBytes];
    for (usize index = 0; index < 21; ++index) {
        wide[index * 3] = static_cast<char>(0xED);
        wide[index * 3 + 1] = static_cast<char>(0x95);
        wide[index * 3 + 2] = static_cast<char>(0x9C);
    }

    wide[63] = 'z';
    CHECK(ValidateNameComponent(StringView(wide, sizeof(wide)), allocator));
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(NameComponent_RejectsWithReasonAndOffset) {
    SystemAllocator allocator;
    char tooLong[MaxNameComponentBytes + 1];
    for (char& character : tooLong) {
        character = 'a';
    }

    const RejectCase cases[] = {
        {"", 0, NameComponentError::Empty, 0},
        {tooLong, sizeof(tooLong), NameComponentError::TooLong, MaxNameComponentBytes},
        {"A\xCC\x8A", 3, NameComponentError::NotNfc, 0},   // A + U+030A(분해형 Å)
        {"abe\xCC\x81", 5, NameComponentError::NotNfc, 2}, // e + U+0301
        {"_a", 2, NameComponentError::InvalidFirst, 0},
        {"-a", 2, NameComponentError::InvalidFirst, 0},
        {"\xCC\x81z", 3, NameComponentError::InvalidFirst, 0}, // M* 시작
        {"a b", 3, NameComponentError::InvalidCharacter, 1},
        {"a.b", 3, NameComponentError::InvalidCharacter, 1},
        {"a/b", 3, NameComponentError::InvalidCharacter, 1},
        {"ab\xFF", 3, NameComponentError::InvalidCharacter, 2},         // 잘못된 UTF-8
        {"a\xC3", 2, NameComponentError::InvalidCharacter, 1},          // 잘린 sequence
        {"a\xE2\x80\x8Bz", 5, NameComponentError::DefaultIgnorable, 1}, // U+200B
        {"a\xEF\xB8\x8F", 4, NameComponentError::DefaultIgnorable, 1},  // U+FE0F(Mn)
        {"\xE2\x80\x8Bz", 4, NameComponentError::DefaultIgnorable, 0},
        {"CON", 3, NameComponentError::ReservedDeviceName, 0},
        {"Nul", 3, NameComponentError::ReservedDeviceName, 0},
        {"com1", 4, NameComponentError::ReservedDeviceName, 0},
        {"COM0", 4, NameComponentError::ReservedDeviceName, 0},
        {"lpt9", 4, NameComponentError::ReservedDeviceName, 0},
        {"LPT\xC2\xB9", 5, NameComponentError::ReservedDeviceName, 0}, // LPT¹
        {"com\xC2\xB3", 5, NameComponentError::ReservedDeviceName, 0}, // com³
    };
    for (const RejectCase& entry : cases) {
        NameComponentError reason = NameComponentError::None;
        usize offset = 99;
        auto result =
            ValidateNameComponent(StringView(entry.text, entry.bytes), allocator, &reason, &offset);
        CHECK(!result && result.Error() == ErrorCode::InvalidArgument);
        CHECK(reason == entry.reason);
        CHECK(offset == entry.offset);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// NFC 빠른 검사로 판정되는 이름(ASCII·한글)은 할당하지 않고, 애매한 결합 문자는 scratch를 쓴다.
// 그 할당 실패는 OutOfMemory이고 reason은 None이다.
TEST(NameComponent_NfcScratchAllocationFailureIsOutOfMemory) {
    SystemAllocator system;
    rocks::test::NFailAllocator allocator(system);
    allocator.FailOnAttempt(1);
    CHECK(ValidateNameComponent(View("Body_01"), allocator));
    CHECK(ValidateNameComponent(View("\xED\x95\x9C\xEA\xB8\x80"), allocator));
    CHECK(allocator.Attempts() == 0);

    NameComponentError reason = NameComponentError::Empty;
    auto failed = ValidateNameComponent(View("q\xCC\x87"), allocator, &reason);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(reason == NameComponentError::None);

    // 분해형 이름은 판정·위치 계산의 어느 할당이 실패해도 OutOfMemory이고, 할당이 모두 되면
    // NotNfc다.
    bool reachedVerdict = false;
    for (uint64 attempt = 1; attempt < 64 && !reachedVerdict; ++attempt) {
        allocator.FailOnAttempt(attempt);
        usize offset = 99;
        auto result = ValidateNameComponent(View("A\xCC\x8A"), allocator, &reason, &offset);
        REQUIRE(!result);
        if (result.Error() == ErrorCode::InvalidArgument) {
            CHECK(reason == NameComponentError::NotNfc);
            CHECK(offset == 0);
            reachedVerdict = true;
        } else {
            CHECK(result.Error() == ErrorCode::OutOfMemory);
        }
    }

    CHECK(reachedVerdict);
    // 위치를 묻지 않으면 위치 계산용 정규화를 하지 않는다(판정 할당만 있다).
    allocator.DisableFailure();
    CHECK(ValidateNameComponent(View("A\xCC\x8A"), allocator, &reason).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(reason == NameComponentError::NotNfc);
    const uint64 verdictOnly = allocator.Attempts();
    allocator.DisableFailure();
    usize position = 99;
    CHECK(ValidateNameComponent(View("A\xCC\x8A"), allocator, &reason, &position).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(position == 0);
    CHECK(verdictOnly > 0 && verdictOnly < allocator.Attempts());
    allocator.DisableFailure();
    CHECK(ValidateNameComponent(View("q\xCC\x87"), allocator));
    CHECK(system.Stats().liveAllocations == 0);
}

// ASCII 이름의 키는 ASCII 소문자화로 할당 없이 만들고 TryMakeCaselessKey와 같다.
TEST(NameComponentKey_AsciiFastPathMatchesCaselessKeyWithoutAllocation) {
    SystemAllocator system;
    rocks::test::NFailAllocator failing(system);
    failing.FailOnAttempt(1);
    char ascii[96];
    usize count = 0;
    for (int character = 0x20; character < 0x7F; ++character) {
        ascii[count++] = static_cast<char>(character);
    }

    // 32 byte 조각마다 비교한다(인라인 buffer 안).
    for (usize start = 0; start < count; start += 32) {
        const usize bytes = count - start < 32 ? count - start : 32;
        const StringView piece(ascii + start, bytes);
        StringBuilder<128> fast(failing);
        REQUIRE(TryMakeNameComponentKey(piece, fast));
        StringBuilder<128> general(system);
        REQUIRE(TryMakeCaselessKey(piece, general));
        CHECK(fast.View() == general.View());
    }

    CHECK(failing.Attempts() == 0);
    StringBuilder<128> body(failing);
    REQUIRE(TryMakeNameComponentKey(View("BoDy_X-9"), body));
    CHECK(body.View() == View("body_x-9"));
    CHECK(failing.Attempts() == 0);
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(NameComponentKey_NonAsciiUsesCaselessKeyAndKeepsOutputOnFailure) {
    SystemAllocator system;
    // Å(U+00C5)와 å(U+00E5)는 같은 키, Straße와 STRASSE는 다른 키(단순 folding).
    StringBuilder<128> upper(system);
    StringBuilder<128> lower(system);
    REQUIRE(TryMakeNameComponentKey(View("\xC3\x85x"), upper));
    REQUIRE(TryMakeNameComponentKey(View("\xC3\xA5X"), lower));
    CHECK(upper.View() == lower.View());
    StringBuilder<128> sharp(system);
    StringBuilder<128> doubled(system);
    REQUIRE(TryMakeNameComponentKey(View("Straße"), sharp));
    REQUIRE(TryMakeNameComponentKey(View("STRASSE"), doubled));
    CHECK(!(sharp.View() == doubled.View()));

    // 기존 내용 뒤에 붙이고, 실패하면 그대로 둔다.
    rocks::test::NFailAllocator failing(system);
    StringBuilder<128> out(failing);
    REQUIRE(out.TryAppend(View("x:")));
    REQUIRE(TryMakeNameComponentKey(View("Ab"), out));
    CHECK(out.View() == View("x:ab"));
    failing.FailOnAttempt(1);
    auto failed = TryMakeNameComponentKey(View("\xC3\x85"), out);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(out.View() == View("x:ab"));
    failing.DisableFailure();
    CHECK(system.Stats().liveAllocations == 0);
}
