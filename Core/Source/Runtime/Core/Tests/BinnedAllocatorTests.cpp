#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

class FreeFailureAllocator final : public Allocator {
public:
    explicit FreeFailureAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    bool failFree = false;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (failFree) {
            return Result<void>::Failure(ErrorCode::Busy);
        }
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

class ReentrantBinnedBacking final : public Allocator {
public:
    explicit ReentrantBinnedBacking(Allocator& backing) noexcept : backing_(backing) {
    }
    BinnedAllocator* target = nullptr;
    ErrorCode nested = ErrorCode::None;
    ErrorCode nestedFree = ErrorCode::None;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (target != nullptr) {
            BinnedAllocator* current = target;
            target = nullptr;
            auto result = current->Allocate(1, 1);
            nested = result.Error();
            if (result && result.Value()) {
                (void)current->Deallocate(result.Value());
            }
            target = current;
        }
        return backing_.Allocate(bytes, alignment, tag);
    }
    Result<void> Deallocate(void* address) noexcept override {
        if (target != nullptr) {
            BinnedAllocator* current = target;
            target = nullptr;
            nestedFree = current->Deallocate(address).Error();
            target = current;
        }
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

struct BinnedWork {
    BinnedAllocator* allocator;
    uint32 number;
    uint32 failures = 0;
};

void RunBinnedWork(void* context) noexcept {
    auto& work = *static_cast<BinnedWork*>(context);
    for (uint32 iteration = 0; iteration < 1000; ++iteration) {
        const usize size = (iteration * 29 + work.number * 17) % 5000 + 1;
        auto result = work.allocator->Allocate(size, iteration % 7 == 0 ? 64 : 16, work.number);
        if (!result || result.Value() == nullptr) {
            ++work.failures;
            return;
        }
        auto* data = static_cast<uint8*>(result.Value());
        data[0] = static_cast<uint8>(work.number);
        data[size - 1] = static_cast<uint8>(iteration);
        platform::YieldThread();
        if ((size > 1 && data[0] != static_cast<uint8>(work.number)) ||
            data[size - 1] != static_cast<uint8>(iteration)) {
            ++work.failures;
        }
        if (!work.allocator->Deallocate(data)) {
            ++work.failures;
            return;
        }
    }
}

struct BinnedReturnWork {
    BinnedAllocator* allocator;
    void** addresses;
    usize count;
    uint32 failures = 0;
};

void ReturnBinnedWork(void* context) noexcept {
    auto& work = *static_cast<BinnedReturnWork*>(context);
    for (usize i = 0; i < work.count; ++i) {
        if (!work.allocator->Deallocate(work.addresses[i])) {
            ++work.failures;
        }
    }
}

struct BinnedDuplicateWork {
    BinnedAllocator* allocator;
    void* address;
    ErrorCode result = ErrorCode::None;
};

void ReturnSameBinnedWork(void* context) noexcept {
    auto& work = *static_cast<BinnedDuplicateWork*>(context);
    work.result = work.allocator->Deallocate(work.address).Error();
}

} // namespace

TEST(CoreBinnedBoundariesAlignmentOwnershipAndLifecycle) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        CHECK(allocator.Allocate(0, 8).Value() == nullptr);
        CHECK(allocator.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(allocator.Allocate(MaxSize, 16).Error() == ErrorCode::Overflow);
        constexpr usize sizes[] = {1,    15,   16,   17,   31,   32,   33,   63,  64,  65,
                                   127,  128,  129,  255,  256,  257,  511,  512, 513, 1023,
                                   1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097};
        void* blocks[sizeof(sizes) / sizeof(sizes[0])]{};
        for (usize i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            auto result = allocator.Allocate(sizes[i], 16, 42);
            REQUIRE(result);
            blocks[i] = result.Value();
            CHECK(reinterpret_cast<uptr>(blocks[i]) % 16 == 0);
            static_cast<uint8*>(blocks[i])[sizes[i] - 1] = static_cast<uint8>(i);
        }
        auto aligned = allocator.Allocate(7, 4096);
        REQUIRE(aligned);
        CHECK(reinterpret_cast<uptr>(aligned.Value()) % 4096 == 0);
        int foreign = 0;
        CHECK(allocator.Deallocate(&foreign).Error() == ErrorCode::NotFound);
        CHECK(allocator.Deallocate(static_cast<uint8*>(blocks[0]) + 1).Error() ==
              ErrorCode::NotFound);
        CHECK(allocator.Trim().Error() == ErrorCode::Busy);
        CHECK(allocator.Shutdown().Error() == ErrorCode::Busy);
        for (usize i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            CHECK(static_cast<uint8*>(blocks[i])[sizes[i] - 1] == static_cast<uint8>(i));
            REQUIRE(allocator.Deallocate(blocks[i]));
        }
        CHECK(allocator.Deallocate(blocks[0]).Error() == ErrorCode::NotFound);
        REQUIRE(allocator.Deallocate(aligned.Value()));
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().liveBytes == 0);
        REQUIRE(allocator.Trim());
        CHECK(system.Stats().liveAllocations == 0);
        REQUIRE(allocator.Shutdown());
        REQUIRE(allocator.Shutdown());
        CHECK(allocator.Allocate(1, 1).Error() == ErrorCode::ShuttingDown);
        CHECK(allocator.Trim().Error() == ErrorCode::ShuttingDown);
        CHECK(allocator.Deallocate(&foreign).Error() == ErrorCode::ShuttingDown);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CoreBinnedFailureRollbackAndReentry) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        BinnedAllocator allocator(failing);
        failing.FailOnAttempt(1);
        CHECK(allocator.Allocate(25, 8).Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(system.Stats().liveAllocations == 0);
        failing.FailOnAttempt(1);
        CHECK(allocator.Allocate(4097, 8).Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Stats().liveAllocations == 0);
        failing.DisableFailure();
        auto result = allocator.Allocate(25, 8);
        REQUIRE(result);
        REQUIRE(allocator.Deallocate(result.Value()));
        REQUIRE(allocator.Trim());
    }
    {
        FreeFailureAllocator failingFree(system);
        BinnedAllocator allocator(failingFree);
        auto result = allocator.Allocate(5000, 8);
        REQUIRE(result);
        static_cast<uint8*>(result.Value())[0] = 77;
        failingFree.failFree = true;
        CHECK(allocator.Deallocate(result.Value()).Error() == ErrorCode::Busy);
        CHECK(allocator.Stats().liveAllocations == 1);
        CHECK(static_cast<uint8*>(result.Value())[0] == 77);
        failingFree.failFree = false;
        REQUIRE(allocator.Deallocate(result.Value()));
    }
    {
        ReentrantBinnedBacking backing(system);
        {
            BinnedAllocator allocator(backing);
            backing.target = &allocator;
            auto result = allocator.Allocate(8, 8);
            REQUIRE(result);
            CHECK(backing.nested == ErrorCode::Busy);
            REQUIRE(allocator.Deallocate(result.Value()));
            REQUIRE(allocator.Trim());
            CHECK(backing.nestedFree == ErrorCode::Busy);
            result = allocator.Allocate(8, 8);
            REQUIRE(result);
            REQUIRE(allocator.Deallocate(result.Value()));
            // 소멸 시 pool이 상위 저장소를 반환하는 callback도 재진입을 차단해야 한다.
            backing.nestedFree = ErrorCode::None;
        }
        CHECK(backing.nestedFree == ErrorCode::Busy);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CoreBinnedFailureKeepsEarlierAllocations) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        BinnedAllocator allocator(failing);
        auto first = allocator.Allocate(24, 8);
        REQUIRE(first);
        static_cast<uint8*>(first.Value())[0] = 91;
        const auto before = allocator.Stats();
        failing.FailOnAttempt(1);
        CHECK(allocator.Allocate(100, 8).Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Stats().liveAllocations == before.liveAllocations);
        CHECK(allocator.Stats().liveBytes == before.liveBytes);
        CHECK(allocator.Stats().totalAllocations == before.totalAllocations);
        CHECK(static_cast<uint8*>(first.Value())[0] == 91);
        failing.FailOnAttempt(1);
        CHECK(allocator.Allocate(5000, 8).Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Stats().liveAllocations == before.liveAllocations);
        CHECK(allocator.Stats().liveBytes == before.liveBytes);
        CHECK(allocator.Stats().totalAllocations == before.totalAllocations);
        CHECK(static_cast<uint8*>(first.Value())[0] == 91);
        failing.DisableFailure();
        REQUIRE(allocator.Deallocate(first.Value()));
        REQUIRE(allocator.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CoreBinnedConcurrentAllocationStress) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        BinnedWork work[4]{{&allocator, 1}, {&allocator, 2}, {&allocator, 3}, {&allocator, 4}};
        platform::Thread threads[4];
        for (usize i = 0; i < 4; ++i) {
            if (threads[i].Start(RunBinnedWork, &work[i]) != platform::ThreadStatus::Success) {
                platform::Fatal("Binned test worker failed to start");
            }
        }
        for (usize i = 0; i < 4; ++i) {
            if (threads[i].WaitForCompletion(20000) != platform::WaitResult::Signaled ||
                threads[i].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Binned test worker failed to finish");
            }
            CHECK(work[i].failures == 0);
        }
        CHECK(allocator.Stats().liveAllocations == 0);
        CHECK(allocator.Stats().totalAllocations == 4000);
        REQUIRE(allocator.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CoreBinnedCrossThreadAndConcurrentDuplicateReturn) {
    SystemAllocator system;
    {
        BinnedAllocator allocator(system);
        void* addresses[128]{};
        for (usize i = 0; i < 128; ++i) {
            auto result = allocator.Allocate(i % 2 == 0 ? 40 : 6000, 16);
            REQUIRE(result);
            addresses[i] = result.Value();
        }
        BinnedReturnWork work[2]{{&allocator, addresses, 64}, {&allocator, addresses + 64, 64}};
        platform::Thread threads[2];
        for (usize i = 0; i < 2; ++i) {
            if (threads[i].Start(ReturnBinnedWork, &work[i]) != platform::ThreadStatus::Success) {
                platform::Fatal("Binned return worker failed to start");
            }
        }
        for (usize i = 0; i < 2; ++i) {
            if (threads[i].WaitForCompletion(20000) != platform::WaitResult::Signaled ||
                threads[i].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Binned return worker failed to finish");
            }
            CHECK(work[i].failures == 0);
        }
        auto result = allocator.Allocate(25, 8);
        REQUIRE(result);
        BinnedDuplicateWork duplicates[2]{{&allocator, result.Value()},
                                          {&allocator, result.Value()}};
        for (usize i = 0; i < 2; ++i) {
            if (threads[i].Start(ReturnSameBinnedWork, &duplicates[i]) !=
                platform::ThreadStatus::Success) {
                platform::Fatal("Binned duplicate worker failed to start");
            }
        }
        for (auto& thread : threads) {
            if (thread.WaitForCompletion(20000) != platform::WaitResult::Signaled ||
                thread.Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Binned duplicate worker failed to finish");
            }
        }
        CHECK((duplicates[0].result == ErrorCode::None &&
               duplicates[1].result == ErrorCode::NotFound) ||
              (duplicates[1].result == ErrorCode::None &&
               duplicates[0].result == ErrorCode::NotFound));
        CHECK(allocator.Stats().liveAllocations == 0);
        REQUIRE(allocator.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}
