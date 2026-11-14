#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Crypto/Bytes.h>
#include <Crypto/Secret.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::crypto;

namespace {

bool IsAligned(const void* pointer, usize alignment) {
    return reinterpret_cast<uptr>(pointer) % alignment == 0;
}

bool IsFilledWith(const void* pointer, usize bytes, uint8 value) {
    const uint8* data = static_cast<const uint8*>(pointer);
    for (usize i = 0; i < bytes; ++i) {
        if (data[i] != value) {
            return false;
        }
    }

    return true;
}

// 시험 전후로 비밀 메모리 사용량이 같아야 한다(누수·회계 오류 검출).
bool SameUsage(const SecretMemoryStatus& left, const SecretMemoryStatus& right) {
    return left.arenaUsedBytes == right.arenaUsedBytes &&
           left.heapAllocations == right.heapAllocations && left.heapBytes == right.heapBytes;
}

} // namespace

TEST(CryptoSecretArenaIsCreated) {
    const SecretMemoryStatus status = GetSecretMemoryStatus();
    // 세 대상 모두 예약·commit이 되어야 한다. 잠금은 기기 한도에 따라 실패할 수 있어 단언하지
    // 않는다(best-effort).
    REQUIRE(status.arenaAvailable);
    CHECK(status.arenaCapacityBytes == 64 * 1024);
    CHECK(status.arenaUsedBytes <= status.arenaCapacityBytes);
    if constexpr (IsAndroid) {
        CHECK(status.dumpExclusionRequested);
    } else {
        CHECK(!status.dumpExclusionRequested);
    }
}

