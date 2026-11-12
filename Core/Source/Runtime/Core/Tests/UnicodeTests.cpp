#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/Unicode.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;

// Unicode 정규화·case folding·속성 표를 UCD 18.0.0 원본 전체와 대조한다. 원본은 저장소의
// Core/Source/ThirdParty/UnicodeData에서 읽는다(시험은 저장소 루트를 작업 디렉터리로 실행된다).
namespace {

constexpr char UcdDirectory[] = "Core/Source/ThirdParty/UnicodeData/ucd-18.0.0/";
constexpr uint32 CodePointLimit = 0x110000;

bool ReadUcd(const char* name, Array<char>& out) noexcept {
    char path[256];
    snprintf(path, sizeof(path), "%s%s", UcdDirectory, name);
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path, platform::CStringLength(path),
                   platform::FileOpenMode::ReadExisting)) {
        printf("cannot open %s (run tests from the repository root)\n", path);
        return false;
    }
    uint64 size = 0;
    if (!file.Size(size) || !out.TryResize(static_cast<usize>(size))) {
        return false;
    }
    const auto read = file.ReadExact(out.Data(), out.Size());
    return read.status && static_cast<bool>(file.Close());
}

// UCD 텍스트를 줄 단위로 넘긴다. 주석(#)과 앞뒤 공백을 뺀 내용이 빈 줄은 건너뛴다.
template <typename Visitor>
void ForEachLine(const Array<char>& text, Visitor&& visitor) noexcept {
    usize start = 0;
    const usize size = text.Size();
    while (start < size) {
        usize end = start;
        while (end < size && text[end] != '\n') {
            ++end;
        }
        usize stop = start;
        while (stop < end && text[stop] != '#') {
            ++stop;
        }
        while (stop > start && (text[stop - 1] == ' ' || text[stop - 1] == '\r')) {
            --stop;
        }
        if (stop > start) {
            visitor(StringView(text.Data() + start, stop - start),
                    StringView(text.Data() + start, end - start));
        }
        start = end + 1;
    }
}

// ';'로 나눈 field 번호 index를 앞뒤 공백 없이 반환한다.
StringView Field(StringView line, usize index) noexcept {
    usize field = 0;
    usize begin = 0;
    for (usize position = 0; position <= line.ByteCount(); ++position) {
        if (position == line.ByteCount() || line.Data()[position] == ';') {
            if (field == index) {
                usize from = begin;
                usize to = position;
                while (from < to && line.Data()[from] == ' ') {
                    ++from;
                }
                while (to > from && line.Data()[to - 1] == ' ') {
                    --to;
                }
                return StringView(line.Data() + from, to - from);
            }
            ++field;
            begin = position + 1;
        }
    }
    return StringView();
}

uint32 ParseHex(StringView text, usize& position) noexcept {
    uint32 value = 0;
    while (position < text.ByteCount() && text.Data()[position] == ' ') {
        ++position;
    }
    while (position < text.ByteCount()) {
        const char c = text.Data()[position];
        uint32 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<uint32>(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<uint32>(c - 'A' + 10);
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<uint32>(c - 'a' + 10);
        } else {
            break;
        }
        value = value * 16 + digit;
        ++position;
    }
    return value;
}

// "XXXX..YYYY" 또는 "XXXX"를 읽는다.
void ParseRange(StringView text, uint32& first, uint32& last) noexcept {
    usize position = 0;
    first = ParseHex(text, position);
    last = first;
    if (position + 1 < text.ByteCount() && text.Data()[position] == '.') {
        position += 2;
        last = ParseHex(text, position);
    }
}

// 공백으로 나눈 16진 코드 포인트 열을 UTF-8로 붙인다.
bool AppendSequence(StringView text, StringBuilderBase& out) noexcept {
    usize position = 0;
    while (position < text.ByteCount()) {
        while (position < text.ByteCount() && text.Data()[position] == ' ') {
            ++position;
        }
        if (position == text.ByteCount()) {
            break;
        }
        if (!out.TryAppendCodePoint(ParseHex(text, position))) {
            return false;
        }
    }
    return true;
}

bool IsScalar(uint32 point) noexcept {
    return point < CodePointLimit && (point < 0xD800 || point > 0xDFFF);
}

bool Normalized(NormalizationForm form, StringView input, StringBuilderBase& out) noexcept {
    out.Clear();
    return static_cast<bool>(TryNormalize(form, input, out));
}

