#include <Base/NumericLimits.h>
#include <Core/Memory/LibraryHeap.h>
#include <TestSupport/Test.h>
#include <stddef.h>

// C 라이브러리 hook용 공용 힙 시험이다(Core/Memory/LibraryHeap.h — HarfBuzz·ICU·nanosvg가 쓴다).
// malloc·calloc·realloc·free와 같은 의미, 정렬, 넘침 거부, 살아 있는 블록 수를 단언한다.

using namespace rocks;

namespace library_heap_tests_file {

TEST(Core_LibraryHeapBehavesLikeCAllocation) {
    const usize before = LibraryHeapLiveBlocks();
    auto* bytes = static_cast<uint8*>(LibraryHeapAllocate(10));
    REQUIRE(bytes != nullptr);
    // 플랫폼 malloc과 같은 16 byte 정렬이다.
    constexpr uptr MallocAlignment = 16;
    CHECK(reinterpret_cast<uptr>(bytes) % MallocAlignment == 0);
    CHECK(LibraryHeapLiveBlocks() == before + 1);
    for (uint8 i = 0; i < 10; ++i) {
        bytes[i] = i;
    }

    // 키우면 앞 내용을 옮기고, 줄이면 앞부분만 남긴다.
    auto* grown = static_cast<uint8*>(LibraryHeapReallocate(bytes, 64));
    REQUIRE(grown != nullptr);
    CHECK(grown[0] == 0 && grown[9] == 9);
    auto* shrunk = static_cast<uint8*>(LibraryHeapReallocate(grown, 4));
    REQUIRE(shrunk != nullptr);
    CHECK(shrunk[3] == 3);
    CHECK(LibraryHeapLiveBlocks() == before + 1);
    // calloc은 0으로 채운다.
    auto* zeroed = static_cast<uint8*>(LibraryHeapAllocateZeroed(8, 4));
    REQUIRE(zeroed != nullptr);
    bool allZero = true;
    for (usize i = 0; i < 32; ++i) {
        allZero = allZero && zeroed[i] == 0;
    }

    CHECK(allZero);
    // 0 byte도 서로 다른 유효 주소이고, nullptr realloc은 할당과 같다.
    void* empty = LibraryHeapAllocate(0);
    void* fresh = LibraryHeapReallocate(nullptr, 16);
    CHECK(empty != nullptr && fresh != nullptr && empty != fresh);
    // 넘치는 요청은 nullptr이고 블록 수가 바뀌지 않는다.
    const usize live = LibraryHeapLiveBlocks();
    CHECK(LibraryHeapAllocate(NumericLimits<usize>::Max()) == nullptr);
    CHECK(LibraryHeapAllocateZeroed(NumericLimits<usize>::Max() / 2, 4) == nullptr);
    CHECK(LibraryHeapLiveBlocks() == live);
    LibraryHeapFree(shrunk);
    LibraryHeapFree(zeroed);
    LibraryHeapFree(empty);
    LibraryHeapFree(fresh);
    LibraryHeapFree(nullptr);
    CHECK(LibraryHeapLiveBlocks() == before);
}

} // namespace library_heap_tests_file
