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

// 주소 모양별 최악 탐사 길이의 상한이다(설계 18.6절). 원장은 주소를 통째로 섞는 기본 hash를 쓰므로
// 주소의 모양(정렬, 이어지는 주소, 블록 간격)이 군집을 만들지 않는다 — 어느 모양이든 탐사 길이가
// 무작위 표 수준이다. 이 상한을 넘으면 hash가 주소 모양에 묶인 것이다(페이지 번호만 섞는 지역성
// hash는 이어지는 주소에서 수백–수천 칸이 된다).
// - ProbeLimitHalfFull: 산 원소만으로 부하율 1/2일 때다.
// - ProbeLimitNearlyFull: 넣기·지우기가 이어져 삭제 칸이 쌓이는 동안(채움이 칸 수의 3/4에 닿아
//   표가 정리될 때까지)이다.
constexpr usize ProbeLimitHalfFull = 64;
constexpr usize ProbeLimitNearlyFull = 64;

// 주소 모양별로 원장 hash의 군집을 본다: 가장 멀리 밀려난 주소를 찾으며 보는 칸 수가 상한 안이다.
// 주소는 지어낸 값이고 넣는 순서도 정해져 있어 결과가 결정적이다.
TEST(CoreAddressLedgerProbeLengthByAddressPattern) {
    struct Pattern {
        const char* name;
        uint32 count;
        uptr stride;
        uptr offset;
    };

    constexpr uptr Base = uptr{0x14a2f} << 28;
    constexpr uptr SlabBytes = uptr{64} << 10;
    constexpr uptr PageBytes = 4096;
    constexpr uint32 Many = 65536;
    constexpr Pattern Patterns[] = {
        // 64KB로 정렬된 주소만(큰 할당·slab), 한 페이지를 16 byte 블록 256개로 채움, 여러 페이지의
        // 같은 offset, 그리고 이어지는 주소(블록 간격 16·32·64·112·208 byte).
        {"64KB aligned", 4096, SlabBytes, 0},
        {"one full page", 256, 16, 0},
        {"same offset in many pages", 4096, PageBytes, 0x40},
        {"sequential 16B", Many, 16, 0},
        {"sequential 32B", Many, 32, 0},
        {"sequential 64B", Many, 64, 0},
        {"sequential 112B", Many, 112, 0},
        {"sequential 208B", Many, 208, 0},
    };

    for (const Pattern& pattern : Patterns) {
        UntrackedPlatformAllocator storage;
        AddressLedger ledger(storage);
        for (uint32 index = 0; index < pattern.count; ++index) {
            REQUIRE(ledger.Insert(FakeAddress(Base + pattern.offset + uptr{index} * pattern.stride),
                                  index + 1));
        }

        CHECK(ledger.Count() == pattern.count);
        const usize longest = ledger.LongestProbe();
        CHECK(longest >= 1);
        CHECK(longest <= ProbeLimitHalfFull);
        // 내용은 탐사 길이와 무관하게 맞다.
        for (uint32 index = 0; index < pattern.count; ++index) {
            usize bytes = 0;
            REQUIRE(ledger.Remove(FakeAddress(Base + pattern.offset + uptr{index} * pattern.stride),
                                  bytes));
            REQUIRE(bytes == index + 1);
        }

        CHECK(ledger.Count() == 0);
        CHECK(ledger.LongestProbe() == 0);
    }
}

// 표가 가장 찰 때도 같다: 살아 있는 주소 창을 한 칸씩 밀면(가장 오래된 주소를 지우고 다음 주소를
// 넣는다 — 이어지는 힙의 churn) 삭제 칸이 쌓여 채움이 칸 수의 3/4에 닿고 표가 정리된다. 그 주기
// 내내 탐사 길이가 상한 안이다.
TEST(CoreAddressLedgerProbeLengthWhileTombstonesAccumulate) {
    constexpr uptr Base = uptr{0x2b91c} << 28;
    constexpr uint32 Live = 65536;
    // 창을 미는 횟수다. 한 번 밀 때마다 채움(산 원소 + 삭제 칸)이 많아야 한 칸 늘고, 새 주소가 빈
    // 칸에서 시작할 확률(1/4 이상)만큼은 는다. 칸 수(Live의 두 배)만큼 밀면 채움이 칸 수의 3/4에
    // 적어도 한 번 닿아 표가 정리된다.
    constexpr uint32 Slides = 2 * Live;
    constexpr uint32 SampleEvery = 512;
    constexpr uptr Strides[] = {16, 64, 208};
    for (uptr stride : Strides) {
        UntrackedPlatformAllocator storage;
        AddressLedger ledger(storage);
        for (uint32 index = 0; index < Live; ++index) {
            REQUIRE(ledger.Insert(FakeAddress(Base + uptr{index} * stride), index + 1));
        }

        usize longest = ledger.LongestProbe();
        for (uint32 slide = 0; slide < Slides; ++slide) {
            usize bytes = 0;
            REQUIRE(ledger.Remove(FakeAddress(Base + uptr{slide} * stride), bytes));
            REQUIRE(bytes == slide + 1);
            REQUIRE(
                ledger.Insert(FakeAddress(Base + uptr{Live + slide} * stride), Live + slide + 1));
            if (slide % SampleEvery == 0) {
                const usize sampled = ledger.LongestProbe();
                longest = sampled > longest ? sampled : longest;
            }
        }

        CHECK(ledger.Count() == Live);
        CHECK(longest >= 1);
        CHECK(longest <= ProbeLimitNearlyFull);
        for (uint32 index = Slides; index < Slides + Live; ++index) {
            usize bytes = 0;
            REQUIRE(ledger.Remove(FakeAddress(Base + uptr{index} * stride), bytes));
            REQUIRE(bytes == index + 1);
        }

        CHECK(ledger.Count() == 0);
    }
}