bool Key(StringView input, StringBuilderBase& out) noexcept {
    out.Clear();
    return static_cast<bool>(TryMakeCaselessKey(input, out));
}

} // namespace

TEST(Unicode_VersionAndHangulAlgorithm) {
    CHECK(UnicodeVersionText() == StringView("18.0.0"));
    SystemAllocator allocator;
    StringBuilder<64> out(allocator);
    // U+1100 U+1161 U+11A8 → U+AC01, 그 역.
    REQUIRE(Normalized(NormalizationForm::NFC, "\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8", out));
    CHECK(out.View() == StringView("\xEA\xB0\x81"));
    REQUIRE(Normalized(NormalizationForm::NFD, "\xEA\xB0\x81", out));
    CHECK(out.View() == StringView("\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8"));
    // LV 음절 + T는 LVT로 합성된다(U+AC00 + U+11A8 → U+AC01).
    REQUIRE(Normalized(NormalizationForm::NFC, "\xEA\xB0\x80\xE1\x86\xA8", out));
    CHECK(out.View() == StringView("\xEA\xB0\x81"));
}

TEST(Unicode_NormalizationTestConformance) {
    SystemAllocator allocator;
    Array<char> text(allocator);
    REQUIRE(ReadUcd("NormalizationTest.txt", text));
    Array<uint8> inPart1(allocator);
    REQUIRE(inPart1.TryResize(CodePointLimit));
    platform::MemoryZero(inPart1.Data(), inPart1.Size());

    StringBuilder<128> columns[5] = {StringBuilder<128>(allocator), StringBuilder<128>(allocator),
                                     StringBuilder<128>(allocator), StringBuilder<128>(allocator),
                                     StringBuilder<128>(allocator)};
    StringBuilder<128> result(allocator);
    StringBuilder<128> keyFirst(allocator);
    StringBuilder<128> keySecond(allocator);
    usize lines = 0;
    usize failures = 0;
    bool part1 = false;
    ForEachLine(text, [&](StringView line, StringView) noexcept {
        if (line.Data()[0] == '@') {
            part1 = line == StringView("@Part1");
            return;
        }
        for (usize column = 0; column < 5; ++column) {
            columns[column].Clear();
            if (!AppendSequence(Field(line, column), columns[column])) {
                ++failures;
                return;
            }
        }
        if (part1) {
            usize position = 0;
            const StringView single = Field(line, 0);
            const uint32 point = ParseHex(single, position);
            if (position == single.ByteCount() && point < CodePointLimit) {
                inPart1[point] = 1;
            }
        }
        ++lines;
        // UAX #15 NormalizationTest의 등식: NFC 열 c2, c4 / NFD 열 c3, c5.
        const int nfc[5] = {1, 1, 1, 3, 3};
        const int nfd[5] = {2, 2, 2, 4, 4};
        for (usize column = 0; column < 5; ++column) {
            if (!Normalized(NormalizationForm::NFC, columns[column].View(), result) ||
                result.View() != columns[nfc[column]].View()) {
                if (failures++ < 5) {
                    printf("NFC mismatch: %.*s (column %d)\n", static_cast<int>(line.ByteCount()),
                           line.Data(), static_cast<int>(column + 1));
                }
            }
            if (!Normalized(NormalizationForm::NFD, columns[column].View(), result) ||
                result.View() != columns[nfd[column]].View()) {
                if (failures++ < 5) {
                    printf("NFD mismatch: %.*s (column %d)\n", static_cast<int>(line.ByteCount()),
                           line.Data(), static_cast<int>(column + 1));
                }
            }
            // IsNormalized는 (정규화 결과 == 입력)과 같아야 한다.
            const NormalizationForm forms[] = {NormalizationForm::NFC, NormalizationForm::NFD};
            for (NormalizationForm form : forms) {
                const auto quick = IsNormalized(form, columns[column].View(), allocator);
                const bool expected = Normalized(form, columns[column].View(), result) &&
                                      result.View() == columns[column].View();
                if (!quick || quick.Value() != expected) {
                    if (failures++ < 5) {
                        printf("IsNormalized mismatch: %.*s\n", static_cast<int>(line.ByteCount()),
                               line.Data());
                    }
                }
            }
            // 결합 문자열에서도 caseless 키는 멱등이다.
            if (!Key(columns[column].View(), keyFirst) || !Key(keyFirst.View(), keySecond) ||
                keyFirst.View() != keySecond.View()) {
                if (failures++ < 5) {
                    printf("caseless key not idempotent: %.*s\n",
                           static_cast<int>(line.ByteCount()), line.Data());
                }
            }
        }
    });
    CHECK(lines > 19000);
    CHECK(failures == 0);

    // Part 1에 없는 모든 scalar X는 NFC(X) = NFD(X) = X다.
    usize identity = 0;
    StringBuilder<16> single(allocator);
    for (uint32 point = 0; point < CodePointLimit; ++point) {
        if (!IsScalar(point) || inPart1[point] != 0) {
            continue;
        }
        single.Clear();
        REQUIRE(single.TryAppendCodePoint(point));
        if (!Normalized(NormalizationForm::NFC, single.View(), result) ||
            result.View() != single.View() ||
            !Normalized(NormalizationForm::NFD, single.View(), result) ||
            result.View() != single.View()) {
            if (identity++ < 5) {
                printf("identity violated at U+%04X\n", point);
            }
        }
    }
    CHECK(identity == 0);
}

