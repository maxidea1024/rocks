// 엔진 런타임은 STL을 쓰지 않는다. 이 테스트 파일만 "사용자 게임 코드가 STL 알고리즘과 Rocks
// 컬렉션을 함께 쓰는 경우"를 재현하려고 STL 헤더를 포함한다. 엔진 빌드 설정처럼 예외가 꺼진
// 상태에서 MSVC STL을 쓰려면 _HAS_EXCEPTIONS=0이 필요하다(libc++는 -fno-exceptions를 자체
// 처리한다).
#if !defined(_HAS_EXCEPTIONS)
#define _HAS_EXCEPTIONS 0
#endif

#include <TestSupport/Test.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/BitArray.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/HashSet.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/SparseArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>

#include <Core/Math/RandomStream.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <numeric>
#include <ranges>
#include <span>

namespace rocks::test {

// C++20 iterator concept을 컴파일 시점에 고정한다. std 태그 없이도 연산만으로 성립해야 한다.
static_assert(std::contiguous_iterator<Array<int>::iterator>);
static_assert(std::ranges::contiguous_range<Array<int>>);
static_assert(std::ranges::contiguous_range<Span<const float>>);
static_assert(std::ranges::contiguous_range<StringView>);
static_assert(std::random_access_iterator<BitArray::ConstIterator>);
static_assert(std::ranges::random_access_range<const BitArray>);
static_assert(std::forward_iterator<HashMap<int, int>::iterator>);
static_assert(std::forward_iterator<HashMap<int, int>::const_iterator>);
static_assert(std::forward_iterator<HashSet<int>::const_iterator>);
static_assert(std::ranges::forward_range<HashMap<int, int>>);
static_assert(std::ranges::forward_range<HashSet<int>>);
static_assert(std::forward_iterator<SparseArray<int>::iterator>);
static_assert(std::forward_iterator<SparseArray<int>::const_iterator>);
static_assert(std::ranges::forward_range<const SparseArray<int>>);
static_assert(std::forward_iterator<SlotArray<int>::iterator>);
static_assert(std::forward_iterator<SlotArray<int>::const_iterator>);
static_assert(std::ranges::forward_range<const SlotArray<int>>);

TEST(StlCompatibility_ArrayWithAlgorithms) {
    SystemAllocator allocator;
    Array<int> values(allocator);
    const int input[] = {5, 3, 9, 1, 7};
    REQUIRE(values.TryAppend(Span<const int>(input)));
    std::sort(values.begin(), values.end());
    CHECK(values[0] == 1 && values[4] == 9);
    CHECK(std::accumulate(values.begin(), values.end(), 0) == 25);
    CHECK(std::find(values.begin(), values.end(), 7) - values.begin() == 3);
    CHECK(std::ranges::max(values) == 9);
    std::ranges::reverse(values);
    CHECK(values[0] == 9);
    // std::span으로 비소유 view를 만들 수 있다.
    std::span<int> span(values);
    CHECK(span.size() == 5 && span[4] == 1);
    CHECK(std::ranges::count_if(values, [](int value) { return value > 4; }) == 3);
}

TEST(StlCompatibility_StringViewAndBitArray) {
    const StringView text("hello rocks");
    CHECK(std::count(text.begin(), text.end(), 'o') == 2);
    CHECK(std::ranges::distance(text) == 11);
    SystemAllocator allocator;
    BitArray bits(allocator);
    REQUIRE(bits.TryAdd(true, 3));
    REQUIRE(bits.TryAdd(false, 2));
    CHECK(std::ranges::count(bits, true) == 3);
    CHECK(std::find(bits.begin(), bits.end(), false) - bits.begin() == 3);
}

TEST(StlCompatibility_HashContainersWithAlgorithms) {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    for (int key = 1; key <= 10; ++key) {
        REQUIRE(map.Insert(key, key * key));
    }
    const auto found =
        std::find_if(map.begin(), map.end(), [](auto entry) { return entry.value == 49; });
    REQUIRE(found != map.end());
    CHECK(found->key == 7);
    CHECK(std::ranges::count_if(map, [](auto entry) { return entry.key % 2 == 0; }) == 5);
    int total = 0;
    std::ranges::for_each(map, [&](auto entry) { total += entry.value; });
    CHECK(total == 385);
    HashSet<int> set(allocator);
    for (int key = 0; key < 20; ++key) {
        REQUIRE(set.Add(key));
    }
    CHECK(std::ranges::count_if(set, [](int key) { return key >= 10; }) == 10);
    CHECK(std::accumulate(set.begin(), set.end(), 0) == 190);
}

namespace {

// 문자열 표기에서 유효 숫자 개수를 센다(부호·지수·소수점·앞뒤 0 제외). to_chars 대조에서만 쓰므로
// to_chars를 쓸 수 없는 배포 대상(macOS 13.3 미만)에서는 쓰이지 않는다.
[[maybe_unused]] int SignificantDigits(const char* text, usize length) noexcept {
    char digits[64];
    int count = 0;
    for (usize index = 0; index < length; ++index) {
        const char character = text[index];
        if (character == 'e' || character == 'E') {
            break;
        }

        if (character >= '0' && character <= '9' && count < 64) {
            digits[count++] = character;
        }
    }

    int first = 0;
    while (first < count && digits[first] == '0') {
        ++first;
    }

    int last = count;
    while (last > first && digits[last - 1] == '0') {
        --last;
    }

    return last - first;
}

// libc++는 배포 대상 OS의 시스템 라이브러리에 실수 to_chars가 없으면(macOS 13.3 미만) 그 함수를
// 쓸 수 없게 표시한다. 그때는 표준 대조(자릿수 비교)를 건너뛰고 왕복만 확인한다. 최단 표기
// 알고리즘은 플랫폼과 무관하므로 Windows·Android 실행이 표준 대조를 맡는다.
#if (defined(_LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT) &&                                  \
     !_LIBCPP_AVAILABILITY_HAS_TO_CHARS_FLOATING_POINT) ||                                         \
    defined(_LIBCPP_AVAILABILITY_HAS_NO_TO_CHARS_FLOATING_POINT)
#define ROCKS_TEST_HAS_FLOAT_TO_CHARS 0
#else
#define ROCKS_TEST_HAS_FLOAT_TO_CHARS 1
#endif

constexpr bool HasStandardFloatToChars = ROCKS_TEST_HAS_FLOAT_TO_CHARS != 0;

template <typename T>
bool MatchesStandardShortest(T value) noexcept {
    SystemAllocator allocator;
    StringBuilder<64> ours(allocator);
    if (!ours.TryAppendFormat("{}", value)) {
        return false;
    }

    // 왕복은 C의 strtod·strtof로 확인한다(Android libc++에는 실수 from_chars가 없다).
    char* end = nullptr;
    T back{};
    if constexpr (sizeof(T) == sizeof(float)) {
        back = std::strtof(ours.CString(), &end);
    } else {
        back = std::strtod(ours.CString(), &end);
    }

    if (end != ours.CString() + ours.ByteCount() || std::memcmp(&back, &value, sizeof(T)) != 0) {
        return false;
    }

#if !ROCKS_TEST_HAS_FLOAT_TO_CHARS
    return true;
#else
    // std::to_chars(scientific)는 왕복하는 가장 짧은 표기를 낸다.
    char standard[64];
    const auto converted =
        std::to_chars(standard, standard + sizeof(standard), value, std::chars_format::scientific);
    return converted.ec == std::errc() &&
           SignificantDigits(ours.CString(), ours.ByteCount()) ==
               SignificantDigits(standard, static_cast<usize>(converted.ptr - standard));
#endif
}

} // namespace

