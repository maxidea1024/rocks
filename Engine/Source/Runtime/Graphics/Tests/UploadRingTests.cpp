#include <Core/Memory/SystemAllocator.h>
#include <Graphics/StagingPool.h>
#include <Graphics/UploadRing.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::graphics;

namespace {

// CPU 메모리 staging과 만든·지운 수 기록이다.
class CountingSource final : public StagingSource {
public:
    explicit CountingSource(Allocator& allocator) noexcept : allocator_(allocator) {
    }

    Result<StagingMemory> CreateStaging(uint64 bytes) noexcept override {
        auto memory = allocator_.Allocate(static_cast<usize>(bytes), 16);
        if (!memory) {
            return Result<StagingMemory>::Failure(memory.Error());
        }

        ++created;
        StagingMemory staging;
        staging.resource = memory.Value();
        staging.data = static_cast<uint8*>(memory.Value());
        staging.size = bytes;
        return Result<StagingMemory>::Success(staging);
    }

    void DestroyStaging(const StagingMemory& memory) noexcept override {
        ++destroyed;
        (void)allocator_.Deallocate(memory.resource);
    }

    uint32 created = 0;
    uint32 destroyed = 0;

private:
    Allocator& allocator_;
};

} // namespace

TEST(UploadRing_RegionsAreNotReusedBeforeTheirFence) {
    SystemAllocator allocator;
    UploadRingAllocator ring(allocator);
    REQUIRE(ring.Reset(1024));
    auto first = ring.Allocate(400, 16);
    REQUIRE(first);
    CHECK(first.Value() == 0);
    ring.CloseBatch(1);
    auto second = ring.Allocate(400, 16);
    REQUIRE(second);
    CHECK(second.Value() == 400);
    ring.CloseBatch(2);
    // 남은 224 byte로는 300 byte를 받을 수 없고, 앞 구간은 fence 1 전이라 감을 수 없다(fence gate).
    CHECK(ring.Allocate(300, 16).Error() == ErrorCode::CapacityExceeded);
    CHECK(ring.OldestFence() == 1);
    ring.Reclaim(0);
    CHECK(ring.Allocate(300, 16).Error() == ErrorCode::CapacityExceeded);

    // fence 1이 끝나면 앞 구간을 다시 쓴다. 끝 칸을 버리고 0으로 감는다.
    ring.Reclaim(1);
    auto wrapped = ring.Allocate(300, 16);
    REQUIRE(wrapped);
    CHECK(wrapped.Value() == 0);
    CHECK(ring.Wraps() == 1);
    // 열린(닫히지 않은) 구간은 어떤 fence로도 회수하지 않는다.
    ring.Reclaim(1000);
    CHECK(ring.HasOpenAllocations());
    CHECK(ring.Allocate(200, 16).Error() == ErrorCode::CapacityExceeded ||
          ring.UsedBytes() <= ring.Capacity());
    ring.CloseBatch(3);
    ring.Reclaim(3);
    CHECK(ring.UsedBytes() == 0);
}

TEST(UploadRing_AlignmentAndLimits) {
    SystemAllocator allocator;
    UploadRingAllocator ring(allocator);
    CHECK(ring.Reset(0).Error() == ErrorCode::InvalidArgument);
    REQUIRE(ring.Reset(4096));
    CHECK(ring.Allocate(0, 16).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.Allocate(8192, 16).Error() == ErrorCode::InvalidArgument);
    CHECK(ring.Allocate(16, 3).Error() == ErrorCode::InvalidArgument);
    auto small = ring.Allocate(10, 16);
    auto aligned = ring.Allocate(100, 512);
    REQUIRE(small && aligned);
    CHECK(aligned.Value() == 512);
    CHECK(ring.UsedBytes() == 612);
    ring.CloseBatch(5);
    ring.Reclaim(5);
    CHECK(ring.UsedBytes() == 0);

    // 가득 차면 감을 자리가 생길 때까지 거부한다.
    REQUIRE(ring.Reset(4096));
    for (uint32 index = 0; index < 4; ++index) {
        REQUIRE(ring.Allocate(1024, 16));
        ring.CloseBatch(10 + index);
    }

    CHECK(ring.Allocate(16, 16).Error() == ErrorCode::CapacityExceeded);
    ring.Reclaim(10);
    auto reused = ring.Allocate(1024, 16);
    REQUIRE(reused);
    CHECK(reused.Value() == 0);
}

