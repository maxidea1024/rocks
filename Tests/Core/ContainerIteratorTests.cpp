#include "Tests/Support/Test.h"
#include <Core/Containers/Array.h>
#include <Core/Containers/BitArray.h>
#include <Core/Containers/HashMap.h>
#include <Core/Containers/HashSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>

// STL 없이 범위 기반 for와 반복자 계약을 검사한다. STL 알고리즘과의 상호 운용은
// StlCompatibilityTests.cpp가 실제 STL 헤더로 검사한다.
namespace rocks::test {

TEST(ContainerIterators_ArrayAndSpanRangeFor) {
    SystemAllocator allocator;
    Array<int> values(allocator);
    for (int value = 1; value <= 5; ++value) {
        REQUIRE(values.TryPushBack(value));
    }
    int sum = 0;
    for (int value : values) {
        sum += value;
    }
    CHECK(sum == 15);
    for (int& value : values) {
        value *= 2; // 반복자로 원소 값을 바꿀 수 있다.
    }
    CHECK(values[4] == 10);
    const Array<int>& constant = values;
    CHECK(constant.end() - constant.begin() == 5);
    CHECK(values.cbegin() == values.View().begin());
    Span<const int> view = values.View();
    int count = 0;
    for (int value : view) {
        count += value > 0 ? 1 : 0;
    }
    CHECK(count == 5);
    Array<int> empty(allocator);
    CHECK(empty.begin() == empty.end());
    for (int value : empty) {
        CHECK(value != value); // 실행되면 안 된다.
    }
}

TEST(ContainerIterators_StringBytes) {
    StringView view("rock");
    usize bytes = 0;
    for (char c : view) {
        bytes += c != '\0' ? 1 : 0;
    }
    CHECK(bytes == 4 && view.end() - view.begin() == 4);
    SystemAllocator allocator;
    String text(allocator);
    REQUIRE(text.TryAssign("abc"));
    char last = 0;
    for (char c : text) {
        last = c;
    }
    CHECK(last == 'c');
}

TEST(ContainerIterators_BitArrayReadsBits) {
    SystemAllocator allocator;
    BitArray bits(allocator);
    for (int index = 0; index < 130; ++index) {
        REQUIRE(bits.TryAdd(index % 3 == 0));
    }
    usize setCount = 0;
    for (bool bit : bits) {
        setCount += bit ? 1 : 0;
    }
    CHECK(setCount == 44); // 0, 3, ..., 129
    auto iterator = bits.begin();
    iterator += 3;
    CHECK(*iterator && !iterator[1] && iterator[3]);
    CHECK(bits.end() - bits.begin() == 130);
    CHECK(bits.begin() < bits.end());
}

TEST(ContainerIterators_HashMapVisitsEachEntryOnce) {
    SystemAllocator allocator;
    HashMap<int, int> map(allocator);
    for (int key = 0; key < 100; ++key) {
        REQUIRE(map.Insert(key, key * 10));
    }
    REQUIRE(map.Erase(7));
    int visited = 0;
    int keySum = 0;
    for (auto [key, value] : map) {
        ++visited;
        keySum += key;
        value += 1; // 값 참조를 통해 컨테이너의 값을 바꾼다.
    }
    CHECK(visited == 99 && keySum == 4950 - 7);
    CHECK(*map.Find(3) == 31);
    const HashMap<int, int>& constant = map;
    int constantVisited = 0;
    for (const auto& entry : constant) {
        constantVisited += entry.value == entry.key * 10 + 1 ? 1 : 0;
    }
    CHECK(constantVisited == 99);
    auto iterator = map.begin();
    CHECK(iterator->value == *map.Find(iterator->key));
    HashMap<int, int> empty(allocator);
    CHECK(empty.begin() == empty.end());
}

TEST(ContainerIterators_HashSetVisitsEachKeyOnce) {
    SystemAllocator allocator;
    HashSet<int> set(allocator);
    for (int key = 0; key < 50; ++key) {
        REQUIRE(set.Add(key * 3));
    }
    int sum = 0;
    int count = 0;
    for (const int& key : set) {
        sum += key;
        ++count;
    }
    CHECK(count == 50 && sum == 3 * 1225);
}

} // namespace rocks::test
