#include <Base/Compiler.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Platform/VirtualMemory.h>
#include <PlatformBase/Private/Common/VirtualMemoryTestAccess.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace virtual_memory_tests_file {

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

// 물리 메모리 총량은 0보다 크고 페이지 크기의 배수 이상이다(시험 기기의 실제 값 — 하한만 본다).
TEST(PlatformPhysicalMemoryIsReported) {
    VirtualMemoryInfo info{};
    REQUIRE(QueryVirtualMemoryInfo(info));
    uint64 bytes = 0;
    REQUIRE(QueryPhysicalMemory(bytes));
    CHECK(bytes >= info.pageSize);
}

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

// 접근을 열어 둔 예약(VirtualMemoryAccess::Open): 예약 → commit → 쓰기 → 가운데 decommit → 다시
// commit → Release가 된다. 큰 범위를 예약해도 commit한 조각만 쓴다. 인자 검사는 Guarded와 같다.
TEST(PlatformVirtualMemoryOpenReservationLifecycle) {
    VirtualMemoryInfo info{};
    REQUIRE(QueryVirtualMemoryInfo(info));
    const usize page = info.pageSize;
    VirtualMemoryRegion region;
    CHECK(region.Reserve(1, VirtualMemoryAccess::Open).code == VirtualMemoryCode::InvalidArgument);
    REQUIRE(region.Reserve(page * 3, VirtualMemoryAccess::Open));
    CHECK(region.Address() != nullptr);
    CHECK(region.Size() == page * 3);
    CHECK(region.Reserve(page, VirtualMemoryAccess::Open).code ==
          VirtualMemoryCode::AlreadyReserved);
    CHECK(region.Commit(1, page).code == VirtualMemoryCode::InvalidArgument);
    CHECK(region.Decommit(0, page * 4).code == VirtualMemoryCode::InvalidArgument);
    REQUIRE(region.Commit(0, page * 3));
    auto* bytes = static_cast<uint8*>(region.Address());
    bytes[0] = 0x41;
    bytes[page] = 0x42;
    bytes[2 * page] = 0x43;
    REQUIRE(region.Decommit(page, page));
    // 이웃 페이지는 그대로다.
    CHECK(bytes[0] == 0x41);
    CHECK(bytes[2 * page] == 0x43);
    REQUIRE(region.Commit(page, page));
    bytes[page] = 0x44;
    bytes[2 * page - 1] = 0x45;
    CHECK(bytes[page] == 0x44);
    CHECK(bytes[2 * page - 1] == 0x45);
    REQUIRE(region.Decommit(0, page * 3));
    REQUIRE(region.Commit(0, page * 3));
    bytes[0] = 0x46;
    CHECK(bytes[0] == 0x46);
    REQUIRE(region.Release());
    CHECK(region.Address() == nullptr);

    // 큰 범위(64 MiB)를 예약하고 끝 조각만 commit해 쓴다. 같은 객체를 다른 종류로 다시 예약할 수
    // 있다.
    constexpr usize Large = usize{64} << 20;
    REQUIRE(Large % page == 0);
    REQUIRE(region.Reserve(Large, VirtualMemoryAccess::Open));
    REQUIRE(region.Commit(Large - page, page));
    auto* tail = static_cast<uint8*>(region.Address()) + Large - page;
    tail[0] = 0x51;
    tail[page - 1] = 0x52;
    CHECK(tail[0] == 0x51);
    CHECK(tail[page - 1] == 0x52);
    REQUIRE(region.Decommit(Large - page, page));
    REQUIRE(region.Release());
    REQUIRE(region.Reserve(page, VirtualMemoryAccess::Guarded));
    REQUIRE(region.Commit(0, page));
    static_cast<uint8*>(region.Address())[0] = 0x53;
    // 마지막 Release는 소멸자가 처리한다.
}

