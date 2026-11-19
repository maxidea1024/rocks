#include <Core/Text/EditDistance.h>
#include <TestSupport/Test.h>

// 한도 안 OSA 편집 거리 시험이다(Core/Text/EditDistance.h): 알려진 거리, 빈 텍스트, 대칭, 인접
// 맞바꾸기, ASCII 대소문자 무시, 한도를 넘으면 한도 + 1, 한도가 최댓값보다 크면 줄여 계산, 전체
// 행렬 기준과의 전수 대조.
using namespace rocks;
namespace edit_distance_tests_file {

namespace {

constexpr EditDistanceCase Sensitive = EditDistanceCase::Sensitive;
constexpr EditDistanceCase Insensitive = EditDistanceCase::AsciiInsensitive;

} // namespace

TEST(EditDistance_KnownDistancesWithinLimit) {
    CHECK(EditDistance("kitten", "sitting", 3, Sensitive) == 3);
    CHECK(EditDistance("sitting", "kitten", 3, Sensitive) == 3);
    CHECK(EditDistance("flaw", "lawn", 3, Sensitive) == 2);
    CHECK(EditDistance("render.shadow.distanse", "render.shadow.distance", 3, Sensitive) == 1);
    CHECK(EditDistance("render.shadow.distance", "render.shadow.distance", 3, Sensitive) == 0);
    CHECK(EditDistance("", "abc", 3, Sensitive) == 3);
    CHECK(EditDistance("abc", "", 3, Sensitive) == 3);
    CHECK(EditDistance("", "", 0, Sensitive) == 0);
    CHECK(EditDistance("Abc", "abc", 1, Sensitive) == 1);
}

// 인접 두 글자 맞바꾸기는 1이다(OSA — 맞바꾼 글자를 다시 고치지 않으므로 "ca"→"abc"는 3).
TEST(EditDistance_AdjacentTranspositionCountsOnce) {
    CHECK(EditDistance("ab", "ba", 1, Sensitive) == 1);
    CHECK(EditDistance("ab", "ba", 0, Sensitive) == 1);
    CHECK(EditDistance("render.shadow.distnace", "render.shadow.distance", 3, Sensitive) == 1);
    CHECK(EditDistance("abcd", "badc", 3, Sensitive) == 2);
    CHECK(EditDistance("ca", "abc", 3, Sensitive) == 3);
}

TEST(EditDistance_AsciiCaseInsensitiveComparison) {
    CHECK(EditDistance("Abc", "abc", 1, Insensitive) == 0);
    CHECK(EditDistance("--VERBOSE", "--verbose", 2, Insensitive) == 0);
    CHECK(EditDistance("Verbsoe", "verbose", 2, Insensitive) == 1);
    CHECK(EditDistance("Ab", "bA", 2, Insensitive) == 1);
    // ASCII 밖 byte는 그대로 비교한다(UTF-8 "É"와 "é"는 다르다).
    CHECK(EditDistance("\xC3\x89", "\xC3\xA9", 2, Insensitive) == 1);
    CHECK(EditDistance("[", "{", 1, Insensitive) == 1);
}

TEST(EditDistance_ReportsLimitPlusOneBeyondTheLimit) {
    CHECK(EditDistance("kitten", "sitting", 2, Sensitive) == 3);
    CHECK(EditDistance("kitten", "sitting", 0, Sensitive) == 1);
    CHECK(EditDistance("a", "abcdef", 3, Sensitive) == 4);
    CHECK(EditDistance("render.graph", "audio.volume", 3, Sensitive) == 4);
    CHECK(EditDistance("abcdefgh", "ijklmnop", MaxEditDistanceLimit, Sensitive) == 8);
    // 가운데 "de"↔"ed"는 맞바꾸기 하나다(Levenshtein이면 8).
    CHECK(EditDistance("abcdefgh", "hgfedcba", MaxEditDistanceLimit, Sensitive) == 7);
}

// 한도가 MaxEditDistanceLimit보다 크면 그 값으로 줄여 계산한다(더 먼 거리는 "멀다"로만 답한다).
TEST(EditDistance_ClampsLimitToTheMaximum) {
    char longText[MaxEditDistanceLimit + 8];
    for (char& value : longText) {
        value = 'x';
    }

    const StringView far(longText, MaxEditDistanceLimit + 8);
    CHECK(EditDistance("", far, MaxEditDistanceLimit + 100, Sensitive) == MaxEditDistanceLimit + 1);
    CHECK(EditDistance("", StringView(longText, MaxEditDistanceLimit), MaxEditDistanceLimit + 100,
                       Sensitive) == MaxEditDistanceLimit);
    CHECK(EditDistance("kitten", "sitting", MaxEditDistanceLimit + 1, Sensitive) == 3);
}

namespace {

constexpr usize MaxReferenceLength = 5;

char FoldReference(char value, EditDistanceCase textCase) noexcept {
    return textCase == EditDistanceCase::AsciiInsensitive && value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

usize Smaller(usize left, usize right) noexcept {
    return left < right ? left : right;
}

// 전체 행렬로 구한 OSA 기준 거리다(짧은 텍스트 비교용).
usize ReferenceDistance(StringView left, StringView right, EditDistanceCase textCase) noexcept {
    usize table[MaxReferenceLength + 1][MaxReferenceLength + 1];
    const usize n = left.ByteCount();
    const usize m = right.ByteCount();
    for (usize i = 0; i <= n; ++i) {
        for (usize j = 0; j <= m; ++j) {
            if (i == 0 || j == 0) {
                table[i][j] = i + j;
                continue;
            }

            const char a = FoldReference(left.Data()[i - 1], textCase);
            const char b = FoldReference(right.Data()[j - 1], textCase);
            usize best = Smaller(table[i - 1][j] + 1, table[i][j - 1] + 1);
            best = Smaller(best, table[i - 1][j - 1] + (a == b ? 0 : 1));
            if (i >= 2 && j >= 2 && a == FoldReference(right.Data()[j - 2], textCase) &&
                FoldReference(left.Data()[i - 2], textCase) == b) {
                best = Smaller(best, table[i - 2][j - 2] + 1);
            }

            table[i][j] = best;
        }
    }

    return table[n][m];
}

} // namespace

// 길이 5 이하의 {a, b, A} 텍스트 쌍 전부에서 한도 0–6의 결과가 전체 행렬 OSA 기준과 맞는다(두 비교
// 방식 모두).
TEST(EditDistance_MatchesFullMatrixOnSmallAlphabet) {
    constexpr char Alphabet[] = {'a', 'b', 'A'};
    constexpr usize TextCapacity = 1 + 3 + 9 + 27 + 81 + 243;
    char texts[TextCapacity][MaxReferenceLength];
    usize lengths[TextCapacity];
    usize count = 0;
    usize combinations = 1;
    for (usize length = 0; length <= MaxReferenceLength; ++length) {
        for (usize code = 0; code < combinations; ++code) {
            usize rest = code;
            for (usize index = 0; index < length; ++index) {
                texts[count][index] = Alphabet[rest % 3];
                rest /= 3;
            }

            lengths[count++] = length;
        }

        combinations *= 3;
    }

    REQUIRE(count == TextCapacity);
    const EditDistanceCase cases[] = {Sensitive, Insensitive};
    usize mismatches = 0;
    for (const EditDistanceCase textCase : cases) {
        for (usize a = 0; a < count; ++a) {
            for (usize b = 0; b < count; ++b) {
                const StringView left(texts[a], lengths[a]);
                const StringView right(texts[b], lengths[b]);
                const usize expected = ReferenceDistance(left, right, textCase);
                for (usize limit = 0; limit <= 6; ++limit) {
                    const usize got = EditDistance(left, right, limit, textCase);
                    mismatches += got == (expected <= limit ? expected : limit + 1) ? 0 : 1;
                }
            }
        }
    }

    CHECK(mismatches == 0);
}
} // namespace edit_distance_tests_file
