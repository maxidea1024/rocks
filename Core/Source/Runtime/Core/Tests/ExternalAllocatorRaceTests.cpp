#include <Core/Memory/ExternalAllocator.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

// 실제 storage는 fixture의 수명 동안 존재한다. 첫 반환이 논리적으로 끝난 뒤에만
// 공급자가 같은 저장소 또는 일부 영역을 다시 발급하도록 동기화한다.
struct ReuseBackend {
    alignas(64) uint8 storage[128]{};
    platform::Mutex mutex;
    platform::ManualResetEvent firstReleaseEntered;
    platform::ManualResetEvent finishFirstRelease;
    platform::ManualResetEvent secondIssued;
    uint32 allocationCount = 0;
    uint32 releaseCount = 0;
    bool usePartialOverlap = false;
};

Result<void*> ReuseAllocate(void* raw, usize bytes, usize alignment, uint32) noexcept {
    auto& backend = *static_cast<ReuseBackend*>(raw);
    if (bytes > 64 || alignment > 16) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    uint32 index = 0;
    {
        platform::LockGuard lock(backend.mutex);
        index = ++backend.allocationCount;
    }

    if (index == 1) {
        return Result<void*>::Success(backend.storage);
    }
    if (index == 2) {
        backend.secondIssued.Signal();
        return Result<void*>::Success(backend.storage + (backend.usePartialOverlap ? 16 : 0));
    }
    return Result<void*>::Failure(ErrorCode::CapacityExceeded);
}

Result<void> ReuseDeallocate(void* raw, void*) noexcept {
    auto& backend = *static_cast<ReuseBackend*>(raw);
    uint32 index = 0;
    {
        platform::LockGuard lock(backend.mutex);
        index = ++backend.releaseCount;
    }

    if (index == 1) {
        // 이 시점에 backing은 블록을 논리적으로 반환했지만 callback은 아직 끝나지 않았다.
        backend.firstReleaseEntered.Signal();
        if (backend.finishFirstRelease.Wait(10000) != platform::WaitResult::Signaled) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }
    }
    return Result<void>::Success();
}

struct ReleaseWork {
    ExternalAllocator* allocator;
    void* address;
    ErrorCode result = ErrorCode::None;
};

void ReleaseFirst(void* raw) noexcept {
    auto& work = *static_cast<ReleaseWork*>(raw);
    work.result = work.allocator->Deallocate(work.address).Error();
}

struct AllocateWork {
    explicit AllocateWork(ExternalAllocator& owner) noexcept : allocator(&owner), finished(false) {
    }

    ExternalAllocator* allocator;
    platform::ManualResetEvent finished;
    void* address = nullptr;
    ErrorCode result = ErrorCode::None;
};

void AllocateAgain(void* raw) noexcept {
    auto& work = *static_cast<AllocateWork*>(raw);
    auto result = work.allocator->Allocate(32, 16);
    if (result) {
        work.address = result.Value();
    } else {
        work.result = result.Error();
    }
    work.finished.Signal();
}

void JoinOrFatal(platform::Thread& thread) noexcept {
    if (thread.WaitForCompletion(10000) != platform::WaitResult::Signaled ||
        thread.Join() != platform::ThreadStatus::Success) {
        platform::Fatal("External allocator reuse worker did not finish");
    }
}

void CheckReuse(bool partial) noexcept {
    ReuseBackend fixture;
    fixture.usePartialOverlap = partial;
    ExternalAllocatorBackend backend;
    backend.context = &fixture;
    backend.allocate = ReuseAllocate;
    backend.deallocate = ReuseDeallocate;
    ExternalAllocator allocator(Move(backend));

    auto first = allocator.Allocate(64, 16);
    REQUIRE(first);
    REQUIRE(first.Value() == fixture.storage);

    ReleaseWork release{&allocator, first.Value()};
    platform::Thread releaseThread;
    if (releaseThread.Start(ReleaseFirst, &release) != platform::ThreadStatus::Success) {
        platform::Fatal("External allocator release worker failed to start");
    }
    if (fixture.firstReleaseEntered.Wait(10000) != platform::WaitResult::Signaled) {
        fixture.finishFirstRelease.Signal();
        JoinOrFatal(releaseThread);
        platform::Fatal("External allocator release callback was not reached");
    }

    AllocateWork allocation(allocator);
    platform::Thread allocationThread;
    if (allocationThread.Start(AllocateAgain, &allocation) != platform::ThreadStatus::Success) {
        fixture.finishFirstRelease.Signal();
        JoinOrFatal(releaseThread);
        platform::Fatal("External allocator reuse worker failed to start");
    }
    if (fixture.secondIssued.Wait(10000) != platform::WaitResult::Signaled) {
        fixture.finishFirstRelease.Signal();
        JoinOrFatal(releaseThread);
        JoinOrFatal(allocationThread);
        platform::Fatal("External allocator second allocation was not issued");
    }

    // 아직 이전 callback을 끝내지 않았으므로 같은 영역의 새 주소를 게시해서는 안 된다.
    CHECK(allocation.finished.Wait(50) == platform::WaitResult::TimedOut);
    CHECK(allocator.Stats().liveAllocations == 1);
    fixture.finishFirstRelease.Signal();
    JoinOrFatal(releaseThread);
    JoinOrFatal(allocationThread);

    CHECK(release.result == ErrorCode::None);
    CHECK(allocation.result == ErrorCode::None);
    CHECK(allocation.address == fixture.storage + (partial ? 16 : 0));
    if (allocation.address) {
        CHECK(allocator.Stats().liveAllocations == 1);
        REQUIRE(allocator.Deallocate(allocation.address));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(allocator.Stats().liveBytes == 0);
    CHECK(fixture.releaseCount == 2);
}

} // namespace

TEST(CoreExternalAllocatorWaitsForSameAddressReuse) {
    CheckReuse(false);
}

TEST(CoreExternalAllocatorWaitsForPartiallyOverlappingReuse) {
    CheckReuse(true);
}