// commit 실패는 영역을 닫지 않는다: 같은 범위를 다시 commit할 수 있고 다른 범위와 decommit도 된다.
// 인자 검사에서 걸린 호출은 주입 차례를 쓰지 않는다. 두 예약 종류 모두 같다.
TEST(PlatformVirtualMemoryCommitFailureIsTransient) {
    using platform::detail::VirtualMemoryTestAccess;
    VirtualMemoryInfo info{};
    REQUIRE(QueryVirtualMemoryInfo(info));
    const usize page = info.pageSize;
    constexpr VirtualMemoryAccess Kinds[] = {VirtualMemoryAccess::Guarded,
                                             VirtualMemoryAccess::Open};
    for (VirtualMemoryAccess kind : Kinds) {
        VirtualMemoryRegion region;
        REQUIRE(region.Reserve(page * 2, kind));
        VirtualMemoryTestAccess::FailCommitOnCall(1);
        CHECK(region.Commit(1, page).code == VirtualMemoryCode::InvalidArgument);
        const VirtualMemoryResult failed = region.Commit(0, page);
        CHECK(failed.code == VirtualMemoryCode::OsFailure);
        CHECK(failed.nativeError == platform::detail::InjectedVirtualMemoryError);
        CHECK(!region.IsPoisoned());
        CHECK(region.Address() != nullptr);
        // 실패한 범위를 decommit해도 되고(commit되지 않은 범위), 다시 commit하면 쓸 수 있다.
        REQUIRE(region.Decommit(0, page));
        REQUIRE(region.Commit(0, page));
        REQUIRE(region.Commit(page, page));
        auto* bytes = static_cast<uint8*>(region.Address());
        bytes[0] = 0x61;
        bytes[page] = 0x62;
        CHECK(bytes[0] == 0x61);
        CHECK(bytes[page] == 0x62);
        REQUIRE(region.Decommit(0, page * 2));
        VirtualMemoryTestAccess::ClearFaults();
        REQUIRE(region.Release());
    }
}

// decommit 실패도 영역을 닫지 않는다. 접근을 열어 둔 예약(과 Windows)에서는 그 범위가 그대로라 계속
// 쓸 수 있고, 어느 종류든 다시 commit·decommit할 수 있다.
TEST(PlatformVirtualMemoryDecommitFailureKeepsRegionOpen) {
    using platform::detail::VirtualMemoryTestAccess;
    VirtualMemoryInfo info{};
    REQUIRE(QueryVirtualMemoryInfo(info));
    const usize page = info.pageSize;
    constexpr VirtualMemoryAccess Kinds[] = {VirtualMemoryAccess::Guarded,
                                             VirtualMemoryAccess::Open};
    for (VirtualMemoryAccess kind : Kinds) {
        VirtualMemoryRegion region;
        REQUIRE(region.Reserve(page * 2, kind));
        REQUIRE(region.Commit(0, page * 2));
        auto* bytes = static_cast<uint8*>(region.Address());
        bytes[0] = 0x71;
        bytes[page] = 0x72;
        VirtualMemoryTestAccess::FailDecommitOnCall(1);
        const VirtualMemoryResult failed = region.Decommit(0, page);
        CHECK(failed.code == VirtualMemoryCode::OsFailure);
        CHECK(failed.nativeError == platform::detail::InjectedVirtualMemoryError);
        CHECK(!region.IsPoisoned());
        if (kind == VirtualMemoryAccess::Open || IsWindows) {
            CHECK(bytes[0] == 0x71);
        }

        // 다른 범위는 영향이 없다.
        CHECK(bytes[page] == 0x72);
        REQUIRE(region.Commit(0, page));
        bytes[0] = 0x73;
        CHECK(bytes[0] == 0x73);
        REQUIRE(region.Decommit(0, page));
        REQUIRE(region.Decommit(page, page));
        VirtualMemoryTestAccess::ClearFaults();
        REQUIRE(region.Release());
    }
}
} // namespace virtual_memory_tests_file
