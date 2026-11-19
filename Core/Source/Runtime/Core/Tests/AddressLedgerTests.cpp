#include <TestSupport/Test.h>

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Private/Memory/AddressLedger.h>
#include <Core/Private/Memory/SystemAllocatorTestAccess.h>

// 할당자 공용 주소 원장과 SystemAllocator의 원장 경로다(Docs/Design/GarbageCollectionScale.ko.md
// 18.6절).
namespace {

using namespace rocks;
using rocks::detail::AddressLedger;
using rocks::detail::SystemAllocatorTestAccess;
using rocks::detail::UntrackedPlatformAllocator;

// 원장은 주소를 역참조하지 않으므로 시험은 지어낸 주소 값을 쓴다.
const void* FakeAddress(uptr value) noexcept {
    return reinterpret_cast<const void*>(value);
}

// 결정적 의사 난수다(seed 고정).
struct LedgerRng {
    uint64 state;

    uint64 Next() noexcept {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 17;
    }
};

} // namespace

// 넣은 주소만 찾고, 지운 주소·없는 주소·null은 찾지 못한다. 같은 주소를 두 번 넣으면
// InvalidArgument다.
TEST(CoreAddressLedgerInsertRemoveAndDuplicates) {
    UntrackedPlatformAllocator storage;
    AddressLedger ledger(storage);
    CHECK(ledger.Count() == 0);
    usize bytes = 123;
    CHECK(!ledger.Remove(FakeAddress(0x1000), bytes));
    CHECK(bytes == 123);
    REQUIRE(ledger.Insert(FakeAddress(0x1000), 64));
    REQUIRE(ledger.Insert(FakeAddress(0x1040), 96));
    CHECK(ledger.Count() == 2);
    CHECK(ledger.Insert(FakeAddress(0x1000), 8).Error() == ErrorCode::InvalidArgument);
    CHECK(ledger.Insert(nullptr, 8).Error() == ErrorCode::InvalidArgument);
    CHECK(ledger.Count() == 2);
    // 블록 가운데 주소는 다른 주소다.
    CHECK(!ledger.Remove(FakeAddress(0x1008), bytes));
    REQUIRE(ledger.Remove(FakeAddress(0x1000), bytes));
    CHECK(bytes == 64);
    CHECK(!ledger.Remove(FakeAddress(0x1000), bytes));
    REQUIRE(ledger.Remove(FakeAddress(0x1040), bytes));
    CHECK(bytes == 96);
    CHECK(ledger.Count() == 0);
    // 지운 주소를 다시 넣을 수 있다(주소 재사용).
    REQUIRE(ledger.Insert(FakeAddress(0x1000), 32));
    REQUIRE(ledger.Remove(FakeAddress(0x1000), bytes));
    CHECK(bytes == 32);
}

// 넣기와 지우기를 섞어 오래 돌려도(지운 칸 정리·표 늘리기를 여러 번 지난다) 모델과 같다.
TEST(CoreAddressLedgerChurnMatchesModel) {
    SystemAllocator memory;
    {
        UntrackedPlatformAllocator storage;
        AddressLedger ledger(storage);
        constexpr uint32 Slots = 4096;
        constexpr uint32 Operations = 200000;
        Array<usize> model(memory);
        REQUIRE(model.TryResize(Slots));
        for (usize& size : model) {
            size = 0;
        }

        LedgerRng rng{0x4c454447ull};
        usize live = 0;
        for (uint32 operation = 0; operation < Operations; ++operation) {
            const uint32 slot = static_cast<uint32>(rng.Next() % Slots);
            const void* address = FakeAddress((uptr{slot} + 1) * 64);
            if (model[slot] == 0) {
                const usize size = 1 + static_cast<usize>(rng.Next() % 8192);
                REQUIRE(ledger.Insert(address, size));
                model[slot] = size;
                ++live;
            } else {
                usize size = 0;
                REQUIRE(ledger.Remove(address, size));
                CHECK(size == model[slot]);
                model[slot] = 0;
                --live;
            }

            REQUIRE(ledger.Count() == live);
        }

        for (uint32 slot = 0; slot < Slots; ++slot) {
            usize size = 0;
            const bool found = ledger.Remove(FakeAddress((uptr{slot} + 1) * 64), size);
            CHECK(found == (model[slot] != 0));
            if (found) {
                CHECK(size == model[slot]);
            }
        }

        CHECK(ledger.Count() == 0);
    }

    CHECK(memory.Stats().liveAllocations == 0);
}

