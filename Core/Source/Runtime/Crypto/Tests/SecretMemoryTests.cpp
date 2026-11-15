#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Crypto/Bytes.h>
#include <Crypto/Private/SecretArena.h>
#include <Crypto/Secret.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>
#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::crypto;
namespace secret = rocks::crypto::detail;

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

// 시험 전후로 비밀 메모리 사용량이 같아야 한다(누수·회계 오류 검출). arena 수는 한 번 늘면 줄지
// 않는다.
bool SameUsage(const SecretMemoryStatus& left, const SecretMemoryStatus& right) {
    return left.arenaUsedBytes == right.arenaUsedBytes &&
           left.bufferAllocations == right.bufferAllocations &&
           left.bufferBytes == right.bufferBytes;
}

} // namespace

TEST(CryptoSecretArenaSlotsAreAlignedAndWipedOnFree) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();

    // 등급 = max(크기, 정렬)을 올린 2의 거듭제곱(최소 64). 슬롯은 등급 크기로 정렬된다.
    struct Case {
        usize bytes;
        usize alignment;
        usize slotBytes;
    };

    const Case cases[] = {{1, 1, 64},      {64, 16, 64},     {65, 8, 128},
                          {100, 256, 256}, {16, 4096, 4096}, {4096, 64, 4096}};
    for (const Case& item : cases) {
        auto first = secret::TryAllocateSecretSlot(item.bytes, item.alignment);
        REQUIRE(first);
        CHECK(IsAligned(first.Value(), item.slotBytes));
        CHECK(GetSecretMemoryStatus().arenaUsedBytes == before.arenaUsedBytes + item.slotBytes);

        // 새 슬롯은 0이다(commit된 새 페이지이거나 해제 때 지운 슬롯). 요청 크기만 쓴다(Asan은
        // 꼬리를 막는다).
        CHECK(IsFilledWith(first.Value(), item.bytes, 0));
        platform::MemorySet(first.Value(), 0xAB, item.bytes);
        void* const address = first.Value();
        secret::FreeSecretSlot(address);

        // 같은 등급의 다음 할당은 방금 비운 가장 낮은 슬롯을 다시 준다. 지워져 있어야 한다.
        auto second = secret::TryAllocateSecretSlot(item.bytes, item.alignment);
        REQUIRE(second);
        CHECK(second.Value() == address);
        CHECK(IsFilledWith(second.Value(), item.bytes, 0));
        secret::FreeSecretSlot(second.Value());
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretArenaRejectsInvalidRequests) {
    CHECK(secret::TryAllocateSecretSlot(0, 64).Error() == ErrorCode::InvalidArgument);
    CHECK(secret::TryAllocateSecretSlot(4097, 64).Error() == ErrorCode::InvalidArgument);
    CHECK(secret::TryAllocateSecretSlot(16, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(secret::TryAllocateSecretSlot(16, 3).Error() == ErrorCode::InvalidArgument);
    CHECK(secret::TryAllocateSecretSlot(16, 8192).Error() == ErrorCode::InvalidArgument);
    secret::FreeSecretSlot(nullptr);
}

TEST(CryptoSecretArenaGrowsInsteadOfFallingBack) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    REQUIRE(before.arenaCount < secret::MaxSecretArenas);

    // 4 KiB 슬롯을 지금 있는 arena가 모두 찰 때까지 잡으면 새 arena가 덧붙는다(힙으로 넘기지
    // 않는다).
    const usize limit = (before.arenaCount + 1) * (secret::SecretArenaBytes / 4096);
    void* slots[(secret::MaxSecretArenas + 1) * (secret::SecretArenaBytes / 4096)] = {};
    usize count = 0;
    while (count < limit && GetSecretMemoryStatus().arenaCount == before.arenaCount) {
        auto slot = secret::TryAllocateSecretSlot(4096, 64);
        REQUIRE(slot);
        slots[count++] = slot.Value();
    }

    const SecretMemoryStatus grown = GetSecretMemoryStatus();
    CHECK(grown.arenaCount == before.arenaCount + 1);
    CHECK(grown.arenaCapacityBytes == grown.arenaCount * secret::SecretArenaBytes);
    CHECK(grown.bufferAllocations == before.bufferAllocations);

    // 마지막 슬롯은 새 arena 안에 있다.
    const uint8* begin = nullptr;
    usize bytes = 0;
    REQUIRE(secret::TryGetSecretArenaRange(grown.arenaCount - 1, begin, bytes));
    const uptr last = reinterpret_cast<uptr>(slots[count - 1]);
    CHECK(bytes == secret::SecretArenaBytes && last >= reinterpret_cast<uptr>(begin) &&
          last < reinterpret_cast<uptr>(begin) + bytes);
    CHECK(!secret::TryGetSecretArenaRange(grown.arenaCount, begin, bytes));

    for (usize i = 0; i < count; ++i) {
        secret::FreeSecretSlot(slots[i]);
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretMemoryStatusReportsLocking) {
    auto slot = secret::TryAllocateSecretSlot(32, 64);
    REQUIRE(slot);
    const SecretMemoryStatus status = GetSecretMemoryStatus();
    CHECK(status.arenaCount >= 1);
    CHECK(status.arenaLockedBytes <= status.arenaCapacityBytes);
    CHECK(status.arenaUsedBytes <= status.arenaCapacityBytes);
    CHECK(status.dumpExclusionRequested == IsAndroid);
    // Windows(작업 집합 최소값)와 macOS는 첫 arena 64 KiB를 잠글 수 있어야 한다. Android는
    // RLIMIT_MEMLOCK이 작은 기기가 있어 단언하지 않는다(best-effort). macOS Asan은 sanitizer
    // runtime이 mlock을 가로채 잠그지 않고 성공을 돌려주므로 이 단언이 의미가 없어 뺀다.
    if constexpr (IsWindows || (IsMac && !ROCKS_ADDRESS_SANITIZER)) {
        CHECK(status.arenaLockedBytes >= secret::SecretArenaBytes);
    }

    secret::FreeSecretSlot(slot.Value());
}

TEST(CryptoSecretBufferAllocatorContract) {
    Allocator& allocator = GetSecretAllocator();
    const SecretMemoryStatus before = GetSecretMemoryStatus();

    // 최소 64 byte 정렬이고, 더 큰 정렬도 지킨다. 키 저장소는 쓰지 않는다.
    auto small = allocator.Allocate(16, 1);
    REQUIRE(small);
    CHECK(IsAligned(small.Value(), 64));
    auto aligned = allocator.Allocate(16, 8192);
    REQUIRE(aligned);
    CHECK(IsAligned(aligned.Value(), 8192));
    auto large = allocator.Allocate(100000, 16);
    REQUIRE(large);

    const SecretMemoryStatus during = GetSecretMemoryStatus();
    CHECK(during.arenaUsedBytes == before.arenaUsedBytes);
    CHECK(during.bufferAllocations == before.bufferAllocations + 3);
    CHECK(during.bufferBytes == before.bufferBytes + 16 + 16 + 100000);

    platform::MemorySet(large.Value(), 0xCD, 100000);
    REQUIRE(allocator.Deallocate(small.Value()));
    REQUIRE(allocator.Deallocate(aligned.Value()));
    REQUIRE(allocator.Deallocate(large.Value()));
    CHECK(SameUsage(GetSecretMemoryStatus(), before));

    // 잘못된 인자와 크기 overflow.
    CHECK(allocator.Allocate(16, 0).Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Allocate(16, 3).Error() == ErrorCode::InvalidArgument);
    CHECK(allocator.Allocate(0, 3).Error() == ErrorCode::InvalidArgument);
    auto empty = allocator.Allocate(0, 8);
    REQUIRE(empty);
    CHECK(empty.Value() == nullptr);
    CHECK(allocator.Allocate(MaxSize - 8, 64).Error() == ErrorCode::Overflow);
    CHECK(allocator.Deallocate(nullptr));

    // 이 할당기 것이 아닌 주소는 역참조하지 않고 NotFound다: 스택 주소, 블록 중간 주소, 이미 해제한
    // 블록.
    uint8 local[64] = {};
    CHECK(allocator.Deallocate(local).Error() == ErrorCode::NotFound);
    auto block = allocator.Allocate(256, 64);
    REQUIRE(block);
    CHECK(allocator.Deallocate(static_cast<uint8*>(block.Value()) + 64).Error() ==
          ErrorCode::NotFound);
    REQUIRE(allocator.Deallocate(block.Value()));
    CHECK(allocator.Deallocate(block.Value()).Error() == ErrorCode::NotFound);

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

namespace {

struct SecretMemoryWork {
    bool ok = true;
};

// 키 슬롯(64 B–4 KiB)과 버퍼(64 B–8 KiB)를 섞어 두 저장소를 동시에 쓴다.
void RunSecretMemoryWork(void* context) noexcept {
    SecretMemoryWork& work = *static_cast<SecretMemoryWork*>(context);
    Allocator& allocator = GetSecretAllocator();
    for (uint32 round = 0; round < 200; ++round) {
        void* slots[4] = {};
        void* buffers[4] = {};
        for (usize i = 0; i < 4; ++i) {
            const usize slotBytes = usize{64} << ((round + i) % 7);
            const usize bufferBytes = usize{64} << ((round + i) % 8);
            auto slot = secret::TryAllocateSecretSlot(slotBytes, 16);
            auto buffer = allocator.Allocate(bufferBytes, 16);
            if (!slot || !buffer) {
                work.ok = false;
                return;
            }

            slots[i] = slot.Value();
            buffers[i] = buffer.Value();
            platform::MemorySet(slots[i], static_cast<uint8>(i + 1), slotBytes);
            platform::MemorySet(buffers[i], static_cast<uint8>(i + 5), bufferBytes);
        }

        for (usize i = 0; i < 4; ++i) {
            const usize slotBytes = usize{64} << ((round + i) % 7);
            const usize bufferBytes = usize{64} << ((round + i) % 8);
            if (!IsFilledWith(slots[i], slotBytes, static_cast<uint8>(i + 1)) ||
                !IsFilledWith(buffers[i], bufferBytes, static_cast<uint8>(i + 5)) ||
                !allocator.Deallocate(buffers[i])) {
                work.ok = false;
            }

            secret::FreeSecretSlot(slots[i]);
        }
    }
}

} // namespace

TEST(CryptoSecretMemoryConcurrentUse) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    SecretMemoryWork work[4];
    platform::Thread threads[4];
    usize started = 0;
    for (; started < 4; ++started) {
        if (threads[started].Start(RunSecretMemoryWork, &work[started]) !=
            platform::ThreadStatus::Success) {
            break;
        }
    }

    // 시작한 스레드는 실패해도 모두 join한다(joinable 상태로 소멸하면 Fatal이다).
    bool joined = true;
    for (usize i = 0; i < started; ++i) {
        joined = threads[i].Join() == platform::ThreadStatus::Success && joined;
        CHECK(work[i].ok);
    }

    CHECK(started == 4);
    CHECK(joined);
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
        // 4 KiB 이하는 키 저장소다.
        CHECK(GetSecretMemoryStatus().arenaUsedBytes == before.arenaUsedBytes + 64);

        auto random = SecretBytes::TryGenerate(32);
        REQUIRE(random);
        CHECK(!IsAllZero(random.Value().View()));

        auto copy = SecretBytes::TryCopyFrom(random.Value().View());
        REQUIRE(copy);
        CHECK(ConstantTimeEquals(copy.Value().View(), random.Value().View()));
        CHECK(copy.Value().View().Data() != random.Value().View().Data());

        // 이동은 저장소를 넘기고 원본을 비운다. 자기 자신으로의 이동 대입은 바꾸지 않는다.
        const uint8* storage = copy.Value().View().Data();
        SecretBytes moved(Move(copy.Value()));
        CHECK(copy.Value().IsEmpty());
        CHECK(copy.Value().View().Data() == nullptr);
        CHECK(moved.View().Data() == storage);
        SecretBytes& alias = moved;
        moved = Move(alias);
        CHECK(moved.View().Data() == storage && moved.Size() == 32);

        moved = Move(random.Value());
        CHECK(random.Value().IsEmpty());
        CHECK(moved.Size() == 32);
        moved.Reset();
        CHECK(moved.IsEmpty());

        // 4 KiB를 넘는 비밀은 버퍼 할당기로 가도 같은 계약이다.
        auto large = SecretBytes::TryCreate(10000);
        REQUIRE(large);
        CHECK(IsAllZero(large.Value().View()));
        CHECK(IsAligned(large.Value().View().Data(), 64));
        CHECK(GetSecretMemoryStatus().bufferAllocations == before.bufferAllocations + 1);

        auto empty = SecretBytes::TryCreate(0);
        REQUIRE(empty);
        CHECK(empty.Value().IsEmpty());
        auto emptyCopy = SecretBytes::TryCopyFrom(Span<const uint8>());
        REQUIRE(emptyCopy);
        CHECK(emptyCopy.Value().IsEmpty());
        CHECK(SecretBytes::TryCreate(MaxSize).Error() == ErrorCode::Overflow);
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}

TEST(CryptoSecretAllocatorBacksGrowingArray) {
    const SecretMemoryStatus before = GetSecretMemoryStatus();
    {
        // 성장할 때마다 이전 버퍼를 해제하며 지운다.
        Array<uint8> plaintext(GetSecretAllocator());
        for (uint32 i = 0; i < 10000; ++i) {
            REQUIRE(plaintext.TryPushBack(static_cast<uint8>(i)));
        }

        CHECK(plaintext.Size() == 10000);
        CHECK(plaintext[9999] == static_cast<uint8>(9999));
        CHECK(GetSecretMemoryStatus().bufferAllocations == before.bufferAllocations + 1);
    }

    CHECK(SameUsage(GetSecretMemoryStatus(), before));
}