TEST(UploadRing_KeepsAdvancingAfterItEmpties) {
    SystemAllocator allocator;
    UploadRingAllocator ring(allocator);
    REQUIRE(ring.Reset(1024));
    // 묶음마다 다 회수돼도 위치는 이어 가며, 끝에 자리가 없으면 0으로 감는다. GPU 완료 시점과
    // 상관없이 같은 위치가 나온다.
    const uint64 expected[3] = {0, 400, 0};
    for (uint64 index = 0; index < 3; ++index) {
        auto offset = ring.Allocate(400, 16);
        REQUIRE(offset);
        CHECK(offset.Value() == expected[index]);
        ring.CloseBatch(index + 1);
        ring.Reclaim(index + 1);
        CHECK(ring.UsedBytes() == 0);
    }

    CHECK(ring.Wraps() == 1);
    // 빈 링은 용량 전체를 한 번에 줄 수 있다.
    auto whole = ring.Allocate(1024, 16);
    REQUIRE(whole);
    CHECK(whole.Value() == 0);
    CHECK(ring.Wraps() == 2);
}

TEST(StagingPool_ReclaimsOnlyAfterTheUploadFence) {
    SystemAllocator allocator;
    CountingSource source(allocator);
    StagingPool pool(allocator, source, 1 << 20);
    auto staging = pool.Acquire(4096);
    REQUIRE(staging);
    CHECK(staging.Value().data != nullptr && staging.Value().size == 4096);
    CHECK(pool.Find(staging.Value().handle));
    REQUIRE(pool.MarkPending(staging.Value().handle));
    pool.Release(staging.Value().handle);
    CHECK(pool.Find(staging.Value().handle).Error() == ErrorCode::StaleHandle);

    // 업로드 묶음이 제출되기 전에는 어떤 완료 값으로도 재사용 목록에 가지 않는다.
    pool.Reclaim(1000);
    CHECK(pool.Stats().waiting == 1);
    pool.AssignPending(7);
    pool.Reclaim(6);
    CHECK(pool.Stats().waiting == 1);
    auto other = pool.Acquire(4096);
    REQUIRE(other);
    CHECK(source.created == 2);
    pool.Release(other.Value().handle);
    pool.Reclaim(7);
    CHECK(pool.Stats().waiting == 0 && pool.Stats().free == 2);

    // 빈 staging을 다시 쓰며 handle은 새로 발급한다.
    auto again = pool.Acquire(3000);
    REQUIRE(again);
    CHECK(source.created == 2);
    CHECK(pool.Stats().reused == 1);
    CHECK(!(again.Value().handle == staging.Value().handle));
    // 두 배보다 작은 요청은 큰 staging을 다시 쓰지 않는다.
    auto tiny = pool.Acquire(16);
    REQUIRE(tiny);
    CHECK(source.created == 3);
}

TEST(StagingPool_BudgetTrimsFreeStagingFirst) {
    SystemAllocator allocator;
    CountingSource source(allocator);
    StagingPool pool(allocator, source, 8192);
    CHECK(pool.Acquire(0).Error() == ErrorCode::InvalidArgument);
    CHECK(pool.Acquire(8193).Error() == ErrorCode::InvalidArgument);
    auto first = pool.Acquire(4096);
    auto second = pool.Acquire(4096);
    REQUIRE(first && second);
    CHECK(pool.Acquire(1024).Error() == ErrorCode::CapacityExceeded);
    pool.Release(first.Value().handle);
    pool.Reclaim(0);
    // 빈 staging을 지워 자리를 만든다(1024는 4096을 재사용하지 않는다).
    auto third = pool.Acquire(1024);
    REQUIRE(third);
    CHECK(source.destroyed == 1);
    CHECK(pool.Stats().createdBytes == 4096 + 1024);
    pool.Clear();
    CHECK(source.destroyed == 3);
}
