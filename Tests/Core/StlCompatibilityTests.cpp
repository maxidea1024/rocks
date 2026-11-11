// 엔진 런타임은 STL을 쓰지 않는다. 이 테스트 파일만 "사용자 게임 코드가 STL 알고리즘과 Rocks
// 컬렉션을 함께 쓰는 경우"를 재현하려고 STL 헤더를 포함한다. 엔진 빌드 설정처럼 예외가 꺼진
// 상태에서 MSVC STL을 쓰려면 _HAS_EXCEPTIONS=0이 필요하다(libc++는 -fno-exceptions를 자체
// 처리한다).
#if !defined(_HAS_EXCEPTIONS)
#define _HAS_EXCEPTIONS 0
#endif

#include "Tests/Support/Test.h"
#include <Core/Containers/Array.h>
#include <Core/Containers/BitArray.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringView.h>

#include <algorithm>
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

} // namespace rocks::test