TEST(CryptoSecretAllocatorSlotsAreAlignedAndWipedOnFree) {
    Allocator& allocator = GetSecretAllocator();
    const SecretMemoryStatus before = GetSecretMemoryStatus();

    // 크기 등급: 1→64, 64→64, 65→128, 100(정렬 256)→256, 4096→4096. 슬롯은 등급 크기로 정렬된다.
    struct Case {
        usize bytes;
        usize alignment;
        usize slotBytes;
    };

    const Case cases[] = {
        {1, 1, 64}, {64, 16, 64}, {65, 8, 128}, {100, 256, 256}, {4096, 64, 4096}};
    for (const Case& item : cases) {
        auto first = allocator.Allocate(item.bytes, item.alignment);
        REQUIRE(first);
        CHECK(IsAligned(first.Value(), item.slotBytes));
        CHECK(GetSecretMemoryStatus().arenaUsedBytes == before.arenaUsedBytes + item.slotBytes);

        // 새 슬롯은 0이다(commit된 새 페이지이거나 해제 때 지운 슬롯).
        CHECK(IsFilledWith(first.Value(), item.slotBytes, 0));
        platform::MemorySet(first.Value(), 0xAB, item.slotBytes);
        void* const address = first.Value();
        REQUIRE(allocator.Deallocate(address));

        // 같은 등급의 다음 할당은 방금 비운 가장 낮은 슬롯을 다시 준다. 슬롯 전체가 지워져 있어야
        // 한다.
        auto second = allocator.Allocate(item.bytes, item.alignment);
        REQUIRE(second);
        CHECK(second.Value() == address);
        CHECK(IsFilledWith(second.Value(), item.slotBytes, 0));
        REQUIRE(allocator.Deallocate(second.Value()));
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretAllocatorFallsBackToHeap) {
    Allocator& allocator = GetSecretAllocator();
    const SecretMemoryStatus before = GetSecretMemoryStatus();

    // 4 KiB를 넘는 크기와 4 KiB를 넘는 정렬은 힙으로 간다. 힙도 최소 64 byte 정렬이다.
    auto large = allocator.Allocate(4097, 1);
    REQUIRE(large);
    CHECK(IsAligned(large.Value(), 64));
    auto aligned = allocator.Allocate(16, 8192);
    REQUIRE(aligned);
    CHECK(IsAligned(aligned.Value(), 8192));

    SecretMemoryStatus during = GetSecretMemoryStatus();
    CHECK(during.arenaUsedBytes == before.arenaUsedBytes);
    CHECK(during.heapAllocations == before.heapAllocations + 2);
    CHECK(during.heapBytes == before.heapBytes + 4097 + 16);

    platform::MemorySet(large.Value(), 0xCD, 4097);
    REQUIRE(allocator.Deallocate(large.Value()));
    REQUIRE(allocator.Deallocate(aligned.Value()));
    CHECK(SameUsage(GetSecretMemoryStatus(), before));

    // arena가 차면 같은 크기도 힙으로 간다. 4 KiB 슬롯 16개 + 1개를 잡는다(이미 쓰고 있는 chunk가
    // 있으면 더 일찍 넘친다).
    void* blocks[17] = {};
    for (void*& block : blocks) {
        auto memory = allocator.Allocate(4096, 64);
        REQUIRE(memory);
        block = memory.Value();
    }

    during = GetSecretMemoryStatus();
    CHECK(during.heapAllocations > before.heapAllocations);
    CHECK(during.arenaUsedBytes <= during.arenaCapacityBytes);
    for (void* block : blocks) {
        REQUIRE(allocator.Deallocate(block));
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretAllocatorRejectsForeignAndInvalidRequests) {
    Allocator& allocator = GetSecretAllocator();
    const SecretMemoryStatus before = GetSecretMemoryStatus();

    CHECK(allocator.Allocate(16, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Allocate(16, 3).Error() == ErrorCode::InvalidArgument);
    auto empty = allocator.Allocate(0, 8);
    REQUIRE(empty);
    CHECK(empty.Value() == nullptr);
    CHECK(allocator.Allocate(MaxSize - 8, 64).Error() == ErrorCode::Overflow);
    CHECK(allocator.Deallocate(nullptr));

    // 이 할당기 것이 아닌 주소는 역참조하지 않고 NotFound다: 스택 주소, 슬롯 중간 주소, 이미 해제한
    // 슬롯·힙 블록.
    uint8 local[64] = {};
    CHECK(allocator.Deallocate(local).Error() == ErrorCode::NotFound);

    auto slot = allocator.Allocate(128, 64);
    REQUIRE(slot);
    CHECK(allocator.Deallocate(static_cast<uint8*>(slot.Value()) + 64).Error() ==
          ErrorCode::NotFound);
    REQUIRE(allocator.Deallocate(slot.Value()));
    CHECK(allocator.Deallocate(slot.Value()).Error() == ErrorCode::NotFound);

    auto block = allocator.Allocate(8192, 64);
    REQUIRE(block);
    REQUIRE(allocator.Deallocate(block.Value()));
    CHECK(allocator.Deallocate(block.Value()).Error() == ErrorCode::NotFound);

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

namespace {

struct SecretAllocatorWork {
    bool ok = true;
};

void RunSecretAllocatorWork(void* context) noexcept {
    SecretAllocatorWork& work = *static_cast<SecretAllocatorWork*>(context);
    Allocator& allocator = GetSecretAllocator();
    for (uint32 round = 0; round < 200; ++round) {
        void* blocks[8] = {};
        for (usize i = 0; i < 8; ++i) {
            // 64 B–8 KiB를 섞어 arena 슬롯과 힙을 함께 쓴다.
            const usize bytes = usize{64} << ((round + i) % 8);
            auto memory = allocator.Allocate(bytes, 16);
            if (!memory) {
                work.ok = false;
                return;
            }

            blocks[i] = memory.Value();
            platform::MemorySet(blocks[i], static_cast<uint8>(i + 1), bytes);
        }

        for (usize i = 0; i < 8; ++i) {
            const usize bytes = usize{64} << ((round + i) % 8);
            if (!IsFilledWith(blocks[i], bytes, static_cast<uint8>(i + 1)) ||
                !allocator.Deallocate(blocks[i])) {
                work.ok = false;
            }
        }
    }
}

} // namespace

TEST(CryptoSecretAllocatorConcurrentUse) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    SecretAllocatorWork work[4];
    platform::Thread threads[4];
    for (usize i = 0; i < 4; ++i) {
        REQUIRE(threads[i].Start(RunSecretAllocatorWork, &work[i]) ==
                platform::ThreadStatus::Success);
    }

    for (usize i = 0; i < 4; ++i) {
        REQUIRE(threads[i].Join() == platform::ThreadStatus::Success);
        CHECK(work[i].ok);
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretBytesOwnsAndWipes) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    {
        auto zero = SecretBytes::TryCreate(48);
        REQUIRE(zero);
        CHECK(zero.Value().Size() == 48);
        CHECK(IsAllZero(zero.Value().View()));
        CHECK(IsAligned(zero.Value().View().Data(), 64));

        auto random = SecretBytes::TryGenerate(32);
        REQUIRE(random);
        CHECK(!IsAllZero(random.Value().View()));

        auto copy = SecretBytes::TryCopyFrom(random.Value().View());
        REQUIRE(copy);
        CHECK(ConstantTimeEquals(copy.Value().View(), random.Value().View()));
        CHECK(copy.Value().View().Data() != random.Value().View().Data());

        // 이동은 저장소를 넘기고 원본을 비운다.
        const uint8* storage = copy.Value().View().Data();
        SecretBytes moved(Move(copy.Value()));
        CHECK(copy.Value().IsEmpty());
        CHECK(copy.Value().View().Data() == nullptr);
        CHECK(moved.View().Data() == storage);

        moved = Move(random.Value());
        CHECK(random.Value().IsEmpty());
        CHECK(moved.Size() == 32);
        moved.Reset();
        CHECK(moved.IsEmpty());

        // 4 KiB를 넘는 비밀은 힙으로 가도 같은 계약이다.
        auto large = SecretBytes::TryCreate(10000);
        REQUIRE(large);
        CHECK(IsAllZero(large.Value().View()));
        CHECK(GetSecretMemoryStatus().heapAllocations == before.heapAllocations + 1);

        auto empty = SecretBytes::TryCreate(0);
        REQUIRE(empty);
        CHECK(empty.Value().IsEmpty());
        auto emptyCopy = SecretBytes::TryCopyFrom(Span<const uint8>());
        REQUIRE(emptyCopy);
        CHECK(emptyCopy.Value().IsEmpty());
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretAllocatorBacksGrowingArray) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    {
        // 성장 재할당이 arena 슬롯에서 시작해 힙으로 넘어간다. 버려지는 이전 버퍼는 해제 때
        // 지워진다.
        Array<uint8> plaintext(GetSecretAllocator());
        for (uint32 i = 0; i < 10000; ++i) {
            REQUIRE(plaintext.TryPushBack(static_cast<uint8>(i)));
        }

        CHECK(plaintext.Size() == 10000);
        CHECK(plaintext[9999] == static_cast<uint8>(9999));
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}
