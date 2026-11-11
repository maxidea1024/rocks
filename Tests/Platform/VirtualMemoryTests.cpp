#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Platform/VirtualMemory.h>
#include <Tests/Support/Test.h>

using namespace rocks;
using namespace rocks::platform;

namespace {

struct PageWorker {
    VirtualMemoryRegion* region;
    usize offset;
    usize pageSize;
    bool passed = true;
};

void ExercisePage(void* data) noexcept {
    auto& worker = *static_cast<PageWorker*>(data);
    for (uint32 i = 0; i < 100; ++i) {
        if (!worker.region->Commit(worker.offset, worker.pageSize)) {
            worker.passed = false;
            return;
        }
        auto* base = static_cast<uint8*>(worker.region->Address());
        if (base == nullptr) {
            worker.passed = false;
            return;
        }
        auto* bytes = base + worker.offset;
        bytes[0] = static_cast<uint8>(i);
        bytes[worker.pageSize - 1] = static_cast<uint8>(i + 1);
        if (bytes[0] != static_cast<uint8>(i) ||
            bytes[worker.pageSize - 1] != static_cast<uint8>(i + 1) ||
            !worker.region->Decommit(worker.offset, worker.pageSize)) {
            worker.passed = false;
            return;
        }
    }
}

void FinishWorker(Thread& thread) noexcept {
    if (thread.WaitForCompletion(20000) != WaitResult::Signaled ||
        thread.Join() != ThreadStatus::Success) {
        Fatal("Virtual memory test worker did not finish");
    }
}

} // namespace

TEST(PlatformVirtualMemoryValidationAndReuse) {
    VirtualMemoryInfo info{};
    REQUIRE(QueryVirtualMemoryInfo(info));
    REQUIRE(info.pageSize > 0);
    REQUIRE(info.reserveGranularity >= info.pageSize);
    REQUIRE(info.pageSize <= MaxSize / 3);

    VirtualMemoryRegion region;
    CHECK(region.Address() == nullptr);
    CHECK(region.Commit(0, info.pageSize).code == VirtualMemoryCode::NotReserved);
    CHECK(region.Reserve(0).code == VirtualMemoryCode::InvalidArgument);
    CHECK(region.Reserve(1).code == VirtualMemoryCode::InvalidArgument);
    REQUIRE(region.Reserve(info.pageSize * 3));
    CHECK(region.Address() != nullptr);
    CHECK(reinterpret_cast<uptr>(region.Address()) % info.reserveGranularity == 0);
    CHECK(region.Size() == info.pageSize * 3);
    CHECK(region.Reserve(info.pageSize).code == VirtualMemoryCode::AlreadyReserved);
    CHECK(region.Commit(0, 0).code == VirtualMemoryCode::InvalidArgument);
    CHECK(region.Commit(1, info.pageSize).code == VirtualMemoryCode::InvalidArgument);
    CHECK(region.Commit(0, 1).code == VirtualMemoryCode::InvalidArgument);
    CHECK(region.Commit(info.pageSize * 3, info.pageSize).code ==
          VirtualMemoryCode::InvalidArgument);
    CHECK(region.Commit(MaxSize, info.pageSize).code == VirtualMemoryCode::InvalidArgument);
    CHECK(region.Decommit(0, MaxSize).code == VirtualMemoryCode::InvalidArgument);
    CHECK(!region.IsPoisoned());

    REQUIRE(region.Commit(0, info.pageSize * 3));
    auto* bytes = static_cast<uint8*>(region.Address());
    bytes[0] = 0x31;
    bytes[info.pageSize] = 0x32;
    bytes[2 * info.pageSize] = 0x33;
    REQUIRE(region.Decommit(info.pageSize, info.pageSize));
    CHECK(bytes[0] == 0x31);
    CHECK(bytes[2 * info.pageSize] == 0x33);
    REQUIRE(region.Commit(info.pageSize, info.pageSize));
    bytes[info.pageSize] = 0x34;
    CHECK(bytes[info.pageSize] == 0x34);
    REQUIRE(region.Release());
    CHECK(region.Address() == nullptr);
    CHECK(region.Size() == 0);
    REQUIRE(region.Release());
    REQUIRE(region.Reserve(info.pageSize));
    REQUIRE(region.Commit(0, info.pageSize));
    // 마지막 Release는 소멸자가 처리한다.
}

TEST(PlatformVirtualMemoryConcurrentDistinctPages) {
    VirtualMemoryInfo info{};
    REQUIRE(QueryVirtualMemoryInfo(info));
    REQUIRE(info.pageSize > 0 && info.pageSize <= MaxSize / 2);

    VirtualMemoryRegion region;
    REQUIRE(region.Reserve(info.pageSize * 2));
    PageWorker workers[] = {{&region, 0, info.pageSize}, {&region, info.pageSize, info.pageSize}};
    Thread threads[2];
    REQUIRE(threads[0].Start(ExercisePage, &workers[0]) == ThreadStatus::Success);
    if (threads[1].Start(ExercisePage, &workers[1]) != ThreadStatus::Success) {
        FinishWorker(threads[0]);
        CHECK(false);
        return;
    }
    FinishWorker(threads[0]);
    FinishWorker(threads[1]);
    CHECK(workers[0].passed);
    CHECK(workers[1].passed);
    REQUIRE(region.Release());
}