// 표를 늘리지 못하면 넣기가 OutOfMemory이고 원장은 그대로다(이미 넣은 주소를 모두 찾는다). 실패가
// 풀리면 같은 주소를 넣을 수 있다.
TEST(CoreAddressLedgerGrowthFailureKeepsContents) {
    UntrackedPlatformAllocator storage;
    AddressLedger ledger(storage);
    constexpr uint32 Limit = 100000;
    // 첫 넣기가 표를 만든다. 그 뒤의 다음 저장소 할당(표 늘리기)을 실패시킨다.
    REQUIRE(ledger.Insert(FakeAddress(64), 1));
    storage.FailOnAttempt(1);
    uint32 inserted = 1;
    ErrorCode failure = ErrorCode::None;
    for (; inserted < Limit; ++inserted) {
        auto result = ledger.Insert(FakeAddress((uptr{inserted} + 1) * 64), inserted + 1);
        if (!result) {
            failure = result.Error();
            break;
        }
    }

    REQUIRE(inserted < Limit);
    CHECK(failure == ErrorCode::OutOfMemory);
    CHECK(ledger.Count() == inserted);
    storage.FailOnAttempt(0);
    REQUIRE(ledger.Insert(FakeAddress((uptr{inserted} + 1) * 64), inserted + 1));
    for (uint32 index = 0; index <= inserted; ++index) {
        usize size = 0;
        REQUIRE(ledger.Remove(FakeAddress((uptr{index} + 1) * 64), size));
        CHECK(size == index + 1);
    }

    CHECK(ledger.Count() == 0);
}

// 살아 있는 할당이 많아도 반환이 끝나고(주소 hash 원장 — 전에는 선형 검색) 원장·통계가 맞는다. 이미
// 반환한 주소와 블록 가운데 주소는 NotFound다.
TEST(CoreSystemAllocatorScalesToManyLiveAllocations) {
    SystemAllocator memory;
    SystemAllocator handles;
    {
        constexpr uint32 Blocks = 200000;
        constexpr usize BlockBytes = 48;
        Array<void*> blocks(handles);
        REQUIRE(blocks.TryReserve(Blocks));
        for (uint32 index = 0; index < Blocks; ++index) {
            auto block = memory.Allocate(BlockBytes, 16);
            REQUIRE(block);
            *static_cast<uint32*>(block.Value()) = index;
            REQUIRE(blocks.TryPushBack(block.Value()));
        }

        CHECK(memory.Stats().liveAllocations == Blocks);
        CHECK(memory.Stats().liveBytes == uint64{Blocks} * BlockBytes);
        CHECK(SystemAllocatorTestAccess::LedgerCount(memory) == Blocks);
        // 홀수 번째를 먼저 반환하고(처음 넣은 것부터 — 선형 목록이면 가장 먼 쪽) 내용이 그대로인지
        // 본다.
        for (uint32 index = 1; index < Blocks; index += 2) {
            CHECK(*static_cast<uint32*>(blocks[index]) == index);
            REQUIRE(memory.Deallocate(blocks[index]));
        }

        CHECK(memory.Stats().liveAllocations == Blocks / 2);
        CHECK(SystemAllocatorTestAccess::LedgerCount(memory) == Blocks / 2);
        CHECK(memory.Deallocate(blocks[1]).Error() == ErrorCode::NotFound);
        CHECK(memory.Deallocate(static_cast<uint8*>(blocks[0]) + 8).Error() == ErrorCode::NotFound);
        for (uint32 index = 0; index < Blocks; index += 2) {
            CHECK(*static_cast<uint32*>(blocks[index]) == index);
            REQUIRE(memory.Deallocate(blocks[index]));
        }

        CHECK(memory.Stats().liveAllocations == 0);
        CHECK(memory.Stats().liveBytes == 0);
        CHECK(memory.Stats().totalAllocations == Blocks);
        CHECK(SystemAllocatorTestAccess::LedgerCount(memory) == 0);
    }

    CHECK(handles.Stats().liveAllocations == 0);
}

// 원장에 적지 못한 할당은 게시되지 않는다: OutOfMemory이고 통계·원장이 그대로이며, 실패가 풀리면
// 다시 할당할 수 있다.
TEST(CoreSystemAllocatorLedgerFailureDoesNotPublish) {
    SystemAllocator memory;
    SystemAllocatorTestAccess::FailLedgerStorageOnAttempt(memory, 1);
    auto failed = memory.Allocate(64, 16);
    CHECK(failed.Error() == ErrorCode::OutOfMemory);
    CHECK(memory.Stats().liveAllocations == 0);
    CHECK(memory.Stats().liveBytes == 0);
    CHECK(memory.Stats().totalAllocations == 0);
    CHECK(SystemAllocatorTestAccess::LedgerCount(memory) == 0);
    SystemAllocatorTestAccess::FailLedgerStorageOnAttempt(memory, 0);
    auto block = memory.Allocate(64, 16);
    REQUIRE(block);
    CHECK(memory.Stats().liveAllocations == 1);
    REQUIRE(memory.Deallocate(block.Value()));
    CHECK(memory.Stats().liveAllocations == 0);
}
