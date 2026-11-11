#include <Core/Memory/DebugAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>
#include <Tests/Support/HostFailure.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

// payload 반환이 실패하면 기존 내용과 ownership이 유지되어야 재시도할 수 있다.
class FailingReleaseAllocator final : public Allocator {
public:
    explicit FailingReleaseAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    void* latestStorage = nullptr;
    void* failAddress = nullptr;
    bool failOnce = false;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        auto result = backing_.Allocate(bytes, alignment, tag);
        if (result) {
            latestStorage = result.Value();
        }
        return result;
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == failAddress && failOnce) {
            failOnce = false;
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

// guard가 회귀해도 재귀가 무한히 이어지지 않게 관측은 한 번만 수행한다.
class ReentrantDebugBacking final : public Allocator {
public:
    explicit ReentrantDebugBacking(Allocator& backing) noexcept : backing_(backing) {
    }

    DebugAllocator* target = nullptr;
    ErrorCode allocationReentry = ErrorCode::None;
    ErrorCode validationReentry = ErrorCode::None;

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (target) {
            DebugAllocator* current = target;
            target = nullptr;
            auto nested = current->Allocate(1, 1);
            allocationReentry = nested.Error();
            if (nested && nested.Value()) {
                (void)current->Deallocate(nested.Value());
            }
            target = current;
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (target) {
            DebugAllocator* current = target;
            target = nullptr;
            validationReentry = current->Validate().Error();
            target = current;
        }
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
};

struct DebugThreadWork {
    DebugAllocator* allocator;
    uint8 pattern;
    uint32 failures = 0;
};

void UseDebugOnWorker(void* raw) noexcept {
    auto& work = *static_cast<DebugThreadWork*>(raw);
    for (uint32 iteration = 0; iteration < 250; ++iteration) {
        auto allocation = work.allocator->Allocate(32, 16, work.pattern);
        if (!allocation || !allocation.Value()) {
            ++work.failures;
            return;
        }
        auto* bytes = static_cast<uint8*>(allocation.Value());
        for (usize index = 0; index < 32; ++index) {
            bytes[index] = work.pattern;
        }
        platform::YieldThread();
        for (usize index = 0; index < 32; ++index) {
            if (bytes[index] != work.pattern) {
                ++work.failures;
            }
        }
        if (!work.allocator->Deallocate(bytes)) {
            ++work.failures;
            return;
        }
    }
}

} // namespace

TEST(CoreDebugAllocatorAlignmentAndOwnership) {
    SystemAllocator backing;
    {
        DebugAllocator allocator(backing, 4096);
        auto empty = allocator.Allocate(0, 8);
        REQUIRE(empty);
        CHECK(empty.Value() == nullptr);
        CHECK(allocator.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(allocator.Allocate(MaxSize, 64).Error() == ErrorCode::Overflow);

        void* addresses[6]{};
        for (usize index = 0; index < 6; ++index) {
            const usize alignment = static_cast<usize>(1) << (index * 2);
            auto allocated = allocator.Allocate(19 + index, alignment, static_cast<uint32>(index));
            REQUIRE(allocated);
            addresses[index] = allocated.Value();
            CHECK(reinterpret_cast<uptr>(addresses[index]) % alignment == 0);
            auto* bytes = static_cast<uint8*>(addresses[index]);
            for (usize offset = 0; offset < 19 + index; ++offset) {
                CHECK(bytes[offset] == 0xCD);
                bytes[offset] = static_cast<uint8>(offset);
            }
        }
        auto clean = allocator.Validate();
        REQUIRE(clean);
        CHECK(clean.Value().issue == DebugMemoryIssue::None);
        CHECK(allocator.Stats().allocations.liveAllocations == 6);

        int32 foreign = 0;
        CHECK(allocator.Deallocate(&foreign).Error() == ErrorCode::NotFound);
        CHECK(allocator.Deallocate(static_cast<uint8*>(addresses[0]) + 1).Error() ==
              ErrorCode::NotFound);
        REQUIRE(allocator.Deallocate(nullptr));
        for (void* address : addresses) {
            REQUIRE(allocator.Deallocate(address));
        }
        CHECK(allocator.Deallocate(addresses[0]).Error() == ErrorCode::NotFound);
        CHECK(allocator.Stats().allocations.liveAllocations == 0);
        CHECK(allocator.Stats().quarantineAllocations == 6);
        REQUIRE(allocator.FlushQuarantine());
        CHECK(allocator.Stats().quarantineAllocations == 0);
        CHECK(allocator.Stats().quarantineBytes == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreDebugAllocatorReportsGuardAndQuarantinedWrite) {
    SystemAllocator backing;
    {
        DebugAllocator allocator(backing, 64);
        auto allocation = allocator.Allocate(24, 16, 219);
        REQUIRE(allocation);
        auto* bytes = static_cast<uint8*>(allocation.Value());

        // 검사 후 원래 guard를 복구해야 정상 반환이 가능하다.
        const uint8 savedFront = bytes[-1];
        bytes[-1] ^= 1;
        auto front = allocator.Validate();
        REQUIRE(front);
        CHECK(front.Value().issue == DebugMemoryIssue::FrontGuard);
        CHECK(front.Value().address == bytes);
        CHECK(front.Value().offset == 15);
        CHECK(front.Value().tag == 219);
        bytes[-1] = savedFront;

        const uint8 savedBack = bytes[24];
        bytes[24] ^= 1;
        auto back = allocator.Validate();
        REQUIRE(back);
        CHECK(back.Value().issue == DebugMemoryIssue::BackGuard);
        CHECK(back.Value().address == bytes);
        CHECK(back.Value().offset == 0);
        CHECK(back.Value().tag == 219);
        bytes[24] = savedBack;

        REQUIRE(allocator.Deallocate(bytes));
        CHECK(allocator.Stats().quarantineBytes == 24);
        CHECK(bytes[7] == 0xDD);
        bytes[7] = 0x71;
        auto freed = allocator.Validate();
        REQUIRE(freed);
        CHECK(freed.Value().issue == DebugMemoryIssue::FreedPayload);
        CHECK(freed.Value().address == bytes);
        CHECK(freed.Value().offset == 7);
        CHECK(freed.Value().tag == 219);
        bytes[7] = 0xDD;
        REQUIRE(allocator.FlushQuarantine());
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreDebugAllocatorEveryBackingAllocationFailureRollsBack) {
    SystemAllocator backing;
    test::NFailAllocator failing(backing);
    {
        DebugAllocator allocator(failing, 64);
        for (uint64 failure = 1; failure <= 2; ++failure) {
            failing.FailOnAttempt(failure);
            CHECK(allocator.Allocate(32, 16).Error() == ErrorCode::OutOfMemory);
            CHECK(allocator.Stats().allocations.liveAllocations == 0);
            CHECK(allocator.Stats().quarantineAllocations == 0);
            CHECK(backing.Stats().liveAllocations == 0);
        }
        failing.DisableFailure();
        auto allocated = allocator.Allocate(32, 16);
        REQUIRE(allocated);
        REQUIRE(allocator.Deallocate(allocated.Value()));
        REQUIRE(allocator.FlushQuarantine());
    }
    CHECK(backing.Stats().liveAllocations == 0);
    CHECK(backing.Stats().liveBytes == 0);
}

TEST(CoreDebugAllocatorQuarantineEvictionAndReleaseFailure) {
    SystemAllocator system;
    FailingReleaseAllocator backing(system);
    {
        DebugAllocator allocator(backing, 16);
        auto first = allocator.Allocate(8, 8, 1);
        REQUIRE(first);
        void* firstStorage = backing.latestStorage;
        auto second = allocator.Allocate(8, 8, 2);
        REQUIRE(second);
        auto third = allocator.Allocate(8, 8, 3);
        REQUIRE(third);
        auto* thirdBytes = static_cast<uint8*>(third.Value());
        thirdBytes[0] = 77;
        REQUIRE(allocator.Deallocate(first.Value()));
        REQUIRE(allocator.Deallocate(second.Value()));
        CHECK(allocator.Stats().quarantineBytes == 16);
        CHECK(allocator.Stats().quarantineAllocations == 2);

        // FIFO eviction 실패는 현재 live 값과 quarantine 기록을 그대로 보존한다.
        backing.failAddress = firstStorage;
        backing.failOnce = true;
        CHECK(allocator.Deallocate(third.Value()).Error() == ErrorCode::DependencyFailed);
        CHECK(thirdBytes[0] == 77);
        CHECK(allocator.Stats().allocations.liveAllocations == 1);
        CHECK(allocator.Stats().quarantineBytes == 16);
        REQUIRE(allocator.Deallocate(third.Value()));
        CHECK(allocator.Stats().allocations.liveAllocations == 0);
        CHECK(allocator.Stats().quarantineBytes == 16);
        CHECK(allocator.Stats().quarantineAllocations == 2);
        REQUIRE(allocator.FlushQuarantine());
    }
    CHECK(system.Stats().liveAllocations == 0);
    CHECK(system.Stats().liveBytes == 0);
}

TEST(CoreDebugAllocatorPartialFlushFailurePreservesRemainingQueue) {
    SystemAllocator system;
    FailingReleaseAllocator backing(system);
    {
        DebugAllocator allocator(backing, 64);
        auto first = allocator.Allocate(7, 8, 1);
        REQUIRE(first);
        auto second = allocator.Allocate(9, 8, 2);
        REQUIRE(second);
        void* secondStorage = backing.latestStorage;

        // 첫 블록 반환은 성공시키고, 두 번째 블록에서만 상위 할당기 오류를 주입한다.
        REQUIRE(allocator.Deallocate(first.Value()));
        REQUIRE(allocator.Deallocate(second.Value()));
        CHECK(allocator.Stats().quarantineAllocations == 2);
        CHECK(allocator.Stats().quarantineBytes == 16);
        backing.failAddress = secondStorage;
        backing.failOnce = true;
        CHECK(allocator.FlushQuarantine().Error() == ErrorCode::DependencyFailed);

        // 이미 반환한 첫 블록은 복구하지 않으며 두 번째 블록과 기록은 재시도할 수 있다.
        CHECK(allocator.Stats().allocations.liveAllocations == 0);
        CHECK(allocator.Stats().quarantineAllocations == 1);
        CHECK(allocator.Stats().quarantineBytes == 9);
        CHECK(system.Stats().liveAllocations == 2);
        auto report = allocator.Validate();
        REQUIRE(report);
        CHECK(report.Value().issue == DebugMemoryIssue::None);
        REQUIRE(allocator.FlushQuarantine());
        CHECK(allocator.Stats().quarantineAllocations == 0);
        CHECK(allocator.Stats().quarantineBytes == 0);
        CHECK(system.Stats().liveAllocations == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
    CHECK(system.Stats().liveBytes == 0);
}

TEST(CoreDebugAllocatorImmediateReleaseFailurePreservesPayload) {
    SystemAllocator system;
    FailingReleaseAllocator backing(system);
    {
        DebugAllocator allocator(backing, 0);
        auto allocation = allocator.Allocate(32, 16);
        REQUIRE(allocation);
        auto* bytes = static_cast<uint8*>(allocation.Value());
        bytes[3] = 81;
        backing.failAddress = backing.latestStorage;
        backing.failOnce = true;
        CHECK(allocator.Deallocate(bytes).Error() == ErrorCode::DependencyFailed);
        CHECK(bytes[3] == 81);
        CHECK(allocator.Stats().allocations.liveAllocations == 1);
        CHECK(allocator.Stats().quarantineAllocations == 0);
        REQUIRE(allocator.Deallocate(bytes));
        CHECK(allocator.Stats().allocations.liveAllocations == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(CoreDebugAllocatorDetectsReentryAndSupportsConcurrentCallers) {
    SystemAllocator system;
    {
        ReentrantDebugBacking backing(system);
        DebugAllocator allocator(backing, 256);
        backing.target = &allocator;
        auto allocation = allocator.Allocate(16, 16);
        REQUIRE(allocation);
        CHECK(backing.allocationReentry == ErrorCode::Busy);
        REQUIRE(allocator.Deallocate(allocation.Value()));
        REQUIRE(allocator.FlushQuarantine());
        CHECK(backing.validationReentry == ErrorCode::Busy);
        backing.target = nullptr;
    }
    {
        DebugAllocator allocator(system, 256);
        DebugThreadWork work[4] = {
            {&allocator, 1}, {&allocator, 2}, {&allocator, 3}, {&allocator, 4}};
        platform::Thread threads[4];
        for (usize index = 0; index < 4; ++index) {
            if (threads[index].Start(UseDebugOnWorker, &work[index]) !=
                platform::ThreadStatus::Success) {
                platform::Fatal("Debug allocator worker failed to start");
            }
        }
        for (auto& thread : threads) {
            if (thread.WaitForCompletion(20000) != platform::WaitResult::Signaled ||
                thread.Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Debug allocator worker did not finish");
            }
        }
        for (const auto& item : work) {
            CHECK(item.failures == 0);
        }
        CHECK(allocator.Stats().allocations.liveAllocations == 0);
        CHECK(allocator.Stats().allocations.totalAllocations == 1000);
        auto clean = allocator.Validate();
        REQUIRE(clean);
        CHECK(clean.Value().issue == DebugMemoryIssue::None);
        REQUIRE(allocator.FlushQuarantine());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

namespace rocks::test {

// 자식 프로세스 전용: 실제 손상과 미반환 할당은 해당 진단 후 즉시 종료해야 한다.
int RunDebugAllocatorNegativeControl(uint32 operation) noexcept {
    PrepareFatalTestProcess();
    SystemAllocator backing;
    DebugAllocator allocator(backing, 128);
    auto allocation = allocator.Allocate(16, 8, 439);
    if (!allocation) {
        return 0;
    }
    auto* bytes = static_cast<uint8*>(allocation.Value());
    if (operation == 0) {
        bytes[-1] ^= 1;
        (void)allocator.Deallocate(bytes);
    } else if (operation == 1) {
        bytes[16] ^= 1;
        (void)allocator.Deallocate(bytes);
    } else if (operation == 2) {
        if (!allocator.Deallocate(bytes)) {
            return 0;
        }
        bytes[0] ^= 1;
        (void)allocator.FlushQuarantine();
    }
    return 0;
}

} // namespace rocks::test