// Rocks 실수 최단 표기의 자릿수를 STL to_chars와 비교하고 C runtime으로 왕복을 확인한다.
TEST(StlCompatibility_ShortestFloatingPointMatchesToChars) {
    if constexpr (!HasStandardFloatToChars) {
        std::printf("  to_chars unavailable for this deployment target: round-trip only\n");
    }

    bool matches = true;
    double power = 1.0;
    for (int exponent = 0; exponent < 1074 && matches; ++exponent) {
        matches = MatchesStandardShortest(power) && MatchesStandardShortest(-power * 3.0);
        power /= 2.0;
    }

    CHECK(matches);
    float single = 1.0f;
    for (int exponent = 0; exponent < 149 && matches; ++exponent) {
        matches = MatchesStandardShortest(single);
        single /= 2.0f;
    }

    CHECK(matches);
    RandomStream random(test::CurrentSeed() ^ 0x70c4ull);
    for (int iteration = 0; iteration < 4000 && matches; ++iteration) {
        const uint64 bits = (static_cast<uint64>(random.NextUInt32()) << 32) | random.NextUInt32();
        double value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        if (value != value || value - value != 0.0) {
            continue;
        }

        const uint32 singleBits = random.NextUInt32();
        float singleValue = 0;
        std::memcpy(&singleValue, &singleBits, sizeof(singleValue));
        matches = MatchesStandardShortest(value) &&
                  (singleValue != singleValue || singleValue - singleValue != 0.0f ||
                   MatchesStandardShortest(singleValue));
    }

    CHECK(matches);
}

TEST(StlCompatibility_SparseAndSlotArraysWithAlgorithms) {
    SystemAllocator allocator;
    SparseArray<int> sparse(allocator);
    for (int value = 0; value < 10; ++value) {
        REQUIRE(sparse.TryAdd(value));
    }
    REQUIRE(sparse.TryRemoveAt(3));
    REQUIRE(sparse.TryRemoveAt(7));
    CHECK(std::accumulate(sparse.begin(), sparse.end(), 0) == 45 - 3 - 7);
    const auto found = std::find(sparse.begin(), sparse.end(), 8);
    REQUIRE(found != sparse.end());
    CHECK(found.Index() == 8);
    CHECK(std::ranges::count_if(sparse, [](int value) { return value % 2 == 0; }) == 5);

    SlotArray<int> slots(allocator);
    for (int value = 1; value <= 5; ++value) {
        REQUIRE(slots.TryAdd(value));
    }
    CHECK(std::ranges::count_if(slots, [](int value) { return value > 2; }) == 3);
    const auto largest = std::ranges::max_element(slots);
    REQUIRE(largest != slots.end());
    CHECK(*largest == 5 && largest.Handle() == (SlotHandle{4, 1}));
}

} // namespace rocks::test