TEST(Unicode_SimpleCaseFoldMatchesCaseFolding) {
    SystemAllocator allocator;
    Array<char> text(allocator);
    REQUIRE(ReadUcd("CaseFolding.txt", text));
    Array<uint32> expected(allocator);
    REQUIRE(expected.TryResize(CodePointLimit));
    for (uint32 point = 0; point < CodePointLimit; ++point) {
        expected[point] = point;
    }
    ForEachLine(text, [&](StringView line, StringView) noexcept {
        const StringView status = Field(line, 1);
        if (status == StringView("C") || status == StringView("S")) {
            usize position = 0;
            const uint32 from = ParseHex(Field(line, 0), position);
            position = 0;
            expected[from] = ParseHex(Field(line, 2), position);
        }
    });
    usize mismatches = 0;
    for (uint32 point = 0; point < CodePointLimit; ++point) {
        if (static_cast<uint32>(SimpleCaseFold(point)) != expected[point] && mismatches++ < 5) {
            printf("fold mismatch at U+%04X\n", point);
        }
    }
    CHECK(mismatches == 0);
    StringBuilder<64> out(allocator);
    REQUIRE(TrySimpleCaseFold("ABC\xC3\x85\xC8\xBA",
                              out)); // ABCÅȺ → abcåⱥ(U+023A → U+2C65, 2 → 3 byte)
    CHECK(out.View() == StringView("abc\xC3\xA5\xE2\xB1\xA5"));
}

TEST(Unicode_PropertiesMatchUnicodeData) {
    SystemAllocator allocator;
    Array<char> text(allocator);
    REQUIRE(ReadUcd("UnicodeData.txt", text));
    static const char* const names[] = {"Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl",
                                        "No", "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc",
                                        "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn"};
    Array<uint8> category(allocator);
    Array<uint8> combining(allocator);
    REQUIRE(category.TryResize(CodePointLimit));
    REQUIRE(combining.TryResize(CodePointLimit));
    platform::MemorySet(category.Data(), static_cast<uint8>(GeneralCategory::Cn), category.Size());
    platform::MemoryZero(combining.Data(), combining.Size());
    uint32 rangeFirst = 0;
    ForEachLine(text, [&](StringView line, StringView) noexcept {
        usize position = 0;
        const uint32 point = ParseHex(Field(line, 0), position);
        const StringView name = Field(line, 1);
        const StringView categoryName = Field(line, 2);
        uint8 index = 0;
        while (index < 30 && categoryName != StringView(names[index])) {
            ++index;
        }
        uint32 decimal = 0; // 결합 등급은 10진이다
        for (usize at = 0; at < Field(line, 3).ByteCount(); ++at) {
            decimal = decimal * 10 + static_cast<uint32>(Field(line, 3).Data()[at] - '0');
        }
        const bool first = name.ByteCount() > 8 && StringView(name.Data() + name.ByteCount() - 8,
                                                              8) == StringView(", First>");
        const bool last = name.ByteCount() > 7 && StringView(name.Data() + name.ByteCount() - 7,
                                                             7) == StringView(", Last>");
        if (first) {
            rangeFirst = point;
            return;
        }
        for (uint32 code = last ? rangeFirst : point; code <= point; ++code) {
            category[code] = index;
            combining[code] = static_cast<uint8>(decimal);
        }
    });
    usize mismatches = 0;
    for (uint32 point = 0; point < CodePointLimit; ++point) {
        if ((static_cast<uint8>(GetGeneralCategory(point)) != category[point] ||
             GetCanonicalCombiningClass(point) != combining[point]) &&
            mismatches++ < 5) {
            printf("property mismatch at U+%04X\n", point);
        }
    }
    CHECK(mismatches == 0);
    CHECK(GetGeneralCategory(0x110000) == GeneralCategory::Cn);
}

