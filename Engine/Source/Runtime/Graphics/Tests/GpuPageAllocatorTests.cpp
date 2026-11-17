#include <Core/Memory/SystemAllocator.h>
#include <Graphics/GpuPageAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

TEST(GpuPageAllocator_AlignsAndReservesPadding) {
    SystemAllocator allocator;
    GpuPageAllocator page(allocator);
    REQUIRE(page.Reset(4096));
    auto first = page.Allocate(10, 4);
    REQUIRE(first);
    CHECK(first.Value().offset == 0 && first.Value().reservedSize == 10);
    // 다음 구간은 10에서 시작하므로 256 정렬이면 앞 246 byte가 함께 잡힌다.
    auto second = page.Allocate(100, 256);
    REQUIRE(second);
    CHECK(second.Value().offset == 256);
    CHECK(second.Value().reservedOffset == 10 && second.Value().reservedSize == 346);
    CHECK(page.UsedBytes() == 356);
    CHECK(page.Allocate(1, 3).Error() == ErrorCode::InvalidArgument);
    CHECK(page.Allocate(0, 4).Error() == ErrorCode::InvalidArgument);
}

TEST(GpuPageAllocator_FirstFitReusesFreedHoles) {
    SystemAllocator allocator;
    GpuPageAllocator page(allocator);
    REQUIRE(page.Reset(1000));
    auto a = page.Allocate(100, 1);
    auto b = page.Allocate(200, 1);
    auto c = page.Allocate(300, 1);
    REQUIRE(a && b && c);
    page.Free(b.Value());
    CHECK(page.FreeRangeCount() == 2);
    // 150은 b의 빈자리(100..300)에 먼저 들어간다.
    auto d = page.Allocate(150, 1);
    REQUIRE(d);
    CHECK(d.Value().offset == 100);
    // 남은 칸(400 + 50 따로)으로는 500을 이어 받을 수 없다.
    CHECK(page.Allocate(500, 1).Error() == ErrorCode::CapacityExceeded);
    auto e = page.Allocate(400, 1);
    REQUIRE(e);
    CHECK(e.Value().offset == 600);
}

TEST(GpuPageAllocator_FreeMergesNeighboursBackToOneRange) {
    SystemAllocator allocator;
    GpuPageAllocator page(allocator);
    REQUIRE(page.Reset(1024));
    GpuPageAllocation blocks[4];
    for (GpuPageAllocation& block : blocks) {
        auto taken = page.Allocate(256, 64);
        REQUIRE(taken);
        block = taken.Value();
    }

    CHECK(page.Allocate(1, 1).Error() == ErrorCode::CapacityExceeded);
    CHECK(page.FreeRangeCount() == 0);
    // 가운데 둘을 따로 놓고(빈 구간 둘이 이어져 하나), 양 끝을 놓으면 전체 하나가 된다.
    page.Free(blocks[1]);
    page.Free(blocks[2]);
    CHECK(page.FreeRangeCount() == 1);
    page.Free(blocks[3]);
    CHECK(page.FreeRangeCount() == 1);
    page.Free(blocks[0]);
    CHECK(page.FreeRangeCount() == 1 && page.IsEmpty());
    auto whole = page.Allocate(1024, 1024);
    REQUIRE(whole);
    CHECK(whole.Value().offset == 0);
}
