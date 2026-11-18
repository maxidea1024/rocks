#include <Core/Text/EditDistance.h>
#include <TestSupport/Test.h>

// 한도 안 편집 거리 시험이다(Core/Text/EditDistance.h): 알려진 거리, 빈 텍스트, 대칭, 한도를 넘으면
// 한도 + 1, 길이 차이만으로 한도를 넘는 경우.
using namespace rocks;

TEST(EditDistance_KnownDistancesWithinLimit) {
    CHECK(EditDistanceWithin("kitten", "sitting", 3) == 3);
    CHECK(EditDistanceWithin("sitting", "kitten", 3) == 3);
    CHECK(EditDistanceWithin("flaw", "lawn", 3) == 2);
    CHECK(EditDistanceWithin("render.shadow.distanse", "render.shadow.distance", 3) == 1);
    CHECK(EditDistanceWithin("render.shadow.distance", "render.shadow.distance", 3) == 0);
    CHECK(EditDistanceWithin("", "abc", 3) == 3);
    CHECK(EditDistanceWithin("abc", "", 3) == 3);
    CHECK(EditDistanceWithin("", "", 0) == 0);
    CHECK(EditDistanceWithin("ab", "ba", 2) == 2);
    CHECK(EditDistanceWithin("Abc", "abc", 1) == 1);
}

TEST(EditDistance_ReportsLimitPlusOneBeyondTheLimit) {
    CHECK(EditDistanceWithin("kitten", "sitting", 2) == 3);
    CHECK(EditDistanceWithin("kitten", "sitting", 0) == 1);
    CHECK(EditDistanceWithin("a", "abcdef", 3) == 4);
    CHECK(EditDistanceWithin("render.graph", "audio.volume", 3) == 4);
    CHECK(EditDistanceWithin("abcdefgh", "hgfedcba", MaxEditDistanceLimit) == 8);
}

namespace {

// 전체 행렬로 구한 기준 거리다(짧은 텍스트 비교용).
usize ReferenceDistance(StringView left, StringView right) noexcept {
    usize row[8];
    for (usize j = 0; j <= right.ByteCount(); ++j) {
        row[j] = j;
    }

    for (usize i = 1; i <= left.ByteCount(); ++i) {
        usize diagonal = row[0];
        row[0] = i;
        for (usize j = 1; j <= right.ByteCount(); ++j) {
            const usize above = row[j];
            const usize substitute = diagonal + (left.Data()[i - 1] == right.Data()[j - 1] ? 0 : 1);
            usize best = above + 1 < row[j - 1] + 1 ? above + 1 : row[j - 1] + 1;
            best = substitute < best ? substitute : best;
            row[j] = best;
            diagonal = above;
        }
    }

    return row[right.ByteCount()];
}

} // namespace

// 길이 4 이하의 {a, b} 텍스트 쌍 전부에서 한도 0–5의 결과가 전체 행렬 기준과 맞는다.
TEST(EditDistance_MatchesFullMatrixOnSmallAlphabet) {
    char texts[31][4];
    usize lengths[31];
    usize count = 0;
    for (usize length = 0; length <= 4; ++length) {
        for (usize bits = 0; bits < (usize{1} << length); ++bits) {
            for (usize index = 0; index < length; ++index) {
                texts[count][index] = (bits >> index) & 1 ? 'b' : 'a';
            }

            lengths[count++] = length;
        }
    }

    REQUIRE(count == 31);
    for (usize a = 0; a < count; ++a) {
        for (usize b = 0; b < count; ++b) {
            const StringView left(texts[a], lengths[a]);
            const StringView right(texts[b], lengths[b]);
            const usize expected = ReferenceDistance(left, right);
            for (usize limit = 0; limit <= 5; ++limit) {
                const usize got = EditDistanceWithin(left, right, limit);
                CHECK(got == (expected <= limit ? expected : limit + 1));
            }
        }
    }
}