TEST(Unicode_DefaultIgnorableMatchesDerivedCoreProperties) {
    SystemAllocator allocator;
    Array<char> text(allocator);
    REQUIRE(ReadUcd("DerivedCoreProperties.txt", text));
    Array<uint8> ignorable(allocator);
    REQUIRE(ignorable.TryResize(CodePointLimit));
    platform::MemoryZero(ignorable.Data(), ignorable.Size());
    ForEachLine(text, [&](StringView line, StringView) noexcept {
        if (Field(line, 1) != StringView("Default_Ignorable_Code_Point")) {
            return;
        }
        uint32 first = 0;
        uint32 last = 0;
        ParseRange(Field(line, 0), first, last);
        for (uint32 point = first; point <= last; ++point) {
            ignorable[point] = 1;
        }
    });
    usize mismatches = 0;
    for (uint32 point = 0; point < CodePointLimit; ++point) {
        if (IsDefaultIgnorable(point) != (ignorable[point] != 0) && mismatches++ < 5) {
            printf("Default_Ignorable mismatch at U+%04X\n", point);
        }
    }
    CHECK(mismatches == 0);
    CHECK(IsDefaultIgnorable(0x3164) && IsDefaultIgnorable(0xFE0F) && !IsDefaultIgnorable('A'));
}

TEST(Unicode_CaselessKeyIsIdempotentAndCaseless) {
    SystemAllocator allocator;
    StringBuilder<64> a(allocator);
    StringBuilder<64> b(allocator);
    StringBuilder<16> single(allocator);
    // 모든 scalar 하나짜리 문자열에서 키의 키는 같은 키다.
    usize failures = 0;
    for (uint32 point = 0; point < CodePointLimit; ++point) {
        if (!IsScalar(point)) {
            continue;
        }
        single.Clear();
        REQUIRE(single.TryAppendCodePoint(point));
        if (!Key(single.View(), a) || !Key(a.View(), b) || a.View() != b.View()) {
            if (failures++ < 5) {
                printf("caseless key not idempotent at U+%04X\n", point);
            }
        }
    }
    CHECK(failures == 0);

    auto same = [&](StringView left, StringView right) noexcept {
        return Key(left, a) && Key(right, b) && a.View() == b.View();
    };
    // U+0130 = NFC("İ")은 NFD를 먼저 해야 "i̇"과 같은 키가 된다.
    CHECK(same("\xC4\xB0", "I\xCC\x87"));
    CHECK(same("\xC4\xB0", "i\xCC\x87"));
    // ypogegrammeni: U+1F80(ᾀ)과 U+1F00 U+03B9(ἀι).
    CHECK(same("\xE1\xBE\x80", "\xE1\xBC\x80\xCE\xB9"));
    // Å(U+00C5) = A + U+030A = å.
    CHECK(same("\xC3\x85", "A\xCC\x8A"));
    CHECK(same("\xC3\x85", "\xC3\xA5"));
    // 단순 folding은 ß를 ss로 바꾸지 않는다.
    CHECK(!same("Stra\xC3\x9F"
                "e",
                "STRASSE"));
    CHECK(same("Hello", "hELLO"));
}

TEST(Unicode_InvalidInputLeavesOutputUnchanged) {
    SystemAllocator allocator;
    StringBuilder<64> out(allocator);
    REQUIRE(out.TryAppend("keep"));
    const StringView invalids[] = {StringView("\xC0\xAF"), StringView("\xED\xA0\x80"),
                                   StringView("\xE2\x82"), StringView("\xF4\x90\x80\x80")};
    for (StringView invalid : invalids) {
        CHECK(TryNormalize(NormalizationForm::NFC, invalid, out).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TryNormalize(NormalizationForm::NFD, invalid, out).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(TrySimpleCaseFold(invalid, out).Error() == ErrorCode::InvalidArgument);
        CHECK(TryMakeCaselessKey(invalid, out).Error() == ErrorCode::InvalidArgument);
        CHECK(IsNormalized(NormalizationForm::NFC, invalid, allocator).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(out.View() == StringView("keep"));
    }
    // 결과는 out 뒤에 붙는다.
    REQUIRE(TryNormalize(NormalizationForm::NFC, "A\xCC\x8A", out));
    CHECK(out.View() == StringView("keep\xC3\x85"));
}
