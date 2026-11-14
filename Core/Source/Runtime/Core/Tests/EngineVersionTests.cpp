#include <Core/Build/EngineVersion.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;

namespace {

bool Prints(EngineVersion version, const char* expected) noexcept {
    char text[EngineVersion::TextBytes];
    const usize length = version.ToChars(text);
    return length == strlen(expected) && strcmp(text, expected) == 0;
}

} // namespace

// 표기는 정규 10진(앞자리 0 없음)이며 가장 긴 값도 TextBytes 안에 들어간다.
TEST(EngineVersion_ToCharsIsCanonicalDecimal) {
    CHECK(Prints(EngineVersion{0, 1, 0}, "0.1.0"));
    CHECK(Prints(EngineVersion{1, 0, 12}, "1.0.12"));
    CHECK(Prints(EngineVersion{10, 200, 3000}, "10.200.3000"));
    constexpr uint16 Largest = NumericLimits<uint16>::Max();
    CHECK(Prints(EngineVersion{Largest, Largest, Largest}, "65535.65535.65535"));
    CHECK(Prints(EngineVersion{}, "0.0.0"));
}

// 순서는 (주, 부, 패치) 사전식이다. 패치가 커도 부가 작으면 앞이다.
TEST(EngineVersion_OrdersByMajorMinorPatch) {
    constexpr EngineVersion ordered[] = {{0, 1, 0}, {0, 1, 1},  {0, 1, 9},
                                         {0, 2, 0}, {0, 10, 0}, {1, 0, 0}};
    constexpr usize count = sizeof(ordered) / sizeof(ordered[0]);
    for (usize left = 0; left < count; ++left) {
        for (usize right = 0; right < count; ++right) {
            CHECK((ordered[left] < ordered[right]) == (left < right));
            CHECK((ordered[left] <= ordered[right]) == (left <= right));
            CHECK((ordered[left] > ordered[right]) == (left > right));
            CHECK((ordered[left] >= ordered[right]) == (left >= right));
            CHECK((ordered[left] == ordered[right]) == (left == right));
            CHECK((ordered[left] != ordered[right]) == (left != right));
        }
    }

    static_assert(EngineVersion{0, 1, 9} < EngineVersion{0, 2, 0});
}

// 빈 값은 주·부가 모두 0인 값이고 패치만으로는 버전이 되지 않는다. 기준은 패치를 0으로 바꾼다.
TEST(EngineVersion_EmptyAndCompatibleBaseline) {
    CHECK(EngineVersion{}.IsEmpty());
    CHECK((EngineVersion{0, 0, 5}).IsEmpty());
    CHECK(!(EngineVersion{0, 1, 0}).IsEmpty());
    CHECK(!(EngineVersion{1, 0, 0}).IsEmpty());
    CHECK((EngineVersion{2, 3, 7}).CompatibleBaseline() == (EngineVersion{2, 3, 0}));
    CHECK((EngineVersion{2, 3, 0}).CompatibleBaseline() == (EngineVersion{2, 3, 0}));
}

// 패키지 로드 판정: compatibleWith의 주·부가 더 새면 거부하고, 같거나 오래되면 엔진 버전으로는
// 거부하지 않는다(실제 해석 가능 여부는 데이터 축이 정한다). 패치는 양쪽 모두 무시한다.
TEST(EngineVersion_PackageLoadRejectsOnlyNewerMajorMinor) {
    constexpr EngineVersion current{1, 4, 2};
    CHECK(current.CanLoadPackageCompatibleWith({1, 4, 0}));
    CHECK(current.CanLoadPackageCompatibleWith({1, 4, 9}));
    CHECK(current.CanLoadPackageCompatibleWith({1, 3, 0}));
    CHECK(current.CanLoadPackageCompatibleWith({0, 9, 0}));
    CHECK(!current.CanLoadPackageCompatibleWith({1, 5, 0}));
    CHECK(!current.CanLoadPackageCompatibleWith({2, 0, 0}));
    CHECK(!current.CanLoadPackageCompatibleWith({2, 1, 0}));
    static_assert(EngineVersion{0, 1, 0}.CanLoadPackageCompatibleWith({0, 1, 0}));
}
